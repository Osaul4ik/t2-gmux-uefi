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
the iGPU has an empty VBT mailbox. After `t2gmux_vbt.bin` is injected, the loader looks for an
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

1. Get your panel EDID in Windows: `tools/get_edid.ps1` (writes `edid_N.bin`).
2. `python tools/make_vbt.py --edid edid_1.bin --lanes 4 --rate hbr2`
   -> `t2gmux_vbt.bin` (built from a real coreboot Whiskey Lake VBT, see
   `tools/template/`; only the eDP child on DDI A stays enabled).
3. Copy `t2gmux_vbt.bin` to the ESP root (together with `SSDT_IGPU.aml`).
4. At the countdown press **4**. The loader copies the VBT into OpRegion+0x400 and re-reads it.

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