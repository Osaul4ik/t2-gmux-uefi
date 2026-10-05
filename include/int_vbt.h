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
VOID _INT_RepBytes(_INT_Rep* r, const UINT8* p, UINTN n);

// unaligned little-endian readers
UINT16 _INT_Rd16(const UINT8* p);
UINT32 _INT_Rd32(const UINT8* p);
UINT64 _INT_Rd64(const UINT8* p);

// ---- result of OpRegion / VBT inspection ----
typedef struct {
    BOOLEAN IgpuFound;
    UINT16  IgpuDevId;
    UINT32  Asls;
    BOOLEAN OpRegionOk;
    UINT32  OpVer;          // raw "over" dword
    BOOLEAN VbtFound;
    BOOLEAN VbtFromRvda;
    UINT16  VbtVer;
    UINT16  BdbVer;
    BOOLEAN EdpFound;       // first child device that looks like eDP
    UINT16  EdpType;
    UINT8   EdpDvoPort;
    UINT8   EdpAux;
    UINT8   EdpDdc;
    UINT8   PanelType;      // from BDB block 40 (0xFF = unknown)
    BOOLEAN HaveLink;
    UINT8   LinkRate;       // 0=1.62 1=2.7 2=5.4 (3=8.1 on newer VBT)
    UINT8   LinkLanes;      // 0=x1 1=x2 3=x4
    const UINT8* OpRegion;  // points into firmware memory
    UINTN   OpSize;
    const UINT8* Vbt;
    UINTN   VbtSize;
} _INT_VbtInfo;

// Pure parser (no EFI services): fills Info and appends a text report.
BOOLEAN _INT_VbtParse(const UINT8* Vbt, UINTN Avail, _INT_Rep* R, _INT_VbtInfo* Info);

// EFI part: locate Intel iGPU, read ASLS, map OpRegion, find VBT, parse.
EFI_STATUS _INT_InspectIgpuOpRegion(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                    _INT_Rep* R, _INT_VbtInfo* Info);

// Write Data to a file in the root of the volume this image was loaded from.
EFI_STATUS _INT_WriteEspFile(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                             CHAR16* Name, const VOID* Data, UINTN Size);

// Read a whole file (<= 1 MB) from the root of the boot volume (pool-allocated).
EFI_STATUS _INT_ReadEspFile(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                            CHAR16* Name, VOID** Data, UINTN* Size);

// Copy a VBT file from the ESP into OpRegion mailbox 4 (OpRegion+0x400).
EFI_STATUS _INT_InjectVbt(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                          CHAR16* Name, _INT_Rep* R);

// Substitute the panel timing of the injected VBT (OpRegion+0x400) with the internal-panel
// EDID the firmware publishes on the dGPU GOP handle (EDID active/discovered protocol).
EFI_STATUS _INT_VbtApplyFirmwareEdid(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                     _INT_Rep* R);

// Set DDI_BUF_CTL(A).DDI_A_4_LANES in the iGPU so i915/Windows see 4 lanes on DDI A.
EFI_STATUS _INT_IgpuForceDdiA4Lanes(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                    _INT_Rep* R);

// Snapshot of iGPU display registers (power wells, DDI A, eDP transcoder, panel power,
// backlight PWM) via BAR0, as a text report. Used to compare the state the Apple
// firmware leaves when it boots from the iGPU with the cold state of a Radeon boot.
EFI_STATUS _INT_DumpIgpuRegs(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                             _INT_Rep* R);

// TRUE if the Intel iGPU is visible right now (firmware booted from the iGPU).
BOOLEAN _INT_IgpuVisible(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle);

#endif