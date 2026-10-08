# Technical details

Background for the loader's behaviour. Not needed to use it; see [../README.md](../README.md).

## ACPI patch (modes 3, 4)

Modes **3** and **4** apply an ACPI patch before Windows starts: mode 3 uses `\SSDT_IGPU_VBT.aml`,
mode 4 uses `\SSDT_IGPU.aml` (a missing file means the mode cannot be started):

1. the only `_BCM` of the firmware table `SaSsdt` (`\_SB.PCI0.IGPU.DD1F._BCM`) is renamed to `XBCM`
   in memory; if the SSDT mentions `XWAK` / `XPTS`, the `_WAK` / `_PTS` of the DSDT (found through
   the FADT) are renamed to `XWAK` / `XPTS` too; checksums are fixed;
2. the SSDT file is appended to a copy of the XSDT (EfiACPIReclaimMemory) and published through a
   copy of the RSDP (`InstallConfigurationTable`); the old RSDP is updated in place when writable.

Checks: SSDT signature/length/checksum, exactly one `_BCM` in SaSsdt, memory writable; on any
later failure the renames are reverted. The result is shown on screen. The GPU device names
(`IGPU`, `GFX0`) are **not** touched: the SSDT uses the firmware's own names.

What the SSDT contains (brightness, sleep, optional VBT) and how to rebuild it:
[ACPI_PATCH_GUIDE.md](ACPI_PATCH_GUIDE.md). The `.asl` source and the generator are in `tools/`.

## Panel data from the dGPU (EDID substitution, mode 4)

Apple's EFI publishes panel data only for the Radeon (its GOP handle carries the EDID protocol);
the iGPU has an empty VBT mailbox. After the built-in VBT is injected, the loader looks for an
internal-panel EDID (EDID active protocol first, then discovered; manufacturer `APP`, valid header
and checksum, first descriptor is a DTD) and writes its first DTD and the active size into the
injected VBT (BDB 41 for the panel index from BDB 40), then fixes the VBT checksum. If the firmware
exposes no such EDID, the timing from the file is kept. The screen shows `EDID from dGPU:
applied / not applied`.

Only the panel timing is substituted. Link rate, lanes and fast-link bits still come from the VBT
file, because the firmware does not publish them.

## Backlight in the VBT

`tools/make_vbt.py` writes the VBT backlight block (BDB 43) as **type NONE** by default
(`--backlight pwm` keeps the template's PWM data). The panel on the T2 MacBook is dimmed by gmux
(port 0x74, driven through ACPI `_BCM`, see the SSDT patch above), not by an Intel PWM, so the
Intel driver must not claim a PWM it cannot use.

## What the mux/rail modes do (3, 4)

Sequence follows Linux `apple-gmux` (T2 MMIO gmux):

1. interrupt mask (0x14) is saved and set to 0xFF, stale status (0x16) cleared
2. DDC (0x28)=iGPU, panel (0x10)=iGPU, external (0x40)=dGPU (Thunderbolt Macs);
   panel readback (0x10 bit 0) is verified, up to 3 attempts
3. rail OFF (0x50: 1, then 0) **only if step 2 was confirmed** - otherwise it is
   skipped, since cutting the Radeon while the panel is still routed to it blacks the screen
4. the POWER bit in 0x16 is polled (like Linux waits for the GPE); if it never
   shows, a fixed 250 ms delay is used
5. status is cleared and the original mask is restored

Not done (cannot be done from Boot Services): ACPI `GMSP(0)` that Linux calls
when clearing MMIO-gmux interrupts, and the eDP link pre-calibration that
`vga_switcheroo` flags as `NEEDS_EDP_CONFIG` for T2 gmux (the iGPU has to train
the panel link itself, hence the VBT injection).

## VBT injection and DDI A 4 lanes (modes 3, 4)

Apple's T2 firmware leaves the Intel OpRegion VBT mailbox empty, so the Windows Intel driver does
not know an eDP panel sits on DDI A.

Mode 4 needs no VBT file. The loader carries a VBT built from a real coreboot Whiskey Lake VBT (see
`tools/template/`, `tools/gen_vbt_base.py`; only the eDP child on DDI A stays enabled), completes it
from the panel (see the live eDP probe section below) and copies it into OpRegion+0x400, then re-reads it.
Copy `SSDT_IGPU.aml` to the ESP root and press **4** at the countdown.

(Mode 3 delivers the VBT through ACPI instead: `SSDT_IGPU_VBT.aml`, see the guide.)

Modes **3** and **4** also set `DDI_BUF_CTL(A).DDI_A_4_LANES` (bit 4, GTTMMADR+0x64000) in the iGPU. On
gen < 11 i915 takes the DDI A lane limit from that bit, Apple's firmware sets it only when it lights
the panel from the iGPU, and with the dGPU as boot GPU it stays clear - the t2linux patch
"i915: 4 lane quirk for mbp15,1" works around exactly that. The write is skipped if BAR0 is
unassigned or the register reads all ones.

If the panel stays dark try `--lanes 2` / `--rate hbr` (the real values are in the panel DPCD; this
tool does not read them). The template VBT is data from the coreboot project (GPL-2.0).

## Backlight in the VBT

`tools/make_vbt.py` writes the VBT backlight block (BDB 43) as **type NONE** by default
(`--backlight pwm` keeps the template's PWM data). The panel on the T2 MacBook is dimmed by gmux
(port 0x74, driven through ACPI `_BCM`, see the ACPI patch above), not by an Intel PWM, so the
Intel driver must not claim a PWM it cannot use.
## Live eDP probe instead of a hand-made VBT (mode 4)

The ACPI tables of the T2 Macs contain no eDP link training for either GPU. The "link training" names
in the Radeon SSDT (`LTRN`, `LTRC`, `LCRL`, `LSTS`, method `PUPD`) retrain the **PCIe** link of the PEG
slot. DisplayPort link training is done by the graphics driver: the Radeon driver for the dGPU, the Intel
driver for DDI A. The Intel driver only starts it if the VBT says an eDP panel is on DDI A, and it takes
the link limits (rate, lanes, PSR) from the VBT. Apple leaves the VBT empty, so the loader supplies one.

Instead of guessing `--lanes/--rate`, `lib/int_edp.c` asks the panel through the iGPU, as i915 would:

1. Request the power wells AUX-A needs (`PWR_WELL_CTL2`: PW1 and DDI A/E IO) if they are down.
2. If the panel is off (`PP_STATUS` bit 31 clear), force VDD (`PP_CONTROL` bit 3), wait 150 ms, release it after.
3. Native AUX reads on `DP_AUX_CH_CTL_A` (0x64010): DPCD `0x000..0x00F`, `0x700` (eDP rev), `0x010` (eDP 1.4
   `SUPPORTED_LINK_RATES`), `0x070` (PSR). Up to 5 attempts per transfer, every wait bounded.
4. EDID block 0 over I2C-over-AUX (address 0x50).
5. `_INT_VbtSetLink` patches rate / lanes (BDB 27), clears fast link training, fixes the checksum. PSR is always
   left **off** (base VBT generated with `--psr off`, and the probe never enables it): with PSR on the Intel
   driver makes the T2 panel flicker. The DTD comes from the dGPU's EDID, else from step 4.

If the probe fails, the built-in defaults (4 x HBR2) are kept; if there is no EDID at all the mailbox is left
empty rather than given a placeholder timing. `DDI_A_4_LANES` is only forced when the panel reports 4 lanes.
The inject log (`*_inject.txt` with key D) lists every step, including the raw register values.

Host tests (no hardware, `gcc` only): `tests/host/run.sh`. They check the VBT patcher byte-for-byte against
`make_vbt.py`, and the AUX code against a simulated panel (retry, dead panel, panel already on, regs reading
all ones). They do not prove the sequence works on real silicon.
