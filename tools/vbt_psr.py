#!/usr/bin/env python3
"""
vbt_psr.py - show / enable / disable / tune Panel Self Refresh (PSR) in an existing VBT.

Works on a finished t2gmux_vbt.bin (so hand-tuned eDP power sequences, bpp etc. are kept) and is also
imported by make_vbt.py (--psr).

Where PSR lives in the VBT (layouts: Linux drivers/gpu/drm/i915/display/intel_vbt_defs.h):
  * BDB version >= 228 : block 44 (LFP power), u16 `psr` at +24, one bit per panel_type
  * BDB version 165-227: block 12 (driver features), bit 9 of the u16 "driver feature flags" (last 2 bytes)
  * block 9 (PSR table), 6 bytes per panel_type: [flags: bit0 full_link, bit1 require_aux_to_wakeup]
    [idle_frames:4 | lines_to_wait:3] [tp1_wakeup u16] [tp2_tp3_wakeup u16]
    For BDB >= 205 the two wakeup fields are codes: 0=500us 1=100us 2=2500us 3=0us.

Usage:
  python tools/vbt_psr.py t2gmux_vbt.bin --show
  python tools/vbt_psr.py t2gmux_vbt.bin --psr off -o t2gmux_vbt_psr_off.bin
  python tools/vbt_psr.py t2gmux_vbt.bin --psr on --idle-frames 6 --tp1 500 --tp2 500 -o out.bin

PSR only does something if the panel itself reports PSR support in DPCD (0x070); the VBT only tells the
Intel driver that it may use it.
"""
import argparse, struct, sys

BDB_PSR, BDB_DRIVER_FEATURES, BDB_LFP_OPTIONS, BDB_LFP_POWER = 9, 12, 40, 44
WAKE_CODE = {500: 0, 100: 1, 2500: 2, 0: 3}
WAKE_US = {v: k for k, v in WAKE_CODE.items()}


def parse_blocks(v):
    bdb_off = struct.unpack_from("<I", v, 28)[0]
    ver = struct.unpack_from("<H", v, bdb_off + 16)[0]
    hdr = struct.unpack_from("<H", v, bdb_off + 18)[0]
    bsize = struct.unpack_from("<H", v, bdb_off + 20)[0]
    pos, end, blocks = bdb_off + hdr, bdb_off + bsize, {}
    while pos + 3 <= end:
        bid = v[pos]
        sz = struct.unpack_from("<H", v, pos + 1)[0]
        if pos + 3 + sz > end:
            break
        blocks.setdefault(bid, (pos + 3, sz))
        pos += 3 + sz
    return ver, blocks


def panel_type(v, blocks):
    p = v[blocks[BDB_LFP_OPTIONS][0]] if BDB_LFP_OPTIONS in blocks else 0
    return p if p < 16 else 0


def _flag_loc(v, blocks, ver):
    """Return ('mask', offset) for BDB>=228, ('flag', offset) for 165..227, else None."""
    if ver >= 228 and BDB_LFP_POWER in blocks and blocks[BDB_LFP_POWER][1] >= 26:
        return "mask", blocks[BDB_LFP_POWER][0] + 24
    if 165 <= ver < 228 and BDB_DRIVER_FEATURES in blocks:
        o, sz = blocks[BDB_DRIVER_FEATURES]
        if sz >= 19:
            return "flag", o + sz - 2
    return None


def get_state(v):
    ver, blocks = parse_blocks(v)
    p = panel_type(v, blocks)
    loc = _flag_loc(v, blocks, ver)
    en = None
    if loc:
        w = struct.unpack_from("<H", v, loc[1])[0]
        en = bool((w >> p) & 1) if loc[0] == "mask" else bool((w >> 9) & 1)
    tbl = None
    if BDB_PSR in blocks and blocks[BDB_PSR][1] >= 96:
        o = blocks[BDB_PSR][0] + p * 6
        f, w, t1, t2 = struct.unpack_from("<BBHH", v, o)
        us = (lambda c: WAKE_US.get(c, 2500)) if ver >= 205 else (lambda c: c * 100)
        tbl = dict(full_link=f & 1, aux_wake=(f >> 1) & 1, idle_frames=w & 15, lines_to_wait=(w >> 4) & 7,
                   tp1_us=us(t1), tp2_tp3_us=us(t2))
    return dict(bdb=ver, panel=p, where=loc[0] if loc else None, enabled=en, table=tbl)


def apply_psr(t, mode="keep", idle_frames=None, tp1=None, tp2=None, full_link=None, aux_wake=None):
    """Patch bytearray t in place, recompute checksum, return a list of log lines."""
    log = []
    ver, blocks = parse_blocks(t)
    p = panel_type(t, blocks)
    if mode in ("on", "off"):
        loc = _flag_loc(t, blocks, ver)
        if not loc:
            sys.exit("PSR: no place to switch it in this VBT (BDB %d)" % ver)
        w = struct.unpack_from("<H", t, loc[1])[0]
        bit = p if loc[0] == "mask" else 9
        w = (w | (1 << bit)) if mode == "on" else (w & ~(1 << bit))
        struct.pack_into("<H", t, loc[1], w)
        log.append("PSR %s (%s, bit %d, BDB %d)" % (mode.upper(), "block 44 mask" if loc[0] == "mask"
                                                  else "block 12 flags", bit, ver))
    if any(x is not None for x in (idle_frames, tp1, tp2, full_link, aux_wake)):
        if BDB_PSR not in blocks or blocks[BDB_PSR][1] < 96:
            sys.exit("PSR: VBT has no (complete) block 9")
        o = blocks[BDB_PSR][0] + p * 6
        f, w, t1, t2 = struct.unpack_from("<BBHH", t, o)
        if full_link is not None: f = (f & ~1) | (1 if full_link else 0)
        if aux_wake is not None:  f = (f & ~2) | (2 if aux_wake else 0)
        if idle_frames is not None:
            if not 0 <= idle_frames <= 15: sys.exit("--idle-frames must be 0..15")
            w = (w & ~15) | idle_frames
        for name, val in (("tp1", tp1), ("tp2", tp2)):
            if val is None: continue
            if ver >= 205:
                if val not in WAKE_CODE: sys.exit("--%s must be one of 0,100,500,2500 (us)" % name)
                code = WAKE_CODE[val]
            else:
                code = val // 100
            if name == "tp1": t1 = code
            else: t2 = code
        struct.pack_into("<BBHH", t, o, f, w, t1, t2)
        log.append("PSR table[panel %d] updated" % p)
    vsize = struct.unpack_from("<H", t, 24)[0]
    t[26] = 0
    t[26] = (-sum(t[:vsize])) & 0xFF
    assert sum(t[:vsize]) & 0xFF == 0
    return log


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vbt")
    ap.add_argument("--show", action="store_true")
    ap.add_argument("--psr", choices=["keep", "on", "off"], default="keep")
    ap.add_argument("--idle-frames", type=int)
    ap.add_argument("--tp1", type=int, help="TP1 wakeup us: 0,100,500,2500")
    ap.add_argument("--tp2", type=int, help="TP2/TP3 wakeup us: 0,100,500,2500")
    ap.add_argument("--full-link", type=int, choices=[0, 1])
    ap.add_argument("--aux-wake", type=int, choices=[0, 1])
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    t = bytearray(open(a.vbt, "rb").read())
    if t[:4] != b"$VBT":
        sys.exit("not a VBT")
    print("before:", get_state(t))
    if a.show or not a.out:
        return
    for l in apply_psr(t, a.psr, a.idle_frames, a.tp1, a.tp2, a.full_link, a.aux_wake):
        print(l)
    open(a.out, "wb").write(bytes(t))
    print("after: ", get_state(t))
    print("wrote %s (%d bytes)" % (a.out, len(t)))


if __name__ == "__main__":
    main()