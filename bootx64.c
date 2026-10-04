#include <efi.h>
#include "include/int_graphics.h"
#include "include/int_print.h"
#include "include/int_event.h"
#include "include/int_guid.h"
#include "include/int_dpath.h"
#include "include/pci_db.h"
#include "include/int_vbt.h"
#include "include/int_mem.h"


#define APPLE_SET_OS_VENDOR  "Apple Inc."
#define APPLE_SET_OS_VERSION "Mac OS X 10.15"

// T2 MMIO gmux (same base as Linux apple-gmux / Windows GmuxDriver)
// Protocol: port select at +0x0E, command at +0x0F, data at +0x00
// Write: data -> select -> command 0x41; poll until command == 0
#define GMUX_PHYS_BASE            0xFE0B0200ULL
#define GMUX_OFF_DATA             0x00
#define GMUX_OFF_PORT_SELECT      0x0E
#define GMUX_OFF_COMMAND          0x0F
#define GMUX_PORT_SWITCH_DISPLAY  0x10
#define GMUX_PORT_INTERRUPT_ENABLE 0x14
#define GMUX_PORT_INTERRUPT_STATUS 0x16
#define GMUX_PORT_SWITCH_DDC      0x28
#define GMUX_PORT_SWITCH_EXTERNAL 0x40
#define GMUX_ROUTE_IGD            2
#define GMUX_ROUTE_DGPU           3
#define GMUX_DDC_IGD              1
#define GMUX_DDC_DGPU             2
#define GMUX_PORT_DISCRETE_POWER  0x50
#define GMUX_INTERRUPT_ENABLE_ALL 0xFF
#define GMUX_INTERRUPT_STATUS_POWER 0x04   // set by gmux when a rail change completed

#pragma pack(1)
typedef struct {
  UINT8   Desc;
  UINT16  Len;
  UINT8   ResType;
  UINT8   GenFlag;
  UINT8   SpecificFlag;
  UINT64  AddrSpaceGranularity;
  UINT64  AddrRangeMin;
  UINT64  AddrRangeMax;
  UINT64  AddrTranslationOffset;
  UINT64  AddrLen;
} EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR;

typedef struct {
   UINT16  VendorId;
   UINT16  DeviceId;
   UINT16  Command;
   UINT16  Status;
   UINT8   RevisionId;
   UINT8   ClassCode[3];
   UINT8   CacheLineSize;
   UINT8   PrimaryLatencyTimer;
   UINT8   HeaderType;
   UINT8   Bist;
} PCI_COMMON_HEADER;
 
typedef struct {
   UINT32  Bar[6];               // Base Address Registers
   UINT32  CardBusCISPtr;        // CardBus CIS Pointer
   UINT16  SubVendorId;          // Subsystem Vendor ID
   UINT16  SubSystemId;          // Subsystem ID
   UINT32  ROMBar;               // Expansion ROM Base Address
   UINT8   CapabilitiesPtr;      // Capabilities Pointer
   UINT8   Reserved[3];
   UINT32  Reserved1;
   UINT8   InterruptLine;        // Interrupt Line
   UINT8   InterruptPin;         // Interrupt Pin
   UINT8   MinGnt;               // Min_Gnt
   UINT8   MaxLat;               // Max_Lat
} PCI_DEVICE_HEADER;
 
typedef struct {
   UINT32  CardBusSocketReg;     // Cardus Socket/ExCA Base Address Register
   UINT8   CapabilitiesPtr;      // 14h in pci-cardbus bridge.
   UINT8   Reserved;
   UINT16  SecondaryStatus;      // Secondary Status
   UINT8   PciBusNumber;         // PCI Bus Number
   UINT8   CardBusBusNumber;     // CardBus Bus Number
   UINT8   SubordinateBusNumber; // Subordinate Bus Number
   UINT8   CardBusLatencyTimer;  // CardBus Latency Timer
   UINT32  MemoryBase0;          // Memory Base Register 0
   UINT32  MemoryLimit0;         // Memory Limit Register 0
   UINT32  MemoryBase1;
   UINT32  MemoryLimit1;
   UINT32  IoBase0;
   UINT32  IoLimit0;             // I/O Base Register 0
   UINT32  IoBase1;              // I/O Limit Register 0
   UINT32  IoLimit1;
   UINT8   InterruptLine;        // Interrupt Line
   UINT8   InterruptPin;         // Interrupt Pin
   UINT16  BridgeControl;        // Bridge Control
} PCI_CARDBUS_HEADER;
 
typedef union {
   PCI_DEVICE_HEADER   Device;
   PCI_CARDBUS_HEADER  CardBus;
} NON_COMMON_UNION;
 
typedef struct {
   PCI_COMMON_HEADER Common;
   NON_COMMON_UNION  NonCommon;
   UINT32            Data[48];
} PCI_CONFIG_SPACE;
#pragma pack(0)


// ---- T2 gmux helpers (Boot Services, physical MMIO identity-mapped on x86_64) ----

static BOOLEAN
GmuxPollClear(volatile UINT8 *Base, EFI_BOOT_SERVICES *BS, UINT32 TimeoutMs)
{
    UINT32 waited = 0;
    while (waited < TimeoutMs) {
        if (Base[GMUX_OFF_COMMAND] == 0)
            return TRUE;
        BS->Stall(1000); // 1 ms
        waited++;
    }
    return FALSE;
}

static BOOLEAN
GmuxWrite8(volatile UINT8 *Base, EFI_BOOT_SERVICES *BS, UINT8 Port, UINT8 Value)
{
    if (!GmuxPollClear(Base, BS, 200))
        return FALSE;
    Base[GMUX_OFF_DATA] = Value;
    Base[GMUX_OFF_PORT_SELECT] = Port;
    Base[GMUX_OFF_COMMAND] = 0x41; // write, len=1
    return GmuxPollClear(Base, BS, 200);
}

static BOOLEAN
GmuxRead8(volatile UINT8 *Base, EFI_BOOT_SERVICES *BS, UINT8 Port, UINT8 *Value)
{
    if (!GmuxPollClear(Base, BS, 200))
        return FALSE;
    Base[GMUX_OFF_PORT_SELECT] = Port;
    Base[GMUX_OFF_COMMAND] = 0x01; // read, len=1
    if (!GmuxPollClear(Base, BS, 200))
        return FALSE;
    *Value = Base[GMUX_OFF_DATA];
    return TRUE;
}

// Detect: command register is not 0xFF on a live gmux (same as Linux).
static BOOLEAN
GmuxDetect(volatile UINT8 *Base)
{
    return Base[GMUX_OFF_COMMAND] != 0xFF;
}

// ---- Interrupt mask / status (Linux gmux_enable/disable/clear_interrupts) ----
// Linux runs with the mask at 0xFF, waits for GMUX_INTERRUPT_STATUS_POWER after a
// rail change and clears status by writing the value back. We do the same around
// the switch, then clear what we latched and restore the mask we found, so the
// Windows gmux driver does not inherit a stale pending event. (Linux also calls
// ACPI GMSP(0) on MMIO gmux to stop a status=0 flood; that needs the ACPI
// interpreter and cannot be done from Boot Services.)
typedef struct {
    UINT8   Mask;
    BOOLEAN MaskValid;
} GMUX_IRQ_SAVE;

static UINT8
GmuxClearStatus(volatile UINT8 *Base, EFI_BOOT_SERVICES *BS)
{
    UINT8 st = 0;
    if (!GmuxRead8(Base, BS, GMUX_PORT_INTERRUPT_STATUS, &st))
        return 0;
    GmuxWrite8(Base, BS, GMUX_PORT_INTERRUPT_STATUS, st); // write back = clear
    return st;
}

static void
GmuxIrqBegin(EFI_BOOT_SERVICES *BS, GMUX_IRQ_SAVE *Save)
{
    volatile UINT8 *Base = (volatile UINT8 *)(UINTN)GMUX_PHYS_BASE;

    Save->Mask = 0;
    Save->MaskValid = FALSE;
    if (!GmuxDetect(Base))
        return;
    Save->MaskValid = GmuxRead8(Base, BS, GMUX_PORT_INTERRUPT_ENABLE, &Save->Mask);
    GmuxWrite8(Base, BS, GMUX_PORT_INTERRUPT_ENABLE, GMUX_INTERRUPT_ENABLE_ALL);
    GmuxClearStatus(Base, BS); // drop stale bits so only our own events are seen
}

// Returns the status bits that were still latched before clearing.
static UINT8
GmuxIrqEnd(EFI_BOOT_SERVICES *BS, const GMUX_IRQ_SAVE *Save)
{
    volatile UINT8 *Base = (volatile UINT8 *)(UINTN)GMUX_PHYS_BASE;
    UINT8 st;

    if (!GmuxDetect(Base))
        return 0;
    st = GmuxClearStatus(Base, BS);
    if (Save->MaskValid)
        GmuxWrite8(Base, BS, GMUX_PORT_INTERRUPT_ENABLE, Save->Mask);
    return st;
}

// Panel routing readback. Linux gmux_read_switch_state(): bit 0 of port 0x10,
// 0 = iGPU, 1 = dGPU. Only this port decides whether the panel is on the iGPU;
// DDC/EXTERNAL are written like Linux but not used as a gate (DDC may be a no-op
// on T2, EXTERNAL is forced to dGPU on Thunderbolt Macs).
static BOOLEAN
GmuxReadPanelIsIGD(EFI_BOOT_SERVICES *BS)
{
    volatile UINT8 *Base = (volatile UINT8 *)(UINTN)GMUX_PHYS_BASE;
    UINT8 val = 0;

    if (!GmuxDetect(Base))
        return FALSE;
    if (!GmuxRead8(Base, BS, GMUX_PORT_SWITCH_DISPLAY, &val))
        return FALSE;
    return (val & 1) == 0;
}

// Route panel to iGPU. Same three writes and order as Linux
// gmux_write_switch_state() for switch_state_* = IGD, with EXTERNAL kept on the
// dGPU (Linux: external_switchable == false when Thunderbolt is present).
// Linux issues the writes once and never checks; here the result is verified by
// readback and retried, because the rail must not be cut unless the panel is
// really on the iGPU.
static BOOLEAN
GmuxSwitchToIGD(EFI_BOOT_SERVICES *BS)
{
    volatile UINT8 *Base = (volatile UINT8 *)(UINTN)GMUX_PHYS_BASE;

    if (!GmuxDetect(Base))
        return FALSE;

    for (UINTN attempt = 0; attempt < 3; attempt++) {
        BOOLEAN ok;

        GmuxWrite8(Base, BS, GMUX_PORT_SWITCH_DDC, GMUX_DDC_IGD);
        ok = GmuxWrite8(Base, BS, GMUX_PORT_SWITCH_DISPLAY, GMUX_ROUTE_IGD);
        GmuxWrite8(Base, BS, GMUX_PORT_SWITCH_EXTERNAL, GMUX_ROUTE_DGPU);

        BS->Stall(10000); // let the mux settle before reading back
        if (ok && GmuxReadPanelIsIGD(BS))
            return TRUE;
    }
    return FALSE;
}

// dGPU power rail (Linux gmux_set_discrete_state / Windows GmuxSetDiscretePower).
// Sequence: write 1, then 3=ON or 0=OFF. Linux then waits for the POWER bit in
// the interrupt status (GPE, 200 ms timeout); we poll the same bit and fall back
// to a fixed settle if it never shows. Do NOT power the card back on from
// Windows after OFF from here - that path hung this machine.
static BOOLEAN
GmuxSetDiscretePower(EFI_BOOT_SERVICES *BS, BOOLEAN PowerOn, BOOLEAN *PowerEvent)
{
    volatile UINT8 *Base = (volatile UINT8 *)(UINTN)GMUX_PHYS_BASE;
    BOOLEAN ok;
    BOOLEAN seen = FALSE;

    if (PowerEvent)
        *PowerEvent = FALSE;
    if (!GmuxDetect(Base))
        return FALSE;

    ok = GmuxWrite8(Base, BS, GMUX_PORT_DISCRETE_POWER, 1);
    ok = (BOOLEAN)(GmuxWrite8(Base, BS, GMUX_PORT_DISCRETE_POWER, PowerOn ? 3 : 0) && ok);

    for (UINT32 waited = 0; waited < 200 && !seen; waited++) {
        UINT8 st = 0;
        if (GmuxRead8(Base, BS, GMUX_PORT_INTERRUPT_STATUS, &st) &&
            (st & GMUX_INTERRUPT_STATUS_POWER)) {
            GmuxWrite8(Base, BS, GMUX_PORT_INTERRUPT_STATUS, st);
            seen = TRUE;
            break;
        }
        BS->Stall(1000);
    }

    BS->Stall(seen ? 20000 : 250000);
    if (PowerEvent)
        *PowerEvent = seen;
    return ok;
}

#ifndef EFI_VARIABLE_NON_VOLATILE
#define EFI_VARIABLE_NON_VOLATILE       0x00000001
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x00000002
#define EFI_VARIABLE_RUNTIME_ACCESS     0x00000004
#endif

// Firmware boot GPU preference (forum "only via EFI" path).
// GUID fa4ce28d-b62f-4c99-9cc3-6815686e30f9, name gpu-power-prefs,
// first data byte 1=iGPU, 0=dGPU. Takes effect next cold boot as well.
static EFI_STATUS
SetGpuPowerPrefsIgd(EFI_RUNTIME_SERVICES *RT)
{
    EFI_GUID Guid = {
        0xfa4ce28d, 0xb62f, 0x4c99,
        { 0x9c, 0xc3, 0x68, 0x15, 0x68, 0x6e, 0x30, 0xf9 }
    };
    UINT8 Data[4] = { 0x01, 0x00, 0x00, 0x00 };
    UINT32 Attr =
        EFI_VARIABLE_NON_VOLATILE |
        EFI_VARIABLE_BOOTSERVICE_ACCESS |
        EFI_VARIABLE_RUNTIME_ACCESS;

    return RT->SetVariable(
        L"gpu-power-prefs",
        &Guid,
        Attr,
        sizeof(Data),
        Data
    );
}

static EFI_STATUS
SetGpuPowerPrefsDgpu(EFI_RUNTIME_SERVICES *RT)
{
    EFI_GUID Guid = {
        0xfa4ce28d, 0xb62f, 0x4c99,
        { 0x9c, 0xc3, 0x68, 0x15, 0x68, 0x6e, 0x30, 0xf9 }
    };
    UINT8 Data[4] = { 0x00, 0x00, 0x00, 0x00 };
    UINT32 Attr =
        EFI_VARIABLE_NON_VOLATILE |
        EFI_VARIABLE_BOOTSERVICE_ACCESS |
        EFI_VARIABLE_RUNTIME_ACCESS;

    return RT->SetVariable(
        L"gpu-power-prefs",
        &Guid,
        Attr,
        sizeof(Data),
        Data
    );
}


// ---- OpRegion / VBT diagnostic dump (keys D / W) ----
// Writes <tag>.txt (readable report), <tag>.bin (raw OpRegion) and
// <tag>_vbt.bin (raw VBT) to the ESP root and shows a short summary.
static VOID
DoVbtDump(EFI_BOOT_SERVICES *BS, EFI_HANDLE Image, _INT_SimpleTextGraphicsStruct *gs,
          CHAR16 *TxtName, CHAR16 *OpName, CHAR16 *VbtName, CHAR16 *Label, UINTN Row)
{
    const UINTN Cap = 49152;
    CHAR8 *Buf = (CHAR8 *)_INT_AllocatePool(BS, Cap);
    _INT_Rep R;
    _INT_VbtInfo Info;
    EFI_STATUS St;

    if (Buf == NULL)
        return;
    _INT_RepInit(&R, Buf, Cap);

    _INT_RepStr(&R, (const CHAR8 *)"=== t2-gmux-uefi OpRegion/VBT dump: ");
    for (UINTN i = 0; Label[i]; i++) {
        CHAR8 t[2] = { (CHAR8)Label[i], 0 };
        _INT_RepStr(&R, t);
    }
    _INT_RepStr(&R, (const CHAR8 *)" ==="); _INT_RepNl(&R);

    // gmux register readback (port: value) so the mux state is part of the report
    {
        volatile UINT8 *Base = (volatile UINT8 *)(UINTN)GMUX_PHYS_BASE;
        static const UINT8 Ports[6] = { GMUX_PORT_SWITCH_DISPLAY, GMUX_PORT_SWITCH_DDC,
                                        GMUX_PORT_SWITCH_EXTERNAL, GMUX_PORT_DISCRETE_POWER,
                                        GMUX_PORT_INTERRUPT_ENABLE, GMUX_PORT_INTERRUPT_STATUS };
        _INT_RepStr(&R, (const CHAR8 *)"gmux readback:");
        if (!GmuxDetect(Base)) {
            _INT_RepStr(&R, (const CHAR8 *)" no gmux");
        } else {
            for (UINTN i = 0; i < 6; i++) {
                UINT8 v = 0;
                _INT_RepStr(&R, (const CHAR8 *)" 0x"); _INT_RepHex(&R, Ports[i], 2);
                _INT_RepStr(&R, (const CHAR8 *)"=");
                if (GmuxRead8(Base, BS, Ports[i], &v)) {
                    _INT_RepHex(&R, v, 2);
                } else {
                    _INT_RepStr(&R, (const CHAR8 *)"??");
                }
            }
        }
        _INT_RepNl(&R);
    }

    St = _INT_InspectIgpuOpRegion(BS, Image, &R, &Info);

    EFI_STATUS S1 = _INT_WriteEspFile(BS, Image, TxtName, Buf, R.len);
    EFI_STATUS S2 = EFI_NOT_FOUND, S3 = EFI_NOT_FOUND;
    if (Info.OpRegion)
        S2 = _INT_WriteEspFile(BS, Image, OpName, Info.OpRegion, Info.OpSize);
    if (Info.Vbt)
        S3 = _INT_WriteEspFile(BS, Image, VbtName, Info.Vbt, Info.VbtSize);

    _INT_SimpleTextGraphicsPrint(gs, 0, Row, TRUE, FALSE,
        L"[%s] iGPU=%s ASLS=%x OpRegion=%s VBT=%s (ver %d, bdb %d)",
        Label, Info.IgpuFound ? L"yes" : L"NO", Info.Asls,
        Info.OpRegionOk ? L"ok" : L"NO", Info.VbtFound ? L"ok" : L"NO",
        (UINTN)Info.VbtVer, (UINTN)Info.BdbVer);
    if (Info.EdpFound) {
        _INT_SimpleTextGraphicsPrint(gs, 0, Row + 1, TRUE, FALSE,
            L"  eDP child: type=%04x dvo_port=%d aux=%02x ddc_pin=%d",
            (UINTN)Info.EdpType, (UINTN)Info.EdpDvoPort, (UINTN)Info.EdpAux, (UINTN)Info.EdpDdc);
    } else {
        _INT_SimpleTextGraphicsPrint(gs, 0, Row + 1, TRUE, FALSE,
            L"  eDP child: NOT FOUND in VBT");
    }
    if (Info.HaveLink) {
        _INT_SimpleTextGraphicsPrint(gs, 0, Row + 2, TRUE, FALSE,
            L"  panel_type=%d link rate code=%d lanes code=%d",
            (UINTN)Info.PanelType, (UINTN)Info.LinkRate, (UINTN)Info.LinkLanes);
    } else {
        _INT_SimpleTextGraphicsPrint(gs, 0, Row + 2, TRUE, FALSE,
            L"  panel_type=%d, no eDP link params", (UINTN)Info.PanelType);
    }
    _INT_SimpleTextGraphicsPrint(gs, 0, Row + 3, TRUE, TRUE,
        L"  files: txt=%s bin=%s vbt=%s (inspect=%lX)",
        EFI_ERROR(S1) ? L"FAIL" : L"ok",
        EFI_ERROR(S2) ? L"-" : L"ok",
        EFI_ERROR(S3) ? L"-" : L"ok", St);

    _INT_FreePool(BS, Buf);
}

VOID PrintGpu(EFI_BOOT_SERVICES* BS, _INT_SimpleTextGraphicsStruct* gs, EFI_HANDLE ImageHandle)
{
    EFI_STATUS Status;

    EFI_GUID efi_pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;

    EFI_HANDLE* PciIoHandleBuf;
    UINTN PciIoHandleCount = 0;

    Status = BS->LocateHandleBuffer(
        ByProtocol, 
        &efi_pci_io_guid, 
        NULL, 
        &PciIoHandleCount,
        &PciIoHandleBuf
    );


    if (EFI_ERROR(Status)) {
        _INT_SimpleTextGraphicsPrint(
            gs, 0, 10, FALSE, TRUE,
            L"PciIo Buffer Error: %lX", Status
        );
    } else if (PciIoHandleCount == 0) {
        _INT_SimpleTextGraphicsPrint(
            gs, 0, 10, FALSE, TRUE,
            L"No PciIo Handles"
        );
    } else {
        
        UINT16 NumOfGpu = 0;

        for (UINTN i = 0; i < PciIoHandleCount; i++) {
            EFI_PCI_IO_PROTOCOL* PciIo;

            Status = BS->OpenProtocol(
                PciIoHandleBuf[i], 
                &efi_pci_io_guid, 
                (VOID**)&PciIo, 
                ImageHandle, 
                NULL, 
                EFI_OPEN_PROTOCOL_GET_PROTOCOL
            );

            if (!EFI_ERROR(Status)) {
                PCI_COMMON_HEADER PciHeader;

                PciIo->Pci.Read(
                    PciIo, 
                    EfiPciIoWidthUint16, 
                    0, 
                    1,
                    &PciHeader.VendorId
                );

                if (PciHeader.VendorId != 0xffff) {
                    PciIo->Pci.Read(
                        PciIo, 
                        EfiPciIoWidthUint32, 
                        0, 
                        sizeof(PciHeader) / sizeof(UINT32),
                        &PciHeader
                    );

                    if (PciHeader.ClassCode[2] == 3) {
                        CHAR16* VendorStr = L"Unknown";
                        CHAR16* DeviceStr = L"Unknown";

                        for (UINT16 vendorIdx = 0; vendorIdx < pci_vendor_db_size; vendorIdx++) {
                            if (pci_vendor_db[vendorIdx]->vendorId == PciHeader.VendorId) {
                                VendorStr = pci_vendor_db[vendorIdx]->vendorName;

                                // binary search
                                UINT16 minNumOfDevices = 0;
                                UINT16 maxNumOfDevices = pci_vendor_db[vendorIdx]->numOfDevices - 1;
        
                                while (maxNumOfDevices >= minNumOfDevices) {
                                    UINT32 midNumOfDevices = (maxNumOfDevices + minNumOfDevices) / 2;

                                    if (PciHeader.DeviceId > pci_vendor_db[vendorIdx]->devices[midNumOfDevices].deviceId) {
                                        minNumOfDevices = midNumOfDevices + 1;
                                    } else if (PciHeader.DeviceId < pci_vendor_db[vendorIdx]->devices[midNumOfDevices].deviceId) {
                                        maxNumOfDevices = midNumOfDevices - 1 ;
                                    } else {
                                        DeviceStr = pci_vendor_db[vendorIdx]->devices[midNumOfDevices].deviceName;
                                        break;
                                    }
                                }

                                break;
                            }
                        }

                        _INT_SimpleTextGraphicsPrint(
                            gs, 0, 10 + NumOfGpu, TRUE, FALSE,
                            L"%04x %04x %s - %s", PciHeader.VendorId, PciHeader.DeviceId, VendorStr, DeviceStr
                        );

                        NumOfGpu++;
                    }
                }
            }
        }

        for (int clearIdx = 0; clearIdx < 4; clearIdx++) {
            _INT_SimpleTextGraphicsPrint(
                gs, 0, 10 + NumOfGpu + clearIdx, TRUE, FALSE,
                L" "
            );
        }
        _INT_SimpleTextGraphicsRefresh(gs);
    }

    _INT_FreePool(BS, PciIoHandleBuf);
}


EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable)
{
    EFI_STATUS Status;

    EFI_BOOT_SERVICES* BS = SystemTable->BootServices;
    SIMPLE_TEXT_OUTPUT_INTERFACE* ConOut = SystemTable->ConOut;
    SIMPLE_INPUT_INTERFACE* ConIn = SystemTable->ConIn;

    _INT_SetGraphicsMode(BS, FALSE);


    _INT_SimpleTextGraphicsStruct gs;
    _INT_memset(&gs, 0, sizeof(_INT_SimpleTextGraphicsStruct));
    gs.BS = BS;
    gs.ConOut = ConOut;

    _INT_SimpleTextGraphicsInit(&gs);

    _INT_SimpleTextGraphicsPrint(
        &gs, 0, 0, FALSE, FALSE,
        L"================== apple_set_os loader v0.5 =================="
    );
    _INT_SimpleTextGraphicsPrint(
        &gs, 0, 1, FALSE, FALSE,
        L"Initializing AppleSetOsProtocol"
    );

    _INT_SimpleTextGraphicsPrint(
        &gs, 0, 9, FALSE, TRUE,
        L"Connected Graphics Cards:"
    );
    //update gpu info
    PrintGpu(BS, &gs, ImageHandle);



    // get apple_set_os protocol
    EFI_HANDLE* AppleSetOsHandleBuf;
    UINTN AppleSetOsHandleCount = 0;

    EFI_GUID apple_set_os_guid = APPLE_SET_OS_GUID;
    Status = BS->LocateHandleBuffer(
        ByProtocol, 
        &apple_set_os_guid, 
        NULL, 
        &AppleSetOsHandleCount,
        &AppleSetOsHandleBuf
    );
    
    if (EFI_ERROR(Status)) {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 1, TRUE, TRUE,
            L"AppleSetOsProtocol Buffer Error: %lX", Status
        );
    } else if (AppleSetOsHandleCount == 0) {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 1, TRUE, TRUE,
            L"No SetOsProtocol Handles"
        );
    } else {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 1, TRUE, TRUE,
            L"SetOsProtocol Handle Count: %d", (UINTN)AppleSetOsHandleCount
        );
    }

    // Keys (after countdown):
    //   Z = skip AppleSetOs
    //   X = gmux panel->iGPU + dGPU rail OFF (no NVRAM)
    //   V = gmux panel->iGPU only (Radeon stays powered, no NVRAM)
    //   C = rail OFF only (mux not changed)
    //   R = NVRAM gpu-power-prefs = dGPU only
    //   E = NVRAM gpu-power-prefs = iGPU only
    //   I = X + inject t2gmux_vbt.bin + dump;  U = V (Radeon stays powered) + inject + dump
    // Default (no key): no mux change, no rail change, no NVRAM
    BOOLEAN DoGmuxSwitch = FALSE;
    BOOLEAN DoDgpuPowerOff = FALSE;
    BOOLEAN DoWritePrefsIgd = FALSE;
    BOOLEAN DoWritePrefsDgpu = FALSE;
    BOOLEAN DoDump = FALSE;
    BOOLEAN DoInject = FALSE;

    if (AppleSetOsHandleCount == 0) {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 2, TRUE, TRUE,
            L"AppleSetOs will not be loaded."
        );
    } else {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 2, TRUE, TRUE,
            L"AppleSetOs ON (Z=skip). Default=no gmux/rail"
        );
    }
    _INT_SimpleTextGraphicsPrint(
        &gs, 0, 7, FALSE, TRUE,
        L"X=mux+off V=mux C=rail R/E=NVRAM D=X+dump W=dump I=X+inject U=V+inject"
    );


    // find and load bootx64_original.efi
    EFI_LOADED_IMAGE_PROTOCOL* LoadedImage;
	EFI_DEVICE_PATH* DevicePath = NULL;
	EFI_HANDLE DriverHandle;

    _INT_SimpleTextGraphicsPrint(
        &gs, 0, 3, FALSE, TRUE,
        L"Initializing LoadedImageProtocol..."
    );
    EFI_GUID efi_loaded_image_protocol_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    Status = BS->HandleProtocol(ImageHandle, &efi_loaded_image_protocol_guid, (VOID**) &LoadedImage);
    if (EFI_ERROR(Status) || LoadedImage == NULL) {
        goto halt;
    }

    _INT_SimpleTextGraphicsPrint(
        &gs, 0, 3, TRUE, TRUE,
        L"Locating bootx64_original.efi..."
    );
    DevicePath = _INT_FileDevicePath(
        BS, 
        LoadedImage->DeviceHandle, 
        L"\\EFI\\Boot\\bootx64_original.efi"
    );

    if (DevicePath == NULL) {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 3, TRUE, TRUE,
            L"Unable to find bootx64_original.efi"
        );
        goto halt;
	}

    _INT_SimpleTextGraphicsPrint(
        &gs, 0, 3, TRUE, TRUE,
            L"Loading bootx64_original.efi to memory..."
    );
    // Attempt to load the driver.
	Status = BS->LoadImage(FALSE, ImageHandle, DevicePath, NULL, 0, &DriverHandle);
    _INT_FreePool(BS, DevicePath);
    DevicePath = NULL;

	if (EFI_ERROR(Status)) {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 3, TRUE, TRUE,
            L"Unable to load bootx64_original.efi to memory"
        );
		goto halt;
	}

    _INT_SimpleTextGraphicsPrint(
        &gs, 0, 3, TRUE, TRUE,
        L"Prepare to run bootx64_original.efi..."
    );

	Status = BS->OpenProtocol(
        DriverHandle, 
        &efi_loaded_image_protocol_guid,
		(VOID**)&LoadedImage, 
        ImageHandle, 
        NULL, 
        EFI_OPEN_PROTOCOL_GET_PROTOCOL
    );
	if (EFI_ERROR(Status)) {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 3, TRUE, TRUE,
            L"Failed to run bootx64_original.efi"
        );
		goto halt;
	}

    _INT_SimpleTextGraphicsPrint(
        &gs, 0, 3, TRUE, TRUE,
        L" "
    );

    _INT_SimpleTextGraphicsPrint(
        &gs, 0, 4, FALSE, TRUE,
        L"----------------------- Ready to boot ------------------------"
    );
    _INT_SimpleTextGraphicsPrint(
        &gs, 0, 5, FALSE, TRUE,
        L"Plug in your eGPU then press any key."
    );
    


    EFI_INPUT_KEY Key;
    Key.UnicodeChar = 0;
    Key.ScanCode = 0;

    for (UINT8 i = 6; i > 0; i--) {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 6, TRUE, TRUE,
            L"Booting bootx64_original.efi in %u second(s)", (UINT32)i
        );

        UINT16 MaxCycle = 20;

        if (i % 3 == 1) {
            MaxCycle = 18;
            PrintGpu(BS, &gs, ImageHandle);
        }

        // refresh screen when idle
        for (UINT16 j = 0; j < MaxCycle; j++) {
            // each cycle is about 50ms 
            _INT_WaitForSingleEvent(BS, ConIn->WaitForKey, 450000);
            if (!EFI_ERROR(ConIn->ReadKeyStroke(ConIn, &Key))) {
                // break loop
                i = 1; j = 20;
            }
            _INT_SimpleTextGraphicsRefresh(&gs);
        }
    }

    // Z / X / V / C / R / E — see UI comments above
    if (Key.UnicodeChar == L'z' || Key.UnicodeChar == L'Z') {
        AppleSetOsHandleCount = 0;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 2, TRUE, TRUE,
            L"AppleSetOs will not be loaded."
        );
    }
    if (Key.UnicodeChar == L'x' || Key.UnicodeChar == L'X') {
        DoGmuxSwitch = TRUE;
        DoDgpuPowerOff = TRUE;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"X: gmux iGPU + dGPU rail OFF (no NVRAM)"
        );
    }
    if (Key.UnicodeChar == L'v' || Key.UnicodeChar == L'V') {
        DoGmuxSwitch = TRUE;
        DoDgpuPowerOff = FALSE;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"V: panel->iGPU only (Radeon powered, no NVRAM)"
        );
    }
    if (Key.UnicodeChar == L'c' || Key.UnicodeChar == L'C') {
        // mux not changed; only rail OFF
        DoGmuxSwitch = FALSE;
        DoDgpuPowerOff = TRUE;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"C: dGPU rail OFF only (mux unchanged)"
        );
    }
    if (Key.UnicodeChar == L'r' || Key.UnicodeChar == L'R') {
        // NVRAM only — dGPU; mux/rail left at default (off)
        DoWritePrefsDgpu = TRUE;
        DoWritePrefsIgd = FALSE;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"R: NVRAM gpu-power-prefs = dGPU only"
        );
    }
    if (Key.UnicodeChar == L'e' || Key.UnicodeChar == L'E') {
        // NVRAM only — iGPU; mux/rail left at default (off)
        DoWritePrefsIgd = TRUE;
        DoWritePrefsDgpu = FALSE;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"E: NVRAM gpu-power-prefs = iGPU only"
        );
    }

    if (Key.UnicodeChar == L'd' || Key.UnicodeChar == L'D') {
        // same actions as X, plus OpRegion/VBT dump before/after
        DoGmuxSwitch = TRUE;
        DoDgpuPowerOff = TRUE;
        DoDump = TRUE;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"D: X (mux+rail OFF) + OpRegion/VBT dump"
        );
    }
    if (Key.UnicodeChar == L'w' || Key.UnicodeChar == L'W') {
        // dump only, no gmux / rail / NVRAM changes
        DoDump = TRUE;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"W: OpRegion/VBT dump only (no gmux changes)"
        );
    }

    if (Key.UnicodeChar == L'i' || Key.UnicodeChar == L'I') {
        // X actions + write \t2gmux_vbt.bin from the ESP into OpRegion mailbox 4
        DoGmuxSwitch = TRUE;
        DoDgpuPowerOff = TRUE;
        DoDump = TRUE;
        DoInject = TRUE;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"I: X (mux+rail OFF) + inject t2gmux_vbt.bin + dump"
        );
    }
    if (Key.UnicodeChar == L'u' || Key.UnicodeChar == L'U') {
        // V actions (mux -> iGPU, Radeon rail stays ON) + same VBT inject + dump as I
        DoGmuxSwitch = TRUE;
        DoDgpuPowerOff = FALSE;
        DoDump = TRUE;
        DoInject = TRUE;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"U: V (mux, Radeon stays ON) + inject t2gmux_vbt.bin + dump"
        );
    }

    // Must come after every key that can set DoDump (D, W, I, U), otherwise I/U
    // never write the "before" report.
    if (DoDump) {
        DoVbtDump(BS, ImageHandle, &gs, L"\\t2gmux_before.txt", L"\\t2gmux_before_opregion.bin",
                  L"\\t2gmux_before_vbt.bin", L"before", 11);
    }

    // load apple_set_os
    for(UINTN i = 0; i < AppleSetOsHandleCount; i++) {
        EFI_APPLE_SET_OS_IFACE* SetOsIface = NULL;

        Status = BS->OpenProtocol(
            AppleSetOsHandleBuf[i],
            &apple_set_os_guid,
            (VOID**)&SetOsIface,
            ImageHandle,
            NULL,
            EFI_OPEN_PROTOCOL_GET_PROTOCOL
        );

        if (EFI_ERROR(Status)) {
            _INT_SimpleTextGraphicsPrint(
                &gs, 0, 1, TRUE, TRUE,
                L"SetOsProtocol Error: %lX", Status
            );
        } else {
            if (SetOsIface->Version != 0){
                _INT_SimpleTextGraphicsPrint(
                    &gs, 0, 1, TRUE, TRUE,
                    L"Setting OsVendor"
                );
                Status = SetOsIface->SetOsVendor((CHAR8*) APPLE_SET_OS_VENDOR);
                if (EFI_ERROR(Status)){
                    _INT_SimpleTextGraphicsPrint(
                        &gs, 0, 1, TRUE, TRUE,
                        L"OsVendor Error: %lX", Status
                    );
                }

                _INT_SimpleTextGraphicsPrint(
                    &gs, 0, 2, TRUE, TRUE,
                    L"Setting OsVersion"
                );
                Status = SetOsIface->SetOsVersion((CHAR8*) APPLE_SET_OS_VERSION);
                if (EFI_ERROR(Status)){
                    _INT_SimpleTextGraphicsPrint(
                        &gs, 0, 2, TRUE, TRUE,
                        L"OsVersion Error: %lX", Status
                    );
                }
            }
        }
    }

    _INT_FreePool(BS, AppleSetOsHandleBuf);

    // ---- Optional gmux panel switch and/or dGPU rail OFF ----
    // Never power the dGPU rail back on from Windows after OFF (known hang on this HW).
    BOOLEAN MuxOk = TRUE;
    GMUX_IRQ_SAVE IrqSave;
    BOOLEAN IrqActive = (BOOLEAN)(DoGmuxSwitch || DoDgpuPowerOff);

    if (IrqActive)
        GmuxIrqBegin(BS, &IrqSave);

    if (DoGmuxSwitch) {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"Gmux: panel -> iGPU..."
        );
        MuxOk = GmuxSwitchToIGD(BS);
        if (MuxOk) {
            _INT_SimpleTextGraphicsPrint(
                &gs, 0, 7, TRUE, TRUE,
                L"Gmux: route OK, readback=iGPU"
            );
        } else {
            _INT_SimpleTextGraphicsPrint(
                &gs, 0, 7, TRUE, TRUE,
                L"Gmux: route FAILED (no gmux or readback != iGPU)"
            );
        }
    }

    if (DoDgpuPowerOff && DoGmuxSwitch && !MuxOk) {
        // Cutting the rail while the panel is still routed to the Radeon blacks the screen.
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 8, TRUE, TRUE,
            L"Gmux: dGPU rail OFF SKIPPED (panel not confirmed on iGPU)"
        );
    } else if (DoDgpuPowerOff) {
        BOOLEAN PowerEvent = FALSE;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 8, TRUE, TRUE,
            L"Gmux: powering OFF dGPU rail (0x50)..."
        );
        if (GmuxSetDiscretePower(BS, FALSE, &PowerEvent)) {
            _INT_SimpleTextGraphicsPrint(
                &gs, 0, 8, TRUE, TRUE,
                L"Gmux: dGPU rail OFF OK (power event %s)",
                PowerEvent ? L"seen" : L"not seen, fixed delay"
            );
        } else {
            _INT_SimpleTextGraphicsPrint(
                &gs, 0, 8, TRUE, TRUE,
                L"Gmux: dGPU rail OFF FAILED"
            );
        }
    }

    if (IrqActive) {
        UINT8 Left = GmuxIrqEnd(BS, &IrqSave);
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 9, TRUE, TRUE,
            L"Gmux: irq status cleared (was %x), mask restored=%s",
            (UINTN)Left, IrqSave.MaskValid ? L"yes" : L"no"
        );
    }

    if (DoGmuxSwitch || DoDgpuPowerOff) {
        for (UINT16 j = 0; j < 80; j++) {
            BS->Stall(10000);
        }
    }

    // NVRAM writes are independent of gmux (keys R / E)
    if (DoWritePrefsIgd) {
        EFI_STATUS st = SetGpuPowerPrefsIgd(SystemTable->RuntimeServices);
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 9, TRUE, TRUE,
            L"NVRAM gpu-power-prefs=iGPU: %s (%lX)",
            EFI_ERROR(st) ? L"FAIL" : L"OK",
            st
        );
        for (UINT16 j = 0; j < 50; j++) {
            BS->Stall(10000);
        }
    } else if (DoWritePrefsDgpu) {
        EFI_STATUS st = SetGpuPowerPrefsDgpu(SystemTable->RuntimeServices);
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 9, TRUE, TRUE,
            L"NVRAM gpu-power-prefs=dGPU: %s (%lX)",
            EFI_ERROR(st) ? L"FAIL" : L"OK",
            st
        );
        for (UINT16 j = 0; j < 50; j++) {
            BS->Stall(10000);
        }
    }

    if (DoInject) {
        const UINTN RCap = 4096;
        CHAR8 *RBuf = (CHAR8 *)_INT_AllocatePool(BS, RCap);
        EFI_STATUS IS = EFI_OUT_OF_RESOURCES;
        EFI_STATUS LS = EFI_OUT_OF_RESOURCES;
        if (RBuf != NULL) {
            _INT_Rep IR;
            _INT_RepInit(&IR, RBuf, RCap);
            IS = _INT_InjectVbt(BS, ImageHandle, L"\\t2gmux_vbt.bin", &IR);
            // Same iGPU-side setup the firmware does when it boots from the iGPU
            // (see _INT_IgpuForceDdiA4Lanes); the mux alone does not provide it.
            LS = _INT_IgpuForceDdiA4Lanes(BS, ImageHandle, &IR);
            _INT_WriteEspFile(BS, ImageHandle, L"\\t2gmux_inject.txt", RBuf, IR.len);
            _INT_FreePool(BS, RBuf);
        }
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 19, TRUE, TRUE,
            L"VBT inject: %s (%lX)", EFI_ERROR(IS) ? L"FAILED" : L"OK", IS
        );
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 20, TRUE, TRUE,
            L"DDI A 4 lanes: %s (%lX)", EFI_ERROR(LS) ? L"FAILED" : L"OK", LS
        );
    }

    if (DoDump) {
        DoVbtDump(BS, ImageHandle, &gs, L"\\t2gmux_after.txt", L"\\t2gmux_after_opregion.bin",
                  L"\\t2gmux_after_vbt.bin", L"after", 15);
        for (UINT16 j = 0; j < 800; j++) {
            BS->Stall(10000);   // ~8 s to read the summary
        }
    }

    _INT_SimpleTextGraphicsPrint(
        &gs, 0, 6, TRUE, TRUE,
        L"Booting bootx64_original.efi..."
    );

    // short delay
    for (UINT16 j = 0; j < 150; j++) {
        BS->Stall(10000);
    }

    ConOut->ClearScreen(ConOut);
    _INT_SetGraphicsMode(BS, TRUE);

    // Load was a success - attempt to start the driver
    Status = BS->StartImage(DriverHandle, NULL, NULL);
    if (EFI_ERROR(Status)) {
        _INT_SetGraphicsMode(BS, FALSE);
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 6, TRUE, TRUE,
            L"Unable to boot bootx64_original.efi"
        );
        goto halt;
    }

    BS->Exit(ImageHandle, EFI_UNSUPPORTED, 0, NULL);
    return EFI_UNSUPPORTED;

halt:
    while (1) { 
        PrintGpu(BS, &gs, ImageHandle);
        for (UINT16 j = 0; j < 4000; j++) {
            BS->Stall(10000);
        }
    }

    BS->Exit(ImageHandle, EFI_UNSUPPORTED, 0, NULL);

    return EFI_UNSUPPORTED;
}