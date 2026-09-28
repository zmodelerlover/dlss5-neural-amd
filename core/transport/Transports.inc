// Every transport compiled into this build, and the factory that picks one. See FrameTransport.hpp.

// The guard every transport that crosses to our own D3D12 device runs first. Between a teardown
// and the new swapchain being announced there is nothing safe to touch, and without the D3D12
// entry points there is nothing to cross to.
bool CrossingReady(swapchain* sc) {
    if (g.noBridge.load() || g.goneSwapchain.load() == sc)
        return false;
    if (LoadGraphicsApi())
        return true;
    g.status.unavailable = true;
    g.status.reason = "the D3D12 or DXGI entry points could not be resolved";
    return false;
}

// The game destroyed the device the D3D12 route runs the network on. The runtime was initialised on
// that device and a new one starts with none of it, so the add-on stands down until the game is
// restarted.
void DeviceGone(const char* reason) {
    g.status.unavailable = true;
    g.status.reason = reason;
    Log("off: %s", reason);
}

// The game destroyed the device a route that crosses to our own D3D12 device latched, as PCSX2 and
// RPCS3 do when they switch game or renderer. Our device, the runtime and the raster never depended
// on it, so only what crossed to it goes, and the next present on the new device brings the route
// up again. This drains our queue and drops everything sized to the swapchain, the latch on it and
// the history; the route forgets the game's device itself. False, with all of it left where it is,
// when the add-on is already down, our device was removed, or our queue did not drain: that wait
// stood it down (WaitFence), and what the GPU may still read is kept, as at every teardown.
bool GameDeviceGone(const char* what) {
    if (g.status.unavailable || g.status.failed || DeviceLost() || !WaitForWorkQueue(g.completion))
        return false;
    ReleaseSwapchainSized();
    // The primary was on that device. Taken again by the first swapchain to present on the new one,
    // should a game destroy its device without the swapchain's own teardown reaching us first.
    g.primarySwapchain.store(nullptr);
    ResetTemporal(what);
    Log("%s: what crossed to it is dropped, and the add-on comes back on the next present from the "
        "new one.", what);
    return true;
}

// A present from a device, or on a queue, other than the one a route latched: a swapchain made after
// the first was destroyed (PrimaryRoute sends a second window's out before it gets here), on another
// device or queue. Nothing built on the first serves it, so it goes out as the game drew it and the
// route carries on with its own; standing down would switch the played window off as well.
bool Foreign(bool foreign, const char* what) {
    static bool said = false;
    if (foreign && !said) {
        said = true;
        Log("a present on another %s than the one the add-on runs on goes out as the game drew "
            "it, as does every later one there. Said once.", what);
    }
    return foreign;
}

// The route the primary swapchain presents through. Kept when that swapchain goes, so a rebuild does
// not put the rows RouteCaps hides back on the panel for a frame.
FrameTransport* g_primaryRoute = nullptr;

// What a route reaches besides colour, in one line: logged when a route first drives the pipeline,
// and under the panel's Debug.
std::string RouteReach(const FrameTransport& route) {
    const uint32_t caps = route.Caps();
    std::string line = std::string(route.Name()) + " route: ";
    // What stands in for the game's vectors: the optical flow where FidelityFX is built in (the 64-bit
    // add-on), the estimator where it is not (the 32-bit bridge's host).
    const std::string guess = AMDNR_WITH_FFX ? "motion by optical flow" : "motion estimated";
    line += caps & FrameTransport::kGameGuides
                ? "depth and motion from the game's own buffers when it renders them (Read from "
                  "the game), " + guess + " otherwise"
            : caps & FrameTransport::kDepth
                ? "depth copied just before the game clears it; " + guess + ": no game MV path on this API"
                : "no depth path on this API; " + guess + ": no game MV path on this API";
    line += caps & FrameTransport::kFeed ? "; Feed.fx can hand over its own"
                                         : "; Feed.fx does not reach the network here";
    if (g.stage.load() != 3 && !(caps & FrameTransport::kStage))
        line += "; Stage=" + std::to_string(g.stage.load()) + " is not read on this route";
    if (g.noBridge.load() && !(caps & FrameTransport::kBridge))
        line += "; NoBridge=1 is not read on this route, which has no bridge";
    return line;
}

// The panel's capability bits, from the route and never a constant: a control for what the route
// cannot reach is not drawn (view_logic.cpp, AvailableOpts). None before the first present.
void RouteCaps(PanelStatus& st) {
    if (g_primaryRoute == nullptr)
        return;
    const uint32_t caps = g_primaryRoute->Caps();
    st.hasFeedEffect = (caps & FrameTransport::kFeed) != 0;
    st.hasGameGuides = (caps & FrameTransport::kGameGuides) != 0;
    st.hasDepth = (caps & FrameTransport::kDepth) != 0;
    st.routeDiagnostics.push_back(RouteReach(*g_primaryRoute));
}

// The route for this present, or nullptr when it is not ours to touch. One swapchain drives the
// pipeline: the first one a route gets, until the game destroys it for good (lifecycle.inc: a DXGI
// resize keeps it, and a Vulkan rebuild or an OpenGL restore takes it back). The alt-tab and
// minimised tests, the history drops and everything sized to the output are shared, and another
// window's presents run through them switched the effect off from the background, reset history on
// every frame, or rebuilt everything at that window's size.
FrameTransport* PrimaryRoute(device* dev, command_queue* queue, swapchain* sc) {
    FrameTransport* const transport = dev != nullptr ? TransportFor(dev->get_api()) : nullptr;
    if (dev != nullptr && transport == nullptr && !g.status.loggedWrongApi) {
        g.status.loggedWrongApi = true;
        Log("unsupported graphics API %u; transports compiled into this build: Vulkan %d, "
            "OpenGL %d", static_cast<unsigned>(dev->get_api()), AMDNR_WITH_VULKAN,
            AMDNR_WITH_OPENGL);
    }
    if (transport == nullptr || queue == nullptr)
        return nullptr;
    if (g.primarySwapchain.load() == nullptr)
        g.primarySwapchain.store(sc);
    if (sc == g.primarySwapchain.load()) {
        if (std::exchange(g_primaryRoute, transport) != transport)
            Log("%s. The panel draws no control for what it cannot reach.",
                RouteReach(*transport).c_str());
        return transport;
    }
    if (!g.status.loggedOtherSwapchain) {
        g.status.loggedOtherSwapchain = true;
        Log("a present on a second swapchain goes out as the game drew it: the add-on stays on "
            "the first one until the game destroys it. Said once.");
    }
    return nullptr;
}

#include "d3d12/D3D12Guides.inc"
#include "d3d12/D3D12Transport.inc"
#include "d3d11/D3D11Guides.inc"
#include "d3d11/D3D11Transport.inc"
#if AMDNR_WITH_VULKAN
#include "vulkan/vk_route.inc"
#include "vulkan/VulkanTransport.inc"
#endif
#if AMDNR_WITH_OPENGL
#include "opengl/gl_route.inc"
#include "opengl/OpenGLTransport.inc"
#endif

// The one place that asks which API this is.
FrameTransport* TransportFor(device_api api) {
    switch (api) {
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

std::span<FrameTransport* const> AllTransports() {
    // OpenGL first: the order the core released them in before they were transports.
    static FrameTransport* const all[]{
        &d3d12route::transport,
        &d3d11route::transport,
#if AMDNR_WITH_OPENGL
        &glroute::transport,
#endif
#if AMDNR_WITH_VULKAN
        &vkroute::transport,
#endif
        nullptr, // so the array is never empty; not counted
    };
    return {all, std::size(all) - 1};
}
