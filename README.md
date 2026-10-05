# GMUX_Control v0.8

A small UEFI loader for the **MacBook Pro 2019 with the T2 chip** (Intel iGPU + AMD Radeon). It starts
before Windows (Boot Camp) and lets you choose which GPU Windows will use: the AMD Radeon as usual, or
the Intel integrated graphics (iGPU).

## What it does

When the Mac starts, the loader shows a menu with four modes. You pick one (or it picks the default after
5 seconds), the loader prepares the GPUs and then starts the normal Windows boot loader.

| Key | Mode | What it does |
|-----|------|--------------|
| **1** | Standard Boot | Clean boot without AppleSetOs or patches. Windows starts as if the loader was not there. |
| **2** | Boot + Apple_set_os | Standard boot + the apple_set_os patch. Windows sees both GPUs (Radeon and Intel HD). |
| **3** | Integrated gfx | Windows runs on the Intel iGPU. The built-in screen is switched to the iGPU, the Radeon is powered off, an ACPI patch fixes brightness and sleep. The VBT (panel data) is inside the ACPI patch. |
| **4** | Integrated gfx + separate VBT | Same as 3, but the VBT is a separate file injected by the loader (it also takes the panel timing from the Radeon). **Default.** |

Modes 3 and 4 give the same result. Use the one whose files you have prepared (see below). If the
panel stays dark in one of them, try the other.

## What you need to prepare

| Mode | Files in the root of the EFI partition (ESP) |
|------|-----------------------------------------------|
| 1, 2 | nothing |
| 3 | `SSDT_IGPU_VBT.aml` |
| 4 | `SSDT_IGPU.aml` and `t2gmux_vbt.bin` |

If a file is missing, that mode cannot be started: the menu stays, the timer stops and the status line
says which file is missing.

How to get the files: see **Install** below (Claude makes them from your EDID and ACPI dumps). Background:

- `t2gmux_vbt.bin` (panel data): [docs/VBT_GUIDE.md](docs/VBT_GUIDE.md). The only thing you need is the
  panel EDID from Windows (`tools/get_edid.ps1`).
- `SSDT_IGPU.aml` / `SSDT_IGPU_VBT.aml` (brightness, sleep, optional VBT):
  [docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md).

## Install

The loader is already built: take the ready `bootx64.efi` (from the project's releases / the build
artifact of the GitHub Actions run). You do not need to build it.

Modes **1** and **2** need nothing else, go straight to step 4. Modes **3** and **4** need patch files
made for **your** Mac (`SSDT_IGPU*.aml`, `t2gmux_vbt.bin`). Claude makes them from the data you collect in
steps 1-3.

### 1. Collect the panel EDID (Windows, Boot Camp)

Run in PowerShell:

```
powershell -ExecutionPolicy Bypass -File .\tools\get_edid.ps1
```

It writes `edid_1.bin`, `edid_2.bin`, ... Keep all of them: Claude picks the internal panel (manufacturer
`APP`). Details: [docs/VBT_GUIDE.md](docs/VBT_GUIDE.md).

### 2. Collect the ACPI tables: DSDT and all SSDT (Windows)

Windows has no built-in tool for this. Download the ACPICA tools for Windows (acpica.org) and run in
PowerShell or cmd, in an empty folder:

```
acpidump.exe -b
```

This writes every table as a binary file: `dsdt.dat`, `ssdt1.dat`, `ssdt2.dat`, ... Take **all** of them
(the one that matters is the table named `SaSsdt`, Claude finds it). Do not disassemble or edit them.

On Linux the same files are in `/sys/firmware/acpi/tables/` (`DSDT`, `SSDT*`, as root); see
[docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md), section 4.

### 3. Send everything to Claude

Attach to one chat with Claude:

- the EDID files (`edid_*.bin`);
- the ACPI tables (`dsdt.dat`, `ssdt*.dat`);
- the **project archive** `t2-gmux-uefi-igfx.zip` (it contains `tools/`, `docs/ACPI_PATCH_GUIDE.md` and
  `docs/VBT_GUIDE.md`, the instructions Claude follows);
- `docs/ACPI_PATCH_GUIDE.md` and `docs/VBT_GUIDE.md` again as separate files, so they are certainly read.

Check before sending: the archive must contain `tools/SSDT_IGPU.asl`, `tools/make_ssdt_igpu.py`,
`tools/make_vbt.py` and a **non-empty** `tools/template/coreboot_google_sarien_data.vbt`.

Message to Claude (copy, change the mode):

```
Make the patch files for my MacBook Pro (T2) for mode 4 (or 3, or both).
Follow docs/ACPI_PATCH_GUIDE.md and docs/VBT_GUIDE.md from the project archive step by step.
Attached: panel EDID (edid_*.bin), DSDT and SSDT dumps (*.dat), the project archive.
Give me the ready files: t2gmux_vbt.bin, SSDT_IGPU.aml (mode 4) and/or SSDT_IGPU_VBT.aml (mode 3).
Show what you compiled and checked, and which ACPI paths you confirmed in my dumps.
```

Claude returns the files for the chosen mode. They are built from your dumps and were not run on
hardware, so test them (step 5) and report the result back to Claude if the panel stays dark.

### 4. Put the loader on the EFI partition

1. Set **Secure Boot = No Security** (macOS recovery, Startup Security Utility).
2. On the EFI partition rename `/EFI/Boot/bootx64.efi` to `bootx64_original.efi`.
3. Copy the ready `bootx64.efi` to `/EFI/Boot/`.

### 5. Put the patch files for your mode in the EFI partition root

| Mode | Copy to the root |
|------|------------------|
| 3 | `SSDT_IGPU_VBT.aml` |
| 4 | `SSDT_IGPU.aml` and `t2gmux_vbt.bin` |

Restart and pick the mode in the loader menu. A mode with missing files will refuse to start and show
which file is missing.

Building the loader yourself is optional (needs Docker):

```
docker build -t apple_set_os_loader .
docker run --rm -v "$(pwd):/build" apple_set_os_loader make clean all
```

## Use

- **Up / Down + Enter** (or Space) starts the highlighted mode. Pressing **1**-**4** starts that mode at once.
- If you press nothing for 5 seconds, the **default** mode starts. Any key stops the timer.
- The default mode is marked with an **x**. Press **X** to make the highlighted mode the default (it is
  saved in `t2gmux_default.txt` on the EFI partition; delete the file to go back to mode 4). X only saves,
  it does not start anything.
- The bottom of the screen lists the graphics cards the loader sees.

## Warnings

- After modes **3** / **4** switch the Radeon off, do **not** power it back on from Windows.
- Do not use `SSDT_IGPU_VBT.aml` under the name `SSDT_IGPU.aml` (or the other way round). In mode 4 it
  would overwrite the VBT the loader injects.
- If the screen is dark, restart the Mac and choose mode **1** (hold the key or press 1 at the menu) to
  boot normally on the Radeon.

## Undo

Delete `bootx64.efi` from `/EFI/Boot/` and rename `bootx64_original.efi` back to `bootx64.efi`. The
loader only writes one file, `t2gmux_default.txt`.

## More

- [docs/VBT_GUIDE.md](docs/VBT_GUIDE.md): how to make `t2gmux_vbt.bin`
- [docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md): how to make the SSDT files
- [docs/TECHNICAL.md](docs/TECHNICAL.md): what the loader does internally (gmux switching, ACPI patch, EDID, lanes)