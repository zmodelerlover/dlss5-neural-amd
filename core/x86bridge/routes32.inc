// The 32-bit pair's D3D10 and OpenGL routes. Included by frontend32.cpp inside its anonymous
// namespace, after the frontend's state. Both run, as D3D9 does, on a D3D11 device of ours on the
// game's GPU, and hand over colour only: D3D10 through a texture its device shares by legacy handle
// (d3d10_stage.h), OpenGL through D3D11 textures imported into its context (gl32.inc). From the
// D3D11 staging on, the frame goes the way every other route's does.

#include "gl32.inc"

// The name of a route that hands over colour only, or nullptr on D3D11.
const char* ColourOnlyRoute() {
    return g.nativeD3D9 ? "D3D9" : g.nativeD3D10 ? "D3D10" : g.nativeGL ? "OpenGL" : nullptr;
}

bool InitD3D10Bridge(device* dev) {
    g.game10 = reinterpret_cast<ID3D10Device*>(dev->get_native());
    if (!g.game10)
        return false;
    const HRESULT hr = d3d10stage::CreateDeviceBeside(g.game10.Get(), g.game11, g.game11ctx, g.luid);
    if (FAILED(hr) || !g.game11 || !g.game11ctx) {
        Log("x86bridge D3D10: no D3D11 device on the game's adapter (hr=0x%08lX)",
            static_cast<unsigned long>(hr));
        return false;
    }
    g.nativeD3D10 = true;
    g.guideDepth.name = "depth";
    g.guideMotion.name = "motion";
    Log("x86bridge native D3D10 interop on LUID=%08lX:%08lX. D3D10 route: colour only; no depth path "
        "on this API; motion estimated: no game MV path on this API",
        g.luid.HighPart, g.luid.LowPart);
    return true;
}

bool InitGLBridge() {
    if (!gl32::Bind() || !gl32::CreateDevice(g.game11, g.game11ctx, g.luid))
        return false;
    g.nativeGL = true;
    g.guideDepth.name = "depth";
    g.guideMotion.name = "motion";
    Log("x86bridge native OpenGL interop on LUID=%08lX:%08lX. OpenGL route: colour only; no depth "
        "path on this API; motion estimated: no game MV path on this API",
        g.luid.HighPart, g.luid.LowPart);
    return true;
}

// The D3D10 or OpenGL back buffer's size and the format it crosses in, with what it crosses
// through made at that size. False with `fault` null: this frame goes out as drawn.
bool TakeBackBuffer(device* dev, swapchain* sc, UINT& w, UINT& h, DXGI_FORMAT& fmt,
                    ID3D10Resource*& back10, const char*& fault) {
    const auto back = sc->get_current_back_buffer();
    if (!back.handle)
        return false;
    const auto d = dev->get_resource_desc(back);
    if (!d.texture.width || !d.texture.height)
        return false;
    w = d.texture.width;
    h = d.texture.height;
    if (g.nativeGL) {
        // The default framebuffer, whatever ReShade says its format is: GL writes the crossing
        // through an RGBA8 view.
        fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        fault = !gl32::Bind() ? "OpenGL context unavailable"
                : !gl32::Ensure(g.game11.Get(), w, h) ? "OpenGL crossing unavailable"
                                                      : nullptr;
        return fault == nullptr;
    }
    if (d.texture.samples > 1) {
        static bool said = false;
        if (!std::exchange(said, true))
            Log("x86bridge D3D10: MSAA back buffer (%u samples): frames go out as the game drew them",
                static_cast<unsigned>(d.texture.samples));
        return false;
    }
    back10 = reinterpret_cast<ID3D10Resource*>(back.handle);
    fmt = static_cast<DXGI_FORMAT>(d.texture.format);
    if (!g.stage10.Ensure(g.game10.Get(), g.game11.Get(), w, h, fmt))
        fault = "D3D10 back buffer not shareable with D3D11";
    return fault == nullptr;
}

// The frame into stageIn11, landed on the game's side. The fault, or nullptr.
const char* Upload(ID3D10Resource* back10) {
    if (g.nativeGL) {
        if (!gl32::BlitIn())
            return "OpenGL input blit failed";
        g.game11ctx->CopyResource(g.stageIn11.Get(), gl32::r.in.on11.Get());
        return nullptr;
    }
    if (!g.stage10.Upload(g.game10.Get(), back10))
        return "D3D10 input copy did not complete";
    g.game11ctx->CopyResource(g.stageIn11.Get(), g.stage10.shared11.Get());
    return nullptr;
}

// stageOut11 back into the game's back buffer. The fault, or nullptr.
const char* Download(ID3D10Resource* back10) {
    g.game11ctx->CopyResource(g.nativeGL ? gl32::r.out.on11.Get() : g.stage10.shared11.Get(),
                              g.stageOut11.Get());
    if (!FlushAndWait11())
        return "return D3D11 queue not drained";
    if (g.nativeGL)
        return gl32::BlitOut() ? nullptr : "OpenGL output blit failed";
    return g.stage10.Download(g.game10.Get(), back10) ? nullptr : "D3D10 output copy did not complete";
}
