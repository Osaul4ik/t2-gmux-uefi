# GMUX_Control v0.8

T2 gmux / dGPU loader with AppleSetOs (formerly "apple_set_os loader").

UEFI loader (Boot Services, before Windows) for the MacBook Pro 2019 (T2, Intel iGPU + AMD Radeon).

How to build the ACPI patch (brightness, sleep, VBT): see [docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md).

## Boot mode menu

The screen is a small ASCII frame: title, status line, the mode menu, the auto-boot timer line and
the list of graphics cards (refreshed every 3 s). Pick a mode with **Up / Down + Enter** (Space also
confirms), or press its number (**1**-**4**) to start it at once. If no key is pressed for 5 s, the **default**
mode starts automatically; any key stops the timer.

The default mode is marked with an **x** in front of its row. Press **X** to save the highlighted
mode as the default: the loader writes its number to `\t2gmux_default.txt` in the ESP root (the
status line confirms it). Without that file the default is **4**. Delete the file to go back to 4.
Old default files with a letter (`D`, `A`, `H`, `I`) are still understood and map to 1, 2, 3, 4.
X only saves, it does not boot.

| Key | Mode | mux panel→iGPU | dGPU rail OFF | What it does |
|-----|------|----------------|---------------|--------------|
| **1** | Standard Boot | no | no | clean boot **without AppleSetOs**, gmux, injects or ACPI patches, no files |
| **2** | Boot + Apple_set_os | no | no | standard boot + the **apple_set_os patch** (AppleSetOs only): no gmux, no VBT / `DDI_A_4_LANES` / ACPI patches, no files; then boots Windows |
| **3** | Integrated gfx | yes | **yes** | AppleSetOs + mux panel->iGPU + Radeon rail OFF + `DDI_A_4_LANES` + full ACPI patch **with the VBT inside the SSDT** (`\SSDT_IGPU_VBT.aml`); no VBT injection from UEFI, no files |
| **4** | Integrated gfx + separate VBT (default) | yes | yes | everything from 3, but with the ACPI patch **without VBT** (`\SSDT_IGPU.aml`) + separate VBT injected from UEFI (`\t2gmux_vbt.bin`) + EDID timing from the dGPU |

**Required files.** A mode whose files are missing in the ESP root cannot be started: the menu stays,
the timer stops and the status line says which file is missing. This also applies to the auto-boot
default (e.g. a missing file stops the countdown instead of booting a half-configured mode).

| Mode | Files in the ESP root |
|------|-----------------------|
| 1, 2 | none |
| 3 | `SSDT_IGPU_VBT.aml` |
| 4 | `SSDT_IGPU.aml` + `t2gmux_vbt.bin` |

AppleSetOs is loaded for modes 2, 3 and 4 (the iGPU has to become visible). Mode 1 skips it.
The loader writes nothing to the ESP except `t2gmux_default.txt` (key X); the result of each step is
only shown on screen.

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
[docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md). The `.asl` source and the generator are in `tools/`.

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
the panel link itself, hence the VBT injection below).

## WARNING

- After dGPU rail OFF (modes **3** / **4**), do **not** power Radeon back on from Windows.
- Mode **3** does not inject a VBT from UEFI and cuts the Radeon rail: its VBT comes from the SSDT
  (`SSDT_IGPU_VBT.aml`, built with `make_ssdt_igpu.py --vbt`). Without that
  file the mode is refused, since an empty OpRegion VBT mailbox can leave the panel dark.
- Do not copy the `--vbt` SSDT as `SSDT_IGPU.aml` for mode **4**: AML runs after UEFI and would overwrite the
  VBT (and the EDID timing taken from the dGPU) with the file's timing.

## Install / build

1. Secure Boot = No Security  
2. Rename `/EFI/Boot/bootx64.efi` → `bootx64_original.efi`  
3. Copy built `bootx64.efi` to `/EFI/Boot/`  

```bash
docker build -t apple_set_os_loader .
docker run --rm -v "$(pwd):/build" apple_set_os_loader make clean all
```

## VBT injection (mode 4) - for "no eDP link training" on the iGPU

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