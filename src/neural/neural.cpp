// ReShade add-on: runs the DLSS-NR network over the presented frame on an AMD GPU via HIP.
// D3D12 only. One core, one table row per target.

#include <imgui.h>

#include <reshade.hpp>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <bcrypt.h>
#include <wrl/client.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <clocale>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
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

// The add-on must not drag d3d12.dll into a process that is not already using D3D12. A static
// import is resolved when ReShade loads the add-on -- in the middle of ReShade's own start-up,
// around the time the swapchain is created -- and that is when ReShade installs the delayed hooks
// it has waiting for d3d12.dll.
//
// Measured on NFS 2015, which is D3D11: an add-on subscribing to nothing and running no code at
// all still made the game's ResizeBuffers fail with DXGI_ERROR_INVALID_CALL, and the game quits
// over that with a DirectX error box. The probe add-on, which imports no graphics library at all,
// resized eight times in the same place without trouble. The import table was the only difference
// left, and these four exports are the whole of it.
//
// So resolve them by hand, and only once something is actually going to be built. On a D3D12
// target the game has already loaded d3d12.dll and this is just a reference count.
HMODULE g_d3d12Module = nullptr, g_compilerModule = nullptr;
decltype(&D3D12CreateDevice) p_D3D12CreateDevice = nullptr;
decltype(&D3D12SerializeRootSignature) p_D3D12SerializeRootSignature = nullptr;
pD3DCompile p_D3DCompile = nullptr;

void Log(const char *fmt, ...);
std::filesystem::path ExeDirectory();

// Bringing d3d12.dll into a game that is not already using D3D12 is what broke this. ReShade
// keeps a set of hooks waiting for that library and installs them the moment it arrives -- its
// own log names it: "Installing delayed hooks for d3d12.dll (Just loaded via LoadLibrary)". Doing
// that while a D3D11 swapchain is already live leaves the swapchain in a state DXGI will not
// resize: the next ResizeBuffers returns DXGI_ERROR_INVALID_CALL, and a game that checks the
// result -- NFS 2015 does -- puts up a DirectX error box and quits.
//
// Measured, on NFS, with the add-on otherwise doing nothing at all:
//   d3d12.dll never loaded         -> 5 resizes, 0 failures, runs
//   d3d12.dll loaded, no device    -> first resize afterwards fails, game quits
// Creating the device is not needed to trigger it; the load alone does. Loading it below
// LoadLibrary, through LdrLoadDll, does not help either, because ReShade is told about the module
// by the loader's notification callback however it arrives.
//
// So the library has to arrive under a name ReShade has no hooks registered against. A private
// copy beside the exe gets its own module identity -- module identity is the full path -- and
// with it a clean export table: GetProcAddress then returns the real D3D12CreateDevice rather
// than ReShade's, which also keeps our device out of ReShade's bookkeeping entirely. That is the
// right relationship anyway: ReShade manages the game's device, this one is ours.
//
// A game already on D3D12 needs none of this. Its d3d12.dll is loaded and hooked long before the
// add-on exists, and the handle is simply reused.
// Exactly as many characters as "d3d12.dll". An import name lives in the file as a
// null-terminated string, so a same-length replacement can be written straight over it -- which
// is what lets the NR runtime be pointed at this copy instead of the system one.
constexpr wchar_t kPrivateD3D12[] = L"dx12p.dll";
static_assert(sizeof(kPrivateD3D12) == sizeof(L"d3d12.dll"));
constexpr char kSystemD3D12Ansi[] = "d3d12.dll";
constexpr char kPrivateD3D12Ansi[] = "dx12p.dll";

// Null unless D3D12 had to be brought in privately. On a game already using D3D12 there is
// nothing to avoid and everything below is skipped.
const wchar_t *g_privateD3D12 = nullptr;

HMODULE LoadPrivateD3D12()
{
    // A game already on D3D12 needs none of this: its d3d12.dll was loaded and hooked long before
    // the add-on existed, and reusing the handle changes nothing.
    if (HMODULE already = GetModuleHandleW(L"d3d12.dll"); already != nullptr)
        return already;

    wchar_t system32[MAX_PATH] {};
    if (GetSystemDirectoryW(system32, MAX_PATH) == 0)
        return LoadLibraryW(L"d3d12.dll");
    const std::filesystem::path sys = system32;
    const std::filesystem::path dir = ExeDirectory() / L"dlss5-runtime";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::create_directories(dir / L"D3D12", ec);
    auto place = [&](const wchar_t *from, const std::filesystem::path &to) {
        std::error_code e;
        const auto source = sys / from;
        // Same size means a copy from an earlier run is still current. It may also be mapped by
        // this very process already, which would make overwriting it fail for no good reason.
        if (std::filesystem::exists(to, e) &&
            std::filesystem::file_size(to, e) == std::filesystem::file_size(source, e))
            return true;
        std::filesystem::copy_file(source, to, std::filesystem::copy_options::overwrite_existing, e);
        return std::filesystem::exists(to);
    };

    // D3D12.dll will not come up without its core component -- D3D12_ERROR_INVALID_REDIST,
    // 0x887E0003, measured -- and a copy loaded from outside the system directory looks for that
    // core beside itself, or in a D3D12 subdirectory, rather than back in System32. Provide both,
    // and load with LOAD_WITH_ALTERED_SEARCH_PATH so its own directory is searched first.
    if (!place(L"D3D12.dll", dir / kPrivateD3D12) ||
        !place(L"D3D12Core.dll", dir / L"D3D12Core.dll"))
    {
        Log("could not place a private copy of D3D12; falling back to the system one, which will "
            "upset a D3D11 game's next resize.");
        return LoadLibraryW(L"d3d12.dll");
    }
    place(L"D3D12Core.dll", dir / L"D3D12" / L"D3D12Core.dll");

    HMODULE loaded = LoadLibraryExW((dir / kPrivateD3D12).c_str(), nullptr,
                                    LOAD_WITH_ALTERED_SEARCH_PATH);
    if (loaded == nullptr)
    {
        Log("the private D3D12 copy would not load (%lu); falling back to the system one.",
            GetLastError());
        return LoadLibraryW(L"d3d12.dll");
    }
    g_privateD3D12 = kPrivateD3D12;
    Log("D3D12 loaded privately as %ls, so ReShade installs no d3d12 hooks over the live "
        "swapchain.", kPrivateD3D12);
    return loaded;
}

bool LoadGraphicsApi()
{
    if (p_D3D12CreateDevice != nullptr)
        return true;
    // The compiler is already in the process (ReShade uses it) and has no delayed hooks waiting,
    // so it needs none of this.
    g_compilerModule = GetModuleHandleW(L"d3dcompiler_47.dll");
    if (g_compilerModule == nullptr)
        g_compilerModule = LoadLibraryW(L"d3dcompiler_47.dll");
    g_d3d12Module = LoadPrivateD3D12();
    if (g_d3d12Module == nullptr || g_compilerModule == nullptr)
    {
        Log("d3d12.dll or d3dcompiler_47.dll could not be loaded; the add-on cannot run here.");
        return false;
    }
    p_D3D12CreateDevice = reinterpret_cast<decltype(p_D3D12CreateDevice)>(
        GetProcAddress(g_d3d12Module, "D3D12CreateDevice"));
    p_D3D12SerializeRootSignature = reinterpret_cast<decltype(p_D3D12SerializeRootSignature)>(
        GetProcAddress(g_d3d12Module, "D3D12SerializeRootSignature"));
    p_D3DCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(g_compilerModule, "D3DCompile"));
    const bool ok = p_D3D12CreateDevice != nullptr && p_D3D12SerializeRootSignature != nullptr &&
                    p_D3DCompile != nullptr;
    if (!ok)
    {
        Log("one of the graphics entry points could not be resolved.");
        p_D3D12CreateDevice = nullptr;
    }
    return ok;
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

// Downscaling with a bilinear sampler reads four texels no matter how many the destination
// pixel actually covers. At 0.50 scale that throws away half the image and keeps its aliasing,
// and the network then spends its capacity denoising the aliasing instead of the picture --
// which is one of the reasons the result reads as a colour change rather than detail. Average
// the whole footprint instead. RenoDX exposes the same choice as its Downsample Filter.
constexpr char kCopyShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float intensity; uint pad; };
float3 ToLinear(float3 c){ return c <= 0.04045 ? c/12.92 : pow(abs(c+0.055)/1.055, 2.4); }
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 float4 c;
 if (sw > dw || sh > dh) {
  float2 r = float2(sw,sh)/float2(dw,dh);
  int2 a = int2(floor(float2(p.xy) * r));
  int2 b = min(int2(ceil(float2(p.xy+1) * r)) - 1, int2(sw-1,sh-1));
  b = min(b, a + 3);                       // 4x4 covers every scale down to 0.25
  float4 acc = 0; float n = 0;
  for (int y=a.y; y<=b.y; ++y) for (int x=a.x; x<=b.x; ++x) { acc += src.Load(int3(x,y,0)); n += 1; }
  c = acc / max(n, 1);
 } else {
  c = src.SampleLevel(smp, (float2(p.xy)+0.5)/float2(dw,dh), 0);
 }
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

// The network runs at a fraction of the back buffer and only its *correction* comes back to
// full resolution -- same residual scheme RenoDX uses. Which filter carries that correction up
// decides how much of it survives: a bilinear stretch is a box blur at these ratios, so every
// high-frequency thing the network did was averaged away before it reached the screen and all
// that was left was the low-frequency part, i.e. colour and brightness. Catmull-Rom keeps the
// detail. It can ring on hard edges, so the bilinear path stays selectable, exactly like
// RenoDX's Upsample Filter. `pad` is packed: bit 0 is the filter (0 bilinear, 1 bicubic), bits
// 1+ are the debug view. The root signature is fixed at 8 constants and every shader shares that
// layout, so a new field rides in the spare word instead of widening all six.
//
// The debug views answer "is any of this reaching the screen", which a still frame cannot: the
// residual is a few percent of the signal, so at 1:1 it is invisible even when it is correct.
// Residual x8 shows the correction on its own against mid grey -- flat grey means the network
// returned its input, and structure means it did not.
// Subtracting the network's output from its input has to happen while both still describe the
// same frame, and compose is not always that moment. In async mode the engine works on its own
// timeline, so by the time compose runs the next frame has already overwritten netColour with
// fresh input and netBase with a copy of it -- the two are then identical and the correction is
// exactly zero. That is the whole of "turning Apply On Same Frame off makes the effect vanish".
// Capture the difference into its own texture at a point where the pair is known to match, and
// let compose read only that.
constexpr char kResidualShader[] = R"(
Texture2D<float4> nr   : register(t0);
Texture2D<float4> base : register(t1);
RWTexture2D<float4> dst : register(u0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float extra; uint pad; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 dst[p.xy] = float4((nr.Load(int3(p.xy,0)) - base.Load(int3(p.xy,0))).rgb, 0);
})";

constexpr char kComposeShader[] = R"(
Texture2D<float4> full : register(t0);
Texture2D<float4> res  : register(t1);
Texture2D<float4> dbgs : register(t2);
RWTexture2D<float4> dst : register(u0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float intensity; uint pad; };
float3 ToLinear(float3 c){ return c <= 0.04045 ? c/12.92 : pow(abs(c+0.055)/1.055, 2.4); }
float3 ToSrgb(float3 c){ return c <= 0.0031308 ? c*12.92 : 1.055*pow(abs(c), 1.0/2.4) - 0.055; }
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dw || p.y>=dh) return;
 float2 uv = (float2(p.xy)+0.5)/float2(dw,dh);
 float3 fix;
 if ((pad & 1) != 0) {
  float2 f = uv * float2(sw,sh) - 0.5;
  int2 i0 = int2(floor(f));
  float2 t = f - float2(i0), t2 = t*t, t3 = t2*t;
  float wx[4] = { -0.5*t3.x + t2.x - 0.5*t.x,  1.5*t3.x - 2.5*t2.x + 1.0,
                  -1.5*t3.x + 2.0*t2.x + 0.5*t.x,  0.5*t3.x - 0.5*t2.x };
  float wy[4] = { -0.5*t3.y + t2.y - 0.5*t.y,  1.5*t3.y - 2.5*t2.y + 1.0,
                  -1.5*t3.y + 2.0*t2.y + 0.5*t.y,  0.5*t3.y - 0.5*t2.y };
  fix = 0;
  [unroll] for (int y=0; y<4; ++y) [unroll] for (int x=0; x<4; ++x) {
   int2 c4 = clamp(i0 + int2(x-1,y-1), int2(0,0), int2(sw-1,sh-1));
   fix += res.Load(int3(c4,0)).rgb * (wx[x]*wy[y]);
  }
 } else {
  fix = res.SampleLevel(smp,uv,0).rgb;
 }
 fix *= intensity;
 uint dbg = pad >> 1;
 if (dbg != 0) {
  float3 d;
  if      (dbg == 3) d = 0.5 + fix * 8.0;                     // the correction, on its own
  else if (dbg == 4) {                                        // motion, in raster pixels
   float2 m = dbgs.SampleLevel(smp,uv,0).xy;
   d = float3(0.5 + m.x/32.0, 0.5 + m.y/32.0, 0.5);
  }
  else if (dbg == 5) d = dbgs.SampleLevel(smp,uv,0).xxx * max(intensity,1e-3);
  else               d = dbgs.SampleLevel(smp,uv,0).rgb;      // 1 input / 2 output
  dst[p.xy] = float4(saturate(d), 1.0);
  return;
 }
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
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint useGuess; float gate; float step; float ratio; };
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
 if (mx - mn < gate) { dst[p.xy] = float2(0,0); return; }

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
 if (useGuess == 0 && best > zero * ratio) bestD = int2(0,0);
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
 float2 v = src.SampleLevel(smp,uv,0);
 // mode 0: the estimator's field, already in coarse pixels, scaled to raster pixels by `step`.
 // mode 1: the game's own velocity buffer, which engines store as a UV-space delta, so it
 // becomes pixels by multiplying by the target size. `step` stays a plain multiplier there --
 // it carries the sign and any remaining convention difference. See MotionScale.
 dst[p.xy] = mode ? v * float2(dw,dh) * step : v * step;
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

// One texture that lives on both devices at once. Created on D3D11 (the game's device owns it),
// shared by NT handle, opened on ours. Ported from session.cpp, where this transport is measured
// and working: submit 0.21-0.27 ms, return 0.066 ms.
struct Bridge
{
    const char *name = "";
    ComPtr<ID3D11Texture2D> on11;
    HANDLE handle = nullptr;
    ComPtr<ID3D12Resource> on12;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    void Destroy()
    {
        on12.Reset();
        if (handle != nullptr)
            CloseHandle(handle);
        handle = nullptr;
        on11.Reset();
        width = height = 0;
        format = DXGI_FORMAT_UNKNOWN;
    }

    bool Ensure(ID3D11Device5 *game11, ID3D12Device *own, UINT w, UINT h, DXGI_FORMAT fmt,
                bool uav = false)
    {
        if (on12 != nullptr && width == w && height == h && format == fmt)
            return true;
        Destroy();
        D3D11_TEXTURE2D_DESC td {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        HRESULT hr = game11->CreateTexture2D(&td, nullptr, &on11);
        if (FAILED(hr))
        {
            Log("bridge %s: CreateTexture2D %ux%u fmt %u failed 0x%08lX.", name, w, h,
                static_cast<unsigned>(fmt), hr);
            return false;
        }
        ComPtr<IDXGIResource1> res;
        if (FAILED(on11.As(&res)) ||
            FAILED(res->CreateSharedHandle(
                nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle)) ||
            FAILED(own->OpenSharedHandle(handle, IID_PPV_ARGS(&on12))))
        {
            Log("bridge %s: sharing failed.", name);
            return false;
        }
        width = w;
        height = h;
        format = fmt;
        Log("bridge %s ready: %ux%u fmt %u, shared D3D11 <-> D3D12", name, w, h,
            static_cast<unsigned>(fmt));
        return true;
    }
};

// A guide buffer the game itself renders -- depth, or real motion vectors -- carried from the
// game's D3D11 device to ours. This is the whole difference between what PCSX2 can feed the
// network and what a modern engine can: PS2 has no velocity buffer at all and its depth only
// exists between a bind and a clear, so the add-on had to *estimate* motion from the image and
// go without depth. Frostbite writes both every frame, at screen resolution, and leaves them
// readable at present time. Three of the Packet's four slots become real instead of one.
//
// One copy per frame, taken at present. The copy is what makes it safe to read: the game keeps
// binding and clearing its own buffer, and a view kept on the live resource reads whatever the
// next pass left there.
struct Guide
{
    const char *name = "";
    // What the bind observation picked, and how often the game bound it. Most-bound wins: the
    // main scene pass outbinds shadow maps and reflection probes by an order of magnitude.
    ComPtr<ID3D11Resource> chosen;
    UINT chosenBinds = 0;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D11Texture2D> snap;  // private, never bound as a target, so an SRV over it survives
    UINT snapW = 0, snapH = 0;
    DXGI_FORMAT snapFmt = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11ShaderResourceView> srv;
    ID3D11Texture2D *srvOf = nullptr;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ID3D11Texture2D *uavOf = nullptr;

    Bridge bridge;
    ComPtr<ID3D12Resource> local;  // our own copy, so SRV reads never lean on state promotion
    bool ready = false;
    bool logged = false;
    bool failed = false;
};

// D3D11 compute that reads a depth-stencil as a single float and writes plain R32_FLOAT. The
// conversion has to happen on this side: R32G8X24_TYPELESS and R24G8_TYPELESS do not open on a
// second device, while R32_FLOAT does. Verbatim from session.cpp, where it is measured working.
constexpr char kGuideDepthCs[] = R"(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {
 uint w, h; dst.GetDimensions(w, h);
 if (p.x >= w || p.y >= h) return;
 dst[p.xy] = src.Load(int3(p.xy, 0));
})";

// True for the depth-stencil formats whose single-float alias can be read through an SRV.
DXGI_FORMAT GuideDepthSrvFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// Bind tallies for the current frame, one entry per resource. Most-bound wins, which is what
// separates the main scene pass from shadow maps, reflection probes and the dozens of
// downsample steps a modern engine also renders into -- the probe log for NFS lists 45 render
// targets in a frame, and the two that matter are bound two orders of magnitude more than the
// rest. Cleared every present.
// The value holds a reference. Keying on the raw pointer is only an identity test; keeping one
// is not safe, because a target the game releases between the bind and the present leaves a
// dangling pointer that the next QueryInterface walks into -- measured, as an access violation
// on the first frame. The description is read here too, while the resource is provably alive.
struct Tallied
{
    ComPtr<ID3D11Resource> res;
    UINT binds = 0;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
std::unordered_map<void *, Tallied> g_depthTally, g_motionTally;

// Takes the frame's tallies and settles which resource each guide reads from. A guide that
// changes resource mid-run drops its snapshot and views, which Ensure rebuilds -- the same
// orphaned-view trap the depth path already paid for once.
void SettleGuide(Guide &guide, std::unordered_map<void *, Tallied> &tally)
{
    const Tallied *best = nullptr;
    for (const auto &entry : tally)
        if (best == nullptr || entry.second.binds > best->binds)
            best = &entry.second;
    if (best == nullptr)
    {
        tally.clear();
        return;
    }
    const bool same = guide.chosen.Get() == best->res.Get();
    guide.chosenBinds = best->binds;
    if (!same)
    {
        guide.chosen = best->res;  // takes a reference; the tally's is about to go
        guide.width = best->width;
        guide.height = best->height;
        guide.format = best->format;
        guide.ready = false;
        guide.logged = false;
        guide.failed = false;
        Log("guide %s: taking %ux%u format %u, bound %u times this frame", guide.name,
            best->width, best->height, static_cast<unsigned>(best->format), best->binds);
    }
    tally.clear();
}

struct State
{
    std::mutex lock;

    bool unavailable = false;
    bool loggedWrongApi = false;
    std::atomic<bool> enabled { true };
    std::atomic<float> structure { 1.0f };
    std::atomic<float> tone { 1.0f };
    std::atomic<float> skin { 1.0f };
    std::atomic<int> passes { 1 };
    std::atomic<float> scale { 0.5f };
    std::atomic<bool> inlineMode { true };
    std::atomic<int> encoding { 0 };
    // 100 nits is the automatic ShortFuse documents for linear BT.709; the old 500 here
    // matched none of the documented conventions (100 / 203 / 250).
    std::atomic<float> diffuseWhite { 100.0f };
    std::atomic<float> intensity { 1.0f };
    std::atomic<bool> bicubic { true };
    std::atomic<int> debugView { 0 };
    std::atomic<bool> measureNow { false };
    std::atomic<float> flowGate { 0.02f };
    std::atomic<float> flowRatio { 0.70f };
    UINT loadedPasses = 0;
    bool loggedPassLimit = false;
    bool failed = false;
    const char *reason = "";

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;

    // The bridge. On D3D11 the game's device cannot run the network, so we make our own D3D12
    // device on the same physical adapter and the two talk through shared textures and fences.
    // Step 1 of that build: bring the device up and prove it landed on the right adapter.
    ComPtr<ID3D11Device5> game11;
    ComPtr<ID3D11DeviceContext4> game11ctx;
    ComPtr<ID3D12Device> workDevice;
    ComPtr<ID3D12CommandQueue> workQueue;
    bool bridgeFailed = false;
    bool loggedBridge = false;
    Bridge bridgeIn { "colour-in" };
    Bridge bridgeOut { "result-out" };
    ComPtr<ID3D12Resource> crossLocal;
    // Private, unshared, on the game's device. The back buffer is copied to and from these, and
    // only these are copied to and from the cross-device shared textures. Copying the back buffer
    // straight into a resource shared with another device enrolls it in a kernel-level sharing
    // dependency, and DXGI will not resize a swapchain whose buffers are still enrolled in one.
    // Two extra full-res copies a frame is a fraction of a millisecond; the resize is not
    // negotiable.
    ComPtr<ID3D11Texture2D> stageIn11, stageOut11;
    UINT stageW = 0, stageH = 0;
    DXGI_FORMAT stageFmt = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12Fence> crossFence, backFence;
    HANDLE crossHandle = nullptr, backHandle = nullptr;
    ComPtr<ID3D11Fence> crossOn11, backOn11;
    UINT64 crossValue = 0, backValue = 0;
    static constexpr UINT kRing = 3;
    ComPtr<ID3D12CommandAllocator> alloc[kRing];
    ComPtr<ID3D12GraphicsCommandList> list[kRing];
    ComPtr<ID3D12Fence> ringFence;
    UINT64 ringValue[kRing] {}, ringSerial = 0;
    HANDLE ringEvent = nullptr;
    // A separate handle on purpose. These are auto-reset events, and two fences with a
    // SetEventOnCompletion registration on the same handle steal each other's signal: the
    // waiter that loses then sits out its full timeout, every frame. That is a stall a few
    // frames in, once the ring starts interleaving with the completion wait.
    HANDLE completionEvent = nullptr;
    bool loggedRound = false;

    static constexpr UINT kMaxPasses = 10;
    static constexpr UINT kInlineMaxPasses = 3;
    bool loggedInlineCap = false;
    bool loggedDeviceLost = false;
    bool loggedNoBackBuffer = false;
    // A bridge failure is usually transitory -- a rebuild caught mid-flight -- so it costs a
    // rebuild, not the rest of the run.
    static constexpr UINT kMaxBridgeRetries = 10;
    UINT bridgeRetries = 0;
    // Safety net: presents seen since the bridge last finished a frame.
    uint64_t presentsSinceFrame = 0;
    uint64_t lastSeenFrame = 0;
    HMODULE runtimes[kMaxPasses] {};
    UINT lastJobs[kMaxPasses] {};
    UINT activePasses = 0;
    HipSetFn hipSet = nullptr;
    int hipDevice = -1;
    bool engineReady = false;

    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> copyPipeline, depthPipeline, composePipeline, residualPipeline;
    ComPtr<ID3D12PipelineState> lumaPipeline, flowPipeline, flowUpPipeline;
    ComPtr<ID3D12DescriptorHeap> heap;

    UINT outWidth = 0, outHeight = 0, netWidth = 0, netHeight = 0;
    DXGI_FORMAT outFormat = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D12Resource> netColour;
    ComPtr<ID3D12Resource> netBase;
    ComPtr<ID3D12Resource> netResidual;
    ComPtr<ID3D12Resource> netMotion;
    ComPtr<ID3D12Resource> lumaA, lumaB, flowSmall, flowCoarse;
    ComPtr<ID3D12Resource> history;
    std::atomic<bool> useHistory { true };
    bool historyValid = false;
    bool loggedHistory = false;
    UINT flowWidth = 0, flowHeight = 0;
    std::atomic<bool> useMotion { true };
    bool loggedFlow = false;
    // Separate from loggedFlow on purpose. The estimator logs on the first frame, long before a
    // game reaches a scene that binds a velocity buffer, and a single shared flag would mean the
    // switch to the game's own vectors -- the thing worth knowing -- never printed.
    bool loggedGameMotion = false;
    ComPtr<ID3D12Resource> flowReadLuma, flowReadFlow;
    bool pendingFlow = false;
    bool flowProbed = false;
    // Reads back what the engine is actually handed for depth and motion. A debug view cannot
    // answer this: "Depth x500" was scaled for the PS2, whose depth peaks near 0.002, so on an
    // engine with a normal 0..1 range it saturates to white and a correct buffer looks like
    // nothing at all. Numbers do not have that problem.
    ComPtr<ID3D12Resource> guideReadDepth, guideReadMotion;
    bool pendingGuides = false;
    std::atomic<bool> probeGuides { true };
    uint64_t nextGuideProbe = 600;
    // Last numbers the probe saw, for the overlay. A menu has no depth and nothing moving, so a
    // flat reading there means nothing -- these only become meaningful in a real scene, which is
    // why the probe keeps re-arming until it sees one.
    std::atomic<float> probeDepthMin { 0.0f }, probeDepthMax { 0.0f };
    std::atomic<float> probeMotionMean { 0.0f }, probeMotionMax { 0.0f };
    std::atomic<int> probeStillPct { -1 };
    std::atomic<bool> guidesLookReal { false };
    // Depth debug view scale. The PS2 peaks near 0.002 so it needed x500; a modern engine fills
    // 0..1 and x500 is pure white, which reads as "the view is broken". The probe sets this from
    // the range it actually measured, so one view works on both.
    std::atomic<float> depthDebugScale { 500.0f };
    ComPtr<ID3D12Resource> netDepth;
    ComPtr<ID3D12Resource> depthAlias;
    ComPtr<ID3D12Resource> depthSnapshot;
    UINT64 depthClears = 0;
    bool loggedSnapshot = false;

    // The D3D11 guide path. Depth and motion straight from the game, for engines that render
    // them -- as opposed to the D3D12 pre-clear snapshot above, which is what PCSX2 needed.
    Guide guideDepth { "depth" };
    Guide guideMotion { "motion" };
    ComPtr<ID3D11ComputeShader> guideDepthCs;
    bool guideDepthCsFailed = false;
    std::atomic<bool> useGameGuides { true };
    // Diagnostic. Runs the entire bridge but never touches the swapchain image, which is the
    // only way to tell a back-buffer reference apart from anything else the add-on does to the
    // device. Picture is untouched with this on; it is not a usable mode.
    std::atomic<bool> noBackBuffer { false };
    // Diagnostic. Loads and observes but never stands the bridge up: no second D3D12 device, no
    // runtime, no shared textures. Does nothing to the picture; it exists to tell "the add-on
    // being attached at all" apart from "what the bridge does to the game's device".
    std::atomic<bool> noBridge { false };
    // Diagnostic ladder for the D3D11 path: 1 stops after d3d12.dll is loaded, 2 stops after our
    // D3D12 device is created, 3 (the default) is the whole bridge. Tells "the library arriving"
    // apart from "a second device existing" apart from "the bridge running".
    std::atomic<int> stage { 3 };
    // Which ReShade events to subscribe to, as a bitmask: 1 bind-render-targets, 2 draw and
    // draw-indexed, 4 clear-depth-stencil, 8 destroy-swapchain, 16 the overlay. Subscribing is
    // not free -- ReShade only turns on the tracking an event needs when something asks for it --
    // so this exists to tell which subscription costs what. Default is everything.
    int events = 31;
    // Which swapchain is mid-teardown, if any. ResizeBuffers runs on its own thread -- measured,
    // thread 35300 while the render thread was elsewhere -- so without a gate the render thread
    // can enter present and take hold of the back buffer again between the teardown and the
    // actual resize, and DXGI then refuses the resize.
    //
    // It holds the swapchain rather than a flag because a game can have more than one alive:
    // PCSX2 destroys and creates them (destroy_swapchain arrives with resize=false), and the
    // init of the new one can land before the destroy of the old. A single flag then latched on
    // for good and the add-on stopped presenting entirely -- stuck at "5 processed", with the
    // game still rendering. Keyed on identity, a stale teardown can only ever gate its own.
    std::atomic<void *> goneSwapchain { nullptr };
    // Frostbite stores velocity as a UV-space delta; the engine reads motion in raster pixels,
    // so the field is multiplied by the target size. Sign and magnitude are engine convention,
    // not something that can be read off the resource, so leave the knob: -1 flips the direction,
    // and a value other than 1 rescales. Watch Debug View "Motion vectors" while panning.
    std::atomic<float> motionScale { 1.0f };
    bool gameMotionActive = false, gameDepthActive = false;

    ID3D12Resource *depthCandidate = nullptr;
    UINT depthWidth = 0, depthHeight = 0;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    UINT depthBinds = 0, depthBestBinds = 0;
    ID3D12Resource *depthBest = nullptr;
    std::atomic<bool> useDepth { true };
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
    float pinnedScale = -1.0f;
    UINT measureTries = 0;
    UINT64 depthEvents = 0;
};

State g;

// Settings live in an ini next to the exe. Two reasons, both practical: nothing in the overlay
// survived a restart, so every A/B test meant re-dialling half a dozen sliders by hand; and a
// headless run had no way to reach them at all, which made a parameter sweep impossible to
// automate. GetPrivateProfile* is the Win32 ini reader -- no parser to write or get wrong.
void LoadSettings()
{
    const auto ini = (ExeDirectory() / L"dlss5-neural.ini").wstring();
    auto num = [&](const wchar_t *key, float fallback) {
        wchar_t buf[64] {};
        if (GetPrivateProfileStringW(L"dlss5", key, L"", buf, 64, ini.c_str()) == 0)
            return fallback;
        // wcstof honours the process locale's decimal separator. On a pt-BR install that is a
        // comma, so "0.50" parses as 0 and stops at the dot -- Scale=0.50 silently became the
        // clamp floor of 0.25. The file is ours and always writes a dot, so parse it in the C
        // locale regardless of what the user's machine is set to.
        static _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
        wchar_t *end = nullptr;
        const float v = c_locale != nullptr ? static_cast<float>(_wcstod_l(buf, &end, c_locale))
                                            : std::wcstof(buf, &end);
        return end == buf ? fallback : v;
    };
    auto flag = [&](const wchar_t *key, bool fallback) {
        return num(key, fallback ? 1.0f : 0.0f) != 0.0f;
    };

    g.scale.store(std::clamp(num(L"Scale", g.scale.load()), 0.25f, 2.0f));
    g.passes.store(std::clamp(static_cast<int>(num(L"Passes", 1.0f)), 1,
                             static_cast<int>(State::kMaxPasses)));
    g.intensity.store(num(L"Intensity", g.intensity.load()));
    g.structure.store(num(L"Structure", g.structure.load()));
    g.skin.store(num(L"Skin", g.skin.load()));
    g.tone.store(num(L"Tone", g.tone.load()));
    g.flowGate.store(num(L"FlowGate", g.flowGate.load()));
    g.flowRatio.store(num(L"FlowRatio", g.flowRatio.load()));
    g.encoding.store(static_cast<int>(num(L"Encoding", 0.0f)));
    g.diffuseWhite.store(num(L"DiffuseWhite", g.diffuseWhite.load()));
    g.debugView.store(std::clamp(static_cast<int>(num(L"DebugView", 0.0f)), 0, 5));
    g.inlineMode.store(flag(L"Inline", g.inlineMode.load()));
    g.bicubic.store(flag(L"Bicubic", g.bicubic.load()));
    g.useMotion.store(flag(L"Motion", g.useMotion.load()));
    g.useHistory.store(flag(L"History", g.useHistory.load()));
    g.useDepth.store(flag(L"Depth", g.useDepth.load()));
    g.useGameGuides.store(flag(L"GameGuides", g.useGameGuides.load()));
    g.noBackBuffer.store(flag(L"NoBackBuffer", g.noBackBuffer.load()));
    g.noBridge.store(flag(L"NoBridge", g.noBridge.load()));
    g.stage.store(static_cast<int>(num(L"Stage", 3.0f)));
    g.events = static_cast<int>(num(L"Events", 31.0f));
    g.motionScale.store(num(L"MotionScale", g.motionScale.load()));

    Log("settings: scale %.2f passes %d intensity %.2f structure %.2f skin %.2f tone %.2f "
        "inline %d bicubic %d motion %d history %d gate %.3f ratio %.2f debug %d",
        static_cast<double>(g.scale.load()), g.passes.load(),
        static_cast<double>(g.intensity.load()), static_cast<double>(g.structure.load()),
        static_cast<double>(g.skin.load()), static_cast<double>(g.tone.load()),
        g.inlineMode.load() ? 1 : 0, g.bicubic.load() ? 1 : 0, g.useMotion.load() ? 1 : 0,
        g.useHistory.load() ? 1 : 0, static_cast<double>(g.flowGate.load()),
        static_cast<double>(g.flowRatio.load()), g.debugView.load());
}

// Our own D3D12 device on the adapter the game is already using, so shared textures and fences
// stay on one GPU and never touch system memory. Modelled on session.cpp::Session::CreateOn,
// which is the measured, working version of this transport.
// The adapter comes from the game's own D3D11 device, which BridgeStep1 has already asked. That
// is the same adapter by construction, and it means no DXGI factory has to be created -- one
// fewer library to touch, and one fewer chance to disturb the runtime the game is using.
bool CreateWorkDevice(IDXGIAdapter *adapter, LUID luid)
{
    DXGI_ADAPTER_DESC ad {};
    adapter->GetDesc(&ad);
    const HRESULT hr =
        p_D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.workDevice));
    if (FAILED(hr))
    {
        Log("bridge: D3D12CreateDevice failed 0x%08lX.", hr);
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g.workDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.workQueue))))
    {
        Log("bridge: could not create the work queue.");
        return false;
    }
    for (UINT i = 0; i < State::kRing; ++i)
    {
        if (FAILED(g.workDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&g.alloc[i]))) ||
            FAILED(g.workDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   g.alloc[i].Get(), nullptr,
                                                   IID_PPV_ARGS(&g.list[i]))))
        {
            Log("bridge: could not create the command allocator or list.");
            return false;
        }
        g.list[i]->Close();
    }
    if (FAILED(g.workDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.ringFence))) ||
        FAILED(g.workDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.crossFence))) ||
        FAILED(g.workDevice->CreateSharedHandle(g.crossFence.Get(), nullptr, GENERIC_ALL, nullptr,
                                                &g.crossHandle)) ||
        FAILED(g.workDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.backFence))) ||
        FAILED(g.workDevice->CreateSharedHandle(g.backFence.Get(), nullptr, GENERIC_ALL, nullptr,
                                                &g.backHandle)))
    {
        Log("bridge: could not create the shared fences.");
        return false;
    }
    g.ringEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    char name[128] {};
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
    const LUID got = g.workDevice->GetAdapterLuid();
    Log("bridge: own D3D12 device on %s (vendor %04X). LUID game %08lX:%08lX, ours %08lX:%08lX -> "
        "%s adapter.", name, ad.VendorId, static_cast<unsigned long>(luid.HighPart),
        luid.LowPart, static_cast<unsigned long>(got.HighPart), got.LowPart,
        (got.HighPart == luid.HighPart && got.LowPart == luid.LowPart) ? "same" : "DIFFERENT");
    return true;
}

// D3D11 path, step 1. Brings up the work device and stops there: the network still does not run
// on this API, so the picture is untouched. What this proves, and all it proves, is that the
// second device comes up on the same GPU as the game.
void BridgeStep1(device *dev)
{
    if (g.bridgeFailed || g.workDevice != nullptr)
        return;
    // First point at which D3D12 is genuinely needed. Everything before this runs without
    // d3d12.dll ever entering the process.
    if (!LoadGraphicsApi())
    {
        g.bridgeFailed = true;
        return;
    }
    if (g.stage.load() < 2)
        return;
    auto *native = reinterpret_cast<ID3D11Device *>(dev->get_native());
    if (native == nullptr || FAILED(native->QueryInterface(IID_PPV_ARGS(&g.game11))))
    {
        Log("bridge: the game's device does not expose ID3D11Device5 (needs Windows 10 1703+).");
        g.bridgeFailed = true;
        return;
    }
    ComPtr<ID3D11DeviceContext> ctx;
    g.game11->GetImmediateContext(&ctx);
    if (ctx == nullptr || FAILED(ctx.As(&g.game11ctx)))
    {
        Log("bridge: the game's context does not expose ID3D11DeviceContext4.");
        g.bridgeFailed = true;
        return;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC ad {};
    if (FAILED(g.game11.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&ad)))
    {
        Log("bridge: could not read the game adapter's LUID.");
        g.bridgeFailed = true;
        return;
    }
    if (!CreateWorkDevice(adapter.Get(), ad.AdapterLuid))
    {
        g.bridgeFailed = true;
        return;
    }
    if (g.stage.load() < 3)
    {
        Log("stage %d: stopping after the work device, on purpose.", g.stage.load());
        return;
    }
    if (FAILED(g.game11->OpenSharedFence(g.crossHandle, IID_PPV_ARGS(&g.crossOn11))) ||
        FAILED(g.game11->OpenSharedFence(g.backHandle, IID_PPV_ARGS(&g.backOn11))))
    {
        Log("bridge: OpenSharedFence on the game's device failed.");
        g.bridgeFailed = true;
        return;
    }
    // From here the network's device IS the work device. Everything downstream -- InitPipeline,
    // EnsureResources, InitEngine, the Packet -- already runs on whatever g.device points at, so
    // this one assignment is what moves the whole pipeline off the game's device.
    g.device = g.workDevice;
    g.queue = g.workQueue;
    Log("bridge: up. The network now runs on our own device; colour crosses in and the result "
        "crosses back.");
}

// The flow probe and the residual measurement copy into readback buffers during recording; this
// is where the results are actually read back and logged. It lived inline in OnPresent, which
// meant the bridge path -- which never reaches that tail -- recorded the copies and then threw
// the numbers away, so a D3D11 run produced no measurement at all and there was nothing to check
// the transport against. Both paths call it now.
float HalfToFloat(uint16_t h)
{
    const int e = (h >> 10) & 0x1f;
    const int m = h & 0x3ff;
    const float v = e == 0 ? m / 1024.0f * 6.103515625e-5f : std::ldexpf(1.0f + m / 1024.0f, e - 15);
    return (h & 0x8000) ? -v : v;
}

void DrainReadbacks(UINT nw, UINT nh)
{
    if (g.pendingGuides)
    {
        g.pendingGuides = false;
        ComPtr<ID3D12Fence> f;
        if (SUCCEEDED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))) &&
            SUCCEEDED(g.queue->Signal(f.Get(), 1)))
        {
            for (int spin = 0; spin < 2000 && f->GetCompletedValue() < 1; ++spin)
                Sleep(1);
            const UINT pitch = (nw * 4 + 255) & ~255u;
            void *a = nullptr, *b = nullptr;
            D3D12_RANGE all { 0, 0 };
            if (SUCCEEDED(g.guideReadDepth->Map(0, &all, &a)) &&
                SUCCEEDED(g.guideReadMotion->Map(0, &all, &b)))
            {
                double lo = 1e30, hi = -1e30, sum = 0.0;
                for (UINT y = 0; y < nh; ++y)
                {
                    auto *row = reinterpret_cast<const float *>(static_cast<const char *>(a) +
                                                                static_cast<size_t>(y) * pitch);
                    for (UINT x = 0; x < nw; ++x)
                    {
                        lo = std::min<double>(lo, row[x]);
                        hi = std::max<double>(hi, row[x]);
                        sum += row[x];
                    }
                }
                const bool depthReal = (hi - lo) > 1e-6;
                g.probeDepthMin.store(static_cast<float>(lo));
                g.probeDepthMax.store(static_cast<float>(hi));
                if (hi > 1e-6)
                    g.depthDebugScale.store(static_cast<float>(1.0 / hi));
                Log("guide probe, depth %ux%u: min %.6f max %.6f mean %.6f%s", nw, nh, lo, hi,
                    sum / (nw * nh),
                    !depthReal ? "  <-- FLAT. A constant, so either this is a menu with no scene "
                                 "or the guide picked the wrong resource."
                               : "  <-- varies, so this is a real depth buffer.");

                UINT64 zero = 0, n = 0;
                double mag = 0.0, biggest = 0.0;
                for (UINT y = 0; y < nh; ++y)
                {
                    auto *row = reinterpret_cast<const uint16_t *>(static_cast<const char *>(b) +
                                                                   static_cast<size_t>(y) * pitch);
                    for (UINT x = 0; x < nw; ++x)
                    {
                        const double dx = HalfToFloat(row[x * 2]), dy = HalfToFloat(row[x * 2 + 1]);
                        if (dx == 0.0 && dy == 0.0)
                            ++zero;
                        mag += std::abs(dx) + std::abs(dy);
                        biggest = std::max(biggest, std::abs(dx) + std::abs(dy));
                        ++n;
                    }
                }
                g.probeMotionMean.store(static_cast<float>(mag / (2.0 * n)));
                g.probeMotionMax.store(static_cast<float>(biggest));
                g.probeStillPct.store(static_cast<int>(100 * zero / n));
                Log("guide probe, motion %ux%u: %llu%% exactly still, mean |d| %.3f px, max %.3f px"
                    "%s", nw, nh, static_cast<unsigned long long>(100 * zero / n), mag / (2.0 * n),
                    biggest,
                    zero == n ? "  <-- ALL ZERO. Either nothing is moving, or the guide picked a "
                                "buffer the engine does not write velocity into."
                              : "");
                // Once a real scene has been seen there is nothing left to answer; until then
                // keep looking, because the first few hundred frames are menus and loading.
                if (depthReal && zero < n)
                {
                    g.guidesLookReal.store(true);
                    g.probeGuides.store(false);
                    Log("guide probe: both guides carry real data. Not probing again.");
                }
                g.guideReadDepth->Unmap(0, nullptr);
                g.guideReadMotion->Unmap(0, nullptr);
            }
        }
        g.guideReadDepth.Reset();
        g.guideReadMotion.Reset();
    }
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
                // A mean residual says the network changed something; it does not say *what*.
                // A flat exposure or saturation shift and a genuine texture edit produce the same
                // mean. They differ in how the correction varies pixel to pixel: a level shift is
                // smooth, so its horizontal gradient is near zero, while structure follows the
                // image and its gradient is a real fraction of the image's own. Measure both and
                // report the ratio -- that is the difference between "it recoloured the frame"
                // and "it worked on the texture", and it is not a judgement call.
                double meanBase = 0.0, gradRes = 0.0, gradIn = 0.0;
                UINT64 nb = 0, ng = 0;
                for (UINT y = 0; y < nh; y += 4)
                {
                    auto *pa = reinterpret_cast<const uint16_t *>(static_cast<const char *>(a) +
                                                                  static_cast<size_t>(y) * rowPitch);
                    auto *pb = reinterpret_cast<const uint16_t *>(static_cast<const char *>(b) +
                                                                  static_cast<size_t>(y) * rowPitch);
                    double prevIn = 0.0, prevRes = 0.0;
                    bool have = false;
                    for (UINT x = 0; x < nw * 4; x += 4)
                    {
                        auto half = [](uint16_t hh) {
                            const int e = (hh >> 10) & 0x1f;
                            const int m = hh & 0x3ff;
                            float v = e == 0 ? m / 1024.0f * 6.103515625e-5f
                                             : std::ldexpf(1.0f + m / 1024.0f, e - 15);
                            return (hh & 0x8000) ? -v : v;
                        };
                        const double in = half(pa[x]);
                        const double res = half(pb[x]) - in;
                        meanBase += std::abs(in);
                        ++nb;
                        if (have)
                        {
                            gradIn += std::abs(in - prevIn);
                            gradRes += std::abs(res - prevRes);
                            ++ng;
                        }
                        prevIn = in;
                        prevRes = res;
                        have = true;
                    }
                }
                const double inputMean = nb ? meanBase / nb : 0.0;
                const double gIn = ng ? gradIn / ng : 0.0;
                const double gRes = ng ? gradRes / ng : 0.0;
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
                    Log("measure, residual detail: local variation of the correction %.6f against "
                        "%.6f in the image itself -- ratio %.3f", gRes, gIn,
                        gIn > 0.0 ? gRes / gIn : 0.0);
                    Log("  Near 0.000 means the correction is smooth across the frame: a colour, "
                        "exposure or saturation shift, with nothing done to texture. Rising toward "
                        "and past 0.100 means the correction follows the image's own detail, which "
                        "is the network working on structure.");
                }
                g.readbackBase->Unmap(0, nullptr);
                g.readbackNr->Unmap(0, nullptr);
            }
        }
        g.readbackBase.Reset();
        g.readbackNr.Reset();
    }
}

bool EnsureResources(UINT w, UINT h, DXGI_FORMAT outFormat, float scale);
bool CreateTexture(UINT w, UINT h, DXGI_FORMAT f, ComPtr<ID3D12Resource> &out, const char *what);
bool RecordNetwork(ID3D12GraphicsCommandList *cmd, ID3D12Resource *colourSrc,
                   DXGI_FORMAT colourFmt, ID3D12Resource *outTarget, bool runNetwork, UINT wanted);

// One frame across the bridge: the game's back buffer goes over, the network runs on our device,
// the finished image comes back. Both crossings are plain CopyResource -- a shared resource
// opened from another device sits in COMMON, which promotes to COPY_SOURCE and COPY_DEST on a
// direct queue, so there is no barrier here to get wrong.
// One frame of one guide, on the game's own device and context. The game's buffer is live and
// keeps being rewritten, so the value has to be taken now and taken by copy.
//
// Motion is the cheap case: an engine's velocity buffer is already R16G16_FLOAT, which opens on
// a second device, so it is a single CopyResource straight into the shared texture. Depth is
// not -- R32G8X24_TYPELESS and R24G8_TYPELESS refuse to open across devices -- so it goes
// through a private snapshot, a compute pass that reads the single float channel, and a shared
// R32_FLOAT. Both of those steps are ported from session.cpp, where the transport is measured.
bool PrepareGuide(Guide &guide, bool isDepth)
{
    if (guide.failed || guide.chosen == nullptr || g.game11 == nullptr || g.workDevice == nullptr)
        return false;
    const UINT w = guide.width, h = guide.height;
    if (w == 0 || h == 0)
        return false;

    if (!isDepth)
    {
        if (!guide.bridge.Ensure(g.game11.Get(), g.workDevice.Get(), w, h, guide.format))
        {
            guide.failed = true;
            Log("guide %s: format %u will not share between the devices; giving up on it.",
                guide.name, static_cast<unsigned>(guide.format));
            return false;
        }
        g.game11ctx->CopyResource(guide.bridge.on11.Get(), guide.chosen.Get());
        guide.ready = true;
        if (!guide.logged)
        {
            guide.logged = true;
            Log("guide %s: %ux%u format %u crossing to the network device, one copy per frame.",
                guide.name, w, h, static_cast<unsigned>(guide.format));
        }
        return true;
    }

    if (g.guideDepthCs == nullptr)
    {
        if (g.guideDepthCsFailed)
            return false;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(p_D3DCompile(kGuideDepthCs, sizeof(kGuideDepthCs) - 1, "guide-depth", nullptr,
                              nullptr, "main", "cs_5_0", 0, 0, &blob, &err)) ||
            FAILED(g.game11->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 nullptr, &g.guideDepthCs)))
        {
            g.guideDepthCsFailed = true;
            Log("guide depth: compute shader failed: %s",
                err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "?");
            return false;
        }
    }

    // The snapshot is never bound as a depth-stencil, so an SRV over it stays valid while the
    // game goes on binding and clearing the original.
    if (guide.snap == nullptr || guide.snapW != w || guide.snapH != h || guide.snapFmt != guide.format)
    {
        guide.srv.Reset();
        guide.srvOf = nullptr;
        guide.snap.Reset();
        D3D11_TEXTURE2D_DESC td {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = guide.format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g.game11->CreateTexture2D(&td, nullptr, &guide.snap)))
        {
            guide.failed = true;
            Log("guide depth: private %ux%u copy of format %u could not be created.", w, h,
                static_cast<unsigned>(guide.format));
            return false;
        }
        guide.snapW = w;
        guide.snapH = h;
        guide.snapFmt = guide.format;
    }
    g.game11ctx->CopyResource(guide.snap.Get(), guide.chosen.Get());

    if (!guide.bridge.Ensure(g.game11.Get(), g.workDevice.Get(), w, h, DXGI_FORMAT_R32_FLOAT, true))
    {
        guide.failed = true;
        return false;
    }
    // Ensure builds a new texture on a resize, and a view left over from the old one writes into
    // an orphan while every read of the new one comes back zero. Rebuild whenever the resource
    // underneath changed, not just when the view is null.
    if (guide.uavOf != guide.bridge.on11.Get())
    {
        guide.uav.Reset();
        guide.uavOf = nullptr;
    }
    if (guide.uav == nullptr)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud {};
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(g.game11->CreateUnorderedAccessView(guide.bridge.on11.Get(), &ud, &guide.uav)))
        {
            guide.failed = true;
            Log("guide depth: UAV over the shared texture failed.");
            return false;
        }
        guide.uavOf = guide.bridge.on11.Get();
    }
    if (guide.srvOf != guide.snap.Get())
    {
        guide.srv.Reset();
        guide.srvOf = nullptr;
    }
    if (guide.srv == nullptr)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
        sd.Format = GuideDepthSrvFormat(guide.format);
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(g.game11->CreateShaderResourceView(guide.snap.Get(), &sd, &guide.srv)))
        {
            guide.failed = true;
            Log("guide depth: SRV over the snapshot failed (fmt %u read as %u).",
                static_cast<unsigned>(guide.format), static_cast<unsigned>(sd.Format));
            return false;
        }
        guide.srvOf = guide.snap.Get();
    }

    // Save exactly the three compute bindings this touches and put them back: the context
    // belongs to the game and it is mid-frame.
    ID3D11ComputeShader *oldCs = nullptr;
    ID3D11ShaderResourceView *oldSrv = nullptr;
    ID3D11UnorderedAccessView *oldUav = nullptr;
    g.game11ctx->CSGetShader(&oldCs, nullptr, nullptr);
    g.game11ctx->CSGetShaderResources(0, 1, &oldSrv);
    g.game11ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);

    UINT keep = static_cast<UINT>(-1);
    ID3D11ShaderResourceView *srv = guide.srv.Get();
    ID3D11UnorderedAccessView *uav = guide.uav.Get();
    g.game11ctx->CSSetShader(g.guideDepthCs.Get(), nullptr, 0);
    g.game11ctx->CSSetShaderResources(0, 1, &srv);
    g.game11ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
    g.game11ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    ID3D11ShaderResourceView *nullSrv = nullptr;
    ID3D11UnorderedAccessView *nullUav = nullptr;
    g.game11ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
    g.game11ctx->CSSetShaderResources(0, 1, &nullSrv);
    g.game11ctx->CSSetShader(oldCs, nullptr, 0);
    g.game11ctx->CSSetShaderResources(0, 1, &oldSrv);
    g.game11ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
    if (oldCs != nullptr)
        oldCs->Release();
    if (oldSrv != nullptr)
        oldSrv->Release();
    if (oldUav != nullptr)
        oldUav->Release();

    guide.ready = true;
    if (!guide.logged)
    {
        guide.logged = true;
        Log("guide depth: %ux%u format %u -> shared R32_FLOAT, one snapshot and one dispatch per "
            "frame. PS2 depth tops out near 0.002; a modern engine's fills the range.",
            w, h, static_cast<unsigned>(guide.format));
    }
    return true;
}

// Submit everything recorded on the game's context and wait, on the CPU, for the GPU to finish
// it. An event query does not report until every command submitted before it has completed.
//
// This replaces the cross-device fences the bridge used to synchronise with. Those queued a
// GPU-side Wait on the game's immediate context, and everything the runtime submitted after it
// -- including ReShade's own overlay pass, which draws into the back buffer -- then sat pending
// behind it. DXGI refuses ResizeBuffers while a pending command references a back buffer, and
// NFS reports that refusal as a DirectX error and quits. Measured: the failure survived not
// touching the back buffer at all from this add-on, which is what ruled the copies out and left
// the queued wait as the only candidate.
//
// The cost is real but already paid: the default mode is inline, where the game waits for the
// network anyway.
// The two private textures that keep the back buffer away from anything shared.
bool EnsureStage(UINT w, UINT h, DXGI_FORMAT fmt)
{
    if (g.stageIn11 != nullptr && g.stageW == w && g.stageH == h && g.stageFmt == fmt)
        return true;
    g.stageIn11.Reset();
    g.stageOut11.Reset();
    g.stageW = g.stageH = 0;
    D3D11_TEXTURE2D_DESC td {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.game11->CreateTexture2D(&td, nullptr, &g.stageIn11)) ||
        FAILED(g.game11->CreateTexture2D(&td, nullptr, &g.stageOut11)))
    {
        Log("bridge: could not create the private staging textures %ux%u fmt %u.", w, h,
            static_cast<unsigned>(fmt));
        g.stageIn11.Reset();
        g.stageOut11.Reset();
        return false;
    }
    g.stageW = w;
    g.stageH = h;
    g.stageFmt = fmt;
    Log("bridge: private staging %ux%u fmt %u, so the back buffer never meets a shared resource.",
        w, h, static_cast<unsigned>(fmt));
    return true;
}

// A removed device fails every call silently from here on: the copies stop landing, the shared
// output keeps whatever it was created with -- black -- and that black is what gets copied into
// the back buffer, every frame, for ever. Worth naming the moment it happens.
bool DeviceLost()
{
    if (g.device == nullptr)
        return false;
    const HRESULT reason = g.device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason))
        return false;
    if (!g.loggedDeviceLost)
    {
        g.loggedDeviceLost = true;
        Log("the D3D12 device was removed (0x%08lX). Nothing this add-on draws will reach the "
            "screen any more, so it is stepping out of the way and leaving the game's own image "
            "alone.", reason);
    }
    g.unavailable = true;
    g.reason = "the D3D12 device was removed; see dlss5-neural.log";
    return true;
}

bool FlushAndWait11()
{
    if (g.game11 == nullptr || g.game11ctx == nullptr)
        return false;
    D3D11_QUERY_DESC qd {};
    qd.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> done;
    if (FAILED(g.game11->CreateQuery(&qd, &done)))
    {
        g.game11ctx->Flush();
        return false;
    }
    g.game11ctx->End(done.Get());
    g.game11ctx->Flush();
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (g.game11ctx->GetData(done.Get(), nullptr, 0, 0) == S_FALSE)
    {
        if (GetTickCount64() > deadline)
        {
            Log("bridge: the game's queue did not drain within 2 s.");
            return false;
        }
        Sleep(0);
    }
    return true;
}

// The other half: wait for our own queue to finish what was just submitted.
void WaitForWorkQueue(UINT64 value)
{
    if (g.fence == nullptr || g.fence->GetCompletedValue() >= value)
        return;
    if (g.completionEvent == nullptr)
        return;
    if (SUCCEEDED(g.fence->SetEventOnCompletion(value, g.completionEvent)))
        WaitForSingleObject(g.completionEvent, 2000);
}

void BridgePresent(device *dev, swapchain *sc)
{
    if (DeviceLost())
        return;
    const resource back = sc->get_current_back_buffer();
    if (back.handle == 0)
    {
        if (!g.loggedNoBackBuffer)
        {
            g.loggedNoBackBuffer = true;
            Log("present with no back buffer to read; skipping those frames.");
        }
        return;
    }
    const resource_desc bd = dev->get_resource_desc(back);
    const auto fmt = static_cast<DXGI_FORMAT>(bd.texture.format);
    const UINT w = bd.texture.width, h = bd.texture.height;

    if (!EnsureResources(w, h, fmt, g.scale.load()))
    {
        g.unavailable = true;
        if (*g.reason == 0)
            g.reason = "could not create the working textures; see dlss5-neural.log";
        return;
    }
    if (!g.bridgeIn.Ensure(g.game11.Get(), g.workDevice.Get(), w, h, fmt) ||
        !g.bridgeOut.Ensure(g.game11.Get(), g.workDevice.Get(), w, h, fmt))
    {
        g.bridgeFailed = true;
        g.unavailable = true;
        g.reason = "the back buffer format is not shareable between the two devices";
        return;
    }
    // The crossed texture is read as an SRV by the copy shader. Rather than lean on state
    // promotion for a shared resource, take a local copy first: a plain copy is the one access
    // that needs no barriers on either side.
    if (g.crossLocal == nullptr || g.crossLocal->GetDesc().Width != w ||
        g.crossLocal->GetDesc().Height != h)
    {
        if (!CreateTexture(w, h, fmt, g.crossLocal, "crossLocal"))
        {
            g.bridgeFailed = true;
            return;
        }
    }
    if (g.fence == nullptr &&
        FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
    {
        g.unavailable = true;
        g.reason = "could not create the completion fence";
        return;
    }

    bool runNetwork = true;
    const bool jobPending =
        g.fence->GetCompletedValue() < g.completion ||
        static_cast<UINT>(InterlockedCompareExchange(
            reinterpret_cast<volatile LONG *>(&At<UINT>(g.runtimes[0], 0x76c14)), 0, 0)) <
            g.lastJobs[0];
    if (jobPending && GetTickCount64() - g.lastJobAt < 500)
    {
        runNetwork = false;
        if (++g.skipped % 120 == 1)
            Log("network skipped: previous evaluation still pending (%llu skipped, %llu done)",
                static_cast<unsigned long long>(g.skipped),
                static_cast<unsigned long long>(g.frame));
    }
    else if (jobPending)
    {
        g.lastJobs[0] = 0;
    }

    // 1. the game's image goes over, and with it whatever guides the game actually renders.
    // This is the step that has no equivalent on PCSX2: there the only thing to send is colour.
    if (!g.noBackBuffer.load())
    {
        if (!EnsureStage(w, h, fmt))
        {
            g.bridgeFailed = true;
            return;
        }
        g.game11ctx->CopyResource(g.stageIn11.Get(),
                                  reinterpret_cast<ID3D11Resource *>(back.handle));
        g.game11ctx->CopyResource(g.bridgeIn.on11.Get(), g.stageIn11.Get());
    }
    SettleGuide(g.guideDepth, g_depthTally);
    SettleGuide(g.guideMotion, g_motionTally);
    g.guideDepth.ready = g.guideMotion.ready = false;
    if (g.useGameGuides.load())
    {
        if (g.useDepth.load())
            PrepareGuide(g.guideDepth, true);
        if (g.useMotion.load())
            PrepareGuide(g.guideMotion, false);
    }
    // The copies above have to have landed before our device reads the shared textures. Waiting
    // here rather than queueing a GPU wait is what keeps the game's context clear of pending
    // cross-device dependencies -- see FlushAndWait11.
    if (!FlushAndWait11())
    {
        g.bridgeFailed = true;
        return;
    }

    // 2. our device does the work
    const UINT i = static_cast<UINT>(g.backValue % State::kRing);
    if (g.ringValue[i] != 0 && g.ringFence->GetCompletedValue() < g.ringValue[i])
    {
        g.ringFence->SetEventOnCompletion(g.ringValue[i], g.ringEvent);
        WaitForSingleObject(g.ringEvent, 2000);
    }
    if (FAILED(g.alloc[i]->Reset()) || FAILED(g.list[i]->Reset(g.alloc[i].Get(), nullptr)))
    {
        Log("bridge: command list reset failed.");
        g.bridgeFailed = true;
        return;
    }
    auto *cmd = g.list[i].Get();
    cmd->CopyResource(g.crossLocal.Get(), g.bridgeIn.on12.Get());
    // Same reason as crossLocal: a shared resource is read here as an SRV, and a plain copy is
    // the one access that needs no barrier on either side.
    auto localise = [&](Guide &guide) {
        if (!guide.ready || guide.bridge.on12 == nullptr)
            return false;
        const auto want = guide.bridge.on12->GetDesc();
        if (guide.local == nullptr || guide.local->GetDesc().Width != want.Width ||
            guide.local->GetDesc().Height != want.Height ||
            guide.local->GetDesc().Format != want.Format)
        {
            guide.local.Reset();
            if (!CreateTexture(static_cast<UINT>(want.Width), want.Height, want.Format,
                               guide.local, guide.name))
                return false;
        }
        Barrier(cmd, guide.local.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(guide.local.Get(), guide.bridge.on12.Get());
        Barrier(cmd, guide.local.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return true;
    };
    g.gameDepthActive = localise(g.guideDepth);
    g.gameMotionActive = localise(g.guideMotion);
    Barrier(cmd, g.crossLocal.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool ok =
        RecordNetwork(cmd, g.crossLocal.Get(), fmt, nullptr, runNetwork, g.loadedPasses);
    Barrier(cmd, g.crossLocal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    if (ok)
    {
        Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(g.bridgeOut.on12.Get(), g.composed.Get());
        Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (const HRESULT hr = cmd->Close(); FAILED(hr))
    {
        // The one path that used to set bridgeFailed without saying anything. It fires on the
        // frame after a swapchain rebuild, and with bridgeFailed latching for the whole run the
        // add-on then returned from every present in silence and left the last image it wrote on
        // screen. Both halves of that were wrong.
        Log("bridge: closing the command list failed (0x%08lX); rebuilding.", hr);
        g.bridgeFailed = true;
        return;
    }
    ID3D12CommandList *lists[] { cmd };
    g.workQueue->ExecuteCommandLists(1, lists);
    for (UINT pass = 0; pass < g.activePasses; ++pass)
        reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(g.runtimes[pass]) + 0x4640)(
            g.workQueue.Get(), 1, lists);
    g.ringValue[i] = ++g.ringSerial;
    g.workQueue->Signal(g.ringFence.Get(), g.ringSerial);
    g.completion = ++g.serial;
    g.workQueue->Signal(g.fence.Get(), g.completion);

    // 3. and the finished image comes back, once our queue has actually produced it
    ++g.backValue;
    WaitForWorkQueue(g.completion);
    if (ok && !g.noBackBuffer.load() && g.stageOut11 != nullptr)
    {
        g.game11ctx->CopyResource(g.stageOut11.Get(), g.bridgeOut.on11.Get());
        g.game11ctx->CopyResource(reinterpret_cast<ID3D11Resource *>(back.handle),
                                  g.stageOut11.Get());
        // And this copy must not still be pending when the game resizes. It is the last thing
        // the add-on does to the swapchain, so draining here leaves nothing outstanding.
        FlushAndWait11();
    }

    // The immediate context keeps a deferred reference to every resource a recorded command
    // touched, and both copies above touch the back buffer. ResizeBuffers refuses -- with
    // DXGI_ERROR_INVALID_CALL, which the game reports as a DirectX error and quits over -- while
    // any reference to a back buffer is outstanding, and the game does its own cleanup *before*
    // the present callback runs, so ours is always the one left. NFS resizes once on the way to
    // fullscreen, which is exactly where it died. Flushing here retires them.
    g.game11ctx->Flush();

    DrainReadbacks(g.netWidth, g.netHeight);

    if (!g.loggedRound)
    {
        g.loggedRound = true;
        Log("bridge: first full round trip done at %ux%u.", w, h);
    }
    if (++g.frame <= 3 || g.frame % 120 == 0)
        Log("frame %llu processed (%llu skipped)", static_cast<unsigned long long>(g.frame),
            static_cast<unsigned long long>(g.skipped));
}

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

// The NR runtime imports d3d12.dll statically, so merely loading it pulls the system copy into
// the process -- and that is the thing this whole dance exists to avoid. Measured: with the
// engine loaded, ReShade installs its d3d12 hooks and the game's next resize dies; without it,
// no hooks and the resizes pass.
//
// An import is just a null-terminated name in the file, so a copy with that name overwritten by
// one of the same length imports whatever we choose. The private D3D12 is already loaded by the
// time this runs, so the loader matches it by base name and never goes to disk for it.
//
// The original file is left alone, and it is the original that the hash is checked against.
std::filesystem::path RuntimeCopyUsingPrivateD3D12(const std::filesystem::path &original,
                                                   UINT index)
{
    if (g_privateD3D12 == nullptr)
        return original;  // the game is on D3D12 already; its d3d12.dll is long since loaded

    const auto patched =
        ExeDirectory() / (L"dlss5-pass" + std::to_wstring(index + 1) + L".dll");
    std::ifstream in(original, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    in.close();
    if (bytes.empty())
        return original;

    // Rewrite ONLY the name in the import table. Overwriting every occurrence of the string in
    // the file was wrong: the same bytes can appear inside code or unrelated data, and patching
    // those corrupts the DLL. The runtime then faulted repeatedly (0xC0000005 at heap addresses)
    // and the frame came back black. Walk the PE properly instead.
    auto rvaToOffset = [&](uint32_t rva) -> size_t {
        if (bytes.size() < 0x40)
            return 0;
        const auto u16 = [&](size_t o) {
            return static_cast<uint16_t>((unsigned char)bytes[o] | ((unsigned char)bytes[o + 1] << 8));
        };
        const auto u32 = [&](size_t o) {
            return static_cast<uint32_t>((unsigned char)bytes[o] | ((unsigned char)bytes[o + 1] << 8) |
                                         ((unsigned char)bytes[o + 2] << 16) |
                                         ((unsigned char)bytes[o + 3] << 24));
        };
        const size_t pe = u32(0x3C);
        if (pe + 24 > bytes.size() || u32(pe) != 0x00004550)
            return 0;
        const uint16_t sections = u16(pe + 6);
        const uint16_t optSize = u16(pe + 20);
        size_t sec = pe + 24 + optSize;
        for (uint16_t i = 0; i < sections; ++i, sec += 40)
        {
            if (sec + 40 > bytes.size())
                return 0;
            const uint32_t va = u32(sec + 12), rawSize = u32(sec + 16), rawPtr = u32(sec + 20);
            if (rva >= va && rva < va + rawSize)
                return rawPtr + (rva - va);
        }
        return 0;
    };
    const auto u16at = [&](size_t o) {
        return static_cast<uint16_t>((unsigned char)bytes[o] | ((unsigned char)bytes[o + 1] << 8));
    };
    const auto u32at = [&](size_t o) {
        return static_cast<uint32_t>((unsigned char)bytes[o] | ((unsigned char)bytes[o + 1] << 8) |
                                     ((unsigned char)bytes[o + 2] << 16) |
                                     ((unsigned char)bytes[o + 3] << 24));
    };
    size_t rewritten = 0;
    if (bytes.size() > 0x40)
    {
        const size_t pe = u32at(0x3C);
        if (pe + 24 < bytes.size() && u32at(pe) == 0x00004550)
        {
            const size_t opt = pe + 24;
            const uint16_t magic = u16at(opt);
            // The import directory is entry 1 of the data directories, which sit after the
            // fixed part of the optional header: 96 bytes for PE32, 112 for PE32+.
            const size_t dirs = opt + (magic == 0x20B ? 112 : 96);
            const uint32_t importRva = u32at(dirs + 1 * 8);
            size_t desc = rvaToOffset(importRva);
            for (; desc != 0 && desc + 20 <= bytes.size(); desc += 20)
            {
                const uint32_t nameRva = u32at(desc + 12);
                if (nameRva == 0)
                    break;  // the null descriptor ends the array
                const size_t nameOff = rvaToOffset(nameRva);
                constexpr size_t n = sizeof(kSystemD3D12Ansi) - 1;
                if (nameOff != 0 && nameOff + n < bytes.size() &&
                    _strnicmp(&bytes[nameOff], kSystemD3D12Ansi, n) == 0 &&
                    bytes[nameOff + n] == 0)
                {
                    std::memcpy(&bytes[nameOff], kPrivateD3D12Ansi, n);
                    ++rewritten;
                }
            }
        }
    }
    if (rewritten == 0)
    {
        Log("no d3d12.dll import found in the runtime; loading it unpatched.");
        return original;
    }

    std::ofstream out(patched, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        Log("could not write %ls; loading the runtime unpatched, which will pull the system "
            "d3d12.dll in.", patched.c_str());
        return original;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    Log("runtime pass %u copied to %ls with its %s import pointed at %s (%zu occurrence(s)).",
        index + 1, patched.filename().c_str(), kSystemD3D12Ansi, kPrivateD3D12Ansi, rewritten);
    return patched;
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

    const auto loadFrom = RuntimeCopyUsingPrivateD3D12(dll, index);
    HMODULE h = LoadLibraryExW(loadFrom.c_str(), nullptr,
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

    // Structure / Skin / Tone are written to three offsets that were found by matching strings in
    // the binary. That the runtime *contains* the names does not prove it reads these words, and
    // an offset that is written but never read looks identical from out here. Dump the float
    // window around them once, right after the engine set its own defaults: a field the engine
    // owns holds a plausible default (0, 1, or something in between), while a field nothing uses
    // stays at whatever it was. Read-only -- this writes nothing.
    if (index == 0)
    {
        char line[512];
        int n = std::snprintf(line, sizeof(line), "engine floats 0x76e28..0x76e44:");
        for (size_t rva = 0x76e28; rva <= 0x76e44 && n > 0 && n < static_cast<int>(sizeof(line));
             rva += 4)
            n += std::snprintf(line + n, sizeof(line) - n, " [%zx]=%.3f", rva,
                               static_cast<double>(At<float>(h, rva)));
        Log("%s", line);
        Log("  written by this add-on: 76e30 tone, 76e34 structure, 76e38 skin. If one of those "
            "reads back as something this add-on never wrote, the engine owns it. To find out "
            "whether they change the picture, run the same scene twice with Skin at 0 and at 3 "
            "and compare the 'measure, residual' line -- if it does not move, the slider is inert.");
    }
    return true;
}

bool CompileShader(const char *source, size_t size, const char *name, ComPtr<ID3D12PipelineState> &out)
{
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(p_D3DCompile(source, size, name, nullptr, nullptr, "main", "cs_5_0",
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
    if (FAILED(p_D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&g.root))))
    {
        Log("root signature failed.");
        return false;
    }
    if (!CompileShader(kCopyShader, sizeof(kCopyShader), "copy", g.copyPipeline) ||
        !CompileShader(kDepthShader, sizeof(kDepthShader), "depth", g.depthPipeline) ||
        !CompileShader(kComposeShader, sizeof(kComposeShader), "compose", g.composePipeline) ||
        !CompileShader(kResidualShader, sizeof(kResidualShader), "residual", g.residualPipeline) ||
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

    // Some games do not present at a fixed size: Xenosaga 2 walks between 1920x1080, 1918x1014,
    // 1918x994 and 1918x1008 every few frames, and PCSX2 flaps between 1920x974 and 1920x971.
    // Letting the raster follow that re-stages the engine every few frames for nothing, and
    // destroys the textures it holds zero-copy handles to while it may still be reading them.
    // Both shaders resample between the back buffer and the raster in either direction, so
    // pinning the raster across a flap costs nothing but a resample.
    //
    // But the pin must not swallow a *deliberate* change. It used to compare sizes only, so it
    // could not tell "the window twitched" from "the user moved Resolution Scale" -- and since a
    // new scale always produces a new size, the slider was inert from frame 2 onward. Setting it
    // to 1.00 left the raster at the 0.50 it booted with, which is exactly the "the network does
    // nothing to textures" report: the network never got a full-resolution frame to work on.
    // Keying the pin on the scale that built the current raster separates the two.
    const bool scaleChanged = scale != g.pinnedScale;
    g.pinnedScale = scale;
    if (g.engineReady && g.netWidth != 0 && !scaleChanged &&
        (nw != g.netWidth || nh != g.netHeight))
    {
        if (!g.loggedPin)
        {
            g.loggedPin = true;
            Log("back buffer changed to %ux%u, which wants a %ux%u raster; keeping the raster at "
                "%ux%u so the engine is not re-staged over a window twitch. Further changes are "
                "handled the same way and not logged. Moving Resolution Scale still re-rasters.",
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

    // That fence covers our own queue. It says nothing about a network job the engine still has
    // in flight -- and the engine holds zero-copy handles into netColour/netMotion/netDepth, so
    // releasing them underneath a running job is a use-after-free on the GPU. Only reachable on
    // a real geometry change, which is rare, so a blocking wait here costs nothing in practice.
    if (g.engineReady && g.runtimes[0] != nullptr)
    {
        const UINT64 deadline = GetTickCount64() + 500;
        while (static_cast<UINT>(InterlockedCompareExchange(
                   reinterpret_cast<volatile LONG *>(&At<UINT>(g.runtimes[0], 0x76c14)), 0, 0)) <
                   g.lastJobs[0] &&
               GetTickCount64() < deadline)
            Sleep(1);
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
         !CreateTexture(nw, nh, DXGI_FORMAT_R16G16B16A16_FLOAT, g.netResidual, "netResidual") ||
         !CreateTexture(nw, nh, DXGI_FORMAT_R16G16_FLOAT, g.netMotion, "netMotion") ||
         !CreateTexture(nw, nh, DXGI_FORMAT_R32_FLOAT, g.netDepth, "netDepth") ||
         !CreateTexture(fw, fh, DXGI_FORMAT_R16_FLOAT, g.lumaA, "lumaA") ||
         !CreateTexture(fw, fh, DXGI_FORMAT_R16_FLOAT, g.lumaB, "lumaB") ||
         !CreateTexture(fw, fh, DXGI_FORMAT_R16G16_FLOAT, g.flowSmall, "flowSmall") ||
         !CreateTexture(fw, fh, DXGI_FORMAT_R16G16_FLOAT, g.flowCoarse, "flowCoarse") ||
         !CreateTexture(nw, nh, DXGI_FORMAT_R16G16B16A16_FLOAT, g.history, "history")))
        return false;
    if (netChanged)
        g.historyValid = false;
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
// A motion-vector target as an engine writes it: two float channels, no more, at something
// close to render resolution. The dozens of small two-channel buffers an engine also produces
// are excluded by the size floor rather than by name, because names are not available here.
bool LooksLikeMotion(const D3D11_TEXTURE2D_DESC &d, UINT screenW, UINT screenH)
{
    if (d.SampleDesc.Count != 1 || d.ArraySize != 1)
        return false;
    if (d.Format != DXGI_FORMAT_R16G16_FLOAT && d.Format != DXGI_FORMAT_R32G32_FLOAT &&
        d.Format != DXGI_FORMAT_R16G16_SNORM)
        return false;
    return screenW == 0 || (d.Width * 2 >= screenW && d.Height * 2 >= screenH);
}

// D3D11 half of the observation. ReShade hands the render targets and the depth-stencil of
// every bind; on D3D12 it hands the add-on only the swapchain (measured: zero depth binds in
// 600 frames), which is why this path exists at all and why PCSX2 had to be moved to D3D11
// before it could show a depth buffer.
void ObserveD3D11(device *dev, const resource_view *rtvs, uint32_t count, resource depthRes)
{
    if (!g.useGameGuides.load())
        return;
    const UINT screenW = g.outWidth, screenH = g.outHeight;
    auto record = [](std::unordered_map<void *, Tallied> &tally, ID3D11Resource *native,
                     const D3D11_TEXTURE2D_DESC &d) {
        Tallied &slot = tally[native];
        if (slot.res == nullptr)
        {
            slot.res = native;
            slot.width = d.Width;
            slot.height = d.Height;
            slot.format = d.Format;
        }
        ++slot.binds;
    };
    if (depthRes.handle != 0)
    {
        auto *native = reinterpret_cast<ID3D11Resource *>(depthRes.handle);
        ComPtr<ID3D11Texture2D> tex;
        D3D11_TEXTURE2D_DESC d {};
        if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&tex))))
        {
            tex->GetDesc(&d);
            if (d.SampleDesc.Count == 1 && d.ArraySize == 1 &&
                GuideDepthSrvFormat(d.Format) != DXGI_FORMAT_UNKNOWN &&
                (screenW == 0 || (d.Width * 2 >= screenW && d.Height * 2 >= screenH)))
                record(g_depthTally, native, d);
        }
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        if (rtvs[i].handle == 0)
            continue;
        const resource res = dev->get_resource_from_view(rtvs[i]);
        if (res.handle == 0)
            continue;
        auto *native = reinterpret_cast<ID3D11Resource *>(res.handle);
        ComPtr<ID3D11Texture2D> tex;
        D3D11_TEXTURE2D_DESC d {};
        if (FAILED(native->QueryInterface(IID_PPV_ARGS(&tex))))
            continue;
        tex->GetDesc(&d);
        if (LooksLikeMotion(d, screenW, screenH))
            record(g_motionTally, native, d);
    }
}

void OnBindDepthStencil(command_list *cmd_list, uint32_t count, const resource_view *rtvs,
                        resource_view dsv)
{
    device *dev = cmd_list != nullptr ? cmd_list->get_device() : nullptr;
    if (dev == nullptr)
        return;
    const resource res = dsv.handle != 0 ? dev->get_resource_from_view(dsv) : resource { 0 };
    if (dev->get_api() == device_api::d3d11)
    {
        std::lock_guard observe(g.lock);
        ObserveD3D11(dev, rtvs, count, res);
    }
    if (dsv.handle == 0)
        return;
    ++g.depthEvents;
    if (res.handle == 0)
        return;

    // Observation is API-agnostic on purpose. Registering the draw events did not make D3D12
    // deliver a single depth-stencil bind in 600 frames, so the question "does this game expose
    // depth to an add-on at all" can only be answered on the other API -- and answering it must
    // not require the D3D11 bridge to already exist. ReShade's own resource_desc reads on both;
    // the native ID3D12Resource cast below does not, so it stays behind the D3D12 check.
    const bool d3d12 = dev->get_api() == device_api::d3d12;
    {
        static UINT logged = 0;
        std::lock_guard observe(g.lock);
        if (logged < 8)
        {
            ++logged;
            const resource_desc rd = dev->get_resource_desc(res);
            Log("depth seen (%s): %ux%u format %u samples %u", d3d12 ? "D3D12" : "D3D11",
                rd.texture.width, rd.texture.height, static_cast<unsigned>(rd.texture.format),
                rd.texture.samples);
        }
    }
    if (!d3d12)
        return;
    auto *native = reinterpret_cast<ID3D12Resource *>(res.handle);
    std::lock_guard guard(g.lock);
    if (native == g.depthBest)
    {
        ++g.depthBinds;
        return;
    }
    const auto d = native->GetDesc();
    static UINT seen = 0;  // separate from the API-agnostic counter above
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

// Subscribing to the draw events is what makes ReShade track render-target state on the game's
// own command lists. Without a subscriber it only reports the swapchain, which is the whole of
// "on D3D12 an add-on sees two render targets and no depth" -- the probe add-on registers these
// and sees eight, depth included, on the same game. The callbacks do nothing; being registered
// is the entire point. Returning false lets the draw proceed.
bool OnDraw(command_list *, uint32_t, uint32_t, uint32_t, uint32_t) { return false; }
bool OnDrawIndexed(command_list *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { return false; }

// Reading the depth buffer at present time returns zeros: PCSX2 has already cleared it by then.
// The only moment its contents exist is immediately before the clear, so take the copy here.
// ReShade calls this before performing the clear, and returning false lets the clear happen.
bool OnClearDepth(command_list *cmd_list, resource_view dsv, const float *, const uint8_t *,
                  uint32_t, const rect *)
{
    if (cmd_list == nullptr || dsv.handle == 0)
        return false;
    device *dev = cmd_list->get_device();
    if (dev == nullptr || dev->get_api() != device_api::d3d12)
        return false;
    const resource res = dev->get_resource_from_view(dsv);
    if (res.handle == 0)
        return false;
    auto *native = reinterpret_cast<ID3D12Resource *>(res.handle);

    std::lock_guard guard(g.lock);
    if (!g.useDepth.load() || native != g.depthBest || g.device == nullptr)
        return false;
    ++g.depthClears;

    const auto d = native->GetDesc();
    if (g.depthSnapshot == nullptr || g.depthSnapshot->GetDesc().Width != d.Width ||
        g.depthSnapshot->GetDesc().Height != d.Height)
    {
        g.depthSnapshot.Reset();
        auto sd = d;
        sd.Format = DepthAliasFormat(d.Format);
        sd.Flags = D3D12_RESOURCE_FLAG_NONE;
        sd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (sd.Format == DXGI_FORMAT_UNKNOWN)
            return false;
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(g.device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                nullptr, IID_PPV_ARGS(&g.depthSnapshot))))
        {
            Log("depth snapshot allocation failed (%llux%u format %d).",
                static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(sd.Format));
            return false;
        }
        Log("depth snapshot: %llux%u, taken before each clear.",
            static_cast<unsigned long long>(d.Width), d.Height);
    }

    auto *cmd = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native());
    if (cmd == nullptr)
        return false;
    // A clear needs the resource in DEPTH_WRITE, so that is the state it is in right now.
    Barrier(cmd, native, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, g.depthSnapshot.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(g.depthSnapshot.Get(), native);
    Barrier(cmd, g.depthSnapshot.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, native, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (!g.loggedSnapshot)
    {
        g.loggedSnapshot = true;
        Log("depth: first snapshot copied before a clear. If the engine log still says depth off, "
            "the copy is happening but the engine is refusing the resource.");
    }
    return false;
}

// ReShade calls this from inside its ResizeBuffers hook, just before handing the call to DXGI.
// That is the one moment where the back buffer has to be completely unreferenced, and where
// dropping the pipeline state costs nothing because the swapchain is about to be rebuilt.
//
// This is what killed NFS on the way to fullscreen. DXGI refuses ResizeBuffers with
// DXGI_ERROR_INVALID_CALL while anything still references a back buffer, and D3D11 counts a
// command list that merely *used* the resource -- the two CopyResource calls the bridge makes
// every frame -- as a reference. The game reports the refusal as a DirectX error and quits.
// Flushing per frame is not enough on its own: the documented sequence is ClearState and then
// Flush, and ClearState is only safe here.
// Everything sized to the swapchain, dropped together. Ensure rebuilds each one on demand.
void ReleaseSwapchainSized()
{
    WaitForWorkQueue(g.completion);
    g.bridgeIn.Destroy();
    g.bridgeOut.Destroy();
    g.crossLocal.Reset();
    g.stageIn11.Reset();
    g.stageOut11.Reset();
    g.stageW = g.stageH = 0;
    g.stageFmt = DXGI_FORMAT_UNKNOWN;
    for (Guide *guide : { &g.guideDepth, &g.guideMotion })
    {
        guide->chosen.Reset();
        guide->srv.Reset();
        guide->srvOf = nullptr;
        guide->uav.Reset();
        guide->uavOf = nullptr;
        guide->snap.Reset();
        guide->snapW = guide->snapH = 0;
        guide->bridge.Destroy();
        guide->local.Reset();
        guide->ready = false;
    }
    g_depthTally.clear();
    g_motionTally.clear();
    g.gameDepthActive = g.gameMotionActive = false;
    g.outWidth = g.outHeight = 0;
}

void OnDestroySwapchain(swapchain *sc, bool resize)
{
    // Before the lock, so a present that is only just starting sees it and backs out.
    g.goneSwapchain.store(sc);
    std::lock_guard guard(g.lock);
    Log("swapchain going away (resize %d) after %llu frames; draining and dropping everything "
        "sized to it.", resize ? 1 : 0, static_cast<unsigned long long>(g.frame));

    if (g.game11ctx != nullptr)
    {
        // Retiring the back-buffer reference needs the work to have *finished*, not just to have
        // been submitted. DXGI refuses ResizeBuffers while a pending command references a back
        // buffer, and the game reports that refusal as a DirectX error and quits over it.
        // ClearState and Flush do not drain it; an event query does, because it does not report
        // until everything submitted before it has completed.
        D3D11_QUERY_DESC qd {};
        qd.Query = D3D11_QUERY_EVENT;
        ComPtr<ID3D11Query> done;
        if (SUCCEEDED(g.game11->CreateQuery(&qd, &done)))
        {
            g.game11ctx->End(done.Get());
            g.game11ctx->Flush();
            // Bounded: hanging here would be a black screen instead of an error, which is not an
            // improvement. The network takes about 16 ms, so this normally returns at once.
            const ULONGLONG deadline = GetTickCount64() + 2000;
            while (g.game11ctx->GetData(done.Get(), nullptr, 0, 0) == S_FALSE)
            {
                if (GetTickCount64() > deadline)
                {
                    Log("resize: gave up waiting for the bridge to drain after 2 s.");
                    break;
                }
                Sleep(0);
            }
        }
        g.game11ctx->ClearState();
        g.game11ctx->Flush();
    }
    ReleaseSwapchainSized();
}

// The new swapchain is up; everything below is sized to it and will be rebuilt on demand.
void OnInitSwapchain(swapchain *sc, bool resize)
{
    std::lock_guard guard(g.lock);
    // Only lift the gate for the swapchain that was actually torn down. A different one being
    // announced says nothing about this one.
    void *gone = g.goneSwapchain.load();
    if (gone == sc || gone == nullptr)
        g.goneSwapchain.store(nullptr);
    g.outWidth = g.outHeight = 0;
    Log("swapchain back (resize %d); the bridge rebuilds at the new size on the next frame.",
        resize ? 1 : 0);
}

bool ToggleRequested()
{
    static bool down = false;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool now = ctrl && (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    const bool pressed = now && !down;
    down = now;
    return pressed;
}

// Everything the network does in one frame, recorded into whatever command list it is handed.
// `colourSrc` is the image to work from and must already be readable as a shader resource; the
// composed result is left in g.composed and copied into `outTarget` when one is given. Pulling
// this out of OnPresent is what lets the same pipeline run on a device that is not the game's:
// the D3D12 path passes the swapchain back buffer, the D3D11 bridge passes shared textures it
// carries in and out. Nothing in here knows or cares which.
bool RecordNetwork(ID3D12GraphicsCommandList *cmd, ID3D12Resource *colourSrc,
                   DXGI_FORMAT colourFmt, ID3D12Resource *outTarget, bool runNetwork, UINT wanted)
{
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

    srv.Format = ColourReadFormat(colourFmt);
    for (UINT i = 0; i < 3; ++i)
        g.device->CreateShaderResourceView(colourSrc, &srv, slot(i));
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g.device->CreateUnorderedAccessView(g.netColour.Get(), nullptr, &uav, slot(3));

    srv.Format = ColourReadFormat(colourFmt);
    g.device->CreateShaderResourceView(colourSrc, &srv, slot(8));
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g.device->CreateShaderResourceView(g.netResidual.Get(), &srv, slot(9));
    const int dbg = g.debugView.load();
    if (dbg == 2)
    {
        g.device->CreateShaderResourceView(g.netColour.Get(), &srv, slot(10));
    }
    else if (dbg == 4 && g.netMotion != nullptr)
    {
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateShaderResourceView(g.netMotion.Get(), &srv, slot(10));
    }
    else if (dbg == 5 && g.netDepth != nullptr)
    {
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        g.device->CreateShaderResourceView(g.netDepth.Get(), &srv, slot(10));
    }
    else
    {
        g.device->CreateShaderResourceView(g.netBase.Get(), &srv, slot(10));
    }
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.Format = ColourReadFormat(colourFmt);
    g.device->CreateUnorderedAccessView(g.composed.Get(), nullptr, &uav, slot(11));

    g.device->CreateShaderResourceView(g.netColour.Get(), &srv, slot(28));
    g.device->CreateShaderResourceView(g.netBase.Get(), &srv, slot(29));
    g.device->CreateShaderResourceView(g.netBase.Get(), &srv, slot(30));
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g.device->CreateUnorderedAccessView(g.netResidual.Get(), nullptr, &uav, slot(31));

    // netColour minus netBase, into netResidual. Emitted wherever the pair is known to match.
    auto captureResidual = [&]() {
        auto t = g.heap->GetGPUDescriptorHandleForHeapStart();
        t.ptr += static_cast<UINT64>(28) * inc;
        cmd->SetComputeRootSignature(g.root.Get());
        ID3D12DescriptorHeap *h = g.heap.Get();
        cmd->SetDescriptorHeaps(1, &h);
        cmd->SetPipelineState(g.residualPipeline.Get());
        cmd->SetComputeRootDescriptorTable(0, t);
        UINT d[8] { nw, nh, nw, nh, 0, 0, 0, 0 };
        cmd->SetComputeRoot32BitConstants(1, 8, d, 0);
        Barrier(cmd, g.netResidual.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->Dispatch((nw + 7) / 8, (nh + 7) / 8, 1);
        Barrier(cmd, g.netResidual.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    };


    const int encMode = g.encoding.load();
    const float white = std::max(1.0f, g.diffuseWhite.load());
    const float kWhite = encMode == 0 ? 1.0f : 203.0f / white;
    // In the depth view `intensity` carries the display scale instead: the PS2 needs about x500
    // and a modern engine x1, and the probe measures which. Nothing else in compose reads it once
    // a debug view has taken over the output.
    const float strength =
        g.debugView.load() == 5 ? g.depthDebugScale.load() * g.intensity.load() : g.intensity.load();

    auto *heap = g.heap.Get();
    if (runNetwork)
    {
    // We only get here with no job outstanding, so netColour holds the last completed output and
    // netBase the input that produced it -- a matched pair, and the last moment it exists before
    // the copy below overwrites netColour. In inline mode this is redundant (the pass after
    // RecordFn re-captures the same frame with no lag) and costs one cheap dispatch.
    if (g.frame > 0)
        captureResidual();

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
    // The game's own velocity buffer, when there is one. This is the input the estimator below
    // exists to replace: an engine knows where every surface was because it has the previous
    // transform, while a block match over two images can only guess and, on PCSX2, measured 99%
    // of blocks still. Resample it into the raster the engine reads and skip the estimator.
    if (g.useMotion.load() && g.gameMotionActive && g.guideMotion.local != nullptr &&
        g.netMotion != nullptr)
    {
        const auto md = g.guideMotion.local->GetDesc();
        srv.Format = md.Format;
        g.device->CreateShaderResourceView(g.guideMotion.local.Get(), &srv, slot(20));
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateUnorderedAccessView(g.netMotion.Get(), nullptr, &uav, slot(23));
        cmd->SetComputeRootSignature(g.root.Get());
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetPipelineState(g.flowUpPipeline.Get());
        auto mtable = heap->GetGPUDescriptorHandleForHeapStart();
        mtable.ptr += 20 * inc;
        cmd->SetComputeRootDescriptorTable(0, mtable);
        const float mscale = g.motionScale.load();
        UINT mdims[8] { nw, nh, static_cast<UINT>(md.Width), md.Height, 1, 0, 0, 0 };
        std::memcpy(&mdims[6], &mscale, sizeof(float));
        cmd->SetComputeRoot32BitConstants(1, 8, mdims, 0);
        Barrier(cmd, g.netMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->Dispatch((nw + 7) / 8, (nh + 7) / 8, 1);
        Barrier(cmd, g.netMotion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        haveMotion = true;
        if (!g.loggedGameMotion)
        {
            g.loggedGameMotion = true;
            Log("motion: the game's own vectors, %llux%u -> %ux%u, scale %.3f. The estimator is "
                "off. Check Debug View \"Motion vectors\" while panning: the field should follow "
                "the camera, and MotionScale flips or rescales it if it does not.",
                static_cast<unsigned long long>(md.Width), md.Height, nw, nh,
                static_cast<double>(mscale));
        }
    }
    else if (g.useMotion.load() && g.flowSmall != nullptr)
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
        // The gate and the accept ratio decide how much of the field is allowed to move, and the
        // right value depends on the game -- 0.02 pins 99% of a dark scene still, while dropping
        // it too far lets noise through. They live in the constant buffer rather than in the
        // shader so they can be dialled in against the flow probe without a rebuild.
        const float gate = g.flowGate.load(), ratio = g.flowRatio.load();
        auto dispatch = [&](ID3D12PipelineState *pso, UINT base, UINT dw, UINT dh, UINT sw, UINT sh,
                            float extra, UINT mode = 0) {
            cmd->SetPipelineState(pso);
            cmd->SetComputeRootDescriptorTable(0, table(base));
            UINT d[8] { dw, dh, sw, sh, mode, 0, 0, 0 };
            std::memcpy(&d[5], &gate, sizeof(gate));
            std::memcpy(&d[6], &extra, sizeof(extra));
            std::memcpy(&d[7], &ratio, sizeof(ratio));
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
    // Three ways depth can arrive, in descending order of how much it is worth. From the game
    // over the bridge: a real, full-range depth buffer an engine wrote and left alone -- this is
    // the one that matches what RenoDX gets handed on NVIDIA. From the pre-clear snapshot: what
    // PCSX2 allows, valid but only for the instant before the emulator wipes it. From the live
    // D3D12 buffer: measured to come back uniformly zero, kept only so the failure is visible.
    const bool fromGame = g.gameDepthActive && g.guideDepth.local != nullptr;
    if (g.useDepth.load() && (fromGame || g.depthSnapshot != nullptr || g.depthBest != nullptr))
    {
        g.depthCandidate = g.depthBest;
        const bool fromSnapshot = !fromGame && g.depthSnapshot != nullptr;
        ID3D12Resource *depthSource = fromGame      ? g.guideDepth.local.Get()
                                      : fromSnapshot ? g.depthSnapshot.Get()
                                                     : g.depthCandidate;
        const auto dd = depthSource->GetDesc();
        bool ok = true;
        if (!fromGame && !fromSnapshot && IsTypedDepth(dd.Format))
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
                Log("depth: %llux%u format %d -> R32_FLOAT at %ux%u, source %s",
                    static_cast<unsigned long long>(dd.Width), dd.Height,
                    static_cast<int>(dd.Format), nw, nh,
                    fromGame        ? "the game's own buffer, over the bridge"
                    : fromSnapshot  ? "pre-clear snapshot"
                                    : "live buffer (expect zeros)");
            }
        }
    }

    // One-shot look at the two guides, once the game has settled.
    if (g.probeGuides.load() && g.frame >= g.nextGuideProbe && g.netDepth != nullptr &&
        g.netMotion != nullptr)
    {
        g.nextGuideProbe = g.frame + 600;
        const UINT pitch = (nw * 4 + 255) & ~255u;
        D3D12_HEAP_PROPERTIES rb {};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Width = static_cast<UINT64>(pitch) * nh;
        const bool okD = SUCCEEDED(g.device->CreateCommittedResource(
            &rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&g.guideReadDepth)));
        const bool okM = SUCCEEDED(g.device->CreateCommittedResource(
            &rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&g.guideReadMotion)));
        if (okD && okM)
        {
            auto grab = [&](ID3D12Resource *src, ID3D12Resource *dst, DXGI_FORMAT f) {
                Barrier(cmd, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION from {}, to {};
                from.pResource = src;
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.pResource = dst;
                to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint.Footprint = { f, nw, nh, 1, pitch };
                cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                Barrier(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            };
            grab(g.netDepth.Get(), g.guideReadDepth.Get(), DXGI_FORMAT_R32_FLOAT);
            grab(g.netMotion.Get(), g.guideReadMotion.Get(), DXGI_FORMAT_R16G16_FLOAT);
            g.pendingGuides = true;
            Log("guide probe armed at frame %llu: depth fed %d, motion fed %d",
                static_cast<unsigned long long>(g.frame), haveDepth ? 1 : 0, haveMotion ? 1 : 0);
        }
    }

    UINT accepted = 0;
    bool nativeFailure = false;
    for (UINT i = 0; i < wanted; ++i)
    {
        HMODULE r = g.runtimes[i];
        // Temporal history. The network is a denoiser: without a previous result to carry
        // forward it starts from nothing every frame, and a motion vector -- which says where a
        // pixel *was* -- has nothing to point at. This is the pair that turns motion from an
        // input the engine merely reports into one it can use.
        //
        // Off by default because these are hardcoded offsets into one specific build: a wrong
        // pointer here does not fail, it hangs the game.
        const bool wantHistory = g.useHistory.load() && g.historyValid && g.history != nullptr;
        At<uint8_t>(r, 0x765f8) = wantHistory ? 1 : 0;
        At<void *>(r, 0x765f0) = wantHistory ? static_cast<void *>(g.history.Get()) : nullptr;
        if (wantHistory && !g.loggedHistory)
        {
            g.loggedHistory = true;
            Log("history: handing the engine last frame's output at %ux%u. Watch the engine log: "
                "it says history off in the engine log when it is ignoring this.", g.netWidth, g.netHeight);
        }
        // Was hardcoded on, which told the engine the motion field was valid on the first
        // frame, before anything had written it. haveMotion is false until there is a real
        // field, from the game or from the estimator.
        At<uint8_t>(r, 0x76e1d) = haveMotion ? 1 : 0;
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

        // The next pass has to read what this one wrote. Every pass records into this same
        // command list and nothing orders them against each other, so the second pass could
        // capture netColour before the first pass's write had landed -- and then write that
        // untouched image back. netColour would end the frame identical to netBase, and the
        // correction would be exactly zero, which is precisely what Passes>1 measured:
        // residual 0.000000 against 0.021 with a single pass. The passes are sequential by
        // intent, so make the pipeline agree.
        if (i + 1 < wanted)
        {
            D3D12_RESOURCE_BARRIER between {};
            between.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            between.UAV.pResource = g.netColour.Get();
            cmd->ResourceBarrier(1, &between);
        }
    }
    g.activePasses = accepted;
    if (accepted != 0)
        g.lastJobAt = GetTickCount64();
    if (nativeFailure)
        return false;

    // Inline mode records the engine's work into this same command list, so by here netColour
    // already holds this frame's output on the GPU timeline. Re-capture, and the correction is
    // this frame's with no lag. In async the engine is on its own timeline and has written
    // nothing yet, so the capture above -- last frame's matched pair -- is the honest one.
    if (g.inlineMode.load() && accepted != 0)
        captureResidual();
    }

    // Retried rather than fired once at a fixed frame: at frame 240 a lot of games are still
    // on a black boot screen, and measuring there reports a black input and a zero residual
    // for a setup that is actually fine. Keep trying every 240 frames until the input has
    // something in it, then stop.
    if (!g.measured && g.activePasses != 0 &&
        (g.measureNow.exchange(false) || (g.frame >= 240 && g.frame % 240 == 0)))
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
    UINT cdims[8] { w, h, nw, nh, static_cast<UINT>(encMode), 0, 0,
                    (g.bicubic.load() ? 1u : 0u) | (static_cast<UINT>(dbg) << 1) };
    std::memcpy(&cdims[5], &kWhite, sizeof(float));
    std::memcpy(&cdims[6], &strength, sizeof(float));
    cmd->SetComputeRoot32BitConstants(1, 8, cdims, 0);
    cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);

    if (outTarget != nullptr)
        cmd->CopyResource(outTarget, g.composed.Get());
    Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Keep this frame's network output as next frame's history. Done here, after compose has
    // read it, so nothing races over the texture.
    if (g.useHistory.load() && g.history != nullptr && g.activePasses != 0)
    {
        Barrier(cmd, g.netColour.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd, g.history.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(g.history.Get(), g.netColour.Get());
        Barrier(cmd, g.history.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, g.netColour.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.historyValid = true;
    }

    return true;
}

// How many passes to run this frame. Inline means the game waits on the GPU for each one and
// they add into a single stall, so the count is held down while it is on.
UINT WantedPasses()
{
    UINT wanted = static_cast<UINT>(
        std::clamp(g.passes.load(), 1, static_cast<int>(State::kMaxPasses)));
    if (g.inlineMode.load() && wanted > State::kInlineMaxPasses)
    {
        if (!g.loggedInlineCap)
        {
            g.loggedInlineCap = true;
            Log("Pass Count held at %u: with Apply On Same Frame the game waits for every pass and "
                "they add into a single stall, which past the driver timeout takes the device down. "
                "Turn Apply On Same Frame off to use more.", State::kInlineMaxPasses);
        }
        wanted = State::kInlineMaxPasses;
    }
    return wanted;
}

// Brings up one runtime per pass, falling back to however many actually loaded. Pass N needs its
// own dlssnr_amd_passN.dll next to the exe -- a copy of pass1 works, same hash -- because each
// pass needs its own copy of the runtime's globals.
bool BringUpEngines(UINT &wanted)
{
    if (!InitPipeline())
        return false;
    for (UINT i = 0; i < wanted; ++i)
    {
        if (InitEngine(i))
            continue;
        if (i == 0)
            return false;
        wanted = i;
        g.passes.store(static_cast<int>(i));
        if (!g.loggedPassLimit)
        {
            g.loggedPassLimit = true;
            Log("pass %u could not be brought up, so Pass Count is held at %u. Each pass needs its "
                "own dlssnr_amd_pass%u.dll next to the exe -- a copy of pass1 works, it has the "
                "same hash. Carrying on with %u pass(es).", i + 1, i, i + 1, i);
        }
        break;
    }
    return true;
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

    if (dev->get_api() == device_api::d3d11)
    {
        // Between a teardown and the new swapchain being announced there is nothing safe to
        // touch, and touching it is exactly what makes the resize fail.
        if (g.noBridge.load() || g.goneSwapchain.load() == sc)
            return;
        // Whatever goes wrong below, the game keeps its picture. If presents keep arriving and the
        // bridge has not completed a frame in a long while, something is stuck, and the honest move
        // is to stand down rather than hold a half-built or stale image on screen. Ten seconds of
        // presents at 60 Hz; a real frame resets it. This is here because two separate bugs -- an
        // event handle shared by two fences, and a teardown flag that latched on -- both showed up
        // as a frozen counter and a black window rather than as anything readable.
        if (g.frame != g.lastSeenFrame)
        {
            g.lastSeenFrame = g.frame;
            g.presentsSinceFrame = 0;
            g.bridgeRetries = 0;  // a frame got through, so the retries were spent well
        }
        else if (++g.presentsSinceFrame > 600)
        {
            g.unavailable = true;
            g.reason = "the bridge stopped completing frames; see dlss5-neural.log";
            Log("600 presents without the bridge finishing a frame. Something is stuck, so the "
                "add-on is standing down and leaving the game's own image alone. The last lines "
                "above this one say how far it got.");
            return;
        }
        BridgeStep1(dev);
        if (g.workDevice == nullptr)
            return;  // BridgeStep1 said why
        if (g.bridgeFailed)
        {
            if (++g.bridgeRetries > State::kMaxBridgeRetries)
            {
                g.unavailable = true;
                g.reason = "the bridge kept failing to rebuild; see dlss5-neural.log";
                Log("bridge: %u rebuilds in a row did not take. Standing down and leaving the "
                    "game's own image alone.", g.bridgeRetries - 1);
                return;
            }
            Log("bridge: rebuilding after a failure (attempt %u of %u).", g.bridgeRetries,
                State::kMaxBridgeRetries);
            ReleaseSwapchainSized();
            g.bridgeFailed = false;
            return;  // next present builds it again
        }
        if (g.stage.load() < 3)
            return;  // diagnostic: stop before the engine, with the device already up
        // This used to be a hardcoded single pass, so Pass Count did nothing at all on the
        // bridge -- which is every D3D11 target, PCSX2 and NFS included. The multi-pass loop
        // existed only on the D3D12 path.
        UINT wanted = WantedPasses();
        if (!BringUpEngines(wanted))
        {
            g.unavailable = true;
            g.reason = "could not bring the engine up on the bridge device";
            Log("off: %s", g.reason);
            return;
        }
        g.loadedPasses = wanted;
        BridgePresent(dev, sc);
        return;
    }
    if (dev->get_api() != device_api::d3d12)
    {
        if (!g.loggedWrongApi)
        {
            g.loggedWrongApi = true;
            Log("a runtime in this process is neither D3D12 nor D3D11; ignored.");
        }
        return;
    }
    if (!LoadGraphicsApi())
    {
        g.unavailable = true;
        g.reason = "the D3D12 or DXGI entry points could not be resolved";
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
    UINT wanted = static_cast<UINT>(
        std::clamp(g.passes.load(), 1, static_cast<int>(State::kMaxPasses)));
    // Inline means the game waits on the GPU for every pass, so the passes add up into one stall.
    // Four of them locked a machine hard: past the driver's timeout Windows removes the D3D12
    // device, and the engine's own ini warns about exactly this. Three is the most that was ever
    // run without trouble, so that is the ceiling while the game is waiting. Async has no stall
    // and keeps the full range.
    if (g.inlineMode.load() && wanted > State::kInlineMaxPasses)
    {
        if (!g.loggedInlineCap)
        {
            g.loggedInlineCap = true;
            Log("Pass Count held at %u: with Apply On Same Frame the game waits for every pass and "
                "they add into a single stall, which past the driver timeout takes the device down. "
                "Turn Apply On Same Frame off to use more.", State::kInlineMaxPasses);
        }
        wanted = State::kInlineMaxPasses;
    }
    bool enginesReady = InitPipeline();
    // Pass N loads dlssnr_amd_passN.dll -- a separate file, so each pass gets its own copy of
    // the runtime's globals. Only pass1 is distributed, so raising Pass Count used to fail
    // InitEngine, set `unavailable`, and kill the add-on permanently for the rest of the run.
    // A missing extra pass is not a reason to stop: fall back to what did load.
    for (UINT i = 0; enginesReady && i < wanted; ++i)
    {
        if (InitEngine(i))
            continue;
        if (i == 0)
        {
            enginesReady = false;
            break;
        }
        wanted = i;
        g.passes.store(static_cast<int>(i));
        if (!g.loggedPassLimit)
        {
            g.loggedPassLimit = true;
            Log("pass %u could not be brought up, so Pass Count is held at %u. Each pass needs "
                "its own dlssnr_amd_pass%u.dll next to the exe -- a copy of pass1 works, it has "
                "the same hash. Carrying on with %u pass(es).",
                i + 1, i, i + 1, i);
        }
        break;
    }
    if (!enginesReady)
    {
        g.unavailable = true;
        g.reason = "could not bring the engine up";
        Log("off: %s", g.reason);
        return;
    }
    g.loadedPasses = wanted;

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
    const resource backRes = back;
    {
        resource_usage a = resource_usage::present, b = resource_usage::shader_resource;
        cmd_list->barrier(1, &backRes, &a, &b);
    }
    if (!RecordNetwork(cmd, backbuffer, bd.Format, nullptr, runNetwork, wanted))
    {
        resource_usage a = resource_usage::shader_resource, b = resource_usage::present;
        cmd_list->barrier(1, &backRes, &a, &b);
        queue->flush_immediate_command_list();
        return;
    }
    {
        resource_usage a = resource_usage::shader_resource, b = resource_usage::copy_dest;
        cmd_list->barrier(1, &backRes, &a, &b);
    }
    cmd->CopyResource(backbuffer, g.composed.Get());
    {
        resource_usage a = resource_usage::copy_dest, b = resource_usage::present;
        cmd_list->barrier(1, &backRes, &a, &b);
    }
    const UINT nw = g.netWidth, nh = g.netHeight;
    ID3D12CommandList *submitted[] { cmd };
    for (UINT i = 0; i < g.activePasses; ++i)
        reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(g.runtimes[i]) + 0x4640)(
            g.queue.Get(), 1, submitted);
    queue->flush_immediate_command_list();
    DrainReadbacks(nw, nh);
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
    ImGui::TextDisabled("Scales the correction after the network, not inside it. RenoDX sends its "
                        "own slider to the runtime as DLSSNR.Intensity; no such parameter has been "
                        "located in this build, so this is a blend and nothing more.");
    
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
    if (ImGui::SliderInt("Pass Count", &passes, 1, static_cast<int>(State::kMaxPasses), "%d", 0))
        g.passes.store(passes);
    if (g.loggedPassLimit)
    {
        char note[256];
        std::snprintf(note, sizeof(note),
                      "Pass Count snapped back: only %u pass(es) could be loaded. Pass 2 and 3 "
                      "need their own dlssnr_amd_pass2.dll / pass3.dll next to the exe -- copy "
                      "dlssnr_amd_pass1.dll and rename it, the hash is the same.",
                      g.loadedPasses);
        Note(kWarn, note);
    }
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

    // What is actually feeding the four Packet slots this frame. The single most useful line in
    // the overlay: the network's ceiling is set here, not by any slider above it. On PCSX2 it
    // reads colour only, and no amount of Structure changes that.
    ImGui::Text("Colour %s   Depth %s   Motion %s   Exposure not fed",
                "from the swapchain",
                g.gameDepthActive    ? "FROM THE GAME"
                : g.depthSnapshot    ? "pre-clear snapshot"
                                     : "none",
                g.gameMotionActive ? "FROM THE GAME" : "estimated");
    // What the guides actually contain, not just whether they were handed over. A buffer can be
    // crossed, bound and fed and still be a cleared constant, which looks identical from the
    // outside and is worth nothing to the network.
    if (g.probeStillPct.load() >= 0)
    {
        const float lo = g.probeDepthMin.load(), hi = g.probeDepthMax.load();
        const bool depthReal = (hi - lo) > 1e-6f;
        const int still = g.probeStillPct.load();
        ImGui::Text("Measured  depth %.4f..%.4f   motion %d%% still, mean %.2f px, max %.2f px",
                    static_cast<double>(lo), static_cast<double>(hi), still,
                    static_cast<double>(g.probeMotionMean.load()),
                    static_cast<double>(g.probeMotionMax.load()));
        if (!depthReal || still >= 100)
            Note(kWarn, "One of the guides is carrying nothing. On a menu that is expected -- a 2D "
                        "screen has no depth and nothing moves. Drive, and this line updates: a "
                        "real scene gives depth a spread and motion a non-zero mean. If it stays "
                        "flat while driving, the guide picked the wrong buffer and the network is "
                        "still working from colour alone.");
    }
    else if (g.gameDepthActive || g.gameMotionActive)
        ImGui::TextDisabled("Measuring the guides... the reading appears within a few hundred "
                            "frames.");

    if (!g.gameDepthActive && !g.gameMotionActive)
        Note(kWarn, "The network is running on colour alone. That is the whole reason a target "
                    "like PCSX2 tops out where it does: a PS2 has no velocity buffer, and its "
                    "depth exists only between a bind and a clear. A modern engine on D3D11 "
                    "hands over both, and this add-on takes them when they are there.");

    bool guides = g.useGameGuides.load();
    if (ImGui::Checkbox("Read Guides From The Game", &guides))
    {
        g.useGameGuides.store(guides);
        Log("menu: game guides %s", guides ? "on" : "off");
    }
    ImGui::TextDisabled("D3D11 only. Watches which depth-stencil and which two-channel float "
                        "target the game binds most, copies them once a frame, and carries them "
                        "over the bridge. Turn off to fall back to estimated motion and no depth.");

    bool depth = g.useDepth.load();
    if (ImGui::Checkbox("Depth", &depth))
        g.useDepth.store(depth);
    bool inverted = g.depthInverted.load();
    if (ImGui::Checkbox("Depth Inverted", &inverted))
        g.depthInverted.store(inverted);
    bool hist = g.useHistory.load();
    if (ImGui::Checkbox("History", &hist))
    {
        g.useHistory.store(hist);
        g.historyValid = false;
        Log("menu: history %s", hist ? "on" : "off");
    }
    ImGui::TextDisabled("Carries the previous result forward. Motion needs this to mean anything.");
    if (hist)
        Note(kWarn, "Experimental. This writes a pointer into the runtime at a fixed offset, and a "
                    "wrong one there hangs the game rather than failing. If the picture smears or "
                    "the game stops responding, this is the first thing to turn off.");

    bool mv = g.useMotion.load();
    if (ImGui::Checkbox("Motion Vectors", &mv))
    {
        g.useMotion.store(mv);
        Log("menu: motion %s", mv ? "on" : "off");
    }
    ImGui::TextDisabled(g.gameMotionActive ? "Read from the game's own velocity buffer."
                                           : "Estimated from the image, not read from the game.");
    if (mv && !g.gameMotionActive)
    {
        Note(kWarn, "The PS2 never computed per-pixel motion, so this is inferred by comparing "
                    "consecutive frames -- the same thing the NVIDIA route does on this target. It "
                    "is wrong where pixels move without the geometry moving: reflections, fire, "
                    "moving shadows, and anything appearing from behind something else.");
    }
    if (mv && g.gameMotionActive)
    {
        {
            v = g.motionScale.load();
            if (ImGui::SliderFloat("Motion Scale", &v, -2.0f, 2.0f, "%.2f", 0))
                g.motionScale.store(v);
            ImGui::TextDisabled("The game's vectors are taken as a UV-space delta and multiplied "
                                "up to pixels. Sign and magnitude are engine convention, not "
                                "something the buffer states, so this is the knob: -1.00 flips "
                                "the direction, 0.50 suits a buffer stored in NDC. Watch Debug "
                                "View \"Motion vectors\" while panning -- the field should follow "
                                "the camera, steadily, not shimmer.");
        }
    }
    if (mv && !g.gameMotionActive)
    {
        v = g.flowGate.load();
        if (ImGui::SliderFloat("Flow Contrast Gate", &v, 0.002f, 0.10f, "%.3f", 0))
            g.flowGate.store(v);
        v = g.flowRatio.load();
        if (ImGui::SliderFloat("Flow Accept Ratio", &v, 0.50f, 1.00f, "%.2f", 0))
            g.flowRatio.store(v);
        {
        ImGui::TextDisabled("Gate: patches flatter than this are forced still. Ratio: a match has "
                            "to beat standing still by this much. The right pair depends on the "
                            "game -- 0.020/0.70 pinned 99%% of a dark God of War 2 scene still. "
                            "The log's flow probe at frame 300 prints what a setting did.");
        }
    }
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
    bool bic = g.bicubic.load();
    if (ImGui::Checkbox("Bicubic Residual Upsample", &bic))
    {
        g.bicubic.store(bic);
        Log("menu: residual upsample %s", bic ? "bicubic" : "bilinear");
    }
    ImGui::TextDisabled("Below 1.00 only the network's correction comes back to full resolution. "
                        "Stretching it bilinearly is a blur, which leaves nothing but the "
                        "low-frequency part -- colour and brightness. Catmull-Rom keeps the rest. "
                        "Turn it off if hard edges ring. No effect at Resolution Scale 1.00.");

    v = g.scale.load();
    if (ImGui::SliderFloat("Resolution Scale", &v, 0.25f, 2.0f, "%.2f", 0))
        g.scale.store(v);
    ImGui::TextDisabled("Cost grows with the square. 0.50 is about 16 ms on an RX 9070 XT, which "
                        "is already most of a 16.7 ms frame at 60 Hz.");
    // Show what the network is actually running at, next to the control that asks for it. When
    // those two disagree the slider is doing nothing, and without this the only symptom is
    // "moving it changes nothing", which reads as the network being weak rather than bypassed.
    if (g.outWidth != 0)
    {
        const UINT wantW = std::max<UINT>(64u, static_cast<UINT>(g.outWidth * v + 0.5f));
        const UINT wantH = std::max<UINT>(64u, static_cast<UINT>(g.outHeight * v + 0.5f));
        ImGui::Text("Network raster: %ux%u", g.netWidth, g.netHeight);
        if (wantW != g.netWidth || wantH != g.netHeight)
        {
            ImGui::SameLine();
            ImGui::TextColored(kWarn, "(asked for %ux%u -- not applied yet)", wantW, wantH);
        }
    }
    // The single most common report is "it only changes colour and exposure". That is not a bug,
    // it is arithmetic: at 0.50 the network is handed a half-resolution image, so the finest
    // thing it can see is two screen pixels wide and it has no way to put detail into a texture
    // it never received. Say so next to the control that causes it.
    if (v < 1.0f)
        Note(kWarn, "AT THIS SETTING TEXTURES CANNOT CHANGE. The network never sees a "
                    "full-resolution pixel, so the correction it can make is limited to colour, "
                    "tone and large-scale shading -- which is why the effect reads as a colour "
                    "filter. Texture detail only appears at 1.00. That is four times the cost of "
                    "0.50, so turn Apply On Same Frame off first: the correction then lags a few "
                    "frames instead of stalling the game, which is what trips the driver reset.");
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
    
    ImGui::SeparatorText("Debug");

    int dbg = g.debugView.load();
    if (ImGui::Combo("Debug View", &dbg,
                     "Off\0Network input\0Network output\0Residual x8\0Motion vectors\0Depth x500\0"))
    {
        g.debugView.store(dbg);
        Log("menu: debug view %d", dbg);
    }
    switch (dbg)
    {
    case 1:
        ImGui::TextDisabled("What the network was handed, stretched back to the screen. If this is "
                            "black, nothing downstream can work.");
        break;
    case 2:
        ImGui::TextDisabled("What the network gave back. Compare against Network input -- if they "
                            "look identical, the network is returning its input.");
        break;
    case 3:
        Note(kWarn, "The correction on its own, x8 against mid grey. THIS IS THE ONE THAT ANSWERS "
                    "'is it doing anything'. Flat grey means the network changed nothing. Visible "
                    "structure that follows edges and texture means it is working -- and how much "
                    "of that structure survives here is exactly what Resolution Scale decides.");
        break;
    case 4:
        ImGui::TextDisabled("Estimated motion. Red is horizontal, green vertical, mid grey is "
                            "still. A flat grey screen while the camera moves means the flow is "
                            "gated off -- lower Flow Contrast Gate.");
        break;
    case 5:
        ImGui::TextDisabled("Depth, x500 because PS2 depth peaks around 0.002 and is solid black "
                            "otherwise. Black on D3D12 is expected: ReShade delivers no depth "
                            "there. Overall Intensity scales this view.");
        break;
    default:
        break;
    }

    if (ImGui::Button("Measure Residual Again"))
    {
        g.measured = false;
        g.measureTries = 0;
        g.measureNow.store(true);
        Log("menu: residual measurement re-armed");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("writes 'measure, residual' to dlss5-neural.log");
    ImGui::TextDisabled("Change one slider, press this, compare the two numbers in the log. That "
                        "is how you tell a control that does something from one that does not.");

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
            LoadSettings();
        }
        // Kept even though it has never fired on D3D12: measured, PCSX2 on D3D12 delivers zero
        // depth-stencil binds in 600 frames, with or without also subscribing to the draw events.
        // The probe tells you why -- on D3D12 ReShade shows an add-on only the two swapchain
        // targets, while the same probe on D3D11 sees eight, depth included. See README,
        // "What the network can actually be fed".
        if (g.events & 1)
            reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
                OnBindDepthStencil);
        if (g.events & 2)
        {
            reshade::register_event<reshade::addon_event::draw>(OnDraw);
            reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
        }
        if (g.events & 4)
            reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(OnClearDepth);
        if (g.events & 8)
            reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
        reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
        if (g.events & 16)
            reshade::register_overlay("DLSS Neural Rendering (AMD)", OnOverlay);
        Log("events subscribed: mask %d", g.events);
        break;
    case DLL_PROCESS_DETACH:
        if (g.events & 16)
            reshade::unregister_overlay("DLSS Neural Rendering (AMD)", OnOverlay);
        reshade::unregister_addon(module);
        if (g_log != nullptr)
            fclose(g_log), g_log = nullptr;
        break;
    }
    return TRUE;
}
