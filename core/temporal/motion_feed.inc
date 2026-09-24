// The motion handed to the runtime, fed the way the reference feeds its own history warp -- C1 of
// its temporal chain, shaders/native_temporal_feed.hlsl: scaled, then anything longer than a cap,
// or not a number, becomes zero. Part of the core's unity build, included by neural.cpp.
//
// No reprojection here. The runtime warps the history by packet.motion itself (E4: on a 2 px/frame
// pan the true motion halves the output's frame-to-frame change, zero or reversed motion does
// nothing), so warping it again would be twice.
//
// MotionCompat in the ini: 0 hands over pixels of the raster, which is what the runtime reads. 1
// reproduces what the reference actually does on its Magpie route: it takes the FFX descriptor's
// jitter (+360) for the motion scale (+368), finds 0, and multiplies motion that is already in
// pixels by the fit width as if it were UV (native_submission_order_probe.cpp:225 against
// native_pre_upscale.h:18-20, native_game_frame.h:192). With its 64 px cap that zeroes all but the
// smallest vectors. MotionMaxPx is the cap, 0 for none; the reference runs 64.

inline constexpr char kMotionFeedShader[] = R"(
RWTexture2D<float2> mv : register(u0);
cbuffer C : register(b0) { uint w; uint h; float k; float maxPx; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=w || p.y>=h) return;
 float2 v = mv[p.xy] * k;
 if (maxPx > 0 && (abs(v.x) > maxPx || abs(v.y) > maxPx || any(isnan(v)))) v = 0;
 mv[p.xy] = v;
})";

ComPtr<ID3D12PipelineState> g_motionFeed;

void FeedMotion(ID3D12GraphicsCommandList *cmd)
{
    const bool compat = g.settings.motionCompat.load() != 0;
    const float maxPx = g.settings.motionMaxPx.load();
    if (!compat && maxPx <= 0.0f)
        return;
    if (g_motionFeed == nullptr &&
        !CompileShader(kMotionFeedShader, sizeof(kMotionFeedShader), "motion feed", g_motionFeed))
        return;
    const float k = compat ? static_cast<float>(g.netWidth) : 1.0f;
    ViewAt(47, g.netMotion.Get(), DXGI_FORMAT_R16G16_FLOAT, true);
    UINT c[4] { g.netWidth, g.netHeight, 0, 0 };
    std::memcpy(&c[2], &k, sizeof(k));
    std::memcpy(&c[3], &maxPx, sizeof(maxPx));
    DispatchOn(cmd, g_motionFeed.Get(), 44, g.netMotion.Get(), c, 4, g.netWidth, g.netHeight);
}
