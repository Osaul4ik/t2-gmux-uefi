# GMUX_Control v0.92

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
| any (optional) | `FakeSecureBoot.efi`: only if you want the **FakeSecureBoot** switch, see **Advanced menu** |

If a file is missing, that mode cannot be started: the menu stays, the timer stops and the status line
says which file is missing.

How to get the files: see **Install** below (make them yourself, or let Claude make them from your ACPI dumps).
Background:

- `SSDT_IGPU.aml` (mode 4, brightness and sleep with Radeon power-off; no VBT file is needed, the loader reads
  the same data from the panel itself: DPCD + EDID over AUX-A): [docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md).
- `SSDT_IGPU_BRT.aml` (mode 5, brightness and resume re-route, no Radeon rail code; the VBT is built on the fly
  here too):
  [docs/ACPI_PATCH_GUIDE_MODE5.md](docs/ACPI_PATCH_GUIDE_MODE5.md).

## Install

The loader is already built: take the ready `bootx64.efi` (from the project's releases / the build
artifact of the GitHub Actions run). You do not need to build it.

Modes **1** and **2** need nothing else, go straight to step 3 (and skip step 5). Modes **4** and **5** need patch files
made for **your** Mac (`SSDT_IGPU*.aml`). You can make them yourself or let Claude make them from the ACPI dumps
you collect in step 1 (step 2).

### 1. Collect the ACPI tables: DSDT and all SSDT (Windows)

Windows has no built-in tool for this. Download the ACPICA Windows binary tools from Intel:
[ACPI Component Architecture Downloads (Windows Binary Tools)](https://www.intel.com/content/www/us/en/download/774881/acpi-component-architecture-downloads-windows-binary-tools.html)
(zip, e.g. `iasl-win-20260408.zip`), unpack it and run in PowerShell or cmd, in an empty folder:

```
acpidump.exe -b
```

This writes every table as a binary file: `dsdt.dat`, `ssdt1.dat`, `ssdt2.dat`, ... Take **all** of them.
Keep the originals untouched (if you patch yourself in 2.1, disassemble copies).

The download page describes the ASL compiler / disassembler (`iasl`); check that `acpidump.exe` is really
in the zip. If it is not, take the dump on Linux (a live USB is enough): the files are in
`/sys/firmware/acpi/tables/` (`DSDT`, `SSDT*`, copy as root), see
[docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md), section 4.

### 2. Make the patch files (modes 4 and 5)

Choose **one** way. Both start from the dumps of step 1 and give you the same result: `SSDT_IGPU.aml` (mode 4)
and/or `SSDT_IGPU_BRT.aml` (mode 5). The files are built from your dumps and were not run on your hardware: test them in step 5.

#### 2.1 Patch it yourself

The repository has the full procedure and a working reference for the MacBook Pro 2019:
[docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md) (mode 4) and
[docs/ACPI_PATCH_GUIDE_MODE5.md](docs/ACPI_PATCH_GUIDE_MODE5.md) (mode 5). They are written for an AI agent, but they
are plain step-by-step instructions you can follow by hand. The sources are `tools/SSDT_IGPU.asl` (mode 4) and
`tools/SSDT_IGPU_BRT.asl` (mode 5). Any general guide on writing an SSDT and compiling it with `iasl` helps with
the basics; what is different here is that the **loader** does the renames, not the AML and not a config file.

1. Disassemble **copies** of your dumps with `iasl -d` (the `iasl` tool is part of ACPICA, see step 1).
2. In the `.dsl` files confirm every path of the reference `.asl` on **your** Mac: exactly one `_BCM` in the table
   `SaSsdt`, the Radeon device with its `ABCM` and `MBWR` methods, exactly one `_WAK` in the DSDT (mode 4 also
   needs `GVEN`, `CSTS` and the gmux window address). Where your dump differs, change the `.asl`.
3. Rules that must stay true:
   - the SSDT defines the **new** `_BCM` (and `_WAK`, mode 4 also `_PTS`) and calls the renamed originals
     `XBCM`, `XWAK`, `XPTS`; the loader renames the originals, never write the renames yourself;
   - mode 5 must contain **no** `_PTS` / `XPTS` and no gmux window; mode 4 contains them;
   - no `_INI` and no VBT in either file: the loader injects the VBT from UEFI;
   - the OEM table ids differ (`IGPUBCM` / `IGPUBRT`), and the files are not interchangeable.
4. Copy the `.asl` out of `tools/` into a working folder and compile it there (never write output into `tools/`):
   ```
   iasl SSDT_IGPU.asl             # gives SSDT_IGPU.aml      (mode 4)
   iasl SSDT_IGPU_BRT.asl         # gives SSDT_IGPU_BRT.aml  (mode 5)
   ```
   (or, from the repository, `python tools/make_ssdt_igpu.py [--asl tools/SSDT_IGPU_BRT.asl] -o <dir>/<name>.aml`). Check the result as
   in section 7 of the guide of your mode (0 errors, only the expected names inside the AML).

#### 2.2 Send everything to Claude

Let Claude do the steps of 2.1 for you. Attach to one chat with Claude:

- the ACPI tables (`dsdt.dat`, `ssdt*.dat`);
- the **project archive** of this repository (GitHub: Code > Download ZIP; it contains `tools/`, `docs/ACPI_PATCH_GUIDE.md` and
  `docs/ACPI_PATCH_GUIDE_MODE5.md`, the instructions Claude follows);
- `docs/ACPI_PATCH_GUIDE.md` (mode 4) and `docs/ACPI_PATCH_GUIDE_MODE5.md` (mode 5) again as separate files,
  so they are certainly read.

Check before sending: the archive must contain `tools/SSDT_IGPU.asl`, `tools/SSDT_IGPU_BRT.asl`,
and `tools/make_ssdt_igpu.py`.

Message to Claude (copy):

```
Make both ACPI patches for my MacBook Pro (T2):
1. SSDT_IGPU.aml (mode 4, Efficient Boot): follow docs/ACPI_PATCH_GUIDE.md.
2. SSDT_IGPU_BRT.aml (mode 5, Hybrid Boot: brightness and resume re-route, no sleep code for the Radeon rail):
   follow docs/ACPI_PATCH_GUIDE_MODE5.md.
Use the project archive and follow each guide step by step, one guide per file; do not mix the two files.
Attached: DSDT and SSDT dumps (*.dat), the project archive.
For each file show what you compiled and checked, and which ACPI paths you confirmed in my dumps.
```

If you need only one mode, delete the other numbered line from the message.

Claude returns the files you asked for. They are built from your dumps and were not run on
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

### 5. Put the patch files on the EFI partition

The patch files go to the **root** of the EFI partition (`S:\` or `/Volumes/EFI/`), **not** into
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
├── FakeSecureBoot.efi              <- optional, adds the FakeSecureBoot switch (Advanced menu)
└── t2gmux_default.txt              <- created by the loader (key X, FakeSecureBoot / Resizable BAR switches)
```

Only the files of the mode you use are needed. Restart and pick the mode in the loader menu. A mode with
missing files refuses to start and shows which file is missing. To replace a file later (a new build),
just overwrite it.

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
  it does not start anything. The Advanced menu cannot be the default. The same file also holds the
  FakeSecureBoot setting (line `FSB=1` / `FSB=0`) and the Resizable BAR setting (line `REBAR=1` / `REBAR=0`);
  X keeps both.
- The bottom of the screen lists the graphics cards the loader sees.

## Advanced menu

Entry **6** opens a submenu instead of booting. It edits the Apple NVRAM variable `gpu-power-prefs`
(vendor GUID `fa4ce28d-b62f-4c99-9cc3-6815686e30f9`), the one `nvram` writes in macOS, and holds the other
actions:

| Entry | What it does |
|-------|--------------|
| FakeSecureBoot: True / False | **Only listed if `FakeSecureBoot.efi` is in the root of the EFI partition.** Enter (or Space) flips it and saves it at once to `t2gmux_default.txt`. With **True** the loader starts `FakeSecureBoot.efi` right before Windows, in any boot mode (see below). A separator follows it. |
| Resizable BAR: On / Off | Always listed, right under FakeSecureBoot. Enter (or Space) flips it and saves it at once to `t2gmux_default.txt` (`REBAR=1`; no such line means **Off**). With **On** the loader enlarges BAR0 of the Radeon right before Windows starts (see below). A separator follows the two switches. |
| Switch to dGPU (delete gpu-power-prefs) | Deletes the variable, back to the firmware default (the Radeon). The Mac then **reboots by itself**. |
| Switch to iGPU (set gpu-power-prefs) | Writes `01 00 00 00`. With that value the firmware uses the Intel iGPU at the next boot. The Mac then **reboots by itself**. |
| `*************************` | separator, not selectable |
| Standart Boot + Intel Secondary | Standard boot + the apple_set_os patch (mode 2). Windows sees both GPUs, the Radeon stays primary and the Intel HD is a secondary adapter. |
| Reboot | Restarts the Mac at once (cold reset). |
| Power off | Shuts the Mac down at once. |
| Back | Returns to the boot menu (Esc does the same). |

### FakeSecureBoot (optional)

[FakeSecureBoot](https://github.com/Shmurkio/FakeSecureBoot) is a small UEFI driver that makes the firmware
answer "Secure Boot is on" when the `SecureBoot` variable is read. Build `FakeSecureBoot.efi` from that
project (EDK2) **as it is** and put it in the root of the EFI partition; the switch then appears at the top of
the Advanced menu. The choice is kept in `t2gmux_default.txt` (second line, `FSB=1`; an older file without that
line means off) and applies to every boot mode, including **1**. On the progress screen it is the line
`FakeSecureBoot: ...` just above `Booting bootx64_original.efi...`: `off`,
`OK (SecureBoot reads 1, unhooked at exit)`, `not found, skipped` (the setting is True but the file is gone)
or `FAILED`. A failure never stops the boot, Windows simply starts without the fake. The loader removes the
driver's hook again right before Windows takes over (otherwise the driver crashes Windows, see
`docs/TECHNICAL.md`), so the fake is seen by bootmgr / winload only. It only changes what the firmware
reports; it does not turn real Secure Boot on.

Because the Windows boot loader sees Secure Boot as enabled, Windows ignores **Test Mode** (`testsigning`) while the
switch is **True**, and drivers that are only test-signed do not load. Set the switch to **False** to use them.

### Resizable BAR

The Mac firmware has no Resizable BAR setting: it leaves BAR0 of the Radeon (the VRAM window) at 256 MB, and
Windows keeps what the firmware set up (GPU-Z: `Resizable BAR enabled in BIOS: No`). With the switch **On** the
loader does what ReBarUEFI does during PCI enumeration, only afterwards, right before it starts Windows:

1. finds the Radeon and the bridges above it, reads its Resizable BAR capability (BAR index 0),
2. looks for a free slot, aligned to its size, in the MMIO window above 4 GB that the root bridge reports,
   taking every BAR and bridge window the firmware programmed as taken,
3. takes the **largest size the card supports that fits**, switches memory decoding off, writes the new size,
   places BAR0 (and the other prefetchable BARs of the Radeon behind it) and the prefetchable windows of the
   bridges above it, switches decoding on again,
4. reads everything back; on any mismatch the old values are written back,
5. if a GOP framebuffer lives inside the old BAR0 (modes 1 and 2: the Radeon drives the screen), it is re-pointed
   to the new address, because bootmgr / winload draw straight into it.

The result is the line `Resizable BAR: ...` on the progress screen, above `FakeSecureBoot: ...`: `OK, BAR0 256 ->
8192 MB at <address> (GOP moved: n)`, `off`, or `skipped, <reason>` / `read-back mismatch, old values restored`
(nothing is changed then). Mode 4 powers the Radeon off, so it is always skipped there. It is applied after the
menu, so if a boot ever ends on a black screen, reboot, open the Advanced menu and set it to **Off**.

Windows needs nothing else (Above 4G Decoding is already reported); after the boot GPU-Z should show
`Resizable BAR enabled in BIOS: Yes` and BAR0 equal to the VRAM size.

### gpu-power-prefs

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

## macOS and `gpu-power-prefs`

macOS resets `gpu-power-prefs`. If you forced the iGPU with **Switch to iGPU** and then boot **macOS**, the flag is
cleared. At the next start the loader sees the preference as dGPU again, so **Hybrid Boot** (mode 5) is shown as
`unavailable: Switch to iGPU` and **Standart Boot** (mode 1) is available. To use Hybrid Boot again, activate
the flag once more in the loader: **Advanced menu > Switch to iGPU** (the Mac reboots), then choose mode 5. Do
this after every boot into macOS. Nothing has to be changed in macOS itself.

## What to expect

- **Mode 4 with `gpu-power-prefs` = dGPU** (the firmware default, Radeon): after the switch to the Intel iGPU the
  picture appears **on the Windows login screen**. Nothing is shown on the built-in display before that (no boot
  logo, no loader screen after the switch).
- **Mode 5 after macOS:** booting macOS resets `gpu-power-prefs` (see **macOS and `gpu-power-prefs`**), so mode 5
  is unavailable until you use **Switch to iGPU** in the Advanced menu again.
- **Mode 5:** the Radeon stays on, only the panel is moved to the iGPU. Its ACPI patch re-routes
  gmux to the iGPU on resume (the firmware puts the panel back on the Radeon, see
  [docs/ACPI_PATCH_GUIDE_MODE5.md](docs/ACPI_PATCH_GUIDE_MODE5.md), section 5.2). That re-route is untested: if
  the Mac hangs or powers off on resume, restart instead of sleeping and report it.

## Warnings

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