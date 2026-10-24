#ifndef GMUX_HYBRID_DIAG_H
#define GMUX_HYBRID_DIAG_H
/* This is an opt-in diagnostic capture, not a persistent config. */
#define HYBRID_DIAG_CAP 16384
static CHAR8 HybridDiagText[HYBRID_DIAG_CAP];
static UINTN HybridDiagUsed;
static BOOLEAN HybridDiagActive;
static EFI_BOOT_SERVICES *HybridDiagBS;
static EFI_HANDLE HybridDiagImage;
static void HybridDiagAppend(const char *s) {
    if (!HybridDiagActive || !s) return;
    while (*s && HybridDiagUsed + 2 < HYBRID_DIAG_CAP)
        HybridDiagText[HybridDiagUsed++] = *s++;
}
static void HybridDiagHex(UINT64 v, UINTN digits) {
    static const char hex[] = "0123456789ABCDEF";
    HybridDiagAppend("0x");
    if (digits > 16) digits = 16;
    for (UINTN i = digits; i > 0 && HybridDiagUsed + 2 < HYBRID_DIAG_CAP; i--)
        HybridDiagText[HybridDiagUsed++] = hex[(v >> ((i - 1) * 4)) & 15];
}
static void HybridDiagStatus(const char *label, EFI_STATUS status) {
    HybridDiagAppend(label);
    HybridDiagHex((UINT64)status, 16);
    HybridDiagAppend("\r\n");
}
static void HybridDiagValue(const char *label, UINT64 value, UINTN digits) {
    HybridDiagAppend(label);
    HybridDiagHex(value, digits);
    HybridDiagAppend("\r\n");
}
static void HybridDiagBegin(EFI_BOOT_SERVICES *bs, EFI_HANDLE img) {
    HybridDiagBS = bs;
    HybridDiagImage = img;
    HybridDiagUsed = 0;
    HybridDiagActive = TRUE;
    HybridDiagAppend("GMUX_Control Hybrid diagnostic: T was pressed\r\n");
}
static void HybridDiagReport(const _INT_Rep *rep) {
    if (!HybridDiagActive || !rep || !rep->buf) return;
    HybridDiagAppend("=== eDP/VBT/ACPI report ===\r\n");
    for (UINTN i = 0; i < rep->len && HybridDiagUsed + 2 < HYBRID_DIAG_CAP; i++)
        HybridDiagText[HybridDiagUsed++] = rep->buf[i];
    HybridDiagAppend("\r\n");
}
static EFI_STATUS HybridDiagFlush(void) {
    if (!HybridDiagActive) return EFI_NOT_READY;
    return _INT_WriteEspFile(HybridDiagBS, HybridDiagImage,
                             L"\\gmux_hybrid_diag.log", HybridDiagText, HybridDiagUsed);
}
#endif
