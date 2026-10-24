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
// Read-only UEFI snapshots. One separate file per checkpoint, only on T boot.
// Capture before/after state WITHOUT enabling PCI memory decode or power wells.
#define GMUX_DEEP_CAP 32768
static CHAR8 GmuxDeepText[GMUX_DEEP_CAP];
static UINTN GmuxDeepLen;
static void GmuxDeepStr(const char *s) {
    if (!s) return;
    while (*s && GmuxDeepLen + 2 < GMUX_DEEP_CAP)
        GmuxDeepText[GmuxDeepLen++] = (CHAR8)*s++;
}
static void GmuxDeepHex(UINT64 x, UINTN digits) {
    static const char t[]="0123456789ABCDEF";
    if (digits>16) digits=16;
    GmuxDeepStr("0x");
    while(digits && GmuxDeepLen+2<GMUX_DEEP_CAP) {
        digits--;
        GmuxDeepText[GmuxDeepLen++]=(CHAR8)t[(x>>(digits*4))&15];
    }
}
static void GmuxDeepVal(const char *key, UINT64 v,UINTN n){GmuxDeepStr(key);GmuxDeepHex(v,n);GmuxDeepStr("\r\n");}
static void GmuxDeepPci(EFI_BOOT_SERVICES *bs,EFI_HANDLE image) {
    EFI_GUID guid=EFI_PCI_IO_PROTOCOL_GUID;
    EFI_HANDLE *handles=NULL; UINTN count=0;
    EFI_STATUS st=bs->LocateHandleBuffer(ByProtocol,&guid,NULL,&count,&handles);
    GmuxDeepVal("PCI enumerate status=",st,16);
    if(EFI_ERROR(st))return;
    for(UINTN i=0;i<count;i++) {
        EFI_PCI_IO_PROTOCOL *p=NULL;
        st=bs->OpenProtocol(handles[i],&guid,(VOID**)&p,image,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if(EFI_ERROR(st)||!p)continue;
        UINT8 cfg[64]={0};
        st=p->Pci.Read(p,EfiPciIoWidthUint8,0,64,cfg);
        if(EFI_ERROR(st))continue;
        UINTN vendor=(UINTN)cfg[0]|((UINTN)cfg[1]<<8);
        UINTN device=(UINTN)cfg[2]|((UINTN)cfg[3]<<8);
        if ((vendor!=0x8086 && vendor!=0x1002) || cfg[11]!=3)continue;
        UINTN seg=0,bus=0,dev=0,func=0;
        st=p->GetLocation(p,&seg,&bus,&dev,&func);
        GmuxDeepStr("\r\nGPU PCI ");GmuxDeepHex(vendor,4);GmuxDeepStr(":");GmuxDeepHex(device,4);GmuxDeepStr("\r\n");
        GmuxDeepVal("GetLocation status=",st,16);
        if(!EFI_ERROR(st)) {
            GmuxDeepVal("Segment=",seg,4);GmuxDeepVal("Bus=",bus,2);
            GmuxDeepVal("Device=",dev,2);GmuxDeepVal("Function=",func,2);
        }
        for(UINTN o=0;o<64;o+=4){
            UINT32 val=(UINT32)cfg[o]|((UINT32)cfg[o+1]<<8)|((UINT32)cfg[o+2]<<16)|((UINT32)cfg[o+3]<<24);
            GmuxDeepStr("CFG+");GmuxDeepHex(o,2);GmuxDeepStr("=");GmuxDeepHex(val,8);GmuxDeepStr("\r\n");
        }
        if(vendor!=0x8086)continue;
        UINT32 asls=0;
        st=p->Pci.Read(p,EfiPciIoWidthUint32,0xFC,1,&asls);
        GmuxDeepVal("ASLS status=",st,16); if(!EFI_ERROR(st))GmuxDeepVal("ASLS=",asls,8);

        if (!EFI_ERROR(st) && asls != 0 && asls != 0xFFFFFFFFu) {
            // ASLS points to the Intel OpRegion, outside the PCI config space.
            // Only read it when the OpRegion signature is present.
            const volatile UINT8 *op=(const volatile UINT8*)(UINTN)asls;
            static const char signature[]="IntelGraphicsMem";
            BOOLEAN signatureOK=TRUE;
            for(UINTN z=0;z<sizeof(signature)-1;z++)
                if(op[z]!=(UINT8)signature[z]){signatureOK=FALSE;break;}
            GmuxDeepVal("OpRegion.signature_ok=",signatureOK,1);
            if(signatureOK){
                UINT32 kib=(UINT32)op[16]|((UINT32)op[17]<<8)|
                           ((UINT32)op[18]<<16)|((UINT32)op[19]<<24);
                GmuxDeepVal("OpRegion.size_kib=",kib,8);
                if(kib>=4 && kib<=64){
                    const volatile UINT8 *vbt=op+0x400;
                    BOOLEAN isVbt=(vbt[0]=='$' && vbt[1]=='V' && vbt[2]=='B' && vbt[3]=='T');
                    GmuxDeepVal("VBT.signature_ok=",isVbt,1);
                    if(isVbt){
                        UINTN sz=(UINTN)vbt[24]|((UINTN)vbt[25]<<8);
                        GmuxDeepVal("VBT.size=",sz,4);
                        if(sz>=32 && sz<=0x1800 && sz<=((UINTN)kib*1024-0x400)){
                            UINT8 sum=0;
                            for(UINTN n=0;n<sz;n++)sum=(UINT8)(sum+vbt[n]);
                            GmuxDeepVal("VBT.checksum_sum=",sum,2);
                            GmuxDeepStr("VBT.first_64_bytes=");
                            for(UINTN n=0;n<64 && n<sz;n++) {
                                GmuxDeepHex(vbt[n],2);
                                if(n+1<64)GmuxDeepStr(" ");
                            }
                            GmuxDeepStr("\r\n");
                        }
                    }
                }
            }
        }

        // Do not touch MMIO unless PCI memory decode is already enabled.
        if(!(cfg[4]&2)) { GmuxDeepStr("Intel MMIO skipped: MEM decode disabled\r\n");continue; }
        static const UINT32 offsets[]={
            0x45404, // PWR_WELL_CTL2
            0xC7200,0xC7204,0xC7208,0xC720C, // PPS status/control/on/off delays
            0x64000,0x64010, // DDI A buffer and AUX
            0x46000, // CDCLK_CTL
            0x70008, // PIPECONF A
            0x60000, // TRANS_CONF A / platform dependent
            0xC4000, // BLC PWM_CTL1 (platform dependent)
            0xC72010, // PP_DIVISOR
            0x64040, // DDI_BUF_CTL_B
            0x64080, // DDI_BUF_CTL_C
            0x64100, // DDI_BUF_CTL_D
            0x6C000, // DPLL_CTRL1 (gen9)
            0x6C004, // DPLL_CTRL2 (gen9)
            0x6C040, // DPLL_STATUS (gen9)
            0x70000  // PIPE A source (platform-specific)
        };
        static const char *names[]={
            "PWR_WELL_CTL2=","PP_STATUS=","PP_CONTROL=","PP_ON_DELAYS=","PP_OFF_DELAYS=",
            "DDI_BUF_CTL_A=","AUX_CTL_A=","CDCLK_CTL=","PIPECONF_A=","TRANS_CONF_A=","BLC_PWM_CTL1=",
            "PP_DIVISOR=","DDI_BUF_CTL_B=","DDI_BUF_CTL_C=","DDI_BUF_CTL_D=",
            "DPLL_CTRL1=","DPLL_CTRL2=","DPLL_STATUS=","PIPE_A_SRC="
        };
        for(UINTN j=0;j<sizeof(offsets)/sizeof(offsets[0]);j++) {
            UINT32 v=0xFFFFFFFFu;
            st=p->Mem.Read(p,EfiPciIoWidthUint32,0,offsets[j],1,&v);
            GmuxDeepStr(names[j]);
            if(EFI_ERROR(st)) {GmuxDeepStr("read_error ");GmuxDeepHex(st,16);}
            else GmuxDeepHex(v,8);
            GmuxDeepStr("\r\n");
        }
    }
    if(handles)bs->FreePool(handles);
}
static void GmuxDeepGop(EFI_BOOT_SERVICES *bs){
    EFI_GUID guid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_HANDLE *handles=NULL;UINTN count=0;
    EFI_STATUS st=bs->LocateHandleBuffer(ByProtocol,&guid,NULL,&count,&handles);
    GmuxDeepVal("GOP enumerate status=",st,16);
    if(EFI_ERROR(st))return;
    GmuxDeepVal("GOP count=",count,4);
    for(UINTN i=0;i<count;i++){
        EFI_GRAPHICS_OUTPUT_PROTOCOL *p=NULL;
        st=bs->HandleProtocol(handles[i],&guid,(VOID**)&p);
        GmuxDeepVal("GOP protocol status=",st,16);
        if(EFI_ERROR(st)||!p||!p->Mode||!p->Mode->Info)continue;
        GmuxDeepVal("GOP mode index=",p->Mode->Mode,4);
        GmuxDeepVal("GOP width=",p->Mode->Info->HorizontalResolution,8);
        GmuxDeepVal("GOP height=",p->Mode->Info->VerticalResolution,8);
        GmuxDeepVal("GOP framebuffer base=",p->Mode->FrameBufferBase,16);
        GmuxDeepVal("GOP framebuffer size=",p->Mode->FrameBufferSize,16);
    }
    if(handles)bs->FreePool(handles);
}
static void GmuxDeepGmux(EFI_BOOT_SERVICES *bs){
    volatile UINT8 *b=(volatile UINT8*)(UINTN)GMUX_PHYS_BASE;
    if(!GmuxDetect(b)) {GmuxDeepStr("GMUX unavailable\r\n");return;}
    UINT8 p[]={0x10,0x28,0x40};
    const char *n[]={"GMUX panel_route=","GMUX ddc_route=","GMUX external_route="};
    for(UINTN i=0;i<3;i++){
        UINT8 v=0;
        if(GmuxRead8(b,bs,p[i],&v))GmuxDeepVal(n[i],v,2);
        else {GmuxDeepStr(n[i]);GmuxDeepStr("READ_FAILED\r\n");}
    }
}
static void GmuxDeepSnapshot(EFI_BOOT_SERVICES *bs,EFI_HANDLE image,CHAR16 *filename,const char *stage){
    if(!HybridDiagActive)return;
    GmuxDeepLen=0;
    GmuxDeepStr("GMUX diagnostic read-only snapshot: ");GmuxDeepStr(stage);GmuxDeepStr("\r\n");
    GmuxDeepGmux(bs);
    GmuxDeepGop(bs);
    GmuxDeepPci(bs,image);
    EFI_STATUS st=_INT_WriteEspFile(bs,image,filename,GmuxDeepText,GmuxDeepLen);
    if(EFI_ERROR(st))HybridDiagStatus("Snapshot write failed=",st);
}

#endif
