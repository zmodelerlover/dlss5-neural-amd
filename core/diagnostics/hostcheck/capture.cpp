// ReShade add-on: saves the presented back buffer once, for hostcheck.
//
// Loaded after amd-nr.addon64, so its present has already composed onto the back buffer by the
// time ReShade finishes the frame. reshade_present comes after ReShade's own overlay, which is why
// the capture waits until the startup banner is long gone. capture_screenshot is ReShade's own
// readback and serves D3D11, D3D12 and Vulkan alike; OpenGL reads the back buffer itself.
//
// HOSTCHECK_OUT names the file (raw RGBA8 rows, top to bottom); a .txt beside it gets the size.
#include <reshade.hpp>

#include <windows.h>

#include <GL/gl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

ULONGLONG g_start = 0;
unsigned g_frames = 0;
bool g_done = false;

void OnPresent(reshade::api::effect_runtime *runtime)
{
    if (g_done)
        return;
    if (g_start == 0)
        g_start = GetTickCount64();
    // Both a time and a count: a slow host must still clear the banner, a fast one must still
    // have run the network for a while. Three inline passes above scale 1 can take seconds a
    // frame, so after 45 s a handful of frames is enough.
    const ULONGLONG ran = GetTickCount64() - g_start;
    const bool enough = ++g_frames >= 120 || (ran >= 45000 && g_frames >= 5);
    if (!enough || ran < 15000)
        return;
    g_done = true;
    const char *out = std::getenv("HOSTCHECK_OUT");
    if (out == nullptr)
        return;
    uint32_t w = 0, h = 0;
    runtime->get_screenshot_width_and_height(&w, &h);
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    bool ok = false;
    if (runtime->get_device()->get_api() == reshade::api::device_api::opengl) {
        // ReShade's own readback refuses the default framebuffer hostcheck's pixel format gets.
        // This still runs before the real SwapBuffers, with the context current, so the back
        // buffer holds the composed frame; GL's rows come bottom first. Resolved by hand, as
        // opengl_import_check asks of everything under core/.
        HMODULE gl = GetModuleHandleW(L"opengl32.dll");
        auto fn = [gl](const char *name) { return GetProcAddress(gl, name); };
        auto readBuffer = reinterpret_cast<void(WINAPI *)(GLenum)>(fn("glReadBuffer"));
        auto pixelStore = reinterpret_cast<void(WINAPI *)(GLenum, GLint)>(fn("glPixelStorei"));
        auto readPixels = reinterpret_cast<void(WINAPI *)(GLint, GLint, GLsizei, GLsizei, GLenum,
                                                          GLenum, void *)>(fn("glReadPixels"));
        auto getError = reinterpret_cast<GLenum(WINAPI *)()>(fn("glGetError"));
        if (gl != nullptr && readPixels != nullptr) {
            readBuffer(GL_BACK);
            pixelStore(GL_PACK_ALIGNMENT, 1);
            std::vector<uint8_t> up(px.size());
            readPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, up.data());
            const size_t row = static_cast<size_t>(w) * 4;
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(&px[y * row], &up[(h - 1 - y) * row], row);
            ok = w != 0 && getError() == GL_NO_ERROR;
        }
    } else {
        ok = w != 0 && runtime->capture_screenshot(px.data());
    }
    if (FILE *f = std::fopen(out, "wb")) {
        std::fwrite(px.data(), 1, ok ? px.size() : 0, f);
        std::fclose(f);
    }
    if (FILE *f = std::fopen((std::string(out) + ".txt").c_str(), "w")) {
        std::fprintf(f, "width=%u\nheight=%u\nok=%d\nframes=%u\n", w, h, ok ? 1 : 0, g_frames);
        std::fclose(f);
    }
}

} // namespace

extern "C" __declspec(dllexport) const char *NAME = "hostcheck capture";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        if (!reshade::register_addon(module))
            return FALSE;
        reshade::register_event<reshade::addon_event::reshade_present>(OnPresent);
    } else if (reason == DLL_PROCESS_DETACH) {
        reshade::unregister_addon(module);
    }
    return TRUE;
}
