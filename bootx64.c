#include <efi.h>
#include "include/int_graphics.h"
#include "include/int_print.h"
#include "include/int_event.h"
#include "include/int_guid.h"
#include "include/int_dpath.h"
#include "include/pci_db.h"


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
#define GMUX_PORT_SWITCH_DDC      0x28
#define GMUX_PORT_SWITCH_EXTERNAL 0x40
#define GMUX_ROUTE_IGD            2
#define GMUX_ROUTE_DGPU           3
#define GMUX_DDC_IGD              1
#define GMUX_DDC_DGPU             2
#define GMUX_PORT_DISCRETE_POWER  0x50

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

// Route panel to iGPU (Linux force_igd analogue at UEFI time).
// DDC + panel + external (external forced to dGPU on TB Macs, like Linux).
static BOOLEAN
GmuxSwitchToIGD(EFI_BOOT_SERVICES *BS)
{
    volatile UINT8 *Base = (volatile UINT8 *)(UINTN)GMUX_PHYS_BASE;
    BOOLEAN ok;

    if (!GmuxDetect(Base))
        return FALSE;

    // DDC -> iGPU (may be no-op on T2/retina, still written like Linux)
    GmuxWrite8(Base, BS, GMUX_PORT_SWITCH_DDC, GMUX_DDC_IGD);

    // Panel -> iGPU
    ok = GmuxWrite8(Base, BS, GMUX_PORT_SWITCH_DISPLAY, GMUX_ROUTE_IGD);

    // External AUX stays on dGPU (Thunderbolt Mac policy)
    GmuxWrite8(Base, BS, GMUX_PORT_SWITCH_EXTERNAL, GMUX_ROUTE_DGPU);

    return ok;
}

static BOOLEAN
GmuxReadPanelIsIGD(EFI_BOOT_SERVICES *BS)
{
    volatile UINT8 *Base = (volatile UINT8 *)(UINTN)GMUX_PHYS_BASE;
    UINT8 val = 0;

    if (!GmuxDetect(Base))
        return FALSE;
    if (!GmuxRead8(Base, BS, GMUX_PORT_SWITCH_DISPLAY, &val))
        return FALSE;
    // Linux/Windows: bit 0 of port 0x10: 0 = iGPU, 1 = dGPU
    return (val & 1) == 0;
}

// dGPU power rail (Linux gmux_set_discrete_state / Windows GmuxSetDiscretePower).
// Sequence: write 1, then 3=ON or 0=OFF, then settle. Do NOT power the card
// back on from Windows after OFF from here - that path hung this machine.
static BOOLEAN
GmuxSetDiscretePower(EFI_BOOT_SERVICES *BS, BOOLEAN PowerOn)
{
    volatile UINT8 *Base = (volatile UINT8 *)(UINTN)GMUX_PHYS_BASE;
    BOOLEAN ok;

    if (!GmuxDetect(Base))
        return FALSE;

    ok = GmuxWrite8(Base, BS, GMUX_PORT_DISCRETE_POWER, 1);
    ok = (BOOLEAN)(GmuxWrite8(Base, BS, GMUX_PORT_DISCRETE_POWER, PowerOn ? 3 : 0) && ok);
    // ~250 ms settle (Linux uses GPE when available; fixed delay otherwise)
    BS->Stall(250000);
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

    // Gmux: panel -> iGPU, optionally power off dGPU + set gpu-power-prefs.
    // X = skip all gmux / power / NVRAM
    // R = panel switch only (keep Radeon powered, no NVRAM write)
    // D = write gpu-power-prefs=0 (dGPU) for recovery, then still can skip with X next boot
    BOOLEAN DoGmuxSwitch = TRUE;
    BOOLEAN DoDgpuPowerOff = TRUE;
    BOOLEAN DoGpuPowerPrefs = TRUE;

    if (AppleSetOsHandleCount == 0) {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 2, TRUE, TRUE,
            L"AppleSetOs will not be loaded."
        );
    } else {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 2, TRUE, TRUE,
            L"AppleSetOs will be loaded, press Z to disable."
        );
    }
    _INT_SimpleTextGraphicsPrint(
        &gs, 0, 7, FALSE, TRUE,
        L"Gmux iGPU+dGPUoff+NVRAM (X=skip all, R=panel only)"
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

    // Z = skip AppleSetOs; X = skip all gmux/power/NVRAM; R = panel only
    if (Key.UnicodeChar == L'z' || Key.UnicodeChar == L'Z') {
        AppleSetOsHandleCount = 0;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 2, TRUE, TRUE,
            L"AppleSetOs will not be loaded."
        );
    }
    if (Key.UnicodeChar == L'x' || Key.UnicodeChar == L'X') {
        DoGmuxSwitch = FALSE;
        DoDgpuPowerOff = FALSE;
        DoGpuPowerPrefs = FALSE;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"Gmux/power/NVRAM: SKIPPED"
        );
    }
    if (Key.UnicodeChar == L'r' || Key.UnicodeChar == L'R') {
        DoDgpuPowerOff = FALSE;
        DoGpuPowerPrefs = FALSE;
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"Gmux panel only (Radeon stays powered)"
        );
    }
    if (Key.UnicodeChar == L'd' || Key.UnicodeChar == L'D') {
        // Recovery: force firmware back to dGPU on next boots; skip iGPU path this boot
        DoGmuxSwitch = FALSE;
        DoDgpuPowerOff = FALSE;
        DoGpuPowerPrefs = FALSE;
        {
            EFI_STATUS st = SetGpuPowerPrefsDgpu(SystemTable->RuntimeServices);
            _INT_SimpleTextGraphicsPrint(
                &gs, 0, 7, TRUE, TRUE,
                L"RECOVERY: gpu-power-prefs=dGPU %s (%lX)",
                EFI_ERROR(st) ? L"FAIL" : L"OK",
                st
            );
        }
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

    // ---- Gmux: panel -> iGPU, optional dGPU rail OFF + gpu-power-prefs ----
    // Order matches Linux force_igd spirit: route first, then power down unused GPU.
    // Never power the dGPU rail back on from Windows after OFF (known hang on this HW).
    if (DoGmuxSwitch) {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"Gmux: panel -> iGPU..."
        );
        if (GmuxSwitchToIGD(BS)) {
            BOOLEAN isIgd = GmuxReadPanelIsIGD(BS);
            _INT_SimpleTextGraphicsPrint(
                &gs, 0, 7, TRUE, TRUE,
                L"Gmux: route OK, readback=%s",
                isIgd ? L"iGPU" : L"dGPU/unknown"
            );
        } else {
            _INT_SimpleTextGraphicsPrint(
                &gs, 0, 7, TRUE, TRUE,
                L"Gmux: route FAILED (no gmux?)"
            );
            DoDgpuPowerOff = FALSE;
        }

        if (DoDgpuPowerOff) {
            _INT_SimpleTextGraphicsPrint(
                &gs, 0, 8, TRUE, TRUE,
                L"Gmux: powering OFF dGPU rail (0x50)..."
            );
            if (GmuxSetDiscretePower(BS, FALSE)) {
                _INT_SimpleTextGraphicsPrint(
                    &gs, 0, 8, TRUE, TRUE,
                    L"Gmux: dGPU rail OFF OK"
                );
            } else {
                _INT_SimpleTextGraphicsPrint(
                    &gs, 0, 8, TRUE, TRUE,
                    L"Gmux: dGPU rail OFF FAILED"
                );
            }
        }

        if (DoGpuPowerPrefs) {
            EFI_STATUS st = SetGpuPowerPrefsIgd(SystemTable->RuntimeServices);
            _INT_SimpleTextGraphicsPrint(
                &gs, 0, 9, TRUE, TRUE,
                L"NVRAM gpu-power-prefs=iGPU: %s (%lX)",
                EFI_ERROR(st) ? L"FAIL" : L"OK",
                st
            );
        }

        for (UINT16 j = 0; j < 120; j++) {
            BS->Stall(10000);
        }
    } else {
        _INT_SimpleTextGraphicsPrint(
            &gs, 0, 7, TRUE, TRUE,
            L"Gmux/power/NVRAM: skipped"
        );
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
