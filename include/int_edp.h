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
