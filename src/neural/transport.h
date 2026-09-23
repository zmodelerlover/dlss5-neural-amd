#pragma once
// The port every graphics API goes through. The network always runs on D3D12; a transport is how
// one API's frame gets to it and back. One implementation per API, and TransportFor is the one
// place that reads device_api -- adding an API is a new transport and a line there, and nothing
// in the core asks which API it is on.
//
// A failure is the same contract on every transport: set g.status.unavailable and reason, and
// leave the game's frame exactly as the game drew it.
//
// Included inside neural.cpp's anonymous namespace, after reshade.hpp and <span>, like the rest
// of the add-on; the implementations live beside their routes.

struct FrameTransport
{
    virtual ~FrameTransport() = default;
    virtual const char *Name() const = 0;
    virtual void Present(reshade::api::device *dev, reshade::api::command_queue *queue,
                         reshade::api::swapchain *sc) = 0;
    // Everything sized to the swapchain. Called on every transport compiled in, not only the
    // active one: a swapchain can be torn down from anywhere, before any present chose one.
    virtual void ReleaseSwapchainSized() {}
    // DllMain. Some routes have to hook before the game creates its device.
    virtual void OnAddonLoad() {}
    virtual void OnAddonUnload(bool processExit) { (void)processExit; }
};

// nullptr for an API this build has no transport for.
FrameTransport *TransportFor(reshade::api::device_api api);

// Every transport compiled into this build, for the calls that are not about one device.
std::span<FrameTransport *const> AllTransports();
