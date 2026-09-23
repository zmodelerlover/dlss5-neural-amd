// ReShade add-on: saves the presented back buffer once, for hostcheck.
//
// Loaded after amd-nr, so its present callback runs after the add-on has composed onto the back
// buffer, and before ReShade draws its own overlay -- no banner to wait out. capture_screenshot is
// ReShade's own readback and serves D3D9, D3D11, D3D12 and Vulkan alike; OpenGL reads the back
// buffer itself.
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

using namespace reshade::api;

effect_runtime *g_runtime = nullptr;
ULONGLONG g_start = 0;
unsigned g_frames = 0;
bool g_done = false;

bool ReadGl(uint32_t w, uint32_t h, std::vector<uint8_t> &px)
{
    // ReShade's own readback refuses the default framebuffer hostcheck's pixel format gets. This
    // runs before the real SwapBuffers with the context current, so the back buffer holds the
    // composed frame; GL's rows come bottom first. Resolved by hand, as opengl_import_check asks of
    // everything under core/.
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    auto fn = [gl](const char *name) { return GetProcAddress(gl, name); };
    auto readBuffer = reinterpret_cast<void(WINAPI *)(GLenum)>(fn("glReadBuffer"));
    auto pixelStore = reinterpret_cast<void(WINAPI *)(GLenum, GLint)>(fn("glPixelStorei"));
    auto readPixels = reinterpret_cast<void(WINAPI *)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                                      void *)>(fn("glReadPixels"));
    auto getError = reinterpret_cast<GLenum(WINAPI *)()>(fn("glGetError"));
    if (gl == nullptr || readPixels == nullptr)
        return false;
    readBuffer(GL_BACK);
    pixelStore(GL_PACK_ALIGNMENT, 1);
    std::vector<uint8_t> up(px.size());
    readPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, up.data());
    const size_t row = static_cast<size_t>(w) * 4;
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy(&px[y * row], &up[(h - 1 - y) * row], row);
    return getError() == GL_NO_ERROR;
}

void OnPresent(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
    if (g_done || g_runtime == nullptr)
        return;
    if (g_start == 0)
        g_start = GetTickCount64();
    // One fixed frame, because what the add-on shows depends on which frame it is: two runs that
    // stop on different frames differ as much as two builds would. 600 is ten seconds at 60 Hz, and
    // three passes above scale 1 still get there on D3D11. On D3D12 they take seconds a frame and
    // never do, so after 45 s a handful of frames is enough, and that capture is only as
    // comparable as its frame count.
    const bool late = GetTickCount64() - g_start >= 45000;
    if (!(++g_frames == 600 || (g_frames < 600 && late && g_frames >= 5)))
        return;
    g_done = true;
    const char *out = std::getenv("HOSTCHECK_OUT");
    if (out == nullptr)
        return;
    uint32_t w = 0, h = 0;
    g_runtime->get_screenshot_width_and_height(&w, &h);
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    const bool ok = w != 0 && (g_runtime->get_device()->get_api() == device_api::opengl
                                   ? ReadGl(w, h, px)
                                   : g_runtime->capture_screenshot(px.data()));
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
        reshade::register_event<reshade::addon_event::init_effect_runtime>(
            [](effect_runtime *runtime) { g_runtime = runtime; });
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(
            [](effect_runtime *runtime) {
                if (g_runtime == runtime)
                    g_runtime = nullptr;
            });
        reshade::register_event<reshade::addon_event::present>(OnPresent);
    } else if (reason == DLL_PROCESS_DETACH) {
        reshade::unregister_addon(module);
    }
    return TRUE;
}
