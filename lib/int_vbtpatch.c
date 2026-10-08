#include "../include/int_edp.h"

#define S(x)  _INT_RepStr(R, (const CHAR8*)(x))
#define NL()  _INT_RepNl(R)
#define DC(v) _INT_RepDec(R, (v))

#define BDB_DRIVER_FEATURES_ID 12
#define BDB_EDP_ID             27
#define BDB_LFP_OPTIONS_ID     40
#define BDB_LFP_POWER_ID       44

VOID _INT_EdpDeriveLink(_INT_EdpCaps* C)
{
    UINT32 best = 0;

    // eDP 1.4 panels list their real rates; the legacy MAX_LINK_RATE can be understated.
    if (C->EdpRev >= 3) {
        for (UINTN i = 0; i < 8; i++)
            if (C->RateTable[i] > best) best = C->RateTable[i];
    }
    if (best >= 27000)      C->VbtRate = 2;       // 5.4 Gbps (HBR3 needs a newer BDB; capped)
    else if (best >= 13500) C->VbtRate = 1;       // 2.7
    else if (best != 0)     C->VbtRate = 0;       // 1.62
    else if (C->MaxLinkRate >= 0x14) C->VbtRate = 2;
    else if (C->MaxLinkRate >= 0x0A) C->VbtRate = 1;
    else                             C->VbtRate = 0;

    if (C->MaxLanes >= 4)      C->VbtLanes = 3;
    else if (C->MaxLanes == 2) C->VbtLanes = 1;
    else                       C->VbtLanes = 0;
}

BOOLEAN _INT_EdidValid(const UINT8* e)
{
    static const UINT8 hdr[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    UINT8 sum = 0;
    for (UINTN i = 0; i < 8; i++) if (e[i] != hdr[i]) return FALSE;
    for (UINTN i = 0; i < 128; i++) sum = (UINT8)(sum + e[i]);
    if (sum != 0) return FALSE;
    return !(e[54] == 0 && e[55] == 0);               // first descriptor must be a DTD
}

BOOLEAN _INT_VbtSetLink(UINT8* v, UINTN size, UINT8 rate, UINT8 lanes,
                        BOOLEAN PsrOk, _INT_Rep* R)
{
    if (size < 64 || v[0] != '$' || v[1] != 'V' || v[2] != 'B' || v[3] != 'T')
        return FALSE;
    UINTN vsz = _INT_Rd16(v + 24);
    UINTN bdb = _INT_Rd32(v + 28);
    if (vsz < 64 || vsz > size || bdb + 22 > vsz) return FALSE;
    UINTN ver  = _INT_Rd16(v + bdb + 16);
    UINTN hdr  = _INT_Rd16(v + bdb + 18);
    UINTN bend = bdb + _INT_Rd16(v + bdb + 20);
    if (bend > vsz) bend = vsz;

    UINTN o12 = 0, s12 = 0, o27 = 0, s27 = 0, o40 = 0, o44 = 0, s44 = 0;
    for (UINTN pos = bdb + hdr; pos + 3 <= bend; ) {
        UINT8 bid = v[pos];
        UINTN bsz = _INT_Rd16(v + pos + 1);
        if (pos + 3 + bsz > bend) break;
        if (bid == BDB_DRIVER_FEATURES_ID && !o12) { o12 = pos + 3; s12 = bsz; }
        if (bid == BDB_EDP_ID && !o27)             { o27 = pos + 3; s27 = bsz; }
        if (bid == BDB_LFP_OPTIONS_ID && !o40)     o40 = pos + 3;
        if (bid == BDB_LFP_POWER_ID && !o44)       { o44 = pos + 3; s44 = bsz; }
        pos += 3 + bsz;
    }
    if (!o27 || !o40) return FALSE;

    UINTN panel = v[o40];
    if (panel >= 16) panel = 0;
    if (s27 < 164 + 16 * 2) return FALSE;

    UINT8* q = v + o27 + 164 + panel * 2;
    q[0] = (UINT8)((rate & 0xF) | ((lanes & 0xF) << 4));          // q[1] (preemph/vswing) kept
    if (s27 >= 214) {
        UINT16 flt = (UINT16)(_INT_Rd16(v + o27 + 212) & ~(1u << panel));
        v[o27 + 212] = (UINT8)(flt & 0xFF);
        v[o27 + 213] = (UINT8)(flt >> 8);
    }

    if (!PsrOk) {
        if (ver >= 228 && o44 && s44 >= 26) {
            UINT16 w = (UINT16)(_INT_Rd16(v + o44 + 24) & ~(1u << panel));
            v[o44 + 24] = (UINT8)(w & 0xFF);
            v[o44 + 25] = (UINT8)(w >> 8);
        } else if (ver >= 165 && ver < 228 && o12 && s12 >= 19) {
            UINT16 w = (UINT16)(_INT_Rd16(v + o12 + s12 - 2) & ~(1u << 9));
            v[o12 + s12 - 2] = (UINT8)(w & 0xFF);
            v[o12 + s12 - 1] = (UINT8)(w >> 8);
        }
    }

    v[26] = 0;
    UINT8 sum = 0;
    for (UINTN i = 0; i < vsz; i++) sum = (UINT8)(sum + v[i]);
    v[26] = (UINT8)(0 - sum);

    if (R) {
        S("VBT link: panel "); DC(panel); S(" rate code "); DC(rate);
        S(" lanes code "); DC(lanes); S(" PSR "); S(PsrOk ? "kept" : "off"); NL();
    }
    return TRUE;
}