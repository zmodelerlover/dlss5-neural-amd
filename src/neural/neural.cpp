// ReShade add-on: runs the DLSS-NR network over the presented frame on an AMD GPU via HIP.
// D3D12 only. One core, one table row per target.

#include <imgui.h>

#include <reshade.hpp>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <bcrypt.h>
#include <wrl/client.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace reshade::api;

namespace
{

enum class Tier
{
    A,
    B,
    C,
};

struct Profile
{
    const wchar_t *exe;
    Tier tier;
    float scale;
    const char *note;
};

constexpr Profile kTargets[] = {
    { L"pcsx2-qt.exe", Tier::C, 1.0f,
      "PCSX2, Direct3D 12" },

    { L"rpcs3.exe", Tier::C, 1.0f,
      "RPCS3, Direct3D 12" },

    { L"NFS16.exe", Tier::C, 1.0f,
      "NFS 2015" },

    { nullptr, Tier::C, 1.0f,
      "uncatalogued target" },
};

const Profile &ProfileForThisProcess()
{
    static const Profile &chosen = []() -> const Profile & {
        wchar_t path[MAX_PATH] {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const std::wstring exe = std::filesystem::path(path).filename().wstring();
        for (const auto &t : kTargets)
            if (t.exe != nullptr && _wcsicmp(t.exe, exe.c_str()) == 0)
                return t;
        return kTargets[std::size(kTargets) - 1];
    }();
    return chosen;
}

std::mutex g_log_mutex;
FILE *g_log = nullptr;

void Log(const char *fmt, ...)
{
    std::lock_guard guard(g_log_mutex);
    if (g_log == nullptr)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

std::filesystem::path ExeDirectory()
{
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

constexpr char kCopyShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float intensity; uint pad; };
float3 ToLinear(float3 c){ return c <= 0.04045 ? c/12.92 : pow(abs(c+0.055)/1.055, 2.4); }
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 float4 c = src.SampleLevel(smp, (float2(p.xy)+0.5)/float2(dw,dh), 0);
 float3 v = c.rgb;
 if (mode != 0) v = ToLinear(saturate(v)) * k;
 dst[p.xy] = float4(v, c.a);
})";

constexpr char kDepthShader[] = R"(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
SamplerState smp : register(s0);
cbuffer Extent : register(b0) { uint dw; uint dh; uint sw; uint sh; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 uint2 t = min(uint2((float2(p.xy)+0.5)*float2(sw,sh)/float2(dw,dh)), uint2(sw-1,sh-1));
 dst[p.xy] = src.Load(int3(t,0));
})";

constexpr char kComposeShader[] = R"(
Texture2D<float4> full : register(t0);
Texture2D<float4> nr   : register(t1);
Texture2D<float4> base : register(t2);
RWTexture2D<float4> dst : register(u0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float intensity; uint pad; };
float3 ToLinear(float3 c){ return c <= 0.04045 ? c/12.92 : pow(abs(c+0.055)/1.055, 2.4); }
float3 ToSrgb(float3 c){ return c <= 0.0031308 ? c*12.92 : 1.055*pow(abs(c), 1.0/2.4) - 0.055; }
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 float2 uv = (float2(p.xy)+0.5)/float2(dw,dh);
 float3 fix = (nr.SampleLevel(smp,uv,0) - base.SampleLevel(smp,uv,0)).rgb * intensity;
 float3 c = full.Load(int3(p.xy,0)).rgb;
 float3 v = (mode != 0) ? ToLinear(saturate(c)) * k : c;
 v = v + fix;
 if (mode != 0) v = ToSrgb(saturate(v / max(k, 1e-6)));
 dst[p.xy] = float4(saturate(v), 1.0);
})";

// Optical flow, in three passes. The PS2 never computed per-pixel motion, so there is nothing to
// capture: this estimates it by comparing two frames, which is what the NVIDIA route does here too
// -- its motion comes from a shader (LumeniteFX), not from the game. Estimated motion is wrong in
// the places you would expect: reflections, fire, moving shadows, and anything newly revealed.
//
// Convention, matching the Feeder's: the vector points from where a pixel is now to where it was,
// so prev_uv = cur_uv + motion. Block matching produces exactly that sign, no flip needed.
constexpr char kLumaShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float> dst : register(u0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float extra; uint pad; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 float2 uv = (float2(p.xy)+0.5)/float2(dw,dh);
 float3 c = src.SampleLevel(smp,uv,0).rgb;
 dst[p.xy] = dot(c, float3(0.299,0.587,0.114));
})";

// Search radius 4 with a 3x3 patch: 81 candidates times 9 taps. A wider window finds faster
// motion but costs the square of the radius, and this runs every frame inside the frame budget.
constexpr char kFlowShader[] = R"(
Texture2D<float> cur : register(t0);
Texture2D<float> prv : register(t1);
Texture2D<float2> guess : register(t2);
RWTexture2D<float2> dst : register(u0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint useGuess; float k; float step; uint pad; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 int2 base = int2(p.xy);
 int2 lim = int2(dw-1, dh-1);
 int st = max(1, (int)step);
 // Nothing to track in a flat patch: every candidate matches equally well, so the winner is
 // whichever the loop happened to try first. That is the aperture problem, and it is why a wider
 // search made the field wilder instead of better. Reject on contrast before searching at all.
 float mn = 1e9, mx = -1e9;
 for (int cy=-1; cy<=1; ++cy) for (int cx=-1; cx<=1; ++cx) {
  float v = cur.Load(int3(clamp(base+int2(cx,cy), int2(0,0), lim), 0));
  mn = min(mn, v); mx = max(mx, v);
 }
 if (mx - mn < 0.02) { dst[p.xy] = float2(0,0); return; }

 int2 g0 = (useGuess != 0) ? int2(round(guess.Load(int3(p.xy,0)))) : int2(0,0);
 float best = 1e9; int2 bestD = g0; float zero = 0.0;
 for (int dy=-4; dy<=4; ++dy) for (int dx=-4; dx<=4; ++dx) {
  int2 d = g0 + int2(dx,dy) * st;
  float sad = 0.0;
  for (int y=-1; y<=1; ++y) for (int x=-1; x<=1; ++x) {
   int2 a = clamp(base+int2(x,y), int2(0,0), lim);
   int2 b = clamp(base+d+int2(x,y), int2(0,0), lim);
   sad += abs(cur.Load(int3(a,0)) - prv.Load(int3(b,0)));
  }
  if (d.x==0 && d.y==0) zero = sad;
  if (sad < best) { best = sad; bestD = d; }
 }
 // Only on the coarse pass: reject a winner that barely beats standing still, which is what flat
 // sky and noise produce. The refine pass trusts the guess it was handed instead.
 // And a match has to explain the image clearly better than standing still, not marginally.
 if (useGuess == 0 && best > zero * 0.70) bestD = int2(0,0);
 dst[p.xy] = float2(bestD);
})";

constexpr char kFlowUpShader[] = R"(
Texture2D<float2> src : register(t0);
RWTexture2D<float2> dst : register(u0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float step; uint pad; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 float2 uv = (float2(p.xy)+0.5)/float2(dw,dh);
 dst[p.xy] = src.SampleLevel(smp,uv,0) * step;
})";

DXGI_FORMAT DepthReadFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:                               return DXGI_FORMAT_UNKNOWN;
    }
}

bool IsTypedDepth(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return true;
    default:                               return false;
    }
}

DXGI_FORMAT DepthAliasFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:                return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:    return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:                return DXGI_FORMAT_R16_TYPELESS;
    default:                                   return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT ColourReadFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:   return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:  return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:    return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:    return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:                                 return f;
    }
}

// D3D12 guarantees typed UAV store for very few formats. B8G8R8A8_UNORM and R10G10B10A2_UNORM
// are NOT among them -- they are optional per driver. A hardcoded table that assumes they work
// means the compose shader writes through an unsupported UAV wherever they don't: undefined
// behaviour, which shows up as a distorted picture and no error anywhere. Ask the device.
bool HasTypedUavStore(ID3D12Device *dev, DXGI_FORMAT f)
{
    if (dev == nullptr)
        return false;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT s {};
    s.Format = f;
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof(s))))
        return false;
    return (s.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

void Barrier(ID3D12GraphicsCommandList *c, ID3D12Resource *r, D3D12_RESOURCE_STATES a,
             D3D12_RESOURCE_STATES b)
{
    if (r == nullptr || a == b)
        return;
    D3D12_RESOURCE_BARRIER v {};
    v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    c->ResourceBarrier(1, &v);
}

constexpr unsigned char kRuntimeSha256[32] = { 0x81, 0xef, 0xaa, 0xdc, 0x8d, 0x0d, 0xea, 0xa2,
                                               0xc2, 0x3f, 0x64, 0xae, 0xe8, 0x3b, 0x81, 0xe9,
                                               0xf4, 0x8e, 0x2d, 0xa4, 0xd0, 0xc3, 0xfb, 0xae,
                                               0x73, 0xfc, 0x68, 0xe0, 0x56, 0x07, 0x01, 0x17 };
constexpr size_t kRuntimeSize = 7156224;

template <class T> T &At(HMODULE h, size_t rva)
{
    return *reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(h) + rva);
}

struct Packet
{
    ID3D12GraphicsCommandList *list;
    ID3D12Resource *colour;
    UINT colourState, pad14;
    ID3D12Resource *motion;
    UINT motionState, pad24;
    ID3D12Resource *depth;
    UINT depthState, pad34;
    ID3D12Resource *exposure;
    UINT exposureState;
    float scaleX, scaleY;
    UINT pad4c;
};
static_assert(sizeof(Packet) == 0x50 && offsetof(Packet, scaleX) == 0x44);

using InitFn = bool(__fastcall *)(void *, const std::string *);
using RecordFn = void(__fastcall *)(Packet *);
using NotifyFn = void(__fastcall *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
using HipSetFn = int (*)(int);

bool RuntimeHashMatches(const std::filesystem::path &file)
{
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
    if (data.size() != kRuntimeSize)
        return false;
    BCRYPT_ALG_HANDLE alg {};
    unsigned char digest[32] {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    const auto result =
        BCryptHash(alg, nullptr, 0, data.data(), static_cast<ULONG>(data.size()), digest, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    return result >= 0 && std::memcmp(digest, kRuntimeSha256, 32) == 0;
}

struct State
{
    std::mutex lock;

    bool unavailable = false;
    bool loggedWrongApi = false;
    std::atomic<bool> enabled { true };
    std::atomic<float> structure { 1.0f };
    std::atomic<float> tone { 0.0f };
    std::atomic<float> skin { 1.0f };
    std::atomic<int> passes { 1 };
    std::atomic<float> scale { 0.5f };
    std::atomic<bool> inlineMode { true };
    std::atomic<int> encoding { 0 };
    std::atomic<float> diffuseWhite { 500.0f };
    std::atomic<float> intensity { 1.0f };
    bool failed = false;
    const char *reason = "";

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;

    HMODULE runtimes[3] {};
    UINT lastJobs[3] {};
    UINT activePasses = 0;
    HipSetFn hipSet = nullptr;
    int hipDevice = -1;
    bool engineReady = false;

    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> copyPipeline, depthPipeline, composePipeline;
    ComPtr<ID3D12PipelineState> lumaPipeline, flowPipeline, flowUpPipeline;
    ComPtr<ID3D12DescriptorHeap> heap;

    UINT outWidth = 0, outHeight = 0, netWidth = 0, netHeight = 0;
    DXGI_FORMAT outFormat = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D12Resource> netColour;
    ComPtr<ID3D12Resource> netBase;
    ComPtr<ID3D12Resource> netMotion;
    ComPtr<ID3D12Resource> lumaA, lumaB, flowSmall, flowCoarse;
    UINT flowWidth = 0, flowHeight = 0;
    std::atomic<bool> useMotion { false };
    bool loggedFlow = false;
    ComPtr<ID3D12Resource> flowReadLuma, flowReadFlow;
    bool pendingFlow = false;
    bool flowProbed = false;
    ComPtr<ID3D12Resource> netDepth;
    ComPtr<ID3D12Resource> depthAlias;

    ID3D12Resource *depthCandidate = nullptr;
    UINT depthWidth = 0, depthHeight = 0;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    UINT depthBinds = 0, depthBestBinds = 0;
    ID3D12Resource *depthBest = nullptr;
    std::atomic<bool> useDepth { false };
    std::atomic<bool> depthInverted { false };
    bool loggedDepth = false;
    ComPtr<ID3D12Resource> composed;
    ComPtr<ID3D12Resource> readbackBase, readbackNr;
    bool measured = false;
    bool pendingMeasure = false;

    uint64_t frame = 0;
    uint64_t skipped = 0;
    UINT64 lastJobAt = 0;
    ComPtr<ID3D12Fence> fence;
    UINT64 serial = 0, completion = 0;

    bool loggedProfile = false;
    bool loggedPin = false;
    UINT measureTries = 0;
    UINT64 depthEvents = 0;
};

State g;

bool InitHip()
{
    if (g.hipSet != nullptr)
        return true;
    HMODULE hip = LoadLibraryExW(L"amdhip64_7.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (hip == nullptr)
    {
        Log("HIP: amdhip64_7.dll failed to load (error %lu). AMD HIP 7 is required.", GetLastError());
        return false;
    }
    auto count = reinterpret_cast<int (*)(int *)>(GetProcAddress(hip, "hipGetDeviceCount"));
    auto props = reinterpret_cast<int (*)(void *, int)>(GetProcAddress(hip, "hipGetDevicePropertiesR0600"));
    g.hipSet = reinterpret_cast<HipSetFn>(GetProcAddress(hip, "hipSetDevice"));
    if (count == nullptr || props == nullptr || g.hipSet == nullptr)
    {
        Log("HIP: R0600 API unavailable.");
        g.hipSet = nullptr;
        return false;
    }
    int n = 0;
    if (count(&n) != 0 || n == 0)
    {
        Log("HIP: no devices enumerated.");
        g.hipSet = nullptr;
        return false;
    }
    const auto luid = g.device->GetAdapterLuid();
    for (int i = 0; i < n; ++i)
    {
        alignas(16) std::vector<unsigned char> p(8192, 0);
        if (props(p.data(), i) == 0 && std::memcmp(p.data() + 272, &luid, 8) == 0)
        {
            g.hipDevice = i;
            Log("HIP: adapter %s matches the game's D3D12 device.", reinterpret_cast<char *>(p.data()));
            break;
        }
    }
    if (g.hipDevice < 0 || g.hipSet(g.hipDevice) != 0)
    {
        Log("HIP: no device matches the D3D12 LUID.");
        g.hipSet = nullptr;
        return false;
    }
    return true;
}

// The engine reads dlssnr_on_amd.ini from DllMain, so it has to exist before LoadLibrary.
// Its built-in default host watchdog budget is 600 ms: long enough for one stalled job to trip
// Windows TDR, which removes the D3D12 device and takes the game with it. That surfaces as the
// game dying on DXGI_ERROR_DEVICE_REMOVED (887A0005), with nothing pointing back here. Writing
// the file when it is missing is cheaper than explaining the crash.
void EnsureEngineIni(const std::filesystem::path &dir)
{
    const auto ini = dir / L"dlssnr_on_amd.ini";
    std::error_code ec;
    if (std::filesystem::exists(ini, ec))
        return;

    std::ofstream f(ini, std::ios::binary);
    if (!f)
    {
        Log("could not write %ls; the engine will fall back to its own defaults.", ini.c_str());
        return;
    }
    f << "[DlssNrOnAmd]\r\n"
         "Enabled=1\r\n"
         "Inline=1\r\n"
         "; Host watchdog budget, milliseconds. The network takes about 16 ms at 0.50 scale, so\r\n"
         "; 100 is a wide margin; past it the frame is shown without the effect instead of\r\n"
         "; freezing. Do not raise this much: the engine's own default is 600 ms, and a stall\r\n"
         "; that long trips Windows TDR, which removes the D3D12 device and kills the game.\r\n"
         "InlineWaitMs=100\r\n"
         "Interop=1\r\n"
         "UseFsrInputs=1\r\n"
         "UseDepth=0\r\n"
         "Temporal=0\r\n"
         "Tonemap=0\r\n"
         "HipDevice=-1\r\n";
    Log("wrote a default dlssnr_on_amd.ini next to the exe.");
}

bool InitEngine(UINT index)
{
    if (g.runtimes[index] != nullptr)
        return true;
    const auto dir = ExeDirectory();
    const auto dll = dir / (L"dlssnr_amd_pass" + std::to_wstring(index + 1) + L".dll");
    const auto weights = dir / L"dlssnr_on_amd_weights.bin";
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec))
    {
        Log("missing: %ls", dll.c_str());
        return false;
    }
    if (!std::filesystem::exists(weights, ec))
    {
        Log("missing: %ls", weights.c_str());
        return false;
    }
    if (!RuntimeHashMatches(dll))
    {
        Log("dlssnr_amd_pass%u.dll hash mismatch; refused.", index + 1);
        return false;
    }
    if (!InitHip())
        return false;

    EnsureEngineIni(dir);

    HMODULE h = LoadLibraryExW(dll.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (h == nullptr)
    {
        Log("LoadLibrary failed for the runtime (error %lu).", GetLastError());
        return false;
    }
    HMODULE pinned {};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(h), &pinned);

    At<ID3D12Device *>(h, 0x764c8) = g.device.Get();
    g.device->AddRef();
    At<ID3D12CommandQueue *>(h, 0x764d0) = g.queue.Get();
    g.queue->AddRef();
    At<int>(h, 0x76f20) = g.hipDevice;
    At<uint8_t>(h, 0x76be0) = g.inlineMode.load() ? 1 : 0;
    At<uint8_t>(h, 0x76c8c) = 1;
    At<uint8_t>(h, 0x76e1c) = 1;
    At<uint8_t>(h, 0x76e1e) = 1;
    At<uint8_t>(h, 0x76e1f) = 0;
    At<int>(h, 0x76e20) = 0;

    const std::string file = weights.string();
    if (g.hipSet(g.hipDevice) != 0 ||
        !reinterpret_cast<InitFn>(reinterpret_cast<uintptr_t>(h) + 0x12380)(
            reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(h) + 0x764d8), &file))
    {
        Log("engine init failed.");
        return false;
    }
    At<uint8_t>(h, 0x767f8) = 1;
    g.runtimes[index] = h;
    g.engineReady = true;
    Log("engine ready: pass %u.", index + 1);
    return true;
}

bool CompileShader(const char *source, size_t size, const char *name, ComPtr<ID3D12PipelineState> &out)
{
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3DCompile(source, size, name, nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error)))
    {
        Log("shader %s failed to compile: %s", name,
            error != nullptr ? static_cast<const char *>(error->GetBufferPointer()) : "?");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps {};
    ps.pRootSignature = g.root.Get();
    ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
    return SUCCEEDED(g.device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&out)));
}

bool InitPipeline()
{
    if (g.root != nullptr)
        return true;
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3 };
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { 2, ranges };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = { 0, 0, 8 };
    D3D12_STATIC_SAMPLER_DESC smp {};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc { 2, params, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&g.root))))
    {
        Log("root signature failed.");
        return false;
    }
    if (!CompileShader(kCopyShader, sizeof(kCopyShader), "copy", g.copyPipeline) ||
        !CompileShader(kDepthShader, sizeof(kDepthShader), "depth", g.depthPipeline) ||
        !CompileShader(kComposeShader, sizeof(kComposeShader), "compose", g.composePipeline) ||
        !CompileShader(kLumaShader, sizeof(kLumaShader), "luma", g.lumaPipeline) ||
        !CompileShader(kFlowShader, sizeof(kFlowShader), "flow", g.flowPipeline) ||
        !CompileShader(kFlowUpShader, sizeof(kFlowUpShader), "flowup", g.flowUpPipeline))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    return SUCCEEDED(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)));
}

bool CreateTexture(UINT w, UINT h, DXGI_FORMAT f, ComPtr<ID3D12Resource> &out, const char *what)
{
    out.Reset();
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 nullptr, IID_PPV_ARGS(&out))))
    {
        Log("texture creation failed: %s %ux%u format %d", what, w, h, static_cast<int>(f));
        return false;
    }
    return true;
}

bool EnsureResources(UINT w, UINT h, DXGI_FORMAT outFormat, float scale)
{
    UINT nw = std::max<UINT>(64u, static_cast<UINT>(w * scale + 0.5f));
    UINT nh = std::max<UINT>(64u, static_cast<UINT>(h * scale + 0.5f));

    // The engine sizes its staging buffers once, when it initialises, from the first raster it is
    // handed, and offers no way to resize them afterwards. Some games do not present at a fixed
    // size: Xenosaga 2 walks between 1920x1080, 1918x1014, 1918x994 and 1918x1008 every few
    // frames. Letting the raster follow that hands the engine a 959x507 texture it still reads as
    // 960x540 -- wrong stride, every row shifted, which is the skewed picture people report. It
    // would also destroy the very textures the engine holds zero-copy handles to.
    //
    // Both shaders already resample between the back buffer and the raster, in either direction,
    // so pinning the raster to whatever the engine came up with costs nothing but a resample.
    if (g.engineReady && g.netWidth != 0 && (nw != g.netWidth || nh != g.netHeight))
    {
        if (!g.loggedPin)
        {
            g.loggedPin = true;
            Log("back buffer changed to %ux%u, which wants a %ux%u raster; keeping the raster at "
                "%ux%u because the engine's staging is fixed at that size. Further changes are "
                "handled the same way and not logged.",
                w, h, nw, nh, g.netWidth, g.netHeight);
        }
        nw = g.netWidth;
        nh = g.netHeight;
    }

    const bool netChanged = g.netWidth != nw || g.netHeight != nh || g.netColour == nullptr;
    const bool outChanged = g.outWidth != w || g.outHeight != h || g.outFormat != outFormat ||
                            g.composed == nullptr;
    if (!netChanged && !outChanged)
        return true;

    // Everything below releases a texture and makes a new one. D3D12 does not keep a resource
    // alive just because an in-flight command list still references it, and last frame's
    // CopyResource out of `composed` can still be running when the back buffer resizes. Dropping
    // it there is a use-after-free on the GPU. Wait for the queue to catch up first -- this only
    // costs anything on an actual size change, which is rare.
    if (g.fence != nullptr && g.fence->GetCompletedValue() < g.completion)
    {
        if (HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr))
        {
            if (SUCCEEDED(g.fence->SetEventOnCompletion(g.completion, done)))
                WaitForSingleObject(done, 1000);
            CloseHandle(done);
        }
    }

    const DXGI_FORMAT composeFormat = ColourReadFormat(outFormat);
    if (!HasTypedUavStore(g.device.Get(), composeFormat))
    {
        Log("this GPU/driver has no typed UAV store for the back buffer format (DXGI %d, read as "
            "%d), so there is no way to write the corrected image back. Stopping instead of "
            "drawing garbage. Try turning HDR off, or a different swapchain format.",
            static_cast<int>(outFormat), static_cast<int>(composeFormat));
        g.reason = "back buffer format has no typed UAV store on this driver";
        return false;
    }

    // Only the textures whose geometry actually moved get rebuilt. composed has to match the back
    // buffer because it is CopyResource'd into it; the network textures must not be touched while
    // the engine is using them.
    // Flow runs on a luminance image an eighth the size. Full resolution would cost the square
    // of that for no gain: the search window is what limits how fast a motion can be tracked, and
    // a coarse field upsamples cleanly because real motion is mostly low frequency.
    const UINT fw = std::max<UINT>(16u, nw / 8u);
    const UINT fh = std::max<UINT>(16u, nh / 8u);

    if (netChanged &&
        (!CreateTexture(nw, nh, DXGI_FORMAT_R16G16B16A16_FLOAT, g.netColour, "netColour") ||
         !CreateTexture(nw, nh, DXGI_FORMAT_R16G16B16A16_FLOAT, g.netBase, "netBase") ||
         !CreateTexture(nw, nh, DXGI_FORMAT_R16G16_FLOAT, g.netMotion, "netMotion") ||
         !CreateTexture(nw, nh, DXGI_FORMAT_R32_FLOAT, g.netDepth, "netDepth") ||
         !CreateTexture(fw, fh, DXGI_FORMAT_R16_FLOAT, g.lumaA, "lumaA") ||
         !CreateTexture(fw, fh, DXGI_FORMAT_R16_FLOAT, g.lumaB, "lumaB") ||
         !CreateTexture(fw, fh, DXGI_FORMAT_R16G16_FLOAT, g.flowSmall, "flowSmall") ||
         !CreateTexture(fw, fh, DXGI_FORMAT_R16G16_FLOAT, g.flowCoarse, "flowCoarse")))
        return false;
    if (netChanged)
    {
        g.flowWidth = fw;
        g.flowHeight = fh;
    }
    if (outChanged && !CreateTexture(w, h, composeFormat, g.composed, "composed"))
        return false;

    g.depthAlias.Reset();
    g.outWidth = w;
    g.outHeight = h;
    g.netWidth = nw;
    g.netHeight = nh;
    g.outFormat = outFormat;
    Log("raster: back buffer %ux%u format %d, network at %ux%u (scale %.2f)", w, h,
        static_cast<int>(outFormat), nw, nh, static_cast<double>(scale));
    return true;
}

// Observation is deliberately NOT gated on the Depth switch. It used to be, and that made the
// question unanswerable: with the switch off -- the default -- this returned immediately, logged
// nothing, and the status line then said "no candidate found", which reads as "this game has no
// depth buffer" when it actually meant "nobody looked". Finding a candidate costs a pointer and a
// GetDesc; only *using* it is gated, further down in the present path.
void OnBindDepthStencil(command_list *cmd_list, uint32_t, const resource_view *, resource_view dsv)
{
    if (dsv.handle == 0)
        return;
    ++g.depthEvents;
    device *dev = cmd_list != nullptr ? cmd_list->get_device() : nullptr;
    if (dev == nullptr || dev->get_api() != device_api::d3d12)
        return;
    const resource res = dev->get_resource_from_view(dsv);
    if (res.handle == 0)
        return;
    auto *native = reinterpret_cast<ID3D12Resource *>(res.handle);
    std::lock_guard guard(g.lock);
    if (native == g.depthBest)
    {
        ++g.depthBinds;
        return;
    }
    const auto d = native->GetDesc();
    static UINT seen = 0;
    const bool readable = DepthReadFormat(d.Format) != DXGI_FORMAT_UNKNOWN;
    const bool denied = (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0;
    if (seen < 8)
    {
        ++seen;
        Log("depth seen: %llux%u format %d flags 0x%x samples %u array %u -> %s",
            static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(d.Format),
            static_cast<unsigned>(d.Flags), d.SampleDesc.Count, d.DepthOrArraySize,
            (!readable ? "format not readable" : denied ? "shader resource denied" : "taken"));
    }
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1 ||
        d.DepthOrArraySize != 1 || !readable || denied)
        return;
    if (g.depthBest == nullptr || d.Width * d.Height > static_cast<UINT64>(g.depthWidth) * g.depthHeight)
    {
        g.depthBest = native;
        g.depthWidth = static_cast<UINT>(d.Width);
        g.depthHeight = d.Height;
        g.depthFormat = d.Format;
        g.depthBinds = 1;
    }
}

bool OnDraw(command_list *, uint32_t, uint32_t, uint32_t, uint32_t) { return false; }
bool OnDrawIndexed(command_list *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { return false; }

bool ToggleRequested()
{
    static bool down = false;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool now = ctrl && (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    const bool pressed = now && !down;
    down = now;
    return pressed;
}

void OnPresent(command_queue *queue, swapchain *sc, const rect *, const rect *, uint32_t,
               const rect *)
{
    const Profile &profile = ProfileForThisProcess();
    std::lock_guard guard(g.lock);
    if (ToggleRequested())
    {
        const bool on = !g.enabled.load();
        g.enabled.store(on);
        Log("Ctrl+End: %s", on ? "on" : "off");
    }
    if (!g.enabled.load() || g.unavailable || g.failed)
        return;
    if (!g.loggedProfile)
    {
        g.loggedProfile = true;
        Log("target: %s", profile.note);
        Log("tier %c, scale %.2f", 'A' + static_cast<int>(profile.tier),
            static_cast<double>(profile.scale));
    }
    device *dev = sc != nullptr ? sc->get_device() : nullptr;
    if (dev == nullptr || queue == nullptr)
        return;
    if (dev->get_api() != device_api::d3d12)
    {
        if (!g.loggedWrongApi)
        {
            g.loggedWrongApi = true;
            Log("a runtime in this process is not D3D12; ignored.");
        }
        return;
    }
    if (g.device == nullptr)
        g.device = reinterpret_cast<ID3D12Device *>(dev->get_native());
    if ((queue->get_type() & command_queue_type::graphics) == static_cast<command_queue_type>(0))
        return;
    if (g.queue == nullptr)
        g.queue = reinterpret_cast<ID3D12CommandQueue *>(queue->get_native());
    command_list *cmd_list = queue->get_immediate_command_list();
    if (cmd_list == nullptr)
        return;
    const UINT wanted = static_cast<UINT>(std::clamp(g.passes.load(), 1, 3));
    bool enginesReady = InitPipeline();
    for (UINT i = 0; enginesReady && i < wanted; ++i)
        enginesReady = InitEngine(i);
    if (!enginesReady)
    {
        g.unavailable = true;
        g.reason = "could not bring the engine up";
        Log("off: %s", g.reason);
        return;
    }

    const resource back = sc->get_current_back_buffer();
    if (back.handle == 0)
        return;
    auto *backbuffer = reinterpret_cast<ID3D12Resource *>(back.handle);
    const auto bd = backbuffer->GetDesc();
    if (bd.SampleDesc.Count != 1 || bd.DepthOrArraySize != 1 ||
        bd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        return;
    if (!EnsureResources(static_cast<UINT>(bd.Width), bd.Height, bd.Format, g.scale.load()))
    {
        g.unavailable = true;
        if (*g.reason == '\0')
            g.reason = "could not create the working textures; see dlss5-neural.log";
        return;
    }

    if (g.fence == nullptr &&
        FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
    {
        g.unavailable = true;
        g.reason = "could not create the completion fence";
        Log("off: %s", g.reason);
        return;
    }
    bool runNetwork = true;
    const bool jobPending =
        g.fence->GetCompletedValue() < g.completion ||
        static_cast<UINT>(InterlockedCompareExchange(
            reinterpret_cast<volatile LONG *>(&At<UINT>(g.runtimes[0], 0x76c14)), 0, 0)) <
            g.lastJobs[0];
    if (jobPending)
    {
        if (GetTickCount64() - g.lastJobAt < 500)
        {
            runNetwork = false;
            if (++g.skipped % 120 == 1)
                Log("network skipped: previous evaluation still pending (%llu skipped, %llu done)",
                    static_cast<unsigned long long>(g.skipped),
                    static_cast<unsigned long long>(g.frame));
        }
        else
        {
            if (g.skipped % 600 == 0)
                Log("previous job did not finish in 500 ms; continuing anyway (%llu skipped)",
                    static_cast<unsigned long long>(g.skipped));
            g.lastJobs[0] = 0;
        }
    }

    auto *cmd = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native());
    const UINT w = g.outWidth, h = g.outHeight, nw = g.netWidth, nh = g.netHeight;
    const UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto heapStart = g.heap->GetCPUDescriptorHandleForHeapStart();
    auto slot = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = heapStart;
        handle.ptr += static_cast<SIZE_T>(i) * inc;
        return handle;
    };

    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    srv.Format = ColourReadFormat(bd.Format);
    for (UINT i = 0; i < 3; ++i)
        g.device->CreateShaderResourceView(backbuffer, &srv, slot(i));
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g.device->CreateUnorderedAccessView(g.netColour.Get(), nullptr, &uav, slot(3));

    srv.Format = ColourReadFormat(bd.Format);
    g.device->CreateShaderResourceView(backbuffer, &srv, slot(8));
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g.device->CreateShaderResourceView(g.netColour.Get(), &srv, slot(9));
    g.device->CreateShaderResourceView(g.netBase.Get(), &srv, slot(10));
    uav.Format = ColourReadFormat(bd.Format);
    g.device->CreateUnorderedAccessView(g.composed.Get(), nullptr, &uav, slot(11));

    const resource backRes = back;
    resource_usage from = resource_usage::present, to = resource_usage::shader_resource;
    cmd_list->barrier(1, &backRes, &from, &to);

    const int encMode = g.encoding.load();
    const float white = std::max(1.0f, g.diffuseWhite.load());
    const float kWhite = encMode == 0 ? 1.0f : 203.0f / white;
    const float strength = g.intensity.load();

    auto *heap = g.heap.Get();
    if (runNetwork)
    {
    cmd->SetComputeRootSignature(g.root.Get());
    cmd->SetDescriptorHeaps(1, &heap);
    cmd->SetPipelineState(g.copyPipeline.Get());
    cmd->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    UINT dims[8] { nw, nh, w, h, static_cast<UINT>(encMode), 0, 0, 0 };
    std::memcpy(&dims[5], &kWhite, sizeof(float));
    std::memcpy(&dims[6], &strength, sizeof(float));
    cmd->SetComputeRoot32BitConstants(1, 8, dims, 0);
    Barrier(cmd, g.netColour.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->Dispatch((nw + 7) / 8, (nh + 7) / 8, 1);
    Barrier(cmd, g.netColour.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);

    Barrier(cmd, g.netBase.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(g.netBase.Get(), g.netColour.Get());
    Barrier(cmd, g.netBase.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, g.netColour.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Optical flow. Three dispatches on small textures: luminance, block match against last
    // frame's luminance, then upsample into the raster the engine reads. The two luminance
    // textures swap every frame so this frame's becomes next frame's reference.
    bool haveMotion = false;
    if (g.useMotion.load() && g.flowSmall != nullptr)
    {
        ID3D12Resource *cur = (g.frame & 1) ? g.lumaB.Get() : g.lumaA.Get();
        ID3D12Resource *prev = (g.frame & 1) ? g.lumaA.Get() : g.lumaB.Get();
        const UINT fw = g.flowWidth, fh = g.flowHeight;

        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        g.device->CreateShaderResourceView(g.netColour.Get(), &srv, slot(12));
        uav.Format = DXGI_FORMAT_R16_FLOAT;
        g.device->CreateUnorderedAccessView(cur, nullptr, &uav, slot(15));

        // Coarse pass at base 16 searches in strides of 2, so it reaches twice as far for the
        // same 81 candidates. Fine pass at base 24 refines around what it found. Two cheap passes
        // beat one wide search: reach grows linearly here and quadratically there.
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        g.device->CreateShaderResourceView(cur, &srv, slot(16));
        g.device->CreateShaderResourceView(prev, &srv, slot(17));
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateShaderResourceView(g.flowCoarse.Get(), &srv, slot(18));
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateUnorderedAccessView(g.flowCoarse.Get(), nullptr, &uav, slot(19));

        srv.Format = DXGI_FORMAT_R16_FLOAT;
        g.device->CreateShaderResourceView(cur, &srv, slot(24));
        g.device->CreateShaderResourceView(prev, &srv, slot(25));
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateShaderResourceView(g.flowCoarse.Get(), &srv, slot(26));
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateUnorderedAccessView(g.flowSmall.Get(), nullptr, &uav, slot(27));

        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateShaderResourceView(g.flowSmall.Get(), &srv, slot(20));
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateUnorderedAccessView(g.netMotion.Get(), nullptr, &uav, slot(23));

        auto table = [&](UINT base) {
            auto t = heap->GetGPUDescriptorHandleForHeapStart();
            t.ptr += static_cast<UINT64>(base) * inc;
            return t;
        };
        auto dispatch = [&](ID3D12PipelineState *pso, UINT base, UINT dw, UINT dh, UINT sw, UINT sh,
                            float extra, UINT mode = 0) {
            cmd->SetPipelineState(pso);
            cmd->SetComputeRootDescriptorTable(0, table(base));
            UINT d[8] { dw, dh, sw, sh, mode, 0, 0, 0 };
            std::memcpy(&d[6], &extra, sizeof(extra));
            cmd->SetComputeRoot32BitConstants(1, 8, d, 0);
            cmd->Dispatch((dw + 7) / 8, (dh + 7) / 8, 1);
        };

        cmd->SetComputeRootSignature(g.root.Get());
        cmd->SetDescriptorHeaps(1, &heap);

        Barrier(cmd, cur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(g.lumaPipeline.Get(), 12, fw, fh, nw, nh, 0.0f);
        Barrier(cmd, cur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        Barrier(cmd, g.flowCoarse.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(g.flowPipeline.Get(), 16, fw, fh, fw, fh, 2.0f, 0);
        Barrier(cmd, g.flowCoarse.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        Barrier(cmd, g.flowSmall.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(g.flowPipeline.Get(), 24, fw, fh, fw, fh, 1.0f, 1);
        Barrier(cmd, g.flowSmall.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // The field is in coarse pixels; the engine reads it at raster resolution, so one coarse
        // pixel is eight of those. Getting this factor wrong tells the network the image moved a
        // different distance than it did, which is worse than telling it nothing.
        Barrier(cmd, g.netMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(g.flowUpPipeline.Get(), 20, nw, nh, fw, fh,
                 static_cast<float>(nw) / static_cast<float>(fw));
        Barrier(cmd, g.netMotion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // One-shot look inside the flow, so a single run answers what is wrong instead of
        // bisecting over several. A degenerate match and a genuinely still scene produce the same
        // mean vector; the luminance stats tell them apart.
        if (!g.flowProbed && g.frame == 300)
        {
            g.flowProbed = true;
            const UINT lumaPitch = (fw * 2 + 255) & ~255u;
            const UINT flowPitch = (fw * 4 + 255) & ~255u;
            D3D12_HEAP_PROPERTIES rb {};
            rb.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bd.Width = static_cast<UINT64>(lumaPitch) * fh;
            const bool okA = SUCCEEDED(g.device->CreateCommittedResource(
                &rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&g.flowReadLuma)));
            bd.Width = static_cast<UINT64>(flowPitch) * fh;
            const bool okB = SUCCEEDED(g.device->CreateCommittedResource(
                &rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&g.flowReadFlow)));
            if (okA && okB)
            {
                auto grab = [&](ID3D12Resource *src, ID3D12Resource *dst, DXGI_FORMAT f, UINT pitch) {
                    Barrier(cmd, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION from {}, to {};
                    from.pResource = src;
                    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    to.pResource = dst;
                    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    to.PlacedFootprint.Footprint = { f, fw, fh, 1, pitch };
                    cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                    Barrier(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                };
                grab(cur, g.flowReadLuma.Get(), DXGI_FORMAT_R16_FLOAT, lumaPitch);
                grab(g.flowSmall.Get(), g.flowReadFlow.Get(), DXGI_FORMAT_R16G16_FLOAT, flowPitch);
                g.pendingFlow = true;
            }
        }

        haveMotion = g.frame > 1;
        if (!g.loggedFlow)
        {
            g.loggedFlow = true;
            Log("motion: flow at %ux%u, coarse pass in strides of 2 then a refine, reaching +/-12 "
                "coarse pixels, about %.0f raster pixels a frame. The PS2 has no real motion to "
                "read, so this is inferred from the image, same as the NVIDIA route does here.",
                fw, fh, 12.0 * static_cast<double>(nw) / static_cast<double>(fw));
        }
    }

    bool haveDepth = false;
    if (g.useDepth.load() && g.depthBest != nullptr)
    {
        g.depthCandidate = g.depthBest;
        ID3D12Resource *depthSource = g.depthCandidate;
        const auto dd = depthSource->GetDesc();
        bool ok = true;
        if (IsTypedDepth(dd.Format))
        {
            const auto alias = DepthAliasFormat(dd.Format);
            if (alias == DXGI_FORMAT_UNKNOWN)
                ok = false;
            else
            {
                if (!g.depthAlias || g.depthAlias->GetDesc().Width != dd.Width ||
                    g.depthAlias->GetDesc().Height != dd.Height ||
                    g.depthAlias->GetDesc().Format != alias)
                {
                    g.depthAlias.Reset();
                    auto ad = dd;
                    ad.Format = alias;
                    ad.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                    ad.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                    D3D12_HEAP_PROPERTIES hp {};
                    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                    ok = SUCCEEDED(g.device->CreateCommittedResource(
                        &hp, D3D12_HEAP_FLAG_NONE, &ad, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        nullptr, IID_PPV_ARGS(&g.depthAlias)));
                }
                if (ok)
                {
                    depthSource = g.depthAlias.Get();
                    Barrier(cmd, g.depthCandidate, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(cmd, depthSource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_DEST);
                    cmd->CopyResource(depthSource, g.depthCandidate);
                    Barrier(cmd, depthSource, D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    Barrier(cmd, g.depthCandidate, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
            }
        }
        if (ok)
        {
            srv.Format = DepthReadFormat(dd.Format);
            for (UINT i = 4; i < 7; ++i)
                g.device->CreateShaderResourceView(depthSource, &srv, slot(i));
            uav.Format = DXGI_FORMAT_R32_FLOAT;
            g.device->CreateUnorderedAccessView(g.netDepth.Get(), nullptr, &uav, slot(7));
            Barrier(cmd, g.netDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetPipelineState(g.depthPipeline.Get());
            auto dtable = heap->GetGPUDescriptorHandleForHeapStart();
            dtable.ptr += 4 * inc;
            cmd->SetComputeRootDescriptorTable(0, dtable);
            UINT ddims[8] { nw, nh, static_cast<UINT>(dd.Width), dd.Height, 0, 0, 0, 0 };
            cmd->SetComputeRoot32BitConstants(1, 8, ddims, 0);
            cmd->Dispatch((nw + 7) / 8, (nh + 7) / 8, 1);
            Barrier(cmd, g.netDepth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            haveDepth = true;
            if (!g.loggedDepth)
            {
                g.loggedDepth = true;
                Log("depth: %ux%u format %d -> R32_FLOAT at %ux%u", g.depthWidth,
                    g.depthHeight, static_cast<int>(dd.Format), nw, nh);
            }
        }
    }

    UINT accepted = 0;
    bool nativeFailure = false;
    for (UINT i = 0; i < wanted; ++i)
    {
        HMODULE r = g.runtimes[i];
        At<uint8_t>(r, 0x765f8) = 0;
        At<void *>(r, 0x765f0) = nullptr;
        At<uint8_t>(r, 0x76e1d) = 1;
        At<uint8_t>(r, 0x76be0) = g.inlineMode.load() ? 1 : 0;
        At<uint8_t>(r, 0x76e1f) = haveDepth ? 1 : 0;
        At<UINT>(r, 0x76e10) = g.depthInverted.load() ? 1u : 0u;
        At<uint8_t>(r, 0x76e14) = 1;
        At<float>(r, 0x76e30) = i == 0 ? g.tone.load() : 0.0f;
        At<float>(r, 0x76e34) = g.structure.load();
        At<float>(r, 0x76e38) = g.skin.load();

        Packet packet {};
        packet.list = cmd;
        packet.colour = g.netColour.Get();
        packet.colourState = 4;
        packet.motion = g.netMotion.Get();
        packet.motionState = 4;
        packet.depth = g.netDepth.Get();
        packet.depthState = 4;
        packet.exposure = nullptr;
        packet.exposureState = 4;
        packet.scaleX = 1.0f;
        packet.scaleY = 1.0f;
        reinterpret_cast<RecordFn>(reinterpret_cast<uintptr_t>(r) + 0xa0b0)(&packet);

        if (At<uint8_t>(r, 0x767fa) != 0)
        {
            nativeFailure = true;
            g.failed = true;
            Log("pass %u reported a native failure. Stopping.", i + 1);
            break;
        }
        if (At<ID3D12CommandList *>(r, 0x76d68) != cmd)
        {
            if (++g.skipped % 600 == 1)
                Log("pass %u refused (%llu total)", i + 1,
                    static_cast<unsigned long long>(g.skipped));
            break;
        }
        g.lastJobs[i] = At<UINT>(r, 0x76d74);
        if (auto *abortWord = At<volatile LONG *>(r, 0x76c68))
            InterlockedExchange(abortWord, 0);
        ++accepted;
    }
    g.activePasses = accepted;
    if (accepted != 0)
        g.lastJobAt = GetTickCount64();
    if (nativeFailure)
    {
        resource_usage a = resource_usage::shader_resource, b = resource_usage::present;
        cmd_list->barrier(1, &backRes, &a, &b);
        queue->flush_immediate_command_list();
        return;
    }
    }

    // Retried rather than fired once at a fixed frame: at frame 240 a lot of games are still
    // on a black boot screen, and measuring there reports a black input and a zero residual
    // for a setup that is actually fine. Keep trying every 240 frames until the input has
    // something in it, then stop.
    if (!g.measured && g.frame >= 240 && g.frame % 240 == 0 && g.activePasses != 0)
    {
        const UINT rowPitch = (nw * 8 + 255) & ~255u;
        const UINT64 size = static_cast<UINT64>(rowPitch) * nh;
        D3D12_HEAP_PROPERTIES rb {};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd2 {};
        bd2.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd2.Width = size;
        bd2.Height = 1;
        bd2.DepthOrArraySize = 1;
        bd2.MipLevels = 1;
        bd2.Format = DXGI_FORMAT_UNKNOWN;
        bd2.SampleDesc.Count = 1;
        bd2.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(g.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd2,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&g.readbackBase))) &&
            SUCCEEDED(g.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd2,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&g.readbackNr))))
        {
            auto grab = [&](ID3D12Resource *src, ID3D12Resource *dst) {
                Barrier(cmd, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION from {}, to {};
                from.pResource = src;
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                from.SubresourceIndex = 0;
                to.pResource = dst;
                to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16B16A16_FLOAT, nw, nh, 1, rowPitch };
                cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                Barrier(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            };
            grab(g.netBase.Get(), g.readbackBase.Get());
            grab(g.netColour.Get(), g.readbackNr.Get());
            g.pendingMeasure = true;
        }
    }

    Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->SetComputeRootSignature(g.root.Get());
    cmd->SetDescriptorHeaps(1, &heap);
    cmd->SetPipelineState(g.composePipeline.Get());
    auto ctable = heap->GetGPUDescriptorHandleForHeapStart();
    ctable.ptr += 8 * inc;
    cmd->SetComputeRootDescriptorTable(0, ctable);
    UINT cdims[8] { w, h, nw, nh, static_cast<UINT>(encMode), 0, 0, 0 };
    std::memcpy(&cdims[5], &kWhite, sizeof(float));
    std::memcpy(&cdims[6], &strength, sizeof(float));
    cmd->SetComputeRoot32BitConstants(1, 8, cdims, 0);
    cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);

    {
        resource_usage a = resource_usage::shader_resource, b = resource_usage::copy_dest;
        cmd_list->barrier(1, &backRes, &a, &b);
    }
    cmd->CopyResource(backbuffer, g.composed.Get());
    {
        resource_usage a = resource_usage::copy_dest, b = resource_usage::present;
        cmd_list->barrier(1, &backRes, &a, &b);
    }
    Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12CommandList *submitted[] { cmd };
    for (UINT i = 0; i < g.activePasses; ++i)
        reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(g.runtimes[i]) + 0x4640)(
            g.queue.Get(), 1, submitted);
    queue->flush_immediate_command_list();
    if (g.pendingFlow)
    {
        g.pendingFlow = false;
        ComPtr<ID3D12Fence> f;
        if (SUCCEEDED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))) &&
            SUCCEEDED(g.queue->Signal(f.Get(), 1)))
        {
            for (int spin = 0; spin < 2000 && f->GetCompletedValue() < 1; ++spin)
                Sleep(1);
            auto half = [](uint16_t h) {
                const int e = (h >> 10) & 0x1f;
                const int m = h & 0x3ff;
                const float v = e == 0 ? m / 1024.0f * 6.103515625e-5f
                                       : std::ldexpf(1.0f + m / 1024.0f, e - 15);
                return (h & 0x8000) ? -v : v;
            };
            const UINT fw = g.flowWidth, fh = g.flowHeight;
            const UINT lumaPitch = (fw * 2 + 255) & ~255u;
            const UINT flowPitch = (fw * 4 + 255) & ~255u;
            void *a = nullptr, *b = nullptr;
            D3D12_RANGE all { 0, 0 };
            if (SUCCEEDED(g.flowReadLuma->Map(0, &all, &a)) &&
                SUCCEEDED(g.flowReadFlow->Map(0, &all, &b)))
            {
                double lo = 1e30, hi = -1e30, sum = 0.0;
                for (UINT y = 0; y < fh; ++y)
                {
                    auto *row = reinterpret_cast<const uint16_t *>(static_cast<const char *>(a) +
                                                                   static_cast<size_t>(y) * lumaPitch);
                    for (UINT x = 0; x < fw; ++x)
                    {
                        const double v = half(row[x]);
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                        sum += v;
                    }
                }
                Log("flow probe, luminance %ux%u: min %.6f max %.6f mean %.6f%s", fw, fh, lo, hi,
                    sum / (fw * fh),
                    (hi - lo) < 1e-6 ? "  <-- flat, so there is nothing for a match to lock onto"
                                     : "  <-- has detail, so matching had something to work with");

                UINT64 zero = 0, atLimit = 0, n = 0;
                double mag = 0.0;
                for (UINT y = 0; y < fh; ++y)
                {
                    auto *row = reinterpret_cast<const uint16_t *>(static_cast<const char *>(b) +
                                                                   static_cast<size_t>(y) * flowPitch);
                    for (UINT x = 0; x < fw; ++x)
                    {
                        const double dx = half(row[x * 2]), dy = half(row[x * 2 + 1]);
                        if (dx == 0.0 && dy == 0.0)
                            ++zero;
                        if (std::abs(dx) >= 12.0 || std::abs(dy) >= 12.0)
                            ++atLimit;
                        mag += std::abs(dx) + std::abs(dy);
                        ++n;
                    }
                }
                Log("flow probe, field: %llu%% of blocks still, %llu%% pinned at the search limit, "
                    "mean |d| %.3f coarse pixels", static_cast<unsigned long long>(100 * zero / n),
                    static_cast<unsigned long long>(100 * atLimit / n), mag / (2.0 * n));
                Log("  a healthy field is mostly still with a minority moving; nearly all pinned at "
                    "the limit means the match is following noise, not the picture.");
                g.flowReadLuma->Unmap(0, nullptr);
                g.flowReadFlow->Unmap(0, nullptr);
            }
        }
        g.flowReadLuma.Reset();
        g.flowReadFlow.Reset();
    }
    if (g.pendingMeasure)
    {
        g.pendingMeasure = false;
        ComPtr<ID3D12Fence> f;
        if (SUCCEEDED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))) &&
            SUCCEEDED(g.queue->Signal(f.Get(), 1)))
        {
            for (int spin = 0; spin < 2000 && f->GetCompletedValue() < 1; ++spin)
                Sleep(1);
            void *a = nullptr, *b = nullptr;
            D3D12_RANGE all { 0, 0 };
            if (SUCCEEDED(g.readbackBase->Map(0, &all, &a)) &&
                SUCCEEDED(g.readbackNr->Map(0, &all, &b)))
            {
                const UINT rowPitch = (nw * 8 + 255) & ~255u;
                double sum = 0.0, peak = 0.0;
                UINT64 n = 0;
                for (UINT y = 0; y < nh; y += 4)
                {
                    auto *pa = reinterpret_cast<const uint16_t *>(static_cast<const char *>(a) +
                                                                  static_cast<size_t>(y) * rowPitch);
                    auto *pb = reinterpret_cast<const uint16_t *>(static_cast<const char *>(b) +
                                                                  static_cast<size_t>(y) * rowPitch);
                    for (UINT x = 0; x < nw * 4; x += 4)
                    {
                        auto half = [](uint16_t h) {
                            const int e = (h >> 10) & 0x1f;
                            const int m = h & 0x3ff;
                            float v = e == 0 ? m / 1024.0f * 6.103515625e-5f
                                             : std::ldexpf(1.0f + m / 1024.0f, e - 15);
                            return (h & 0x8000) ? -v : v;
                        };
                        const double d = std::abs(half(pa[x]) - half(pb[x]));
                        sum += d;
                        if (d > peak)
                            peak = d;
                        ++n;
                    }
                }
                double meanBase = 0.0;
                UINT64 nb = 0;
                for (UINT y = 0; y < nh; y += 4)
                {
                    auto *pa = reinterpret_cast<const uint16_t *>(static_cast<const char *>(a) +
                                                                  static_cast<size_t>(y) * rowPitch);
                    for (UINT x = 0; x < nw * 4; x += 4)
                    {
                        auto half = [](uint16_t hh) {
                            const int e = (hh >> 10) & 0x1f;
                            const int m = hh & 0x3ff;
                            float v = e == 0 ? m / 1024.0f * 6.103515625e-5f
                                             : std::ldexpf(1.0f + m / 1024.0f, e - 15);
                            return (hh & 0x8000) ? -v : v;
                        };
                        meanBase += std::abs(half(pa[x]));
                        ++nb;
                    }
                }
                const double inputMean = nb ? meanBase / nb : 0.0;
                if (inputMean <= 0.0 && g.measureTries < 12)
                {
                    ++g.measureTries;
                    Log("measure: the network was handed a black frame (attempt %u); the game is "
                        "probably still on a loading screen. Retrying in 240 frames.",
                        g.measureTries);
                }
                else
                {
                    g.measured = true;
                    Log("measure, network input: mean absolute %.6f (%llu samples)",
                        inputMean, static_cast<unsigned long long>(nb));
                    Log("  0.000000 means a black image was handed to the network.");
                    Log("measure, residual at %ux%u: mean %.6f, max %.6f (%llu samples)",
                        nw, nh, n ? sum / n : 0.0, peak, static_cast<unsigned long long>(n));
                    Log("  mean 0.000000 means the network returned its input unchanged.");
                }
                g.readbackBase->Unmap(0, nullptr);
                g.readbackNr->Unmap(0, nullptr);
            }
        }
        g.readbackBase.Reset();
        g.readbackNr.Reset();
    }
    g.completion = ++g.serial;
    if (FAILED(g.queue->Signal(g.fence.Get(), g.completion)))
    {
        g.failed = true;
        Log("completion fence Signal failed. Stopping.");
    }

    if (++g.frame <= 3 || g.frame % 120 == 0)
        Log("frame %llu processed (%llu skipped)", static_cast<unsigned long long>(g.frame),
            static_cast<unsigned long long>(g.skipped));

    // One line, once, so a log tells us whether depth is even reachable on this API.
    if (g.frame == 600)
        Log("guides after 600 frames: %llu depth-stencil bind events, best candidate %s. Motion: "
            "the PS2 never computed per-pixel motion, so there is none to take.",
            static_cast<unsigned long long>(g.depthEvents),
            g.depthBest != nullptr ? "found" : "none");
}

// Anything that makes one evaluation slower is a driver-reset risk while Apply On Same Frame is
// on, because in that mode the game is blocked on the GPU until the network finishes. Past the
// Windows driver timeout the display driver resets and the game dies on DEVICE_REMOVED, with
// nothing in any log pointing back here -- so the warning goes next to the control.
void Note(const ImVec4 &colour, const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

const ImVec4 kWarn { 1.0f, 0.80f, 0.30f, 1.0f };
const ImVec4 kDanger { 1.0f, 0.45f, 0.35f, 1.0f };

void OnOverlay(effect_runtime *)
{
    bool on = g.enabled.load();
    if (ImGui::Checkbox("Enabled", &on))
    {
        g.enabled.store(on);
        Log("menu: %s", on ? "on" : "off");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Ctrl+End)");

    ImGui::SeparatorText("Source");

    ImGui::BeginDisabled();
    int mode = 0, hook = 0, require = 0;
    ImGui::Combo("Options Mode", &mode, "DLSS-NR\0");
    ImGui::Combo("Hook Method", &hook, "Present\0");
    ImGui::Combo("Require DLSS", &require, "Off\0");
    ImGui::EndDisabled();
    
    int enc = g.encoding.load();
    if (ImGui::Combo("Encoding", &enc, "sRGB\0Linear\0scRGB-nl\0"))
        g.encoding.store(enc);
    float white = g.diffuseWhite.load();
    if (ImGui::SliderFloat("Diffuse White", &white, 80.0f, 1000.0f, "%.0f nits", 0))
        g.diffuseWhite.store(white);
    
    ImGui::BeginDisabled();
    bool ui = true;
    ImGui::Checkbox("UI Correction", &ui);
    ImGui::EndDisabled();
    
    ImGui::SeparatorText("Neural Rendering");

    float v = g.intensity.load();
    if (ImGui::SliderFloat("Overall Intensity", &v, 0.0f, 2.0f, "%.2f", 0))
        g.intensity.store(v);
    
    v = g.structure.load();
    if (ImGui::SliderFloat("Structure Intensity", &v, 0.0f, 3.0f, "%.2f", 0))
        g.structure.store(v);
    v = g.skin.load();
    if (ImGui::SliderFloat("Skin Structure Strength", &v, 0.0f, 3.0f, "%.2f", 0))
        g.skin.store(v);
    v = g.tone.load();
    if (ImGui::SliderFloat("Local Tone Strength", &v, 0.0f, 3.0f, "%.2f", 0))
        g.tone.store(v);

    int passes = g.passes.load();
    if (ImGui::SliderInt("Pass Count", &passes, 1, 3, "%d", 0))
        g.passes.store(passes);
    if (passes > 1)
        Note(kWarn, "Each pass is another full evaluation of the network: 2 costs twice what 1 "
                    "costs, 3 costs three times. It multiplies with Resolution Scale, and it "
                    "carries the same driver-reset risk. Leave it at 1 unless you are measuring.");

    ImGui::BeginDisabled();
    float dummy = 1.0f;
    int model = 0;
    ImGui::SliderFloat("Global Tone Strength", &dummy, 0.0f, 1.0f, "%.2f", 0);
    ImGui::Combo("Model", &model, "Model A\0");
    ImGui::Checkbox("Character Mask", &on);
    ImGui::EndDisabled();
    
    ImGui::SeparatorText("Guides");

    bool depth = g.useDepth.load();
    if (ImGui::Checkbox("Depth", &depth))
        g.useDepth.store(depth);
    bool inverted = g.depthInverted.load();
    if (ImGui::Checkbox("Depth Inverted", &inverted))
        g.depthInverted.store(inverted);
    bool mv = g.useMotion.load();
    if (ImGui::Checkbox("Motion Vectors", &mv))
    {
        g.useMotion.store(mv);
        Log("menu: motion %s", mv ? "on" : "off");
    }
    ImGui::TextDisabled("Estimated from the image, not read from the game.");
    if (mv)
        Note(kWarn, "The PS2 never computed per-pixel motion, so this is inferred by comparing "
                    "consecutive frames -- the same thing the NVIDIA route does on this target. It "
                    "is wrong where pixels move without the geometry moving: reflections, fire, "
                    "moving shadows, and anything appearing from behind something else. It also "
                    "does nothing on its own while the engine's history is off, which it currently "
                    "is: a motion vector says where a pixel was, and there is nothing kept to look "
                    "it up in.");
    ImGui::BeginDisabled();
    bool no = false;
    ImGui::Checkbox("Jitter", &no);
    ImGui::Checkbox("Exposure", &no);
    ImGui::EndDisabled();
    
    ImGui::SeparatorText("Performance");

    bool inl = g.inlineMode.load();
    if (ImGui::Checkbox("Apply On Same Frame", &inl))
    {
        g.inlineMode.store(inl);
        Log("menu: mode %s", inl ? "inline" : "async");
    }
    ImGui::TextDisabled(inl ? "this frame's residual -- the game waits for the network"
                            : "previous frame's residual -- the game does not wait");
    v = g.scale.load();
    if (ImGui::SliderFloat("Resolution Scale", &v, 0.25f, 2.0f, "%.2f", 0))
        g.scale.store(v);
    ImGui::TextDisabled("Cost grows with the square. 0.50 is about 16 ms on an RX 9070 XT, which "
                        "is already most of a 16.7 ms frame at 60 Hz.");
    if (v > 1.0f)
        Note(kDanger, "CAN RESET THE DISPLAY DRIVER. Above 1.00 one evaluation takes hundreds of "
                      "milliseconds. With Apply On Same Frame on, the game blocks for that whole "
                      "time, which trips the Windows driver timeout: the driver resets and the "
                      "game dies with DXGI_ERROR_DEVICE_REMOVED (887A0005). If you want to try "
                      "it anyway, turn Apply On Same Frame off first.");
    else if (v > 0.50f)
        Note(kWarn, "ABOVE 0.50 IS ALREADY RISKY. 0.50 is about 16 ms against a 16.7 ms frame, so "
                    "there is no headroom left: past it the evaluation stops fitting inside a "
                    "frame, you get skipped frames -- which is what flicker is -- and on a slower "
                    "card the stall can grow far enough to reset the display driver. Raise it only "
                    "if the skip count below stays low, and back off the moment it climbs.");
    ImGui::BeginDisabled();
    float ratio = 1.0f;
    ImGui::SliderFloat("Upscaling Ratio", &ratio, 1.0f, 2.0f, "%.2fx", 0);
    ImGui::EndDisabled();
    
    ImGui::SeparatorText("Status");

    const Profile &profile = ProfileForThisProcess();
    std::lock_guard guard(g.lock);
    if (g.unavailable)
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Unavailable: %s", g.reason);
    else if (g.failed)
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Stopped after an error. See the log.");
    else if (!g.enabled.load())
        ImGui::TextDisabled("Off.");
    else if (g.frame == 0)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "No frames processed yet.");
    else
    {
        const uint64_t seen = g.frame + g.skipped;
        const double pct = seen != 0 ? 100.0 * static_cast<double>(g.skipped) / seen : 0.0;
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                           "Running: %llu processed, %llu skipped (%.0f%%)",
                           static_cast<unsigned long long>(g.frame),
                           static_cast<unsigned long long>(g.skipped), pct);
        // A skipped frame reuses whatever the network textures hold, and a job that is still
        // running is writing them while compose reads them. A few percent is invisible; a third
        // of the frames is a correction that changes every frame, which reads as flicker.
        if (seen > 300 && pct >= 10.0)
            Note(kWarn, "High skip rate. The network is not finishing inside a frame, so the "
                        "correction being shown is stale or half-written and changes frame to "
                        "frame. That is what the flicker is. Lower Resolution Scale, set Pass "
                        "Count to 1, and lower the emulator's own upscale multiplier -- it is "
                        "competing for the same GPU.");
    }
    ImGui::Text("Target: %s", profile.note);
    if (g.outWidth != 0)
        ImGui::Text("Back buffer %ux%u  ->  network %ux%u", g.outWidth, g.outHeight, g.netWidth,
                    g.netHeight);
    if (g.depthBest != nullptr)
        ImGui::Text("Depth candidate: %ux%u format %d, %llu binds%s", g.depthWidth, g.depthHeight,
                    static_cast<int>(g.depthFormat),
                    static_cast<unsigned long long>(g.depthBinds),
                    g.useDepth.load() ? ", feeding it" : " (Depth switch is off)");
    else if (g.depthEvents == 0)
        ImGui::TextDisabled("Depth: ReShade has not delivered a single depth-stencil bind on this "
                            "API, so there is nothing to find. Not the same as the game having no "
                            "depth buffer.");
    else
        ImGui::TextDisabled("Depth: %llu binds seen, none usable (wrong format, multisampled, or "
                            "shader reads denied). See the log.",
                            static_cast<unsigned long long>(g.depthEvents));
    ImGui::TextDisabled("Log: dlss5-neural.log");
}

}

extern "C" __declspec(dllexport) const char *NAME = "dlss5 neural";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Runs the DLSS-NR network over the presented frame. D3D12, AMD GPU with HIP 7.";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module))
            return FALSE;
        {
            const auto log = ExeDirectory() / L"dlss5-neural.log";
            g_log = _wfopen(log.c_str(), L"w");
            Log("dlss5 neural: %s", ProfileForThisProcess().note);
        }
        // Kept even though it has never fired on D3D12: measured, PCSX2 on D3D12 delivers zero
        // depth-stencil binds in 600 frames, with or without also subscribing to the draw events.
        // The probe tells you why -- on D3D12 ReShade shows an add-on only the two swapchain
        // targets, while the same probe on D3D11 sees eight, depth included. See README,
        // "What the network can actually be fed".
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
            OnBindDepthStencil);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
        reshade::register_overlay("DLSS Neural Rendering (AMD)", OnOverlay);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_overlay("DLSS Neural Rendering (AMD)", OnOverlay);
        reshade::unregister_addon(module);
        if (g_log != nullptr)
            fclose(g_log), g_log = nullptr;
        break;
    }
    return TRUE;
}
