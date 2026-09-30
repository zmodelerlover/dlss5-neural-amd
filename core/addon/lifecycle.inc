// ReShade's swapchain, effect runtime and device lifecycle: what is drained and dropped when a
// swapchain goes, what is let back in when the next one is announced, which effect runtime the
// guides come from, and what a route does when the game destroys the device it latched. Part of the
// core's unity build, included by neural.cpp.

// Another window's swapchain (PrimaryRoute, Transports.inc): its presents went out as drawn, so
// nothing of ours is on it, and its teardown or arrival says nothing about the primary.
bool OtherSwapchain(swapchain *sc)
{
    void *primary = g.primarySwapchain.load();
    return primary != nullptr && primary != sc;
}

// Everything built for the primary swapchain, drained and dropped. Ensure rebuilds it on demand.
void DrainAndRelease()
{
    if (g.bridge.game11ctx != nullptr)
    {
        // Retiring the back-buffer reference needs the work to have *finished*, not just been
        // submitted: DXGI refuses ResizeBuffers while a pending command references a back buffer,
        // and the game quits over it. Flush does not drain it; an event query does, because it
        // does not report until everything submitted before it has completed.
        if (!d3d11guides::FlushAndWait11(g.bridge.game11.Get(), g.bridge.game11ctx.Get()))
            Log("resize: the bridge did not drain (query failed, 2 s timeout, or device removed).");
        // No ClearState: nothing of ours is bound, it only emptied the game's cached pipeline (PCSX2
        // lost its device 57 ms later, DRIVER_INTERNAL_ERROR; gone without it on the bench).
        g.bridge.game11ctx->Flush();
    }
    ReleaseSwapchainSized();
}

void OnDestroySwapchain(swapchain *sc, bool resize)
{
    if (OtherSwapchain(sc))
        return;  // and it must not take goneSwapchain from a primary mid-resize
    // Before the lock, so a present that is only just starting sees it and backs out.
    g.goneSwapchain.store(sc);
    std::lock_guard guard(g.lock);
    // A DXGI resize keeps the latch. Without resize it is let go, so another window can be taken
    // while this one is down, but that is not always the end of it: ReShade reports a Vulkan
    // swapchain rebuilt over oldSwapchain, and an OpenGL window minimised, as a destroy and a later
    // init of the same object, and that init takes the latch back (OnInitSwapchain).
    if (!resize && g.primarySwapchain.load() == sc)
    {
        g.primarySwapchain.store(nullptr);
        g.releasedPrimary = sc;
    }
    Log("swapchain going away (resize %d) after %llu frames; draining and dropping everything "
        "sized to it.", resize ? 1 : 0, static_cast<unsigned long long>(g.status.frame));
    DrainAndRelease();
}

// The new swapchain is up; everything below is sized to it and will be rebuilt on demand.
void OnInitSwapchain(swapchain *sc, bool resize)
{
    std::lock_guard guard(g.lock);
    // Only lift the gate for the swapchain that was actually torn down. A different one being
    // announced says nothing about this one.
    void *gone = g.goneSwapchain.load();
    if (gone == sc || gone == nullptr)
        g.goneSwapchain.store(nullptr);
    // The primary went down for a resize and another swapchain is announced instead: the resize failed
    // and the game made a new one. Where Winds Meet does this on its way from the 1334x750 start window to
    // the game's own size (its ResizeBuffers drops a flag the swapchain was made with, E_INVALIDARG).
    // ReShade frees the old one without a second destroy, so a latch left on it held the played window
    // out for good -- "no frames yet" -- and OnInitEffects called into the freed object and crashed.
    if (gone != nullptr && gone != sc && gone == g.primarySwapchain.load())
    {
        g.goneSwapchain.store(nullptr);
        g.releasedPrimary = nullptr;
        g.primarySwapchain.store(sc);
        g.status.loggedOtherSwapchain = false;
        Log("the swapchain that went down for a resize was replaced by a new one; the add-on moves to it.");
    }
    // The primary back from its destroy takes the latch again, from any window taken while it was
    // down, or the played window would go out raw until that one closed. What was built for that
    // window goes first: the routes hold one swapchain's worth. Keyed on the address, so a new
    // swapchain made where the released one was takes it too, as the game's remade one would.
    if (sc == g.releasedPrimary)
    {
        g.releasedPrimary = nullptr;
        if (OtherSwapchain(sc))
        {
            Log("the first swapchain is back; the one taken while it was down goes out as drawn.");
            DrainAndRelease();
            g.status.loggedOtherSwapchain = false;
        }
        g.primarySwapchain.store(sc);
    }
    if (OtherSwapchain(sc))
        return;
    g.outWidth = g.outHeight = 0;
    Log("swapchain back (resize %d); the bridge rebuilds at the new size on the next frame.",
        resize ? 1 : 0);
}

// The companion effect's textures are ReShade's, and only ReShade knows where they live. It
// hands over the runtime here; the guides are taken from it at present, once the whole effect
// chain has run, so what crosses is this frame's field rather than last frame's. Another window's
// runtime is left alone: it would run its effects into the primary's frame and feed it its guides.
void OnInitEffects(effect_runtime *runtime)
{
    std::lock_guard guard(g.lock);
    const auto *primary = static_cast<swapchain *>(g.primarySwapchain.load());
    if (primary != nullptr && primary->get_hwnd() != runtime->get_hwnd())
        return;
    g.effects = runtime;
    g.feedSignature = -1;
}

void OnDestroyEffects(effect_runtime *runtime)
{
    std::lock_guard guard(g.lock);
    if (g.effects != runtime)
        return;
    g.effects = nullptr;
    g.feedSignature = -1;
    // The guides point into textures that runtime owned. Letting them stand would copy from
    // freed memory on the next present.
    for (Guide *guide : { &g.guideMotion, &g.guideDepth })
        if (guide->external)
        {
            guide->external = false;
            guide->chosen.Reset();
            guide->ready = false;
        }
}

// The device a route latched is being destroyed: D3D12 stands down, and a route that crosses to
// our own device drops what crossed and waits for the new one (FrameTransport.hpp).
void OnDestroyDevice(device *dev)
{
    std::lock_guard guard(g.lock);
    if (FrameTransport *transport = TransportFor(dev->get_api()))
        transport->OnDestroyDevice(dev);
}
