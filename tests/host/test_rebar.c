// Pure helpers of the Resizable BAR step (lib/int_rebar_plan.c): size choice, slot search, bridge window registers.
#include <stdio.h>
#include <stdlib.h>
#include "../../include/int_rebar.h"

#define G(n) ((UINT64)(n) << 30)
#define M(n) ((UINT64)(n) << 20)
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    // ---- size choice: capability 256 MB .. 8 GB = exponents 8..13, currently 256 MB (8) ----
    UINT32 Sizes = 0x3F00;
    CHECK(_INT_RebarNextSize(Sizes, 8, 15) == 13);
    CHECK(_INT_RebarNextSize(Sizes, 8, 13) == 12);
    CHECK(_INT_RebarNextSize(Sizes, 8, 10) == 9);
    CHECK(_INT_RebarNextSize(Sizes, 8, 9) == -1);          // 256 MB is the current size
    CHECK(_INT_RebarNextSize(Sizes, 13, 15) == -1);        // already the largest
    CHECK(_INT_RebarNextSize(0, 0, 15) == -1);
    CHECK(_INT_RebarNextSize(0x00000001u, 0, 15) == -1);   // only 1 MB supported, current 1 MB
    printf("case sizes\n");

    // ---- slot search: 8 GB BAR0 + 2 MB BAR2, window 4 GB .. 64 GB ----
    UINT64 Sz[2] = { G(8), M(2) }, A[2], End;
    _INT_Range U[3];
    U[0].Lo = G(4); U[0].Hi = G(5) - 1;                    // something at 4 GB
    CHECK(_INT_RebarPlace(U, 1, G(4), G(64) - 1, Sz, 2, A, &End));
    CHECK(A[0] == G(8) && A[1] == G(16) && End == G(16) + M(2) - 1);
    printf("case place first aligned slot\n");

    U[1].Lo = G(8); U[1].Hi = G(8) + M(1) - 1;             // 1 MB inside the first candidate
    CHECK(_INT_RebarPlace(U, 2, G(4), G(64) - 1, Sz, 2, A, &End));
    CHECK(A[0] == G(16) && A[1] == G(24) && End == G(24) + M(2) - 1);
    printf("case place skips used\n");

    CHECK(!_INT_RebarPlace(U, 2, G(4), G(12) - 1, Sz, 2, A, &End));   // window too small
    CHECK(!_INT_RebarPlace(U, 2, G(4), G(16) + M(2) - 2, Sz, 2, A, &End)); // one byte short at the top
    CHECK(_INT_RebarPlace(NULL, 0, G(4), G(24) + M(2) - 1, Sz, 2, A, &End)); // exactly fits
    CHECK(A[0] == G(8));
    printf("case place no fit\n");

    // unaligned window start, 4 GB BAR0 only
    UINT64 S1[1] = { G(4) };
    CHECK(_INT_RebarPlace(NULL, 0, G(4) + M(256), G(32) - 1, S1, 1, A, &End));
    CHECK(A[0] == G(8) && End == G(12) - 1);
    printf("case place alignment\n");

    // ---- bridge prefetchable window registers ----
    UINT32 L, BU, LU;
    UINT64 B, Lm;
    _INT_BrEncodePref(G(8), G(16) + M(2) - 1, 0x00010001u, &L, &BU, &LU);
    CHECK(BU == 2 && LU == 4);
    CHECK((L & 0xF) == 1 && ((L >> 16) & 0xF) == 1);       // 64-bit capability nibbles kept
    _INT_BrDecodePref(L, BU, LU, &B, &Lm);
    CHECK(B == G(8) && Lm == G(16) + M(2) - 1);
    _INT_BrDecodePref(0x0000FFF1u, 0, 0, &B, &Lm);         // closed window: base above limit
    CHECK(Lm < B);
    _INT_BrDecodePref(0x0000FFF0u, 0x7, 0x9, &B, &Lm);     // 32-bit only bridge: upper dwords ignored
    CHECK(B == 0xFFF00000ULL && Lm == 0xFFFFFULL);
    _INT_BrDecodeMem(0xD030D000u, &B, &Lm);
    CHECK(B == 0xD0000000ULL && Lm == 0xD03FFFFFULL);
    printf("case bridge windows\n");

    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("ALL OK\n");
    return 0;
}