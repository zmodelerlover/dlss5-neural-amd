// hostcheck's OpenGL host: a legacy WGL context drawing the frame with glDrawPixels.
//
// ReShade is opengl32.dll beside the exe. LoadLibrary finds that copy first, and every entry point
// comes through GetProcAddress, as the add-on's own OpenGL route does it: opengl_import_check
// holds every source under core/ to that, and a diagnostic is no exception.
#include <GL/gl.h>

#pragma comment(lib, "gdi32.lib")

int hostcheck::RunOpenGL(const Image &img)
{
    HMODULE gl = LoadLibraryW(L"opengl32.dll");
    auto fn = [gl](const char *name) { return GetProcAddress(gl, name); };
    auto createContext = reinterpret_cast<HGLRC(WINAPI *)(HDC)>(fn("wglCreateContext"));
    auto makeCurrent = reinterpret_cast<BOOL(WINAPI *)(HDC, HGLRC)>(fn("wglMakeCurrent"));
    auto viewport = reinterpret_cast<void(WINAPI *)(GLint, GLint, GLsizei, GLsizei)>(fn("glViewport"));
    auto pixelStore = reinterpret_cast<void(WINAPI *)(GLenum, GLint)>(fn("glPixelStorei"));
    auto rasterPos = reinterpret_cast<void(WINAPI *)(GLfloat, GLfloat)>(fn("glRasterPos2f"));
    auto drawPixels = reinterpret_cast<void(WINAPI *)(GLsizei, GLsizei, GLenum, GLenum, const void *)>(
        fn("glDrawPixels"));
    if (gl == nullptr || createContext == nullptr || drawPixels == nullptr) {
        std::fprintf(stderr, "opengl32.dll\n");
        return 3;
    }

    HWND wnd = MakeWindow(img.w, img.h, true);
    HDC dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    if (!SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd)) {
        std::fprintf(stderr, "SetPixelFormat failed\n");
        return 3;
    }
    HGLRC rc = createContext(dc);
    if (rc == nullptr || !makeCurrent(dc, rc)) {
        std::fprintf(stderr, "wglCreateContext failed\n");
        return 3;
    }
    // GL's rows run bottom to top.
    std::vector<unsigned char> flipped(img.rgba.size());
    const size_t row = static_cast<size_t>(img.w) * 4;
    for (UINT y = 0; y < img.h; ++y)
        std::memcpy(&flipped[y * row], &img.rgba[(img.h - 1 - y) * row], row);
    // Paced to the display like the other hosts, so the captured frame lands at the same time too.
    auto getProc = reinterpret_cast<PROC(WINAPI *)(LPCSTR)>(fn("wglGetProcAddress"));
    if (auto swapInterval = reinterpret_cast<BOOL(WINAPI *)(int)>(getProc("wglSwapIntervalEXT")))
        swapInterval(1);
    viewport(0, 0, img.w, img.h);
    pixelStore(GL_UNPACK_ALIGNMENT, 1);
    return Loop([&] {
        rasterPos(-1.0f, -1.0f);
        drawPixels(img.w, img.h, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
        return SwapBuffers(dc) != FALSE;
    });
}
