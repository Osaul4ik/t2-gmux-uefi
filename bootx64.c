#include <efi.h>
#include "include/int_graphics.h"
#include "include/int_print.h"
#include "include/int_event.h"
#include "include/int_guid.h"
#include "include/int_dpath.h"
#include "include/pci_db.h"
#include "include/int_vbt.h"
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

// ---- OpRegion / VBT diagnostic dump (key L) ----
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

    _INT_SimpleTextGraphicsPrint(gs, 2, Row, TRUE, FALSE,
        L"[%s] iGPU=%s ASLS=%x OpRegion=%s VBT=%s (ver %d, bdb %d)",
        Label, Info.IgpuFound ? L"yes" : L"NO", Info.Asls,
        Info.OpRegionOk ? L"ok" : L"NO", Info.VbtFound ? L"ok" : L"NO",
        (UINTN)Info.VbtVer, (UINTN)Info.BdbVer);
    if (Info.EdpFound) {
        _INT_SimpleTextGraphicsPrint(gs, 2, Row + 1, TRUE, FALSE,
            L"  eDP child: type=%04x dvo_port=%d aux=%02x ddc_pin=%d",
            (UINTN)Info.EdpType, (UINTN)Info.EdpDvoPort, (UINTN)Info.EdpAux, (UINTN)Info.EdpDdc);
    } else {
        _INT_SimpleTextGraphicsPrint(gs, 2, Row + 1, TRUE, FALSE,
            L"  eDP child: NOT FOUND in VBT");
    }
    if (Info.HaveLink) {
        _INT_SimpleTextGraphicsPrint(gs, 2, Row + 2, TRUE, FALSE,
            L"  panel_type=%d link rate code=%d lanes code=%d",
            (UINTN)Info.PanelType, (UINTN)Info.LinkRate, (UINTN)Info.LinkLanes);
    } else {
        _INT_SimpleTextGraphicsPrint(gs, 2, Row + 2, TRUE, FALSE,
            L"  panel_type=%d, no eDP link params", (UINTN)Info.PanelType);
    }
    _INT_SimpleTextGraphicsPrint(gs, 2, Row + 3, TRUE, TRUE,
        L"  files: txt=%s bin=%s vbt=%s (inspect=%lX)",
        EFI_ERROR(S1) ? L"FAIL" : L"ok",
        EFI_ERROR(S2) ? L"-" : L"ok",
        EFI_ERROR(S3) ? L"-" : L"ok", St);

    _INT_FreePool(BS, Buf);
}

// Output file names: \t2gmux_L_<boot>_<what>, e.g. \t2gmux_L_rad_after.txt.
//   boot = "igpu" if the Intel iGPU was already visible before AppleSetOs
//          (firmware booted from the iGPU), "rad" on a Radeon boot
// so runs with different boot GPUs never overwrite each other.
static VOID
MakeName(CHAR16 *Out, const CHAR16 *Boot, const CHAR16 *What)
{
    static const CHAR16 Pre[] = L"\\t2gmux_L_";
    UINTN n = 0;

    for (UINTN i = 0; Pre[i]; i++) Out[n++] = Pre[i];
    for (UINTN i = 0; Boot[i]; i++) Out[n++] = Boot[i];
    Out[n++] = L'_';
    for (UINTN i = 0; What[i]; i++) Out[n++] = What[i];
    Out[n] = 0;
}

// Register snapshot (\\t2gmux_<key>_<boot>_regs_<tag>.txt): iGPU display state at this moment.
// Compare "before" and "after" to see what the loader changes in the iGPU state.
static VOID
DoRegsDump(EFI_BOOT_SERVICES *BS, EFI_HANDLE Image, _INT_SimpleTextGraphicsStruct *gs,
           CHAR16 *FileName, CHAR16 *Label, UINTN Row)
{
    const UINTN Cap = 8192;
    CHAR8 *Buf = (CHAR8 *)_INT_AllocatePool(BS, Cap);
    _INT_Rep R;
    EFI_STATUS Rs, Ws;

    if (Buf == NULL)
        return;
    _INT_RepInit(&R, Buf, Cap);
    _INT_RepStr(&R, (const CHAR8 *)"=== t2-gmux-uefi iGPU registers: ");
    for (UINTN i = 0; Label[i]; i++) {
        CHAR8 t[2] = { (CHAR8)Label[i], 0 };
        _INT_RepStr(&R, t);
    }
    _INT_RepStr(&R, (const CHAR8 *)" ==="); _INT_RepNl(&R);

    Rs = _INT_DumpIgpuRegs(BS, Image, &R);
    Ws = _INT_WriteEspFile(BS, Image, FileName, Buf, R.len);

    _INT_SimpleTextGraphicsPrint(gs, 2, Row, TRUE, TRUE,
        L"[%s] regs: read=%lX file=%s", Label, Rs, EFI_ERROR(Ws) ? L"FAIL" : L"ok");
    _INT_FreePool(BS, Buf);
}

// ---- UI: plain ASCII frame on the text console ----
//
//  +======================================================================+
//  |                          GMUX_Control v0.7                           |
//  +======================================================================+
//  | Status: ...                                                          |
//  +----------------------------------------------------------------------+
//  | Boot mode:                                                           |
//  | > [D]  Only AMD Radeon        ...                                    |
//  |   [A]  AMD Radeon + Intel HD  ...                                    |
//  |   [I]  Intel HD               ...                                    |
//  |   [L]  Intel HD + Logs        ...                                    |
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
#define APP_TITLE       L"GMUX_Control v0.7"
#define UI_W            72      // frame width in columns, including both border chars

#define ROW_TOP         0
#define ROW_TITLE       1
#define ROW_TITLE_END   2
#define ROW_STATUS      3
#define ROW_SEP1        4
#define ROW_MENU_HDR    5
#define MENU_ROW        6       // 4 rows: 6..9
#define ROW_SEP2        10
#define TIMER_ROW       11
#define HINT_ROW        12
#define ROW_SEP3        13
#define GPU_HDR_ROW     14
#define GPU_ROW         15      // GPU_ROWS rows: 15..18
#define GPU_ROWS        4
#define ROW_END         19

// progress screen (after a mode is chosen)
#define PR_MODE         3
#define PR_SETOS        4
#define PR_MUX          5
#define PR_RAIL         6
#define PR_IRQ          7
#define PR_VBT          8
#define PR_DDI          9
#define PR_ACPI         10
#define PR_BEFORE       11      // 4 rows: 11..14
#define PR_REGS_BEFORE  15
#define PR_AFTER        16      // 4 rows: 16..19
#define PR_REGS_AFTER   20
#define PR_FILES        21
#define PR_BOOT         22

#define COUNTDOWN_SECS  5
#define TICKS_PER_SEC   20      // one tick = one 50 ms wait

typedef enum {
    MODE_RADEON = 0,            // D: only AMD Radeon (plain boot)
    MODE_RADEON_INTEL,          // A: AMD Radeon + Intel HD (AppleSetOs only)
    MODE_INTEL,                 // I: Intel HD (AppleSetOs + mux + Radeon OFF + injects)
    MODE_INTEL_LOG,             // L: Intel HD + logs
    MODE_COUNT
} BOOT_MODE;

static CHAR16 *MenuText[MODE_COUNT] = {
    L"[D]  Only AMD Radeon        plain boot, nothing is touched",
    L"[A]  AMD Radeon + Intel HD  AppleSetOs only",
    L"[I]  Intel HD               mux + Radeon OFF + injects",
    L"[L]  Intel HD + Logs        same as I + log files on the ESP",
};

static CHAR16 *MenuName[MODE_COUNT] = {
    L"D - Only AMD Radeon",
    L"A - AMD Radeon + Intel HD",
    L"I - Intel HD",
    L"L - Intel HD + Logs",
};

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
    for (UINTN m = 0; m < MODE_COUNT; m++)
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
    UI_PRINT(gs, HINT_ROW, L"Up/Down + Enter to select, or press D / A / I / L");
    UI_PRINT(gs, GPU_HDR_ROW, L"Graphics cards:");
}

static VOID
MenuDraw(_INT_SimpleTextGraphicsStruct *gs, BOOT_MODE Sel)
{
    for (UINTN m = 0; m < MODE_COUNT; m++) {
        UI_PRINT(gs, MENU_ROW + m, L"%s %s", (m == (UINTN)Sel) ? L">" : L" ", MenuText[m]);
    }
}

// Full redraw, then the selected row is repainted inverted (inside the border).
static VOID
MenuRefresh(_INT_SimpleTextGraphicsStruct *gs, BOOT_MODE Sel)
{
    UINTN Row = MENU_ROW + (UINTN)Sel;

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
    //   D = Only AMD Radeon: plain boot - no AppleSetOs, no mux, no rail, no injects, no files
    //   A = AMD Radeon + Intel HD: AppleSetOs only - no gmux, no rail, no VBT/DDI/ACPI patches, no files
    //   I = Intel HD: AppleSetOs + mux->iGPU + Radeon rail OFF + inject t2gmux_vbt.bin + DDI A 4 lanes + ACPI patch
    //   L = Intel HD + Logs: same as I, plus the OpRegion/VBT/register dump files (before/after/inject)
    BOOLEAN DoSetOs = FALSE;   // A / I / L: load AppleSetOs (the iGPU becomes visible)
    BOOLEAN DoSwitch = FALSE;  // I / L: mux->iGPU + Radeon rail OFF + VBT/DDI/ACPI patches
    BOOLEAN DoDump = FALSE;    // L: write dump / log files


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
    // Up/Down = move, Enter/Space = confirm, D/A/I/L = pick and confirm at once.
    // Any key stops the auto-boot timer; if no key is pressed for COUNTDOWN_SECS
    // the mode I (Intel HD: AppleSetOs + mux + Radeon OFF + injects) is started.
    BOOT_MODE Sel = MODE_INTEL;
    BOOLEAN TimerOn = TRUE;
    BOOLEAN Chosen = FALSE;
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

            MenuDraw(&gs, Sel);
            if (TimerOn) {
                UI_PRINT(&gs, TIMER_ROW,
                    L"Auto-boot in %u s: [I] Intel HD   (any key stops the timer)",
                    (UINT32)(COUNTDOWN_SECS - Sec));
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
                Sel = (BOOT_MODE)((Sel + MODE_COUNT - 1) % MODE_COUNT);
                TimerOn = FALSE;
            } else if (Key.ScanCode == 0x02) {         // Down
                Sel = (BOOT_MODE)((Sel + 1) % MODE_COUNT);
                TimerOn = FALSE;
            } else if (c == 0x0D || c == 0x0A || c == L' ') {   // Enter / Space
                Chosen = TRUE;
            } else if (c == L'd' || c == L'D') {
                Sel = MODE_RADEON; Chosen = TRUE;
            } else if (c == L'a' || c == L'A') {
                Sel = MODE_RADEON_INTEL; Chosen = TRUE;
            } else if (c == L'i' || c == L'I') {
                Sel = MODE_INTEL; Chosen = TRUE;
            } else if (c == L'l' || c == L'L') {
                Sel = MODE_INTEL_LOG; Chosen = TRUE;
            } else {
                TimerOn = FALSE;                       // any other key just stops the timer
            }
            Dirty = TRUE;
        } else {
            Tick++;
        }

        if (!Chosen && TimerOn && Tick >= (UINT32)(COUNTDOWN_SECS * TICKS_PER_SEC))
            Chosen = TRUE;                             // timeout: mode I
    }

    switch (Sel) {
    case MODE_RADEON_INTEL:
        DoSetOs = TRUE;
        break;
    case MODE_INTEL:
        DoSetOs = TRUE;
        DoSwitch = TRUE;
        break;
    case MODE_INTEL_LOG:
        DoSetOs = TRUE;
        DoSwitch = TRUE;
        DoDump = TRUE;
        break;
    default:
        break;                                         // MODE_RADEON: nothing is touched
    }

    // The menu is done: everything below the title is used for progress output.
    ClearRowsFrom(&gs, ROW_STATUS);
    LOG(&gs, PR_MODE, L"Mode: %s", MenuName[Sel]);

    // File name tag: which GPU the firmware booted from (see MakeName).
    const CHAR16 *BootTag = L"rad";
    CHAR16 N1[64], N2[64], N3[64], N4[64];
    if (DoDump) {
        if (_INT_IgpuVisible(BS, ImageHandle))
            BootTag = L"igpu";
        MakeName(N1, BootTag, L"*");
        LOG(&gs, PR_FILES, L"files: %s", N1);
    }

    // Must come after the mode is known (L), otherwise L
    // never writes the "before" report.
    if (DoDump) {
        MakeName(N1, BootTag, L"before.txt");
        MakeName(N2, BootTag, L"before_opregion.bin");
        MakeName(N3, BootTag, L"before_vbt.bin");
        DoVbtDump(BS, ImageHandle, &gs, N1, N2, N3, L"before", PR_BEFORE);
        MakeName(N4, BootTag, L"regs_before.txt");
        DoRegsDump(BS, ImageHandle, &gs, N4, L"before", PR_REGS_BEFORE);
    }

    // load apple_set_os - only for A / I / L; D = plain Windows boot
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

    // ---- I / L: gmux panel switch, dGPU rail OFF, VBT inject ----
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

        if (!MuxOk) {
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

        // VBT inject + DDI A 4 lanes + optional ACPI patch
        const UINTN RCap = 8192;
        CHAR8 *RBuf = (CHAR8 *)_INT_AllocatePool(BS, RCap);
        EFI_STATUS IS = EFI_OUT_OF_RESOURCES;
        EFI_STATUS LS = EFI_OUT_OF_RESOURCES;
        EFI_STATUS AS = EFI_OUT_OF_RESOURCES;
        if (RBuf != NULL) {
            _INT_Rep IR;
            _INT_RepInit(&IR, RBuf, RCap);
            IS = _INT_InjectVbt(BS, ImageHandle, L"\\t2gmux_vbt.bin", &IR);
            // Same iGPU-side setup the firmware does when it boots from the iGPU
            // (see _INT_IgpuForceDdiA4Lanes); the mux alone does not provide it.
            LS = _INT_IgpuForceDdiA4Lanes(BS, ImageHandle, &IR);
            // ACPI patch (brightness via gmux): \SSDT_IGPU.aml from the ESP root, optional.
            AS = _INT_AcpiApplyPatch(BS, SystemTable, ImageHandle, L"\\SSDT_IGPU.aml", &IR);
            if (DoDump) {
                MakeName(N1, BootTag, L"inject.txt");
                _INT_WriteEspFile(BS, ImageHandle, N1, RBuf, IR.len);
            }
            _INT_FreePool(BS, RBuf);
        }
        LOG(&gs, PR_VBT, L"VBT inject: %s (%lX)", EFI_ERROR(IS) ? L"FAILED" : L"OK", IS);
        LOG(&gs, PR_DDI, L"DDI A 4 lanes: %s (%lX)", EFI_ERROR(LS) ? L"FAILED" : L"OK", LS);
        LOG(&gs, PR_ACPI, L"ACPI patch SSDT_IGPU.aml: %s (%lX)",
            AS == EFI_NOT_FOUND ? L"not found, skipped" : (EFI_ERROR(AS) ? L"FAILED" : L"OK"), AS);
    }

    if (DoDump) {
        MakeName(N1, BootTag, L"after.txt");
        MakeName(N2, BootTag, L"after_opregion.bin");
        MakeName(N3, BootTag, L"after_vbt.bin");
        DoVbtDump(BS, ImageHandle, &gs, N1, N2, N3, L"after", PR_AFTER);
        MakeName(N4, BootTag, L"regs_after.txt");
        DoRegsDump(BS, ImageHandle, &gs, N4, L"after", PR_REGS_AFTER);
        for (UINT16 j = 0; j < 800; j++) {
            BS->Stall(10000);   // ~8 s to read the summary
        }
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