// Every transport compiled into this build, and the factory that picks one. See transport.h.

// The guard every transport that crosses to our own D3D12 device runs first. Between a teardown
// and the new swapchain being announced there is nothing safe to touch, and without the D3D12
// entry points there is nothing to cross to.
bool CrossingReady(swapchain *sc)
{
    if (g.noBridge.load() || g.goneSwapchain.load() == sc)
        return false;
    if (LoadGraphicsApi())
        return true;
    g.status.unavailable = true;
    g.status.reason = "the D3D12 or DXGI entry points could not be resolved";
    return false;
}

#include "d3d12_guides.inc"
#include "transport_d3d12.inc"
#include "d3d11_guides.inc"
#include "transport_d3d11.inc"
#if AMDNR_WITH_VULKAN
#include "vk_route.inc"
#include "transport_vulkan.inc"
#endif
#if AMDNR_WITH_OPENGL
#include "gl_route.inc"
#include "transport_opengl.inc"
#endif

// The one place that asks which API this is.
FrameTransport *TransportFor(device_api api)
{
    switch (api)
    {
    case device_api::d3d12:
        return &d3d12route::transport;
    case device_api::d3d11:
        return &d3d11route::transport;
#if AMDNR_WITH_VULKAN
    case device_api::vulkan:
        return &vkroute::transport;
#endif
#if AMDNR_WITH_OPENGL
    case device_api::opengl:
        return &glroute::transport;
#endif
    default:
        return nullptr;
    }
}

std::span<FrameTransport *const> AllTransports()
{
    // OpenGL first: the order the core released them in before they were transports.
    static FrameTransport *const all[] {
        &d3d12route::transport,
        &d3d11route::transport,
#if AMDNR_WITH_OPENGL
        &glroute::transport,
#endif
#if AMDNR_WITH_VULKAN
        &vkroute::transport,
#endif
        nullptr,  // so the array is never empty; not counted
    };
    return { all, std::size(all) - 1 };
}
