#ifndef int_acpi_h
#define int_acpi_h
#include <efi.h>
#include <efiapi.h>
#include <efidef.h>
#include "int_vbt.h"

// ACPI patch for the Intel panel brightness (modes 4 / 5).
//
// Reads a compiled SSDT (\SSDT_IGPU.aml for mode 4, \SSDT_IGPU_BRT.aml for mode 5, in the ESP root) and:
//   1. renames the single method "_BCM" of the firmware table "SaSsdt"
//      (\_SB.PCI0.IGPU.DD1F._BCM) to "XBCM" in memory, fixing the checksum
//   2. appends the SSDT to a copy of the XSDT (EfiACPIReclaimMemory) and
//      publishes it through a copy of the RSDP (InstallConfigurationTable)
// so Windows loads the new _BCM, which calls XBCM (Intel) and GFX0.ABCM (gmux).
//
// Returns EFI_NOT_FOUND if the file does not exist (nothing is touched).
// On any later failure the in-memory SaSsdt rename is reverted.
EFI_STATUS _INT_AcpiApplyPatch(EFI_BOOT_SERVICES* BS, EFI_SYSTEM_TABLE* ST,
                               EFI_HANDLE ImageHandle, CHAR16* Name, _INT_Rep* R);

#endif