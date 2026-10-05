# GMUX_Control v0.7

T2 gmux / dGPU loader with AppleSetOs (formerly "apple_set_os loader").

UEFI loader (Boot Services, before Windows).

## Boot mode menu

The screen is a small ASCII frame: title, status line, the mode menu, the auto-boot timer line and
the list of graphics cards (refreshed every 3 s). Pick a mode with **Up / Down + Enter** (Space also
confirms), or press its letter to start it at once. If no key is pressed for 5 s, mode **I**
(AppleSetOs + injects) starts automatically; any key stops the timer.

| Key | Mode | mux panel→iGPU | dGPU rail OFF | Log files | What it does |
|-----|------|----------------|---------------|-----------|--------------|
| **D** | Only AMD Radeon | no | no | no | plain Windows boot: **no AppleSetOs**, no gmux, no injects, no files |
| **A** | AMD Radeon + Intel HD | no | no | no | **AppleSetOs only**: no gmux, no VBT/`DDI_A_4_LANES`/ACPI patches, no files; then boots Windows |
| **P** | AppleSetOs + ACPI patch | no | no | no | **AppleSetOs + ACPI patch only**: loads AppleSetOs, then applies `\\SSDT_IGPU.aml` (if present). No gmux, no rail, no VBT/`DDI_A_4_LANES`, no files |
| **K** | ACPI patch + 4 lanes | no | no | no | AppleSetOs + ACPI patch (`\\SSDT_IGPU.aml`) + `DDI_A_4_LANES`; **no VBT injection**, no gmux, no rail, no files |
| **J** | Mux Intel + ACPI patch, Radeon ON | yes | **no** | no | same as H, but the Radeon rail is **not** switched off: AppleSetOs + mux panel->iGPU + `DDI_A_4_LANES` + ACPI patch (`\\SSDT_IGPU.aml`); **no VBT injection**, no files |
| **H** | Mux Intel + 4 lanes + ACPI patch | yes | **yes** | no | AppleSetOs + mux panel->iGPU + Radeon rail OFF + `DDI_A_4_LANES` + ACPI patch (`\\SSDT_IGPU.aml`); **no VBT injection**, no files |
| **I** | Intel HD (auto after 5 s, selected at start) | yes | yes | no | AppleSetOs + full switch to the iGPU: mux + Radeon rail OFF + VBT injection + `DDI_A_4_LANES` + ACPI patch (if `\SSDT_IGPU.aml` exists) |
| **U** | Intel HD, Radeon ON | yes | **no** | no | same as I, but the Radeon rail is **not** switched off: AppleSetOs + mux + VBT injection + `DDI_A_4_LANES` + ACPI patch |
| **L** | Intel HD + Logs | yes | yes | yes | same as I, plus OpRegion/VBT/register dumps before/after and `inject.txt` |

AppleSetOs is loaded for A, P, K, J, H, I, U and L (the iGPU has to become visible). D skips it.
I writes nothing to the ESP; the result is only shown on screen.

## ACPI patch for brightness (P, K, J, H, I, U, L)

If `\SSDT_IGPU.aml` exists in the ESP root, keys **P**, **K**, **J**, **H**, **I**, **U** and **L** apply an ACPI patch before
Windows starts (no file = skipped, nothing is touched):

1. the only `_BCM` of the firmware table `SaSsdt` (`\_SB.PCI0.IGPU.DD1F._BCM`) is renamed to `XBCM`
   in memory, the table checksum is fixed;
2. the SSDT file is appended to a copy of the XSDT (EfiACPIReclaimMemory) and published through a
   copy of the RSDP (`InstallConfigurationTable`); the old RSDP is updated in place when writable.

The SSDT defines the new `DD1F._BCM`: it calls the original `XBCM` (Intel path) and Apple's own
`GFX0.ABCM` (gmux port 0x74 = pct * 0xFFFF / 100). Checks: SSDT signature/length/checksum, exactly
one `_BCM` in SaSsdt, memory writable; on any later failure the rename is reverted. The result is
shown on screen (row 21); with **L** it is also written to `t2gmux_L_<boot>_inject.txt`.

## Panel data from the dGPU (EDID substitution, I / U / L)

Apple's EFI publishes panel data only for the Radeon (its GOP handle carries the EDID protocol);
the iGPU has an empty VBT mailbox. After `t2gmux_vbt.bin` is injected, the loader looks for an
internal-panel EDID (EDID active protocol first, then discovered; manufacturer `APP`, valid header
and checksum, first descriptor is a DTD) and writes its first DTD and the active size into the
injected VBT (BDB 41 for the panel index from BDB 40), then fixes the VBT checksum. If the firmware
exposes no such EDID, the timing from the file is kept. Screen row 8 shows `EDID from dGPU:
applied / not applied`; the details (old -> new pixel clock) are in `inject.txt` with **L**.

Only the panel timing is substituted. Link rate, lanes and fast-link bits still come from the VBT
file, because the firmware does not publish them.

## Backlight in the VBT

`tools/make_vbt.py` writes the VBT backlight block (BDB 43) as **type NONE** by default
(`--backlight pwm` keeps the template's PWM data). The panel on the T2 MacBook is dimmed by gmux
(port 0x74, driven through ACPI `_BCM`, see the SSDT patch above), not by an Intel PWM, so the
Intel driver must not claim a PWM it cannot use.

## What the mux/rail keys do (J, H, I, U, L)

Sequence follows Linux `apple-gmux` (T2 MMIO gmux):

1. interrupt mask (0x14) is saved and set to 0xFF, stale status (0x16) cleared
2. DDC (0x28)=iGPU, panel (0x10)=iGPU, external (0x40)=dGPU (Thunderbolt Macs);
   panel readback (0x10 bit 0) is verified, up to 3 attempts
3. rail OFF (0x50: 1, then 0) **only for H / I / L and only if step 2 was confirmed** - otherwise it is
   skipped, since cutting the Radeon while the panel is still routed to it blacks
   the screen; **J** and **U** skip this step on purpose and leave the Radeon powered
4. the POWER bit in 0x16 is polled (like Linux waits for the GPE); if it never
   shows, a fixed 250 ms delay is used
5. status is cleared and the original mask is restored

Not done (cannot be done from Boot Services): ACPI `GMSP(0)` that Linux calls
when clearing MMIO-gmux interrupts, and the eDP link pre-calibration that
`vga_switcheroo` flags as `NEEDS_EDP_CONFIG` for T2 gmux (the iGPU has to train
the panel link itself, hence the VBT injection below).

## WARNING

- After dGPU rail OFF (**H** / **I** / **L**), do **not** power Radeon back on from Windows.
- **J** / **H** do not inject a VBT (**H** also cuts the Radeon rail): the iGPU only has a VBT if the firmware
  booted from it (Intel boot). On a Radeon boot the OpRegion VBT mailbox is empty, so the panel may stay dark.

## Install / build

1. Secure Boot = No Security  
2. Rename `/EFI/Boot/bootx64.efi` → `bootx64_original.efi`  
3. Copy built `bootx64.efi` to `/EFI/Boot/`  

```bash
docker build -t apple_set_os_loader .
docker run --rm -v "$(pwd):/build" apple_set_os_loader make clean all
```

## Output file names

Only key **L** writes files. Every dump file is written to the ESP root as

    t2gmux_L_<boot>_<what>

- `<boot>` = `igpu` if the Intel iGPU was already visible before AppleSetOs
  (the firmware booted from the iGPU), otherwise `rad` (Radeon boot)
- `<what>` = `before.txt`, `before_opregion.bin`, `before_vbt.bin`,
  `after.txt`, `after_opregion.bin`, `after_vbt.bin`, `regs_before.txt`,
  `regs_after.txt`, `inject.txt`

Example: `t2gmux_L_rad_after.txt` = key L on a Radeon boot. The countdown screen
shows the prefix on its last line (`files: \t2gmux_L_rad_*`). The input file
`t2gmux_vbt.bin` keeps its name.

## OpRegion / VBT dump (key L)

Diagnoses "no eDP link training when the Intel driver loads": the Intel
driver takes DDI port, AUX channel, link rate, lane count and panel power
timings from the VBT in the OpRegion (PCI config 0xFC `ASLS` of the iGPU).

Files written to the ESP root (before = before `apple_set_os`, after = after
apple_set_os + gmux/rail actions; key L writes both):

- `before.txt` / `after.txt` – readable report (gmux readback, OpRegion, VBT child devices, eDP block)
- `*_opregion.bin`, `*_vbt.bin` – raw data

All file names follow the scheme in "Output file names" above.

Share the `.txt` and `_vbt.bin` files to analyse them. Parser is checked only
against a synthetic VBT, not yet against real hardware.

## iGPU register snapshot (key L)

Besides the OpRegion/VBT files, L writes two register snapshots of the
Intel display engine (BAR0): `regs_before.txt` (before AppleSetOs and the
mux) and `regs_after.txt` (after mux, rail and injection). They hold power
wells, CDCLK/DPLL, DDI A (`DDI_BUF_CTL`, `DP_TP_*`, AUX), the eDP transcoder
(function control, timings, M/N), pipe A/plane 1, the panel power sequencer
(`PP_*`) and the backlight PWM (`BLC_PWM_*`).

Why: compare the state before and after the mux/rail/injection steps to see which
registers the loader changes (PP_ON_DELAYS, BLC_PWM_*, DP_TP_CTL, DPLL, TRANS_DDI_FUNC_CTL_EDP ...).

- On a Radeon boot the iGPU is hidden until AppleSetOs, so `before` only says
  "no Intel iGPU visible"; the cold state is in `after`.
- All ones (`0xFFFFFFFF`) means that block was powered down, which is information too.
- Offsets are gen 9/9.5 with the CNP PCH layout (Linux i915 names); not yet
  checked against this hardware.

## VBT injection (keys I, L) - for "no eDP link training" on the iGPU

Apple's T2 firmware leaves the Intel OpRegion VBT mailbox empty (see the dump
above), so the Windows Intel driver does not know an eDP panel sits on DDI A.

1. Get your panel EDID in Windows: `tools/get_edid.ps1` (writes `edid_N.bin`).
2. `python tools/make_vbt.py --edid edid_1.bin --lanes 4 --rate hbr2`
   -> `t2gmux_vbt.bin` (built from a real coreboot Whiskey Lake VBT, see
   `tools/template/`; only the eDP child on DDI A stays enabled).
3. Copy `t2gmux_vbt.bin` to the ESP root.
4. At the countdown press **I** (mux + Radeon OFF + inject) or **L** (same, plus log files). The loader copies the
   VBT into OpRegion+0x400 and re-reads it; with **L** it also writes `after.txt` /
   `inject.txt` (with the prefix described in "Output file names") so you can see whether it stuck.

**I** and **L** also set
`DDI_BUF_CTL(A).DDI_A_4_LANES` (bit 4, GTTMMADR+0x64000) in the iGPU. On gen < 11
i915 takes the DDI A lane limit from that bit, Apple's firmware sets it only when
it lights the panel from the iGPU, and with the dGPU as boot GPU it stays clear -
the t2linux patch "i915: 4 lane quirk for mbp15,1" works around exactly that.
With **L** the result (register value before/readback) is in `inject.txt`. The write is
skipped if BAR0 is unassigned or the register reads all ones.

If the panel stays dark try `--lanes 2` / `--rate hbr` (the real values are in
the panel DPCD; this tool does not read them). The template VBT is data from
the coreboot project (GPL-2.0).