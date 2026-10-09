# GMUX_Control v0.8

A small UEFI loader for the **MacBook Pro 2019 with the T2 chip** (Intel iGPU + AMD Radeon). It starts
before Windows (Boot Camp) and lets you choose which GPU Windows will use: the AMD Radeon as usual, or
the Intel integrated graphics (iGPU).


## ☕ Support

If you find this project useful, consider buying me a coffee!

[![Ko-fi](https://img.shields.io/badge/Support%20me%20on-Ko--fi-ff5e5b?logo=ko-fi&logoColor=white)](https://ko-fi.com/osaul4ik)



## What it does

When the Mac starts, the loader shows a menu with three boot modes and an **Advanced menu** entry. A row of
`*************************` splits them from the Advanced menu; it cannot be selected. You pick one (or it
picks the default after 5 seconds), the loader prepares the GPUs and then starts the normal Windows boot loader.

| Key | Mode | What it does |
|-----|------|--------------|
| **1** | Standart Boot (Radeon only) | Clean boot without AppleSetOs or patches. Windows starts as if the loader was not there, on the Radeon. |
| **4** | Efficient Boot (Intel only) | Windows runs on the Intel iGPU. The built-in screen is switched to the iGPU, the Radeon is powered off, an ACPI patch fixes brightness and sleep. The VBT is built by the loader itself: it asks the panel over the iGPU's own eDP AUX channel (link rate, lanes, PSR, EDID) and injects the result. No VBT file needed. **Default.** |
| **5** | Hybrid Boot (Intel + Radeon) | Same as 4, but the Radeon is **not** switched off. The ACPI patch is a separate, brightness-only file: no sleep fix, sleep is left as the firmware has it. |
| | `*************************` | separator, not selectable |
| **6** | Advanced menu | Does not boot anything. Opens a submenu (see **Advanced menu**). |

Mode 4 needs no hand-made VBT: the iGPU asks the panel what it supports.

## What you need to prepare

| Mode | Files in the root of the EFI partition (ESP) |
|------|-----------------------------------------------|
| 1, 2 | nothing |
| 4 | `SSDT_IGPU.aml` |
| 5 | `SSDT_IGPU_BRT.aml` |

If a file is missing, that mode cannot be started: the menu stays, the timer stops and the status line
says which file is missing.

How to get the files: see **Install** below (Claude makes them from your ACPI dumps). Background:

- `SSDT_IGPU.aml` (mode 4, brightness and sleep; no VBT file is needed, the loader reads the same data from
  the panel itself: DPCD + EDID over AUX-A): [docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md).
- `SSDT_IGPU_BRT.aml` (mode 5, brightness only, no sleep fix):
  [docs/ACPI_PATCH_GUIDE_MODE5.md](docs/ACPI_PATCH_GUIDE_MODE5.md).

## Install

The loader is already built: take the ready `bootx64.efi` (from the project's releases / the build
artifact of the GitHub Actions run). You do not need to build it.

Modes **1** and **2** need nothing else, go straight to step 3 (and skip step 5). Modes **4** and **5** need patch files
made for **your** Mac (`SSDT_IGPU*.aml`). Claude makes them from the data you collect in steps 1-2.

### 1. Collect the ACPI tables: DSDT and all SSDT (Windows)

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

### 2. Send everything to Claude

Attach to one chat with Claude:

- the ACPI tables (`dsdt.dat`, `ssdt*.dat`);
- the **project archive** `t2-gmux-uefi-main.zip` (it contains `tools/`, `docs/ACPI_PATCH_GUIDE.md` and
  `docs/ACPI_PATCH_GUIDE_MODE5.md`, the instructions Claude follows);
- `docs/ACPI_PATCH_GUIDE.md` (mode 4) and `docs/ACPI_PATCH_GUIDE_MODE5.md` (mode 5) again as separate files,
  so they are certainly read.

Check before sending: the archive must contain `tools/SSDT_IGPU.asl`, `tools/SSDT_IGPU_BRT.asl`,
and `tools/make_ssdt_igpu.py`.

Message to Claude (copy):

```
Make SSDT_IGPU.aml for my MacBook Pro (T2) for mode 4.
Follow docs/ACPI_PATCH_GUIDE.md from the project archive step by step.
Attached: DSDT and SSDT dumps (*.dat), the project archive.
Show what you compiled and checked, and which ACPI paths you confirmed in my dumps.
```

For **mode 5** use this message instead:

```
Make SSDT_IGPU_BRT.aml for my MacBook Pro (T2) for mode 5.
Follow docs/ACPI_PATCH_GUIDE_MODE5.md from the project archive step by step.
Attached: DSDT and SSDT dumps (*.dat), the project archive.
Brightness only, no sleep fix. Show what you compiled and checked, and which ACPI paths you confirmed in my dumps.
```

Claude returns the files for the chosen mode. They are built from your dumps and were not run on
hardware, so test them (step 5) and report the result back to Claude if the panel stays dark.

### 3. Open the EFI partition

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

### 4. Put the loader on the EFI partition

1. Set **Secure Boot = No Security** (macOS recovery, Startup Security Utility).
2. In `EFI/Boot/` rename `bootx64.efi` to `bootx64_original.efi`.
3. Copy the ready `bootx64.efi` to `EFI/Boot/`.

### 5. Put the files made by Claude on the EFI partition

The files made by Claude go to the **root** of the EFI partition (`S:\` or `/Volumes/EFI/`), **not** into
`EFI/Boot/`:

| Mode | Copy to the root of the EFI partition |
|------|----------------------------------------|
| 4 | `SSDT_IGPU.aml` |
| 5 | `SSDT_IGPU_BRT.aml` |

Names must match exactly. The result:

```
EFI partition
├── EFI
│   └── Boot
│       ├── bootx64.efi             <- the loader
│       └── bootx64_original.efi    <- the original Windows boot loader
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

- **Up / Down + Enter** (or Space) starts the highlighted mode. The `*************************` separator is
  skipped: the highlight only moves over entries. Pressing **1**, **4** or **5** starts that mode at once,
  **6** opens the Advanced menu.
- If you press nothing for 5 seconds, the **default** mode starts. Any key stops the timer.
- The default mode is marked with an **x**. Press **X** to make the highlighted mode the default (it is
  saved in `t2gmux_default.txt` on the EFI partition; delete the file to go back to mode 4). X only saves,
  it does not start anything. The Advanced menu cannot be the default.
- The bottom of the screen lists the graphics cards the loader sees.

## Advanced menu

Entry **6** opens a submenu instead of booting:

| Entry | What it does |
|-------|--------------|
| Standart Boot + Intel Secondary | Standard boot + the apple_set_os patch (mode 2). Windows sees both GPUs, the Radeon stays primary and the Intel HD is a secondary adapter. |
| Reboot | Restarts the Mac at once (cold reset). |
| Power off | Shuts the Mac down at once. |

Esc returns to the boot menu. If the reset or shutdown call returns, the status line shows the error and the
menu stays.

## What to expect

- After the switch to the Intel iGPU the picture appears **on the Windows login screen**. Nothing is
  shown on the built-in display before that (no boot logo, no loader screen after the switch).
- **Mode 4:** after **Win + Ctrl + Shift + B** (graphics driver restart) the screen goes **black**. Do not
  use that shortcut in mode 4; restart the Mac to get the picture back.
- **Mode 5:** the Radeon stays on, only the panel is moved to the iGPU. This mode is new and has not been
  tested on hardware; until it has, avoid **Win + Ctrl + Shift + B** as in mode 4. Its ACPI patch has no
  sleep fix, so after sleep the panel may stay dark (the firmware re-routes gmux on resume, see
  [docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md), section 5.2): restart instead of sleeping if that happens.

## Warnings

- After mode **4** switches the Radeon off, do **not** power it back on from Windows.
- Do not use `SSDT_IGPU.aml` under the name `SSDT_IGPU_BRT.aml`: it contains the sleep patch that mode 5
  must not have.
- If the screen is dark, restart the Mac and choose mode **1** (hold the key or press 1 at the menu) to
  boot normally on the Radeon.

## Undo

Delete `bootx64.efi` from `/EFI/Boot/` and rename `bootx64_original.efi` back to `bootx64.efi`. The
loader only writes one file, `t2gmux_default.txt`.

## More

- [docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md): how to make `SSDT_IGPU.aml` (mode 4)
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