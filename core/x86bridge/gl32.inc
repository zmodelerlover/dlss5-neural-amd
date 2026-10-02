// The 32-bit pair's OpenGL route. Included by frontend32.cpp inside its anonymous namespace, after
// the frontend's state.
//
// The 64-bit route (transport/opengl) imports textures of the network's own device into the game's
// context. Here the D3D side is a D3D11 device of ours on the context's GPU, as on D3D9, and GL imports
// its textures as GL_HANDLE_TYPE_D3D11_IMAGE_EXT: measured on an RX 9070 XT, Adrenalin 26.8.1, in a
// 32-bit process, the bytes cross both ways. From the D3D11 textures on, the frame goes the way a
// D3D9 game's does. Every GL entry point is resolved by hand (gl_api.inc), and the handles are
// never closed: docs/opengl-route.md, rule 1.

namespace gl32 {

#include "../transport/opengl/gl_api.inc"

constexpr GLenum GL_HANDLE_TYPE_D3D11_IMAGE_EXT_ = 0x958B;

// One direction: a D3D11 texture shared by NT handle, and the GL objects over the same memory.
struct Crossing {
    ComPtr<ID3D11Texture2D> on11;
    GLuint memory = 0, texture = 0, fbo = 0;
};

struct Route {
    Api api{};
    HGLRC context = nullptr;
    LUID luid{};
    bool luidKnown = false;
    Crossing in, out;
    UINT width = 0, height = 0;
    GLint samples = 1;  // the default framebuffer's; above 1 the frame is resolved first
    GLuint resolveTexture = 0, resolveFbo = 0;
    std::vector<HANDLE> keptHandles;
};
Route& r = *new Route;  // never destroyed: the handles it keeps are the process's to the end

// What a blit touches of the context: the bindings, the buffer selection, scissor and sRGB.
struct Saved {
    GLint readFbo = 0, drawFbo = 0, readBuffer = 0, drawBuffer = 0;
    bool scissor = false, srgb = false, haveRead = false, haveDraw = false;
};
void Save(Saved& s) {
    r.api.getIntegerv(GL_READ_FRAMEBUFFER_BINDING_, &s.readFbo);
    r.api.getIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_, &s.drawFbo);
    s.scissor = r.api.isEnabled(GL_SCISSOR_TEST_) != 0;
    s.srgb = r.api.isEnabled(GL_FRAMEBUFFER_SRGB_) != 0;
    r.api.Drain();
}
void Restore(const Saved& s) {
    if (s.haveRead) {
        r.api.bindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
        r.api.readBuffer(static_cast<GLenum>(s.readBuffer));
    }
    if (s.haveDraw) {
        r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
        r.api.drawBuffer(static_cast<GLenum>(s.drawBuffer));
    }
    r.api.bindFramebuffer(GL_READ_FRAMEBUFFER_, static_cast<GLuint>(s.readFbo));
    r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, static_cast<GLuint>(s.drawFbo));
    (s.scissor ? r.api.enable : r.api.disable)(GL_SCISSOR_TEST_);
    (s.srgb ? r.api.enable : r.api.disable)(GL_FRAMEBUFFER_SRGB_);
    r.api.Drain();
}

bool ContextAlive() {
    return r.api.coreLoaded && r.context != nullptr && r.api.getCurrentContext() == r.context;
}

// Everything sized to the window. GL objects are deleted only in their own context; without it
// current they are forgotten and die with it.
void Release() {
    const bool alive = ContextAlive();
    for (Crossing* c : {&r.in, &r.out}) {
        if (alive) {
            if (c->fbo != 0)
                r.api.deleteFramebuffers(1, &c->fbo);
            if (c->texture != 0)
                r.api.deleteTextures(1, &c->texture);
            if (c->memory != 0)
                r.api.deleteMemoryObjects(1, &c->memory);
        }
        *c = Crossing{};
    }
    if (alive) {
        if (r.resolveFbo != 0)
            r.api.deleteFramebuffers(1, &r.resolveFbo);
        if (r.resolveTexture != 0)
            r.api.deleteTextures(1, &r.resolveTexture);
        r.api.Drain();
    }
    r.resolveFbo = r.resolveTexture = 0;
    r.width = r.height = 0;
}

// The context presenting now, with its entry points and its GPU. False with the reason logged.
bool Bind() {
    if (!r.api.LoadCore()) {
        Log("x86bridge OpenGL: opengl32.dll has no wgl entry points");
        return false;
    }
    const HGLRC current = r.api.getCurrentContext();
    if (current == nullptr) {
        Log("x86bridge OpenGL: present with no current context");
        return false;
    }
    if (current == r.context)
        return true;
    if (r.context != nullptr) {
        Log("x86bridge OpenGL: the game replaced its context; the crossing is rebuilt on the new one");
        g.reset = true;
    }
    Release();  // the old context's objects are not current here: forgotten, not deleted
    r.context = current;
    if (!r.api.LoadForContext()) {
        Log("x86bridge OpenGL: this context has no GL_EXT_memory_object_win32 entry points");
        return false;
    }
    if (r.api.getUnsignedBytev != nullptr) {
        GLubyte bytes[8]{};
        r.api.Drain();
        r.api.getUnsignedBytev(GL_DEVICE_LUID_EXT_, bytes);
        if (r.api.Clean()) {
            std::memcpy(&r.luid, bytes, sizeof(r.luid));
            r.luidKnown = true;
        }
    }
    return true;
}

bool AdapterForLuid(LUID luid, ComPtr<IDXGIAdapter1>& out) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == luid.LowPart &&
            desc.AdapterLuid.HighPart == luid.HighPart) {
            out = adapter;
            return true;
        }
        adapter.Reset();
    }
    return false;
}

// Our D3D11 device on the GPU the context runs on: the one its LUID names. A context that reports
// none cannot be proved to share memory with anything, so the route stands down.
bool CreateDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& ctx, LUID& luid) {
    ComPtr<IDXGIAdapter1> adapter;
    if (!r.luidKnown || !AdapterForLuid(r.luid, adapter)) {
        Log("x86bridge OpenGL: the context reports no device LUID a DXGI adapter carries");
        return false;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                   &device, nullptr, &ctx);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1, D3D11_SDK_VERSION,
                               &device, nullptr, &ctx);
    if (FAILED(hr)) {
        Log("x86bridge OpenGL: D3D11CreateDevice on the context's adapter failed (hr=0x%08lX)",
            static_cast<unsigned long>(hr));
        return false;
    }
    luid = r.luid;
    return true;
}

bool Build(Crossing& c, ID3D11Device* dev, UINT w, UINT h, const char* what) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // what GL writes through an RGBA8 view
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    ComPtr<IDXGIResource1> res;
    HANDLE handle = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &c.on11)) || FAILED(c.on11.As(&res)) ||
        FAILED(res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                       nullptr, &handle))) {
        Log("x86bridge OpenGL: the shared %s texture could not be made", what);
        return false;
    }
    r.keptHandles.push_back(handle);
    r.api.Drain();
    r.api.createMemoryObjects(1, &c.memory);
    const GLint dedicated = 1;
    r.api.memoryObjectParameteriv(c.memory, GL_DEDICATED_MEMORY_OBJECT_EXT_, &dedicated);
    r.api.importMemoryWin32(c.memory, static_cast<GLuint64>(w) * h * 4,
                            GL_HANDLE_TYPE_D3D11_IMAGE_EXT_, handle);
    if (c.memory == 0 || !r.api.Clean()) {
        Log("x86bridge OpenGL: importing the %s texture as GL_HANDLE_TYPE_D3D11_IMAGE_EXT was refused",
            what);
        return false;
    }
    r.api.genTextures(1, &c.texture);
    r.api.bindTexture(GL_TEXTURE_2D_, c.texture);
    r.api.texParameteri(GL_TEXTURE_2D_, GL_TEXTURE_TILING_EXT_, static_cast<GLint>(GL_OPTIMAL_TILING_EXT_));
    r.api.texStorageMem2D(GL_TEXTURE_2D_, 1, GL_RGBA8_, static_cast<GLsizei>(w),
                          static_cast<GLsizei>(h), c.memory, 0);
    const bool stored = r.api.Clean();
    r.api.bindTexture(GL_TEXTURE_2D_, 0);
    if (!stored) {
        Log("x86bridge OpenGL: no storage on the imported %s memory", what);
        return false;
    }
    r.api.genFramebuffers(1, &c.fbo);
    r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, c.fbo);
    r.api.framebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, c.texture, 0);
    const GLenum status = r.api.checkFramebufferStatus(GL_DRAW_FRAMEBUFFER_);
    r.api.Drain();
    if (status != GL_FRAMEBUFFER_COMPLETE_) {
        Log("x86bridge OpenGL: the %s framebuffer is incomplete (0x%X)", what, status);
        return false;
    }
    return true;
}

// The two crossings at the window's size, and the resolve target when the default framebuffer is
// multisampled. Called with the game's context current (Bind).
bool Ensure(ID3D11Device* dev, UINT w, UINT h) {
    if (r.in.fbo != 0 && r.out.fbo != 0 && r.width == w && r.height == h)
        return true;
    Release();
    Saved saved{};
    Save(saved);
    r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    r.samples = 1;
    r.api.getIntegerv(GL_SAMPLES_, &r.samples);
    r.samples = r.samples > 1 ? r.samples : 1;  // 0 without multisampling
    r.api.Drain();
    bool ok = Build(r.in, dev, w, h, "colour") && Build(r.out, dev, w, h, "result");
    if (ok && r.samples > 1 && r.api.texStorage2D != nullptr) {
        r.api.genTextures(1, &r.resolveTexture);
        r.api.bindTexture(GL_TEXTURE_2D_, r.resolveTexture);
        r.api.texStorage2D(GL_TEXTURE_2D_, 1, GL_RGBA8_, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
        r.api.bindTexture(GL_TEXTURE_2D_, 0);
        r.api.genFramebuffers(1, &r.resolveFbo);
        r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, r.resolveFbo);
        r.api.framebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_,
                                   r.resolveTexture, 0);
        ok = r.api.checkFramebufferStatus(GL_DRAW_FRAMEBUFFER_) == GL_FRAMEBUFFER_COMPLETE_;
        r.api.Drain();
    } else if (ok && r.samples > 1) {
        Log("x86bridge OpenGL: the default framebuffer has %d samples and the context has no "
            "glTexStorage2D to resolve them into", r.samples);
        ok = false;
    }
    Restore(saved);
    if (!ok) {
        Release();
        return false;
    }
    r.width = w;
    r.height = h;
    Log("x86bridge OpenGL: %ux%u crossing built (%d sample(s)), D3D11 textures imported into the "
        "game's context", w, h, r.samples);
    return true;
}

// The default framebuffer into the colour crossing, flipped to D3D's top-down rows, and landed.
bool BlitIn() {
    Saved saved{};
    Save(saved);
    const GLint w = static_cast<GLint>(r.width), h = static_cast<GLint>(r.height);
    r.api.disable(GL_SCISSOR_TEST_);
    r.api.disable(GL_FRAMEBUFFER_SRGB_);
    r.api.bindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    r.api.getIntegerv(GL_READ_BUFFER_, &saved.readBuffer);
    saved.haveRead = true;
    r.api.readBuffer(GL_BACK_);
    r.api.Drain();
    if (r.samples > 1) {
        // A multisampled source cannot be flipped in the same blit that resolves it.
        r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, r.resolveFbo);
        r.api.blitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
        r.api.bindFramebuffer(GL_READ_FRAMEBUFFER_, r.resolveFbo);
        r.api.readBuffer(GL_COLOR_ATTACHMENT0_);
    }
    r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, r.in.fbo);
    r.api.blitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
    const bool ok = r.api.Clean();
    r.api.finish();
    Restore(saved);
    return ok;
}

// The result crossing back onto the default framebuffer. Not waited on: the next present's BlitIn
// finishes it before D3D11 writes the crossing again.
bool BlitOut() {
    Saved saved{};
    Save(saved);
    const GLint w = static_cast<GLint>(r.width), h = static_cast<GLint>(r.height);
    r.api.disable(GL_SCISSOR_TEST_);
    r.api.disable(GL_FRAMEBUFFER_SRGB_);
    r.api.bindFramebuffer(GL_READ_FRAMEBUFFER_, r.out.fbo);
    r.api.readBuffer(GL_COLOR_ATTACHMENT0_);
    r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    r.api.getIntegerv(GL_DRAW_BUFFER_, &saved.drawBuffer);
    saved.haveDraw = true;
    r.api.drawBuffer(GL_BACK_);
    r.api.Drain();
    r.api.blitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
    const bool ok = r.api.Clean();
    r.api.flush();
    Restore(saved);
    return ok;
}

}  // namespace gl32
