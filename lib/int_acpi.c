#include "../include/int_acpi.h"
#include "../include/int_mem.h"
#include <efiprot.h>

#define S(x)  _INT_RepStr(R, (const CHAR8*)(x))
#define NL()  _INT_RepNl(R)
#define HX(v, d) _INT_RepHex(R, (v), (d))
#define DC(v) _INT_RepDec(R, (v))

#define ACPI_HDR_SIZE   36
#define ACPI_HDR_LEN    4
#define ACPI_HDR_CSUM   9
#define ACPI_HDR_OEMTID 16

#pragma pack(1)
typedef struct {
    CHAR8   Signature[8];
    UINT8   Checksum;
    CHAR8   OemId[6];
    UINT8   Revision;
    UINT32  RsdtAddress;
    UINT32  Length;
    UINT64  XsdtAddress;
    UINT8   ExtChecksum;
    UINT8   Reserved[3];
} ACPI_RSDP;
#pragma pack()

static BOOLEAN GuidEq(const EFI_GUID* a, const EFI_GUID* b)
{
    const UINT8* x = (const UINT8*)a;
    const UINT8* y = (const UINT8*)b;
    for (UINTN i = 0; i < sizeof(EFI_GUID); i++)
        if (x[i] != y[i]) return FALSE;
    return TRUE;
}

static UINT8 Sum8(const UINT8* p, UINTN n)
{
    UINT8 s = 0;
    for (UINTN i = 0; i < n; i++) s = (UINT8)(s + p[i]);
    return s;
}

static BOOLEAN SigEq(const UINT8* p, const char* s, UINTN n)
{
    for (UINTN i = 0; i < n; i++)
        if (p[i] != (UINT8)s[i]) return FALSE;
    return TRUE;
}

// Locate "_BCM" (AML: 0x14 <PkgLength 1..2 bytes> '_BCM'). Returns the offset of the
// name, or 0. *Count = number of raw "_BCM" occurrences in the table.
static UINTN FindBcm(const UINT8* t, UINTN len, const char* nm, UINTN* Count)
{
    UINTN pos = 0;
    *Count = 0;
    for (UINTN i = ACPI_HDR_SIZE; i + 4 <= len; i++) {
        if (SigEq(t + i, nm, 4)) {
            (*Count)++;
            if (i >= 3 && (t[i - 2] == 0x14 || t[i - 3] == 0x14))
                pos = i;
        }
    }
    return pos;
}

// Does the file contain the 4-byte name anywhere (used to see if the SSDT wants XWAK).
static BOOLEAN HasName(const UINT8* t, UINTN len, const char* nm)
{
    for (UINTN i = 0; i + 4 <= len; i++)
        if (SigEq(t + i, nm, 4)) return TRUE;
    return FALSE;
}

// DSDT address from the FADT ("FACP"): X_Dsdt (+140) if present, else Dsdt (+40).
static UINT8* FindDsdt(UINT8* Xsdt, UINTN N)
{
    for (UINTN i = 0; i < N; i++) {
        UINT8* T = (UINT8*)(UINTN)_INT_Rd64(Xsdt + ACPI_HDR_SIZE + i * 8);
        if (!T || !SigEq(T, "FACP", 4)) continue;
        UINT32 L = _INT_Rd32(T + ACPI_HDR_LEN);
        UINT64 D = 0;
        if (L >= 148) D = _INT_Rd64(T + 140);
        if (D == 0 && L >= 44) D = _INT_Rd32(T + 40);
        return (UINT8*)(UINTN)D;
    }
    return NULL;
}

EFI_STATUS _INT_AcpiApplyPatch(EFI_BOOT_SERVICES* BS, EFI_SYSTEM_TABLE* ST,
                               EFI_HANDLE ImageHandle, CHAR16* Name, _INT_Rep* R)
{
    EFI_STATUS Status;
    VOID* FileData = NULL;
    UINTN FileSize = 0;
    EFI_GUID acpi20 = { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };  // ACPI_20_TABLE_GUID 8868e871-e4f1-11d3-bc22-0080c73c8881

    S("ACPI patch: "); NL();

    Status = _INT_ReadEspFile(BS, ImageHandle, Name, &FileData, &FileSize);
    if (EFI_ERROR(Status)) {
        S("  file not found / unreadable, skipped (status "); HX(Status, 8); S(")"); NL();
        return Status;
    }

    // ---- 1. validate the SSDT file ----
    UINT8* File = (UINT8*)FileData;
    if (FileSize < ACPI_HDR_SIZE || !SigEq(File, "SSDT", 4) ||
        _INT_Rd32(File + ACPI_HDR_LEN) != (UINT32)FileSize || Sum8(File, FileSize) != 0) {
        S("  bad SSDT file (signature / length / checksum)"); NL();
        _INT_FreePool(BS, FileData);
        return EFI_COMPROMISED_DATA;
    }
    S("  file ok, "); DC(FileSize); S(" bytes"); NL();

    // ---- 2. RSDP and XSDT ----
    ACPI_RSDP* Rsdp = NULL;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        if (GuidEq(&ST->ConfigurationTable[i].VendorGuid, &acpi20)) {
            Rsdp = (ACPI_RSDP*)ST->ConfigurationTable[i].VendorTable;
            break;
        }
    }
    if (!Rsdp || !SigEq((const UINT8*)Rsdp->Signature, "RSD PTR ", 8) ||
        Rsdp->Revision < 2 || Rsdp->XsdtAddress == 0) {
        S("  no ACPI 2.0 RSDP / XSDT"); NL();
        _INT_FreePool(BS, FileData);
        return EFI_UNSUPPORTED;
    }

    UINT8* Xsdt = (UINT8*)(UINTN)Rsdp->XsdtAddress;
    UINT32 XLen = _INT_Rd32(Xsdt + ACPI_HDR_LEN);
    if (!SigEq(Xsdt, "XSDT", 4) || XLen < ACPI_HDR_SIZE || Sum8(Xsdt, XLen) != 0) {
        S("  XSDT invalid"); NL();
        _INT_FreePool(BS, FileData);
        return EFI_COMPROMISED_DATA;
    }
    UINTN N = (XLen - ACPI_HDR_SIZE) / 8;
    S("  XSDT at "); HX((UINT64)(UINTN)Xsdt, 8); S(", entries "); DC(N); NL();

    // ---- 3. find SaSsdt, check we are not installed already ----
    UINT8* Sa = NULL;
    for (UINTN i = 0; i < N; i++) {
        UINT8* T = (UINT8*)(UINTN)_INT_Rd64(Xsdt + ACPI_HDR_SIZE + i * 8);
        if (!T || !SigEq(T, "SSDT", 4)) continue;
        if (SigEq(T + ACPI_HDR_OEMTID, (const char*)(File + ACPI_HDR_OEMTID), 8) &&
            _INT_Rd32(T + ACPI_HDR_LEN) == (UINT32)FileSize) {
            S("  already installed (same OEM table id), skipped"); NL();
            _INT_FreePool(BS, FileData);
            return EFI_ALREADY_STARTED;
        }
        if (SigEq(T + ACPI_HDR_OEMTID, "SaSsdt", 6))
            Sa = T;
    }
    if (!Sa) {
        S("  SaSsdt not found in XSDT"); NL();
        _INT_FreePool(BS, FileData);
        return EFI_NOT_FOUND;
    }
    UINT32 SaLen = _INT_Rd32(Sa + ACPI_HDR_LEN);
    UINTN Cnt = 0, CntX = 0;
    UINTN Off = FindBcm(Sa, SaLen, "_BCM", &Cnt);
    FindBcm(Sa, SaLen, "XBCM", &CntX);
    S("  SaSsdt at "); HX((UINT64)(UINTN)Sa, 8); S(" len "); DC(SaLen);
    S(", _BCM x"); DC(Cnt); S(" at +"); HX(Off, 4); S(", XBCM x"); DC(CntX); NL();
    if (Cnt != 1 || Off == 0 || CntX != 0) {
        S("  unexpected SaSsdt layout, not patched"); NL();
        _INT_FreePool(BS, FileData);
        return EFI_UNSUPPORTED;
    }

    // ---- 3b. DSDT renames. The SSDT may replace _WAK (re-route gmux after resume) and
    //          _PTS (remember whether the Radeon is off before sleep); each one needs the
    //          DSDT original renamed to XWAK / XPTS. Only done for names the SSDT mentions. ----
    BOOLEAN WantWak = HasName(File, FileSize, "XWAK");
    BOOLEAN WantPts = HasName(File, FileSize, "XPTS");
    UINT8* Ds = NULL;
    UINT32 DsLen = 0;
    UINTN OffW = 0, OffP = 0;
    if (WantWak || WantPts) {
        UINTN CntW = 0, CntWX = 0, CntP = 0, CntPX = 0;
        Ds = FindDsdt(Xsdt, N);
        if (!Ds || !SigEq(Ds, "DSDT", 4)) {
            S("  DSDT not found via FADT, not patched"); NL();
            _INT_FreePool(BS, FileData);
            return EFI_NOT_FOUND;
        }
        DsLen = _INT_Rd32(Ds + ACPI_HDR_LEN);
        OffW = FindBcm(Ds, DsLen, "_WAK", &CntW);
        FindBcm(Ds, DsLen, "XWAK", &CntWX);
        OffP = FindBcm(Ds, DsLen, "_PTS", &CntP);
        FindBcm(Ds, DsLen, "XPTS", &CntPX);
        S("  DSDT at "); HX((UINT64)(UINTN)Ds, 8); S(" len "); DC(DsLen);
        S(", _WAK x"); DC(CntW); S(" at +"); HX(OffW, 4); S(", XWAK x"); DC(CntWX);
        S(", _PTS x"); DC(CntP); S(" at +"); HX(OffP, 4); S(", XPTS x"); DC(CntPX); NL();
        if (Sum8(Ds, DsLen) != 0 ||
            (WantWak && (CntW != 1 || OffW == 0 || CntWX != 0)) ||
            (WantPts && (CntP != 1 || OffP == 0 || CntPX != 0))) {
            S("  unexpected DSDT layout, not patched"); NL();
            _INT_FreePool(BS, FileData);
            return EFI_UNSUPPORTED;
        }
    }

    // ---- 4. allocate everything that can fail BEFORE touching firmware tables ----
    UINT8* NewSsdt = NULL;
    UINT8* NewXsdt = NULL;
    ACPI_RSDP* NewRsdp = NULL;
    if (EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, FileSize, (VOID**)&NewSsdt)) ||
        EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, XLen + 8, (VOID**)&NewXsdt)) ||
        EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, sizeof(ACPI_RSDP) > Rsdp->Length ? sizeof(ACPI_RSDP) : Rsdp->Length, (VOID**)&NewRsdp))) {
        S("  AllocatePool(ACPI reclaim) failed"); NL();
        if (NewSsdt) BS->FreePool(NewSsdt);
        if (NewXsdt) BS->FreePool(NewXsdt);
        _INT_FreePool(BS, FileData);
        return EFI_OUT_OF_RESOURCES;
    }
    _INT_memcpy(NewSsdt, File, FileSize);
    _INT_memcpy(NewXsdt, Xsdt, XLen);
    UINT64 NewEntry = (UINT64)(UINTN)NewSsdt;
    _INT_memcpy(NewXsdt + XLen, &NewEntry, 8);
    UINT32 NewXLen = XLen + 8;
    _INT_memcpy(NewXsdt + ACPI_HDR_LEN, &NewXLen, 4);
    NewXsdt[ACPI_HDR_CSUM] = 0;
    NewXsdt[ACPI_HDR_CSUM] = (UINT8)(0 - Sum8(NewXsdt, NewXLen));

    UINTN RLen = Rsdp->Length < sizeof(ACPI_RSDP) ? sizeof(ACPI_RSDP) : Rsdp->Length;
    _INT_memcpy(NewRsdp, Rsdp, RLen);
    NewRsdp->XsdtAddress = (UINT64)(UINTN)NewXsdt;
    NewRsdp->ExtChecksum = 0;
    NewRsdp->ExtChecksum = (UINT8)(0 - Sum8((const UINT8*)NewRsdp, NewRsdp->Length));

    // ---- 5. rename _BCM -> XBCM in the firmware SaSsdt (verified, reverted on failure) ----
    volatile UINT8* Name4 = Sa + Off;
    UINT8 OldCsum = Sa[ACPI_HDR_CSUM];
    Name4[0] = 'X';
    Sa[ACPI_HDR_CSUM] = 0;
    Sa[ACPI_HDR_CSUM] = (UINT8)(0 - Sum8(Sa, SaLen));
    if (Name4[0] != 'X' || Sum8(Sa, SaLen) != 0) {
        Name4[0] = '_';
        Sa[ACPI_HDR_CSUM] = OldCsum;
        S("  SaSsdt memory is not writable, aborted (nothing changed)"); NL();
        BS->FreePool(NewSsdt); BS->FreePool(NewXsdt); BS->FreePool(NewRsdp);
        _INT_FreePool(BS, FileData);
        return EFI_ACCESS_DENIED;
    }

    UINT8 OldDsCsum = 0;
    if (WantWak || WantPts) {
        OldDsCsum = Ds[ACPI_HDR_CSUM];
        if (WantWak) Ds[OffW] = 'X';
        if (WantPts) Ds[OffP] = 'X';
        Ds[ACPI_HDR_CSUM] = 0;
        Ds[ACPI_HDR_CSUM] = (UINT8)(0 - Sum8(Ds, DsLen));
        if ((WantWak && ((volatile UINT8*)Ds)[OffW] != 'X') ||
            (WantPts && ((volatile UINT8*)Ds)[OffP] != 'X') || Sum8(Ds, DsLen) != 0) {
            if (WantWak) Ds[OffW] = '_';
            if (WantPts) Ds[OffP] = '_';
            Ds[ACPI_HDR_CSUM] = OldDsCsum;
            Name4[0] = '_';
            Sa[ACPI_HDR_CSUM] = OldCsum;
            S("  DSDT memory is not writable, aborted (nothing changed)"); NL();
            BS->FreePool(NewSsdt); BS->FreePool(NewXsdt); BS->FreePool(NewRsdp);
            _INT_FreePool(BS, FileData);
            return EFI_ACCESS_DENIED;
        }
    }

    // ---- 6. publish the new RSDP (config table), best-effort update of the old one ----
    Status = BS->InstallConfigurationTable(&acpi20, NewRsdp);
    if (EFI_ERROR(Status)) {
        Name4[0] = '_';
        Sa[ACPI_HDR_CSUM] = OldCsum;
        if (WantWak || WantPts) {
            if (WantWak) Ds[OffW] = '_';
            if (WantPts) Ds[OffP] = '_';
            Ds[ACPI_HDR_CSUM] = OldDsCsum;
        }
        S("  InstallConfigurationTable failed ("); HX(Status, 8); S("), SaSsdt rename reverted"); NL();
        BS->FreePool(NewSsdt); BS->FreePool(NewXsdt); BS->FreePool(NewRsdp);
        _INT_FreePool(BS, FileData);
        return Status;
    }
    {
        // The firmware RSDP may live in RAM that is writable; keep it in sync if possible.
        volatile ACPI_RSDP* Old = (volatile ACPI_RSDP*)Rsdp;
        UINT64 OldX = Old->XsdtAddress;
        UINT8  OldE = Old->ExtChecksum;
        Old->XsdtAddress = NewRsdp->XsdtAddress;
        Old->ExtChecksum = NewRsdp->ExtChecksum;
        if (Old->XsdtAddress != NewRsdp->XsdtAddress) {
            Old->XsdtAddress = OldX;
            Old->ExtChecksum = OldE;
            S("  old RSDP is read-only (new one published via config table)"); NL();
        } else {
            S("  old RSDP updated in place too"); NL();
        }
    }

    S("  OK: _BCM->XBCM"); if (WantWak) S(", _WAK->XWAK"); if (WantPts) S(", _PTS->XPTS");
    S(", new SSDT at "); HX((UINT64)(UINTN)NewSsdt, 8);
    S(", new XSDT at "); HX((UINT64)(UINTN)NewXsdt, 8); S(", entries "); DC(N + 1); NL();

    _INT_FreePool(BS, FileData);
    return EFI_SUCCESS;
}