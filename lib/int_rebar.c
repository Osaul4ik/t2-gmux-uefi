#include "../include/int_rebar.h"
#include "../include/int_mem.h"
#include "../include/int_vbt.h"
#include <efiprot.h>

// Resizable BAR for the Radeon, done here because the Mac firmware has no such setting: Apple's EFI has already
// enumerated PCI and left BAR0 (the VRAM aperture) at 256 MB, and Windows keeps what the firmware configured.
//
// Same job as ReBarUEFI does during PCI enumeration, only afterwards:
//   1. find the Radeon (1002:xxxx, display class) and the bridges above it (root port -> ... -> GPU),
//   2. read its Resizable BAR capability (ext. capability 0x0015, BAR index 0) and take the largest size
//      that is supported AND fits (next step),
//   3. look for a free, size-aligned slot in the root bridge's MMIO window above 4 GB. "Used" = every BAR and every
//      bridge window the firmware programmed, except the ones being replaced (prefetchable BARs of the Radeon,
//      prefetchable windows of the bridges above it),
//   4. memory decode off -> new size -> BARs -> bridge windows -> memory decode back on,
//   5. read everything back; on a mismatch the old values are written back,
//   6. a GOP framebuffer inside the old BAR0 is re-pointed (bootmgr / winload draw straight into it).
// Nothing is touched before the plan is complete.

#define PCI_CMD             0x04
#define PCI_CMD_MEM         0x0002
#define PCI_BAR0            0x10
#define BR_MEM_WIN          0x20
#define BR_PREF_WIN         0x24
#define BR_PREF_BASE_U      0x28
#define BR_PREF_LIMIT_U     0x2C

#define EXTCAP_REBAR        0x0015
#define REBAR_MAX_EXP       12              // 4 GB (2^12 MB): this Mac's Radeon has 4 GB of VRAM

// Fallback when the root bridge reports no MMIO window above 4 GB (Apple's EFI does not hand one out through
// EFI_PCI_ROOT_BRIDGE_IO.Configuration()). These are the M64B / M64L values the firmware leaves in the ACPI
// SANV region of this MacBook (read from RAM: M64B = 0x4000000000, M64L = 0x4000000000), i.e. the window
// Windows is told about in the PCI0 _CRS.
#define RB_FALLBACK_LO      0x4000000000ULL
#define RB_FALLBACK_HI      0x7FFFFFFFFFULL
#define GB4                 0x100000000ULL

#define RB_MAXDEV           128
#define RB_MAXUSED          512
#define RB_MAXCHAIN         4
#define RB_MAXBAR           4               // prefetchable BARs of the GPU that are re-placed together

typedef struct {
    EFI_PCI_IO_PROTOCOL* Io;
    UINTN   Seg, Bus, Dev, Fn;
    UINT16  Ven;
    UINT8   Class;                          // base class
    UINT8   Hdr;                            // header type without the multi-function bit
    UINT8   Sec, Sub;                       // bridges: secondary / subordinate bus
} RB_DEV;

typedef struct {
    BOOLEAN Valid;                          // memory BAR with a non-zero address
    BOOLEAN Pref;
    BOOLEAN Is64;
    UINT64  Addr;
    UINT64  Size;                           // 0 = the bus driver could not tell
} RB_BAR;

typedef struct {
    UINT32  L, BU, LU;                      // bridge prefetchable window registers 0x24 / 0x28 / 0x2C
} RB_PREFWIN;

static UINT32 C32(RB_DEV* d, UINT32 off)
{
    UINT32 v = 0xFFFFFFFFu;
    d->Io->Pci.Read(d->Io, EfiPciIoWidthUint32, off, 1, &v);
    return v;
}

static VOID W32(RB_DEV* d, UINT32 off, UINT32 v)
{
    d->Io->Pci.Write(d->Io, EfiPciIoWidthUint32, off, 1, &v);
}

static UINT16 C16(RB_DEV* d, UINT32 off)
{
    UINT16 v = 0xFFFFu;
    d->Io->Pci.Read(d->Io, EfiPciIoWidthUint16, off, 1, &v);
    return v;
}

// 16-bit write: the command register shares its dword with the status register (write-1-to-clear bits)
static VOID W16(RB_DEV* d, UINT32 off, UINT16 v)
{
    d->Io->Pci.Write(d->Io, EfiPciIoWidthUint16, off, 1, &v);
}

static UINTN Enumerate(EFI_BOOT_SERVICES* BS, EFI_HANDLE Image, RB_DEV* D, UINTN Max)
{
    EFI_GUID guid = EFI_PCI_IO_PROTOCOL_GUID;
    EFI_HANDLE* H = NULL;
    UINTN Cnt = 0, N = 0;

    if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &guid, NULL, &Cnt, &H)))
        return 0;
    for (UINTN i = 0; i < Cnt && N < Max; i++) {
        RB_DEV* d = &D[N];
        UINT32 w;

        if (EFI_ERROR(BS->OpenProtocol(H[i], &guid, (VOID**)&d->Io, Image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL)))
            continue;
        if (EFI_ERROR(d->Io->GetLocation(d->Io, &d->Seg, &d->Bus, &d->Dev, &d->Fn)))
            continue;
        w = C32(d, 0);
        if ((w & 0xFFFF) == 0xFFFF)
            continue;                                       // not there (Radeon rail off)
        d->Ven = (UINT16)(w & 0xFFFF);
        d->Class = (UINT8)(C32(d, 8) >> 24);
        d->Hdr = (UINT8)((C32(d, 0x0C) >> 16) & 0x7F);
        d->Sec = d->Sub = 0;
        if (d->Hdr == 1) {
            w = C32(d, 0x18);
            d->Sec = (UINT8)((w >> 8) & 0xFF);
            d->Sub = (UINT8)((w >> 16) & 0xFF);
        }
        N++;
    }
    _INT_FreePool(BS, H);
    return N;
}

static UINT32 FindExtCap(RB_DEV* d, UINT32 Id)
{
    UINT32 off = 0x100;

    for (UINTN i = 0; i < 48 && off >= 0x100; i++) {
        UINT32 h = C32(d, off);

        if (h == 0 || h == 0xFFFFFFFFu)
            return 0;
        if ((h & 0xFFFF) == Id)
            return off;
        off = (h >> 20) & 0xFFC;
    }
    return 0;
}

static RB_DEV* ParentOf(RB_DEV* D, UINTN N, const RB_DEV* Child)
{
    for (UINTN i = 0; i < N; i++)
        if (D[i].Hdr == 1 && D[i].Seg == Child->Seg && D[i].Sec == Child->Bus)
            return &D[i];
    return NULL;
}

static UINTN BarSlots(const RB_DEV* d)
{
    return d->Hdr == 0 ? 6 : (d->Hdr == 1 ? 2 : 0);
}

// One BAR slot as the firmware programmed it. The size comes from the PCI bus driver (GetBarAttributes),
// nothing is probed. A 64-bit BAR takes two slots (Is64).
static VOID ReadBar(EFI_BOOT_SERVICES* BS, RB_DEV* d, UINTN Slot, RB_BAR* b)
{
    UINT32 lo = C32(d, PCI_BAR0 + 4 * (UINT32)Slot);
    VOID* P = NULL;

    b->Valid = b->Pref = b->Is64 = FALSE;
    b->Addr = b->Size = 0;
    if (lo == 0xFFFFFFFFu || (lo & 1))
        return;                                             // I/O BAR or nothing
    b->Addr = lo & ~0xFULL;
    b->Pref = (lo & 8) ? TRUE : FALSE;
    if (((lo >> 1) & 3) == 2) {
        b->Is64 = TRUE;
        b->Addr |= (UINT64)C32(d, PCI_BAR0 + 4 * (UINT32)Slot + 4) << 32;
    }
    if (b->Addr == 0)
        return;
    b->Valid = TRUE;
    if (!EFI_ERROR(d->Io->GetBarAttributes(d->Io, (UINT8)Slot, NULL, &P)) && P != NULL) {
        const UINT8* p = (const UINT8*)P;

        if (p[0] == 0x8A)                                   // QWORD address space descriptor, length at +38
            b->Size = _INT_Rd64(p + 38);
        else if (p[0] == 0x87)                              // DWORD address space descriptor, length at +22
            b->Size = _INT_Rd32(p + 22);
        BS->FreePool(P);
    }
}

static BOOLEAN AddUsed(_INT_Range* U, UINTN* N, UINT64 Lo, UINT64 Hi)
{
    if (Hi < Lo)
        return TRUE;                                        // empty / disabled window
    if (*N >= RB_MAXUSED)
        return FALSE;
    U[*N].Lo = Lo;
    U[*N].Hi = Hi;
    (*N)++;
    return TRUE;
}

// Prefetchable window of a bridge as programmed. FALSE = the window is closed (base above limit, or 0).
static BOOLEAN BridgePref(RB_DEV* b, UINT64* Base, UINT64* Limit)
{
    _INT_BrDecodePref(C32(b, BR_PREF_WIN), C32(b, BR_PREF_BASE_U), C32(b, BR_PREF_LIMIT_U), Base, Limit);
    return (*Limit >= *Base && *Base != 0) ? TRUE : FALSE;
}

// The largest MMIO window above 4 GB the root bridge of the Radeon decodes. A separate prefetchable window
// (SpecificFlag 0x06) wins over a plain one. Descriptors are ACPI address space descriptors: QWORD 0x8A
// (memory ranges, normally the bus range too), DWORD 0x87, WORD 0x88 (bus range of some firmware).
static BOOLEAN RootWindow(EFI_BOOT_SERVICES* BS, UINTN Seg, UINTN Bus, UINT64* Lo, UINT64* Hi)
{
    EFI_GUID guid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
    EFI_HANDLE* H = NULL;
    UINTN Cnt = 0;
    UINT64 PLo = 0, PHi = 0, ALo = 0, AHi = 0;

    if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &guid, NULL, &Cnt, &H)))
        return FALSE;
    for (UINTN i = 0; i < Cnt; i++) {
        EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL* Rb = NULL;
        VOID* Res = NULL;
        const UINT8* p;
        BOOLEAN BusSeen = FALSE, BusOk = FALSE;

        if (EFI_ERROR(BS->HandleProtocol(H[i], &guid, (VOID**)&Rb)) || Rb == NULL)
            continue;
        if (Rb->SegmentNumber != Seg || EFI_ERROR(Rb->Configuration(Rb, &Res)) || Res == NULL)
            continue;
        for (p = (const UINT8*)Res; p[0] == 0x8A || p[0] == 0x87 || p[0] == 0x88; p += 3 + _INT_Rd16(p + 1)) {
            if (p[3] != 2)
                continue;                                   // not a bus number range
            BusSeen = TRUE;
            if (p[0] == 0x8A && _INT_Rd64(p + 14) <= Bus && Bus <= _INT_Rd64(p + 22))
                BusOk = TRUE;
            else if (p[0] == 0x88 && _INT_Rd16(p + 8) <= Bus && Bus <= _INT_Rd16(p + 10))
                BusOk = TRUE;
        }
        if (BusOk || !BusSeen) {                            // no bus range given: one root bridge per segment
            for (p = (const UINT8*)Res; p[0] == 0x8A || p[0] == 0x87 || p[0] == 0x88; p += 3 + _INT_Rd16(p + 1)) {
                UINT64 Min, Max;

                if (p[0] != 0x8A || p[3] != 0)
                    continue;                               // only QWORD memory ranges can be above 4 GB
                Min = _INT_Rd64(p + 14);
                Max = _INT_Rd64(p + 22);
                if (Max < GB4 || Max < Min)
                    continue;
                if (Min < GB4)
                    Min = GB4;
                if (p[5] & 0x06) {
                    if (Max - Min > PHi - PLo) {
                        PLo = Min; PHi = Max;
                    }
                } else if (Max - Min > AHi - ALo) {
                    ALo = Min; AHi = Max;
                }
            }
        }
        BS->FreePool(Res);
    }
    _INT_FreePool(BS, H);
    if (PHi > PLo) {
        *Lo = PLo; *Hi = PHi;
        return TRUE;
    }
    if (AHi > ALo) {
        *Lo = ALo; *Hi = AHi;
        return TRUE;
    }
    return FALSE;
}

// bootmgr / winload write straight into the GOP framebuffer; if it lives in the old BAR0 it has to follow.
static UINT32 MoveFramebuffers(EFI_BOOT_SERVICES* BS, UINT64 OldBase, UINT64 OldSize, UINT64 NewBase)
{
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_HANDLE* H = NULL;
    UINTN Cnt = 0;
    UINT32 Moved = 0;

    if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &guid, NULL, &Cnt, &H)))
        return 0;
    for (UINTN i = 0; i < Cnt; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL* Gop = NULL;
        UINT64 Fb;

        if (EFI_ERROR(BS->HandleProtocol(H[i], &guid, (VOID**)&Gop)) || Gop == NULL || Gop->Mode == NULL)
            continue;
        Fb = Gop->Mode->FrameBufferBase;
        if (Fb >= OldBase && Fb - OldBase < OldSize) {
            Gop->Mode->FrameBufferBase = NewBase + (Fb - OldBase);
            Moved++;
        }
    }
    _INT_FreePool(BS, H);
    return Moved;
}

EFI_STATUS _INT_RebarApply(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle, _INT_RebarResult* Out)
{
    RB_DEV* D = (RB_DEV*)_INT_AllocatePool(BS, RB_MAXDEV * sizeof(RB_DEV));
    _INT_Range* U = (_INT_Range*)_INT_AllocatePool(BS, RB_MAXUSED * sizeof(_INT_Range));
    UINTN N = 0, NU = 0;
    RB_DEV* G = NULL;
    RB_DEV* Ch[RB_MAXCHAIN];
    UINTN NC = 0;
    UINT32 Cap = 0, Ent = 0, Ctrl = 0, Sizes = 0, CurExp = 0;
    UINT32 Code = _INT_REBAR_ERR;
    BOOLEAN HaveGpu = FALSE;

    Out->Code = _INT_REBAR_ERR;
    Out->OldExp = Out->NewExp = 0;
    Out->NewBase = Out->WinLo = Out->WinHi = 0;
    Out->FbMoved = 0;
    Out->Gpu = 0;
    Out->WinFb = 0;
    if (D == NULL || U == NULL)
        goto out;

    N = Enumerate(BS, ImageHandle, D, RB_MAXDEV);

    // ---- 1. the Radeon and its ReBAR capability ----
    for (UINTN i = 0; i < N && G == NULL; i++) {
        UINT32 c, c0, nb;

        if (D[i].Ven != 0x1002 || D[i].Class != 3 || D[i].Hdr != 0)
            continue;
        HaveGpu = TRUE;
        c = FindExtCap(&D[i], EXTCAP_REBAR);
        if (c == 0)
            continue;
        c0 = C32(&D[i], c + 8);
        nb = (c0 >> 5) & 7;
        for (UINT32 e = 0; e < nb && e < 6; e++) {
            if ((C32(&D[i], c + 8 + 8 * e) & 7) == 0) {     // BAR index 0 = VRAM aperture
                G = &D[i];
                Cap = c;
                Ent = e;
                break;
            }
        }
    }
    if (G == NULL) {
        Code = HaveGpu ? _INT_REBAR_NO_CAP : _INT_REBAR_NO_GPU;
        goto out;
    }
    Out->Gpu = (UINT32)((G->Bus << 8) | (G->Dev << 3) | G->Fn);
    Ctrl = C32(G, Cap + 8 + 8 * Ent);
    Sizes = C32(G, Cap + 4 + 8 * Ent) >> 4;
    CurExp = (Ctrl >> 8) & 0x1F;
    Out->OldExp = Out->NewExp = CurExp;

    // ---- 2. bridges above the Radeon (direct parent first, root port last) ----
    {
        const RB_DEV* Cur = G;

        while (NC < RB_MAXCHAIN) {
            RB_DEV* P = ParentOf(D, N, Cur);
            if (P == NULL)
                break;
            Ch[NC++] = P;
            Cur = P;
        }
    }
    if (NC == 0) {
        Code = _INT_REBAR_BRIDGE;
        goto out;
    }
    for (UINTN k = 0; k < NC; k++) {
        if ((C32(Ch[k], BR_PREF_WIN) & 0xF) != 1) {         // no 64-bit prefetchable window
            Code = _INT_REBAR_BRIDGE;
            goto out;
        }
    }

    // ---- 3. prefetchable BARs of the Radeon (BAR0 first) ----
    UINT64 Sz[RB_MAXBAR], Addr[RB_MAXBAR], OldAddr[RB_MAXBAR];
    UINT32 Slot[RB_MAXBAR], OldLo[RB_MAXBAR], OldHi[RB_MAXBAR];
    UINTN NP = 0;

    for (UINTN s = 0; s < BarSlots(G);) {
        RB_BAR b;

        ReadBar(BS, G, s, &b);
        if (b.Valid && b.Pref) {
            if (NP >= RB_MAXBAR || b.Size == 0 || (NP == 0 && (s != 0 || !b.Is64))) {
                Code = _INT_REBAR_ERR;
                goto out;
            }
            Slot[NP] = (UINT32)s;
            Sz[NP] = b.Size;
            OldAddr[NP] = b.Addr;
            OldLo[NP] = C32(G, PCI_BAR0 + 4 * (UINT32)s);
            OldHi[NP] = b.Is64 ? C32(G, PCI_BAR0 + 4 * (UINT32)s + 4) : 0;
            if (NP > 0 && !b.Is64) {                        // the code below writes BAR pairs
                Code = _INT_REBAR_ERR;
                goto out;
            }
            NP++;
        }
        s += b.Is64 ? 2 : 1;
    }
    if (NP == 0 || Sz[0] != (1ULL << (20 + CurExp))) {      // BAR0 must be the one the capability describes
        Code = _INT_REBAR_ERR;
        goto out;
    }

    // ---- 4. nothing else may live in the prefetchable windows that are about to be replaced ----
    {
        const RB_DEV* Root = Ch[NC - 1];

        for (UINTN j = 0; j < N; j++) {
            BOOLEAN IsChain = FALSE;

            if (D[j].Seg != G->Seg || D[j].Bus < Root->Sec || D[j].Bus > Root->Sub || &D[j] == G)
                continue;
            for (UINTN k = 0; k < NC; k++)
                if (Ch[k] == &D[j])
                    IsChain = TRUE;
            if (IsChain)
                continue;
            if (D[j].Hdr == 1) {
                UINT64 B, L;
                if (BridgePref(&D[j], &B, &L)) {
                    Code = _INT_REBAR_OTHER;
                    goto out;
                }
            }
            for (UINTN s = 0; s < BarSlots(&D[j]);) {
                RB_BAR b;

                ReadBar(BS, &D[j], s, &b);
                if (b.Valid && b.Pref) {
                    Code = _INT_REBAR_OTHER;
                    goto out;
                }
                s += b.Is64 ? 2 : 1;
            }
        }
    }

    // ---- 5. address space that is taken ----
    for (UINTN j = 0; j < N; j++) {
        BOOLEAN IsChain = FALSE;

        for (UINTN k = 0; k < NC; k++)
            if (Ch[k] == &D[j])
                IsChain = TRUE;
        for (UINTN s = 0; s < BarSlots(&D[j]);) {
            RB_BAR b;

            ReadBar(BS, &D[j], s, &b);
            s += b.Is64 ? 2 : 1;
            if (!b.Valid || (&D[j] == G && b.Pref))
                continue;                                   // BARs of the Radeon being replaced
            if (!AddUsed(U, &NU, b.Addr, b.Addr + (b.Size ? b.Size : 0x100000ULL) - 1))
                goto out;                                   // too many: do not guess
        }
        if (D[j].Hdr == 1) {
            UINT64 B, L;

            _INT_BrDecodeMem(C32(&D[j], BR_MEM_WIN), &B, &L);
            if (!AddUsed(U, &NU, B, L))
                goto out;
            if (!IsChain && BridgePref(&D[j], &B, &L) && !AddUsed(U, &NU, B, L))
                goto out;
        }
    }

    // ---- 6. window above 4 GB, then the largest size that fits ----
    UINT64 WLo = 0, WHi = 0, End = 0;
    UINT32 NewExp = CurExp;
    BOOLEAN Placed = FALSE, AnyLarger = FALSE;
    UINT32 Below = REBAR_MAX_EXP + 1;

    if (!RootWindow(BS, G->Seg, Ch[NC - 1]->Bus, &WLo, &WHi)) {
        WLo = RB_FALLBACK_LO;
        WHi = RB_FALLBACK_HI;
        Out->WinFb = 1;
    }
    Out->WinLo = WLo;
    Out->WinHi = WHi;
    for (;;) {
        INT32 n = _INT_RebarNextSize(Sizes, CurExp, Below);

        if (n < 0)
            break;
        AnyLarger = TRUE;
        Sz[0] = 1ULL << (20 + n);
        if (_INT_RebarPlace(U, NU, WLo, WHi, Sz, NP, Addr, &End)) {
            NewExp = (UINT32)n;
            Placed = TRUE;
            break;
        }
        Below = (UINT32)n;
    }
    Sz[0] = 1ULL << (20 + CurExp);                          // Sz[0] is the old size again
    if (!Placed) {
        Code = AnyLarger ? _INT_REBAR_NO_FIT : _INT_REBAR_ALREADY;
        goto out;
    }

    // ---- 7. write: decode off -> size -> BARs -> windows -> decode on ----
    {
        UINT16 Cmd = C16(G, PCI_CMD);
        RB_PREFWIN Old[RB_MAXCHAIN];
        UINT64 NewSz0 = 1ULL << (20 + NewExp);
        UINT32 lo, hi, L, BU, LU;
        UINT64 m;
        BOOLEAN Ok = TRUE;

        for (UINTN k = 0; k < NC; k++) {
            Old[k].L = C32(Ch[k], BR_PREF_WIN);
            Old[k].BU = C32(Ch[k], BR_PREF_BASE_U);
            Old[k].LU = C32(Ch[k], BR_PREF_LIMIT_U);
        }

        W16(G, PCI_CMD, (UINT16)(Cmd & ~PCI_CMD_MEM));
        W32(G, Cap + 8 + 8 * Ent, (Ctrl & ~(0x1Fu << 8)) | (NewExp << 8));

        // the BAR must now answer with the new size, or the capability is not what we think it is
        W32(G, PCI_BAR0, 0xFFFFFFFFu);
        W32(G, PCI_BAR0 + 4, 0xFFFFFFFFu);
        lo = C32(G, PCI_BAR0);
        hi = C32(G, PCI_BAR0 + 4);
        W32(G, PCI_BAR0, OldLo[0]);
        W32(G, PCI_BAR0 + 4, OldHi[0]);
        m = ((UINT64)hi << 32) | (lo & ~0xFULL);
        if (~m + 1 != NewSz0)
            Ok = FALSE;

        if (Ok) {
            for (UINTN k = NC; k-- > 0;) {                  // root port first
                _INT_BrEncodePref(Addr[0], End, Old[k].L, &L, &BU, &LU);
                W32(Ch[k], BR_PREF_WIN, L);
                W32(Ch[k], BR_PREF_BASE_U, BU);
                W32(Ch[k], BR_PREF_LIMIT_U, LU);
            }
            for (UINTN p = 0; p < NP; p++) {
                W32(G, PCI_BAR0 + 4 * Slot[p], (UINT32)(Addr[p] & 0xFFFFFFF0u) | (OldLo[p] & 0xFu));
                W32(G, PCI_BAR0 + 4 * Slot[p] + 4, (UINT32)(Addr[p] >> 32));
            }
            W16(G, PCI_CMD, Cmd);

            // read everything back
            if (((C32(G, Cap + 8 + 8 * Ent) >> 8) & 0x1F) != NewExp)
                Ok = FALSE;
            for (UINTN p = 0; p < NP && Ok; p++) {
                RB_BAR b;

                ReadBar(BS, G, Slot[p], &b);
                if (!b.Valid || b.Addr != Addr[p])
                    Ok = FALSE;
            }
            for (UINTN k = 0; k < NC && Ok; k++) {
                UINT64 B, Lm;

                if (!BridgePref(Ch[k], &B, &Lm) || B != Addr[0] || Lm != End)
                    Ok = FALSE;
            }
            if ((C32(G, 0) & 0xFFFF) != 0x1002)
                Ok = FALSE;
        }

        if (!Ok) {                                          // put everything back
            W16(G, PCI_CMD, (UINT16)(Cmd & ~PCI_CMD_MEM));
            for (UINTN k = NC; k-- > 0;) {
                W32(Ch[k], BR_PREF_WIN, Old[k].L);
                W32(Ch[k], BR_PREF_BASE_U, Old[k].BU);
                W32(Ch[k], BR_PREF_LIMIT_U, Old[k].LU);
            }
            W32(G, Cap + 8 + 8 * Ent, Ctrl);
            for (UINTN p = 0; p < NP; p++) {
                W32(G, PCI_BAR0 + 4 * Slot[p], OldLo[p]);
                W32(G, PCI_BAR0 + 4 * Slot[p] + 4, OldHi[p]);
            }
            W16(G, PCI_CMD, Cmd);
            Code = _INT_REBAR_VERIFY;
            goto out;
        }

        Out->NewExp = NewExp;
        Out->NewBase = Addr[0];
        Out->FbMoved = MoveFramebuffers(BS, OldAddr[0], Sz[0], Addr[0]);
        Code = _INT_REBAR_OK;
    }

out:
    Out->Code = Code;
    if (D != NULL)
        _INT_FreePool(BS, D);
    if (U != NULL)
        _INT_FreePool(BS, U);
    return (Code == _INT_REBAR_OK || Code == _INT_REBAR_ALREADY) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}