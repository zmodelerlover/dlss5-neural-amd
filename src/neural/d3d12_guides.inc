// The guides the native D3D12 transport finds on the game's own device: which buffer is the
// scene depth, told apart by how often it is bound and cleared, and a copy taken before each
// clear because by present time PCSX2 has already wiped it.

namespace d3d12route
{

void TallyBoundDepth(resource res)
{
    if (res.handle == 0)
        return;
    auto *native = reinterpret_cast<ID3D12Resource *>(res.handle);
    std::lock_guard guard(g.lock);
    if (native == g.depthBest.Get())
        ++g.depthBinds;  // the status line's running total, which outlives one present
    const auto d = native->GetDesc();
    static UINT seen = 0;  // separate from the API-agnostic counter above
    const bool readable = DepthReadFormat(d.Format) != DXGI_FORMAT_UNKNOWN;
    const bool denied = (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0;
    if (seen < 8)
    {
        ++seen;
        Log("depth seen: %llux%u format %d flags 0x%x samples %u array %u -> %s",
            static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(d.Format),
            static_cast<unsigned>(d.Flags), d.SampleDesc.Count, d.DepthOrArraySize,
            (!readable ? "format not readable" : denied ? "shader resource denied" : "taken"));
    }
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1 ||
        d.DepthOrArraySize != 1 || !readable || denied)
        return;
    if (!ScreenShaped(d.Width, d.Height, g.outWidth, g.outHeight))
        return;
    ++TallyD12Depth(native, d).binds;
}

void SnapshotBeforeClear(command_list *cmd_list, device *dev, resource_view dsv)
{
    const resource res = dev->get_resource_from_view(dsv);
    if (res.handle == 0)
        return;
    auto *native = reinterpret_cast<ID3D12Resource *>(res.handle);

    std::lock_guard guard(g.lock);
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
    if (!g.settings.useDepth.load() || native != g.depthBest.Get() || g.device == nullptr)
        return;
    ++g.depthClears;

    const auto d = native->GetDesc();
    if (g.depthSnapshot == nullptr || g.depthSnapshot->GetDesc().Width != d.Width ||
        g.depthSnapshot->GetDesc().Height != d.Height)
    {
        g.depthSnapshot.Reset();
        auto sd = d;
        sd.Format = DepthAliasFormat(d.Format);
        // ALLOW_DEPTH_STENCIL, and not NONE, for the same reason the live-buffer alias path uses
        // it. R32G8X24_TYPELESS without the flag is a plain one-plane 64-bit texture; the game's
        // depth-stencil resource is planar. CopyResource between those two layouts is not a valid
        // copy, and D3D12 does not refuse it -- it just produces garbage. Every probe read on the
        // result came back min -3e38, max 2e36, mean NaN, which then sailed through the "min is
        // not max, so it is real depth" check below and was handed to the network as depth.
        sd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        sd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (sd.Format == DXGI_FORMAT_UNKNOWN)
            return;
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(g.device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                nullptr, IID_PPV_ARGS(&g.depthSnapshot))))
        {
            Log("depth snapshot allocation failed (%llux%u format %d).",
                static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(sd.Format));
            return;
        }
        Log("depth snapshot: %llux%u, taken before each clear.",
            static_cast<unsigned long long>(d.Width), d.Height);
    }

    auto *cmd = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native());
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
    if (!g.loggedSnapshot)
    {
        g.loggedSnapshot = true;
        Log("depth: first snapshot copied before a clear. If the engine log still says depth off, "
            "the copy is happening but the engine is refusing the resource.");
    }
    return;
}

}  // namespace d3d12route
