// The guides the D3D11 transport hands the network: the game's own depth and motion, copied
// across to our device. The work itself is core/shared/d3d11_guides.h, the same code the 32-bit
// bridge runs; what stays here is where this route keeps its devices and its switches.

namespace d3d11route {

// One frame of one guide. The shared texture is opened on our own device, which is why this has
// nothing to do until the work device is up.
bool PrepareGuide(Guide& guide, bool isDepth) {
    if (g.bridge.workDevice == nullptr)
        return false;
    auto ensure = [&](UINT w, UINT h, DXGI_FORMAT fmt, bool uav) {
        return guide.bridge.Ensure(g.bridge.game11.Get(), g.bridge.workDevice.Get(), w, h, fmt,
                                   uav);
    };
    return d3d11guides::PrepareGuide(g.bridge.game11.Get(), g.bridge.game11ctx.Get(), guide,
                                     isDepth, g.guideDepthCs, g.guideDepthCsFailed, p_D3DCompile,
                                     ensure, Log);
}

// Observation is deliberately NOT gated on the Depth switch. It used to be, and that made the
// question unanswerable: with the switch off -- the default -- this returned immediately, logged
// nothing, and the status line then said "no candidate found", which reads as "this game has no
// depth buffer" when it actually meant "nobody looked". Finding a candidate costs a pointer and a
// GetDesc; only *using* it is gated, further down in the present path.
void ObserveD3D11(device* dev, const resource_view* rtvs, uint32_t count, resource depthRes) {
    if (!g.settings.useGameGuides.load())
        return;
    // Not while ReShade is drawing its own effect chain. Every shader in that chain renders into
    // screen-sized intermediates, and an optical-flow shader's are two-channel float ones the
    // size of the screen -- which is the exact description this observation uses to recognise a
    // velocity buffer. Without this gate the motion guide could settle on a provider's working
    // texture, or on the companion effect's own previous-frame copy, and prefer it over the
    // game's real one on nothing better than which got bound more often.
    if (g.inEffects.load())
        return;
    d3d11guides::ObserveD3D11(dev, rtvs, count, depthRes, g.outWidth, g.outHeight, g_depthTally,
                              g_motionTally);
}

} // namespace d3d11route
