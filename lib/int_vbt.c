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

VOID _INT_RepBytes(_INT_Rep* r, const UINT8* p, UINTN n)
{
    for (UINTN i = 0; i < n; i++) {
        _INT_RepHex(r, p[i], 2);
        rep_c(r, ' ');
    }
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

#define S(x)  _INT_RepStr(R, (const CHAR8*)(x))
#define NL()  _INT_RepNl(R)
#define HX(v, d) _INT_RepHex(R, (v), (d))
#define DC(v) _INT_RepDec(R, (v))

// ---------------------------------------------------------------------------
// name tables (Intel i915 intel_vbt_defs.h conventions)
// ---------------------------------------------------------------------------

static const char* DvoPortName(UINT8 p)
{
    switch (p) {
    case 0:  return "HDMI-A";
    case 1:  return "HDMI-B";
    case 2:  return "HDMI-C";
    case 3:  return "HDMI-D";
    case 4:  return "LVDS";
    case 5:  return "TV";
    case 6:  return "CRT";
    case 7:  return "DP-B";
    case 8:  return "DP-C";
    case 9:  return "DP-D";
    case 10: return "DP-A (eDP port)";
    case 11: return "DP-E";
    case 12: return "HDMI-E";
    case 13: return "DP-F";
    case 14: return "HDMI-F";
    default: return "?";
    }
}

static const char* AuxName(UINT8 a)
{
    switch (a) {
    case 0x40: return "AUX-A";
    case 0x10: return "AUX-B";
    case 0x20: return "AUX-C";
    case 0x30: return "AUX-D";
    case 0x50: return "AUX-E";
    case 0x60: return "AUX-F";
    case 0x00: return "none";
    default:   return "?";
    }
}

static const char* RateName(UINT8 r)
{
    switch (r) {
    case 0:  return "1.62 Gbps (RBR)";
    case 1:  return "2.7 Gbps (HBR)";
    case 2:  return "5.4 Gbps (HBR2)";
    case 3:  return "8.1 Gbps (HBR3)";
    default: return "?";
    }
}

static const char* LaneName(UINT8 l)
{
    switch (l) {
    case 0:  return "x1";
    case 1:  return "x2";
    case 3:  return "x4";
    default: return "?";
    }
}

// ---------------------------------------------------------------------------
// VBT parser
// ---------------------------------------------------------------------------

#define BDB_GENERAL_DEFINITIONS  2
#define BDB_EDP                  27
#define BDB_LVDS_OPTIONS         40

static VOID ParseChildren(const UINT8* d, UINTN sz, _INT_Rep* R, _INT_VbtInfo* Info)
{
    if (sz < 6) { S("  block 2 too small"); NL(); return; }

    UINT8 csz = d[4];
    S("  crt_ddc_gmbus_pin="); HX(d[0], 2);
    S(" dpms_bits="); HX(d[1], 2);
    S(" child_dev_size="); DC(csz); NL();
    if (csz < 17) { S("  child size too small"); NL(); return; }

    UINTN n = (sz - 5) / csz;
    for (UINTN i = 0; i < n; i++) {
        const UINT8* c = d + 5 + i * csz;
        UINT16 handle = _INT_Rd16(c);
        UINT16 dtype  = _INT_Rd16(c + 2);
        if (dtype == 0 || dtype == 0xFFFF)
            continue;

        UINT8 dvo  = c[16];
        UINT8 i2c  = c[17];
        UINT8 ddc  = c[19];
        UINT8 f1   = csz > 23 ? c[23] : 0;
        UINT8 sup  = csz > 24 ? c[24] : 0;
        UINT8 aux  = csz > 25 ? c[25] : 0;

        // eDP-like: INTERNAL_CONNECTOR + DISPLAYPORT bits as used by i915 (0x1806 pattern)
        BOOLEAN edp = ((dtype & 0x1806) == 0x1806) && (dvo == 10 || dvo == 7 || dvo == 8 || dvo == 9);

        S("  child["); DC(i); S("] handle="); HX(handle, 4);
        S(" type="); HX(dtype, 4);
        S(edp ? " (eDP?)" : "");
        NL();
        S("     dvo_port="); DC(dvo); S(" "); S(DvoPortName(dvo));
        S("  i2c_pin="); DC(i2c);
        S("  ddc_pin="); DC(ddc);
        S("  aux="); HX(aux, 2); S(" "); S(AuxName(aux)); NL();
        S("     flags23="); HX(f1, 2);
        S(" (lane_reversal="); DC((f1 >> 1) & 1);
        S(" hpd_invert="); DC((f1 >> 4) & 1);
        S(")  support24="); HX(sup, 2);
        S(" (hdmi="); DC(sup & 1);
        S(" dp="); DC((sup >> 1) & 1);
        S(" tmds="); DC((sup >> 2) & 1); S(")"); NL();
        S("     raw: ");
        _INT_RepBytes(R, c, csz > 40 ? 40 : csz);
        NL();

        if (edp && !Info->EdpFound) {
            Info->EdpFound = TRUE;
            Info->EdpType = dtype;
            Info->EdpDvoPort = dvo;
            Info->EdpAux = aux;
            Info->EdpDdc = ddc;
        }
    }
}

static VOID ParseEdp(const UINT8* d, UINTN sz, UINT8 panel, _INT_Rep* R, _INT_VbtInfo* Info)
{
    S("  eDP block, size="); DC(sz); S(" panel_type="); DC(panel); NL();
    if (panel >= 16) {
        S("  panel_type invalid (>=16) -> cannot index eDP tables"); NL();
        return;
    }

    // struct bdb_edp: power_seqs[16] (5 x u16 each = 10 bytes), color_depth u32,
    // fast_link_params[16] (2 bytes each), sdrrs u32, s3d u16, t3_opt u16,
    // vswing_preemph u64, fast_link_training u16, dpcd_600h u16, ...
    if (sz >= 160) {
        const UINT8* p = d + panel * 10;
        S("  power seq (raw, units 100us; t11_t12 units 100ms):"); NL();
        S("    t1_t3="); DC(_INT_Rd16(p));
        S(" t8="); DC(_INT_Rd16(p + 2));
        S(" t9="); DC(_INT_Rd16(p + 4));
        S(" t10="); DC(_INT_Rd16(p + 6));
        S(" t11_t12="); DC(_INT_Rd16(p + 8)); NL();
    }
    if (sz >= 164 + 16 * 2) {
        const UINT8* q = d + 164 + panel * 2;
        UINT8 rate  = q[0] & 0xF;
        UINT8 lanes = (UINT8)(q[0] >> 4);
        UINT8 pre   = q[1] & 0xF;
        UINT8 vsw   = (UINT8)(q[1] >> 4);
        Info->HaveLink = TRUE;
        Info->LinkRate = rate;
        Info->LinkLanes = lanes;
        S("  VBT link params for this panel:"); NL();
        S("    rate="); DC(rate); S(" -> "); S(RateName(rate)); NL();
        S("    lanes="); DC(lanes); S(" -> "); S(LaneName(lanes)); NL();
        S("    preemphasis="); DC(pre); S(" vswing="); DC(vsw); NL();
    }
    if (sz >= 216) {
        UINT16 flt = _INT_Rd16(d + 212);
        UINT16 d600 = _INT_Rd16(d + 214);
        S("  fast_link_training bit="); DC((flt >> panel) & 1);
        S("  dpcd_600h_write_required bit="); DC((d600 >> panel) & 1); NL();
    }
}

BOOLEAN _INT_VbtParse(const UINT8* v, UINTN avail, _INT_Rep* R, _INT_VbtInfo* Info)
{
    if (avail < 48 || v[0] != '$' || v[1] != 'V' || v[2] != 'B' || v[3] != 'T') {
        S("VBT: signature not found"); NL();
        return FALSE;
    }

    UINT16 ver      = _INT_Rd16(v + 20);
    UINT16 hdr      = _INT_Rd16(v + 22);
    UINT16 vbt_size = _INT_Rd16(v + 24);
    UINT32 bdb_off  = _INT_Rd32(v + 28);
    UINTN  limit    = vbt_size < avail ? vbt_size : avail;

    Info->VbtFound = TRUE;
    Info->VbtVer = ver;

    S("VBT: signature=");
    for (UINTN i = 0; i < 20 && v[i]; i++) { CHAR8 t[2] = { (CHAR8)v[i], 0 }; S(t); }
    S(" version="); DC(ver);
    S(" header_size="); DC(hdr);
    S(" vbt_size="); DC(vbt_size);
    S(" bdb_offset="); DC(bdb_off); NL();

    if (bdb_off + 22 > limit) {
        S("VBT: bdb_offset out of range"); NL();
        return FALSE;
    }

    const UINT8* bdb = v + bdb_off;
    static const CHAR8 bsig[] = "BIOS_DATA_BLOCK";
    for (UINTN i = 0; bsig[i]; i++) {
        if (bdb[i] != (UINT8)bsig[i]) {
            S("BDB: signature mismatch"); NL();
            return FALSE;
        }
    }

    UINT16 bver  = _INT_Rd16(bdb + 16);
    UINT16 bhdr  = _INT_Rd16(bdb + 18);
    UINT16 bsize = _INT_Rd16(bdb + 20);
    Info->BdbVer = bver;
    S("BDB: version="); DC(bver);
    S(" header_size="); DC(bhdr);
    S(" bdb_size="); DC(bsize); NL();

    UINTN end = (UINTN)bdb_off + bsize;
    if (end > limit) end = limit;
    UINTN pos = (UINTN)bdb_off + bhdr;

    UINT8 panel = 0xFF;
    const UINT8* edp_d = NULL;
    UINTN edp_sz = 0;
    const UINT8* gen_d = NULL;
    UINTN gen_sz = 0;

    S("BDB blocks (id:size):");
    while (pos + 3 <= end) {
        UINT8  id = v[pos];
        UINT16 sz = _INT_Rd16(v + pos + 1);
        if (pos + 3 + sz > end) break;
        const UINT8* d = v + pos + 3;

        S(" "); DC(id); S(":"); DC(sz);

        if (id == BDB_GENERAL_DEFINITIONS) { gen_d = d; gen_sz = sz; }
        else if (id == BDB_EDP)            { edp_d = d; edp_sz = sz; }
        else if (id == BDB_LVDS_OPTIONS && sz >= 1) { panel = d[0]; }

        pos += 3 + sz;
    }
    NL();

    S("Child devices:"); NL();
    if (gen_d) ParseChildren(gen_d, gen_sz, R, Info);
    else { S("  (no general definitions block)"); NL(); }

    Info->PanelType = panel;
    S("LVDS options panel_type="); DC(panel); NL();

    if (edp_d) ParseEdp(edp_d, edp_sz, panel, R, Info);
    else { S("(no eDP block 27)"); NL(); }

    return TRUE;
}
