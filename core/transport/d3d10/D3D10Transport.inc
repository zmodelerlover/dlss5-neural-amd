// The D3D10 transport: the D3D11 one, with a hop in front. D3D10 shares textures by legacy handle
// only, which D3D12 cannot open, so the back buffer goes first to a D3D11 device of our own on the
// game's adapter (d3d10_stage.h), and from there over the D3D11 route's bridge as it stands, our
// device standing where the game's D3D11 device would. Colour only: the game's depth and motion
// are on its D3D10 device, which the D3D11 guide code cannot read.

namespace d3d10route {

// The game's device as ReShade hands it, compared against on every present and destroy_device.
ID3D10Device* gameDevice = nullptr;
bool loggedRemoved = false;

struct Devices {
    ComPtr<ID3D10Device> game10;
    // Ours, made once and never released: ReShade wraps a D3D11 device made in its process, and
    // its last release would re-enter destroy_device under g.lock (the 32-bit frontend's Retire
    // says the same of its private device). It does not depend on the game's device, so a new one
    // reuses it.
    ComPtr<ID3D11Device> own;
    ComPtr<ID3D11DeviceContext> ownCtx;
    d3d10stage::Stage stage;
};
Devices& d = *new Devices;  // never destroyed, as State g is: see neural.cpp

bool BringUp(ID3D10Device* native) {
    if (gameDevice != nullptr)
        return true;
    if (g.bridge.failed || native == nullptr)
        return false;
    if (d.own == nullptr) {
        LUID luid{};
        const HRESULT hr = d3d10stage::CreateDeviceBeside(native, d.own, d.ownCtx, luid);
        if (FAILED(hr) || d.own == nullptr) {
            Log("bridge: could not make a D3D11 device on the D3D10 game's adapter (0x%08lX).", hr);
            d.own.Reset();
            d.ownCtx.Reset();
            g.bridge.failed = true;
            return false;
        }
        Log("bridge: D3D10 game. A D3D11 device of ours on its adapter (LUID %08lX:%08lX) takes "
            "the frame from it, and the D3D11 route carries it from there: colour only.",
            luid.HighPart, luid.LowPart);
    }
    if (!d3d11route::OpenBridge(d.own.Get()))
        return false;
    gameDevice = native;
    d.game10 = native;
    return true;
}

struct Transport final : FrameTransport {
    const char* Name() const override { return "D3D10"; }
    uint32_t Caps() const override { return kStage | kBridge; }
    void ReleaseSwapchainSized() override { d.stage.Release(); }
    void OnDestroyDevice(device* dev) override {
        if (gameDevice == nullptr ||
            reinterpret_cast<ID3D10Device*>(dev->get_native()) != gameDevice ||
            !GameDeviceGone("the game destroyed its D3D10 device"))
            return;
        gameDevice = nullptr;
        d.game10.Reset();
        g.bridge.crossOn11.Reset();
        g.bridge.backOn11.Reset();
        g.bridge.failed = false;
        g.bridge.retries = 0;
        g.bridge.presentsSinceFrame = 0;
        g.loggedRound = loggedRemoved = false;
    }
    void Present(device* dev, command_queue*, swapchain* sc) override {
        if (g.noBridge.load() || g.goneSwapchain.load() == sc)
            return;
        auto* native = reinterpret_cast<ID3D10Device*>(dev->get_native());
        if (gameDevice != nullptr && Foreign(native != gameDevice, "D3D10 device"))
            return;
        if (!d3d11route::Watchdog() || !BringUp(native))
            return;
        if (FAILED(d.game10->GetDeviceRemovedReason())) {
            g.bridge.presentsSinceFrame = 0;
            if (!std::exchange(loggedRemoved, true))
                Log("the game's D3D10 device was removed; its frames go out as drawn until it "
                    "makes a new one, and the add-on comes back on that.");
            return;
        }
        if (!d3d11route::ReadyToCross())
            return;
        const resource back = sc->get_current_back_buffer();
        const resource_desc bd = back.handle != 0 ? dev->get_resource_desc(back) : resource_desc{};
        // No back buffer, or a multisampled one: BridgePresent says so and reads nothing.
        if (back.handle == 0 || bd.texture.samples > 1) {
            d3d11route::BridgePresent(dev, back, nullptr, false);
            return;
        }
        auto* back10 = reinterpret_cast<ID3D10Resource*>(back.handle);
        const auto fmt = static_cast<DXGI_FORMAT>(bd.texture.format);
        if (!d.stage.Ensure(d.game10.Get(), d.own.Get(), bd.texture.width, bd.texture.height, fmt)) {
            Log("bridge: the D3D10 back buffer (%ux%u fmt %u) could not be shared with our D3D11 "
                "device.",
                bd.texture.width, bd.texture.height, static_cast<unsigned>(fmt));
            g.bridge.failed = true;
            g.status.unavailable = true;
            g.status.reason = "the back buffer format is not shareable between D3D10 and D3D11";
            return;
        }
        const bool copy = !g.noBackBuffer.load();
        if (copy && !d.stage.Upload(d.game10.Get(), back10)) {
            Log("bridge: the game's D3D10 queue did not drain (query failed, 2 s timeout, or "
                "device removed).");
            g.bridge.failed = true;
            return;
        }
        const uint64_t before = g.status.frame;
        d3d11route::BridgePresent(dev, back, d.stage.shared11.Get(), false);
        // A frame that went through left the result, or the frame as drawn, in shared11.
        if (copy && g.status.frame != before && !d.stage.Download(d.game10.Get(), back10)) {
            Log("bridge: the result did not land in the D3D10 back buffer in time.");
            g.bridge.failed = true;
        }
    }
};
Transport transport;

} // namespace d3d10route
