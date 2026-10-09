#include "../include/int_vbt.h"

// ---------------------------------------------------------------------------
// report builder
// ---------------------------------------------------------------------------

VOID _INT_RepInit(_INT_Rep* r, CHAR8* buf, UINTN cap)
{
    r->buf = buf;
    r->cap = cap;
    r->len = 0;
    if (cap) buf[0] = 0;
}

static VOID rep_c(_INT_Rep* r, CHAR8 c)
{
    if (r->len + 1 < r->cap) {
        r->buf[r->len++] = c;
        r->buf[r->len] = 0;
    }
}

VOID _INT_RepStr(_INT_Rep* r, const CHAR8* s)
{
    while (*s) rep_c(r, *s++);
}

VOID _INT_RepNl(_INT_Rep* r)
{
    rep_c(r, '\r');
    rep_c(r, '\n');
}

VOID _INT_RepHex(_INT_Rep* r, UINT64 v, UINTN digits)
{
    static const CHAR8 h[] = "0123456789ABCDEF";
    for (UINTN i = digits; i > 0; i--)
        rep_c(r, h[(v >> ((i - 1) * 4)) & 0xF]);
}

VOID _INT_RepDec(_INT_Rep* r, UINT64 v)
{
    CHAR8 t[21];
    UINTN n = 0;
    if (v == 0) { rep_c(r, '0'); return; }
    while (v) { t[n++] = (CHAR8)('0' + (v % 10)); v /= 10; }
    while (n) rep_c(r, t[--n]);
}

UINT16 _INT_Rd16(const UINT8* p)
{
    return (UINT16)(p[0] | (p[1] << 8));
}

UINT32 _INT_Rd32(const UINT8* p)
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

UINT64 _INT_Rd64(const UINT8* p)
{
    return (UINT64)_INT_Rd32(p) | ((UINT64)_INT_Rd32(p + 4) << 32);
}