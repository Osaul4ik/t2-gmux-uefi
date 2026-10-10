# Instructions for Claude: generate the mode 5 ACPI patch (brightness + resume)

This file is written for Claude (or any AI agent) that is asked to build `SSDT_IGPU_BRT.aml`, the ACPI
patch of **mode 5** of the `t2-gmux-uefi` loader. It is not a user manual. Follow it as a procedure, in
order, and report what you actually verified.

Mode 5 is mode 4 with the Radeon left powered ON. It patches **brightness** and re-routes gmux on **resume**
(`_WAK`). It has no Radeon rail code, because nothing is switched off. For the patch file of mode 4 use
[ACPI_PATCH_GUIDE.md](ACPI_PATCH_GUIDE.md); do not mix the two guides.

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
- **Never put a VBT into this SSDT** (no `IGPU._INI`). Mode 5 takes its VBT from the loader (UEFI route). A VBT
  written by AML would overwrite it, together with the panel timing the loader took from the panel.
- **Do not add Radeon rail code.** No `_PTS`, no `XPTS`, no gmux MMIO window, no rail OFF. See section 6.

## 1. Mode 5 and the file it needs

| Key | Mode | ACPI patch file (ESP root) | VBT | Radeon rail |
|-----|------|----------------------------|-----|-------------|
| **4** | Integrated gfx + built-in VBT | `\SSDT_IGPU.aml` (brightness + sleep) | built into the loader, completed from the panel | switched OFF |
| **5** | Integrated gfx + built-in VBT, Radeon ON | `\SSDT_IGPU_BRT.aml` (brightness + resume re-route) | built into the loader, completed from the panel | left ON |

Mode 5 does everything mode 4 does (AppleSetOs, gmux panel -> iGPU, VBT injection, DDI A 4 lanes) except
the Radeon rail OFF. Its ACPI patch is a separate file with its own name.

A mode whose file is missing cannot be started: the menu stays, the timer stops and the status line says
`mode 5 unavailable: SSDT_IGPU_BRT.aml not found`. This also applies to the auto-boot default.

## 2. How the loader uses the SSDT (what you are plugging into)

In mode 5 the loader does this in memory before Windows starts:

1. In the firmware table `SaSsdt` it renames the only `_BCM` to `XBCM` (it refuses to patch if there is
   not exactly one `_BCM`).
2. It renames the DSDT `_WAK` / `_PTS` to `XWAK` / `XPTS` **only if the SSDT file contains those 4-byte
   names**, each one on its own. The mode 5 file contains `XWAK` but not `XPTS`, so only `_WAK` is renamed
   and the DSDT `_PTS` is left as the firmware has it.
3. It appends the SSDT to a copy of the XSDT and republishes the RSDP. If a later step fails it reverts
   the rename. If a table with the same OEM table id and length is already installed it skips the patch.

So the SSDT defines a **new** `_BCM` that calls the renamed original `XBCM`, and a **new** `_WAK` that
calls the renamed original `XWAK`, and nothing else.

## 3. Prerequisites

Install tools if missing: `apt-get install -y acpica-tools` (provides `iasl`). Python 3 is needed for the
generator. Check with `which iasl`.

Repo files you need (all under `tools/`):

- `SSDT_IGPU_BRT.asl`: the source of this patch.
- `make_ssdt_igpu.py`: compiles it with `iasl` (called with `--asl`).

You do **not** need an EDID or any VBT file for this file.

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

## 5. The patch (`_BCM` and `_WAK`)

### 5.1 Brightness (`_BCM`)

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
(`DD1F`, `DD1F.XBCM`, `GFX0.ABCM`, `GFX0.MBWR`, `BRTL`, `XWAK`). `XBCM` is declared under `DD1F` because the
loader renames `_BCM` inside `SaSsdt`, where `DD1F` lives.

### 5.2 Resume (`_WAK`)

Problem: after resume the firmware leaves gmux in its default routing (panel on the Radeon), while the
Intel driver still drives the panel on DDI A. The panel goes dark or Windows hangs on resume. The loader
only routes gmux once, at boot.

Confirm in the dumps: the DSDT has exactly one `_WAK` (1 Arg, returns a 2-element package) and
`GFX0.MBWR` takes 3 arguments and waits for `GFX0.CSTS` itself (gmux window `0xFE0B0200`, independent of the
Radeon's PCI BARs, so it works while the Radeon is on).

```
Method (_WAK, 1, NotSerialized)
{
    Local0 = XWAK (Arg0)                                         // must be returned unchanged
    If (CondRefOf (\_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR))
    {
        \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR (0x28, One, One)      // DDC -> iGPU
        \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR (0x10, One, 0x02)     // panel -> iGPU
        \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR (0x40, One, 0x03)     // external stays on the Radeon
        Sleep (0x0A)
        If (CondRefOf (\_SB.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM))
        {
            \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM (BRTL)            // restore brightness
        }
    }

    Return (Local0)
}
```

The three values are the ones the loader writes at boot (DDC = 1, panel = 2 = iGPU, external = 3 = dGPU).
Unlike mode 4 there is no rail code: the Radeon is never switched off in mode 5, so `_PTS` is not touched
and there is no `T2OF`.

## 6. What is deliberately NOT in this file

| Left out | Why |
|----------|-----|
| `_PTS` and the name `XPTS` | The Radeon is never switched off in mode 5, so there is nothing to remember before sleep. The loader renames the DSDT `_PTS` only for an SSDT that mentions `XPTS`, so this name must not appear in the compiled AML, not even as an `External (...)` line (`iasl` writes the name of an external into the AML). Mentioning it in an `.asl` comment is harmless. |
| `OperationRegion (T2GR ...)` at `0xFE0B0200`, `T2OF`, `T2RD` | Only used by the rail-off check of mode 4. |
| `External` for `GFX0.CSTS`, `GFX0.GVEN` | Only used by the rail-off check of mode 4. |
| `T2VB` buffer and `IGPU._INI` | A VBT written from AML would overwrite the one the loader injects. Mode 5 gets its VBT from UEFI. |

Consequence to tell the user: with this patch sleep / resume is the firmware's own, plus the gmux re-route
of section 5.2. That re-route is the same as in mode 4 but it has **not been confirmed on hardware**; say
so, and do not claim it fixes a hang until the user reports back.

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
- The disassembly defines `_BCM` and `_WAK` and nothing else: no `_PTS`, no `_INI`, no `T2GR`.
- The raw AML contains none of the names the loader looks for:

```
python3 -c "d=open('$OUT/SSDT_IGPU_BRT.aml','rb').read(); print({n:d.count(n.encode()) for n in ('_PTS','XPTS','_INI','T2GR')})"
```

  Every count must be 0. (`XBCM` and `XWAK` appear twice, `_BCM` and `_WAK` once, that is expected.)
- Every path in the `.dsl` exists in the dumps from section 4 (grep each one).
- The OEM table id is `IGPUBRT`, so the file does not collide with `SSDT_IGPU.aml` (`IGPUBCM`).
- The reference build was 513 bytes.

Deliver the file and say plainly that it was not run on hardware. On the target it goes to the ESP root,
named exactly:

- mode 5: `\SSDT_IGPU_BRT.aml`

The loader progress screen shows `ACPI patch SSDT_IGPU_BRT.aml: ...`: `OK` = applied, `FAILED` = a check
failed and the rename was reverted. A missing file is caught earlier: the mode cannot be started.
Rollback is deleting the file (mode 5 then becomes unavailable; use mode 1 or 2).

## 8. Adapting to another machine

Confirm each of these in that machine's dumps and change `tools/SSDT_IGPU_BRT.asl` where it differs:

- Radeon device path (`PEG0.EGP0.EGP1.GFX0`) and the methods `ABCM`, `MBWR`.
- The gmux port numbers and values written in `_WAK` (0x28, 0x10, 0x40) and exactly one `_WAK` in the DSDT.
- Brightness path (`IGPU.DD1F`) and exactly one `_BCM` in the table the loader renames in.

Do not use this patch on non-Apple machines. On models without a Radeon there is nothing for `ABCM` to
drive: confirm what the machine actually has before building anything.

## 9. Pitfalls

- Mixing up the files. `SSDT_IGPU.aml` copied to `SSDT_IGPU_BRT.aml` makes mode 5 apply the rail-off check
  (the loader sees `XPTS` and renames the DSDT `_PTS`) and the gmux MMIO window. Check with the counts in
  section 7. `SSDT_IGPU_BRT.aml` copied to `SSDT_IGPU.aml` makes mode 4 lose its rail-off after sleep.
- Adding `IGPU._INI` / a VBT buffer to the `.asl`: the VBT injected by the loader is overwritten.
- Do not write `iasl` output files into `tools/`, and do not point `-o` into `tools/`.