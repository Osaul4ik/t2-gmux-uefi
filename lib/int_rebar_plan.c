#include "../include/int_rebar.h"

#define MB1  0x100000ULL

static UINT64 AlignUp(UINT64 v, UINT64 a)
{
    return (v + a - 1) & ~(a - 1);
}

INT32 _INT_RebarNextSize(UINT32 Sizes, UINT32 Cur, UINT32 Below)
{
    UINT32 n = (Below > 28) ? 28 : Below;

    while (n-- > 0) {
        if (n <= Cur)
            break;
        if ((Sizes >> n) & 1u)
            return (INT32)n;
    }
    return -1;
}

BOOLEAN _INT_RangeFree(const _INT_Range* U, UINTN N, UINT64 Lo, UINT64 Hi)
{
    for (UINTN i = 0; i < N; i++)
        if (U[i].Lo <= Hi && U[i].Hi >= Lo)
            return FALSE;
    return TRUE;
}

BOOLEAN _INT_RebarPlace(const _INT_Range* U, UINTN N, UINT64 WinLo, UINT64 WinHi,
                        const UINT64* Size, UINTN Cnt, UINT64* Addr, UINT64* End)
{
    if (Cnt == 0 || Size[0] == 0 || (Size[0] & (Size[0] - 1)))
        return FALSE;
    if (WinHi > 0x7FFFFFFFFFFFFFFFULL)                      // keeps every sum below far from 2^64
        WinHi = 0x7FFFFFFFFFFFFFFFULL;

    for (UINT64 X = AlignUp(WinLo, Size[0]); X >= WinLo && X + Size[0] - 1 <= WinHi; X += Size[0]) {
        UINT64 Cur = X;
        UINT64 E;

        for (UINTN k = 0; k < Cnt; k++) {
            UINT64 A = (k == 0) ? X : AlignUp(Cur, Size[k]);
            Addr[k] = A;
            Cur = A + Size[k];
        }
        E = AlignUp(Cur, MB1) - 1;
        if (E > WinHi)
            return FALSE;                                   // a higher X only ends higher
        if (_INT_RangeFree(U, N, X, E)) {
            *End = E;
            return TRUE;
        }
    }
    return FALSE;
}

VOID _INT_BrDecodePref(UINT32 L, UINT32 BU, UINT32 LU, UINT64* Base, UINT64* Limit)
{
    UINT64 b = (UINT64)(L & 0xFFF0u) << 16;
    UINT64 l = ((UINT64)((L >> 16) & 0xFFF0u) << 16) | (MB1 - 1);

    if ((L & 0xFu) == 1) {                                  // 64-bit capable window
        b |= (UINT64)BU << 32;
        l |= (UINT64)LU << 32;
    }
    *Base = b;
    *Limit = l;
}

VOID _INT_BrEncodePref(UINT64 Base, UINT64 Limit, UINT32 OldL, UINT32* L, UINT32* BU, UINT32* LU)
{
    UINT32 b = (UINT32)((Base >> 16) & 0xFFF0u);
    UINT32 l = (UINT32)((Limit >> 16) & 0xFFF0u);

    // keep the capability nibbles (0x1 = 64-bit) the bridge reports
    *L = (b | (OldL & 0xFu)) | ((l | ((OldL >> 16) & 0xFu)) << 16);
    *BU = (UINT32)(Base >> 32);
    *LU = (UINT32)(Limit >> 32);
}

VOID _INT_BrDecodeMem(UINT32 W, UINT64* Base, UINT64* Limit)
{
    *Base = (UINT64)(W & 0xFFF0u) << 16;
    *Limit = ((UINT64)((W >> 16) & 0xFFF0u) << 16) | (MB1 - 1);
}