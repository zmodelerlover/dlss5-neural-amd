// Where this frame's motion comes from: the game's own vectors, optical flow, or the estimator.
// Part of the core's unity build, included by neural.cpp inside RecordNetwork: it records into
// that function's `cmd` with its descriptors, sets its `haveMotion`, `motionFrom` (which of the
// three, for the temporal line) and g_motionSinceEvaluation (read by DropStaleHistory), and records
// the flow probe's readback copies that probes.inc reads.

    GuideSource motionFrom = GuideSource::Estimated;

    // The game's own velocity buffer, when there is one. This is the input the estimator below
    // exists to replace: an engine knows where every surface was because it has the previous
    // transform, while a block match over two images can only guess and, on PCSX2, measured 99%
    // of blocks still. Resample it into the raster the engine reads and skip the estimator.
    const int flow = g.settings.useMotion.load() ? g.settings.opticalFlow.load() : 0;
    if (flow != 2 && g.settings.useMotion.load() && g.gameMotionActive && g.guideMotion.local != nullptr &&
        g.netMotion != nullptr)
    {
        const auto md = g.guideMotion.local->GetDesc();
        srv.Format = md.Format;
        g.device->CreateShaderResourceView(g.guideMotion.local.Get(), &srv, slot(20));
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateUnorderedAccessView(g.netMotion.Get(), nullptr, &uav, slot(23));
        cmd->SetComputeRootSignature(g.root.Get());
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetPipelineState(g.flowUpPipeline.Get());
        auto mtable = heap->GetGPUDescriptorHandleForHeapStart();
        mtable.ptr += 20 * inc;
        cmd->SetComputeRootDescriptorTable(0, mtable);
        const float mscale = g.settings.motionScale.load();
        UINT mdims[8] { nw, nh, static_cast<UINT>(md.Width), md.Height, 1, 0, 0, 0 };
        std::memcpy(&mdims[6], &mscale, sizeof(float));
        cmd->SetComputeRoot32BitConstants(1, 8, mdims, 0);
        Barrier(cmd, g.netMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->Dispatch((nw + 7) / 8, (nh + 7) / 8, 1);
        Barrier(cmd, g.netMotion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        haveMotion = true;
        motionFrom = g.guideMotion.external ? GuideSource::Effect : GuideSource::Game;
        if (!g.loggedGameMotion)
        {
            g.loggedGameMotion = true;
            Log("motion: %s, %llux%u -> %ux%u, scale %.3f. The estimator is "
                "off. Check Debug View \"Motion vectors\" while panning: the field should follow "
                "the camera, and MotionScale flips or rescales it if it does not.",
                g.guideMotion.external ? "an optical-flow shader, through AMD_Neural_Feed.fx"
                                       : "the game's own vectors",
                static_cast<unsigned long long>(md.Width), md.Height, nw, nh,
                static_cast<double>(mscale));
        }
    }
    else if (flow != 0 && OpticalFlowReady(w, h))
        haveMotion = FlowMotion(cmd, colourSrc, colourFmt, haveDepth && g.depthUsable.load()), motionFrom = GuideSource::Flow;
    else if (g.settings.useMotion.load() && g.flowSmall != nullptr)
    {
        // Swapped per estimator run, not per present: with the network sitting out every other
        // present, the present count had the same parity on every run and `prev` was never rewritten.
        const bool odd = (g.flowRuns++ & 1) != 0;
        ID3D12Resource *cur = odd ? g.lumaB.Get() : g.lumaA.Get();
        ID3D12Resource *prev = odd ? g.lumaA.Get() : g.lumaB.Get();
        const UINT fw = g.flowWidth, fh = g.flowHeight;

        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        g.device->CreateShaderResourceView(g.netColour.Get(), &srv, slot(12));
        uav.Format = DXGI_FORMAT_R16_FLOAT;
        g.device->CreateUnorderedAccessView(cur, nullptr, &uav, slot(15));

        // Coarse pass at base 16 searches in strides of 2, so it reaches twice as far for the
        // same 81 candidates. Fine pass at base 24 refines around what it found. Two cheap passes
        // beat one wide search: reach grows linearly here and quadratically there.
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        g.device->CreateShaderResourceView(cur, &srv, slot(16));
        g.device->CreateShaderResourceView(prev, &srv, slot(17));
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateShaderResourceView(g.flowCoarse.Get(), &srv, slot(18));
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateUnorderedAccessView(g.flowCoarse.Get(), nullptr, &uav, slot(19));

        srv.Format = DXGI_FORMAT_R16_FLOAT;
        g.device->CreateShaderResourceView(cur, &srv, slot(24));
        g.device->CreateShaderResourceView(prev, &srv, slot(25));
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateShaderResourceView(g.flowCoarse.Get(), &srv, slot(26));
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateUnorderedAccessView(g.flowSmall.Get(), nullptr, &uav, slot(27));

        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateShaderResourceView(g.flowSmall.Get(), &srv, slot(20));
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        g.device->CreateUnorderedAccessView(g.netMotion.Get(), nullptr, &uav, slot(23));

        auto table = [&](UINT base) {
            auto t = heap->GetGPUDescriptorHandleForHeapStart();
            t.ptr += static_cast<UINT64>(base) * inc;
            return t;
        };
        // The gate and the accept ratio decide how much of the field is allowed to move, and the
        // right value depends on the game -- 0.02 pins 99% of a dark scene still, while dropping
        // it too far lets noise through. They live in the constant buffer rather than in the
        // shader so they can be dialled in against the flow probe without a rebuild.
        const float gate = g.settings.flowGate.load(), ratio = g.settings.flowRatio.load();
        auto dispatch = [&](ID3D12PipelineState *pso, UINT base, UINT dw, UINT dh, UINT sw, UINT sh,
                            float extra, UINT mode = 0) {
            cmd->SetPipelineState(pso);
            cmd->SetComputeRootDescriptorTable(0, table(base));
            UINT d[8] { dw, dh, sw, sh, mode, 0, 0, 0 };
            std::memcpy(&d[5], &gate, sizeof(gate));
            std::memcpy(&d[6], &extra, sizeof(extra));
            std::memcpy(&d[7], &ratio, sizeof(ratio));
            cmd->SetComputeRoot32BitConstants(1, 8, d, 0);
            cmd->Dispatch((dw + 7) / 8, (dh + 7) / 8, 1);
        };

        cmd->SetComputeRootSignature(g.root.Get());
        cmd->SetDescriptorHeaps(1, &heap);

        Barrier(cmd, cur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(g.lumaPipeline.Get(), 12, fw, fh, nw, nh, 0.0f);
        Barrier(cmd, cur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        Barrier(cmd, g.flowCoarse.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(g.flowPipeline.Get(), 16, fw, fh, fw, fh, 2.0f, 0);
        Barrier(cmd, g.flowCoarse.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        Barrier(cmd, g.flowSmall.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(g.flowPipeline.Get(), 24, fw, fh, fw, fh, 1.0f, 1);
        Barrier(cmd, g.flowSmall.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // The field is in coarse pixels; the engine reads it at raster resolution, so one coarse
        // pixel is eight of those. Getting this factor wrong tells the network the image moved a
        // different distance than it did, which is worse than telling it nothing.
        Barrier(cmd, g.netMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // MotionScale rides on the same multiplier here as it does on the game's own vectors.
        // Without this the slider only ever touched games that hand over a velocity buffer, and
        // the estimated field -- the one that is a guess and most needs turning down -- had no
        // control at all.
        dispatch(g.flowUpPipeline.Get(), 20, nw, nh, fw, fh,
                 static_cast<float>(nw) / static_cast<float>(fw) * g.settings.motionScale.load());
        Barrier(cmd, g.netMotion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // One-shot look inside the flow, so a single run answers what is wrong instead of
        // bisecting over several. A degenerate match and a genuinely still scene produce the same
        // mean vector; the luminance stats tell them apart.
        if (!g.flowProbed && g.status.frame == 300)
        {
            g.flowProbed = true;
            const UINT lumaPitch = (fw * 2 + 255) & ~255u;
            const UINT flowPitch = (fw * 4 + 255) & ~255u;
            D3D12_HEAP_PROPERTIES rb {};
            rb.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bd.Width = static_cast<UINT64>(lumaPitch) * fh;
            const bool okA = SUCCEEDED(g.device->CreateCommittedResource(
                &rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&g.flowReadLuma)));
            bd.Width = static_cast<UINT64>(flowPitch) * fh;
            const bool okB = SUCCEEDED(g.device->CreateCommittedResource(
                &rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&g.flowReadFlow)));
            if (okA && okB)
            {
                auto grab = [&](ID3D12Resource *src, ID3D12Resource *dst, DXGI_FORMAT f, UINT pitch) {
                    Barrier(cmd, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION from {}, to {};
                    from.pResource = src;
                    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    to.pResource = dst;
                    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    to.PlacedFootprint.Footprint = { f, fw, fh, 1, pitch };
                    cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                    Barrier(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                };
                grab(cur, g.flowReadLuma.Get(), DXGI_FORMAT_R16_FLOAT, lumaPitch);
                grab(g.flowSmall.Get(), g.flowReadFlow.Get(), DXGI_FORMAT_R16G16_FLOAT, flowPitch);
                g.pendingFlow = true;
            }
        }

        haveMotion = g_motionSinceEvaluation = g.flowRuns > 2;
        if (!g.loggedFlow)
        {
            g.loggedFlow = true;
            Log("motion: flow at %ux%u, coarse pass in strides of 2 then a refine, reaching +/-12 "
                "coarse pixels, about %.0f raster pixels a frame. The PS2 has no real motion to "
                "read, so this is inferred from the image, same as the NVIDIA route does here.",
                fw, fh, 12.0 * static_cast<double>(nw) / static_cast<double>(fw));
        }
    }
