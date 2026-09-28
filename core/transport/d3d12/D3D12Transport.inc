// The native D3D12 transport: the game's own device runs the network, so there is nothing to
// cross. The port is in FrameTransport.hpp.

namespace d3d12route {

// Same frame means the game waits for the network. The runtime only makes the GPU queue wait -- a
// spin in the game's own list -- and this route never held the CPU, so with DXGI's three frames of
// latency every present recorded during that spin found the job pending in JobGate and went out raw
// (Rollout Inline: 13325 of 17285). WaitForPreviousJob (runtimes.inc) is the hold that fixed it.

struct Transport final : FrameTransport {
    const char* Name() const override { return "D3D12"; }
    // Depth only as SnapshotBeforeClear's copy: no velocity tally, and no Feed.fx, here yet. The
    // copy is the game's buffer, so GameGuides=0 (ini only on this route: the panel's Read from the
    // game is D3D11's) stops it and the tally behind it, as it stops the D3D11 route looking at all;
    // the copy already taken goes stale after two presents and is no longer handed over.
    uint32_t Caps() const override { return kDepth; }
    void OnTargetsBound(device*, uint32_t, const resource_view*, resource depth) override {
        if (g.settings.useGameGuides.load())
            TallyBoundDepth(depth);
    }
    void OnDepthCleared(command_list* cmd, device* dev, resource_view dsv) override {
        if (g.settings.useGameGuides.load())
            SnapshotBeforeClear(cmd, dev, dsv);
    }
    // g.device and the runtime hold references, so the old device outlives this and a new one never
    // takes its address: without this event, the compare in Present still skips the new one.
    void OnDestroyDevice(device* dev) override {
        if (g.device.Get() == reinterpret_cast<ID3D12Device*>(dev->get_native()))
            DeviceGone("the game destroyed its D3D12 device; restart the game to re-enable");
    }
    void Present(device* dev, command_queue* queue, swapchain* sc) override {
        if (!LoadGraphicsApi()) {
            g.status.unavailable = true;
            g.status.reason = "the D3D12 or DXGI entry points could not be resolved";
            return;
        }
        auto* const device12 = reinterpret_cast<ID3D12Device*>(dev->get_native());
        if (g.device == nullptr)
            g.device = device12;
        if ((queue->get_type() & command_queue_type::graphics) ==
            static_cast<command_queue_type>(0))
            return;
        auto* const queue12 = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
        if (g.queue == nullptr)
            g.queue = queue12;
        // Latched once: the heap, the fence and the runtime are on these two. The device being
        // destroyed or removed stands the add-on down before a present gets here.
        if (Foreign(g.device.Get() != device12 || g.queue.Get() != queue12,
                    "D3D12 device or queue"))
            return;
        command_list* cmd_list = queue->get_immediate_command_list();
        if (cmd_list == nullptr)
            return;
        UINT wanted = WantedPasses();
        if (!BringUpEngines(wanted)) {
            g.status.unavailable = true;
            if (*g.status.reason == 0) {
                g.status.reason = "could not bring the engine up";
                Log("off: %s", g.status.reason);
            }
            return;
        }
        g.loadedPasses = wanted;

        const resource back = sc->get_current_back_buffer();
        if (back.handle == 0)
            return;
        auto* backbuffer = reinterpret_cast<ID3D12Resource*>(back.handle);
        const auto bd = backbuffer->GetDesc();
        if (bd.SampleDesc.Count != 1 || bd.DepthOrArraySize != 1 ||
            bd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            return;
        if (!EnsureResources(static_cast<UINT>(bd.Width), bd.Height, bd.Format, EffectiveScale())) {
            g.status.unavailable = true;
            if (*g.status.reason == '\0')
                g.status.reason = "could not create the working textures; see amd-nr.log";
            return;
        }

        if (g.fence == nullptr &&
            FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) {
            g.status.unavailable = true;
            g.status.reason = "could not create the completion fence";
            Log("off: %s", g.status.reason);
            return;
        }
        const bool runNetwork = JobGate();

        auto* cmd = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
        const resource backRes = back;
        {
            resource_usage a = resource_usage::present, b = resource_usage::shader_resource;
            cmd_list->barrier(1, &backRes, &a, &b);
        }
        // A failure goes through the tail below too: only the tail puts the back buffer back in
        // present and signals the fence a teardown waits on for this list.
        const bool ok = RecordNetwork(cmd, backbuffer, bd.Format, nullptr, runNetwork, wanted, [&]() {
                ID3D12CommandList* submitted[]{cmd};
                queue->flush_immediate_command_list();
                NotifyRuntimes(g.queue.Get(), 1, submitted);
                if (!FinishSubmittedPass())
                    return false;
                cmd_list = queue->get_immediate_command_list();
                if (cmd_list == nullptr)
                    return false;
                cmd = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
                return cmd != nullptr;
            });
        // The one exit that skips the tail, left in shader_resource: only if a graphics queue hands
        // back no immediate list after a mid-pass flush, which it never does.
        if (cmd_list == nullptr)
            return;
        if (ok && CompositionIsFresh(runNetwork)) {
            {
                resource_usage a = resource_usage::shader_resource, b = resource_usage::copy_dest;
                cmd_list->barrier(1, &backRes, &a, &b);
            }
            // RecordNetwork leaves composed readable by shaders, not as a copy source.
            Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyResource(backbuffer, g.composed.Get());
            Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            {
                resource_usage a = resource_usage::copy_dest, b = resource_usage::present;
                cmd_list->barrier(1, &backRes, &a, &b);
            }
        } else {
            // Skipped or failed frame: the game's own image goes out untouched. The back buffer was
            // put into shader_resource on the way in and has to reach present either way.
            resource_usage a = resource_usage::shader_resource, b = resource_usage::present;
            cmd_list->barrier(1, &backRes, &a, &b);
        }
        // Executed before the runtime is told, as on every other route: the runtime's own detour
        // executes the list before notifying (the second change in tools/runtime-patches.json).
        const UINT nw = g.netWidth, nh = g.netHeight;
        ID3D12CommandList* submitted[]{cmd};
        queue->flush_immediate_command_list();
        if (g.activePasses != 0)
            NotifyRuntimes(g.queue.Get(), 1, submitted);
        DrainReadbacks(nw, nh);
        g.completion = ++g.serial;
        if (FAILED(g.queue->Signal(g.fence.Get(), g.completion))) {
            g.status.failed = true;
            Log("completion fence Signal failed. Stopping.");
        }

        if (++g.status.frame <= 3 || g.status.frame % 120 == 0)
            Log("frame %llu processed (%llu skipped)",
                static_cast<unsigned long long>(g.status.frame),
                static_cast<unsigned long long>(g.status.skipped));

        // One line, once, so a log tells us whether depth is even reachable on this API.
        if (g.status.frame == 600)
            Log("guides after 600 frames: %llu depth-stencil bind events, best candidate %s. Motion: "
                "the PS2 never computed per-pixel motion, so there is none to take.",
                static_cast<unsigned long long>(g.depthEvents.load()),
                g.depthBest != nullptr ? "found" : "none");
    }
};
Transport transport;

} // namespace d3d12route
