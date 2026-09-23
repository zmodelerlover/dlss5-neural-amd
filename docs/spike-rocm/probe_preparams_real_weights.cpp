// AUDIT rung: shipped k_pre_block + REAL trained block0 weights.
// Teeth: repeat-diff must be 0; +52 dead pad must change 0; controls must move.
#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CK(x) do{ hipError_t e=(x); if(e!=hipSuccess){ \
  printf("FAIL %d %s -> %s\n",__LINE__,#x,hipGetErrorString(e)); exit(2);} }while(0)
struct PreParams { const void* inA; void* out; void* blob; int d0,d1;
  float inScale; unsigned seed; float c0,c1,c2; unsigned pad52;
  void* out2; const void* inB; float c3,c4; };
static const size_t BUF = 32u<<20;
static void *dInA,*dOut,*dBlob; static hipFunction_t k;
static void run(int which,float val,unsigned pad,std::vector<unsigned char>& out){
  CK(hipMemset(dOut,0,BUF));
  PreParams p{}; p.inA=dInA; p.out=dOut; p.blob=dBlob; p.d0=64; p.d1=64;
  p.inScale=1.0f/16.0f; p.seed=0xDEADBEEFu;
  p.c0=0.25f; p.c1=-0.5f; p.c2=0.0f; p.pad52=pad;
  p.out2=nullptr; p.inB=nullptr; p.c3=0.75f; p.c4=-0.125f;
  if(which==40)p.c0=val; else if(which==44)p.c1=val;
  else if(which==48)p.c2=val; else if(which==72)p.c3=val; else if(which==76)p.c4=val;
  size_t sz=sizeof(p);
  void* ex[]={HIP_LAUNCH_PARAM_BUFFER_POINTER,&p,HIP_LAUNCH_PARAM_BUFFER_SIZE,&sz,HIP_LAUNCH_PARAM_END};
  CK(hipModuleLaunchKernel(k,8,8,1, 256,1,1, 0,0,nullptr,ex));
  CK(hipDeviceSynchronize());
  out.assign(BUF,0); CK(hipMemcpy(out.data(),dOut,BUF,hipMemcpyDeviceToHost));
}
static size_t diff(const std::vector<unsigned char>&x,const std::vector<unsigned char>&y){
  size_t c=0; for(size_t i=0;i<x.size();i++) if(x[i]!=y[i]) c++; return c; }
int main(int argc,char**argv){
  const char* co = argc>1?argv[1]:"../gfx1201.co";
  int USE_REAL = argc>2?atoi(argv[2]):1;
  FILE* f=fopen(co,"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  std::vector<char> img(n); fread(img.data(),1,n,f); fclose(f);
  FILE* g=fopen("D:/pcsx2-v2.8.2-test/dlssnr_on_amd_weights.bin","rb");
  if(!g){printf("FAIL: no weights file\n");return 2;}
  std::vector<unsigned char> real(21696); fseek(g,5673,SEEK_SET);
  if(fread(real.data(),1,21696,g)!=21696){printf("FAIL: short read\n");return 2;}
  fclose(g);
  hipModule_t mod; CK(hipModuleLoadData(&mod,img.data()));
  CK(hipModuleGetFunction(&k,mod,"_Z21k_pre_block_1h_32_fp89PreParams"));
  CK(hipMalloc(&dInA,BUF)); CK(hipMalloc(&dOut,BUF)); CK(hipMalloc(&dBlob,BUF));
  unsigned s=0x12345678u;
  std::vector<float> im(BUF/4);
  for(size_t i=0;i<im.size();i++){ s=s*1664525u+1013904223u; im[i]=(float)((s>>8)&0xFFFFFF)/16777216.0f; }
  CK(hipMemcpy(dInA,im.data(),BUF,hipMemcpyHostToDevice));
  std::vector<unsigned short> syn(BUF/2);
  for(size_t i=0;i<syn.size();i++){ s=s*1664525u+1013904223u; syn[i]=(unsigned short)(0x3000u+((s>>16)&0x03FEu)); }
  std::vector<unsigned char> a((unsigned char*)syn.data(),(unsigned char*)syn.data()+BUF);
  if(USE_REAL) for(size_t i=0;i<real.size();i++) a[i]=real[i];
  CK(hipMemcpy(dBlob,a.data(),BUF,hipMemcpyHostToDevice));
  printf("arena = %s\n", USE_REAL?"REAL trained block0.layer0.layer (21696 B) + synthetic tail"
                                 :"ALL SYNTHETIC (the original harness)");
  std::vector<unsigned char> base,rep,t; run(-1,0,0,base); run(-1,0,0,rep);
  size_t nz=0; int hist[256]={0};
  for(size_t i=0;i<base.size();i++) if(base[i]){nz++;hist[base[i]]++;}
  int b1=0,b2=0; for(int j=0;j<256;j++) if(hist[j]>hist[b1]) b1=j;
  hist[b1]=0; for(int j=0;j<256;j++) if(hist[j]>hist[b2]) b2=j;
  size_t rd=diff(base,rep);
  printf("nonzero=%zu  top two bytes 0x%02x/0x%02x  repeat-diff=%zu\n",nz,b1,b2,rd);
  int offs[]={40,44,48,72,76}; const char* nm[]={"+40 ctl0","+44 ctl1","+48 SLOT","+72 ctl2","+76 ctl3"};
  size_t d[5];
  for(int i=0;i<5;i++){ run(offs[i],0.9f,0,t); d[i]=diff(base,t);
    printf("  %-9s val=0.9   changed=%zu\n",nm[i],d[i]); }
  run(-1,0,12345u,t); size_t dpad=diff(base,t);
  printf("  +52 DEADPAD          changed=%zu\n",dpad);
  printf("  -- +48 dose response --\n");
  for(float v : {0.001f,0.01f,0.1f,0.9f}){ run(48,v,0,t);
    printf("     +48=%-6.3f        changed=%zu\n",v,diff(base,t)); }
  int rc=0;
  if(!nz){printf("FAIL no output\n");rc=1;}
  if(rd){printf("FAIL nondeterministic -> every count above is noise\n");rc=1;}
  if(dpad){printf("FAIL +52 dead pad reached the output\n");rc=1;}
  if(!(d[0]||d[1]||d[3]||d[4])){printf("FAIL known controls inert\n");rc=1;}
  if(!d[2]){printf("FAIL +48 inert with these weights\n");rc=1;}
  printf(rc?"RESULT: FAIL\n":"RESULT: PASS\n");
  return rc;
}
