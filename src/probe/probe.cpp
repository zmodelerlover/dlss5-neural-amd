// ReShade add-on: per-frame render target inventory, plus content dumps on chosen frames.

#include <reshade.hpp>

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace reshade::api;

namespace
{
constexpr uint64_t kDumpEvery = 600;
constexpr uint64_t kDumpFirst = 3;

constexpr uint64_t kContentFrames[] = { 600, 1200, 1800 };

std::mutex g_mutex;
FILE *g_log = nullptr;
uint64_t g_frame = 0;
uint32_t g_swap_width = 0, g_swap_height = 0;

struct Target
{
    resource_desc desc {};
    uint32_t draws = 0;
    bool as_color = false;
    bool as_depth = false;
};

std::map<command_list *, std::vector<resource>> g_bound_color;
std::map<command_list *, resource> g_bound_depth;
std::map<uint64_t, Target> g_frame_targets;

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

const char *FormatName(format value)
{
    switch (value)
    {
    case format::r8g8b8a8_unorm:      return "R8G8B8A8_UNORM";
    case format::r8g8b8a8_unorm_srgb: return "R8G8B8A8_UNORM_SRGB";
    case format::b8g8r8a8_unorm:      return "B8G8R8A8_UNORM";
    case format::r10g10b10a2_unorm:   return "R10G10B10A2_UNORM";
    case format::r11g11b10_float:     return "R11G11B10_FLOAT";
    case format::r16g16_float:        return "R16G16_FLOAT";
    case format::r16g16_snorm:        return "R16G16_SNORM";
    case format::r16g16b16a16_float:  return "R16G16B16A16_FLOAT";
    case format::r32g32_float:        return "R32G32_FLOAT";
    case format::r32_float:           return "R32_FLOAT";
    case format::r32_typeless:        return "R32_TYPELESS";
    case format::r16_typeless:        return "R16_TYPELESS";
    case format::r8_unorm:            return "R8_UNORM";
    case format::d16_unorm:           return "D16_UNORM";
    case format::d24_unorm_s8_uint:   return "D24_UNORM_S8_UINT";
    case format::d32_float:           return "D32_FLOAT";
    case format::d32_float_s8_uint:   return "D32_FLOAT_S8X24_UINT";
    case format::r24_g8_typeless:     return "R24G8_TYPELESS";
    case format::r32_g8_typeless:     return "R32G8X24_TYPELESS";
    default:                          return "?";
    }
}

bool IsDepthFormat(format value)
{
    switch (value)
    {
    case format::d16_unorm:
    case format::d24_unorm_s8_uint:
    case format::d32_float:
    case format::d32_float_s8_uint:
    case format::r24_g8_typeless:
    case format::r32_g8_typeless:
    case format::r32_typeless:
    case format::r16_typeless:
        return true;
    default:
        return false;
    }
}

bool IsMotionFormat(format value)
{
    return value == format::r16g16_float || value == format::r16g16_snorm || value == format::r32g32_float;
}

bool IsColorFormat(format value)
{
    switch (value)
    {
    case format::r11g11b10_float:
    case format::r16g16b16a16_float:
    case format::r10g10b10a2_unorm:
    case format::r8g8b8a8_unorm:
    case format::r8g8b8a8_unorm_srgb:
    case format::b8g8r8a8_unorm:
        return true;
    default:
        return false;
    }
}

const char *Guess(const Target &t)
{
    const auto &tex = t.desc.texture;
    const bool full_res = (g_swap_width != 0 && tex.width == g_swap_width && tex.height == g_swap_height);

    if (t.as_depth || IsDepthFormat(tex.format))
        return "  <-- DEPTH";
    if (IsMotionFormat(tex.format))
        return "  <-- MOTION?";
    if (IsColorFormat(tex.format) && !full_res)
        return "  <-- COLOUR? (render resolution)";
    if (full_res)
        return "  (screen resolution)";
    return "";
}

float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1Fu;
    uint32_t mantissa = h & 0x03FFu;
    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            const uint32_t bits = sign;
            float out;
            std::memcpy(&out, &bits, 4);
            return out;
        }
        exponent = 1;
        while ((mantissa & 0x0400u) == 0)
        {
            mantissa <<= 1;
            exponent--;
        }
        mantissa &= 0x03FFu;
    }
    else if (exponent == 31)
    {
        const uint32_t bits = sign | 0x7F800000u | (mantissa << 13);
        float out;
        std::memcpy(&out, &bits, 4);
        return out;
    }
    const uint32_t bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

uint8_t ToByte(float v)
{
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

void TexelToRgb(const uint8_t *src, format fmt, uint8_t out[3])
{
    switch (fmt)
    {
    case format::r16g16_float:
    {
        uint16_t xy[2];
        std::memcpy(xy, src, 4);
        const float x = HalfToFloat(xy[0]), y = HalfToFloat(xy[1]);
        out[0] = ToByte(0.5f + x * 8.0f);
        out[1] = ToByte(0.5f + y * 8.0f);
        out[2] = 128;
        break;
    }
    case format::r32g32_float:
    {
        float xy[2];
        std::memcpy(xy, src, 8);
        out[0] = ToByte(0.5f + xy[0] * 8.0f);
        out[1] = ToByte(0.5f + xy[1] * 8.0f);
        out[2] = 128;
        break;
    }
    case format::r16g16b16a16_float:
    {
        uint16_t rgba[4];
        std::memcpy(rgba, src, 8);
        for (int i = 0; i < 3; ++i)
            out[i] = ToByte(powf(std::max(0.0f, HalfToFloat(rgba[i])), 1.0f / 2.2f));
        break;
    }
    case format::r32_float:
    case format::r32_typeless:
    case format::d32_float:
    case format::d32_float_s8_uint:
    case format::r32_g8_typeless:
    {
        float d;
        std::memcpy(&d, src, 4);
        const uint8_t g = ToByte(powf(std::clamp(d, 0.0f, 1.0f), 0.25f));
        out[0] = out[1] = out[2] = g;
        break;
    }
    case format::r11g11b10_float:
    {
        uint32_t packed;
        std::memcpy(&packed, src, 4);
        const uint32_t r = packed & 0x7FFu, g = (packed >> 11) & 0x7FFu, b = (packed >> 22) & 0x3FFu;
        const float fr = HalfToFloat(static_cast<uint16_t>((r & 0x7C0u) << 4 | (r & 0x3Fu) << 4));
        const float fg = HalfToFloat(static_cast<uint16_t>((g & 0x7C0u) << 4 | (g & 0x3Fu) << 4));
        const float fb = HalfToFloat(static_cast<uint16_t>((b & 0x3E0u) << 5 | (b & 0x1Fu) << 5));
        out[0] = ToByte(powf(std::max(0.0f, fr), 1.0f / 2.2f));
        out[1] = ToByte(powf(std::max(0.0f, fg), 1.0f / 2.2f));
        out[2] = ToByte(powf(std::max(0.0f, fb), 1.0f / 2.2f));
        break;
    }
    default:
        out[0] = src[0];
        out[1] = src[1];
        out[2] = src[2];
        break;
    }
}

std::string GamePath(const char *name)
{
    char path[MAX_PATH] {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char *slash = strrchr(path, '\\'))
        *(slash + 1) = '\0';
    return std::string(path) + name;
}

void DumpResource(device *dev, command_queue *queue, resource res, const resource_desc &desc, const char *role)
{
    const uint32_t texel = format_row_pitch(desc.texture.format, 1);
    if (texel == 0 || texel > 16)
        return;

    resource_desc staging_desc(desc.texture.width, desc.texture.height, 1, 1, desc.texture.format, 1,
                               memory_heap::readback, resource_usage::copy_dest);
    resource staging = {};
    if (!dev->create_resource(staging_desc, nullptr, resource_usage::copy_dest, &staging))
    {
        Log("  dump %s: could not create the readback texture", role);
        return;
    }

    command_list *cmd = queue->get_immediate_command_list();
    cmd->barrier(res, resource_usage::undefined, resource_usage::copy_source);
    cmd->copy_texture_region(res, 0, nullptr, staging, 0, nullptr);
    cmd->barrier(res, resource_usage::copy_source, resource_usage::undefined);
    queue->flush_immediate_command_list();
    queue->wait_idle();

    subresource_data mapped {};
    if (dev->map_texture_region(staging, 0, nullptr, map_access::read_only, &mapped) && mapped.data != nullptr)
    {
        const uint8_t *base = static_cast<const uint8_t *>(mapped.data);
        const uint32_t pitch = mapped.row_pitch;

        char name[64];
        snprintf(name, sizeof(name), "dlss5-%s-f%llu.raw", role, g_frame);
        if (FILE *f = fopen(GamePath(name).c_str(), "wb"))
        {
            for (uint32_t y = 0; y < desc.texture.height; ++y)
                fwrite(base + static_cast<size_t>(y) * pitch, 1, static_cast<size_t>(desc.texture.width) * texel, f);
            fclose(f);
        }
        snprintf(name, sizeof(name), "dlss5-%s-f%llu.ppm", role, g_frame);
        if (FILE *f = fopen(GamePath(name).c_str(), "wb"))
        {
            fprintf(f, "P6\n%u %u\n255\n", desc.texture.width, desc.texture.height);
            std::vector<uint8_t> row(static_cast<size_t>(desc.texture.width) * 3);
            for (uint32_t y = 0; y < desc.texture.height; ++y)
            {
                const uint8_t *line = base + static_cast<size_t>(y) * pitch;
                for (uint32_t x = 0; x < desc.texture.width; ++x)
                    TexelToRgb(line + static_cast<size_t>(x) * texel, desc.texture.format, &row[x * 3]);
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
        }
        dev->unmap_texture_region(staging, 0);
        Log("  dump %s: %ux%u %s -> dlss5-%s-f%llu.ppm", role, desc.texture.width, desc.texture.height,
            FormatName(desc.texture.format), role, g_frame);
    }
    else
    {
        Log("  dump %s: map_texture_region failed", role);
    }
    dev->destroy_resource(staging);
}

void DumpCandidates(device *dev, command_queue *queue)
{
    struct Pick
    {
        uint64_t handle = 0;
        uint32_t draws = 0;
        resource_desc desc {};
    } motion, depth, colour;

    for (const auto &[handle, t] : g_frame_targets)
    {
        if (t.desc.type != resource_type::texture_2d)
            continue;
        const auto fmt = t.desc.texture.format;
        Pick *slot = nullptr;
        if (IsMotionFormat(fmt))
            slot = &motion;
        else if (t.as_depth || IsDepthFormat(fmt))
            slot = &depth;
        else if (IsColorFormat(fmt))
            slot = &colour;
        if (slot != nullptr && t.draws > slot->draws)
            *slot = { handle, t.draws, t.desc };
    }

    Log("");
    Log("===== content dump at frame %llu =====", g_frame);
    const std::pair<Pick *, const char *> picks[] = { { &motion, "motion" },
                                                      { &depth, "depth" },
                                                      { &colour, "colour" } };
    for (const auto &[pick, role] : picks)
    {
        if (pick->handle == 0)
        {
            Log("  dump %s: no candidate this frame", role);
            continue;
        }
        DumpResource(dev, queue, resource { pick->handle }, pick->desc, role);
    }
}

void RecordTarget(device *dev, resource res, bool as_depth)
{
    if (res.handle == 0)
        return;
    auto &entry = g_frame_targets[res.handle];
    if (entry.draws == 0)
        entry.desc = dev->get_resource_desc(res);
    entry.draws++;
    if (as_depth)
        entry.as_depth = true;
    else
        entry.as_color = true;
}

void OnBindRenderTargets(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
    device *dev = cmd->get_device();
    std::lock_guard<std::mutex> lock(g_mutex);

    auto &color = g_bound_color[cmd];
    color.clear();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (rtvs[i].handle == 0)
            continue;
        color.push_back(dev->get_resource_from_view(rtvs[i]));
    }
    g_bound_depth[cmd] = (dsv.handle != 0) ? dev->get_resource_from_view(dsv) : resource { 0 };
}

bool OnDraw(command_list *cmd)
{
    device *dev = cmd->get_device();
    std::lock_guard<std::mutex> lock(g_mutex);

    auto color = g_bound_color.find(cmd);
    if (color != g_bound_color.end())
        for (resource res : color->second)
            RecordTarget(dev, res, false);

    auto depth = g_bound_depth.find(cmd);
    if (depth != g_bound_depth.end())
        RecordTarget(dev, depth->second, true);

    return false;
}

bool OnDrawSimple(command_list *cmd, uint32_t, uint32_t, uint32_t, uint32_t) { return OnDraw(cmd); }
bool OnDrawIndexed(command_list *cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { return OnDraw(cmd); }

void OnInitSwapchain(swapchain *sc, bool)
{
    device *dev = sc->get_device();
    const resource back = sc->get_current_back_buffer();
    if (back.handle == 0)
        return;
    const resource_desc desc = dev->get_resource_desc(back);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_swap_width = desc.texture.width;
    g_swap_height = desc.texture.height;
    Log("swapchain %ux%u format %s (%u)", g_swap_width, g_swap_height, FormatName(desc.texture.format),
        static_cast<uint32_t>(desc.texture.format));
}

void DumpInventory()
{
    std::vector<std::pair<uint64_t, Target>> sorted(g_frame_targets.begin(), g_frame_targets.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second.draws > b.second.draws; });

    Log("");
    Log("===== frame %llu -- %zu distinct render targets =====", g_frame, sorted.size());
    Log("%-18s %-10s %-24s %7s %6s %s", "handle", "size", "format", "draws", "role", "guess");

    for (const auto &[handle, t] : sorted)
    {
        if (t.desc.type != resource_type::texture_2d)
            continue;
        const auto &tex = t.desc.texture;
        char size[32];
        snprintf(size, sizeof(size), "%ux%u", tex.width, tex.height);
        const char *role = t.as_depth ? (t.as_color ? "ambos" : "depth") : "color";
        Log("%-18llx %-10s %-20s(%2u) %7u %6s %s", handle, size, FormatName(tex.format),
            static_cast<uint32_t>(tex.format), t.draws, role, Guess(t));
    }
}

void OnPresent(command_queue *queue, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_frame++;

    if (g_frame <= kDumpFirst || (g_frame % kDumpEvery) == 0)
        DumpInventory();

    for (uint64_t at : kContentFrames)
    {
        if (g_frame == at && queue != nullptr)
        {
            DumpCandidates(queue->get_device(), queue);
            break;
        }
    }

    g_frame_targets.clear();
}

void OpenLog()
{
    g_log = fopen(GamePath("dlss5-probe.log").c_str(), "w");
    Log("dlss5 probe -- per-frame inventory of render targets");
    Log("inventory for the first %llu frames, then every %llu", kDumpFirst, kDumpEvery);
    Log("conteudo gravado nos frames 600, 1200 e 1800 (.ppm e .raw ao lado do executavel)");
}
}

extern "C" __declspec(dllexport) const char *NAME = "dlss5 probe";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Per-frame render target inventory and content dump: finds and proves colour, depth and motion "
    "em jogos sem contrato de upscaler.";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module))
            return FALSE;
        OpenLog();
        reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
        reshade::register_event<reshade::addon_event::draw>(OnDrawSimple);
        reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(module);
        if (g_log != nullptr)
            fclose(g_log);
        break;
    }
    return TRUE;
}
