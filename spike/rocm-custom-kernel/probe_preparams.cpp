// Launch the SHIPPED k_pre_block (carved gfx1201 code object) and measure which
// PreParams scalar fields actually reach the kernel's output.
#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CK(x) do{ hipError_t e=(x); if(e!=hipSuccess){ \
  printf("FAIL %d %s -> %s\n",__LINE__,#x,hipGetErrorString(e)); exit(2);} }while(0)

struct PreParams {
  const void* inA;  void* out;  void* blob;      // +0 +8 +16
  int d0, d1;                                    // +24 +28
  float inScale;  unsigned seed;                 // +32 +36
  float c0, c1, c2;  unsigned pad52;             // +40 +44 +48 +52
  void* out2;  const void* inB;                  // +56 +64
  float c3, c4;                                  // +72 +76
};
static_assert(sizeof(PreParams)==80,"");

static const size_t BUF = 32u<<20;
int main(int argc,char**argv){
  const char* co = argc>1?argv[1]:"gfx1201.co";
  FILE* f=fopen(co,"rb"); if(!f){printf("FAIL: no %s\n",co);return 2;}
  fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  std::vector<char> img(n); fread(img.data(),1,n,f); fclose(f);
  hipModule_t mod; hipFunction_t k;
  CK(hipModuleLoadData(&mod,img.data()));
  CK(hipModuleGetFunction(&k,mod,"_Z21k_pre_block_1h_32_fp89PreParams"));

  void *dInA,*dOut,*dBlob;
  CK(hipMalloc(&dInA,BUF)); CK(hipMalloc(&dOut,BUF)); CK(hipMalloc(&dBlob,BUF));
  unsigned s=0x12345678u;
  // image: real floats in [0,1) so the float3 loads are never NaN
  std::vector<float> img3(BUF/4);
  for(size_t i=0;i<img3.size();i++){ s=s*1664525u+1013904223u; img3[i]=(float)((s>>8)&0xFFFFFF)/16777216.0f; }
  CK(hipMemcpy(dInA,img3.data(),BUF,hipMemcpyHostToDevice));
  // blob: fp16 halfwords ~0.125..0.25, and every BYTE stays a finite e4m3
  // (never 0x7F/0xFF) because the same arena is also read as fp8.
  std::vector<unsigned short> w(BUF/2);
  for(size_t i=0;i<w.size();i++){ s=s*1664525u+1013904223u; w[i]=(unsigned short)(0x3000u+((s>>16)&0x03FEu)); }
  CK(hipMemcpy(dBlob,w.data(),BUF,hipMemcpyHostToDevice));

  auto run=[&](int which,float val,int gx,int gy,int bx,std::vector<unsigned char>& out){
    CK(hipMemset(dOut,0,BUF));
    PreParams p{};
    p.inA=dInA; p.out=dOut; p.blob=dBlob; p.d0=64; p.d1=64;
    p.inScale=1.0f/16.0f; p.seed=0xDEADBEEFu;
    p.c0=0.25f; p.c1=-0.5f; p.c2=0.0f; p.pad52=0u;
    p.out2=nullptr; p.inB=nullptr; p.c3=0.75f; p.c4=-0.125f;
    switch(which){
      case 32: p.inScale=val; break;  case 36: p.seed=(unsigned)val; break;
      case 40: p.c0=val; break;       case 44: p.c1=val; break;
      case 48: p.c2=val; break;       case 52: p.pad52=(unsigned)val; break;
      case 72: p.c3=val; break;       case 76: p.c4=val; break;
      default: break;
    }
    size_t sz=sizeof(p);
    void* extra[]={HIP_LAUNCH_PARAM_BUFFER_POINTER,&p,HIP_LAUNCH_PARAM_BUFFER_SIZE,&sz,HIP_LAUNCH_PARAM_END};
    CK(hipModuleLaunchKernel(k,gx,gy,1, bx,1,1, 0,0,nullptr,extra));
    CK(hipDeviceSynchronize());
    out.assign(BUF,0);
    CK(hipMemcpy(out.data(),dOut,BUF,hipMemcpyDeviceToHost));
  };
  auto diff=[](const std::vector<unsigned char>&x,const std::vector<unsigned char>&y){
    size_t c=0; for(size_t i=0;i<x.size();i++) if(x[i]!=y[i]) c++; return c; };

  int GX=argc>2?atoi(argv[2]):1, GY=argc>3?atoi(argv[3]):1, BX=argc>4?atoi(argv[4]):256;
  std::vector<unsigned char> base,rep,t;
  run(-1,0,GX,GY,BX,base); run(-1,0,GX,GY,BX,rep);
  size_t nz=0,last=0; for(size_t i=0;i<base.size();i++) if(base[i]){nz++;last=i;}
  printf("grid=%dx%d blk=%d nonzero=%zu (last 0x%zx) repeat-diff=%zu\n",GX,GY,BX,nz,last,diff(base,rep));
  {int hist[256]={0}; for(size_t i2=0;i2<base.size();i2++) if(base[i2]) hist[base[i2]]++;
   printf("  top vals:"); for(int q=0;q<6;q++){int bi=0;for(int j=0;j<256;j++) if(hist[j]>hist[bi]) bi=j; if(!hist[bi])break; printf(" 0x%02x x%d",bi,hist[bi]); hist[bi]=0;} printf("\n");}
  struct{int off;float v;const char*tag;} pr[]={
    {32,0.5f,"+32 inScale  (eng+0x34)"},{36,7.0f,"+36 seed     (eng+0x38)"},
    {40,0.9f,"+40 ctl0     (eng+0x20)"},{44,0.9f,"+44 ctl1     (eng+0x24)"},
    {48,1.0f,"+48 HOST-ZERO SLOT     "},{52,12345.f,"+52 padding hole       "},
    {72,0.9f,"+72 ctl2     (eng+0x28)"},{76,0.9f,"+76 ctl3     (eng+0x2c)"}};
  size_t dv[8];
  for(int i=0;i<8;i++){ run(pr[i].off,pr[i].v,GX,GY,BX,t); dv[i]=diff(base,t);
    printf("  %s  changed=%zu\n",pr[i].tag,dv[i]); }
  int rc=0;
  if(nz==0){printf("FAIL: no output\n");rc=1;}
  if(diff(base,rep)!=0){printf("FAIL: nondeterministic\n");rc=1;}
  if(!(dv[2]||dv[3]||dv[6]||dv[7])){printf("FAIL: known controls inert -> harness not exercising net\n");rc=1;}
  if(!dv[4]){printf("FAIL: +48 inert -> no free live float slot in PreParams\n");rc=1;}
  if(dv[5]){printf("FAIL: +52 reaches the output -> it is not padding\n");rc=1;}
  printf(rc?"RESULT: FAIL\n":"RESULT: PASS  (+48 is a live 5th float input; +52 is dead padding)\n");
  return rc;
}
