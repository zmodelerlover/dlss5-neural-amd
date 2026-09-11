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

#include "build_config.h"
#if DLSS5_WITH_VULKAN
#include "../vkshared/vk_raw.inc"
#endif

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
#include <functional>
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

// Cosmetic only: nothing here gates behaviour. tier and scale go to the log, note goes to the
// status line, and a game that is not listed runs exactly the same as one that is. It exists so
// the log says which target a report came from, not to enable anything -- do not let it read as
// a whitelist.
constexpr Profile kTargets[] = {
    { L"eurotrucks2.exe", Tier::C, 1.0f,
      "Euro Truck Simulator 2" },

    { L"pcsx2-qt.exe", Tier::C, 1.0f,
      "PCSX2" },

    { L"rpcs3.exe", Tier::C, 1.0f,
      "RPCS3" },

    { L"NFS16.exe", Tier::C, 1.0f,
      "NFS 2015" },

    { nullptr, Tier::C, 1.0f,
      "uncatalogued target (runs the same as a listed one)" },
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
cbuffer C : register(b0) { uint dw; uint dh; uint sw; uint sh; uint mode; float k; float intensity; uint pad; float limit; float fade; float colour; float guard; };
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
float3 ToLinear(float3 c){ return c <= 0.04045 ? c/12.92 : pow(abs(c+0.055)/1.055, 2.4); }
float3 ToSrgb(float3 c){ return c <= 0.0031308 ? c*12.92 : 1.055*pow(abs(c), 1.0/2.4) - 0.055; }
// The largest fraction of the correction that leaves every channel inside [0,1], applied to the
// whole triple at once. Clamping per channel instead is a hue rotation -- whichever channel hits
// the wall first decides the colour of the rest -- and that rotation is what a big correction
// showed up as: blown, wrong-coloured pixels rather than a stronger version of the same picture.
// Exactly nothing when the sum was already representable. hhkbble's cube scaling, from the
// multi-pass work on the OptiScaler DLSS-NR fork.
float3 CubeScale(float3 p, float3 t){
 float3 d = t - p;
 float a = 1.0;
 [unroll] for (int i = 0; i < 3; ++i) {
  if (d[i] > 1e-6) a = min(a, (1.0 - p[i]) / d[i]);
  else if (d[i] < -1e-6) a = min(a, (0.0 - p[i]) / d[i]);
 }
 return p + saturate(a) * d;
}
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
 // Normalised before anything is decided about it, so "how big is this correction" means the
 // same thing whatever the encoding is: 1.0 is white. The correction arrives in the network's
 // own space, which is the frame's linear light times the Diffuse White scale, and a limit
 // expressed in that space would mean something different at 100 nits and at 1000.
 fix /= (mode != 0) ? max(k, 1e-6) : 1.0;
 // The network works in tiles, and a tile where it extrapolated rather than saw returns a
 // correction nothing like the few percent it returns everywhere else -- one measured run came
 // back with a mean of 0.072 and a **maximum of 4.16**, in a picture whose own mean is 0.13.
 // That is where the blown blocks come from, and every extra pass runs on top of the last one's
 // blown block, so it compounds rather than averaging out.
 //
 // Scaled as a whole triple rather than clamped per channel. A per-channel clamp on an outlier
 // is a hue rotation -- the same failure the composition below exists to avoid -- so it turned
 // a blown block into a blown *coloured* block. Here the direction of the correction survives
 // and only its size gives way, and a correction already inside the limit is untouched.
 if (limit > 0.0) {
  float mag = max(abs(fix.r), max(abs(fix.g), abs(fix.b)));
  if (mag > limit) fix *= limit / mag;
 }
 // The tiles at the frame border have no neighbour on one side, and bicubic upsampling rings on
 // top of that. fade rolls the correction off over a band, using the smaller of the two
 // distances, so a corner gets both rolloffs and lands hardest.
 if (fade > 0.0) { float2 e = min(uv, 1.0 - uv) / fade; fix *= saturate(min(e.x, e.y)); }
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
 // The picture the network was shown, rebuilt here instead of read back. The copy shader is a
 // pure function of the pixel, so this reproduces it exactly and at full resolution -- which
 // means the only thing carried up from the reduced raster is the correction itself, and the two
 // pictures being compared below are at the same scale. Everything from here on works in that
 // normalised space, where 1.0 is white, so the encoding drops out of the arithmetic.
 float3 P = (mode != 0) ? ToLinear(saturate(c)) : c;
 float3 E = fix;
 float3 v;
 if (guard <= 0.0) {
  // Additive. What this add-on did until now, kept so the two can be compared in one session:
  // the correction is added per channel and whatever leaves the cube is clipped per channel.
  v = P + E;
 } else {
  // Ratio composition, after the OptiScaler DLSS-NR fork and RenoDX's DLSS 5 addon before it.
  //
  // The point is that nothing here adds a colour difference to a picture. The network's answer is
  // made into a whole picture of its own (M), its luminance is compared against the frame's as a
  // ratio, that ratio is bounded, and the two well-formed pictures are blended. A bounded ratio
  // cannot move hue; a difference can, and with more than one pass it did -- N passes meant N
  // times the difference, which clipped, and a clipped channel is a hue rotation. That is what
  // "3 passes looks deep fried" is.
  float3 M = CubeScale(P, P + E);
  float pl = dot(P, kLuma), ml = dot(M, kLuma);
  // A ratio against a near-black pixel is unbounded, and clamping it is not the same as taming
  // it: a shadow pixel sits around a thousandth, so a tiny edit becomes an enormous ratio, hits
  // the guard, and that pixel boils frame to frame. The same floor on both sides leaves bright
  // pixels alone -- there it vanishes against the luminance -- and lets the ratio fall smoothly
  // to one as the light goes out. No edit at all is the right answer for a pixel with no light.
  const float floorY = 1.0 / 512.0;
  float ratio = (ml + floorY) / (pl + floorY);
  // Two-sided, and one scalar taken from luminance applied to the whole triple. A per-channel
  // bound would be the hue distorter this whole path exists to avoid.
  float bounded = clamp(ratio, 1.0 / guard, guard);
  // The network can hand back an empty picture for an input it could not read. Rescaling that
  // collapses the frame to black -- and it is the one case where the two ends of the blend below
  // do not share a luminance -- so the frame goes through on its own brightness instead.
  float3 lit = ml > 1e-5 ? M * (bounded / max(ratio, 1e-6)) : P * bounded;
  // Both ends of this blend now carry the same luminance and differ only in chroma, so Colour
  // Strength moves colour and nothing else. 0 keeps the game's own hue exactly, with only the
  // light carrying what the network decided; 1 brings the network's colour with it.
  v = lerp(P * bounded, lit, saturate(colour));
  // A last scale rather than a clip, for the same reason as CubeScale: bounded can be larger
  // than ratio, so the scaling above can push a channel past 1 again.
  float peak = max(v.r, max(v.g, v.b));
  if (peak > 1.0) v /= peak;
  v = max(v, 0.0);
 }
 dst[p.xy] = float4((mode != 0) ? ToSrgb(saturate(v)) : saturate(v), 1.0);
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

// v0.2.17 of DLSS-NR-on-AMD, lifted out of its setup by tools/extract_runtime.py and run
// through tools/patch_runtime.py -- this is the hash of the patched file, which is what
// the add-on loads. Every offset below was re-derived against this build. v0.2.14, used
// until now, has a different .data layout, and the add-on refuses it by name rather than
// writing into the wrong globals.
constexpr unsigned char kRuntimeSha256[32] = { 0xdd, 0xd8, 0x2d, 0x31, 0x3a, 0xa7, 0x4c, 0x2e,
                                               0x76, 0x02, 0xd1, 0x7d, 0xfb, 0x7e, 0x7c, 0xd9,
                                               0x0c, 0xca, 0x9b, 0xfc, 0x03, 0x06, 0xf5, 0x81,
                                               0x68, 0x4d, 0x35, 0xd7, 0x5d, 0x1b, 0x35, 0x0b };
constexpr size_t kRuntimeSize = 7248384;

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
    // The runtime is one fixed-size binary, so anything of a different size is the wrong file --
    // and that is knowable from the directory entry. Reading it in to find out pulls the whole of
    // whatever was pointed at into memory first, which is a strange way to reject a wrong DLL.
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(file, sizeError);
    if (sizeError)
        return false;
    // Say which file was rejected and what it is, not just that something was. This check refuses
    // any build of the runtime but the one whose layout these offsets were read out of, and a
    // user who has a *newer* dlssnr_amd_pass1.dll hits it through no fault of their own -- the
    // add-on then printed one cryptic line and shut down. Every offset in InitEngine is a raw
    // write into that DLL's globals, so accepting a different build is not an option; saying
    // plainly which build is wanted is.
    if (size != kRuntimeSize)
    {
        Log("dlssnr_amd_pass1.dll is %llu bytes; this add-on is built against the %zu-byte build "
            "and every address it writes belongs to that one. Refused.",
            static_cast<unsigned long long>(size), kRuntimeSize);
        return false;
    }
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
    if (result >= 0 && std::memcmp(digest, kRuntimeSha256, 32) == 0)
        return true;
    char got[80] {}, want[80] {};
    for (int i = 0; i < 16; ++i)
    {
        std::snprintf(got + i * 2, 3, "%02x", digest[i]);
        std::snprintf(want + i * 2, 3, "%02x", kRuntimeSha256[i]);
    }
    Log("dlssnr_amd_pass1.dll is the right size but a different build: SHA-256 starts %s..., and "
        "this add-on is built against %s.... Refused.", got, want);
    return false;
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
            // The texture is alive by here, and the handle may be too. Ensure calls Destroy() on
            // the way in, so a retry would reclaim them -- but the depth path latches instead of
            // retrying, and a failure path should not depend on someone else trying again.
            Destroy();
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
    // Who is trying to take the slot, and for how many presents running. Identity only -- never
    // dereferenced, and cleared the moment it stops winning -- so no reference is needed.
    ID3D11Resource *challenger = nullptr;
    UINT challengerFrames = 0;
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
    if (guide.chosen.Get() == best->res.Get())
    {
        guide.chosenBinds = best->binds;
        guide.challenger = nullptr;
        guide.challengerFrames = 0;
        tally.clear();
        return;
    }

    // A challenger has to win more than one frame.
    //
    // The tally is cleared every present, so "most-bound" was decided by a single frame, and a
    // single frame is not always a representative one. The GTA San Andreas log has the depth
    // guide walk away from a buffer bound 96440 times to one bound *9 times*, on the frame the
    // game changed resolution and stopped drawing its scene pass: the incumbent simply was not
    // in that frame's tally, so a nine-bind buffer won by being the only thing there. Every frame
    // after that fed the network the wrong depth, and nothing demotes a chosen guide, so it never
    // recovered.
    //
    // Three frames is enough to outlast a resolution change, a loading screen or an alt-tab, and
    // short enough that a real switch costs nothing anyone can see.
    if (guide.challenger != best->res.Get())
    {
        guide.challenger = best->res.Get();
        guide.challengerFrames = 1;
        tally.clear();
        return;
    }
    if (++guide.challengerFrames < 3)
    {
        tally.clear();
        return;
    }
    guide.challenger = nullptr;
    guide.challengerFrames = 0;
    guide.chosenBinds = best->binds;
    guide.chosen = best->res;  // takes a reference; the tally's is about to go
    guide.width = best->width;
    guide.height = best->height;
    guide.format = best->format;
    guide.ready = false;
    guide.logged = false;
    guide.failed = false;
    Log("guide %s: taking %ux%u format %u, bound %u times a frame for three frames running",
        guide.name, best->width, best->height, static_cast<unsigned>(best->format), best->binds);
    tally.clear();
}

struct State
{
    std::mutex lock;

    bool unavailable = false;
    bool loggedWrongApi = false;
    // Starts off unless StartOn says otherwise. The add-on rewrites every presented frame, and
    // the settings that do that are the ones that have taken the machine down, so the shipped
    // default is still off -- but "off every single launch" was a diagnostic's rule, not a
    // user's, and somebody who has already chosen their settings should not have to press a key
    // every time. StartOn is a normal setting now: in the overlay, and written back.
    std::atomic<bool> enabled { false };
    std::atomic<bool> startOn { false };
    // Which key toggles the effect. A virtual-key code plus a modifier mask: 1 Ctrl, 2 Alt,
    // 4 Shift. Ctrl+End is the default because that is what every note, log line and README
    // already says. A bare key with no modifier is allowed and is the user's business -- it will
    // fire during normal play if they bind a letter.
    std::atomic<int> toggleKey { VK_END };
    std::atomic<int> toggleMods { 1 };
    // Switch the effect off when the game stops being the focused window, and leave it off.
    //
    // Not a pause. Coming back to a game that quietly resumed rewriting every frame is the
    // surprise; being handed the game's own image and turning the effect back on deliberately is
    // the point. The hotkey brings it back.
    //
    // Separate from the minimised handling further down, which is an unconditional safety and
    // does resume on its own: nothing we draw is visible while minimised and every wait in this
    // file misbehaves there, so sitting those frames out is never a user-visible decision.
    std::atomic<bool> disableOnAltTab { false };
    std::atomic<float> structure { 1.0f };
    std::atomic<float> tone { 1.0f };
    std::atomic<float> skin { 1.0f };
    std::atomic<int> passes { 1 };
    std::atomic<bool> serialPasses { true };
    // 0 English, 1 Portugues do Brasil. English by default.
    std::atomic<int> language { 0 };
    // The engine's own option struct, mapped by decompiling its ini reader rather than guessed:
    //   8d9d0 LocalTone (0.0)   8d9d4 LocalStructure (1.0)   8d9d8 SkinStructure (-1.0)
    //   8d9dc Scale (0.03125)   8d9e0 UseAutoMask (1)        8d9e4 ToneChannels (0)
    //   8d9bc Enabled  8d9bd Temporal  8d9be UseFsrInputs  8d9bf UseDepth  8d9c0 Tonemap (-1)
    // The last four of these were never written by this add-on, and two were written wrong.
    // Defaults here are the engine's own, so leaving them alone changes nothing.
    std::atomic<int> autoMask { 1 };
    std::atomic<int> toneChannels { 0 };
    std::atomic<float> engineScale { 0.03125f };
    // -1 follows the selected input encoding. Our FP16 transport is also used
    // for SDR, so the runtime's format-based auto detection alone is incorrect.
    std::atomic<int> tonemap { -1 };
    // 0 follow the guides, 1 force off, 2 force on.
    std::atomic<int> temporalMode { 0 };
    std::atomic<float> scale { 0.5f };
    std::atomic<bool> inlineMode { true };
    std::atomic<int> encoding { 0 };
    // 100 nits is the automatic ShortFuse documents for linear BT.709; the old 500 here
    // matched none of the documented conventions (100 / 203 / 250).
    std::atomic<float> diffuseWhite { 100.0f };
    std::atomic<float> intensity { 1.0f };
    // Two limits on the correction itself, applied in compose after the intensity mix. The
    // network is tiled and the tiles at the frame border are extrapolated on one side, so the
    // correction there is invented rather than seen; bicubic upsampling rings on top of it. The
    // report is specks and crawling colour in the corners. limit caps a single pixel of
    // correction, fade rolls the whole correction off over a border band. Both default to off,
    // so nothing changes until they are turned up.
    // Measured, not chosen: a two-pass run on God of War reported a mean correction of 0.072 with
    // a **maximum of 4.16**, against a picture whose own mean is 0.13. A correction four times
    // brighter than white is not something the network saw, it is a tile where it extrapolated --
    // and it is exactly the blown block on screen. Off was the old default and it let all of that
    // through. 0.25 is still more than three times the typical correction, so an ordinary pixel
    // never meets it.
    std::atomic<float> residualLimit { 0.25f };
    std::atomic<float> residualFade { 0.0f };
    // Halve Structure for each later pass. Ours, not the reference's -- upstream leaves structure
    // at full on every pass and only zeroes Local Tone after the first, which this now does by
    // default. Off, because the residual measurement says the chain compounds (one pass is a mean
    // of 0.021, two is 0.072) but nothing here has measured that halving structure is the right
    // answer to that, and matching the reference is worth more than an unmeasured idea.
    //
    // Confirmed in execution, not just in their source. Their per-pass parameter readback prints
    // Intensity 1, LocalStructure 1, SkinStructure -1 on pass 1 and the identical three on pass
    // 2, with only LocalTone going 1 -> 0. Their ini says it in one line: "Omitted controls
    // inherit pass 1, except later-pass local tone defaults to 0." Nothing tapers over there.
    std::atomic<bool> passTaper { false };
    // How the network's answer is put back onto the frame. Above zero this is the highlight
    // guard -- the most compose may move a pixel's luminance, in either direction -- and it
    // doubles as the switch: zero selects the old additive composition, which is kept only so
    // the two can be compared inside one session.
    //
    // Additive is what made Pass Count useless. Two passes is twice the difference and three is
    // three times it, added per channel and then clipped per channel, and a clipped channel is a
    // hue rotation -- so the count did not read as more detail, it read as more saturation and
    // then as garbage. The ratio path bounds luminance instead and leaves hue to a blend between
    // two finished pictures, which is what the OptiScaler DLSS-NR fork does and where the
    // arrangement comes from. 2.0 is that fork's own default.
    std::atomic<float> ratioGuard { 2.0f };
    // The guard is applied once, to the finished composition, while the passes compound the
    // ratio it bounds. Left fixed, the third pass spends most of its contribution against the
    // clamp -- the fork's own tooltip says to raise it by hand with the count. Doing it here
    // instead means one extra pass buys one extra multiple of headroom.
    //
    // Off by default, and that is now a reading rather than a preference. Nine OptiScaler logs
    // from RTX machines carry 254 'composition:' lines across seven games, at one and at two
    // passes, and every one of them says guard 2.0x -- the single 1.5x in the set is a user who
    // dragged the slider down. The reference ini says the same in its own words: MaxRatio,
    // "Default (auto) is 2.0", with no mention of the pass count. So the shipped behaviour over
    // there is a fixed bound at every count, and matching it is worth more than our idea about
    // headroom. The idea stays available as a switch; it is just not what runs unasked.
    std::atomic<bool> guardTracksPasses { false };
    // Whether the network's colour arrives with its light. Both ends of the blend carry the same
    // luminance, so this cannot shift hue on its own: at 0 every pixel keeps the game's exact
    // colour and only its brightness carries the network's verdict.
    std::atomic<float> colourStrength { 1.0f };
    std::atomic<bool> bicubic { true };
    // Show the network's own answer instead of composing it onto the game's frame.
    //
    // This is the picture the Debug View "Network output" always drew; it is here as a mode
    // because on the D3D12 route it was the one that looked right, and a thing people run for
    // hours should not live in a diagnostics dropdown. It is not a better composition -- it is
    // no composition. Highlight Guard, Colour Strength and both residual limits are bypassed
    // entirely, because there is no residual for them to bound.
    std::atomic<bool> networkOutput { false };
    std::atomic<int> debugView { 0 };
    std::atomic<bool> measureNow { false };
    bool diagnostics = false;
    bool capturePair = false;
    std::atomic<float> flowGate { 0.02f };
    std::atomic<float> flowRatio { 0.70f };
    UINT loadedPasses = 0;
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

    // Capped at 3, not the runtime's 10. Nothing above 1 has ever measured better here, and the
    // one reading ever taken of it -- Passes=3 -- came back with a residual of exactly zero.
    // Ten was copied from the ShortFuse route on the strength of its README, which is not a
    // reason to leave a machine-crashing control open that wide. Default stays 1.
    //
    // Three is also the reference fork's own ceiling: "Values are clamped to 1..3", with a
    // separate UnlockPasses flag for an extended range, and its description of 2 and 3 is
    // "deliberately over-processed". Nine RTX logs across seven games back that up -- not one of
    // them runs above two passes. So the cap here is not us being conservative, it is the same
    // number.
    static constexpr UINT kMaxPasses = 3;
    bool loggedDeviceLost = false;
    bool loggedNoBackBuffer = false;
    // A bridge failure is usually transitory -- a rebuild caught mid-flight -- so it costs a
    // rebuild, not the rest of the run.
    static constexpr UINT kMaxBridgeRetries = 10;
    UINT bridgeRetries = 0;
    // Safety net: presents seen since the bridge last finished a frame.
    uint64_t presentsSinceFrame = 0;
    uint64_t lastSeenFrame = 0;
    // Whether the last present was to a window nobody can see. Alt-tab is the only way this
    // becomes true in practice, and it is the state in which every wait in this file misbehaves.
    bool windowHidden = false;
    bool loggedHidden = false;
    // Share the runtime and weights. Inline passes submit and finish both the GPU
    // list and HIP job before changing tuning globals for the following pass.
    // Temporal state remains shared; separate per-pass histories are future work.
    HMODULE runtime = nullptr;
    UINT lastJob = 0;
    UINT activePasses = 0;
    // Log recording and parameter handoff once per process.
    bool loggedPassDetail = false;
    // Per-pass profiles, the same idea as the reference fork's "Per pass" tree: what each run of
    // the network over this frame is told, where it should differ from the values above.
    //
    // A later pass is looking at a picture the first pass already edited, so asking it for the
    // same amount again is asking it to sharpen its own sharpening -- which is the other half of
    // why a high count looks wrong. Turning structure and tone down as the chain goes on is the
    // control for that, and it is the one thing the composition cannot do from outside.
    //
    // All off by default, so an install that never opens the tree behaves exactly as before.
    std::atomic<bool> passOverride[kMaxPasses] {};
    std::atomic<float> passStructure[kMaxPasses] {};
    std::atomic<float> passTone[kMaxPasses] {};
    std::atomic<float> passSkin[kMaxPasses] {};
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
    // Cleared by the overlay's History checkbox and read and set by present. The overlay only
    // takes g.lock for its Status section at the bottom, and the checkbox is above that, so the
    // two threads share no lock here. Atomic, like the switches beside it.
    std::atomic<bool> historyValid { false };
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
    // Holds a reference of its own. This is a resource the GAME owns: a level load, a resize or
    // a device reset frees it, and a bare pointer then points into freed memory while the depth
    // path is still calling GetDesc, two barriers and a CopyResource against it on a later frame.
    ComPtr<ID3D12Resource> depthBest;
    std::atomic<bool> useDepth { true };
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
    // Raised on the bind event, which arrives on whatever thread is recording, and read from
    // present and the overlay. The increment sits outside g.lock on purpose -- observation must
    // not be gated on the Depth switch -- so the counter itself has to carry the guarantee.
    std::atomic<UINT64> depthEvents { 0 };
};

State g;

// Whether this frame's composition carries something the network produced for THIS frame.
//
// Compose runs on every present, including the ones where the network was skipped because its
// previous evaluation was still on the GPU. On those frames the residual it reads belongs to an
// older picture, and pasting it onto a frame that has already moved is not a correction -- it is
// a ghost of where the edges used to be. A slow scene hides it completely, which is why no test
// before GTA V ever saw it: NFS skipped 1 frame in 1205. GTA V skipped 13,921 of 37,584 -- 37%,
// because the network costs 29 ms and the game presents faster than that -- and at speed those
// frames read as a heavy trail behind everything.
//
// The measurement that says dropping it is the right answer rather than a trade: the correction's
// own mean in that scene is 0.003. Leaving it out of a frame is below anyone's threshold;
// putting it in the wrong place is not.
bool CompositionIsFresh(bool ranNetwork)
{
    // A debug view draws a buffer, not a correction, so it has nothing that can go stale -- and
    // gating it here would make the views themselves strobe, which is the opposite of readable.
    //
    // Network Output is ungated for the same reason and a second one: what it shows on a skipped
    // frame is the previous *whole* picture, which reads as a held frame. That is a different
    // artefact from a correction aimed at where the edges used to be, and on D3D12 it is the one
    // that was preferred. Gating it would replace the held frame with the game's own image and
    // make the mode flicker between two different pictures, which is worse than either.
    if (g.debugView.load() != 0 || g.networkOutput.load())
        return true;
    return ranNetwork && g.activePasses != 0;
}

int RuntimeTonemap()
{
    const int requested = g.tonemap.load();
    // Encoding=0 is the UI's sRGB passthrough. The copy shader stores these SDR
    // code values in FP16; that storage format does not turn them into HDR.
    // Other encodings keep the runtime's auto behaviour. Explicit overrides
    // remain available, including 1 to reproduce the old SDR auto result.
    return requested == -1 && g.encoding.load() == 0 ? 0 : requested;
}

// Settings live in an ini next to the exe. Two reasons, both practical: nothing in the overlay
// survived a restart, so every A/B test meant re-dialling half a dozen sliders by hand; and a
// headless run had no way to reach them at all, which made a parameter sweep impossible to
// automate. GetPrivateProfile* is the Win32 ini reader -- no parser to write or get wrong.
// A settings file nobody can read is a settings file nobody edits. The overlay writes this one
// back through WritePrivateProfileString, which cannot carry a comment, so an install that had
// only ever been saved from the overlay ended up as a bare list of keys -- and the two questions
// people actually have, "can it come on by itself" and "can I change the key", had no visible
// answer in it at all.
//
// Written only when the file is absent, so a personal tuning is never overwritten. The values
// here are the same defaults the code carries; this file existing changes nothing about how the
// add-on behaves.
void EnsureNeuralIni()
{
    const auto ini = ExeDirectory() / L"dlss5-neural.ini";
    std::error_code ec;
    if (std::filesystem::exists(ini, ec))
        return;

    std::ofstream f(ini, std::ios::binary);
    if (!f)
    {
        Log("could not write %ls; the built-in defaults are used instead.", ini.c_str());
        return;
    }
    f << "[dlss5]\r\n"
         "; Written because no dlss5-neural.ini was here. Every value below is the default, so\r\n"
         "; this file changes nothing until you edit it. The overlay's Save writes back here.\r\n"
         "\r\n"
         "; --- starting up -------------------------------------------------------------\r\n"
         "; 1 = the effect is already on when the game opens. 0 = the game shows its own\r\n"
         "; image until you press the hotkey. Set this to 1 for a game that is a chore to\r\n"
         "; get back into; you will not have to press anything again.\r\n"
         "StartOn=0\r\n"
         "\r\n"
         "; --- the hotkey --------------------------------------------------------------\r\n"
         "; ToggleKey is a Windows virtual-key code; 0x23 (35) is End. ToggleMods adds up\r\n"
         "; 1 Ctrl + 2 Alt + 4 Shift, so 1 is Ctrl and 0 is no modifier at all. Together\r\n"
         "; these default to Ctrl+End. Easier than looking codes up: open the overlay,\r\n"
         "; click the key button, press the combination you want, then Save.\r\n"
         "ToggleKey=35\r\n"
         "ToggleMods=1\r\n"
         "\r\n"
         "; 1 = alt-tabbing out switches the effect OFF, and it stays off when you come\r\n"
         "; back -- you turn it on again with the hotkey. 0 = alt-tab changes nothing.\r\n"
         "; Either way, a minimised window is always sat out and always resumes on its own.\r\n"
         "DisableOnAltTab=0\r\n"
         "\r\n"
         "; --- the picture -------------------------------------------------------------\r\n"
         "; Scale is the resolution the network runs at, as a fraction of the screen. Lower\r\n"
         "; is faster and the network answers differently, not just softer. Passes is how\r\n"
         "; many times it runs over the frame; 1 is the default and 2 is already heavy.\r\n"
         "Scale=1.0\r\n"
         "Passes=1\r\n"
         "; 0 sRGB (use this for an ordinary SDR game), 1 Linear, 2 scRGB-nl.\r\n"
         "Encoding=0\r\n"
         "; 0 English, 1 Portugues do Brasil.\r\n"
         "Language=0\r\n"
         "\r\n"
         "; --- everything else ---------------------------------------------------------\r\n"
         "; The overlay carries the rest and explains each one where it sits. Change things\r\n"
         "; there, press Save, and they appear in this file.\r\n";
    Log("wrote a commented dlss5-neural.ini next to the exe; every value in it is a default.");
}

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
    g.language.store(std::clamp(static_cast<int>(num(L"Language", 0.0f)), 0, 1));
    g.passes.store(std::clamp(static_cast<int>(num(L"Passes", 1.0f)), 1,
                             static_cast<int>(State::kMaxPasses)));
    g.serialPasses.store(flag(L"SerialPasses", true));
    g.intensity.store(num(L"Intensity", g.intensity.load()));
    g.residualLimit.store(std::max(0.0f, num(L"ResidualLimit", g.residualLimit.load())));
    g.residualFade.store(std::clamp(num(L"EdgeFade", 0.0f), 0.0f, 0.49f));
    g.passTaper.store(flag(L"PassTaper", g.passTaper.load()));
    g.ratioGuard.store(std::clamp(num(L"Guard", g.ratioGuard.load()), 0.0f, 8.0f));
    g.guardTracksPasses.store(flag(L"GuardPerPass", g.guardTracksPasses.load()));
    g.colourStrength.store(std::clamp(num(L"ColourStrength", g.colourStrength.load()), 0.0f, 1.0f));
    g.structure.store(num(L"Structure", g.structure.load()));
    g.skin.store(num(L"Skin", g.skin.load()));
    g.tone.store(num(L"Tone", g.tone.load()));
    // Per-pass profiles. Seeded from the globals so a pass whose override is switched on for the
    // first time starts where the chain already was, rather than at zero.
    for (UINT i = 0; i < State::kMaxPasses; ++i)
    {
        wchar_t key[32];
        swprintf_s(key, L"Pass%uOverride", i + 1);
        g.passOverride[i].store(flag(key, false));
        swprintf_s(key, L"Pass%uStructure", i + 1);
        g.passStructure[i].store(num(key, g.structure.load()));
        swprintf_s(key, L"Pass%uTone", i + 1);
        g.passTone[i].store(num(key, g.tone.load()));
        swprintf_s(key, L"Pass%uSkin", i + 1);
        g.passSkin[i].store(num(key, g.skin.load()));
    }
    g.flowGate.store(num(L"FlowGate", g.flowGate.load()));
    g.flowRatio.store(num(L"FlowRatio", g.flowRatio.load()));
    g.encoding.store(static_cast<int>(num(L"Encoding", 0.0f)));
    g.diffuseWhite.store(num(L"DiffuseWhite", g.diffuseWhite.load()));
    g.debugView.store(std::clamp(static_cast<int>(num(L"DebugView", 0.0f)), 0, 5));
    g.inlineMode.store(flag(L"Inline", g.inlineMode.load()));
    g.bicubic.store(flag(L"Bicubic", g.bicubic.load()));
    g.networkOutput.store(flag(L"NetworkOutput", false));
    g.useMotion.store(flag(L"Motion", g.useMotion.load()));
    g.useHistory.store(flag(L"History", g.useHistory.load()));
    g.useDepth.store(flag(L"Depth", g.useDepth.load()));
    g.useGameGuides.store(flag(L"GameGuides", g.useGameGuides.load()));
    g.noBackBuffer.store(flag(L"NoBackBuffer", g.noBackBuffer.load()));
    g.noBridge.store(flag(L"NoBridge", g.noBridge.load()));
    g.stage.store(static_cast<int>(num(L"Stage", 3.0f)));
    g.events = static_cast<int>(num(L"Events", 31.0f));
    // Diagnostic, in the same family as Stage / Events / NoBridge: read at load, never written
    // back, and off unless the file asks for it. The add-on starts switched off on purpose and
    // the switch is a keypress, which means a route can only be exercised by a person standing
    // at the machine -- and the Vulkan route is the one that has to be booted, watched and shut
    // down without one. Anything left holding this on gets an add-on that starts on, which is
    // why it is not in the overlay and not saved.
    const bool startOn = flag(L"StartOn", false);
    g.startOn.store(startOn);
    g.enabled.store(startOn);
    if (startOn)
        Log("StartOn=1: the effect is on from the first frame. Set it to 0, or clear the box in "
            "the overlay, to go back to starting with the game's own image.");

    // Hotkey. Stored as a virtual-key code and a modifier mask rather than as text, because
    // parsing "Ctrl+End" back into a key is a table that is wrong on the first non-US layout.
    // The overlay writes both by capturing an actual keypress, so nobody has to look up a code.
    g.toggleKey.store(std::clamp(static_cast<int>(num(L"ToggleKey", VK_END)), 0, 0xFE));
    g.toggleMods.store(std::clamp(static_cast<int>(num(L"ToggleMods", 1.0f)), 0, 7));
    g.disableOnAltTab.store(flag(L"DisableOnAltTab", false));
    g.motionScale.store(num(L"MotionScale", g.motionScale.load()));
    g.autoMask.store(static_cast<int>(num(L"AutoMask", 1.0f)));
    g.toneChannels.store(static_cast<int>(num(L"ToneChannels", 0.0f)));
    g.engineScale.store(num(L"EngineScale", 0.03125f));
    g.tonemap.store(static_cast<int>(num(L"Tonemap", -1.0f)));
    g.temporalMode.store(std::clamp(static_cast<int>(num(L"Temporal", 0.0f)), 0, 2));
    g.diagnostics = flag(L"Diagnostics", false);

    Log("settings: scale %.2f passes %d intensity %.2f structure %.2f skin %.2f tone %.2f "
        "inline %d bicubic %d motion %d history %d gate %.3f ratio %.2f debug %d",
        static_cast<double>(g.scale.load()), g.passes.load(),
        static_cast<double>(g.intensity.load()), static_cast<double>(g.structure.load()),
        static_cast<double>(g.skin.load()), static_cast<double>(g.tone.load()),
        g.inlineMode.load() ? 1 : 0, g.bicubic.load() ? 1 : 0, g.useMotion.load() ? 1 : 0,
        g.useHistory.load() ? 1 : 0, static_cast<double>(g.flowGate.load()),
        static_cast<double>(g.flowRatio.load()), g.debugView.load());
    Log("compose: %s, guard %.2f%s, colour strength %.2f, residual limit %.3f, edge fade %.3f, "
        "later passes %s",
        g.ratioGuard.load() > 0.0f ? "ratio" : "additive",
        static_cast<double>(g.ratioGuard.load()),
        g.guardTracksPasses.load() ? " (+1 per extra pass)" : "",
        static_cast<double>(g.colourStrength.load()),
        static_cast<double>(g.residualLimit.load()),
        static_cast<double>(g.residualFade.load()),
        g.passTaper.load() ? "tapered by half each" : "at full strength");
    for (UINT i = 0; i < State::kMaxPasses; ++i)
        if (g.passOverride[i].load())
            Log("  pass %u profile: structure %.2f tone %.2f skin %.2f", i + 1,
                static_cast<double>(g.passStructure[i].load()),
                static_cast<double>(g.passTone[i].load()),
                static_cast<double>(g.passSkin[i].load()));
}

// The other half of LoadSettings, which was missing: everything the overlay changed was lost on
// exit, so an A/B test meant editing the ini by hand between runs. WritePrivateProfileStringW is
// the matching Win32 writer, and the numbers are formatted in the C locale for the same reason
// the reader parses in it -- a pt-BR install would otherwise write "0,50", which the reader then
// stops at the comma.
void SaveSettings()
{
    const auto ini = (ExeDirectory() / L"dlss5-neural.ini").wstring();
    auto num = [&](const wchar_t *key, double v) {
        wchar_t buf[64];
        static _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
        if (c_locale != nullptr)
            _swprintf_s_l(buf, 64, L"%.4g", c_locale, v);
        else
            swprintf_s(buf, 64, L"%.4g", v);
        WritePrivateProfileStringW(L"dlss5", key, buf, ini.c_str());
    };
    auto flag = [&](const wchar_t *key, bool v) {
        WritePrivateProfileStringW(L"dlss5", key, v ? L"1" : L"0", ini.c_str());
    };

    num(L"Scale", g.scale.load());
    num(L"Passes", g.passes.load());
    num(L"Language", g.language.load());
    num(L"AutoMask", g.autoMask.load());
    num(L"ToneChannels", g.toneChannels.load());
    num(L"EngineScale", g.engineScale.load());
    num(L"Tonemap", g.tonemap.load());
    num(L"Temporal", g.temporalMode.load());
    num(L"Intensity", g.intensity.load());
    num(L"ResidualLimit", g.residualLimit.load());
    num(L"EdgeFade", g.residualFade.load());
    num(L"Guard", g.ratioGuard.load());
    num(L"ColourStrength", g.colourStrength.load());
    flag(L"GuardPerPass", g.guardTracksPasses.load());
    flag(L"PassTaper", g.passTaper.load());
    num(L"Structure", g.structure.load());
    num(L"Skin", g.skin.load());
    num(L"Tone", g.tone.load());
    for (UINT i = 0; i < State::kMaxPasses; ++i)
    {
        wchar_t key[32];
        swprintf_s(key, L"Pass%uOverride", i + 1);
        flag(key, g.passOverride[i].load());
        swprintf_s(key, L"Pass%uStructure", i + 1);
        num(key, g.passStructure[i].load());
        swprintf_s(key, L"Pass%uTone", i + 1);
        num(key, g.passTone[i].load());
        swprintf_s(key, L"Pass%uSkin", i + 1);
        num(key, g.passSkin[i].load());
    }
    num(L"FlowGate", g.flowGate.load());
    num(L"FlowRatio", g.flowRatio.load());
    num(L"Encoding", g.encoding.load());
    num(L"DiffuseWhite", g.diffuseWhite.load());
    num(L"DebugView", g.debugView.load());
    num(L"MotionScale", g.motionScale.load());
    flag(L"Inline", g.inlineMode.load());
    flag(L"Bicubic", g.bicubic.load());
    flag(L"NetworkOutput", g.networkOutput.load());
    flag(L"Motion", g.useMotion.load());
    flag(L"History", g.useHistory.load());
    flag(L"Depth", g.useDepth.load());
    flag(L"GameGuides", g.useGameGuides.load());
    // StartOn used to be deliberately unsaved, so a diagnostic could not leave an install that
    // boots with the effect on. It is a normal setting now and the overlay owns it, so it has to
    // survive a save like everything else beside it.
    flag(L"StartOn", g.startOn.load());
    flag(L"DisableOnAltTab", g.disableOnAltTab.load());
    num(L"ToggleKey", g.toggleKey.load());
    num(L"ToggleMods", g.toggleMods.load());
    // Stage / Events / NoBridge / NoBackBuffer are deliberately not written back. They are
    // startup diagnostics, they cannot take effect live, and rewriting them here would quietly
    // re-save a one-off value that was meant for a single run.
    Log("settings saved to dlss5-neural.ini");
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
            D3D12_RANGE all { 0, static_cast<SIZE_T>((nw * 8 + 255) & ~255u) * nh };
            if (f->GetCompletedValue() >= 1 &&
                SUCCEEDED(g.readbackBase->Map(0, &all, &a)) &&
                SUCCEEDED(g.readbackNr->Map(0, &all, &b)))
            {
                const UINT rowPitch = (nw * 8 + 255) & ~255u;
                if (g.capturePair)
                {
                    g.capturePair = false;
                    std::error_code ec;
                    const auto dir = ExeDirectory() / L"dlss5-captures";
                    std::filesystem::create_directories(dir, ec);
                    const std::string prefix = "frame-" + std::to_string(g.frame) + "-" +
                                               std::to_string(GetTickCount64());
                    auto save = [&](const char *kind, const void *data) {
                        std::ofstream file(dir / (prefix + kind + ".raw"), std::ios::binary);
                        for (UINT y = 0; y < nh && file; ++y)
                            file.write(static_cast<const char *>(data) +
                                static_cast<size_t>(y) * rowPitch, static_cast<size_t>(nw) * 8);
                        return file.good();
                    };
                    if (!ec && save("-input", a) && save("-runtime", b))
                    {
                        std::ofstream meta(dir / (prefix + ".txt"));
                        meta << "width=" << nw << "\nheight=" << nh << "\nformat=RGBA16F_LE\n"
                             << "encoding=" << g.encoding.load() << "\npasses=" << g.passes.load()
                             << "\ntonemap=" << RuntimeTonemap() << "\ninline=" << g.inlineMode.load()
                             << "\njob=" << g.lastJob << "\n";
                        Log("capture pair saved: %ls / %s (input and runtime; not the raw neural tensor)",
                            dir.c_str(), prefix.c_str());
                    }
                    else Log("capture pair could not be written to %ls", dir.c_str());
                }
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
            }
            if (a != nullptr) g.readbackBase->Unmap(0, nullptr);
            if (b != nullptr) g.readbackNr->Unmap(0, nullptr);
        }
        g.readbackBase.Reset();
        g.readbackNr.Reset();
    }
}

bool EnsureResources(UINT w, UINT h, DXGI_FORMAT outFormat, float scale);
bool CreateTexture(UINT w, UINT h, DXGI_FORMAT f, ComPtr<ID3D12Resource> &out, const char *what);
using SubmitPassFn = std::function<bool()>;
bool RecordNetwork(ID3D12GraphicsCommandList *&cmd, ID3D12Resource *colourSrc,
                   DXGI_FORMAT colourFmt, ID3D12Resource *outTarget, bool runNetwork, UINT wanted,
                   const SubmitPassFn &submitPass = {});

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

// Wait for a fence to reach a value, and do not give up on it.
//
// This used to be a bare WaitForSingleObject with a 2 s cap whose result was thrown away, in both
// of the places that wait on our own queue. Two seconds is generous while the game is in front
// and nowhere near enough the moment it is not: Windows deprioritises a background process's GPU
// work heavily, so on the way into and out of an alt-tab our queue routinely takes longer than
// that. Every caller then carried on as if the GPU were finished -- resetting a command allocator
// whose list is still executing, releasing shared textures the queue still references, reading a
// result that is still being written. All three are undefined behaviour, and all three happen in
// the same few frames users report as "crashes when I alt-tab".
//
// Waiting longer is slow. Carrying on is an access violation. The only real end to the wait is
// the device dying, which is what the loop checks for.
bool WaitFence(ID3D12Fence *fence, UINT64 value, HANDLE ev, const char *what)
{
    if (fence == nullptr || fence->GetCompletedValue() >= value)
        return true;
    if (ev == nullptr || FAILED(fence->SetEventOnCompletion(value, ev)))
        return false;
    for (UINT waited = 0;; waited += 2)
    {
        if (WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0)
        {
            if (waited != 0)
                Log("bridge: %s took %u s -- the process was most likely in the background.",
                    what, waited);
            return true;
        }
        if (DeviceLost())
        {
            Log("bridge: %s will never complete; the device is gone.", what);
            return false;
        }
        if (waited == 0)
            Log("bridge: still waiting on %s after 2 s. Holding on rather than reusing what the "
                "GPU still owns.", what);
    }
}

// The other half: wait for our own queue to finish what was just submitted.
void WaitForWorkQueue(UINT64 value)
{
    WaitFence(g.fence.Get(), value, g.completionEvent, "the network to finish");
}

// Both sides must finish before the next pass changes the module's tuning.
// The GPU can finish via the timeout fallback while the HIP worker is still
// reading the previous pass's globals, so the queue fence alone is insufficient.
bool FinishSubmittedPass()
{
    if (g.completionEvent == nullptr)
        g.completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g.completionEvent == nullptr || g.fence == nullptr)
        return false;
    g.completion = ++g.serial;
    if (FAILED(g.queue->Signal(g.fence.Get(), g.completion)) ||
        !WaitFence(g.fence.Get(), g.completion, g.completionEvent, "the intermediate pass"))
        return false;
    const UINT64 deadline = GetTickCount64() + 5000;
    while (static_cast<UINT>(InterlockedCompareExchange(
        reinterpret_cast<volatile LONG *>(&At<UINT>(g.runtime, 0x8d6f4)), 0, 0)) < g.lastJob)
    {
        if (GetTickCount64() >= deadline || DeviceLost())
        {
            Log("intermediate pass: job %u did not finish; refusing to overwrite its parameters",
                g.lastJob);
            return false;
        }
        Sleep(1);
    }
    return true;
}

bool SubmitPrivatePass(ID3D12GraphicsCommandList *cmd, ID3D12CommandAllocator *allocator)
{
    if (FAILED(cmd->Close()))
        return false;
    ID3D12CommandList *lists[] {cmd};
    g.queue->ExecuteCommandLists(1, lists);
    reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(g.runtime) + 0x9170)(
        g.queue.Get(), 1, lists);
    if (!FinishSubmittedPass())
        return false;
    return SUCCEEDED(allocator->Reset()) && SUCCEEDED(cmd->Reset(allocator, nullptr));
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
            reinterpret_cast<volatile LONG *>(&At<UINT>(g.runtime, 0x8d6f4)), 0, 0)) <
            g.lastJob;
    if (jobPending && GetTickCount64() - g.lastJobAt < 500)
    {
        runNetwork = false;
        if (++g.skipped % 120 == 1)
            Log("network skipped: previous evaluation still pending (%llu skipped, %llu done). Those "
                    "frames go out as the game drew them; a correction aimed at an older picture "
                    "reads as a trail, not as detail.",
                static_cast<unsigned long long>(g.skipped),
                static_cast<unsigned long long>(g.frame));
    }
    else if (jobPending)
    {
        g.lastJob = 0;
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
    // Resetting an allocator whose command list is still executing is undefined behaviour, so
    // this wait is not optional and cannot be allowed to expire.
    if (g.ringValue[i] != 0 &&
        !WaitFence(g.ringFence.Get(), g.ringValue[i], g.ringEvent, "the ring slot to come free"))
        return;
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
        RecordNetwork(cmd, g.crossLocal.Get(), fmt, nullptr, runNetwork, g.loadedPasses,
            [&]() { return SubmitPrivatePass(cmd, g.alloc[i].Get()); });
    if (!ok && g.failed)
        return;
    Barrier(cmd, g.crossLocal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    if (ok && CompositionIsFresh(runNetwork))
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
    if (g.activePasses != 0)
        reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(g.runtime) + 0x9170)(
            g.workQueue.Get(), 1, lists);
    g.ringValue[i] = ++g.ringSerial;
    g.workQueue->Signal(g.ringFence.Get(), g.ringSerial);
    g.completion = ++g.serial;
    g.workQueue->Signal(g.fence.Get(), g.completion);

    // 3. and the finished image comes back, once our queue has actually produced it
    ++g.backValue;
    WaitForWorkQueue(g.completion);
    // Gated on freshness as well: without that, a skipped frame copies back whatever composition
    // the last evaluated frame left in the shared texture -- a whole stale picture, which is a
    // worse artefact than the stale correction this is here to avoid.
    if (ok && CompositionIsFresh(runNetwork) && !g.noBackBuffer.load() && g.stageOut11 != nullptr)
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
std::filesystem::path RuntimeCopyUsingPrivateD3D12(const std::filesystem::path &original)
{
    if (g_privateD3D12 == nullptr)
        return original;  // the game is on D3D12 already; its d3d12.dll is long since loaded

    const auto patched = ExeDirectory() / L"dlss5-pass1.dll";
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
    Log("runtime copied to %ls with its %s import pointed at %s (%zu occurrence(s)).",
        patched.filename().c_str(), kSystemD3D12Ansi, kPrivateD3D12Ansi, rewritten);
    return patched;
}


// Who jumped to null, and from where.
//
// A call through a null pointer faults with ExceptionAddress == 0, and the engine's own handler
// prints exactly that and nothing else, which names the victim and not the culprit. The return
// address a `call` pushes is still sitting at RSP, so one read of it says which instruction in
// the runtime made the call, as an offset from the module base -- and that is a line in IDA.
//
// First-chance and read-only: this returns CONTINUE_SEARCH always, so it changes no behaviour.
LONG CALLBACK NullJumpProbe(EXCEPTION_POINTERS *e)
{
    static LONG reported = 0;
    if (e == nullptr || e->ExceptionRecord == nullptr || e->ContextRecord == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        e->ExceptionRecord->ExceptionAddress != nullptr)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&reported, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t rsp = static_cast<uintptr_t>(e->ContextRecord->Rsp);
    const uintptr_t base = reinterpret_cast<uintptr_t>(g.runtime);
    for (int i = 0; i < 8; ++i)
    {
        uintptr_t slot = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(rsp + i * 8), &slot,
                              sizeof(slot), nullptr) == 0)
            continue;
        HMODULE owner {};
        char name[MAX_PATH] {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(slot), &owner) &&
            GetModuleFileNameA(owner, name, MAX_PATH) != 0)
        {
            const char *leaf = std::strrchr(name, '\\');
            Log("fault probe: jumped to null; stack+%d returns to %s+0x%llx%s", i * 8,
                leaf ? leaf + 1 : name,
                static_cast<unsigned long long>(slot - reinterpret_cast<uintptr_t>(owner)),
                owner == g.runtime ? "   <<< THE RUNTIME" : "");
        }
    }
    (void) base;
    return EXCEPTION_CONTINUE_SEARCH;
}

bool InitEngine()
{
    if (g.runtime != nullptr)
        return true;
    const auto dir = ExeDirectory();
    const auto dll = dir / L"dlssnr_amd_pass1.dll";
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
        // Said in the panel too. This is the one failure a user can actually fix, and the log
        // line above it says which file and which build, so pointing at the log is worth it.
        g.reason = "dlssnr_amd_pass1.dll is a different build to the one this add-on is built "
                   "against; see dlss5-neural.log";
        Log("off: %s", g.reason);
        return false;
    }
    if (!InitHip())
        return false;

    EnsureEngineIni(dir);

    const auto loadFrom = RuntimeCopyUsingPrivateD3D12(dll);
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

    // Armed once, before the first raw write into the runtime. Everything below this line is a
    // hardcoded offset into someone else's binary, and the failure mode of getting one wrong is a
    // jump into nothing -- see NullJumpProbe.
    static bool probeUp = false;
    if (!probeUp)
    {
        AddVectoredExceptionHandler(1, NullJumpProbe);
        probeUp = true;
    }
    At<ID3D12Device *>(h, 0x8cee8) = g.device.Get();
    g.device->AddRef();
    At<ID3D12CommandQueue *>(h, 0x8cef0) = g.queue.Get();
    g.queue->AddRef();
    At<int>(h, 0x8dad0) = g.hipDevice;
    At<uint8_t>(h, 0x8d6c0) = g.inlineMode.load() ? 1 : 0;
    At<uint8_t>(h, 0x8d82c) = 1;
    At<uint8_t>(h, 0x8d9bc) = 1;
    At<uint8_t>(h, 0x8d9be) = 1;
    At<uint8_t>(h, 0x8d9bf) = 0;
    At<int>(h, 0x8d9c0) = RuntimeTonemap();
    Log("input contract: encoding %d, tonemap requested %d -> runtime %d; FP16 is transport, "
        "not a colour-space declaration. Restart after changing encoding or tonemap.",
        g.encoding.load(), g.tonemap.load(), RuntimeTonemap());

    const std::string file = weights.string();
    if (g.hipSet(g.hipDevice) != 0 ||
        !reinterpret_cast<InitFn>(reinterpret_cast<uintptr_t>(h) + 0x19240)(
            reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(h) + 0x8cef8), &file))
    {
        Log("engine init failed.");
        return false;
    }
    At<uint8_t>(h, 0x8d218) = 1;
    g.runtime = h;
    g.engineReady = true;
    Log("engine ready.");

    // Structure / Skin / Tone are written to three offsets that were found by matching strings in
    // the binary. That the runtime *contains* the names does not prove it reads these words, and
    // an offset that is written but never read looks identical from out here. Dump the float
    // window around them once, right after the engine set its own defaults: a field the engine
    // owns holds a plausible default (0, 1, or something in between), while a field nothing uses
    // stays at whatever it was. Read-only -- this writes nothing.
    {
        char line[512];
        int n = std::snprintf(line, sizeof(line), "engine floats 0x8d9c8..0x8d9e4:");
        for (size_t rva = 0x8d9c8; rva <= 0x8d9e4 && n > 0 && n < static_cast<int>(sizeof(line));
             rva += 4)
            n += std::snprintf(line + n, sizeof(line) - n, " [%zx]=%.3f", rva,
                               static_cast<double>(At<float>(h, rva)));
        Log("%s", line);
        // Dump the byte window the engine has just finished initialising. A field the engine
        // owns holds a plausible default; a field nothing uses holds whatever the loader left.
        // This is read-only and runs after init, so what it prints is the engine's own state.
        n = std::snprintf(line, sizeof(line), "engine bytes 0x8d9b0..0x8d9c7:");
        for (size_t rva = 0x8d9b0; rva <= 0x8d9c7 && n > 0 && n < static_cast<int>(sizeof(line));
             ++rva)
            n += std::snprintf(line + n, sizeof(line) - n, " %02x",
                               static_cast<unsigned>(At<uint8_t>(h, rva)));
        Log("%s", line);
        Log("  8d9b0 is DepthInverted, pinned to the engine's own default of 1 and no longer a "
            "control. 8d9e0 UseAutoMask, 8d9e4 ToneChannels and 8d9dc Scale are the fields the "
            "Engine tab writes; what they read back as here is the engine's own state before "
            "this add-on touches them.");
        Log("  written by this add-on: 8d9d0 tone, 8d9d4 structure, 8d9d8 skin. If one of those "
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
    // Twelve, not eight: compose needs four more than the rest -- the residual limit, the edge
    // fade, the colour strength and the highlight guard. A shader declaring a shorter cbuffer
    // over a longer root constant block is fine, so the other six keep passing eight.
    params[1].Constants = { 0, 0, 12 };
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
    if (g.engineReady && g.runtime != nullptr)
    {
        const UINT64 deadline = GetTickCount64() + 500;
        while (static_cast<UINT>(InterlockedCompareExchange(
                   reinterpret_cast<volatile LONG *>(&At<UINT>(g.runtime, 0x8d6f4)), 0, 0)) <
                   g.lastJob &&
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
        g.historyValid.store(false);
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
    if (native == g.depthBest.Get())
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
        g.depthBest = native;  // ComPtr: takes a reference
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
    if (!g.useDepth.load() || native != g.depthBest.Get() || g.device == nullptr)
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
        // A raw identity pointer must not outlive the resources it was compared against: the
        // next allocation the game makes can land on the same address.
        guide->challenger = nullptr;
        guide->challengerFrames = 0;
    }
    g_depthTally.clear();
    g_motionTally.clear();
    // depthBest now holds a reference of its own, so it has to be let go here as well as
    // remembered. Holding a reference on a resource the game owns across its own teardown is
    // the same shape as the bug that broke this add-on's swapchain resize once already: the
    // game cannot finish releasing what we are still pointing at. The next bind re-finds it.
    g.depthBest.Reset();
    g.depthCandidate = nullptr;
    g.depthWidth = g.depthHeight = 0;
    g.depthFormat = DXGI_FORMAT_UNKNOWN;
    g.depthBinds = g.depthBestBinds = 0;
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
    const int key = g.toggleKey.load(), mods = g.toggleMods.load();
    const auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool modsOk = (!(mods & 1) || held(VK_CONTROL)) && (!(mods & 2) || held(VK_MENU)) &&
                        (!(mods & 4) || held(VK_SHIFT));
    const bool now = key != 0 && modsOk && held(key);
    const bool pressed = now && !down;
    down = now;
    return pressed;
}

// The bound key, spelled the way the user's keyboard layout spells it.
//
// Win32 already knows every key's localised name, so there is no table here to fall out of date
// or to be wrong on a non-US layout. The one trap is the extended-key bit: without it
// GetKeyNameText answers with the numpad twin, and End prints as "Num 1".
std::string HotkeyName()
{
    const int key = g.toggleKey.load(), mods = g.toggleMods.load();
    std::string out;
    if (mods & 1) out += "Ctrl+";
    if (mods & 2) out += "Alt+";
    if (mods & 4) out += "Shift+";
    if (key == 0)
        return out.empty() ? "unbound" : out + "?";

    UINT sc = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
    switch (key)
    {
    case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
        sc |= 0x100;
        break;
    default:
        break;
    }
    wchar_t wide[64] {};
    if (sc != 0 && GetKeyNameTextW(static_cast<LONG>(sc) << 16, wide, 64) > 0)
    {
        char name[128] {};
        if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, sizeof(name), nullptr, nullptr) > 0)
            return out + name;
    }
    char fallback[16] {};
    std::snprintf(fallback, sizeof(fallback), "VK 0x%02X", key);
    return out + fallback;
}

// What compose is allowed to move a pixel by, this frame.
//
// Zero stays zero -- that is the additive path, not a guard of nothing. Above it, the guard is a
// bound on the *finished* composition while the passes compound the ratio inside it, so a fixed
// value means the second and third pass spend their contribution against the clamp and cost
// frametime for nothing. One extra pass, one extra multiple of headroom.
float EffectiveGuard()
{
    const float base = g.ratioGuard.load();
    if (base <= 0.0f || !g.guardTracksPasses.load())
        return base;
    const UINT running = std::max(1u, g.activePasses);
    return base + static_cast<float>(running - 1);
}

// What one pass of the network is told. The globals unless that pass carries its own profile.
struct PassTune
{
    float structure, tone, skin;
};

PassTune TuningFor(UINT pass)
{
    if (pass < State::kMaxPasses && g.passOverride[pass].load())
        return { g.passStructure[pass].load(), g.passTone[pass].load(), g.passSkin[pass].load() };
    // Local Tone on the first pass only, and full Structure on every pass. That is not a guess and
    // it is not symmetry for its own sake -- it is what the reference fork does, in one line of
    // PassProfiles.h:
    //
    //     pass == 0 ? cfg.DlssNrLocalTone.value_or_default() : 0.0f
    //
    // This add-on had it right and then lost it. The old code was `i == 0 ? tone : 0.0f`, which
    // last session read as an asymmetry nobody chose and was "fixed" into giving every pass the
    // same value. It was chosen, upstream, deliberately: local tone is a tone decision about the
    // frame, and a second pass re-deciding the tone of a frame whose tone the first pass already
    // moved is how a chain runs away from the picture it started with.
    PassTune t { g.structure.load(), pass == 0 ? g.tone.load() : 0.0f, g.skin.load() };
    // Ours, on top, and off by default: the reference does not taper structure and nothing here
    // has measured that it should. Skin is never tapered -- -1 is the engine's own default and it
    // means "follow local structure", so it is a mode and not a strength.
    if (pass > 0 && g.passTaper.load())
        t.structure *= std::pow(0.5f, static_cast<float>(pass));
    return t;
}

// Everything the network does in one frame, recorded into whatever command list it is handed.
// `colourSrc` is the image to work from and must already be readable as a shader resource; the
// composed result is left in g.composed and copied into `outTarget` when one is given. Pulling
// this out of OnPresent is what lets the same pipeline run on a device that is not the game's:
// the D3D12 path passes the swapchain back buffer, the D3D11 bridge passes shared textures it
// carries in and out. Nothing in here knows or cares which.
bool RecordNetwork(ID3D12GraphicsCommandList *&cmd, ID3D12Resource *colourSrc,
                   DXGI_FORMAT colourFmt, ID3D12Resource *outTarget, bool runNetwork, UINT wanted,
                   const SubmitPassFn &submitPass)
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
    // Network Output reaches the shader as view 2, which is the same path the debug view has
    // always drawn: bind netColour to the debug slot and let compose sample it instead of
    // building a composition. Reusing it rather than adding a second one means the mode cannot
    // drift away from the picture that was actually tested. An explicit Debug View still wins,
    // so the diagnostics stay usable with the mode on.
    const int dbg = g.debugView.load() != 0 ? g.debugView.load() : (g.networkOutput.load() ? 2 : 0);
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
    // Diffuse White says how many nits a value of 1.0 stands for, and the network's own unit is
    // the reference for the encoding: 100 nits for linear BT.709, 203 for scRGB-nl. So a pixel at
    // 1.0 has to be handed white/reference.
    //
    // This was `203 / white`, which is the reciprocal, with 203 used for both modes. It ran the
    // slider backwards against its own label -- raising Diffuse White dimmed what the network saw
    // -- and on the NFS settings (Linear, 100 nits, the documented automatic, so the scale should
    // be exactly 1.0) it multiplied the whole linear image by 2.03 instead. Both the direction and
    // the constant were wrong.
    const float reference = encMode == 2 ? 203.0f : 100.0f;
    const float kWhite = encMode == 0 ? 1.0f : white / reference;
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
        // MotionScale rides on the same multiplier here as it does on the game's own vectors.
        // Without this the slider only ever touched games that hand over a velocity buffer, and
        // the estimated field -- the one that is a guess and most needs turning down -- had no
        // control at all.
        dispatch(g.flowUpPipeline.Get(), 20, nw, nh, fw, fh,
                 static_cast<float>(nw) / static_cast<float>(fw) * g.motionScale.load());
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
        g.depthCandidate = g.depthBest.Get();
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
        HMODULE r = g.runtime;
        // Temporal history. The network is a denoiser: without a previous result to carry
        // forward it starts from nothing every frame, and a motion vector -- which says where a
        // pixel *was* -- has nothing to point at. This is the pair that turns motion from an
        // input the engine merely reports into one it can use.
        //
        // Off by default because these are hardcoded offsets into one specific build: a wrong
        // pointer here does not fail, it hangs the game.
        const bool wantHistory = g.useHistory.load() && g.historyValid.load() && g.history != nullptr;
        At<uint8_t>(r, 0x8d018) = wantHistory ? 1 : 0;
        At<void *>(r, 0x8d010) = wantHistory ? static_cast<void *>(g.history.Get()) : nullptr;
        if (wantHistory && !g.loggedHistory)
        {
            g.loggedHistory = true;
            Log("history: handing the engine last frame's output at %ux%u. Watch the engine log: "
                "it says history off in the engine log when it is ignoring this.", g.netWidth, g.netHeight);
        }
        // 8d9bd is Temporal, not "motion is valid" -- the engine's own ini reader reads the
        // key "Temporal" into this byte. The old name was a guess and it made the session-2
        // measurement look unexplained: Temporal=1 was the only run where the engine reported
        // non-zero motion, which is not a coincidence, it is what temporal accumulation is for.
        // Auto still follows haveMotion, which is the sane default; the other two are explicit.
        const int tm = g.temporalMode.load();
        At<uint8_t>(r, 0x8d9bd) =
            static_cast<uint8_t>(tm == 1 ? 0 : tm == 2 ? 1 : (haveMotion ? 1 : 0));
        // Never written before. UseAutoMask is the engine's own character masking -- the same
        // field RenoDX exposes as "Character Mask" -- and it defaults to 1, so the add-on was
        // silently relying on the default. ToneChannels and Scale were not known to exist.
        At<int>(r, 0x8d9e0) = g.autoMask.load();
        // Bits 2 and 4 of ToneChannels stopped being tone channels in v0.2.17. The apply shader
        // now reads them as the timeout policy, in one line:
        //
        //     if (tone & 4) { if (flags.Load(12) != 0) d = (tone & 2) ? prev[id.xy].rgb
        //                                                            : float3(0, 0, 0); }
        //
        // Bit 4 turns the guard on at all; bit 2 chooses last frame's residual over nothing. With
        // both clear -- the engine's own default -- a timed-out frame keeps whatever half-written
        // bytes are in the residual buffer, which is the one outcome nobody wants.
        //
        // Against v0.2.14 this add-on patched that line in the binary to `return;`: on a timeout
        // keep the current frame and never paste a stale correction. Upstream has since made the
        // same choice available as a flag, so the patch is gone and these two bits carry it. Bit 1
        // is the only one left that means what the name says.
        At<int>(r, 0x8d9e4) = (g.toneChannels.load() & ~2) | 4;
        At<float>(r, 0x8d9dc) = g.engineScale.load();
        At<int>(r, 0x8d9c0) = RuntimeTonemap();
        At<uint8_t>(r, 0x8d6c0) = g.inlineMode.load() ? 1 : 0;
        At<uint8_t>(r, 0x8d9bf) = haveDepth ? 1 : 0;
        // 8d9b0 DepthInverted, pinned to the engine's own default. Both runtimes boot this at
        // 1 -- the NVIDIA DLL writes options+260 = 1 when the parameter is absent, and the AMD
        // port's static initialiser sets dword_180076E10 = 1 -- and no run here ever produced a
        // reading that told the two settings apart. It was a switch that could only be wrong, so
        // it is written, not exposed.
        At<UINT>(r, 0x8d9b0) = 1u;
        At<uint8_t>(r, 0x8d9b4) = 1;
        // All three come from one place now, and that place is per-pass. Local Tone is written on
        // the first pass only -- which is what the original `i == 0 ? tone : 0.0f` here did, and
        // last session removed it as an asymmetry nobody had chosen. Somebody had: the reference
        // fork's PassProfiles.h makes exactly that choice, in one line, deliberately.
        const PassTune tune = TuningFor(i);
        At<float>(r, 0x8d9d0) = tune.tone;
        At<float>(r, 0x8d9d4) = tune.structure;
        At<float>(r, 0x8d9d8) = tune.skin;

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
        // Read before the call so the report below can say whether this pass moved anything.
        // The marker at 8d908 cannot answer that on its own: pass 1 sets it to cmd, so on pass 2
        // the equality test below is comparing cmd against cmd whatever the engine did, and a
        // silently refused pass 2 would be counted as accepted. The job id is the field that
        // changes per evaluation, so an id that does not move is a pass that did not run.
        const UINT jobBefore = At<UINT>(r, 0x8d914);
        reinterpret_cast<RecordFn>(reinterpret_cast<uintptr_t>(r) + 0xf600)(&packet);
        const UINT jobAfter = At<UINT>(r, 0x8d914);

        if (At<uint8_t>(r, 0x8d21a) != 0)
        {
            nativeFailure = true;
            g.failed = true;
            Log("pass %u reported a native failure. Stopping.", i + 1);
            break;
        }
        if (At<ID3D12CommandList *>(r, 0x8d908) != cmd)
        {
            if (++g.skipped % 600 == 1)
                Log("pass %u refused (%llu total)", i + 1,
                    static_cast<unsigned long long>(g.skipped));
            break;
        }
        g.lastJob = jobAfter;
        // v0.2.17: 0x8d808 and 0x8d80c are two watchdog job counters, NOT a
        // host pointer to an abort word. Its watchdog (0x16462/0x16468) writes
        // a job id to each DWORD when a timeout occurs. Interpreting the pair
        // as a pointer then writing through it crashes on the next recording
        // (reproduced at frame 28 in framecheck). The runtime owns resetting
        // the real GPU abort flag through hipMemcpyAsync; leave it to do so.
        ++accepted;

        // Reported, not enforced. Whether the engine bumps the job id once per recording or once
        // per submission is not established, so acting on this would risk breaking out of the
        // loop on a pass that was in fact fine -- which is exactly the failure being fixed. It
        // prints instead, and the log then says plainly whether a second pass is real work.
        if (wanted > 1 && !g.loggedPassDetail)
            Log("pass %u of %u: job id %u -> %u (%s), list marker %s", i + 1, wanted, jobBefore,
                jobAfter, jobAfter != jobBefore ? "moved, the engine recorded something"
                                               : "DID NOT MOVE -- this pass may be a no-op",
                At<ID3D12CommandList *>(r, 0x8d908) == cmd ? "ours" : "not ours");

        // Inline submission orders both the image dependency and the CPU tuning.
        // The legacy batch path below only orders resource accesses on the GPU.
        if (i + 1 < wanted)
        {
            if (g.serialPasses.load() && g.inlineMode.load())
            {
                // The worker reads tuning from module globals when it runs, not
                // when RecordFn records a job. Finish this pass before the next
                // one overwrites them. A UAV barrier only orders GPU accesses.
                if (!submitPass || !submitPass())
                {
                    g.failed = true;
                    Log("could not finish pass %u before changing its parameters", i + 1);
                    return false;
                }
            }
            else
            {
                D3D12_RESOURCE_BARRIER between {};
                between.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                between.UAV.pResource = g.netColour.Get();
                cmd->ResourceBarrier(1, &between);
            }
        }
    }
    g.activePasses = accepted;
    // The same table the instrumented NVIDIA fork prints, in the same columns, so the two logs can
    // be put side by side and read off. Once per arrangement: it reprints when the count or any
    // resolved value changes and stays quiet otherwise.
    if (runNetwork)
    {
        char table[512];
        int n = std::snprintf(table, sizeof(table),
                              "resolved tuning per pass (%u asked, %u accepted)\n"
                              "  pass   structure  tone       skin", wanted, accepted);
        for (UINT i = 0; i < wanted && n > 0 && n < static_cast<int>(sizeof(table)); ++i)
        {
            const PassTune t = TuningFor(i);
            n += std::snprintf(table + n, sizeof(table) - n,
                               "\n  %-6u %-10.4f %-10.4f %-10.4f%s", i + 1,
                               static_cast<double>(t.structure), static_cast<double>(t.tone),
                               static_cast<double>(t.skin),
                               g.passOverride[i].load() ? "  (own profile)" : "");
        }
        static char lastTable[512] = {};
        if (std::strcmp(table, lastTable) != 0)
        {
            std::strncpy(lastTable, table, sizeof(lastTable) - 1);
            Log("%s", table);
        }
    }
    if (wanted > 1 && !g.loggedPassDetail)
    {
        g.loggedPassDetail = true;
        Log("multipass parameter handoff: %s",
            g.serialPasses.load() && g.inlineMode.load()
                ? "each inline pass completes before the next tuning is written"
                : "legacy batch (worker may read the last pass's tuning for every pass)");
        Log("pass count: %u asked for, %u accepted. Compare the 'measure, residual' line against "
            "a run at 1 -- an extra pass that records but changes nothing reads as the same "
            "residual, and an extra pass that is a no-op reads as the same residual too. The "
            "per-pass job ids above are what tells those two apart.", wanted, accepted);
    }
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
    UINT cdims[12] { w, h, nw, nh, static_cast<UINT>(encMode), 0, 0,
                     (g.bicubic.load() ? 1u : 0u) | (static_cast<UINT>(dbg) << 1), 0, 0, 0, 0 };
    std::memcpy(&cdims[5], &kWhite, sizeof(float));
    std::memcpy(&cdims[6], &strength, sizeof(float));
    const float rlimit = g.residualLimit.load(), rfade = g.residualFade.load();
    std::memcpy(&cdims[8], &rlimit, sizeof(float));
    std::memcpy(&cdims[9], &rfade, sizeof(float));
    const float cstrength = g.colourStrength.load();
    const float guardEff = EffectiveGuard();
    std::memcpy(&cdims[10], &cstrength, sizeof(float));
    std::memcpy(&cdims[11], &guardEff, sizeof(float));
    cmd->SetComputeRoot32BitConstants(1, 12, cdims, 0);
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
        g.historyValid.store(true);
    }
    else
    {
        // The chain did not run this frame -- skipped because the previous evaluation was still
        // pending, or refused by the engine -- so the history texture still holds the frame
        // before last, and it was never marked stale. Next frame the denoiser is handed a
        // two-frame-old image together with one frame of motion, and every further skip widens
        // the gap without ever clearing it: the history stayed valid from the first frame that
        // set it until the resolution changed.
        //
        // That mismatch is the ghosting. It is worst precisely where frames get dropped -- loads,
        // cutscenes, alt-tab, anything that stalls the queue -- which is where it gets reported.
        // Invalidating costs the denoiser one frame of accumulation; not invalidating costs a
        // smear that has no way to decay.
        g.historyValid.store(false);
    }

    return true;
}

// How many passes to run this frame.
//
// Inline means the game is blocked on the GPU until every pass has finished, so N passes add
// into one stall of N times a single evaluation. That used to force the count back to 1 whenever
// inline was on -- and inline is the default, so the slider moved, saved, and did nothing on
// every default install. The reason for the force was the crash theory, and one full engine per
// pass in VRAM is gone. Cost in inline is framerate, which the overlay colours and says.
UINT WantedPasses()
{
    return static_cast<UINT>(
        std::clamp(g.passes.load(), 1, static_cast<int>(State::kMaxPasses)));
}

// One engine, recorded once per pass. Nothing to fall back to and no extra files to install.
bool BringUpEngines(UINT &)
{
    return InitPipeline() && InitEngine();
}

#if DLSS5_WITH_VULKAN
#include "vk_route.inc"
#endif

void OnPresent(command_queue *queue, swapchain *sc, const rect *, const rect *, uint32_t,
               const rect *)
{
    const Profile &profile = ProfileForThisProcess();
    std::lock_guard guard(g.lock);
    // Opt-in diagnostic controls, only on the foreground game's swapchain.
    // No per-frame file polling and no UI interaction needed for matched captures.
    if (g.diagnostics)
    {
        static bool reloadDown = false, captureDown = false;
        const bool foreground = sc != nullptr && sc->get_hwnd() == GetForegroundWindow();
        const bool ctrl = foreground && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool reload = ctrl && (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        const bool capture = ctrl && (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
        if (reload && !reloadDown) { LoadSettings(); Log("Ctrl+Home: diagnostic settings reloaded"); }
        if (capture && !captureDown)
        {
            g.capturePair = true; g.measured = false; g.measureTries = 0;
            g.measureNow.store(true);
            Log("Ctrl+PageDown: matched input/runtime capture requested");
        }
        reloadDown = reload; captureDown = capture;
    }
    if (ToggleRequested())
    {
        const bool on = !g.enabled.load();
        g.enabled.store(on);
        Log("%s: %s", HotkeyName().c_str(), on ? "on" : "off");
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

    // Alt-tab, and the whole class of bugs behind it.
    //
    // Nothing this add-on produces is visible while the game's window is minimised, and every
    // expensive thing it does behaves badly there. Windows deprioritises a background process's
    // GPU work, so our cross-device copies, our fence waits and -- worst of all -- the engine's
    // own *inline* CPU spin all run long. That spin is on the game's render thread, with a fixed
    // iteration cap sized for a foreground frame, and it is what "Styx freezes randomly when I
    // alt-tab to desktop" looks like from inside the process.
    //
    // Standing down here costs a frame nobody is looking at, and it covers all three routes.
    //
    if (auto *hwnd = static_cast<HWND>(sc->get_hwnd()); hwnd != nullptr)
    {
        // Alt-tab, when the user asked for it to switch the effect off. A real switch-off, not a
        // pause: the next frame leaves through the `!enabled` return further up and stays there
        // until the hotkey is pressed. Losing focus is the broader condition and fires first, so
        // this also covers exclusive fullscreen, which minimises on the way out.
        if (g.disableOnAltTab.load() && GetForegroundWindow() != hwnd)
        {
            g.enabled.store(false);
            // Whenever it is switched back on, that first frame must not be handed a history
            // from before the alt-tab, however many minutes ago that was.
            g.historyValid.store(false);
            Log("alt-tab: effect switched off, because Disable On Alt-Tab is on. It stays off; "
                "press %s in the game to bring it back.", HotkeyName().c_str());
            return;
        }

        // Minimised, unconditionally and whatever the switch above is set to. This one resumes
        // on its own, because it is a safety rather than a preference: there is no decision for
        // a user to make about frames nobody can see.
        if (IsIconic(hwnd))
        {
            if (!g.loggedHidden)
            {
                g.loggedHidden = true;
                Log("the window is minimised, so the add-on is sitting the frame out. It picks "
                    "back up on restore. This is not an error, and it is only logged once.");
            }
            g.windowHidden = true;
            return;
        }
    }
    if (g.windowHidden)
    {
        g.windowHidden = false;
        // Coming back from minimised, the last network output is however many seconds old, while
        // the motion vectors handed with it describe a single frame of movement. Feeding that to
        // a temporal denoiser is asking it to smear a stale frame across the new one, which is
        // the ghosting people see for a second or two after alt-tabbing back. Start clean.
        g.historyValid.store(false);
        Log("window restored; dropping the temporal history so nothing from before the alt-tab "
            "is carried into the new frame.");
    }

#if DLSS5_WITH_VULKAN
    // Vulkan. The host -- RPCS3 is the one this was built for -- never makes a D3D12 call, so
    // the network cannot run on its device. Same answer as D3D11: a second D3D12 device of our
    // own, and shared textures between the two. The crossing runs the other way round, because
    // memory exported from Vulkan is opaque and D3D12 cannot open it. See vk_route.inc.
    if (dev->get_api() == device_api::vulkan)
    {
        if (g.noBridge.load() || g.goneSwapchain.load() == sc)
            return;
        if (!LoadGraphicsApi())
        {
            g.unavailable = true;
            g.reason = "the D3D12 or DXGI entry points could not be resolved";
            return;
        }
        vkroute::Present(queue, sc);
        return;
    }
#endif

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
            // Do not paper over a reason the bring-up already gave. The hash refusal names the
            // file the user has to replace; "could not bring the engine up" names nothing.
            if (*g.reason == 0)
            {
                g.reason = "could not bring the engine up on the bridge device";
                Log("off: %s", g.reason);
            }
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
            Log("unsupported graphics API %u; Vulkan transport in this build: %d",
                static_cast<unsigned>(dev->get_api()), DLSS5_WITH_VULKAN);
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
    UINT wanted = WantedPasses();
    if (!BringUpEngines(wanted))
    {
        g.unavailable = true;
        if (*g.reason == 0)
        {
            g.reason = "could not bring the engine up";
            Log("off: %s", g.reason);
        }
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
            reinterpret_cast<volatile LONG *>(&At<UINT>(g.runtime, 0x8d6f4)), 0, 0)) <
            g.lastJob;
    if (jobPending)
    {
        if (GetTickCount64() - g.lastJobAt < 500)
        {
            runNetwork = false;
            if (++g.skipped % 120 == 1)
                Log("network skipped: previous evaluation still pending (%llu skipped, %llu done). Those "
                    "frames go out as the game drew them; a correction aimed at an older picture "
                    "reads as a trail, not as detail.",
                    static_cast<unsigned long long>(g.skipped),
                    static_cast<unsigned long long>(g.frame));
        }
        else
        {
            if (g.skipped % 600 == 0)
                Log("previous job did not finish in 500 ms; continuing anyway (%llu skipped)",
                    static_cast<unsigned long long>(g.skipped));
            g.lastJob = 0;
        }
    }

    auto *cmd = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native());
    const resource backRes = back;
    {
        resource_usage a = resource_usage::present, b = resource_usage::shader_resource;
        cmd_list->barrier(1, &backRes, &a, &b);
    }
    if (!RecordNetwork(cmd, backbuffer, bd.Format, nullptr, runNetwork, wanted, [&]() {
        ID3D12CommandList *submitted[] {cmd};
        reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(g.runtime) + 0x9170)(
            g.queue.Get(), 1, submitted);
        queue->flush_immediate_command_list();
        if (!FinishSubmittedPass())
            return false;
        cmd_list = queue->get_immediate_command_list();
        if (cmd_list == nullptr)
            return false;
        cmd = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native());
        return cmd != nullptr;
    }))
    {
        if (g.failed)
            return;
        resource_usage a = resource_usage::shader_resource, b = resource_usage::present;
        cmd_list->barrier(1, &backRes, &a, &b);
        queue->flush_immediate_command_list();
        return;
    }
    if (CompositionIsFresh(runNetwork))
    {
        {
            resource_usage a = resource_usage::shader_resource, b = resource_usage::copy_dest;
            cmd_list->barrier(1, &backRes, &a, &b);
        }
        cmd->CopyResource(backbuffer, g.composed.Get());
        {
            resource_usage a = resource_usage::copy_dest, b = resource_usage::present;
            cmd_list->barrier(1, &backRes, &a, &b);
        }
    }
    else
    {
        // Skipped frame: the game's own image goes out untouched. Same transition the early-out
        // above uses, because the back buffer was put into shader_resource on the way in and has
        // to reach present either way.
        resource_usage a = resource_usage::shader_resource, b = resource_usage::present;
        cmd_list->barrier(1, &backRes, &a, &b);
    }
    const UINT nw = g.netWidth, nh = g.netHeight;
    ID3D12CommandList *submitted[] { cmd };
    if (g.activePasses != 0)
        reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(g.runtime) + 0x9170)(
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
            static_cast<unsigned long long>(g.depthEvents.load()),
            g.depthBest != nullptr ? "found" : "none");
}

// Anything that makes one evaluation slower is a driver-reset risk while the game is waiting on
// the GPU for it, so the warning goes next to the control rather than in a readme.
void Note(const ImVec4 &colour, const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

const ImVec4 kWarn { 1.0f, 0.80f, 0.30f, 1.0f };
const ImVec4 kDanger { 1.0f, 0.45f, 0.35f, 1.0f };

// Every user-visible string in the overlay goes through this. Two literals at the call site
// instead of an id, a table and a lookup: the translation is then impossible to get out of sync
// with the text it translates, and adding a control cannot leave a dangling key behind.
// ponytail: a real string table earns its keep at a third language, not at two.
const char *T(const char *en, const char *pt)
{
    return g.language.load() == 0 ? en : pt;
}

// A tooltip instead of a paragraph. Every control had its explanation printed underneath it,
// which made the panel a wall of grey text you had to read past to reach the next slider. The
// text is worth keeping -- most of it is a measured result, not a description -- so it moves
// behind the marker and the panel goes back to being a panel.
void Help(const char *en, const char *pt)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (!ImGui::IsItemHovered())
        return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
    ImGui::TextUnformatted(T(en, pt));
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// How well each control is actually known, printed next to it. The question "which of these
// does anything in the game" had to be answered by reading the source or three handoffs, and
// the honest answer is not the same for any two controls. So it goes in the panel:
//
//   MEASURED  a 'measure, residual' reading moved when this changed. It does something.
//   TRACED    the write reaches a consumer -- a decompiled reader, or our own shader -- but no
//             reading here has ever separated it from its default.
//   UNKNOWN   written into a real engine field whose effect nobody here has established.
//   INERT     swept and measured to change nothing. Kept only to be re-checked elsewhere.
enum Known { kMeasured, kTraced, kUnknown, kInert };
void Tag(Known k)
{
    static const ImVec4 colours[] { { 0.4f, 1.0f, 0.4f, 1.0f },  { 0.55f, 0.75f, 1.0f, 1.0f },
                                    { 1.0f, 0.80f, 0.30f, 1.0f }, { 0.6f, 0.6f, 0.6f, 1.0f } };
    static const char *en[] { "MEASURED", "TRACED", "UNKNOWN", "INERT" };
    static const char *pt[] { "MEDIDO", "RASTREADO", "DESCONHECIDO", "INERTE" };
    ImGui::SameLine();
    ImGui::TextColored(colours[k], "%s", T(en[k], pt[k]));
}

// Paints a control red or amber while its CURRENT VALUE is one that has caused trouble. Scoped
// so it can be declared in an if-init and still wrap the widget:
//     if (Risk r(kDanger, cond); ImGui::SliderFloat(...))
// ImGui draws a widget's label with ImGuiCol_Text, so pushing the colour colours the control.
struct Risk
{
    bool on;
    Risk(const ImVec4 &colour, bool active) : on(active)
    {
        if (on)
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
    }
    ~Risk()
    {
        if (on)
            ImGui::PopStyleColor();
    }
    Risk(const Risk &) = delete;
    Risk &operator=(const Risk &) = delete;
};

void StatusLine()
{
    if (g.unavailable)
        ImGui::TextColored(kDanger, T("Unavailable: %s", "Indisponível: %s"), g.reason);
    else if (g.failed)
        ImGui::TextColored(kDanger, T("Stopped after an error. See the log.",
                                      "Parou depois de um erro. Veja o log."));
    else if (!g.enabled.load())
        ImGui::TextDisabled(T("Off.", "Desligado."));
    else if (g.frame == 0)
        ImGui::TextColored(kWarn, T("No frames processed yet.", "Nenhum quadro processado ainda."));
    else
    {
        const uint64_t seen = g.frame + g.skipped;
        const double pct = seen != 0 ? 100.0 * static_cast<double>(g.skipped) / seen : 0.0;
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                           T("Running: %llu processed, %llu skipped (%.0f%%)",
                             "Rodando: %llu processados, %llu pulados (%.0f%%)"),
                           static_cast<unsigned long long>(g.frame),
                           static_cast<unsigned long long>(g.skipped), pct);
    }
}

void OnOverlay(effect_runtime *)
{
    bool on = g.enabled.load();
    if (ImGui::Checkbox(T("Enabled", "Ligado"), &on))
    {
        g.enabled.store(on);
        Log("menu: %s", on ? "on" : "off");
    }
    ImGui::SameLine();
    const std::string hotkey = HotkeyName();
    ImGui::TextDisabled("(%s)", hotkey.c_str());
    ImGui::SameLine();
    StatusLine();

    {
        bool start = g.startOn.load();
        if (ImGui::Checkbox(T("Enabled from the first frame", "Ligado desde o primeiro quadro"),
                            &start))
        {
            g.startOn.store(start);
            Log("menu: start enabled %d", start ? 1 : 0);
        }
        Help("Whether Enabled above is already ticked when the game opens, instead of waiting "
             "for the hotkey every time. `StartOn=1` in dlss5-neural.ini.\n\n"
             "For a game that takes a while to get back into, pressing a key every launch is "
             "work for nothing. Set this once and it stays set.",

             "Se o Ligado aí em cima já vem marcado quando o jogo abre, em vez de esperar a "
             "tecla de atalho toda vez. `StartOn=1` no dlss5-neural.ini.\n\n"
             "Em jogo que dá trabalho pra voltar, apertar tecla todo lançamento é trabalho à "
             "toa. Marque uma vez e fica.");

        bool altTab = g.disableOnAltTab.load();
        if (ImGui::Checkbox(T("Disable the effect on alt-tab",
                              "Desativar o efeito ao dar alt-tab"), &altTab))
        {
            g.disableOnAltTab.store(altTab);
            Log("menu: disable on alt-tab %d", altTab ? 1 : 0);
        }
        Help("Switch the effect off the moment the game stops being the window in front, and "
             "leave it off. Coming back to the game, it is still off: turn it on with the hotkey "
             "or the box above when you want it. `DisableOnAltTab=1` in dlss5-neural.ini.\n\n"
             "This does not resume by itself, on purpose. Sitting out a minimised window is a "
             "separate safety that is always on and does resume, because frames nobody can see "
             "are not a decision anyone needs to make.",

             "Desliga o efeito no instante em que o jogo deixa de ser a janela da frente, e "
             "deixa desligado. Ao voltar pro jogo ele continua desligado: você liga na tecla de "
             "atalho ou na caixa aí em cima quando quiser. `DisableOnAltTab=1` no "
             "dlss5-neural.ini.\n\n"
             "Não volta sozinho, de propósito. Pular quadros de janela minimizada é outra coisa, "
             "uma proteção que está sempre ligada e essa sim volta sozinha — quadro que ninguém "
             "vê não é decisão de usuário.");

        // Rebinding by capturing a real keypress, rather than by typing a virtual-key code.
        // Only advances while the overlay is open, which is where the button is.
        static bool capturing = false;
        ImGui::TextUnformatted(T("Toggle hotkey", "Tecla de atalho"));
        ImGui::SameLine();
        if (ImGui::Button(capturing ? T("press a key (Esc cancels)", "aperte uma tecla (Esc cancela)")
                                    : hotkey.c_str()))
            capturing = !capturing;
        Help("Click, then press the combination you want. Modifiers held at that moment are part "
             "of the binding. Esc cancels and keeps the current one.\n\n"
             "Saved to the ini as ToggleKey (a virtual-key code) and ToggleMods (1 Ctrl, 2 Alt, "
             "4 Shift, added together). A key with no modifier is allowed and will fire during "
             "normal play, so pick one the game does not use.",

             "Clique e aperte a combinação que quiser. Os modificadores segurados nesse momento "
             "fazem parte do atalho. Esc cancela e mantém o atual.\n\n"
             "Salvo no ini como ToggleKey (código de tecla virtual) e ToggleMods (1 Ctrl, 2 Alt, "
             "4 Shift, somados). Uma tecla sem modificador é permitida e vai disparar durante o "
             "jogo normal, então escolha uma que o jogo não use.");

        if (capturing)
        {
            // From 0x08 so the mouse buttons, which are what clicked the button, cannot bind.
            for (int vk = 0x08; vk <= 0xFE; ++vk)
            {
                if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN ||
                    vk == VK_RWIN || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU ||
                    vk == VK_RMENU || vk == VK_LSHIFT || vk == VK_RSHIFT)
                    continue;
                if ((GetAsyncKeyState(vk) & 0x8000) == 0)
                    continue;
                capturing = false;
                if (vk == VK_ESCAPE)
                    break;
                int mods = 0;
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= 1;
                if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= 2;
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= 4;
                g.toggleKey.store(vk);
                g.toggleMods.store(mods);
                Log("menu: toggle bound to %s", HotkeyName().c_str());
                break;
            }
        }
    }

    int lang = g.language.load();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::Combo(T("Language", "Idioma"), &lang, "English\0Português (Brasil)\0"))
    {
        g.language.store(lang);
        Log("menu: language %d", lang);
    }

    if (ImGui::CollapsingHeader(T("Image", "Imagem"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        int enc = g.encoding.load();
        if (ImGui::Combo(T("Encoding", "Codificação"), &enc, "sRGB\0Linear\0scRGB-nl\0"))
        {
            g.encoding.store(enc);
            Log("menu: encoding %d", enc);
        }
        Help("Conversion applied before the runtime and reversed during composition.\n\n"
             "Use sRGB for an SDR frame: it passes the code values through unchanged, and "
             "automatic Tonemap stays off even though the transport texture is FP16. "
             "Diffuse White has no effect in this mode. Linear and scRGB-nl are experimental "
             "conversion paths, not evidence of the model's training domain.\n\n"
             "Restart the game after changing Encoding or Tonemap; the runtime caches its "
             "input conversion when it creates staging resources.",

             "Conversão aplicada antes do runtime e revertida na composição.\n\n"
             "Use sRGB para um quadro SDR: os valores passam sem conversão, e o Tonemap "
             "automático fica desligado mesmo com transporte em FP16. Branco Difuso não "
             "tem efeito nesse modo. Linear e scRGB-nl são conversões experimentais; não "
             "comprovam o domínio em que o modelo foi treinado.\n\n"
             "Reinicie o jogo após alterar Codificação ou Tonemap; o runtime guarda a "
             "conversão ao criar os recursos de entrada.");
        Tag(kMeasured);

        ImGui::BeginDisabled(g.encoding.load() == 0);
        float white = g.diffuseWhite.load();
        if (ImGui::SliderFloat(T("Diffuse White", "Branco Difuso"), &white, 80.0f, 1000.0f,
                               "%.0f nits", 0))
            g.diffuseWhite.store(white);
        ImGui::EndDisabled();
        Help("How many nits a value of 1.0 means, which sets the scale of the linear image the "
             "network is handed.\n\n"
             "The documented automatics are 100 for linear BT.709, 203 for scRGB-nl and 250 for "
             "PQ or scRGB linear. Anything else is a guess, and a wrong scale here looks like the "
             "network over- or under-reacting everywhere at once.\n\n"
             "Inert while Encoding is sRGB.",

             "Quantos nits um valor de 1.0 significa, o que define a escala da imagem linear "
             "entregue à rede.\n\n"
             "Os automáticos documentados são 100 para linear BT.709, 203 para scRGB-nl e 250 "
             "para PQ ou scRGB linear. Qualquer outro valor é chute, e uma escala errada aqui "
             "parece a rede reagindo demais ou de menos em tudo ao mesmo tempo.\n\n"
             "Inerte enquanto a Codificação for sRGB.");
        Tag(kTraced);

        float v = g.intensity.load();
        if (ImGui::SliderFloat(T("Overall Intensity", "Intensidade Geral"), &v, 0.0f, 2.0f,
                               "%.2f", 0))
            g.intensity.store(v);
        Help("Blends the network's correction over the game's image, after the fact.\n\n"
             "This is a mix, not a parameter of the network: it can show more or less of what the "
             "network did, never make it do more. 0.00 is the same picture as switching the "
             "add-on off, which makes it the fastest A/B there is.",

             "Mistura a correção da rede sobre a imagem do jogo, depois do fato.\n\n"
             "Isto é uma mistura, não um parâmetro da rede: pode mostrar mais ou menos do que a "
             "rede fez, nunca fazer ela fazer mais. 0.00 dá a mesma imagem que desligar o add-on, "
             "o que faz dele o A/B mais rápido que existe.");
        Tag(kMeasured);

        // How the correction is put back onto the frame. This is the arrangement Pass Count
        // needed: additive composition made a second pass mean twice the difference, clipped per
        // channel, and a clipped channel is a hue rotation rather than more detail.
        {
            int comp = g.ratioGuard.load() > 0.0f ? 1 : 0;
            const char *items[] = { T("Additive (old)", "Aditiva (antiga)"),
                                    T("Ratio (bounded)", "Razão (limitada)") };
            if (ImGui::Combo(T("Composition", "Composição"), &comp, items, 2))
            {
                g.ratioGuard.store(comp == 1 ? 2.0f : 0.0f);
                Log("menu: composition %s", comp == 1 ? "ratio" : "additive");
            }
            Help("How the network's answer is put back onto the frame.\n\n"
                 "Additive adds the correction to the picture, channel by channel, and clips "
                 "whatever leaves the range. That is what this add-on always did, and it is why "
                 "Pass Count was useless: two passes is twice the difference and three is three "
                 "times it, and a clipped channel is a hue rotation -- so the count did not read "
                 "as more detail, it read as more saturation and then as a mess.\n\n"
                 "Ratio turns the answer into a picture of its own, compares its luminance "
                 "against the frame's as a ratio, bounds that ratio, and blends two finished "
                 "pictures. A bounded ratio cannot move hue. This is what the OptiScaler DLSS-NR "
                 "fork does, and RenoDX's DLSS 5 addon before it.\n\n"
                 "Kept switchable so both can be seen in one session. Ratio is the default.",

                 "Como a resposta da rede volta para o quadro.\n\n"
                 "Aditiva soma a correção na imagem, canal por canal, e corta o que sair da "
                 "faixa. É o que este add-on sempre fez, e é por isso que o Número de Passes não "
                 "servia: dois passes é o dobro da diferença e três é o triplo, e canal cortado é "
                 "rotação de matiz -- então a contagem não aparecia como mais detalhe, aparecia "
                 "como mais saturação e depois como sujeira.\n\n"
                 "Razão transforma a resposta numa imagem própria, compara a luminância dela com "
                 "a do quadro como razão, limita essa razão, e mistura duas imagens inteiras. Uma "
                 "razão limitada não move matiz. É o que o fork DLSS-NR do OptiScaler faz, e o "
                 "addon DLSS 5 do RenoDX antes dele.\n\n"
                 "Deixado trocável para dar para ver os dois na mesma sessão. Razão é o padrão.");
            Tag(kTraced);
        }

        ImGui::BeginDisabled(g.ratioGuard.load() <= 0.0f);
        v = g.colourStrength.load();
        if (ImGui::SliderFloat(T("Colour Strength", "Força da Cor"), &v, 0.0f, 1.0f, "%.2f", 0))
            g.colourStrength.store(v);
        Help("Whether the network's colour arrives with its light.\n\n"
             "0 keeps the game's own hue exactly: every pixel is the original colour and only its "
             "brightness carries what the network decided. This is the setting for \"it changed "
             "the colours\" -- at 0 it cannot, by construction. 1 brings the network's colour "
             "with it.\n\n"
             "It cannot shift hue on its own either way: both ends of the blend carry the same "
             "luminance, so this moves chroma and nothing else.\n\n"
             "Inert on Additive composition.",

             "Se a cor da rede vem junto com a luz dela.\n\n"
             "0 mantém a matiz do jogo exatamente: cada pixel fica com a cor original e só o "
             "brilho carrega o que a rede decidiu. É este o controle para \"mudou as cores\" -- "
             "em 0 ele não consegue mudar, por construção. 1 traz a cor da rede junto.\n\n"
             "Também não desloca matiz sozinho nos dois sentidos: as duas pontas da mistura "
             "carregam a mesma luminância, então isto mexe em croma e mais nada.\n\n"
             "Inerte na composição Aditiva.");
        Tag(kTraced);

        v = g.ratioGuard.load();
        if (v > 0.0f && ImGui::SliderFloat(T("Highlight Guard", "Trava de Realce"), &v, 1.0f, 8.0f,
                                           "%.1fx", 0))
            g.ratioGuard.store(v);
        Help("The most compose may move any pixel, as a multiple of what it already was, in both "
             "directions. A pixel may not be brightened past this nor darkened past its "
             "reciprocal.\n\n"
             "One scalar, taken from luminance and applied to the whole triple, so it bounds "
             "brightness without touching hue. Lights are where the network has least to say and "
             "where an unbounded answer does the most damage.\n\n"
             "2.0x is the reference fork's default and leaves detail intact. Raise it only if "
             "bright areas look clipped.",

             "O máximo que a composição pode mover um pixel, como múltiplo do que ele já era, nos "
             "dois sentidos. Um pixel não pode ser clareado além disto nem escurecido além do "
             "inverso.\n\n"
             "Um escalar só, tirado da luminância e aplicado no trio inteiro, então limita brilho "
             "sem tocar em matiz. Luzes são onde a rede tem menos a dizer e onde uma resposta sem "
             "limite estraga mais.\n\n"
             "2.0x é o padrão do fork de referência e não come detalhe. Só aumente se áreas "
             "claras parecerem estouradas.");
        Tag(kTraced);

        bool track = g.guardTracksPasses.load();
        if (ImGui::Checkbox(T("Guard follows Pass Count", "Trava acompanha o Número de Passes"),
                            &track))
        {
            g.guardTracksPasses.store(track);
            Log("menu: guard follows pass count %d", track ? 1 : 0);
        }
        Help("Adds one multiple of headroom per extra pass, so 2.0x becomes 3.0x at two passes "
             "and 4.0x at three. Off by default.\n\n"
             "The guard is applied once, to the finished composition, while the passes compound "
             "the ratio inside it. Left fixed, the third pass spends most of its contribution "
             "against the clamp -- it costs a whole extra network run and most of it is thrown "
             "away. The reference fork's tooltip suggests raising the guard by hand with the "
             "count; this does it for you.\n\n"
             "It is off because the reference does not actually do it. Nine OptiScaler logs off "
             "RTX machines carry 254 composition lines across seven games, at one pass and at "
             "two, and every one of them reads guard 2.0x -- the single 1.5x in the set is "
             "someone dragging the slider down. Their ini says MaxRatio defaults to 2.0 and "
             "never mentions the count. A fixed bound is also the honest way to see what an "
             "extra pass is contributing.",

             "Acrescenta um múltiplo de folga por passe extra, então 2.0x vira 3.0x em dois "
             "passes e 4.0x em três. Desligado por padrão.\n\n"
             "A trava é aplicada uma vez, na composição pronta, enquanto os passes acumulam a "
             "razão dentro dela. Fixa, o terceiro passe gasta quase toda a contribuição dele "
             "contra o limite -- custa uma rodada inteira da rede e joga a maior parte fora. O "
             "tooltip do fork de referência sugere subir a trava na mão junto com a contagem; "
             "isto faz por você.\n\n"
             "Está desligado porque o fork de referência não faz isso de verdade. Nove logs de "
             "OptiScaler de máquinas RTX trazem 254 linhas de composição em sete jogos, em um "
             "passe e em dois, e todas dizem guard 2.0x -- o único 1.5x do conjunto é alguém "
             "baixando o slider. A ini deles diz que MaxRatio tem padrão 2.0 e nunca cita a "
             "contagem. Um limite fixo também é o jeito honesto de ver o que um passe extra "
             "está somando.");
        Tag(kMeasured);
        ImGui::EndDisabled();

        v = g.residualLimit.load();
        if (ImGui::SliderFloat(T("Residual Limit", "Limite do Resíduo"), &v, 0.0f, 0.50f,
                               v <= 0.0f ? T("off", "desligado") : "%.3f", 0))
            g.residualLimit.store(v);
        Help("Caps how far the correction may push a single pixel, as a fraction of white. 0 is "
             "off and nothing is capped.\n\n"
             "This is the control for the blown blocks. A measured two-pass run on God of War "
             "reported a mean correction of 0.072 with a MAXIMUM OF 4.16 -- four times brighter "
             "than white, in a picture whose own mean is 0.13. That is not something the network "
             "saw; it is a tile where it extrapolated. Every extra pass then runs on top of that "
             "blown tile, so it compounds instead of averaging away.\n\n"
             "The whole correction is scaled, not clamped per channel: clamping one channel of a "
             "triple is a hue rotation, which turned a blown block into a blown coloured one.\n\n"
             "0.25 by default, which is over three times the typical correction, so an ordinary "
             "pixel never meets it. Lower it until the blocks go; too low flattens the whole "
             "effect, which Residual x8 shows immediately.",

             "Limita o quanto a correção pode empurrar um pixel, como fração do branco. 0 é "
             "desligado e nada é limitado.\n\n"
             "É este o controle dos blocos estourados. Uma medição de dois passes no God of War "
             "deu correção média 0.072 com MÁXIMO DE 4.16 -- quatro vezes mais claro que o "
             "branco, numa imagem cuja média é 0.13. Isso não é algo que a rede viu; é um bloco "
             "onde ela extrapolou. Cada passe extra roda em cima desse bloco estourado, então "
             "acumula em vez de diluir.\n\n"
             "A correção inteira é escalada, não cortada canal por canal: cortar um canal de um "
             "trio é rotação de matiz, o que transformava bloco estourado em bloco estourado e "
             "colorido.\n\n"
             "0.25 por padrão, que é mais de três vezes a correção típica, então pixel normal "
             "nunca encosta nele. Baixe até os blocos sumirem; baixo demais achata o efeito "
             "inteiro, o que o Resíduo x8 mostra na hora.");
        Tag(kTraced);

        v = g.residualFade.load();
        if (ImGui::SliderFloat(T("Edge Fade", "Suavizar Bordas"), &v, 0.0f, 0.25f,
                               v <= 0.0f ? T("off", "desligado") : "%.3f", 0))
            g.residualFade.store(v);
        Help("Rolls the correction off to nothing over a band at the frame border, given as a "
             "fraction of the frame. 0.02 is a 2% band -- about 20 pixels at 1080p. 0 is off.\n\n"
             "The tiles at the border have no neighbour on one side, so what the network returns "
             "there is invented rather than seen, and bicubic upsampling rings on top of it. That "
             "is the glitching in the corners: a corner is inside two border bands at once, so it "
             "gets both rolloffs and is the first place to go wrong and the first place this "
             "fixes.\n\n"
             "Cheaper than turning the whole effect down, because it only touches the band.",

             "Vai apagando a correção até zero numa faixa na borda do quadro, dada como fração do "
             "quadro. 0.02 é uma faixa de 2% -- uns 20 pixels em 1080p. 0 é desligado.\n\n"
             "Os blocos da borda não têm vizinho de um lado, então o que a rede devolve ali é "
             "inventado, não visto, e o upsample bicúbico ainda toca sino em cima. É isso o "
             "glitch nos cantos: um canto está dentro de duas faixas de borda ao mesmo tempo, "
             "então leva as duas quedas -- é o primeiro lugar a estragar e o primeiro que isto "
             "conserta.\n\n"
             "Mais barato que baixar o efeito inteiro, porque só toca na faixa.");
        Tag(kTraced);

        v = g.structure.load();
        if (ImGui::SliderFloat(T("Structure Intensity", "Intensidade de Estrutura"), &v, 0.0f,
                               3.0f, "%.2f", 0))
            g.structure.store(v);
        Help("Written into the engine at a fixed offset, and the one control measured to matter.\n\n"
             "Measured on God of War 2: at 0 the residual collapses 25x, which is the proof that "
             "what reaches the screen comes from the network at all. From 1 to 3 the magnitude "
             "grows about 6% and the structure ratio goes 0.18 to 0.28. It saturates; 3 is the "
             "useful end of it.",

             "Escrito no motor num offset fixo, e o único controle medido como relevante.\n\n"
             "Medido no God of War 2: em 0 o resíduo despenca 25x, o que é a prova de que o que "
             "chega na tela vem da rede. De 1 para 3 a magnitude cresce uns 6% e a razão de "
             "estrutura vai de 0,18 para 0,28. Satura; 3 é o fim útil dele.");
        Tag(kMeasured);

        v = g.skin.load();
        if (ImGui::SliderFloat(T("Skin Structure Strength", "Força de Estrutura na Pele"), &v,
                               -1.0f, 3.0f, "%.2f", 0))
            g.skin.store(v);
        Help("Same kind of offset, aimed at skin. Measured worth about 1.5% of the residual, "
             "which is close to run-to-run noise.\n\n"
             "-1.00 is the sentinel the engine itself boots with, and it is the strongest "
             "evidence anything reads this field at all: it means automatic. Writing 1.00 over "
             "it, which this add-on used to do on startup, turns the automatic off before you "
             "ever touch the slider.",

             "Mesmo tipo de offset, mirado na pele. Medido valendo uns 1,5% do resíduo, o que "
             "está perto do ruído entre execuções.\n\n"
             "-1.00 é a sentinela com que o próprio motor liga, e é a evidência mais forte de "
             "que alguém lê esse campo: significa automático. Escrever 1.00 por cima, o que este "
             "add-on fazia no início, desliga o automático antes de você tocar no slider.");
        Tag(kMeasured);
    }

    if (ImGui::CollapsingHeader(T("Performance", "Desempenho"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        // Same-frame timing is what turns every other cost into a stall, so it is coloured
        // exactly when there is a cost big enough for that to matter.
        int timing = g.inlineMode.load() ? 0 : 1;
        const float sc = g.scale.load();
        const int np = g.passes.load();
        const bool inlineHeavy = g.inlineMode.load() && (sc > 0.50f || np > 1);
        if (Risk r(g.inlineMode.load() && sc > 1.0f ? kDanger : kWarn, inlineHeavy);
            ImGui::Combo(T("Timing", "Momento"), &timing,
                         T("Same frame (inline)\0Async (previous frame)\0",
                           "Mesmo quadro (inline)\0Assíncrono (quadro anterior)\0")))
        {
            g.inlineMode.store(timing == 0);
            Log("menu: mode %s", timing == 0 ? "inline" : "async");
        }
        Help("Same frame waits for the current neural result. More passes increase frame time. "
             "This preview preserves each pass's parameters in this mode.\n\n"
             "Async uses an older correction. It retains the legacy pass handoff and can show "
             "stale detail; use Same frame when comparing per-pass settings.",
             "Mesmo quadro espera o resultado neural atual. Mais passes aumentam o tempo de quadro. "
             "Esta preview preserva os parâmetros de cada passe neste modo.\n\n"
             "Assíncrono usa uma correção anterior. Mantém a execução antiga dos passes e pode "
             "mostrar detalhes atrasados; use Mesmo quadro para comparar ajustes por passe.");
        Tag(kMeasured);

        float v = g.scale.load();
        if (Risk r(v > 1.0f && g.inlineMode.load() ? kDanger : kWarn, v > 0.50f);
            ImGui::SliderFloat(T("Resolution Scale", "Escala de Resolução"), &v, 0.25f, 2.0f,
                               "%.2f", 0))
            g.scale.store(v);
        Help("Network width and height relative to the game frame. 0.50 uses a quarter of the pixels; "
             "1.00 uses the full frame. Lower scales reduce fine detail and inference cost. "
             "Materials can still change below 1.00. Measure frame time at the chosen resolution.",
             "Largura e altura da rede em relação ao quadro do jogo. 0.50 usa um quarto dos pixels; "
             "1.00 usa o quadro inteiro. Escalas menores reduzem detalhe fino e custo da inferência. "
             "Materiais ainda podem mudar abaixo de 1.00. Meça o tempo de quadro na resolução escolhida.");
        Tag(kMeasured);
        if (g.outWidth != 0)
        {
            const UINT wantW = std::max<UINT>(64u, static_cast<UINT>(g.outWidth * v + 0.5f));
            const UINT wantH = std::max<UINT>(64u, static_cast<UINT>(g.outHeight * v + 0.5f));
            ImGui::Text(T("Network raster: %ux%u", "Raster da rede: %ux%u"), g.netWidth, g.netHeight);
            if (wantW != g.netWidth || wantH != g.netHeight)
            {
                ImGui::SameLine();
                ImGui::TextColored(kWarn, T("(asked for %ux%u -- not applied yet)",
                                            "(pediu %ux%u -- ainda não aplicado)"), wantW, wantH);
            }
        }
        if (v > 1.0f)
            Note(kWarn, T("Supersampling the neural input increases GPU time and memory use.",
                          "Supersampling da entrada neural aumenta o tempo de GPU e o uso de memória."));

        int passes = g.passes.load();
        const bool passDanger = g.inlineMode.load() && passes > 1 && sc > 1.0f;
        if (Risk r(passDanger ? kDanger : kWarn, passes > 1);
            ImGui::SliderInt(T("Pass Count", "Número de Passes"), &passes, 1,
                             static_cast<int>(State::kMaxPasses), "%d", 0))
        {
            g.passes.store(passes);
            Log("menu: pass count %d", passes);
        }
        Help("Runs the network over its own output one to three times. The final correction is "
             "measured against the original input. Later passes default to zero Local Tone; "
             "use Per pass or Taper to reduce Structure.\n\n"
             "More passes can strengthen material changes, grain and halos. GPU cost grows roughly "
             "with the pass count. Two and three inline passes were exercised on a saved NFS frame; "
             "quality in motion still needs validation.",
             "Roda a rede sobre a própria saída de uma a três vezes. A correção final é medida "
             "contra a entrada original. Os passes seguintes usam Tom Local zero por padrão; "
             "use Por passe ou Diminuir passes seguintes para reduzir Estrutura.\n\n"
             "Mais passes podem intensificar mudanças de material, granulado e halos. O custo de GPU "
             "cresce aproximadamente com a contagem. Dois e três passes inline foram executados "
             "num quadro salvo do NFS; a qualidade em movimento ainda precisa de validação.");
        Tag(kMeasured);
        if (passes > 1)
            Note(kWarn, T("Each extra pass costs another inference. Compare detail and frame time.",
                          "Cada passe extra custa outra inferência. Compare detalhe e tempo de quadro."));

        if (passes > 1)
        {
            bool taper = g.passTaper.load();
            if (ImGui::Checkbox(T("Taper later passes", "Diminuir passes seguintes"), &taper))
            {
                g.passTaper.store(taper);
                Log("menu: pass taper %s", taper ? "on" : "off");
            }
            Help("Halves Structure on each later pass: 1.0, 0.5, 0.25. Local Tone already defaults "
                 "to zero after pass 1. Skin keeps its selected mode. Explicit per-pass overrides "
                 "take precedence. This can reduce accumulated grain and outlines; compare in your scene.",
                 "Reduz Estrutura pela metade a cada passe: 1.0, 0.5, 0.25. Tom Local já usa zero "
                 "depois do primeiro passe. Pele mantém o modo escolhido. Ajustes explícitos por passe "
                 "têm prioridade. Pode reduzir granulado e contornos acumulados; compare na sua cena.");
            Tag(kTraced);
        }

        // Per-pass profiles, the reference fork's "Per pass" tree. A later pass is looking at a
        // picture an earlier one already edited, so the same numbers again ask it to sharpen its
        // own sharpening -- and that is the half of "3 passes looks deep fried" that the
        // composition cannot reach from outside, because it happens inside the network.
        if (passes > 1 && ImGui::TreeNode(T("Per pass", "Por passe")))
        {
            ImGui::TextUnformatted(T("What each run of the network is told, where it should "
                                     "differ from the values below.",
                                     "O que cada rodada da rede recebe, onde deve diferir dos "
                                     "valores abaixo."));
            for (int i = 0; i < passes; ++i)
            {
                char label[32];
                snprintf(label, sizeof(label), T("Pass %d", "Passe %d"), i + 1);
                if (!ImGui::TreeNode(label))
                    continue;
                ImGui::PushID(i);
                bool own = g.passOverride[i].load();
                if (ImGui::Checkbox(T("Own settings", "Ajustes próprios"), &own))
                {
                    g.passOverride[i].store(own);
                    Log("menu: pass %d profile %s", i + 1, own ? "on" : "off");
                }
                ImGui::BeginDisabled(!own);
                float pv = g.passStructure[i].load();
                if (ImGui::SliderFloat(T("Structure", "Estrutura"), &pv, 0.0f, 3.0f, "%.2f", 0))
                    g.passStructure[i].store(pv);
                pv = g.passTone[i].load();
                if (ImGui::SliderFloat(T("Local Tone", "Tom Local"), &pv, 0.0f, 3.0f, "%.2f", 0))
                    g.passTone[i].store(pv);
                pv = g.passSkin[i].load();
                if (ImGui::SliderFloat(T("Skin", "Pele"), &pv, 0.0f, 3.0f, "%.2f", 0))
                    g.passSkin[i].store(pv);
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::TreePop();
            }
            Note(kWarn, T("A pass with its own settings off follows the Engine tab, exactly as "
                          "before. The useful shape is a taper -- full on pass 1, less on 2, "
                          "less again on 3 -- because each pass is editing the last one's work.",
                          "Um passe com os ajustes próprios desligados segue a aba Motor, "
                          "exatamente como antes. O formato útil é uma queda -- cheio no passe "
                          "1, menos no 2, menos ainda no 3 -- porque cada passe está editando o "
                          "trabalho do anterior."));
            ImGui::TreePop();
        }

        bool bic = g.bicubic.load();
        if (ImGui::Checkbox(T("Bicubic Residual Upsample", "Upsample Bicúbico do Resíduo"), &bic))
        {
            g.bicubic.store(bic);
            Log("menu: residual upsample %s", bic ? "bicubic" : "bilinear");
        }
        Help("Below Resolution Scale 1.00 only the correction comes back up to full resolution. "
             "Stretching it bilinearly is a blur, which throws away everything but the "
             "low-frequency part -- colour and brightness -- and that alone was enough to make "
             "the whole effect look like a colour filter. Catmull-Rom keeps the rest.\n\n"
             "Turn it off if hard edges ring. No effect at all at Resolution Scale 1.00.",

             "Abaixo de Escala de Resolução 1.00, só a correção volta para a resolução cheia. "
             "Esticar ela bilinearmente é um borrão, que joga fora tudo menos a parte de baixa "
             "frequência -- cor e brilho -- e isso sozinho já bastava para o efeito inteiro "
             "parecer um filtro de cor. Catmull-Rom mantém o resto.\n\n"
             "Desligue se arestas duras ficarem com halo. Nenhum efeito em Escala 1.00.");
        Tag(kMeasured);
    }

    if (ImGui::CollapsingHeader(T("Guides", "Guias"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        // What is actually feeding the four Packet slots this frame. The most useful line in the
        // overlay: the network's ceiling is set here, not by any slider above it.
        ImGui::Text(T("Colour %s   Depth %s   Motion %s   Exposure not fed",
                      "Cor %s   Profundidade %s   Movimento %s   Exposição não alimentada"),
                    T("from the swapchain", "da swapchain"),
                    g.gameDepthActive    ? T("FROM THE GAME", "DO JOGO")
                    : g.depthSnapshot    ? T("pre-clear snapshot", "snapshot antes do clear")
                                         : T("none", "nenhuma"),
                    g.gameMotionActive ? T("FROM THE GAME", "DO JOGO") : T("estimated", "estimado"));
        Help("The four inputs the engine takes. This line, not any slider above it, sets the "
             "ceiling on what the network can do.\n\n"
             "On PCSX2 it is colour only: a PS2 never computed per-pixel motion, and its depth "
             "exists only between a bind and a clear. A modern engine on D3D11 hands over depth "
             "and motion both, and they are taken when they are there.",

             "As quatro entradas que o motor aceita. Esta linha, e não um slider acima dela, "
             "define o teto do que a rede consegue fazer.\n\n"
             "No PCSX2 é só cor: o PS2 nunca calculou movimento por pixel, e a profundidade dele "
             "existe só entre um bind e um clear. Um motor moderno em D3D11 entrega profundidade "
             "e movimento, e os dois são pegos quando estão lá.");

        // A buffer can be crossed, bound and fed and still be a cleared constant, which looks
        // identical from the outside and is worth nothing to the network.
        if (g.probeStillPct.load() >= 0)
        {
            const float lo = g.probeDepthMin.load(), hi = g.probeDepthMax.load();
            const bool depthReal = (hi - lo) > 1e-6f;
            const int still = g.probeStillPct.load();
            ImGui::Text(T("Measured  depth %.4f..%.4f   motion %d%% still, mean %.2f px, max %.2f px",
                          "Medido  profundidade %.4f..%.4f   movimento %d%% parado, média %.2f px, "
                          "máx %.2f px"),
                        static_cast<double>(lo), static_cast<double>(hi), still,
                        static_cast<double>(g.probeMotionMean.load()),
                        static_cast<double>(g.probeMotionMax.load()));
            Help("What the guides actually contain, not just whether they were handed over.\n\n"
                 "Read it while playing, not on a menu: a 2D screen genuinely has no depth and "
                 "nothing on it moves. In a real scene depth should have a spread and motion a "
                 "non-zero mean. If it stays flat while you drive, the guide picked the wrong "
                 "buffer and the network is still working from colour alone.",

                 "O que os guias realmente contêm, não só se foram entregues.\n\n"
                 "Leia jogando, não no menu: uma tela 2D genuinamente não tem profundidade e "
                 "nada nela se move. Numa cena de verdade a profundidade tem que ter espalhamento "
                 "e o movimento uma média diferente de zero. Se ficar chapado enquanto você "
                 "dirige, o guia pegou o buffer errado e a rede continua trabalhando só com cor.");
            if (!depthReal || still >= 100)
                Note(kWarn, T("A guide is carrying nothing. Expected on a menu -- check it while "
                              "driving.",
                              "Um guia não está carregando nada. Esperado no menu -- confira "
                              "dirigindo."));
        }
        else if (g.gameDepthActive || g.gameMotionActive)
            ImGui::TextDisabled(T("Measuring the guides...", "Medindo os guias..."));

        if (!g.gameDepthActive && !g.gameMotionActive)
            Note(kWarn, T("The network is running on colour alone.",
                          "A rede está rodando só com cor."));

        bool guides = g.useGameGuides.load();
        if (ImGui::Checkbox(T("Read Guides From The Game", "Ler Guias do Jogo"), &guides))
        {
            g.useGameGuides.store(guides);
            Log("menu: game guides %s", guides ? "on" : "off");
        }
        Help("D3D11 only. Watches which depth-stencil and which two-channel float target the game "
             "binds most often, copies each once a frame and carries them over the bridge to the "
             "network's device.\n\n"
             "Turn it off to fall back to estimated motion and no depth.",

             "Só D3D11. Observa qual depth-stencil e qual render target float de dois canais o "
             "jogo mais liga, copia cada um uma vez por quadro e leva pela ponte até o device da "
             "rede.\n\n"
             "Desligue para voltar a movimento estimado e nenhuma profundidade.");
        Tag(kMeasured);

        bool depth = g.useDepth.load();
        if (ImGui::Checkbox(T("Depth", "Profundidade"), &depth))
        {
            g.useDepth.store(depth);
            Log("menu: depth %s", depth ? "on" : "off");
        }
        Help("Hands the depth buffer to the engine and sets the flag that says it is valid.\n\n"
             "Depth is what stops the network guessing at geometry. It mainly buys stability and "
             "disocclusion, not sharper texture -- do not expect this one to transform the image.",

             "Entrega o buffer de profundidade ao motor e liga a flag que diz que ele é válido.\n\n"
             "Profundidade é o que faz a rede parar de chutar geometria. Compra principalmente "
             "estabilidade e desoclusão, não textura mais afiada -- não espere que este "
             "transforme a imagem.");
        Tag(kTraced);

        // Depth Inverted used to be a checkbox here. It is gone: 8d9b0 is pinned to the
        // engine's own default of 1 in the record path. No run on either target ever
        // produced a reading that told the two settings apart, so the only thing the switch
        // could do was be set wrong.
        bool hist = g.useHistory.load();
        if (Risk r(kWarn, hist); ImGui::Checkbox(T("History", "Histórico"), &hist))
        {
            g.useHistory.store(hist);
            g.historyValid.store(false);
            Log("menu: history %s", hist ? "on" : "off");
        }
        Help("Hands the engine last frame's output to carry forward.\n\n"
             "Motion vectors say where a pixel was. Without history there is nothing for them to "
             "point at, so this is the switch that turns motion from a number the engine reports "
             "into one it can use.\n\n"
             "Experimental: it writes a pointer into the runtime at a fixed offset, and a wrong "
             "one there hangs the game rather than failing. If the picture smears or the game "
             "stops responding, this is the first thing to turn off.",

             "Entrega ao motor a saída do quadro anterior para carregar adiante.\n\n"
             "Vetores de movimento dizem onde um pixel estava. Sem histórico não há para onde "
             "eles apontarem, então esta é a chave que transforma movimento de um número que o "
             "motor reporta em um que ele consegue usar.\n\n"
             "Experimental: escreve um ponteiro no runtime num offset fixo, e um ponteiro errado "
             "ali congela o jogo em vez de falhar. Se a imagem borrar ou o jogo parar de "
             "responder, esta é a primeira coisa a desligar.");
        Tag(kTraced);
        if (hist)
            Note(kWarn, T("Experimental -- first thing to turn off if the game hangs or smears.",
                          "Experimental -- primeira coisa a desligar se o jogo congelar ou "
                          "borrar."));

        bool mv = g.useMotion.load();
        if (ImGui::Checkbox(T("Motion Vectors", "Vetores de Movimento"), &mv))
        {
            g.useMotion.store(mv);
            Log("menu: motion %s", mv ? "on" : "off");
        }
        if (g.gameMotionActive)
            Help("Read from the game's own velocity buffer -- the real thing, per pixel.",
                 "Lidos do próprio buffer de velocidade do jogo -- a coisa real, por pixel.");
        else
            Help("Estimated by comparing consecutive frames, because this target has no velocity "
                 "buffer to read. It is wrong wherever pixels move without the geometry moving: "
                 "reflections, fire, moving shadows, and anything appearing from behind something "
                 "else. The NVIDIA route does the same thing here.",

                 "Estimados comparando quadros consecutivos, porque este alvo não tem buffer de "
                 "velocidade para ler. É errado onde pixels se movem sem a geometria se mover: "
                 "reflexos, fogo, sombras em movimento, e qualquer coisa que aparece de trás de "
                 "outra. A rota da NVIDIA faz o mesmo aqui.");
        Tag(kMeasured);

        if (mv)
        {
            float v = g.motionScale.load();
            if (ImGui::SliderFloat(T("Motion Scale", "Escala do Movimento"), &v, -2.0f, 2.0f,
                                   "%.2f", 0))
                g.motionScale.store(v);
            Help("How much motion the network is told there is. It multiplies the field on its "
                 "way into the engine, whichever field that is -- the game's own velocity buffer "
                 "or the estimated one. 0.00 says nothing moved, 1.00 is as measured, -1.00 "
                 "flips the direction, 0.50 suits a buffer stored in NDC.\n\n"
                 "It used to apply to the game's vectors only, and it was hidden on every target "
                 "without them -- so the estimated field, the one that is a guess and the one "
                 "most in need of turning down, had no control at all. Both go through it now.\n\n"
                 "Set it by eye: put Debug View on 'Motion vectors' and pan the camera. The field "
                 "should follow the camera steadily. Shimmer means it is too high -- turn it down "
                 "rather than turning motion off, which is the blunt version of the same thing.",

                 "Quanto movimento a rede é informada que existe. Multiplica o campo no caminho "
                 "para o motor, seja qual for o campo -- o buffer de velocidade do próprio jogo "
                 "ou o estimado. 0.00 diz que nada se moveu, 1.00 é como foi medido, -1.00 "
                 "inverte a direção, 0.50 serve para um buffer guardado em NDC.\n\n"
                 "Antes valia só para os vetores do jogo, e ficava escondido em todo alvo sem "
                 "eles -- então o campo estimado, que é um chute e é o que mais precisa ser "
                 "baixado, não tinha controle nenhum. Agora os dois passam por ele.\n\n"
                 "Ajuste no olho: ponha a Visão de Debug em 'Vetores de movimento' e gire a "
                 "câmera. O campo tem que acompanhar a câmera, firme. Cintilar quer dizer alto "
                 "demais -- baixe, em vez de desligar o movimento, que é a versão bruta da mesma "
                 "coisa.");
            Tag(kTraced);
        }
        if (mv && !g.gameMotionActive)
        {
            float v = g.flowGate.load();
            if (ImGui::SliderFloat(T("Flow Contrast Gate", "Portão de Contraste do Fluxo"), &v,
                                   0.002f, 0.10f, "%.3f", 0))
                g.flowGate.store(v);
            Help("The first of two filters that decide which pixels are allowed to move.\n\n"
                 "Before searching anything, the estimator measures the brightest and darkest "
                 "luma in the 3x3 block around the pixel. If the difference is below this number "
                 "the pixel is declared still and no search happens at all.\n\n"
                 "Why: a flat block -- clear sky, a painted wall -- matches equally well at every "
                 "offset, so the winner is simply whichever offset the loop tried first. That is "
                 "the aperture problem, and it is why widening the search made the field wilder "
                 "instead of better. There is nothing to track, so do not pretend to track it.\n\n"
                 "RAISE it and more of the screen is frozen: cleaner, but the network is told "
                 "nothing moved. 0.020 froze 99% of a dark God of War 2 scene. LOWER it and "
                 "flatter blocks get tracked: more coverage, noisier vectors.\n\n"
                 "The number is a luma difference on a 0..1 scale, so 0.020 means 2% contrast.\n\n"
                 "Only used when motion is estimated. With a game that hands over real velocity "
                 "vectors this does nothing, which is why it is hidden then.",

                 "O primeiro de dois filtros que decidem quais pixels têm permissão de se mover.\n\n"
                 "Antes de procurar qualquer coisa, o estimador mede a luma mais clara e a mais "
                 "escura no bloco 3x3 em volta do pixel. Se a diferença for menor que este "
                 "número, o pixel é declarado parado e nenhuma busca acontece.\n\n"
                 "Por quê: um bloco chapado -- céu limpo, uma parede pintada -- casa igualmente "
                 "bem em todo deslocamento, então o vencedor é simplesmente o primeiro que o laço "
                 "testou. Isso é o problema da abertura, e é por isso que alargar a busca deixou "
                 "o campo mais doido em vez de melhor. Não há o que rastrear, então não finja que "
                 "há.\n\n"
                 "AUMENTE e mais da tela fica congelada: mais limpo, mas a rede é informada de "
                 "que nada se moveu. 0,020 congelou 99% de uma cena escura do God of War 2. "
                 "DIMINUA e blocos mais chapados passam a ser rastreados: mais cobertura, vetores "
                 "mais ruidosos.\n\n"
                 "O número é uma diferença de luma na escala 0..1, então 0,020 quer dizer 2% de "
                 "contraste.\n\n"
                 "Só é usado quando o movimento é estimado. Num jogo que entrega vetores de "
                 "velocidade de verdade isto não faz nada, por isso fica escondido.");
            Tag(kMeasured);
            v = g.flowRatio.load();
            if (ImGui::SliderFloat(T("Flow Accept Ratio", "Razão de Aceite do Fluxo"), &v, 0.50f,
                                   1.00f, "%.2f", 0))
                g.flowRatio.store(v);
            Help("The second filter, applied after the search instead of before it.\n\n"
                 "The estimator compares the 3x3 block against 81 candidate offsets in the "
                 "previous frame and keeps the one with the lowest error. It also records the "
                 "error of not moving at all. The winner is only believed if its error is below "
                 "the standing-still error times this number.\n\n"
                 "So the slider runs backwards from what you would guess: 1.00 is the most "
                 "PERMISSIVE -- any winner at least as good as standing still is accepted -- and "
                 "0.50 is the strictest, demanding a match that explains the block with half the "
                 "error of not moving. Lower means more of the screen is forced still.\n\n"
                 "Gate rejects a block before searching, on the grounds that there is nothing in "
                 "it. Ratio rejects a result after searching, on the grounds that the answer is "
                 "not convincing. Between them they are what stop the field being noise.\n\n"
                 "The flow probe at frame 300 prints what a pair actually did: look for the "
                 "percentage of blocks reported still.\n\n"
                 "Only used when motion is estimated.",

                 "O segundo filtro, aplicado depois da busca em vez de antes.\n\n"
                 "O estimador compara o bloco 3x3 contra 81 deslocamentos candidatos no quadro "
                 "anterior e fica com o de menor erro. Ele também guarda o erro de não se mover. "
                 "O vencedor só é aceito se o erro dele for menor que o erro de ficar parado "
                 "vezes este número.\n\n"
                 "Ou seja, o slider anda ao contrário do que se imagina: 1.00 é o mais "
                 "PERMISSIVO -- qualquer vencedor tão bom quanto ficar parado é aceito -- e 0.50 "
                 "é o mais rígido, exigindo um casamento que explique o bloco com metade do erro "
                 "de não se mover. Mais baixo significa mais tela forçada a parada.\n\n"
                 "O Portão rejeita um bloco antes de buscar, com o argumento de que não há nada "
                 "nele. A Razão rejeita um resultado depois de buscar, com o argumento de que a "
                 "resposta não convence. Juntos, são o que impede o campo de virar ruído.\n\n"
                 "A sonda de fluxo no quadro 300 imprime o que um par realmente fez: procure a "
                 "porcentagem de blocos reportados parados.\n\n"
                 "Só é usado quando o movimento é estimado.");
            Tag(kMeasured);
        }
    }

    if (ImGui::CollapsingHeader(T("Debug", "Depuração")))
    {
        int dbg = g.debugView.load();
        if (ImGui::Combo(T("Debug View", "Visão de Debug"), &dbg,
                         T("Off\0Network input\0Network output\0Residual x8\0Motion vectors\0Depth x500\0",
                           "Desligado\0Entrada da rede\0Saída da rede\0Resíduo x8\0Vetores de movimento\0Profundidade x500\0")))
        {
            g.debugView.store(dbg);
            Log("menu: debug view %d", dbg);
        }
        Help("Replaces the screen with one stage of the pipeline.\n\n"
             "Network input: what the network was handed, stretched back up. Black here means "
             "nothing downstream can work.\n\n"
             "Network output: what it gave back. Identical to the input means the network is "
             "returning what it was given.\n\n"
             "Residual x8: the correction alone against mid grey. This is the one that answers "
             "'is it doing anything'. Flat grey means it changed nothing; structure that follows "
             "edges and texture means it is working -- and how much of that survives is exactly "
             "what Resolution Scale decides.\n\n"
             "Motion vectors: red is horizontal, green vertical, mid grey is still. Flat grey "
             "while the camera moves means the flow is gated off -- lower Flow Contrast Gate.\n\n"
             "Depth x500: multiplied because PS2 depth peaks around 0.002 and is otherwise solid "
             "black. Overall Intensity scales this view.",

             "Substitui a tela por um estágio do pipeline.\n\n"
             "Entrada da rede: o que foi entregue à rede, esticado de volta. Preto aqui significa "
             "que nada depois disso pode funcionar.\n\n"
             "Saída da rede: o que ela devolveu. Idêntica à entrada significa que a rede está "
             "devolvendo o que recebeu.\n\n"
             "Resíduo x8: a correção sozinha contra cinza médio. Esta é a que responde 'está "
             "fazendo alguma coisa'. Cinza chapado significa que a rede não mudou nada; estrutura "
             "que segue arestas e textura significa que está funcionando -- e quanto disso "
             "sobrevive é exatamente o que a Escala de Resolução decide.\n\n"
             "Vetores de movimento: vermelho é horizontal, verde é vertical, cinza médio é "
             "parado. Cinza chapado com a câmera se movendo significa fluxo bloqueado -- baixe o "
             "Portão de Contraste do Fluxo.\n\n"
             "Profundidade x500: multiplicada porque a profundidade do PS2 chega a uns 0,002 e é "
             "preto puro fora isso. A Intensidade Geral escala esta visão.");
        Tag(kMeasured);

        if (ImGui::Button(T("Measure Residual Again", "Medir Resíduo de Novo")))
        {
            g.measured = false;
            g.measureTries = 0;
            g.measureNow.store(true);
            Log("menu: residual measurement re-armed");
        }
        Help("Writes a 'measure, residual' line to dlss5-neural.log: the size of the correction, "
             "and how much of it follows the image's own detail rather than being a flat shift.\n\n"
             "Change one control, press this, compare the two numbers. It is the only way to tell "
             "a control that does something from one that does not.",

             "Escreve uma linha 'measure, residual' no dlss5-neural.log: o tamanho da correção, e "
             "quanto dela segue o detalhe da própria imagem em vez de ser um deslocamento "
             "chapado.\n\n"
             "Mude um controle, aperte isto, compare os dois números. É o único jeito de separar "
             "um controle que faz algo de um que não faz.");
    }

    if (ImGui::CollapsingHeader(T("Engine", "Motor")))
    {
        ImGui::TextDisabled(T("The engine's own option struct. Every offset below came from "
                              "decompiling the runtime's ini reader, not from guesswork -- but "
                              "an offset being real says nothing about what writing it does. "
                              "The tag after each control says how far that is actually known.",
                              "A struct de opções do próprio motor. Todo offset abaixo veio de "
                              "decompilar o leitor de ini do runtime, não de chute -- mas um "
                              "offset ser real não diz nada sobre o que escrever nele faz. A "
                              "etiqueta depois de cada controle diz até onde isso é sabido."));
        ImGui::TextDisabled(T("Every default here is the engine's own, so an untouched tab "
                              "changes nothing.",
                              "Todo padrão aqui é o do próprio motor, então esta aba intocada "
                              "não muda nada."));
        ImGui::Separator();

        bool mask = g.autoMask.load() != 0;
        if (Risk r(kDanger, !mask);
            ImGui::Checkbox(T("Character Mask", "Máscara de Personagem"), &mask))
        {
            g.autoMask.store(mask ? 1 : 0);
            Log("menu: automask %d", mask ? 1 : 0);
        }
        Help("UseAutoMask, at 8d9e0. The engine's semantic character mask: it is what makes Skin "
             "Structure Strength apply to characters rather than to the whole frame. This is the "
             "same control RenoDX exposes as Character Mask.\n\n"
             "It defaults to 1 and this add-on never wrote it, so it has always been on by "
             "default. Turning it off is a real test: if Skin stops doing even its measured 1.5%, "
             "the mask is what was carrying it.",

             "UseAutoMask, em 8d9e0. A máscara semântica de personagem do motor: é o que faz a "
             "Força de Estrutura na Pele se aplicar a personagens em vez do quadro inteiro. É o "
             "mesmo controle que o RenoDX expõe como Character Mask.\n\n"
             "O padrão é 1 e este add-on nunca escrevia esse campo, então sempre esteve ligado "
             "por omissão. Desligar é um teste de verdade: se a Pele parar de fazer até os 1,5% "
             "medidos, era a máscara que carregava aquilo.");
        Tag(kMeasured);
        if (!mask)
            Note(kDanger, T("Off removes the effect from the whole frame, not just from "
                            "characters -- seen in game. The engine derives its structure and "
                            "tone parameters through this mask, so with it off there is nothing "
                            "left to derive them from. Turn it back on unless you are measuring.",
                            "Desligado tira o efeito do quadro inteiro, não só dos personagens "
                            "-- visto no jogo. O motor deriva os parâmetros de estrutura e tom "
                            "através desta máscara, então com ela desligada não sobra de onde "
                            "derivar. Religue, a não ser que esteja medindo."));

        int tmode = g.temporalMode.load();
        if (ImGui::Combo(T("Temporal", "Temporal"), &tmode,
                         T("Auto (follow the guides)\0Off\0On\0",
                           "Automático (segue os guias)\0Desligado\0Ligado\0")))
        {
            g.temporalMode.store(tmode);
            Log("menu: temporal %d", tmode);
        }
        Help("Temporal accumulation, at 8d9bd. This add-on had the byte labelled 'motion is "
             "valid' -- a guess that turned out wrong. The engine's ini reader reads the key "
             "Temporal into it.\n\n"
             "That explains a measurement nobody could account for: Temporal=1 was the only run "
             "where the engine reported non-zero motion. Not a coincidence -- accumulating over "
             "time is what gives a motion vector something to point at.\n\n"
             "Auto turns it on whenever a motion field exists, which is the old behaviour. Off "
             "and On are explicit, for A/B.",

             "Acumulação temporal, em 8d9bd. Este add-on rotulava esse byte como 'movimento "
             "válido' -- um chute que estava errado. O leitor de ini do motor lê a chave "
             "Temporal para ele.\n\n"
             "Isso explica uma medição que ninguém conseguia justificar: Temporal=1 foi a única "
             "execução em que o motor reportou movimento diferente de zero. Não é coincidência "
             "-- acumular ao longo do tempo é o que dá a um vetor de movimento algo para onde "
             "apontar.\n\n"
             "Automático liga sempre que existe campo de movimento, que é o comportamento "
             "antigo. Desligado e Ligado são explícitos, para A/B.");
        Tag(kTraced);

        int tone = g.tonemap.load();
        if (ImGui::SliderInt(T("Tonemap", "Tonemap"), &tone, -1, 3, "%d", 0))
            g.tonemap.store(tone);
        Help("-1: automatic for the selected encoding. With sRGB, sends 0 (off); other "
             "encodings keep the runtime's automatic detection. 0..3: explicit runtime "
             "override. 1 reproduces the old automatic behaviour on an SDR FP16 input.\n\n"
             "Restart the game after changing this setting.",

             "-1: automático pela codificação escolhida. Em sRGB, envia 0 (desligado); "
             "as outras codificações mantêm a detecção do runtime. 0..3: valor explícito "
             "para o runtime. 1 reproduz o automático antigo na entrada SDR em FP16.\n\n"
             "Reinicie o jogo após alterar esta opção.");
        Tag(kMeasured);

        int ch = g.toneChannels.load();
        if (ImGui::SliderInt(T("Tone Channels", "Canais de Tom"), &ch, 0, 3, "%d", 0))
            g.toneChannels.store(ch);
        Help("ToneChannels, at 8d9e4. An ini key of the engine that nothing in this project knew "
             "existed until the reader was decompiled. Default 0. What it does is unknown -- it "
             "is here to be A/B'd against the residual, like everything else with an unknown "
             "effect.",

             "ToneChannels, em 8d9e4. Uma chave de ini do motor que ninguém neste projeto sabia "
             "que existia até o leitor ser decompilado. Padrão 0. O que ela faz é desconhecido "
             "-- está aqui para ser testada em A/B contra o resíduo, como tudo que tem efeito "
             "desconhecido.");
        Tag(kUnknown);

        float es = g.engineScale.load();
        if (ImGui::SliderFloat(T("Engine Scale", "Escala do Motor"), &es, 0.0f, 1.0f, "%.5f", 0))
            g.engineScale.store(es);
        Help("Scale, at 8d9dc, default 0.03125. This is the 0.031 that showed up in the startup "
             "float dump and was written down as 'a live value we never wrote' -- it is an ini "
             "key of the engine named Scale, and 0.03125 is exactly 1/32.\n\n"
             "Unrelated to Resolution Scale under Performance, which is ours. Nothing is known "
             "about what this one scales. Move it in small steps and watch the residual.",

             "Scale, em 8d9dc, padrão 0.03125. É o 0,031 que apareceu no despejo de floats da "
             "inicialização e foi anotado como 'valor vivo que nunca escrevemos' -- é uma chave "
             "de ini do motor chamada Scale, e 0,03125 é exatamente 1/32.\n\n"
             "Nada a ver com a Escala de Resolução em Desempenho, que é nossa. Nada se sabe "
             "sobre o que esta escala. Mexa em passos pequenos e olhe o resíduo.");
        Tag(kUnknown);
        ImGui::SameLine();
        // A 0..1 slider at five decimals cannot be dragged back onto exactly 1/32, and this is a
        // field nobody knows the effect of -- so leaving it a hair off its default is a way to
        // change the picture and never find out why.
        if (ImGui::SmallButton(T("Reset to 1/32", "Voltar para 1/32")))
        {
            g.engineScale.store(0.03125f);
            Log("menu: engine scale back to the default 0.03125");
        }

        ImGui::Separator();
        ImGui::TextDisabled(T("Model A / B / C: not portable to this runtime. Closed.",
                              "Model A / B / C: não é portável para este runtime. Encerrado."));
        Help("Model A/B/C is real and it does change the picture -- on NVIDIA. It is DLSSNR.Style, "
             "an int at options offset 236 in nvngx_dlssnr.dll. Style 0 is Model A and is the "
             "literal baseline: it has no table entry and overwrites nothing. Style 1 and 2 pick "
             "a row out of an 8x68-byte table and lerp fourteen floats into options+292..+344, "
             "scaled by LocalToneStrength. Three of those fourteen are non-zero across both rows. "
             "It is one weight set, not three networks.~~"
             "It cannot be ported here, and this was chased to the end rather than assumed. The "
             "three constants -0.10, -0.25 and -0.15 do not exist in the AMD binary. The AMD "
             "option block is mapped field by field and no slot is Style. Nothing in that binary "
             "writes a span of fourteen floats. And the HIP kernels on the appearance path are "
             "too small to take them: the style vector is 56 bytes, while k_final_head and "
             "k_post_block each receive a 32-byte struct by value.~~"
             "Whoever did the port compiled the network with the neutral style baked in. The "
             "kernels are precompiled GCN code objects inside the DLL with no source, so adding "
             "the fields means recompiling them. Model A is the only one that exists on this "
             "side. Not reopening it.",

             "Model A/B/C é real e muda a imagem sim -- na NVIDIA. É o DLSSNR.Style, um int no "
             "offset 236 da struct de opções do nvngx_dlssnr.dll. Style 0 é o Model A e é o "
             "baseline literal: não tem entrada na tabela e não sobrescreve nada. Style 1 e 2 "
             "pegam uma linha de uma tabela de 8x68 bytes e interpolam catorze floats para "
             "options+292..+344, escalados pelo LocalToneStrength. Três desses catorze são "
             "diferentes de zero nas duas linhas. É um conjunto de pesos só, não três redes.~~"
             "Não dá para portar para cá, e isso foi perseguido até o fim, não assumido. As três "
             "constantes -0,10, -0,25 e -0,15 não existem no binário AMD. O bloco de opções do "
             "AMD está mapeado campo a campo e nenhum slot é Style. Nada naquele binário escreve "
             "uma sequência de catorze floats. E os kernels HIP do caminho de aparência não têm "
             "espaço: o vetor de style são 56 bytes, e o k_final_head e o k_post_block recebem um "
             "struct de 32 bytes por valor.~~"
             "Quem portou compilou a rede com o style neutro embutido. Os kernels são code "
             "objects GCN pré-compilados dentro da DLL, sem fonte, então acrescentar os campos "
             "significa recompilar os kernels. Model A é o único que existe deste lado. Não "
             "vamos reabrir.");
    }

    if (ImGui::CollapsingHeader(T("Advanced", "Avançado")))
    {
        float v = g.tone.load();
        if (ImGui::SliderFloat(T("Local Tone Strength", "Força de Tom Local"), &v, 0.0f, 3.0f,
                               "%.2f", 0))
            g.tone.store(v);
        Help("Measured inert. A full sweep produced numbers byte-for-byte identical to the "
             "baseline, which matches the RenoDX note that Global Tone is not visible on the "
             "recovered NGX path. It is here to be re-checked on a different target, not to be "
             "used. Left at 1.",

             "Medido inerte. Uma varredura completa produziu números byte a byte idênticos ao "
             "baseline, o que bate com a nota do RenoDX de que Global Tone não é visível no "
             "caminho NGX recuperado. Está aqui para ser reconferido num alvo diferente, não "
             "para ser usado. Deixado em 1.");
        Tag(kInert);

        ImGui::Text(T("Engine offsets written: 8d9d0 tone, 8d9d4 structure, 8d9d8 skin",
                      "Offsets escritos no motor: 8d9d0 tom, 8d9d4 estrutura, 8d9d8 pele"));
        Help("These three are hardcoded RVAs into one specific build of the runtime, found by "
             "matching strings in the binary. That the binary contains the words does not prove "
             "it reads these fields -- an offset written but never read looks identical from out "
             "here. The log dumps the float window around them at startup.",

             "Estes três são RVAs hardcoded numa build específica do runtime, achados casando "
             "strings no binário. Que o binário contenha as palavras não prova que ele lê estes "
             "campos -- um offset escrito e nunca lido é idêntico visto daqui. O log despeja a "
             "janela de floats em volta deles na inicialização.");

        ImGui::Separator();
        ImGui::Text(T("Restart-only: Stage=%d  Events=%d  NoBridge=%d  NoBackBuffer=%d",
                      "Só na reinicialização: Stage=%d  Events=%d  NoBridge=%d  NoBackBuffer=%d"),
                    g.stage.load(), g.events, g.noBridge.load() ? 1 : 0,
                    g.noBackBuffer.load() ? 1 : 0);
        Help("Diagnostics that decide what gets built at startup, so they cannot be changed live "
             "-- set them in dlss5-neural.ini.\n\n"
             "Stage=1 stops before D3D12 loads, 2 before the engine, 3 is everything. Events is a "
             "bitmask: 1 bind, 2 draw, 4 clear, 8 destroy_swapchain, 16 overlay. These are what "
             "found the DXGI resize failure.",

             "Diagnósticos que decidem o que é construído na inicialização, então não podem ser "
             "mudados ao vivo -- ajuste no dlss5-neural.ini.\n\n"
             "Stage=1 para antes da D3D12 carregar, 2 antes do motor, 3 é tudo. Events é uma "
             "máscara de bits: 1 bind, 2 draw, 4 clear, 8 destroy_swapchain, 16 overlay. Foram "
             "eles que acharam a falha de resize da DXGI.");
    }

    // Experimental. Things that work, were measured on one route, and are not the shipped
    // arrangement. New entries go above Network Output, which stays at the bottom.
    if (ImGui::CollapsingHeader(T("Experimental", "Experimental")))
    {
        ImGui::TextDisabled(T("Proof of concept. Measured on one route each, and not the shipped "
                              "arrangement.",
                              "Prova de conceito. Medidos numa rota cada, e não são o arranjo "
                              "padrão."));
        ImGui::Separator();

        bool raw = g.networkOutput.load();
        if (ImGui::Checkbox(T("Network Output (bypass composition) -- proof of concept",
                              "Saída da Rede (ignora a composição) -- prova de conceito"), &raw))
        {
            g.networkOutput.store(raw);
            Log("menu: network output mode %d", raw ? 1 : 0);
        }
        Help(
            "A PROOF OF CONCEPT. It is here because it was preferred by eye in one game, not "
            "because it is finished or because it is known to be better. Treat it as something "
            "to try and report on, not as a setting to leave on and forget.\n\n"
            "Show the network's answer directly instead of composing it onto the game's frame.\n\n"
            "WHAT IT TURNS OFF. There is no residual in this mode, so Highlight Guard, Colour "
            "Strength, Residual Limit and Edge Fade all do nothing. Nothing bounds how far a "
            "pixel may move, and hue is whatever the network returned. That is the trade.\n\n"
            "WHERE IT COMES FROM. Tested on D3D12, in GTA V Enhanced. There the ratio "
            "composition showed a heavy trail behind everything while driving. The cause was "
            "measured: the network costs about 29 ms at full Resolution Scale and the game "
            "presents faster, so 37% of frames (13,921 of 37,584) were skipped with the previous "
            "evaluation still on the GPU -- and the correction being pasted on belonged to a "
            "picture that had already moved. This mode has no correction to misplace, so the "
            "trail cannot happen.\n\n"
            "ON A SKIPPED FRAME. You see the previous network output, a whole picture, which "
            "reads as a held frame rather than as a trail. It is deliberately not gated on "
            "freshness: gating it would flicker between two different pictures instead.\n\n"
            "WHAT IS NOT KNOWN. It was preferred by eye on one game, on one route, at Pass Count "
            "2 and full Resolution Scale. Nothing here has measured it against the composition on "
            "a slow scene, in HDR, or on the D3D11 and Vulkan routes.",

            "UMA PROVA DE CONCEITO. Está aqui porque foi preferido a olho em um jogo, não porque "
            "esteja pronto nem porque se saiba que é melhor. Trate como algo para experimentar e "
            "relatar, não como ajuste para deixar ligado e esquecer.\n\n"
            "Mostra a resposta da rede direto, em vez de compô-la sobre o quadro do jogo.\n\n"
            "O QUE ISTO DESLIGA. Não existe resíduo neste modo, então Trava de Realce, Força da "
            "Cor, Limite do Resíduo e Esmaecimento de Borda não fazem nada. Nada limita o quanto "
            "um pixel pode andar, e o matiz é o que a rede devolveu. Essa é a troca.\n\n"
            "DE ONDE VEIO. Testado em D3D12, no GTA V Enhanced. Lá a composição por razão deixava "
            "um rastro pesado atrás de tudo ao dirigir. A causa foi medida: a rede custa cerca de "
            "29 ms na Escala de Resolução cheia e o jogo apresenta mais rápido, então 37% dos "
            "quadros (13.921 de 37.584) foram pulados com a avaliação anterior ainda na GPU -- e "
            "a correção colada vinha de uma imagem que já tinha andado. Este modo não tem "
            "correção para colar no lugar errado, então o rastro não acontece.\n\n"
            "NUM QUADRO PULADO. Você vê a saída anterior da rede, uma imagem inteira, que lê como "
            "quadro segurado e não como rastro. É de propósito que ele não é cortado por "
            "atualidade: cortar faria piscar entre duas imagens diferentes.\n\n"
            "O QUE NÃO SE SABE. Foi preferido a olho em um jogo, numa rota, com Número de Passes "
            "2 e Escala de Resolução cheia. Nada aqui mediu ele contra a composição em cena lenta, "
            "em HDR, nem nas rotas D3D11 e Vulkan.");
    }

    if (ImGui::CollapsingHeader(T("Status", "Estado"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        const Profile &profile = ProfileForThisProcess();
        std::lock_guard guard(g.lock);
        const uint64_t seen = g.frame + g.skipped;
        const double pct = seen != 0 ? 100.0 * static_cast<double>(g.skipped) / seen : 0.0;
        // A skipped frame reuses whatever the network textures hold, and a job still running is
        // writing them while compose reads them. A few percent is invisible; a third of the
        // frames is a correction that changes every frame, which reads as flicker.
        if (seen > 300 && pct >= 10.0)
            Note(kWarn, T("High skip rate. The network is not finishing inside a frame, so what "
                          "is on screen is stale or half-written and changes frame to frame -- "
                          "that is the flicker. Lower Resolution Scale, set Pass Count to 1, and "
                          "lower the emulator's own upscale multiplier: it competes for the same "
                          "GPU.",
                          "Taxa de pulo alta. A rede não está terminando dentro de um quadro, "
                          "então o que está na tela é velho ou escrito pela metade e muda de "
                          "quadro a quadro -- é isso o piscar. Baixe a Escala de Resolução, ponha "
                          "o Número de Passes em 1, e baixe o multiplicador de upscale do próprio "
                          "emulador: ele disputa a mesma GPU."));
        ImGui::Text(T("Target: %s", "Alvo: %s"), profile.note);
        if (g.outWidth != 0)
            ImGui::Text(T("Back buffer %ux%u  ->  network %ux%u",
                          "Back buffer %ux%u  ->  rede %ux%u"),
                        g.outWidth, g.outHeight, g.netWidth, g.netHeight);
        if (g.depthBest != nullptr)
            ImGui::Text(T("Depth candidate: %ux%u format %d, %llu binds%s",
                          "Candidato a profundidade: %ux%u formato %d, %llu binds%s"),
                        g.depthWidth, g.depthHeight, static_cast<int>(g.depthFormat),
                        static_cast<unsigned long long>(g.depthBinds),
                        g.useDepth.load() ? T(", feeding it", ", alimentando")
                                          : T(" (Depth switch is off)",
                                              " (chave de Profundidade desligada)"));
        else if (g.depthEvents.load() == 0)
            ImGui::TextDisabled(T("Depth: no depth-stencil bind delivered on this API.",
                                  "Profundidade: nenhum bind de depth-stencil entregue nesta API."));
        else
            ImGui::TextDisabled(T("Depth: %llu binds seen, none usable.",
                                  "Profundidade: %llu binds vistos, nenhum usável."),
                                static_cast<unsigned long long>(g.depthEvents.load()));
        ImGui::TextDisabled(T("Log: dlss5-neural.log", "Log: dlss5-neural.log"));
    }

    ImGui::Separator();
    ImGui::TextColored(kDanger, T("Red", "Vermelho"));
    ImGui::SameLine();
    ImGui::TextWrapped(T("- this setting, at the value it is holding right now, can take the "
                         "display driver down with it. The game dies on DXGI_ERROR_DEVICE_REMOVED "
                         "and the desktop goes with it, with nothing in any log pointing back "
                         "here. Change it before you go further.",

                         "- este ajuste, no valor em que está agora, pode levar o driver de vídeo "
                         "junto. O jogo morre com DXGI_ERROR_DEVICE_REMOVED e o desktop vai "
                         "junto, sem nada em log nenhum apontando de volta para cá. Mude antes de "
                         "seguir."));
    ImGui::TextColored(kWarn, T("Amber", "Âmbar"));
    ImGui::SameLine();
    ImGui::TextWrapped(T("- past what has actually been measured on this machine. Not known to "
                         "break, not known to work either. Change one thing at a time, save "
                         "first, and watch the skip rate under Status: if it climbs, back off.",

                         "- passou do que realmente foi medido nesta máquina. Não se sabe que "
                         "quebra, nem que funciona. Mude uma coisa por vez, salve antes, e olhe a "
                         "taxa de pulo em Estado: se subir, recue."));
    ImGui::TextDisabled(T("A control is coloured only for the value it currently holds, so "
                          "turning it back down clears it. Hover any (?) for what a control does "
                          "and what was measured about it.",

                          "Um controle só fica colorido pelo valor que está segurando, então "
                          "baixar o valor limpa a cor. Passe o mouse em qualquer (?) para o que "
                          "um controle faz e o que foi medido sobre ele."));
    // The other legend. "Which of these actually does something in the game" was a question you
    // could only answer by reading the source, so the answer is printed next to each control and
    // spelled out once here.
    ImGui::TextDisabled(T("Tags:", "Etiquetas:"));
    ImGui::SameLine();
    Tag(kMeasured);
    ImGui::SameLine();
    ImGui::TextDisabled(T("a residual reading moved when it changed.", "uma leitura de resíduo "
                          "mudou quando ele mudou."));
    ImGui::TextDisabled(" ");
    ImGui::SameLine();
    Tag(kTraced);
    ImGui::SameLine();
    ImGui::TextDisabled(T("reaches a consumer, never separated from its default here.",
                          "chega num consumidor, mas nunca foi separado do padrão dele aqui."));
    ImGui::TextDisabled(" ");
    ImGui::SameLine();
    Tag(kUnknown);
    ImGui::SameLine();
    ImGui::TextDisabled(T("a real engine field whose effect nobody here has established.",
                          "um campo real do motor cujo efeito ninguém aqui estabeleceu."));
    ImGui::TextDisabled(" ");
    ImGui::SameLine();
    Tag(kInert);
    ImGui::SameLine();
    ImGui::TextDisabled(T("swept and measured to change nothing.",
                          "varrido e medido como não mudando nada."));

    ImGui::Separator();
    if (ImGui::Button(T("Save Settings", "Salvar Ajustes")))
        SaveSettings();
    Help("Writes everything above to dlss5-neural.ini next to the exe, so it survives a restart.\n\n"
         "Without this the overlay is a scratchpad: every A/B test meant re-dialling half a dozen "
         "controls by hand on the next run.",

         "Escreve tudo acima no dlss5-neural.ini ao lado do exe, para sobreviver a um restart.\n\n"
         "Sem isto o overlay é um rascunho: cada teste A/B significava re-ajustar meia dúzia de "
         "controles na mão na execução seguinte.");
    ImGui::SameLine();
    if (ImGui::Button(T("Reload Settings", "Recarregar Ajustes")))
    {
        LoadSettings();
        Log("menu: settings reloaded from the ini");
    }
    Help("Re-reads dlss5-neural.ini, discarding anything changed here since the last save.",
         "Relê o dlss5-neural.ini, descartando qualquer coisa mudada aqui desde o último salvamento.");
}

}

extern "C" __declspec(dllexport) const char *NAME = "dlss5 neural";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Runs DLSS-NR on AMD with HIP 7. D3D11/D3D12"
#if DLSS5_WITH_VULKAN
    " and experimental Vulkan"
#endif
    ". SDR and serialized inline multipass preview.";

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
            Log("preview 2026-09-10: SDR input contract, serialized inline passes; Vulkan %d",
                DLSS5_WITH_VULKAN);
            // Before the read, so a first run has a documented file to read and the user has
            // something to edit without being told which keys exist.
            EnsureNeuralIni();
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
#if DLSS5_WITH_VULKAN
        // Has to happen here and not at the first present: the host's VkDevice is created when a
        // game boots, and by the time a frame is presented it is far too late to change what that
        // device was created with. Patches one import-table entry and does nothing at all in a
        // process that has no static vkCreateDevice import, which is every D3D11 and D3D12 target.
        if (!g.noBridge.load())
            vkroute::devicehook::Install();
#endif
        break;
    case DLL_PROCESS_DETACH:
#if DLSS5_WITH_VULKAN
        vkroute::devicehook::Remove();
#endif
        if (g.events & 16)
            reshade::unregister_overlay("DLSS Neural Rendering (AMD)", OnOverlay);
        reshade::unregister_addon(module);
        if (g_log != nullptr)
            fclose(g_log), g_log = nullptr;
        break;
    }
    return TRUE;
}
