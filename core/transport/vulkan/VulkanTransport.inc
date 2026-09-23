// The Vulkan transport: the port in FrameTransport.hpp over the route in vk_route.inc.

namespace vkroute
{

// Vulkan. The host -- RPCS3 is the one this was built for -- never makes a D3D12 call, so the
// network cannot run on its device. Same answer as D3D11: a second D3D12 device of our own, and
// shared textures between the two. The crossing runs the other way round, because memory exported
// from Vulkan is opaque and D3D12 cannot open it.
struct Transport final : FrameTransport
{
    const char *Name() const override { return "Vulkan"; }
    void Present(device *, command_queue *queue, swapchain *sc) override
    {
        if (CrossingReady(sc))
            vkroute::Present(queue, sc);
    }
    // The imported VkImages have the same lifetime as the D3D12 resources the core drops. Invalidate
    // the route even when the replacement swapchain keeps the same size and format, otherwise its
    // fast path returns with crossLocal already released.
    void ReleaseSwapchainSized() override { vkroute::ReleaseSwapchainSized(); }
    // Has to happen at load and not at the first present: the host's VkDevice is created when a
    // game boots, and by the time a frame is presented it is far too late to change what that
    // device was created with. Patches one import-table entry and does nothing at all in a process
    // that has no static vkCreateDevice import, which is every D3D11 and D3D12 target.
    void OnAddonLoad() override
    {
        if (!g.noBridge.load())
            devicehook::Install();
    }
    // At process termination Windows is already tearing every module down. MinHook removal
    // suspends threads, which is useful for an explicit unload but unsafe under the loader lock
    // while the process is exiting.
    void OnAddonUnload(bool processExit) override
    {
        if (!processExit)
            devicehook::Remove();
    }
};
Transport transport;

}  // namespace vkroute
