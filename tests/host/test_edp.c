// Simulated eDP panel behind the iGPU AUX-A registers: exercises lib/int_edp.c's
// register sequence, AUX packing/unpacking, retries, DPCD + EDID reads.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../include/int_edp.h"

UINT16 _INT_Rd16(const UINT8* p){return p[0]|(p[1]<<8);}
UINT32 _INT_Rd32(const UINT8* p){return p[0]|(p[1]<<8)|(p[2]<<16)|((UINT32)p[3]<<24);}
VOID _INT_RepStr(_INT_Rep* r,const CHAR8* s){(void)r;fputs((const char*)s,stdout);}
VOID _INT_RepNl(_INT_Rep* r){(void)r;puts("");}
VOID _INT_RepDec(_INT_Rep* r,UINT64 v){(void)r;printf("%llu",(unsigned long long)v);}
VOID _INT_RepHex(_INT_Rep* r,UINT64 v,UINTN d){(void)r;printf("%0*llx",(int)d,(unsigned long long)v);}

static UINT32 regs[0x100000/4];
static UINT8 dpcd[0x800], edid[128];
static int i2c_off, fail_first = 0, aux_calls = 0, pp_forced = 0, pp_final_vdd = -1;
static int panel_dead = 0;

static EFI_STATUS Stall(UINTN us){(void)us;return 0;}
static EFI_STATUS PciRead(EFI_PCI_IO_PROTOCOL*p,int w,UINT32 off,UINTN c,void*b){(void)p;(void)w;(void)c;
  UINT32 v = off==4?2: off==0x10?0xA0000004u: 0; *(UINT32*)b=v; return 0;}
static EFI_STATUS Attr(EFI_PCI_IO_PROTOCOL*p,int o,UINT64 a,UINT64*r){(void)p;(void)o;(void)a;(void)r;return 0;}

static void aux_run(void){
  UINT8 tx[20]={0}; UINT32 ctl=regs[0x64010/4]&~((1u<<28)|(1u<<25)); /* W1C status bits */ int n=(ctl>>20)&0x1f; aux_calls++;
  for(int i=0;i<n;i++) tx[i]=regs[(0x64014+4*(i/4))/4]>>(24-8*(i%4));
  UINT8 rx[20]={0}; int rn=0; int err=0;
  if(panel_dead || (fail_first && aux_calls==1)) err=1;
  else {
    int req=tx[0]>>4, addr=((tx[0]&0xF)<<16)|(tx[1]<<8)|tx[2], len=tx[3]+1;
    if(req==0x9){ rx[0]=0; for(int i=0;i<len;i++) rx[1+i]=dpcd[addr+i]; rn=len+1; }
    else if((req&3)==0 && (req&8)==0){ // i2c write
      if(addr!=0x50){err=1;} else { i2c_off=tx[4]; rx[0]=0; rn=1; } }
    else if((req&3)==1){ if(addr!=0x50){err=1;} else { rx[0]=0; for(int i=0;i<len;i++) rx[1+i]=edid[i2c_off+i]; i2c_off+=len; rn=len+1; } }
    else err=1;
  }
  if(err){ regs[0x64010/4]=(ctl&~(1u<<31))|(1u<<28); return; }
  for(int i=0;i<rn;i++){ UINT32*r=&regs[(0x64014+4*(i/4))/4]; if(i%4==0)*r=0; *r|=(UINT32)rx[i]<<(24-8*(i%4)); }
  regs[0x64010/4]=(ctl&~(1u<<31)&~(0x1fu<<20))|(1u<<30)|((UINT32)rn<<20);
}
static EFI_STATUS MemRead(EFI_PCI_IO_PROTOCOL*p,int w,UINT8 bar,UINT64 off,UINTN c,void*b){(void)p;(void)w;(void)bar;(void)c;*(UINT32*)b=regs[off/4];return 0;}
static EFI_STATUS MemWrite(EFI_PCI_IO_PROTOCOL*p,int w,UINT8 bar,UINT64 off,UINTN c,void*b){(void)p;(void)w;(void)bar;(void)c;
  UINT32 v=*(UINT32*)b;
  if(off==0x64010){ // status bits are write-1-to-clear, SEND_BUSY starts a transfer
    if(v&(1u<<31)){ regs[off/4]=v; aux_run(); return 0; }
    regs[off/4]&=~(v&((1u<<30)|(1u<<28)|(1u<<25))); return 0; }
  if(off==0x45404){ UINT32 st=0; if(v&(1u<<29))st|=1u<<28; if(v&(1u<<3))st|=1u<<2; regs[off/4]=v|st; return 0; }
  if(off==0xC7204){ pp_forced += !!(v&8) ; pp_final_vdd=!!(v&8); if((v>>16)!=0xABCD){puts("PP write without unlock key");exit(9);} regs[off/4]=v&0xFFFF; return 0; }
  regs[off/4]=v; return 0; }

static EFI_PCI_IO_PROTOCOL pci={{PciRead},{MemRead,MemWrite},Attr};
static EFI_BOOT_SERVICES bs={Stall};
EFI_PCI_IO_PROTOCOL* _INT_FindIgpu(EFI_BOOT_SERVICES*b,EFI_HANDLE h,UINT32*id,EFI_STATUS*st){(void)b;(void)h;*id=0x8086;*st=0;return &pci;}

#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); return 1; } }while(0)

static void panel_setup(UINT8 rate,UINT8 lanes,UINT8 edprev,UINT8 psr){
  memset(dpcd,0,sizeof dpcd); dpcd[0]=0x14; dpcd[1]=rate; dpcd[2]=lanes|0x80; dpcd[0x0D]=0x08;
  dpcd[0x700]=edprev; dpcd[0x70]=psr;
  if(edprev>=3){ UINT16 t[4]={8100,13500,21600,27000}; for(int i=0;i<4;i++){dpcd[0x10+2*i]=t[i]&255;dpcd[0x11+2*i]=t[i]>>8;} }
  memset(edid,0,128); edid[1]=edid[2]=edid[3]=edid[4]=edid[5]=edid[6]=0xFF; edid[8]=6; edid[9]=0x10;
  for(int i=0;i<18;i++) edid[54+i]=(i==0)?0x4C:(i==1)?0x73:0x11+i; // DTD with nonzero clock
  UINT8 s=0; for(int i=0;i<127;i++) s+=edid[i]; edid[127]=(UINT8)-s;
  memset(regs,0,sizeof regs); i2c_off=0; aux_calls=0; pp_forced=0; pp_final_vdd=-1;
}

int main(void){
  _INT_EdpCaps c;
  // 1) MBP-like panel: HBR2, 4 lanes, eDP 1.4 rate table, PSR present; panel initially OFF
  panel_setup(0x14,4,3,1); fail_first=0;
  CHECK(_INT_EdpProbe(&bs,NULL,&c,(_INT_Rep*)1)==0);
  CHECK(c.Valid && c.MaxLanes==4 && c.VbtLanes==3 && c.VbtRate==2 && c.PsrSupport==1 && c.EdpRev==3);
  CHECK(c.HasEdid && memcmp(c.Edid,edid,128)==0);
  CHECK(pp_forced==1 && pp_final_vdd==0);          // VDD forced for AUX, released afterwards
  CHECK((regs[0x45404/4]&(1u<<28)) && (regs[0x45404/4]&(1u<<2)));
  puts("case 1 OK");
  // 2) 2-lane HBR panel, legacy DPCD only, panel already ON -> no PP_CONTROL writes
  panel_setup(0x0A,2,0,0); regs[0xC7200/4]=1u<<31;
  CHECK(_INT_EdpProbe(&bs,NULL,&c,(_INT_Rep*)1)==0);
  CHECK(c.VbtLanes==1 && c.VbtRate==1 && c.PsrSupport==0 && pp_forced==0 && pp_final_vdd==-1);
  puts("case 2 OK");
  // 3) first AUX attempt times out -> retry path
  panel_setup(0x14,4,3,1); fail_first=1;
  CHECK(_INT_EdpProbe(&bs,NULL,&c,(_INT_Rep*)1)==0 && c.Valid);
  puts("case 3 (retry) OK");
  // 4) dead panel -> clean failure, defaults untouched, VDD released
  panel_setup(0x14,4,3,1); fail_first=0; panel_dead=1;
  CHECK(_INT_EdpProbe(&bs,NULL,&c,(_INT_Rep*)1)!=0 && !c.Valid && !c.HasEdid && pp_final_vdd==0);
  puts("case 4 (dead panel) OK");
  // 5) registers read all ones (display well down) -> nothing written
  panel_setup(0x14,4,3,1); panel_dead=0; for(size_t i=0;i<sizeof regs/4;i++) regs[i]=0xFFFFFFFF;
  CHECK(_INT_EdpProbe(&bs,NULL,&c,(_INT_Rep*)1)!=0 && aux_calls==0);
  puts("case 5 (all ones) OK");
  puts("ALL OK");
  return 0;
}
