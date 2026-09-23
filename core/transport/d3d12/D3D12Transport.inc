// The native D3D12 transport: the game's own device runs the network, so there is nothing to
// cross. The port is in FrameTransport.hpp.

namespace d3d12route
{

struct Transport final : FrameTransport
{
    const char *Name() const override { return "D3D12"; }
    void OnTargetsBound(device *, uint32_t, const resource_view *, resource depth) override
    {
        TallyBoundDepth(depth);
    }
    void OnDepthCleared(command_list *cmd, device *dev, resource_view dsv) override
    {
        SnapshotBeforeClear(cmd, dev, dsv);
    }
    void Present(device *dev, command_queue *queue, swapchain *sc) override
    {
        if (!LoadGraphicsApi())
        {
            g.status.unavailable = true;
            g.status.reason = "the D3D12 or DXGI entry points could not be resolved";
            return;
        }
        if (g.device == nullptr)
            g.device = reinterpret_cast<ID3D12Device *>(dev->get_native());
        if ((queue->get_type() & command_queue_type::graphics) == static_cast<command_queue_type>(0))
            return;
        if (g.queue == nullptr)
            g.queue = reinterpret_cast<ID3D12CommandQueue *>(queue->get_native());
        command_list *cmd_list = queue->get_immediate_command_list();
        if (cmd_list == nullptr)
            return;
        UINT wanted = WantedPasses();
        if (!BringUpEngines(wanted))
        {
            g.status.unavailable = true;
            if (*g.status.reason == 0)
            {
                g.status.reason = "could not bring the engine up";
                Log("off: %s", g.status.reason);
            }
            return;
        }
        g.loadedPasses = wanted;

        const resource back = sc->get_current_back_buffer();
        if (back.handle == 0)
            return;
        auto *backbuffer = reinterpret_cast<ID3D12Resource *>(back.handle);
        const auto bd = backbuffer->GetDesc();
        if (bd.SampleDesc.Count != 1 || bd.DepthOrArraySize != 1 ||
            bd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            return;
        if (!EnsureResources(static_cast<UINT>(bd.Width), bd.Height, bd.Format, EffectiveScale()))
        {
            g.status.unavailable = true;
            if (*g.status.reason == '\0')
                g.status.reason = "could not create the working textures; see amd-nr.log";
            return;
        }

        if (g.fence == nullptr &&
            FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
        {
            g.status.unavailable = true;
            g.status.reason = "could not create the completion fence";
            Log("off: %s", g.status.reason);
            return;
        }
        bool runNetwork = true;
        const bool jobPending =
            g.fence->GetCompletedValue() < g.completion ||
            RuntimeBusy();
        if (!jobPending && g.jobRunning)
        {
            g.jobRunning = false;
            NoteJobCost(GetTickCount64() - g.lastJobAt);
        }
        if (jobPending)
        {
            if (GetTickCount64() - g.lastJobAt < 500)
            {
                runNetwork = false;
                if (++g.status.skipped % 120 == 1)
                    Log("network skipped: previous evaluation still pending (%llu skipped, %llu done). Those "
                        "frames go out as the game drew them; a correction aimed at an older picture "
                        "reads as a trail, not as detail.",
                        static_cast<unsigned long long>(g.status.skipped),
                        static_cast<unsigned long long>(g.status.frame));
            }
            else
            {
                if (g.status.skipped % 600 == 0)
                    Log("previous job did not finish in 500 ms; continuing anyway (%llu skipped)",
                        static_cast<unsigned long long>(g.status.skipped));
                ResetJobs();
            }
        }

        auto *cmd = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native());
        const resource backRes = back;
        {
            resource_usage a = resource_usage::present, b = resource_usage::shader_resource;
            cmd_list->barrier(1, &backRes, &a, &b);
        }
        if (!RecordNetwork(cmd, backbuffer, bd.Format, nullptr, runNetwork, wanted, [&]() {
            ID3D12CommandList *submitted[] {cmd};
            NotifyRuntimes(g.queue.Get(), 1, submitted);
            queue->flush_immediate_command_list();
            if (!FinishSubmittedPass())
                return false;
            cmd_list = queue->get_immediate_command_list();
            if (cmd_list == nullptr)
                return false;
            cmd = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native());
            return cmd != nullptr;
        }))
        {
            if (g.status.failed)
                return;
            resource_usage a = resource_usage::shader_resource, b = resource_usage::present;
            cmd_list->barrier(1, &backRes, &a, &b);
            queue->flush_immediate_command_list();
            return;
        }
        if (CompositionIsFresh(runNetwork))
        {
            {
                resource_usage a = resource_usage::shader_resource, b = resource_usage::copy_dest;
                cmd_list->barrier(1, &backRes, &a, &b);
            }
            cmd->CopyResource(backbuffer, g.composed.Get());
            {
                resource_usage a = resource_usage::copy_dest, b = resource_usage::present;
                cmd_list->barrier(1, &backRes, &a, &b);
            }
        }
        else
        {
            // Skipped frame: the game's own image goes out untouched. Same transition the early-out
            // above uses, because the back buffer was put into shader_resource on the way in and has
            // to reach present either way.
            resource_usage a = resource_usage::shader_resource, b = resource_usage::present;
            cmd_list->barrier(1, &backRes, &a, &b);
        }
        const UINT nw = g.netWidth, nh = g.netHeight;
        ID3D12CommandList *submitted[] { cmd };
        if (g.activePasses != 0)
            NotifyRuntimes(g.queue.Get(), 1, submitted);
        queue->flush_immediate_command_list();
        DrainReadbacks(nw, nh);
        g.completion = ++g.serial;
        if (FAILED(g.queue->Signal(g.fence.Get(), g.completion)))
        {
            g.status.failed = true;
            Log("completion fence Signal failed. Stopping.");
        }

        if (++g.status.frame <= 3 || g.status.frame % 120 == 0)
            Log("frame %llu processed (%llu skipped)", static_cast<unsigned long long>(g.status.frame),
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

}  // namespace d3d12route
