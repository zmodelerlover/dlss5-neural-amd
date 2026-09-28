// The guides the native D3D12 transport finds on the game's own device: which buffer is the
// scene depth, told apart by how often it is bound and cleared, and a copy taken before each
// clear because by present time PCSX2 has already wiped it.

namespace d3d12route {

// The D3D11 route's bind tally (guide_choice.h), for D3D12 depth, which had none: it picked by
// size alone. Separate because these are ID3D12Resource and because depth here is decided on
// clears as well as binds -- the buffer the game clears every frame is the one it draws the scene
// into.
struct D12Depth {
    ComPtr<ID3D12Resource> res;
    UINT binds = 0, clears = 0;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
auto& g_d12DepthTally = *new std::unordered_map<void*, D12Depth>; // never destroyed, as g is
UINT g_d12DepthCold = 0;

// One candidate's entry, created on first sight. Binds and clears both come through here, because
// a clear is evidence in its own right: an engine is allowed to clear a buffer it has not bound
// yet this present, and counting that only when the resource happened to be bound first left the
// buffer the game clears every frame looking like one it never clears.
D12Depth& TallyD12Depth(ID3D12Resource* native, const D3D12_RESOURCE_DESC& d) {
    D12Depth& slot = g_d12DepthTally[native];
    if (slot.res == nullptr) {
        slot.res = native; // ComPtr: takes a reference
        slot.width = static_cast<UINT>(d.Width);
        slot.height = d.Height;
        slot.format = d.Format;
    }
    return slot;
}

// Screen-shaped: the aspect within 3% of the swapchain's, and at least a ninth of its area.
//
// This is what "largest wins" was missing. A 2048x2048 R16 shadow map is larger than the scene
// depth of a game rendering 1129x706 into a 1920x1200 swapchain, and it won every time. An
// unknown swapchain size decides nothing rather than accepting everything, which is how a 1x1
// buffer used to get through the floor before the raster was known.
bool ScreenShaped(UINT64 w, UINT h, UINT screenW, UINT screenH) {
    if (w < kGuideFloor || h < kGuideFloor)
        return false;
    // The swapchain size is only known once the effect has been enabled once, and the default is
    // to start switched off. Rejecting everything until then would mean a fresh install never
    // finds a guide at all, which is the very thing the observation code says it exists to avoid.
    // The floor above is what keeps a 1x1 out; shape can only be judged once there is a shape to
    // judge against.
    if (screenW == 0 || screenH == 0)
        return true;
    const double aspect = static_cast<double>(w) / static_cast<double>(h);
    const double screen = static_cast<double>(screenW) / static_cast<double>(screenH);
    if (std::abs(aspect - screen) > 0.03 * screen)
        return false;
    return static_cast<double>(w) * static_cast<double>(h) >=
           static_cast<double>(screenW) * static_cast<double>(screenH) / 9.0;
}

// Which D3D12 buffer is the scene depth, decided once per present.
//
// Three things went wrong with "largest wins", all measured on Cyberpunk 2077 at 1129x706 into a
// 1920x1200 swapchain: it took a 2048x2048 shadow map; with a shape filter added it took a
// 1920x1200 buffer the game drew into 7 times a frame over the scene depth it drew into ~2,400
// times; and when two buffers were the same size it settled on the one the game never clears, so
// the pre-clear snapshot froze on a stale frame.
//
// So: count binds, count clears, and when anything screen-shaped was cleared this present, only
// cleared buffers are eligible. A challenger needs a clear margin over the incumbent, and with no
// incumbent three presents are added up first -- the same rule, and the same reason, as
// SettleGuide. Caller holds g.lock. OnPresent calls it on every route, as it always has: the tally
// holds references to the game's buffers, and settling is what lets go of them each present.
void SettleD3D12Depth() {
    bool anyCleared = false;
    for (const auto& entry : g_d12DepthTally)
        if (entry.second.clears > 0) {
            anyCleared = true;
            break;
        }

    const D12Depth* best = nullptr;
    for (const auto& entry : g_d12DepthTally) {
        if (anyCleared && entry.second.clears == 0)
            continue;
        if (best == nullptr || entry.second.binds > best->binds)
            best = &entry.second;
    }
    if (best == nullptr) {
        g_d12DepthTally.clear();
        return;
    }

    if (best->res.Get() == g.depthBest.Get()) {
        g.depthBestBinds = best->binds;
        g_d12DepthCold = 0;
        g_d12DepthTally.clear();
        return;
    }
    if (g.depthBest == nullptr) {
        if (++g_d12DepthCold < 3)
            return; // deliberately not cleared: three presents of binds add up
        g_d12DepthCold = 0;
    } else if (best->binds <= g.depthBestBinds + g.depthBestBinds / 4) {
        g_d12DepthTally.clear(); // no margin, so the incumbent keeps the slot
        return;
    }

    RearmGuideProbe(g.depthBest != nullptr && g.settings.useDepth.load()); // incumbent still set
    g.depthBest = best->res;
    g.depthWidth = best->width;
    g.depthHeight = best->height;
    g.depthFormat = best->format;
    g.depthBinds = g.depthBestBinds = best->binds;
    // The snapshot is deliberately NOT released here. A dispatch recorded into the game's command
    // list still reads it, and D3D12 does not keep a resource alive because an in-flight list
    // references it -- the rule EnsureResources spells out, and it waits for the queue before it
    // drops anything. This runs at the top of present with no such wait, and a job routinely spans
    // several presents. It is only no longer read: the next clear of the new buffer copies over it
    // (rebuilt if the size or format changed), and until then the network gets no depth.
    g.snapshotOf = nullptr;
    Log("depth (D3D12): taking %ux%u format %d, bound %u times a present and cleared %u",
        best->width, best->height, static_cast<int>(best->format), best->binds, best->clears);
    g_d12DepthTally.clear();
}

void TallyBoundDepth(resource res) {
    if (res.handle == 0)
        return;
    auto* native = reinterpret_cast<ID3D12Resource*>(res.handle);
    const auto guard = LockForHook();
    if (native == g.depthBest.Get())
        ++g.depthBinds; // the status line's running total, which outlives one present
    const auto d = native->GetDesc();
    static UINT seen = 0; // separate from the API-agnostic counter above
    const bool readable = DepthReadFormat(d.Format) != DXGI_FORMAT_UNKNOWN;
    const bool denied = (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0;
    if (seen < 8) {
        ++seen;
        Log("depth seen: %llux%u format %d flags 0x%x samples %u array %u -> %s",
            static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(d.Format),
            static_cast<unsigned>(d.Flags), d.SampleDesc.Count, d.DepthOrArraySize,
            (!readable ? "format not readable"
             : denied  ? "shader resource denied"
                       : "taken"));
    }
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1 ||
        d.DepthOrArraySize != 1 || !readable || denied)
        return;
    if (!ScreenShaped(d.Width, d.Height, g.outWidth, g.outHeight))
        return;
    ++TallyD12Depth(native, d).binds;
}

void SnapshotBeforeClear(command_list* cmd_list, device* dev, resource_view dsv) {
    const resource res = dev->get_resource_from_view(dsv);
    if (res.handle == 0)
        return;
    auto* native = reinterpret_cast<ID3D12Resource*>(res.handle);

    const auto guard = LockForHook();
    // Counted for whichever candidate this is, not only for the one holding the slot: "the game
    // clears it every frame" is how the scene depth is told from a buffer that is merely the same
    // size, and that has to be known about a challenger before it can win. Recorded rather than
    // looked up, so a clear that comes before this present's first bind still counts.
    {
        const auto cd = native->GetDesc();
        if (ScreenShaped(cd.Width, cd.Height, g.outWidth, g.outHeight) &&
            DepthReadFormat(cd.Format) != DXGI_FORMAT_UNKNOWN &&
            (cd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0 &&
            cd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && cd.SampleDesc.Count == 1 &&
            cd.DepthOrArraySize == 1)
            ++TallyD12Depth(native, cd).clears;
    }
    // Only on the device the snapshot and the network live on. This hook runs even once the add-on
    // has stood down, and a clear on the game's next device would be a copy across devices.
    if (!g.settings.useDepth.load() || native != g.depthBest.Get() ||
        g.device.Get() != reinterpret_cast<ID3D12Device*>(dev->get_native()))
        return;
    ++g.depthClears;

    const auto d = native->GetDesc();
    if (g.depthSnapshot == nullptr || g.depthSnapshot->GetDesc().Width != d.Width ||
        g.depthSnapshot->GetDesc().Height != d.Height ||
        g.depthSnapshot->GetDesc().Format != DepthAliasFormat(d.Format)) {
        // Parked, not released: last present's list may still read it and nothing here waits for
        // the queue. ponytail: one per size or format change, freed at the next raster change,
        // after EnsureResources' fence wait. Nor is it the snapshot of anything any more.
        g.snapshotOf = nullptr;
        if (g.depthSnapshot != nullptr)
            g.parked.push_back(std::move(g.depthSnapshot));
        auto sd = d;
        sd.Format = DepthAliasFormat(d.Format);
        // ALLOW_DEPTH_STENCIL, and not NONE. R32G8X24_TYPELESS without the flag is a plain
        // one-plane 64-bit texture; the game's depth-stencil resource is planar. CopyResource
        // between those two layouts is not a valid copy, and D3D12 does not refuse it -- it just
        // produces garbage. Every probe read on the result came back min -3e38, max 2e36, mean
        // NaN, which then sailed through the "min is not max, so it is real depth" check below and
        // was handed to the network as depth.
        sd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        sd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (sd.Format == DXGI_FORMAT_UNKNOWN)
            return;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &sd,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                     nullptr, IID_PPV_ARGS(&g.depthSnapshot)))) {
            Log("depth snapshot allocation failed (%llux%u format %d).",
                static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(sd.Format));
            return;
        }
        Log("depth snapshot: %llux%u, taken before each clear.",
            static_cast<unsigned long long>(d.Width), d.Height);
    }

    auto* cmd = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
    if (cmd == nullptr)
        return;
    // A clear needs the resource in DEPTH_WRITE, so that is the state it is in right now.
    Barrier(cmd, native, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, g.depthSnapshot.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(g.depthSnapshot.Get(), native);
    Barrier(cmd, g.depthSnapshot.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, native, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    // Provenance: of the pick (a new pick, or letting it go, drops it) and at most two presents old
    // is the only snapshot RecordNetwork hands over, so a game that stops clearing is no depth.
    g.snapshotOf = native;
    g.snapshotAt = g.status.frame;
    if (!g.loggedSnapshot) {
        g.loggedSnapshot = true;
        Log("depth: first snapshot copied before a clear. If the engine log still says depth off, "
            "the copy is happening but the engine is refusing the resource.");
    }
    return;
}

} // namespace d3d12route
