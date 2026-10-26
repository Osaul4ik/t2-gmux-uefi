/* GMUX_Control: display pipeline state of a boot where the firmware lit the internal panel.
 * Include after panel_cache.h (uses PanelCrc32 / PanelIgpuId) and int_edp.h.
 *
 * File \gmux_disp_state.bin, little endian:
 *   0 "DSP1"   4 version (1)   5 reserved   6 count (u16)   8 Intel PCI device id (u16)   10 reserved (2)
 *   12 count x { offset u32, value u32 }      then CRC32 of everything before it
 */
#ifndef GMUX_DISP_CACHE_H
#define GMUX_DISP_CACHE_H
#define DISP_CACHE_FILE L"\\gmux_disp_state.bin"
#define DISP_CACHE_MAX_SIZE (12 + 8 * _INT_DISP_MAX + 4)

static UINTN DispCachePack(UINT8 *b, UINT16 id, const _INT_DispState *s) {
    UINTN n = s->Count > _INT_DISP_MAX ? _INT_DISP_MAX : s->Count, o = 12;
    for (UINTN i = 0; i < DISP_CACHE_MAX_SIZE; i++) b[i] = 0;
    b[0]='D'; b[1]='S'; b[2]='P'; b[3]='1'; b[4]=1;
    b[6]=(UINT8)n; b[7]=(UINT8)(n>>8); b[8]=(UINT8)id; b[9]=(UINT8)(id>>8);
    for (UINTN i = 0; i < n; i++) {
        for (UINTN j = 0; j < 4; j++) { b[o+j]=(UINT8)(s->R[i].Off>>(8*j)); b[o+4+j]=(UINT8)(s->R[i].Val>>(8*j)); }
        o += 8;
    }
    UINT32 crc = PanelCrc32(b, o);
    for (UINTN j = 0; j < 4; j++) b[o+j]=(UINT8)(crc>>(8*j));
    return o + 4;
}
static EFI_STATUS DispCacheUnpack(const UINT8 *b, UINTN size, UINT16 id, _INT_DispState *s) {
    s->Count = 0;
    if (size < 16 || b[0]!='D'||b[1]!='S'||b[2]!='P'||b[3]!='1'||b[4]!=1) return EFI_COMPROMISED_DATA;
    UINTN n = (UINTN)(b[6] | (b[7] << 8));
    if (n == 0 || n > _INT_DISP_MAX || size != 12 + 8 * n + 4) return EFI_COMPROMISED_DATA;
    UINT16 fid = (UINT16)(b[8] | (b[9] << 8));
    UINT32 stored = 0;
    for (UINTN j = 0; j < 4; j++) stored |= (UINT32)b[12+8*n+j] << (8*j);
    if (stored != PanelCrc32(b, 12 + 8 * n) || (id && fid != id)) return EFI_COMPROMISED_DATA;
    for (UINTN i = 0; i < n; i++) {
        UINT32 off = 0, val = 0;
        for (UINTN j = 0; j < 4; j++) { off |= (UINT32)b[12+8*i+j] << (8*j); val |= (UINT32)b[16+8*i+j] << (8*j); }
        s->R[i].Off = off; s->R[i].Val = val;
    }
    s->Count = (UINT32)n;
    return EFI_SUCCESS;
}

#ifndef PANEL_CACHE_PURE
static EFI_STATUS DispCacheLoad(EFI_BOOT_SERVICES *BS, EFI_HANDLE Image, _INT_DispState *s) {
    VOID *data = NULL; UINTN size = 0; UINT16 id = 0;
    EFI_STATUS st = _INT_ReadEspFile(BS, Image, DISP_CACHE_FILE, &data, &size);
    if (EFI_ERROR(st)) return st;
    st = PanelIgpuId(BS, Image, &id);
    if (!EFI_ERROR(st)) st = DispCacheUnpack((const UINT8 *)data, size, id, s);
    BS->FreePool(data);
    return st;
}
/* Rewrites only when the content changed. EFI_ALREADY_STARTED = identical, nothing written. */
static EFI_STATUS DispCacheStore(EFI_BOOT_SERVICES *BS, EFI_HANDLE Image, const _INT_DispState *s) {
    UINT16 id = 0; EFI_STATUS st = PanelIgpuId(BS, Image, &id);
    if (EFI_ERROR(st)) return st;
    UINT8 b[DISP_CACHE_MAX_SIZE];
    UINTN n = DispCachePack(b, id, s);
    VOID *old = NULL; UINTN size = 0;
    st = _INT_ReadEspFile(BS, Image, DISP_CACHE_FILE, &old, &size);
    if (!EFI_ERROR(st)) {
        BOOLEAN same = (size == n);
        for (UINTN i = 0; same && i < n; i++) if (((UINT8 *)old)[i] != b[i]) same = FALSE;
        BS->FreePool(old);
        if (same) return EFI_ALREADY_STARTED;
    }
    return _INT_WriteEspFile(BS, Image, DISP_CACHE_FILE, b, n);
}
#endif
#endif