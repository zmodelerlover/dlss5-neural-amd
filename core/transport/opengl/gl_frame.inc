// The OpenGL route's two blits and one frame. Included by gl_route.inc, in namespace glroute.

// ---- the two blits ---------------------------------------------------------------------------
//
// Y is inverted in both directions, and the two inversions are not a mistake that cancels: OpenGL
// puts the origin at the bottom left and the network's frame is top-left like every D3D surface
// the rest of this add-on handles, so the image is turned over on the way in and turned back on
// the way out. Getting one of them wrong produces an upside-down game with no error anywhere.

bool BlitIn(Route &r)
{
    SelfIssued selfIssued;
    Api &api = r.api;
    SavedState saved {};
    Save(api, saved);

    const GLint w = static_cast<GLint>(r.width), h = static_cast<GLint>(r.height);

    api.disable(GL_SCISSOR_TEST_);
    api.disable(GL_FRAMEBUFFER_SRGB_);
    api.bindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    api.getIntegerv(GL_READ_BUFFER_, &saved.readBuffer);
    saved.haveReadBuffer = true;
    api.readBuffer(GL_BACK_);
    api.Drain();

    bool ok = true;
    if (r.defaultSamples > 1)
    {
        // Two steps, because one blit cannot be asked to resolve samples and mirror Y at once:
        // with a multisampled read framebuffer the spec requires source and destination
        // rectangles of the same size, and a driver is within its rights to read a mirrored
        // rectangle as a different one. Resolve straight, then flip.
        api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, r.resolveFbo);
        api.blitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
        ok = api.Clean();
        api.bindFramebuffer(GL_READ_FRAMEBUFFER_, r.resolveFbo);
        api.readBuffer(GL_COLOR_ATTACHMENT0_);
        api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, r.in.fbo);
        api.blitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
        ok = api.Clean() && ok;
    }
    else
    {
        api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, r.in.fbo);
        api.blitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
        ok = api.Clean();
    }

    // The hand-over. With the imported fences it is a signal the D3D12 queue waits on, and the
    // flush is not optional: a signal sitting in the GL command queue is a wait that never ends
    // on the other side. Without them it is glFinish, the CPU stall the D3D11 route uses.
    if (r.semaphores)
    {
        const GLuint64 value = ++r.toD3DValue;
        const GLenum layout = GL_LAYOUT_TRANSFER_SRC_EXT_;  // D3D12 reads it as a copy source
        api.semaphoreParameterui64v(r.semToD3D, GL_D3D12_FENCE_VALUE_EXT_, &value);
        api.signalSemaphore(r.semToD3D, 0, nullptr, 1, &r.in.texture, &layout);
        api.flush();
        ok = api.Clean() && ok;
    }
    else
    {
        api.finish();
    }
    Restore(api, saved);
    return ok;
}

bool BlitOut(Route &r)
{
    SelfIssued selfIssued;
    Api &api = r.api;
    SavedState saved {};
    Save(api, saved);

    const GLint w = static_cast<GLint>(r.width), h = static_cast<GLint>(r.height);

    api.disable(GL_SCISSOR_TEST_);
    api.disable(GL_FRAMEBUFFER_SRGB_);
    api.bindFramebuffer(GL_READ_FRAMEBUFFER_, r.out.fbo);
    api.readBuffer(GL_COLOR_ATTACHMENT0_);
    api.Drain();

    // The other half of the hand-over: the network's result is in memory D3D12 has just written,
    // and GL has to be told to wait for that before it reads it. Without the fences this is
    // covered by the CPU wait the caller already did.
    if (r.semaphores)
    {
        const GLuint64 value = r.toGlValue;
        const GLenum layout = GL_LAYOUT_TRANSFER_SRC_EXT_;  // GL reads it as a blit source
        api.semaphoreParameterui64v(r.semToGl, GL_D3D12_FENCE_VALUE_EXT_, &value);
        api.waitSemaphore(r.semToGl, 0, nullptr, 1, &r.out.texture, &layout);
    }

    api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    api.getIntegerv(GL_DRAW_BUFFER_, &saved.drawBuffer);
    saved.haveDrawBuffer = true;
    api.drawBuffer(GL_BACK_);

    // Into the default framebuffer, multisampled or not: a single-sampled source written to a
    // multisampled destination replicates to every sample, which is what a frame that has already
    // been resolved should do.
    api.blitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
    const bool ok = api.Clean();

    Restore(api, saved);
    return ok;
}

// ---- one frame --------------------------------------------------------------------------------

void Present(command_queue *queue, swapchain *sc)
{
    Route &r = g_route;
    if (r.failed)
        return;

    if (!filterInstalled)
    {
        filterInstalled = true;
        g_previousFilter = SetUnhandledExceptionFilter(ReportFault);
    }

    device *dev = sc->get_device();
    if (!r.api.LoadCore())
    {
        r.Stop("opengl32.dll has no wgl entry points, which should be impossible inside an "
               "OpenGL host.");
        return;
    }

    // ReShade reports the back buffer as GL_FRAMEBUFFER_DEFAULT with resource_type::surface, so
    // the handle is not something to copy -- it is read for its dimensions and nothing else.
    const resource back = sc->get_current_back_buffer();
    if (back.handle == 0)
        return;
    const resource_desc bd = dev->get_resource_desc(back);
    if (bd.texture.width == 0 || bd.texture.height == 0)
        return;
    // The dimensions come from here; the sample count does not. Ensure reads GL_SAMPLES from GL
    // itself, because that is the number the blit rules are written against and it is one call
    // away -- and because a route that depends on a second party's description of the default
    // framebuffer has one more thing that can quietly change under it. What ReShade thinks is
    // logged once beside it, so the two can be compared in the field.
    if (!r.loggedBackBuffer)
    {
        r.loggedBackBuffer = true;
        Log("opengl: ReShade reports the back buffer as %ux%u, %u sample(s), format %u.",
            bd.texture.width, bd.texture.height, bd.texture.samples,
            static_cast<unsigned>(bd.texture.format));
    }

    if (!Ensure(r, bd.texture.width, bd.texture.height))
        return;

    // Stage 2 means what it has always meant here: bring the bridge up and stop before the
    // engine. On this route that is worth more than a diagnostic. It blits the frame across to
    // D3D12, copies it inside our device and blits it back, so the whole transport is exercised
    // against a real game with the network taken out of the question -- and every way the
    // transport can be wrong is visible on screen rather than in a log. An upside-down game is a
    // flip that was not undone; a blue sky gone orange is the channel order; a black screen is
    // memory that never aliased.
    const bool transportOnly = g.stage.load() < 3;
    if (!transportOnly)
    {
        if (!EnsureResources(r.width, r.height, kCrossingFormat, g.settings.scale.load()))
        {
            g.status.unavailable = true;
            if (*g.status.reason == 0)
                g.status.reason = "could not create the working textures; see amd-nr.log";
            Log("opengl: %s", g.status.reason);
            return;
        }

        UINT wanted = WantedPasses();
        if (!BringUpEngines(wanted))
        {
            g.status.unavailable = true;
            g.status.reason = "could not bring the engine up on the OpenGL bridge";
            Log("opengl: %s", g.status.reason);
            return;
        }
        g.loadedPasses = wanted;
    }
    else if (!r.loggedTransportOnly)
    {
        r.loggedTransportOnly = true;
        Log("opengl: Stage=%d, so this is the transport on its own: the frame crosses to D3D12 "
            "and comes back untouched. If the picture looks exactly as it did without the "
            "add-on, the crossing is correct.", g.stage.load());
    }

    // The same guard the other routes carry: the engine's HIP worker can outlast our queue (its
    // watchdog lets the list go on), and starting a second evaluation on top of one still running
    // is how this hangs rather than skips.
    // The parentheses are the point. Without them && binds tighter than || and the right-hand
    // side -- which reads the engine's job counter through g.runtime -- is evaluated even when
    // the engine was never loaded, which in transport-only mode is a null dereference on the
    // first frame. That is precisely how this route's second run ended.
    auto jobPending = [&] {
        return !transportOnly &&
               (g.fence->GetCompletedValue() < g.completion ||
                RuntimeBusy());
    };

    bool runNetwork = !transportOnly;
    if (jobPending() && GetTickCount64() - g.lastJobAt < 500)
    {
        // Starting a second evaluation on top of one still running is how this hangs rather than
        // skips, which is why the other routes carry the same guard. The route waits for the
        // outstanding job, as the D3D11 and Vulkan routes do: that paces the game to the network,
        // so every frame counter -- the game's, the overlay's, the driver's -- counts frames the
        // player can actually see, instead of duplicates of the last result.
        WaitForPreviousJob();
        runNetwork = !jobPending();
    }

    // --- 1. the host's frame comes over -------------------------------------------------------
    if (!BlitIn(r))
    {
        r.Stop("the blit out of the default framebuffer reported a GL error.");
        return;
    }

    // With the fences in play the queue waits for GL's signal that the frame has landed in the
    // shared texture; without them BlitIn already called glFinish. The first signal is seen on
    // the CPU before any queue waits on it: a queue wait that is never satisfied cannot be
    // released from here, and the first pass's list and every CPU wait on the queue would hang
    // behind it. Polled, so no registration outlives the timeout on an event another wait uses.
    if (r.semaphores && !r.semaphoresProven)
    {
        const UINT64 until = GetTickCount64() + 2000;
        while (g.bridge.backFence->GetCompletedValue() < r.toD3DValue && GetTickCount64() < until)
            Sleep(1);
        r.semaphores = r.semaphoresProven = g.bridge.backFence->GetCompletedValue() >= r.toD3DValue;
        if (!r.semaphores)
        {
            SelfIssued selfIssued;
            r.api.finish();
        }
        Log("opengl: GL's first signal on the imported fence %s", r.semaphores
                ? "reached D3D12; the CPU stall is dropped from here on."
                : "did not reach D3D12 within 2 s, so the fences are not driving this queue. "
                  "Falling back to the CPU stall until the host replaces its GL context.");
    }
    if (r.semaphores)
        g.bridge.workQueue->Wait(g.bridge.backFence.Get(), r.toD3DValue);

    // --- 2. our device does the work ----------------------------------------------------------
    const UINT slot = static_cast<UINT>(g.serial % State::kRing);
    if (g.ringValue[slot] != 0 &&
        !WaitFence(g.ringFence.Get(), g.ringValue[slot], g.ringEvent, "the ring slot to come free"))
        return;
    const HRESULT allocatorHr = g.alloc[slot]->Reset();
    const HRESULT listHr = SUCCEEDED(allocatorHr)
                               ? g.list[slot]->Reset(g.alloc[slot].Get(), nullptr)
                               : allocatorHr;
    if (FAILED(allocatorHr) || FAILED(listHr))
    {
        Log("opengl: work slot %u reset failed (allocator 0x%08lX, list 0x%08lX); recreating the "
            "pair.", slot, allocatorHr, listHr);
        if (!RecreateWorkSlot(slot))
            r.Stop("the D3D12 work slot could not be recovered.");
        return;
    }
    auto *cmd12 = g.list[slot].Get();
    bool fresh = false;
    if (transportOnly)
    {
        // In and straight back out, through our own device, so the round trip is real: the frame
        // is read from the imported texture, written to a resource this device owns, and written
        // back to the other imported texture.
        cmd12->CopyResource(g.bridge.crossLocal.Get(), r.in.on12.Get());
        Barrier(cmd12, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd12->CopyResource(r.out.on12.Get(), g.bridge.crossLocal.Get());
        Barrier(cmd12, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        fresh = true;
    }
    else
    {
        cmd12->CopyResource(g.bridge.crossLocal.Get(), r.in.on12.Get());
        Barrier(cmd12, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const bool ok =
            RecordNetwork(cmd12, g.bridge.crossLocal.Get(), kCrossingFormat, nullptr, runNetwork,
                          g.loadedPasses,
                          [&]() { return SubmitPrivatePass(cmd12, g.alloc[slot].Get()); });
        if (!ok && g.status.failed)
            return;
        Barrier(cmd12, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

        // Freshness gate, same as the other routes: on a frame where the network was skipped the
        // residual belongs to an older picture, and the host is better served by its own image
        // than by a correction aimed at where things used to be.
        fresh = ok && CompositionIsFresh(runNetwork);
        if (fresh)
        {
            Barrier(cmd12, g.composed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd12->CopyResource(r.out.on12.Get(), g.composed.Get());
            Barrier(cmd12, g.composed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    if (const HRESULT hr = cmd12->Close(); FAILED(hr))
    {
        Log("opengl: closing work slot %u failed (0x%08lX); discarding the unsubmitted job and "
            "recreating the pair.", slot, hr);
        ResetJobs();
        g.activePasses = 0;
        g.historyValid.store(false);
        if (!RecreateWorkSlot(slot))
            r.Stop("the D3D12 work slot could not be recovered after Close failed.");
        return;
    }
    ID3D12CommandList *lists[] { cmd12 };
    g.bridge.workQueue->ExecuteCommandLists(1, lists);
    if (!transportOnly && g.activePasses != 0)
        NotifyRuntimes(g.bridge.workQueue.Get(), 1, lists);
    g.ringValue[slot] = ++g.ringSerial;
    g.bridge.workQueue->Signal(g.ringFence.Get(), g.ringSerial);
    g.completion = ++g.serial;
    g.bridge.workQueue->Signal(g.fence.Get(), g.completion);
    if (r.semaphores)
    {
        // The value GL waits for in BlitOut. A second signal rather than reusing g.fence,
        // because g.fence is not shared and only the exported one can cross.
        r.toGlValue = r.toGlValue + 1;
        g.bridge.workQueue->Signal(g.bridge.crossFence.Get(), r.toGlValue);
    }

    // Without the fences the CPU waits here; with them GL waits in BlitOut.
    if (!r.semaphores)
        WaitForWorkQueue(g.completion);

    // --- 3. and the finished image goes back over the host's ----------------------------------
    //
    // `fresh` means the network produced a new composition this frame. When it did not, the
    // result texture still holds the last one and showing it again is what keeps the effect from
    // strobing. The same reasoning is already written down for Network Output in
    // CompositionIsFresh: a held frame and a correction aimed at where the edges used to be are
    // different artefacts, and flickering between two different pictures is worse than either.
    if (fresh)
    {
        r.haveResult = true;
        ++r.freshFrames;
    }
    // Before the first result there is nothing to show but the game's own frame, and after a
    // network failure `fresh` is false with nothing new behind it -- in both cases the honest
    // thing is to leave the host's image alone.
    const bool show = (fresh || r.haveResult) && !g.noBackBuffer.load();
    if (show && !BlitOut(r))
    {
        r.Stop("the blit back into the default framebuffer reported a GL error.");
        return;
    }

    // The readbacks are the network's own measurements -- the flow probe and the residual. In
    // transport-only mode there is no network and netWidth/netHeight are zero, so there is
    // nothing to drain and no reason to go looking.
    if (!transportOnly)
        DrainReadbacks(g.netWidth, g.netHeight);

    if (!r.loggedFirstFrame)
    {
        r.loggedFirstFrame = true;
        Log("opengl: first full round trip done at %ux%u.", r.width, r.height);
    }
    if (++g.status.frame <= 3 || g.status.frame % 120 == 0)
        Log("frame %llu processed (%llu fresh)", static_cast<unsigned long long>(g.status.frame),
            static_cast<unsigned long long>(r.freshFrames));
}
