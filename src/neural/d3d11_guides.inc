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

}  // namespace d3d11route
