/*
 * SSDT_IGPU.asl - ACPI patch for the t2-gmux-uefi loader (MacBook Pro with T2, Intel iGPU + AMD Radeon).
 *
 * Build (see docs/ACPI_PATCH_GUIDE.md, section 7):
 *   python tools/make_ssdt_igpu.py -o SSDT_IGPU.aml      mode 4
 *
 * Edit only this file, never a generated .aml. This table carries no VBT: the loader injects
 * the VBT from UEFI, and AML must not overwrite it.
 *
 * The loader does the renames, not this file:
 *   SaSsdt  _BCM -> XBCM   (always)
 *   DSDT    _PTS -> XPTS   (only because this file mentions XPTS)
 *   DSDT    _WAK -> XWAK   (only because this file mentions XWAK)
 *
 * Reference machine: MacBook Pro 2019 (T2). Every path, address and name below is a hypothesis
 * on any other machine until it is confirmed in that machine's own dsdt / SaSsdt dump.
 * gmux MMIO window 0xFE0B0200 is specific to the T2 gmux.
 */
DefinitionBlock ("", "SSDT", 2, "T2GMUX", "IGPUBCM", 0x00002000)
{
    External (_SB_.PCI0.IGPU.DD1F, DeviceObj)
    External (_SB_.PCI0.IGPU.DD1F.XBCM, MethodObj)                  // 1 Arg, renamed _BCM in SaSsdt
    External (_SB_.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM, MethodObj)        // 1 Arg, gmux backlight
    External (_SB_.PCI0.PEG0.EGP0.EGP1.GFX0.CSTS, MethodObj)        // 0 Args, gmux ready
    External (_SB_.PCI0.PEG0.EGP0.EGP1.GFX0.GVEN, FieldUnitObj)     // Radeon vendor id, 0xFFFF = off
    External (_SB_.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR, MethodObj)        // 3 Args, gmux write
    External (BRTL, FieldUnitObj)                                   // current brightness
    External (XPTS, MethodObj)                                      // 1 Arg, renamed DSDT _PTS
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

    // Direct gmux MMIO window: data at +0x00, command at +0x0E, status at +0x0F.
    OperationRegion (T2GR, SystemMemory, 0xFE0B0200, 0x10)
    Field (T2GR, ByteAcc, NoLock, Preserve)
    {
        T2GD,   8,
        Offset (0x0E),
        T2GC,   8,
        T2GS,   8
    }

    Name (T2OF, Zero)               // 1 = Radeon was already off before sleep

    // Read one gmux register; 0xFF means "not valid".
    Method (T2RD, 1, Serialized)
    {
        Local1 = 0xFF
        If (\_SB.PCI0.PEG0.EGP0.EGP1.GFX0.CSTS ())
        {
            T2GC = Arg0
            T2GS = One
            If (\_SB.PCI0.PEG0.EGP0.EGP1.GFX0.CSTS ())
            {
                Local1 = T2GD
            }
        }

        Return (Local1)
    }

    Method (_PTS, 1, NotSerialized)
    {
        If ((\_SB.PCI0.PEG0.EGP0.EGP1.GFX0.GVEN == 0xFFFF))
        {
            T2OF = One
        }
        Else
        {
            T2OF = Zero
        }

        XPTS (Arg0)
    }

    Method (_WAK, 1, NotSerialized)
    {
        Local0 = XWAK (Arg0)
        If (CondRefOf (\_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR))
        {
            \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR (0x28, One, One)       // DDC
            \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR (0x10, One, 0x02)      // panel
            \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR (0x40, One, 0x03)      // external
            Sleep (0x0A)
            If ((T2OF == One))
            {
                Local1 = T2RD (0x10)
                If (((Local1 != 0xFF) && ((Local1 & One) == Zero)))
                {
                    \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR (0x50, One, One)   // Radeon rail off again
                    \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.MBWR (0x50, One, Zero)
                    Sleep (0xFA)
                }
            }

            If (CondRefOf (\_SB.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM))
            {
                \_SB.PCI0.PEG0.EGP0.EGP1.GFX0.ABCM (BRTL)             // restore brightness
            }
        }

        Return (Local0)
    }
}
