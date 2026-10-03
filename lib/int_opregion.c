#include "../include/int_vbt.h"
#include "../include/int_mem.h"
#include <efiprot.h>

#define S(x)  _INT_RepStr(R, (const CHAR8*)(x))
#define NL()  _INT_RepNl(R)
#define HX(v, d) _INT_RepHex(R, (v), (d))
#define DC(v) _INT_RepDec(R, (v))

#define IGPU_PCI_ASLS      0xFC
#define OPREGION_ASLE_OFF  0x100
#define ASLE_RVDA_OFF      130   // offset of rvda (u64) inside the ASLE struct
#define ASLE_RVDS_OFF      138   // offset of rvds (u32)

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

EFI_STATUS _INT_InspectIgpuOpRegion(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                    _INT_Rep* R, _INT_VbtInfo* Info)
{
    EFI_STATUS Status;
    UINT32 Id = 0;

    _INT_memset(Info, 0, sizeof(*Info));
    Info->PanelType = 0xFF;

    EFI_PCI_IO_PROTOCOL* Igpu = FindIgpu(BS, ImageHandle, &Id, &Status);
    if (!Igpu) {
        if (EFI_ERROR(Status)) {
            S("PciIo locate failed: 0x"); HX(Status, 16); NL();
        } else {
            S("No Intel display controller found via PciIo"); NL();
        }
        return EFI_NOT_FOUND;
    }

    Info->IgpuFound = TRUE;
    Info->IgpuDevId = (UINT16)(Id >> 16);

    UINTN seg = 0, bus = 0, dev = 0, fn = 0;
    Igpu->GetLocation(Igpu, &seg, &bus, &dev, &fn);

    UINT32 cmd = 0, asls = 0, subsys = 0;
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, 4, 1, &cmd);
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, 0x2C, 1, &subsys);
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_ASLS, 1, &asls);
    Info->Asls = asls;

    S("Intel iGPU: "); HX(Id & 0xFFFF, 4); S(":"); HX(Id >> 16, 4);
    S(" at "); DC(bus); S(":"); DC(dev); S("."); DC(fn);
    S("  command="); HX(cmd & 0xFFFF, 4);
    S("  subsystem="); HX(subsys & 0xFFFF, 4); S(":"); HX(subsys >> 16, 4); NL();
    S("ASLS (PCI 0xFC) = 0x"); HX(asls, 8); NL();

    if (asls == 0 || asls == 0xFFFFFFFF) {
        S("No OpRegion pointer -> the Intel driver gets no VBT from firmware."); NL();
        return EFI_NOT_FOUND;
    }

    const UINT8* op = (const UINT8*)(UINTN)asls;   // identity mapped in Boot Services
    static const CHAR8 sig[] = "IntelGraphicsMem";
    for (UINTN i = 0; sig[i]; i++) {
        if (op[i] != (UINT8)sig[i]) {
            S("OpRegion signature mismatch at 0x"); HX(asls, 8); S(": ");
            _INT_RepBytes(R, op, 16); NL();
            return EFI_NOT_FOUND;
        }
    }

    UINT32 opkb = _INT_Rd32(op + 16);
    UINT32 over = _INT_Rd32(op + 20);
    UINT32 mbox = _INT_Rd32(op + 0x58);
    UINTN opsize = (UINTN)opkb * 1024;
    if (opsize < 0x2000)  opsize = 0x2000;
    if (opsize > 0x10000) opsize = 0x10000;

    Info->OpRegionOk = TRUE;
    Info->OpVer = over;
    Info->OpRegion = op;
    Info->OpSize = opsize;

    S("OpRegion: size="); DC(opkb); S("KB  version=");
    DC(over >> 24); S("."); DC((over >> 16) & 0xFF); S("."); DC((over >> 8) & 0xFF);
    S("  mailboxes=0x"); HX(mbox, 8); NL();

    S("  firmware ver: ");
    for (UINTN i = 0; i < 32 && op[24 + i]; i++) { CHAR8 t[2] = { (CHAR8)op[24 + i], 0 }; S(t); }
    NL();

    // 1) VBT via ASLE.rvda (OpRegion >= 2.1: relative to OpRegion base)
    const UINT8* vbt = NULL;
    UINTN vbt_avail = 0;
    UINT64 rvda = _INT_Rd64(op + OPREGION_ASLE_OFF + ASLE_RVDA_OFF);
    UINT32 rvds = _INT_Rd32(op + OPREGION_ASLE_OFF + ASLE_RVDS_OFF);
    S("ASLE rvda=0x"); HX(rvda, 16); S(" rvds="); DC(rvds); NL();

    if (rvda != 0 && rvds >= 48 && rvds <= 0x10000) {
        BOOLEAN ge21 = (over >> 24) > 2 || ((over >> 24) == 2 && ((over >> 16) & 0xFF) >= 1);
        UINT64 addr = ge21 ? (UINT64)asls + rvda : rvda;
        if (addr < 0x100000000ULL && IsVbt((const UINT8*)(UINTN)addr)) {
            vbt = (const UINT8*)(UINTN)addr;
            vbt_avail = rvds;
            Info->VbtFromRvda = TRUE;
            S("VBT located through rvda at 0x"); HX(addr, 8); NL();
        }
    }

    // 2) fallback: scan the OpRegion mailbox 4 area for "$VBT"
    if (!vbt) {
        for (UINTN off = 0x100; off + 48 <= opsize; off += 4) {
            if (IsVbt(op + off)) {
                vbt = op + off;
                vbt_avail = opsize - off;
                S("VBT located by scan at OpRegion+0x"); HX(off, 4); NL();
                break;
            }
        }
    }

    if (!vbt) {
        S("VBT not found in OpRegion."); NL();
        return EFI_NOT_FOUND;
    }

    _INT_VbtParse(vbt, vbt_avail, R, Info);

    UINT16 vs = _INT_Rd16(vbt + 24);
    Info->Vbt = vbt;
    Info->VbtSize = (vs && vs <= vbt_avail) ? vs : vbt_avail;
    return EFI_SUCCESS;
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

// Replace the (empty) VBT mailbox of the firmware OpRegion with a VBT file
// from the ESP. Mailbox 4 lives at OpRegion+0x400 and is 6 KB.
#define OPREGION_VBT_OFF   0x400
#define OPREGION_VBT_MAX   0x1800

EFI_STATUS _INT_InjectVbt(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                          CHAR16* Name, _INT_Rep* R)
{
    EFI_STATUS Status;
    UINT32 Id = 0;
    VOID* Data = NULL;
    UINTN Size = 0;

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

    Status = _INT_ReadEspFile(BS, ImageHandle, Name, &Data, &Size);
    if (EFI_ERROR(Status)) {
        S("Inject: cannot read VBT file from ESP, status=0x"); HX(Status, 16); NL();
        return Status;
    }

    UINT8* v = (UINT8*)Data;
    if (Size < 48 || !IsVbt(v)) {
        S("Inject: file is not a VBT (no $VBT signature)"); NL();
        _INT_FreePool(BS, Data);
        return EFI_COMPROMISED_DATA;
    }

    UINTN vsz = _INT_Rd16(v + 24);
    if (vsz == 0 || vsz > Size) vsz = Size;
    UINTN room = opsize - OPREGION_VBT_OFF;
    if (room > OPREGION_VBT_MAX) room = OPREGION_VBT_MAX;
    if (vsz > room) {
        S("Inject: VBT is "); DC(vsz); S(" bytes, mailbox only has "); DC(room); NL();
        _INT_FreePool(BS, Data);
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

    _INT_FreePool(BS, Data);
    return ok ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}
