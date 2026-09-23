// hostcheck's D3D11 host: a flip-model swapchain whose back buffer is overwritten with the frame
// before every present.
#include <d3d11.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")

int hostcheck::RunD3D11(const Image &img)
{
    using Microsoft::WRL::ComPtr;
    HWND wnd = MakeWindow(img.w, img.h, false);
    DXGI_SWAP_CHAIN_DESC sd {};
    sd.BufferDesc.Width = img.w;
    sd.BufferDesc.Height = img.h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = wnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain> sc;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr,
                                             0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx))) {
        std::fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed\n");
        return 3;
    }
    // HOSTCHECK_RESIZE: one ResizeBuffers, at the same size, eight seconds in. It takes the add-on
    // through the same raster rebuild a Resolution Scale change does, while the network is busy.
    const bool resize = std::getenv("HOSTCHECK_RESIZE") != nullptr;
    const ULONGLONG resizeAt = GetTickCount64() + 8000;
    bool resized = false;
    return Loop([&] {
        if (resize && !resized && GetTickCount64() >= resizeAt) {
            resized = true;
            if (FAILED(sc->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0)))
                return false;
        }
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back))))
            return false;
        ctx->UpdateSubresource(back.Get(), 0, nullptr, img.rgba.data(), img.w * 4, 0);
        return SUCCEEDED(sc->Present(1, 0));
    });
}
