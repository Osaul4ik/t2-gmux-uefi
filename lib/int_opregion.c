#include "../include/int_vbt.h"
#include "../include/int_mem.h"
#include "../include/int_edp.h"
#include <efiprot.h>

#define S(x)  _INT_RepStr(R, (const CHAR8*)(x))
#define NL()  _INT_RepNl(R)
#define HX(v, d) _INT_RepHex(R, (v), (d))
#define DC(v) _INT_RepDec(R, (v))

#define IGPU_PCI_ASLS      0xFC
#define IGPU_PCI_BAR0      0x10
#define IGPU_DDI_BUF_CTL_A 0x64000   // GTTMMADR (BAR0) offset
#define DDI_A_4_LANES      (1u << 4)
#define OPREGION_ASLE_OFF  0x300   // mailbox 3 (ASLE); 0x100 is mailbox 1 (display lists)
#define ASLE_RVDA_OFF      186   // offset of rvda (u64) inside the ASLE struct (OpRegion+0x3BA)
#define ASLE_RVDS_OFF      194   // offset of rvds (u32) (OpRegion+0x3C2)

static BOOLEAN IsVbt(const UINT8* p)
{
    return p[0] == '$' && p[1] == 'V' && p[2] == 'B' && p[3] == 'T';
}


static EFI_PCI_IO_PROTOCOL* FindIgpu(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                     UINT32* IdOut, EFI_STATUS* StOut)
{
    EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;
    EFI_HANDLE* Handles = NULL;
    UINTN Count = 0;
    EFI_PCI_IO_PROTOCOL* Found = NULL;

    *StOut = BS->LocateHandleBuffer(ByProtocol, &pci_guid, NULL, &Count, &Handles);
    if (EFI_ERROR(*StOut))
        return NULL;

    for (UINTN i = 0; i < Count; i++) {
        EFI_PCI_IO_PROTOCOL* Pci;
        UINT32 w0 = 0, w8 = 0;

        if (EFI_ERROR(BS->OpenProtocol(Handles[i], &pci_guid, (VOID**)&Pci, ImageHandle,
                                       NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL)))
            continue;
        if (EFI_ERROR(Pci->Pci.Read(Pci, EfiPciIoWidthUint32, 0, 1, &w0)))
            continue;
        if ((w0 & 0xFFFF) != 0x8086)
            continue;
        if (EFI_ERROR(Pci->Pci.Read(Pci, EfiPciIoWidthUint32, 8, 1, &w8)))
            continue;
        if ((w8 >> 24) != 3)               // base class: display controller
            continue;

        Found = Pci;
        *IdOut = w0;
        break;
    }
    _INT_FreePool(BS, Handles);
    return Found;
}

EFI_PCI_IO_PROTOCOL* _INT_FindIgpu(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                   UINT32* IdOut, EFI_STATUS* StOut)
{
    return FindIgpu(BS, ImageHandle, IdOut, StOut);
}

EFI_STATUS _INT_WriteEspFile(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                             CHAR16* Name, const VOID* Data, UINTN Size)
{
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* Li = NULL;
    EFI_FILE_IO_INTERFACE* Fs = NULL;
    EFI_FILE_HANDLE Root = NULL, File = NULL;
    EFI_STATUS Status;

    Status = BS->HandleProtocol(ImageHandle, &li_guid, (VOID**)&Li);
    if (EFI_ERROR(Status) || !Li) return Status;

    Status = BS->HandleProtocol(Li->DeviceHandle, &fs_guid, (VOID**)&Fs);
    if (EFI_ERROR(Status) || !Fs) return Status;

    Status = Fs->OpenVolume(Fs, &Root);
    if (EFI_ERROR(Status)) return Status;

    // remove an older copy so the new file is not padded by stale bytes
    if (!EFI_ERROR(Root->Open(Root, &File, Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
        File->Delete(File);
        File = NULL;
    }

    Status = Root->Open(Root, &File, Name,
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(Status)) {
        Root->Close(Root);
        return Status;
    }

    UINTN Len = Size;
    Status = File->Write(File, &Len, (VOID*)Data);
    File->Flush(File);
    File->Close(File);
    Root->Close(Root);

    if (!EFI_ERROR(Status) && Len != Size)
        Status = EFI_DEVICE_ERROR;
    return Status;
}

EFI_STATUS _INT_ReadEspFile(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                            CHAR16* Name, VOID** Data, UINTN* Size)
{
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* Li = NULL;
    EFI_FILE_IO_INTERFACE* Fs = NULL;
    EFI_FILE_HANDLE Root = NULL, File = NULL;
    EFI_STATUS Status;
    UINT64 Len = 0;

    *Data = NULL;
    *Size = 0;

    Status = BS->HandleProtocol(ImageHandle, &li_guid, (VOID**)&Li);
    if (EFI_ERROR(Status) || !Li) return EFI_NOT_FOUND;
    Status = BS->HandleProtocol(Li->DeviceHandle, &fs_guid, (VOID**)&Fs);
    if (EFI_ERROR(Status) || !Fs) return EFI_NOT_FOUND;
    Status = Fs->OpenVolume(Fs, &Root);
    if (EFI_ERROR(Status)) return Status;

    Status = Root->Open(Root, &File, Name, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        Root->Close(Root);
        return Status;
    }

    // file size: seek to end, read position, seek back
    File->SetPosition(File, 0xFFFFFFFFFFFFFFFFULL);
    File->GetPosition(File, &Len);
    File->SetPosition(File, 0);

    if (Len == 0 || Len > 0x100000) {
        File->Close(File);
        Root->Close(Root);
        return EFI_BAD_BUFFER_SIZE;
    }

    VOID* Buf = _INT_AllocatePool(BS, (UINTN)Len);
    if (!Buf) {
        File->Close(File);
        Root->Close(Root);
        return EFI_OUT_OF_RESOURCES;
    }

    UINTN Rd = (UINTN)Len;
    Status = File->Read(File, &Rd, Buf);
    File->Close(File);
    Root->Close(Root);

    if (EFI_ERROR(Status) || Rd != (UINTN)Len) {
        _INT_FreePool(BS, Buf);
        return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;
    }

    *Data = Buf;
    *Size = Rd;
    return EFI_SUCCESS;
}

// Replace the (empty) VBT mailbox of the firmware OpRegion with the built-in VBT. Mailbox 4 lives at OpRegion+0x400 and is 6 KB.
#define OPREGION_VBT_OFF   0x400
#define OPREGION_VBT_MAX   0x1800

static EFI_STATUS InjectBuf(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                            const UINT8* v, UINTN Size, _INT_Rep* R)
{
    EFI_STATUS Status;
    UINT32 Id = 0;

    EFI_PCI_IO_PROTOCOL* Igpu = FindIgpu(BS, ImageHandle, &Id, &Status);
    if (!Igpu) {
        S("Inject: no Intel iGPU found"); NL();
        return EFI_NOT_FOUND;
    }

    UINT32 asls = 0;
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_ASLS, 1, &asls);
    if (asls == 0 || asls == 0xFFFFFFFF) {
        S("Inject: ASLS is 0, no OpRegion to write into"); NL();
        return EFI_NOT_FOUND;
    }

    UINT8* op = (UINT8*)(UINTN)asls;
    static const CHAR8 sig[] = "IntelGraphicsMem";
    for (UINTN i = 0; sig[i]; i++) {
        if (op[i] != (UINT8)sig[i]) {
            S("Inject: OpRegion signature mismatch"); NL();
            return EFI_NOT_FOUND;
        }
    }

    UINTN opsize = (UINTN)_INT_Rd32(op + 16) * 1024;
    if (opsize < OPREGION_VBT_OFF + 0x100) {
        S("Inject: OpRegion too small ("); DC(opsize); S(" bytes)"); NL();
        return EFI_BUFFER_TOO_SMALL;
    }

    // The Intel driver prefers a valid VBT behind ASLE.rvda/rvds over mailbox 4, so a non-empty
    // rvda makes this injection invisible to it. It is cleared after the VBT is written (below).
    UINT64 rvda = _INT_Rd64(op + OPREGION_ASLE_OFF + ASLE_RVDA_OFF);
    UINT32 rvds = _INT_Rd32(op + OPREGION_ASLE_OFF + ASLE_RVDS_OFF);
    S("Inject: ASLE rvda=0x"); HX(rvda, 16); S(" rvds="); DC(rvds); NL();

    if (Size < 48 || !IsVbt(v)) {
        S("Inject: data is not a VBT (no $VBT signature)"); NL();
        return EFI_COMPROMISED_DATA;
    }

    UINTN vsz = _INT_Rd16(v + 24);
    if (vsz == 0 || vsz > Size) vsz = Size;
    UINTN room = opsize - OPREGION_VBT_OFF;
    if (room > OPREGION_VBT_MAX) room = OPREGION_VBT_MAX;
    if (vsz > room) {
        S("Inject: VBT is "); DC(vsz); S(" bytes, mailbox only has "); DC(room); NL();
        return EFI_BUFFER_TOO_SMALL;
    }

    // clear the whole mailbox, then copy the VBT in
    for (UINTN i = 0; i < room; i++) op[OPREGION_VBT_OFF + i] = 0;
    for (UINTN i = 0; i < vsz; i++)  op[OPREGION_VBT_OFF + i] = v[i];

    // read back to make sure the write stuck (memory may be read-only/remapped)
    BOOLEAN ok = TRUE;
    for (UINTN i = 0; i < vsz; i++)
        if (op[OPREGION_VBT_OFF + i] != v[i]) { ok = FALSE; break; }

    S("Inject: wrote "); DC(vsz); S(" bytes at OpRegion+0x400, readback ");
    S(ok ? "OK" : "MISMATCH (write did not stick)"); NL();

    // A VBT behind rvda/rvds would be preferred over the one just written: clear both so the
    // driver falls back to mailbox 4.
    if (ok && (rvda != 0 || rvds != 0)) {
        UINT8* rp = op + OPREGION_ASLE_OFF + ASLE_RVDA_OFF;   // rvda (8) + rvds (4) are contiguous
        for (UINTN i = 0; i < 12; i++) rp[i] = 0;
        BOOLEAN cleared = (_INT_Rd64(op + OPREGION_ASLE_OFF + ASLE_RVDA_OFF) == 0 &&
                           _INT_Rd32(op + OPREGION_ASLE_OFF + ASLE_RVDS_OFF) == 0);
        S("Inject: rvda/rvds were set, cleared: "); S(cleared ? "OK" : "FAILED (write did not stick)"); NL();
    }

    return ok ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

// VBT built into the loader (lib/vbt_base.c); panel specifics are patched in afterwards.
EFI_STATUS _INT_InjectVbtBuiltin(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle, _INT_Rep* R)
{
    return InjectBuf(BS, ImageHandle, _INT_VbtBase, _INT_VbtBaseSize, R);
}

// DDI A max lane count, as i915 sees it on display gen < 11 (T2 Macs with gmux are
// all CFL/WHL-class, gen 9.5): intel_ddi_max_lanes() reads DDI_BUF_CTL(A) bit 4
// (DDI_A_4_LANES) at driver init; clear = only 2 lanes, so a 4-lane eDP link can
// never be computed no matter what the VBT says. Apple's firmware sets the bit
// only when it lights the panel from the iGPU at boot. When the dGPU is the boot
// GPU (our case) it stays clear - the t2linux patch "i915: 4 lane quirk for
// mbp15,1" (QUIRK_DDI_A_FORCE_4_LANES) exists for exactly this. The driver loads
// after us and cannot be patched, so set the bit here, like the firmware would.
EFI_STATUS _INT_IgpuForceDdiA4Lanes(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                    _INT_Rep* R)
{
    EFI_STATUS Status;
    UINT32 Id = 0;
    UINT32 cmd = 0, bar0 = 0, bar1 = 0, v = 0;

    EFI_PCI_IO_PROTOCOL* Igpu = FindIgpu(BS, ImageHandle, &Id, &Status);
    if (!Igpu) {
        S("DDI A: no Intel iGPU found"); NL();
        return EFI_NOT_FOUND;
    }

    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, 4, 1, &cmd);
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_BAR0, 1, &bar0);
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_BAR0 + 4, 1, &bar1);

    // BAR0 must be an assigned memory BAR before anything decodes it.
    UINT64 base = (UINT64)(bar0 & ~0xFu);
    if ((bar0 & 0x6) == 0x4)
        base |= (UINT64)bar1 << 32;
    if ((bar0 & 1) || base == 0) {
        S("DDI A: iGPU BAR0 not assigned (BAR0=0x"); HX(bar0, 8); S("), skipped"); NL();
        return EFI_NOT_READY;
    }

    if (!(cmd & 0x2)) {
        Status = Igpu->Attributes(Igpu, EfiPciIoAttributeOperationEnable,
                                  EFI_PCI_IO_ATTRIBUTE_MEMORY, NULL);
        S("DDI A: iGPU memory decode was off, enable status=0x"); HX(Status, 16); NL();
        if (EFI_ERROR(Status))
            return Status;
    }

    Status = Igpu->Mem.Read(Igpu, EfiPciIoWidthUint32, 0, IGPU_DDI_BUF_CTL_A, 1, &v);
    if (EFI_ERROR(Status)) {
        S("DDI A: DDI_BUF_CTL_A read failed, status=0x"); HX(Status, 16); NL();
        return Status;
    }
    S("DDI A: DDI_BUF_CTL_A = 0x"); HX(v, 8); NL();
    if (v == 0xFFFFFFFF) {
        S("DDI A: reads all ones (display power well down?), not writing"); NL();
        return EFI_NOT_READY;
    }
    if (v & DDI_A_4_LANES) {
        S("DDI A: DDI_A_4_LANES already set"); NL();
        return EFI_SUCCESS;
    }

    v |= DDI_A_4_LANES;
    Status = Igpu->Mem.Write(Igpu, EfiPciIoWidthUint32, 0, IGPU_DDI_BUF_CTL_A, 1, &v);
    if (EFI_ERROR(Status)) {
        S("DDI A: DDI_BUF_CTL_A write failed, status=0x"); HX(Status, 16); NL();
        return Status;
    }

    UINT32 rb = 0;
    Igpu->Mem.Read(Igpu, EfiPciIoWidthUint32, 0, IGPU_DDI_BUF_CTL_A, 1, &rb);
    S("DDI A: set DDI_A_4_LANES, readback 0x"); HX(rb, 8);
    S((rb & DDI_A_4_LANES) ? " OK" : " MISMATCH (bit did not stick)"); NL();
    return (rb & DDI_A_4_LANES) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}


// ---------------------------------------------------------------------------
// Substitute the panel timing in the injected VBT with the EDID the firmware
// published for the dGPU. Apple's EFI only provides panel data for the Radeon
// (its GOP handle carries the EDID protocol); the iGPU OpRegion VBT mailbox is
// empty, so the VBT built from the template has to be given the dGPU's panel.
// ---------------------------------------------------------------------------
#define BDB_LFP_OPTIONS_ID   40
#define BDB_LFP_DATA_PTRS_ID 41

static BOOLEAN EdidLooksInternal(const UINT8* e, UINTN n)
{
    static const UINT8 hdr[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    UINT8 sum = 0;
    if (!e || n < 128) return FALSE;
    for (UINTN i = 0; i < 8; i++) if (e[i] != hdr[i]) return FALSE;
    for (UINTN i = 0; i < 128; i++) sum = (UINT8)(sum + e[i]);
    if (sum != 0) return FALSE;
    if (e[8] != 0x06 || e[9] != 0x10) return FALSE;      // manufacturer "APP"
    if (e[54] == 0 && e[55] == 0) return FALSE;          // first descriptor must be a DTD
    return TRUE;
}

static const UINT8* FindFirmwareEdid(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                     UINTN* SizeOut, const char** SrcOut)
{
    EFI_GUID guids[2] = { EFI_EDID_ACTIVE_PROTOCOL_GUID, EFI_EDID_DISCOVERED_PROTOCOL_GUID };
    static const char* names[2] = { "EDID active", "EDID discovered" };
    (void)ImageHandle;

    for (UINTN g = 0; g < 2; g++) {
        EFI_HANDLE* Handles = NULL;
        UINTN Count = 0;
        if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &guids[g], NULL, &Count, &Handles)))
            continue;
        for (UINTN i = 0; i < Count; i++) {
            EFI_EDID_ACTIVE_PROTOCOL* P = NULL;      // same layout as the discovered one
            if (EFI_ERROR(BS->HandleProtocol(Handles[i], &guids[g], (VOID**)&P)) || !P)
                continue;
            if (EdidLooksInternal(P->Edid, P->SizeOfEdid)) {
                *SizeOut = P->SizeOfEdid;
                *SrcOut = names[g];
                return P->Edid;
            }
        }
    }
    return NULL;
}

static EFI_STATUS ApplyEdidToVbt(UINT8* v, const UINT8* edid, UINTN EdidSize,
                                 const char* Src, _INT_Rep* R);

EFI_STATUS _INT_VbtApplyFirmwareEdid(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                     _INT_Rep* R)
{
    EFI_STATUS Status;
    UINT32 Id = 0;
    UINTN EdidSize = 0;
    const char* Src = NULL;

    EFI_PCI_IO_PROTOCOL* Igpu = FindIgpu(BS, ImageHandle, &Id, &Status);
    if (!Igpu) { S("EDID subst: no Intel iGPU found"); NL(); return EFI_NOT_FOUND; }

    UINT32 asls = 0;
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_ASLS, 1, &asls);
    if (asls == 0 || asls == 0xFFFFFFFF) { S("EDID subst: no OpRegion"); NL(); return EFI_NOT_FOUND; }

    UINT8* v = (UINT8*)(UINTN)asls + OPREGION_VBT_OFF;
    if (!IsVbt(v)) { S("EDID subst: no VBT in mailbox 4 (inject first)"); NL(); return EFI_NOT_FOUND; }

    const UINT8* edid = FindFirmwareEdid(BS, ImageHandle, &EdidSize, &Src);
    if (!edid) {
        S("EDID subst: firmware exposes no internal-panel (APP) EDID, VBT timing kept"); NL();
        return EFI_NOT_FOUND;
    }

    return ApplyEdidToVbt(v, edid, EdidSize, Src, R);
}

static EFI_STATUS ApplyEdidToVbt(UINT8* v, const UINT8* edid, UINTN EdidSize,
                                 const char* Src, _INT_Rep* R)
{
    UINTN vsz = _INT_Rd16(v + 24);
    UINTN bdb = _INT_Rd32(v + 28);
    if (vsz < 64 || vsz > OPREGION_VBT_MAX || bdb + 22 > vsz) {
        S("EDID subst: bad VBT header"); NL();
        return EFI_COMPROMISED_DATA;
    }
    UINTN hdr = _INT_Rd16(v + bdb + 18);
    UINTN bend = bdb + _INT_Rd16(v + bdb + 20);
    if (bend > vsz) bend = vsz;

    // locate BDB 40 (panel index) and BDB 41 (LFP data pointers)
    UINTN o40 = 0, o41 = 0, s41 = 0;
    for (UINTN pos = bdb + hdr; pos + 3 <= bend; ) {
        UINT8 bid = v[pos];
        UINTN bsz = _INT_Rd16(v + pos + 1);
        if (pos + 3 + bsz > bend) break;
        if (bid == BDB_LFP_OPTIONS_ID && !o40) o40 = pos + 3;
        if (bid == BDB_LFP_DATA_PTRS_ID && !o41) { o41 = pos + 3; s41 = bsz; }
        pos += 3 + bsz;
    }
    if (!o40 || !o41) { S("EDID subst: VBT lacks BDB 40/41"); NL(); return EFI_NOT_FOUND; }

    UINTN panel = v[o40];
    if (panel >= 16) panel = 0;
    UINTN e = o41 + 1 + panel * 9;
    if (e + 6 > o41 + s41) { S("EDID subst: BDB 41 too short"); NL(); return EFI_COMPROMISED_DATA; }

    UINTN fp = bdb + _INT_Rd16(v + e);
    UINTN dv = bdb + _INT_Rd16(v + e + 3);
    if (fp + 4 > vsz || dv + 18 > vsz) { S("EDID subst: timing offsets outside VBT"); NL(); return EFI_COMPROMISED_DATA; }

    const UINT8* d = edid + 54;
    UINT16 hact = (UINT16)(d[2] | ((d[4] >> 4) << 8));
    UINT16 vact = (UINT16)(d[5] | ((d[7] >> 4) << 8));
    UINT32 oldclk = _INT_Rd16(v + dv);
    UINT32 newclk = _INT_Rd16(d);

    v[fp + 0] = (UINT8)(hact & 0xFF); v[fp + 1] = (UINT8)(hact >> 8);
    v[fp + 2] = (UINT8)(vact & 0xFF); v[fp + 3] = (UINT8)(vact >> 8);
    for (UINTN i = 0; i < 18; i++) v[dv + i] = d[i];

    // whole VBT must sum to zero
    v[26] = 0;
    UINT8 sum = 0;
    for (UINTN i = 0; i < vsz; i++) sum = (UINT8)(sum + v[i]);
    v[26] = (UINT8)(0 - sum);

    // read back
    BOOLEAN ok = TRUE;
    for (UINTN i = 0; i < 18; i++) if (v[dv + i] != d[i]) { ok = FALSE; break; }

    S("EDID subst: "); S(Src); S(" ("); DC(EdidSize); S(" bytes) -> VBT panel "); DC(panel);
    S(": "); DC(hact); S("x"); DC(vact); S(", clock "); DC(oldclk * 10); S(" -> "); DC(newclk * 10);
    S(" kHz, readback "); S(ok ? "OK" : "MISMATCH"); NL();
    return ok ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}
// ---------------------------------------------------------------------------
// Mailbox 4 helpers for the built-in VBT path
// ---------------------------------------------------------------------------
static UINT8* MailboxVbt(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle, BOOLEAN NeedVbt,
                         UINTN* RoomOut, _INT_Rep* R)
{
    EFI_STATUS Status;
    UINT32 Id = 0, asls = 0;
    EFI_PCI_IO_PROTOCOL* Igpu = FindIgpu(BS, ImageHandle, &Id, &Status);
    if (!Igpu) { S("VBT: no Intel iGPU found"); NL(); return NULL; }
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_ASLS, 1, &asls);
    if (asls == 0 || asls == 0xFFFFFFFF) { S("VBT: no OpRegion"); NL(); return NULL; }
    UINT8* op = (UINT8*)(UINTN)asls;
    UINTN opsize = (UINTN)_INT_Rd32(op + 16) * 1024;
    if (opsize < OPREGION_VBT_OFF + 0x100) { S("VBT: OpRegion too small"); NL(); return NULL; }
    UINTN room = opsize - OPREGION_VBT_OFF;
    if (room > OPREGION_VBT_MAX) room = OPREGION_VBT_MAX;
    *RoomOut = room;
    UINT8* v = op + OPREGION_VBT_OFF;
    if (NeedVbt && !IsVbt(v)) { S("VBT: no VBT in mailbox 4 (inject first)"); NL(); return NULL; }
    return v;
}

EFI_STATUS _INT_VbtApplyEdidBuf(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                const UINT8* Edid, UINTN Size, const char* Src, _INT_Rep* R)
{
    UINTN room = 0;
    UINT8* v = MailboxVbt(BS, ImageHandle, TRUE, &room, R);
    if (!v) return EFI_NOT_FOUND;
    return ApplyEdidToVbt(v, Edid, Size, Src, R);
}

EFI_STATUS _INT_VbtApplyDpcd(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                             const _INT_EdpCaps* C, _INT_Rep* R)
{
    UINTN room = 0;
    UINT8* v = MailboxVbt(BS, ImageHandle, TRUE, &room, R);
    if (!v) return EFI_NOT_FOUND;
    if (!C->Valid) { S("VBT link: no DPCD data, built-in defaults kept"); NL(); return EFI_NOT_READY; }
    UINTN vsz = _INT_Rd16(v + 24);
    // PSR stays off even when the panel reports it (DPCD 0x070): with PSR the Intel
    // driver makes the T2 panel flicker.
    if (C->PsrSupport) {
        S("VBT link: panel reports PSR (DPCD 0x070 = "); DC(C->PsrSupport);
        S("), keeping it disabled in the VBT"); NL();
    }
    if (!_INT_VbtSetLink(v, vsz < room ? vsz : room, C->VbtRate, C->VbtLanes,
                         FALSE, R)) {
        S("VBT link: patch failed (unexpected VBT layout)"); NL();
        return EFI_COMPROMISED_DATA;
    }
    return EFI_SUCCESS;
}

EFI_STATUS _INT_VbtClearMailbox(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle, _INT_Rep* R)
{
    UINTN room = 0;
    UINT8* v = MailboxVbt(BS, ImageHandle, FALSE, &room, R);
    if (!v) return EFI_NOT_FOUND;
    for (UINTN i = 0; i < room; i++) v[i] = 0;
    S("VBT: mailbox 4 cleared again"); NL();
    return EFI_SUCCESS;
}