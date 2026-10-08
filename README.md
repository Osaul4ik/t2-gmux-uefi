# GMUX_Control v0.8

A small UEFI loader for the **MacBook Pro 2019 with the T2 chip** (Intel iGPU + AMD Radeon). It starts
before Windows (Boot Camp) and lets you choose which GPU Windows will use: the AMD Radeon as usual, or
the Intel integrated graphics (iGPU).


## ☕ Support

If you find this project useful, consider buying me a coffee!

[![Ko-fi](https://img.shields.io/badge/Support%20me%20on-Ko--fi-ff5e5b?logo=ko-fi&logoColor=white)](https://ko-fi.com/osaul4ik)



## What it does

When the Mac starts, the loader shows a menu with five boot modes and an **Advanced Menu** entry. Rows of
`*************` split them into groups; they cannot be selected. You pick one (or it picks the default after
5 seconds), the loader prepares the GPUs and then starts the normal Windows boot loader.

| Key | Mode | What it does |
|-----|------|--------------|
| **1** | Standard Boot | Clean boot without AppleSetOs or patches. Windows starts as if the loader was not there. |
| **2** | Boot + Apple_set_os | Standard boot + the apple_set_os patch. Windows sees both GPUs (Radeon and Intel HD). |
| **3** | Integrated gfx (**recommended**) | Windows runs on the Intel iGPU. The built-in screen is switched to the iGPU, the Radeon is powered off, an ACPI patch fixes brightness and sleep. The VBT (panel data) is inside the ACPI patch. |
| **4** | Integrated gfx + built-in VBT | Same as 3, but the VBT is built by the loader itself: it asks the panel over the iGPU's own eDP AUX channel (link rate, lanes, PSR, EDID) and injects the result. No VBT file needed. **Default.** |
| | `*************` | separator, not selectable |
| **5** | Integrated gfx + built-in VBT, Radeon ON | Same as 4, but the Radeon is **not** switched off. The ACPI patch is a separate, brightness-only file: no sleep fix, sleep is left as the firmware has it. |
| | `*************` | separator, not selectable |
| **6** | Advanced Menu | Does not boot anything. Opens a submenu to switch the GPU power preference in NVRAM (see **Advanced Menu**). |

Modes 3 and 4 both move Windows to the Intel iGPU, but they behave differently after a graphics driver
restart (see **What to expect**). **Mode 3 is recommended.** Mode 4 needs no hand-made VBT: the iGPU
asks the panel what it supports.

## What you need to prepare

| Mode | Files in the root of the EFI partition (ESP) |
|------|-----------------------------------------------|
| 1, 2 | nothing |
| 3 | `SSDT_IGPU_VBT.aml` |
| 4 | `SSDT_IGPU.aml` |
| 5 | `SSDT_IGPU_BRT.aml` |

If a file is missing, that mode cannot be started: the menu stays, the timer stops and the status line
says which file is missing.

How to get the files: see **Install** below (Claude makes them from your EDID and ACPI dumps). Background:

- `t2gmux_vbt.bin` (only to build `SSDT_IGPU_VBT.aml` for mode 3): [docs/VBT_GUIDE.md](docs/VBT_GUIDE.md).
  Mode 4 needs no VBT file: the loader reads the same data from the panel itself (DPCD + EDID over AUX-A).
- `SSDT_IGPU.aml` / `SSDT_IGPU_VBT.aml` (brightness, sleep, optional VBT):
  [docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md).
- `SSDT_IGPU_BRT.aml` (mode 5, brightness only, no sleep fix):
  [docs/ACPI_PATCH_GUIDE_MODE5.md](docs/ACPI_PATCH_GUIDE_MODE5.md).

## Install

The loader is already built: take the ready `bootx64.efi` (from the project's releases / the build
artifact of the GitHub Actions run). You do not need to build it.

Modes **1** and **2** need nothing else, go straight to step 4 (and skip step 6). Modes **3**, **4** and **5** need patch files
made for **your** Mac (`SSDT_IGPU*.aml`, `t2gmux_vbt.bin` for mode 3 only). Claude makes them from the data you collect in
steps 1-3.

### 1. Collect the panel EDID (Windows, Boot Camp)

Run in PowerShell:

```
powershell -ExecutionPolicy Bypass -File .\get_edid.ps1
```

It writes `edid_1.bin`, `edid_2.bin`, ... Keep all of them: Claude picks the internal panel (manufacturer
`APP`). Details: [docs/VBT_GUIDE.md](docs/VBT_GUIDE.md).

### 2. Collect the ACPI tables: DSDT and all SSDT (Windows)

Windows has no built-in tool for this. Download the ACPICA Windows binary tools from Intel:
[ACPI Component Architecture Downloads (Windows Binary Tools)](https://www.intel.com/content/www/us/en/download/774881/acpi-component-architecture-downloads-windows-binary-tools.html)
(zip, e.g. `iasl-win-20260408.zip`), unpack it and run in PowerShell or cmd, in an empty folder:

```
acpidump.exe -b
```

This writes every table as a binary file: `dsdt.dat`, `ssdt1.dat`, `ssdt2.dat`, ... Take **all** of them.
Do not disassemble or edit.

The download page describes the ASL compiler / disassembler (`iasl`); check that `acpidump.exe` is really
in the zip. If it is not, take the dump on Linux (a live USB is enough): the files are in
`/sys/firmware/acpi/tables/` (`DSDT`, `SSDT*`, copy as root), see
[docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md), section 4.

### 3. Send everything to Claude

Attach to one chat with Claude:

- the EDID files (`edid_*.bin`);
- the ACPI tables (`dsdt.dat`, `ssdt*.dat`);
- the **project archive** `t2-gmux-uefi-main.zip` (it contains `tools/`, `docs/ACPI_PATCH_GUIDE.md`,
  `docs/ACPI_PATCH_GUIDE_MODE5.md` and `docs/VBT_GUIDE.md`, the instructions Claude follows);
- `docs/ACPI_PATCH_GUIDE.md` (modes 3 / 4), `docs/ACPI_PATCH_GUIDE_MODE5.md` (mode 5) and
  `docs/VBT_GUIDE.md` again as separate files, so they are certainly read.

Check before sending: the archive must contain `tools/SSDT_IGPU.asl`, `tools/SSDT_IGPU_BRT.asl`,
`tools/make_ssdt_igpu.py`, `tools/make_vbt.py` and a **non-empty**
`tools/template/coreboot_google_sarien_data.vbt` (the last two are only needed for modes 3 / 4).

Message to Claude (copy, change the mode):

```
Make the patch files for my MacBook Pro (T2) for mode 4 (or 3, or both).
Follow docs/ACPI_PATCH_GUIDE.md and docs/VBT_GUIDE.md from the project archive step by step.
Attached: panel EDID (edid_*.bin), DSDT and SSDT dumps (*.dat), the project archive.
Give me the ready files: SSDT_IGPU.aml (mode 4) and/or SSDT_IGPU_VBT.aml (mode 3, built with t2gmux_vbt.bin).
Show what you compiled and checked, and which ACPI paths you confirmed in my dumps.
```

For **mode 5** use this message instead (it needs no EDID and no VBT):

```
Make SSDT_IGPU_BRT.aml for my MacBook Pro (T2) for mode 5.
Follow docs/ACPI_PATCH_GUIDE_MODE5.md from the project archive step by step.
Attached: DSDT and SSDT dumps (*.dat), the project archive.
Brightness only, no sleep fix. Show what you compiled and checked, and which ACPI paths you confirmed in my dumps.
```

Claude returns the files for the chosen mode. They are built from your dumps and were not run on
hardware, so test them (step 6) and report the result back to Claude if the panel stays dark.

### 4. Open the EFI partition

All files go to the **EFI partition** (ESP) of the Mac's internal disk, the one Windows boots from.

- **From Windows** (PowerShell or cmd as Administrator):
  ```
  mountvol S: /S
  ```
  The EFI partition is now drive `S:`. When you are done: `mountvol S: /D`.
- **From macOS** (Terminal):
  ```
  diskutil list                      # find the partition of type EFI on the internal disk, usually disk0s1
  sudo diskutil mount disk0s1        # it appears as /Volumes/EFI
  ```

### 5. Put the loader on the EFI partition

1. Set **Secure Boot = No Security** (macOS recovery, Startup Security Utility).
2. In `EFI/Boot/` rename `bootx64.efi` to `bootx64_original.efi`.
3. Copy the ready `bootx64.efi` to `EFI/Boot/`.

### 6. Put the files made by Claude on the EFI partition

The files made by Claude go to the **root** of the EFI partition (`S:\` or `/Volumes/EFI/`), **not** into
`EFI/Boot/`:

| Mode | Copy to the root of the EFI partition |
|------|----------------------------------------|
| 3 | `SSDT_IGPU_VBT.aml` |
| 4 | `SSDT_IGPU.aml` |
| 5 | `SSDT_IGPU_BRT.aml` |

Names must match exactly. The result:

```
EFI partition
├── EFI
│   └── Boot
│       ├── bootx64.efi             <- the loader
│       └── bootx64_original.efi    <- the original Windows boot loader
├── SSDT_IGPU_VBT.aml               <- mode 3
├── SSDT_IGPU.aml                   <- mode 4
├── SSDT_IGPU_BRT.aml               <- mode 5
└── t2gmux_default.txt              <- created by the loader (key X)
```

Only the files of the mode you use are needed. Restart and pick the mode in the loader menu. A mode with
missing files refuses to start and shows which file is missing. To replace a file later (a new build from
Claude), just overwrite it.

Building the loader yourself is optional (needs Docker):

```
docker build -t apple_set_os_loader .
docker run --rm -v "$(pwd):/build" apple_set_os_loader make clean all
```

## Drivers: 
Intel driver: https://www.intel.com/content/www/us/en/download/776137/intel-7th-10th-gen-processor-graphics-windows.html

Intel control panel: http://www.microsoft.com/store/apps/9PLFNLNT3G5G

## Use

- **Up / Down + Enter** (or Space) starts the highlighted mode. The `*************` separators are skipped:
  the highlight only moves over entries. Pressing **1**-**5** starts that mode at once, **6** opens the
  Advanced Menu.
- If you press nothing for 5 seconds, the **default** mode starts. Any key stops the timer.
- The default mode is marked with an **x**. Press **X** to make the highlighted mode the default (it is
  saved in `t2gmux_default.txt` on the EFI partition; delete the file to go back to mode 4). X only saves,
  it does not start anything. The Advanced Menu cannot be the default.
- The bottom of the screen lists the graphics cards the loader sees.

## Advanced Menu

Entry **6** opens a submenu instead of booting. It edits the Apple NVRAM variable `gpu-power-prefs`
(vendor GUID `fa4ce28d-b62f-4c99-9cc3-6815686e30f9`), the one `nvram` writes in macOS.

| Entry | What it does |
|-------|--------------|
| Switch to iGPU | Writes `01 00 00 00`. With that value the firmware uses the Intel iGPU at the next boot. |
| Switch to dGPU | Deletes the variable, back to the firmware default (the Radeon). |
| Back | Returns to the boot menu (Esc does the same). |

Only the entry that would change something is shown: if the variable exists only **Switch to dGPU**
is listed, if it does not exist only **Switch to iGPU**. If the variable cannot be read at all, both are
listed. A line under the entries shows the current value. After a switch the status line shows the result
(checked by reading the variable back). The firmware reads the variable at boot, so the change needs a
**restart**.

The write is done from the UEFI loader and has not been tested on hardware. To undo it, use
**Switch to dGPU**, or reset the NVRAM of the Mac (Cmd + Option + P + R while powering on).

## What to expect

- After the switch to the Intel iGPU the picture appears **on the Windows login screen**. Nothing is
  shown on the built-in display before that (no boot logo, no loader screen after the switch).
- **Mode 3 (recommended, full patch):** the screen survives a restart of the graphics driver, for example
  **Win + Ctrl + Shift + B**.
- **Mode 4:** after **Win + Ctrl + Shift + B** (graphics driver restart) the screen goes **black**. Do not
  use that shortcut in mode 4; restart the Mac to get the picture back.
- **Mode 5:** the Radeon stays on, only the panel is moved to the iGPU. This mode is new and has not been
  tested on hardware; until it has, avoid **Win + Ctrl + Shift + B** as in mode 4. Its ACPI patch has no
  sleep fix, so after sleep the panel may stay dark (the firmware re-routes gmux on resume, see
  [docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md), section 5.2): restart instead of sleeping if that happens.

## Warnings

- After modes **3** / **4** switch the Radeon off, do **not** power it back on from Windows.
- Do not use `SSDT_IGPU_VBT.aml` under the name `SSDT_IGPU.aml` (or the other way round). In mode 4 it
  would overwrite the VBT the loader injects.
- Do not use `SSDT_IGPU.aml` under the name `SSDT_IGPU_BRT.aml`: it contains the sleep patch that mode 5
  must not have.
- If the screen is dark, restart the Mac and choose mode **1** (hold the key or press 1 at the menu) to
  boot normally on the Radeon.

## Undo

Delete `bootx64.efi` from `/EFI/Boot/` and rename `bootx64_original.efi` back to `bootx64.efi`. The
loader only writes one file, `t2gmux_default.txt`.

## More

- [docs/VBT_GUIDE.md](docs/VBT_GUIDE.md): how to make `t2gmux_vbt.bin`
- [docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md): how to make the SSDT files of modes 3 and 4
- [docs/ACPI_PATCH_GUIDE_MODE5.md](docs/ACPI_PATCH_GUIDE_MODE5.md): how to make `SSDT_IGPU_BRT.aml` (mode 5)
- [docs/TECHNICAL.md](docs/TECHNICAL.md): what the loader does internally (gmux switching, ACPI patch, EDID, lanes)

## Credits

Author: [Osaul4ik](https://github.com/Osaul4ik).

This project was possible thanks to
[aa15032261/apple_set_os-loader](https://github.com/aa15032261/apple_set_os-loader), a tiny EFI loader
that sets up `apple_set_os` and chain-loads the original boot loader. It is the base this loader grew
from. That project is in turn based on
[0xbb/apple_set_os.efi](https://github.com/0xbb/apple_set_os.efi), which credits Andreas Heider for
discovering the `apple_set_os` trick.

## License

Copyright (c) 2026 Osaul4ik. The code and documents of this project are released under the
[MIT License](LICENSE).

Third-party parts keep their own terms:

- `tools/template/coreboot_google_sarien_data.vbt` is data from the coreboot project (GPL-2.0).
- `pci_db/pci.ids` is the PCI ID database and comes under its own license (see the header of that file).
- Code taken from the projects named in Credits stays under the terms of those projects.