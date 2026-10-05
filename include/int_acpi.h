#ifndef int_acpi_h
#define int_acpi_h
#include <efi.h>
#include <efiapi.h>
#include <efidef.h>
#include "int_vbt.h"

// 1 = also swap the ACPI roles of the GPUs in memory (all tables, same-length names):
//     IGPU -> GFX0 (Intel takes the dGPU name), GFX0 -> EGFX (Radeon becomes the eGPU name).
//     0 = brightness patch only.
#ifndef ACPI_ROLE_RENAME
#define ACPI_ROLE_RENAME 1
#endif

// ACPI patch for the Intel panel brightness (keys I / U).
//
// Reads a compiled SSDT (default \SSDT_IGPU.aml in the ESP root) and:
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