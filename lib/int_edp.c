#include "../include/int_edp.h"
#include <efiprot.h>

#define S(x)  _INT_RepStr(R, (const CHAR8*)(x))
#define NL()  _INT_RepNl(R)
#define HX(v, d) _INT_RepHex(R, (v), (d))
#define DC(v) _INT_RepDec(R, (v))

// Gen 9 / 9.5 (SKL..CFL) display registers, DDI A / AUX-A, as in Linux i915.
#define DP_AUX_CH_CTL_A     0x64010
#define DP_AUX_CH_DATA_A(i) (0x64014 + 4 * (i))
#define PWR_WELL_CTL1       0x45400
#define PWR_WELL_CTL2       0x45404
#define PP_STATUS           0xC7200
#define PP_CONTROL          0xC7204
#define PP_ON_DELAYS        0xC7208
#define PP_OFF_DELAYS       0xC720C
#define PP_DIVISOR          0xC7210
#define PCH_RAWCLK_FREQ     0xC6204

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
#define EDP_BLC_ENABLE      (1u << 2)
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


// Cold boot with an external display: Apple EFI may leave the internal panel OFF
// even when AUX is readable via FORCE_VDD. Match the panel-power stage of i915
// without touching pipe / PLL / DDI training or enabling backlight.
#define PP_POWER_ON (1u << 0)
#define PP_RESET (1u << 1)
#define PP_ON_STATUS (1u << 31)
#define PP_SEQUENCE_MASK (3u << 28)            // 0 = none, 1 = power up, 2 = power down
#define PP_CYCLE_DELAY_ACTIVE (1u << 27)       // T12 power cycle delay still counting
#define PP_STATE_MASK 0xFu                     // PP_SEQUENCE_STATE
#define PP_STATE_OFF_IDLE 0x0u
#define PP_STATE_ON_IDLE 0x8u

static EFI_STATUS PpsIgpu(EFI_BOOT_SERVICES *BS, EFI_HANDLE Image,
                          EFI_PCI_IO_PROTOCOL **Out, _INT_Rep *R)
{
    EFI_STATUS st=EFI_SUCCESS;
    UINT32 id=0,cmd=0;
    EFI_PCI_IO_PROTOCOL *p=_INT_FindIgpu(BS,Image,&id,&st);
    if (!p) { S("PPS: Intel device missing");NL();return EFI_NOT_FOUND; }
    st=p->Pci.Read(p,EfiPciIoWidthUint32,4,1,&cmd);
    if(EFI_ERROR(st))return st;
    if(!(cmd&0x2)) {
        st=p->Attributes(p,EfiPciIoAttributeOperationEnable,
                         EFI_PCI_IO_ATTRIBUTE_MEMORY,NULL);
        if(EFI_ERROR(st))return st;
    }
    *Out=p;
    return EFI_SUCCESS;
}

EFI_STATUS _INT_EdpPanelPowerState(EFI_BOOT_SERVICES *BS,EFI_HANDLE Image,
                                   BOOLEAN *Powered,UINT32 *StatusReg,
                                   UINT32 *ControlReg,_INT_Rep *R)
{
    EFI_PCI_IO_PROTOCOL *p=NULL;
    EFI_STATUS st=PpsIgpu(BS,Image,&p,R);
    if(EFI_ERROR(st))return st;
    st=p->Mem.Read(p,EfiPciIoWidthUint32,0,PP_STATUS,1,StatusReg);
    if(EFI_ERROR(st))return st;
    st=p->Mem.Read(p,EfiPciIoWidthUint32,0,PP_CONTROL,1,ControlReg);
    if(EFI_ERROR(st))return st;
    if(*StatusReg==0xFFFFFFFFu||*ControlReg==0xFFFFFFFFu)return EFI_NOT_READY;
    *Powered=((*StatusReg & PP_ON_STATUS)!=0);
    S("PPS snapshot: status=0x");HX(*StatusReg,8);
    S(" control=0x");HX(*ControlReg,8);
    S(" actual panel powered=");DC(*Powered);NL();
    return EFI_SUCCESS;
}

EFI_STATUS _INT_EdpPanelPrepare(EFI_BOOT_SERVICES *BS,EFI_HANDLE Image,_INT_Rep *R)
{
    EFI_PCI_IO_PROTOCOL *p=NULL;
    EFI_STATUS st=PpsIgpu(BS,Image,&p,R);
    if(EFI_ERROR(st))return st;
    DEV d={BS,p};
    UINT32 ps=Rd(&d,PP_STATUS),pc=Rd(&d,PP_CONTROL),pw=Rd(&d,PWR_WELL_CTL2);
    if(ps==0xFFFFFFFFu||pc==0xFFFFFFFFu||pw==0xFFFFFFFFu)
        return EFI_NOT_READY;
    if(ps & PP_ON_STATUS){S("PPS: already powered; not changing state");NL();return EFI_SUCCESS;}
    // Ensure the display power domains serving PPS/DDI-A are accessible.
    UINT32 wanted=PW_PW1_REQ|PW_DDI_AE_REQ;
    if((pw & wanted)!=wanted) Wr(&d,PWR_WELL_CTL2,pw|wanted);
    BOOLEAN wells=FALSE;
    for(UINTN i=0;i<300;i++) {
        pw=Rd(&d,PWR_WELL_CTL2);
        if((pw&(PW_PW1_STATE|PW_DDI_AE_STATE))==
              (PW_PW1_STATE|PW_DDI_AE_STATE)){wells=TRUE;break;}
        BS->Stall(100);
    }
    if(!wells){S("PPS: power wells failed");NL();return EFI_TIMEOUT;}

    // i915 wait_panel_power_cycle(): never start a power-up while the previous
    // power-down is still running or the T12 power-cycle delay is still counting.
    {
        BOOLEAN idle=FALSE;
        for(UINTN i=0;i<15000;i++) {                       // <= 1.5 s bounded wait
            ps=Rd(&d,PP_STATUS);
            if(!(ps & PP_ON_STATUS) && !(ps & PP_CYCLE_DELAY_ACTIVE) &&
               (ps & PP_SEQUENCE_MASK)==0 && (ps & PP_STATE_MASK)==PP_STATE_OFF_IDLE)
                {idle=TRUE;break;}
            BS->Stall(100);
        }
        // Like i915 (which only logs an error here) go on: refusing would leave the panel dark.
        if(!idle){S("PPS: WARNING power cycle delay did not finish, status=0x");HX(ps,8);NL();}
    }

    // Intel i915 panel-on step. Do not enable EDP_BLC_ENABLE until the
    // operating system has completed link training / modeset.
    // FORCE_VDD is preserved while the power-up sequence runs (the sequencer needs the
    // panel supply anyway) and dropped again afterwards, see below.
    UINT32 next=(pc & 0x0000FFFFu) | PP_POWER_ON | PP_RESET;
    next=(next & ~((UINT32)EDP_BLC_ENABLE)) | PP_UNLOCK;
    Wr(&d,PP_CONTROL,next);
    UINT32 posted=Rd(&d,PP_CONTROL);
    S("PPS: control after panel-on request=0x");HX(posted,8);NL();

    // i915 wait_panel_on(): PP_ON alone only says the sequence STARTED (the log of a real
    // run shows status 0x9000000A = PP_ON + sequence "power up" + state S1). Wait until it
    // is finished (PP_ON, no sequence running, state ON_IDLE), so that the driver that is
    // loaded next does not take over a panel that is still in the middle of T1/T2.
    BOOLEAN done=FALSE;
    for(UINTN i=0;i<20000;i++) {                           // <= 2 s bounded wait (i915: 5 s)
        ps=Rd(&d,PP_STATUS);
        if((ps & (PP_ON_STATUS|PP_SEQUENCE_MASK|PP_STATE_MASK))==(PP_ON_STATUS|PP_STATE_ON_IDLE)) {
            done=TRUE;
            break;
        }
        BS->Stall(100);
    }
    if(!done) {
        S("PPS: panel power ON TIMEOUT, status=0x");HX(ps,8);NL();
        return EFI_TIMEOUT;
    }
    S("PPS: panel power ON confirmed, status=0x");HX(ps,8);NL();

    // The firmware of a boot that lights the panel from the iGPU leaves PP_CONTROL
    // without FORCE_VDD (dump: 0x67 = on + reset + backlight). The power-up above was
    // started with the VDD override that Apple EFI left set (0x68), and nothing
    // else would ever clear it: the next driver would find an override it did not
    // set. The panel is on, so VDD stays supplied by the sequencer itself.
    pc=Rd(&d,PP_CONTROL);
    if(pc & PP_FORCE_VDD) {
        Wr(&d,PP_CONTROL,((pc & 0x0000FFFFu) & ~((UINT32)PP_FORCE_VDD)) | PP_UNLOCK);
        pc=Rd(&d,PP_CONTROL);
        ps=Rd(&d,PP_STATUS);
        S("PPS: VDD override released, control=0x");HX(pc,8);S(" status=0x");HX(ps,8);NL();
        if(!(ps & PP_ON_STATUS)) {
            S("PPS: panel dropped after VDD release");NL();
            return EFI_DEVICE_ERROR;
        }
    }
    return EFI_SUCCESS;
}

EFI_STATUS _INT_EdpReadPps(EFI_BOOT_SERVICES *BS,EFI_HANDLE Image,_INT_EdpPps *P,_INT_Rep *R)
{
    EFI_PCI_IO_PROTOCOL *p=NULL;
    P->Valid=FALSE;P->OnDelays=P->OffDelays=P->Divisor=P->RawClk=0;
    EFI_STATUS st=PpsIgpu(BS,Image,&p,R);
    if(EFI_ERROR(st))return st;
    DEV d={BS,p};
    UINT32 on=Rd(&d,PP_ON_DELAYS),off=Rd(&d,PP_OFF_DELAYS),dv=Rd(&d,PP_DIVISOR),rc=Rd(&d,PCH_RAWCLK_FREQ);
    if(on==0xFFFFFFFFu||off==0xFFFFFFFFu||dv==0xFFFFFFFFu||rc==0xFFFFFFFFu)return EFI_NOT_READY;
    P->OnDelays=on;P->OffDelays=off;P->Divisor=dv;P->RawClk=rc;
    P->Valid=(on!=0||off!=0);
    S("PPS timing: on=0x");HX(on,8);S(" off=0x");HX(off,8);S(" div=0x");HX(dv,8);S(" rawclk=0x");HX(rc,8);NL();
    return EFI_SUCCESS;
}

EFI_STATUS _INT_EdpRestorePps(EFI_BOOT_SERVICES *BS,EFI_HANDLE Image,const _INT_EdpPps *C,_INT_Rep *R)
{
    EFI_PCI_IO_PROTOCOL *p=NULL;
    if(!C||!C->Valid)return EFI_NOT_FOUND;
    EFI_STATUS st=PpsIgpu(BS,Image,&p,R);
    if(EFI_ERROR(st))return st;
    DEV d={BS,p};
    UINT32 ps=Rd(&d,PP_STATUS),pc=Rd(&d,PP_CONTROL);
    UINT32 on=Rd(&d,PP_ON_DELAYS),off=Rd(&d,PP_OFF_DELAYS),dv=Rd(&d,PP_DIVISOR),rc=Rd(&d,PCH_RAWCLK_FREQ);
    if(ps==0xFFFFFFFFu||pc==0xFFFFFFFFu||on==0xFFFFFFFFu||dv==0xFFFFFFFFu||rc==0xFFFFFFFFu)
        return EFI_NOT_READY;
    if(ps & PP_ON)return EFI_SUCCESS;                      // panel running: never touch the timing
    BOOLEAN need_rc=(rc==0 && C->RawClk!=0);
    BOOLEAN need_dv=((dv>>8)==0 && (C->Divisor>>8)!=0);
    BOOLEAN need_on=(on==0 && C->OnDelays!=0);
    BOOLEAN need_off=(off==0 && C->OffDelays!=0);
    if(!need_rc&&!need_dv&&!need_on&&!need_off)return EFI_SUCCESS;
    // i915 order: rawclk first (the divider is derived from it), then the PPS registers,
    // which need the unlock key in PP_CONTROL.
    if(need_rc)Wr(&d,PCH_RAWCLK_FREQ,C->RawClk);
    Wr(&d,PP_CONTROL,(pc & 0x0000FFFFu)|PP_UNLOCK);
    if(need_on)Wr(&d,PP_ON_DELAYS,C->OnDelays);
    if(need_off)Wr(&d,PP_OFF_DELAYS,C->OffDelays);
    if(need_dv)Wr(&d,PP_DIVISOR,C->Divisor);
    S("PPS timing restored from cache: rawclk=0x");HX(Rd(&d,PCH_RAWCLK_FREQ),8);
    S(" on=0x");HX(Rd(&d,PP_ON_DELAYS),8);S(" off=0x");HX(Rd(&d,PP_OFF_DELAYS),8);
    S(" div=0x");HX(Rd(&d,PP_DIVISOR),8);NL();
    return EFI_SUCCESS;
}

EFI_STATUS _INT_EdpReleaseWells(EFI_BOOT_SERVICES *BS,EFI_HANDLE Image,_INT_Rep *R)
{
    EFI_PCI_IO_PROTOCOL *p=NULL;
    EFI_STATUS st=PpsIgpu(BS,Image,&p,R);
    if(EFI_ERROR(st))return st;
    DEV d={BS,p};
    UINT32 pw=Rd(&d,PWR_WELL_CTL2);
    if(pw==0xFFFFFFFFu)return EFI_NOT_READY;
    UINT32 req=PW_PW1_REQ|PW_DDI_AE_REQ;
    if(pw & req) {
        Wr(&d,PWR_WELL_CTL2,pw & ~req);
        S("Power wells: driver requests released, PWR_WELL_CTL2=0x");HX(Rd(&d,PWR_WELL_CTL2),8);NL();
    }
    return EFI_SUCCESS;
}

// The replay lights the pipeline and needs PW1 + DDI_A_E to stay up. Dropping the loader's
// driver-register requests (EdpReleaseWells) would cut them under the lit pipe, and leaving them
// puts requests in PWR_WELL_CTL2, a register the firmware never uses and the OS driver owns.
// A firmware-lit boot holds the same wells through the BIOS register (PWR_WELL_CTL1), so move the
// requests there first and drop the driver ones only once the BIOS ones read back and the wells
// are still up. On any doubt the driver requests stay (nothing is ever cut).
EFI_STATUS _INT_EdpHandoffWells(EFI_BOOT_SERVICES *BS,EFI_HANDLE Image,_INT_Rep *R)
{
    EFI_PCI_IO_PROTOCOL *p=NULL;
    EFI_STATUS st=PpsIgpu(BS,Image,&p,R);
    if(EFI_ERROR(st))return st;
    DEV d={BS,p};
    UINT32 bios=Rd(&d,PWR_WELL_CTL1),drv=Rd(&d,PWR_WELL_CTL2);
    if(bios==0xFFFFFFFFu||drv==0xFFFFFFFFu)return EFI_NOT_READY;
    UINT32 req=PW_PW1_REQ|PW_DDI_AE_REQ,up=PW_PW1_STATE|PW_DDI_AE_STATE;
    if(!(drv & req))return EFI_SUCCESS;                    // the loader holds nothing
    if((bios & req)!=req)Wr(&d,PWR_WELL_CTL1,bios|req);
    bios=Rd(&d,PWR_WELL_CTL1);
    if((bios & req)!=req){
        S("Power wells: BIOS request did not stick, PWR_WELL_CTL1=0x");HX(bios,8);S(", driver requests kept");NL();
        return EFI_DEVICE_ERROR;
    }
    Wr(&d,PWR_WELL_CTL2,drv & ~req);
    BS->Stall(2000);
    drv=Rd(&d,PWR_WELL_CTL2);
    if((drv & up)!=up){
        Wr(&d,PWR_WELL_CTL2,drv|req);                      // roll back, the pipeline needs them
        S("Power wells: wells dropped after the handoff, driver requests restored");NL();
        return EFI_DEVICE_ERROR;
    }
    S("Power wells: held by the BIOS register now, PWR_WELL_CTL1=0x");HX(Rd(&d,PWR_WELL_CTL1),8);
    S(" PWR_WELL_CTL2=0x");HX(drv,8);NL();
    return EFI_SUCCESS;
}

// =====================================================================================
// Display state capture / replay (see int_edp.h)
// =====================================================================================
#define DPLL_CTRL1            0x6C058
#define DPLL_CTRL2            0x6C05C
#define DPLL_STATUS           0x6C060
#define DDI_BUF_CTL_A         0x64000
#define DP_TP_CTL_A           0x64040
#define DP_TP_STATUS_A        0x64044
#define DDI_BUF_TRANS_A       0x64E00
#define TRANS_HTOTAL_EDP      0x6F000
#define TRANS_VTOTAL_EDP      0x6F00C
#define TRANS_MSA_MISC_EDP    0x6F410
#define TRANS_DDI_FUNC_EDP    0x6F400
#define PIPECONF_EDP          0x7F008
#define PIPESRC_A             0x6001C
#define PIPE_BOTTOM_COLOR_A   0x70034
#define BLC_PWM_CTL1          0xC8250
#define BLC_PWM_FREQ          0xC8254
#define BLC_PWM_DUTY          0xC8258

#define DDI_BUF_ENABLE        (1u << 31)
#define DDI_BUF_IDLE          (1u << 7)
#define DDI_BUF_TRANS_SEL(n)  ((UINT32)(n) << 24)
#define DDI_BUF_TRANS_MASK    (0xFu << 24)
#define TP_ENABLE             (1u << 31)
#define TP_ENHANCED           (1u << 18)
#define TP_PAT1               (0u << 8)
#define TP_PAT2               (1u << 8)
#define TP_IDLE               (2u << 8)
#define TP_NORMAL             (3u << 8)
#define TP_PAT3               (4u << 8)
#define TP_STATUS_IDLE_DONE   (1u << 25)
#define FUNC_ENABLE           (1u << 31)
#define PIPECONF_ENABLE       (1u << 31)

static const UINT32 kDispRegs[] = {
    DPLL_CTRL1, DPLL_CTRL2,
    0x64E00,0x64E04,0x64E08,0x64E0C,0x64E10,0x64E14,0x64E18,0x64E1C,0x64E20,0x64E24,
    0x64E28,0x64E2C,0x64E30,0x64E34,0x64E38,0x64E3C,0x64E40,0x64E44,0x64E48,0x64E4C,
    DDI_BUF_CTL_A, DP_TP_CTL_A,
    0x6F000,0x6F004,0x6F008,0x6F00C,0x6F010,0x6F014,0x6F028,   // HTOTAL..VSYNC, VSYNCSHIFT
    0x6F030,0x6F034,0x6F040,0x6F044,TRANS_MSA_MISC_EDP,        // data / link M N, MSA misc
    TRANS_DDI_FUNC_EDP, PIPECONF_EDP,
    BLC_PWM_CTL1, BLC_PWM_FREQ, BLC_PWM_DUTY
};

static BOOLEAN DispGet(const _INT_DispState* St, UINT32 off, UINT32* v)
{
    for (UINT32 i = 0; i < St->Count && i < _INT_DISP_MAX; i++)
        if (St->R[i].Off == off) { if (v) *v = St->R[i].Val; return TRUE; }
    return FALSE;
}

EFI_STATUS _INT_DispCapture(EFI_BOOT_SERVICES* BS, EFI_HANDLE Image, _INT_DispState* St, _INT_Rep* R)
{
    EFI_PCI_IO_PROTOCOL* p = NULL;
    St->Count = 0;
    EFI_STATUS st = PpsIgpu(BS, Image, &p, R);
    if (EFI_ERROR(st)) return st;
    DEV d = { BS, p };
    for (UINTN i = 0; i < sizeof(kDispRegs) / sizeof(kDispRegs[0]) && St->Count < _INT_DISP_MAX; i++) {
        UINT32 v = Rd(&d, kDispRegs[i]);
        if (v == 0xFFFFFFFFu) { St->Count = 0; return EFI_NOT_READY; }
        St->R[St->Count].Off = kDispRegs[i];
        St->R[St->Count].Val = v;
        St->Count++;
    }
    UINT32 buf = 0, tp = 0, fn = 0, pc = 0;
    DispGet(St, DDI_BUF_CTL_A, &buf); DispGet(St, DP_TP_CTL_A, &tp);
    DispGet(St, TRANS_DDI_FUNC_EDP, &fn); DispGet(St, PIPECONF_EDP, &pc);
    if (!(buf & DDI_BUF_ENABLE) || !(tp & TP_ENABLE) || !(fn & FUNC_ENABLE) || !(pc & PIPECONF_ENABLE)) {
        S("Display state: eDP pipeline not lit, nothing captured"); NL();
        St->Count = 0;
        return EFI_NOT_READY;
    }
    S("Display state captured: "); DC(St->Count); S(" registers"); NL();
    return EFI_SUCCESS;
}

// Native AUX write of len (1..16) DPCD bytes.
static EFI_STATUS DpcdWrite(DEV* D, UINT32 addr, const UINT8* in, UINTN len)
{
    for (UINTN defer = 0; defer < 7; defer++) {
        UINT8 tx[20];
        UINT8 rx[20];
        UINTN rn = 1;
        tx[0] = (UINT8)(0x80 | ((addr >> 16) & 0xF));
        tx[1] = (UINT8)(addr >> 8);
        tx[2] = (UINT8)addr;
        tx[3] = (UINT8)(len - 1);
        for (UINTN i = 0; i < len; i++) tx[4 + i] = in[i];
        EFI_STATUS St = AuxXfer(D, tx, 4 + len, rx, &rn);
        if (EFI_ERROR(St)) return St;
        UINT8 code = (UINT8)(rx[0] >> 4);
        if (code == 0x2) { D->BS->Stall(400); continue; }     // AUX_DEFER
        return code == 0 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    }
    return EFI_TIMEOUT;
}
static EFI_STATUS DpcdWrite1(DEV* D, UINT32 addr, UINT8 v) { return DpcdWrite(D, addr, &v, 1); }

// DDI buffer translation index for (voltage swing, pre-emphasis), as i915 translate_signal_level
// for the 10-entry SKL/KBL/CFL eDP table. -1 = combination not allowed.
static int LevelIndex(UINT8 vs, UINT8 pe)
{
    static const signed char t[4][4] = { {0, 1, 2, 3}, {4, 5, 6, -1}, {7, 8, -1, -1}, {9, -1, -1, -1} };
    return t[vs & 3][pe & 3];
}
static UINT8 PeMax(UINT8 vs) { return (UINT8)(3 - (vs & 3)); }

// Lane-set byte for DPCD 0x103..: swing, pre-emphasis and the "max reached" flags.
static UINT8 LaneSet(UINT8 vs, UINT8 pe)
{
    UINT8 b = (UINT8)(vs | (pe << 3));
    if (vs >= 3) b |= 0x04;
    if (pe >= PeMax(vs)) b |= 0x20;
    return b;
}

// Largest requested swing / pre-emphasis over the active lanes, then clamped to a legal pair.
static VOID NextLevel(const UINT8* adj, UINTN lanes, UINT8* vs, UINT8* pe)
{
    UINT8 v = 0, e = 0;
    for (UINTN i = 0; i < lanes; i++) {
        UINT8 b = (UINT8)(adj[i / 2] >> ((i & 1) * 4));
        if ((b & 3) > v) v = b & 3;
        if (((b >> 2) & 3) > e) e = (b >> 2) & 3;
    }
    if (e > PeMax(v)) e = PeMax(v);
    *vs = v; *pe = e;
}

// Lane status helpers (DPCD 0x202..0x205): per lane CR_DONE bit0, EQ_DONE bit1, SYMBOL_LOCK bit2.
static BOOLEAN LanesOk(const UINT8* st, UINTN lanes, UINT8 mask)
{
    for (UINTN i = 0; i < lanes; i++) {
        UINT8 b = (UINT8)(st[i / 2] >> ((i & 1) * 4));
        if ((b & mask) != mask) return FALSE;
    }
    return TRUE;
}

// DP link training, i915 intel_dp_link_training_clock_recovery() / _channel_equalization().
static EFI_STATUS TrainLink(DEV* D, UINT8 rate, UINTN lanes, BOOLEAN enh, BOOLEAN ssc,
                            UINT32 bufctl, _INT_Rep* R)
{
    UINT8 sink[16];
    UINT8 st[6];
    UINT8 vs = 0, pe = 0;
    BOOLEAN tps3 = FALSE, enhCap = FALSE;
    UINT32 tpEnh = enh ? TP_ENHANCED : 0;

    if (!EFI_ERROR(DpcdRead(D, 0x000, sink, 16))) {
        tps3 = (sink[2] & 0x40) != 0;
        enhCap = (sink[2] & 0x80) != 0;
    }
    if (!enhCap) { enh = FALSE; tpEnh = 0; }
    UINTN crInterval = 100, eqInterval = 400;
    if (sink[0x0E] & 0x7F) { crInterval = eqInterval = (UINTN)(sink[0x0E] & 0x7F) * 4000; }

    EFI_STATUS e = DpcdWrite1(D, 0x600, 0x01);                       // SET_POWER: D0
    if (EFI_ERROR(e)) { S("Link training: panel does not answer AUX writes"); NL(); return e; }
    DpcdWrite1(D, 0x100, rate);                                      // LINK_BW_SET
    DpcdWrite1(D, 0x101, (UINT8)(lanes | (enh ? 0x80 : 0)));         // LANE_COUNT_SET
    DpcdWrite1(D, 0x107, ssc ? 0x10 : 0x00);                         // DOWNSPREAD_CTRL
    DpcdWrite1(D, 0x108, 0x01);                                      // 8b/10b

    bufctl &= ~(DDI_BUF_ENABLE | DDI_BUF_TRANS_MASK);
    Wr(D, DP_TP_CTL_A, TP_ENABLE | tpEnh | TP_PAT1);
    Wr(D, DDI_BUF_CTL_A, bufctl | DDI_BUF_TRANS_SEL(LevelIndex(0, 0)) | DDI_BUF_ENABLE);
    D->BS->Stall(600);
    for (UINTN i = 0; i < 50 && (Rd(D, DDI_BUF_CTL_A) & DDI_BUF_IDLE); i++) D->BS->Stall(50);

    // ---- clock recovery ----
    BOOLEAN cr = FALSE;
    UINTN sameVs = 0;
    UINT8 prevVs = 0xFF;
    for (UINTN tries = 0; tries < 10 && !cr; tries++) {
        UINT8 cmd[5] = { 0x21, 0, 0, 0, 0 };                          // TPS1, scrambling disabled
        for (UINTN i = 0; i < lanes; i++) cmd[1 + i] = LaneSet(vs, pe);
        Wr(D, DDI_BUF_CTL_A, bufctl | DDI_BUF_TRANS_SEL(LevelIndex(vs, pe)) | DDI_BUF_ENABLE);
        e = (tries == 0) ? DpcdWrite(D, 0x102, cmd, 5) : DpcdWrite(D, 0x103, cmd + 1, 4);
        if (EFI_ERROR(e)) return e;
        D->BS->Stall(crInterval);
        e = DpcdRead(D, 0x202, st, 6);
        if (EFI_ERROR(e)) return e;
        if (LanesOk(st, lanes, 0x01)) { cr = TRUE; break; }
        UINT8 nv, ne;
        NextLevel(st + 4, lanes, &nv, &ne);
        if (nv == prevVs) { if (++sameVs >= 5) break; } else { sameVs = 0; prevVs = nv; }
        vs = nv; pe = ne;
    }
    if (!cr) { S("Link training: clock recovery failed"); NL(); return EFI_DEVICE_ERROR; }

    // ---- channel equalization ----
    BOOLEAN eq = FALSE;
    BOOLEAN useTps3 = tps3 && rate >= 0x14;
    Wr(D, DP_TP_CTL_A, TP_ENABLE | tpEnh | (useTps3 ? TP_PAT3 : TP_PAT2));
    for (UINTN tries = 0; tries < 6 && !eq; tries++) {
        UINT8 cmd[5] = { (UINT8)(useTps3 ? 0x23 : 0x22), 0, 0, 0, 0 };
        for (UINTN i = 0; i < lanes; i++) cmd[1 + i] = LaneSet(vs, pe);
        Wr(D, DDI_BUF_CTL_A, bufctl | DDI_BUF_TRANS_SEL(LevelIndex(vs, pe)) | DDI_BUF_ENABLE);
        e = (tries == 0) ? DpcdWrite(D, 0x102, cmd, 5) : DpcdWrite(D, 0x103, cmd + 1, 4);
        if (EFI_ERROR(e)) return e;
        D->BS->Stall(eqInterval);
        e = DpcdRead(D, 0x202, st, 6);
        if (EFI_ERROR(e)) return e;
        if (!LanesOk(st, lanes, 0x01)) { S("Link training: clock recovery lost during EQ"); NL(); return EFI_DEVICE_ERROR; }
        if (LanesOk(st, lanes, 0x07) && (st[2] & 0x01)) { eq = TRUE; break; }
        NextLevel(st + 4, lanes, &vs, &pe);
    }
    if (!eq) { S("Link training: equalization failed"); NL(); return EFI_DEVICE_ERROR; }
    S("Link trained: rate=0x"); HX(rate, 2); S(" lanes="); DC(lanes);
    S(" swing="); DC(vs); S(" preemph="); DC(pe); NL();
    return EFI_SUCCESS;
}

static VOID DispFail(DEV* D)
{
    Wr(D, DDI_BUF_CTL_A, Rd(D, DDI_BUF_CTL_A) & ~DDI_BUF_ENABLE);
    Wr(D, DP_TP_CTL_A, 0);
}

EFI_STATUS _INT_DispReplay(EFI_BOOT_SERVICES* BS, EFI_HANDLE Image, const _INT_DispState* St, _INT_Rep* R)
{
    EFI_PCI_IO_PROTOCOL* p = NULL;
    UINT32 c1, c2, cbuf, ctp, cht, cvt, cfn, cpc;
    EFI_STATUS st = PpsIgpu(BS, Image, &p, R);
    if (EFI_ERROR(st)) return st;
    if (!DispGet(St, DPLL_CTRL1, &c1) || !DispGet(St, DPLL_CTRL2, &c2) ||
        !DispGet(St, DDI_BUF_CTL_A, &cbuf) || !DispGet(St, DP_TP_CTL_A, &ctp) ||
        !DispGet(St, TRANS_HTOTAL_EDP, &cht) || !DispGet(St, TRANS_VTOTAL_EDP, &cvt) ||
        !DispGet(St, TRANS_DDI_FUNC_EDP, &cfn) || !DispGet(St, PIPECONF_EDP, &cpc))
        return EFI_NOT_FOUND;
    DEV d = { BS, p };

    UINT32 ps = Rd(&d, PP_STATUS), pw = Rd(&d, PWR_WELL_CTL2);
    if (ps == 0xFFFFFFFFu || pw == 0xFFFFFFFFu) return EFI_NOT_READY;
    if ((ps & (PP_ON | PP_SEQUENCE_MASK | PP_STATE_MASK)) != (PP_ON | PP_STATE_ON_IDLE)) {
        S("Display replay: panel is not powered, skipped"); NL(); return EFI_NOT_READY;
    }
    if ((pw & (PW_PW1_STATE | PW_DDI_AE_STATE)) != (PW_PW1_STATE | PW_DDI_AE_STATE)) {
        S("Display replay: power wells are down, skipped"); NL(); return EFI_NOT_READY;
    }
    if (Rd(&d, DDI_BUF_CTL_A) & DDI_BUF_ENABLE) {
        S("Display replay: DDI A already enabled, nothing to do"); NL(); return EFI_SUCCESS;
    }

    // 1. DPLL0 link-rate divider + DDI A clock routing, only the DPLL0 / port A fields.
    UINT32 cur = Rd(&d, DPLL_CTRL1);
    UINT32 n1 = (cur & ~0x3Fu) | (c1 & 0x3Fu);
    if (n1 != cur) Wr(&d, DPLL_CTRL1, n1);
    cur = Rd(&d, DPLL_CTRL2);
    UINT32 m2 = 0xFu | (1u << 15);                 // DDI A: override [0], clock select [3:1], clock off [15]
    UINT32 n2 = (cur & ~m2) | (c2 & m2);
    if (n2 != cur) Wr(&d, DPLL_CTRL2, n2);
    BOOLEAN lock = FALSE;
    for (UINTN i = 0; i < 100; i++) { if (Rd(&d, DPLL_STATUS) & 1u) { lock = TRUE; break; } BS->Stall(50); }
    if (!lock) { S("Display replay: DPLL0 not locked"); NL(); return EFI_DEVICE_ERROR; }

    // 2. DDI buffer translation table (voltage swing / pre-emphasis levels the firmware used).
    for (UINT32 i = 0; i < St->Count; i++)
        if (St->R[i].Off >= DDI_BUF_TRANS_A && St->R[i].Off < DDI_BUF_TRANS_A + 0x50)
            Wr(&d, St->R[i].Off, St->R[i].Val);

    // 3. Link parameters exactly as the firmware trained them.
    static const UINT8 rates[6] = { 0x14, 0x0A, 0x06, 0x0C, 0x08, 0x10 };   // DPLL_CTRL1 rate field -> DPCD
    UINT32 rf = (c1 >> 1) & 7;
    if (rf > 5) return EFI_UNSUPPORTED;
    UINTN lanes = ((cbuf >> 1) & 7) + 1;
    if (lanes != 1 && lanes != 2 && lanes != 4) return EFI_UNSUPPORTED;
    st = TrainLink(&d, rates[rf], lanes, (ctp & TP_ENHANCED) != 0, (c1 & (1u << 4)) != 0, cbuf, R);
    if (EFI_ERROR(st)) { DispFail(&d); return st; }

    // 4. Transcoder: timing, M/N, MSA, pipe size (no scaler / plane: the pipe is native size).
    for (UINT32 i = 0; i < St->Count; i++) {
        UINT32 o = St->R[i].Off;
        if ((o >= TRANS_HTOTAL_EDP && o <= 0x6F044) || o == TRANS_MSA_MISC_EDP) Wr(&d, o, St->R[i].Val);
    }
    UINT32 hact = (cht & 0xFFFFu), vact = (cvt & 0xFFFFu);                 // active - 1
    Wr(&d, PIPESRC_A, (hact << 16) | vact);
    Wr(&d, PIPE_BOTTOM_COLOR_A, 0xC00003FFu);                              // plain blue background

    // 5. Link idle, sink out of training, then transcoder function + pipe on.
    Wr(&d, DP_TP_CTL_A, TP_ENABLE | (ctp & TP_ENHANCED) | TP_IDLE);
    DpcdWrite1(&d, 0x102, 0x00);
    BS->Stall(100);
    Wr(&d, TRANS_DDI_FUNC_EDP, cfn);
    Wr(&d, PIPECONF_EDP, cpc);
    BOOLEAN pipe = FALSE;
    for (UINTN i = 0; i < 200; i++) { if (Rd(&d, PIPECONF_EDP) & (1u << 30)) { pipe = TRUE; break; } BS->Stall(100); }
    for (UINTN i = 0; i < 50; i++) { if (Rd(&d, DP_TP_STATUS_A) & TP_STATUS_IDLE_DONE) break; BS->Stall(100); }
    Wr(&d, DP_TP_CTL_A, TP_ENABLE | (ctp & TP_ENHANCED) | TP_NORMAL);
    if (!pipe) { S("Display replay: pipe did not start"); NL(); return EFI_DEVICE_ERROR; }

    // 6. Backlight (PCH PWM) as the firmware had it, then the panel's backlight enable.
    UINT32 f, du, b1;
    if (DispGet(St, BLC_PWM_FREQ, &f) && DispGet(St, BLC_PWM_DUTY, &du) && DispGet(St, BLC_PWM_CTL1, &b1)) {
        Wr(&d, BLC_PWM_FREQ, f);
        Wr(&d, BLC_PWM_DUTY, du);
        Wr(&d, BLC_PWM_CTL1, b1);
    }
    UINT32 pc = Rd(&d, PP_CONTROL);
    Wr(&d, PP_CONTROL, ((pc & 0xFFFFu) | EDP_BLC_ENABLE) | PP_UNLOCK);
    S("Display replay: eDP pipeline up (pipe A, eDP transcoder, link trained, backlight on)"); NL();
    return EFI_SUCCESS;
}