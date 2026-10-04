# apple_set_os loader + T2 gmux / dGPU / NVRAM

UEFI loader (Boot Services, before Windows).

## Keys during countdown

| Key | mux panel→iGPU | dGPU rail OFF | Other |
|-----|----------------|---------------|-------|
| *(none)*, **Space** or any other key | no | no | plain Windows boot (immediately on a key, otherwise after 6 s): **no AppleSetOs**, no gmux, no NVRAM |
| **R** | no | no | writes NVRAM `gpu-power-prefs` = **iGPU first**, then **restarts** |
| **D** | no | no | **deletes** NVRAM `gpu-power-prefs` (Radeon is the boot GPU again), then **restarts** |
| **I** | yes | yes | full switch to the iGPU: mux + Radeon rail OFF + VBT injection + `DDI_A_4_LANES` + OpRegion/VBT dump before/after |
| **U** | yes | **no** (Radeon stays on) | same as I, Radeon stays powered |
| **B** | no | no | **dump only**: no AppleSetOs, nothing switched or injected; writes the clean OpRegion/VBT/gmux and iGPU register dumps, then boots normally |

AppleSetOs is loaded only for I and U (B and the plain boot skip it) (the iGPU has to become visible for
them). R and D only change NVRAM and restart, without AppleSetOs. They restart
only if the NVRAM operation succeeded; on failure the message stays on screen
and the normal boot continues. D on a machine where the variable does not exist
counts as success (already the default).

Recommended flow for a working Intel panel: **R** (restart, the firmware now
lights the panel from the iGPU), then **I** or **U** on the next boot. Pressing
I/U straight on a Radeon boot switches the mux under a live panel and the Intel
driver has to bring the eDP link up from cold; that is what leaves the panel
black with no backlight.

## What the mux/rail keys do (I, U)

Sequence follows Linux `apple-gmux` (T2 MMIO gmux):

1. interrupt mask (0x14) is saved and set to 0xFF, stale status (0x16) cleared
2. DDC (0x28)=iGPU, panel (0x10)=iGPU, external (0x40)=dGPU (Thunderbolt Macs);
   panel readback (0x10 bit 0) is verified, up to 3 attempts
3. rail OFF (0x50: 1, then 0) **only if step 2 was confirmed** - otherwise it is
   skipped, since cutting the Radeon while the panel is still routed to it blacks
   the screen
4. the POWER bit in 0x16 is polled (like Linux waits for the GPE); if it never
   shows, a fixed 250 ms delay is used
5. status is cleared and the original mask is restored

Not done (cannot be done from Boot Services): ACPI `GMSP(0)` that Linux calls
when clearing MMIO-gmux interrupts, and the eDP link pre-calibration that
`vga_switcheroo` flags as `NEEDS_EDP_CONFIG` for T2 gmux (the iGPU has to train
the panel link itself, hence the VBT injection below).

## WARNING

- After dGPU rail OFF (**I**), do **not** power Radeon back on from Windows.
- Recovery NVRAM: press **D** (deletes `gpu-power-prefs`, Radeon boots first again, and restarts).

## Install / build

1. Secure Boot = No Security  
2. Rename `/EFI/Boot/bootx64.efi` → `bootx64_original.efi`  
3. Copy built `bootx64.efi` to `/EFI/Boot/`  

```bash
docker build -t apple_set_os_loader .
docker run --rm -v "$(pwd):/build" apple_set_os_loader make clean all
```

## Output file names

Every dump file is written to the ESP root as

    t2gmux_<key>_<boot>_<what>

- `<key>` = the key you pressed: `I`, `U` or `B`
- `<boot>` = `igpu` if the Intel iGPU was already visible before AppleSetOs
  (the firmware booted from the iGPU, e.g. after **R** + restart), otherwise `rad`
  (Radeon boot)
- `<what>` = `before.txt`, `before_opregion.bin`, `before_vbt.bin`,
  `after.txt`, `after_opregion.bin`, `after_vbt.bin`, `regs_before.txt`,
  `regs_after.txt`, `inject.txt` (I/U) or `clean.txt`, `clean_opregion.bin`,
  `clean_vbt.bin`, `regs_clean.txt` (B)

Example: `t2gmux_I_rad_after.txt` = key I on a Radeon boot,
`t2gmux_B_igpu_regs_clean.txt` = clean register dump after **R**. Runs with a
different key or boot GPU never overwrite each other (the same combination run
twice does). The countdown screen shows the prefix on its last line
(`files: \t2gmux_I_rad_*`). The input file `t2gmux_vbt.bin` keeps its name.

## OpRegion / VBT dump (keys I, U)

Diagnoses "no eDP link training when the Intel driver loads": the Intel
driver takes DDI port, AUX channel, link rate, lane count and panel power
timings from the VBT in the OpRegion (PCI config 0xFC `ASLS` of the iGPU).

Files written to the ESP root (before = before `apple_set_os`, after = after
apple_set_os + gmux/rail actions; keys I and U write both):

- `before.txt` / `after.txt` – readable report (gmux readback, OpRegion, VBT child devices, eDP block)
- `*_opregion.bin`, `*_vbt.bin` – raw data

All file names follow the scheme in "Output file names" below.

Share the `.txt` and `_vbt.bin` files to analyse them. Parser is checked only
against a synthetic VBT, not yet against real hardware.

## Clean dump (key B)

B changes nothing: no AppleSetOs, no mux, no rail, no injection, no NVRAM. It
writes the same reports as I/U but for the untouched state, with the tag `clean`:

- `clean.txt`, `clean_opregion.bin`, `clean_vbt.bin` (gmux readback, OpRegion, VBT)
- `regs_clean.txt` (iGPU display registers)

Use it after **R** + restart: the firmware has lit the panel from the iGPU, so
this is the reference state to compare against the Radeon-boot `after` files.
On a Radeon boot the iGPU is hidden without AppleSetOs, so the iGPU parts only
report "no Intel iGPU visible" (the gmux readback is still written).

## iGPU register snapshot (keys I, U)

Besides the OpRegion/VBT files, I and U write two register snapshots of the
Intel display engine (BAR0): `regs_before.txt` (before AppleSetOs and the
mux) and `regs_after.txt` (after mux, rail and injection). They hold power
wells, CDCLK/DPLL, DDI A (`DDI_BUF_CTL`, `DP_TP_*`, AUX), the eDP transcoder
(function control, timings, M/N), pipe A/plane 1, the panel power sequencer
(`PP_*`) and the backlight PWM (`BLC_PWM_*`).

Why: with `gpu-power-prefs` = iGPU (key R) the Apple firmware lights the panel
from Intel and the picture works; on a Radeon boot the same I/U leaves the panel
black. Take both runs and diff the `before` files - the registers that differ
(PP_ON_DELAYS, BLC_PWM_*, DP_TP_CTL, DPLL, TRANS_DDI_FUNC_CTL_EDP ...) are what
the firmware programs and the loader does not.

- On a Radeon boot the iGPU is hidden until AppleSetOs, so `before` only says
  "no Intel iGPU visible"; the cold state is in `after`.
- All ones (`0xFFFFFFFF`) means that block was powered down, which is information too.
- Offsets are gen 9/9.5 with the CNP PCH layout (Linux i915 names); not yet
  checked against this hardware.

## VBT injection (keys I, U) - for "no eDP link training" on the iGPU

Apple's T2 firmware leaves the Intel OpRegion VBT mailbox empty (see the dump
above), so the Windows Intel driver does not know an eDP panel sits on DDI A.

1. Get your panel EDID in Windows: `tools/get_edid.ps1` (writes `edid_N.bin`).
2. `python tools/make_vbt.py --edid edid_1.bin --lanes 4 --rate hbr2`
   -> `t2gmux_vbt.bin` (built from a real coreboot Whiskey Lake VBT, see
   `tools/template/`; only the eDP child on DDI A stays enabled).
3. Copy `t2gmux_vbt.bin` to the ESP root.
4. At the countdown press **I** (mux + Radeon OFF + inject) or **U** (same, Radeon stays powered). The loader copies the
   VBT into OpRegion+0x400, re-reads it and writes `after.txt` /
   `inject.txt` (with the prefix described in "Output file names") so you can see whether it stuck.

I and **U** also set
`DDI_BUF_CTL(A).DDI_A_4_LANES` (bit 4, GTTMMADR+0x64000) in the iGPU. On gen < 11
i915 takes the DDI A lane limit from that bit, Apple's firmware sets it only when
it lights the panel from the iGPU, and with the dGPU as boot GPU it stays clear -
the t2linux patch "i915: 4 lane quirk for mbp15,1" works around exactly that.
The result (register value before/readback) is in `inject.txt`. The write is
skipped if BAR0 is unassigned or the register reads all ones.

If the panel stays dark try `--lanes 2` / `--rate hbr` (the real values are in
the panel DPCD; this tool does not read them). The template VBT is data from
the coreboot project (GPL-2.0).