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
#include "hotkey_capture.h"
#include "ini_text.h"
#include "log_export.h"
#include "../ui/panel.h"
#include "../core/shaders/motion.h"
#include "../core/shaders/compose.h"
#include "../core/shaders/input.h"
#include "../ui/panel_model.h"
#include "../ui/view_logic.h"
#if AMDNR_WITH_VULKAN
#include <MinHook.h>
#include "../vkshared/vk_raw.inc"
#endif

#include <windows.h>
#include <shlobj.h>

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

    { L"RDR2.exe", Tier::C, 1.0f,
      "Red Dead Redemption 2" },

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
    const std::filesystem::path dir = ExeDirectory() / L"amd-nr-runtime";

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

// Every address written into the runtime lives in this header, and nowhere else. It used to be
// four copies across neural.cpp, vk_route.inc, host64.cpp and framecheck.cpp; a version bump
// updated two of them and the 32-bit bridge crashed on its first frame.
#include "runtime_offsets.h"

// v0.3.0 of DLSS-NR-on-AMD, lifted out of its setup by tools/extract_runtime.py and run
// through tools/patch_runtime.py -- this is the hash of the patched file, which is what
// the add-on loads. Every offset below was re-derived against this build. Nothing moved by
// a constant: the .data globals shifted by 0xa080 near the device pointer, 0xa0e0 across the
// inline block, 0xa158 across the job block and 0xa160 across the option struct, because
// v0.3.0 inserts new globals between them. Older builds are refused by hash rather than
// written into with the wrong addresses.
constexpr unsigned char kRuntimeSha256[32] = { 0x70, 0xaf, 0x3f, 0xb7, 0x57, 0xf8, 0x3f, 0x71,
                                               0xec, 0x94, 0x7c, 0xe4, 0x61, 0x97, 0x0f, 0xde,
                                               0xcc, 0x96, 0x36, 0x86, 0x4b, 0xc0, 0x1d, 0x95,
                                               0x2a, 0xbf, 0xfb, 0x36, 0xae, 0x31, 0x0b, 0xe6 };
constexpr size_t kRuntimeSize = 7290880;

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
    // Presents counted before the first guide is taken at all. Separate from the challenger
    // streak because the cold start is a different question: see SettleGuide.
    UINT coldFrames = 0;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    // Set when the companion effect supplied this guide instead of the bind observation. The
    // tally then has nothing to say about it: a ReShade effect texture is never bound as a
    // render target the add-on can see, so SettleGuide would walk the slot straight back onto
    // whatever the game happened to draw into most.
    bool external = false;

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

// The same idea for D3D12 depth, which had none: it picked by size alone. Separate because these
// are ID3D12Resource and because depth here is decided on clears as well as binds -- the buffer
// the game clears every frame is the one it draws the scene into.
// No guide buffer worth having is smaller than this. It exists because the relative floors below
// are measured against a swapchain size that is zero until the effect has been switched on once --
// a Darksiders 3 log has "guide motion: taking 1x1 format 16, bound 2928 times this frame", taken
// in exactly that window, with CreateTexture2D failing on it the next line.
constexpr UINT kGuideFloor = 256;

struct D12Depth
{
    ComPtr<ID3D12Resource> res;
    UINT binds = 0, clears = 0;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
std::unordered_map<void *, D12Depth> g_d12DepthTally;
UINT g_d12DepthCold = 0;

// One candidate's entry, created on first sight. Binds and clears both come through here, because
// a clear is evidence in its own right: an engine is allowed to clear a buffer it has not bound
// yet this present, and counting that only when the resource happened to be bound first left the
// buffer the game clears every frame looking like one it never clears.
D12Depth &TallyD12Depth(ID3D12Resource *native, const D3D12_RESOURCE_DESC &d)
{
    D12Depth &slot = g_d12DepthTally[native];
    if (slot.res == nullptr)
    {
        slot.res = native;  // ComPtr: takes a reference
        slot.width = static_cast<UINT>(d.Width);
        slot.height = d.Height;
        slot.format = d.Format;
    }
    return slot;
}

// Screen-shaped: the aspect within 3% of the swapchain's, and at least a ninth of its area.
//
// This is what "largest wins" was missing. A 2048x2048 R16 shadow map is larger than the scene
// depth of a game rendering 1129x706 into a 1920x1200 swapchain, and it won every time. An
// unknown swapchain size decides nothing rather than accepting everything, which is how a 1x1
// buffer used to get through the floor before the raster was known.
bool ScreenShaped(UINT64 w, UINT h, UINT screenW, UINT screenH)
{
    if (w < kGuideFloor || h < kGuideFloor)
        return false;
    // The swapchain size is only known once the effect has been enabled once, and the default is
    // to start switched off. Rejecting everything until then would mean a fresh install never
    // finds a guide at all, which is the very thing the observation code says it exists to avoid.
    // The floor above is what keeps a 1x1 out; shape can only be judged once there is a shape to
    // judge against.
    if (screenW == 0 || screenH == 0)
        return true;
    const double aspect = static_cast<double>(w) / static_cast<double>(h);
    const double screen = static_cast<double>(screenW) / static_cast<double>(screenH);
    if (std::abs(aspect - screen) > 0.03 * screen)
        return false;
    return static_cast<double>(w) * static_cast<double>(h) >=
           static_cast<double>(screenW) * static_cast<double>(screenH) / 9.0;
}


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

    // Nothing chosen yet: add three presents up, then take the leader.
    //
    // The streak rule below exists to protect an incumbent, and with no incumbent there is
    // nothing to protect -- only the question of having seen enough. Asking one candidate to win
    // three presents *running* is a rule this case cannot always satisfy: an engine that rotates
    // two or three depth targets never presents the same one three times in a row, so the
    // challenger changed every present, the streak reset every present, and the guide was never
    // taken at all. No depth, in a game that has depth, for as long as it runs. Measured against
    // tools/guide_switch_check.py: a rotating pair leaves the slot empty after a hundred presents.
    //
    // So the tally is left standing rather than cleared, and three presents of binds add up
    // before the leader is taken. Rotating targets each keep their own share and one of them
    // wins; the scene pass still outbinds a shadow map by an order of magnitude; and a single odd
    // frame still cannot decide it alone.
    const bool cold = guide.chosen == nullptr;
    if (cold)
    {
        if (++guide.coldFrames < 3)
            return;  // deliberately NOT cleared -- leaving it standing is what accumulates
        guide.coldFrames = 0;
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
    else if (guide.challenger != best->res.Get())
    {
        guide.challenger = best->res.Get();
        guide.challengerFrames = 1;
        tally.clear();
        return;
    }
    else if (++guide.challengerFrames < 3)
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
    // Whatever the companion effect had here, the game has just outbid it with a buffer it
    // renders itself. That is not a guess at motion, so it wins.
    guide.external = false;
    Log("guide %s: taking %ux%u format %u, bound %u times %s", guide.name, best->width,
        best->height, static_cast<unsigned>(best->format), best->binds,
        cold ? "over the first three presents" : "a frame for three frames running");
    tally.clear();
}

// The optional-control bits (ui::Opt), the section hues, T and the widgets are the panel's, in
// src/ui/, shared with the 32-bit bridge. Unqualified here so the code that reads them reads the
// same as it did when they were defined in this file.
using namespace ui;

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
    // -1, not 1. It is the value the engine boots with and it means "derive it from local
    // structure" -- a mode, not a strength. Shipping 1.0 here wrote that automatic off on
    // startup, before anybody had touched a control, while the overlay's own tooltip said the
    // add-on had stopped doing exactly that.
    std::atomic<float> skin { -1.0f };
    std::atomic<int> passes { 1 };
    std::atomic<bool> serialPasses { true };
    // 0 English, 1 Portugues do Brasil. English by default.
    std::atomic<int> language { 0 };
    // A bit per optional control, saying whether the panel draws a widget for it. Nothing else:
    // every one of them is live and settable from the ini whether its bit is set or not. The
    // cascade at the bottom of the panel is what turns them on, one at a time, so a panel grows
    // by what somebody asked for rather than by everything that exists. See enum Opt.
    std::atomic<uint32_t> optional { 0 };
    // The engine's own option struct, mapped by decompiling its ini reader rather than guessed:
    //   97b30 LocalTone (0.0)   97b34 LocalStructure (1.0)   97b38 SkinStructure (-1.0)
    //   97b3c Scale (0.03125)   97b40 UseAutoMask (1)        97b44 ToneChannels (0)
    //   97b1c Enabled  97b1d Temporal  97b1e UseFsrInputs  97b1f UseDepth  97b20 Tonemap (-1)
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
    // Neural Rendering Model A (0), B (1) or C (2). A is the neutral vector and what every
    // release so far has drawn, so 0 is the default and selecting it changes nothing.
    std::atomic<int> style { 0 };
    // DLSSNR scales a style's coefficients by LocalToneStrength, clamped to [0,1], as
    // (value - neutral) * t + neutral. Same knob, same range, so a style can be taken at part
    // strength instead of only on or off.
    std::atomic<float> styleStrength { 1.0f };
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
    // One runtime module per pass, runtimes[0] == runtime. The runtime keeps its temporal state
    // -- history reprojection, auto exposure, the post-network history it blends -- in module
    // globals and in one HIP engine object, so two passes through one module hand each pass
    // the other stage's previous frame as "last frame": measured as lighting noise that history
    // removes at one pass and stops removing at two. The reference (OptiScaler) holds one NGX
    // feature per pass for the same reason. A copy of the DLL under another file name is a
    // separate module with separate globals, which is all a second feature is here.
    HMODULE runtimes[kMaxPasses] {};
    UINT lastJobs[kMaxPasses] {};
    // Bit i set when runtimes[i] recorded onto the list about to be submitted; NotifyRuntimes
    // tells exactly those modules and clears it.
    UINT recordedMask = 0;
    std::filesystem::path runtimeFile;  // the file runtime was loaded from; the copies come from it
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
    // One history per pass, not one for the chain. A pass's history has to be the output of
    // *that* pass on the previous frame: the denoiser blends its input against the reprojected
    // history, and pass 2's input is pass 1's output, which is a different image from the one
    // the chain ended on. Handing every pass the chain's final output means pass 1 and pass 2
    // are both given a reference that matches neither of their inputs, the temporal term never
    // agrees with the spatial one, and the accumulation does not converge -- measured in game
    // as lighting noise that history removes at one pass and stops removing at two.
    //
    // The reference fork reaches the same arrangement from the other end: each of its passes
    // holds its own NGX feature, and an NGX feature carries its own history.
    ComPtr<ID3D12Resource> history[kMaxPasses];
    std::atomic<bool> useHistory { true };
    // Bit i is set when history[i] holds pass i's output from the previous frame. A bitmask
    // rather than a flag because the passes fill in one at a time: on the first frame of a
    // three-pass chain, pass 1 has a history and passes 2 and 3 do not, and handing a pass a
    // texture that was never written is the stale-reference problem this whole comment is about.
    //
    // Cleared by the overlay's History checkbox and read and set by present. The overlay only
    // takes g.lock for its Status section at the bottom, and the checkbox is above that, so the
    // two threads share no lock here. Atomic, like the switches beside it.
    std::atomic<uint32_t> historyValid { 0 };
    bool loggedHistory = false;
    bool loggedEffectsFirst = false;
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
    // 120, not 600: the probe is what measures the depth range, and the depth range is what the
    // guide is scaled by. The automatic `measure, residual` fires at 240, so a probe at 600 meant
    // the one measurement of what the network did was always taken before the guide was right.
    // The JUNK and FLAT guards are what make an early look safe; if the guides are not real yet
    // it re-arms and tries again.
    uint64_t nextGuideProbe = 120;
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
    // The same measurement, on the path the network actually reads. The probe was already
    // computing 1/max for the debug view and throwing it away everywhere else, so the guide the
    // network got was the raw buffer: on PCSX2 that is 0..0.002, which is 0.2% of the range and
    // flat as far as the network is concerned. Left at 1.0 until something has been measured,
    // and only moved when the measured range is far enough below full that it is a defect
    // rather than a scene -- a modern engine fills the range and keeps 1.0.
    // Off. It shipped on, and its own help text said it reads as the wrong operation on this
    // bench: PCSX2's depth already has its bulk at the top of its own tiny range, so scaling
    // by 1/max lands nearly every pixel at 0.99 rather than spreading anything out. A control
    // the overlay painted amber for being past what was measured has no business being the
    // default. Still settable as DepthNormalise in the ini.
    std::atomic<bool> depthNormalise { false };
    std::atomic<float> depthScale { 1.0f };
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
    // The companion effect, shaders/AMD_Neural_Feed.fx, when the user has installed it. It
    // hands over a real optical-flow field -- iMMERSE Launchpad runs an eight-level pyramid,
    // against the two levels and radius of four this add-on can afford next to the network --
    // and ReShade's own depth buffer, which is curated per game in a way the bind observation
    // here cannot be. Both are only read at present, after ReShade has finished writing them.
    reshade::api::effect_runtime *effects = nullptr;
    std::atomic<bool> useFeedEffect { true };
    // True between reshade_begin_effects and reshade_finish_effects: the window in which the
    // render targets being bound belong to ReShade's shaders and not to the game.
    std::atomic<bool> inEffects { false };
    char feedStatus[192] = "";
    int feedSignature = -1;
    // Diagnostic. Runs the entire bridge but never touches the swapchain image, which is the
    // only way to tell a back-buffer reference apart from anything else the add-on does to the
    // device. Picture is untouched with this on; it is not a usable mode.
    std::atomic<bool> noBackBuffer { false };
    // OpenGL only, and read-only from the ini like the rest of that family. The route hands over
    // between the two APIs with the imported D3D12 fences when the driver has them; setting this
    // to 0 puts it back on the CPU stall the other routes use, which is the only way to compare
    // the two on one machine and the first thing to try if a GL host misbehaves.
    std::atomic<bool> glSemaphores { true };
    // OpenGL only. How many frames in a row may repeat the last result when the game presents
    // faster than the network answers. Zero -- the default -- means never: the route waits for
    // the network instead, so every frame that reaches the screen is a new one and the frame
    // counter the player sees counts frames they can actually see. Above zero trades that for a
    // higher present rate made partly of duplicates, which is a real choice on a
    // variable-refresh display and a misleading number everywhere else.
    std::atomic<int> glHoldFrames { 0 };
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
    // 97b10 DepthInverted. 1 is both runtimes' own default; RenoDX writes 0 explicitly on its
    // Present route (ETS2 trace, where its depth was a dummy, so that 0 says nothing about any
    // game's real buffer). Exposed so the two can be told apart on a game with real depth; no run
    // here has yet.
    std::atomic<int> depthInverted { 1 };
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
    // Set by the residual measurement when the network handed back its input: the effect is
    // running and changing nothing. Shown in red on the status line and re-checked every 1800
    // frames, because this has happened more than once, for more than one reason, and every
    // time it was found by eye instead of by the add-on.
    std::atomic<bool> inert { false };
    bool pendingMeasure = false;

    uint64_t frame = 0;
    uint64_t skipped = 0;
    UINT64 lastJobAt = 0;
    // A job that is running right now, and how the network's real cost is kept. A dispatch that
    // takes seconds is not slow, it is a display-driver reset waiting to happen -- see
    // NoteJobCost. scaleCap is 0 when the person's own Scale is being honoured in full.
    bool jobRunning = false;
    std::atomic<float> scaleCap { 0.0f };
    std::atomic<UINT64> worstJobMs { 0 };
    UINT longJobs = 0;
    UINT junkProbes = 0;
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

// Defined further down, beside the raster code they belong to; used from both present paths,
// which come first.
float EffectiveScale();
void NoteJobCost(UINT64 ms);

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
bool StyleCoefficients(int style, float strength, float &expo, float &con, float &sat);
float StyleGradeStrength();

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
    if (ranNetwork && g.activePasses != 0)
        return true;
    // A style is colour grading on the finished frame, and it does not go stale: it is a function
    // of the pixel in front of it, not of anything the network said. Gating it with the correction
    // meant the grade came and went with the skip rate -- Model B's frame, then the game's own
    // frame, then Model B's again -- which is a flicker in the one thing that is supposed to be
    // constant. NVIDIA has no equivalent of a skipped frame here: its grading is inside the
    // evaluate, so every frame it shows carries it.
    //
    // The correction is what is dropped on these frames, not the compose: RecordNetwork zeroes the
    // residual for them, so what gets pasted is the game's own picture with the grade on it and
    // nothing aimed at where the edges used to be.
    float expo = 0.0f, con = 0.0f, sat = 0.0f;
    return StyleCoefficients(g.style.load(), StyleGradeStrength(), expo, con, sat);
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
void LoadSettings();
// Defined with the other composition helpers, below; wanted in LoadSettings only so the log can
// report which model was selected.
bool StyleCoefficients(int style, float strength, float &expo, float &con, float &sat);
void SaveSettings(bool quiet = false);

// Returns whether it read the settings itself, which it does only on the run that writes the file.
// The caller uses that to skip a second read: on a first run this used to parse the ini twice and
// print the same two "settings:" and "compose:" lines twice, describing one state.
// v0.6.5 renamed the add-on's files. Somebody upgrading has a dlss5-neural.ini they spent time
// on, and a rename that silently resets every setting to default is a worse first impression than
// any rebrand is worth -- so the old file is carried over once, section header and all, and left
// in place rather than deleted. If both exist the new one wins and nothing is touched.
//
// ponytail: a copy and one string replace. The keys did not change, only the section they sit in
// and the name of the file holding them.
void MigrateLegacyIni()
{
    const auto here = ExeDirectory();
    const auto now = here / L"amd-nr.ini", was = here / L"dlss5-neural.ini";
    std::error_code ec;
    if (std::filesystem::exists(now, ec) || !std::filesystem::exists(was, ec))
        return;

    std::ifstream in(was, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (text.empty())
        return;
    if (const size_t at = text.find("[dlss5]"); at != std::string::npos)
        text.replace(at, 7, "[amd-nr]");

    std::ofstream out(now, std::ios::binary);
    if (!out)
        return;
    out << text;
    out.close();
    Log("carried dlss5-neural.ini over to amd-nr.ini; the old file is left where it was.");
}

bool EnsureNeuralIni()
{
    MigrateLegacyIni();
    const auto ini = ExeDirectory() / L"amd-nr.ini";
    std::error_code ec;
    if (std::filesystem::exists(ini, ec))
        return false;

    std::ofstream f(ini, std::ios::binary);
    if (!f)
    {
        Log("could not write %ls; the built-in defaults are used instead.", ini.c_str());
        return false;
    }
    f << "[amd-nr]\r\n"
         "; Written because no amd-nr.ini was here. Every value below is the default, so\r\n"
         "; this file changes nothing until you edit it. The overlay writes back here on its own,\r\n"
         "; as soon as a control settles; the Save button does the same thing on demand.\r\n"
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
         "; click the key button and press the combination you want; it is kept on its own.\r\n"
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
         "; Everything else the overlay carries follows, each at its default, so this file on\r\n"
         "; its own is enough to tune the add-on with the overlay never opened -- which is what\r\n"
         "; a game running under Lossless Scaling or Magpie needs, because there the overlay\r\n"
         "; sits behind somebody else's picture -- and because the overlay deliberately shows\r\n"
         "; only the fifteen controls worth reaching for. Everything else lives here and nowhere\r\n"
         "; else: the engine's option struct, the guide switches, the per-pass profiles, the\r\n"
         "; composition bounds. Press Reload in the overlay to pick an edit up without\r\n"
         "; restarting the game.\r\n"
         "; Intensity leads because it is the one people reach for: the weight of the whole\r\n"
         "; effect, 1 being the network at full strength.\r\n"
         "Intensity=1\r\n"
         "\r\n"
         "; --- Neural Rendering Model ---------------------------------------------------\r\n"
         "; 0 = Model A, 1 = Model B, 2 = Model C, the same three DLSSNR.Style selects on\r\n"
         "; NVIDIA. Deep Fried Chicken names the same three Default, Natural and Cinematic,\r\n"
         "; in that order, so Natural is 1 and Cinematic is 2.\r\n"
         "; On NVIDIA a model is two things: an input of the network (style/128, which is\r\n"
         "; what changes lighting and detail there) and a grade on the finished frame. This\r\n"
         "; runtime has no slot for the first: its network takes tone, structure and the two\r\n"
         "; derived skin/structure values, and nothing else. So here a model is its grade\r\n"
         "; only: B darkens by 0.1 stop, flattens contrast a quarter off its S-curve and\r\n"
         "; takes a tenth of the saturation; C only takes 15 percent of the saturation, both\r\n"
         "; scaled by Tone clamped to 0..1, constants read out of nvngx_dlssnr.dll.\r\n"
         "; NR Preset does not exist here or on NVIDIA: the shipping DLL carries one set of\r\n"
         "; weights (preset 1) and any other value falls back to it. docs/styles-model-abc.md.\r\n"
         "Style=0\r\n"
         "; Scales the grade half of the model towards neutral, on top of Tone. 1 is the full\r\n"
         "; grade; 0 leaves the network input alone and removes only the colour change.\r\n"
         "StyleStrength=1\r\n";
    f.close();

    // The rest of the keys, through the same writer the overlay's Save uses, so a file written
    // here and a file written by Save carry exactly the same set -- an edit made without the
    // overlay cannot need a key that only exists once somebody has pressed a button.
    //
    // LoadSettings first, and not because anything needs reading: the default that belongs in the
    // file is the one LoadSettings applies when a key is absent, and for a dozen of them that is a
    // literal in the reader rather than g's constructed value (Passes, Encoding, Tonemap and the
    // rest). Reading the eight keys above and letting every other fallback land in g is what makes
    // the written file describe the run it is about to have instead of a slightly different one.
    LoadSettings();
    SaveSettings(/*quiet=*/true);
    Log("wrote a commented amd-nr.ini next to the exe; every value in it is a default.");
    return true;
}

void LoadSettings()
{
    const auto ini = (ExeDirectory() / L"amd-nr.ini").wstring();
    if (ini_text::StripUtf8Bom(ini))
        Log("removed a UTF-8 byte-order mark from amd-nr.ini: it was hiding every setting "
            "in the file, and all of them were reading as their defaults.");
    auto num = [&](const wchar_t *key, float fallback) {
        wchar_t buf[64] {};
        if (GetPrivateProfileStringW(L"amd-nr", key, L"", buf, 64, ini.c_str()) == 0)
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
    // Advanced=1 was the single switch this replaced; honour it once as "show all of them".
    // Advanced=1 was the single switch this replaced; honour it once as "show all of them".
    // kOptAll lives beside enum Opt, so a new bit widens both the mask and this in one edit.
    g.optional.store(static_cast<uint32_t>(num(L"HiddenShown",
        static_cast<float>(flag(L"Advanced", false) ? kOptAll : g.optional.load()))) & kOptAll);
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
    g.style.store(std::clamp(static_cast<int>(num(L"Style", 0.0f)), 0, 2));
    g.styleStrength.store(std::clamp(num(L"StyleStrength", 1.0f), 0.0f, 1.0f));
    {
        // Stated in the log because a style is a small change to the whole frame, and a
        // measurement run that does not say which one it drew cannot be compared to another.
        float se = 0.0f, sc = 0.0f, ss = 0.0f;
        if (StyleCoefficients(g.style.load(), StyleGradeStrength(), se, sc, ss))
            Log("Style=%d at strength %.2f: exposure %+.3f stops, contrast %+.3f, saturation "
                "%+.3f, applied to the composed frame.",
                g.style.load(), static_cast<double>(StyleGradeStrength()),
                static_cast<double>(se), static_cast<double>(sc), static_cast<double>(ss));
    }
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
    g.depthInverted.store(flag(L"DepthInverted", true) ? 1 : 0);
    g.depthNormalise.store(flag(L"DepthNormalise", g.depthNormalise.load()));
    g.useGameGuides.store(flag(L"GameGuides", g.useGameGuides.load()));
    g.useFeedEffect.store(flag(L"FeedEffect", g.useFeedEffect.load()));
    g.noBackBuffer.store(flag(L"NoBackBuffer", g.noBackBuffer.load()));
    g.noBridge.store(flag(L"NoBridge", g.noBridge.load()));
    g.glSemaphores.store(flag(L"GlSemaphores", g.glSemaphores.load()));
    g.glHoldFrames.store(std::clamp(static_cast<int>(num(L"GlHoldFrames", 0.0f)), 0, 8));
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
    {
        // The post kernel's output scale; at zero the network's answer never reaches the frame,
        // which reads as "enabled and disabled look the same" with the log saying every frame was
        // processed. Refused here rather than allowed, because nothing distinguishes a run with
        // it at zero from a run where the network broke, and Intensity 0 already exists for
        // "show me the game's own frame".
        float es = num(L"EngineScale", 0.03125f);
        if (!(es >= 1e-4f))
        {
            Log("WARNING: EngineScale=%.6f would make the network's output identical to its "
                "input; using the runtime's default 0.03125 instead.", static_cast<double>(es));
            es = 0.03125f;
        }
        g.engineScale.store(es);
    }
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
// Every key that belongs in the ini, listed once. SaveSettings writes them and the overlay's
// autosave hashes them; two lists would drift, and a setting present in one but not the other is a
// control that quietly stops being saved -- which is the bug this whole pair exists to prevent.
template <class Num, class Flag>
void ForEachSetting(Num num, Flag flag)
{
    num(L"Scale", g.scale.load());
    num(L"Passes", g.passes.load());
    num(L"Language", g.language.load());
    num(L"HiddenShown", static_cast<float>(g.optional.load()));
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
    num(L"Style", static_cast<float>(g.style.load()));
    num(L"StyleStrength", g.styleStrength.load());
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
    flag(L"DepthInverted", g.depthInverted.load() != 0);
    flag(L"DepthNormalise", g.depthNormalise.load());
    flag(L"GameGuides", g.useGameGuides.load());
    flag(L"FeedEffect", g.useFeedEffect.load());
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
}

void SaveSettings(bool quiet)
{
    const auto ini = (ExeDirectory() / L"amd-nr.ini").wstring();
    ForEachSetting(
        [&](const wchar_t *key, double v) {
            wchar_t buf[64];
            static _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
            if (c_locale != nullptr)
                _swprintf_s_l(buf, 64, L"%.4g", c_locale, v);
            else
                swprintf_s(buf, 64, L"%.4g", v);
            WritePrivateProfileStringW(L"amd-nr", key, buf, ini.c_str());
        },
        [&](const wchar_t *key, bool v) {
            WritePrivateProfileStringW(L"amd-nr", key, v ? L"1" : L"0", ini.c_str());
        });
    // Quiet while the first-run file is being filled in, and while autosaving: that line is the
    // only proof anyone has that the overlay's Save reached the disk, so it keeps meaning only that.
    if (!quiet) Log("settings saved to amd-nr.ini");
}

// "Has anything changed" without reading the ini back. FNV-1a over the same values, rounded through
// float first so a control that never left its own precision cannot look like an edit.
uint64_t SettingsFingerprint()
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](double v) {
        const float f = static_cast<float>(v);
        uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        h = (h ^ bits) * 1099511628211ull;
    };
    ForEachSetting([&](const wchar_t *, double v) { mix(v); },
                   [&](const wchar_t *, bool v) { mix(v ? 1.0 : 0.0); });
    return h;
}

// Our own D3D12 device on the adapter the game is already using, so shared textures and fences
// stay on one GPU and never touch system memory. Modelled on session.cpp::Session::CreateOn,
// which is the measured, working version of this transport.
// The adapter comes from the game's own D3D11 device, which BridgeStep1 has already asked. That
// is the same adapter by construction, and it means no DXGI factory has to be created -- one
// fewer library to touch, and one fewer chance to disturb the runtime the game is using.
bool RecreateWorkSlot(UINT slot)
{
    if (slot >= State::kRing || g.workDevice == nullptr)
        return false;

    // A failed Close leaves the list recording and unusable, while an allocator Reset followed by
    // a failed list Reset leaves the pair out of step. Recreate both so a transient bad recording
    // cannot poison this ring slot for the rest of the process.
    g.list[slot].Reset();
    g.alloc[slot].Reset();
    const HRESULT allocatorHr = g.workDevice->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc[slot]));
    const HRESULT listHr = SUCCEEDED(allocatorHr)
                               ? g.workDevice->CreateCommandList(
                                     0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[slot].Get(), nullptr,
                                     IID_PPV_ARGS(&g.list[slot]))
                               : allocatorHr;
    const HRESULT closeHr = SUCCEEDED(listHr) ? g.list[slot]->Close() : listHr;
    if (FAILED(allocatorHr) || FAILED(listHr) || FAILED(closeHr))
    {
        Log("bridge: could not recreate work slot %u (allocator 0x%08lX, list 0x%08lX, "
            "close 0x%08lX).", slot, allocatorHr, listHr, closeHr);
        g.list[slot].Reset();
        g.alloc[slot].Reset();
        return false;
    }
    g.ringValue[slot] = 0;
    return true;
}

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
        if (!RecreateWorkSlot(i))
        {
            Log("bridge: could not create the command allocator or list.");
            return false;
        }
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
                // Counted over samples that are actually depth. A depth buffer read correctly
                // is in 0..1 everywhere; the numbers that arrive when the read is wrong are
                // 1e38, -3e38 and NaN, and "min is not max" called every one of those a real
                // depth buffer. NaN fails both comparisons below, so it lands outside the range
                // on its own and never reaches the mean.
                double lo = 1e30, hi = -1e30, sum = 0.0;
                UINT64 inRange = 0;
                const UINT64 samples = static_cast<UINT64>(nw) * nh;
                for (UINT y = 0; y < nh; ++y)
                {
                    auto *row = reinterpret_cast<const float *>(static_cast<const char *>(a) +
                                                                static_cast<size_t>(y) * pitch);
                    for (UINT x = 0; x < nw; ++x)
                    {
                        const float v = row[x];
                        if (!(v >= 0.0f && v <= 1.0f))
                            continue;
                        lo = std::min<double>(lo, v);
                        hi = std::max<double>(hi, v);
                        sum += v;
                        ++inRange;
                    }
                }
                const double inPct = samples == 0 ? 0.0 : 100.0 * static_cast<double>(inRange) /
                                                              static_cast<double>(samples);
                const bool depthJunk = inPct < 90.0;
                const bool depthReal = !depthJunk && inRange > 0 && (hi - lo) > 1e-6;
                if (inRange == 0)
                    lo = hi = 0.0;
                g.probeDepthMin.store(static_cast<float>(lo));
                g.probeDepthMax.store(static_cast<float>(hi));
                // Only from a reading that is actually depth: under JUNK, hi is the maximum of
                // whatever few samples happened to land in 0..1, which is noise.
                if (!depthJunk && hi > 1e-6)
                    g.depthDebugScale.store(static_cast<float>(1.0 / hi));
                // The guide the network reads, from the same measurement. Gated on depthReal so
                // a flat buffer cannot latch a scale, and on hi < 0.5 so a game whose depth
                // already fills the range is left exactly as it was. Clamped because a max that
                // measures near zero would otherwise turn rounding noise into the whole signal.
                if (depthReal && hi < 0.5)
                {
                    const float s = std::clamp(static_cast<float>(1.0 / hi), 1.0f, 4096.0f);
                    g.depthScale.store(s);
                    Log("guide depth: range tops out at %.6f, so the buffer fills %.2f%% of 0..1 "
                        "-- scaling the guide by %.1fx to give the network the range it was "
                        "trained on. DepthNormalise=0 turns this off.",
                        hi, 100.0 * hi, static_cast<double>(s));
                }
                Log("guide probe, depth %ux%u: min %.6f max %.6f mean %.6f, %.1f%% in 0..1%s", nw,
                    nh, lo, hi, inRange == 0 ? 0.0 : sum / static_cast<double>(inRange), inPct,
                    depthJunk ? "  <-- JUNK. Most of this is not in 0..1, so it is not being read "
                                "as depth: wrong resource, or a copy between two layouts that do "
                                "not match. The network is better off without it."
                    : !depthReal ? "  <-- FLAT. A constant, so either this is a menu with no scene "
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
                // Naming the source is the difference between a number and a comparison: two
                // readings of "8% still" mean nothing unless it is known which of the three
                // things producing the field was running for each.
                Log("guide probe, motion %ux%u (%s): %llu%% exactly still, mean |d| %.3f px, "
                    "max %.3f px%s", nw, nh,
                    g.guideMotion.external  ? "from the effect"
                    : g.gameMotionActive    ? "the game's own"
                                            : "estimated",
                    static_cast<unsigned long long>(100 * zero / n), mag / (2.0 * n), biggest,
                    zero == n ? "  <-- ALL ZERO. Either nothing is moving, or the guide picked a "
                                "buffer the engine does not write velocity into."
                              : "");
                // Once a real scene has been seen there is nothing left to answer; until then
                // keep looking, because the first few hundred frames are menus and loading -- and
                // because junk must never end the search. It used to: the first probe fired on
                // depth that was 1e38 and motion that was uninitialised, both passed, and the
                // add-on stopped asking for the rest of the run.
                if (depthReal && zero < n)
                {
                    g.guidesLookReal.store(true);
                    g.probeGuides.store(false);
                    Log("guide probe: both guides carry real data. Not probing again.");
                }
                else if (++g.junkProbes >= 5)
                {
                    // Junk used to end the search, which was the bug. Never ending it is the
                    // other one: each probe allocates two readback buffers and spins on a fence
                    // on the present thread, and in a game whose depth reads as junk that is the
                    // one thing guaranteed to keep happening. Five is enough to outlast menus
                    // and loading screens; past that the answer is not going to change.
                    g.probeGuides.store(false);
                    Log("guide probe: five readings and the guides still do not carry usable "
                        "data. Not probing again -- the lines above say what was wrong with "
                        "each one.");
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
                    const auto dir = ExeDirectory() / L"amd-nr-captures";
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
                    const double meanRes = n ? sum / n : 0.0;
                    const bool first = !g.measured;
                    g.measured = true;
                    if (first)
                    {
                        Log("measure, network input: mean absolute %.6f (%llu samples)",
                            inputMean, static_cast<unsigned long long>(nb));
                        Log("  0.000000 means a black image was handed to the network.");
                        Log("measure, residual at %ux%u: mean %.6f, max %.6f (%llu samples)",
                            nw, nh, meanRes, peak, static_cast<unsigned long long>(n));
                        Log("  mean 0.000000 means the network returned its input unchanged.");
                        Log("measure, residual detail: local variation of the correction %.6f "
                            "against %.6f in the image itself -- ratio %.3f", gRes, gIn,
                            gIn > 0.0 ? gRes / gIn : 0.0);
                        Log("  Near 0.000 means the correction is smooth across the frame: a "
                            "colour, exposure or saturation shift, with nothing done to texture. "
                            "Rising toward and past 0.100 means the correction follows the "
                            "image's own detail, which is the network working on structure.");
                    }
                    // The watchdog. A working run measures a residual of a few percent of the
                    // input (God of War: 0.021 on a 0.13 mean; a run that had broken measured
                    // 0.00024 on 0.45). One part in a thousand is far below any working reading
                    // and far above readback noise.
                    const bool inert = meanRes < inputMean * 1e-3;
                    if (inert != g.inert.load() || (first && inert))
                    {
                        g.inert.store(inert);
                        if (inert)
                            Log("WARNING: the network is returning its input unchanged (residual "
                                "mean %.6f against an input mean of %.6f). The picture will not "
                                "change with the effect on, whatever the log says about frames. "
                                "Checks, in order: output scale 97b3c %.5f (the runtime's default "
                                "is 0.03125); intensity %.2f; structure %.2f (0 removes the "
                                "effect); the runtime log for GPU errors or 'output stores are "
                                "being dropped'.",
                                meanRes, inputMean, static_cast<double>(g.engineScale.load()),
                                static_cast<double>(g.intensity.load()),
                                static_cast<double>(g.structure.load()));
                        else
                            Log("measure: the network is changing the frame again (residual mean "
                                "%.6f against %.6f).", meanRes, inputMean);
                    }
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
bool CreateTexture(UINT w, UINT h, DXGI_FORMAT f, ComPtr<ID3D12Resource> &out, const char *what,
                   D3D12_RESOURCE_STATES initialState =
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
        // BIND_DEPTH_STENCIL as well, for a depth format, so this is the same kind of resource as
        // the one about to be copied into it. A depth-stencil surface is planar and compressed;
        // the same typeless format without the flag is neither, and CopyResource between the two
        // is a copy across layouts. The D3D12 half of this add-on made exactly this mistake with
        // its pre-clear snapshot and every probe of the result came back min -3e38, max 2e36,
        // mean NaN -- garbage that was then handed to the network as depth. This is that same
        // copy, on the D3D11 bridge, and Darksiders 3 takes it: R24G8_TYPELESS at 2560x1440.
        //
        // Kept to depth formats: a motion guide is an ordinary two-channel colour target and the
        // flag would only make its creation fail.
        const bool isDepth = GuideDepthSrvFormat(guide.format) != DXGI_FORMAT_UNKNOWN;
        td.BindFlags = isDepth ? (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL)
                               : D3D11_BIND_SHADER_RESOURCE;
        HRESULT made = g.game11->CreateTexture2D(&td, nullptr, &guide.snap);
        if (FAILED(made) && isDepth)
        {
            // Some formats reach here that no driver will give a depth-stencil view of. Falling
            // back leaves the old behaviour rather than losing the guide outright, and says so,
            // because a copy on this path is then the suspect for anything odd downstream.
            Log("guide depth: %ux%u format %u was refused as a depth-stencil copy (0x%08lX); "
                "falling back to a plain shader-resource copy, which may not read correctly.",
                w, h, static_cast<unsigned>(guide.format), static_cast<unsigned long>(made));
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            made = g.game11->CreateTexture2D(&td, nullptr, &guide.snap);
        }
        if (FAILED(made))
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

// The motion-vector shaders the companion effect can be compiled against. Name checks only:
// the effect binds the selected provider's output texture itself, so one that is not listed
// here still works. This exists to answer "is anything actually writing that texture", which
// is the difference between a real field and a page of zeros the network would read as
// "nothing moved" -- a wrong answer, where no answer at all leaves the estimator in charge.
constexpr struct
{
    const char *file, *tech;
} kMvProviders[] = {
    { "MartysMods_LAUNCHPAD.fx", "MartysMods_Launchpad" },
    { "vort_Motion.fx", "vort_MotionEffects" },
    { "lumenite_Kernel.fx", "Lumenite_Kernel" },
    { "lumenite_QuantMotion.fx", "Lumenite_QuantMotion" },
    { "qUINT_motionvectors.fx", "MotionVectors" },
    { "dh_uber_motion.fx", "DH_UBER_MOTION_020" },
    { "MotionEstimation.fx", "DRME" },
};

// Whether a technique of that name exists and is ticked. ReShade keeps a technique whose effect
// failed to compile in its list and lets it be "enabled" -- it just never runs -- so this is
// necessary but not sufficient, and the guide probe is what catches the rest.
bool TechniqueOn(const char *file, const char *tech)
{
    if (g.effects == nullptr)
        return false;
    const auto t = g.effects->find_technique(file, tech);
    return t.handle != 0 && g.effects->get_technique_state(t);
}

bool AnyMvProviderOn()
{
    for (const auto &p : kMvProviders)
        if (TechniqueOn(p.file, p.tech))
            return true;
    return false;
}

// One texture of the companion effect, as the D3D11 resource ReShade allocated for it.
// ReShade owns the lifetime; this is read inside present, between the effect chain finishing
// and the copy that carries it across, which is the window where it is both written and alive.
bool FeedTexture(const char *name, Guide &guide)
{
    if (g.effects == nullptr)
        return false;
    const auto var = g.effects->find_texture_variable("AMD_Neural_Feed.fx", name);
    if (var.handle == 0)
        return false;
    reshade::api::resource_view srv {}, srgb {};
    g.effects->get_texture_binding(var, &srv, &srgb);
    if (srv.handle == 0)
        return false;
    reshade::api::device *dev = g.effects->get_device();
    if (dev == nullptr)
        return false;
    const reshade::api::resource res = dev->get_resource_from_view(srv);
    auto *native = reinterpret_cast<ID3D11Resource *>(res.handle);
    if (native == nullptr)
        return false;
    const auto desc = dev->get_resource_desc(res);
    if (desc.texture.width < kGuideFloor || desc.texture.height < kGuideFloor)
        return false;
    // reshade::api::format is DXGI's numbering, value for value, which is what lets the rest of
    // the guide path -- built against DXGI_FORMAT throughout -- take this without a translation
    // table that would have to be kept in step with two enums at once.
    const auto format = static_cast<DXGI_FORMAT>(desc.texture.format);
    if (guide.chosen.Get() != native || guide.format != format ||
        guide.width != desc.texture.width || guide.height != desc.texture.height)
    {
        guide.chosen = native;
        guide.width = desc.texture.width;
        guide.height = desc.texture.height;
        guide.format = format;
        guide.ready = false;
        guide.logged = false;
        guide.failed = false;
        guide.challenger = nullptr;
        guide.challengerFrames = 0;
    }
    guide.external = true;
    return true;
}

// Take the companion effect's guides when it is installed and switched on, and hand the slots
// back to the bind observation when it is not. Called once per present, before the tally is
// settled, because the tally must not be consulted for a slot the effect owns.
//
// The game's own depth is preferred over the effect's: a real depth buffer is the geometry the
// engine drew, while ReShade's is whatever its heuristic selected, and on the routes where the
// observation finds nothing the effect is the only source there is. Motion is the other way
// round -- the effect's field beats this add-on's own estimator on every count -- but a game
// that writes a velocity buffer still beats both, because that one is not a guess at all.
void AdoptFeedEffect()
{
    // A texture whose technique is not ticked is not being written. It still resolves, still
    // has the right size, and still copies across without complaint -- as whatever was in it
    // when the technique was last on, or as zeros. Both read to the network as fact.
    const bool ticked = g.useFeedEffect.load() && g.effects != nullptr &&
                        TechniqueOn("AMD_Neural_Feed.fx", "AMD_Neural_Feed");
    const bool provider = ticked && AnyMvProviderOn();
    const bool haveGameMotion = g.guideMotion.chosen != nullptr && !g.guideMotion.external;
    const bool haveGameDepth = g.guideDepth.chosen != nullptr && !g.guideDepth.external;

    bool mv = false, depth = false;
    // Depth needs only the effect; motion needs a provider behind it as well, because the
    // effect is a validator and a converter, not an estimator -- with nothing writing the
    // provider's texture it forwards zeros, and zeros are worse than the estimator.
    if (provider && !haveGameMotion)
        mv = FeedTexture("AMDNR_MV", g.guideMotion);
    if (ticked && !haveGameDepth)
        depth = FeedTexture("AMDNR_Depth", g.guideDepth);
    for (Guide *guide : { &g.guideMotion, &g.guideDepth })
    {
        const bool taken = (guide == &g.guideMotion) ? mv : depth;
        if (guide->external && !taken)
        {
            // The effect went away -- unticked, reloaded, or the game changed resolution and
            // ReShade rebuilt its textures. Drop the slot rather than keep copying from a
            // resource that is no longer the one being written.
            guide->external = false;
            guide->chosen.Reset();
            guide->ready = false;
            guide->failed = false;
        }
    }

    // Says the state once per change, not once per frame. Games recreate swapchains in bursts
    // and ReShade its effects with them, and a line per present would bury everything else.
    const int signature = (ticked ? 1 : 0) | (mv ? 2 : 0) | (depth ? 4 : 0) |
                          (haveGameMotion ? 8 : 0) | (haveGameDepth ? 16 : 0) |
                          (provider ? 32 : 0);
    if (signature == g.feedSignature)
        return;
    const bool first = g.feedSignature < 0;
    g.feedSignature = signature;
    // Re-arm the guide probe whenever the motion source changes hands. The probe is the only
    // instrument that says what is actually in the field, and it latched itself off after one
    // reading -- a reading taken in the first seconds, while ReShade was still compiling, so it
    // always measured the estimator and never the provider that replaced it. Toggling the
    // provider's technique is exactly the A/B this is for, and it has to be measurable more
    // than once. Not on the first call: the probe is already armed then, and re-arming would
    // only push the reading further out.
    if (!first)
    {
        g.probeGuides.store(true);
        g.nextGuideProbe = g.frame + 120;
    }
    std::snprintf(g.feedStatus, sizeof(g.feedStatus),
                  "AMD_Neural_Feed.fx: %s; motion %s, depth %s",
                  g.effects == nullptr      ? "no effect runtime yet"
                  : !g.useFeedEffect.load() ? "switched off"
                  : !ticked                 ? "not installed, or its technique is not enabled"
                  : !provider ? "enabled, but no motion-vector shader is enabled above it"
                              : "enabled",
                  mv               ? "from the effect"
                  : haveGameMotion ? "from the game"
                                   : "estimated",
                  depth           ? "from the effect"
                  : haveGameDepth ? "from the game"
                                  : "none of its own");
    // The raw answers behind the summary, because the summary was seen flapping between
    // "enabled" and "no motion-vector shader" several times a second on a preset that had not
    // changed. A handle of 0 is find_technique refusing while ReShade is loading; a state of 0
    // is the technique itself switched off.
    const auto feedTech = g.effects ? g.effects->find_technique("AMD_Neural_Feed.fx", "AMD_Neural_Feed") : reshade::api::effect_technique { 0 };
    const auto lpTech = g.effects ? g.effects->find_technique("MartysMods_LAUNCHPAD.fx", "MartysMods_Launchpad") : reshade::api::effect_technique { 0 };
    Log("%s  [frame %llu: feed handle %d state %d, launchpad handle %d state %d]", g.feedStatus,
        static_cast<unsigned long long>(g.frame), feedTech.handle != 0 ? 1 : 0,
        feedTech.handle != 0 && g.effects->get_technique_state(feedTech) ? 1 : 0,
        lpTech.handle != 0 ? 1 : 0,
        lpTech.handle != 0 && g.effects->get_technique_state(lpTech) ? 1 : 0);
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

// The module pass `slot` records through. Without a copy of its own a pass shares the first
// module, which is how every pass ran before the copies existed.
HMODULE RuntimeFor(UINT slot)
{
    return slot < State::kMaxPasses && g.runtimes[slot] != nullptr ? g.runtimes[slot] : g.runtime;
}

// Whether any module still has a job in flight: its job counter has not reached the id of the
// last job this add-on recorded through it.
bool RuntimeBusy()
{
    for (UINT i = 0; i < State::kMaxPasses; ++i)
    {
        HMODULE m = g.runtimes[i];
        if (m == nullptr)
            continue;
        if (static_cast<UINT>(InterlockedCompareExchange(
                reinterpret_cast<volatile LONG *>(&At<UINT>(m, rt::kJobCounter)), 0, 0)) <
            g.lastJobs[i])
            return true;
    }
    return false;
}

void ResetJobs()
{
    g.lastJob = 0u;
    for (UINT &j : g.lastJobs)
        j = 0;
    g.recordedMask = 0;
}

// The frame-notify, to every module that recorded onto the lists being submitted. A module that
// recorded nothing is not told: what its notify does with a list it never saw is not known.
void NotifyRuntimes(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    for (UINT i = 0; i < State::kMaxPasses; ++i)
        if ((g.recordedMask & (1u << i)) != 0 && g.runtimes[i] != nullptr)
            reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(g.runtimes[i]) + rt::kNotifyFn)(
                queue, count, lists);
    g.recordedMask = 0;
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
    g.reason = "the D3D12 device was removed; see amd-nr.log";
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
    while (RuntimeBusy())
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
    NotifyRuntimes(g.queue.Get(), 1, lists);
    if (!FinishSubmittedPass())
        return false;
    return SUCCEEDED(allocator->Reset()) && SUCCEEDED(cmd->Reset(allocator, nullptr));
}

void RenderEffectsAheadOfNetwork(device *dev, resource back);

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

    if (!EnsureResources(w, h, fmt, EffectiveScale()))
    {
        g.unavailable = true;
        if (*g.reason == 0)
            g.reason = "could not create the working textures; see amd-nr.log";
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
        if (!CreateTexture(w, h, fmt, g.crossLocal, "crossLocal",
                           D3D12_RESOURCE_STATE_COPY_DEST))
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
        RuntimeBusy();
    if (!jobPending && g.jobRunning)
    {
        g.jobRunning = false;
        NoteJobCost(GetTickCount64() - g.lastJobAt);
    }
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
        ResetJobs();
    }

    // 0. when the companion effect is on, ReShade's chain runs now, before the copy below reads
    // the back buffer. See RenderEffectsAheadOfNetwork for why the order matters.
    RenderEffectsAheadOfNetwork(dev, back);

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
    // Settled first, and unconditionally, so a game that renders its own buffers can take a slot
    // back off the companion effect. The observation no longer sees ReShade's own targets, so an
    // entry in these tallies is the game's by construction.
    SettleGuide(g.guideDepth, g_depthTally);
    SettleGuide(g.guideMotion, g_motionTally);
    AdoptFeedEffect();
    g.guideDepth.ready = g.guideMotion.ready = false;
    if (g.useGameGuides.load())
    {
        // The effect's depth is already a plain R32_FLOAT, which opens on the second device as
        // it stands. It goes down the motion path -- one CopyResource -- rather than the depth
        // one, whose snapshot and compute pass exist only to get a planar depth-stencil format
        // into a shape that can cross at all.
        if (g.useDepth.load())
            PrepareGuide(g.guideDepth, !g.guideDepth.external);
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
    const HRESULT allocatorHr = g.alloc[i]->Reset();
    const HRESULT listHr = SUCCEEDED(allocatorHr)
                               ? g.list[i]->Reset(g.alloc[i].Get(), nullptr)
                               : allocatorHr;
    if (FAILED(allocatorHr) || FAILED(listHr))
    {
        Log("bridge: work slot %u reset failed (allocator 0x%08lX, list 0x%08lX); "
            "recreating the pair.", i, allocatorHr, listHr);
        if (!RecreateWorkSlot(i))
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
        ResetJobs();
        g.activePasses = 0;
        g.historyValid.store(0);
        if (!RecreateWorkSlot(i))
            g.bridgeFailed = true;
        return;
    }
    ID3D12CommandList *lists[] { cmd };
    g.workQueue->ExecuteCommandLists(1, lists);
    if (g.activePasses != 0)
        NotifyRuntimes(g.workQueue.Get(), 1, lists);
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
    // v0.3.0 renamed the inline switch: the key is now `Async`, and it is the inverse of the old
    // `Inline` (Async=0 means inline). An ini this add-on wrote for v0.2.17 is still correct by
    // accident -- the unknown `Inline` key is ignored and `Async` defaults to 0 -- so an existing
    // file is left alone, as it always was.
    f << "[DlssNrOnAmd]\r\n"
         "Enabled=1\r\n"
         "Async=0\r\n"
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

    const auto patched = ExeDirectory() / L"amd-nr-pass1.dll";
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

// Hand a freshly loaded module the device, the queue and the switches, then let it load the
// weights. The same for the first module and for the per-pass copies.
bool ArmRuntime(HMODULE h)
{
    At<ID3D12Device *>(h, rt::kDevice) = g.device.Get();
    g.device->AddRef();
    At<ID3D12CommandQueue *>(h, rt::kQueue) = g.queue.Get();
    g.queue->AddRef();
    At<int>(h, rt::kHipDevice) = g.hipDevice;
    At<uint8_t>(h, rt::kInlineMode) = g.inlineMode.load() ? 1 : 0;
    At<uint8_t>(h, rt::kInterop) = 1;
    At<uint8_t>(h, rt::kEnabled) = 1;
    At<uint8_t>(h, rt::kUseFsrInputs) = 1;
    At<uint8_t>(h, rt::kUseDepth) = 0;
    At<int>(h, rt::kTonemap) = RuntimeTonemap();

    const std::string file = (ExeDirectory() / L"dlssnr_on_amd_weights.bin").string();
    if (g.hipSet(g.hipDevice) != 0 ||
        !reinterpret_cast<InitFn>(reinterpret_cast<uintptr_t>(h) + rt::kInitFn)(
            reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(h) + rt::kEngineObject), &file))
    {
        Log("engine init failed.");
        return false;
    }
    At<uint8_t>(h, rt::kReady) = 1;
    return true;
}

// A second (third) copy of the runtime, for pass `slot` + 1. The loader keys modules by file
// name, so a byte-identical copy under another name is a separate module: its own globals, its
// own HIP engine object, its own weights in VRAM (about 150 MB each) and, the point of it, its
// own temporal state. Loaded on demand, the first time the pass count asks for it, which costs
// one long frame once.
bool LoadExtraRuntime(UINT slot)
{
    if (g.runtime == nullptr || g.runtimeFile.empty() || slot == 0 || slot >= State::kMaxPasses)
        return false;
    wchar_t leaf[32];
    std::swprintf(leaf, 32, L"amd-nr-pass%u.dll", slot + 1);
    const auto copy = ExeDirectory() / leaf;
    std::error_code ec;
    std::filesystem::copy_file(g.runtimeFile, copy,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        Log("pass %u: could not write %ls (%s)", slot + 1, copy.c_str(), ec.message().c_str());
        return false;
    }
    HMODULE h = LoadLibraryExW(copy.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (h == nullptr)
    {
        Log("pass %u: LoadLibrary failed for %ls (error %lu)", slot + 1, leaf, GetLastError());
        return false;
    }
    HMODULE pinned {};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(h), &pinned);
    if (!ArmRuntime(h))
    {
        Log("pass %u: its own copy of the runtime did not initialise; the pass will share the "
            "first module and its history.", slot + 1);
        return false;
    }
    g.runtimes[slot] = h;
    Log("pass %u: running through its own copy of the runtime (%ls), so it carries its own "
        "temporal state instead of alternating with pass 1's.", slot + 1, leaf);
    return true;
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
                   "against; see amd-nr.log";
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
    Log("input contract: encoding %d, tonemap requested %d -> runtime %d; FP16 is transport, "
        "not a colour-space declaration. Restart after changing encoding or tonemap.",
        g.encoding.load(), g.tonemap.load(), RuntimeTonemap());
    if (!ArmRuntime(h))
        return false;
    g.runtime = h;
    g.runtimes[0] = h;
    g.runtimeFile = loadFrom;
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
        int n = std::snprintf(line, sizeof(line), "engine floats 0x%zx..0x%zx:", rt::kFloatDumpFirst,
                              rt::kFloatDumpLast);
        for (size_t rva = rt::kFloatDumpFirst;
             rva <= rt::kFloatDumpLast && n > 0 && n < static_cast<int>(sizeof(line));
             rva += 4)
            n += std::snprintf(line + n, sizeof(line) - n, " [%zx]=%.3f", rva,
                               static_cast<double>(At<float>(h, rva)));
        Log("%s", line);
        // Dump the byte window the engine has just finished initialising. A field the engine
        // owns holds a plausible default; a field nothing uses holds whatever the loader left.
        // This is read-only and runs after init, so what it prints is the engine's own state.
        n = std::snprintf(line, sizeof(line), "engine bytes 0x%zx..0x%zx:", rt::kByteDumpFirst,
                          rt::kByteDumpLast);
        for (size_t rva = rt::kByteDumpFirst;
             rva <= rt::kByteDumpLast && n > 0 && n < static_cast<int>(sizeof(line));
             ++rva)
            n += std::snprintf(line + n, sizeof(line) - n, " %02x",
                               static_cast<unsigned>(At<uint8_t>(h, rva)));
        Log("%s", line);
        Log("  97b10 is DepthInverted, pinned to the engine's own default of 1 and no longer a "
            "control. 97b40 UseAutoMask, 97b44 ToneChannels and 97b3c Scale are the fields the "
            "Engine tab writes; what they read back as here is the engine's own state before "
            "this add-on touches them.");
        Log("  written by this add-on: 97b30 tone, 97b34 structure, 97b38 skin. If one of those "
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
    // Fifteen, not eight: compose needs seven more than the rest -- the residual limit, the edge
    // fade, the colour strength, the highlight guard, and the three style coefficients. A shader
    // declaring a shorter cbuffer over a longer root constant block is fine, so the other six
    // keep passing eight.
    params[1].Constants = { 0, 0, 15 };
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
    if (!CompileShader(shaders::kCopyShader, sizeof(shaders::kCopyShader), "copy", g.copyPipeline) ||
        !CompileShader(shaders::kDepthShader, sizeof(shaders::kDepthShader), "depth", g.depthPipeline) ||
        !CompileShader(shaders::kComposeShader, sizeof(shaders::kComposeShader), "compose", g.composePipeline) ||
        !CompileShader(shaders::kResidualShader, sizeof(shaders::kResidualShader), "residual", g.residualPipeline) ||
        !CompileShader(shaders::kLumaShader, sizeof(shaders::kLumaShader), "luma", g.lumaPipeline) ||
        !CompileShader(shaders::kFlowShader, sizeof(shaders::kFlowShader), "flow", g.flowPipeline) ||
        !CompileShader(shaders::kFlowUpShader, sizeof(shaders::kFlowUpShader), "flowup", g.flowUpPipeline))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    return SUCCEEDED(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)));
}

bool CreateTexture(UINT w, UINT h, DXGI_FORMAT f, ComPtr<ID3D12Resource> &out, const char *what,
                   D3D12_RESOURCE_STATES initialState)
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
    if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initialState,
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
            const bool idle = WaitFence(g.fence.Get(), g.completion, done,
                                        "the old raster before changing resolution");
            CloseHandle(done);
            if (!idle)
            {
                g.reason = "the GPU did not release the old raster before its resolution changed";
                return false;
            }
        }
        else return false;
    }

    // That fence covers our own queue. It says nothing about a network job the engine still has
    // in flight -- and the engine holds zero-copy handles into netColour/netMotion/netDepth, so
    // releasing them underneath a running job is a use-after-free on the GPU. Only reachable on
    // a real geometry change, which is rare, so a blocking wait here costs nothing in practice.
    if (g.engineReady && g.runtime != nullptr)
    {
        const UINT64 deadline = GetTickCount64() + 5000;
        while (RuntimeBusy() &&
                   GetTickCount64() < deadline)
            Sleep(1);
        if (RuntimeBusy())
        {
            Log("raster: runtime job %u did not become idle in 5 s; keeping its textures alive "
                "instead of releasing memory that the GPU may still own.", g.lastJob);
            g.reason = "the neural runtime did not become idle for a resolution change";
            return false;
        }
    }

    // The bridge owns three reusable command lists. A completed list may still retain driver-side
    // bookkeeping for every resource recorded into it until Reset, which makes rapid resolution
    // changes look like a VRAM leak even after the corresponding ComPtr is released. The UI now
    // commits one scale after an edit instead of one per mouse movement; retire all three lists
    // at that single boundary so the old raster has no command-list lifetime left either.
    if (netChanged && g.workDevice != nullptr)
    {
        for (UINT i = 0; i < State::kRing; ++i)
        {
            if (RecreateWorkSlot(i))
                continue;
            Log("raster: could not retire work slot %u before changing resolution.", i);
            return false;
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
         !CreateTexture(nw, nh, DXGI_FORMAT_R16G16B16A16_FLOAT, g.netResidual, "netResidual") ||
         !CreateTexture(nw, nh, DXGI_FORMAT_R16G16_FLOAT, g.netMotion, "netMotion") ||
         !CreateTexture(nw, nh, DXGI_FORMAT_R32_FLOAT, g.netDepth, "netDepth") ||
         !CreateTexture(fw, fh, DXGI_FORMAT_R16_FLOAT, g.lumaA, "lumaA") ||
         !CreateTexture(fw, fh, DXGI_FORMAT_R16_FLOAT, g.lumaB, "lumaB") ||
         !CreateTexture(fw, fh, DXGI_FORMAT_R16G16_FLOAT, g.flowSmall, "flowSmall") ||
         !CreateTexture(fw, fh, DXGI_FORMAT_R16G16_FLOAT, g.flowCoarse, "flowCoarse")))
        return false;
    // One per pass. Built for every slot rather than for the current pass count, because the
    // count is a live control: allocating on demand would put a texture creation in the middle
    // of a frame the first time somebody moves the slider. Three at 1306x662 RGBA16F is 10 MB.
    if (netChanged)
        for (UINT i = 0; i < State::kMaxPasses; ++i)
        {
            char name[16];
            std::snprintf(name, sizeof(name), "history%u", i + 1);
            if (!CreateTexture(nw, nh, DXGI_FORMAT_R16G16B16A16_FLOAT, g.history[i], name))
                return false;
        }
    if (netChanged)
        g.historyValid.store(0);
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

// What the network is actually run at: the person's Scale, held down when this card has already
// shown it cannot carry it.
//
// The engine's inline mode makes the game's own queue wait for the network, and the watchdog this
// add-on writes into dlssnr_on_amd.ini (InlineWaitMs=100) only stops the CPU from waiting -- it
// cannot cancel a dispatch already on the GPU. A Conan Exiles log from an RX 9070 has jobs of
// 277 ms and then 2711 ms at 1920x1080, which is Windows TDR territory: the driver resets, the
// device is removed, and the game goes with it. The person reporting it had the game and then the
// whole PC go down, and found by hand that 0.50 was the setting that survived.
//
// So the cost is measured and the scale is held one step below whatever produced it. Their own
// setting is never overwritten -- the cap is separate, and the overlay says it is in force.
float EffectiveScale()
{
    return ui::EffectiveScale(g.scale.load(), g.scaleCap.load());
}

// One finished evaluation, timed from this side rather than read out of the engine's log. Three
// dangerous jobs and the scale comes down a step: one is a shader compile or an alt-tab, three is
// this card at this resolution.
void NoteJobCost(UINT64 ms)
{
    constexpr UINT64 kDanger = 250;  // an order of magnitude past a frame, far short of TDR
    // A reading this large is not a job. The GPU cannot hold one for half a minute -- Windows
    // resets the driver long before -- so it is a pause that slipped past the latch: the effect
    // switched off and on, a window restored on a path that does not clear it. Counting it would
    // cap the scale for something that never ran.
    if (ms > 30000)
        return;
    if (ms > g.worstJobMs.load())
        g.worstJobMs.store(ms);
    if (ms < kDanger)
    {
        if (ms < kDanger / 2)
            g.longJobs = 0;  // comfortably back inside budget: the streak was a hitch
        return;
    }
    if (++g.longJobs < 3)
        return;
    g.longJobs = 0;

    const float now = EffectiveScale();
    const float next = std::max(0.25f, now - 0.25f);
    if (next >= now)
    {
        Log("the network took %llu ms at scale %.2f, which is already the lowest. This card "
            "cannot carry this resolution; turn the effect off rather than risk the driver.",
            static_cast<unsigned long long>(ms), static_cast<double>(now));
        return;
    }
    g.scaleCap.store(next);
    Log("the network took %llu ms three times at scale %.2f. A dispatch that long resets the "
        "display driver and takes the game with it, so the scale is held at %.2f. Your own Scale "
        "setting is untouched; raise the cap by setting Scale again in the overlay.",
        static_cast<unsigned long long>(ms), static_cast<double>(now), static_cast<double>(next));
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
    // An absolute floor first, because the relative one below is measured against a swapchain
    // size that is zero until the effect has been enabled once -- and "anything passes while the
    // size is unknown" is how a 1x1 buffer became the motion guide.
    if (d.Width < kGuideFloor || d.Height < kGuideFloor)
        return false;
    if (screenW == 0 || screenH == 0)
        return true;
    return d.Width * 2 >= screenW && d.Height * 2 >= screenH;
}

// D3D11 half of the observation. ReShade hands the render targets and the depth-stencil of
// every bind; on D3D12 it hands the add-on only the swapchain (measured: zero depth binds in
// 600 frames), which is why this path exists at all and why PCSX2 had to be moved to D3D11
// before it could show a depth buffer.
void ObserveD3D11(device *dev, const resource_view *rtvs, uint32_t count, resource depthRes)
{
    if (!g.useGameGuides.load())
        return;
    // Not while ReShade is drawing its own effect chain. Every shader in that chain renders into
    // screen-sized intermediates, and an optical-flow shader's are two-channel float ones the
    // size of the screen -- which is the exact description this observation uses to recognise a
    // velocity buffer. Without this gate the motion guide could settle on a provider's working
    // texture, or on the companion effect's own previous-frame copy, and prefer it over the
    // game's real one on nothing better than which got bound more often.
    if (g.inEffects.load())
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
            // Same two floors, for the same reason.
            if (d.SampleDesc.Count == 1 && d.ArraySize == 1 &&
                d.Width >= kGuideFloor && d.Height >= kGuideFloor &&
                GuideDepthSrvFormat(d.Format) != DXGI_FORMAT_UNKNOWN &&
                (screenW == 0 || screenH == 0 ||
                 (d.Width * 2 >= screenW && d.Height * 2 >= screenH)))
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

// True on a thread while this add-on is issuing commands into the host's API on its own account.
//
// On D3D11 and D3D12 that distinction never mattered: the bridge records into its own command
// list on its own device, and ReShade has nothing to say about it. In OpenGL there is no such
// separation. Every call the route makes goes through the same hooked entry points the game uses,
// so ReShade reports the route's own glBindFramebuffer back to this add-on as a render-target
// bind -- on the present thread, inside OnPresent, which already holds g.lock. The second lock on
// a std::mutex from the same thread does not deadlock under MSVC: it throws std::system_error,
// which is unhandled, which closes the game. That is exactly how the OpenGL route's first run
// ended, with the log stopping mid-way through building the crossing and an E06D7363 in
// KERNELBASE to show for it.
//
// So the observers ignore what this add-on did itself. They would be wrong to count it in any
// case: the route's binds are not the game drawing its frame.
thread_local bool g_selfIssued = false;

struct SelfIssued
{
    SelfIssued() { g_selfIssued = true; }
    ~SelfIssued() { g_selfIssued = false; }
    SelfIssued(const SelfIssued &) = delete;
    SelfIssued &operator=(const SelfIssued &) = delete;
};

// ReShade fires the add-on's `present` event BEFORE it renders its own effect chain: on D3D11
// dxgi_swapchain.cpp invokes addon_event::present and only then present_effect_runtime, whose
// on_present is what calls render_effects. So a texture the companion effect writes, read from
// inside OnPresent, still holds what the chain produced for the PREVIOUS frame. The network was
// being handed frame N's colour with frame N-1's flow -- vectors that describe a motion the
// picture has already finished making -- which is why an eight-level Launchpad pyramid measured
// no different from the two-level estimator it was meant to replace.
//
// The fix is to run the chain here, ahead of the copy that carries the back buffer across.
// runtime::render_effects refuses to run twice in one frame (_effects_rendered_this_frame), so
// ReShade's own on_present then skips straight to the overlay, and the frame the user sees is
// still the composed one. Only when the effect is ticked: without it the chain keeps its usual
// place after the network, which is what every other user of this add-on has today.
//
// Marked self-issued because ReShade reports the chain's own render-target binds back through
// OnBindDepthStencil, which takes g.lock -- and the caller already holds it.
void RenderEffectsAheadOfNetwork(device *dev, resource back)
{
    if (g.effects == nullptr || !g.useFeedEffect.load() ||
        !TechniqueOn("AMD_Neural_Feed.fx", "AMD_Neural_Feed"))
        return;
    command_queue *queue = g.effects->get_command_queue();
    if (queue == nullptr)
        return;
    // render_effects returns without drawing when the view is zero, so it needs a real one.
    // ponytail: a view per frame; cache it by back-buffer handle if it ever shows in a profile.
    const resource_desc bd = dev->get_resource_desc(back);
    resource_view rtv {};
    if (!dev->create_resource_view(back, resource_usage::render_target,
                                   resource_view_desc(format_to_default_typed(bd.texture.format, 0)),
                                   &rtv))
        return;
    {
        SelfIssued self;
        g.effects->render_effects(queue->get_immediate_command_list(), rtv, resource_view {});
    }
    dev->destroy_resource_view(rtv);
    if (!g.loggedEffectsFirst)
    {
        g.loggedEffectsFirst = true;
        Log("effects: AMD_Neural_Feed is on, so ReShade's chain now runs before the network "
            "each frame and the guides it hands over are this frame's, not last frame's.");
    }
}

void OnBindDepthStencil(command_list *cmd_list, uint32_t count, const resource_view *rtvs,
                        resource_view dsv)
{
    if (g_selfIssued)
        return;
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
            // Name the API rather than assuming it is the other one. This observation is
            // API-agnostic and Vulkan reaches it too, where "D3D11" would be a plain lie in the
            // one log a Vulkan problem is diagnosed from.
            const char *api = dev->get_api() == device_api::d3d12   ? "D3D12"
                              : dev->get_api() == device_api::d3d11 ? "D3D11"
                              : dev->get_api() == device_api::vulkan ? "Vulkan"
                              : dev->get_api() == device_api::opengl ? "OpenGL"
                                                                     : "other API";
            Log("depth seen (%s): %ux%u format %u samples %u", api,
                rd.texture.width, rd.texture.height, static_cast<unsigned>(rd.texture.format),
                rd.texture.samples);
        }
    }
    if (!d3d12)
        return;
    auto *native = reinterpret_cast<ID3D12Resource *>(res.handle);
    std::lock_guard guard(g.lock);
    if (native == g.depthBest.Get())
        ++g.depthBinds;  // the status line's running total, which outlives one present
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
    if (!ScreenShaped(d.Width, d.Height, g.outWidth, g.outHeight))
        return;
    ++TallyD12Depth(native, d).binds;
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
    if (g_selfIssued || cmd_list == nullptr || dsv.handle == 0)
        return false;
    device *dev = cmd_list->get_device();
    if (dev == nullptr || dev->get_api() != device_api::d3d12)
        return false;
    const resource res = dev->get_resource_from_view(dsv);
    if (res.handle == 0)
        return false;
    auto *native = reinterpret_cast<ID3D12Resource *>(res.handle);

    std::lock_guard guard(g.lock);
    // Counted for whichever candidate this is, not only for the one holding the slot: "the game
    // clears it every frame" is how the scene depth is told from a buffer that is merely the same
    // size, and that has to be known about a challenger before it can win. Recorded rather than
    // looked up, so a clear that comes before this present's first bind still counts.
    {
        const auto cd = native->GetDesc();
        if (ScreenShaped(cd.Width, cd.Height, g.outWidth, g.outHeight) &&
            DepthReadFormat(cd.Format) != DXGI_FORMAT_UNKNOWN &&
            (cd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0 &&
            cd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && cd.SampleDesc.Count == 1 &&
            cd.DepthOrArraySize == 1)
            ++TallyD12Depth(native, cd).clears;
    }
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
        // ALLOW_DEPTH_STENCIL, and not NONE, for the same reason the live-buffer alias path uses
        // it. R32G8X24_TYPELESS without the flag is a plain one-plane 64-bit texture; the game's
        // depth-stencil resource is planar. CopyResource between those two layouts is not a valid
        // copy, and D3D12 does not refuse it -- it just produces garbage. Every probe read on the
        // result came back min -3e38, max 2e36, mean NaN, which then sailed through the "min is
        // not max, so it is real depth" check below and was handed to the network as depth.
        sd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
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
#if AMDNR_WITH_VULKAN
namespace vkroute { void ReleaseSwapchainSized(); }
#endif
#if AMDNR_WITH_OPENGL
namespace glroute { void ReleaseSwapchainSized(); }
#endif

void ReleaseSwapchainSized()
{
    WaitForWorkQueue(g.completion);
#if AMDNR_WITH_OPENGL
    // The imported GL textures and their FBOs have the same lifetime as the D3D12 resources
    // below. Unlike the Vulkan route this one may be called with no GL context on the thread --
    // a swapchain can be destroyed from anywhere -- and it checks for that itself.
    glroute::ReleaseSwapchainSized();
#endif
#if AMDNR_WITH_VULKAN
    // The imported VkImages have the same lifetime as the D3D12 resources below. Invalidate the
    // route even when the replacement swapchain keeps the same size and format, otherwise its
    // fast path returns with crossLocal already released.
    vkroute::ReleaseSwapchainSized();
#endif
    // Closed command lists retain references to their recorded resources until Reset. Retire all
    // slots while the queue is idle so no recording from the old swapchain survives the teardown.
    if (g.workDevice != nullptr)
    {
        for (UINT i = 0; i < State::kRing; ++i)
        {
            if (RecreateWorkSlot(i))
                continue;
            g.bridgeFailed = true;
            g.unavailable = true;
            g.reason = "could not retire the D3D12 work slots during swapchain teardown";
            break;
        }
    }
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
        guide->coldFrames = 0;
    }
    g_depthTally.clear();
    g_motionTally.clear();
    g_d12DepthTally.clear();
    g_d12DepthCold = 0;
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

// The bound key as the user's keyboard layout spells it. See hotkey_capture.h.
std::string HotkeyName()
{
    return hotkey::Name(g.toggleKey.load(), g.toggleMods.load());
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

// The Model B and Model C coefficients, and the strength scaling DLSSNR applies to them.
//
// These three numbers are not tuned here. They are the descriptor table in nvngx_dlssnr.dll,
// read out of the image at record+108 (mask 0x34) and record+176 (mask 0x20), feeding slots 75,
// 77 and 78 of the style vector -- exposure in stops, contrast towards a smoothstep, and HSV
// saturation. docs/styles-model-abc.md has the disassembly that says which is which.
//
// The neutral value of all three slots is zero, so the runtime's (value - neutral) * t + neutral
// reduces to value * t. Returns whether anything is actually being applied, so Style=0 and
// StyleStrength=0 both skip the work instead of running an identity.
// There is no style input on this runtime. 97b3c ("Scale", default 1/32) was taken for one on
// 21/09 because it sits right after the four control floats in the engine object
// (0x96F98..0x96FA4: tone, structure, skinEff, structEff) and the NVIDIA forward takes a fifth
// value, style/128. It is not: the engine's own dump line prints "ctl (%.2f %.2f %.2f %.2f)",
// four values, and the fifth float (0x96FA8, object+48) goes to the post kernel that writes the
// output, not to the network. Writing style/128 there made Model A (0) return the input
// unchanged -- measured in ETS2: residual mean 0.00024 against an input mean of 0.45 -- and cut
// Models B and C to a quarter and a half. So on AMD a Model is its grade and nothing else; the
// network half of it has no slot to reach.
//
// How much of the model's grade is applied. The NVIDIA DLL scales a style's coefficients by
// LocalToneStrength clamped to [0,1]; Model Strength sits on top of that, so at 1 the grade is
// exactly what the runtime would do with the same Tone.
float StyleGradeStrength()
{
    return g.styleStrength.load() * std::clamp(g.tone.load(), 0.0f, 1.0f);
}

bool StyleCoefficients(int style, float strength, float &expo, float &con, float &sat)
{
    expo = con = sat = 0.0f;
    const float t = std::clamp(strength, 0.0f, 1.0f);
    if (style == 1)        { expo = -0.10f * t; con = -0.25f * t; sat = -0.10f * t; }
    else if (style == 2)   { sat = -0.15f * t; }
    else                   return false;
    return t > 0.0f;
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

// Whether the controls a pass is about to receive differ from the ones it received last time.
// This is CG2R_ResetTemporalHistoryOnControlChange, measured on the ETS2 trace rather than read:
// the NVIDIA DLL drops the history when Style, UseAutoMask, LocalTone, LocalStructure or Skin
// change -- ints exactly, floats beyond 1e-5 -- and keeps it when Intensity does. Checked where
// the values are written, so every road to them (overlay, ini reload, per-pass profile, taper,
// the override) is one comparison instead of a reset in every handler. The first observation
// only records; there is no history to drop yet.
bool ControlsChanged(UINT slot, const PassTune &t, float outScale, int autoMask)
{
    struct Ctl { float tone, structure, skin, scale; int mask; bool seen; };
    static Ctl last[State::kMaxPasses] {};
    Ctl &l = last[std::min(slot, State::kMaxPasses - 1)];
    const auto moved = [](float a, float b) { return std::fabs(a - b) > 1e-5f; };
    const bool changed = l.seen && (moved(l.tone, t.tone) || moved(l.structure, t.structure) ||
                                    moved(l.skin, t.skin) || moved(l.scale, outScale) ||
                                    l.mask != autoMask);
    l = { t.tone, t.structure, t.skin, outScale, autoMask, true };
    return changed;
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
    // 6 rather than 2 so the shader can tell the mode from the debug view: same buffer, but the
    // mode takes the Model's grade and the view does not.
    const int dbg = g.debugView.load() != 0 ? g.debugView.load() : (g.networkOutput.load() ? 6 : 0);
    if (dbg == 2 || dbg == 6)
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
            Log("motion: %s, %llux%u -> %ux%u, scale %.3f. The estimator is "
                "off. Check Debug View \"Motion vectors\" while panning: the field should follow "
                "the camera, and MotionScale flips or rescales it if it does not.",
                g.guideMotion.external ? "an optical-flow shader, through AMD_Neural_Feed.fx"
                                       : "the game's own vectors",
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
            const float dscale = g.depthNormalise.load() ? g.depthScale.load() : 1.0f;
            std::memcpy(&ddims[4], &dscale, sizeof(float));
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
        const UINT slot = std::min(i, State::kMaxPasses - 1);
        HMODULE r = RuntimeFor(slot);
        // Temporal history. The network is a denoiser: without a previous result to carry
        // forward it starts from nothing every frame, and a motion vector -- which says where a
        // pixel *was* -- has nothing to point at. This is the pair that turns motion from an
        // input the engine merely reports into one it can use.
        //
        // Off by default because these are hardcoded offsets into one specific build: a wrong
        // pointer here does not fail, it hangs the game.
        // This pass's own previous output, not the chain's. See the declaration of history[].
        //
        // Decided before the history is looked at, so a control that changed this frame drops
        // it now and not one frame late. See ControlsChanged for which controls and why.
        const PassTune tune = TuningFor(i);
        const float outScale = g.engineScale.load();
        const int autoMask = g.autoMask.load();
        if (ControlsChanged(slot, tune, outScale, autoMask))
        {
            g.historyValid.store(0);
            Log("pass %u: reset temporal history after control change (tone %.2f, structure %.2f, "
                "skin %.2f, automask %d, output scale %.5f)",
                i + 1, static_cast<double>(tune.tone), static_cast<double>(tune.structure),
                static_cast<double>(tune.skin), autoMask, static_cast<double>(outScale));
        }
        const bool wantHistory = g.useHistory.load() &&
                                 (g.historyValid.load() & (1u << slot)) != 0 &&
                                 g.history[slot] != nullptr;
        At<uint8_t>(r, rt::kHistoryOn) = wantHistory ? 1 : 0;
        At<void *>(r, rt::kHistory) =
            wantHistory ? static_cast<void *>(g.history[slot].Get()) : nullptr;
        if (wantHistory && !g.loggedHistory)
        {
            g.loggedHistory = true;
            Log("history: handing each pass its own previous output at %ux%u -- pass %u reads "
                "what pass %u wrote last frame, not what the chain ended on. Watch the engine "
                "log: it says history off there when it is ignoring this.",
                g.netWidth, g.netHeight, slot + 1, slot + 1);
        }
        // 97b1d is Temporal, not "motion is valid" -- the engine's own ini reader reads the
        // key "Temporal" into this byte. The old name was a guess and it made the session-2
        // measurement look unexplained: Temporal=1 was the only run where the engine reported
        // non-zero motion, which is not a coincidence, it is what temporal accumulation is for.
        // Auto still follows haveMotion, which is the sane default; the other two are explicit.
        const int tm = g.temporalMode.load();
        At<uint8_t>(r, rt::kTemporal) =
            static_cast<uint8_t>(tm == 1 ? 0 : tm == 2 ? 1 : (haveMotion ? 1 : 0));
        // Never written before. UseAutoMask is the engine's own character masking -- the same
        // field RenoDX exposes as "Character Mask" -- and it defaults to 1, so the add-on was
        // silently relying on the default. ToneChannels and Scale were not known to exist.
        At<int>(r, rt::kUseAutoMask) = autoMask;
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
        //
        // Bit 4 also has to be set for another reason, read in the worker (0x180019070): when the
        // whole ToneChannels word is 0 the runtime zeroes LocalTone and LocalStructure before they
        // reach the network. Writing this field as 0 would silently run the network with no tone
        // and no structure control, whatever the sliders say.
        At<int>(r, rt::kToneChannels) = (g.toneChannels.load() & ~2) | 4;
        // 97b3c IS a scale, and it is not a control of the network. It lands at object+48
        // (0x96FA8), right after the four control floats, and from there it goes to the post
        // kernel that writes the output (sub_18002D2D0, the second off_18006BC68 launch, argument
        // after the history pointer), not to the pre kernel that feeds the network. Between 21/09
        // 22:24 and 22/09 00:04 this line wrote style/128 here, on the reading that it was the
        // NVIDIA forward's fifth control. Measured in ETS2 with Model A (0 here): the network
        // returned its input, residual mean 0.00024 against an input of 0.45, and nothing on
        // screen changed with the effect, the Model or the pass count. Model B and C were at a
        // quarter and a half of the effect. The runtime's own default, 1/32, is what goes here;
        // LoadSettings refuses anything near zero.
        At<float>(r, rt::kScale) = outScale;
        At<int>(r, rt::kTonemap) = RuntimeTonemap();
        At<uint8_t>(r, rt::kInlineMode) = g.inlineMode.load() ? 1 : 0;
        At<uint8_t>(r, rt::kUseDepth) = haveDepth ? 1 : 0;
        // 97b10 DepthInverted. Both runtimes boot this at 1 -- the NVIDIA DLL writes options+260
        // = 1 when the parameter is absent, and the AMD port's static initialiser sets
        // dword_180076E10 = 1. RenoDX sends 0 explicitly, measured on ETS2 where its depth was a
        // dummy, so neither value has been shown right for a real buffer yet. Default 1, exposed
        // under Depth so the comparison can be made.
        At<UINT>(r, rt::kDepthInverted) = g.depthInverted.load() != 0 ? 1u : 0u;
        At<uint8_t>(r, rt::kFsrFlagsSeen) = 1;
        // All three come from one place now, and that place is per-pass. Local Tone is written on
        // the first pass only -- which is what the original `i == 0 ? tone : 0.0f` here did, and
        // last session removed it as an asymmetry nobody had chosen. Somebody had: the reference
        // fork's PassProfiles.h makes exactly that choice, in one line, deliberately.
        At<float>(r, rt::kLocalTone) = tune.tone;
        At<float>(r, rt::kLocalStructure) = tune.structure;
        At<float>(r, rt::kSkinStructure) = tune.skin;

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
        // The marker at 97a60 cannot answer that on its own: pass 1 sets it to cmd, so on pass 2
        // the equality test below is comparing cmd against cmd whatever the engine did, and a
        // silently refused pass 2 would be counted as accepted. The job id is the field that
        // changes per evaluation, so an id that does not move is a pass that did not run.
        const UINT jobBefore = At<UINT>(r, rt::kJobId);
        reinterpret_cast<RecordFn>(reinterpret_cast<uintptr_t>(r) + rt::kRecordFn)(&packet);
        const UINT jobAfter = At<UINT>(r, rt::kJobId);

        if (At<uint8_t>(r, rt::kNativeFailure) != 0)
        {
            nativeFailure = true;
            g.failed = true;
            Log("pass %u reported a native failure. Stopping.", i + 1);
            break;
        }
        if (At<ID3D12CommandList *>(r, rt::kListMarker) != cmd)
        {
            if (++g.skipped % 600 == 1)
                Log("pass %u refused (%llu total)", i + 1,
                    static_cast<unsigned long long>(g.skipped));
            break;
        }
        g.lastJob = jobAfter;
        for (UINT m = 0; m < State::kMaxPasses; ++m)
            if (g.runtimes[m] == r)
                g.lastJobs[m] = jobAfter;
        g.recordedMask |= 1u << (r == g.runtime ? 0 : slot);
        // v0.3.0: 0x97950 and 0x97954 are two watchdog job counters, NOT a
        // host pointer to an abort word. Its watchdog (0x1b27a/0x1b281) writes
        // a job id to each DWORD when a timeout occurs. Interpreting the pair
        // as a pointer then writing through it crashes on the next recording
        // (reproduced at frame 28 in framecheck, against the v0.2.17 pair at
        // 0x8d808/0x8d80c). The runtime owns resetting the real GPU abort flag
        // -- v0.2.17 did it with hipMemcpyAsync, v0.3.0 stores straight through
        // its own host pointer; either way, leave it to do so.
        ++accepted;

        // Keep what this pass produced as this pass's history for the next frame. Recorded on
        // the same list, straight after the pass, so on the GPU it reads what the pass wrote and
        // lands before the next pass overwrites netColour. Doing it once after the loop -- which
        // is what this used to do -- could only ever capture the last pass, so every earlier
        // pass was handed a reference belonging to a different stage of the chain.
        if (g.useHistory.load() && g.history[slot] != nullptr)
        {
            Barrier(cmd, g.netColour.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cmd, g.history[slot].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(g.history[slot].Get(), g.netColour.Get());
            Barrier(cmd, g.history[slot].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, g.netColour.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g.historyValid.fetch_or(1u << slot);
        }

        // Reported, not enforced. Whether the engine bumps the job id once per recording or once
        // per submission is not established, so acting on this would risk breaking out of the
        // loop on a pass that was in fact fine -- which is exactly the failure being fixed. It
        // prints instead, and the log then says plainly whether a second pass is real work.
        if (wanted > 1 && !g.loggedPassDetail)
            Log("pass %u of %u: job id %u -> %u (%s), list marker %s", i + 1, wanted, jobBefore,
                jobAfter, jobAfter != jobBefore ? "moved, the engine recorded something"
                                               : "DID NOT MOVE -- this pass may be a no-op",
                At<ID3D12CommandList *>(r, rt::kListMarker) == cmd ? "ours" : "not ours");

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
    {
        g.lastJobAt = GetTickCount64();
        g.jobRunning = true;
    }
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
    // Once in full at frame 240 (or on request), then silently every 1800 frames for the inert
    // watchdog, which only speaks when its verdict changes.
    if (g.activePasses != 0 &&
        ((!g.measured && (g.measureNow.exchange(false) || (g.frame >= 240 && g.frame % 240 == 0))) ||
         (g.measured && g.frame % 1800 == 0)))
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
    float gexp = 0.0f, gcon = 0.0f, gsat = 0.0f;
    StyleCoefficients(g.style.load(), StyleGradeStrength(), gexp, gcon, gsat);
    // On a frame the network sat out, the residual still holds the last one's answer. Compose
    // runs anyway -- a selected style has to reach every frame that is shown, or it flickers --
    // but with no correction, because one aimed at where the picture used to be reads as a trail.
    // Zero intensity makes every step of the composition the identity, additive or ratio alike,
    // so what comes out is the game's own frame with the grade on it. A debug view is exempt for
    // the reason CompositionIsFresh gives: what it draws is a buffer, not a correction.
    const bool stale = !(runNetwork && g.activePasses != 0) && dbg == 0 && !g.networkOutput.load();
    const float composeStrength = stale ? 0.0f : strength;
    // v0.6.0's packing, untouched: bit 0 is the filter and everything above it is the debug
    // view, so there is no spare bit here and the style does not take one.
    UINT cdims[15] { w, h, nw, nh, static_cast<UINT>(encMode), 0, 0,
                     (g.bicubic.load() ? 1u : 0u) | (static_cast<UINT>(dbg) << 1),
                     0, 0, 0, 0, 0, 0, 0 };
    std::memcpy(&cdims[5], &kWhite, sizeof(float));
    std::memcpy(&cdims[6], &composeStrength, sizeof(float));
    const float rlimit = g.residualLimit.load(), rfade = g.residualFade.load();
    std::memcpy(&cdims[8], &rlimit, sizeof(float));
    std::memcpy(&cdims[9], &rfade, sizeof(float));
    const float cstrength = g.colourStrength.load();
    const float guardEff = EffectiveGuard();
    std::memcpy(&cdims[10], &cstrength, sizeof(float));
    std::memcpy(&cdims[11], &guardEff, sizeof(float));
    std::memcpy(&cdims[12], &gexp, sizeof(float));
    std::memcpy(&cdims[13], &gcon, sizeof(float));
    std::memcpy(&cdims[14], &gsat, sizeof(float));
    cmd->SetComputeRoot32BitConstants(1, 15, cdims, 0);
    cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);

    if (outTarget != nullptr)
        cmd->CopyResource(outTarget, g.composed.Get());
    Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Each pass took its own copy as it finished, so there is nothing to capture here any more.
    // What is left is the invalidation, which is still a whole-chain decision.
    if (!(g.useHistory.load() && g.activePasses != 0))
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
        g.historyValid.store(0);
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

// The first engine, plus one copy per further pass. Copies only in serial inline mode: that is
// the only mode in which each pass is submitted and finished on its own, which is what lets
// each module be told about exactly the list it recorded. The legacy batch and async paths keep
// the single shared module they always had. A copy that fails to come up clamps the pass count
// to what did, and says so once, rather than silently running that pass through pass 1's state.
bool BringUpEngines(UINT &wanted)
{
    if (!InitPipeline() || !InitEngine())
        return false;
    if (!(g.serialPasses.load() && g.inlineMode.load()))
        return true;
    for (UINT slot = 1; slot < wanted; ++slot)
    {
        if (g.runtimes[slot] != nullptr)
            continue;
        if (!LoadExtraRuntime(slot))
        {
            static UINT saidFor = 0;
            if (saidFor != wanted)
            {
                saidFor = wanted;
                Log("pass count clamped to %u: no separate runtime for pass %u. See the lines "
                    "above for why.", slot, slot + 1);
            }
            wanted = slot;
            break;
        }
    }
    return true;
}

#if AMDNR_WITH_VULKAN
#include "vk_route.inc"
#endif
#if AMDNR_WITH_OPENGL
#include "gl_route.inc"
#endif

// Which D3D12 buffer is the scene depth, decided once per present.
//
// Three things went wrong with "largest wins", all measured on Cyberpunk 2077 at 1129x706 into a
// 1920x1200 swapchain: it took a 2048x2048 shadow map; with a shape filter added it took a
// 1920x1200 buffer the game drew into 7 times a frame over the scene depth it drew into ~2,400
// times; and when two buffers were the same size it settled on the one the game never clears, so
// the pre-clear snapshot froze on a stale frame.
//
// So: count binds, count clears, and when anything screen-shaped was cleared this present, only
// cleared buffers are eligible. A challenger needs a clear margin over the incumbent, and with no
// incumbent three presents are added up first -- the same rule, and the same reason, as
// SettleGuide. Caller holds g.lock.
void SettleD3D12Depth()
{
    bool anyCleared = false;
    for (const auto &entry : g_d12DepthTally)
        if (entry.second.clears > 0)
        {
            anyCleared = true;
            break;
        }

    const D12Depth *best = nullptr;
    for (const auto &entry : g_d12DepthTally)
    {
        if (anyCleared && entry.second.clears == 0)
            continue;
        if (best == nullptr || entry.second.binds > best->binds)
            best = &entry.second;
    }
    if (best == nullptr)
    {
        g_d12DepthTally.clear();
        return;
    }

    if (best->res.Get() == g.depthBest.Get())
    {
        g.depthBestBinds = best->binds;
        g_d12DepthCold = 0;
        g_d12DepthTally.clear();
        return;
    }
    if (g.depthBest == nullptr)
    {
        if (++g_d12DepthCold < 3)
            return;  // deliberately not cleared: three presents of binds add up
        g_d12DepthCold = 0;
    }
    else if (best->binds <= g.depthBestBinds + g.depthBestBinds / 4)
    {
        g_d12DepthTally.clear();  // no margin, so the incumbent keeps the slot
        return;
    }

    g.depthBest = best->res;
    g.depthWidth = best->width;
    g.depthHeight = best->height;
    g.depthFormat = best->format;
    g.depthBinds = g.depthBestBinds = best->binds;
    // The snapshot is deliberately NOT released here. A dispatch recorded into the game's command
    // list still reads it, and D3D12 does not keep a resource alive because an in-flight list
    // references it -- the rule EnsureResources spells out, and it waits for the queue before it
    // drops anything. This runs at the top of present with no such wait, and the whole reason the
    // job bookkeeping exists is that a job routinely spans several presents. Nothing needs to be
    // freed anyway: the next clear of the new buffer copies over it, and OnClearDepth rebuilds it
    // when the size or format actually changes.
    Log("depth (D3D12): taking %ux%u format %d, bound %u times a present and cleared %u",
        best->width, best->height, static_cast<int>(best->format), best->binds, best->clears);
    g_d12DepthTally.clear();
}

// The companion effect's textures are ReShade's, and only ReShade knows where they live. It
// hands over the runtime here; the guides are taken from it at present, once the whole effect
// chain has run, so what crosses is this frame's field rather than last frame's.
void OnInitEffects(effect_runtime *runtime)
{
    std::lock_guard guard(g.lock);
    g.effects = runtime;
    g.feedSignature = -1;
}

// No lock and no work: this runs twice per frame on the render thread, and all it does is mark
// the window in which a render-target bind belongs to ReShade rather than to the game. The
// runtime pointer is taken in OnInitEffects, where the lock is already held.
void OnBeginEffects(effect_runtime *, command_list *, resource_view, resource_view)
{
    g.inEffects.store(true);
}

void OnFinishEffects(effect_runtime *, command_list *, resource_view, resource_view)
{
    g.inEffects.store(false);
}

void OnDestroyEffects(effect_runtime *runtime)
{
    std::lock_guard guard(g.lock);
    if (g.effects != runtime)
        return;
    g.effects = nullptr;
    g.feedSignature = -1;
    // The guides point into textures that runtime owned. Letting them stand would copy from
    // freed memory on the next present.
    for (Guide *guide : { &g.guideMotion, &g.guideDepth })
        if (guide->external)
        {
            guide->external = false;
            guide->chosen.Reset();
            guide->ready = false;
        }
}

void OnPresent(command_queue *queue, swapchain *sc, const rect *, const rect *, uint32_t,
               const rect *)
{
    const Profile &profile = ProfileForThisProcess();
    std::lock_guard guard(g.lock);
    // Which buffer is the scene depth is decided here, once a present, on a whole frame's worth
    // of binds and clears. On D3D11 the equivalent is SettleGuide, a few lines into the bridge.
    SettleD3D12Depth();
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
            g.historyValid.store(0);
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
        g.historyValid.store(0);
        // And the job clock with it. jobRunning is a latch: set when a job is submitted, read on
        // the next present that finds the job finished. Across a pause -- minimised, disabled,
        // alt-tabbed -- no present runs, so the next one measures the whole pause and calls it a
        // network job. Three of those and the scale would be capped for having been alt-tabbed.
        g.jobRunning = false;
        Log("window restored; dropping the temporal history so nothing from before the alt-tab "
            "is carried into the new frame.");
    }

#if AMDNR_WITH_VULKAN
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

#if AMDNR_WITH_OPENGL
    // OpenGL. Same answer again -- our own D3D12 device, shared textures imported into the host --
    // with the crossing rebuilt around a framebuffer blit, because ReShade hands an OpenGL add-on
    // the default framebuffer rather than a texture and there is nothing to copy. See
    // gl_route.inc.
    if (dev->get_api() == device_api::opengl)
    {
        if (g.noBridge.load() || g.goneSwapchain.load() == sc)
            return;
        if (!LoadGraphicsApi())
        {
            g.unavailable = true;
            g.reason = "the D3D12 or DXGI entry points could not be resolved";
            return;
        }
        glroute::Present(queue, sc);
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
            g.reason = "the bridge stopped completing frames; see amd-nr.log";
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
                g.reason = "the bridge kept failing to rebuild; see amd-nr.log";
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
            Log("unsupported graphics API %u; transports compiled into this build: Vulkan %d, "
                "OpenGL %d", static_cast<unsigned>(dev->get_api()), AMDNR_WITH_VULKAN,
                AMDNR_WITH_OPENGL);
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
    if (!EnsureResources(static_cast<UINT>(bd.Width), bd.Height, bd.Format, EffectiveScale()))
    {
        g.unavailable = true;
        if (*g.reason == '\0')
            g.reason = "could not create the working textures; see amd-nr.log";
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
        RuntimeBusy();
    if (!jobPending && g.jobRunning)
    {
        g.jobRunning = false;
        NoteJobCost(GetTickCount64() - g.lastJobAt);
    }
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
            ResetJobs();
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
        NotifyRuntimes(g.queue.Get(), 1, submitted);
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
        NotifyRuntimes(g.queue.Get(), 1, submitted);
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

// The Export logs button: this run's log, the runtime's, ReShade's and the ini, all beside the
// game. See log_export.h, shared with the 32-bit bridge.
std::wstring ExportLogs()
{
    const auto r = logexport::ToDesktop({ ExeDirectory() }, { L"amd-nr.log", L"dlssnr_on_amd.log",
                                                              L"ReShade.log", L"amd-nr.ini" });
    if (r.copied != 0)
        Log("menu: exported %d file(s) to %ls", r.copied, r.folder.c_str());
    return r.folder.wstring();
}

// What the panel shows and does not own, read under the lock the present path holds while it
// changes these. Copied out so drawing never happens with the lock held.
PanelStatus ReadPanelStatus()
{
    PanelStatus st;
    std::lock_guard guard(g.lock);
    st.run = g.unavailable ? RunState::Unavailable : g.failed ? RunState::Error : RunState::Ready;
    st.reason = g.reason;
    st.processed = g.frame;
    st.skipped = g.skipped;
    st.routeNote = ProfileForThisProcess().note;
    st.outWidth = g.outWidth;
    st.outHeight = g.outHeight;
    st.netWidth = g.netWidth;
    st.netHeight = g.netHeight;
    st.scaleCap = g.scaleCap.load();
    st.depthSource = g.guideDepth.external && g.gameDepthActive ? GuideSource::Effect
                     : g.gameDepthActive                        ? GuideSource::Game
                     : g.depthSnapshot                          ? GuideSource::Snapshot
                                                                : GuideSource::None;
    st.motionSource = g.guideMotion.external && g.gameMotionActive ? GuideSource::Effect
                      : g.gameMotionActive                         ? GuideSource::Game
                                                                   : GuideSource::Estimated;
    st.gameMotionActive = g.gameMotionActive;
    st.stillPct = g.probeStillPct.load();
    st.depthMin = g.probeDepthMin.load();
    st.depthMax = g.probeDepthMax.load();
    st.stage = g.stage.load();
    st.events = g.events;
    st.noBridge = g.noBridge.load();
    st.noBackBuffer = g.noBackBuffer.load();
    st.hotkeyName = HotkeyName();
    st.hasFeedEffect = true;
    st.feedStatus = g.feedStatus;
    return st;
}

// The panel's plain copy of the settings, filled from the engine's atomics.
PanelSettings ReadPanelSettings()
{
    PanelSettings s;
#define X(type, name, low, high) s.name = static_cast<type>(g.name.load());
#include "../x86bridge/settings_fields.inc"
#undef X
    for (int i = 0; i < kMaxPasses; ++i)
    {
        s.passOverride[i] = g.passOverride[i].load() ? 1u : 0u;
        s.passStructure[i] = g.passStructure[i].load();
        s.passTone[i] = g.passTone[i].load();
        s.passSkin[i] = g.passSkin[i].load();
    }
    s.useFeedEffect = g.useFeedEffect.load() ? 1u : 0u;
    return s;
}

// The settings as the engine constructs them, before amd-nr.ini is read: what a fresh ini is
// written with, and so what Factory Defaults restores. Captured once, in DllMain.
PanelSettings g_factory;

// What the panel changed, written back to the engine. Only the fields that moved: the render thread
// writes some of these atomics too -- the scale cap's partner fields, the history flag -- and a
// whole-struct write would put back what the panel read before it did.
void ApplyPanelSettings(const PanelSettings &before, const PanelSettings &after)
{
#define X(type, name, low, high)                                                                   \
    if (after.name != before.name)                                                                 \
    {                                                                                              \
        g.name.store(static_cast<decltype(g.name.load())>(after.name));                           \
        Log("menu: " #name " %g -> %g", static_cast<double>(before.name),                          \
            static_cast<double>(after.name));                                                      \
    }
#include "../x86bridge/settings_fields.inc"
#undef X
    for (int i = 0; i < kMaxPasses; ++i)
    {
        if (after.passOverride[i] != before.passOverride[i])
        {
            g.passOverride[i].store(after.passOverride[i] != 0);
            Log("menu: pass %d profile %s", i + 1, after.passOverride[i] != 0 ? "on" : "off");
        }
        if (after.passStructure[i] != before.passStructure[i])
            g.passStructure[i].store(after.passStructure[i]);
        if (after.passTone[i] != before.passTone[i])
            g.passTone[i].store(after.passTone[i]);
        if (after.passSkin[i] != before.passSkin[i])
            g.passSkin[i].store(after.passSkin[i]);
    }
    if (after.useFeedEffect != before.useFeedEffect)
    {
        g.useFeedEffect.store(after.useFeedEffect != 0);
        g.feedSignature = -1;  // take whatever the effect hands over next as new, not as a repeat
        Log("menu: companion effect %s", after.useFeedEffect != 0 ? "on" : "off");
    }
    // A raster of a new size, a depth read the other way round, or history switched: last frame's
    // output no longer means what the next frame's motion vectors assume, so do not carry it.
    if (after.scale != before.scale || after.depthInverted != before.depthInverted ||
        after.useHistory != before.useHistory)
        g.historyValid.store(0);
}

void HandlePanelActions(const PanelActions &actions, const PanelSettings &after)
{
    if (actions.Has(PanelAction::FactoryDefaults))
    {
        ApplyPanelSettings(after, KeepPreferences(g_factory, after));
        Log("menu: factory defaults restored; enabled, startup, hotkey, language and the panel's "
            "arrangement kept");
    }
    if (actions.Has(PanelAction::Save))
        SaveSettings();
    if (actions.Has(PanelAction::Reload))
    {
        LoadSettings();
        Log("menu: settings reloaded from the ini");
    }
    if (actions.Has(PanelAction::LiftScaleCap))
    {
        // Letting go of Scale overrules a cap NoteJobCost put on. If this card still cannot carry
        // it, three long jobs put the cap back.
        const float cap = g.scaleCap.exchange(0.0f);
        g.longJobs = 0;
        if (cap > 0.0f)
            Log("menu: resolution scale %.2f; the automatic cap is lifted and it runs at that until "
                "the network is measured too slow for it again.", static_cast<double>(after.scale));
    }
    if (actions.Has(PanelAction::MeasureResidual))
    {
        g.measured = false;
        g.measureTries = 0;
        g.measureNow.store(true);
        Log("menu: residual measurement re-armed");
    }
}

// The overlay, rebuilt 22/09/2026. It used to carry 47 controls across eight headers; it carries
// fifteen across five now. Nothing was deleted: every atomic, every LoadSettings line and every
// SaveSettings line is untouched, so each hidden control still reads its key out of amd-nr.ini and
// still writes it back. What went is the widget, and with it the chance of somebody dragging a
// slider whose effect nobody here has established into a state that makes the add-on look broken.
//
// The panel itself is src/ui/, shared with the 32-bit bridge. This is the 64-bit side of it: fill
// the panel's copy from the engine, draw, write back what changed, carry out what was asked.
void OnOverlay(effect_runtime *runtime)
{
    // Rebinding by capturing a real keypress. Only advances while the overlay is open, which is
    // where the button is.
    static hotkey::Capture capture;
    static std::wstring exported;
    static bool exportFailed = false;

    PanelStatus status = ReadPanelStatus();
    status.hotkeyArmed = capture.armed;
    status.exportedPath = exported;
    status.exportFailed = exportFailed;
    PanelSettings panel = ReadPanelSettings();
    const PanelSettings before = panel;

    const PanelActions actions = DrawPanel(panel, status);

    ApplyPanelSettings(before, panel);
    HandlePanelActions(actions, panel);
    if (actions.Has(PanelAction::ToggleHotkeyCapture))
        capture.Toggle();
    // ReShade's key state, never GetAsyncKeyState: it hooks that one and answers 0 for every key
    // while the overlay is blocking the keyboard, which is the whole time this panel is open. See
    // hotkey_capture.h.
    int boundKey = 0, boundMods = 0;
    if (capture.Poll([runtime](int vk) { return runtime->is_key_down(static_cast<uint32_t>(vk)); },
                     boundKey, boundMods))
    {
        g.toggleKey.store(boundKey);
        g.toggleMods.store(boundMods);
        Log("menu: toggle bound to %s", HotkeyName().c_str());
    }
    if (actions.Has(PanelAction::ExportLogs))
    {
        SaveSettings(/*quiet=*/true);   // so the ini beside the logs is the run they describe
        exported = ExportLogs();
        exportFailed = exported.empty();
    }

    // Autosave. The Save button stays -- it is still the only thing that says out loud that a write
    // happened -- but nothing should be lost because somebody never scrolled this far. Written when
    // a control settles rather than while it is being dragged: WritePrivateProfileString rewrites
    // the whole file once per key, so a slider held down would be fifty full rewrites a second.
    static uint64_t saved = SettingsFingerprint();
    if (const uint64_t now = SettingsFingerprint(); now != saved && !ImGui::IsAnyItemActive())
    {
        SaveSettings(/*quiet=*/true);
        saved = now;
        static bool said = false;
        if (!said)
        {
            Log("settings autosaved to amd-nr.ini; every later change saves itself the same way.");
            said = true;
        }
    }
}

}

extern "C" __declspec(dllexport) const char *NAME = "AMD Neural Rendering";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Runs DLSS-NR on AMD with HIP 7. D3D11/D3D12"
#if AMDNR_WITH_VULKAN
    ", experimental Vulkan"
#endif
#if AMDNR_WITH_OPENGL
    ", experimental OpenGL"
#endif
    ". Bounded-ratio composition, up to three passes.";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module))
            return FALSE;
        {
            const auto log = ExeDirectory() / L"amd-nr.log";
            g_log = _wfopen(log.c_str(), L"w");
            Log("AMD Neural Rendering: %s", ProfileForThisProcess().note);
            Log("preview 2026-09-10: SDR input contract, serialized inline passes; Vulkan %d, "
                "OpenGL %d", AMDNR_WITH_VULKAN, AMDNR_WITH_OPENGL);
            // Before the read, so a first run has a documented file to read and the user has
            // something to edit without being told which keys exist. On the run that writes it,
            // it has already read the settings for the reason its own comment gives, and a second
            // read here would only re-parse what it just wrote.
            g_factory = ReadPanelSettings();  // the constructed defaults, before any ini is read
            if (!EnsureNeuralIni())
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
        reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitEffects);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffects);
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(OnBeginEffects);
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
        if (g.events & 16)
            reshade::register_overlay("AMD Neural Rendering", OnOverlay);
        Log("events subscribed: mask %d", g.events);
#if AMDNR_WITH_VULKAN
        // Has to happen here and not at the first present: the host's VkDevice is created when a
        // game boots, and by the time a frame is presented it is far too late to change what that
        // device was created with. Patches one import-table entry and does nothing at all in a
        // process that has no static vkCreateDevice import, which is every D3D11 and D3D12 target.
        if (!g.noBridge.load())
            vkroute::devicehook::Install();
#endif
        break;
    case DLL_PROCESS_DETACH:
#if AMDNR_WITH_VULKAN
        // At process termination Windows is already tearing every module down. MinHook removal
        // suspends threads, which is useful for an explicit unload but unsafe under the loader
        // lock while the process is exiting.
        if (reserved == nullptr)
            vkroute::devicehook::Remove();
#endif
        if (g.events & 16)
            reshade::unregister_overlay("AMD Neural Rendering", OnOverlay);
        reshade::unregister_addon(module);
        if (g_log != nullptr)
            fclose(g_log), g_log = nullptr;
        break;
    }
    return TRUE;
}
