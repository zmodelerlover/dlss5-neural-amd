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
    // Nor while no present will settle what is seen: effect off, standing down, bridge being
    // rebuilt or held off by a diagnostic key, window minimised. Every entry holds a reference to
    // one of the game's targets, and only SettleGuide lets them go. Cleared too, for the frame
    // that was seen as the effect went off, and so are the streaks: a cold start counted before
    // the pause would otherwise let the first present back decide the guide alone, and a
    // challenger is an identity the game may have reused by then. The 32-bit bridge clears its
    // tallies on every present that does not settle instead.
    if (!g.settings.enabled.load() || g.status.unavailable || g.status.failed ||
        g.status.windowHidden || g.bridge.failed || g.noBridge.load() || g.stage.load() < 3) {
        g_depthTally.clear();
        g_motionTally.clear();
        for (Guide* guide : {&g.guideDepth, &g.guideMotion}) {
            guide->challenger = nullptr;
            guide->challengerFrames = guide->coldFrames = 0;
        }
        return;
    }
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

// Called by CrossGuides before it settles the guides.
void PassOverFlatDepth() {
    // Depth read FLAT twice in a moving scene: the next buffer the tally holds that has not read FLAT
    // is tried before the copy before the clears (issue #19, three same-size depth buffers), three at
    // most. Before the settle, while the tally still holds this frame. A FLAT menu or sky is not the
    // buffer's last word, so 4800 presents after the last move every buffer is a candidate again.
    if (g.depthRotations != 0 && g.status.frame >= g.depthRotatedAt + 4800) {
        Log("guide depth: the buffers passed over as FLAT are candidates again");
        for (const void*& p : guides::passedOver)
            p = nullptr;
        g.depthRotations = 0;
    }
    if (g.depthFlatOf != nullptr && g.depthFlatOf == g.guideDepth.chosen.Get() && g.depthRotations < 3 &&
        std::any_of(g_depthTally.begin(), g_depthTally.end(), [](const auto& e) {
            return e.first != g.depthFlatOf && !guides::PassedOver(e.first);
        })) {
        Log("guide depth: %p reads FLAT in a moving scene; trying the next buffer before the copy before the clears",
            g.depthFlatOf);
        guides::PassOver(g.depthFlatOf);
        ++g.depthRotations;
        g.depthRotatedAt = g.status.frame;
        g.flatProbes = 0;
        g.guideDepth.chosen.Reset();
        g.guideDepth.challenger = nullptr;
        g.guideDepth.challengerFrames = g.guideDepth.coldFrames = 0;
    }
    g.depthFlatOf = nullptr;
}

} // namespace d3d11route
