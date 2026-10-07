// The OpenGL route's state and its shared textures. Included by gl_route.inc, in namespace glroute.

// One direction of the crossing: a D3D12 texture, the handle that exported it, and the GL objects
// that see the same memory.
struct Crossing
{
    ComPtr<ID3D12Resource> on12;
    HANDLE handle = nullptr;  // owned by the route for the life of the process; never closed
    GLuint memory = 0, texture = 0, fbo = 0;

    // GL objects only, and only when their own context is current. The handle is kept, and on12
    // is released by the caller once nothing can still be reading it.
    void ReleaseGl(Api &api, bool contextAlive)
    {
        if (contextAlive)
        {
            if (fbo != 0 && api.deleteFramebuffers != nullptr)
                api.deleteFramebuffers(1, &fbo);
            if (texture != 0 && api.deleteTextures != nullptr)
                api.deleteTextures(1, &texture);
            if (memory != 0 && api.deleteMemoryObjects != nullptr)
                api.deleteMemoryObjects(1, &memory);
            api.Drain();
        }
        fbo = 0;
        texture = 0;
        memory = 0;
    }
};

struct Route
{
    bool failed = false;
    bool ready = false;
    bool loggedReady = false;
    bool loggedFirstFrame = false;
    bool loggedNoContext = false;
    bool loggedContextChange = false;
    bool loggedBackBuffer = false;
    bool loggedTransportOnly = false;

    Api api {};
    HGLRC context = nullptr;
    LUID luid {};
    bool luidKnown = false;

    Crossing in, out;
    UINT width = 0, height = 0;

    // The default framebuffer's own sample count, read from GL rather than from ReShade: ReShade
    // reports the back buffer as one sample whatever the window was created with, and a
    // multisampled default framebuffer cannot be blitted into an ordinary texture with a flip in
    // one step. When this is above 1 the frame lands in `resolve` first.
    GLint defaultSamples = 1;
    GLuint resolveTexture = 0, resolveFbo = 0;

    // The imported fences, when the driver has them. glprobe proved they work here; this is the
    // route using them, which is a different claim.
    GLuint semToGl = 0, semToD3D = 0;
    bool semaphores = false;
    bool semaphoresProven = false;   // GL's first signal was seen landing, on the CPU
    bool loggedSemaphores = false;
    UINT64 toD3DValue = 0, toGlValue = 0;

    // Frames that had a composition of their own. The strobe the first semaphore build shipped (the
    // game running ahead, every frame the network could not answer going out raw) is kept off by
    // JobGate's wait for the previous job, not by repeating the last result; the log line that
    // prints this against the frame count is how a GL run shows the two stay close.
    UINT64 freshFrames = 0;

    // Deliberately leaked, one per crossing ever built. See rule 1 in gl_route.inc: closing these
    // is what faults the driver, and there are two per swapchain size the game ever uses.
    std::vector<HANDLE> keptHandles;

    void Stop(const char *why)
    {
        failed = true;
        ready = false;
        g.status.unavailable = true;  // so the panel says it, as for every other stand-down
        g.status.reason = why;        // a literal at every call
        Log("opengl: %s Standing down and leaving the host's own image alone.", why);
    }
};

Route &g_route = *new Route;  // never destroyed, as State g is: see neural.cpp

// Where did it die? This route drives a driver that has already been seen to fault inside its own
// ICD, from its own thread, in a call unrelated to the one that caused it (see glprobe). A crash
// with no module name is unattributable, and "the game closed" is all the user would report. The
// filter names the module and the offset and then lets the previous handler run, so nothing else
// changes about how the process dies.
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
bool filterInstalled = false;  // only then does an unload have a filter to put back

LONG WINAPI ReportFault(EXCEPTION_POINTERS *info)
{
    void *at = info->ExceptionRecord->ExceptionAddress;
    HMODULE module = nullptr;
    wchar_t name[MAX_PATH] = L"(unknown module)";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(at), &module) &&
        module != nullptr)
        GetModuleFileNameW(module, name, MAX_PATH);
    Log("FAULT: exception %08lX at %p = %ls+0x%llX on thread %lu.",
        info->ExceptionRecord->ExceptionCode, at, name,
        module != nullptr ? static_cast<unsigned long long>(
                                reinterpret_cast<uintptr_t>(at) -
                                reinterpret_cast<uintptr_t>(module))
                          : 0ull,
        GetCurrentThreadId());
    return g_previousFilter != nullptr ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

// The crossing is always this pair, whatever ReShade says the back buffer is. See gl_route.inc:
// GL writes through an RGBA8 view, so D3D12 has to read an RGBA8 resource or the picture comes
// back with red and blue exchanged.
constexpr DXGI_FORMAT kCrossingFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr GLenum kCrossingGlFormat = GL_RGBA8_;

bool AdapterForLuid(LUID luid, ComPtr<IDXGIAdapter> &out)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(DxgiCreateFactory(0, IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == luid.LowPart &&
            desc.AdapterLuid.HighPart == luid.HighPart)
            return SUCCEEDED(adapter.As(&out));
        adapter.Reset();
    }
    return false;
}

// ---- the state the route borrows and puts back -----------------------------------------------
//
// OpenGL has no command buffer to record into and no descriptor to scope: everything below is
// global to the context, and the game is in the middle of using it. Anything left changed here
// surfaces somewhere else entirely -- a HUD that stops drawing, a scissor that clips the world --
// with nothing to connect it back to this add-on. Blits are affected by the scissor test and by
// sRGB conversion and by nothing else, so those two plus the bindings and the buffer selection
// are the whole footprint.
struct SavedState
{
    GLint readFbo = 0, drawFbo = 0;
    GLint readBuffer = 0, drawBuffer = 0;
    bool scissor = false, srgb = false;
    bool haveReadBuffer = false, haveDrawBuffer = false;
};

void Save(Api &api, SavedState &s)
{
    api.getIntegerv(GL_READ_FRAMEBUFFER_BINDING_, &s.readFbo);
    api.getIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_, &s.drawFbo);
    s.scissor = api.isEnabled(GL_SCISSOR_TEST_) != 0;
    s.srgb = api.isEnabled(GL_FRAMEBUFFER_SRGB_) != 0;
    api.Drain();
}

void Restore(Api &api, const SavedState &s)
{
    // Buffer selection is per framebuffer, so each one is restored while its own framebuffer is
    // bound -- which is also why it was read that way.
    if (s.haveReadBuffer)
    {
        api.bindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
        api.readBuffer(static_cast<GLenum>(s.readBuffer));
    }
    if (s.haveDrawBuffer)
    {
        api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
        api.drawBuffer(static_cast<GLenum>(s.drawBuffer));
    }
    api.bindFramebuffer(GL_READ_FRAMEBUFFER_, static_cast<GLuint>(s.readFbo));
    api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, static_cast<GLuint>(s.drawFbo));
    if (s.scissor)
        api.enable(GL_SCISSOR_TEST_);
    else
        api.disable(GL_SCISSOR_TEST_);
    if (s.srgb)
        api.enable(GL_FRAMEBUFFER_SRGB_);
    else
        api.disable(GL_FRAMEBUFFER_SRGB_);
    api.Drain();
}

// ---- building one direction of the crossing --------------------------------------------------

bool BuildCrossing(Route &r, Crossing &c, UINT w, UINT h, const char *what)
{
    // Everything below issues GL calls through the same hooks the game uses, so ReShade will
    // report them back to this add-on. SelfIssued is what stops the observers from treating the
    // route's own work as the game's -- and, on the present thread, from re-entering a lock this
    // add-on already holds.
    SelfIssued selfIssued;
    c.ReleaseGl(r.api, true);
    c.on12.Reset();
    // The old handle is not closed and not reused: it goes on the kept list and stays open.
    if (c.handle != nullptr)
    {
        r.keptHandles.push_back(c.handle);
        c.handle = nullptr;
    }

    Log("opengl: building the %s crossing at %ux%u.", what, w, h);

    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = kCrossingFormat;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    HRESULT hr = g.bridge.workDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&c.on12));
    if (FAILED(hr))
    {
        Log("opengl: creating the shared %s texture failed 0x%08lX.", what, hr);
        return false;
    }
    hr = g.bridge.workDevice->CreateSharedHandle(c.on12.Get(), nullptr, GENERIC_ALL, nullptr, &c.handle);
    if (FAILED(hr))
    {
        Log("opengl: exporting the shared %s handle failed 0x%08lX.", what, hr);
        return false;
    }
    const UINT64 bytes = g.bridge.workDevice->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes;
    Log("opengl:   %s D3D12 texture created and exported, %llu bytes.", what,
        static_cast<unsigned long long>(bytes));

    r.api.Drain();
    r.api.createMemoryObjects(1, &c.memory);
    if (c.memory == 0)
    {
        Log("opengl: no memory object for %s.", what);
        return false;
    }
    // Before the import, not after: a D3D12 resource handle names one resource rather than a heap
    // to suballocate, and the dedication is fixed at import time.
    Log("opengl:   %s memory object %u.", what, c.memory);
    const GLint dedicated = 1;
    r.api.memoryObjectParameteriv(c.memory, GL_DEDICATED_MEMORY_OBJECT_EXT_, &dedicated);
    if (!r.api.Clean())
    {
        Log("opengl: the driver refused the dedicated flag on the %s memory object.", what);
        return false;
    }
    Log("opengl:   %s dedicated flag accepted; importing the handle.", what);
    r.api.importMemoryWin32(c.memory, bytes, GL_HANDLE_TYPE_D3D12_RESOURCE_EXT_, c.handle);
    if (!r.api.Clean())
    {
        // Named handle type, no fallback. OPAQUE_WIN32 is accepted by this driver for a D3D12
        // handle and is wrong; taking it would trade a clean stand-down for a driver fault later.
        Log("opengl: importing the %s texture as GL_HANDLE_TYPE_D3D12_RESOURCE_EXT was refused.",
            what);
        return false;
    }

    Log("opengl:   %s memory imported; creating the texture.", what);
    r.api.genTextures(1, &c.texture);
    r.api.bindTexture(GL_TEXTURE_2D_, c.texture);
    // The D3D12 resource is D3D12_TEXTURE_LAYOUT_UNKNOWN -- the driver's own swizzled layout --
    // and OPTIMAL is how that is spelled here. LINEAR would be a different layout over the same
    // bytes and would read as noise rather than as an error.
    r.api.texParameteri(GL_TEXTURE_2D_, GL_TEXTURE_TILING_EXT_,
                        static_cast<GLint>(GL_OPTIMAL_TILING_EXT_));
    r.api.Drain();
    r.api.texStorageMem2D(GL_TEXTURE_2D_, 1, kCrossingGlFormat, static_cast<GLsizei>(w),
                          static_cast<GLsizei>(h), c.memory, 0);
    const bool storageOk = r.api.Clean();
    r.api.bindTexture(GL_TEXTURE_2D_, 0);
    if (!storageOk)
    {
        Log("opengl: glTexStorageMem2DEXT on the imported %s memory failed.", what);
        return false;
    }

    Log("opengl:   %s storage created on texture %u; building the FBO.", what, c.texture);
    // The FBO is built once and kept. Attaching per frame would be legal and would put an
    // avoidable driver validation in the middle of every present.
    r.api.genFramebuffers(1, &c.fbo);
    r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, c.fbo);
    r.api.framebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_,
                               c.texture, 0);
    const GLenum status = r.api.checkFramebufferStatus(GL_DRAW_FRAMEBUFFER_);
    r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    r.api.Drain();
    if (status != GL_FRAMEBUFFER_COMPLETE_)
    {
        Log("opengl: the %s FBO is incomplete (status 0x%X). The texture imported and cannot be "
            "rendered to, so the blit has nothing to land in.", what, status);
        return false;
    }

    Log("opengl: %s %ux%u crossed as a D3D12 resource imported into the host's context "
        "(%llu bytes, GL texture %u, fbo %u).", what, w, h,
        static_cast<unsigned long long>(bytes), c.texture, c.fbo);
    return true;
}
