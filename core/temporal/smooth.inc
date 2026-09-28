// The reference's filters around the history, per pass, ported from its shaders: the history guard
// (C4, native_history_guard.hlsl), the history reprojected by the motion (C2+C3,
// native_temporal_coordinates.hlsl + native_temporal_sample.hlsl, the float path its release runs),
// and the output smooth (C6, native_output_smooth.hlsl). Part of the core's unity build.
//
// The runtime warps the history it is handed by itself, so the reprojection here feeds only the
// smooth, which needs the previous output where each pixel was -- the reference blends against its
// own warped history. The guard runs on the history before it is handed over, in place: that
// texture is rewritten with this pass's output straight after anyway.
//
// HistoryGuard: 1 replaces a history pixel darker than 3/255 with the input where the input is
// brighter than 12/255. OutputSmooth: the strength, 0 off, the reference's 0.8; OutputSmoothLimit
// the difference in /255 past which a pixel is left alone, the reference's 10. The smooth runs before
// the history copy, so it feeds the next frame's history too: recursive, as there.

inline constexpr char kWarpShader[] = R"(
Texture2D<float4> hist : register(t0);
Texture2D<float2> mv : register(t1);
RWTexture2D<float4> dst : register(u0);
cbuffer C : register(b0) { uint w; uint h; };
float3 fetch(float2 xy) {
 float2 p = clamp(xy - .5, 0, float2(w - 1, h - 1));
 uint2 lo = uint2(floor(p)), hi = min(lo + 1, uint2(w - 1, h - 1));
 float2 f = p - float2(lo);
 float3 top = lerp(hist[lo].rgb, hist[uint2(hi.x, lo.y)].rgb, f.x);
 float3 bottom = lerp(hist[uint2(lo.x, hi.y)].rgb, hist[hi].rgb, f.x);
 return lerp(top, bottom, f.y);
}
void axis(float p, uint extent, out float3 positions, out float3 weights) {
 float center = floor(p - .5) + .5, t = saturate(p - center), t2 = t * t, t3 = t2 * t;
 float left = t2 - .5 * (t + t3), middle = 1.5 * t3 - 2.5 * t2 + 1, right = .5 * (t3 - t2);
 float other = 1 - left - middle - right, sum = middle + other;
 positions = clamp(float3(center - 1, mad(other, 1.0 / sum, center), center + 2), .5, float(extent) - .5);
 weights = float3(left, sum, right);
}
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
 if (id.x >= w || id.y >= h) return;
 float2 c = float2(id.xy) + .5 + mv[id.xy];
 float3 px, py, wx, wy; axis(c.x, w, px, wx); axis(c.y, h, py, wy);
 float top = wx.y * wy.x, left = wx.x * wy.y, center = wx.y * wy.y, bottom = wx.y * wy.z, right = wx.z * wy.y;
 float3 r = fetch(float2(px.y, py.x)) * top;
 r = mad(fetch(float2(px.x, py.y)), left, r);
 r = mad(fetch(float2(px.y, py.y)), center, r);
 r = mad(fetch(float2(px.y, py.z)), bottom, r);
 r = mad(fetch(float2(px.z, py.y)), right, r);
 dst[id.xy] = float4(r / (left + top + center + bottom + right), 1);
})";

inline constexpr char kGuardShader[] = R"(
Texture2D<float4> input : register(t0);
RWTexture2D<float4> hist : register(u0);
cbuffer C : register(b0) { uint w; uint h; float dark; float bright; };
[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {
 if (p.x >= w || p.y >= h) return;
 float4 v = hist[p.xy]; float3 b = input[p.xy].rgb;
 if (max(max(v.x, v.y), v.z) < dark && max(max(b.x, b.y), b.z) > bright) hist[p.xy] = float4(b, v.w);
})";

// What it writes becomes the next frame's history, so a NaN or inf is never stored: a NaN in either
// side survives the lerp even at weight 0, and would be warped into its neighbours every frame after.
// The network's own pixel, else the warped previous one, else netBase, the chain's input: no
// correction, where 0 would be a darkening kept in the history. The bit compare, not isnan, because
// /O3 is free to fold isnan away.
inline constexpr char kSmoothShader[] = R"(
Texture2D<float4> warped : register(t0);
Texture2D<float4> input : register(t1);
RWTexture2D<float4> dst : register(u0);
cbuffer C : register(b0) { uint w; uint h; float threshold; float strength; };
bool bad(float3 c) { return any((asuint(c) & 0x7fffffff) >= 0x7f800000); }
[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {
 if (p.x >= w || p.y >= h) return;
 float4 o = dst[p.xy]; float3 v = warped[p.xy].rgb;
 float3 a = abs(o.rgb - v); float d = max(max(a.x, a.y), a.z);
 float3 r = lerp(o.rgb, v, strength * saturate(1.0 - d / threshold));
 if (bad(r)) r = !bad(o.rgb) ? o.rgb : !bad(v) ? v : input[p.xy].rgb;
 dst[p.xy] = float4(r, o.a);
})";

struct HistoryFilters
{
    ComPtr<ID3D12PipelineState> warp, guard, smooth;
    ComPtr<ID3D12Resource> warped[State::kMaxPasses];
    bool failed = false;
} &g_filters = *new HistoryFilters;  // never destroyed, as State g is: see neural.cpp

bool FiltersReady()
{
    if (g_filters.failed)
        return false;
    if (g_filters.warp == nullptr &&
        (!CompileShader(kWarpShader, sizeof(kWarpShader), "history warp", g_filters.warp) ||
         !CompileShader(kGuardShader, sizeof(kGuardShader), "history guard", g_filters.guard) ||
         !CompileShader(kSmoothShader, sizeof(kSmoothShader), "output smooth", g_filters.smooth)))
        g_filters.failed = true;
    return !g_filters.failed;
}

// Slots 64 + 12 * slot: the warp's table at +0 (history, motion, u0 at +3), the smooth's at +4, the
// guard's at +8. A pass apiece, because passes that are not submitted one by one share a list.
UINT FilterSlots(UINT slot) { return 64 + 12 * slot; }

// Before pass `slot` records, when it is being handed its history: the guard, then the history
// warped by this frame's motion for the smooth to blend against after the pass.
void PrepareHistory(ID3D12GraphicsCommandList *cmd, UINT slot)
{
    const bool guard = g.settings.historyGuard.load() != 0, smooth = g.settings.outputSmooth.load() > 0.0f;
    if (!(guard || smooth) || !FiltersReady())
        return;
    const UINT w = g.netWidth, h = g.netHeight, base = FilterSlots(slot);
    ID3D12Resource *history = g.history[slot].Get();
    if (guard)
    {
        ViewAt(base + 8, g.netColour.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        ViewAt(base + 11, history, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        const float dark = 3.0f / 255.0f, bright = 12.0f / 255.0f;
        UINT k[4] { w, h, 0, 0 };
        std::memcpy(&k[2], &dark, sizeof(dark));
        std::memcpy(&k[3], &bright, sizeof(bright));
        DispatchOn(cmd, g_filters.guard.Get(), base + 8, history, k, 4, w, h);
    }
    if (!smooth)
        return;
    auto &warped = g_filters.warped[slot];
    if (warped == nullptr || warped->GetDesc().Width != w || warped->GetDesc().Height != h)
        if (!CreateTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, warped, "warped history"))
        {
            g_filters.failed = true;
            return;
        }
    ViewAt(base, history, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ViewAt(base + 1, g.netMotion.Get(), DXGI_FORMAT_R16G16_FLOAT);
    ViewAt(base + 3, warped.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    const UINT k[2] { w, h };
    DispatchOn(cmd, g_filters.warp.Get(), base, warped.Get(), k, 2, w, h);
}

// After pass `slot` wrote netColour, before that becomes its history: blend toward the warped
// previous output where the two differ by less than the limit. The pass is on this same list, so
// netColour already holds its answer.
void SmoothOutput(ID3D12GraphicsCommandList *cmd, UINT slot)
{
    const float strength = g.settings.outputSmooth.load();
    // Said once per change: off by setting, off because the filters did not build, or on -- the
    // last one being what a player asks about.
    static int logged = -1;
    const int state = strength <= 0.0f ? 0 : g_filters.failed ? 1 : 2;
    if (state != logged)
    {
        logged = state;
        static const char *why[] { "off (OutputSmooth=0)", "OFF: requested, but its filters failed to build", "on" };
        Log("output smooth: %s, strength %.2f, limit %.1f/255", why[state], static_cast<double>(strength),
            static_cast<double>(g.settings.outputSmoothLimit.load()));
    }
    if (state != 2 || g_filters.warped[slot] == nullptr)
        return;
    const UINT w = g.netWidth, h = g.netHeight, base = FilterSlots(slot) + 4;
    ViewAt(base, g_filters.warped[slot].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    ViewAt(base + 1, g.netBase.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    ViewAt(base + 3, g.netColour.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    const float threshold = std::max(1e-3f, g.settings.outputSmoothLimit.load()) / 255.0f;
    UINT k[4] { w, h, 0, 0 };
    std::memcpy(&k[2], &threshold, sizeof(threshold));
    std::memcpy(&k[3], &strength, sizeof(strength));
    DispatchOn(cmd, g_filters.smooth.Get(), base, g.netColour.Get(), k, 4, w, h);
    g_stats.smoothed |= 1u << slot;  // the temporal line (stats.inc)
}
