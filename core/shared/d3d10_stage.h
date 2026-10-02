#pragma once
// A D3D10 game's frame on a D3D11 device of our own. D3D10 makes and opens shared textures by
// legacy handle only, which D3D12 cannot open, so the back buffer goes to D3D11 first: a texture
// the game's device shares, opened on ours, and from there the D3D11 path as it stands. The 64-bit
// route (transport/d3d10) and the 32-bit frontend both cross through this.
//
// The back buffer is copied into a private texture of the game's device and only that one into the
// shared texture, for the reason d3d11_guides.h keeps stageIn11: DXGI will not resize a swapchain
// whose buffers are enrolled in a sharing dependency.
#include <d3d10_1.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "d3d11_guides.h"

namespace d3d10stage {

using Microsoft::WRL::ComPtr;

// The game's queue drained on the CPU, as FlushAndWait11 does on D3D11.
inline bool FlushAndWait10(ID3D10Device* dev) {
    D3D10_QUERY_DESC qd{};
    qd.Query = D3D10_QUERY_EVENT;
    ComPtr<ID3D10Query> done;
    if (dev == nullptr || FAILED(dev->CreateQuery(&qd, &done))) {
        if (dev != nullptr)
            dev->Flush();
        return false;
    }
    done->End();
    dev->Flush();
    BOOL finished = FALSE;
    const HRESULT hr = d3d11guides::PollQuery([&] {
        const HRESULT got = done->GetData(&finished, sizeof(finished), 0);
        return got == S_FALSE && FAILED(dev->GetDeviceRemovedReason()) ? E_FAIL : got;
    });
    return SUCCEEDED(hr) && SUCCEEDED(dev->GetDeviceRemovedReason());
}

// Our D3D11 device on the adapter the game's D3D10 device runs on, and that adapter's LUID.
// D3D11CreateDevice is resolved by hand so the 64-bit add-on needs no import for it.
inline HRESULT CreateDeviceBeside(ID3D10Device* game, ComPtr<ID3D11Device>& device,
                                  ComPtr<ID3D11DeviceContext>& ctx, LUID& luid) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    HRESULT hr = game->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr))
        hr = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(hr))
        hr = adapter->GetDesc(&desc);
    if (FAILED(hr))
        return hr;
    HMODULE module = GetModuleHandleW(L"d3d11.dll");
    if (module == nullptr)
        module = LoadLibraryW(L"d3d11.dll");
    const auto create = module != nullptr ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
                                                GetProcAddress(module, "D3D11CreateDevice"))
                                          : nullptr;
    if (create == nullptr)
        return E_NOINTERFACE;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    hr = create(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels, 2, D3D11_SDK_VERSION, &device, nullptr, &ctx);
    if (hr == E_INVALIDARG)  // 11_1 unknown to the runtime
        hr = create(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1, D3D11_SDK_VERSION, &device,
                    nullptr, &ctx);
    luid = desc.AdapterLuid;
    return hr;
}

// The back buffer's copy on both devices, at the back buffer's size and format.
struct Stage {
    ComPtr<ID3D10Texture2D> local10, shared10;
    ComPtr<ID3D11Texture2D> shared11;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    void Release() {
        shared11.Reset();
        shared10.Reset();
        local10.Reset();
        width = height = 0;
        format = DXGI_FORMAT_UNKNOWN;
    }

    // False when either device refuses the format or the share.
    bool Ensure(ID3D10Device* game, ID3D11Device* own, UINT w, UINT h, DXGI_FORMAT fmt) {
        if (shared11 != nullptr && width == w && height == h && format == fmt)
            return true;
        Release();
        D3D10_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = fmt;
        d.Usage = D3D10_USAGE_DEFAULT;
        d.BindFlags = D3D10_BIND_SHADER_RESOURCE | D3D10_BIND_RENDER_TARGET;
        ComPtr<IDXGIResource> res;
        HANDLE handle = nullptr;
        if (FAILED(game->CreateTexture2D(&d, nullptr, &local10)))
            return false;
        d.MiscFlags = D3D10_RESOURCE_MISC_SHARED;
        if (FAILED(game->CreateTexture2D(&d, nullptr, &shared10)) || FAILED(shared10.As(&res)) ||
            FAILED(res->GetSharedHandle(&handle)) || handle == nullptr ||
            FAILED(own->OpenSharedResource(handle, IID_PPV_ARGS(&shared11)))) {
            Release();
            return false;
        }
        width = w;
        height = h;
        format = fmt;
        return true;
    }

    // The back buffer into shared11, landed before our device reads it.
    bool Upload(ID3D10Device* game, ID3D10Resource* back) {
        game->CopyResource(local10.Get(), back);
        game->CopyResource(shared10.Get(), local10.Get());
        return FlushAndWait10(game);
    }

    // shared11, which our device has finished with, back into the back buffer. Drained, so
    // nothing of ours still references the back buffer when the game resizes.
    bool Download(ID3D10Device* game, ID3D10Resource* back) {
        game->CopyResource(local10.Get(), shared10.Get());
        game->CopyResource(back, local10.Get());
        return FlushAndWait10(game);
    }
};

}  // namespace d3d10stage
