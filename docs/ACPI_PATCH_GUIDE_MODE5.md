# Instructions for Claude: generate the mode 5 ACPI patch (brightness only)

This file is written for Claude (or any AI agent) that is asked to build `SSDT_IGPU_BRT.aml`, the ACPI
patch of **mode 5** of the `t2-gmux-uefi` loader. It is not a user manual. Follow it as a procedure, in
order, and report what you actually verified.

Mode 5 is mode 4 with the Radeon left powered ON, and it patches **brightness only**. Sleep is not
touched. For the patch files of modes 3 and 4 use [ACPI_PATCH_GUIDE.md](ACPI_PATCH_GUIDE.md); do not
mix the two guides.

Reference hardware: MacBook Pro 2019 with the T2 chip (Intel iGPU + AMD Radeon, Apple gmux), Windows
via Boot Camp. Every path and name below comes from the brightness part of the working
`tools/SSDT_IGPU.asl`. On any other machine, treat them as a hypothesis until you have confirmed them in
that machine's own ACPI dump (section 4 and section 8).

## 0. Ground rules

- Never say the patch works. You cannot run it. Say what you compiled and what you disassembled.
- Do not invent ACPI paths, method names or addresses. Read them from the dumped tables.
- Do not rename devices. The SSDT uses the firmware's own names (`IGPU`, `GFX0`).
- Only edit `tools/SSDT_IGPU_BRT.asl`, never a generated `.aml`. Rebuild the `.aml` from the `.asl`.
- Never overwrite `tools/SSDT_IGPU_BRT.asl` with generated output. Generate into a different directory.
- Keep the two markers `/*VBT_EXTERNALS*/` and `/*VBT_BLOCK*/` in the `.asl`. `make_ssdt_igpu.py` stops
  with an error if either is missing. In this build both are replaced with nothing.
- **Never build this file with `--vbt`.** Mode 5 takes its VBT from the loader (UEFI route). A VBT inside
  the SSDT would overwrite it, together with the panel timing the loader took from the panel.
- **Do not add sleep code.** No `_PTS`, no `_WAK`, no `XPTS`, no `XWAK`, no gmux MMIO window. See section 6.

## 1. Mode 5 and the file it needs

| Key | Mode | ACPI patch file (ESP root) | VBT | Radeon rail |
|-----|------|----------------------------|-----|-------------|
| **4** | Integrated gfx + built-in VBT | `\SSDT_IGPU.aml` (brightness + sleep) | built into the loader, completed from the panel | switched OFF |
| **5** | Integrated gfx + built-in VBT, Radeon ON | `\SSDT_IGPU_BRT.aml` (brightness only) | built into the loader, completed from the panel | left ON |

Mode 5 does everything mode 4 does (AppleSetOs, gmux panel -> iGPU, VBT injection, DDI A 4 lanes) except
the Radeon rail OFF. Its ACPI patch is a separate file with its own name.

A mode whose file is missing cannot be started: the menu stays, the timer stops and the status line says
`mode 5 unavailable: SSDT_IGPU_BRT.aml not found`. This also applies to the auto-boot default.

## 2. How the loader uses the SSDT (what you are plugging into)

In mode 5 the loader does this in memory before Windows starts:

1. In the firmware table `SaSsdt` it renames the only `_BCM` to `XBCM` (it refuses to patch if there is
   not exactly one `_BCM`).
2. It renames the DSDT `_WAK` / `_PTS` to `XWAK` / `XPTS` **only if the SSDT file contains those 4-byte
   names**. The mode 5 file contains neither, so the DSDT is left exactly as the firmware has it.
3. It appends the SSDT to a copy of the XSDT and republishes the RSDP. If a later step fails it reverts
   the rename. If a table with the same OEM table id and length is already installed it skips the patch.

So the SSDT defines a **new** `_BCM` that calls the renamed original `XBCM`, and nothing else.

## 3. Prerequisites

Install tools if missing: `apt-get install -y acpica-tools` (provides `iasl`). Python 3 is needed for the
generator. Check with `which iasl`.

Repo files you need (all under `tools/`):

- `SSDT_IGPU_BRT.asl`: the source of this patch.
- `make_ssdt_igpu.py`: compiles it with `iasl` (called with `--asl`, without `--vbt`).

You do **not** need `make_vbt.py`, the VBT template or an EDID for this file.

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

Windows: `acpidump.exe -b` from ACPICA writes `dsdt.dat`, `ssdt1.dat`, ... Take all of them and
disassemble with `iasl -d`. `ABCM` may live in another SSDT than `SaSsdt`, so disassemble every table and
search all of the `.dsl` files.

## 5. The brightness patch (`_BCM`)

Goal: the Intel brightness method keeps working and the Apple gmux backlight is driven as well. On the
reference machine the panel is dimmed by gmux (port 0x74) through Apple's `ABCM` method of the Radeon
device, not by an Intel PWM.

Find and confirm in the dumps (grep the `.dsl` files, quote what you found):

- `sassdt.dsl` contains **exactly one** `_BCM` and you know its full path
  (reference: `\_SB.PCI0.IGPU.DD1F._BCM`).
- The Radeon device path and its `ABCM` method, one argument
  (reference: `\_SB.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM`).

The whole body of the patch (reference values, replace with confirmed ones):

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
(`DD1F`, `DD1F.XBCM`, `GFX0.ABCM`). `XBCM` is declared under `DD1F` because the loader renames `_BCM` inside
`SaSsdt`, where `DD1F` lives.

## 6. What is deliberately NOT in this file

| Left out | Why |
|----------|-----|
| `_PTS`, `_WAK`, and the names `XPTS`, `XWAK` | Sleep is not touched in mode 5. The loader renames the DSDT `_PTS` / `_WAK` only for an SSDT that mentions `XPTS` / `XWAK`, so these names must not appear in the compiled AML, not even as an `External (...)` line (`iasl` writes the name of an external into the AML). Mentioning them in an `.asl` comment is harmless. |
| `OperationRegion (T2GR ...)` at `0xFE0B0200`, `T2OF`, `T2RD` | Only used by the sleep code. |
| `External` for `GFX0.CSTS`, `GFX0.GVEN`, `GFX0.MBWR`, `BRTL` | Only used by the sleep code. |
| `T2VB` buffer and `IGPU._INI` | That is the VBT-in-SSDT route of mode 3. Mode 5 gets its VBT from UEFI. |

Consequence to tell the user: with this patch the sleep / resume path is the firmware's own. The existing
guide ([ACPI_PATCH_GUIDE.md](ACPI_PATCH_GUIDE.md), section 5.2) describes why modes 3 / 4 re-route gmux after
resume; mode 5 does not, so after sleep the panel may stay dark. Do not claim otherwise and do not add the
sleep code on your own.

## 7. Build and verify

Build into a directory other than `tools/`:

```
OUT=/some/other/dir
python tools/make_ssdt_igpu.py --asl tools/SSDT_IGPU_BRT.asl -o $OUT/SSDT_IGPU_BRT.aml
iasl -d $OUT/SSDT_IGPU_BRT.aml                 # disassemble what you built
```

Check, and quote the evidence:

- `iasl` printed `Compilation successful. 0 Errors` for the file. Read warnings and remarks, do not hide
  them.
- The disassembly contains `_BCM` and nothing else defined: no `_PTS`, no `_WAK`, no `_INI`, no `T2GR`.
- The raw AML contains none of the names the loader looks for:

```
python3 -c "d=open('$OUT/SSDT_IGPU_BRT.aml','rb').read(); print({n:d.count(n.encode()) for n in ('_PTS','_WAK','XPTS','XWAK','_INI')})"
```

  Every count must be 0. (`XBCM` appears twice and `_BCM` once, that is expected.)
- Every path in the `.dsl` exists in the dumps from section 4 (grep each one).
- The OEM table id is `IGPUBRT`, so the file does not collide with `SSDT_IGPU.aml` (`IGPUBCM`).
- The reference build was 226 bytes.

Deliver the file and say plainly that it was not run on hardware. On the target it goes to the ESP root,
named exactly:

- mode 5: `\SSDT_IGPU_BRT.aml`

The loader progress screen shows `ACPI patch SSDT_IGPU_BRT.aml: ...`: `OK` = applied, `FAILED` = a check
failed and the rename was reverted. A missing file is caught earlier: the mode cannot be started.
Rollback is deleting the file (mode 5 then becomes unavailable; use mode 1 or 2).

## 8. Adapting to another machine

Confirm each of these in that machine's dumps and change `tools/SSDT_IGPU_BRT.asl` where it differs:

- Radeon device path (`PEG0.EGP0.EGP1.GFX0`) and the method `ABCM`.
- Brightness path (`IGPU.DD1F`) and exactly one `_BCM` in the table the loader renames in.

Do not use this patch on non-Apple machines. On models without a Radeon there is nothing for `ABCM` to
drive: confirm what the machine actually has before building anything.

## 9. Pitfalls

- Mixing up the files. `SSDT_IGPU.aml` copied to `SSDT_IGPU_BRT.aml` makes mode 5 apply the sleep patch
  (the loader sees `XWAK` / `XPTS` and renames the DSDT methods) and the gmux MMIO window. Check with the
  counts in section 7. `SSDT_IGPU_BRT.aml` copied to `SSDT_IGPU.aml` makes mode 4 lose its sleep fix.
- Building the `.asl` with `--vbt`: `_INI` appears and the VBT injected by the loader is overwritten.
- Removing the markers from the `.asl` makes the generator stop. Keep them.
- Do not write `iasl` output files into `tools/`, and do not point `-o` into `tools/`.
