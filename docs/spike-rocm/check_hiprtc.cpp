// RUNG 2: can the add-on compile a custom kernel for THIS card at runtime, no offline toolchain?
// This file contains ZERO __global__/__device__ code of its own. The kernel under test exists
// only as a source string, compiled by hiprtc at run time for whatever architecture
// hipGetDeviceProperties reports. Nothing here is hardcoded to gfx1201.
// Exit 0 = claim holds. Non-zero = it does not.
// ponytail: asserts + exit code, no framework.
#include <hip/hip_runtime.h>
#include <hip/hiprtc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>
#include "modules.h"

static int fails = 0;
#define CK(call) do { hipError_t _e=(call); if(_e!=hipSuccess){ \
  printf("  FAIL %s -> %d %s\n", #call, (int)_e, hipGetErrorName(_e)); fails++; } } while(0)

typedef std::chrono::steady_clock clk;
static double ms_since(clk::time_point t0){
  return std::chrono::duration<double,std::milli>(clk::now()-t0).count();
}

static const char* kGood = R"HIP(
extern "C" __global__ void style_apply(const float* in, float* out, float style_scale,
                                       float bias, int n)
{
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) out[i] = in[i] * style_scale + bias;
}
)HIP";

// deliberately broken: missing semicolon + misspelled parameter.
static const char* kBroken = R"HIP(
extern "C" __global__ void style_apply(const float* in, float* out, float style_scale,
                                       float bias, int n)
{
  int i = blockIdx.x * blockDim.x + threadIdx.x
  if (i < n) out[i] = in[i] * stlye_scale + bias;
}
)HIP";

int main(int argc, char** argv)
{
  bool useBroken = false, useWrongExpect = false;
  const char* cachePath = nullptr;
  const char* loadPath  = nullptr;
  const char* archOverride = nullptr;   // compile for a card this machine does not have
  for (int a = 1; a < argc; ++a) {
    if      (!strcmp(argv[a], "--broken"))        useBroken = true;
    else if (!strcmp(argv[a], "--wrong-expect"))  useWrongExpect = true;
    else if (!strcmp(argv[a], "--cache")      && a+1 < argc) cachePath = argv[++a];
    else if (!strcmp(argv[a], "--from-cache") && a+1 < argc) loadPath  = argv[++a];
    else if (!strcmp(argv[a], "--arch")       && a+1 < argc) archOverride = argv[++a];
  }

  int major=0, minor=0;
  hiprtcVersion(&major, &minor);
  int rtv=0, drv=0; hipRuntimeGetVersion(&rtv); hipDriverGetVersion(&drv);

  CK(hipSetDevice(0));
  hipDeviceProp_t pr{};
  CK(hipGetDeviceProperties(&pr, 0));
  std::string arch    = archOverride ? archOverride : pr.gcnArchName;  // <-- discovered, never hardcoded
  std::string archOpt = "--offload-arch=" + arch;
  printf("device      : %s\n", pr.name);
  printf("gcnArchName : %s   (%s)\n", arch.c_str(),
         archOverride ? "OVERRIDDEN: cross-compiling for a card not in this box" : "discovered at runtime");
  printf("hiprtc      : %d.%d   hipRuntimeGetVersion=%d hipDriverGetVersion=%d\n",
         major, minor, rtv, drv);
  printf("compile opt : %s\n\n", archOpt.c_str());
  if (arch.empty()) { printf("FAIL: gcnArchName empty\n"); return 3; }

  std::vector<char> code;
  double tCompile = 0.0;

  if (loadPath) {
    // ---------- cache hit: hiprtc never called ----------
    clk::time_point tc = clk::now();
    FILE* f = fopen(loadPath, "rb");
    if (!f) { printf("FAIL: cannot open cache %s\n", loadPath); return 3; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    code.resize((size_t)sz);
    if (fread(code.data(), 1, (size_t)sz, f) != (size_t)sz) { printf("FAIL: short read\n"); return 3; }
    fclose(f);
    tCompile = ms_since(tc);
    printf("CACHE HIT: read %ld bytes from %s   [%.1f ms, hiprtc never called]\n\n",
           sz, loadPath, tCompile);
  } else {
    // ---------- compile from source string ----------
    const char* src = useBroken ? kBroken : kGood;
    hiprtcProgram prog = nullptr;
    clk::time_point t0 = clk::now();
    hiprtcResult r = hiprtcCreateProgram(&prog, src, "style_apply.hip", 0, nullptr, nullptr);
    printf("hiprtcCreateProgram            -> %d %s\n", (int)r, hiprtcGetErrorString(r));
    if (r != HIPRTC_SUCCESS) return 3;

    const char* opts[] = { archOpt.c_str(), "-O3" };
    r = hiprtcCompileProgram(prog, 2, opts);
    tCompile = ms_since(t0);
    printf("hiprtcCompileProgram           -> %d %s   [%.1f ms]\n",
           (int)r, hiprtcGetErrorString(r), tCompile);

    size_t logSize = 0; hiprtcGetProgramLogSize(prog, &logSize);
    if (logSize > 1) {
      std::vector<char> log(logSize+1, 0);
      hiprtcGetProgramLog(prog, log.data());
      printf("---- hiprtc compiler log (%zu bytes) ----\n%s----------------------------------------\n",
             logSize, log.data());
    }
    if (r != HIPRTC_SUCCESS) {
      printf("\nRESULT: FAIL (compile error reported, log above)\n");
      return 4;
    }

    size_t codeSize = 0;
    hiprtcGetCodeSize(prog, &codeSize);
    code.resize(codeSize);
    r = hiprtcGetCode(prog, code.data());
    printf("hiprtcGetCode                  -> %d %s   %zu bytes, magic %02x %c%c%c\n",
           (int)r, hiprtcGetErrorString(r), codeSize,
           (unsigned char)code[0], code[1], code[2], code[3]);
    hiprtcDestroyProgram(&prog);
    if (r != HIPRTC_SUCCESS || codeSize == 0) { printf("FAIL: no code\n"); return 3; }

    if (cachePath) {   // what a shipped add-on does on first run
      FILE* f = fopen(cachePath, "wb");
      if (f) { fwrite(code.data(), 1, codeSize, f); fclose(f);
               printf("cached code object             -> %s (%zu bytes)\n", cachePath, codeSize); }
    }
  }

  if (archOverride) {   // cannot run it here; the point is that it compiled
    printf("\ncross-compile for %s produced %zu bytes. Not launched (wrong card).\n",
           arch.c_str(), code.size());
    dump_modules();
    printf("RESULT: PASS\n"); return 0;
  }

  // ---------- load + launch ----------
  clk::time_point t1 = clk::now();
  hipModule_t mod = nullptr;
  CK(hipModuleLoadData(&mod, code.data()));
  hipFunction_t fn = nullptr;
  CK(hipModuleGetFunction(&fn, mod, "style_apply"));
  double tLoad = ms_since(t1);
  printf("hipModuleLoadData+GetFunction  -> ok   [%.1f ms]\n", tLoad);
  if (!mod || !fn) { printf("FAIL: module/function\n"); return 3; }

  const int n = 1024;
  std::vector<float> hin(n), hout(n, -1.0f);
  for (int i = 0; i < n; ++i) hin[i] = (float)i;
  float style_scale = 0.5f, bias = 0.25f;     // both exact in fp32

  float *din = nullptr, *dout = nullptr;
  CK(hipMalloc(&din,  n*sizeof(float)));
  CK(hipMalloc(&dout, n*sizeof(float)));
  CK(hipMemcpy(din, hin.data(), n*sizeof(float), hipMemcpyHostToDevice));
  int nn = n;
  void* args[] = { &din, &dout, &style_scale, &bias, &nn };
  CK(hipModuleLaunchKernel(fn, (n+255)/256,1,1, 256,1,1, 0, nullptr, args, nullptr));
  CK(hipDeviceSynchronize());
  CK(hipMemcpy(hout.data(), dout, n*sizeof(float), hipMemcpyDeviceToHost));

  // ---------- verify EXACTLY ----------
  float fudge = useWrongExpect ? 1e-3f : 0.0f;     // teeth: a wrong expectation must fail
  int bad = 0, firstBad = -1;
  for (int i = 0; i < n; ++i) {
    float expect = hin[i] * style_scale + bias + fudge;
    if (hout[i] != expect) { if (firstBad < 0) firstBad = i; ++bad; }
  }
  printf("\nout[0..5] = %.4f %.4f %.4f %.4f %.4f %.4f   (expect 0.2500 0.7500 1.2500 1.7500 2.2500 2.7500)\n",
         hout[0],hout[1],hout[2],hout[3],hout[4],hout[5]);
  printf("exact mismatches: %d / %d%s\n", bad, n,
         useWrongExpect ? "   [--wrong-expect: the expectation is deliberately wrong]" : "");
  if (bad) {
    printf("  first bad i=%d got %.9g expected %.9g\n", firstBad, hout[firstBad],
           hin[firstBad]*style_scale + bias + fudge);
    fails++;
  }

  CK(hipFree(din)); CK(hipFree(dout)); CK(hipModuleUnload(mod));
  printf("\n%s=%.1f ms  load+getfunc=%.1f ms\n", loadPath ? "cacheread" : "compile", tCompile, tLoad);
  dump_modules();
  printf("RESULT: %s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
