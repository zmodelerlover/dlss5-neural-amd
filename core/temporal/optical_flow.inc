// FidelityFX Optical Flow as a motion source: the one the reference uses on its Magpie route, where
// the game hands over no vectors -- the same position this add-on is in on every route but D3D11.
// Part of the core's unity build, included by neural.cpp ahead of RecordNetwork.
//
// The chain is the reference's (Magpie AmdOpticalFlowProvider.cpp): the frame copied to 8-bit at its
// own size (Quality, no downscale), ffxOpticalflowContextDispatch -- 8x8 blocks, seven passes,
// sRGB transfer, luminance 0..1 -- and the sparse field densified at p/8 - 0.5 into frame pixels,
// bilinearly and along the depth (below), current to previous. A reset dispatch has no previous
// frame and yields zero, as there. Then into the raster the engine reads, scaled by nw/w.
//
// OpticalFlow in the ini: 0 off (the block estimator), 1 when the game and the companion effect
// give nothing -- the default, since v0.7.1 -- 2 always, parity with the reference. A build without
// -Ffx (the 32-bit bridge's host) reads it as 0, so the game's vectors are never passed over there.

// Descriptor tables of the root signature's t0-t2/u0 shape live in slots 32..63, one each, because a
// descriptor is read when the list executes and a slot rewritten later in the same list would be
// read by both: prepare 32, densify 36, the raster 40 and the motion feed 44.
void DispatchOn(ID3D12GraphicsCommandList *cmd, ID3D12PipelineState *pso, UINT base, ID3D12Resource *out,
                const UINT *k, UINT count, UINT dw, UINT dh)
{
    const UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12DescriptorHeap *heap = g.heap.Get();
    auto table = heap->GetGPUDescriptorHandleForHeapStart();
    table.ptr += UINT64(base) * inc;
    cmd->SetComputeRootSignature(g.root.Get());
    cmd->SetDescriptorHeaps(1, &heap);
    cmd->SetPipelineState(pso);
    cmd->SetComputeRootDescriptorTable(0, table);
    cmd->SetComputeRoot32BitConstants(1, count, k, 0);
    Barrier(cmd, out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->Dispatch((dw + 7) / 8, (dh + 7) / 8, 1);
    Barrier(cmd, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// A view of `res` as `fmt` in heap slot i: an SRV, or a UAV when `uav`.
void ViewAt(UINT i, ID3D12Resource *res, DXGI_FORMAT fmt, bool uav = false)
{
    auto slot = g.heap->GetCPUDescriptorHandleForHeapStart();
    slot.ptr += SIZE_T(i) * g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (uav)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d {};
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Format = fmt;
        g.device->CreateUnorderedAccessView(res, nullptr, &d, slot);
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC d {};
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    d.Format = fmt;
    g.device->CreateShaderResourceView(res, &d, slot);
}

#if AMDNR_WITH_FFX

inline constexpr char kFlowPrepareShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer C : register(b0) { uint w; uint h; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=w || p.y>=h) return;
 dst[p.xy] = src.Load(int3(p.xy,0));
})";

// Bilinear over the four nearest blocks, each weighted down by how far its depth is from the
// pixel's: an 8x8 block straddling a silhouette carries one vector, and plain bilinear smeared it
// 8-16 px across the edge -- a skater's vectors on the road behind, the road's on the skater -- which
// is where the history misaligned. dw = 0 (no depth this frame) is plain bilinear. The relative
// difference works on either depth convention; at 10% a block keeps half its weight. The floor
// hands a pixel unlike all four back to bilinear rather than to whichever is least unlike.
// ponytail: a block's depth is its centre pixel; the block's min and max if thin objects halo.
inline constexpr char kFlowDensifyShader[] = R"(
Texture2D<int2> sparse : register(t0);
Texture2D<float> depth : register(t1);
RWTexture2D<float2> dense : register(u0);
cbuffer C : register(b0) { uint w; uint h; uint ow; uint oh; uint vw; uint vh; float sx; float sy; uint zero; uint dw; uint dh; };
float Z(float2 xy) { return depth.Load(int3(min(uint2(xy * float2(dw,dh) / float2(w,h)), uint2(dw-1,dh-1)), 0)); }
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=w || p.y>=h) return;
 if(zero != 0) { dense[p.xy] = 0; return; }
 float2 s = (float2(p.xy)+0.5) * (float2(ow,oh)/float2(w,h)) / 8.0 - 0.5;
 int2 p0 = int2(floor(s)); float2 f = frac(s); int2 lim = int2(vw,vh) - 1;
 float zp = dw != 0 ? Z(float2(p.xy)+0.5) : 0.0;
 float2 v = 0; float sum = 0;
 [unroll] for (int i = 0; i < 4; ++i) {
  int2 o = int2(i & 1, i >> 1), q = clamp(p0 + o, 0, lim);
  float k = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
  if (dw != 0) {
   float zb = Z((float2(q) + 0.5) * 8.0 * float2(w,h) / float2(ow,oh));
   float r = abs(zp - zb) / max(max(zp, zb), 1e-6) / 0.1;
   k *= 1.0 / (1.0 + r * r) + 1e-3;
  }
  v += k * float2(sparse.Load(int3(q, 0))); sum += k;
 }
 dense[p.xy] = v / max(sum, 1e-9) * float2(sx,sy);
})";

struct OpticalFlow
{
    FfxOpticalflowContext context {};
    std::vector<unsigned char> scratch;
    bool created = false, failed = false, primed = false, logged = false;
    UINT dispatched = 0;  // since the context was made
    UINT width = 0, height = 0;
    ComPtr<ID3D12Resource> input, sparse, scd, dense;
    ComPtr<ID3D12PipelineState> prepare, densify;
} &g_flow = *new OpticalFlow;  // never destroyed, as State g is: see neural.cpp

void DestroyOpticalFlow()
{
    if (g_flow.created)
        ffxOpticalflowContextDestroy(&g_flow.context);
    g_flow.created = g_flow.primed = false;
    g_flow.dispatched = 0;
    g_flow.input.Reset(); g_flow.sparse.Reset(); g_flow.scd.Reset(); g_flow.dense.Reset();
}

// A context for a w x h frame, made on first use and remade when the frame changes size. False
// when the SDK refuses, once and for good, so the caller falls back to the estimator.
bool OpticalFlowReady(UINT w, UINT h)
{
    if (g_flow.failed)
        return false;
    if (g_flow.created && g_flow.width == w && g_flow.height == h)
        return true;
    DestroyOpticalFlow();
    auto fail = [](const char *what, int code) {
        Log("optical flow: %s failed (%d); back to the estimator for this run.", what, code);
        g_flow.failed = true;
        return false;
    };
    // The SDK reports what it refuses only through this; without it a failure is a silent zero field.
    ffxSetPrintMessageCallback([](uint32_t type, const wchar_t *message) { Log("ffx %u: %ls", type, message); },
                               FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE);
    g_flow.scratch.resize(ffxGetScratchMemorySizeDX12(FFX_OPTICALFLOW_CONTEXT_COUNT));
    FfxOpticalflowContextDescription desc {};
    desc.resolution = { w, h };
    FfxErrorCode rc = ffxGetInterfaceDX12(&desc.backendInterface, ffxGetDeviceDX12(g.device.Get()),
                                          g_flow.scratch.data(), g_flow.scratch.size(), FFX_OPTICALFLOW_CONTEXT_COUNT);
    if (rc != FFX_OK)
        return fail("ffxGetInterfaceDX12", rc);
    if ((rc = ffxOpticalflowContextCreate(&g_flow.context, &desc)) != FFX_OK)
        return fail("ffxOpticalflowContextCreate", rc);
    g_flow.created = true;
    FfxOpticalflowSharedResourceDescriptions shared {};
    if ((rc = ffxOpticalflowGetSharedResourceDescriptions(&g_flow.context, &shared)) != FFX_OK)
        return fail("the shared resource query", rc);
    const auto &v = shared.opticalFlowVector.resourceDescription, &s = shared.opticalFlowSCD.resourceDescription;
    if (!CreateTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, g_flow.input, "flow input") ||
        !CreateTexture(v.width, v.height, ffxGetDX12FormatFromSurfaceFormat(static_cast<FfxApiSurfaceFormat>(v.format)),
                       g_flow.sparse, "flow vectors") ||
        !CreateTexture(s.width, std::max(s.height, 1u), ffxGetDX12FormatFromSurfaceFormat(static_cast<FfxApiSurfaceFormat>(s.format)),
                       g_flow.scd, "flow scene change") ||
        !CreateTexture(w, h, DXGI_FORMAT_R16G16_FLOAT, g_flow.dense, "flow dense"))
        return fail("texture creation", 0);
    if ((g_flow.prepare == nullptr &&
         !CompileShader(kFlowPrepareShader, sizeof(kFlowPrepareShader), "flow prepare", g_flow.prepare)) ||
        (g_flow.densify == nullptr &&
         !CompileShader(kFlowDensifyShader, sizeof(kFlowDensifyShader), "flow densify", g_flow.densify)))
        return fail("shader compilation", 0);
    g_flow.width = w;
    g_flow.height = h;
    Log("optical flow: FidelityFX, %ux%u frame, %ux%u blocks of 8x8, into the %ux%u raster.",
        w, h, v.width, v.height, g.netWidth, g.netHeight);
    return true;
}

// Records the flow of `src` (the whole frame) against the frame it was last run on, into the
// frame-size field g_flow.dense, in frame pixels. Returns whether that is real motion. It is not
// on a reset, which has no previous frame, nor for the six frames after one: the SDK reports a
// scene change while FrameIndex() <= 5 (IsSceneChanged) and writes zero, measured here as six
// frames of an all-zero field on a pan.
bool RecordOpticalFlow(ID3D12GraphicsCommandList *cmd, ID3D12Resource *src, DXGI_FORMAT fmt, bool depth)
{
    const UINT w = g_flow.width, h = g_flow.height;
    ViewAt(32, src, ColourReadFormat(fmt));
    ViewAt(35, g_flow.input.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, true);
    const UINT size[2] { w, h };
    DispatchOn(cmd, g_flow.prepare.Get(), 32, g_flow.input.Get(), size, 2, w, h);

    FfxOpticalflowDispatchDescription desc {};
    desc.commandList = ffxGetCommandListDX12(cmd);
    desc.color = ffxGetResourceDX12(g_flow.input.Get(), ffxGetResourceDescriptionDX12(g_flow.input.Get()),
                                    L"amd-nr flow input", FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.opticalFlowVector = ffxGetResourceDX12(g_flow.sparse.Get(),
        ffxGetResourceDescriptionDX12(g_flow.sparse.Get(), FFX_API_RESOURCE_USAGE_UAV), L"amd-nr flow vectors",
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.opticalFlowSCD = ffxGetResourceDX12(g_flow.scd.Get(),
        ffxGetResourceDescriptionDX12(g_flow.scd.Get(), FFX_API_RESOURCE_USAGE_UAV), L"amd-nr flow scene change",
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.reset = !g_flow.primed;
    desc.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    desc.minMaxLuminance = { 0.0f, 1.0f };
    if (const FfxErrorCode rc = ffxOpticalflowContextDispatch(&g_flow.context, &desc); rc != FFX_OK)
    {
        Log("optical flow: dispatch failed (%d); back to the estimator for this run.", rc);
        g_flow.failed = true;
        return false;
    }
    const bool real = g_flow.primed && g_flow.dispatched >= 6;
    g_flow.primed = true;
    ++g_flow.dispatched;

    const auto vd = g_flow.sparse->GetDesc();
    ViewAt(36, g_flow.sparse.Get(), DXGI_FORMAT_R16G16_SINT);
    ViewAt(37, g.netDepth.Get(), DXGI_FORMAT_R32_FLOAT);  // bound either way; read only when dw != 0
    ViewAt(39, g_flow.dense.Get(), DXGI_FORMAT_R16G16_FLOAT, true);
    const float one = 1.0f;
    UINT dk[11] { w, h, w, h, static_cast<UINT>(vd.Width), vd.Height, 0, 0, real ? 0u : 1u,
                  depth ? g.netWidth : 0u, depth ? g.netHeight : 0u };
    std::memcpy(&dk[6], &one, sizeof(one));
    std::memcpy(&dk[7], &one, sizeof(one));
    DispatchOn(cmd, g_flow.densify.Get(), 36, g_flow.dense.Get(), dk, 11, w, h);
    return real;
}

// The frame-size field into the raster the engine reads, in raster pixels.
void FlowToRaster(ID3D12GraphicsCommandList *cmd)
{
    const UINT w = g_flow.width, h = g_flow.height, nw = g.netWidth, nh = g.netHeight;
    ViewAt(40, g_flow.dense.Get(), DXGI_FORMAT_R16G16_FLOAT);
    ViewAt(43, g.netMotion.Get(), DXGI_FORMAT_R16G16_FLOAT, true);
    const float step = static_cast<float>(nw) / static_cast<float>(w) * g.settings.motionScale.load();
    UINT uk[8] { nw, nh, w, h, 0, 0, 0, 0 };
    std::memcpy(&uk[6], &step, sizeof(step));
    DispatchOn(cmd, g.flowUpPipeline.Get(), 40, g.netMotion.Get(), uk, 8, nw, nh);
    if (!g_flow.logged)
    {
        g_flow.logged = true;
        Log("motion: FidelityFX optical flow, %s. The estimator is off.",
            g.settings.opticalFlow.load() == 2 ? "forced over any game or effect vectors (parity with the reference)"
                                               : "because neither the game nor the companion effect gives vectors");
    }
}

#else

bool OpticalFlowReady(UINT, UINT) { return false; }
bool RecordOpticalFlow(ID3D12GraphicsCommandList *, ID3D12Resource *, DXGI_FORMAT, bool) { return false; }
void FlowToRaster(ID3D12GraphicsCommandList *) {}
void DestroyOpticalFlow() {}

#endif

// The optical flow as the network's motion. Returns whether it is real motion, which also spans
// the gap since the last evaluation: the flow runs only on evaluated frames.
bool FlowMotion(ID3D12GraphicsCommandList *cmd, ID3D12Resource *src, DXGI_FORMAT fmt, bool depth)
{
    const bool real = RecordOpticalFlow(cmd, src, fmt, depth);
    FlowToRaster(cmd);
    g_motionSinceEvaluation = real;
    return real;
}
