// The OpenGL route's hand-loaded GL entry points. Included by gl_route.inc, in namespace glroute.

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
