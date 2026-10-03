# apple_set_os loader + T2 gmux / dGPU / NVRAM

UEFI loader (Boot Services, before Windows).

## Keys during countdown

| Key | mux panel→iGPU | dGPU rail OFF | NVRAM |
|-----|----------------|---------------|-------|
| *(default)* | no | no | — |
| **Z** | — | — | skip AppleSetOs |
| **X** | yes | yes | — |
| **V** | yes | no | — |
| **C** | no | **yes** | — |
| **R** | no | no | **dGPU only** |
| **E** | no | no | **iGPU only** |
| **D** | yes | yes | — (same as X **+ OpRegion/VBT dump**) |
| **W** | no | no | — (**OpRegion/VBT dump only**) |

## WARNING

- After dGPU rail OFF (**X** or **C**), do **not** power Radeon back on from Windows.
- **C** (rail off without mux switch) will black the panel if firmware still routes display through dGPU — use only if you know why.
- Recovery NVRAM: press **R**, reboot.

## Install / build

1. Secure Boot = No Security  
2. Rename `/EFI/Boot/bootx64.efi` → `bootx64_original.efi`  
3. Copy built `bootx64.efi` to `/EFI/Boot/`  

```bash
docker build -t apple_set_os_loader .
docker run --rm -v "$(pwd):/build" apple_set_os_loader make clean all
```

## OpRegion / VBT dump (keys D, W)

Diagnoses "no eDP link training when the Intel driver loads": the Intel
driver takes DDI port, AUX channel, link rate, lane count and panel power
timings from the VBT in the OpRegion (PCI config 0xFC `ASLS` of the iGPU).

Files written to the ESP root (before = before `apple_set_os`, after = after
apple_set_os + gmux/rail actions):

- `t2gmux_before.txt` / `t2gmux_after.txt` – readable report (gmux readback, OpRegion, VBT child devices, eDP block)
- `t2gmux_*_opregion.bin`, `t2gmux_*_vbt.bin` – raw data

Share the `.txt` and `_vbt.bin` files to analyse them. Parser is checked only
against a synthetic VBT, not yet against real hardware.

## VBT injection (key I) - for "no eDP link training" on the iGPU

Apple's T2 firmware leaves the Intel OpRegion VBT mailbox empty (see the dump
above), so the Windows Intel driver does not know an eDP panel sits on DDI A.

1. Get your panel EDID in Windows: `tools/get_edid.ps1` (writes `edid_N.bin`).
2. `python tools/make_vbt.py --edid edid_1.bin --lanes 4 --rate hbr2`
   -> `t2gmux_vbt.bin` (built from a real coreboot Whiskey Lake VBT, see
   `tools/template/`; only the eDP child on DDI A stays enabled).
3. Copy `t2gmux_vbt.bin` to the ESP root.
4. At the countdown press **I** (= X + inject + dump). The loader copies the
   VBT into OpRegion+0x400, re-reads it and writes `t2gmux_after.txt` /
   `t2gmux_inject.txt` so you can see whether it stuck.

If the panel stays dark try `--lanes 2` / `--rate hbr` (the real values are in
the panel DPCD; this tool does not read them). The template VBT is data from
the coreboot project (GPL-2.0).
