// ReShade add-on: session probe.

#include <reshade.hpp>

#include <windows.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace reshade::api;

namespace
{
constexpr uint64_t kCaptureFrames[] = { 600, 1200 };

FILE *g_log = nullptr;
uint64_t g_frame = 0;

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

struct Session
{
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *localFence = nullptr;
    UINT64 localValue = 0;
    HANDLE localEvent = nullptr;

    ID3D12Resource *shared = nullptr;
    HANDLE sharedHandle = nullptr;
    ID3D11Texture2D *sharedOn11 = nullptr;
    ID3D12Fence *crossFence = nullptr;
    HANDLE crossHandle = nullptr;
    ID3D11Fence *crossOn11 = nullptr;
    UINT64 crossValue = 0;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    ID3D11Device5 *game11 = nullptr;
    ID3D11DeviceContext4 *game11ctx = nullptr;

    bool failed = false;

    ~Session() { Destroy(); }

    void Destroy()
    {
        Release(sharedOn11);
        Release(crossOn11);
        Release(game11ctx);
        Release(game11);
        if (sharedHandle != nullptr)
            CloseHandle(sharedHandle);
        if (crossHandle != nullptr)
            CloseHandle(crossHandle);
        if (localEvent != nullptr)
            CloseHandle(localEvent);
        sharedHandle = crossHandle = localEvent = nullptr;
        Release(shared);
        Release(crossFence);
        Release(localFence);
        Release(list);
        Release(allocator);
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
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&localFence))))
        {
            Log("session: could not create the queue, allocator, list or fence");
            return false;
        }
        list->Close();
        localEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&crossFence))) ||
            FAILED(device->CreateSharedHandle(crossFence, nullptr, GENERIC_ALL, nullptr, &crossHandle)))
        {
            Log("session: could not create the shared fence");
            return false;
        }
        return true;
    }

    bool CreateBridge(UINT w, UINT h, DXGI_FORMAT fmt)
    {
        Release(sharedOn11);
        if (sharedHandle != nullptr)
            CloseHandle(sharedHandle);
        sharedHandle = nullptr;
        Release(shared);

        if (game11 == nullptr)
            return false;

        D3D11_TEXTURE2D_DESC td {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

        HRESULT hr = game11->CreateTexture2D(&td, nullptr, &sharedOn11);
        if (FAILED(hr))
        {
            Log("session: bridge texture %ux%u fmt %u failed on D3D11 0x%08lX", w, h, fmt, hr);
            return false;
        }
        IDXGIResource1 *dxgiRes = nullptr;
        if (FAILED(sharedOn11->QueryInterface(IID_PPV_ARGS(&dxgiRes))))
        {
            Log("session: bridge texture does not expose IDXGIResource1");
            return false;
        }
        hr = dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                         &sharedHandle);
        dxgiRes->Release();
        if (FAILED(hr))
        {
            Log("session: CreateSharedHandle on the texture failed 0x%08lX", hr);
            return false;
        }
        if (FAILED(device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&shared))))
        {
            Log("session: OpenSharedHandle on our own device failed");
            return false;
        }
        width = w;
        height = h;
        format = fmt;
        Log("session: bridge ready %ux%u fmt %u", w, h, fmt);
        return true;
    }

    bool CopyIn(ID3D11Resource *source)
    {
        if (sharedOn11 == nullptr || game11ctx == nullptr)
            return false;
        game11ctx->CopyResource(sharedOn11, source);
        ++crossValue;
        return SUCCEEDED(game11ctx->Signal(crossOn11, crossValue));
    }

    bool WaitForCopy()
    {
        if (FAILED(queue->Wait(crossFence, crossValue)))
            return false;
        ++localValue;
        if (FAILED(queue->Signal(localFence, localValue)))
            return false;
        if (localFence->GetCompletedValue() < localValue)
        {
            localFence->SetEventOnCompletion(localValue, localEvent);
            WaitForSingleObject(localEvent, 2000);
        }
        return true;
    }
};

Session g_session;

void DumpBridge()
{
    Session &s = g_session;
    const UINT texel = (s.format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : 4;
    const UINT rowBytes = (s.width * texel + 255u) & ~255u;
    const UINT64 size = static_cast<UINT64>(rowBytes) * s.height;

    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource *readback = nullptr;
    if (FAILED(s.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&readback))))
    {
        Log("crossing: readback buffer failed");
        return;
    }

    s.allocator->Reset();
    s.list->Reset(s.allocator, nullptr);
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { s.shared, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                     D3D12_RESOURCE_STATE_COPY_SOURCE };
    s.list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
    src.pResource = s.shared;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { s.format, s.width, s.height, 1, rowBytes };
    s.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    s.list->ResourceBarrier(1, &b);
    s.list->Close();
    ID3D12CommandList *lists[] = { s.list };
    s.queue->ExecuteCommandLists(1, lists);

    ++s.localValue;
    s.queue->Signal(s.localFence, s.localValue);
    if (s.localFence->GetCompletedValue() < s.localValue)
    {
        s.localFence->SetEventOnCompletion(s.localValue, s.localEvent);
        WaitForSingleObject(s.localEvent, 2000);
    }

    void *mapped = nullptr;
    D3D12_RANGE range { 0, static_cast<SIZE_T>(size) };
    if (SUCCEEDED(readback->Map(0, &range, &mapped)) && mapped != nullptr)
    {
        char name[64];
        snprintf(name, sizeof(name), "dlss5-crossing-f%llu.ppm", g_frame);
        if (FILE *f = fopen(GamePath(name).c_str(), "wb"))
        {
            fprintf(f, "P6\n%u %u\n255\n", s.width, s.height);
            std::vector<uint8_t> row(static_cast<size_t>(s.width) * 3);
            const uint8_t *base = static_cast<const uint8_t *>(mapped);
            for (UINT y = 0; y < s.height; ++y)
            {
                const uint8_t *line = base + static_cast<size_t>(y) * rowBytes;
                for (UINT x = 0; x < s.width; ++x)
                {
                    const uint8_t *px = line + static_cast<size_t>(x) * texel;
                    row[x * 3 + 0] = px[2];
                    row[x * 3 + 1] = px[1];
                    row[x * 3 + 2] = px[0];
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
            Log("crossing: wrote dlss5-crossing-f%llu.ppm (%ux%u)", g_frame, s.width, s.height);
        }
        readback->Unmap(0, nullptr);
    }
    readback->Release();
}

void OnInitDevice(device *dev)
{
    if (g_session.device != nullptr || g_session.failed)
        return;

    if (dev->get_api() != device_api::d3d11)
    {
        Log("session: API %u not handled yet; D3D11 only for now", static_cast<uint32_t>(dev->get_api()));
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
        else if (FAILED(g_session.game11->OpenSharedFence(g_session.crossHandle, IID_PPV_ARGS(&g_session.crossOn11))))
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
    g_frame++;
    if (g_session.device == nullptr || g_session.failed)
        return;

    bool wanted = false;
    for (uint64_t at : kCaptureFrames)
        wanted |= (g_frame == at);
    if (!wanted)
        return;

    device *dev = sc->get_device();
    const resource back = sc->get_current_back_buffer();
    if (back.handle == 0)
        return;
    const resource_desc desc = dev->get_resource_desc(back);

    if (g_session.shared == nullptr || g_session.width != desc.texture.width ||
        g_session.height != desc.texture.height)
    {
        if (!g_session.CreateBridge(desc.texture.width, desc.texture.height,
                                    static_cast<DXGI_FORMAT>(desc.texture.format)))
        {
            g_session.failed = true;
            return;
        }
    }

    const LARGE_INTEGER zero {};
    LARGE_INTEGER t0 = zero, t1 = zero, freq = zero;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    if (!g_session.CopyIn(reinterpret_cast<ID3D11Resource *>(back.handle)))
    {
        Log("crossing: CopyIn failed");
        g_session.failed = true;
        return;
    }
    if (!g_session.WaitForCopy())
    {
        Log("crossing: waiting on the fence failed");
        g_session.failed = true;
        return;
    }
    QueryPerformanceCounter(&t1);
    Log("crossing f%llu: %.3f ms for the frame to reach our own device", g_frame,
        1000.0 * double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart));

    DumpBridge();
}

void OpenLog()
{
    g_log = fopen(GamePath("dlss5-session.log").c_str(), "w");
    Log("dlss5 session -- own D3D12 device and the frame crossing");
    Log("captura nos frames 600 e 1200");
}
}

extern "C" __declspec(dllexport) const char *NAME = "dlss5 session";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Creates its own D3D12 device on the game's adapter and brings the frame across by shared texture and fence "
    "compartilhadas. Prova o transporte cross-device que a rede vai usar.";

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
