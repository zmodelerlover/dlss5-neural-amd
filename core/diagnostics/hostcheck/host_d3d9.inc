// hostcheck's D3D9 host, for the 32-bit bridge's native D3D9 route: the frame is uploaded once to
// a default-pool surface and stretched onto the back buffer before every present.
#include <d3d9.h>

#pragma comment(lib, "d3d9.lib")

int hostcheck::RunD3D9(const Image &img)
{
    using Microsoft::WRL::ComPtr;
    auto fail = [](const char *what) {
        std::fprintf(stderr, "%s failed\n", what);
        return 3;
    };
    HWND wnd = MakeWindow(img.w, img.h, false);
    ComPtr<IDirect3D9> d3d;
    d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
    D3DPRESENT_PARAMETERS pp {};
    pp.BackBufferWidth = img.w;
    pp.BackBufferHeight = img.h;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    ComPtr<IDirect3DDevice9> dev;
    if (d3d == nullptr ||
        FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
        return fail("CreateDevice");

    ComPtr<IDirect3DSurface9> staging, frame;
    D3DLOCKED_RECT lr {};
    if (FAILED(dev->CreateOffscreenPlainSurface(img.w, img.h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                &staging, nullptr)) ||
        FAILED(dev->CreateOffscreenPlainSurface(img.w, img.h, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
                                                &frame, nullptr)) ||
        FAILED(staging->LockRect(&lr, nullptr, 0)))
        return fail("surfaces");
    // D3D9's X8R8G8B8 is BGRA in memory.
    for (UINT y = 0; y < img.h; ++y) {
        unsigned char *row = static_cast<unsigned char *>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
        for (UINT x = 0; x < img.w; ++x) {
            const unsigned char *p = &img.rgba[(static_cast<size_t>(y) * img.w + x) * 4];
            row[x * 4 + 0] = p[2];
            row[x * 4 + 1] = p[1];
            row[x * 4 + 2] = p[0];
            row[x * 4 + 3] = 255;
        }
    }
    staging->UnlockRect();
    if (FAILED(dev->UpdateSurface(staging.Get(), nullptr, frame.Get(), nullptr)))
        return fail("UpdateSurface");
    return Loop([&] {
        ComPtr<IDirect3DSurface9> back;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) ||
            FAILED(dev->StretchRect(frame.Get(), nullptr, back.Get(), nullptr, D3DTEXF_NONE)))
            return false;
        return SUCCEEDED(dev->Present(nullptr, nullptr, nullptr, nullptr));
    });
}
