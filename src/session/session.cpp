// ReShade add-on: measures the D3D11 -> D3D12 crossing for the real network inputs.
//
// Why this exists. The AMD network runtime is D3D12, but on D3D12 ReShade shows an add-on only
// the swapchain: two render targets, no depth. On D3D11 the same probe sees eight, including a
// real depth buffer and the colour target at render resolution, which is a better input than the
// presented back buffer. So the sources are on one API and the network is on the other.
//
// Before building that bridge for real, one number decides whether it is worth building at all:
// what it costs to carry colour and depth across, every frame. That is all this measures. It
// It composes what came back onto the right half of the screen, so the trip is visible as well as
// timed. It runs no network.

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
uint64_t g_clears = 0;
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
            Destroy();
            return false;
        }
        hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                     nullptr, &handle);
        res->Release();
        if (FAILED(hr))
        {
            Log("bridge %s: CreateSharedHandle failed 0x%08lX", name, hr);
            Destroy();
            return false;
        }
        if (FAILED(own->OpenSharedHandle(handle, IID_PPV_ARGS(&on12))))
        {
            Log("bridge %s: OpenSharedHandle on our own device failed", name);
            // depthUnshareable latches after a depth failure here, so nothing retries and nothing
            // else reclaims the texture and the handle. A failure path should stand on its own.
            Destroy();
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
    Bridge result { "result" };

    // Three (allocator, list) pairs so the D3D12 side never has to stall waiting for its own
    // previous frame before it can record the next one.
    static constexpr UINT kRing = 3;
    ID3D12CommandAllocator *alloc[kRing] {};
    ID3D12GraphicsCommandList *list[kRing] {};
    UINT64 ringValue[kRing] {};

    // The way back: D3D12 signals, the game's D3D11 context waits on it before composing.
    ID3D12Fence *backFence = nullptr;
    HANDLE backHandle = nullptr;
    ID3D11Fence *backOn11 = nullptr;
    UINT64 backValue = 0;

    // Compose on D3D11. The swapchain back buffer has no UAV, so it goes copy -> compute -> copy
    // rather than a fullscreen draw: two full-res copies are cheap, and this way the only game
    // state touched is three compute bindings.
    ID3D11ComputeShader *composeCs = nullptr;
    ID3D11ShaderResourceView *resultSrv = nullptr;
    ID3D11Texture2D *tempSrc = nullptr, *tempDst = nullptr, *scan = nullptr;
    ID3D11Texture2D *rawScan = nullptr;
    UINT scanW = 0, scanH = 0;
    ID3D11ShaderResourceView *tempSrcSrv = nullptr;
    ID3D11UnorderedAccessView *tempDstUav = nullptr;
    UINT tempW = 0, tempH = 0;
    DXGI_FORMAT tempFmt = DXGI_FORMAT_UNKNOWN;
    double roundSum = 0.0, roundMax = 0.0;
    uint64_t roundN = 0;

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

    // A private, non-shared copy of the game's depth buffer, taken while the content is still
    // there. Read at present it is all zeros -- measured two ways, through this add-on's own
    // shader and through the probe's ReShade readback, both entirely zero -- because by then
    // PCSX2 has moved on, and because D3D11 quietly drops an SRV over a resource that is still
    // bound as a writable depth-stencil. Snapshotting at the moment PCSX2 binds a *different*
    // depth target avoids both: that is when it has finished with this one.
    ID3D11Texture2D *depthCopy = nullptr;
    ID3D11Texture2D *uavOf = nullptr;
    UINT copyW = 0, copyH = 0;
    DXGI_FORMAT copyFmt = DXGI_FORMAT_UNKNOWN;
    uint64_t snaps = 0;

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
        result.Destroy();
        Release(tempDstUav);
        Release(tempSrcSrv);
        Release(scan);
        Release(rawScan);
        Release(tempDst);
        Release(tempSrc);
        Release(resultSrv);
        Release(composeCs);
        Release(backOn11);
        if (backHandle != nullptr)
            CloseHandle(backHandle);
        backHandle = nullptr;
        Release(backFence);
        for (UINT i = 0; i < kRing; ++i)
        {
            Release(list[i]);
            Release(alloc[i]);
        }
        Release(depthUav);
        Release(depthSrv);
        Release(depthCopy);
        Release(depthCs);
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

        for (UINT i = 0; i < kRing; ++i)
        {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&alloc[i]))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i],
                                                 nullptr, IID_PPV_ARGS(&list[i]))))
            {
                Log("session: could not create the command allocator or list");
                return false;
            }
            list[i]->Close();
        }

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&crossFence))) ||
            FAILED(device->CreateSharedHandle(crossFence, nullptr, GENERIC_ALL, nullptr, &crossHandle)) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&backFence))) ||
            FAILED(device->CreateSharedHandle(backFence, nullptr, GENERIC_ALL, nullptr, &backHandle)))
        {
            Log("session: could not create the shared fences");
            return false;
        }
        return true;
    }

    // Called from the bind event, on the game's own thread, at the moment PCSX2 stops using this
    // depth target. One CopyResource; the destination is never bound as a depth-stencil, so it
    // can be read as an SRV afterwards without the runtime dropping the view.
    void SnapshotDepth(ID3D11Resource *src, UINT w, UINT h, DXGI_FORMAT fmt)
    {
        if (game11 == nullptr || src == nullptr)
            return;
        if (depthCopy == nullptr || copyW != w || copyH != h || copyFmt != fmt)
        {
            Release(depthSrv);
            Release(depthCopy);
            copyW = copyH = 0;
            D3D11_TEXTURE2D_DESC td {};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = fmt;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(game11->CreateTexture2D(&td, nullptr, &depthCopy)))
            {
                Log("depth: private copy %ux%u fmt %u could not be created", w, h,
                    static_cast<unsigned>(fmt));
                return;
            }
            copyW = w;
            copyH = h;
            copyFmt = fmt;
            Log("depth: snapshotting %ux%u fmt %u at bind time into a private copy", w, h,
                static_cast<unsigned>(fmt));
        }
        game11ctx->CopyResource(depthCopy, src);
        ++snaps;
    }

    // Builds the depth path once: the shader, the UAV over the shared R32_FLOAT texture, and an
    // SRV over the private snapshot. Returns false once and then stays false, so a game whose
    // depth cannot be read does not retry every frame.
    bool EnsureDepthPath(ID3D11Resource *src, UINT w, UINT h, DXGI_FORMAT srcFmt)
    {
        if (depthCs == nullptr)
        {
            static const char kCs[] =
                "Texture2D<float> src : register(t0);\n"
                "RWTexture2D<float> dst : register(u0);\n"
                "[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {\n"
                "  uint w, h; dst.GetDimensions(w, h);\n"
                "  if (p.x >= w || p.y >= h) return;\n"
                "  dst[p.xy] = src.Load(int3(p.xy, 0));\n"
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
        // A resize makes Ensure build a new texture, and a view over the old one then writes into
        // an orphan while every read of the new one comes back zero. Rebuild the view whenever the
        // texture underneath it changed, not just when there is none.
        if (uavOf != depth.on11)
        {
            Release(depthUav);
            uavOf = nullptr;
        }
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
            uavOf = depth.on11;
        }

        if (depthSrv == nullptr)
        {
            if (depthCopy == nullptr)
                return false;
            src = depthCopy;
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
                Log("depth: SRV over the private snapshot failed 0x%08lX (fmt %u read as %u)", hr,
                    static_cast<unsigned>(srcFmt), static_cast<unsigned>(sd.Format));
                return false;
            }
            Log("depth: SRV created over the snapshot, view fmt %u (resource fmt %u)",
                static_cast<unsigned>(sd.Format), static_cast<unsigned>(srcFmt));
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

    // D3D12 half of the round trip. It waits for the game's copies on the GPU timeline, does its
    // work, and signals back. A CopyResource stands in for the network here on purpose: the point
    // is to prove the transport in both directions and price it, not to run a network twice.
    bool WorkAndSignBack()
    {
        const UINT i = static_cast<UINT>(backValue % kRing);
        // Only blocks if this pair is still in flight, which with three of them it will not be.
        if (ringValue[i] != 0 && localFence->GetCompletedValue() < ringValue[i])
        {
            localFence->SetEventOnCompletion(ringValue[i], localEvent);
            WaitForSingleObject(localEvent, 2000);
        }
        if (FAILED(alloc[i]->Reset()) || FAILED(list[i]->Reset(alloc[i], nullptr)))
            return false;

        // Shared resources opened from another device sit in COMMON, and COMMON promotes to
        // COPY_SOURCE and COPY_DEST on a direct queue, so a plain copy needs no barriers.
        list[i]->CopyResource(result.on12, depth.on12);
        if (FAILED(list[i]->Close()))
            return false;
        ID3D12CommandList *lists[] = { list[i] };
        queue->ExecuteCommandLists(1, lists);

        ++localValue;
        ringValue[i] = localValue;
        if (FAILED(queue->Signal(localFence, localValue)))
            return false;
        ++backValue;
        return SUCCEEDED(queue->Signal(backFence, backValue));
    }

    bool EnsureComposePath(UINT w, UINT h, DXGI_FORMAT fmt)
    {
        if (composeCs == nullptr)
        {
            // Right half only, so one look tells you whether the trip worked: the left half is the
            // untouched game, the right half is what came back from the other device.
            static const char kCs[] =
                "Texture2D<float4> src : register(t0);\n"
                "Texture2D<float>  ret : register(t1);\n"
                "RWTexture2D<float4> dst : register(u0);\n"
                "[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {\n"
                "  uint w, h; dst.GetDimensions(w, h);\n"
                "  if (p.x >= w || p.y >= h) return;\n"
                "  float4 c = src.Load(int3(p.xy, 0));\n"
                "  if (p.x > w / 2) {\n"
                "    uint rw, rh; ret.GetDimensions(rw, rh);\n"
                "    int2 t = int2(p.xy * float2(rw, rh) / float2(w, h));\n"
                "    float d = ret.Load(int3(t, 0));\n"
                "    c.rgb = lerp(c.rgb, d.xxx, 0.85);\n"
                "  }\n"
                "  dst[p.xy] = c;\n"
                "}\n";
            ID3DBlob *blob = nullptr, *err = nullptr;
            if (FAILED(D3DCompile(kCs, sizeof(kCs) - 1, "compose", nullptr, nullptr, "main",
                                  "cs_5_0", 0, 0, &blob, &err)))
            {
                Log("compose: shader failed to compile: %s",
                    err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "?");
                Release(err);
                return false;
            }
            Release(err);
            const HRESULT hr = game11->CreateComputeShader(blob->GetBufferPointer(),
                                                           blob->GetBufferSize(), nullptr,
                                                           &composeCs);
            blob->Release();
            if (FAILED(hr))
            {
                Log("compose: CreateComputeShader failed 0x%08lX", hr);
                return false;
            }
        }
        if (resultSrv == nullptr &&
            FAILED(game11->CreateShaderResourceView(result.on11, nullptr, &resultSrv)))
        {
            Log("compose: SRV over the returned texture failed");
            return false;
        }
        if (tempSrc != nullptr && tempW == w && tempH == h && tempFmt == fmt)
            return true;

        Release(tempDstUav);
        Release(tempSrcSrv);
        Release(tempDst);
        Release(tempSrc);
        D3D11_TEXTURE2D_DESC td {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(game11->CreateTexture2D(&td, nullptr, &tempSrc)))
        {
            Log("compose: scratch copy of the back buffer failed (%ux%u fmt %u)", w, h,
                static_cast<unsigned>(fmt));
            return false;
        }
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(game11->CreateTexture2D(&td, nullptr, &tempDst)) ||
            FAILED(game11->CreateShaderResourceView(tempSrc, nullptr, &tempSrcSrv)) ||
            FAILED(game11->CreateUnorderedAccessView(tempDst, nullptr, &tempDstUav)))
        {
            Log("compose: scratch target or views failed (%ux%u fmt %u)", w, h,
                static_cast<unsigned>(fmt));
            return false;
        }
        tempW = w;
        tempH = h;
        tempFmt = fmt;
        Log("compose ready: %ux%u fmt %u, right half shows what came back", w, h,
            static_cast<unsigned>(fmt));
        return true;
    }

    void Compose(ID3D11Resource *backbuffer, UINT w, UINT h)
    {
        game11ctx->Wait(backOn11, backValue);
        game11ctx->CopyResource(tempSrc, backbuffer);

        ID3D11ComputeShader *oldCs = nullptr;
        ID3D11ShaderResourceView *oldSrv[2] { nullptr, nullptr };
        ID3D11UnorderedAccessView *oldUav = nullptr;
        game11ctx->CSGetShader(&oldCs, nullptr, nullptr);
        game11ctx->CSGetShaderResources(0, 2, oldSrv);
        game11ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);

        UINT keep = static_cast<UINT>(-1);
        ID3D11ShaderResourceView *srvs[2] { tempSrcSrv, resultSrv };
        game11ctx->CSSetShader(composeCs, nullptr, 0);
        game11ctx->CSSetShaderResources(0, 2, srvs);
        game11ctx->CSSetUnorderedAccessViews(0, 1, &tempDstUav, &keep);
        game11ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

        ID3D11UnorderedAccessView *nullUav = nullptr;
        ID3D11ShaderResourceView *nullSrv[2] { nullptr, nullptr };
        game11ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
        game11ctx->CSSetShaderResources(0, 2, nullSrv);
        game11ctx->CSSetShader(oldCs, nullptr, 0);
        game11ctx->CSSetShaderResources(0, 2, oldSrv);
        game11ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
        Release(oldCs);
        Release(oldSrv[0]);
        Release(oldSrv[1]);
        Release(oldUav);

        game11ctx->CopyResource(backbuffer, tempDst);
    }

    // Numbers, not eyes. Sampled at both ends of the chain, because "it came back empty" and
    // "it was never filled in" look identical from the far end and need opposite fixes:
    //   game depth --[compute]--> depth.on11 --[D3D12 copy]--> result.on11
    // The whole texture is scanned on a stride rather than a block from the middle, since the
    // middle of a frame is often just background and would read as flat even when it is fine.
    void Scan(Bridge &b, const char *what)
    {
        if (b.on11 == nullptr || b.width == 0)
            return;
        if (scan == nullptr || scanW != b.width || scanH != b.height)
        {
            Release(scan);
            scanW = scanH = 0;
            D3D11_TEXTURE2D_DESC td {};
            td.Width = b.width;
            td.Height = b.height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R32_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(game11->CreateTexture2D(&td, nullptr, &scan)))
                return;
            scanW = b.width;
            scanH = b.height;
        }
        game11ctx->CopyResource(scan, b.on11);
        D3D11_MAPPED_SUBRESOURCE m {};
        if (FAILED(game11ctx->Map(scan, 0, D3D11_MAP_READ, 0, &m)))
            return;
        double lo = 1e30, hi = -1e30, sum = 0.0;
        uint64_t n = 0, nonzero = 0;
        for (UINT y = 0; y < scanH; y += 4)
        {
            auto *row = reinterpret_cast<const float *>(static_cast<const char *>(m.pData) +
                                                        static_cast<size_t>(y) * m.RowPitch);
            for (UINT x = 0; x < scanW; x += 4)
            {
                const double v = row[x];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                sum += v;
                if (v != 0.0)
                    ++nonzero;
                ++n;
            }
        }
        game11ctx->Unmap(scan, 0);
        Log("%s: min %.6f max %.6f mean %.6f, %llu of %llu samples non-zero%s", what, lo, hi,
            n ? sum / n : 0.0, static_cast<unsigned long long>(nonzero),
            static_cast<unsigned long long>(n), nonzero == 0 ? "  <-- empty" : "");
    }

    // Reads the private snapshot in its own format instead of the converted copy. R32G8X24 is
    // eight bytes a texel with the depth in the first four, so a staging texture of the same
    // typeless format can be walked directly. This separates "the copy brought nothing" from
    // "the shader read it wrong", which the converted texture alone cannot.
    void ScanSnapshot()
    {
        if (depthCopy == nullptr || copyW == 0)
            return;
        if (rawScan == nullptr)
        {
            D3D11_TEXTURE2D_DESC td {};
            td.Width = copyW;
            td.Height = copyH;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = copyFmt;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(game11->CreateTexture2D(&td, nullptr, &rawScan)))
            {
                Log("snapshot scan: staging in fmt %u could not be created",
                    static_cast<unsigned>(copyFmt));
                return;
            }
        }
        game11ctx->CopyResource(rawScan, depthCopy);
        D3D11_MAPPED_SUBRESOURCE m {};
        if (FAILED(game11ctx->Map(rawScan, 0, D3D11_MAP_READ, 0, &m)))
        {
            Log("snapshot scan: Map failed");
            return;
        }
        double lo = 1e30, hi = -1e30;
        uint64_t n = 0, nonzero = 0;
        for (UINT y = 0; y < copyH; y += 4)
        {
            auto *row = static_cast<const char *>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
            for (UINT x = 0; x < copyW; x += 4)
            {
                float v = 0.0f;
                std::memcpy(&v, row + static_cast<size_t>(x) * 8, sizeof(v));
                lo = std::min(lo, static_cast<double>(v));
                hi = std::max(hi, static_cast<double>(v));
                if (v != 0.0f)
                    ++nonzero;
                ++n;
            }
        }
        game11ctx->Unmap(rawScan, 0);
        Log("private snapshot, read raw as float: min %.6f max %.6f, %llu of %llu non-zero%s", lo,
            hi, static_cast<unsigned long long>(nonzero), static_cast<unsigned long long>(n),
            nonzero == 0 ? "  <-- the copy brought nothing, so the shader is not the problem" : "");
    }

    void SampleReturned()
    {
        ScanSnapshot();
        Log("depth: %llu clears intercepted, %llu snapshots taken",
            static_cast<unsigned long long>(g_clears), static_cast<unsigned long long>(snaps));
        Scan(depth, "depth after the compute pass, before crossing");
        Scan(result, "result after coming back from D3D12");
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
        Log("round trip over %llu frames: %.3f ms mean / %.3f ms max (D3D12 record, submit, "
            "signal back, and compose on D3D11)",
            static_cast<unsigned long long>(roundN), roundN ? roundSum / roundN : 0.0, roundMax);
        roundSum = roundMax = 0.0;
        roundN = 0;
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

// PCSX2 clears its depth buffer, and a cleared buffer is uniformly zero -- which is exactly what
// four different reads of it produced. ReShade's own Generic Depth add-on has an option for this
// case, "preserve before clear", for the same reason. So the content has to be taken just before
// the clear wipes it, not at present and not at a bind change.
//
// Returning false leaves the clear to go ahead as normal: nothing about the game's rendering
// changes, the copy just happens first.
bool OnClearDepthStencil(command_list *cmd, resource_view dsv, const float *, const uint8_t *,
                         uint32_t, const rect *)
{
    device *dev = cmd != nullptr ? cmd->get_device() : nullptr;
    if (dev == nullptr || dev->get_api() != device_api::d3d11 || dsv.handle == 0)
        return false;
    const resource res = dev->get_resource_from_view(dsv);
    if (res.handle == 0)
        return false;

    std::lock_guard guard(g_lock);
    auto *native = reinterpret_cast<ID3D11Resource *>(res.handle);
    if (g_seen.depth == nullptr || native != g_seen.depth)
        return false;
    // Several clears a frame are normal. The last one before present is the one that had the most
    // drawn into it, so overwriting each time leaves the fullest.
    g_session.SnapshotDepth(native, g_seen.depthW, g_seen.depthH, g_seen.depthFmt);
    ++g_clears;
    return false;
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
                                                          IID_PPV_ARGS(&g_session.crossOn11))) ||
                 FAILED(g_session.game11->OpenSharedFence(g_session.backHandle,
                                                          IID_PPV_ARGS(&g_session.backOn11))))
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

void OnPresent(command_queue *, swapchain *sc, const rect *, const rect *, uint32_t, const rect *)
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
    // Only give up once there was actually something to try: the snapshot is taken during the
    // frame, so the first present can arrive before one exists, and failing permanently there
    // would disable depth for the whole run.
    // Checked every frame, not just once: a resize releases the SRV over the snapshot, and gating
    // this on depthUav alone meant it was never rebuilt -- the shader then sampled a null view,
    // which reads as zero and looks exactly like a game with no depth.
    if (!g_session.depthUnshareable &&
        (g_session.depthUav == nullptr || g_session.depthSrv == nullptr) &&
        g_session.depthCopy != nullptr &&
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

    // The way back. Only attempted once depth is actually crossing, because that is what the
    // D3D12 side has to hand over.
    if (!g_session.depthUnshareable && g_session.depth.on12 != nullptr)
    {
        LARGE_INTEGER t0 {};
        QueryPerformanceCounter(&t0);

        device *dev = sc->get_device();
        const resource back = sc->get_current_back_buffer();
        const resource_desc bd = back.handle != 0 ? dev->get_resource_desc(back) : resource_desc {};

        if (back.handle != 0 &&
            g_session.result.Ensure(g_session.game11, g_session.device, g_session.depth.width,
                                    g_session.depth.height, DXGI_FORMAT_R32_FLOAT) &&
            g_session.EnsureComposePath(bd.texture.width, bd.texture.height,
                                        static_cast<DXGI_FORMAT>(bd.texture.format)))
        {
            if (!g_session.WorkAndSignBack())
            {
                Log("return: the D3D12 side could not record or signal");
                g_session.failed = true;
                return;
            }
            g_session.Compose(reinterpret_cast<ID3D11Resource *>(back.handle), bd.texture.width,
                              bd.texture.height);
            const double ms = MsSince(t0);
            g_session.roundSum += ms;
            g_session.roundMax = std::max(g_session.roundMax, ms);
            ++g_session.roundN;
        }
    }

    if (g_frame % 240 == 0)
        g_session.SampleReturned();
    if (g_frame % kReportEvery == 0)
        g_session.Report();
}

void OpenLog()
{
    g_log = fopen(GamePath("dlss5-session.log").c_str(), "w");
    Log("dlss5 session -- what it costs to carry colour and depth from D3D11 to a D3D12 device");
    Log("needs PCSX2 on Direct3D 11. Runs no network. The right half of the screen shows what came back.");
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
        reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(OnClearDepthStencil);
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
