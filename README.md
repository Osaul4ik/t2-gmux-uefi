# GMUX_Control v0.91

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
| **1** | Standart Boot (Radeon only) | Clean boot without AppleSetOs or patches. Windows starts as if the loader was not there, on the Radeon. **Available only while `gpu-power-prefs` (NVRAM) is not set to the iGPU value**; with the iGPU preference it is shown as `unavailable: Switch to dGPU` and cannot be started (see **Advanced menu**). |
| **4** | Efficient Boot (Intel only) | Windows runs on the Intel iGPU. The built-in screen is switched to the iGPU, the **Radeon is powered off**, an ACPI patch fixes brightness and sleep (it also powers the Radeon off again after resume if it was off before sleep). The VBT is built by the loader itself, on the fly: it asks the panel over the iGPU's own eDP AUX channel (link rate, lanes, PSR, EDID) and injects the result. No VBT file needed. **Default.** |
| **5** | Hybrid Boot (Intel + Radeon) | Same as 4 (the VBT is built on the fly in the same way), but the Radeon is **not** switched off and stays available as a secondary adapter. The ACPI patch is a separate file: brightness plus a gmux re-route on resume (sleep), no Radeon rail code. **Available only while `gpu-power-prefs` (NVRAM) is set to the iGPU value**; otherwise it is shown as `unavailable: Switch to iGPU` and cannot be started (see **Advanced menu**). |
| | `*************************` | separator, not selectable |
| **6** | Advanced menu | Does not boot anything. Opens a submenu (see **Advanced menu**). |

Modes 4 and 5 need no hand-made VBT: the loader builds it on the fly and the iGPU asks the panel what it supports.

## Power consumption

Approximate figures, they depend on the Windows power settings (power mode / plan).

| Mode | Idle | Work |
|------|------|------|
| **1** AMD default (iGPU hidden) | 12 W | 26 W |
| **4** Efficient (Intel only, AMD off) | 8.3 W | 11.6 W |
| **5** Hybrid (iGPU + AMD secondary) | 10.1 W | 16.8 W |

## What you need to prepare

| Mode | Files in the root of the EFI partition (ESP) |
|------|-----------------------------------------------|
| 1, 2 | nothing |
| 4 | `SSDT_IGPU.aml` |
| 5 | `SSDT_IGPU_BRT.aml` |

If a file is missing, that mode cannot be started: the menu stays, the timer stops and the status line
says which file is missing.

How to get the files: see **Install** below (Claude makes them from your ACPI dumps). Background:

- `SSDT_IGPU.aml` (mode 4, brightness and sleep with Radeon power-off; no VBT file is needed, the loader reads
  the same data from the panel itself: DPCD + EDID over AUX-A): [docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md).
- `SSDT_IGPU_BRT.aml` (mode 5, brightness and resume re-route, no Radeon rail code; the VBT is built on the fly
  here too):
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
- the **project archive** of this repository (GitHub: Code > Download ZIP; it contains `tools/`, `docs/ACPI_PATCH_GUIDE.md` and
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
Brightness and resume re-route, no sleep code for the Radeon rail. Show what you compiled and checked, and which ACPI paths you confirmed in my dumps.
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
  skipped: the highlight only moves over entries. Pressing **1**, **4** or **5** starts that mode at once
  (**1** only while the preference is not iGPU, **5** only while it is iGPU, see **Advanced menu**), **6** opens the
  Advanced menu.
- If you press nothing for 5 seconds, the **default** mode starts. Any key stops the timer.
- The default mode is marked with an **x**. Press **X** to make the highlighted mode the default (it is
  saved in `t2gmux_default.txt` on the EFI partition; delete the file to go back to mode 4). X only saves,
  it does not start anything. The Advanced menu cannot be the default.
- The bottom of the screen lists the graphics cards the loader sees.

## Advanced menu

Entry **6** opens a submenu instead of booting. It edits the Apple NVRAM variable `gpu-power-prefs`
(vendor GUID `fa4ce28d-b62f-4c99-9cc3-6815686e30f9`), the one `nvram` writes in macOS, and holds the other
actions:

| Entry | What it does |
|-------|--------------|
| Switch to dGPU (delete gpu-power-prefs) | Deletes the variable, back to the firmware default (the Radeon). The Mac then **reboots by itself**. |
| Switch to iGPU (set gpu-power-prefs) | Writes `01 00 00 00`. With that value the firmware uses the Intel iGPU at the next boot. The Mac then **reboots by itself**. |
| `*************************` | separator, not selectable |
| Standart Boot + Intel Secondary | Standard boot + the apple_set_os patch (mode 2). Windows sees both GPUs, the Radeon stays primary and the Intel HD is a secondary adapter. |
| Reboot | Restarts the Mac at once (cold reset). |
| Power off | Shuts the Mac down at once. |
| Back | Returns to the boot menu (Esc does the same). |

The loader reads `gpu-power-prefs` at start and after leaving the Advanced menu, and only the entry that
would change something is shown:

- preference = **iGPU** (variable present, first byte `01`): only **Switch to dGPU** is listed,
  **Hybrid Boot** (mode 5) is available, and **Standart Boot** (mode 1) is shown as
  `unavailable: Switch to dGPU`, the highlight skips it, key **1** only shows a message and the auto-boot
  default cannot start it;
- preference = **dGPU** (variable absent or another value): only **Switch to iGPU** is listed,
  **Standart Boot** is available, and **Hybrid Boot** is shown as `unavailable: Switch to iGPU`, the
  highlight skips it, key **5** only shows a message and the auto-boot default cannot start it;
- variable cannot be read at all: both switch entries are listed, **Standart Boot** is available and
  **Hybrid Boot** stays unavailable.

A line under the entries shows the current value. After a switch the status line shows the result (checked
by reading the variable back). The firmware reads the variable at boot, so after a successful switch the
loader shows the result for about a second and a half and reboots the Mac; the menu that follows has
**Hybrid Boot** available after Switch to iGPU, and **Standart Boot** available after Switch to dGPU. If the write fails there is **no** reboot and the menu stays. If the reset itself
fails, the status line says so.

The write is done from the UEFI loader and has not been tested on hardware. To undo it, use
**Switch to dGPU**, or reset the NVRAM of the Mac (Cmd + Option + P + R while powering on).

## What to expect

- After the switch to the Intel iGPU the picture appears **on the Windows login screen**. Nothing is
  shown on the built-in display before that (no boot logo, no loader screen after the switch).
- **Mode 4:** after **Win + Ctrl + Shift + B** (graphics driver restart) the screen goes **black**. Do not
  use that shortcut in mode 4; restart the Mac to get the picture back.
- **Mode 5:** the Radeon stays on, only the panel is moved to the iGPU. Avoid **Win + Ctrl + Shift + B** as in
  mode 4. Its ACPI patch re-routes
  gmux to the iGPU on resume (the firmware puts the panel back on the Radeon, see
  [docs/ACPI_PATCH_GUIDE_MODE5.md](docs/ACPI_PATCH_GUIDE_MODE5.md), section 5.2). That re-route is untested: if
  the Mac hangs or powers off on resume, restart instead of sleeping and report it.

## Warnings

- After mode **4** switches the Radeon off, do **not** power it back on from Windows.
- Do not use `SSDT_IGPU.aml` under the name `SSDT_IGPU_BRT.aml`: it contains the Radeon rail-off check that mode 5
  must not have.
- If the screen is dark, restart the Mac and choose mode **1** (press 1 at the menu) to boot normally on the
  Radeon. Mode 1 is unavailable while `gpu-power-prefs` is set to the iGPU: then open the **Advanced menu**, use
  **Switch to dGPU** (the Mac reboots) and choose mode 1 in the menu that follows.

## Undo

If the preference is set to the iGPU, first use **Switch to dGPU** in the Advanced menu (or reset the NVRAM), so
the firmware goes back to the Radeon. Then delete `bootx64.efi` from `/EFI/Boot/` and rename
`bootx64_original.efi` back to `bootx64.efi`. Besides `gpu-power-prefs` (Advanced menu only), the loader writes
one file, `t2gmux_default.txt`.

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