#include "../include/int_edp.h"
#include <efiprot.h>

#define S(x)  _INT_RepStr(R, (const CHAR8*)(x))
#define NL()  _INT_RepNl(R)
#define HX(v, d) _INT_RepHex(R, (v), (d))
#define DC(v) _INT_RepDec(R, (v))

// Gen 9 / 9.5 (SKL..CFL) display registers, DDI A / AUX-A, as in Linux i915.
#define DP_AUX_CH_CTL_A     0x64010
#define DP_AUX_CH_DATA_A(i) (0x64014 + 4 * (i))
#define PWR_WELL_CTL2       0x45404
#define PP_STATUS           0xC7200
#define PP_CONTROL          0xC7204

#define AUX_SEND_BUSY       (1u << 31)
#define AUX_DONE            (1u << 30)
#define AUX_INTERRUPT       (1u << 29)
#define AUX_TIMEOUT_ERR     (1u << 28)
#define AUX_TIMEOUT_MAX     (3u << 26)
#define AUX_RECEIVE_ERR     (1u << 25)
#define AUX_MSG_SIZE(n)     ((UINT32)(n) << 20)
#define AUX_FW_SYNC_SKL     (23u << 5)           // FW_SYNC_PULSE_SKL(24)
#define AUX_SYNC_SKL        31u                  // SYNC_PULSE_SKL(32)

#define PW_PW1_REQ          (1u << 29)           // SKL_PW_CTL_IDX_PW_1 = 14
#define PW_PW1_STATE        (1u << 28)
#define PW_DDI_AE_REQ       (1u << 3)            // SKL_PW_CTL_IDX_DDI_A_E = 1
#define PW_DDI_AE_STATE     (1u << 2)

#define PP_ON               (1u << 31)
#define PP_FORCE_VDD        (1u << 3)
#define PP_UNLOCK           0xABCD0000u

typedef struct {
    EFI_BOOT_SERVICES*   BS;
    EFI_PCI_IO_PROTOCOL* Pci;
} DEV;

static UINT32 Rd(DEV* D, UINT32 off)
{
    UINT32 v = 0xFFFFFFFF;
    D->Pci->Mem.Read(D->Pci, EfiPciIoWidthUint32, 0, off, 1, &v);
    return v;
}

static VOID Wr(DEV* D, UINT32 off, UINT32 v)
{
    D->Pci->Mem.Write(D->Pci, EfiPciIoWidthUint32, 0, off, 1, &v);
}

// One AUX transaction as i915 intel_dp_aux_xfer() does it: retried up to 5 times,
// every wait bounded. tx/rx are raw AUX bytes (header + payload / reply + payload).
static EFI_STATUS AuxXfer(DEV* D, const UINT8* tx, UINTN txn, UINT8* rx, UINTN* rxn)
{
    for (UINTN attempt = 0; attempt < 5; attempt++) {
        UINT32 ctl = 0;
        BOOLEAN idle = FALSE;

        for (UINTN i = 0; i < 100; i++) {                    // <= 10 ms
            ctl = Rd(D, DP_AUX_CH_CTL_A);
            if (ctl == 0xFFFFFFFF) return EFI_NOT_READY;     // block is powered down
            if (!(ctl & AUX_SEND_BUSY)) { idle = TRUE; break; }
            D->BS->Stall(100);
        }
        if (!idle) return EFI_TIMEOUT;

        for (UINTN i = 0; i < txn; i += 4) {
            UINT32 w = 0;
            for (UINTN j = 0; j < 4; j++) {
                w <<= 8;
                if (i + j < txn) w |= tx[i + j];
            }
            Wr(D, DP_AUX_CH_DATA_A(i / 4), w);
        }

        UINT32 send = AUX_SEND_BUSY | AUX_DONE | AUX_INTERRUPT | AUX_TIMEOUT_ERR |
                      AUX_TIMEOUT_MAX | AUX_RECEIVE_ERR | AUX_MSG_SIZE(txn) |
                      AUX_FW_SYNC_SKL | AUX_SYNC_SKL;
        Wr(D, DP_AUX_CH_CTL_A, send);

        for (UINTN i = 0; i < 100; i++) {
            ctl = Rd(D, DP_AUX_CH_CTL_A);
            if (!(ctl & AUX_SEND_BUSY)) break;
            D->BS->Stall(100);
        }
        // clear the sticky status bits
        Wr(D, DP_AUX_CH_CTL_A, ctl | AUX_DONE | AUX_TIMEOUT_ERR | AUX_RECEIVE_ERR);

        if ((ctl & AUX_SEND_BUSY) || (ctl & AUX_TIMEOUT_ERR) || (ctl & AUX_RECEIVE_ERR) ||
            !(ctl & AUX_DONE)) {
            D->BS->Stall(1000);
            continue;
        }
        UINTN n = (ctl >> 20) & 0x1F;
        if (n == 0 || n > 20) { D->BS->Stall(1000); continue; }
        if (n > *rxn) n = *rxn;
        for (UINTN i = 0; i < n; i += 4) {
            UINT32 w = Rd(D, DP_AUX_CH_DATA_A(i / 4));
            for (UINTN j = 0; j < 4 && i + j < n; j++)
                rx[i + j] = (UINT8)(w >> (24 - 8 * j));
        }
        *rxn = n;
        return EFI_SUCCESS;
    }
    return EFI_DEVICE_ERROR;
}

// Native AUX read of len (1..16) DPCD bytes.
static EFI_STATUS DpcdRead(DEV* D, UINT32 addr, UINT8* out, UINTN len)
{
    for (UINTN defer = 0; defer < 7; defer++) {
        UINT8 tx[4] = { (UINT8)(0x90 | ((addr >> 16) & 0xF)), (UINT8)(addr >> 8),
                        (UINT8)addr, (UINT8)(len - 1) };
        UINT8 rx[20];
        UINTN rn = len + 1;
        EFI_STATUS St = AuxXfer(D, tx, 4, rx, &rn);
        if (EFI_ERROR(St)) return St;
        UINT8 code = (UINT8)(rx[0] >> 4);
        if (code == 0x2) { D->BS->Stall(400); continue; }     // AUX_DEFER
        if (code != 0 || rn < len + 1) return EFI_DEVICE_ERROR;
        for (UINTN i = 0; i < len; i++) out[i] = rx[1 + i];
        return EFI_SUCCESS;
    }
    return EFI_TIMEOUT;
}

// I2C-over-AUX transfer to the EDID EEPROM (0x50). mot = middle-of-transaction.
static EFI_STATUS I2cAux(DEV* D, BOOLEAN read, BOOLEAN mot, UINT8* data, UINTN len)
{
    for (UINTN defer = 0; defer < 7; defer++) {
        UINT8 tx[5];
        UINT8 rx[20];
        UINTN txn = 4, rn = read ? len + 1 : 1;
        tx[0] = (UINT8)(((read ? 0x1 : 0x0) | (mot ? 0x4 : 0x0)) << 4);
        tx[1] = 0x00;
        tx[2] = 0x50;
        tx[3] = (UINT8)(len - 1);
        if (!read) { tx[4] = data[0]; txn = 5; }
        EFI_STATUS St = AuxXfer(D, tx, txn, rx, &rn);
        if (EFI_ERROR(St)) return St;
        UINT8 code = (UINT8)(rx[0] >> 4);
        if ((code & 0xC) == 0x8 || (code & 0x3) == 0x2) { D->BS->Stall(400); continue; }   // defer
        if (code != 0) return EFI_DEVICE_ERROR;                                             // NACK
        if (read) {
            if (rn < len + 1) return EFI_DEVICE_ERROR;
            for (UINTN i = 0; i < len; i++) data[i] = rx[1 + i];
        }
        return EFI_SUCCESS;
    }
    return EFI_TIMEOUT;
}

static EFI_STATUS ReadEdidAux(DEV* D, UINT8* edid)
{
    UINT8 off = 0;
    EFI_STATUS St = I2cAux(D, FALSE, TRUE, &off, 1);
    if (EFI_ERROR(St)) return St;
    for (UINTN i = 0; i < 128; i += 16) {
        St = I2cAux(D, TRUE, i + 16 < 128, edid + i, 16);
        if (EFI_ERROR(St)) return St;
    }
    return _INT_EdidValid(edid) ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}

EFI_STATUS _INT_EdpProbe(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                         _INT_EdpCaps* C, _INT_Rep* R)
{
    EFI_STATUS Status;
    UINT32 Id = 0, cmd = 0, bar0 = 0, bar1 = 0;
    DEV D;

    for (UINTN i = 0; i < sizeof(*C); i++) ((UINT8*)C)[i] = 0;

    D.BS = BS;
    D.Pci = _INT_FindIgpu(BS, ImageHandle, &Id, &Status);
    if (!D.Pci) { S("eDP probe: no Intel iGPU found"); NL(); return EFI_NOT_FOUND; }

    D.Pci->Pci.Read(D.Pci, EfiPciIoWidthUint32, 4, 1, &cmd);
    D.Pci->Pci.Read(D.Pci, EfiPciIoWidthUint32, 0x10, 1, &bar0);
    D.Pci->Pci.Read(D.Pci, EfiPciIoWidthUint32, 0x14, 1, &bar1);
    UINT64 base = (UINT64)(bar0 & ~0xFu);
    if ((bar0 & 0x6) == 0x4) base |= (UINT64)bar1 << 32;
    if ((bar0 & 1) || base == 0) {
        S("eDP probe: iGPU BAR0 not assigned"); NL();
        return EFI_NOT_READY;
    }
    if (!(cmd & 0x2)) {
        Status = D.Pci->Attributes(D.Pci, EfiPciIoAttributeOperationEnable,
                                   EFI_PCI_IO_ATTRIBUTE_MEMORY, NULL);
        if (EFI_ERROR(Status)) { S("eDP probe: cannot enable iGPU memory decode"); NL(); return Status; }
    }

    UINT32 pw = Rd(&D, PWR_WELL_CTL2);
    UINT32 pps = Rd(&D, PP_STATUS);
    UINT32 ppc = Rd(&D, PP_CONTROL);
    S("eDP probe: PWR_WELL_CTL2=0x"); HX(pw, 8); S(" PP_STATUS=0x"); HX(pps, 8);
    S(" PP_CONTROL=0x"); HX(ppc, 8); S(" AUX_CTL=0x"); HX(Rd(&D, DP_AUX_CH_CTL_A), 8); NL();
    if (pw == 0xFFFFFFFF || pps == 0xFFFFFFFF) {
        S("eDP probe: display registers read all ones, not touching anything"); NL();
        return EFI_NOT_READY;
    }

    // 1. power wells the AUX-A channel sits in (same requests i915 makes at init)
    UINT32 want = 0;
    if (!(pw & PW_PW1_STATE))   want |= PW_PW1_REQ;
    if (!(pw & PW_DDI_AE_STATE)) want |= PW_DDI_AE_REQ;
    if (want) {
        Wr(&D, PWR_WELL_CTL2, pw | want);
        UINT32 st = 0;
        for (UINTN i = 0; i < 300; i++) {                      // <= 30 ms (i915: 3 ms)
            st = Rd(&D, PWR_WELL_CTL2);
            if ((st & PW_PW1_STATE) && (st & PW_DDI_AE_STATE)) break;
            BS->Stall(100);
        }
        S("eDP probe: requested power wells, PWR_WELL_CTL2=0x"); HX(st, 8); NL();
    }

    // 2. panel VDD (needed for AUX when the panel is not already on)
    BOOLEAN ForcedVdd = FALSE;
    if (!(pps & PP_ON) && !(ppc & PP_FORCE_VDD)) {
        Wr(&D, PP_CONTROL, ((ppc & 0xFFFF) | PP_FORCE_VDD) | PP_UNLOCK);
        ForcedVdd = TRUE;
        BS->Stall(150000);                                     // T1+T3 with margin
        S("eDP probe: panel was off, forced VDD for AUX"); NL();
    }

    // 3. DPCD
    UINT8 b[16];
    Status = DpcdRead(&D, 0x000, b, 16);
    if (EFI_ERROR(Status)) {
        S("eDP probe: DPCD read failed, status=0x"); HX(Status, 16);
        S(" (AUX_CTL=0x"); HX(Rd(&D, DP_AUX_CH_CTL_A), 8); S(")"); NL();
    } else {
        C->Valid = TRUE;
        C->DpcdRev = b[0];
        C->MaxLinkRate = b[1];
        C->MaxLanes = (UINT8)(b[2] & 0x1F);
        S("eDP probe: DPCD rev 0x"); HX(b[0], 2); S(" max rate 0x"); HX(b[1], 2);
        S(" max lanes "); DC(C->MaxLanes); S(" eDP cfg cap 0x"); HX(b[0x0D], 2); NL();

        UINT8 t;
        if (!EFI_ERROR(DpcdRead(&D, 0x700, &t, 1))) C->EdpRev = t;
        if (!EFI_ERROR(DpcdRead(&D, 0x070, &t, 1))) C->PsrSupport = t;
        if (C->EdpRev >= 3 && !EFI_ERROR(DpcdRead(&D, 0x010, b, 16))) {
            for (UINTN i = 0; i < 8; i++) C->RateTable[i] = _INT_Rd16(b + 2 * i);
            S("eDP probe: SUPPORTED_LINK_RATES (x200 kHz):");
            for (UINTN i = 0; i < 8 && C->RateTable[i]; i++) { S(" "); DC(C->RateTable[i]); }
            NL();
        }
        S("eDP probe: eDP rev "); DC(C->EdpRev); S(", PSR support "); DC(C->PsrSupport); NL();
        _INT_EdpDeriveLink(C);
        S("eDP probe: -> VBT rate code "); DC(C->VbtRate); S(", lanes code "); DC(C->VbtLanes); NL();

        // 4. EDID straight from the panel (fallback for when the dGPU GOP has none)
        if (!EFI_ERROR(ReadEdidAux(&D, C->Edid))) {
            C->HasEdid = TRUE;
            S("eDP probe: EDID read over I2C-over-AUX OK"); NL();
        } else {
            S("eDP probe: EDID over AUX not available"); NL();
        }
    }

    if (ForcedVdd) {
        UINT32 now = Rd(&D, PP_CONTROL);
        Wr(&D, PP_CONTROL, ((now & 0xFFFF) & ~PP_FORCE_VDD) | PP_UNLOCK);
        S("eDP probe: VDD override released"); NL();
    }
    return C->Valid ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}