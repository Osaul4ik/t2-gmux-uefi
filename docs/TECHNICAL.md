# Technical details

Background for the loader's behaviour. Not needed to use it; see [../README.md](../README.md).

## ACPI patch (mode 4)

Mode **4** applies an ACPI patch before Windows starts with `\SSDT_IGPU.aml` (a missing file means the
mode cannot be started):

1. the only `_BCM` of the firmware table `SaSsdt` (`\_SB.PCI0.IGPU.DD1F._BCM`) is renamed to `XBCM`
   in memory; if the SSDT mentions `XWAK` / `XPTS`, the `_WAK` / `_PTS` of the DSDT (found through
   the FADT) are renamed to `XWAK` / `XPTS` too; checksums are fixed;
2. the SSDT file is appended to a copy of the XSDT (EfiACPIReclaimMemory) and published through a
   copy of the RSDP (`InstallConfigurationTable`); the old RSDP is updated in place when writable.

Checks: SSDT signature/length/checksum, exactly one `_BCM` in SaSsdt, memory writable; on any
later failure the renames are reverted. The result is shown on screen. The GPU device names
(`IGPU`, `GFX0`) are **not** touched: the SSDT uses the firmware's own names.

What the SSDT contains (brightness, sleep) and how to rebuild it:
[ACPI_PATCH_GUIDE.md](ACPI_PATCH_GUIDE.md). The `.asl` source and the generator are in `tools/`.

### Mode 5: brightness + resume patch

Mode 5 applies the same mechanism with its own file, `\SSDT_IGPU_BRT.aml` (source
`tools/SSDT_IGPU_BRT.asl`, procedure [ACPI_PATCH_GUIDE_MODE5.md](ACPI_PATCH_GUIDE_MODE5.md)). The file defines
the new `_BCM` and the new `_WAK`. It contains the name `XWAK` but not `XPTS`, so the loader renames only the
DSDT `_WAK` (and `_BCM` -> `XBCM` in `SaSsdt`); `_PTS` stays as the firmware has it. The new `_WAK` calls
`XWAK`, then writes the gmux routes again (DDC, panel -> iGPU, external) and restores the brightness: after
resume the firmware leaves the panel on the Radeon. There is no rail code, because the Radeon stays powered.

## Advanced menu: gpu-power-prefs (NVRAM)

Menu entry 6 is not a boot mode. It opens `AdvancedMenu()` in `bootx64.c`, which uses the runtime
services (`GetVariable` / `SetVariable`) on the Apple variable `gpu-power-prefs`, vendor GUID
`fa4ce28d-b62f-4c99-9cc3-6815686e30f9`, attributes `0x07` (non-volatile + boot services + runtime, as macOS
writes it). Entries: [FakeSecureBoot toggle + separator, only with `\FakeSecureBoot.efi`, see below], Switch to dGPU,
Switch to iGPU, a separator, Standart Boot + Intel Secondary, Reboot, Power off, Back.

- Switch to iGPU: `SetVariable` with the 4 bytes `01 00 00 00`.
- Switch to dGPU: `SetVariable` with size 0, which deletes the variable (an already absent variable is not
  an error).
- Both are verified by reading the variable back; the status line shows the result. After a verified
  success `ResetNow()` waits 1.5 s and calls `ResetSystem(EfiResetCold, ...)` so the firmware reads the new
  value. A failed write does not reboot. Reboot / Power off call the same function with `EfiResetCold` /
  `EfiResetShutdown` without changing anything.
- Visibility: value found with first byte `01` -> only Switch to dGPU; variable found with another value or
  `EFI_NOT_FOUND` -> only Switch to iGPU; any other read error -> both.
- Standart Boot + Intel Secondary makes `AdvancedMenu()` return TRUE: the caller starts mode 2
  (standard boot + AppleSetOs). Back / Esc return FALSE.

Modes 1 and 5 depend on the same variable: `GpuPrefsIsIGpu()` is true only when the variable is found
and its first byte is `01`. The loader reads it once at start and again after returning from the Advanced menu
(a successful switch reboots anyway). `ModeGpuOff()` is the single place that ties a mode to it: mode 5
(Hybrid Boot) is off while the value is not iGPU, mode 1 (Standart Boot) is off while it is iGPU. An
off mode is drawn as `unavailable: Switch to iGPU` / `unavailable: Switch to dGPU`, `MenuStep()` skips it for
Up/Down, its key (5 / 1) shows a status message, and `ModeAvailable()` refuses it (this also covers the
auto-boot default; the highlight then moves to mode 4). An unreadable variable counts as not confirmed
(= dGPU), so mode 5 stays unavailable and mode 1 stays available.

The firmware reads the variable early at boot, so a change only applies after a restart; that is why a
successful switch reboots at once. Nothing else is touched: no gmux access, no ACPI; the only file written is `t2gmux_default.txt`, and only by the
FakeSecureBoot switch. The Advanced
menu is never saved as the default mode. The main menu walks over `MenuOrder[]` = modes 1, 4, 5 and the
Advanced entry; mode 2 is not in it (a default file holding 2 falls back to mode 1; an unknown value falls
back to mode 4).

## FakeSecureBoot (optional, any mode)

`FakeSecureBoot.efi` ([Shmurkio/FakeSecureBoot](https://github.com/Shmurkio/FakeSecureBoot), `UEFI_DRIVER`)
saves `gRT->GetVariable` and replaces it with a hook that answers `SecureBoot` (global variable GUID
`8be4df61-93ca-11d2-aa0d-00e098032b8c`) with one byte `01` and passes every other query through.

- **Switch.** `AdvancedMenu()` lists `FakeSecureBoot: True/False` as the first entry (then a separator) only when
  `EspFileExists(FSB_FILE)` is true; that check runs once in `efi_main` before the frame is drawn, because it
  also decides the menu height (`gMenuRows` = 9 instead of 7; every row macro follows it, so without the file the
  screen is unchanged). Enter flips `Fsb` and calls `DefaultSave()` at once; a failed save is shown in the status
  line and the new value still counts for this boot.
- **File format.** `t2gmux_default.txt` is `<mode letter>\nFSB=<0|1>\n`. `DefaultLoad()` takes the mode from the first
  non-blank character as before (so an older loader still reads the file) and looks for `FSB=` separately
  (missing = off). `DefaultSave()` always writes both lines, so X and the switch never overwrite each other.
- **Chain.** `FsbStart()` runs after the mode's own steps (AppleSetOs, gmux, VBT, ACPI) and right before
  `StartImage(bootx64_original.efi)`: `LoadImage` + `StartImage` of `\FakeSecureBoot.efi` with our image as parent.
  The driver returns `EFI_SUCCESS`, which keeps it resident, so the image is unloaded only if the start failed.
  The result is checked by comparing `gRT->GetVariable` before and after, then a `SecureBoot` read through the
  hook (printed as `SecureBoot reads N`). Setting True with a missing file, a failed load or a failed start is
  logged and the boot goes on. Mode 1 is the same: the switch is the one exception to "nothing is touched".
- **Limits.** The driver is a boot-services image: its hook lives in boot-services memory, so it is meant for
  what runs before `ExitBootServices`. The upstream hook writes the answer without checking `Data` /
  `*DataSize`, so a caller that probes the size of `SecureBoot` with a NULL buffer would fault inside it.
  The loader does not change the driver; this is how `FakeSecureBoot.efi` itself behaves.

## Panel data from the dGPU (EDID substitution, modes 4, 5)

Apple's EFI publishes panel data only for the Radeon (its GOP handle carries the EDID protocol);
the iGPU has an empty VBT mailbox. After the built-in VBT is injected, the loader looks for an
internal-panel EDID (EDID active protocol first, then discovered; manufacturer `APP`, valid header
and checksum, first descriptor is a DTD) and writes its first DTD and the active size into the
injected VBT (BDB 41 for the panel index from BDB 40), then fixes the VBT checksum. If the firmware
exposes no such EDID, the panel EDID read over AUX is used instead. The screen shows `EDID from dGPU:
applied / not applied`.

Only the panel timing is substituted. Link rate, lanes and fast-link bits come from the panel DPCD
(see the live eDP probe section below), because the firmware does not publish them.

## Backlight in the VBT

`tools/make_vbt.py` writes the VBT backlight block (BDB 43) as **type NONE** by default
(`--backlight pwm` keeps the template's PWM data). The panel on the T2 MacBook is dimmed by gmux
(port 0x74, driven through ACPI `_BCM`, see the SSDT patch above), not by an Intel PWM, so the
Intel driver must not claim a PWM it cannot use.

## What the mux/rail modes do (4; mode 5 without the rail step)

Sequence follows Linux `apple-gmux` (T2 MMIO gmux):

1. interrupt mask (0x14) is saved and set to 0xFF, stale status (0x16) cleared
2. DDC (0x28)=iGPU, panel (0x10)=iGPU, external (0x40)=dGPU (Thunderbolt Macs);
   panel readback (0x10 bit 0) is verified, up to 3 attempts
3. rail OFF (0x50: 1, then 0) **only if step 2 was confirmed** - otherwise it is
   skipped, since cutting the Radeon while the panel is still routed to it blacks the screen
4. the POWER bit in 0x16 is polled (like Linux waits for the GPE); if it never
   shows, a fixed 250 ms delay is used
5. status is cleared and the original mask is restored

Mode 5 runs steps 1, 2 and 5 only: the panel is moved to the iGPU, but the rail step (3, 4) is never
issued and the Radeon stays powered.

Not done (cannot be done from Boot Services): ACPI `GMSP(0)` that Linux calls
when clearing MMIO-gmux interrupts, and the eDP link pre-calibration that
`vga_switcheroo` flags as `NEEDS_EDP_CONFIG` for T2 gmux (the iGPU has to train
the panel link itself, hence the VBT injection).

## VBT injection and DDI A 4 lanes (modes 4, 5)

Apple's T2 firmware leaves the Intel OpRegion VBT mailbox empty, so the Windows Intel driver does
not know an eDP panel sits on DDI A.

No VBT file is needed. The loader carries a VBT built from a real coreboot Whiskey Lake VBT (see
`tools/template/`, `tools/gen_vbt_base.py`; only the eDP child on DDI A stays enabled), completes it
from the panel (see the live eDP probe section below) and copies it into OpRegion+0x400, then re-reads it.
Mode 4 uses `SSDT_IGPU.aml`, mode 5 the same UEFI route with `SSDT_IGPU_BRT.aml`.

Modes **4** and **5** also set `DDI_BUF_CTL(A).DDI_A_4_LANES` (bit 4, GTTMMADR+0x64000) in the iGPU. On
gen < 11 i915 takes the DDI A lane limit from that bit, Apple's firmware sets it only when it lights
the panel from the iGPU, and with the dGPU as boot GPU it stays clear - the t2linux patch
"i915: 4 lane quirk for mbp15,1" works around exactly that. The write is skipped if BAR0 is
unassigned or the register reads all ones.

The template VBT is data from the coreboot project (GPL-2.0).

## Live eDP probe instead of a hand-made VBT (modes 4, 5)

The ACPI tables of the T2 Macs contain no eDP link training for either GPU. The "link training" names
in the Radeon SSDT (`LTRN`, `LTRC`, `LCRL`, `LSTS`, method `PUPD`) retrain the **PCIe** link of the PEG
slot. DisplayPort link training is done by the graphics driver: the Radeon driver for the dGPU, the Intel
driver for DDI A. The Intel driver only starts it if the VBT says an eDP panel is on DDI A, and it takes
the link limits (rate, lanes, PSR) from the VBT. Apple leaves the VBT empty, so the loader supplies one.

Instead of guessing lanes and rate, `lib/int_edp.c` asks the panel through the iGPU, as i915 would:

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
Host tests (no hardware, `gcc` only): `tests/host/run.sh`. They check the VBT patcher byte-for-byte against
`make_vbt.py`, and the AUX code against a simulated panel (retry, dead panel, panel already on, regs reading
all ones). They do not prove the sequence works on real silicon.