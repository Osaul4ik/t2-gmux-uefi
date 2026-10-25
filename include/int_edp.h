#ifndef int_edp_h
#define int_edp_h
#include "int_vbt.h"
#include <efiprot.h>

// What the panel reports over the iGPU's own eDP AUX channel (DDI A / AUX-A).
typedef struct {
    BOOLEAN Valid;          // DPCD 0x000..0x00F could be read
    UINT8   DpcdRev;        // 0x000
    UINT8   MaxLinkRate;    // 0x001 (0x06 RBR, 0x0A HBR, 0x14 HBR2, 0x1E HBR3)
    UINT8   MaxLanes;       // 0x002 & 0x1F
    UINT8   EdpRev;         // 0x700 (3 = eDP 1.4: SUPPORTED_LINK_RATES valid)
    UINT8   PsrSupport;     // 0x070 (0 = panel has no PSR)
    UINT16  RateTable[8];   // 0x010.., units of 200 kHz (0 = unused)
    UINT8   VbtRate;        // derived: VBT code 0=1.62 1=2.7 2=5.4
    UINT8   VbtLanes;       // derived: VBT code 0=x1 1=x2 3=x4
    BOOLEAN HasEdid;        // 128-byte base EDID read through I2C-over-AUX
    UINT8   Edid[128];
} _INT_EdpCaps;

// Panel power sequencer timing as the FIRMWARE programmed it. On a boot where the firmware lit
// the internal panel these are the known-good values; on a boot with an external display the
// firmware leaves PP_DIVISOR / rawclk at reset (0) and i915 would program them before the first
// power-up, so the loader restores them from the ESP cache.
typedef struct {
    BOOLEAN Valid;
    UINT32  OnDelays;       // PP_ON_DELAYS
    UINT32  OffDelays;      // PP_OFF_DELAYS
    UINT32  Divisor;        // PP_DIVISOR (reference divider [31:8], power cycle delay [4:0])
    UINT32  RawClk;         // PCH_RAWCLK_FREQ
} _INT_EdpPps;

// First Intel display-class PCI device (the iGPU), NULL if not visible.
EFI_PCI_IO_PROTOCOL* _INT_FindIgpu(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                   UINT32* IdOut, EFI_STATUS* StOut);

// Pure helpers (no EFI services, host-testable).
VOID    _INT_EdpDeriveLink(_INT_EdpCaps* C);
BOOLEAN _INT_EdidValid(const UINT8* e);
// Patch link rate / lanes for the active panel into a VBT in RAM, clear fast link
// training, switch PSR off when the panel has none, fix the checksum.
BOOLEAN _INT_VbtSetLink(UINT8* v, UINTN size, UINT8 rate, UINT8 lanes,
                        BOOLEAN PsrOk, _INT_Rep* R);

// Power-up of what AUX-A needs (power wells, panel VDD), DPCD + EDID read via AUX-A.
// Restores the panel-power state it found. Never writes anything the i915 driver
// would not write at init.
EFI_STATUS _INT_EdpProbe(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                         _INT_EdpCaps* C, _INT_Rep* R);

// Snapshot Intel panel power at pre-Windows. PP_STATUS bit31 = actual panel power.
// Unlike FORCE_VDD this is not merely AUX availability.
EFI_STATUS _INT_EdpPanelPowerState(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                   BOOLEAN* Powered, UINT32* StatusReg,
                                   UINT32* ControlReg, _INT_Rep* R);
// Explicitly power on the built-in panel PPS after switching GMUX to Intel.
// Does not enable DDI buffer, pipe or backlight; Windows owns link training.
EFI_STATUS _INT_EdpPanelPrepare(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                 _INT_Rep* R);

// Read the sequencer timing registers (Valid = firmware programmed them).
EFI_STATUS _INT_EdpReadPps(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                           _INT_EdpPps* P, _INT_Rep* R);
// Panel OFF only: write cached timing into registers that are still at reset value (0).
// Never overwrites a value the firmware already programmed. Call before _INT_EdpPanelPrepare.
EFI_STATUS _INT_EdpRestorePps(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                              const _INT_EdpPps* Cached, _INT_Rep* R);

// Drop the driver-level (PWR_WELL_CTL2) requests for PW1 / DDI_A_E that the loader made for its
// AUX and panel work. The firmware boots with them clear (the BIOS register holds the wells), so
// this hands the iGPU to the OS driver in the state the firmware left it in.
EFI_STATUS _INT_EdpReleaseWells(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle, _INT_Rep* R);

// Built-in VBT (lib/vbt_base.c) -> OpRegion mailbox 4.
extern const UINT8 _INT_VbtBase[];
extern const UINTN _INT_VbtBaseSize;
EFI_STATUS _INT_InjectVbtBuiltin(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle, _INT_Rep* R);

// Apply a live-probed link (rate / lanes / PSR) to the VBT in mailbox 4.
EFI_STATUS _INT_VbtApplyDpcd(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                             const _INT_EdpCaps* C, _INT_Rep* R);
// Same as _INT_VbtApplyFirmwareEdid but with a caller-supplied EDID.
EFI_STATUS _INT_VbtApplyEdidBuf(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
                                const UINT8* Edid, UINTN Size, const char* Src, _INT_Rep* R);
// Wipe mailbox 4 again (restores the empty firmware state).
EFI_STATUS _INT_VbtClearMailbox(EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle, _INT_Rep* R);

#endif