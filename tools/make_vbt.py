#!/usr/bin/env python3
"""
make_vbt.py - build t2gmux_vbt.bin for the t2-gmux-uefi loader (key I).

Apple's T2 firmware leaves the Intel OpRegion VBT mailbox empty, so the Windows
Intel driver does not know that an eDP panel is wired to DDI A and never starts
link training.  This tool takes a real, complete VBT (coreboot's Whiskey Lake
board, same gen9.5 display engine) and patches it for the Mac:

  * only the eDP child device (DDI A, AUX-A) stays enabled
  * eDP link rate / lane count / bpp for the active panel
  * native resolution + detailed timing taken from your panel's EDID
  * fast link training disabled (full training with DPCD)
  * header checksum recomputed

Usage:
  python make_vbt.py --edid edid.bin            [--lanes 4] [--rate hbr2] [-o t2gmux_vbt.bin]
  python make_vbt.py --edid-hex 00ffffffffffff00...        (hex string or a text file with hex)

Copy the result to the ESP root as  \\t2gmux_vbt.bin  and boot the loader with key I.

Layouts follow the Linux kernel's drivers/gpu/drm/i915/display/intel_vbt_defs.h.
"""
import argparse, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vbt_psr import apply_psr

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TEMPLATE = os.path.join(HERE, "template", "coreboot_google_sarien_data.vbt")

BDB_GENERAL_DEFINITIONS = 2
BDB_EDP = 27
BDB_LFP_OPTIONS = 40
BDB_LFP_DATA_PTRS = 41
BDB_LFP_BACKLIGHT = 43

RATES = {"rbr": 0, "hbr": 1, "hbr2": 2}
LANES = {1: 0, 2: 1, 4: 3}
BPP = {18: 0, 24: 1, 30: 2}


def load_edid(path, hexstr):
    if hexstr:
        txt = hexstr
    else:
        raw = open(path, "rb").read()
        if len(raw) >= 128 and raw[:8] == bytes([0, 255, 255, 255, 255, 255, 255, 0]):
            return raw
        txt = raw.decode("ascii", "ignore")
    h = re.sub(r"[^0-9a-fA-F]", "", txt)
    raw = bytes.fromhex(h[: len(h) // 2 * 2])
    if len(raw) < 128 or raw[:8] != bytes([0, 255, 255, 255, 255, 255, 255, 0]):
        sys.exit("EDID: not a valid EDID (needs the 00 FF FF FF FF FF FF 00 header, >=128 bytes)")
    return raw


def parse_blocks(v):
    bdb_off = struct.unpack_from("<I", v, 28)[0]
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
    return bdb_off, blocks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--edid", help="EDID file (binary, or text with hex)")
    ap.add_argument("--edid-hex", help="EDID as hex string")
    ap.add_argument("--template", default=DEFAULT_TEMPLATE)
    ap.add_argument("--lanes", type=int, choices=[1, 2, 4], default=4)
    ap.add_argument("--rate", choices=list(RATES), default="hbr2")
    ap.add_argument("--bpp", type=int, choices=[18, 24, 30], default=24)
    ap.add_argument("--keep-external", action="store_true", help="keep the template's DP-B/C/D children")
    ap.add_argument("--backlight", choices=["none", "pwm"], default="none",
                    help="none (default): tell Intel it has no backlight control - the T2 panel is dimmed by gmux (ACPI _BCM), "
                         "not by an Intel PWM; pwm: keep the template's PWM data")
    ap.add_argument("--keep-fast-link", action="store_true", help="leave fast link training as in the template")
    ap.add_argument("--psr", choices=["keep", "on", "off"], default="keep", help="Panel Self Refresh switch in the VBT (default keep = as in template); see tools/vbt_psr.py")
    ap.add_argument("--psr-idle-frames", type=int)
    ap.add_argument("--psr-tp1", type=int, help="us: 0,100,500,2500")
    ap.add_argument("--psr-tp2", type=int, help="us: 0,100,500,2500")
    ap.add_argument("--psr-full-link", type=int, choices=[0, 1])
    ap.add_argument("--psr-aux-wake", type=int, choices=[0, 1])
    ap.add_argument("-o", "--out", default="t2gmux_vbt.bin")
    a = ap.parse_args()

    if not (a.edid or a.edid_hex):
        ap.error("--edid or --edid-hex is required (panel timing comes from it)")
    edid = load_edid(a.edid, a.edid_hex)
    dtd = bytearray(edid[54:72])
    clock = struct.unpack_from("<H", dtd, 0)[0]
    if clock == 0:
        sys.exit("EDID: first detailed timing descriptor is empty (not a timing)")
    hact = dtd[2] | ((dtd[4] >> 4) << 8)
    vact = dtd[5] | ((dtd[7] >> 4) << 8)

    t = bytearray(open(a.template, "rb").read())
    if t[:4] != b"$VBT":
        sys.exit("template is not a VBT")
    vsize = struct.unpack_from("<H", t, 24)[0]
    t = t[:vsize]
    bdb_off, blocks = parse_blocks(t)
    for need in (BDB_GENERAL_DEFINITIONS, BDB_EDP, BDB_LFP_OPTIONS, BDB_LFP_DATA_PTRS):
        if need not in blocks:
            sys.exit("template lacks BDB block %d" % need)

    # panel index used by every per-panel table
    o, _ = blocks[BDB_LFP_OPTIONS]
    panel = t[o]
    if panel >= 16:
        panel = 0
        t[o] = 0
    print("template panel_type index:", panel)

    # --- child devices ---
    o, sz = blocks[BDB_GENERAL_DEFINITIONS]
    csz = t[o + 4]
    n = (sz - 5) // csz
    edp_seen = False
    for i in range(n):
        c = o + 5 + i * csz
        dtype = struct.unpack_from("<H", t, c + 2)[0]
        dvo = t[c + 16]
        is_edp = dtype in (0x1806, 0x78C6) and dvo == 10
        if is_edp and not edp_seen:
            edp_seen = True
            t[c + 25] = 0x40            # AUX-A
            print("child %d: eDP on DDI A, AUX-A kept" % i)
        elif not a.keep_external and dtype not in (0, 0xFFFF):
            t[c:c + csz] = bytes(csz)
            print("child %d: type %04X (dvo %d) disabled" % (i, dtype, dvo))
    if not edp_seen:
        sys.exit("template has no eDP child on DDI A")

    # --- eDP block ---
    o, sz = blocks[BDB_EDP]
    rate, lanes = RATES[a.rate], LANES[a.lanes]
    q = o + 164 + panel * 2
    t[q] = (rate & 0xF) | ((lanes & 0xF) << 4)
    cd = struct.unpack_from("<I", t, o + 160)[0]
    cd = (cd & ~(3 << (panel * 2))) | (BPP[a.bpp] << (panel * 2))
    struct.pack_into("<I", t, o + 160, cd)
    if not a.keep_fast_link and sz >= 214:
        flt = struct.unpack_from("<H", t, o + 212)[0] & ~(1 << panel)
        struct.pack_into("<H", t, o + 212, flt)
    print("eDP: rate=%s lanes=%d bpp=%d" % (a.rate, a.lanes, a.bpp))

    # --- native timing ---
    o, sz = blocks[BDB_LFP_DATA_PTRS]
    e = o + 1 + panel * 9
    fp_off, fp_sz = struct.unpack_from("<HB", t, e)
    dv_off, dv_sz = struct.unpack_from("<HB", t, e + 3)
    fp = bdb_off + fp_off
    dv = bdb_off + dv_off
    struct.pack_into("<HH", t, fp, hact, vact)
    t[dv:dv + 18] = dtd
    print("panel timing: %dx%d, pixel clock %.2f MHz (from EDID DTD)" % (hact, vact, clock / 100))

    # --- backlight (block 43): no Intel PWM, gmux owns the panel brightness ---
    if a.backlight == "none" and BDB_LFP_BACKLIGHT in blocks:
        o, sz = blocks[BDB_LFP_BACKLIGHT]
        es = t[o]
        for i in range(16):
            t[o + 1 + i * es] &= ~0x03                    # data[i].type: PWM(2) -> NONE(0)
            if o + 1 + 16 * es + 16 + i < o + sz:
                t[o + 1 + 16 * es + 16 + i] = 0x00       # backlight_control[i]: type NONE, controller 0
        print("backlight: type NONE (brightness is done by gmux / ACPI _BCM)")
    else:
        print("backlight: template PWM data kept")

    for line in apply_psr(t, a.psr, a.psr_idle_frames, a.psr_tp1, a.psr_tp2, a.psr_full_link, a.psr_aux_wake):
        print(line)

    # --- checksum: whole VBT must sum to 0 ---
    t[26] = 0
    t[26] = (-sum(t)) & 0xFF
    assert sum(t) & 0xFF == 0

    open(a.out, "wb").write(bytes(t))
    print("wrote %s (%d bytes, mailbox room 6144)" % (a.out, len(t)))
    if len(t) > 6144:
        print("WARNING: VBT larger than the 6 KB mailbox, loader will refuse it")


if __name__ == "__main__":
    main()