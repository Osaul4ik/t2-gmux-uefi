#include <efi.h>
#include "include/int_graphics.h"
#include "include/int_print.h"
#include "include/int_event.h"
#include "include/int_guid.h"
#include "include/int_dpath.h"
#include "include/pci_db.h"
#include "include/int_vbt.h"
#include "include/int_edp.h"
#include "include/int_mem.h"
#include "include/int_acpi.h"


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

// ---- UI: plain ASCII frame on the text console ----
//
//  +======================================================================+
//  |                          GMUX_Control v0.7                           |
//  +======================================================================+
//  | Status: ...                                                          |
//  +----------------------------------------------------------------------+
//  | Boot mode:                                                           |
//  | > [1] Standart Boot (Radeon only)                                    |
//  |   [4] Efficient Boot (Intel only)                                    |
//  |   [5] Hybrid Boot (Intel + Radeon)   (only if gpu-power-prefs = iGPU)|
//  |   *************************    (separator, never selectable)         |
//  |   [6] Advanced menu            (Switch to dGPU / iGPU, separator,    |
//  |                                 Standart Boot + Intel Secondary,     |
//  |                                 Reboot, Power off, Back)             |
//  +----------------------------------------------------------------------+
//  | Auto-boot in N s ...                                                 |
//  | Up/Down + Enter ...                                                  |
//  +----------------------------------------------------------------------+
//  | Graphics cards:                                                      |
//  |   (up to GPU_ROWS cards)                                             |
//  +======================================================================+
//
// After a mode is chosen the frame below the title is cleared and the rows PR_* are
// used for progress output (no frame there).
#define APP_TITLE       L"GMUX_Control v0.8"
#define UI_W            72      // frame width in columns, including both border chars
#define UI_HINT_MAIN    L"Up/Down + Enter, or the mode number. X = save selected as default (x)"

#define ROW_TOP         0
#define ROW_TITLE       1
#define ROW_TITLE_END   2
#define ROW_STATUS      3
#define ROW_SEP1        4
#define ROW_MENU_HDR    5
#define MENU_ROW        6       // MENU_ROWS rows: 6..(6+MENU_ROWS-1)
#define MENU_ADV        MODE_COUNT          // menu entry 6 (Advanced menu): not a boot mode
#define MENU_ITEMS      4                   // modes 1, 4, 5 + Advanced menu: what Up/Down walk over
#define MENU_ROWS       7                   // rows reserved for a menu: the Advanced menu is the longest (2 + separator + 4)
#define MENU_SEP_ROW    (MENU_ROW + 3)      // between mode 5 and the Advanced menu
#define MENU_SEP_TEXT   L"*************************"
#define ROW_SEP2        (MENU_ROW + MENU_ROWS)
#define TIMER_ROW       (ROW_SEP2 + 1)
#define HINT_ROW        (ROW_SEP2 + 2)
#define ROW_SEP3        (ROW_SEP2 + 3)
#define GPU_HDR_ROW     (ROW_SEP2 + 4)
#define GPU_ROW         (ROW_SEP2 + 5)      // GPU_ROWS rows
#define GPU_ROWS        4
#define ROW_END         (GPU_ROW + GPU_ROWS)

// progress screen (after a mode is chosen)
#define PR_MODE         3
#define PR_SETOS        4
#define PR_MUX          5
#define PR_RAIL         6
#define PR_IRQ          7
#define PR_VBT          8
#define PR_DDI          9
#define PR_ACPI         10
#define PR_BOOT         11

#define COUNTDOWN_SECS  5
#define TICKS_PER_SEC   20      // one tick = one 50 ms wait

typedef enum {
    MODE_RADEON = 0,            // 1: Standart Boot (clean boot, nothing is touched)
    MODE_RADEON_INTEL,          // 2: Standart Boot + Intel Secondary (standard boot + apple_set_os), Advanced menu
    MODE_INTEL,                 // 4: Efficient Boot (AppleSetOs + mux + Radeon OFF + DDI A 4 lanes + ACPI patch + built-in VBT)
    MODE_INTEL_DGPU_ON,         // 5: Hybrid Boot: as 4, but the Radeon rail stays ON; ACPI patch = brightness + gmux re-route on resume
    MODE_COUNT
} BOOT_MODE;

// Main menu: modes 1, 4, 5 and the Advanced menu entry (6). Mode 2 is not in the main
// menu (it is reached from the Advanced menu). One separator row ("*************************")
// sits between mode 5 and the Advanced menu. It is not an entry, so Up/Down (which walk over
// entries only) can never land on it.
static const UINTN MenuOrder[MENU_ITEMS] = { MODE_RADEON, MODE_INTEL, MODE_INTEL_DGPU_ON, MENU_ADV };

// Position of a mode / MENU_ADV in the menu (0 if it is not in the main menu).
static UINTN
MenuPosOf(UINTN M)
{
    for (UINTN i = 0; i < MENU_ITEMS; i++)
        if (MenuOrder[i] == M)
            return i;
    return 0;
}

// Row of an entry in the frame (the separator row is skipped).
static UINTN
MenuRowOf(UINTN M)
{
    UINTN Pos = MenuPosOf(M);
    return MENU_ROW + Pos + ((Pos >= 3) ? 1 : 0);
}

// indexed by mode; mode 2 has no main-menu text
static CHAR16 *MenuText[MODE_COUNT + 1] = {
    [MODE_RADEON]          = L"[1] Standart Boot (Radeon only)",
    [MODE_INTEL]           = L"[4] Efficient Boot (Intel only)",
    [MODE_INTEL_DGPU_ON]   = L"[5] Hybrid Boot (Intel + Radeon)",
    [MENU_ADV]             = L"[6] Advanced menu",
};

// mode 5 while gpu-power-prefs does not hold the iGPU value: shown, but cannot be started
static CHAR16 *MenuText5Off = L"[5] Hybrid Boot (Intel + Radeon) - unavailable: Switch to iGPU";

static CHAR16 *MenuName[MODE_COUNT] = {
    L"1 - Standart Boot (Radeon only)",
    L"2 - Standart Boot + Intel Secondary",
    L"4 - Efficient Boot (Intel only)",
    L"5 - Hybrid Boot (Intel + Radeon)",
};

// Default mode (the one with the x mark, started by the auto-boot timer) is kept in a
// one-character file in the root of the ESP (1, 4 or 5); key X in the menu rewrites it.
#define DEFAULT_FILE    L"\\t2gmux_default.txt"
static const CHAR16 ModeLetter[MODE_COUNT] = { L'1', L'2', L'4', L'5' };

static BOOT_MODE
DefaultLoad(EFI_BOOT_SERVICES *BS, EFI_HANDLE Image)
{
    VOID *Data = NULL;
    UINTN Size = 0;
    BOOT_MODE Def = MODE_INTEL;                    // no file / unreadable: 4

    if (!EFI_ERROR(_INT_ReadEspFile(BS, Image, DEFAULT_FILE, &Data, &Size)) && Data != NULL) {
        for (UINTN i = 0; i < Size; i++) {
            CHAR8 ch = ((CHAR8 *)Data)[i];
            UINTN m;
            for (m = 0; m < MODE_COUNT; m++) {
                if ((CHAR8)ModeLetter[m] == ch) {
                    Def = (BOOT_MODE)m;
                    break;
                }
            }
            if (m < MODE_COUNT || (ch != ' ' && ch != '\r' && ch != '\n'))
                break;                             // first non-blank character decides
        }
        _INT_FreePool(BS, Data);
    }
    if (Def == MODE_RADEON_INTEL)                  // mode 2 is not in the main menu
        Def = MODE_RADEON;
    return Def;
}

static EFI_STATUS
DefaultSave(EFI_BOOT_SERVICES *BS, EFI_HANDLE Image, BOOT_MODE Mode)
{
    CHAR8 Buf[2];

    Buf[0] = (CHAR8)ModeLetter[Mode];
    Buf[1] = '\n';
    return _INT_WriteEspFile(BS, Image, DEFAULT_FILE, Buf, 2);
}

// ---- required files per mode ----
// 4 = ACPI patch without VBT + built-in VBT completed from the panel and injected from UEFI: needs \SSDT_IGPU.aml.
// 5 = same as 4 (Radeon stays ON), but with its own SSDT (brightness + resume re-route): needs \SSDT_IGPU_BRT.aml.
// A mode whose files are missing cannot be chosen: the menu stays and shows a message.
#define ACPI_FILE_BASE  L"\\SSDT_IGPU.aml"          // mode 4
#define ACPI_FILE_BRT   L"\\SSDT_IGPU_BRT.aml"      // mode 5 (brightness + resume re-route)

static BOOLEAN
EspFileExists(EFI_BOOT_SERVICES *BS, EFI_HANDLE Image, const CHAR16 *Name)
{
    VOID *Data = NULL;
    UINTN Size = 0;
    BOOLEAN Ok = FALSE;

    if (!EFI_ERROR(_INT_ReadEspFile(BS, Image, (CHAR16 *)Name, &Data, &Size)) && Data != NULL && Size != 0)
        Ok = TRUE;
    if (Data != NULL)
        _INT_FreePool(BS, Data);
    return Ok;
}

// TRUE if the mode can start. Otherwise *Msg points to the reason (shown in the status line).
static BOOLEAN
ModeAvailable(EFI_BOOT_SERVICES *BS, EFI_HANDLE Image, BOOT_MODE Mode, BOOLEAN IGpuPref, const CHAR16 **Msg)
{
    if (Mode == MODE_INTEL) {
        // No VBT file: the built-in VBT is completed from the panel itself
        // (DPCD / EDID over the iGPU's AUX channel).
        if (!EspFileExists(BS, Image, ACPI_FILE_BASE)) {
            *Msg = L"mode 4 unavailable: SSDT_IGPU.aml not found";
            return FALSE;
        }
    } else if (Mode == MODE_INTEL_DGPU_ON) {
        // mode 5 is only available while gpu-power-prefs holds the iGPU value (Advanced menu)
        if (!IGpuPref) {
            *Msg = L"mode 5 unavailable: Switch to iGPU first (Advanced menu)";
            return FALSE;
        }
        if (!EspFileExists(BS, Image, ACPI_FILE_BRT)) {
            *Msg = L"mode 5 unavailable: SSDT_IGPU_BRT.aml not found";
            return FALSE;
        }
    }
    return TRUE;
}

// Row = Edge + (UI_W-2) x Fill + Edge, rest of the line blank. Edits the text buffer only.
static VOID
UiRowFill(_INT_SimpleTextGraphicsStruct *gs, UINTN Row, CHAR16 Edge, CHAR16 Fill)
{
    if (gs->buf == NULL || Row >= gs->row)
        return;
    for (UINTN j = 0; j < gs->col; j++)
        gs->buf[Row][j] = 0x20;
    for (UINTN j = 1; j + 1 < UI_W && j < gs->col; j++)
        gs->buf[Row][j] = Fill;
    gs->buf[Row][0] = Edge;
    if (UI_W - 1 < gs->col)
        gs->buf[Row][UI_W - 1] = Edge;
    gs->buf[Row][gs->col] = 0;
}

static VOID
UiRule(_INT_SimpleTextGraphicsStruct *gs, UINTN Row, CHAR16 Fill)
{
    UiRowFill(gs, Row, L'+', Fill);
}

static VOID
UiBlank(_INT_SimpleTextGraphicsStruct *gs, UINTN Row)
{
    UiRowFill(gs, Row, L'|', L' ');
}

// After text was printed into a framed row: restore the right border and cut
// everything that ran past it.
static VOID
UiBorder(_INT_SimpleTextGraphicsStruct *gs, UINTN Row)
{
    if (gs->buf == NULL || Row >= gs->row)
        return;
    for (UINTN j = UI_W; j < gs->col; j++)
        gs->buf[Row][j] = 0x20;
    if (UI_W - 1 < gs->col)
        gs->buf[Row][UI_W - 1] = L'|';
}

// Text inside the frame (left margin 2), no refresh.
#define UI_PRINT(g, r, ...) do { \
        UiBlank((g), (r)); \
        _INT_SimpleTextGraphicsPrint((g), 2, (r), FALSE, FALSE, __VA_ARGS__); \
        UiBorder((g), (r)); \
    } while (0)

// Status line (row ROW_STATUS), refreshed at once.
#define UI_STATUS(g, fmt, ...) do { \
        UI_PRINT((g), ROW_STATUS, L"Status: " fmt, ##__VA_ARGS__); \
        _INT_SimpleTextGraphicsRefresh(g); \
    } while (0)

// Progress line (after the menu), refreshed at once.
#define LOG(g, r, ...) _INT_SimpleTextGraphicsPrint((g), 2, (r), TRUE, TRUE, __VA_ARGS__)

static VOID
UiDrawFrame(_INT_SimpleTextGraphicsStruct *gs)
{
    UiRule(gs, ROW_TOP, L'=');
    UiBlank(gs, ROW_TITLE);
    UiRule(gs, ROW_TITLE_END, L'=');
    UiBlank(gs, ROW_STATUS);
    UiRule(gs, ROW_SEP1, L'-');
    UiBlank(gs, ROW_MENU_HDR);
    for (UINTN m = 0; m < MENU_ROWS; m++)
        UiBlank(gs, MENU_ROW + m);
    UiRule(gs, ROW_SEP2, L'-');
    UiBlank(gs, TIMER_ROW);
    UiBlank(gs, HINT_ROW);
    UiRule(gs, ROW_SEP3, L'-');
    UiBlank(gs, GPU_HDR_ROW);
    for (UINTN g = 0; g < GPU_ROWS; g++)
        UiBlank(gs, GPU_ROW + g);
    UiRule(gs, ROW_END, L'=');

    // title, centred
    _INT_SimpleTextGraphicsPrint(gs, (UI_W - 17) / 2, ROW_TITLE, FALSE, FALSE, APP_TITLE);
    UiBorder(gs, ROW_TITLE);

    UI_PRINT(gs, ROW_MENU_HDR, L"Boot mode:");
    UI_PRINT(gs, HINT_ROW, UI_HINT_MAIN);
    UI_PRINT(gs, GPU_HDR_ROW, L"Graphics cards:");
}

static VOID
MenuDraw(_INT_SimpleTextGraphicsStruct *gs, BOOT_MODE Sel, BOOT_MODE Def, BOOLEAN Mode5Active)
{
    for (UINTN i = 0; i < MENU_ITEMS; i++) {
        UINTN m = MenuOrder[i];
        BOOLEAN Off = (m == (UINTN)MODE_INTEL_DGPU_ON && !Mode5Active);
        UI_PRINT(gs, MenuRowOf(m), L"%s%s %s", (m == (UINTN)Sel) ? L">" : L" ",
                 (m == (UINTN)Def) ? L"x" : L" ", Off ? MenuText5Off : MenuText[m]);
    }
    // separator: plain text, no marker, never highlighted (Sel is always an entry)
    UI_PRINT(gs, MENU_SEP_ROW, L"   " MENU_SEP_TEXT);
}

// Full redraw, then the given row is repainted inverted (inside the border).
static VOID
UiRefreshHighlight(_INT_SimpleTextGraphicsStruct *gs, UINTN Row)
{
    gs->ConOut->SetAttribute(gs->ConOut, 0x07);
    _INT_SimpleTextGraphicsRefresh(gs);
    if (gs->buf != NULL && Row < gs->row && gs->col >= UI_W) {
        CHAR16 tmp[UI_W];
        UINTN n = 0;

        for (UINTN j = 1; j + 1 < UI_W; j++)
            tmp[n++] = gs->buf[Row][j];
        tmp[n] = 0;

        gs->ConOut->SetCursorPosition(gs->ConOut, 1, Row);
        gs->ConOut->SetAttribute(gs->ConOut, 0x70);
        gs->ConOut->OutputString(gs->ConOut, tmp);
        gs->ConOut->SetAttribute(gs->ConOut, 0x07);
        gs->ConOut->SetCursorPosition(gs->ConOut, 0, 0);
    }
}

static VOID
MenuRefresh(_INT_SimpleTextGraphicsStruct *gs, BOOT_MODE Sel)
{
    UiRefreshHighlight(gs, MenuRowOf((UINTN)Sel));
}

// ---- Apple GPU power preference in NVRAM (gpu-power-prefs) ----
// Variable gpu-power-prefs, vendor GUID fa4ce28d-b62f-4c99-9cc3-6815686e30f9 (the one macOS
// `nvram` writes). Value 01 00 00 00 makes the firmware use the Intel iGPU at the next boot;
// without the variable the firmware default (the Radeon) is used. So:
//   Switch to iGPU = write 01 00 00 00      Switch to dGPU = delete the variable
// The loader reads it at start and after the Advanced menu:
//   iGPU value (first byte 01)   -> "Hybrid Boot" (mode 5) is available, "Switch to iGPU" is hidden
//   variable absent / other      -> "Hybrid Boot" is unavailable, "Switch to dGPU" is hidden
//   variable cannot be read      -> "Hybrid Boot" is unavailable, both switch entries are shown
// The firmware reads the variable at boot, so after a successful switch the loader reboots the Mac.
#define GPU_PREFS_NAME  L"gpu-power-prefs"
#define GPU_PREFS_ATTR  0x07            // NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS, as macOS writes it
static EFI_GUID GpuPrefsGuid = { 0xfa4ce28d, 0xb62f, 0x4c99, { 0x9c, 0xc3, 0x68, 0x15, 0x68, 0x6e, 0x30, 0xf9 } };

typedef struct {
    BOOLEAN Known;                      // the variable could be queried (found or not found)
    BOOLEAN Present;
    UINTN   Size;
    UINT8   Data[4];                    // first bytes of the value
    EFI_STATUS Status;
} GPU_PREFS;

static VOID
GpuPrefsRead(EFI_RUNTIME_SERVICES *RT, GPU_PREFS *P)
{
    UINT8 Buf[16];
    UINTN Size = sizeof(Buf);
    UINT32 Attr = 0;

    for (UINTN i = 0; i < sizeof(*P); i++)
        ((UINT8 *)P)[i] = 0;
    P->Status = RT->GetVariable(GPU_PREFS_NAME, &GpuPrefsGuid, &Attr, &Size, Buf);
    if (P->Status == EFI_SUCCESS || P->Status == EFI_BUFFER_TOO_SMALL) {
        P->Known = TRUE;
        P->Present = TRUE;
        P->Size = Size;
        for (UINTN i = 0; i < sizeof(P->Data) && i < Size && i < sizeof(Buf); i++)
            P->Data[i] = Buf[i];
    } else if (P->Status == EFI_NOT_FOUND) {
        P->Known = TRUE;
    }
}

// TRUE only if the variable is there and holds the iGPU value (first byte 01). An unreadable
// variable counts as "not confirmed", so Hybrid Boot stays unavailable then.
static BOOLEAN
GpuPrefsIsIGpu(EFI_RUNTIME_SERVICES *RT)
{
    GPU_PREFS P;

    GpuPrefsRead(RT, &P);
    return (P.Known && P.Present && P.Size >= 1 && P.Data[0] == 0x01) ? TRUE : FALSE;
}

// Write the value (iGPU) or delete the variable (dGPU), then read it back.
static EFI_STATUS
GpuPrefsApply(EFI_RUNTIME_SERVICES *RT, BOOLEAN ToIGpu)
{
    GPU_PREFS P;
    EFI_STATUS St;

    if (ToIGpu) {
        UINT8 Val[4] = { 0x01, 0x00, 0x00, 0x00 };
        St = RT->SetVariable(GPU_PREFS_NAME, &GpuPrefsGuid, GPU_PREFS_ATTR, sizeof(Val), Val);
    } else {
        St = RT->SetVariable(GPU_PREFS_NAME, &GpuPrefsGuid, GPU_PREFS_ATTR, 0, NULL);
        if (St == EFI_NOT_FOUND)
            St = EFI_SUCCESS;           // already absent
    }
    if (EFI_ERROR(St))
        return St;

    GpuPrefsRead(RT, &P);
    if (ToIGpu)
        return (P.Present && P.Size == 4 && P.Data[0] == 0x01) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    return (P.Known && !P.Present) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

// Up/Down step over the main menu entries (MenuOrder), wrapping around and skipping Hybrid Boot
// (mode 5) while it is unavailable. Step = 1 (down) or MENU_ITEMS - 1 (up).
static UINTN
MenuStep(UINTN Sel, UINTN Step, BOOLEAN Mode5Active)
{
    UINTN Pos = MenuPosOf(Sel);

    do {
        Pos = (Pos + Step) % MENU_ITEMS;
    } while (!Mode5Active && MenuOrder[Pos] == (UINTN)MODE_INTEL_DGPU_ON);
    return MenuOrder[Pos];
}

// ---- Advanced menu ----
//   Switch to dGPU (delete gpu-power-prefs) + reboot     (hidden while the preference is not iGPU)
//   Switch to iGPU (set gpu-power-prefs) + reboot        (hidden while the preference is iGPU)
//   *************************
//   Standart Boot + Intel Secondary   (mode 2: standard boot + AppleSetOs, the iGPU is visible
//                                      next to the Radeon)
//   Reboot
//   Power off
//   Back                              (Esc does the same)
typedef enum { ADV_TO_DGPU = 0, ADV_TO_IGPU, ADV_STD_INTEL, ADV_REBOOT, ADV_POWEROFF, ADV_BACK, ADV_COUNT } ADV_ITEM;
static CHAR16 *AdvText[ADV_COUNT] = {
    L"Switch to dGPU (delete gpu-power-prefs) + reboot",
    L"Switch to iGPU (set gpu-power-prefs) + reboot",
    L"Standart Boot + Intel Secondary",
    L"Reboot",
    L"Power off",
    L"Back",
};

// Cold reset / shutdown through the runtime services. They do not return on success; if one
// does return, the caller shows an error and stays in the menu.
static EFI_STATUS
ResetNow(EFI_BOOT_SERVICES *BS, EFI_RUNTIME_SERVICES *RT, EFI_RESET_TYPE Type)
{
    BS->Stall(1500000);                 // 1.5 s: the status line stays readable
    RT->ResetSystem(Type, EFI_SUCCESS, 0, NULL);
    return EFI_DEVICE_ERROR;
}

// Returns TRUE if "Standart Boot + Intel Secondary" was chosen (the caller starts mode 2),
// FALSE on Back / Esc (back to the boot menu).
static BOOLEAN
AdvancedMenu(EFI_BOOT_SERVICES *BS, EFI_RUNTIME_SERVICES *RT, SIMPLE_INPUT_INTERFACE *ConIn,
             _INT_SimpleTextGraphicsStruct *gs)
{
    ADV_ITEM Items[ADV_COUNT];
    UINTN N = 0;
    UINTN Group = 0;                    // number of switch entries (the separator follows them)
    UINTN Sel = 0;
    BOOLEAN Dirty = TRUE;
    BOOLEAN Done = FALSE;
    BOOLEAN Start = FALSE;

    while (!Done) {
        if (Dirty) {
            GPU_PREFS P;
            BOOLEAN IsIGpu;

            GpuPrefsRead(RT, &P);
            IsIGpu = (P.Known && P.Present && P.Size >= 1 && P.Data[0] == 0x01);
            N = 0;
            if (!P.Known || IsIGpu)
                Items[N++] = ADV_TO_DGPU;       // hidden while the preference is not iGPU
            if (!P.Known || !IsIGpu)
                Items[N++] = ADV_TO_IGPU;       // hidden while the preference is iGPU
            Group = N;
            Items[N++] = ADV_STD_INTEL;
            Items[N++] = ADV_REBOOT;
            Items[N++] = ADV_POWEROFF;
            Items[N++] = ADV_BACK;
            if (Sel >= N)
                Sel = N - 1;

            UI_PRINT(gs, ROW_MENU_HDR, L"Advanced menu:");
            for (UINTN r = 0; r < MENU_ROWS; r++)
                UiBlank(gs, MENU_ROW + r);
            for (UINTN i = 0; i < N; i++)
                UI_PRINT(gs, MENU_ROW + i + ((i >= Group) ? 1 : 0), L"%s %s",
                         (i == Sel) ? L">" : L" ", AdvText[Items[i]]);
            UI_PRINT(gs, MENU_ROW + Group, L"   " MENU_SEP_TEXT);

            if (!P.Known) {
                UI_PRINT(gs, TIMER_ROW, L"gpu-power-prefs: cannot be read (%lX)", P.Status);
            } else if (!P.Present) {
                UI_PRINT(gs, TIMER_ROW, L"gpu-power-prefs: not set (firmware default, dGPU)");
            } else if (P.Size == 4) {
                UI_PRINT(gs, TIMER_ROW, L"gpu-power-prefs: set = %02x %02x %02x %02x%s",
                         (UINTN)P.Data[0], (UINTN)P.Data[1], (UINTN)P.Data[2], (UINTN)P.Data[3],
                         IsIGpu ? L" (iGPU)" : L"");
            } else {
                UI_PRINT(gs, TIMER_ROW, L"gpu-power-prefs: set, %d byte(s)", (UINTN)P.Size);
            }
            UI_PRINT(gs, HINT_ROW, L"Up/Down + Enter. Esc = back. A GPU switch reboots the Mac.");
            UiRefreshHighlight(gs, MENU_ROW + Sel + ((Sel >= Group) ? 1 : 0));
            Dirty = FALSE;
        }

        {
            EFI_INPUT_KEY Key;
            UINTN Idx = 0;
            CHAR16 c;

            if (EFI_ERROR(BS->WaitForEvent(1, &ConIn->WaitForKey, &Idx))) {
                BS->Stall(50000);
                continue;
            }
            Key.UnicodeChar = 0;
            Key.ScanCode = 0;
            if (EFI_ERROR(ConIn->ReadKeyStroke(ConIn, &Key)))
                continue;
            c = Key.UnicodeChar;

            if (Key.ScanCode == 0x01) {                         // Up
                Sel = (Sel + N - 1) % N;
                Dirty = TRUE;
            } else if (Key.ScanCode == 0x02) {                  // Down
                Sel = (Sel + 1) % N;
                Dirty = TRUE;
            } else if (Key.ScanCode == 0x17) {                  // Esc
                Done = TRUE;
            } else if (c == 0x0D || c == 0x0A || c == L' ') {   // Enter / Space
                ADV_ITEM It = Items[Sel];

                if (It == ADV_BACK) {
                    Done = TRUE;
                } else if (It == ADV_STD_INTEL) {
                    Start = TRUE;
                    Done = TRUE;
                } else if (It == ADV_REBOOT || It == ADV_POWEROFF) {
                    BOOLEAN Off = (It == ADV_POWEROFF);
                    EFI_STATUS RSt;

                    UI_STATUS(gs, L"%s...", Off ? L"powering off" : L"rebooting");
                    RSt = ResetNow(BS, RT, Off ? EfiResetShutdown : EfiResetCold);
                    UI_STATUS(gs, L"%s FAILED (%lX)", Off ? L"power off" : L"reboot", RSt);   // only reached if ResetSystem returned
                } else {
                    BOOLEAN ToIGpu = (It == ADV_TO_IGPU);
                    EFI_STATUS St = GpuPrefsApply(RT, ToIGpu);

                    if (!EFI_ERROR(St)) {
                        // the firmware reads gpu-power-prefs at boot: reboot now to apply it
                        EFI_STATUS RSt;

                        UI_STATUS(gs, L"%s done, rebooting...", ToIGpu ? L"Switch to iGPU" : L"Switch to dGPU");
                        RSt = ResetNow(BS, RT, EfiResetCold);
                        UI_STATUS(gs, L"%s done, but reboot FAILED (%lX)",
                                  ToIGpu ? L"Switch to iGPU" : L"Switch to dGPU", RSt);
                    } else {
                        UI_STATUS(gs, L"%s FAILED (%lX), no reboot", ToIGpu ? L"Switch to iGPU" : L"Switch to dGPU", St);
                    }
                    Sel = 0;
                    Dirty = TRUE;                               // the hidden entry swaps
                }
            }
        }
    }

    // back to the boot menu: the caller redraws the entries and the timer row
    UI_PRINT(gs, ROW_MENU_HDR, L"Boot mode:");
    UI_PRINT(gs, HINT_ROW, UI_HINT_MAIN);
    return Start;
}

static VOID
ClearRowsFrom(_INT_SimpleTextGraphicsStruct *gs, UINTN From)
{
    for (UINTN r = From; r < gs->row; r++) {
        _INT_SimpleTextGraphicsPrint(gs, 0, r, TRUE, FALSE, L" ");
    }
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
        UI_PRINT(gs, GPU_ROW, L"PciIo Buffer Error: %lX", Status);
        _INT_SimpleTextGraphicsRefresh(gs);
    } else if (PciIoHandleCount == 0) {
        UI_PRINT(gs, GPU_ROW, L"No PciIo Handles");
        _INT_SimpleTextGraphicsRefresh(gs);
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

                        if (NumOfGpu < GPU_ROWS) {
                            UI_PRINT(gs, GPU_ROW + NumOfGpu,
                                L"%04x %04x %s - %s", PciHeader.VendorId, PciHeader.DeviceId, VendorStr, DeviceStr);
                        }

                        NumOfGpu++;
                    }
                }
            }
        }

        for (UINTN r = NumOfGpu; r < GPU_ROWS; r++) {
            UiBlank(gs, GPU_ROW + r);
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

    UiDrawFrame(&gs);
    UI_STATUS(&gs, L"starting...");

    // update gpu info
    PrintGpu(BS, &gs, ImageHandle);


    // get apple_set_os protocol (only looked up here; reported when a mode needs it)
    EFI_HANDLE* AppleSetOsHandleBuf = NULL;
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
        AppleSetOsHandleCount = 0;
        AppleSetOsHandleBuf = NULL;
    }

    // Boot modes (chosen in the menu):
    //   1 = Standart Boot: clean boot - no AppleSetOs, no mux, no rail, no injects, no files
    //   2 = Standart Boot + Intel Secondary (Advanced menu): standard boot + AppleSetOs patch - no gmux,
    //       no rail, no VBT/DDI/ACPI patches, no files
    //   4 = Efficient Boot: AppleSetOs + mux->iGPU + Radeon rail OFF + DDI A 4 lanes + ACPI patch
    //       (SSDT_IGPU.aml) + the built-in VBT injected from UEFI (link from panel DPCD, timing from EDID)
    //   5 = Hybrid Boot: copy of 4, but the Radeon rail stays ON, and the ACPI patch is SSDT_IGPU_BRT.aml:
    //       brightness (_BCM) + _WAK (gmux re-route on resume); no _PTS, no rail code
    BOOLEAN DoSetOs = FALSE;   // 2 / 4 / 5: load AppleSetOs (the iGPU becomes visible)
    BOOLEAN DoSwitch = FALSE;  // 4 / 5: mux->iGPU + DDI A 4 lanes + VBT injection + ACPI patch
    BOOLEAN DoRailOff = FALSE; // 4: additionally Radeon rail OFF (5 leaves it ON)
    CHAR16 *AcpiFile = NULL;   // SSDT file of the chosen mode (set below)


    // find and load bootx64_original.efi
    EFI_LOADED_IMAGE_PROTOCOL* LoadedImage;
    EFI_DEVICE_PATH* DevicePath = NULL;
    EFI_HANDLE DriverHandle;

    UI_STATUS(&gs, L"initializing LoadedImageProtocol...");
    EFI_GUID efi_loaded_image_protocol_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    Status = BS->HandleProtocol(ImageHandle, &efi_loaded_image_protocol_guid, (VOID**) &LoadedImage);
    if (EFI_ERROR(Status) || LoadedImage == NULL) {
        UI_STATUS(&gs, L"LoadedImageProtocol error: %lX", Status);
        goto halt;
    }

    UI_STATUS(&gs, L"locating bootx64_original.efi...");
    DevicePath = _INT_FileDevicePath(
        BS, 
        LoadedImage->DeviceHandle, 
        L"\\EFI\\Boot\\bootx64_original.efi"
    );

    if (DevicePath == NULL) {
        UI_STATUS(&gs, L"unable to find bootx64_original.efi");
        goto halt;
    }

    UI_STATUS(&gs, L"loading bootx64_original.efi to memory...");
    // Attempt to load the driver.
    Status = BS->LoadImage(FALSE, ImageHandle, DevicePath, NULL, 0, &DriverHandle);
    _INT_FreePool(BS, DevicePath);
    DevicePath = NULL;

    if (EFI_ERROR(Status)) {
        UI_STATUS(&gs, L"unable to load bootx64_original.efi to memory");
        goto halt;
    }

    UI_STATUS(&gs, L"preparing bootx64_original.efi...");

    Status = BS->OpenProtocol(
        DriverHandle, 
        &efi_loaded_image_protocol_guid,
        (VOID**)&LoadedImage, 
        ImageHandle, 
        NULL, 
        EFI_OPEN_PROTOCOL_GET_PROTOCOL
    );
    if (EFI_ERROR(Status)) {
        UI_STATUS(&gs, L"failed to run bootx64_original.efi");
        goto halt;
    }

    UI_STATUS(&gs, L"bootx64_original.efi ready");

    // ---- boot mode menu ----
    // Up/Down = move (over entries only, the separators are skipped), Enter/Space = confirm,
    // 1 / 4 / 5 = pick and confirm at once, 6 = Advanced menu,
    // X = save the highlighted mode as the default (marked x). Any key stops the auto-boot
    // timer; if no key is pressed for COUNTDOWN_SECS the default mode is started.
    BOOT_MODE Def = DefaultLoad(BS, ImageHandle);
    BOOT_MODE Sel = Def;
    BOOLEAN Mode5Active = GpuPrefsIsIGpu(SystemTable->RuntimeServices);   // Hybrid Boot needs the iGPU preference
    BOOLEAN TimerOn = TRUE;
    BOOLEAN Chosen = FALSE;
    BOOLEAN Want = FALSE;       // a start was requested; Chosen only if the mode's files exist
    BOOLEAN Dirty = TRUE;
    UINT32 Tick = 0;
    UINT32 LastSec = 0xFFFFFFFF;

    while (!Chosen) {
        EFI_INPUT_KEY Key;
        EFI_STATUS WS;
        UINT32 Sec = Tick / TICKS_PER_SEC;

        // redraw on change and once per second
        if (Dirty || Sec != LastSec) {
            if (Sec != LastSec && Sec != 0 && (Sec % 3) == 0)
                PrintGpu(BS, &gs, ImageHandle);       // pick up a newly plugged GPU
            LastSec = Sec;
            Dirty = FALSE;

            MenuDraw(&gs, Sel, Def, Mode5Active);
            if (TimerOn) {
                UI_PRINT(&gs, TIMER_ROW,
                    L"Auto-boot in %u s: [%c] default   (any key stops the timer)",
                    (UINT32)(COUNTDOWN_SECS - Sec), ModeLetter[Def]);
            } else {
                UI_PRINT(&gs, TIMER_ROW, L"Timer stopped - choose a mode and press Enter");
            }
            MenuRefresh(&gs, Sel);
        }

        Key.UnicodeChar = 0;
        Key.ScanCode = 0;

        // one tick = one 50 ms wait for a key
        WS = _INT_WaitForSingleEvent(BS, ConIn->WaitForKey, 500000);
        if (EFI_ERROR(WS) && WS != EFI_TIMEOUT)
            BS->Stall(50000);                          // event not usable: still pace the loop

        if (!EFI_ERROR(ConIn->ReadKeyStroke(ConIn, &Key))) {
            CHAR16 c = Key.UnicodeChar;

            if (Key.ScanCode == 0x01) {                // Up
                Sel = (BOOT_MODE)MenuStep(Sel, MENU_ITEMS - 1, Mode5Active);
                TimerOn = FALSE;
            } else if (Key.ScanCode == 0x02) {         // Down
                Sel = (BOOT_MODE)MenuStep(Sel, 1, Mode5Active);
                TimerOn = FALSE;
            } else if (c == 0x0D || c == 0x0A || c == L' ') {   // Enter / Space
                Want = TRUE;
            } else if (c == L'x' || c == L'X') {          // save the highlighted mode as default
                if ((UINTN)Sel == MENU_ADV) {
                    UI_STATUS(&gs, L"Advanced menu cannot be the default");
                } else {
                    EFI_STATUS DS = DefaultSave(BS, ImageHandle, Sel);
                    if (!EFI_ERROR(DS)) {
                        Def = Sel;
                        UI_STATUS(&gs, L"default saved: %s", MenuName[Def]);
                    } else {
                        UI_STATUS(&gs, L"default NOT saved (%lX)", DS);
                    }
                }
                TimerOn = FALSE;
            } else if (c == L'1') {
                Sel = MODE_RADEON; Want = TRUE;
            } else if (c == L'4') {
                Sel = MODE_INTEL; Want = TRUE;
            } else if (c == L'5') {
                if (Mode5Active) {
                    Sel = MODE_INTEL_DGPU_ON; Want = TRUE;
                } else {
                    UI_STATUS(&gs, L"mode 5 unavailable: Switch to iGPU first (Advanced menu)");
                    TimerOn = FALSE;
                }
            } else if (c == L'6') {
                Sel = (BOOT_MODE)MENU_ADV; Want = TRUE;
            } else {
                TimerOn = FALSE;                       // any other key just stops the timer
            }
            Dirty = TRUE;
        } else {
            Tick++;
        }

        if (!Want && TimerOn && Tick >= (UINT32)(COUNTDOWN_SECS * TICKS_PER_SEC))
            Want = TRUE;                               // timeout: default mode

        if (Want) {
            const CHAR16 *Why = NULL;

            Want = FALSE;
            if ((UINTN)Sel == MENU_ADV) {
                // not a boot mode: open the submenu, then come back to this menu
                // (or start "Standart Boot + Intel Secondary" = mode 2)
                if (AdvancedMenu(BS, SystemTable->RuntimeServices, ConIn, &gs)) {
                    Sel = MODE_RADEON_INTEL;
                    Chosen = TRUE;
                }
                Mode5Active = GpuPrefsIsIGpu(SystemTable->RuntimeServices);   // may have changed
                TimerOn = FALSE;
                Dirty = TRUE;
            } else if (ModeAvailable(BS, ImageHandle, Sel, Mode5Active, &Why)) {
                Chosen = TRUE;
            } else {
                // refuse: stay in the menu, stop the timer, show the reason
                TimerOn = FALSE;
                Dirty = TRUE;
                UI_STATUS(&gs, L"%s", Why);
                if (Sel == MODE_INTEL_DGPU_ON && !Mode5Active)
                    Sel = MODE_INTEL;                  // do not leave the highlight on the unavailable entry
            }
        }
    }

    switch (Sel) {
    case MODE_RADEON_INTEL:
        DoSetOs = TRUE;
        break;
    case MODE_INTEL:
        DoSetOs = TRUE;
        DoSwitch = TRUE;
        DoRailOff = TRUE;
        AcpiFile = ACPI_FILE_BASE;
        break;
    case MODE_INTEL_DGPU_ON:
        DoSetOs = TRUE;                                // as 4 ...
        DoSwitch = TRUE;
        AcpiFile = ACPI_FILE_BRT;                      // ... SSDT without rail code
        // DoRailOff stays FALSE: the Radeon is not switched off
        break;
    default:
        break;                                         // MODE_RADEON: nothing is touched
    }

    // The menu is done: everything below the title is used for progress output.
    ClearRowsFrom(&gs, ROW_STATUS);
    LOG(&gs, PR_MODE, L"Mode: %s", MenuName[Sel]);

    // load apple_set_os - only for modes 2 / 4 / 5; mode 1 = plain Windows boot
    if (!DoSetOs) {
        LOG(&gs, PR_SETOS, L"AppleSetOs: not loaded");
    } else if (AppleSetOsHandleCount == 0) {
        LOG(&gs, PR_SETOS, L"AppleSetOs: NOT AVAILABLE on this firmware");
    } else {
        EFI_STATUS SetOsErr = EFI_SUCCESS;
        BOOLEAN SetOsDone = FALSE;

        for (UINTN i = 0; i < AppleSetOsHandleCount; i++) {
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
                SetOsErr = Status;
                continue;
            }
            if (SetOsIface->Version != 0) {
                Status = SetOsIface->SetOsVendor((CHAR8*) APPLE_SET_OS_VENDOR);
                if (EFI_ERROR(Status))
                    SetOsErr = Status;

                Status = SetOsIface->SetOsVersion((CHAR8*) APPLE_SET_OS_VERSION);
                if (EFI_ERROR(Status))
                    SetOsErr = Status;

                SetOsDone = TRUE;
            }
        }

        if (EFI_ERROR(SetOsErr)) {
            LOG(&gs, PR_SETOS, L"AppleSetOs: ERROR %lX", SetOsErr);
        } else if (SetOsDone) {
            LOG(&gs, PR_SETOS, L"AppleSetOs: OK (%a, %a)",
                APPLE_SET_OS_VENDOR, APPLE_SET_OS_VERSION);
        } else {
            LOG(&gs, PR_SETOS, L"AppleSetOs: skipped (protocol version 0)");
        }
    }

    if (AppleSetOsHandleBuf != NULL)
        _INT_FreePool(BS, AppleSetOsHandleBuf);

    // ---- modes 4 / 5: gmux panel switch, (4: dGPU rail OFF), VBT inject, ACPI patch ----
    // Never power the dGPU rail back on from Windows after OFF (known hang on this HW).
    if (DoSwitch) {
        GMUX_IRQ_SAVE IrqSave;
        GmuxIrqBegin(BS, &IrqSave);

        LOG(&gs, PR_MUX, L"Gmux: panel -> iGPU...");
        BOOLEAN MuxOk = GmuxSwitchToIGD(BS);
        if (MuxOk) {
            LOG(&gs, PR_MUX, L"Gmux: route OK, readback=iGPU");
        } else {
            LOG(&gs, PR_MUX, L"Gmux: route FAILED (no gmux or readback != iGPU)");
        }

        if (!DoRailOff) {
            // Panel on the iGPU, the Radeon stays powered (it is the render GPU).
            LOG(&gs, PR_RAIL, L"Gmux: dGPU rail left ON");
        } else if (!MuxOk) {
            // Cutting the rail while the panel is still routed to the Radeon blacks the screen.
            LOG(&gs, PR_RAIL, L"Gmux: dGPU rail OFF SKIPPED (panel not confirmed on iGPU)");
        } else {
            BOOLEAN PowerEvent = FALSE;
            LOG(&gs, PR_RAIL, L"Gmux: powering OFF dGPU rail (0x50)...");
            if (GmuxSetDiscretePower(BS, FALSE, &PowerEvent)) {
                LOG(&gs, PR_RAIL, L"Gmux: dGPU rail OFF OK (power event %s)",
                    PowerEvent ? L"seen" : L"not seen, fixed delay");
            } else {
                LOG(&gs, PR_RAIL, L"Gmux: dGPU rail OFF FAILED");
            }
        }

        UINT8 Left = GmuxIrqEnd(BS, &IrqSave);
        LOG(&gs, PR_IRQ, L"Gmux: irq status cleared (was %x), mask restored=%s",
            (UINTN)Left, IrqSave.MaskValid ? L"yes" : L"no");

        for (UINT16 j = 0; j < 80; j++) {
            BS->Stall(10000);
        }

        // VBT inject + DDI A 4 lanes + ACPI patch
        const UINTN RCap = 8192;
        CHAR8 *RBuf = (CHAR8 *)_INT_AllocatePool(BS, RCap);
        EFI_STATUS IS = EFI_OUT_OF_RESOURCES;
        EFI_STATUS LS = EFI_OUT_OF_RESOURCES;
        EFI_STATUS AS = EFI_OUT_OF_RESOURCES;
        EFI_STATUS ES = EFI_NOT_FOUND;
        EFI_STATUS DS = EFI_NOT_READY;
        BOOLEAN EdidFromAux = FALSE;
        BOOLEAN Skip4Lanes = FALSE;
        _INT_EdpCaps Caps;
        for (UINTN k = 0; k < sizeof(Caps); k++)
            ((UINT8 *)&Caps)[k] = 0;
        if (RBuf != NULL) {
            _INT_Rep IR;
            _INT_RepInit(&IR, RBuf, RCap);
            // The iGPU trains the link itself, so ask the panel what it can do: DPCD
            // (rate / lanes / PSR) and EDID over the iGPU's own AUX-A channel.
            _INT_EdpProbe(BS, ImageHandle, &Caps, &IR);

            IS = _INT_InjectVbtBuiltin(BS, ImageHandle, &IR);
            if (!EFI_ERROR(IS))
                DS = _INT_VbtApplyDpcd(BS, ImageHandle, &Caps, &IR);
            // Apple's EFI only has panel data for the dGPU: put its EDID timing into the iGPU VBT;
            // if it has none, take the EDID the panel itself returned over AUX.
            if (!EFI_ERROR(IS)) {
                ES = _INT_VbtApplyFirmwareEdid(BS, ImageHandle, &IR);
                if (EFI_ERROR(ES) && Caps.HasEdid) {
                    ES = _INT_VbtApplyEdidBuf(BS, ImageHandle, Caps.Edid, 128,
                                              "panel EDID over AUX", &IR);
                    EdidFromAux = !EFI_ERROR(ES);
                }
                // The built-in VBT only has a placeholder timing: without a real EDID the
                // driver would get a wrong panel, so leave the mailbox empty as before.
                if (EFI_ERROR(ES)) {
                    _INT_VbtClearMailbox(BS, ImageHandle, &IR);
                    IS = EFI_NOT_READY;
                }
            }
            // Same iGPU-side setup the firmware does when it boots from the iGPU
            // (see _INT_IgpuForceDdiA4Lanes); the mux alone does not provide it.
            // A panel that reports fewer than 4 lanes must not get the 4-lane strap.
            if (Caps.Valid && Caps.VbtLanes != 3)
                Skip4Lanes = TRUE;
            else
                LS = _INT_IgpuForceDdiA4Lanes(BS, ImageHandle, &IR);
            // ACPI patch (brightness via gmux) from the ESP root: mode 4 = SSDT_IGPU.aml,
            // mode 5 = SSDT_IGPU_BRT.aml (brightness + resume re-route).
            AS = _INT_AcpiApplyPatch(BS, SystemTable, ImageHandle, AcpiFile, &IR);
            _INT_FreePool(BS, RBuf);
        }
        LOG(&gs, PR_VBT, L"VBT %s (%lX): link %s, EDID %s",
            EFI_ERROR(IS) ? L"FAILED" : L"OK", IS,
            EFI_ERROR(DS) ? L"default 4xHBR2 (probe failed)" : L"from panel DPCD",
            EFI_ERROR(ES) ? L"not applied" : (EdidFromAux ? L"from panel" : L"from dGPU"));
        if (Skip4Lanes)
            LOG(&gs, PR_DDI, L"DDI A 4 lanes: skipped (panel reports %lX lane(s))", (UINTN)Caps.MaxLanes);
        else
            LOG(&gs, PR_DDI, L"DDI A 4 lanes: %s (%lX)", EFI_ERROR(LS) ? L"FAILED" : L"OK", LS);
        LOG(&gs, PR_ACPI, L"ACPI patch %s: %s (%lX)",
            AcpiFile + 1,                              // file name without the leading backslash
            AS == EFI_NOT_FOUND ? L"not found, skipped" : (EFI_ERROR(AS) ? L"FAILED" : L"OK"), AS);
    }

    LOG(&gs, PR_BOOT, L"Booting bootx64_original.efi...");

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
        UiDrawFrame(&gs);
        UI_STATUS(&gs, L"unable to boot bootx64_original.efi (%lX)", Status);
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