// The D3D11 transport: our own D3D12 device beside the game's D3D11 one, the two sharing
// textures and fences. The port is in FrameTransport.hpp; the guides this route prepares are in
// D3D11Guides.inc.

namespace d3d11route {

// The game's device as ReShade hands it, before the QueryInterface: game11 need not be the same
// pointer, and this is what a present and a destroy_device are compared against.
ID3D11Device* gameDevice = nullptr;

// D3D11 path, step 1. Brings up the work device and stops there: the network still does not run
// on this API, so the picture is untouched. What this proves, and all it proves, is that the
// second device comes up on the same GPU as the game.
void BridgeStep1(device* dev) {
    if (g.bridge.failed || g.bridge.workDevice != nullptr)
        return;
    // First point at which D3D12 is genuinely needed. Everything before this runs without
    // d3d12.dll ever entering the process.
    if (!LoadGraphicsApi()) {
        g.bridge.failed = true;
        return;
    }
    if (g.stage.load() < 2)
        return;
    auto* native = reinterpret_cast<ID3D11Device*>(dev->get_native());
    if (native == nullptr || FAILED(native->QueryInterface(IID_PPV_ARGS(&g.bridge.game11)))) {
        Log("bridge: the game's device does not expose ID3D11Device5 (needs Windows 10 1703+).");
        g.bridge.failed = true;
        return;
    }
    gameDevice = native;
    ComPtr<ID3D11DeviceContext> ctx;
    g.bridge.game11->GetImmediateContext(&ctx);
    if (ctx == nullptr || FAILED(ctx.As(&g.bridge.game11ctx))) {
        Log("bridge: the game's context does not expose ID3D11DeviceContext4.");
        g.bridge.failed = true;
        return;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC ad{};
    if (FAILED(g.bridge.game11.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&ad))) {
        Log("bridge: could not read the game adapter's LUID.");
        g.bridge.failed = true;
        return;
    }
    if (!CreateWorkDevice(adapter.Get(), ad.AdapterLuid)) {
        g.bridge.failed = true;
        return;
    }
    if (g.stage.load() < 3) {
        Log("stage %d: stopping after the work device, on purpose.", g.stage.load());
        return;
    }
    if (FAILED(g.bridge.game11->OpenSharedFence(g.bridge.crossHandle,
                                                IID_PPV_ARGS(&g.bridge.crossOn11))) ||
        FAILED(g.bridge.game11->OpenSharedFence(g.bridge.backHandle,
                                                IID_PPV_ARGS(&g.bridge.backOn11)))) {
        Log("bridge: OpenSharedFence on the game's device failed.");
        g.bridge.failed = true;
        return;
    }
    // From here the network's device IS the work device. Everything downstream -- InitPipeline,
    // EnsureResources, InitEngine, the Packet -- already runs on whatever g.device points at, so
    // this one assignment is what moves the whole pipeline off the game's device.
    g.device = g.bridge.workDevice;
    g.queue = g.bridge.workQueue;
    Log("bridge: up. The network now runs on our own device; colour crosses in and the result "
        "crosses back.");
}

// The CPU-side drain, and why it replaced the cross-device fences, is d3d11_guides.h. The cost is
// real but already paid: the default mode is inline, where the game waits for the network anyway.
bool FlushAndWait11() {
    if (d3d11guides::FlushAndWait11(g.bridge.game11.Get(), g.bridge.game11ctx.Get()))
        return true;
    Log("bridge: the game's queue did not drain (query failed, 2 s timeout, or device removed).");
    return false;
}

// One frame across the bridge: the game's back buffer goes over, the network runs on our device,
// the finished image comes back. Both crossings are plain CopyResource -- a shared resource
// opened from another device sits in COMMON, which promotes to COPY_SOURCE and COPY_DEST on a
// direct queue, so there is no barrier here to get wrong.
void BridgePresent(device* dev, swapchain* sc) {
    const resource back = sc->get_current_back_buffer();
    if (back.handle == 0) {
        if (!g.loggedNoBackBuffer) {
            g.loggedNoBackBuffer = true;
            Log("present with no back buffer to read; skipping those frames.");
        }
        return;
    }
    const resource_desc bd = dev->get_resource_desc(back);
    const auto fmt = static_cast<DXGI_FORMAT>(bd.texture.format);
    const UINT w = bd.texture.width, h = bd.texture.height;
    // A multisampled back buffer cannot be copied into the single-sample staging: the copies are
    // dropped, and frames would count as processed with nothing changed. It goes out as drawn and
    // counts as nothing, as on D3D12 and the 32-bit bridge. Not a stuck bridge, so the stand-down
    // in Present is held off; and the tallies go, since SettleGuide is not reached from here.
    if (bd.texture.samples > 1) {
        g_depthTally.clear();
        g_motionTally.clear();
        g.bridge.presentsSinceFrame = 0;
        static bool said = false;
        if (!said) {
            said = true;
            Log("MSAA back buffer (%u samples): frames go out as the game drew them.",
                static_cast<unsigned>(bd.texture.samples));
        }
        return;
    }

    if (!EnsureResources(w, h, fmt, EffectiveScale())) {
        g.status.unavailable = true;
        if (*g.status.reason == 0)
            g.status.reason = "could not create the working textures; see amd-nr.log";
        return;
    }
    if (!g.bridge.in.Ensure(g.bridge.game11.Get(), g.bridge.workDevice.Get(), w, h, fmt) ||
        !g.bridge.out.Ensure(g.bridge.game11.Get(), g.bridge.workDevice.Get(), w, h, fmt)) {
        g.bridge.failed = true;
        g.status.unavailable = true;
        g.status.reason = "the back buffer format is not shareable between the two devices";
        return;
    }
    // The crossed texture is read as an SRV by the copy shader. Rather than lean on state
    // promotion for a shared resource, take a local copy first: a plain copy is the one access
    // that needs no barriers on either side. Made in the format it is read as, like composed:
    // CreateTexture allows UAV, which an _SRGB format refuses, and a view must match its resource.
    const DXGI_FORMAT crossFmt = ColourReadFormat(fmt);
    if (g.bridge.crossLocal == nullptr || g.bridge.crossLocal->GetDesc().Width != w ||
        g.bridge.crossLocal->GetDesc().Height != h ||
        g.bridge.crossLocal->GetDesc().Format != crossFmt) {
        if (!CreateTexture(w, h, crossFmt, g.bridge.crossLocal, "crossLocal",
                           D3D12_RESOURCE_STATE_COPY_DEST)) {
            g.bridge.failed = true;
            return;
        }
    }
    if (g.fence == nullptr &&
        FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) {
        g.status.unavailable = true;
        g.status.reason = "could not create the completion fence";
        return;
    }

    const bool runNetwork = JobGate();

    // 0. when the companion effect is on, ReShade's chain runs now, before the copy below reads
    // the back buffer. See RenderEffectsAheadOfNetwork for why the order matters.
    RenderEffectsAheadOfNetwork(dev, back);

    // 1. the game's image goes over, and with it whatever guides the game actually renders.
    // This is the step that has no equivalent on PCSX2: there the only thing to send is colour.
    if (!g.noBackBuffer.load()) {
        if (!d3d11guides::EnsureStage(g.bridge.game11.Get(), g.bridge, w, h, fmt, Log)) {
            g.bridge.failed = true;
            return;
        }
        g.bridge.game11ctx->CopyResource(g.bridge.stageIn11.Get(),
                                         reinterpret_cast<ID3D11Resource*>(back.handle));
        g.bridge.game11ctx->CopyResource(g.bridge.in.on11.Get(), g.bridge.stageIn11.Get());
    }
    // Settled first, and unconditionally, so a game that renders its own buffers can take a slot
    // back off the companion effect. The observation no longer sees ReShade's own targets, so an
    // entry in these tallies is the game's by construction.
    const bool hadDepth = g.guideDepth.chosen != nullptr && g.settings.useDepth.load();  // in use
    const bool hadMotion = g.guideMotion.chosen != nullptr && g.settings.useMotion.load();
    const bool tookDepth = SettleGuide(g.guideDepth, g_depthTally, Log);
    const bool tookMotion = SettleGuide(g.guideMotion, g_motionTally, Log);
    if (tookDepth || tookMotion)
        RearmGuideProbe((tookDepth && hadDepth) || (tookMotion && hadMotion));
    AdoptFeedEffect();
    // Depth the probe withheld as copied at present is copied before the game's clears instead,
    // and looked at again, the first time on trial (d3d11_guides.h). depthUsable and depthVaried
    // are this buffer's: every take re-arms the probe.
    if (d3d11guides::FallBackToPreClear(g.guideDepth, !g.depthUsable.load(), g.depthVaried.load(),
                                        Log))
        RearmGuideProbe(false);
    g.guideDepth.ready = g.guideMotion.ready = false;
    if (g.settings.useGameGuides.load()) {
        // The effect's depth is already a plain R32_FLOAT, which opens on the second device as
        // it stands. It goes down the motion path -- one CopyResource -- rather than the depth
        // one, whose snapshot and compute pass exist only to get a planar depth-stencil format
        // into a shape that can cross at all.
        if (g.settings.useDepth.load())
            PrepareGuide(g.guideDepth, !g.guideDepth.external);
        if (g.settings.useMotion.load())
            PrepareGuide(g.guideMotion, false);
    }
    // The copies above have to have landed before our device reads the shared textures. Waiting
    // here rather than queueing a GPU wait is what keeps the game's context clear of pending
    // cross-device dependencies -- see FlushAndWait11.
    if (!FlushAndWait11()) {
        g.bridge.failed = true;
        return;
    }

    // 2. our device does the work
    const UINT i = static_cast<UINT>(g.bridge.backValue % State::kRing);
    // Resetting an allocator whose command list is still executing is undefined behaviour, so
    // this wait is not optional, and one that gives up (FenceWaitCapMs) returns before the reset.
    if (g.ringValue[i] != 0 &&
        !WaitFence(g.ringFence.Get(), g.ringValue[i], g.ringEvent, "the ring slot to come free"))
        return;
    const HRESULT allocatorHr = g.alloc[i]->Reset();
    const HRESULT listHr =
        SUCCEEDED(allocatorHr) ? g.list[i]->Reset(g.alloc[i].Get(), nullptr) : allocatorHr;
    if (FAILED(allocatorHr) || FAILED(listHr)) {
        Log("bridge: work slot %u reset failed (allocator 0x%08lX, list 0x%08lX); "
            "recreating the pair.",
            i, allocatorHr, listHr);
        if (!RecreateWorkSlot(i))
            g.bridge.failed = true;
        return;
    }
    auto* cmd = g.list[i].Get();
    cmd->CopyResource(g.bridge.crossLocal.Get(), g.bridge.in.on12.Get());
    // Same reason as crossLocal: a shared resource is read here as an SRV, and a plain copy is
    // the one access that needs no barrier on either side.
    auto localise = [&](Guide& guide) {
        if (!guide.ready || guide.bridge.on12 == nullptr)
            return false;
        const auto want = guide.bridge.on12->GetDesc();
        if (guide.local == nullptr || guide.local->GetDesc().Width != want.Width ||
            guide.local->GetDesc().Height != want.Height ||
            guide.local->GetDesc().Format != want.Format) {
            guide.local.Reset();
            if (!CreateTexture(static_cast<UINT>(want.Width), want.Height, want.Format, guide.local,
                               guide.name))
                return false;
        }
        Barrier(cmd, guide.local.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(guide.local.Get(), guide.bridge.on12.Get());
        Barrier(cmd, guide.local.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return true;
    };
    g.gameDepthActive = localise(g.guideDepth);
    g.gameMotionActive = localise(g.guideMotion);
    Barrier(cmd, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool ok =
        RecordNetwork(cmd, g.bridge.crossLocal.Get(), fmt, nullptr, runNetwork, g.loadedPasses,
                      [&]() { return SubmitPrivatePass(cmd, g.alloc[i].Get()); });
    if (!ok && g.status.failed)
        return;
    Barrier(cmd, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    if (ok && CompositionIsFresh(runNetwork)) {
        Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(g.bridge.out.on12.Get(), g.composed.Get());
        Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (const HRESULT hr = cmd->Close(); FAILED(hr)) {
        // The one path that used to set bridgeFailed without saying anything. It fires on the
        // frame after a swapchain rebuild, and with bridgeFailed latching for the whole run the
        // add-on then returned from every present in silence and left the last image it wrote on
        // screen. Both halves of that were wrong.
        Log("bridge: closing the command list failed (0x%08lX); rebuilding.", hr);
        RetireUnsubmitted(g.bridge.workQueue.Get(), cmd);
        if (!RecreateWorkSlot(i))
            g.bridge.failed = true;
        return;
    }
    ID3D12CommandList* lists[]{cmd};
    g.bridge.workQueue->ExecuteCommandLists(1, lists);
    if (g.activePasses != 0)
        NotifyRuntimes(g.bridge.workQueue.Get(), 1, lists);
    g.ringValue[i] = ++g.ringSerial;
    g.bridge.workQueue->Signal(g.ringFence.Get(), g.ringSerial);
    g.completion = ++g.serial;
    g.bridge.workQueue->Signal(g.fence.Get(), g.completion);

    // 3. and the finished image comes back, once our queue has actually produced it
    ++g.bridge.backValue;
    // A wait that gave up has stood the add-on down; FlushAndWait11 above already let go of the
    // back buffer, so nothing on the game's side is left holding it.
    if (!WaitForWorkQueue(g.completion))
        return;
    // Gated on freshness as well: without that, a skipped frame copies back whatever composition
    // the last evaluated frame left in the shared texture -- a whole stale picture, which is a
    // worse artefact than the stale correction this is here to avoid.
    if (ok && CompositionIsFresh(runNetwork) && !g.noBackBuffer.load() &&
        g.bridge.stageOut11 != nullptr) {
        g.bridge.game11ctx->CopyResource(g.bridge.stageOut11.Get(), g.bridge.out.on11.Get());
        g.bridge.game11ctx->CopyResource(reinterpret_cast<ID3D11Resource*>(back.handle),
                                         g.bridge.stageOut11.Get());
        // And this copy must not still be pending when the game resizes. It is the last thing
        // the add-on does to the swapchain, so draining here leaves nothing outstanding.
        FlushAndWait11();
    }

    // The immediate context keeps a deferred reference to every resource a recorded command
    // touched, and both copies above touch the back buffer. ResizeBuffers refuses -- with
    // DXGI_ERROR_INVALID_CALL, which the game reports as a DirectX error and quits over -- while
    // any reference to a back buffer is outstanding, and the game does its own cleanup *before*
    // the present callback runs, so ours is always the one left. NFS resizes once on the way to
    // fullscreen, which is exactly where it died. Flushing here retires them.
    g.bridge.game11ctx->Flush();

    DrainReadbacks(g.netWidth, g.netHeight);

    if (!g.loggedRound) {
        g.loggedRound = true;
        Log("bridge: first full round trip done at %ux%u.", w, h);
    }
    if (++g.status.frame <= 3 || g.status.frame % 120 == 0)
        Log("frame %llu processed (%llu skipped)", static_cast<unsigned long long>(g.status.frame),
            static_cast<unsigned long long>(g.status.skipped));
}

struct Transport final : FrameTransport {
    const char* Name() const override { return "D3D11"; }
    uint32_t Caps() const override { return kDepth | kGameGuides | kFeed | kStage | kBridge; }
    void OnTargetsBound(device* dev, uint32_t count, const resource_view* rtvs,
                        resource depth) override {
        const auto observe = LockForHook();
        ObserveD3D11(dev, rtvs, count, depth);
    }
    // The depth copy before a clear, once the probe withheld the one taken at present
    // (d3d11_guides.h); not while ReShade draws its chain, or while nothing would read the copy:
    // ObserveD3D11's stand-down test, and the Depth switch.
    void OnDepthCleared(command_list* cmd, device*, resource_view dsv) override {
        if (g.inEffects.load())
            return;
        const auto guard = LockForHook();
        if (g.settings.enabled.load() && g.settings.useGameGuides.load() &&
            g.settings.useDepth.load() && !g.status.unavailable && !g.status.failed &&
            !g.status.windowHidden && !g.bridge.failed && !g.noBridge.load() && g.stage.load() >= 3)
            d3d11guides::SnapshotDepthBeforeClear(cmd, dsv, gameDevice, g.guideDepth);
    }
    void OnDestroyDevice(device* dev) override {
        if (gameDevice != nullptr &&
            reinterpret_cast<ID3D11Device*>(dev->get_native()) == gameDevice)
            DeviceGone("the game destroyed its D3D11 device; restart the game to re-enable");
    }
    void Present(device* dev, command_queue*, swapchain* sc) override {
        // Between a teardown and the new swapchain being announced there is nothing safe to
        // touch, and touching it is exactly what makes the resize fail.
        if (g.noBridge.load() || g.goneSwapchain.load() == sc)
            return;
        // Before the watchdog: another device's presents must not count as the bridge stalling.
        if (gameDevice != nullptr &&
            Foreign(reinterpret_cast<ID3D11Device*>(dev->get_native()) != gameDevice,
                    "D3D11 device"))
            return;
        // Whatever goes wrong below, the game keeps its picture. If presents keep arriving and the
        // bridge has not completed a frame in a long while, something is stuck, and the honest move
        // is to stand down rather than hold a half-built or stale image on screen. Ten seconds of
        // presents at 60 Hz; a real frame resets it. This is here because two separate bugs -- an
        // event handle shared by two fences, and a teardown flag that latched on -- both showed up
        // as a frozen counter and a black window rather than as anything readable.
        if (g.status.frame != g.bridge.lastSeenFrame) {
            g.bridge.lastSeenFrame = g.status.frame;
            g.bridge.presentsSinceFrame = 0;
            g.bridge.retries = 0; // a frame got through, so the retries were spent well
        } else if (++g.bridge.presentsSinceFrame > 600) {
            g.status.unavailable = true;
            g.status.reason = "the bridge stopped completing frames; see amd-nr.log";
            Log("600 presents without the bridge finishing a frame. Something is stuck, so the "
                "add-on is standing down and leaving the game's own image alone. The last lines "
                "above this one say how far it got.");
            return;
        }
        BridgeStep1(dev);
        if (g.bridge.workDevice == nullptr)
            return; // BridgeStep1 said why
        // The bridge is built on the game's device as first seen. OnPresent checks our own device;
        // a removed game device comes back as a new one, with none of this on it.
        if (FAILED(g.bridge.game11->GetDeviceRemovedReason())) {
            DeviceGone("the game's D3D11 device was removed; restart the game to re-enable");
            return;
        }
        if (g.bridge.failed) {
            if (++g.bridge.retries > State::kMaxBridgeRetries) {
                g.status.unavailable = true;
                g.status.reason = "the bridge kept failing to rebuild; see amd-nr.log";
                Log("bridge: %u rebuilds in a row did not take. Standing down and leaving the "
                    "game's own image alone.",
                    g.bridge.retries - 1);
                return;
            }
            Log("bridge: rebuilding after a failure (attempt %u of %u).", g.bridge.retries,
                State::kMaxBridgeRetries);
            ::ReleaseSwapchainSized();
            g.bridge.failed = false;
            return; // next present builds it again
        }
        if (g.stage.load() < 3)
            return; // diagnostic: stop before the engine, with the device already up
        // This used to be a hardcoded single pass, so Pass Count did nothing at all on the
        // bridge -- which is every D3D11 target, PCSX2 and NFS included. The multi-pass loop
        // existed only on the D3D12 path.
        UINT wanted = WantedPasses();
        if (!BringUpEngines(wanted)) {
            g.status.unavailable = true;
            // Do not paper over a reason the bring-up already gave. The hash refusal names the
            // file the user has to replace; "could not bring the engine up" names nothing.
            if (*g.status.reason == 0) {
                g.status.reason = "could not bring the engine up on the bridge device";
                Log("off: %s", g.status.reason);
            }
            return;
        }
        g.loadedPasses = wanted;
        BridgePresent(dev, sc);
        return;
    }
};
Transport transport;

} // namespace d3d11route
