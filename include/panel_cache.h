/* GMUX_Control persistent Intel panel data. Include after int_vbt.h / int_edp.h. */
#ifndef GMUX_PANEL_CACHE_H
#define GMUX_PANEL_CACHE_H
#define PANEL_CACHE_FILE L"\\gmux_panel_cache.bin"
#define PANEL_CACHE_SIZE 144
/* Fixed on-disk layout: magic(4), version(1), rate(1), lanes(1),
   reserved(1), Intel PCI device id(2 LE), EDID(128), reserved(2), CRC32(4). */
static UINT32 PanelCrc32(const UINT8 *p, UINTN n) {
    UINT32 crc=0xFFFFFFFFu;
    for (UINTN i=0;i<n;i++) {
        crc ^= p[i];
        for (UINTN j=0;j<8;j++) crc=(crc>>1) ^ ((crc&1)?0xEDB88320u:0u);
    }
    return ~crc;
}
static BOOLEAN PanelEdidOk(const UINT8 *e) {
    static const UINT8 sig[8]={0,255,255,255,255,255,255,0};
    UINT8 sum=0;
    for(UINTN i=0;i<8;i++)if(e[i]!=sig[i])return FALSE;
    for(UINTN i=0;i<128;i++)sum=(UINT8)(sum+e[i]);
    return sum==0 && e[8]==0x06 && e[9]==0x10 && (e[54]||e[55]);
}
static EFI_STATUS PanelIgpuId(EFI_BOOT_SERVICES *BS,EFI_HANDLE Image,UINT16 *id) {
    EFI_STATUS st=EFI_NOT_FOUND;
    UINT32 raw=0;
    EFI_PCI_IO_PROTOCOL *p=_INT_FindIgpu(BS,Image,&raw,&st);
    if(!p)return EFI_NOT_FOUND;
    *id=(UINT16)(raw>>16);
    return EFI_SUCCESS;
}
static EFI_STATUS PanelCacheLoad(EFI_BOOT_SERVICES *BS,EFI_HANDLE Image,
                                  UINT8 *edid,UINT8 *rate,UINT8 *lanes) {
    VOID *data=NULL;UINTN size=0;
    EFI_STATUS st=_INT_ReadEspFile(BS,Image,PANEL_CACHE_FILE,&data,&size);
    if(EFI_ERROR(st))return st;
    UINT8 *b=(UINT8*)data;UINT16 id=0;
    if(size!=PANEL_CACHE_SIZE) { BS->FreePool(data);return EFI_COMPROMISED_DATA; }
    st=PanelIgpuId(BS,Image,&id);
    if(!EFI_ERROR(st)) {
        UINT32 stored=(UINT32)b[140]|((UINT32)b[141]<<8)|((UINT32)b[142]<<16)|((UINT32)b[143]<<24);
        if(size!=PANEL_CACHE_SIZE || b[0]!='P'||b[1]!='N'||b[2]!='L'||b[3]!='1'||
           b[4]!=1 || b[5]>2 || (b[6]!=0&&b[6]!=1&&b[6]!=3) ||
           (UINT16)(b[8]|((UINT16)b[9]<<8))!=id ||
           !PanelEdidOk(b+10) || stored!=PanelCrc32(b,140)) st=EFI_COMPROMISED_DATA;
        else {for(UINTN i=0;i<128;i++)edid[i]=b[10+i];*rate=b[5];*lanes=b[6];}
    }
    BS->FreePool(data);
    return st;
}
/* One-time write; never replace any existing cache, including a damaged one. */
static EFI_STATUS PanelCacheSaveOnce(EFI_BOOT_SERVICES *BS,EFI_HANDLE Image,
                                      const UINT8 *edid,const _INT_EdpCaps *caps) {
    if(!caps->Valid || !PanelEdidOk(edid) || caps->VbtRate>2 ||
       (caps->VbtLanes!=0&&caps->VbtLanes!=1&&caps->VbtLanes!=3))return EFI_INVALID_PARAMETER;
    VOID *old=NULL;UINTN size=0;
    EFI_STATUS st=_INT_ReadEspFile(BS,Image,PANEL_CACHE_FILE,&old,&size);
    if(!EFI_ERROR(st)) {BS->FreePool(old);return EFI_ALREADY_STARTED;}
    if(st!=EFI_NOT_FOUND)return st;
    UINT16 id=0;st=PanelIgpuId(BS,Image,&id);
    if(EFI_ERROR(st))return st;
    UINT8 b[PANEL_CACHE_SIZE];
    for(UINTN i=0;i<sizeof(b);i++)b[i]=0;
    b[0]='P';b[1]='N';b[2]='L';b[3]='1';b[4]=1;
    b[5]=caps->VbtRate;b[6]=caps->VbtLanes;
    b[8]=(UINT8)id;b[9]=(UINT8)(id>>8);
    for(UINTN i=0;i<128;i++)b[i+10]=edid[i];
    UINT32 crc=PanelCrc32(b,140);
    for(UINTN i=0;i<4;i++)b[140+i]=(UINT8)(crc>>(i*8));
    return _INT_WriteEspFile(BS,Image,PANEL_CACHE_FILE,b,sizeof(b));
}
#endif
