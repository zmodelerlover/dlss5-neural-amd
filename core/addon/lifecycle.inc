// ReShade's swapchain and device lifecycle: what is drained and dropped when a swapchain goes, what
// is let back in when the next one is announced, and which route stands down when the device it
// built on is destroyed. Part of the core's unity build, included by neural.cpp.

void OnDestroySwapchain(swapchain *sc, bool resize)
{
    // Before the lock, so a present that is only just starting sees it and backs out.
    g.goneSwapchain.store(sc);
    std::lock_guard guard(g.lock);
    Log("swapchain going away (resize %d) after %llu frames; draining and dropping everything "
        "sized to it.", resize ? 1 : 0, static_cast<unsigned long long>(g.status.frame));

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

// The new swapchain is up; everything below is sized to it and will be rebuilt on demand.
void OnInitSwapchain(swapchain *sc, bool resize)
{
    std::lock_guard guard(g.lock);
    // Only lift the gate for the swapchain that was actually torn down. A different one being
    // announced says nothing about this one.
    void *gone = g.goneSwapchain.load();
    if (gone == sc || gone == nullptr)
        g.goneSwapchain.store(nullptr);
    g.outWidth = g.outHeight = 0;
    Log("swapchain back (resize %d); the bridge rebuilds at the new size on the next frame.",
        resize ? 1 : 0);
}

// The device a route latched is being destroyed: that route stands down (FrameTransport.hpp),
// because a game that makes a new one hands it none of what was built on the old.
void OnDestroyDevice(device *dev)
{
    std::lock_guard guard(g.lock);
    if (FrameTransport *transport = TransportFor(dev->get_api()))
        transport->OnDestroyDevice(dev);
}
