#ifndef int_rebar_h
#define int_rebar_h
#include <efi.h>
#include <efiapi.h>
#include <efidef.h>

// ---- pure helpers (lib/int_rebar_plan.c, no EFI services, host-testable) ----

typedef struct {
    UINT64 Lo;              // inclusive
    UINT64 Hi;              // inclusive
} _INT_Range;

// Resizable BAR capability: `Sizes` = capability register >> 4 (bit n = BAR of 2^(20+n) bytes).
// Largest supported exponent n with Cur < n < Below, or -1.
INT32   _INT_RebarNextSize(UINT32 Sizes, UINT32 Cur, UINT32 Below);

// TRUE if [Lo, Hi] touches none of the N used ranges.
BOOLEAN _INT_RangeFree(const _INT_Range* U, UINTN N, UINT64 Lo, UINT64 Hi);

// Place Cnt BARs one after the other inside [WinLo, WinHi]: Size[0] (the resized BAR) is aligned to its own
// size, every other one to its own size after it. Lowest free slot wins. *End = last byte of the block,
// rounded up to the 1 MB bridge window granularity.
BOOLEAN _INT_RebarPlace(const _INT_Range* U, UINTN N, UINT64 WinLo, UINT64 WinHi,
                        const UINT64* Size, UINTN Cnt, UINT64* Addr, UINT64* End);

// PCI-PCI bridge windows. Pref: dword 0x24 + 0x28 (base upper) + 0x2C (limit upper). Mem: dword 0x20.
VOID    _INT_BrDecodePref(UINT32 L, UINT32 BU, UINT32 LU, UINT64* Base, UINT64* Limit);
VOID    _INT_BrEncodePref(UINT64 Base, UINT64 Limit, UINT32 OldL, UINT32* L, UINT32* BU, UINT32* LU);
VOID    _INT_BrDecodeMem(UINT32 W, UINT64* Base, UINT64* Limit);

// ---- Resizable BAR for the Radeon (lib/int_rebar.c) ----

#define _INT_REBAR_OK         0     // BAR0 resized
#define _INT_REBAR_ALREADY    1     // already at the largest size that is supported / fits
#define _INT_REBAR_NO_GPU     2     // no Radeon visible (rail off?)
#define _INT_REBAR_NO_CAP     3     // the Radeon has no Resizable BAR capability for BAR0
#define _INT_REBAR_NO_WINDOW  4     // the root bridge reports no MMIO window above 4 GB
#define _INT_REBAR_NO_FIT     5     // no free aligned slot for any larger size
#define _INT_REBAR_BRIDGE     6     // an upstream bridge has no 64-bit prefetchable window
#define _INT_REBAR_OTHER      7     // another prefetchable BAR sits behind the same bridges
#define _INT_REBAR_VERIFY     8     // read-back mismatch, everything was put back
#define _INT_REBAR_ERR        9     // PCI access / resource query failed, nothing was changed

typedef struct {
    UINT32  Code;           // _INT_REBAR_*
    UINT32  OldExp;         // BAR0 size before, 2^(20+n)
    UINT32  NewExp;         // BAR0 size after (== OldExp unless Code == OK)
    UINT64  NewBase;        // BAR0 address after
    UINT64  WinLo;          // root bridge MMIO window above 4 GB that was used (diagnostics)
    UINT64  WinHi;
    UINT32  FbMoved;        // GOP framebuffers that were inside the old BAR0 and were re-pointed
    UINT32  Gpu;            // bus:dev.fn of the Radeon (bus<<8 | dev<<3 | fn)
    UINT32  WinFb;          // 1 = the root bridge reported no window above 4 GB, the built-in fallback window was used
} _INT_RebarResult;

// Resize BAR0 of the Radeon to the largest size that is supported and fits, re-place its prefetchable BARs
// and the prefetchable windows of the bridges above it, re-point the GOP framebuffer. Boot Services only.
// Everything is verified; on any mismatch the old values are written back.
EFI_STATUS _INT_RebarApply(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle, _INT_RebarResult* Out);

#endif