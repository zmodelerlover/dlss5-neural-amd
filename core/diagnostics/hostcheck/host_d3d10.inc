// hostcheck's D3D10 host: a D3D10.1 device and a blt-model swapchain, as the D3D10 games of its
// time made, whose back buffer is overwritten with the frame before every present. Like Just Cause
// 2 it compiles a shader through d3d10_1.dll after the device is up, which under ReShade 6.8.0 as
// dxgi.dll looped until the stack ran out (core/shared/d3d10_compile_unhook.h).
#include <d3d10_1.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d10_1.lib")

int hostcheck::RunD3D10(const Image &img)
{
    using Microsoft::WRL::ComPtr;
    HWND wnd = MakeWindow(img.w, img.h, false);
    DXGI_SWAP_CHAIN_DESC sd {};
    sd.BufferDesc.Width = img.w;
    sd.BufferDesc.Height = img.h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = wnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain> sc;
    ComPtr<ID3D10Device1> dev;
    if (FAILED(D3D10CreateDeviceAndSwapChain1(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                              D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &sd,
                                              &sc, &dev))) {
        std::fprintf(stderr, "D3D10CreateDeviceAndSwapChain1 failed\n");
        return 3;
    }
    // Through d3d10_1.dll by name, as the game does.
    using Compile = HRESULT(WINAPI *)(LPCSTR, SIZE_T, LPCSTR, const D3D10_SHADER_MACRO *,
                                      LPD3D10INCLUDE, LPCSTR, LPCSTR, UINT, ID3D10Blob **,
                                      ID3D10Blob **);
    const char shader[] = "float4 main() : SV_Target { return 1; }";
    ComPtr<ID3D10Blob> code, errors;
    auto compile = reinterpret_cast<Compile>(
        GetProcAddress(GetModuleHandleW(L"d3d10_1.dll"), "D3D10CompileShader"));
    if (compile == nullptr || FAILED(compile(shader, sizeof(shader) - 1, "hostcheck", nullptr,
                                             nullptr, "main", "ps_4_0", 0, &code, &errors))) {
        std::fprintf(stderr, "D3D10CompileShader through d3d10_1.dll failed\n");
        return 3;
    }
    const bool resize = std::getenv("HOSTCHECK_RESIZE") != nullptr;
    const ULONGLONG resizeAt = GetTickCount64() + 8000;
    bool resized = false;
    return Loop([&] {
        if (resize && !resized && GetTickCount64() >= resizeAt) {
            resized = true;
            if (FAILED(sc->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0)))
                return false;
        }
        ComPtr<ID3D10Texture2D> back;
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back))))
            return false;
        dev->UpdateSubresource(back.Get(), 0, nullptr, img.rgba.data(), img.w * 4, 0);
        return SUCCEEDED(sc->Present(1, 0));
    });
}
