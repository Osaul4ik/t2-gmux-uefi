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

How to make the files:

- `t2gmux_vbt.bin` (panel data): [docs/VBT_GUIDE.md](docs/VBT_GUIDE.md). The only thing you need is the
  panel EDID from Windows (`tools/get_edid.ps1`).
- `SSDT_IGPU.aml` / `SSDT_IGPU_VBT.aml` (brightness, sleep, optional VBT):
  [docs/ACPI_PATCH_GUIDE.md](docs/ACPI_PATCH_GUIDE.md).

## Install

The loader is already built: take the ready `bootx64.efi` (from the project's releases / the build
artifact of the GitHub Actions run). You do not need to build anything.

1. Set **Secure Boot = No Security** (macOS recovery, Startup Security Utility).
2. On the EFI partition rename `/EFI/Boot/bootx64.efi` to `bootx64_original.efi`.
3. Copy the ready `bootx64.efi` to `/EFI/Boot/`.
4. Copy the files for your mode (table above) to the root of the EFI partition.

Building it yourself is optional (needs Docker):

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