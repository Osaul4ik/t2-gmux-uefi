/*
 * SSDT_IGPU_BRT.asl - ACPI patch of mode 5 (Hybrid Boot) for the t2-gmux-uefi loader
 * (MacBook Pro with T2, Intel iGPU + AMD Radeon). Brightness + gmux re-route on resume.
 * The Radeon stays powered, so there is no rail code here.
 *
 * Build (see docs/ACPI_PATCH_GUIDE_MODE5.md, section 7):
 *   python tools/make_ssdt_igpu.py --asl tools/SSDT_IGPU_BRT.asl -o SSDT_IGPU_BRT.aml      mode 5
 *
 * Edit only this file, never a generated .aml. This table carries no VBT: the loader injects
 * the VBT from UEFI, and AML must not overwrite it.
 *
 * The loader does the renames, not this file:
 *   SaSsdt  _BCM -> XBCM   (always)
 *   DSDT    _WAK -> XWAK   (only because this file mentions XWAK)
 * The name XPTS must NOT appear in this file's AML (not even as an External): the loader would then
 * rename the DSDT _PTS, and there is no replacement for it here.
 *
 * Reference machine: MacBook Pro 2019 (T2). Every path and name below is a hypothesis on any
 * other machine until it is confirmed in that machine's own dsdt / SaSsdt dump.
 */
DefinitionBlock ("", "SSDT", 2, "T2GMUX", "IGPUBRT", 0x00002000)
{
    External (_SB_.PCI0.IGPU.DD1F, DeviceObj)
    External (_SB_.PCI0.IGPU.DD1F.XBCM, MethodObj)                  // 1 Arg, renamed _BCM in SaSsdt
    External (_SB_.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM, MethodObj)        // 1 Arg, gmux backlight
    External (_SB_.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR, MethodObj)        // 3 Args, gmux write
    External (BRTL, FieldUnitObj)                                   // current brightness
    External (XWAK, MethodObj)                                      // 1 Arg, renamed DSDT _WAK

    // Brightness: keep the Intel side, drive gmux as well.
    Scope (\_SB.PCI0.IGPU.DD1F)
    {
        Method (_BCM, 1, NotSerialized)
        {
            XBCM (Arg0)
            If (CondRefOf (\_SB.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM))
            {
                \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM (Arg0)
            }
        }
    }

    // Resume: the firmware puts the panel back on the Radeon, route gmux to the iGPU again.
    Method (_WAK, 1, NotSerialized)
    {
        Local0 = XWAK (Arg0)
        If (CondRefOf (\_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR))
        {
            \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR (0x28, One, One)       // DDC
            \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR (0x10, One, 0x02)      // panel
            \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR (0x40, One, 0x03)      // external
            Sleep (0x0A)
            If (CondRefOf (\_SB.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM))
            {
                \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM (BRTL)             // restore brightness
            }
        }

        Return (Local0)
    }
}
