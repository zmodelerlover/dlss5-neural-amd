// The OpenGL route: the Vulkan route's shape, with the host's half rebuilt around a framebuffer
// blit, because in OpenGL there is no back buffer to copy.
//
// What is the same, and is the same for the same reason: the network runs on our own private
// D3D12 device (CreateWorkDevice), so the shared textures are created THERE, exported as NT
// handles and imported into the host -- never the other way round. A handle exported from OpenGL
// is not something D3D12 can open.
//
//   D3D12 CreateCommittedResource(D3D12_HEAP_FLAG_SHARED) -> CreateSharedHandle
//     -> glCreateMemoryObjectsEXT, GL_DEDICATED_MEMORY_OBJECT_EXT = 1
//     -> glImportMemoryWin32HandleEXT(GL_HANDLE_TYPE_D3D12_RESOURCE_EXT)
//     -> glTexStorageMem2DEXT, then an FBO wrapping that texture
//
// What is different, measured under Luanti with the glinfo add-on rather than assumed:
//
//   * ReShade hands this route a back buffer whose handle is GL_FRAMEBUFFER_DEFAULT with the name
//     GL_BACK, and whose resource_type is `surface`, not texture_2d. There is no texture to copy,
//     so the crossing is glBlitFramebuffer between FBO 0 and an FBO of ours. Every copy in this
//     file is a blit for that reason.
//   * There is no command buffer. ReShade reports an immediate command list whose native handle
//     is a synthetic value; GL commands go straight into the current context on the calling
//     thread. So the route issues its own GL calls and puts back every piece of state it touched.
//   * The context can be replaced under us. Under SDL -- Luanti, and a great many OpenGL games --
//     contexts are created and destroyed during start-up, and ReShade loads and unloads its
//     add-ons once per context: seven times in 230 ms, measured. Every GL object here belongs to
//     one context and dies with it, so the route keys everything on wglGetCurrentContext() and
//     rebuilds when that changes.
//
// The three rules glprobe paid for, which are not in any specification:
//
//   1. The shared NT handle is NOT ours to close. Vulkan takes its own reference and lets the
//      application close the handle it exported; EXT_memory_object_win32 is written the same way
//      and this driver does not honour it. Closing a handle after the import faults inside the
//      ICD, from a driver thread, some time later -- so the crash lands in an unrelated GL call
//      and looks like anything but its cause. Measured: 3 runs in 8 died with closing, 0 in 8
//      without. Handles therefore go into a list and stay open for the life of the process. A
//      resize leaks two kernel handles, deliberately.
//   2. The memory object must be declared dedicated BEFORE the import (D3D12 resource handles
//      back exactly one resource), the same rule the Vulkan route meets as
//      VkMemoryDedicatedAllocateInfo.
//   3. GL_HANDLE_TYPE_OPAQUE_WIN32_EXT is ACCEPTED for a D3D12 resource handle on this driver and
//      is the wrong type. "The import succeeded" proves nothing about the handle type, so this
//      route names D3D12_RESOURCE and stands down if that is refused rather than falling back.
//
// Channel order, which is the one thing here that would silently look like a bug in the network:
// the crossing is created as R8G8B8A8_UNORM even though ReShade reports the GL back buffer as
// B8G8R8A8_UNORM. That report is ReShade's own description of the default framebuffer, not a
// statement about memory. The blit writes our texture through GL's view of it, which is RGBA8 --
// byte 0 is red -- and D3D12 has to read the same bytes the same way round. Creating the crossing
// as BGRA would put blue where the network expects red, with no error anywhere and a picture that
// looks plausible until you notice the sky is orange.
//
// Synchronisation is the CPU stall the D3D11 route has run ten thousand frames on: glFinish, then
// hand over; wait for our own queue, then blit back. glprobe proved that the imported D3D12
// fences work here as GL semaphores -- glWaitSemaphoreEXT on an imported fence completed -- so a
// GPU-side handover is available and is the obvious next step. It is not the first version: this
// route has enough new surface without also being the first one in the project not to stall.

namespace glroute
{

// ---- the slice of OpenGL this route needs ----------------------------------------------------

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLubyte = unsigned char;
using GLuint64 = unsigned long long;
using GLbitfield = unsigned int;
using GLboolean = unsigned char;

constexpr GLenum GL_NO_ERROR_ = 0;
constexpr GLenum GL_TEXTURE_2D_ = 0x0DE1;
constexpr GLenum GL_RGBA8_ = 0x8058;
constexpr GLenum GL_SCISSOR_TEST_ = 0x0C11;
constexpr GLenum GL_FRAMEBUFFER_SRGB_ = 0x8DB9;
constexpr GLenum GL_DRAW_BUFFER_ = 0x0C01;
constexpr GLenum GL_READ_BUFFER_ = 0x0C02;
constexpr GLenum GL_BACK_ = 0x0405;
constexpr GLenum GL_COLOR_BUFFER_BIT_ = 0x4000;
constexpr GLenum GL_NEAREST_ = 0x2600;
constexpr GLenum GL_READ_FRAMEBUFFER_ = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER_ = 0x8CA9;
constexpr GLenum GL_READ_FRAMEBUFFER_BINDING_ = 0x8CAA;
constexpr GLenum GL_DRAW_FRAMEBUFFER_BINDING_ = 0x8CA6;
constexpr GLenum GL_COLOR_ATTACHMENT0_ = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5;
constexpr GLenum GL_TEXTURE_TILING_EXT_ = 0x9580;
constexpr GLenum GL_DEDICATED_MEMORY_OBJECT_EXT_ = 0x9581;
constexpr GLenum GL_OPTIMAL_TILING_EXT_ = 0x9584;
constexpr GLenum GL_HANDLE_TYPE_D3D12_RESOURCE_EXT_ = 0x958A;
constexpr GLenum GL_DEVICE_LUID_EXT_ = 0x9599;
constexpr GLenum GL_HANDLE_TYPE_D3D12_FENCE_EXT_ = 0x9594;
constexpr GLenum GL_D3D12_FENCE_VALUE_EXT_ = 0x9595;
constexpr GLenum GL_LAYOUT_TRANSFER_SRC_EXT_ = 0x9592;
constexpr GLenum GL_SAMPLES_ = 0x80A9;
constexpr GLenum GL_RENDERBUFFER_ = 0x8D41;

using PFN_glGetError = GLenum(APIENTRY *)();
using PFN_glGetIntegerv = void(APIENTRY *)(GLenum, GLint *);
using PFN_glGenTextures = void(APIENTRY *)(GLsizei, GLuint *);
using PFN_glDeleteTextures = void(APIENTRY *)(GLsizei, const GLuint *);
using PFN_glBindTexture = void(APIENTRY *)(GLenum, GLuint);
using PFN_glTexParameteri = void(APIENTRY *)(GLenum, GLenum, GLint);
using PFN_glFinish = void(APIENTRY *)();
using PFN_glFlush = void(APIENTRY *)();
using PFN_glEnable = void(APIENTRY *)(GLenum);
using PFN_glDisable = void(APIENTRY *)(GLenum);
using PFN_glIsEnabled = GLboolean(APIENTRY *)(GLenum);
using PFN_glReadBuffer = void(APIENTRY *)(GLenum);
using PFN_glDrawBuffer = void(APIENTRY *)(GLenum);

using PFN_glGetUnsignedBytevEXT = void(APIENTRY *)(GLenum, GLubyte *);
using PFN_glCreateMemoryObjectsEXT = void(APIENTRY *)(GLsizei, GLuint *);
using PFN_glDeleteMemoryObjectsEXT = void(APIENTRY *)(GLsizei, const GLuint *);
using PFN_glMemoryObjectParameterivEXT = void(APIENTRY *)(GLuint, GLenum, const GLint *);
using PFN_glImportMemoryWin32HandleEXT = void(APIENTRY *)(GLuint, GLuint64, GLenum, void *);
using PFN_glTexStorageMem2DEXT = void(APIENTRY *)(GLenum, GLsizei, GLenum, GLsizei, GLsizei,
                                                  GLuint, GLuint64);
using PFN_glGenSemaphoresEXT = void(APIENTRY *)(GLsizei, GLuint *);
using PFN_glDeleteSemaphoresEXT = void(APIENTRY *)(GLsizei, const GLuint *);
using PFN_glImportSemaphoreWin32HandleEXT = void(APIENTRY *)(GLuint, GLenum, void *);
using PFN_glSemaphoreParameterui64vEXT = void(APIENTRY *)(GLuint, GLenum, const GLuint64 *);
using PFN_glWaitSemaphoreEXT = void(APIENTRY *)(GLuint, GLuint, const GLuint *, GLuint,
                                                const GLuint *, const GLenum *);
using PFN_glSignalSemaphoreEXT = void(APIENTRY *)(GLuint, GLuint, const GLuint *, GLuint,
                                                  const GLuint *, const GLenum *);
using PFN_glTexStorage2D = void(APIENTRY *)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
using PFN_glGenFramebuffers = void(APIENTRY *)(GLsizei, GLuint *);
using PFN_glDeleteFramebuffers = void(APIENTRY *)(GLsizei, const GLuint *);
using PFN_glBindFramebuffer = void(APIENTRY *)(GLenum, GLuint);
using PFN_glFramebufferTexture2D = void(APIENTRY *)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_glCheckFramebufferStatus = GLenum(APIENTRY *)(GLenum);
using PFN_glBlitFramebuffer = void(APIENTRY *)(GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                                               GLint, GLbitfield, GLenum);

using PFN_wglGetCurrentContext = HGLRC(WINAPI *)();
using PFN_wglGetProcAddress = PROC(WINAPI *)(LPCSTR);

// opengl32.dll is resolved by hand and never imported. A static import would pull OpenGL into
// every D3D11 and D3D12 game this add-on loads into, and neural.cpp already records what one
// unwanted import did to NFS 2015's ResizeBuffers.
struct Api
{
    HMODULE module = nullptr;

    PFN_wglGetCurrentContext getCurrentContext = nullptr;
    PFN_wglGetProcAddress getProcAddress = nullptr;

    PFN_glGetError getError = nullptr;
    PFN_glGetIntegerv getIntegerv = nullptr;
    PFN_glGenTextures genTextures = nullptr;
    PFN_glDeleteTextures deleteTextures = nullptr;
    PFN_glBindTexture bindTexture = nullptr;
    PFN_glTexParameteri texParameteri = nullptr;
    PFN_glFinish finish = nullptr;
    PFN_glFlush flush = nullptr;
    PFN_glEnable enable = nullptr;
    PFN_glDisable disable = nullptr;
    PFN_glIsEnabled isEnabled = nullptr;
    PFN_glReadBuffer readBuffer = nullptr;
    PFN_glDrawBuffer drawBuffer = nullptr;

    PFN_glGetUnsignedBytevEXT getUnsignedBytev = nullptr;
    PFN_glCreateMemoryObjectsEXT createMemoryObjects = nullptr;
    PFN_glDeleteMemoryObjectsEXT deleteMemoryObjects = nullptr;
    PFN_glMemoryObjectParameterivEXT memoryObjectParameteriv = nullptr;
    PFN_glImportMemoryWin32HandleEXT importMemoryWin32 = nullptr;
    PFN_glTexStorageMem2DEXT texStorageMem2D = nullptr;
    PFN_glTexStorage2D texStorage2D = nullptr;
    PFN_glGenSemaphoresEXT genSemaphores = nullptr;
    PFN_glDeleteSemaphoresEXT deleteSemaphores = nullptr;
    PFN_glImportSemaphoreWin32HandleEXT importSemaphoreWin32 = nullptr;
    PFN_glSemaphoreParameterui64vEXT semaphoreParameterui64v = nullptr;
    PFN_glWaitSemaphoreEXT waitSemaphore = nullptr;
    PFN_glSignalSemaphoreEXT signalSemaphore = nullptr;
    PFN_glGenFramebuffers genFramebuffers = nullptr;
    PFN_glDeleteFramebuffers deleteFramebuffers = nullptr;
    PFN_glBindFramebuffer bindFramebuffer = nullptr;
    PFN_glFramebufferTexture2D framebufferTexture2D = nullptr;
    PFN_glCheckFramebufferStatus checkFramebufferStatus = nullptr;
    PFN_glBlitFramebuffer blitFramebuffer = nullptr;

    bool coreLoaded = false;

    template <typename T>
    void FromModule(const char *name, T &out)
    {
        out = reinterpret_cast<T>(GetProcAddress(module, name));
    }
    // WGL says extension functions come from wglGetProcAddress and GL 1.1 ones from the module;
    // drivers disagree about everything in between, so try both before believing a null.
    template <typename T>
    void FromContext(const char *name, T &out)
    {
        out = reinterpret_cast<T>(getProcAddress(name));
        if (out == nullptr)
            FromModule(name, out);
    }

    bool LoadCore()
    {
        if (coreLoaded)
            return true;
        module = GetModuleHandleW(L"opengl32.dll");
        if (module == nullptr)
            module = LoadLibraryW(L"opengl32.dll");
        if (module == nullptr)
            return false;
        FromModule("wglGetCurrentContext", getCurrentContext);
        FromModule("wglGetProcAddress", getProcAddress);
        FromModule("glGetError", getError);
        FromModule("glGetIntegerv", getIntegerv);
        FromModule("glGenTextures", genTextures);
        FromModule("glDeleteTextures", deleteTextures);
        FromModule("glBindTexture", bindTexture);
        FromModule("glTexParameteri", texParameteri);
        FromModule("glFinish", finish);
        FromModule("glFlush", flush);
        FromModule("glEnable", enable);
        FromModule("glDisable", disable);
        FromModule("glIsEnabled", isEnabled);
        FromModule("glReadBuffer", readBuffer);
        FromModule("glDrawBuffer", drawBuffer);
        coreLoaded = getCurrentContext != nullptr && getProcAddress != nullptr &&
                     getError != nullptr && getIntegerv != nullptr && genTextures != nullptr &&
                     bindTexture != nullptr && finish != nullptr && flush != nullptr &&
                     readBuffer != nullptr &&
                     drawBuffer != nullptr && isEnabled != nullptr;
        return coreLoaded;
    }

    // Per context, and only with that context current. A pointer taken from one context is not
    // promised to work in another, which matters here because the host replaces its context.
    bool LoadForContext()
    {
        FromContext("glGetUnsignedBytevEXT", getUnsignedBytev);
        FromContext("glCreateMemoryObjectsEXT", createMemoryObjects);
        FromContext("glDeleteMemoryObjectsEXT", deleteMemoryObjects);
        FromContext("glMemoryObjectParameterivEXT", memoryObjectParameteriv);
        FromContext("glImportMemoryWin32HandleEXT", importMemoryWin32);
        FromContext("glTexStorageMem2DEXT", texStorageMem2D);
        FromContext("glTexStorage2D", texStorage2D);
        FromContext("glGenSemaphoresEXT", genSemaphores);
        FromContext("glDeleteSemaphoresEXT", deleteSemaphores);
        FromContext("glImportSemaphoreWin32HandleEXT", importSemaphoreWin32);
        FromContext("glSemaphoreParameterui64vEXT", semaphoreParameterui64v);
        FromContext("glWaitSemaphoreEXT", waitSemaphore);
        FromContext("glSignalSemaphoreEXT", signalSemaphore);
        FromContext("glGenFramebuffers", genFramebuffers);
        FromContext("glDeleteFramebuffers", deleteFramebuffers);
        FromContext("glBindFramebuffer", bindFramebuffer);
        FromContext("glFramebufferTexture2D", framebufferTexture2D);
        FromContext("glCheckFramebufferStatus", checkFramebufferStatus);
        FromContext("glBlitFramebuffer", blitFramebuffer);
        return createMemoryObjects != nullptr && memoryObjectParameteriv != nullptr &&
               importMemoryWin32 != nullptr && texStorageMem2D != nullptr &&
               genFramebuffers != nullptr && bindFramebuffer != nullptr &&
               framebufferTexture2D != nullptr && blitFramebuffer != nullptr;
    }

    // GL keeps errors in a queue and reports the oldest first. Draining before a sequence means a
    // failure reported after it belongs to that sequence; draining after means nothing of ours is
    // left in the queue for the game to find. The game's own errors are drained with them, which
    // is a real if small side effect of being here at all.
    void Drain()
    {
        for (int i = 0; i < 16 && getError() != GL_NO_ERROR_; ++i)
            ;
    }
    bool Clean()
    {
        bool clean = true;
        for (int i = 0; i < 16; ++i)
        {
            const GLenum e = getError();
            if (e == GL_NO_ERROR_)
                break;
            clean = false;
        }
        return clean;
    }
};

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
    bool semaphoresProven = false;   // the first frame completed with them in the loop
    bool loggedSemaphores = false;
    UINT64 toD3DValue = 0, toGlValue = 0;

    // Holding the last result. Without the CPU stall the game runs ahead of the network, and on a
    // frame where the previous evaluation has not finished there is no new result to show. The
    // other routes never meet this: they stall, so every frame is a fresh one. Leaving the game's
    // own image on those frames is what the first semaphore build did, and alternating between a
    // corrected frame and an uncorrected one at a hundred and ninety a second is a strobe, not an
    // effect. The result texture still holds the last composed frame, so it is shown again.
    unsigned held = 0;
    bool haveResult = false;
    bool loggedHold = false;
    UINT64 freshFrames = 0, heldFrames = 0;

    // Deliberately leaked, one per crossing ever built. See rule 1 in the header: closing these
    // is what faults the driver, and there are two per swapchain size the game ever uses.
    std::vector<HANDLE> keptHandles;

    void Stop(const char *why)
    {
        failed = true;
        ready = false;
        Log("opengl: %s Standing down and leaving the host's own image alone.", why);
    }
};

Route g_route;

// Where did it die? This route drives a driver that has already been seen to fault inside its own
// ICD, from its own thread, in a call unrelated to the one that caused it (see glprobe). A crash
// with no module name is unattributable, and "the game closed" is all the user would report. The
// filter names the module and the offset and then lets the previous handler run, so nothing else
// changes about how the process dies.
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

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

// The crossing is always this pair, whatever ReShade says the back buffer is. See the header:
// GL writes through an RGBA8 view, so D3D12 has to read an RGBA8 resource or the picture comes
// back with red and blue exchanged.
constexpr DXGI_FORMAT kCrossingFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr GLenum kCrossingGlFormat = GL_RGBA8_;

bool AdapterForLuid(LUID luid, ComPtr<IDXGIAdapter> &out)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
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

// The GL side of a teardown. Called from ReleaseSwapchainSized, which may run on a thread with no
// context at all -- a swapchain can be destroyed from anywhere -- so it checks before touching a
// single GL object. What it cannot delete it forgets: those objects die with their context.
void ReleaseSwapchainSized()
{
    Route &r = g_route;
    r.ready = false;

    SelfIssued selfIssued;
    const bool contextAlive = r.api.coreLoaded && r.context != nullptr &&
                              r.api.getCurrentContext() == r.context;
    if (r.api.coreLoaded && !contextAlive && (r.in.texture != 0 || r.out.texture != 0))
        Log("opengl: releasing without the route's context current; the GL objects are left to "
            "die with it. This is not a leak of anything the driver still tracks.");
    if (contextAlive)
        r.api.finish();
    r.in.ReleaseGl(r.api, contextAlive);
    r.out.ReleaseGl(r.api, contextAlive);
    if (r.resolveFbo != 0 || r.resolveTexture != 0)
    {
        if (contextAlive)
        {
            if (r.resolveFbo != 0)
                r.api.deleteFramebuffers(1, &r.resolveFbo);
            if (r.resolveTexture != 0)
                r.api.deleteTextures(1, &r.resolveTexture);
            r.api.Drain();
        }
        r.resolveFbo = 0;
        r.resolveTexture = 0;
    }
    if (r.in.handle != nullptr)
    {
        r.keptHandles.push_back(r.in.handle);
        r.in.handle = nullptr;
    }
    if (r.out.handle != nullptr)
    {
        r.keptHandles.push_back(r.out.handle);
        r.out.handle = nullptr;
    }
    r.in.on12.Reset();
    r.out.on12.Reset();
    r.width = 0;
    r.height = 0;
    r.loggedReady = false;
    r.loggedFirstFrame = false;
}

// ---- the imported fences ----------------------------------------------------------------------
//
// A D3D12 fence is a 64-bit monotonic counter and EXT_semaphore_win32 carries the value to wait
// for as a parameter on the semaphore, which is how a counter fits through an interface built for
// binary semaphores. Two of them, one per direction, exactly as the Vulkan route imports them:
// crossHandle is D3D12 signalling the host, backHandle is the host signalling D3D12.
//
// These replace two CPU stalls a frame -- a glFinish and a fence wait -- with two queue-side
// waits. The route does not trust them on the strength of the specification: the first frame
// still confirms on the CPU that the pipeline completed, and if it does not, the route falls back
// to stalling and says so. A wait that never completes is a hung GPU and a game that has to be
// killed, which is not a failure mode to discover in someone else's session.
bool ImportFences(Route &r)
{
    if (r.api.importSemaphoreWin32 == nullptr || r.api.genSemaphores == nullptr ||
        r.api.semaphoreParameterui64v == nullptr || r.api.waitSemaphore == nullptr ||
        r.api.signalSemaphore == nullptr)
        return false;
    if (g.bridge.crossHandle == nullptr || g.bridge.backHandle == nullptr)
        return false;

    GLuint pair[2] { 0, 0 };
    r.api.Drain();
    r.api.genSemaphores(2, pair);
    if (!r.api.Clean() || pair[0] == 0 || pair[1] == 0)
        return false;
    r.api.importSemaphoreWin32(pair[0], GL_HANDLE_TYPE_D3D12_FENCE_EXT_, g.bridge.crossHandle);
    const bool first = r.api.Clean();
    r.api.importSemaphoreWin32(pair[1], GL_HANDLE_TYPE_D3D12_FENCE_EXT_, g.bridge.backHandle);
    const bool second = r.api.Clean();
    if (!first || !second)
    {
        if (r.api.deleteSemaphores != nullptr)
            r.api.deleteSemaphores(2, pair);
        r.api.Drain();
        return false;
    }
    r.semToGl = pair[0];
    r.semToD3D = pair[1];
    // The fences already carry whatever the D3D11 route left on them in another life; start above
    // their current values so every wait below is for a value that has not been reached yet.
    r.toGlValue = g.bridge.crossFence->GetCompletedValue();
    r.toD3DValue = g.bridge.backFence->GetCompletedValue();
    return true;
}

// The default framebuffer's sample count, read from GL. ReShade reports the back buffer as one
// sample whatever the window actually has, so this is the only honest source. A multisampled
// default framebuffer needs one more step on the way in, because a blit that resolves samples and
// mirrors Y in the same call is not something every driver accepts.
void ReadDefaultSamples(Route &r)
{
    SavedState saved {};
    Save(r.api, saved);
    r.api.bindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    r.api.Drain();
    GLint samples = 1;
    r.api.getIntegerv(GL_SAMPLES_, &samples);
    if (!r.api.Clean() || samples < 1)
        samples = 1;
    r.defaultSamples = samples;
    Restore(r.api, saved);
}

// An ordinary single-sample texture the resolve lands in before the flip. Only built when the
// default framebuffer is multisampled.
bool BuildResolveTarget(Route &r, UINT w, UINT h)
{
    if (r.api.texStorage2D == nullptr)
    {
        Log("opengl: the default framebuffer is multisampled and this context has no "
            "glTexStorage2D, so there is nowhere to resolve it to.");
        return false;
    }
    if (r.resolveFbo != 0)
        r.api.deleteFramebuffers(1, &r.resolveFbo);
    if (r.resolveTexture != 0)
        r.api.deleteTextures(1, &r.resolveTexture);
    r.resolveFbo = 0;
    r.resolveTexture = 0;
    r.api.Drain();

    r.api.genTextures(1, &r.resolveTexture);
    r.api.bindTexture(GL_TEXTURE_2D_, r.resolveTexture);
    r.api.texStorage2D(GL_TEXTURE_2D_, 1, kCrossingGlFormat, static_cast<GLsizei>(w),
                       static_cast<GLsizei>(h));
    r.api.bindTexture(GL_TEXTURE_2D_, 0);
    if (!r.api.Clean())
    {
        Log("opengl: could not create the resolve texture.");
        return false;
    }
    r.api.genFramebuffers(1, &r.resolveFbo);
    r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, r.resolveFbo);
    r.api.framebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_,
                               r.resolveTexture, 0);
    const GLenum status = r.api.checkFramebufferStatus(GL_DRAW_FRAMEBUFFER_);
    r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    r.api.Drain();
    if (status != GL_FRAMEBUFFER_COMPLETE_)
    {
        Log("opengl: the resolve FBO is incomplete (status 0x%X).", status);
        return false;
    }
    Log("opengl: the default framebuffer has %d samples; resolving through an intermediate "
        "texture on the way in.", r.defaultSamples);
    return true;
}

// ---- everything that happens once, plus the resize path --------------------------------------

bool Ensure(Route &r, UINT w, UINT h)
{
    if (r.failed)
        return false;

    HGLRC current = r.api.getCurrentContext();
    if (current == nullptr)
    {
        if (!r.loggedNoContext)
        {
            r.loggedNoContext = true;
            Log("opengl: present arrived on a thread with no current GL context. Nothing can be "
                "done from here, so these frames are left alone.");
        }
        return false;
    }
    if (current != r.context)
    {
        // A new context owns none of the old one's objects, and the old one may already be gone.
        // Forget rather than delete, and rebuild against what is current now.
        if (r.context != nullptr && !r.loggedContextChange)
        {
            r.loggedContextChange = true;
            Log("opengl: the host replaced its GL context. Rebuilding the crossing against the "
                "new one; the old objects died with it.");
        }
        r.in.ReleaseGl(r.api, false);
        r.out.ReleaseGl(r.api, false);
        if (r.in.handle != nullptr)
        {
            r.keptHandles.push_back(r.in.handle);
            r.in.handle = nullptr;
        }
        if (r.out.handle != nullptr)
        {
            r.keptHandles.push_back(r.out.handle);
            r.out.handle = nullptr;
        }
        r.in.on12.Reset();
        r.out.on12.Reset();
        r.context = current;
        r.ready = false;
        r.width = 0;
        r.height = 0;
        if (!r.api.LoadForContext())
        {
            r.Stop("this GL context has no external-memory entry points. The driver advertises "
                   "GL_EXT_memory_object_win32 or it does not, and after context creation that "
                   "cannot be changed.");
            return false;
        }
        r.luidKnown = false;
    }

    // Which GPU is this context on? EXT_memory_object reports the LUID, which is the identifier
    // D3D12 uses, so the two can be matched exactly rather than by adapter name -- and this
    // machine has two adapters reporting the same name.
    if (!r.luidKnown && r.api.getUnsignedBytev != nullptr)
    {
        GLubyte bytes[8] {};
        r.api.Drain();
        r.api.getUnsignedBytev(GL_DEVICE_LUID_EXT_, bytes);
        if (r.api.Clean())
        {
            std::memcpy(&r.luid, bytes, sizeof(r.luid));
            r.luidKnown = true;
        }
    }

    if (g.bridge.workDevice == nullptr)
    {
        if (!r.luidKnown)
        {
            r.Stop("the GL context reports no device LUID, so there is no way to prove the work "
                   "device would land on the same GPU.");
            return false;
        }
        ComPtr<IDXGIAdapter> adapter;
        if (!AdapterForLuid(r.luid, adapter))
        {
            r.Stop("no DXGI adapter carries this GL context's LUID. OpenGL and D3D12 would be "
                   "different GPUs, and nothing can be shared between them.");
            return false;
        }
        if (!CreateWorkDevice(adapter.Get(), r.luid))
        {
            r.Stop("the work D3D12 device did not come up.");
            return false;
        }
        g.device = g.bridge.workDevice;
        g.queue = g.bridge.workQueue;
        // The completion fence this route waits on before blitting the result back. The D3D11
        // route creates it inside its own frame function, so a route that does not go through
        // there has to create it here -- and Signal on a null fence is not a wait, it is nothing.
        if (FAILED(g.bridge.workDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
        {
            r.Stop("could not create the completion fence.");
            return false;
        }
    }

    if (!r.semaphores && !r.loggedSemaphores)
    {
        SelfIssued selfIssued;
        r.semaphores = g.settings.glSemaphores.load() && ImportFences(r);
        r.loggedSemaphores = true;
        Log("opengl: imported fences %s. %s", r.semaphores ? "in use"
                                                 : g.settings.glSemaphores.load() ? "unavailable"
                                                                         : "off by GlSemaphores=0",
            r.semaphores ? "The hand-over between the two APIs runs on the GPU; the first frame "
                           "is still confirmed on the CPU before the stall is dropped."
                         : "Falling back to the CPU stall -- glFinish and a fence wait -- which "
                           "is what the D3D11 route has always used.");
    }

    const bool built = r.in.texture != 0 && r.in.on12 != nullptr && r.out.texture != 0 &&
                       r.out.on12 != nullptr && g.bridge.crossLocal != nullptr;
    if (r.ready && r.width == w && r.height == h && built)
        return true;

    if (r.ready && r.width == w && r.height == h)
        Log("opengl: the route was ready but a resource is gone; rebuilding it.");

    r.ready = false;
    {
        SelfIssued selfIssued;
        ReadDefaultSamples(r);
    }
    if (!BuildCrossing(r, r.in, w, h, "colour-in") || !BuildCrossing(r, r.out, w, h, "result-out"))
    {
        r.Stop("the shared textures would not cross.");
        return false;
    }
    if (r.defaultSamples > 1)
    {
        SelfIssued selfIssued;
        if (!BuildResolveTarget(r, w, h))
        {
            r.Stop("the multisampled default framebuffer has nowhere to resolve to.");
            return false;
        }
    }

    // Our private copy on our own device, so the network never reads shared memory directly --
    // the same reason the D3D11 route keeps crossLocal.
    g.bridge.crossLocal.Reset();
    if (!CreateTexture(w, h, kCrossingFormat, g.bridge.crossLocal, "crossLocal",
                       D3D12_RESOURCE_STATE_COPY_DEST))
    {
        r.Stop("could not create the private copy of the frame.");
        return false;
    }

    r.width = w;
    r.height = h;
    r.ready = true;
    if (!r.loggedReady)
    {
        r.loggedReady = true;
        Log("opengl: bridge up at %ux%u, default framebuffer %d sample(s)%s. The frame crosses "
            "by blit between the default framebuffer and a D3D12 texture imported into this "
            "context.", w, h, r.defaultSamples,
            r.defaultSamples > 1 ? ", resolved on the way in" : "");
    }
    return true;
}

// ---- the two blits ---------------------------------------------------------------------------
//
// Y is inverted in both directions, and the two inversions are not a mistake that cancels: OpenGL
// puts the origin at the bottom left and the network's frame is top-left like every D3D surface
// the rest of this add-on handles, so the image is turned over on the way in and turned back on
// the way out. Getting one of them wrong produces an upside-down game with no error anywhere.

bool BlitIn(Route &r)
{
    SelfIssued selfIssued;
    Api &api = r.api;
    SavedState saved {};
    Save(api, saved);

    const GLint w = static_cast<GLint>(r.width), h = static_cast<GLint>(r.height);

    api.disable(GL_SCISSOR_TEST_);
    api.disable(GL_FRAMEBUFFER_SRGB_);
    api.bindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    api.getIntegerv(GL_READ_BUFFER_, &saved.readBuffer);
    saved.haveReadBuffer = true;
    api.readBuffer(GL_BACK_);
    api.Drain();

    bool ok = true;
    if (r.defaultSamples > 1)
    {
        // Two steps, because one blit cannot be asked to resolve samples and mirror Y at once:
        // with a multisampled read framebuffer the spec requires source and destination
        // rectangles of the same size, and a driver is within its rights to read a mirrored
        // rectangle as a different one. Resolve straight, then flip.
        api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, r.resolveFbo);
        api.blitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
        ok = api.Clean();
        api.bindFramebuffer(GL_READ_FRAMEBUFFER_, r.resolveFbo);
        api.readBuffer(GL_COLOR_ATTACHMENT0_);
        api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, r.in.fbo);
        api.blitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
        ok = api.Clean() && ok;
    }
    else
    {
        api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, r.in.fbo);
        api.blitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
        ok = api.Clean();
    }

    // The hand-over. With the imported fences it is a signal the D3D12 queue waits on, and the
    // flush is not optional: a signal sitting in the GL command queue is a wait that never ends
    // on the other side. Without them it is glFinish, the CPU stall the D3D11 route uses.
    if (r.semaphores)
    {
        const GLuint64 value = ++r.toD3DValue;
        const GLenum layout = GL_LAYOUT_TRANSFER_SRC_EXT_;  // D3D12 reads it as a copy source
        api.semaphoreParameterui64v(r.semToD3D, GL_D3D12_FENCE_VALUE_EXT_, &value);
        api.signalSemaphore(r.semToD3D, 0, nullptr, 1, &r.in.texture, &layout);
        api.flush();
        ok = api.Clean() && ok;
    }
    else
    {
        api.finish();
    }
    Restore(api, saved);
    return ok;
}

bool BlitOut(Route &r)
{
    SelfIssued selfIssued;
    Api &api = r.api;
    SavedState saved {};
    Save(api, saved);

    const GLint w = static_cast<GLint>(r.width), h = static_cast<GLint>(r.height);

    api.disable(GL_SCISSOR_TEST_);
    api.disable(GL_FRAMEBUFFER_SRGB_);
    api.bindFramebuffer(GL_READ_FRAMEBUFFER_, r.out.fbo);
    api.readBuffer(GL_COLOR_ATTACHMENT0_);
    api.Drain();

    // The other half of the hand-over: the network's result is in memory D3D12 has just written,
    // and GL has to be told to wait for that before it reads it. Without the fences this is
    // covered by the CPU wait the caller already did.
    if (r.semaphores)
    {
        const GLuint64 value = r.toGlValue;
        const GLenum layout = GL_LAYOUT_TRANSFER_SRC_EXT_;  // GL reads it as a blit source
        api.semaphoreParameterui64v(r.semToGl, GL_D3D12_FENCE_VALUE_EXT_, &value);
        api.waitSemaphore(r.semToGl, 0, nullptr, 1, &r.out.texture, &layout);
    }

    api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    api.getIntegerv(GL_DRAW_BUFFER_, &saved.drawBuffer);
    saved.haveDrawBuffer = true;
    api.drawBuffer(GL_BACK_);

    // Into the default framebuffer, multisampled or not: a single-sampled source written to a
    // multisampled destination replicates to every sample, which is what a frame that has already
    // been resolved should do.
    api.blitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
    const bool ok = api.Clean();

    Restore(api, saved);
    return ok;
}

// ---- one frame --------------------------------------------------------------------------------

void Present(command_queue *queue, swapchain *sc)
{
    Route &r = g_route;
    if (r.failed)
        return;

    static bool filterInstalled = false;
    if (!filterInstalled)
    {
        filterInstalled = true;
        g_previousFilter = SetUnhandledExceptionFilter(ReportFault);
    }

    device *dev = sc->get_device();
    if (!r.api.LoadCore())
    {
        r.Stop("opengl32.dll has no wgl entry points, which should be impossible inside an "
               "OpenGL host.");
        return;
    }

    // ReShade reports the back buffer as GL_FRAMEBUFFER_DEFAULT with resource_type::surface, so
    // the handle is not something to copy -- it is read for its dimensions and nothing else.
    const resource back = sc->get_current_back_buffer();
    if (back.handle == 0)
        return;
    const resource_desc bd = dev->get_resource_desc(back);
    if (bd.texture.width == 0 || bd.texture.height == 0)
        return;
    // The dimensions come from here; the sample count does not. Ensure reads GL_SAMPLES from GL
    // itself, because that is the number the blit rules are written against and it is one call
    // away -- and because a route that depends on a second party's description of the default
    // framebuffer has one more thing that can quietly change under it. What ReShade thinks is
    // logged once beside it, so the two can be compared in the field.
    if (!r.loggedBackBuffer)
    {
        r.loggedBackBuffer = true;
        Log("opengl: ReShade reports the back buffer as %ux%u, %u sample(s), format %u.",
            bd.texture.width, bd.texture.height, bd.texture.samples,
            static_cast<unsigned>(bd.texture.format));
    }

    if (!Ensure(r, bd.texture.width, bd.texture.height))
        return;

    // Stage 2 means what it has always meant here: bring the bridge up and stop before the
    // engine. On this route that is worth more than a diagnostic. It blits the frame across to
    // D3D12, copies it inside our device and blits it back, so the whole transport is exercised
    // against a real game with the network taken out of the question -- and every way the
    // transport can be wrong is visible on screen rather than in a log. An upside-down game is a
    // flip that was not undone; a blue sky gone orange is the channel order; a black screen is
    // memory that never aliased.
    const bool transportOnly = g.stage.load() < 3;
    if (!transportOnly)
    {
        if (!EnsureResources(r.width, r.height, kCrossingFormat, g.settings.scale.load()))
        {
            g.status.unavailable = true;
            if (*g.status.reason == 0)
                g.status.reason = "could not create the working textures; see amd-nr.log";
            Log("opengl: %s", g.status.reason);
            return;
        }

        UINT wanted = WantedPasses();
        if (!BringUpEngines(wanted))
        {
            g.status.unavailable = true;
            g.status.reason = "could not bring the engine up on the OpenGL bridge";
            Log("opengl: %s", g.status.reason);
            return;
        }
        g.loadedPasses = wanted;
    }
    else if (!r.loggedTransportOnly)
    {
        r.loggedTransportOnly = true;
        Log("opengl: Stage=%d, so this is the transport on its own: the frame crosses to D3D12 "
            "and comes back untouched. If the picture looks exactly as it did without the "
            "add-on, the crossing is correct.", g.stage.load());
    }

    // The same guard the other routes carry: the engine keeps its own timeline in async mode, and
    // starting a second evaluation on top of one still running is how this hangs rather than
    // skips.
    // The parentheses are the point. Without them && binds tighter than || and the right-hand
    // side -- which reads the engine's job counter through g.runtime -- is evaluated even when
    // the engine was never loaded, which in transport-only mode is a null dereference on the
    // first frame. That is precisely how this route's second run ended.
    // How many frames in a row may repeat the last result before the route waits for a new one.
    // Zero by default, and the default is the point: a repeated frame is one the player cannot
    // tell from a new one, so a route that repeats frames makes every frame counter -- the
    // game's, the overlay's, the driver's -- report a rate nobody is seeing. Waiting instead
    // paces the game to the network, which is what the D3D11 and Vulkan routes have always done
    // and what makes the number on screen mean something. GlHoldFrames in the ini raises it for
    // anyone who wants the present rate instead.
    const unsigned kMaxHeld = static_cast<unsigned>(g.settings.glHoldFrames.load());

    auto jobPending = [&] {
        return !transportOnly &&
               (g.fence->GetCompletedValue() < g.completion ||
                RuntimeBusy());
    };

    bool runNetwork = !transportOnly;
    bool holdThisFrame = false;
    if (jobPending() && GetTickCount64() - g.lastJobAt < 500)
    {
        // Starting a second evaluation on top of one still running is how this hangs rather than
        // skips, which is why the other routes carry the same guard. What is different here is
        // what happens to the frame: it is not handed back to the game, it is given the last
        // result again -- up to kMaxHeld times. After that the route waits for the outstanding
        // job instead, so the picture is never older than a few frames and never falls back to
        // an uncorrected one mid-sequence.
        if (kMaxHeld != 0 && r.haveResult && r.held < kMaxHeld)
        {
            holdThisFrame = true;
            runNetwork = false;
        }
        else
        {
            if (g.fence->GetCompletedValue() < g.completion)
            {
                g.fence->SetEventOnCompletion(g.completion, g.completionEvent);
                WaitForSingleObject(g.completionEvent, 500);
            }
            runNetwork = !jobPending();
            // Only with holding enabled. With it off, a frame the network could not answer shows
            // the game's own image rather than a duplicate -- that is the honest thing, and after
            // the wait above it is also rare.
            holdThisFrame = !runNetwork && r.haveResult && kMaxHeld != 0;
        }
    }

    // A held frame does nothing but put the last result back on the screen. Crossing the game's
    // new frame over to a device that is not going to look at it, copying it there and submitting
    // a command list for it is work whose only effect is to slow the game down -- and the first
    // build of this hold did exactly that, which cost more than half the frame rate.
    if (holdThisFrame)
    {
        ++r.held;
        ++r.heldFrames;
        ++g.status.skipped;
        if (!g.noBackBuffer.load() && !BlitOut(r))
        {
            r.Stop("the blit back into the default framebuffer reported a GL error.");
            return;
        }
        if (!r.loggedHold)
        {
            r.loggedHold = true;
            Log("opengl: GlHoldFrames=%u, so a frame the network has not answered repeats the "
                "last result instead of waiting. The present rate rises and part of it is "
                "duplicates -- every frame counter will read high by that much.", kMaxHeld);
        }
        if (++g.status.frame % 600 == 0)
            Log("frame %llu processed (%llu fresh, %llu held)",
                static_cast<unsigned long long>(g.status.frame),
                static_cast<unsigned long long>(r.freshFrames),
                static_cast<unsigned long long>(r.heldFrames));
        return;
    }

    // --- 1. the host's frame comes over -------------------------------------------------------
    if (!BlitIn(r))
    {
        r.Stop("the blit out of the default framebuffer reported a GL error.");
        return;
    }

    // --- 2. our device does the work ----------------------------------------------------------
    const UINT slot = static_cast<UINT>(g.serial % State::kRing);
    if (g.ringValue[slot] != 0 && g.ringFence->GetCompletedValue() < g.ringValue[slot])
    {
        g.ringFence->SetEventOnCompletion(g.ringValue[slot], g.ringEvent);
        WaitForSingleObject(g.ringEvent, 2000);
    }
    const HRESULT allocatorHr = g.alloc[slot]->Reset();
    const HRESULT listHr = SUCCEEDED(allocatorHr)
                               ? g.list[slot]->Reset(g.alloc[slot].Get(), nullptr)
                               : allocatorHr;
    if (FAILED(allocatorHr) || FAILED(listHr))
    {
        Log("opengl: work slot %u reset failed (allocator 0x%08lX, list 0x%08lX); recreating the "
            "pair.", slot, allocatorHr, listHr);
        if (!RecreateWorkSlot(slot))
            r.Stop("the D3D12 work slot could not be recovered.");
        return;
    }
    auto *cmd12 = g.list[slot].Get();
    bool fresh = false;
    if (transportOnly)
    {
        // In and straight back out, through our own device, so the round trip is real: the frame
        // is read from the imported texture, written to a resource this device owns, and written
        // back to the other imported texture.
        cmd12->CopyResource(g.bridge.crossLocal.Get(), r.in.on12.Get());
        Barrier(cmd12, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd12->CopyResource(r.out.on12.Get(), g.bridge.crossLocal.Get());
        Barrier(cmd12, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        fresh = true;
    }
    else
    {
        cmd12->CopyResource(g.bridge.crossLocal.Get(), r.in.on12.Get());
        Barrier(cmd12, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const bool ok =
            RecordNetwork(cmd12, g.bridge.crossLocal.Get(), kCrossingFormat, nullptr, runNetwork,
                          g.loadedPasses,
                          [&]() { return SubmitPrivatePass(cmd12, g.alloc[slot].Get()); });
        if (!ok && g.status.failed)
            return;
        Barrier(cmd12, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

        // Freshness gate, same as the other routes: on a frame where the network was skipped the
        // residual belongs to an older picture, and the host is better served by its own image
        // than by a correction aimed at where things used to be.
        fresh = ok && CompositionIsFresh(runNetwork);
        if (fresh)
        {
            Barrier(cmd12, g.composed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd12->CopyResource(r.out.on12.Get(), g.composed.Get());
            Barrier(cmd12, g.composed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    if (const HRESULT hr = cmd12->Close(); FAILED(hr))
    {
        Log("opengl: closing work slot %u failed (0x%08lX); discarding the unsubmitted job and "
            "recreating the pair.", slot, hr);
        ResetJobs();
        g.activePasses = 0;
        g.historyValid.store(false);
        if (!RecreateWorkSlot(slot))
            r.Stop("the D3D12 work slot could not be recovered after Close failed.");
        return;
    }
    ID3D12CommandList *lists[] { cmd12 };
    // With the fences in play the queue waits for the GL signal that the frame has landed in the
    // shared texture. Without them BlitIn already called glFinish and there is nothing to wait
    // for.
    if (r.semaphores)
        g.bridge.workQueue->Wait(g.bridge.backFence.Get(), r.toD3DValue);
    g.bridge.workQueue->ExecuteCommandLists(1, lists);
    if (!transportOnly && g.activePasses != 0)
        NotifyRuntimes(g.bridge.workQueue.Get(), 1, lists);
    g.ringValue[slot] = ++g.ringSerial;
    g.bridge.workQueue->Signal(g.ringFence.Get(), g.ringSerial);
    g.completion = ++g.serial;
    g.bridge.workQueue->Signal(g.fence.Get(), g.completion);
    if (r.semaphores)
    {
        // The value GL waits for in BlitOut. A second signal rather than reusing g.fence,
        // because g.fence is not shared and only the exported one can cross.
        r.toGlValue = r.toGlValue + 1;
        g.bridge.workQueue->Signal(g.bridge.crossFence.Get(), r.toGlValue);
    }

    // The first frame is confirmed on the CPU even with the fences in use, and only then is the
    // stall dropped. If the round trip does not complete, the GPU-side hand-over is not working
    // and continuing with it means a hung queue and a game that has to be killed -- so the route
    // steps back to stalling, which is known to work, and says so once.
    if (!r.semaphores)
    {
        WaitForWorkQueue(g.completion);
    }
    else if (!r.semaphoresProven)
    {
        g.fence->SetEventOnCompletion(g.completion, g.completionEvent);
        if (WaitForSingleObject(g.completionEvent, 2000) == WAIT_OBJECT_0)
        {
            r.semaphoresProven = true;
            Log("opengl: the imported fences carried the first frame; the CPU stall is dropped "
                "from here on.");
        }
        else
        {
            r.semaphores = false;
            r.toD3DValue = 0;
            r.toGlValue = 0;
            Log("opengl: the first frame did not complete within 2 s with the imported fences in "
                "the loop, so they are not driving this queue. Falling back to the CPU stall for "
                "the rest of the session.");
            WaitForWorkQueue(g.completion);
        }
    }

    // --- 3. and the finished image goes back over the host's ----------------------------------
    //
    // `fresh` means the network produced a new composition this frame. When it did not, the
    // result texture still holds the last one and showing it again is what keeps the effect from
    // strobing; the count is bounded above, where the decision is made. The same reasoning is
    // already written down for Network Output in CompositionIsFresh: a held frame and a
    // correction aimed at where the edges used to be are different artefacts, and flickering
    // between two different pictures is worse than either.
    if (fresh)
    {
        r.haveResult = true;
        r.held = 0;
        ++r.freshFrames;
    }
    // Before the first result there is nothing to show but the game's own frame, and after a
    // network failure `fresh` is false with nothing new behind it -- in both cases the honest
    // thing is to leave the host's image alone.
    const bool show = (fresh || r.haveResult) && !g.noBackBuffer.load();
    if (show && !BlitOut(r))
    {
        r.Stop("the blit back into the default framebuffer reported a GL error.");
        return;
    }

    // The readbacks are the network's own measurements -- the flow probe and the residual. In
    // transport-only mode there is no network and netWidth/netHeight are zero, so there is
    // nothing to drain and no reason to go looking.
    if (!transportOnly)
        DrainReadbacks(g.netWidth, g.netHeight);

    if (!r.loggedFirstFrame)
    {
        r.loggedFirstFrame = true;
        Log("opengl: first full round trip done at %ux%u.", r.width, r.height);
    }
    if (++g.status.frame <= 3 || g.status.frame % 120 == 0)
        Log("frame %llu processed (%llu fresh, %llu held)", static_cast<unsigned long long>(g.status.frame),
            static_cast<unsigned long long>(r.freshFrames),
            static_cast<unsigned long long>(r.heldFrames));
}

}  // namespace glroute
