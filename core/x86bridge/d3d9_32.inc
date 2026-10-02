// The 32-bit pair's D3D9 route: a D3D11 device of ours on the game's adapter, and the frame carried
// to it through D3D9Ex shared textures or, on plain D3D9, through the CPU. Included by frontend32.cpp
// inside its anonymous namespace, after the frontend's state.

void ReleaseD3D9Stage()
{
    g.cpuIn11.Reset();
    g.cpuOut11.Reset();
    g.sharedIn11.Reset();
    g.sharedOut11.Reset();
    g.readback9.Reset();
    g.upload9.Reset();
    g.stageInSurface9.Reset();
    g.stageOutSurface9.Reset();
    g.stageIn9.Reset();
    g.stageOut9.Reset();
    g.stage9W = g.stage9H = 0;
    g.stage9Fmt = D3DFMT_UNKNOWN;
    g.d3d9Shared = false;
}

// The transport format for the classic CPU route. The X8 pair is carried as its A8 twin, which
// is the same four bytes a pixel with one channel the game does not read -- and the reason
// is not tidiness: B8G8R8X8_UNORM has no typed UAV at all on this hardware, so the x64 host had
// nowhere to write the corrected image and stopped rather than draw garbage. Measured on a Radeon
// RX 9070 XT, driver 32.0.31041: format 88 reports uav=0 typed_store=0, format 87 reports both.
// That is what a D3D9 game with an X8R8G8B8 back buffer -- Oblivion, and most of its generation --
// ran into. The X8B8G8R8 pair below was already carried as R8G8B8A8_UNORM for the same reason.
DXGI_FORMAT D3D9CpuFormat(D3DFORMAT format)
{
    switch (format)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool FindD3D9Adapter(IDirect3DDevice9 *device9, ComPtr<IDXGIAdapter1> &match)
{
    D3DDEVICE_CREATION_PARAMETERS creation {};
    ComPtr<IDirect3D9> d3d9;
    if (FAILED(device9->GetCreationParameters(&creation)) ||
        FAILED(device9->GetDirect3D(&d3d9)))
        return false;
    const HMONITOR monitor = d3d9->GetAdapterMonitor(creation.AdapterOrdinal);
    if (monitor == nullptr)
        return false;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        for (UINT outputIndex = 0;; ++outputIndex)
        {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_OUTPUT_DESC desc {};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
            {
                match = adapter;
                return true;
            }
        }
    }
    return false;
}

bool InitD3D9Bridge(device *reshadeDevice)
{
    // ReShade's API object already exposes the original D3D9 device from get_native(). Querying
    // the private proxy-only unwrapped IID here fails with E_NOINTERFACE on that original object.
    auto *native = reinterpret_cast<IDirect3DDevice9 *>(reshadeDevice->get_native());
    if (native == nullptr)
        return false;
    g.game9 = native;

    ComPtr<IDXGIAdapter1> adapter;
    if (!FindD3D9Adapter(native, adapter))
        return false;
    DXGI_ADAPTER_DESC1 adapterDesc {};
    if (FAILED(adapter->GetDesc1(&adapterDesc)))
        return false;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL level {};
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION, &g.game11, &level, &g.game11ctx);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1,
            static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
            &g.game11, &level, &g.game11ctx);
    if (FAILED(hr) || g.game11 == nullptr || g.game11ctx == nullptr)
        return false;

    g.luid = adapterDesc.AdapterLuid;
    g.nativeD3D9 = true;
    g.guideDepth.name = "depth"; g.guideMotion.name = "motion";
    Log("x86bridge native D3D9 interop on LUID=%08lX:%08lX feature=%04X. D3D9 route: colour only; "
        "no depth path on this API; motion estimated: no game MV path on this API",
        g.luid.HighPart, g.luid.LowPart, static_cast<unsigned>(level));
    return true;
}

HRESULT FlushAndWait9();

bool EnsureD3D9Stage(UINT width, UINT height, D3DFORMAT format, DXGI_FORMAT &dxgiFormat)
{
    if (g.stageIn9 != nullptr && g.stage9W == width && g.stage9H == height &&
        g.stage9Fmt == format)
    {
        D3D11_TEXTURE2D_DESC desc {};
        auto *texture = g.d3d9Shared ? g.sharedIn11.Get() : g.cpuIn11.Get();
        if (texture == nullptr)
            return false;
        texture->GetDesc(&desc);
        dxgiFormat = desc.Format;
        return true;
    }

    ReleaseD3D9Stage();
    HANDLE inputHandle = nullptr, outputHandle = nullptr;
    const bool shared = SUCCEEDED(g.game9->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                            format, D3DPOOL_DEFAULT, &g.stageIn9, &inputHandle)) &&
        inputHandle != nullptr &&
        SUCCEEDED(g.game9->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format,
                            D3DPOOL_DEFAULT, &g.stageOut9, &outputHandle)) &&
        outputHandle != nullptr &&
        SUCCEEDED(g.stageIn9->GetSurfaceLevel(0, &g.stageInSurface9)) &&
        SUCCEEDED(g.stageOut9->GetSurfaceLevel(0, &g.stageOutSurface9)) &&
        SUCCEEDED(g.game11->OpenSharedResource(inputHandle, IID_PPV_ARGS(&g.sharedIn11))) &&
        SUCCEEDED(g.game11->OpenSharedResource(outputHandle, IID_PPV_ARGS(&g.sharedOut11)));
    if (shared)
    {
        D3D11_TEXTURE2D_DESC inputDesc {}, outputDesc {};
        g.sharedIn11->GetDesc(&inputDesc);
        g.sharedOut11->GetDesc(&outputDesc);
        if (inputDesc.Width == width && inputDesc.Height == height &&
            inputDesc.SampleDesc.Count == 1 && inputDesc.Format != DXGI_FORMAT_UNKNOWN &&
            outputDesc.Width == width && outputDesc.Height == height &&
            outputDesc.SampleDesc.Count == 1 && outputDesc.Format == inputDesc.Format)
        {
            g.d3d9Shared = true;
            dxgiFormat = inputDesc.Format;
        }
        else
            ReleaseD3D9Stage();
    }

    if (!g.d3d9Shared)
    {
        ReleaseD3D9Stage();
        dxgiFormat = D3D9CpuFormat(format);
        if (dxgiFormat == DXGI_FORMAT_UNKNOWN ||
            FAILED(g.game9->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format,
                D3DPOOL_DEFAULT, &g.stageIn9, nullptr)) ||
            FAILED(g.game9->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format,
                D3DPOOL_DEFAULT, &g.stageOut9, nullptr)) ||
            FAILED(g.stageIn9->GetSurfaceLevel(0, &g.stageInSurface9)) ||
            FAILED(g.stageOut9->GetSurfaceLevel(0, &g.stageOutSurface9)) ||
            FAILED(g.game9->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_SYSTEMMEM,
                &g.readback9, nullptr)) ||
            FAILED(g.game9->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_SYSTEMMEM,
                &g.upload9, nullptr)))
        {
            ReleaseD3D9Stage();
            return false;
        }
        D3D11_TEXTURE2D_DESC cpu {};
        cpu.Width = width;
        cpu.Height = height;
        cpu.MipLevels = cpu.ArraySize = cpu.SampleDesc.Count = 1;
        cpu.Format = dxgiFormat;
        cpu.Usage = D3D11_USAGE_STAGING;
        cpu.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g.game11->CreateTexture2D(&cpu, nullptr, &g.cpuIn11)))
        {
            ReleaseD3D9Stage();
            return false;
        }
        cpu.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g.game11->CreateTexture2D(&cpu, nullptr, &g.cpuOut11)))
        {
            ReleaseD3D9Stage();
            return false;
        }
    }
    g.stage9W = width;
    g.stage9H = height;
    g.stage9Fmt = format;
    Log("bridge: native D3D9 %ux%u fmt %u <-> D3D11 fmt %u using %s staging",
        width, height, static_cast<unsigned>(format), static_cast<unsigned>(dxgiFormat),
        g.d3d9Shared ? "shared GPU" : "classic CPU-compatible");
    return true;
}

HRESULT UploadD3D9Frame(IDirect3DSurface9 *backBuffer)
{
    HRESULT hr = g.game9->StretchRect(backBuffer, nullptr, g.stageInSurface9.Get(), nullptr,
        D3DTEXF_NONE);
    if (FAILED(hr))
        return hr;
    if (g.d3d9Shared)
    {
        if (FAILED(hr = FlushAndWait9()))
            return hr;
        g.game11ctx->CopyResource(g.stageIn11.Get(), g.sharedIn11.Get());
        return S_OK;
    }
    if (FAILED(hr = g.game9->GetRenderTargetData(g.stageInSurface9.Get(), g.readback9.Get())))
        return hr;
    D3DLOCKED_RECT source {};
    D3D11_MAPPED_SUBRESOURCE destination {};
    if (FAILED(hr = g.readback9->LockRect(&source, nullptr, D3DLOCK_READONLY)))
        return hr;
    const HRESULT mapped = g.game11ctx->Map(g.cpuIn11.Get(), 0, D3D11_MAP_WRITE, 0, &destination);
    const size_t rowBytes = static_cast<size_t>(g.stage9W) * 4;
    if (FAILED(mapped) || source.Pitch <= 0 || static_cast<size_t>(source.Pitch) < rowBytes ||
        (SUCCEEDED(mapped) && destination.RowPitch < rowBytes))
    {
        if (SUCCEEDED(mapped))
            g.game11ctx->Unmap(g.cpuIn11.Get(), 0);
        g.readback9->UnlockRect();
        return FAILED(mapped) ? mapped : E_UNEXPECTED;
    }
    for (UINT y = 0; y < g.stage9H; ++y)
        std::memcpy(static_cast<unsigned char *>(destination.pData) + destination.RowPitch * y,
            static_cast<const unsigned char *>(source.pBits) + source.Pitch * y, rowBytes);
    g.game11ctx->Unmap(g.cpuIn11.Get(), 0);
    g.readback9->UnlockRect();
    g.game11ctx->CopyResource(g.stageIn11.Get(), g.cpuIn11.Get());
    return S_OK;
}

HRESULT DownloadD3D9Frame(IDirect3DSurface9 *backBuffer)
{
    if (g.d3d9Shared)
    {
        g.game11ctx->CopyResource(g.sharedOut11.Get(), g.stageOut11.Get());
        if (!FlushAndWait11())
            return E_FAIL;
    }
    else
    {
        g.game11ctx->CopyResource(g.cpuOut11.Get(), g.stageOut11.Get());
        if (!FlushAndWait11())
            return E_FAIL;
        D3D11_MAPPED_SUBRESOURCE source {};
        D3DLOCKED_RECT destination {};
        HRESULT hr = g.game11ctx->Map(g.cpuOut11.Get(), 0, D3D11_MAP_READ, 0, &source);
        if (FAILED(hr))
            return hr;
        const HRESULT locked = g.upload9->LockRect(&destination, nullptr, 0);
        const size_t rowBytes = static_cast<size_t>(g.stage9W) * 4;
        if (FAILED(locked) || destination.Pitch <= 0 ||
            (SUCCEEDED(locked) && static_cast<size_t>(destination.Pitch) < rowBytes) ||
            source.RowPitch < rowBytes)
        {
            if (SUCCEEDED(locked))
                g.upload9->UnlockRect();
            g.game11ctx->Unmap(g.cpuOut11.Get(), 0);
            return FAILED(locked) ? locked : E_UNEXPECTED;
        }
        for (UINT y = 0; y < g.stage9H; ++y)
            std::memcpy(static_cast<unsigned char *>(destination.pBits) + destination.Pitch * y,
                static_cast<const unsigned char *>(source.pData) + source.RowPitch * y, rowBytes);
        g.upload9->UnlockRect();
        g.game11ctx->Unmap(g.cpuOut11.Get(), 0);
        if (FAILED(hr = g.game9->UpdateSurface(g.upload9.Get(), nullptr, g.stageOutSurface9.Get(), nullptr)))
            return hr;
    }
    HRESULT hr = g.game9->StretchRect(g.stageOutSurface9.Get(), nullptr, backBuffer, nullptr,
        D3DTEXF_NONE);
    return FAILED(hr) ? hr : FlushAndWait9();
}

HRESULT FlushAndWait9()
{
    if (g.game9 == nullptr)
        return E_POINTER;
    ComPtr<IDirect3DQuery9> query;
    HRESULT hr = g.game9->CreateQuery(D3DQUERYTYPE_EVENT, &query);
    if (FAILED(hr))
        return hr;
    if (query == nullptr)
        return E_UNEXPECTED;
    if (FAILED(hr = query->Issue(D3DISSUE_END)))
        return hr;
    return d3d11guides::PollQuery([&] { return query->GetData(nullptr, 0, D3DGETDATA_FLUSH); });
}
