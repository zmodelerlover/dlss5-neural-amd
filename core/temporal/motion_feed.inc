// The motion handed to the runtime, fed the way the reference feeds its own history warp -- C1 of
// its temporal chain, shaders/native_temporal_feed.hlsl: scaled, then anything longer than a cap,
// or not a number, becomes zero. Part of the core's unity build, included by neural.cpp.
//
// No reprojection here. The runtime warps the history by packet.motion itself (E4: on a 2 px/frame
// pan the true motion halves the output's frame-to-frame change, zero or reversed motion does
// nothing), so warping it again would be twice.
//
// The motion goes over in pixels of the raster, which is what the runtime reads. MotionMaxPx in the
// ini is the cap; the reference runs 64, and 0 means the raster itself.
//
// It runs every frame, whatever the source. A vector longer than the raster is not motion, and
// Tomb Raider showed what one does: the 960x540 RG16F the guide took there reads 65504 px (FP16
// max) on every pixel, the runtime's history and the output smooth both fetch off the frame, and
// what is left on screen is the network alone, flickering. With no source this frame the field is
// zeroed rather than left stale. The bit compare catches inf and NaN too, isnan or not.

inline constexpr char kMotionFeedShader[] = R"(
RWTexture2D<float2> mv : register(u0);
cbuffer C : register(b0) { uint w; uint h; float k; float maxPx; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=w || p.y>=h) return;
 float2 v = mv[p.xy] * k;
 if (any((asuint(v) & 0x7fffffff) > asuint(maxPx))) v = 0;
 mv[p.xy] = v;
})";

// Never destroyed, as State g is: see neural.cpp.
ComPtr<ID3D12PipelineState> &g_motionFeed = *new ComPtr<ID3D12PipelineState>;

// The probe saw a field longer than the raster. The feed has already zeroed it; this gives the
// motion back to the estimator. ponytail: it lasts until a motion buffer is taken again, on every
// route (the x86 bridge hears of it in FRAME) -- another one, or this one after a resize. The take
// re-probes, so a buffer that is still not velocity is demoted again ~120 presents later, the feed
// zeroing it meanwhile. Nothing remembers which buffer it was: a raw pointer can be the game's next
// allocation, and a reference would hold its buffer across its own resize.
void DemoteMotion(double meanPx)
{
    if (g.guideMotion.failed)
        return;
    g.guideMotion.failed = true;
    Log("guide motion: mean |d| %.0f px, longer than the raster, so this buffer is not velocity. "
        "Giving up on it; the estimator takes the motion over.", meanPx);
}

void FeedMotion(ID3D12GraphicsCommandList *cmd, bool haveMotion)
{
    const float set = g.settings.motionMaxPx.load();
    const float maxPx = set > 0.0f ? set : static_cast<float>(std::max(g.netWidth, g.netHeight));
    if (g.netMotion == nullptr || (g_motionFeed == nullptr &&
        !CompileShader(kMotionFeedShader, sizeof(kMotionFeedShader), "motion feed", g_motionFeed)))
        return;
    const float k = haveMotion ? 1.0f : 0.0f;
    ViewAt(47, g.netMotion.Get(), DXGI_FORMAT_R16G16_FLOAT, true);
    UINT c[4] { g.netWidth, g.netHeight, 0, 0 };
    std::memcpy(&c[2], &k, sizeof(k));
    std::memcpy(&c[3], &maxPx, sizeof(maxPx));
    DispatchOn(cmd, g_motionFeed.Get(), 44, g.netMotion.Get(), c, 4, g.netWidth, g.netHeight);
}
