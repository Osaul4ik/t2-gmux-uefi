# apple_set_os loader + T2 gmux / dGPU / NVRAM

UEFI loader (Boot Services, before Windows).

## Keys during countdown

| Key | mux panel→iGPU | dGPU rail OFF | Other |
|-----|----------------|---------------|-------|
| *(none)*, **Space** or any other key | no | no | plain Windows boot (immediately on a key, otherwise after 6 s): **no AppleSetOs**, no gmux, no NVRAM |
| **R** | no | no | writes NVRAM `gpu-power-prefs` = **dGPU only**, then **restarts** |
| **D** | yes | yes | OpRegion/VBT dump before/after |
| **I** | yes | yes | D + VBT injection + `DDI_A_4_LANES` |
| **U** | yes | **no** (Radeon stays on) | like I |

AppleSetOs is loaded only for D, I and U (the iGPU has to become visible for
them). R only writes NVRAM and restarts, without AppleSetOs. R restarts only if
the NVRAM write succeeded; on failure the message stays on screen and the normal
boot continues.

## What the mux/rail keys do (D, I, U)

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

- After dGPU rail OFF (**D** or **I**), do **not** power Radeon back on from Windows.
- Recovery NVRAM: press **R** (writes dGPU-only and restarts).

## Install / build

1. Secure Boot = No Security  
2. Rename `/EFI/Boot/bootx64.efi` → `bootx64_original.efi`  
3. Copy built `bootx64.efi` to `/EFI/Boot/`  

```bash
docker build -t apple_set_os_loader .
docker run --rm -v "$(pwd):/build" apple_set_os_loader make clean all
```

## OpRegion / VBT dump (keys D, I, U)

Diagnoses "no eDP link training when the Intel driver loads": the Intel
driver takes DDI port, AUX channel, link rate, lane count and panel power
timings from the VBT in the OpRegion (PCI config 0xFC `ASLS` of the iGPU).

Files written to the ESP root (before = before `apple_set_os`, after = after
apple_set_os + gmux/rail actions; keys D, I and U all write both):

- `t2gmux_before.txt` / `t2gmux_after.txt` – readable report (gmux readback, OpRegion, VBT child devices, eDP block)
- `t2gmux_*_opregion.bin`, `t2gmux_*_vbt.bin` – raw data

Share the `.txt` and `_vbt.bin` files to analyse them. Parser is checked only
against a synthetic VBT, not yet against real hardware.

## VBT injection (keys I, U) - for "no eDP link training" on the iGPU

Apple's T2 firmware leaves the Intel OpRegion VBT mailbox empty (see the dump
above), so the Windows Intel driver does not know an eDP panel sits on DDI A.

1. Get your panel EDID in Windows: `tools/get_edid.ps1` (writes `edid_N.bin`).
2. `python tools/make_vbt.py --edid edid_1.bin --lanes 4 --rate hbr2`
   -> `t2gmux_vbt.bin` (built from a real coreboot Whiskey Lake VBT, see
   `tools/template/`; only the eDP child on DDI A stays enabled).
3. Copy `t2gmux_vbt.bin` to the ESP root.
4. At the countdown press **I** (= D + inject) or **U** (same, Radeon stays powered). The loader copies the
   VBT into OpRegion+0x400, re-reads it and writes `t2gmux_after.txt` /
   `t2gmux_inject.txt` so you can see whether it stuck.

I and **U** also set
`DDI_BUF_CTL(A).DDI_A_4_LANES` (bit 4, GTTMMADR+0x64000) in the iGPU. On gen < 11
i915 takes the DDI A lane limit from that bit, Apple's firmware sets it only when
it lights the panel from the iGPU, and with the dGPU as boot GPU it stays clear -
the t2linux patch "i915: 4 lane quirk for mbp15,1" works around exactly that.
The result (register value before/readback) is in `t2gmux_inject.txt`. The write is
skipped if BAR0 is unassigned or the register reads all ones.

If the panel stays dark try `--lanes 2` / `--rate hbr` (the real values are in
the panel DPCD; this tool does not read them). The template VBT is data from
the coreboot project (GPL-2.0).