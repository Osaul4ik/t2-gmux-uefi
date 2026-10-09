#ifndef int_vbt_h
#define int_vbt_h
#include <efi.h>
#include <efiapi.h>
#include <efidef.h>

// ---- tiny ASCII report builder (no libc) ----
typedef struct {
    CHAR8*  buf;
    UINTN   len;
    UINTN   cap;
} _INT_Rep;

VOID _INT_RepInit(_INT_Rep* r, CHAR8* buf, UINTN cap);
VOID _INT_RepStr(_INT_Rep* r, const CHAR8* s);
VOID _INT_RepNl(_INT_Rep* r);
VOID _INT_RepHex(_INT_Rep* r, UINT64 v, UINTN digits);
VOID _INT_RepDec(_INT_Rep* r, UINT64 v);

// unaligned little-endian readers
UINT16 _INT_Rd16(const UINT8* p);
UINT32 _INT_Rd32(const UINT8* p);
UINT64 _INT_Rd64(const UINT8* p);

// Write Data to a file in the root of the volume this image was loaded from.
EFI_STATUS _INT_WriteEspFile(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                             CHAR16* Name, const VOID* Data, UINTN Size);

// Read a whole file (<= 1 MB) from the root of the boot volume (pool-allocated).
EFI_STATUS _INT_ReadEspFile(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                            CHAR16* Name, VOID** Data, UINTN* Size);

// Substitute the panel timing of the injected VBT (OpRegion+0x400) with the internal-panel
// EDID the firmware publishes on the dGPU GOP handle (EDID active/discovered protocol).
EFI_STATUS _INT_VbtApplyFirmwareEdid(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                     _INT_Rep* R);

// Set DDI_BUF_CTL(A).DDI_A_4_LANES in the iGPU so i915/Windows see 4 lanes on DDI A.
EFI_STATUS _INT_IgpuForceDdiA4Lanes(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                    _INT_Rep* R);

#endif