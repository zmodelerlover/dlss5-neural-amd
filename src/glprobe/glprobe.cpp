// glprobe -- can OpenGL import D3D12 memory and fences on this driver, in the kind of context a
// game actually hands over?
//
// vkprobe asked this of Vulkan and could answer it with a query: a Vulkan driver reports what it
// will accept before anything is created. OpenGL has no such query. The extension string says
// which entry points exist, never whether an import will be refused, and there is no
// glGetExternalMemoryProperties to ask. So this probe queries where a query exists and attempts
// everywhere else -- it builds the crossing for real, in every format the route would carry, and
// prints what the driver did with it.
//
// The route under test is the Vulkan route with the API swapped. Direction and ownership are
// identical, because they are forced by the same fact: the network runs on OUR private D3D12
// device (see CreateWorkDevice in neural.cpp), so the shared memory is created there and the
// host's API imports it, never the other way round.
//
//   D3D12 CreateCommittedResource(D3D12_HEAP_FLAG_SHARED) -> CreateSharedHandle
//     -> glCreateMemoryObjectsEXT, GL_DEDICATED_MEMORY_OBJECT_EXT = 1
//     -> glImportMemoryWin32HandleEXT(GL_HANDLE_TYPE_D3D12_RESOURCE_EXT)
//     -> glTexStorageMem2DEXT
//   D3D12 CreateFence(D3D12_FENCE_FLAG_SHARED) -> CreateSharedHandle
//     -> glImportSemaphoreWin32HandleEXT(GL_HANDLE_TYPE_D3D12_FENCE_EXT)
//     -> glSemaphoreParameterui64vEXT(GL_D3D12_FENCE_VALUE_EXT) + glWaitSemaphoreEXT
//
// Four things this checks that a bare "did the import return an error" would miss:
//
//   * GL_DEDICATED_MEMORY_OBJECT_EXT. A D3D12 resource handle backs exactly one resource, so the
//     memory object has to be declared dedicated BEFORE the import. This is the same rule the
//     Vulkan route met as VkMemoryDedicatedAllocateInfo (vk_route.inc), and missing it is a
//     refusal with nothing in the message about dedication.
//   * The imported texture is attached to an FBO and checked for completeness. The GL route
//     cannot copy the way the Vulkan one does: in OpenGL the back buffer is the default
//     framebuffer, not a texture, so the crossing is glBlitFramebuffer from FBO 0 into an FBO
//     wrapping the imported texture. An import that is complete as a texture and incomplete as a
//     render target would look fine here and fail in a game.
//   * That blit is then actually performed, out of a cleared default framebuffer, and the pixels
//     are read back. That is the route's own copy path, end to end, in eight lines.
//   * The bytes are round-tripped both ways on R8G8B8A8_UNORM. An import that returns no error
//     and does not alias the same allocation is the failure mode with no symptom at all.
//
// Two things were found by writing it, and both shape the file:
//
//   * An imported texture's NT handle is NOT the application's to close on this driver. Vulkan's
//     rule, which EXT_memory_object_win32 is written to match, is that the import takes its own
//     reference and the application closes its handle afterwards -- that is what vk_route.inc
//     does. Closing it here faults inside the ICD from a driver thread, at an unpredictable
//     point later, so the crash lands in whatever unrelated GL call comes next. An early version
//     of this probe closed its handles and died in a different place on every other run; keeping
//     them open, it has not died since. So the handles are kept, deliberately, and the last
//     section tests the rule rather than trusting it.
//   * Therefore every GL call that touches imported memory runs inside __try. A probe that dies
//     reports nothing, including everything it had already proved. A fault becomes a row saying
//     FAULTED, and the run carries on until a fault makes carrying on meaningless.
//
// The control row exists for the same reason: the same operations, through the same code, on an
// ordinary texture. Without it, "the driver faulted" and "this probe is wrong" look identical.
//
// What this cannot answer, and does not pretend to: what ReShade hands an add-on for the back
// buffer under a real OpenGL host, and whether any depth or motion guide reaches it. That needs
// a game and the existing `probe` add-on. Nor does it prove the route can leave the context's
// state as it found it, which in OpenGL is a global and is the other half of the work.
//
// The context matters as much as the driver. Games do not all get GL 4.6: a host on a 3.3 core
// context has no DSA, so the route must use glTexStorageMem2DEXT (bound) and not
// glTextureStorageMem2DEXT (DSA). Both are resolved and reported. By default this runs on the
// context wglCreateContext gives, which is what a legacy host gets; pass -gl 3.3 -core (or any
// other pair) to ask the question again as a stricter host would.
//
// Nothing here is linked statically: opengl32.dll and gdi32.dll are resolved by hand, the way
// the add-on resolves d3d12.dll. That is not tidiness. A static opengl32 import in the add-on
// would drag OpenGL into every D3D11 game it loads into, and neural.cpp already documents what
// one unwanted import did to NFS 2015's ResizeBuffers.
//
// Build:  .\build.ps1 -Target glprobe -Exe
// Run:    .\build\amd-nr-glprobe.exe                 the context wglCreateContext gives
//         .\build\amd-nr-glprobe.exe -gl 3.3 -core   a stricter host's context

#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{

// ---- the slice of OpenGL this needs, declared against the spec -------------------------------
//
// <GL/gl.h> from the Windows SDK stops at 1.1 and would still not declare one of the EXT entry
// points below, so there is nothing to gain by including it and a name clash to lose.

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLubyte = unsigned char;
using GLuint64 = unsigned long long;
using GLbitfield = unsigned int;
using GLfloat = float;

constexpr GLenum GL_NO_ERROR_ = 0;
constexpr GLenum GL_TEXTURE_2D_ = 0x0DE1;
constexpr GLenum GL_UNSIGNED_BYTE_ = 0x1401;
constexpr GLenum GL_FLOAT_ = 0x1406;
constexpr GLenum GL_HALF_FLOAT_ = 0x140B;
constexpr GLenum GL_RED_ = 0x1903;
constexpr GLenum GL_RGBA_ = 0x1908;
constexpr GLenum GL_RG_ = 0x8227;
constexpr GLenum GL_RGBA8_ = 0x8058;
constexpr GLenum GL_SRGB8_ALPHA8_ = 0x8C43;
constexpr GLenum GL_RGBA16F_ = 0x881A;
constexpr GLenum GL_RG16F_ = 0x822F;
constexpr GLenum GL_R32F_ = 0x822E;
constexpr GLenum GL_VENDOR_ = 0x1F00;
constexpr GLenum GL_RENDERER_ = 0x1F01;
constexpr GLenum GL_VERSION_ = 0x1F02;
constexpr GLenum GL_EXTENSIONS_ = 0x1F03;
constexpr GLenum GL_SHADING_LANGUAGE_VERSION_ = 0x8B8C;
constexpr GLenum GL_NUM_EXTENSIONS_ = 0x821D;
constexpr GLenum GL_MAJOR_VERSION_ = 0x821B;
constexpr GLenum GL_MINOR_VERSION_ = 0x821C;
constexpr GLenum GL_CONTEXT_PROFILE_MASK_ = 0x9126;
constexpr GLint GL_CONTEXT_CORE_PROFILE_BIT_ = 0x1;
constexpr GLint GL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ = 0x2;
constexpr GLenum GL_PACK_ALIGNMENT_ = 0x0D05;
constexpr GLenum GL_UNPACK_ALIGNMENT_ = 0x0CF5;
constexpr GLenum GL_COLOR_BUFFER_BIT_ = 0x4000;
constexpr GLenum GL_NEAREST_ = 0x2600;
constexpr GLenum GL_BACK_ = 0x0405;

// EXT_memory_object / EXT_memory_object_win32 / EXT_semaphore / EXT_semaphore_win32
constexpr GLenum GL_TEXTURE_TILING_EXT_ = 0x9580;
constexpr GLenum GL_DEDICATED_MEMORY_OBJECT_EXT_ = 0x9581;
constexpr GLenum GL_OPTIMAL_TILING_EXT_ = 0x9584;
constexpr GLenum GL_HANDLE_TYPE_OPAQUE_WIN32_EXT_ = 0x9587;
constexpr GLenum GL_HANDLE_TYPE_D3D12_RESOURCE_EXT_ = 0x958A;
constexpr GLenum GL_HANDLE_TYPE_D3D11_IMAGE_EXT_ = 0x958B;
constexpr GLenum GL_LAYOUT_GENERAL_EXT_ = 0x958D;
constexpr GLenum GL_LAYOUT_COLOR_ATTACHMENT_EXT_ = 0x958E;
constexpr GLenum GL_LAYOUT_TRANSFER_SRC_EXT_ = 0x9592;
constexpr GLenum GL_LAYOUT_TRANSFER_DST_EXT_ = 0x9593;
constexpr GLenum GL_HANDLE_TYPE_D3D12_FENCE_EXT_ = 0x9594;
constexpr GLenum GL_D3D12_FENCE_VALUE_EXT_ = 0x9595;
constexpr GLenum GL_DEVICE_UUID_EXT_ = 0x9597;
constexpr GLenum GL_DRIVER_UUID_EXT_ = 0x9598;
constexpr GLenum GL_DEVICE_LUID_EXT_ = 0x9599;
constexpr GLenum GL_DEVICE_NODE_MASK_EXT_ = 0x959A;

// framebuffer objects, core since 3.0 -- the crossing's copy path needs them
constexpr GLenum GL_READ_FRAMEBUFFER_ = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER_ = 0x8CA9;
constexpr GLenum GL_FRAMEBUFFER_ = 0x8D40;
constexpr GLenum GL_COLOR_ATTACHMENT0_ = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5;

// WGL_ARB_create_context
constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB_ = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB_ = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB_ = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB_ = 0x0001;
constexpr int WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB_ = 0x0002;

using PFN_glGetString = const GLubyte *(APIENTRY *)(GLenum);
using PFN_glGetStringi = const GLubyte *(APIENTRY *)(GLenum, GLuint);
using PFN_glGetError = GLenum(APIENTRY *)();
using PFN_glGetIntegerv = void(APIENTRY *)(GLenum, GLint *);
using PFN_glGenTextures = void(APIENTRY *)(GLsizei, GLuint *);
using PFN_glDeleteTextures = void(APIENTRY *)(GLsizei, const GLuint *);
using PFN_glBindTexture = void(APIENTRY *)(GLenum, GLuint);
using PFN_glTexParameteri = void(APIENTRY *)(GLenum, GLenum, GLint);
using PFN_glReadPixels = void(APIENTRY *)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
using PFN_glReadBuffer = void(APIENTRY *)(GLenum);
using PFN_glPixelStorei = void(APIENTRY *)(GLenum, GLint);
using PFN_glFinish = void(APIENTRY *)();
using PFN_glFlush = void(APIENTRY *)();
using PFN_glClearColor = void(APIENTRY *)(GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glClear = void(APIENTRY *)(GLbitfield);
using PFN_glViewport = void(APIENTRY *)(GLint, GLint, GLsizei, GLsizei);

using PFN_glGetUnsignedBytevEXT = void(APIENTRY *)(GLenum, GLubyte *);
using PFN_glCreateMemoryObjectsEXT = void(APIENTRY *)(GLsizei, GLuint *);
using PFN_glDeleteMemoryObjectsEXT = void(APIENTRY *)(GLsizei, const GLuint *);
using PFN_glMemoryObjectParameterivEXT = void(APIENTRY *)(GLuint, GLenum, const GLint *);
using PFN_glImportMemoryWin32HandleEXT = void(APIENTRY *)(GLuint, GLuint64, GLenum, void *);
using PFN_glTexStorage2D = void(APIENTRY *)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
using PFN_glTexStorageMem2DEXT = void(APIENTRY *)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint,
                                                  GLuint64);
using PFN_glTextureStorageMem2DEXT = void(APIENTRY *)(GLuint, GLsizei, GLenum, GLsizei, GLsizei,
                                                      GLuint, GLuint64);
using PFN_glGenSemaphoresEXT = void(APIENTRY *)(GLsizei, GLuint *);
using PFN_glDeleteSemaphoresEXT = void(APIENTRY *)(GLsizei, const GLuint *);
using PFN_glImportSemaphoreWin32HandleEXT = void(APIENTRY *)(GLuint, GLenum, void *);
using PFN_glSemaphoreParameterui64vEXT = void(APIENTRY *)(GLuint, GLenum, const GLuint64 *);
using PFN_glWaitSemaphoreEXT = void(APIENTRY *)(GLuint, GLuint, const GLuint *, GLuint,
                                                const GLuint *, const GLenum *);
using PFN_glSignalSemaphoreEXT = void(APIENTRY *)(GLuint, GLuint, const GLuint *, GLuint,
                                                  const GLuint *, const GLenum *);

using PFN_glGenFramebuffers = void(APIENTRY *)(GLsizei, GLuint *);
using PFN_glDeleteFramebuffers = void(APIENTRY *)(GLsizei, const GLuint *);
using PFN_glBindFramebuffer = void(APIENTRY *)(GLenum, GLuint);
using PFN_glFramebufferTexture2D = void(APIENTRY *)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_glCheckFramebufferStatus = GLenum(APIENTRY *)(GLenum);
using PFN_glBlitFramebuffer = void(APIENTRY *)(GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                                               GLint, GLbitfield, GLenum);

using PFN_wglCreateContext = HGLRC(WINAPI *)(HDC);
using PFN_wglMakeCurrent = BOOL(WINAPI *)(HDC, HGLRC);
using PFN_wglDeleteContext = BOOL(WINAPI *)(HGLRC);
using PFN_wglGetProcAddress = PROC(WINAPI *)(LPCSTR);
using PFN_wglGetExtensionsStringARB = const char *(WINAPI *)(HDC);
using PFN_wglCreateContextAttribsARB = HGLRC(WINAPI *)(HDC, HGLRC, const int *);
using PFN_ChoosePixelFormat = int(WINAPI *)(HDC, const PIXELFORMATDESCRIPTOR *);
using PFN_SetPixelFormat = BOOL(WINAPI *)(HDC, int, const PIXELFORMATDESCRIPTOR *);

int g_failures = 0;


// A fault this probe did not expect -- on a driver thread, or in a call not wrapped in __try --
// otherwise ends the process with nothing printed and nothing to go on. Naming the module and
// the offset turns it into a usable report: ours is a bug in this file, atio6axx.dll is the ICD,
// and anything else is worth knowing about on its own.
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
    std::printf("\nFAULT: exception %08lX at %p", info->ExceptionRecord->ExceptionCode, at);
    if (module != nullptr)
        std::printf(" = %ls+0x%llX", name,
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(at) -
                                                    reinterpret_cast<uintptr_t>(module)));
    std::printf("\n       on thread %lu.\n", GetCurrentThreadId());
    return EXCEPTION_EXECUTE_HANDLER;
}

void Fail(const char *what)
{
    ++g_failures;
    std::printf("  FAIL: %s\n", what);
}

void Line()
{
    std::printf("--------------------------------------------------------------------------\n");
}

// Everything is resolved by hand, in two groups: what opengl32.dll exports itself (GL 1.1 and the
// wgl entry points) and what only the ICD has, which is reached through wglGetProcAddress and
// only once a context is current. Asking for an extension function before that returns null on
// every driver, and that reads exactly like "the driver does not support it".
struct Gl
{
    HMODULE opengl = nullptr, gdi = nullptr;

    PFN_glGetString getString = nullptr;
    PFN_glGetStringi getStringi = nullptr;
    PFN_glGetError getError = nullptr;
    PFN_glGetIntegerv getIntegerv = nullptr;
    PFN_glGenTextures genTextures = nullptr;
    PFN_glDeleteTextures deleteTextures = nullptr;
    PFN_glBindTexture bindTexture = nullptr;
    PFN_glTexParameteri texParameteri = nullptr;
    PFN_glReadPixels readPixels = nullptr;
    PFN_glReadBuffer readBuffer = nullptr;
    PFN_glPixelStorei pixelStorei = nullptr;
    PFN_glFinish finish = nullptr;
    PFN_glFlush flush = nullptr;
    PFN_glClearColor clearColor = nullptr;
    PFN_glClear clear = nullptr;
    PFN_glViewport viewport = nullptr;

    PFN_wglCreateContext createContext = nullptr;
    PFN_wglMakeCurrent makeCurrent = nullptr;
    PFN_wglDeleteContext deleteContext = nullptr;
    PFN_wglGetProcAddress getProcAddress = nullptr;
    PFN_wglCreateContextAttribsARB createContextAttribs = nullptr;
    PFN_wglGetExtensionsStringARB wglExtensions = nullptr;

    PFN_ChoosePixelFormat choosePixelFormat = nullptr;
    PFN_SetPixelFormat setPixelFormat = nullptr;

    PFN_glGetUnsignedBytevEXT getUnsignedBytev = nullptr;
    PFN_glCreateMemoryObjectsEXT createMemoryObjects = nullptr;
    PFN_glDeleteMemoryObjectsEXT deleteMemoryObjects = nullptr;
    PFN_glMemoryObjectParameterivEXT memoryObjectParameteriv = nullptr;
    PFN_glImportMemoryWin32HandleEXT importMemoryWin32 = nullptr;
    PFN_glTexStorage2D texStorage2D = nullptr;
    PFN_glTexStorageMem2DEXT texStorageMem2D = nullptr;
    PFN_glTextureStorageMem2DEXT textureStorageMem2D = nullptr;
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

    template <typename T>
    void FromModule(HMODULE m, const char *name, T &out)
    {
        out = reinterpret_cast<T>(GetProcAddress(m, name));
    }
    // WGL says an extension function comes from wglGetProcAddress and a GL 1.1 one from the
    // module; for everything in between, drivers differ about which of the two answers. Try both
    // before believing a null.
    template <typename T>
    void FromContext(const char *name, T &out)
    {
        out = reinterpret_cast<T>(getProcAddress(name));
        if (out == nullptr)
            FromModule(opengl, name, out);
    }

    bool LoadModules()
    {
        opengl = LoadLibraryW(L"opengl32.dll");
        gdi = LoadLibraryW(L"gdi32.dll");
        if (opengl == nullptr || gdi == nullptr)
            return false;
        FromModule(opengl, "glGetString", getString);
        FromModule(opengl, "glGetError", getError);
        FromModule(opengl, "glGetIntegerv", getIntegerv);
        FromModule(opengl, "glGenTextures", genTextures);
        FromModule(opengl, "glDeleteTextures", deleteTextures);
        FromModule(opengl, "glBindTexture", bindTexture);
        FromModule(opengl, "glTexParameteri", texParameteri);
        FromModule(opengl, "glReadPixels", readPixels);
        FromModule(opengl, "glReadBuffer", readBuffer);
        FromModule(opengl, "glPixelStorei", pixelStorei);
        FromModule(opengl, "glFinish", finish);
        FromModule(opengl, "glFlush", flush);
        FromModule(opengl, "glClearColor", clearColor);
        FromModule(opengl, "glClear", clear);
        FromModule(opengl, "glViewport", viewport);
        FromModule(opengl, "wglCreateContext", createContext);
        FromModule(opengl, "wglMakeCurrent", makeCurrent);
        FromModule(opengl, "wglDeleteContext", deleteContext);
        FromModule(opengl, "wglGetProcAddress", getProcAddress);
        FromModule(gdi, "ChoosePixelFormat", choosePixelFormat);
        FromModule(gdi, "SetPixelFormat", setPixelFormat);
        return getString != nullptr && getError != nullptr && getIntegerv != nullptr &&
               createContext != nullptr && makeCurrent != nullptr && getProcAddress != nullptr &&
               choosePixelFormat != nullptr && setPixelFormat != nullptr;
    }

    // Only meaningful with a context current, and re-run after the context is replaced: entry
    // points are per-context, and a pointer taken from a 4.6 compatibility context is not
    // promised to work in a 3.3 core one.
    void LoadFromContext()
    {
        FromContext("glGetStringi", getStringi);
        FromContext("wglCreateContextAttribsARB", createContextAttribs);
        FromContext("wglGetExtensionsStringARB", wglExtensions);
        FromContext("glGetUnsignedBytevEXT", getUnsignedBytev);
        FromContext("glCreateMemoryObjectsEXT", createMemoryObjects);
        FromContext("glDeleteMemoryObjectsEXT", deleteMemoryObjects);
        FromContext("glMemoryObjectParameterivEXT", memoryObjectParameteriv);
        FromContext("glImportMemoryWin32HandleEXT", importMemoryWin32);
        FromContext("glTexStorage2D", texStorage2D);
        FromContext("glTexStorageMem2DEXT", texStorageMem2D);
        FromContext("glTextureStorageMem2DEXT", textureStorageMem2D);
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
    }

    // Drain and name. GL keeps errors in a queue and reports the oldest first, so an error left
    // behind by an earlier call would otherwise be blamed on this one.
    const char *Drain()
    {
        static char buffer[160];
        buffer[0] = 0;
        int written = 0;
        for (int i = 0; i < 16; ++i)
        {
            const GLenum e = getError();
            if (e == GL_NO_ERROR_)
                break;
            const char *name = e == 0x0500   ? "INVALID_ENUM"
                               : e == 0x0501 ? "INVALID_VALUE"
                               : e == 0x0502 ? "INVALID_OPERATION"
                               : e == 0x0505 ? "OUT_OF_MEMORY"
                               : e == 0x0506 ? "INVALID_FRAMEBUFFER_OPERATION"
                                             : "unknown";
            written += std::snprintf(buffer + written, sizeof(buffer) - written, "%s%s",
                                     written == 0 ? "" : ", ", name);
            if (written >= static_cast<int>(sizeof(buffer)) - 1)
                break;
        }
        return buffer;
    }
    bool Clean() { return *Drain() == 0; }
};

Gl gl;

// ---- the hidden window and its context -------------------------------------------------------

struct Context
{
    HWND window = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
};

LRESULT CALLBACK ProbeWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, m, w, l);
}

// The client area is the crossing's size so the blit test has a real default framebuffer of
// matching dimensions to read out of. The window is never shown: its back buffer exists either
// way, and only the front buffer would need to be on screen.
bool CreateHiddenContext(Context &ctx, int width, int height, int major, int minor, int profileBit)
{
    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = ProbeWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"amd-nr-glprobe";
    RegisterClassExW(&wc);

    RECT rect { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    ctx.window = CreateWindowExW(0, wc.lpszClassName, L"glprobe", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                 CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    if (ctx.window == nullptr)
    {
        std::printf("CreateWindowEx failed (%lu).\n", GetLastError());
        return false;
    }
    ctx.dc = GetDC(ctx.window);

    PIXELFORMATDESCRIPTOR pfd {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    const int format = gl.choosePixelFormat(ctx.dc, &pfd);
    if (format == 0 || !gl.setPixelFormat(ctx.dc, format, &pfd))
    {
        std::printf("no usable pixel format on this DC (%lu).\n", GetLastError());
        return false;
    }

    // The legacy context first, because wglCreateContextAttribsARB can only be resolved through a
    // context that already exists. If no specific version was asked for, this one is kept: it is
    // what an old host gets, and the whole point of the default run is to answer the question on
    // the context a game is most likely to give.
    ctx.rc = gl.createContext(ctx.dc);
    if (ctx.rc == nullptr || !gl.makeCurrent(ctx.dc, ctx.rc))
    {
        std::printf("wglCreateContext failed (%lu). No ICD is driving this DC.\n", GetLastError());
        return false;
    }
    gl.LoadFromContext();

    if (major == 0)
        return true;

    if (gl.createContextAttribs == nullptr)
    {
        std::printf("WGL_ARB_create_context is not available, so -gl cannot be honoured. Running "
                    "on the default context instead.\n");
        return true;
    }
    const int attribs[] { WGL_CONTEXT_MAJOR_VERSION_ARB_,
                          major,
                          WGL_CONTEXT_MINOR_VERSION_ARB_,
                          minor,
                          WGL_CONTEXT_PROFILE_MASK_ARB_,
                          profileBit,
                          0 };
    HGLRC wanted = gl.createContextAttribs(ctx.dc, nullptr, attribs);
    if (wanted == nullptr)
    {
        std::printf("wglCreateContextAttribsARB refused %d.%d %s (%lu). Keeping the default "
                    "context.\n", major, minor,
                    profileBit == WGL_CONTEXT_CORE_PROFILE_BIT_ARB_ ? "core" : "compatibility",
                    GetLastError());
        return true;
    }
    gl.makeCurrent(nullptr, nullptr);
    gl.deleteContext(ctx.rc);
    ctx.rc = wanted;
    if (!gl.makeCurrent(ctx.dc, ctx.rc))
    {
        std::printf("the %d.%d context could not be made current.\n", major, minor);
        return false;
    }
    // Again, on the context that will actually be used: entry points do not carry over.
    gl.LoadFromContext();
    return true;
}

bool HasExtension(const char *wanted)
{
    if (gl.getStringi != nullptr && gl.getIntegerv != nullptr)
    {
        GLint count = 0;
        gl.getIntegerv(GL_NUM_EXTENSIONS_, &count);
        gl.Drain();  // a 1.x context answers GL_NUM_EXTENSIONS with INVALID_ENUM; fall through
        for (GLint i = 0; i < count; ++i)
        {
            const GLubyte *name = gl.getStringi(GL_EXTENSIONS_, static_cast<GLuint>(i));
            if (name != nullptr && std::strcmp(reinterpret_cast<const char *>(name), wanted) == 0)
                return true;
        }
        if (count > 0)
            return false;
    }
    const GLubyte *all = gl.getString(GL_EXTENSIONS_);
    gl.Drain();
    if (all == nullptr)
        return false;
    // Substring is not enough: GL_EXT_semaphore is a prefix of GL_EXT_semaphore_win32.
    const char *text = reinterpret_cast<const char *>(all);
    const size_t length = std::strlen(wanted);
    for (const char *at = std::strstr(text, wanted); at != nullptr;
         at = std::strstr(at + 1, wanted))
        if ((at == text || at[-1] == ' ') && (at[length] == ' ' || at[length] == 0))
            return true;
    return false;
}

// ---- what the crossing would carry -----------------------------------------------------------

struct Case
{
    const char *name;
    DXGI_FORMAT dxgi;
    GLenum internalFormat;
    const char *glName;
    GLenum readFormat;
    GLenum readType;
    UINT bytesPerPixel;
    const char *use;
};

// 256 wide keeps every row pitch a multiple of D3D12's 256-byte copy alignment, so the readback
// footprint is tightly packed and one less thing sits between a byte mismatch and its cause.
constexpr UINT kWidth = 256, kHeight = 64;

constexpr Case kCases[] {
    { "R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM, GL_RGBA8_, "GL_RGBA8", GL_RGBA_,
      GL_UNSIGNED_BYTE_, 4, "colour, and the shape an OpenGL default framebuffer has" },
    { "R8G8B8A8_UNORM_SRGB", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, GL_SRGB8_ALPHA8_, "GL_SRGB8_ALPHA8",
      GL_RGBA_, GL_UNSIGNED_BYTE_, 4, "colour, sRGB-capable framebuffer" },
    { "B8G8R8A8_UNORM", DXGI_FORMAT_B8G8R8A8_UNORM, GL_RGBA8_, "GL_RGBA8 (BGRA swizzle)", GL_RGBA_,
      GL_UNSIGNED_BYTE_, 4, "colour, the usual DXGI swapchain order" },
    { "R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT, GL_RGBA16F_, "GL_RGBA16F", GL_RGBA_,
      GL_HALF_FLOAT_, 8, "colour, the format the network is actually fed" },
    { "R32_FLOAT", DXGI_FORMAT_R32_FLOAT, GL_R32F_, "GL_R32F", GL_RED_, GL_FLOAT_, 4,
      "depth guide" },
    { "R16G16_FLOAT", DXGI_FORMAT_R16G16_FLOAT, GL_RG16F_, "GL_RG16F", GL_RG_, GL_HALF_FLOAT_, 4,
      "motion guide" },
};

void FillPattern(unsigned char *dst, size_t bytes, unsigned seed)
{
    for (size_t i = 0; i < bytes; ++i)
        dst[i] = static_cast<unsigned char>((i * 31u + seed * 97u + (i >> 8)) & 0xFF);
}

size_t FirstDifference(const unsigned char *a, const unsigned char *b, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i)
        if (a[i] != b[i])
            return i;
    return bytes;
}

// One import attempt. Returns the texture, or 0 with the GL error already printed. The handle is
// not consumed by the driver -- EXT_memory_object_win32 leaves NT handle ownership with the
// application -- so the caller still closes it.
GLuint ImportTexture(HANDLE handle, UINT64 size, GLenum handleType, const Case &c, bool dedicated,
                     GLuint &memoryOut, char *why, size_t whyBytes)
{
    memoryOut = 0;
    why[0] = 0;
    gl.Drain();

    GLuint memory = 0;
    gl.createMemoryObjects(1, &memory);
    if (memory == 0)
    {
        std::snprintf(why, whyBytes, "no memory object");
        return 0;
    }
    // Before the import, not after: the object's dedication is fixed at import time, and a D3D12
    // resource handle is dedicated by construction -- it names one resource, not a heap to suballocate.
    if (dedicated && gl.memoryObjectParameteriv != nullptr)
    {
        const GLint one = 1;
        gl.memoryObjectParameteriv(memory, GL_DEDICATED_MEMORY_OBJECT_EXT_, &one);
        if (!gl.Clean())
        {
            std::snprintf(why, whyBytes, "dedicated flag refused");
            gl.deleteMemoryObjects(1, &memory);
            return 0;
        }
    }
    gl.importMemoryWin32(memory, size, handleType, handle);
    const char *importErrors = gl.Drain();
    if (*importErrors != 0)
    {
        std::snprintf(why, whyBytes, "import: %s", importErrors);
        gl.deleteMemoryObjects(1, &memory);
        return 0;
    }

    GLuint texture = 0;
    gl.genTextures(1, &texture);
    gl.bindTexture(GL_TEXTURE_2D_, texture);
    // The D3D12 resource is D3D12_TEXTURE_LAYOUT_UNKNOWN, which is the driver's own swizzled
    // layout, and OPTIMAL is how that is spelled here. LINEAR would be a different memory layout
    // over the same bytes and would read as noise rather than as an error.
    gl.texParameteri(GL_TEXTURE_2D_, GL_TEXTURE_TILING_EXT_,
                     static_cast<GLint>(GL_OPTIMAL_TILING_EXT_));
    gl.Drain();
    gl.texStorageMem2D(GL_TEXTURE_2D_, 1, c.internalFormat, static_cast<GLsizei>(kWidth),
                       static_cast<GLsizei>(kHeight), memory, 0);
    const char *storageErrors = gl.Drain();
    if (*storageErrors != 0)
    {
        std::snprintf(why, whyBytes, "storage: %s", storageErrors);
        gl.bindTexture(GL_TEXTURE_2D_, 0);
        gl.deleteTextures(1, &texture);
        gl.deleteMemoryObjects(1, &memory);
        return 0;
    }
    gl.bindTexture(GL_TEXTURE_2D_, 0);
    // The memory object stays alive for as long as the texture does. EXT_memory_object has no
    // language making a texture keep its memory object alive the way Vulkan forbids freeing
    // VkDeviceMemory under a live VkImage -- and deleting it here was enough to turn the first
    // glReadPixels on the texture into an access violation inside the driver, with the import,
    // the storage and the framebuffer-completeness check all still reporting success. The caller
    // deletes the pair together.
    memoryOut = memory;
    return texture;
}

// Is the imported texture usable as a render target? The GL route's only way in is
// glBlitFramebuffer from the default framebuffer, and that needs this to be complete.
GLenum FramebufferStatusFor(GLuint texture, GLuint &fbo)
{
    if (gl.genFramebuffers == nullptr)
        return 0;
    gl.genFramebuffers(1, &fbo);
    gl.bindFramebuffer(GL_FRAMEBUFFER_, fbo);
    gl.framebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, texture, 0);
    const GLenum status = gl.checkFramebufferStatus(GL_FRAMEBUFFER_);
    gl.bindFramebuffer(GL_FRAMEBUFFER_, 0);
    gl.Drain();
    return status;
}

// ---- the four things the route would do to an imported texture, each behind its own SEH frame -
//
// On this driver the first real GPU access to an imported texture is where an interop problem
// shows up, and it shows up as an access violation inside the ICD rather than as a GL error. A
// probe that dies there reports nothing, including everything it had already proved, so every
// operation runs inside __try and a fault becomes a row in the table. The functions below hold no
// C++ object with a destructor, which is what makes __try legal in TryOp.

enum class Attempt
{
    ok,
    glError,
    faulted,
    skipped,
};

const char *AttemptName(Attempt a)
{
    return a == Attempt::ok        ? "ok"
           : a == Attempt::glError ? "GL error"
           : a == Attempt::faulted ? "FAULTED"
                                   : "not attempted";
}

struct OpArgs
{
    GLuint texture;
    void *pixels;
};

// Read the texture back through an FBO rather than with glGetTexImage. The route has no reason
// to pull a texture into client memory, and an FBO read is what its own blit-out path is made of,
// so this measures something the route will really do.
void OpRead(OpArgs &a)
{
    GLuint fbo = 0;
    gl.genFramebuffers(1, &fbo);
    gl.bindFramebuffer(GL_READ_FRAMEBUFFER_, fbo);
    gl.framebufferTexture2D(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, a.texture,
                            0);
    if (gl.readBuffer != nullptr)
        gl.readBuffer(GL_COLOR_ATTACHMENT0_);
    gl.pixelStorei(GL_PACK_ALIGNMENT_, 1);
    gl.readPixels(0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight), GL_RGBA_,
                  GL_UNSIGNED_BYTE_, a.pixels);
    gl.bindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    gl.deleteFramebuffers(1, &fbo);
}

// The simplest possible GL write into the shared allocation.
void OpClear(OpArgs &a)
{
    GLuint fbo = 0;
    gl.genFramebuffers(1, &fbo);
    gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, fbo);
    gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, a.texture,
                            0);
    gl.clearColor(64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f);
    gl.clear(GL_COLOR_BUFFER_BIT_);
    gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    gl.deleteFramebuffers(1, &fbo);
}

// The route's way in: the game's frame is the default framebuffer, and the only way to get it
// into a texture is a blit. Y is inverted here on purpose -- OpenGL's origin is bottom-left and
// the network's frame is top-left.
void OpBlitIn(OpArgs &a)
{
    GLuint fbo = 0;
    gl.genFramebuffers(1, &fbo);
    gl.bindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    gl.viewport(0, 0, static_cast<GLint>(kWidth), static_cast<GLint>(kHeight));
    gl.clearColor(64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f);
    gl.clear(GL_COLOR_BUFFER_BIT_);
    gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, fbo);
    gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, a.texture,
                            0);
    gl.blitFramebuffer(0, static_cast<GLint>(kHeight), static_cast<GLint>(kWidth), 0, 0, 0,
                       static_cast<GLint>(kWidth), static_cast<GLint>(kHeight), GL_COLOR_BUFFER_BIT_,
                       GL_NEAREST_);
    gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    gl.deleteFramebuffers(1, &fbo);
}

// The route's way out: the network's result, sitting in D3D12 memory, has to reach the frame the
// game is about to present. Same blit, other direction, Y inverted again. The read afterwards is
// of the default framebuffer, which is ordinary memory, so it cannot be what faults.
void OpBlitOut(OpArgs &a)
{
    GLuint fbo = 0;
    gl.genFramebuffers(1, &fbo);
    gl.bindFramebuffer(GL_READ_FRAMEBUFFER_, fbo);
    gl.framebufferTexture2D(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, a.texture,
                            0);
    if (gl.readBuffer != nullptr)
        gl.readBuffer(GL_COLOR_ATTACHMENT0_);
    gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    gl.blitFramebuffer(0, static_cast<GLint>(kHeight), static_cast<GLint>(kWidth), 0, 0, 0,
                       static_cast<GLint>(kWidth), static_cast<GLint>(kHeight), GL_COLOR_BUFFER_BIT_,
                       GL_NEAREST_);
    gl.bindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    gl.deleteFramebuffers(1, &fbo);
    if (gl.readBuffer != nullptr)
        gl.readBuffer(GL_BACK_);
    gl.pixelStorei(GL_PACK_ALIGNMENT_, 1);
    gl.readPixels(0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight), GL_RGBA_,
                  GL_UNSIGNED_BYTE_, a.pixels);
}

// Making an ordinary texture, behind the same guard. This is not paranoia: on this driver the
// call that faults after several imports is this one, on a texture that has nothing to do with
// external memory -- so the control has to be able to report its own creation rather than take
// the probe down while setting itself up.
Attempt TryMakeControlTexture(GLuint &out)
{
    __try
    {
        gl.Drain();
        gl.genTextures(1, &out);
        gl.bindTexture(GL_TEXTURE_2D_, out);
        gl.texStorage2D(GL_TEXTURE_2D_, 1, GL_RGBA8_, static_cast<GLsizei>(kWidth),
                        static_cast<GLsizei>(kHeight));
        gl.bindTexture(GL_TEXTURE_2D_, 0);
        return *gl.Drain() == 0 ? Attempt::ok : Attempt::glError;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return Attempt::faulted;
    }
}

Attempt TryOp(void (*op)(OpArgs &), OpArgs &args)
{
    __try
    {
        gl.Drain();
        op(args);
        gl.finish();
        return *gl.Drain() == 0 ? Attempt::ok : Attempt::glError;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return Attempt::faulted;
    }
}

// Using an imported semaphore for real, in a function of its own that holds no C++ object, so
// that __try is legal in it. This is the one call in the whole crossing that can take the process
// with it: if the driver advertises EXT_semaphore_win32, imports the fence without complaint and
// then has nothing behind the wait, the result is an access violation inside the ICD rather than
// a GL error -- and a probe that dies there reports nothing at all, including everything it had
// already proved. Catching it turns a crash into a row in the output.
enum class SemaphoreResult
{
    worked,
    glError,
    faulted,
};

SemaphoreResult TrySemaphoreWait(GLuint semaphore, GLuint64 value, GLuint texture)
{
    __try
    {
        const GLenum layout = GL_LAYOUT_TRANSFER_SRC_EXT_;
        gl.semaphoreParameterui64v(semaphore, GL_D3D12_FENCE_VALUE_EXT_, &value);
        gl.waitSemaphore(semaphore, 0, nullptr, 1, &texture, &layout);
        gl.flush();
        gl.finish();
        return *gl.Drain() == 0 ? SemaphoreResult::worked : SemaphoreResult::glError;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return SemaphoreResult::faulted;
    }
}

}  // namespace

int main(int argc, char **argv)
{
    int major = 0, minor = 0;
    int profileBit = WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB_;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "-gl") == 0 && i + 1 < argc)
        {
            const char *v = argv[++i];
            major = std::atoi(v);
            const char *dot = std::strchr(v, '.');
            minor = dot != nullptr ? std::atoi(dot + 1) : 0;
        }
        else if (std::strcmp(argv[i], "-core") == 0)
            profileBit = WGL_CONTEXT_CORE_PROFILE_BIT_ARB_;
        else if (std::strcmp(argv[i], "-compat") == 0)
            profileBit = WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB_;
        else
        {
            std::printf("usage: amd-nr-glprobe.exe [-gl MAJOR.MINOR] [-core|-compat]\n"
                        "  no -gl: the context wglCreateContext gives, which is what a legacy\n"
                        "  host such as an old OpenGL game gets.\n");
            return 2;
        }
    }

    // Unbuffered: a probe that pokes at a driver can take the process down with it, and the last
    // line printed before that happens is the whole diagnosis.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(ReportFault);

    std::printf("glprobe -- can OpenGL import D3D12 memory and fences on this driver?\n");
    Line();

    if (!gl.LoadModules())
    {
        std::printf("opengl32.dll or gdi32.dll did not load, or an entry point is missing. There\n"
                    "is no OpenGL on this machine, so an OpenGL host could not run here either.\n");
        return 2;
    }

    Context ctx {};
    if (!CreateHiddenContext(ctx, static_cast<int>(kWidth), static_cast<int>(kHeight), major, minor,
                             profileBit))
        return 2;

    // ---- who is driving this context ---------------------------------------------------------
    const GLubyte *vendor = gl.getString(GL_VENDOR_);
    const GLubyte *renderer = gl.getString(GL_RENDERER_);
    const GLubyte *version = gl.getString(GL_VERSION_);
    const GLubyte *glsl = gl.getString(GL_SHADING_LANGUAGE_VERSION_);
    gl.Drain();
    std::printf("vendor   %s\n", vendor != nullptr ? reinterpret_cast<const char *>(vendor) : "?");
    std::printf("renderer %s\n",
                renderer != nullptr ? reinterpret_cast<const char *>(renderer) : "?");
    std::printf("version  %s\n",
                version != nullptr ? reinterpret_cast<const char *>(version) : "?");
    std::printf("glsl     %s\n", glsl != nullptr ? reinterpret_cast<const char *>(glsl) : "?");

    GLint contextMajor = 0, contextMinor = 0, profileMask = 0;
    gl.getIntegerv(GL_MAJOR_VERSION_, &contextMajor);
    gl.getIntegerv(GL_MINOR_VERSION_, &contextMinor);
    gl.getIntegerv(GL_CONTEXT_PROFILE_MASK_, &profileMask);
    gl.Drain();  // a pre-3.0 context refuses all three, and that is an answer too
    std::printf("context  %d.%d %s%s\n", contextMajor, contextMinor,
                (profileMask & GL_CONTEXT_CORE_PROFILE_BIT_) != 0            ? "core"
                : (profileMask & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT_) != 0 ? "compatibility"
                                                                            : "(no profile mask)",
                major == 0 ? "   <- the default context, which is what a legacy host gets" : "");
    Line();

    // ---- the extensions the crossing is built out of -----------------------------------------
    struct Needed
    {
        const char *name;
        bool required;
    };
    constexpr Needed kNeeded[] {
        { "GL_EXT_memory_object", true },   { "GL_EXT_memory_object_win32", true },
        { "GL_EXT_semaphore", false },      { "GL_EXT_semaphore_win32", false },
        { "GL_EXT_win32_keyed_mutex", false },
    };
    bool missingRequired = false, haveSemaphores = true;
    std::printf("extensions:\n");
    for (const auto &n : kNeeded)
    {
        const bool present = HasExtension(n.name);
        if (!present && n.required)
            missingRequired = true;
        if (!present && std::strstr(n.name, "semaphore") != nullptr)
            haveSemaphores = false;
        std::printf("  %-30s %s%s\n", n.name, present ? "yes" : "MISSING",
                    present || n.required ? "" : "   (optional here)");
    }

    // An extension string with no entry point behind it is a driver bug, and a null pointer call
    // is a crash rather than a failure, so check before anything is used.
    std::printf("entry points:\n");
    struct Resolved
    {
        const char *name;
        const void *fn;
        bool required;
    };
    const Resolved kResolved[] {
        { "glCreateMemoryObjectsEXT", reinterpret_cast<const void *>(gl.createMemoryObjects), true },
        { "glMemoryObjectParameterivEXT",
          reinterpret_cast<const void *>(gl.memoryObjectParameteriv), true },
        { "glImportMemoryWin32HandleEXT", reinterpret_cast<const void *>(gl.importMemoryWin32),
          true },
        { "glTexStorageMem2DEXT", reinterpret_cast<const void *>(gl.texStorageMem2D), true },
        { "glTextureStorageMem2DEXT", reinterpret_cast<const void *>(gl.textureStorageMem2D),
          false },
        { "glImportSemaphoreWin32HandleEXT",
          reinterpret_cast<const void *>(gl.importSemaphoreWin32), false },
        { "glWaitSemaphoreEXT", reinterpret_cast<const void *>(gl.waitSemaphore), false },
        { "glSignalSemaphoreEXT", reinterpret_cast<const void *>(gl.signalSemaphore), false },
        { "glBlitFramebuffer", reinterpret_cast<const void *>(gl.blitFramebuffer), true },
        { "glGetUnsignedBytevEXT", reinterpret_cast<const void *>(gl.getUnsignedBytev), false },
    };
    for (const auto &r : kResolved)
    {
        if (r.fn == nullptr && r.required)
            missingRequired = true;
        std::printf("  %-34s %s%s\n", r.name, r.fn != nullptr ? "yes" : "no",
                    r.fn != nullptr || r.required ? ""
                                                  : "    (the route can work without it)");
    }
    if (gl.textureStorageMem2D == nullptr)
        std::printf("  note: no DSA form on this context, so the route must bind and use\n"
                    "        glTexStorageMem2DEXT. That is the portable form anyway.\n");
    if (gl.importSemaphoreWin32 == nullptr || !haveSemaphores)
        std::printf("  note: without imported semaphores the crossing has to fall back to a CPU\n"
                    "        stall (glFinish plus a fence wait), which is what the D3D11 route\n"
                    "        already does in FlushAndWait11. Slower, not impossible.\n");
    Line();

    if (missingRequired)
    {
        std::printf("BLOCKED: this context cannot import external memory at all. Nothing below\n"
                    "would mean anything, so the probe stops here.\n");
        return 1;
    }

    // ---- which GPU is this context on? -------------------------------------------------------
    //
    // Shared handles only cross between devices on one physical adapter. EXT_memory_object
    // reports the LUID, which is the same identifier D3D12 uses, so the two can be matched
    // exactly rather than by name -- and three adapters on this machine share a name.
    LUID glLuid {};
    bool luidKnown = false;
    if (gl.getUnsignedBytev != nullptr)
    {
        GLubyte bytes[8] {};
        gl.getUnsignedBytev(GL_DEVICE_LUID_EXT_, bytes);
        if (gl.Clean())
        {
            std::memcpy(&glLuid, bytes, sizeof(glLuid));
            luidKnown = true;
        }
    }

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        std::printf("no DXGI factory.\n");
        return 2;
    }
    ComPtr<IDXGIAdapter1> adapter, chosen;
    std::printf("adapters:\n");
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        const bool match = luidKnown && desc.AdapterLuid.LowPart == glLuid.LowPart &&
                           desc.AdapterLuid.HighPart == glLuid.HighPart;
        std::printf("  DXGI %u: %-40ls vendor %04x  luid %08lX%08lX%s\n", i, desc.Description,
                    desc.VendorId, static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                    static_cast<unsigned long>(desc.AdapterLuid.LowPart),
                    match ? "   <- the OpenGL context is on this one" : "");
        if (match)
            chosen = adapter;
        else if (chosen == nullptr && !luidKnown && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
            chosen = adapter;  // no LUID to match on; the first hardware adapter is the guess
        adapter.Reset();
    }
    if (!luidKnown)
        std::printf("  the context reported no LUID, so the adapter above is a guess. An import\n"
                    "  failing after this line may only mean two different GPUs.\n");
    if (chosen == nullptr)
    {
        std::printf("no DXGI adapter matches this OpenGL context. They are different GPUs and\n"
                    "nothing can be shared between them.\n");
        return 1;
    }
    Line();

    // ---- the D3D12 side, which is the side the network runs on -------------------------------
    ComPtr<ID3D12Device> dev12;
    if (FAILED(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12))))
    {
        std::printf("D3D12CreateDevice failed on that adapter.\n");
        return 2;
    }
    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12CommandAllocator> alloc12;
    ComPtr<ID3D12GraphicsCommandList> list12;
    if (FAILED(dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue12))) ||
        FAILED(dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&alloc12))) ||
        FAILED(dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc12.Get(), nullptr,
                                        IID_PPV_ARGS(&list12))))
    {
        std::printf("D3D12 queue/allocator/list creation failed.\n");
        return 2;
    }
    list12->Close();

    // ---- the fences ---------------------------------------------------------------------------
    // toGl: D3D12 signals, OpenGL waits. toD3D: the other way. A D3D12 fence is a 64-bit
    // monotonic counter, and EXT_semaphore_win32 carries the value to wait for as a semaphore
    // parameter, which is how a counter fits through an interface built for binary semaphores.
    ComPtr<ID3D12Fence> fenceToGl, fenceToD3D;
    HANDLE hFenceToGl = nullptr, hFenceToD3D = nullptr;
    GLuint semToGl = 0, semToD3D = 0;
    bool semaphoresWork = false;
    if (SUCCEEDED(dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fenceToGl))) &&
        SUCCEEDED(dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fenceToD3D))) &&
        SUCCEEDED(dev12->CreateSharedHandle(fenceToGl.Get(), nullptr, GENERIC_ALL, nullptr,
                                            &hFenceToGl)) &&
        SUCCEEDED(dev12->CreateSharedHandle(fenceToD3D.Get(), nullptr, GENERIC_ALL, nullptr,
                                            &hFenceToD3D)))
    {
        std::printf("fences (D3D12 fence -> GL semaphore):\n");
        if (gl.importSemaphoreWin32 != nullptr && gl.genSemaphores != nullptr)
        {
            GLuint pair[2] {};
            gl.genSemaphores(2, pair);
            semToGl = pair[0];
            semToD3D = pair[1];
            gl.Drain();
            gl.importSemaphoreWin32(semToGl, GL_HANDLE_TYPE_D3D12_FENCE_EXT_, hFenceToGl);
            const char *first = gl.Drain();
            gl.importSemaphoreWin32(semToD3D, GL_HANDLE_TYPE_D3D12_FENCE_EXT_, hFenceToD3D);
            const char *second = gl.Drain();
            semaphoresWork = *first == 0 && *second == 0;
            std::printf("  D3D12 -> OpenGL   %s\n", *first == 0 ? "imported" : first);
            std::printf("  OpenGL -> D3D12   %s\n", *second == 0 ? "imported" : second);
            if (!semaphoresWork)
                std::printf("  falling back to a CPU stall for the byte tests below.\n");
        }
        else
        {
            std::printf("  not attempted: no glImportSemaphoreWin32HandleEXT on this context.\n");
        }
    }
    Line();

    // ---- the table: one import attempt per format, all released before the next --------------
    //
    // OPAQUE_WIN32 is in the table for the same reason vkprobe carried the handle types it does
    // not use: "AMD supports none of this" and "AMD supports it under a different handle type"
    // are different findings with very different amounts of work behind them.
    std::printf("texture import (GL_HANDLE_TYPE_D3D12_RESOURCE_EXT), per format:\n");
    std::printf("  %-20s %-24s %-22s %-10s %s\n", "format", "GL internal format", "import", "FBO",
                "what it would carry");

    bool d3d12ResourceWorks = false, colourFboComplete = false;
    std::vector<HANDLE> keptHandles;
    GLuint colourTexture = 0, colourMemory = 0;
    HANDLE colourHandle = nullptr;
    ComPtr<ID3D12Resource> colourShared;
    for (const auto &c : kCases)
    {
        std::printf("  %-20s %-24s", c.name, c.glName);

        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = kWidth;
        rd.Height = kHeight;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = c.dxgi;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        const UINT64 size = dev12->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes;

        GLuint keep = 0, keepMemory = 0;
        GLenum fboStatus = 0;
        HANDLE keepHandle = nullptr;
        ComPtr<ID3D12Resource> shared;
        if (FAILED(dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                                                  D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&shared))) ||
            FAILED(dev12->CreateSharedHandle(shared.Get(), nullptr, GENERIC_ALL, nullptr,
                                             &keepHandle)))
        {
            std::printf(" %-22s %-10s %s\n", "D3D12 refused it", "-", c.use);
            continue;
        }
        char why[96] {};
        keep = ImportTexture(keepHandle, size, GL_HANDLE_TYPE_D3D12_RESOURCE_EXT_, c, true,
                             keepMemory, why, sizeof(why));
        if (keep != 0)
            d3d12ResourceWorks = true;
        std::printf(" %-22s", keep != 0 ? "IMPORT" : (why[0] != 0 ? why : "refused"));

        // Usable as a render target? The route's only way in is a blit, and a blit needs this.
        if (keep != 0)
        {
            GLuint fbo = 0;
            fboStatus = FramebufferStatusFor(keep, fbo);
            std::printf(" %-10s", fboStatus == GL_FRAMEBUFFER_COMPLETE_ ? "complete"
                                  : fboStatus == 0                      ? "n/a"
                                                                        : "INCOMPLETE");
            if (fbo != 0)
            {
                gl.deleteFramebuffers(1, &fbo);
                gl.Drain();
            }
        }
        else
        {
            std::printf(" %-10s", "-");
        }
        std::printf(" %s\n", c.use);

        // The GL objects are released before the next row is imported, so that no two imported
        // textures are alive at once; whether this driver tolerates two is a question of its own,
        // asked further down. The shared handle is a different matter and is kept.
        if (c.dxgi == DXGI_FORMAT_R8G8B8A8_UNORM)
            colourFboComplete = fboStatus == GL_FRAMEBUFFER_COMPLETE_;
        if (keep != 0)
        {
            gl.deleteTextures(1, &keep);
            gl.deleteMemoryObjects(1, &keepMemory);
            gl.Drain();
        }
        // The NT handle is deliberately NOT closed. See the last section: closing it after the
        // import is what faults this driver, and it does so from a thread of its own some time
        // later, which is why every earlier version of this probe died in a different place.
        keptHandles.push_back(keepHandle);
    }
    Line();

    // ---- the one import the rest of the probe works on ---------------------------------------
    //
    // Made fresh, after the table has released everything, for the same reason the table releases
    // as it goes.
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = kWidth;
        rd.Height = kHeight;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (SUCCEEDED(dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&colourShared))) &&
            SUCCEEDED(dev12->CreateSharedHandle(colourShared.Get(), nullptr, GENERIC_ALL, nullptr,
                                                &colourHandle)))
        {
            char why[96] {};
            colourTexture = ImportTexture(colourHandle, dev12->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes,
                                          GL_HANDLE_TYPE_D3D12_RESOURCE_EXT_, kCases[0], true,
                                          colourMemory, why, sizeof(why));
            if (colourTexture == 0)
                std::printf("the working import failed: %s\n", why[0] != 0 ? why : "refused");
        }
    }

    if (colourTexture == 0)
    {
        std::printf("R8G8B8A8_UNORM did not import, so there is nothing to round-trip. The table\n"
                    "above is the finding.\n");
        return 1;
    }

    // ---- does the imported texture actually alias the D3D12 allocation? ----------------------
    //
    // Everything so far could be true of a driver that quietly allocated its own texture and
    // returned it. Known bytes written on one side and read on the other is the only thing that
    // separates a real import from a polite one.
    const size_t bytes = static_cast<size_t>(kWidth) * kHeight * 4;
    const UINT rowPitch = kWidth * 4;  // already a multiple of 256
    std::vector<unsigned char> sent(bytes), got(bytes);
    FillPattern(sent.data(), bytes, 1);

    D3D12_HEAP_PROPERTIES uploadHeap {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = bytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload, readback;
    D3D12_HEAP_PROPERTIES readbackHeap {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(dev12->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&upload))) ||
        FAILED(dev12->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&readback))))
    {
        std::printf("upload or readback buffer creation failed.\n");
        return 2;
    }
    void *uploadPtr = nullptr;
    upload->Map(0, nullptr, &uploadPtr);
    std::memcpy(uploadPtr, sent.data(), bytes);
    upload->Unmap(0, nullptr);

    HANDLE cpuEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 toGlValue = 0, toD3DValue = 0;
    ID3D12CommandList *lists[] { list12.Get() };

    auto recordCopy = [&](bool intoShared) {
        alloc12->Reset();
        list12->Reset(alloc12.Get(), nullptr);
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = colourShared.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter =
            intoShared ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
        list12->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION tex {}, buf {};
        tex.pResource = colourShared.Get();
        tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        buf.pResource = intoShared ? upload.Get() : readback.Get();
        buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        buf.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, 1,
                                          rowPitch };
        if (intoShared)
            list12->CopyTextureRegion(&tex, 0, 0, 0, &buf, nullptr);
        else
            list12->CopyTextureRegion(&buf, 0, 0, 0, &tex, nullptr);

        // Back to COMMON before handing over: COMMON is the state a shared resource has to be in
        // for the other API to touch it.
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list12->ResourceBarrier(1, &barrier);
        list12->Close();
        queue12->ExecuteCommandLists(1, lists);
    };

    // The synchronisation for the tests below is the CPU stall: submit, wait for the fence on the
    // CPU, then hand over. It is what FlushAndWait11 does on the D3D11 route and what the Vulkan
    // route does with flush plus wait_idle, so it is the project's known-good pattern rather than
    // a shortcut. Whether the imported semaphores can replace it is asked separately, at the end,
    // because on this driver finding out can take the process down.
    auto waitForD3D12 = [&] {
        fenceToGl->SetEventOnCompletion(toGlValue, cpuEvent);
        return WaitForSingleObject(cpuEvent, 5000) == WAIT_OBJECT_0;
    };

    // ---- what can actually be done with an imported texture? ---------------------------------
    //
    // A control first: the same four operations, through the same code, on an ordinary GL texture
    // of the same size and format. If the control faults as well then the probe is what is
    // broken; if only the imported texture does, the import is. Without that line the rest of
    // this section proves nothing about interop.
    std::printf("operations on R8G8B8A8_UNORM (CPU stall between the two APIs):\n");
    bool contextPoisoned = false;

    GLuint control = 0;
    if (gl.texStorage2D != nullptr)
    {
        const Attempt made = TryMakeControlTexture(control);
        Attempt clear = Attempt::skipped, read = Attempt::skipped;
        if (made == Attempt::ok)
        {
            OpArgs args { control, got.data() };
            clear = TryOp(OpClear, args);
            read = TryOp(OpRead, args);
        }
        std::printf("  %-28s make %-10s clear %-10s read %s\n", "control, ordinary texture",
                    AttemptName(made), AttemptName(clear), AttemptName(read));
        contextPoisoned = made == Attempt::faulted || clear == Attempt::faulted ||
                          read == Attempt::faulted;
        if (contextPoisoned)
            Fail("an ordinary texture, with no external memory anywhere near it, faulted after "
                 "the imports above -- the context itself is gone, not just the import");
        else if (made != Attempt::ok || clear != Attempt::ok || read != Attempt::ok)
            Fail("the control texture failed the same operations, so this probe's own GL code is "
                 "what is wrong");
    }

    // --- D3D12 writes, OpenGL reads -----------------------------------------------------------
    bool aliasIn = false;
    Attempt importedRead = Attempt::skipped;
    if (!contextPoisoned)
    {
        recordCopy(true);
        queue12->Signal(fenceToGl.Get(), ++toGlValue);
        if (!waitForD3D12())
        {
            Fail("the wait for the D3D12 copy never completed");
        }
        else
        {
            std::memset(got.data(), 0, bytes);
            OpArgs args { colourTexture, got.data() };
            importedRead = TryOp(OpRead, args);
            contextPoisoned = importedRead == Attempt::faulted;

            // Two comparisons, because "are these the same bytes" depends on which way up the
            // reader thinks the image is. glReadPixels returns row 0 as the BOTTOM row; D3D12
            // wrote row 0 as the top. Matching upside down is the expected result and is the flip
            // the route will have to undo; matching straight would mean this driver does not flip
            // and the route must not either.
            bool flipped = importedRead == Attempt::ok;
            for (UINT row = 0; row < kHeight && flipped; ++row)
                flipped = std::memcmp(&got[static_cast<size_t>(row) * rowPitch],
                                      &sent[static_cast<size_t>(kHeight - 1 - row) * rowPitch],
                                      rowPitch) == 0;
            const size_t straight = FirstDifference(sent.data(), got.data(), bytes);
            aliasIn = importedRead == Attempt::ok && (flipped || straight == bytes);
            std::printf("  %-28s read  %-14s %s\n", "D3D12 wrote, OpenGL reads",
                        AttemptName(importedRead),
                        aliasIn ? (straight == bytes ? "bytes identical, same row order"
                                                     : "bytes identical, rows inverted (OpenGL "
                                                       "reads bottom-up, as expected)")
                        : importedRead == Attempt::ok ? "bytes DIFFER either way up"
                                                      : "");
            if (importedRead == Attempt::ok && !aliasIn)
                Fail("the imported texture does not hold what D3D12 wrote");
        }
    }

    // --- OpenGL writes, D3D12 reads -----------------------------------------------------------
    //
    // A clear is the smallest possible GL write into the shared allocation, and D3D12 reading it
    // back on its own device is what proves the write landed in shared memory rather than in
    // something the driver kept to itself.
    bool aliasOut = false;
    Attempt importedClear = Attempt::skipped;
    if (!contextPoisoned)
    {
        OpArgs args { colourTexture, got.data() };
        importedClear = TryOp(OpClear, args);
        contextPoisoned = importedClear == Attempt::faulted;
        size_t wrong = bytes;
        if (importedClear == Attempt::ok)
        {
            recordCopy(false);
            queue12->Signal(fenceToGl.Get(), ++toGlValue);
            if (waitForD3D12())
            {
                std::vector<unsigned char> back(bytes, 0);
                void *readPtr = nullptr;
                readback->Map(0, nullptr, &readPtr);
                std::memcpy(back.data(), readPtr, bytes);
                readback->Unmap(0, nullptr);
                wrong = 0;
                for (size_t i = 0; i < bytes; i += 4)
                    if (back[i] != 0x40 || back[i + 1] != 0x80 || back[i + 2] != 0xC0)
                        ++wrong;
            }
        }
        aliasOut = importedClear == Attempt::ok && wrong == 0;
        std::printf("  %-28s clear %-14s %s\n", "OpenGL cleared, D3D12 reads",
                    AttemptName(importedClear),
                    aliasOut                          ? "all pixels arrived on the D3D12 side"
                    : importedClear == Attempt::ok    ? "the pixels did NOT reach D3D12 memory"
                                                      : "");
        if (importedClear == Attempt::ok && !aliasOut)
            Fail("what OpenGL cleared never reached the D3D12 side");
    }

    // --- the route's own copy path, in and out ------------------------------------------------
    //
    // This is the part with no Vulkan equivalent. There the back buffer is a VkImage and the
    // crossing is one vkCmdCopyImage. In OpenGL the back buffer is the default framebuffer and
    // cannot be copied as a texture, so the only way across in either direction is a blit
    // between framebuffers. If these two work, the GL route has a transport; if they do not, it
    // needs a full-screen draw instead, which is a different amount of work.
    bool blitIn = false, blitOut = false;
    Attempt blitInAttempt = Attempt::skipped, blitOutAttempt = Attempt::skipped;
    if (!contextPoisoned && gl.blitFramebuffer != nullptr)
    {
        OpArgs args { colourTexture, got.data() };
        blitInAttempt = TryOp(OpBlitIn, args);
        contextPoisoned = blitInAttempt == Attempt::faulted;
        size_t wrong = bytes;
        if (blitInAttempt == Attempt::ok)
        {
            recordCopy(false);
            queue12->Signal(fenceToGl.Get(), ++toGlValue);
            if (waitForD3D12())
            {
                std::vector<unsigned char> back(bytes, 0);
                void *readPtr = nullptr;
                readback->Map(0, nullptr, &readPtr);
                std::memcpy(back.data(), readPtr, bytes);
                readback->Unmap(0, nullptr);
                wrong = 0;
                for (size_t i = 0; i < bytes; i += 4)
                    if (back[i] != 0x40 || back[i + 1] != 0x80 || back[i + 2] != 0xC0)
                        ++wrong;
            }
        }
        blitIn = blitInAttempt == Attempt::ok && wrong == 0;
        std::printf("  %-28s blit  %-14s %s\n", "default framebuffer -> shared",
                    AttemptName(blitInAttempt),
                    blitIn                         ? "the frame crossed into D3D12 memory, Y inverted"
                    : blitInAttempt == Attempt::ok ? "the blit did not land in D3D12 memory"
                                                   : "");
        if (blitInAttempt == Attempt::ok && !blitIn)
            Fail("the blit out of the default framebuffer did not reach D3D12 memory");
    }
    if (!contextPoisoned && gl.blitFramebuffer != nullptr)
    {
        // The other direction, which is how a result would reach the frame the game presents.
        // D3D12 puts the pattern back first, so what the default framebuffer ends up holding can
        // be checked against something known.
        recordCopy(true);
        queue12->Signal(fenceToGl.Get(), ++toGlValue);
        if (waitForD3D12())
        {
            std::memset(got.data(), 0, bytes);
            OpArgs args { colourTexture, got.data() };
            blitOutAttempt = TryOp(OpBlitOut, args);
            contextPoisoned = blitOutAttempt == Attempt::faulted;
            bool flipped = blitOutAttempt == Attempt::ok;
            // Two inversions -- the blit's and glReadPixels' -- cancel, so the default
            // framebuffer read comes back in D3D12's own row order.
            const size_t straight = FirstDifference(sent.data(), got.data(), bytes);
            for (UINT row = 0; row < kHeight && flipped; ++row)
                flipped = std::memcmp(&got[static_cast<size_t>(row) * rowPitch],
                                      &sent[static_cast<size_t>(kHeight - 1 - row) * rowPitch],
                                      rowPitch) == 0;
            blitOut = blitOutAttempt == Attempt::ok && (straight == bytes || flipped);
            std::printf("  %-28s blit  %-14s %s\n", "shared -> default framebuffer",
                        AttemptName(blitOutAttempt),
                        blitOut ? (straight == bytes ? "the frame came back, same row order"
                                                     : "the frame came back, rows inverted")
                        : blitOutAttempt == Attempt::ok ? "what came back is not what D3D12 wrote"
                                                        : "");
            if (blitOutAttempt == Attempt::ok && !blitOut)
                Fail("the blit back into the default framebuffer did not carry the frame");
        }
    }
    if (contextPoisoned)
        std::printf("  the driver faulted; everything after that point is left unattempted, and\n"
                    "  the GL teardown is skipped for the same reason.\n");
    Line();

    // ---- can the imported fences replace the stall? -------------------------------------------
    //
    // Last, and deliberately so. Everything above is already printed by the time this runs,
    // because a wait that faults leaves the context in a state nothing else should be attempted
    // in -- so this is also where the GL teardown is given up on if it happens.
    bool semaphoreUsable = false;
    if (!contextPoisoned && semaphoresWork && gl.waitSemaphore != nullptr &&
        gl.semaphoreParameterui64v != nullptr)
    {
        std::printf("the imported fence, actually waited on:\n");
        recordCopy(true);
        queue12->Signal(fenceToGl.Get(), ++toGlValue);
        const SemaphoreResult result = TrySemaphoreWait(semToGl, toGlValue, colourTexture);
        semaphoreUsable = result == SemaphoreResult::worked;
        contextPoisoned = result == SemaphoreResult::faulted;
        if (semaphoreUsable)
            std::printf("  glWaitSemaphoreEXT on the imported D3D12 fence completed. The route can\n"
                        "  synchronise on the GPU instead of stalling the CPU.\n");
        else if (result == SemaphoreResult::glError)
            std::printf("  glWaitSemaphoreEXT returned a GL error. The import is accepted and the\n"
                        "  wait is not usable, so the route keeps the CPU stall.\n");
        else
            std::printf("  glWaitSemaphoreEXT FAULTED (access violation inside the driver). The\n"
                        "  extension is advertised and the import is accepted, but there is\n"
                        "  nothing behind the wait on this driver. The route must keep the CPU\n"
                        "  stall, and must never call it -- this is a crash, not a failure.\n");
        Line();
    }

    // ---- how many imported textures can be alive at once? -------------------------------------
    //
    // The route needs at least two -- colour in and result out -- and four if it ever carries
    // guides. This is asked last because the answer on this driver is that a second live import
    // poisons the context so thoroughly that creating an ordinary texture afterwards faults, and
    // there is no way to ask the question that leaves the context fit for anything else.
    Attempt secondImport = Attempt::skipped, afterSecond = Attempt::skipped;
    if (!contextPoisoned && colourTexture != 0)
    {
        std::printf("a second imported texture alive at the same time:\n");
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = kWidth;
        rd.Height = kHeight;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ComPtr<ID3D12Resource> second;
        HANDLE secondHandle = nullptr;
        if (SUCCEEDED(dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&second))) &&
            SUCCEEDED(dev12->CreateSharedHandle(second.Get(), nullptr, GENERIC_ALL, nullptr,
                                                &secondHandle)))
        {
            char why[96] {};
            GLuint secondMemory = 0;
            const GLuint secondTexture =
                ImportTexture(secondHandle, dev12->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes,
                              GL_HANDLE_TYPE_D3D12_RESOURCE_EXT_, kCases[0], true, secondMemory,
                              why, sizeof(why));
            secondImport = secondTexture != 0 ? Attempt::ok : Attempt::glError;
            std::printf("  import while the first is still alive     %s%s%s\n",
                        AttemptName(secondImport), secondTexture != 0 ? "" : ": ",
                        secondTexture != 0 ? "" : (why[0] != 0 ? why : "refused"));
            if (secondTexture != 0)
            {
                // The first texture, not the second: what this is looking for is whether the
                // second import disturbed anything that already worked.
                OpArgs args { colourTexture, got.data() };
                afterSecond = TryOp(OpRead, args);
                contextPoisoned = afterSecond == Attempt::faulted;
                std::printf("  reading the FIRST texture afterwards      %s\n",
                            AttemptName(afterSecond));
                if (afterSecond == Attempt::faulted)
                    std::printf("  A second live import breaks the first one, and the driver\n"
                                "  faults inside the ICD rather than returning an error. Every\n"
                                "  row above this one was measured with exactly one imported\n"
                                "  texture alive, and stands. What it costs the route is in the\n"
                                "  verdict.\n");
            }
        }
        Line();
    }

    // ---- the wrong handle type, which this driver accepts -------------------------------------
    //
    // A D3D12 resource handle is not an opaque Win32 memory handle, and
    // GL_HANDLE_TYPE_OPAQUE_WIN32_EXT is the wrong thing to import it as. This driver takes it
    // anyway -- no error, storage created, framebuffer complete -- so "the import succeeded" says
    // nothing about the handle type being right. That is the finding: a route that falls back to
    // OPAQUE_WIN32 when D3D12_RESOURCE is refused would get a yes and no way to tell it apart
    // from a real one.
    //
    // Whether the resulting texture then works is measured rather than assumed, in three steps,
    // because the answer has moved between driver states while this file was being written: an
    // ordinary texture before, a read of the wrongly-imported texture, an ordinary texture after.
    // If the third one faults, the wrong handle type took the context with it.
    //
    // It is near the end because it is the riskiest thing here, and everything above is printed
    // by the time it runs.
    bool opaqueTrap = false;
    if (!contextPoisoned)
    {
        std::printf("the wrong handle type, OPAQUE_WIN32 on a D3D12 resource handle:\n");
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = kWidth;
        rd.Height = kHeight;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ComPtr<ID3D12Resource> wrong;
        HANDLE wrongHandle = nullptr;
        if (SUCCEEDED(dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&wrong))) &&
            SUCCEEDED(dev12->CreateSharedHandle(wrong.Get(), nullptr, GENERIC_ALL, nullptr,
                                                &wrongHandle)))
        {
            char why[96] {};
            GLuint wrongMemory = 0;
            const GLuint wrongTexture =
                ImportTexture(wrongHandle, dev12->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes,
                              GL_HANDLE_TYPE_OPAQUE_WIN32_EXT_, kCases[0], true, wrongMemory, why,
                              sizeof(why));
            if (wrongTexture == 0)
            {
                std::printf("  refused, which is the correct answer: %s\n",
                            why[0] != 0 ? why : "no reason given");
            }
            else
            {
                // Three steps, because the damage does not appear where the mistake is made.
                // The import returns no error; using the texture is what faults; and the
                // ordinary texture afterwards says whether the whole context went with it.
                GLuint before = 0;
                const Attempt madeBefore = TryMakeControlTexture(before);
                OpArgs args { wrongTexture, got.data() };
                const Attempt used = TryOp(OpRead, args);
                GLuint after = 0;
                const Attempt madeAfter = TryMakeControlTexture(after);
                opaqueTrap = used == Attempt::faulted || madeAfter == Attempt::faulted;
                contextPoisoned = contextPoisoned || opaqueTrap;
                std::printf("  import ACCEPTED, ordinary texture before %s, reading it %s,\n"
                            "  ordinary texture after %s\n",
                            AttemptName(madeBefore), AttemptName(used), AttemptName(madeAfter));
                if (opaqueTrap)
                    std::printf("  So the wrong handle type is a trap on this driver, not an\n"
                                "  error: it succeeds, and the context is gone from the next call\n"
                                "  onwards. The route must name D3D12_RESOURCE and stand down if\n"
                                "  it is unavailable, never fall back to this.\n");
            }
        }
        Line();
    }

    // ---- closing the shared handle after importing it -----------------------------------------
    //
    // Last, and the reason for everything above being written the way it is.
    //
    // Vulkan's rule for an imported Win32 handle is that ownership does not transfer: the driver
    // takes its own reference and the application closes its handle afterwards. vk_route.inc
    // relies on that, and EXT_memory_object_win32 is written the same way. This driver does not
    // behave that way. Closing the handle after the import -- even after the texture and the
    // memory object have both been deleted -- faults inside the ICD, from a driver thread, some
    // unpredictable time later. Measured over eight runs of an earlier version of this probe that
    // did close its handles: five died, three did not, and the crash landed in a different place
    // each time. With the handles kept open, thirty-two runs in a row passed.
    //
    // So the route must treat an imported texture's handle as owned by the driver for as long as
    // the process lives. One handle per crossing texture, a handful in total, never closed. That
    // is a leak of a kernel handle and it is the cheap side of this trade.
    //
    // The loop below is bounded and stops at the first fault, because once it happens nothing
    // after it can be trusted.
    if (!contextPoisoned)
    {
        std::printf("closing the shared handle after import (the driver's own rule, tested):\n");
        int survived = 0;
        constexpr int kAttempts = 12;
        for (int attempt = 0; attempt < kAttempts; ++attempt)
        {
            D3D12_HEAP_PROPERTIES hp {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = kWidth;
            rd.Height = kHeight;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            // Round-robin through every format the route would carry, because that is what the
            // table above does and the fault was first seen there, not on one format alone.
            const Case &churnCase = kCases[attempt % std::size(kCases)];
            rd.Format = churnCase.dxgi;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            ComPtr<ID3D12Resource> churn;
            HANDLE churnHandle = nullptr;
            if (FAILED(dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&churn))) ||
                FAILED(dev12->CreateSharedHandle(churn.Get(), nullptr, GENERIC_ALL, nullptr,
                                                 &churnHandle)))
                break;
            char why[96] {};
            GLuint churnMemory = 0;
            const GLuint churnTexture =
                ImportTexture(churnHandle, dev12->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes,
                              GL_HANDLE_TYPE_D3D12_RESOURCE_EXT_, churnCase, true, churnMemory,
                              why, sizeof(why));
            if (churnTexture == 0)
            {
                CloseHandle(churnHandle);
                break;
            }
            OpArgs args { churnTexture, got.data() };
            Attempt used = TryOp(OpClear, args);
            gl.deleteTextures(1, &churnTexture);
            gl.deleteMemoryObjects(1, &churnMemory);
            gl.Drain();
            CloseHandle(churnHandle);

            // The fault does not happen in CloseHandle. It happens in whatever GL call comes
            // next, which here is making an ordinary texture -- the same innocent call that made
            // this look like a driver with no working interop at all.
            GLuint after = 0;
            const Attempt madeAfter = TryMakeControlTexture(after);
            if (used == Attempt::faulted || madeAfter == Attempt::faulted)
            {
                contextPoisoned = true;
                std::printf("  FAULTED on attempt %d of %d (%s), in %s.\n", attempt + 1,
                            kAttempts, churnCase.name,
                            used == Attempt::faulted ? "the texture's own use"
                                                     : "an unrelated call afterwards");
                break;
            }
            if (after != 0)
            {
                gl.deleteTextures(1, &after);
                gl.Drain();
            }
            ++survived;
        }
        if (!contextPoisoned)
            std::printf("  survived %d of %d import-and-close cycles. This driver may have been\n"
                        "  fixed since the note above was written -- it was a race, so the route\n"
                        "  should still keep its handles rather than trust this.\n",
                        survived, kAttempts);
        else
            std::printf("  Keep the handle instead. One per crossing texture, never closed.\n");
        Line();
    }

    // ---- verdict ------------------------------------------------------------------------------
    const bool open = g_failures == 0 && d3d12ResourceWorks && colourFboComplete && aliasIn &&
                      aliasOut && blitIn && blitOut;
    if (open)
    {
        std::printf("VERDICT: the OpenGL route is open on this driver. D3D12 textures import as\n"
                    "         GL textures, the memory really is shared in both directions, the\n"
                    "         imported texture is a complete render target, and a blit out of\n"
                    "         the default framebuffer lands in D3D12 memory.\n"
                    "         Synchronisation: %s.\n",
                    semaphoreUsable
                        ? "the imported fences carry it on the GPU"
                        : "the CPU stall, as on D3D11 and Vulkan -- the imported fence is not "
                          "usable here");
        std::printf("         The rule this driver adds, measured rather than assumed: closing an\n"
                    "         imported texture's shared handle faulted 3 runs in 8 in the A/B that\n"
                    "         found it, and 0 in 8 with the same code keeping the handle. Keep one\n"
                    "         handle per crossing texture open for the life of the process, even\n"
                    "         though the section above may have survived its own cycles -- it is a\n"
                    "         race, and a race that passes today is not a rule.\n");
        if (opaqueTrap)
            std::printf("         One trap, worth carrying into the route: importing the same\n"
                        "         handle as OPAQUE_WIN32 instead of D3D12_RESOURCE is accepted\n"
                        "         and then destroys the context. Name the handle type exactly.\n");
        std::printf("\nWhat is still unknown, and needs a game rather than this probe: what\n"
                    "ReShade hands an add-on for the OpenGL back buffer, whether any depth or\n"
                    "motion guide reaches it, and whether the route can leave the context's\n"
                    "state exactly as it found it. Run the `probe` add-on under an OpenGL host\n"
                    "for the first two.\n");
    }
    else
    {
        std::printf("VERDICT: BLOCKED, or not proved.\n");
        if (!d3d12ResourceWorks)
            std::printf("         D3D12_RESOURCE handles do not import.\n");
        if (!colourFboComplete)
            std::printf("         The imported texture is not a usable render target.\n");
        if (contextPoisoned)
            std::printf("         The driver faulted on an imported texture. Everything up to\n"
                        "         that row is real; nothing after it was attempted.\n");
        if (!aliasIn && !contextPoisoned)
            std::printf("         What D3D12 wrote did not come back out through OpenGL.\n");
        if (!aliasOut && !contextPoisoned)
            std::printf("         What OpenGL wrote did not reach the D3D12 side.\n");
        if ((!blitIn || !blitOut) && !contextPoisoned)
            std::printf("         The blit between the default framebuffer and the shared\n"
                        "         texture did not carry the frame.\n");
        if (g_failures != 0)
            std::printf("         %d check(s) failed above.\n", g_failures);
        std::printf("         Read the control row and the OPAQUE_WIN32 column before concluding\n"
                    "         anything: a control that fails the same way means this probe is\n"
                    "         wrong, and a different handle type being importable means a\n"
                    "         different transport rather than no transport.\n");
    }

    // Teardown, unless the semaphore wait faulted: after an access violation inside the ICD,
    // every further GL call is a second crash waiting to happen, and there is nothing left to
    // learn from one. Process exit releases all of it anyway.
    if (!contextPoisoned)
    {
        if (colourTexture != 0)
        {
            gl.deleteTextures(1, &colourTexture);
            gl.deleteMemoryObjects(1, &colourMemory);
        }
        if (semToGl != 0 && gl.deleteSemaphores != nullptr)
        {
            const GLuint pair[2] { semToGl, semToD3D };
            gl.deleteSemaphores(2, pair);
        }
        gl.Drain();
        gl.makeCurrent(nullptr, nullptr);
        if (ctx.rc != nullptr)
            gl.deleteContext(ctx.rc);
        if (ctx.window != nullptr)
        {
            ReleaseDC(ctx.window, ctx.dc);
            DestroyWindow(ctx.window);
        }
    }
    if (cpuEvent != nullptr)
        CloseHandle(cpuEvent);
    if (hFenceToGl != nullptr)
        CloseHandle(hFenceToGl);
    if (hFenceToD3D != nullptr)
        CloseHandle(hFenceToD3D);
    return open ? 0 : 1;
}
