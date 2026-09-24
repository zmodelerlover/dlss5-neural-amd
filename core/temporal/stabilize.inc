// FSR3 at 1x over the whole frame, ahead of the network: the reference's Magpie chain, where the
// network is handed an image FSR3 has already accumulated in time. Part of the core's unity build,
// included by neural.cpp after optical_flow.inc.
//
// Parameters are the reference's own (Magpie FSR3Upscaler.cpp:530-559 and FsrHdrProtocol): the
// 3.1.5 provider out of amd_fidelityfx_upscaler_dx12.dll through the loader beside the exe; render
// and upscale size both the frame's; the optical flow's field as the motion, scale (1,1), jitter 0;
// a flat depth of 0 read as inverted and infinite; exposure 1, pre-exposure 1; reactive 0.2 and
// transparency 0; RCAS on at 0.2; 16.6667 ms; near 1, far FLT_MAX, 60 degrees; sRGB non-linear input
// and colour space. The input is the flow's own 8-bit copy of the frame, which is what the
// reference captures, and the output replaces the frame for everything the network pass reads.
//
// Stabilise in the ini: 0 off, 1 on every present (the reference runs on every frame it captures),
// 2 on evaluated frames only -- FSR3 then sees one frame in every evaluation, with the motion
// between them.

#if AMDNR_WITH_FFX

inline constexpr char kFillShader[] = R"(
RWTexture2D<float4> dst : register(u0);
cbuffer C : register(b0) { uint w; uint h; float v; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=w || p.y>=h) return;
 dst[p.xy] = v;
})";

struct Stabiliser
{
    HMODULE loader = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    PfnFfxQuery query = nullptr;
    ffxContext context = nullptr;
    bool failed = false, filled = false, logged = false;
    UINT width = 0, height = 0;
    ComPtr<ID3D12Resource> output, depth, exposure, reactive, transparency;
    ComPtr<ID3D12PipelineState> fill;
} g_stab;

bool StabiliserReady(UINT w, UINT h)
{
    if (g_stab.failed)
        return false;
    if (g_stab.context != nullptr && g_stab.width == w && g_stab.height == h)
        return true;
    auto fail = [](const char *what, int code) {
        Log("stabiliser: %s (%d); the network gets the frame as it is for this run.", what, code);
        g_stab.failed = true;
        return false;
    };
    if (g_stab.context != nullptr)
        g_stab.destroy(&g_stab.context, nullptr);
    g_stab.context = nullptr;
    if (g_stab.loader == nullptr)
    {
        g_stab.loader = LoadLibraryW((ExeDirectory() / L"amd_fidelityfx_loader_dx12.dll").c_str());
        if (g_stab.loader == nullptr)
            return fail("amd_fidelityfx_loader_dx12.dll is not beside the exe", static_cast<int>(GetLastError()));
        g_stab.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_stab.loader, "ffxCreateContext"));
        g_stab.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g_stab.loader, "ffxDestroyContext"));
        g_stab.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_stab.loader, "ffxDispatch"));
        g_stab.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(g_stab.loader, "ffxQuery"));
        if (!g_stab.create || !g_stab.destroy || !g_stab.dispatch || !g_stab.query)
            return fail("the loader's exports are incomplete", 0);
    }
    // The provider by name, as the reference picks it: the DLL also carries FSR 4.
    ffxQueryDescGetVersions versions {};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = g.device.Get();
    uint64_t count = 0;
    versions.outputCount = &count;
    if (g_stab.query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0)
        return fail("no upscaler provider", 0);
    std::vector<uint64_t> ids(count);
    std::vector<const char *> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    g_stab.query(nullptr, &versions.header);
    uint64_t chosen = 0;
    for (uint64_t i = 0; i < count; ++i)
        if (names[i] != nullptr && std::strstr(names[i], "3.1.5") != nullptr)
            chosen = ids[i];
    if (chosen == 0)
        return fail("no FSR 3.1.5 provider in amd_fidelityfx_upscaler_dx12.dll", static_cast<int>(count));

    ffxCreateContextDescUpscale desc {};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
                 FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    desc.maxRenderSize = { w, h };
    desc.maxUpscaleSize = { w, h };
    ffxCreateBackendDX12Desc backend {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = g.device.Get();
    ffxCreateContextDescUpscaleVersion version {};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    ffxOverrideVersion pinned {};
    pinned.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    pinned.versionId = chosen;
    desc.header.pNext = &backend.header;
    backend.header.pNext = &version.header;
    version.header.pNext = &pinned.header;
    if (const auto rc = g_stab.create(&g_stab.context, &desc.header, nullptr); rc != FFX_API_RETURN_OK)
        return fail("ffxCreateContext refused FSR 3.1.5", static_cast<int>(rc));
    if (!CreateTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, g_stab.output, "stabilised") ||
        !CreateTexture(w, h, DXGI_FORMAT_R32_FLOAT, g_stab.depth, "flat depth") ||
        !CreateTexture(1, 1, DXGI_FORMAT_R32_FLOAT, g_stab.exposure, "exposure") ||
        !CreateTexture(w, h, DXGI_FORMAT_R8_UNORM, g_stab.reactive, "reactive") ||
        !CreateTexture(w, h, DXGI_FORMAT_R8_UNORM, g_stab.transparency, "transparency") ||
        (g_stab.fill == nullptr && !CompileShader(kFillShader, sizeof(kFillShader), "fill", g_stab.fill)))
        return fail("resources", 0);
    g_stab.width = w;
    g_stab.height = h;
    g_stab.filled = false;
    Log("stabiliser: FSR 3.1.5 at 1x over the %ux%u frame, the reference's parameters.", w, h);
    return true;
}

// FSR3 over the flow's copy of the frame, with the flow's field. `reset` whenever that field
// starts over, as the reference resets it on its guidance reset. Returns the stabilised frame, or
// null when FSR3 refused.
ID3D12Resource *Stabilise(ID3D12GraphicsCommandList *cmd, bool reset)
{
    const UINT w = g_stab.width, h = g_stab.height;
    if (!g_stab.filled)
    {
        const struct { ID3D12Resource *res; DXGI_FORMAT fmt; float v; } fills[] {
            { g_stab.depth.Get(), DXGI_FORMAT_R32_FLOAT, 0.0f }, { g_stab.exposure.Get(), DXGI_FORMAT_R32_FLOAT, 1.0f },
            { g_stab.reactive.Get(), DXGI_FORMAT_R8_UNORM, 0.2f }, { g_stab.transparency.Get(), DXGI_FORMAT_R8_UNORM, 0.0f } };
        UINT base = 48;
        for (const auto &f : fills)
        {
            const auto d = f.res->GetDesc();
            ViewAt(base + 3, f.res, f.fmt, true);
            UINT k[3] { static_cast<UINT>(d.Width), d.Height, 0 };
            std::memcpy(&k[2], &f.v, sizeof(float));
            DispatchOn(cmd, g_stab.fill.Get(), base, f.res, k, 3, k[0], k[1]);
            base += 4;
        }
        g_stab.filled = true;
    }
    ffxDispatchDescUpscale d {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmd;
    d.color = ffxApiGetResourceDX12(g_flow.input.Get());
    d.depth = ffxApiGetResourceDX12(g_stab.depth.Get());
    d.motionVectors = ffxApiGetResourceDX12(g_flow.dense.Get());
    d.exposure = ffxApiGetResourceDX12(g_stab.exposure.Get());
    d.reactive = ffxApiGetResourceDX12(g_stab.reactive.Get());
    d.transparencyAndComposition = ffxApiGetResourceDX12(g_stab.transparency.Get());
    d.output = ffxApiGetResourceDX12(g_stab.output.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset = { 0.0f, 0.0f };
    d.motionVectorScale = { 1.0f, 1.0f };
    d.renderSize = { w, h };
    d.upscaleSize = { w, h };
    d.enableSharpening = true;
    d.sharpness = 0.2f;
    d.frameTimeDelta = 16.6667f;
    d.preExposure = 1.0f;
    d.reset = reset;
    d.cameraNear = 1.0f;
    d.cameraFar = FLT_MAX;
    d.cameraFovAngleVertical = 1.04719755f;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    if (const auto rc = g_stab.dispatch(&g_stab.context, &d.header); rc != FFX_API_RETURN_OK)
    {
        Log("stabiliser: dispatch failed (%d); the network gets the frame as it is for this run.", static_cast<int>(rc));
        g_stab.failed = true;
        return nullptr;
    }
    if (!g_stab.logged)
    {
        g_stab.logged = true;
        Log("stabiliser: the network now reads FSR3's output, %s.",
            g.settings.stabilise.load() == 1 ? "run on every present" : "run on evaluated frames only");
    }
    return g_stab.output.Get();
}

#else

bool StabiliserReady(UINT, UINT) { return false; }
ID3D12Resource *Stabilise(ID3D12GraphicsCommandList *, bool) { return nullptr; }

#endif

// What runs ahead of the network pass: the flow, when the stabiliser needs it this frame, and the
// stabiliser. `src`/`fmt` are what the pass reads from here on -- the stabilised frame, or the
// game's own when the stabiliser is off or failed.
struct Ahead
{
    ID3D12Resource *src;
    DXGI_FORMAT fmt;
    bool flowRan, flowReal;
};

Ahead RunAhead(ID3D12GraphicsCommandList *cmd, bool runNetwork, ID3D12Resource *src, DXGI_FORMAT fmt)
{
    Ahead a { src, fmt, false, false };
    const int mode = g.settings.stabilise.load();
    if (mode == 0 || (mode == 2 && !runNetwork) || !OpticalFlowReady(g.outWidth, g.outHeight) ||
        !StabiliserReady(g.outWidth, g.outHeight))
        return a;
    a.flowRan = true;
    a.flowReal = RecordOpticalFlow(cmd, src, fmt);
    if (ID3D12Resource *out = Stabilise(cmd, FlowStarted()))
    {
        a.src = out;
        a.fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return a;
}

// The optical flow as the network's motion: the field RunAhead already made this frame, or a new
// one. Returns whether it is real motion, and says whether it spans the gap since the last
// evaluation -- it does unless the stabiliser runs it on every present.
bool FlowMotion(ID3D12GraphicsCommandList *cmd, const Ahead &a, ID3D12Resource *src, DXGI_FORMAT fmt)
{
    const bool real = a.flowRan ? a.flowReal : RecordOpticalFlow(cmd, src, fmt);
    FlowToRaster(cmd);
    g_motionSinceEvaluation = real && g.settings.stabilise.load() != 1;
    return real;
}
