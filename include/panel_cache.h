/* GMUX_Control persistent internal-panel data (ESP file). Include after int_vbt.h / int_edp.h.
 *
 * Everything the VBT / DDI patch is built from lives in this file:
 *   EDID (128 bytes), DPCD link data (rev, max rate, max lanes, eDP rev, PSR, rate table)
 *   and the derived VBT rate / lane codes.
 * It is (re)written on every boot that reads the panel itself (no external display, the
 * firmware lit the internal panel) and is the ONLY source on a boot with an external display,
 * where the AMD GOP / EDID memory describes the external monitor, not the panel.
 *
 * v2 layout, 168 bytes, little endian:
 *   0  "PNL2"            4  version (2)       5  VBT rate        6  VBT lanes
 *   7  DPCD rev          8  Intel PCI dev id (2)                 10 max link rate
 *   11 max lanes         12 eDP rev           13 PSR support     14 flags (bit0 = PPS valid)
 *   16 rate table 8 x u16 (units of 200 kHz)  32 EDID (128)
 *   160 PPS timing as programmed by the firmware: PP_ON_DELAYS, PP_OFF_DELAYS, PP_DIVISOR,
 *       PCH_RAWCLK_FREQ (4 x u32)             176 reserved(4)    180 CRC32 of bytes 0..179
 * v1 ("PNL1", 144 bytes: rate, lanes, EDID) is still read; the next live boot rewrites it as v2.
 */
#ifndef GMUX_PANEL_CACHE_H
#define GMUX_PANEL_CACHE_H
#define PANEL_CACHE_FILE L"\\gmux_panel_cache.bin"
#define PANEL_CACHE_SIZE 184
#define PANEL_CACHE_SIZE_V1 144

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
/* No struct assignment: the loader is freestanding (no memcpy). */
static void PanelCapsCopyLink(_INT_EdpCaps *d, const _INT_EdpCaps *s) {
    d->Valid=s->Valid; d->DpcdRev=s->DpcdRev; d->MaxLinkRate=s->MaxLinkRate;
    d->MaxLanes=s->MaxLanes; d->EdpRev=s->EdpRev; d->PsrSupport=s->PsrSupport;
    for(UINTN i=0;i<8;i++)d->RateTable[i]=s->RateTable[i];
    d->VbtRate=s->VbtRate; d->VbtLanes=s->VbtLanes;
}
static BOOLEAN PanelLinkCodesOk(UINT8 rate,UINT8 lanes) {
    return rate<=2 && (lanes==0||lanes==1||lanes==3);
}
/* Serialise into b[PANEL_CACHE_SIZE]. */
static void PanelCachePack(UINT8 *b,UINT16 id,const UINT8 *edid,const _INT_EdpCaps *c,const _INT_EdpPps *pps) {
    for(UINTN i=0;i<PANEL_CACHE_SIZE;i++)b[i]=0;
    b[0]='P';b[1]='N';b[2]='L';b[3]='2';b[4]=2;
    b[5]=c->VbtRate;b[6]=c->VbtLanes;b[7]=c->DpcdRev;
    b[8]=(UINT8)id;b[9]=(UINT8)(id>>8);
    b[10]=c->MaxLinkRate;b[11]=c->MaxLanes;b[12]=c->EdpRev;b[13]=c->PsrSupport;
    for(UINTN i=0;i<8;i++){b[16+2*i]=(UINT8)c->RateTable[i];b[17+2*i]=(UINT8)(c->RateTable[i]>>8);}
    for(UINTN i=0;i<128;i++)b[32+i]=edid[i];
    if(pps && pps->Valid) {
        const UINT32 v[4]={pps->OnDelays,pps->OffDelays,pps->Divisor,pps->RawClk};
        b[14]|=1;
        for(UINTN i=0;i<4;i++)for(UINTN j=0;j<4;j++)b[160+4*i+j]=(UINT8)(v[i]>>(j*8));
    }
    UINT32 crc=PanelCrc32(b,180);
    for(UINTN i=0;i<4;i++)b[180+i]=(UINT8)(crc>>(i*8));
}
/* Validate + parse a v2 or v1 file image. id==0 skips the iGPU id check (host tests). */
static EFI_STATUS PanelCacheUnpack(const UINT8 *b,UINTN size,UINT16 id,UINT8 *edid,_INT_EdpCaps *c,_INT_EdpPps *pps) {
    UINT32 stored; UINT16 fid;
    for(UINTN i=0;i<sizeof(*c);i++)((UINT8*)c)[i]=0;
    for(UINTN i=0;i<sizeof(*pps);i++)((UINT8*)pps)[i]=0;
    if(size==PANEL_CACHE_SIZE && b[0]=='P'&&b[1]=='N'&&b[2]=='L'&&b[3]=='2'&&b[4]==2) {
        stored=(UINT32)b[180]|((UINT32)b[181]<<8)|((UINT32)b[182]<<16)|((UINT32)b[183]<<24);
        fid=(UINT16)(b[8]|((UINT16)b[9]<<8));
        if(stored!=PanelCrc32(b,180) || (id&&fid!=id) || !PanelLinkCodesOk(b[5],b[6]) ||
           !PanelEdidOk(b+32) || b[11]>4) return EFI_COMPROMISED_DATA;
        c->Valid=TRUE; c->VbtRate=b[5]; c->VbtLanes=b[6]; c->DpcdRev=b[7];
        c->MaxLinkRate=b[10]; c->MaxLanes=b[11]; c->EdpRev=b[12]; c->PsrSupport=b[13];
        for(UINTN i=0;i<8;i++)c->RateTable[i]=(UINT16)(b[16+2*i]|((UINT16)b[17+2*i]<<8));
        for(UINTN i=0;i<128;i++)edid[i]=b[32+i];
        if(b[14]&1) {
            UINT32 *o[4]={&pps->OnDelays,&pps->OffDelays,&pps->Divisor,&pps->RawClk};
            for(UINTN i=0;i<4;i++){UINT32 v=0;for(UINTN j=0;j<4;j++)v|=(UINT32)b[160+4*i+j]<<(j*8);*o[i]=v;}
            pps->Valid=TRUE;
        }
        return EFI_SUCCESS;
    }
    if(size==PANEL_CACHE_SIZE_V1 && b[0]=='P'&&b[1]=='N'&&b[2]=='L'&&b[3]=='1'&&b[4]==1) {
        stored=(UINT32)b[140]|((UINT32)b[141]<<8)|((UINT32)b[142]<<16)|((UINT32)b[143]<<24);
        fid=(UINT16)(b[8]|((UINT16)b[9]<<8));
        if(stored!=PanelCrc32(b,140) || (id&&fid!=id) || !PanelLinkCodesOk(b[5],b[6]) ||
           !PanelEdidOk(b+10)) return EFI_COMPROMISED_DATA;
        c->Valid=TRUE; c->VbtRate=b[5]; c->VbtLanes=b[6];
        c->MaxLanes=(UINT8)(b[6]==3?4:b[6]==1?2:1);
        for(UINTN i=0;i<128;i++)edid[i]=b[10+i];
        return EFI_SUCCESS;
    }
    return EFI_COMPROMISED_DATA;
}

#ifndef PANEL_CACHE_PURE
static EFI_STATUS PanelIgpuId(EFI_BOOT_SERVICES *BS,EFI_HANDLE Image,UINT16 *id) {
    EFI_STATUS st=EFI_NOT_FOUND;
    UINT32 raw=0;
    EFI_PCI_IO_PROTOCOL *p=_INT_FindIgpu(BS,Image,&raw,&st);
    if(!p)return EFI_NOT_FOUND;
    *id=(UINT16)(raw>>16);
    return EFI_SUCCESS;
}
/* Load + validate the cache. EFI_NOT_FOUND = no file. */
static EFI_STATUS PanelCacheLoad(EFI_BOOT_SERVICES *BS,EFI_HANDLE Image,
                                 UINT8 *edid,_INT_EdpCaps *caps,_INT_EdpPps *pps) {
    VOID *data=NULL;UINTN size=0;UINT16 id=0;
    EFI_STATUS st=_INT_ReadEspFile(BS,Image,PANEL_CACHE_FILE,&data,&size);
    if(EFI_ERROR(st))return st;
    st=PanelIgpuId(BS,Image,&id);
    if(!EFI_ERROR(st)) st=PanelCacheUnpack((const UINT8*)data,size,id,edid,caps,pps);
    BS->FreePool(data);
    return st;
}
/* Store the panel data read from the live panel. The file is rewritten when the stored
 * image differs (or is damaged / v1); identical content is not rewritten (ESP wear).
 * Returns EFI_ALREADY_STARTED when nothing had to be written. */
static EFI_STATUS PanelCacheStore(EFI_BOOT_SERVICES *BS,EFI_HANDLE Image,
                                  const UINT8 *edid,const _INT_EdpCaps *caps,const _INT_EdpPps *pps) {
    if(!caps->Valid || !PanelEdidOk(edid) || !PanelLinkCodesOk(caps->VbtRate,caps->VbtLanes))
        return EFI_INVALID_PARAMETER;
    UINT16 id=0;EFI_STATUS st=PanelIgpuId(BS,Image,&id);
    if(EFI_ERROR(st))return st;
    UINT8 b[PANEL_CACHE_SIZE];
    PanelCachePack(b,id,edid,caps,pps);
    VOID *old=NULL;UINTN size=0;
    st=_INT_ReadEspFile(BS,Image,PANEL_CACHE_FILE,&old,&size);
    if(!EFI_ERROR(st)) {
        BOOLEAN same=(size==PANEL_CACHE_SIZE);
        for(UINTN i=0;same&&i<PANEL_CACHE_SIZE;i++)if(((UINT8*)old)[i]!=b[i])same=FALSE;
        BS->FreePool(old);
        if(same)return EFI_ALREADY_STARTED;
    }
    return _INT_WriteEspFile(BS,Image,PANEL_CACHE_FILE,b,sizeof(b));
}
#endif
#endif