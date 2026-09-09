// ReShade add-on: measures the D3D11 -> D3D12 crossing for the real network inputs.
//
// Why this exists. The AMD network runtime is D3D12, but on D3D12 ReShade shows an add-on only
// the swapchain: two render targets, no depth. On D3D11 the same probe sees eight, including a
// real depth buffer and the colour target at render resolution, which is a better input than the
// presented back buffer. So the sources are on one API and the network is on the other.
//
// Before building that bridge for real, one number decides whether it is worth building at all:
// what it costs to carry colour and depth across, every frame. That is all this measures. It
// runs no network and changes no pixels.

#include <reshade.hpp>

#include <windows.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace reshade::api;

namespace
{
constexpr uint64_t kReportEvery = 120;

FILE *g_log = nullptr;
uint64_t g_frame = 0;
std::mutex g_lock;

void Log(const char *fmt, ...)
{
    if (g_log == nullptr)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

std::string GamePath(const char *name)
{
    char path[MAX_PATH] {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char *slash = strrchr(path, '\\'))
        *(slash + 1) = '\0';
    return std::string(path) + name;
}

template <class T> void Release(T *&p)
{
    if (p != nullptr)
    {
        p->Release();
        p = nullptr;
    }
}

double MsSince(LARGE_INTEGER t0)
{
    LARGE_INTEGER t1 {}, freq {};
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    return 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart);
}

// What the bind events turn up. Written on the render thread, read at present.
struct Seen
{
    ID3D11Resource *colour = nullptr;
    ID3D11Resource *depth = nullptr;
    UINT colourW = 0, colourH = 0, depthW = 0, depthH = 0;
    DXGI_FORMAT colourFmt = DXGI_FORMAT_UNKNOWN, depthFmt = DXGI_FORMAT_UNKNOWN;
    bool logged = false;
};
Seen g_seen;

// One shared texture: created on the game's D3D11 device, opened on our own D3D12 device.
// The copy happens on D3D11, so the texture has to live there.
struct Bridge
{
    const char *name = "";
    ID3D11Texture2D *on11 = nullptr;
    HANDLE handle = nullptr;
    ID3D12Resource *on12 = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    void Destroy()
    {
        Release(on12);
        if (handle != nullptr)
            CloseHandle(handle);
        handle = nullptr;
        Release(on11);
        width = height = 0;
        format = DXGI_FORMAT_UNKNOWN;
    }

    bool Ensure(ID3D11Device5 *game11, ID3D12Device *own, UINT w, UINT h, DXGI_FORMAT fmt,
                bool wantUav = false)
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
        // Copy destination, plus a UAV when a shader has to write into it. Measured: R32_FLOAT
        // shares fine with the UAV bind added; RENDER_TARGET is not needed by anything here.
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (wantUav ? D3D11_BIND_UNORDERED_ACCESS : 0);
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

        HRESULT hr = game11->CreateTexture2D(&td, nullptr, &on11);
        if (FAILED(hr))
        {
            Log("bridge %s: CreateTexture2D %ux%u fmt %u failed 0x%08lX -- this format may not be "
                "shareable, which would mean converting it on the D3D11 side first.",
                name, w, h, static_cast<unsigned>(fmt), hr);
            return false;
        }
        IDXGIResource1 *res = nullptr;
        if (FAILED(on11->QueryInterface(IID_PPV_ARGS(&res))))
        {
            Log("bridge %s: no IDXGIResource1", name);
            return false;
        }
        hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                     nullptr, &handle);
        res->Release();
        if (FAILED(hr))
        {
            Log("bridge %s: CreateSharedHandle failed 0x%08lX", name, hr);
            return false;
        }
        if (FAILED(own->OpenSharedHandle(handle, IID_PPV_ARGS(&on12))))
        {
            Log("bridge %s: OpenSharedHandle on our own device failed", name);
            return false;
        }
        width = w;
        height = h;
        format = fmt;
        Log("bridge %s ready: %ux%u fmt %u, shared D3D11 -> D3D12", name, w, h,
            static_cast<unsigned>(fmt));
        return true;
    }
};

struct Session
{
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Fence *localFence = nullptr;
    UINT64 localValue = 0;
    HANDLE localEvent = nullptr;

    ID3D12Fence *crossFence = nullptr;
    HANDLE crossHandle = nullptr;
    ID3D11Fence *crossOn11 = nullptr;
    UINT64 crossValue = 0;

    Bridge colour { "colour" };
    Bridge depth { "depth" };

    ID3D11Device5 *game11 = nullptr;
    ID3D11DeviceContext4 *game11ctx = nullptr;

    // PCSX2's depth is R32G8X24_TYPELESS, and D3D11 refuses to create a shared texture in that
    // format at all -- measured, E_INVALIDARG, it is not a permissions problem. So it gets read
    // through an SRV and written into an R32_FLOAT shared texture by a compute shader, which is
    // the format the network wants anyway. Compute rather than a fullscreen draw because the CS
    // pipeline state is separate: three binds to save and restore instead of a dozen, and no way
    // to leave the game's graphics state disturbed.
    ID3D11ComputeShader *depthCs = nullptr;
    ID3D11ShaderResourceView *depthSrv = nullptr;
    ID3D11UnorderedAccessView *depthUav = nullptr;
    ID3D11Resource *depthSrvOf = nullptr;

    bool failed = false;
    bool depthUnshareable = false;

    // Running cost, reset every report.
    double copySum = 0.0, copyMax = 0.0;
    double landSum = 0.0, landMax = 0.0;
    uint64_t copyN = 0, landN = 0;

    ~Session() { Destroy(); }

    void Destroy()
    {
        colour.Destroy();
        depth.Destroy();
        Release(depthUav);
        Release(depthSrv);
        Release(depthCs);
        depthSrvOf = nullptr;
        Release(crossOn11);
        Release(game11ctx);
        Release(game11);
        if (crossHandle != nullptr)
            CloseHandle(crossHandle);
        if (localEvent != nullptr)
            CloseHandle(localEvent);
        crossHandle = localEvent = nullptr;
        Release(crossFence);
        Release(localFence);
        Release(queue);
        Release(device);
    }

    bool CreateOn(LUID adapterLuid)
    {
        IDXGIFactory4 *factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;
        IDXGIAdapter1 *adapter = nullptr;
        const bool found = SUCCEEDED(factory->EnumAdapterByLuid(adapterLuid, IID_PPV_ARGS(&adapter)));
        factory->Release();
        if (!found)
        {
            Log("session: no adapter matched the game's LUID");
            return false;
        }
        DXGI_ADAPTER_DESC1 ad {};
        adapter->GetDesc1(&ad);
        const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        adapter->Release();
        if (FAILED(hr))
        {
            Log("session: D3D12CreateDevice failed 0x%08lX", hr);
            return false;
        }
        char name[128] {};
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
        Log("session: own D3D12 device on %s (vendor %04X)", name, ad.VendorId);

        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&localFence))))
        {
            Log("session: could not create the queue or fence");
            return false;
        }
        localEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&crossFence))) ||
            FAILED(device->CreateSharedHandle(crossFence, nullptr, GENERIC_ALL, nullptr, &crossHandle)))
        {
            Log("session: could not create the shared fence");
            return false;
        }
        return true;
    }

    // Builds the depth path once: the shader, the UAV over the shared R32_FLOAT texture, and an
    // SRV over whichever depth resource the game is currently using. Returns false once and then
    // stays false, so a game whose depth cannot be read does not retry every frame.
    bool EnsureDepthPath(ID3D11Resource *src, UINT w, UINT h, DXGI_FORMAT srcFmt)
    {
        if (depthCs == nullptr)
        {
            static const char kCs[] =
                "Texture2D<float4> src : register(t0);\n"
                "RWTexture2D<float> dst : register(u0);\n"
                "[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {\n"
                "  uint w, h; dst.GetDimensions(w, h);\n"
                "  if (p.x >= w || p.y >= h) return;\n"
                "  dst[p.xy] = src.Load(int3(p.xy, 0)).x;\n"
                "}\n";
            ID3DBlob *blob = nullptr, *err = nullptr;
            if (FAILED(D3DCompile(kCs, sizeof(kCs) - 1, "depth", nullptr, nullptr, "main", "cs_5_0",
                                  0, 0, &blob, &err)))
            {
                Log("depth: shader failed to compile: %s",
                    err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "?");
                Release(err);
                return false;
            }
            Release(err);
            const HRESULT hr = game11->CreateComputeShader(blob->GetBufferPointer(),
                                                           blob->GetBufferSize(), nullptr, &depthCs);
            blob->Release();
            if (FAILED(hr))
            {
                Log("depth: CreateComputeShader failed 0x%08lX", hr);
                return false;
            }
        }

        if (!depth.Ensure(game11, device, w, h, DXGI_FORMAT_R32_FLOAT, true))
            return false;
        if (depthUav == nullptr)
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud {};
            ud.Format = DXGI_FORMAT_R32_FLOAT;
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (FAILED(game11->CreateUnorderedAccessView(depth.on11, &ud, &depthUav)))
            {
                Log("depth: UAV over the shared texture failed");
                return false;
            }
        }

        if (src != depthSrvOf)
        {
            Release(depthSrv);
            depthSrvOf = nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
            sd.Format = (srcFmt == DXGI_FORMAT_R32G8X24_TYPELESS)
                            ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                            : (srcFmt == DXGI_FORMAT_R24G8_TYPELESS)
                                  ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                                  : (srcFmt == DXGI_FORMAT_R32_TYPELESS) ? DXGI_FORMAT_R32_FLOAT
                                                                         : srcFmt;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            const HRESULT hr = game11->CreateShaderResourceView(src, &sd, &depthSrv);
            if (FAILED(hr))
            {
                Log("depth: SRV over the game's depth buffer failed 0x%08lX (fmt %u read as %u). "
                    "The buffer was probably created without BIND_SHADER_RESOURCE.",
                    hr, static_cast<unsigned>(srcFmt), static_cast<unsigned>(sd.Format));
                return false;
            }
            depthSrvOf = src;
        }
        return true;
    }

    // Save exactly the three CS bindings this touches and put them back. PCSX2 owns this context.
    void ConvertDepth(UINT w, UINT h)
    {
        ID3D11ComputeShader *oldCs = nullptr;
        ID3D11ShaderResourceView *oldSrv = nullptr;
        ID3D11UnorderedAccessView *oldUav = nullptr;
        game11ctx->CSGetShader(&oldCs, nullptr, nullptr);
        game11ctx->CSGetShaderResources(0, 1, &oldSrv);
        game11ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);

        UINT keep = static_cast<UINT>(-1);
        game11ctx->CSSetShader(depthCs, nullptr, 0);
        game11ctx->CSSetShaderResources(0, 1, &depthSrv);
        game11ctx->CSSetUnorderedAccessViews(0, 1, &depthUav, &keep);
        game11ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

        ID3D11UnorderedAccessView *nullUav = nullptr;
        ID3D11ShaderResourceView *nullSrv = nullptr;
        game11ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
        game11ctx->CSSetShaderResources(0, 1, &nullSrv);
        game11ctx->CSSetShader(oldCs, nullptr, 0);
        game11ctx->CSSetShaderResources(0, 1, &oldSrv);
        game11ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
        Release(oldCs);
        Release(oldSrv);
        Release(oldUav);
    }

    // The whole per-frame cost: two copies on the game's device plus one fence signal. No CPU
    // wait -- that is what makes this usable every frame instead of once.
    bool Cross(ID3D11Resource *colour_src, ID3D11Resource *depth_src)
    {
        LARGE_INTEGER t0 {};
        QueryPerformanceCounter(&t0);
        game11ctx->CopyResource(colour.on11, colour_src);
        if (depth_src != nullptr && depthUav != nullptr)
            ConvertDepth(depth.width, depth.height);
        ++crossValue;
        if (FAILED(game11ctx->Signal(crossOn11, crossValue)))
            return false;
        const double ms = MsSince(t0);
        copySum += ms;
        copyMax = std::max(copyMax, ms);
        ++copyN;

        // Our D3D12 queue waits on the GPU timeline. Costs nothing on the CPU.
        return SUCCEEDED(queue->Wait(crossFence, crossValue));
    }

    // Occasional CPU wait, only to find out how long the data actually takes to land. Sampling
    // this instead of doing it every frame is the difference between a measurement and a stall.
    void SampleLatency()
    {
        LARGE_INTEGER t0 {};
        QueryPerformanceCounter(&t0);
        ++localValue;
        if (FAILED(queue->Signal(localFence, localValue)))
            return;
        if (localFence->GetCompletedValue() < localValue)
        {
            localFence->SetEventOnCompletion(localValue, localEvent);
            WaitForSingleObject(localEvent, 2000);
        }
        const double ms = MsSince(t0);
        landSum += ms;
        landMax = std::max(landMax, ms);
        ++landN;
    }

    void Report()
    {
        if (copyN == 0)
            return;
        Log("crossing over %llu frames: submit %.3f ms mean / %.3f ms max; land %.3f ms mean / "
            "%.3f ms max (%llu samples). colour %ux%u fmt %u, depth %s",
            static_cast<unsigned long long>(copyN), copySum / copyN, copyMax,
            landN ? landSum / landN : 0.0, landMax, static_cast<unsigned long long>(landN),
            colour.width, colour.height, static_cast<unsigned>(colour.format),
            depthUav != nullptr ? "converted and crossing" : (depthUnshareable ? "NOT shareable" : "none"));
        copySum = copyMax = landSum = landMax = 0.0;
        copyN = landN = 0;
    }
};

Session g_session;

bool IsDepthFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return true;
    default:                    return false;
    }
}

// Pick the depth buffer by area, then take the colour target that is exactly the same size. That
// pairing is what makes the two usable together: measured on PCSX2, colour and depth both come
// out 1536x1254, while the swapchain is 1918x1008 -- very slightly *larger* in pixel count, so
// "biggest colour target" would pick the wrong one.
void OnBindRenderTargets(command_list *cmd, uint32_t count, const resource_view *rtvs,
                         resource_view dsv)
{
    device *dev = cmd != nullptr ? cmd->get_device() : nullptr;
    if (dev == nullptr || dev->get_api() != device_api::d3d11)
        return;

    std::lock_guard guard(g_lock);

    if (dsv.handle != 0)
    {
        const resource res = dev->get_resource_from_view(dsv);
        if (res.handle != 0)
        {
            const resource_desc d = dev->get_resource_desc(res);
            if (d.texture.samples == 1 && IsDepthFormat(static_cast<DXGI_FORMAT>(d.texture.format)) &&
                static_cast<UINT64>(d.texture.width) * d.texture.height >
                    static_cast<UINT64>(g_seen.depthW) * g_seen.depthH)
            {
                g_seen.depth = reinterpret_cast<ID3D11Resource *>(res.handle);
                g_seen.depthW = d.texture.width;
                g_seen.depthH = d.texture.height;
                g_seen.depthFmt = static_cast<DXGI_FORMAT>(d.texture.format);
            }
        }
    }

    if (g_seen.depthW == 0)
        return;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (rtvs[i].handle == 0)
            continue;
        const resource res = dev->get_resource_from_view(rtvs[i]);
        if (res.handle == 0)
            continue;
        const resource_desc d = dev->get_resource_desc(res);
        if (d.texture.samples != 1 || d.texture.width != g_seen.depthW ||
            d.texture.height != g_seen.depthH)
            continue;
        g_seen.colour = reinterpret_cast<ID3D11Resource *>(res.handle);
        g_seen.colourW = d.texture.width;
        g_seen.colourH = d.texture.height;
        g_seen.colourFmt = static_cast<DXGI_FORMAT>(d.texture.format);
        break;
    }
}

void OnInitDevice(device *dev)
{
    if (g_session.device != nullptr || g_session.failed)
        return;

    if (dev->get_api() != device_api::d3d11)
    {
        Log("session: this measures the D3D11 -> D3D12 crossing, so it needs PCSX2 on Direct3D 11. "
            "Current API is %u. Nothing else will be logged.",
            static_cast<uint32_t>(dev->get_api()));
        g_session.failed = true;
        return;
    }

    auto *native = reinterpret_cast<ID3D11Device *>(dev->get_native());
    if (native == nullptr || FAILED(native->QueryInterface(IID_PPV_ARGS(&g_session.game11))))
    {
        Log("session: the game's device does not expose ID3D11Device5 (needs Windows 10 1703+)");
        g_session.failed = true;
        return;
    }
    ID3D11DeviceContext *ctx = nullptr;
    g_session.game11->GetImmediateContext(&ctx);
    if (ctx == nullptr || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&g_session.game11ctx))))
    {
        Log("session: the game's context does not expose ID3D11DeviceContext4");
        Release(ctx);
        g_session.failed = true;
        return;
    }
    Release(ctx);

    IDXGIDevice *dxgi = nullptr;
    IDXGIAdapter *adapter = nullptr;
    DXGI_ADAPTER_DESC ad {};
    if (SUCCEEDED(g_session.game11->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&ad)))
    {
        if (!g_session.CreateOn(ad.AdapterLuid))
            g_session.failed = true;
        else if (FAILED(g_session.game11->OpenSharedFence(g_session.crossHandle,
                                                          IID_PPV_ARGS(&g_session.crossOn11))))
        {
            Log("session: OpenSharedFence on the game's device failed");
            g_session.failed = true;
        }
    }
    else
    {
        Log("session: could not read the game adapter's LUID");
        g_session.failed = true;
    }
    Release(adapter);
    Release(dxgi);
    if (!g_session.failed)
        Log("session: ready -- own device, own queue, shared fence");
}

void OnDestroyDevice(device *)
{
    g_session.Destroy();
}

void OnPresent(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
    ++g_frame;
    if (g_session.device == nullptr || g_session.failed)
        return;

    std::lock_guard guard(g_lock);
    if (g_seen.colour == nullptr)
    {
        if (g_frame == 600)
            Log("no colour target matching a depth target has turned up in 600 frames. Either the "
                "bind events are not arriving, or nothing renders colour and depth at one size.");
        return;
    }

    if (!g_seen.logged)
    {
        g_seen.logged = true;
        Log("sources found: colour %ux%u fmt %u, depth %ux%u fmt %u -- same size, so they need no "
            "realignment.",
            g_seen.colourW, g_seen.colourH, static_cast<unsigned>(g_seen.colourFmt), g_seen.depthW,
            g_seen.depthH, static_cast<unsigned>(g_seen.depthFmt));
    }

    if (!g_session.colour.Ensure(g_session.game11, g_session.device, g_seen.colourW, g_seen.colourH,
                                 g_seen.colourFmt))
    {
        g_session.failed = true;
        return;
    }
    // Depth is allowed to fail on its own: it needs a conversion pass, and if that cannot be set
    // up it is worth knowing separately rather than taking the whole measurement down with it.
    if (!g_session.depthUnshareable && g_session.depthUav == nullptr &&
        !g_session.EnsureDepthPath(g_seen.depth, g_seen.depthW, g_seen.depthH, g_seen.depthFmt))
    {
        g_session.depthUnshareable = true;
        Log("depth: giving up on the depth path; colour will still be measured.");
    }

    if (!g_session.Cross(g_seen.colour, g_session.depthUnshareable ? nullptr : g_seen.depth))
    {
        Log("crossing: copy or signal failed");
        g_session.failed = true;
        return;
    }

    if (g_frame % 20 == 0)
        g_session.SampleLatency();
    if (g_frame % kReportEvery == 0)
        g_session.Report();
}

void OpenLog()
{
    g_log = fopen(GamePath("dlss5-session.log").c_str(), "w");
    Log("dlss5 session -- what it costs to carry colour and depth from D3D11 to a D3D12 device");
    Log("needs PCSX2 on Direct3D 11. Runs no network and changes no pixels.");
}
}

extern "C" __declspec(dllexport) const char *NAME = "dlss5 session";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Measures the per-frame cost of carrying the render-resolution colour and the depth buffer "
    "from the game's D3D11 device to a separate D3D12 device by shared texture and shared fence.";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module))
            return FALSE;
        OpenLog();
        reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
        reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
            OnBindRenderTargets);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(module);
        if (g_log != nullptr)
            fclose(g_log);
        break;
    }
    return TRUE;
}
