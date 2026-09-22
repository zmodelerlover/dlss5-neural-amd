// RUNG 1: can WE load the shipped runtime's own gfx1201 code object and launch its kernels?
// Fails (non-zero) if the module will not load, a kernel will not resolve, a launch errors,
// or the kernel does not write exactly what its ISA says it writes.
//
// usage: check.exe <gfx1201.co> [symbol-override-for-probe]
//   the 2nd arg exists so the check can be shown to have teeth (point it at a bogus name).

#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <cmath>

static int fails = 0;
#define HIPCK(call) do { hipError_t _e = (call); if (_e != hipSuccess) { \
  printf("  FAIL %s -> %d %s\n", #call, (int)_e, hipGetErrorString(_e)); ++fails; } } while (0)
#define WANT(cond, msg) do { if (!(cond)) { printf("  FAIL %s\n", msg); ++fails; } \
  else printf("  ok   %s\n", msg); } while (0)

// _Z6k_mean10MeanParams, by_value 32 B, reconstructed from the gfx1201 ISA (see report).
struct MeanParams {
    const void* src;   // +0   float3* image
    int         pitch; // +8   row stride in float3 elements
    int         h;     // +12  rows
    int         w;     // +16  columns
    int         hole;  // +20  never loaded by the kernel
    float*      out;   // +24  global_atomic_add_f32 target
};
static_assert(sizeof(MeanParams) == 32, "MeanParams must be 32 bytes");

// exact fp32 bit patterns of the Rec.709 weights found in the kernel
static float bits(unsigned u) { float f; std::memcpy(&f, &u, 4); return f; }

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : "../gfx1201.co";
    const char* probeSym = (argc > 2) ? argv[2] : "_Z13k_align_probePh";

    FILE* f = fopen(path, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> co(n);
    if (fread(co.data(), 1, n, f) != (size_t)n) { printf("FAIL: short read\n"); return 2; }
    fclose(f);
    printf("image: %s  %ld bytes  magic %02x %02x %02x %02x\n",
           path, n, co[0], co[1], co[2], co[3]);

    hipDeviceProp_t p{};
    HIPCK(hipGetDeviceProperties(&p, 0));
    printf("device: %s  %s\n", p.name, p.gcnArchName);

    // ---- A. load THEIR bare code object -------------------------------------------------
    printf("[A] hipModuleLoadData on the bare carved ELF\n");
    hipModule_t mod = nullptr;
    HIPCK(hipModuleLoadData(&mod, co.data()));
    if (!mod) { printf("RESULT: FAIL (no module)\n"); return 1; }

    hipFunction_t probe = nullptr;
    hipError_t ge = hipModuleGetFunction(&probe, mod, probeSym);
    printf("  hipModuleGetFunction(\"%s\") -> %d %s\n", probeSym, (int)ge, hipGetErrorString(ge));
    if (ge != hipSuccess) { printf("RESULT: FAIL (symbol)\n"); return 1; }

    // ---- B. launch it. ISA: lane0 stores u16 0x2211 at ptr+1 (unaligned, by design) -----
    printf("[B] launch THEIR _Z13k_align_probePh over a 0xCC buffer\n");
    unsigned char* dbuf = nullptr;
    HIPCK(hipMalloc((void**)&dbuf, 64));
    HIPCK(hipMemset(dbuf, 0xCC, 64));          // null stream, same as the launch below
    void* pargs[] = { &dbuf };
    HIPCK(hipModuleLaunchKernel(probe, 1,1,1, 64,1,1, 0, nullptr, pargs, nullptr));
    HIPCK(hipDeviceSynchronize());
    unsigned char host[8] = {0};
    HIPCK(hipMemcpy(host, dbuf, 8, hipMemcpyDeviceToHost));
    printf("  buf[0..7] = %02x %02x %02x %02x %02x %02x %02x %02x   (expect cc 11 22 cc ...)\n",
           host[0],host[1],host[2],host[3],host[4],host[5],host[6],host[7]);
    WANT(host[0]==0xcc && host[1]==0x11 && host[2]==0x22 && host[3]==0xcc,
         "their kernel ran and wrote exactly what its ISA says");

    // ---- C. teeth: the same code path must FAIL on a bogus symbol and a corrupt image ----
    printf("[C] negative controls (these MUST fail)\n");
    hipFunction_t bogus = nullptr;
    hipError_t be = hipModuleGetFunction(&bogus, mod, "_Z20k_this_does_not_existPh");
    printf("  getFunction(bogus) -> %d %s\n", (int)be, hipGetErrorString(be));
    WANT(be != hipSuccess, "bogus symbol is rejected");
    std::vector<unsigned char> broken = co;
    broken[0] = 0x00;                                  // kill the ELF magic
    hipModule_t bad = nullptr;
    hipError_t le = hipModuleLoadData(&bad, broken.data());
    printf("  loadData(corrupt) -> %d %s\n", (int)le, hipGetErrorString(le));
    WANT(le != hipSuccess, "corrupt image is rejected");
    (void)hipGetLastError();

    // ---- D. real work: _Z6k_mean10MeanParams with a reconstructed 32-byte struct ---------
    // ISA says: mean over h*w of (0.2126 r + 0.7152 g + 0.0722 b), row pitch `pitch`,
    // grid-stride loop, 256-wide LDS reduction, lane0 atomically adds blockSum/(h*w).
    printf("[D] launch THEIR _Z6k_mean10MeanParams with a reconstructed MeanParams\n");
    hipFunction_t kmean = nullptr;
    HIPCK(hipModuleGetFunction(&kmean, mod, "_Z6k_mean10MeanParams"));
    if (!kmean) { printf("RESULT: FAIL (k_mean symbol)\n"); return 1; }

    const int W = 64, H = 48, PITCH = 80;   // deliberately all different: a swapped
                                            // field in the struct gives a different answer
    std::vector<float> img((size_t)PITCH * H * 3, 0.0f);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < PITCH; ++x) {
        float* px = &img[((size_t)y * PITCH + x) * 3];
        if (x < W) { px[0] = 0.001f*x; px[1] = 0.002f*y; px[2] = 0.5f; }
        else       { px[0] = px[1] = px[2] = 1000.0f; }   // padding poison
      }
    const float wr = bits(0x3e59b3d0), wg = bits(0x3f371759), wb = bits(0x3d93dd98);
    double ref = 0.0;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const float* px = &img[((size_t)y * PITCH + x) * 3];
        ref += (double)(wr*px[0] + wg*px[1] + wb*px[2]);
      }
    ref /= (double)(W*H);

    float* dimg = nullptr; float* dout = nullptr;
    HIPCK(hipMalloc((void**)&dimg, img.size()*4));
    HIPCK(hipMalloc((void**)&dout, 4));
    HIPCK(hipMemcpy(dimg, img.data(), img.size()*4, hipMemcpyHostToDevice));
    HIPCK(hipMemset(dout, 0, 4));

    MeanParams mp{};
    mp.src = dimg; mp.pitch = PITCH; mp.h = H; mp.w = W; mp.hole = 0; mp.out = dout;
    size_t msz = sizeof(mp);
    void* cfg[] = { HIP_LAUNCH_PARAM_BUFFER_POINTER, &mp,
                    HIP_LAUNCH_PARAM_BUFFER_SIZE, &msz,
                    HIP_LAUNCH_PARAM_END };
    // 4 blocks x 256 threads over 3072 pixels -> 3 grid-stride iterations, LDS 1024 B.
    HIPCK(hipModuleLaunchKernel(kmean, 4,1,1, 256,1,1, 0, nullptr, nullptr, cfg));
    HIPCK(hipDeviceSynchronize());
    float got = -1.0f;
    HIPCK(hipMemcpy(&got, dout, 4, hipMemcpyDeviceToHost));
    printf("  k_mean -> %.8f   cpu reference %.8f   (padding poisoned with 1000.0)\n",
           got, ref);
    WANT(std::fabs(got - (float)ref) < 1e-5f * (float)(ref > 0 ? ref : 1.0),
         "k_mean returned the predicted Rec.709 mean -> the 32-byte layout is right");

    HIPCK(hipFree(dbuf)); HIPCK(hipFree(dimg)); HIPCK(hipFree(dout));
    HIPCK(hipModuleUnload(mod));
    printf("RESULT: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
