# Instructions for Claude: generate the ACPI patch (brightness, sleep, VBT)

This file is written for Claude (or any AI agent) that is asked to build the ACPI patch files
`SSDT_IGPU.aml` and `SSDT_IGPU_VBT.aml` for the `t2-gmux-uefi` loader in this repository. It is not a
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
- Only edit `tools/SSDT_IGPU.asl`, never a generated `.aml`. Rebuild the `.aml` files from the `.asl`.
- Never overwrite `tools/SSDT_IGPU.asl` with generated output. Generate into a different directory
  or file name.
- Keep the two markers `/*VBT_EXTERNALS*/` and `/*VBT_BLOCK*/` in the `.asl`. `make_ssdt_igpu.py` stops
  with an error if either is missing.
- If an input is missing (EDID, VBT, template), ask the user for it or find it yourself in the repo.
  Do not fabricate binary data.
- Two different files exist and they are not interchangeable. Never copy one under the other's name
  (section 6).

## 1. Boot modes and the files they need

The loader menu has four modes:

| Key | Mode | ACPI patch file (ESP root) | VBT |
|-----|------|----------------------------|-----|
| **1** | Standard Boot | none | none |
| **2** | Boot + Apple_set_os | none | none |
| **3** | Integrated gfx | `\SSDT_IGPU_VBT.aml` | inside the SSDT (built with `--vbt`) |
| **4** | Integrated gfx + separate VBT | `\SSDT_IGPU.aml` | injected from UEFI, file `\t2gmux_vbt.bin` |

A mode whose files are missing cannot be started. The menu stays, the timer stops and the status line
names the missing file (for mode 3 `SSDT_IGPU_VBT.aml`; for mode 4 `SSDT_IGPU.aml` and / or
`t2gmux_vbt.bin`). This also applies to the auto-boot default.

Modes **1** and **2** never load an SSDT.

## 2. How the loader uses the SSDT (what you are plugging into)

In modes **3** and **4** the loader does this in memory before Windows starts, with the SSDT file of
that mode:

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

Mode **3** does not inject a VBT from UEFI: its VBT comes from `SSDT_IGPU_VBT.aml`. Mode **4**
injects `\t2gmux_vbt.bin` from UEFI and applies the EDID timing from the Radeon; its SSDT
(`SSDT_IGPU.aml`) carries no VBT.

## 3. Prerequisites

Install tools if missing: `apt-get install -y acpica-tools` (provides `iasl`). Python 3 is needed for
the generator. Check with `which iasl`.

Repo files you need (all under `tools/`):

- `SSDT_IGPU.asl`: the source (one source, two builds, see section 7).
- `make_ssdt_igpu.py`: compiles it with `iasl`, optionally embedding a VBT.
- `make_vbt.py` + `template/coreboot_google_sarien_data.vbt`: builds the VBT. If the template is 0 bytes
  (it is binary and gets lost in some archives), get it from the repository or from coreboot
  (board `google/sarien`). Do not continue with an empty template.
- `get_edid.ps1`: the user runs it on Windows to get `edid_N.bin`.

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

## 5. Brightness and sleep patch (both SSDT files)

Both `SSDT_IGPU.aml` and `SSDT_IGPU_VBT.aml` contain the same brightness and sleep code. They differ
only in the VBT block (section 6).

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

## 6. VBT: two files, two delivery routes

The T2 firmware leaves the Intel OpRegion VBT mailbox empty, so the Windows Intel driver never learns
about the eDP panel on DDI A. The two modes deliver the VBT differently, and each needs its own SSDT:

| | Mode 3, Integrated gfx | Mode 4, Integrated gfx + separate VBT |
|---|---|---|
| SSDT file | `SSDT_IGPU_VBT.aml` | `SSDT_IGPU.aml` |
| Built with | `--vbt t2gmux_vbt.bin` | no `--vbt` |
| VBT delivered by | ACPI: `IGPU._INI` inside the SSDT | UEFI: loader reads `\t2gmux_vbt.bin` |
| `t2gmux_vbt.bin` on the ESP | not needed (used only at build time) | required |
| EDID timing from the Radeon | no, the timing of the file stays | yes, written into the injected VBT |

How the ACPI route (mode 3) works: the SSDT built with `--vbt` carries the VBT and `IGPU._INI` copies
it to OpRegion+0x400 (mailbox 4, 0x1800 bytes max) while ACPI initialises, before the Intel driver
starts.

How the UEFI route (mode 4) works: the loader reads `\t2gmux_vbt.bin`, writes it to OpRegion+0x400,
then applies the EDID timing it finds on the Radeon.

Build steps for the VBT:

1. The user takes the panel EDID on Windows with `tools/get_edid.ps1`, giving `edid_N.bin`.
   The internal panel has manufacturer `APP`.
2. `python tools/make_vbt.py --edid edid_1.bin --lanes 4 --rate hbr2 -o t2gmux_vbt.bin`
   If the panel stays dark the user can retry `--lanes 2` or `--rate hbr`. Do not claim a specific
   value is right without a working result.

Build steps for the two SSDT files are in section 7.

What `_INI` does (generated by the script, do not hand-write it; present only in the `--vbt` build): it
takes `ASLS` (or `ASLB`) as the OpRegion address, stops silently if that is 0 or 0xFFFFFFFF, checks the
signature `IntelGraphicsMem` and that the OpRegion is large enough, then copies the VBT buffer to
OpRegion+0x400. It does not touch `rvda` / `rvds`, and it leaves the OpRegion header alone: i915 takes
the VBT from OpRegion+0x400 by its validity, there is no header flag for mailbox 4.

**Never put the `--vbt` SSDT into mode 4 under the name `SSDT_IGPU.aml`.** AML runs after UEFI. It
overwrites the VBT, and with it the EDID timing taken from the Radeon, with the file's timing. For
mode 4 build without `--vbt`. For mode 3 build with `--vbt` and name the result `SSDT_IGPU_VBT.aml`.

## 7. Build and verify

Build both files from the same `.asl`, into a directory other than `tools/`:

```
OUT=/some/other/dir
python tools/make_ssdt_igpu.py                         -o $OUT/SSDT_IGPU.aml       # mode 4, no VBT
python tools/make_ssdt_igpu.py --vbt t2gmux_vbt.bin    -o $OUT/SSDT_IGPU_VBT.aml   # mode 3, VBT inside
iasl -d $OUT/SSDT_IGPU.aml                              # disassemble what you built
iasl -d $OUT/SSDT_IGPU_VBT.aml
```

Build only the file for the mode the user asked about if they did not ask for both.

Check, and quote the evidence:

- `iasl` printed `Compilation successful. 0 Errors` for each file. Read warnings and remarks, do not
  hide them.
- Both disassemblies contain `_BCM`, `_PTS` and `_WAK`. `_INI` appears **only** in
  `SSDT_IGPU_VBT.aml` and **not** in `SSDT_IGPU.aml`. If `SSDT_IGPU.aml` contains `_INI`, it was built
  with `--vbt`: stop and rebuild.
- Every path in the `.dsl` exists in the dumps from section 4 (grep each one).
- For `SSDT_IGPU_VBT.aml`: the `T2VB` buffer size equals the VBT size padded to 8 bytes, and the VBT
  starts with `$VBT`.
- The plain build (`SSDT_IGPU.aml`) was 903 bytes on the reference build.

Deliver the file(s) and say plainly that they were not run on hardware. On the target the files go to
the ESP root, named exactly as in the table in section 1:

- mode 3: `\SSDT_IGPU_VBT.aml`
- mode 4: `\SSDT_IGPU.aml` and `\t2gmux_vbt.bin`

The loader progress screen shows `ACPI patch <file name>: ...` with the name of the file that mode
used: `OK` = applied, `FAILED` = a check failed and the renames were reverted. A missing file is
caught earlier: the mode cannot be started and the menu shows which file is missing. Rollback is
deleting the files (the modes then become unavailable; use mode 1 or 2).

## 8. Adapting to another machine

The reference patch is tied to one machine. Before reusing it elsewhere, confirm each of these in that
machine's dumps, and change the `.asl` where it differs:

- Radeon device path (`PEG0.EGP0.EGP1.GFX0`) and the methods / fields `ABCM`, `CSTS`, `MBWR`, `GVEN`.
- gmux MMIO address (`0xFE0B0200`) and the register offsets used.
- Brightness path (`IGPU.DD1F`) and exactly one `_BCM` in the table the loader renames in.
- The VBT: own EDID, own lane count and link rate.

Do not use this patch on non-Apple machines: the MMIO address would point at unrelated registers. On
models without a Radeon there is nothing for the gmux part to drive, so build brightness only after
confirming what the machine actually has.

## 9. Pitfalls already hit

- `make_ssdt_igpu.py --keep-asl` writes `<out>.generated.asl`. An older version wrote `<out>.asl`, which
  overwrote the template when the output sat next to it.
- A 0-byte VBT template makes `make_vbt.py` fail. Fix the template, do not work around it.
- In `sh -c`, process substitution (`<(...)`) is a syntax error. Use `bash -c` for such commands.
- Do not write `iasl` output files into `tools/`.
- Mixing up the two SSDT files: `SSDT_IGPU_VBT.aml` renamed to `SSDT_IGPU.aml` makes mode 4 overwrite
  the injected VBT; `SSDT_IGPU.aml` used as `SSDT_IGPU_VBT.aml` makes mode 3 start with an empty VBT
  mailbox and the panel may stay dark. Check for `_INI` as in section 7.
- Old guide and old README used the letters D / A / H / I for the modes. They are now 1 / 2 / 3 / 4
  (D = 1, A = 2, H = 3, I = 4).
