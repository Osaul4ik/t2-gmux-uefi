#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/int_edp.h"
UINT16 _INT_Rd16(const UINT8* p){return p[0]|(p[1]<<8);}
UINT32 _INT_Rd32(const UINT8* p){return p[0]|(p[1]<<8)|(p[2]<<16)|((UINT32)p[3]<<24);}
VOID _INT_RepStr(_INT_Rep* r,const CHAR8* s){(void)r;fputs((const char*)s,stdout);}
VOID _INT_RepNl(_INT_Rep* r){(void)r;puts("");}
VOID _INT_RepDec(_INT_Rep* r,UINT64 v){(void)r;printf("%llu",(unsigned long long)v);}
int main(int argc,char**argv){ (void)argc;
  // argv: in.vbt out.vbt rate lanes psr
  FILE*f=fopen(argv[1],"rb"); static UINT8 b[8192]; size_t n=fread(b,1,sizeof b,f); fclose(f);
  int ok=_INT_VbtSetLink(b,n,atoi(argv[3]),atoi(argv[4]),atoi(argv[5]),(_INT_Rep*)1);
  if(!ok){puts("SetLink failed");return 2;}
  f=fopen(argv[2],"wb"); fwrite(b,1,n,f); fclose(f);
  // derive tests
  _INT_EdpCaps c; memset(&c,0,sizeof c);
  c.MaxLinkRate=0x14; c.MaxLanes=4; _INT_EdpDeriveLink(&c); if(c.VbtRate!=2||c.VbtLanes!=3){puts("derive1 FAIL");return 3;}
  c.MaxLinkRate=0x0A; c.MaxLanes=2; _INT_EdpDeriveLink(&c); if(c.VbtRate!=1||c.VbtLanes!=1){puts("derive2 FAIL");return 3;}
  c.MaxLinkRate=0x06; c.MaxLanes=1; _INT_EdpDeriveLink(&c); if(c.VbtRate!=0||c.VbtLanes!=0){puts("derive3 FAIL");return 3;}
  c.EdpRev=3; c.MaxLinkRate=0x0A; c.RateTable[0]=8100; c.RateTable[1]=13500; c.RateTable[2]=21600; c.RateTable[3]=27000; c.MaxLanes=4;
  _INT_EdpDeriveLink(&c); if(c.VbtRate!=2){puts("derive4 FAIL");return 3;}
  puts("derive OK");
  return 0;
}
