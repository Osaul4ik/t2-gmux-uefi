# Instructions for Claude: generate the ACPI patch (brightness, sleep)

This file is written for Claude (or any AI agent) that is asked to build the ACPI patch files
`SSDT_IGPU.aml` (mode 4, Efficient Boot) for the `t2-gmux-uefi` loader in this repository. It is not a
user manual. Follow it as a procedure, in order, and report what you actually verified.

Reference hardware: MacBook Pro 2019 with the T2 chip (Intel iGPU + AMD Radeon, Apple gmux), Windows
via Boot Camp. Everything below is derived from the working `tools/SSDT_IGPU.asl`. On any other
machine, treat every path, address and name as a hypothesis until you have confirmed it in that
machine's own ACPI dump (section 8).

## 0. Ground rules

- Never say the patch works. You cannot run it. Say what you compiled and what you disassembled.
- Do not invent ACPI paths, method names or addresses. Read them from the dumped tables.
- Do not rename devices. The SSDT uses the firmware's own names (`IGPU`, `GFX0`). The loader does not
  rename GPU devices.
- Only edit `tools/SSDT_IGPU.asl`, never a generated `.aml`. Rebuild the `.aml` from the `.asl`.
- Never overwrite `tools/SSDT_IGPU.asl` with generated output. Generate into a different directory
  or file name.
- If an input is missing (an ACPI dump), ask the user for it. Do not fabricate binary data.
- `SSDT_IGPU.aml` (mode 4) and `SSDT_IGPU_BRT.aml` (mode 5) are not interchangeable. Never copy one
  under the other's name.

## 1. Boot modes and the files they need

The loader has four modes: 1, 4 and 5 in the main menu and 2 in the Advanced menu:

| Key | Mode | ACPI patch file (ESP root) | VBT |
|-----|------|----------------------------|-----|
| **1** | Standart Boot (Radeon only) | none | none |
| **2** | Standart Boot + Intel Secondary (Advanced menu) | none | none |
| **4** | Efficient Boot (Intel only) | `\SSDT_IGPU.aml` | built into the loader, completed from the panel (DPCD / EDID), injected from UEFI; no file |
| **5** | Hybrid Boot (Intel + Radeon) | `\SSDT_IGPU_BRT.aml` | as mode 4 |

Mode **5** is mode 4 with the Radeon left ON and a brightness-only SSDT (no sleep fix). Its patch file is
built from `tools/SSDT_IGPU_BRT.asl` and has its own procedure: [ACPI_PATCH_GUIDE_MODE5.md](ACPI_PATCH_GUIDE_MODE5.md).
Everything below in this file is about mode 4.

A mode whose files are missing cannot be started. The menu stays, the timer stops and the status line
names the missing file (`SSDT_IGPU.aml` for mode 4). This also applies to the auto-boot default.

Modes **1** and **2** never load an SSDT.

## 2. How the loader uses the SSDT (what you are plugging into)

In mode **4** the loader does this in memory before Windows starts:

1. In the firmware table `SaSsdt` it renames the only `_BCM` to `XBCM` (it refuses to patch if there
   is not exactly one `_BCM`).
2. If the SSDT file contains the 4-byte names `XWAK` / `XPTS`, it renames the DSDT's `_WAK` / `_PTS`
   (located through the FADT) to `XWAK` / `XPTS`.
3. It appends the SSDT to a copy of the XSDT and republishes the RSDP. If any later step fails it
   reverts the renames.

So your SSDT must define **new** `_BCM`, `_PTS`, `_WAK` that call the renamed originals
(`XBCM`, `XPTS`, `XWAK`). If the SSDT does not mention `XWAK` / `XPTS`, the loader does not rename
`_WAK` / `_PTS`, and your SSDT must not define them. The renames are done by the loader, never
by the AML.

Mode **4** injects its built-in VBT from UEFI (link from the panel DPCD, timing from the EDID); its SSDT
(`SSDT_IGPU.aml`) carries no VBT.

## 3. Prerequisites

Install tools if missing: `apt-get install -y acpica-tools` (provides `iasl`). Python 3 is needed for
the generator. Check with `which iasl`.

Repo files you need (all under `tools/`):

- `SSDT_IGPU.asl`: the source.
- `make_ssdt_igpu.py`: compiles it with `iasl`.

## 4. Get the ACPI tables

Ask the user for a dump, or tell them how to take one.

Linux, as root:

```
cd /sys/firmware/acpi/tables
cp DSDT /tmp/dsdt.dat
grep -l SaSsdt SSDT* | head          # find the SaSsdt table
cp SSDT3 /tmp/sassdt.dat            # use the file grep printed
cd /tmp && iasl -d dsdt.dat sassdt.dat
```

Windows: `acpidump` from ACPICA gives the same tables. Disassemble with `iasl -d`.

You now work from `dsdt.dsl` and `sassdt.dsl`. Read them with `grep` / `view`, do not guess.

## 5. Brightness and sleep patch

`SSDT_IGPU.aml` carries the brightness patch (5.1) and the sleep patch (5.2).

### 5.1 Brightness (`_BCM`)

Goal: the Intel brightness method keeps working and the Apple gmux backlight is driven as well.
On the reference machine the panel is dimmed by gmux (port 0x74) through Apple's `ABCM` method of the
Radeon device, not by an Intel PWM.

Find and confirm in the dumps:

- `sassdt.dsl` contains exactly one `_BCM` and you know its full path
  (reference: `\_SB.PCI0.IGPU.DD1F._BCM`).
- The Radeon device path and its `ABCM` method
  (reference: `\_SB.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM`, one argument).

Pattern (reference values, replace with confirmed ones):

```
Scope (\_SB.PCI0.IGPU.DD1F)
{
    Method (_BCM, 1, NotSerialized)
    {
        XBCM (Arg0)                                              // original, Intel side
        If (CondRefOf (\_SB.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM))
        {
            \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM (Arg0)            // gmux side
        }
    }
}
```

Add matching `External (...)` lines at the top of the definition block for everything you reference
(`DD1F`, `DD1F.XBCM`, `GFX0.ABCM`, ...). `XBCM` is declared under `DD1F` because the loader renames
`_BCM` inside `SaSsdt`, where `DD1F` lives.

### 5.2 Sleep (`_PTS` and `_WAK`)

Problem: after resume the firmware re-enables the Radeon and leaves gmux in its default routing, so
the internal panel goes dark.

Reference behaviour of the working SSDT:

- `_PTS (Arg0)`: store whether the Radeon is already off before sleep
  (`GFX0.GVEN == 0xFFFF` means the dGPU is off) into a global `T2OF`, then call `XPTS (Arg0)`.
- `_WAK (Arg0)`: `Local0 = XWAK (Arg0)`; then, if `GFX0.MBWR` exists, re-route gmux with
  `MBWR (0x28, 1, 1)` (DDC), `MBWR (0x10, 1, 2)` (panel) and `MBWR (0x40, 1, 3)` (external), then sleep
  10 ms. If `T2OF == 1`, read the panel port directly through the gmux MMIO window; if the read is
  valid and bit 0 is clear, power the Radeon rail off again (`MBWR (0x50, 1, 1)` then
  `MBWR (0x50, 1, 0)`, then 250 ms). Finally restore brightness with `ABCM (BRTL)` and
  `Return (Local0)`.
- The MMIO window is `OperationRegion (T2GR, SystemMemory, 0xFE0B0200, 0x10)` with data at +0x00,
  command at +0x0E and status at +0x0F. This address is specific to the T2 gmux.

Rules:

- `_PTS` returns nothing. `_WAK` must return what `XWAK` returned. Do not drop that return value.
- Every object you reference (`GFX0.CSTS`, `GFX0.GVEN`, `GFX0.MBWR`, `BRTL`, `XPTS`, `XWAK`) needs an
  `External` line.
- Guard calls to methods that might not exist with `CondRefOf`. The reference `_PTS` / `_WAK` call
  `GFX0.CSTS` and read `GFX0.GVEN` unguarded, which is fine on the reference machine and breaks on a
  machine without that device. Add guards when you adapt it.

## 6. VBT

The T2 firmware leaves the Intel OpRegion VBT mailbox empty, so the Windows Intel driver never learns
about the eDP panel on DDI A. In mode 4 the loader fixes that itself, from UEFI: it writes its built-in
VBT to OpRegion+0x400, patches the link from the panel DPCD (PSR stays off) and the timing from the
EDID (Radeon, else the panel over AUX). No VBT file and no EDID are needed from the user, and the SSDT
carries no VBT.

**The SSDT must never write the VBT.** AML runs after UEFI: an AML copy into OpRegion+0x400 would
overwrite the injected VBT and the EDID timing taken from the Radeon. `SSDT_IGPU.aml` therefore must not
contain `_INI` on the IGPU device.

## 7. Build and verify

Build into a directory other than `tools/`:

```
OUT=/some/other/dir
python tools/make_ssdt_igpu.py -o $OUT/SSDT_IGPU.aml
iasl -d $OUT/SSDT_IGPU.aml                              # disassemble what you built
```

Check, and quote the evidence:

- `iasl` printed `Compilation successful. 0 Errors`. Read warnings and remarks, do not hide them.
- The disassembly contains `_BCM`, `_PTS` and `_WAK`, and **no** `_INI`.
- Every path in the `.dsl` exists in the dumps from section 4 (grep each one).

Deliver the file and say plainly that it was not run on hardware. On the target it goes to the ESP
root, named exactly `\SSDT_IGPU.aml`.

The loader progress screen shows `ACPI patch SSDT_IGPU.aml: ...`: `OK` = applied, `FAILED` = a check
failed and the renames were reverted. A missing file is caught earlier: the mode cannot be started and
the menu shows which file is missing. Rollback is deleting the file (mode 4 then becomes unavailable;
use mode 1 or 2).

## 8. Adapting to another machine

The reference patch is tied to one machine. Before reusing it elsewhere, confirm each of these in that
machine's dumps, and change the `.asl` where it differs:

- Radeon device path (`PEG0.EGP0.EGP1.GFX0`) and the methods / fields `ABCM`, `CSTS`, `MBWR`, `GVEN`.
- gmux MMIO address (`0xFE0B0200`) and the register offsets used.
- Brightness path (`IGPU.DD1F`) and exactly one `_BCM` in the table the loader renames in.

Do not use this patch on non-Apple machines: the MMIO address would point at unrelated registers. On
models without a Radeon there is nothing for the gmux part to drive, so build brightness only after
confirming what the machine actually has.

## 9. Pitfalls already hit

- `make_ssdt_igpu.py --keep-asl` writes `<out>.generated.asl`. An older version wrote `<out>.asl`, which
  overwrote the template when the output sat next to it.
- In `sh -c`, process substitution (`<(...)`) is a syntax error. Use `bash -c` for such commands.
- Do not write `iasl` output files into `tools/`.
- Do not use `SSDT_IGPU.aml` as `SSDT_IGPU_BRT.aml` (mode 5): it carries the sleep patch and the gmux MMIO
  window, which mode 5 must not have.