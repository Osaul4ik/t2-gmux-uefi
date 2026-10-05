# How to make `t2gmux_vbt.bin` (VBT for modes 3 and 4)

The T2 firmware leaves the Intel OpRegion VBT mailbox empty, so the Windows Intel driver does not know
that an eDP panel sits on DDI A and never starts link training. The VBT file fixes that.

| Mode | How the VBT is used |
|------|---------------------|
| 3, Integrated gfx | `t2gmux_vbt.bin` is embedded **into** `SSDT_IGPU_VBT.aml` at build time (`make_ssdt_igpu.py --vbt`). The file itself is not needed on the ESP. |
| 4, Integrated gfx + separate VBT | `t2gmux_vbt.bin` is copied to the ESP root and injected by the loader. Used together with `SSDT_IGPU.aml` (built without `--vbt`). |

Modes 1 and 2 do not use a VBT.

## 1. What you need (minimum)

**Only one thing: the EDID of the internal panel.** That is a 128-byte (or 256-byte) blob, taken once.

| Input | Required | From where |
|-------|----------|------------|
| EDID of the internal panel (`edid_N.bin`) | **yes** | Windows (Boot Camp), see 2.1 |
| Link lanes and link rate of the panel | no, defaults `--lanes 4 --rate hbr2` | only if the panel stays dark, see 4 |
| anything from macOS | **no** | only an alternative source of the EDID, see 2.2 |

Everything else (the VBT skeleton, the eDP child on DDI A, the backlight block) comes from the template
in the repo, and the loader later replaces the panel timing with the one the Radeon reports (mode 4).

Before you start, check that `tools/template/coreboot_google_sarien_data.vbt` is **not empty**. It is a
binary file and some archives turn it into 0 bytes; `make_vbt.py` fails with an empty template. Restore it
from the repository or from coreboot (board `google/sarien`) first.

## 2. Take the EDID

### 2.1 Windows (recommended, nothing to install)

Run in PowerShell, from the `tools` folder (or copy `get_edid.ps1` anywhere):

```
powershell -ExecutionPolicy Bypass -File .\get_edid.ps1
```

It writes `edid_1.bin`, `edid_2.bin`, ... for every monitor Windows has ever seen and prints the registry
path of each. Pick the internal panel:

- the manufacturer code is **`APP`** (the registry path contains something like `DISPLAY\APPxxxx`);
- the EDID is 128 or 256 bytes;
- ignore external monitors and stale entries from other displays.

If several `APP` entries exist, use the one whose key is under the currently present device. When in doubt,
build with each and compare the resolution that `make_vbt.py` prints.

Take the EDID while Windows is running on the **Radeon** (mode 1 or 2), or in any mode: the registry copy
is the panel EDID that Windows read earlier, so it does not matter which GPU you booted from.

### 2.2 macOS (only if you have no Windows EDID)

macOS is not needed. If you still want the EDID from there, open Terminal:

```
ioreg -lw0 -r -c AppleDisplay | grep IODisplayEDID
```

If that prints nothing, try:

```
ioreg -lw0 -r -c IODisplayConnect | grep IODisplayEDID
```

The line looks like `"IODisplayEDID" = <00ffffffffffff00...>`. Copy the hex between `<` and `>` and
pass it to the tool with `--edid-hex` (or save it to a text file and use `--edid file.txt`, the tool
accepts hex text). The internal panel again has manufacturer `APP`; the hex starts with `00ffffffffffff00`
and the bytes after it encode the manufacturer. The output format of `ioreg` differs between macOS
versions; if neither command prints an EDID, use Windows.

## 3. Build the VBT

From the repo root:

```
python tools/make_vbt.py --edid edid_1.bin --lanes 4 --rate hbr2 -o t2gmux_vbt.bin
```

or, with the hex from macOS:

```
python tools/make_vbt.py --edid-hex 00ffffffffffff00... --lanes 4 --rate hbr2 -o t2gmux_vbt.bin
```

What the tool does: starts from a real coreboot Whiskey Lake VBT, keeps only the eDP child on DDI A
(AUX-A), sets lane count / link rate / bpp, takes the native resolution and detailed timing from the
EDID, disables fast link training, sets the backlight block to type NONE (the panel is dimmed by gmux,
not by an Intel PWM), and recomputes the checksum.

Options you might need: `--bpp 18|24|30` (default 24), `--backlight pwm` (keep the template's PWM data,
normally not wanted), `--keep-fast-link`, `--keep-external`, `--template <file>`.

## 4. If the panel stays dark

The tool does not read the panel's real link capabilities (DPCD); it uses defaults. Retry in this order,
one change at a time, and note which one works:

1. `--lanes 4 --rate hbr2` (default)
2. `--lanes 2 --rate hbr2`
3. `--lanes 4 --rate hbr`
4. `--lanes 2 --rate hbr`

Do not assume a value is right before you have seen a working picture.

Optional, to know the real values instead of guessing: boot Linux with the panel on the iGPU and read the
first DPCD bytes of the eDP AUX channel (byte 1 = max link rate: `0x06` RBR, `0x0A` HBR, `0x14` HBR2;
byte 2, low 5 bits = max lane count):

```
sudo xxd -l 16 /dev/drm_dp_aux0      # the right aux device number can differ
```

This needs a kernel with T2 support and a panel routed to the iGPU, so it is not always possible; the
trial order above needs nothing but the loader.

## 5. Use the result

**Mode 4** (Integrated gfx + separate VBT):

1. Build `SSDT_IGPU.aml` **without** `--vbt` (see [ACPI_PATCH_GUIDE.md](ACPI_PATCH_GUIDE.md)).
2. Copy to the ESP root: `SSDT_IGPU.aml` and `t2gmux_vbt.bin`.
3. In the loader press **4**. If either file is missing, the mode is refused and the status line names it.

**Mode 3** (Integrated gfx):

1. Build `SSDT_IGPU_VBT.aml` **with** the VBT:
   `python tools/make_ssdt_igpu.py --vbt t2gmux_vbt.bin -o SSDT_IGPU_VBT.aml`
2. Copy to the ESP root: `SSDT_IGPU_VBT.aml` (no `t2gmux_vbt.bin` needed).
3. In the loader press **3**.

Never rename `SSDT_IGPU_VBT.aml` to `SSDT_IGPU.aml` or the other way round: in mode 4 the SSDT's VBT would
overwrite the injected VBT and the EDID timing taken from the Radeon.

## 6. Check the file

```
python - <<'EOF'
d = open("t2gmux_vbt.bin", "rb").read()
print(len(d), d[:4])          # expect b'$VBT' and a size below 0x1800 (6144) bytes
EOF
```

`make_vbt.py` also prints the panel resolution it took from the EDID; it must match the real panel
(for example the native resolution of your MacBook model). If it does not, you picked the wrong EDID.

The template VBT is data from the coreboot project (GPL-2.0).
