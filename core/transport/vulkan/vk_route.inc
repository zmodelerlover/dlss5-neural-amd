// The Vulkan route: the same bridge as D3D11, with the crossing running the other way.
//
// On D3D11 the shared textures are created on the game's device and opened on ours, because
// D3D11 and D3D12 both speak the same shared-handle dialect. Vulkan does not: a handle exported
// from Vulkan is opaque memory, and D3D12 cannot open it. So the direction flips -- the textures
// and the fences are created on OUR D3D12 device, exported as NT handles, and imported into the
// host's VkDevice as VkImage. That is not a preference, it is the only way round that works, and
// it is what every working DLSS 5 Vulkan add-on does.
//
// Two things keep this smaller than the reference implementations:
//
//   * The common path swaps a single import-table pointer. Hosts that resolve Vulkan dynamically
//     need the inline fallback in vk_devicehook.inc, since the interop extensions must be
//     appended before the device exists and ReShade does not add them itself.
//   * They own the submission. We record into ReShade's own immediate command buffer and let
//     ReShade submit it, so there is no command pool, no queue family to match, and no second
//     submission racing the host's.
//
// The synchronisation is the CPU-side stall the D3D11 route already uses (FlushAndWait11):
// flush, wait for idle, then hand over. Slower than a fence pair and a great deal easier to be
// sure of, and the D3D11 route has run ten thousand frames on it. The imported fences are built
// anyway because the async version needs them and because their import is the single best test
// that the interop is real.
//
// The layout rule, which is silent when broken: the imported image leaves VK_IMAGE_LAYOUT_
// UNDEFINED exactly once, at build time, and stays VK_IMAGE_LAYOUT_GENERAL forever. A transition
// out of UNDEFINED may discard the contents, so doing it per frame would throw away the frame
// that was just copied in and leave no error anywhere.

namespace vkroute
{

using namespace vkraw;

#include "vk_devicehook.inc"
#include "vk_crossing.inc"

// Everything that only has to happen once per host VkDevice, plus the resize path. Returns false
// with a reason already logged.
bool Ensure(Route &r, device *dev, command_queue *queue, command_list *immediate, UINT w, UINT h,
            DXGI_FORMAT fmt)
{
    if (r.failed)
        return false;

    if (r.device == nullptr)
    {
        r.device = reinterpret_cast<VkDevice>(dev->get_native());
        if (r.device == nullptr)
        {
            r.Stop("ReShade reported no native VkDevice.");
            return false;
        }
        if (!r.api.LoadCore())
        {
            r.Stop("vulkan-1.dll or its core entry points are missing.");
            return false;
        }
        if (!r.api.LoadDevice(r.device))
        {
            // The exact symptom of a device created without the interop extensions. ReShade's own
            // Vulkan layer enables them, so this means either ReShade is not the layer in front of
            // this device or that changed in a newer build.
            r.Stop("the host's VkDevice has no external-memory/semaphore entry points -- "
                   "vkImportSemaphoreWin32HandleKHR is missing. The device was created without "
                   "the KHR interop extensions, and after creation that cannot be fixed.");
            return false;
        }
        Log("vulkan: host VkDevice %p, interop entry points resolved.",
            static_cast<void *>(r.device));
        // Once. A device the host makes after destroying one is on the same GPU, and a second
        // instance would be one more never destroyed. ponytail: a host that moves to another GPU
        // has its imports refused, and the route stands down.
        if (r.gpu == nullptr && !FindPhysicalDevice(r))
        {
            r.Stop("could not identify the physical device.");
            return false;
        }
    }

    if (g.bridge.workDevice == nullptr)
    {
        ComPtr<IDXGIAdapter> adapter;
        if (!AdapterForLuid(r.luid, adapter))
        {
            r.Stop("no DXGI adapter carries the Vulkan device's LUID. D3D12 and Vulkan would be "
                   "different GPUs, and nothing can be shared between them.");
            return false;
        }
        if (!CreateWorkDevice(adapter.Get(), r.luid))
        {
            r.Stop("the work D3D12 device did not come up.");
            return false;
        }
        g.device = g.bridge.workDevice;
        g.queue = g.bridge.workQueue;
        // The completion fence, the one this route waits on before copying the result back.
        // CreateWorkDevice makes the ring fence and the two shared ones; this one is created by
        // the D3D11 route inside its own frame function, so the Vulkan route never had it -- and
        // Signal on a null fence is not a wait, it is nothing, so the copy back would have raced
        // the network with no error anywhere to say so.
        if (FAILED(g.bridge.workDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
        {
            r.Stop("could not create the completion fence.");
            return false;
        }
    }
    // Into every VkDevice the host makes: the pair on the last one died with it, and a work device
    // another route made first never had them imported at all.
    if (r.semToVk == nullptr &&
        (!ImportFence(r, g.bridge.crossHandle, r.semToVk, "D3D12 -> Vulkan") ||
         !ImportFence(r, g.bridge.backHandle, r.semToD3D, "Vulkan -> D3D12")))
    {
        r.Stop("the shared fences would not import.");
        return false;
    }

    const bool crossingsReady = r.in.image != nullptr && r.in.memory != nullptr &&
                                r.in.on12 != nullptr && r.out.image != nullptr &&
                                r.out.memory != nullptr && r.out.on12 != nullptr;
    if (r.ready && r.width == w && r.height == h && r.format == fmt &&
        crossingsReady && g.bridge.crossLocal != nullptr)
        return true;

    if (r.ready && r.width == w && r.height == h && r.format == fmt)
        Log("vulkan: route was marked ready after a swapchain teardown but one or more shared "
            "resources are gone; rebuilding it.");

    const int vkFmt = VkFormatFor(fmt);
    if (vkFmt == 0)
    {
        Log("vulkan: swapchain format DXGI %d has no Vulkan equivalent here. Guessing one would "
            "reorder channels silently, so the route stops instead.", static_cast<int>(fmt));
        r.Stop("unsupported swapchain format.");
        return false;
    }

    r.ready = false;
    auto *cmd = reinterpret_cast<VkCommandBuffer>(immediate->get_native());
    if (cmd == nullptr)
    {
        r.Stop("ReShade gave no immediate command buffer.");
        return false;
    }
    // Submitted and waited on failure too: a barrier already recorded on r.in must be done before
    // the teardown destroys that image, and the teardown does not wait on the host's device.
    const bool crossed = BuildCrossing(r, r.in, w, h, fmt, vkFmt, cmd, "colour-in") &&
                         BuildCrossing(r, r.out, w, h, fmt, vkFmt, cmd, "result-out");
    queue->flush_immediate_command_list();
    queue->wait_idle();
    if (!crossed)
    {
        r.Stop("the shared textures would not cross.");
        return false;
    }

    // Our private copy, on our device, so the network never reads a shared resource directly --
    // same reason the D3D11 route keeps crossLocal.
    g.bridge.crossLocal.Reset();
    if (!CreateTexture(w, h, fmt, g.bridge.crossLocal, "crossLocal",
                       D3D12_RESOURCE_STATE_COPY_DEST))
    {
        r.Stop("could not create the private copy of the frame.");
        return false;
    }

    r.width = w;
    r.height = h;
    r.format = fmt;
    r.vkFormat = vkFmt;
    r.ready = true;
    if (!r.loggedReady)
    {
        r.loggedReady = true;
        Log("vulkan: bridge up at %ux%u. Textures cross as D3D12 resources imported into the "
            "host's VkDevice; ReShade's own command buffer carries the copies.", w, h);
    }
    return true;
}

// One frame. Copy the host's back buffer into the shared texture, run the network on our D3D12
// queue, copy the result back over the back buffer.
void Present(command_queue *queue, swapchain *sc)
{
    Route &r = g_route;
    devicehook::ReportOnce();
    device *dev = sc->get_device();

    const command_queue_type queueType = queue->get_type();
    if ((queueType & command_queue_type::graphics) == static_cast<command_queue_type>(0))
    {
        if (!r.loggedUnsupportedQueue)
        {
            r.loggedUnsupportedQueue = true;
            Log("vulkan: present arrived on a non-graphics queue (flags 0x%X). ReShade does not "
                "provide an immediate command list for this queue, so this frame is skipped. "
                "For DOOM Eternal, set r_presentFromAsync to 0.",
                static_cast<unsigned>(queueType));
        }
        return;
    }

    command_list *immediate = queue->get_immediate_command_list();
    if (immediate == nullptr)
    {
        if (!r.loggedUnsupportedQueue)
        {
            r.loggedUnsupportedQueue = true;
            Log("vulkan: the graphics present queue returned no immediate command list; this "
                "frame is skipped instead of dereferencing a null pointer.");
        }
        return;
    }
    if (!r.loggedQueue)
    {
        r.loggedQueue = true;
        Log("vulkan: present queue flags 0x%X; graphics immediate command list available.",
            static_cast<unsigned>(queueType));
    }

    const resource back = sc->get_current_back_buffer();
    if (back.handle == 0)
        return;
    const resource_desc bd = dev->get_resource_desc(back);
    if (bd.texture.samples != 1)
        return;

    // Resolved once, here, so the D3D12 side of the crossing, the private copy, the working
    // textures and compose all agree on one concrete format. A typeless swapchain is legal on the
    // D3D12 side and impossible on the Vulkan one; carrying it further would mean the two sides of
    // the same memory disagreeing about what is in it.
    if (!Ensure(r, dev, queue, immediate, bd.texture.width, bd.texture.height,
                ColourReadFormat(static_cast<DXGI_FORMAT>(bd.texture.format))))
        return;

    // The working textures. Both of the other routes call this and this one did not, so
    // netColour, netBase, netResidual, netMotion, netDepth and composed were all null while
    // RecordNetwork wrote descriptors through them -- an access violation, not a failure. This
    // is the other half of the first Vulkan crash; bringing the engine up alone was never going
    // to be enough.
    if (!EnsureResources(r.width, r.height, r.format, EffectiveScale()))
    {
        g.status.unavailable = true;
        if (*g.status.reason == 0)
            g.status.reason = "could not create the working textures; see amd-nr.log";
        Log("vulkan: %s", g.status.reason);
        return;
    }

    // Same order the other two routes use, and the reason the first Vulkan run crashed: without
    // it g.runtime is null and RecordNetwork dereferences it at a fixed offset, which is an
    // access violation rather than a failure. The work device exists by here, which is what
    // InitPipeline and InitEngine need.
    UINT wanted = WantedPasses();
    if (!BringUpEngines(wanted))
    {
        g.status.unavailable = true;
        // Not over a reason the bring-up already gave: mochizuki's own words (MzOff), or the file
        // the hash refusal names. "could not bring the engine up" names nothing.
        if (*g.status.reason == 0)
        {
            g.status.reason = "could not bring the engine up on the Vulkan bridge";
            Log("vulkan: %s", g.status.reason);
        }
        return;
    }
    g.loadedPasses = wanted;

    // The gate every route shares. Our own queue is drained every frame further down, but the
    // engine's HIP worker can outlast it (its watchdog lets the list go on), and starting a second
    // evaluation on top of one still running is how this hangs rather than skips.
    const bool runNetwork = JobGate();

    auto *backImage = reinterpret_cast<VkImage>(back.handle);
    VkImageCopy region {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { r.width, r.height, 1 };

    // --- 1. the host's frame comes over ---------------------------------------------------
    // ReShade's barrier() moves the host's own image, because ReShade is what knows the layout
    // it is in; the copy itself is raw Vulkan on the same command buffer.
    {
        command_list *list = immediate;
        auto *cmd = reinterpret_cast<VkCommandBuffer>(list->get_native());
        if (cmd == nullptr)
            return;
        list->barrier(back, resource_usage::present, resource_usage::copy_source);
        r.api.cmdCopyImage(cmd, backImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.in.image,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        list->barrier(back, resource_usage::copy_source, resource_usage::present);
        queue->flush_immediate_command_list();
        queue->wait_idle();
    }

    // --- 2. our device does the work ------------------------------------------------------
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
        Log("vulkan: work slot %u reset failed (allocator 0x%08lX, list 0x%08lX); "
            "recreating the pair.", slot, allocatorHr, listHr);
        if (!RecreateWorkSlot(slot))
            r.Stop("the D3D12 work slot could not be recovered.");
        return;
    }
    auto *cmd12 = g.list[slot].Get();
    cmd12->CopyResource(g.bridge.crossLocal.Get(), r.in.on12.Get());
    Barrier(cmd12, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool ok = RecordNetwork(cmd12, g.bridge.crossLocal.Get(), r.format, nullptr, runNetwork,
                                  g.loadedPasses,
                                  [&]() { return SubmitPrivatePass(cmd12, g.alloc[slot].Get()); });
    if (!ok && g.status.failed)
        return;
    Barrier(cmd12, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    // Freshness gate, same as the other two routes: on a frame where the network was skipped the
    // residual belongs to an older picture, and the host is better served by its own image than
    // by a correction aimed at where things used to be.
    const bool fresh = ok && CompositionIsFresh(runNetwork);
    if (fresh)
    {
        Barrier(cmd12, g.composed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd12->CopyResource(r.out.on12.Get(), g.composed.Get());
        Barrier(cmd12, g.composed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (const HRESULT hr = cmd12->Close(); FAILED(hr))
    {
        Log("vulkan: closing work slot %u failed (0x%08lX); discarding the unsubmitted job and "
            "recreating the pair.", slot, hr);
        RetireUnsubmitted(g.bridge.workQueue.Get(), cmd12);
        if (!RecreateWorkSlot(slot))
            r.Stop("the D3D12 work slot could not be recovered after Close failed.");
        return;
    }
    ID3D12CommandList *lists[] { cmd12 };
    g.bridge.workQueue->ExecuteCommandLists(1, lists);
    if (g.activePasses != 0)
        NotifyRuntimes(g.bridge.workQueue.Get(), 1, lists);
    g.ringValue[slot] = ++g.ringSerial;
    g.bridge.workQueue->Signal(g.ringFence.Get(), g.ringSerial);
    g.completion = ++g.serial;
    g.bridge.workQueue->Signal(g.fence.Get(), g.completion);
    ArmStallWatch();
    if (!WaitForWorkQueue(g.completion))
        return;  // gave up and stood down: r.out.image may still be being written

    // --- 3. and the finished image goes back over the host's -------------------------------
    if (fresh && !g.noBackBuffer.load())
    {
        command_list *list = immediate;
        auto *cmd = reinterpret_cast<VkCommandBuffer>(list->get_native());
        if (cmd != nullptr)
        {
            list->barrier(back, resource_usage::present, resource_usage::copy_dest);
            r.api.cmdCopyImage(cmd, r.out.image, VK_IMAGE_LAYOUT_GENERAL, backImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            list->barrier(back, resource_usage::copy_dest, resource_usage::present);
            queue->flush_immediate_command_list();
            queue->wait_idle();
        }
    }

    DrainReadbacks(g.netWidth, g.netHeight);

    if (!r.loggedFirstFrame)
    {
        r.loggedFirstFrame = true;
        Log("vulkan: first full round trip done at %ux%u.", r.width, r.height);
    }
    if (++g.status.frame <= 3 || g.status.frame % 120 == 0)
        Log("frame %llu processed (%llu skipped)", static_cast<unsigned long long>(g.status.frame),
            static_cast<unsigned long long>(g.status.skipped));
}

}  // namespace vkroute
