// glinfo -- what does ReShade actually hand an add-on under an OpenGL host?
//
// glprobe answered the driver's half of the OpenGL question offline: D3D12 textures import, the
// memory aliases both ways, the fences work. This answers the other half, and it cannot be
// answered without a game: what ReShade puts in front of the add-on once it is inside one.
//
// Three questions, in the order the route needs them:
//
//   1. Does `present` arrive at all, and on what? In D3D11 and D3D12 the back buffer is a texture
//      the route can copy. In OpenGL there may be no texture: the back buffer is the default
//      framebuffer, and ReShade encodes a GL object in the resource handle -- the top 24 bits are
//      the object type (GL_FRAMEBUFFER_DEFAULT, GL_TEXTURE_2D, GL_RENDERBUFFER) and the low 32
//      are the name (reshade_api_resource.hpp:385). Which of those comes back decides whether the
//      route's way in is a framebuffer blit or a texture copy. That is the single most important
//      line in this log.
//   2. Do render-target and depth binds reach an add-on here? The existing `probe` says "2
//      distinct render targets" and then prints nothing, because it skips everything that is not
//      resource_type::texture_2d and in OpenGL the bound targets are not. So this one prints what
//      it sees whatever the type, which is the only way to find out whether guides are possible.
//   3. Is there an immediate command list, and is the queue a graphics queue? The Vulkan route
//      records into ReShade's immediate command list (vk_route.inc). If OpenGL has none, the GL
//      route issues its own calls straight into the current context, which is a different design
//      with a different set of state to put back.
//
// It also answers a question nobody asked. Under SDL -- which is what Luanti and a great many
// OpenGL games use -- ReShade loads and unloads its add-ons several times during start-up, once
// per GL context created and destroyed while the window is being set up. An add-on that opens its
// log with "w" in DllMain therefore throws away everything it wrote before the last context, and
// anything expensive it builds there is built and thrown away that many times. This log appends,
// and stamps each load, so the reloads are visible rather than silently destructive. The real
// add-on will have to survive the same treatment: neural.cpp's DllMain brings up HIP and the
// engine, and doing that six times during a game's start-up is not free.
//
// Build:  .\build.ps1 -Target glinfo
// Use:    copy amd-nr-glinfo.addon64 next to the game's exe, beside ReShade's opengl32.dll,
//         play for a minute, close the game, read amd-nr-glinfo.log next to the exe.

#include <reshade.hpp>

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

using namespace reshade::api;

namespace
{

FILE *g_log = nullptr;
std::mutex g_mutex;
uint64_t g_frame = 0;
uint64_t g_bindEvents = 0, g_depthBinds = 0, g_clears = 0;
std::set<uint64_t> g_seenColour, g_seenDepth;
uint64_t g_lastBackBuffer = 0;
bool g_loggedPresent = false;

std::string GamePath(const char *name)
{
    char path[MAX_PATH] {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string full(path);
    const size_t cut = full.find_last_of("\\/");
    return (cut == std::string::npos ? std::string() : full.substr(0, cut + 1)) + name;
}

void Log(const char *fmt, ...)
{
    if (g_log == nullptr)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

const char *ApiName(device_api api)
{
    switch (api)
    {
    case device_api::d3d9: return "D3D9";
    case device_api::d3d10: return "D3D10";
    case device_api::d3d11: return "D3D11";
    case device_api::d3d12: return "D3D12";
    case device_api::opengl: return "OpenGL";
    case device_api::vulkan: return "Vulkan";
    default: return "?";
    }
}

// The top 24 bits of an OpenGL resource handle are the GL object type. Naming them is the whole
// point of this add-on: "0x8218" means nothing, "GL_FRAMEBUFFER_DEFAULT" decides the design.
const char *GlObjectName(uint64_t handle)
{
    switch (static_cast<unsigned>(handle >> 40))
    {
    case 0x0DE1: return "GL_TEXTURE_2D";
    case 0x8218: return "GL_FRAMEBUFFER_DEFAULT";
    case 0x8D41: return "GL_RENDERBUFFER";
    case 0x8D40: return "GL_FRAMEBUFFER";
    case 0x9100: return "GL_TEXTURE_2D_MULTISAMPLE";
    case 0x84F5: return "GL_TEXTURE_RECTANGLE";
    case 0x8C1A: return "GL_TEXTURE_2D_ARRAY";
    case 0x8513: return "GL_TEXTURE_CUBE_MAP";
    case 0x82E0: return "GL_BUFFER";
    case 0x8074: return "GL_VERTEX_ARRAY";
    case 0: return "(no type -- not an OpenGL handle)";
    default: return "(unknown GL object type)";
    }
}

const char *TypeName(resource_type type)
{
    switch (type)
    {
    case resource_type::buffer: return "buffer";
    case resource_type::texture_1d: return "texture_1d";
    case resource_type::texture_2d: return "texture_2d";
    case resource_type::texture_3d: return "texture_3d";
    case resource_type::surface: return "surface";
    default: return "unknown";
    }
}

// One line that says everything about a resource: its GL identity, what ReShade thinks it is, and
// the dimensions and format the route would have to match on the D3D12 side.
void Describe(const char *what, device *dev, resource res)
{
    if (res.handle == 0)
    {
        Log("  %-14s (null)", what);
        return;
    }
    const resource_desc desc = dev->get_resource_desc(res);
    Log("  %-14s handle %016llx  %-26s name %u", what,
        static_cast<unsigned long long>(res.handle), GlObjectName(res.handle),
        static_cast<unsigned>(res.handle & 0xFFFFFFFFull));
    if (desc.type == resource_type::buffer)
        Log("                 %s, %llu bytes", TypeName(desc.type),
            static_cast<unsigned long long>(desc.buffer.size));
    else
        Log("                 %s %ux%u, format %u, %u sample(s), levels %u, usage 0x%x",
            TypeName(desc.type), desc.texture.width, desc.texture.height,
            static_cast<unsigned>(desc.texture.format), desc.texture.samples,
            desc.texture.levels, static_cast<unsigned>(desc.usage));
}

void OnInitSwapchain(swapchain *sc, bool resize)
{
    device *dev = sc->get_device();
    std::lock_guard<std::mutex> lock(g_mutex);

    Log("");
    Log("=== swapchain %s ===", resize ? "resized" : "created");
    Log("  api            %s (0x%x)", ApiName(dev->get_api()),
        static_cast<unsigned>(dev->get_api()));
    Log("  hwnd           %p", sc->get_hwnd());
    Log("  back buffers   %u", sc->get_back_buffer_count());
    for (uint32_t i = 0; i < sc->get_back_buffer_count(); ++i)
    {
        char label[32];
        std::snprintf(label, sizeof(label), "back buffer %u", i);
        Describe(label, dev, sc->get_back_buffer(i));
    }
    Log("  Read the GL object type above. GL_FRAMEBUFFER_DEFAULT means the route's way in is");
    Log("  glBlitFramebuffer out of FBO 0; a texture type means it can be copied directly.");
}

void OnDestroySwapchain(swapchain *, bool resize)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Log("=== swapchain %s ===", resize ? "about to resize" : "destroyed");
}

// Everything the host binds, whatever type it is. The existing probe filters to texture_2d and so
// reports nothing here; the filter is the bug, not the absence of data.
void OnBindTargets(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
    device *dev = cmd->get_device();
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_bindEvents;
    if (dsv.handle != 0)
        ++g_depthBinds;

    const bool verbose = g_bindEvents <= 8;
    if (verbose)
        Log("");
    if (verbose)
        Log("--- bind #%llu on frame %llu: %u colour target(s)%s ---",
            static_cast<unsigned long long>(g_bindEvents),
            static_cast<unsigned long long>(g_frame), count,
            dsv.handle != 0 ? ", with depth" : ", no depth");

    for (uint32_t i = 0; i < count; ++i)
    {
        const resource res = rtvs[i].handle != 0 ? dev->get_resource_from_view(rtvs[i]) : resource {};
        if (res.handle != 0)
            g_seenColour.insert(res.handle);
        if (verbose)
        {
            char label[32];
            std::snprintf(label, sizeof(label), "colour %u", i);
            Describe(label, dev, res);
        }
    }
    if (dsv.handle != 0)
    {
        const resource res = dev->get_resource_from_view(dsv);
        if (res.handle != 0)
            g_seenDepth.insert(res.handle);
        if (verbose)
            Describe("depth", dev, res);
    }
}

bool OnClearDepth(command_list *, resource_view, const float *, const uint8_t *, uint32_t,
                  const rect *)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_clears;
    return false;
}

void OnPresent(command_queue *queue, swapchain *sc, const rect *, const rect *, uint32_t,
               const rect *)
{
    device *dev = sc != nullptr ? sc->get_device() : nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_frame;

    if (!g_loggedPresent && dev != nullptr)
    {
        g_loggedPresent = true;
        Log("");
        Log("=== first present ===");
        Log("  api            %s", ApiName(dev->get_api()));
        Log("  queue type     0x%x (graphics bit %s)", static_cast<unsigned>(queue->get_type()),
            (queue->get_type() & command_queue_type::graphics) != static_cast<command_queue_type>(0)
                ? "set"
                : "NOT SET");
        command_list *immediate = queue->get_immediate_command_list();
        Log("  immediate list %s", immediate != nullptr ? "present" : "none");
        if (immediate != nullptr)
            Log("                 native %p -- in OpenGL there is no command buffer behind this,",
                immediate->get_native());
        Log("  Describe() of the current back buffer follows. If it says GL_FRAMEBUFFER_DEFAULT,");
        Log("  the route cannot copy it as a texture and must blit out of FBO 0.");
        Describe("back buffer", dev, sc->get_current_back_buffer());
    }

    // The back buffer identity is logged again whenever it changes, because a route that caches
    // it has to know how often that cache goes stale.
    if (dev != nullptr)
    {
        const resource back = sc->get_current_back_buffer();
        if (back.handle != g_lastBackBuffer)
        {
            g_lastBackBuffer = back.handle;
            Log("  frame %llu: back buffer handle is now %016llx (%s)",
                static_cast<unsigned long long>(g_frame),
                static_cast<unsigned long long>(back.handle), GlObjectName(back.handle));
        }
    }

    // A heartbeat, so a log from a session that never got past the menu is distinguishable from
    // one where the events simply stopped arriving.
    if (g_frame % 300 == 0)
        Log("frame %llu: %llu bind events (%llu with depth), %llu depth clears, %zu distinct "
            "colour targets, %zu distinct depth targets",
            static_cast<unsigned long long>(g_frame),
            static_cast<unsigned long long>(g_bindEvents),
            static_cast<unsigned long long>(g_depthBinds),
            static_cast<unsigned long long>(g_clears), g_seenColour.size(), g_seenDepth.size());
}

}  // namespace

extern "C" __declspec(dllexport) const char *NAME = "AMD NR glinfo";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Reports what ReShade hands an add-on under an OpenGL host: the back buffer's GL object type, "
    "whether render-target and depth binds arrive, and whether there is an immediate command list.";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module))
            return FALSE;
        // Appended, not truncated: under SDL this add-on is loaded and unloaded several times
        // while the window is being created, and "w" would leave only the last load's lines.
        if (g_log == nullptr)
            g_log = fopen(GamePath("amd-nr-glinfo.log").c_str(), "a");
        {
            SYSTEMTIME now {};
            GetLocalTime(&now);
            Log("");
            Log("######## glinfo loaded %02u:%02u:%02u.%03u into pid %lu ########", now.wHour,
                now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId());
        }
        reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
            OnBindTargets);
        reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(OnClearDepth);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
        break;
    case DLL_PROCESS_DETACH:
        Log("######## glinfo unloaded after %llu frames, %llu bind events ########",
            static_cast<unsigned long long>(g_frame),
            static_cast<unsigned long long>(g_bindEvents));
        reshade::unregister_addon(module);
        if (g_log != nullptr)
        {
            fclose(g_log);
            g_log = nullptr;
        }
        break;
    }
    return TRUE;
}
