// The OpenGL transport: the port in FrameTransport.hpp over the route in gl_route.inc.

namespace glroute
{

// OpenGL. Same answer as Vulkan -- our own D3D12 device, shared textures imported into the host --
// with the crossing rebuilt around a framebuffer blit, because ReShade hands an OpenGL add-on the
// default framebuffer rather than a texture and there is nothing to copy.
struct Transport final : FrameTransport
{
    const char *Name() const override { return "OpenGL"; }
    void Present(device *, command_queue *queue, swapchain *sc) override
    {
        if (CrossingReady(sc))
            glroute::Present(queue, sc);
    }
    // The imported GL textures and their FBOs have the same lifetime as the D3D12 resources the
    // core drops. Unlike the Vulkan route this one may be called with no GL context on the thread
    // -- a swapchain can be destroyed from anywhere -- and it checks for that itself.
    void ReleaseSwapchainSized() override { glroute::ReleaseSwapchainSized(); }
};
Transport transport;

}  // namespace glroute
