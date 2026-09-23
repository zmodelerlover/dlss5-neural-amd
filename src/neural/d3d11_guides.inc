// The guides the D3D11 transport hands the network: the game's own depth and motion, copied
// across to our device.

namespace d3d11route
{

// One frame across the bridge: the game's back buffer goes over, the network runs on our device,
// the finished image comes back. Both crossings are plain CopyResource -- a shared resource
// opened from another device sits in COMMON, which promotes to COPY_SOURCE and COPY_DEST on a
// direct queue, so there is no barrier here to get wrong.
// One frame of one guide, on the game's own device and context. The game's buffer is live and
// keeps being rewritten, so the value has to be taken now and taken by copy.
//
// Motion is the cheap case: an engine's velocity buffer is already R16G16_FLOAT, which opens on
// a second device, so it is a single CopyResource straight into the shared texture. Depth is
// not -- R32G8X24_TYPELESS and R24G8_TYPELESS refuse to open across devices -- so it goes
// through a private snapshot, a compute pass that reads the single float channel, and a shared
// R32_FLOAT. Both of those steps are ported from session.cpp, where the transport is measured.
bool PrepareGuide(Guide &guide, bool isDepth)
{
    if (guide.failed || guide.chosen == nullptr || g.bridge.game11 == nullptr || g.bridge.workDevice == nullptr)
        return false;
    const UINT w = guide.width, h = guide.height;
    if (w == 0 || h == 0)
        return false;

    if (!isDepth)
    {
        if (!guide.bridge.Ensure(g.bridge.game11.Get(), g.bridge.workDevice.Get(), w, h, guide.format))
        {
            guide.failed = true;
            Log("guide %s: format %u will not share between the devices; giving up on it.",
                guide.name, static_cast<unsigned>(guide.format));
            return false;
        }
        g.bridge.game11ctx->CopyResource(guide.bridge.on11.Get(), guide.chosen.Get());
        guide.ready = true;
        if (!guide.logged)
        {
            guide.logged = true;
            Log("guide %s: %ux%u format %u crossing to the network device, one copy per frame.",
                guide.name, w, h, static_cast<unsigned>(guide.format));
        }
        return true;
    }

    if (g.guideDepthCs == nullptr)
    {
        if (g.guideDepthCsFailed)
            return false;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(p_D3DCompile(shaders::kGuideDepthCs, sizeof(shaders::kGuideDepthCs) - 1, "guide-depth", nullptr,
                              nullptr, "main", "cs_5_0", 0, 0, &blob, &err)) ||
            FAILED(g.bridge.game11->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 nullptr, &g.guideDepthCs)))
        {
            g.guideDepthCsFailed = true;
            Log("guide depth: compute shader failed: %s",
                err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "?");
            return false;
        }
    }

    // The snapshot is never bound as a depth-stencil, so an SRV over it stays valid while the
    // game goes on binding and clearing the original.
    if (guide.snap == nullptr || guide.snapW != w || guide.snapH != h || guide.snapFmt != guide.format)
    {
        guide.srv.Reset();
        guide.srvOf = nullptr;
        guide.snap.Reset();
        D3D11_TEXTURE2D_DESC td {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = guide.format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        // BIND_DEPTH_STENCIL as well, for a depth format, so this is the same kind of resource as
        // the one about to be copied into it. A depth-stencil surface is planar and compressed;
        // the same typeless format without the flag is neither, and CopyResource between the two
        // is a copy across layouts. The D3D12 half of this add-on made exactly this mistake with
        // its pre-clear snapshot and every probe of the result came back min -3e38, max 2e36,
        // mean NaN -- garbage that was then handed to the network as depth. This is that same
        // copy, on the D3D11 bridge, and Darksiders 3 takes it: R24G8_TYPELESS at 2560x1440.
        //
        // Kept to depth formats: a motion guide is an ordinary two-channel colour target and the
        // flag would only make its creation fail.
        const bool isDepth = GuideDepthSrvFormat(guide.format) != DXGI_FORMAT_UNKNOWN;
        td.BindFlags = isDepth ? (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL)
                               : D3D11_BIND_SHADER_RESOURCE;
        HRESULT made = g.bridge.game11->CreateTexture2D(&td, nullptr, &guide.snap);
        if (FAILED(made) && isDepth)
        {
            // Some formats reach here that no driver will give a depth-stencil view of. Falling
            // back leaves the old behaviour rather than losing the guide outright, and says so,
            // because a copy on this path is then the suspect for anything odd downstream.
            Log("guide depth: %ux%u format %u was refused as a depth-stencil copy (0x%08lX); "
                "falling back to a plain shader-resource copy, which may not read correctly.",
                w, h, static_cast<unsigned>(guide.format), static_cast<unsigned long>(made));
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            made = g.bridge.game11->CreateTexture2D(&td, nullptr, &guide.snap);
        }
        if (FAILED(made))
        {
            guide.failed = true;
            Log("guide depth: private %ux%u copy of format %u could not be created.", w, h,
                static_cast<unsigned>(guide.format));
            return false;
        }
        guide.snapW = w;
        guide.snapH = h;
        guide.snapFmt = guide.format;
    }
    g.bridge.game11ctx->CopyResource(guide.snap.Get(), guide.chosen.Get());

    if (!guide.bridge.Ensure(g.bridge.game11.Get(), g.bridge.workDevice.Get(), w, h, DXGI_FORMAT_R32_FLOAT, true))
    {
        guide.failed = true;
        return false;
    }
    // Ensure builds a new texture on a resize, and a view left over from the old one writes into
    // an orphan while every read of the new one comes back zero. Rebuild whenever the resource
    // underneath changed, not just when the view is null.
    if (guide.uavOf != guide.bridge.on11.Get())
    {
        guide.uav.Reset();
        guide.uavOf = nullptr;
    }
    if (guide.uav == nullptr)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud {};
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(g.bridge.game11->CreateUnorderedAccessView(guide.bridge.on11.Get(), &ud, &guide.uav)))
        {
            guide.failed = true;
            Log("guide depth: UAV over the shared texture failed.");
            return false;
        }
        guide.uavOf = guide.bridge.on11.Get();
    }
    if (guide.srvOf != guide.snap.Get())
    {
        guide.srv.Reset();
        guide.srvOf = nullptr;
    }
    if (guide.srv == nullptr)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
        sd.Format = GuideDepthSrvFormat(guide.format);
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(g.bridge.game11->CreateShaderResourceView(guide.snap.Get(), &sd, &guide.srv)))
        {
            guide.failed = true;
            Log("guide depth: SRV over the snapshot failed (fmt %u read as %u).",
                static_cast<unsigned>(guide.format), static_cast<unsigned>(sd.Format));
            return false;
        }
        guide.srvOf = guide.snap.Get();
    }

    // Save exactly the three compute bindings this touches and put them back: the context
    // belongs to the game and it is mid-frame.
    ID3D11ComputeShader *oldCs = nullptr;
    ID3D11ShaderResourceView *oldSrv = nullptr;
    ID3D11UnorderedAccessView *oldUav = nullptr;
    g.bridge.game11ctx->CSGetShader(&oldCs, nullptr, nullptr);
    g.bridge.game11ctx->CSGetShaderResources(0, 1, &oldSrv);
    g.bridge.game11ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);

    UINT keep = static_cast<UINT>(-1);
    ID3D11ShaderResourceView *srv = guide.srv.Get();
    ID3D11UnorderedAccessView *uav = guide.uav.Get();
    g.bridge.game11ctx->CSSetShader(g.guideDepthCs.Get(), nullptr, 0);
    g.bridge.game11ctx->CSSetShaderResources(0, 1, &srv);
    g.bridge.game11ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
    g.bridge.game11ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    ID3D11ShaderResourceView *nullSrv = nullptr;
    ID3D11UnorderedAccessView *nullUav = nullptr;
    g.bridge.game11ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
    g.bridge.game11ctx->CSSetShaderResources(0, 1, &nullSrv);
    g.bridge.game11ctx->CSSetShader(oldCs, nullptr, 0);
    g.bridge.game11ctx->CSSetShaderResources(0, 1, &oldSrv);
    g.bridge.game11ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
    if (oldCs != nullptr)
        oldCs->Release();
    if (oldSrv != nullptr)
        oldSrv->Release();
    if (oldUav != nullptr)
        oldUav->Release();

    guide.ready = true;
    if (!guide.logged)
    {
        guide.logged = true;
        Log("guide depth: %ux%u format %u -> shared R32_FLOAT, one snapshot and one dispatch per "
            "frame. PS2 depth tops out near 0.002; a modern engine's fills the range.",
            w, h, static_cast<unsigned>(guide.format));
    }
    return true;
}

// Observation is deliberately NOT gated on the Depth switch. It used to be, and that made the
// question unanswerable: with the switch off -- the default -- this returned immediately, logged
// nothing, and the status line then said "no candidate found", which reads as "this game has no
// depth buffer" when it actually meant "nobody looked". Finding a candidate costs a pointer and a
// GetDesc; only *using* it is gated, further down in the present path.
//
// D3D11 half of the observation. ReShade hands the render targets and the depth-stencil of
// every bind; on D3D12 it hands the add-on only the swapchain (measured: zero depth binds in
// 600 frames), which is why this path exists at all and why PCSX2 had to be moved to D3D11
// before it could show a depth buffer.
void ObserveD3D11(device *dev, const resource_view *rtvs, uint32_t count, resource depthRes)
{
    if (!g.settings.useGameGuides.load())
        return;
    // Not while ReShade is drawing its own effect chain. Every shader in that chain renders into
    // screen-sized intermediates, and an optical-flow shader's are two-channel float ones the
    // size of the screen -- which is the exact description this observation uses to recognise a
    // velocity buffer. Without this gate the motion guide could settle on a provider's working
    // texture, or on the companion effect's own previous-frame copy, and prefer it over the
    // game's real one on nothing better than which got bound more often.
    if (g.inEffects.load())
        return;
    const UINT screenW = g.outWidth, screenH = g.outHeight;
    auto record = [](std::unordered_map<void *, Tallied> &tally, ID3D11Resource *native,
                     const D3D11_TEXTURE2D_DESC &d) {
        Tallied &slot = tally[native];
        if (slot.res == nullptr)
        {
            slot.res = native;
            slot.width = d.Width;
            slot.height = d.Height;
            slot.format = d.Format;
        }
        ++slot.binds;
    };
    if (depthRes.handle != 0)
    {
        auto *native = reinterpret_cast<ID3D11Resource *>(depthRes.handle);
        ComPtr<ID3D11Texture2D> tex;
        D3D11_TEXTURE2D_DESC d {};
        if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&tex))))
        {
            tex->GetDesc(&d);
            // Same two floors, for the same reason.
            if (d.SampleDesc.Count == 1 && d.ArraySize == 1 &&
                d.Width >= kGuideFloor && d.Height >= kGuideFloor &&
                GuideDepthSrvFormat(d.Format) != DXGI_FORMAT_UNKNOWN &&
                (screenW == 0 || screenH == 0 ||
                 (d.Width * 2 >= screenW && d.Height * 2 >= screenH)))
                record(g_depthTally, native, d);
        }
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        if (rtvs[i].handle == 0)
            continue;
        const resource res = dev->get_resource_from_view(rtvs[i]);
        if (res.handle == 0)
            continue;
        auto *native = reinterpret_cast<ID3D11Resource *>(res.handle);
        ComPtr<ID3D11Texture2D> tex;
        D3D11_TEXTURE2D_DESC d {};
        if (FAILED(native->QueryInterface(IID_PPV_ARGS(&tex))))
            continue;
        tex->GetDesc(&d);
        if (LooksLikeMotion(d, screenW, screenH))
            record(g_motionTally, native, d);
    }
}

}  // namespace d3d11route
