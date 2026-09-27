// Run lossless SDR frames through the actual add-on pipeline, without a game.
// Build: ./build.ps1 -Target framecheck -Exe
// Run in a private directory containing the runtime, weights and amd-nr.ini:
//   amd-nr-framecheck.exe input.ppm [frames=20]
//   amd-nr-framecheck.exe --seq <dir> [frames=N] [skip=K] [mv=truth|off] [dump=runtime,composed,...]
// With NrBackend=mochizuki in amd-nr.ini the directory holds MochizukiNrRuntime.dll and
// dlssnr-amd\ instead of the danielblnc runtime and its weights.
// PPM must be binary P6, RGB8, with no comments. Outputs are tightly packed raw
// textures plus capture.csv (formats/dimensions) and timings.csv (GPU and wall ms).
//
// The single-frame mode repeats one image with everything temporal off, for byte comparisons.
// The sequence mode is the temporal instrument: frame<i>.ppm in order (the last one repeats past
// the end), optionally mv<i>.f32 -- float32 x,y per pixel, pixels, prev = cur + mv, the FFX
// convention the estimator also uses -- fed through the game-velocity path the way D3D11 feeds a
// real buffer. skip=K runs the network on one frame in K+1, which is what a pending job does to
// a real route. Every frame's captures and seq.csv land beside the exe.
// This includes the production implementation so experiments cannot silently use
// a second copy of its shaders or runtime ABI. It never registers with ReShade.
#include "../../addon/neural.cpp"
#include <chrono>
#include <map>
#include <stdexcept>

namespace framecheck
{
void Check(bool ok, const char *what)
{
    if (!ok) throw std::runtime_error(what);
}
void Hr(HRESULT hr, const char *what)
{
    if (FAILED(hr))
    {
        std::fprintf(stderr, "%s: 0x%08lX\n", what, hr);
        throw std::runtime_error(what);
    }
}
ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = type;
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    Hr(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                     : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&result)), "buffer");
    return result;
}
ID3D12GraphicsCommandList *Begin()
{
    Hr(g.alloc[0]->Reset(), "allocator reset");
    Hr(g.list[0]->Reset(g.alloc[0].Get(), nullptr), "list reset");
    return g.list[0].Get();
}
ComPtr<ID3D12InfoQueue> g_debugMessages;
void FlushDebugMessages()
{
    if (g_debugMessages == nullptr)
        return;
    for (UINT64 i = 0, n = g_debugMessages->GetNumStoredMessages(); i < n; ++i)
    {
        SIZE_T size = 0;
        g_debugMessages->GetMessage(i, nullptr, &size);
        std::vector<char> buffer(size);
        auto *m = reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
        if (SUCCEEDED(g_debugMessages->GetMessage(i, m, &size)))
            Log("d3d12 %d/%d: %.*s", m->Severity, m->ID, static_cast<int>(m->DescriptionByteLength), m->pDescription);
    }
    g_debugMessages->ClearStoredMessages();
}
// Returns how long the runtime's job counter took to reach the job just recorded, in ms.
UINT64 Submit(bool notify = false)
{
    Hr(g.list[0]->Close(), "list close");
    ID3D12CommandList *lists[] {g.list[0].Get()};
    g.queue->ExecuteCommandLists(1, lists);
    if (notify)
        reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(g.runtime) + rt::kNotifyFn)(
            g.queue.Get(), 1, lists);
    const auto value = ++g.ringSerial;
    Hr(g.queue->Signal(g.ringFence.Get(), value), "signal");
    Hr(g.ringFence->SetEventOnCompletion(value, g.ringEvent), "fence event");
    Check(WaitForSingleObject(g.ringEvent, 30000) == WAIT_OBJECT_0, "GPU timed out");
    FlushDebugMessages();
    Hr(g.device->GetDeviceRemovedReason(), "device removed");
    if (!notify)
        return 0;
    // A GPU fence alone can complete on the timeout fallback while HIP is
    // still writing. Match the host routes' additional job-completion check
    // before reusing the fixture/output or starting the next evaluation.
    const auto start = GetTickCount64();
    while (RuntimeBusy())
    {
        Check(GetTickCount64() - start < 30000, "runtime job did not complete");
        Sleep(1);
    }
    return GetTickCount64() - start;
}
struct Layout
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT rows = 0;
    UINT64 rowBytes = 0, bytes = 0;
    explicit Layout(ID3D12Resource *res)
    {
        const auto d = res->GetDesc();
        g.device->GetCopyableFootprints(&d, 0, 1, 0, &footprint, &rows, &rowBytes, &bytes);
    }
};
void Dump(ID3D12Resource *res, const std::string &name, std::ofstream &manifest)
{
    const Layout l(res);
    auto rb = Buffer(l.bytes, D3D12_HEAP_TYPE_READBACK);
    auto *cmd = Begin();
    Barrier(cmd, res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from {}, to {};
    from.pResource = res; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = rb.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = l.footprint;
    cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(cmd, res, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Submit();
    const unsigned char *data = nullptr;
    D3D12_RANGE range {0, static_cast<SIZE_T>(l.bytes)};
    Hr(rb->Map(0, &range, reinterpret_cast<void **>(const_cast<unsigned char **>(&data))), "readback map");
    std::ofstream out(ExeDirectory() / (name + ".raw"), std::ios::binary);
    for (UINT y = 0; y < l.rows; ++y)
        out.write(reinterpret_cast<const char *>(data + l.footprint.Offset +
            y * l.footprint.Footprint.RowPitch), l.rowBytes);
    Check(out.good(), "write capture");
    D3D12_RANGE written {0, 0}; rb->Unmap(0, &written);
    const auto d = res->GetDesc();
    manifest << name << ".raw," << d.Width << ',' << d.Height << ',' << d.Format << '\n';
    manifest.flush();
}
std::vector<unsigned char> ReadPpm(const std::filesystem::path &path, UINT &w, UINT &h)
{
    std::ifstream in(path, std::ios::binary);
    std::string magic; UINT maxValue = 0;
    in >> magic >> w >> h >> maxValue;
    Check(in.good() && magic == "P6" && maxValue == 255 && w >= 64 && h >= 64 &&
          w <= 8192 && h <= 8192, "expected P6 RGB8 input (64..8192 pixels)");
    Check(in.get() == '\n', "PPM header must end with LF");
    std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
    in.read(reinterpret_cast<char *>(rgb.data()), rgb.size());
    Check(static_cast<size_t>(in.gcount()) == rgb.size(), "truncated PPM");
    return rgb;
}
// Packed pixels of srcBytes each into a texture of dstBytes-per-pixel texels. RGB8 into RGBA8
// gets an opaque alpha; anything else is copied as it is.
void Upload(ID3D12Resource *res, const unsigned char *data, UINT srcBytes, UINT dstBytes)
{
    const Layout l(res);
    const auto d = res->GetDesc();
    auto upload = Buffer(l.bytes, D3D12_HEAP_TYPE_UPLOAD);
    unsigned char *dst = nullptr;
    D3D12_RANGE none {0, 0};
    Hr(upload->Map(0, &none, reinterpret_cast<void **>(&dst)), "upload map");
    for (UINT y = 0; y < d.Height; ++y)
        for (UINT x = 0; x < d.Width; ++x)
        {
            auto *p = dst + l.footprint.Offset + y * l.footprint.Footprint.RowPitch + x * dstBytes;
            std::memcpy(p, data + (static_cast<size_t>(y) * d.Width + x) * srcBytes, srcBytes);
            if (dstBytes == 4 && srcBytes == 3) p[3] = 255;
        }
    upload->Unmap(0, nullptr);
    auto *cmd = Begin();
    Barrier(cmd, res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION from {}, to {};
    from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = l.footprint;
    to.pResource = res; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(cmd, res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Submit();
}
// Up to the point both modes share: settings read, device and pipeline built for a w x h frame,
// the engine loaded. `temporal` is the sequence mode, the only one allowed the temporal inputs.
void Bring(UINT w, UINT h, bool temporal)
{
    g_log = _wfopen((ExeDirectory() / L"amd-nr.log").c_str(), L"w");
    Check(g_log != nullptr, "open log");
    LoadSettings();
    Check(g.settings.passes >= 1 && g.settings.passes <= 3 && !g.settings.useDepth && g.settings.encoding == 0,
          "requires 1..3 passes, SDR and depth off");
    Check(temporal || (!g.settings.useMotion && !g.settings.useHistory && !g.settings.useGameGuides &&
                       g.settings.temporalMode == 1),
          "the single-frame mode requires temporal forced off and all guides/history off; "
          "temporal runs belong to --seq");
    // The harness has no ReShade hooks; use the system D3D12 module directly.
    const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    Check(d3d12 != nullptr && LoadGraphicsApi(), "graphics API");
    // AMDNR_D3D12_DEBUG=1: the D3D12 debug layer, its messages in amd-nr.log after every frame.
    // For code that fails without a word, as a third-party backend's release build does.
    const bool debugLayer = _wgetenv(L"AMDNR_D3D12_DEBUG") != nullptr;
    if (debugLayer)
    {
        ComPtr<ID3D12Debug> debug;
        const auto get = reinterpret_cast<decltype(&D3D12GetDebugInterface)>(GetProcAddress(d3d12, "D3D12GetDebugInterface"));
        Check(get != nullptr && SUCCEEDED(get(IID_PPV_ARGS(&debug))), "D3D12 debug layer");
        debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory1> factory;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 ad {};
    for (UINT i = 0; ; ++i)
    {
        adapter.Reset();
        Hr(factory->EnumAdapters1(i, &adapter), "no AMD adapter");
        Hr(adapter->GetDesc1(&ad), "adapter description");
        if (ad.VendorId == 0x1002 && !(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
    }
    Check(CreateWorkDevice(adapter.Get(), ad.AdapterLuid), "work device");
    g.device = g.bridge.workDevice; g.queue = g.bridge.workQueue;
    if (debugLayer)
        g.device.As(&g_debugMessages);
    Hr(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)), "pass fence");
    Check(InitPipeline() && EnsureResources(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, g.settings.scale), "pipeline/resources");
    Check(MzSelected() ? MzInit() : InitEngine(), "engine initialization");
}
struct Clock
{
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> times;
    UINT64 frequency = 0;
    Clock()
    {
        D3D12_QUERY_HEAP_DESC qd {}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = 2;
        Hr(g.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries)), "query heap");
        times = Buffer(16, D3D12_HEAP_TYPE_READBACK);
        Hr(g.queue->GetTimestampFrequency(&frequency), "timestamp frequency");
    }
    // One evaluation through the real entry point, timed on the GPU. Returns GPU ms; `waited` is
    // how long the runtime's job counter took after the GPU finished.
    double Frame(ID3D12Resource *source, bool runNetwork, UINT64 &waited)
    {
        auto *cmd = Begin();
        cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        Check(RecordNetwork(cmd, source, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, runNetwork,
            WantedPasses(), [&]() { return SubmitPrivatePass(cmd, g.alloc[0].Get()); }), "record network");
        cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, times.Get(), 0);
        waited = Submit(runNetwork && g.activePasses != 0 && g.runtime != nullptr);
        UINT64 *ticks = nullptr; D3D12_RANGE range {0, 16}, none {0, 0};
        Hr(times->Map(0, &range, reinterpret_cast<void **>(&ticks)), "timestamp map");
        const double gpu = (ticks[1] - ticks[0]) * 1000.0 / frequency;
        times->Unmap(0, &none);
        return gpu;
    }
};
UINT WatchdogJob()
{
    if (g.runtime == nullptr)
        return 0;  // mochizuki: no danielblnc watchdog
    return static_cast<UINT>(InterlockedCompareExchange(
        reinterpret_cast<volatile LONG *>(&At<UINT>(g.runtime, rt::kWatchdogJobB)), 0, 0));
}
// The mochizuki runtime builds its network on a thread of its own and lets frames through without
// it until then; both runners wait for it before they measure anything.
void AwaitMochizuki(ID3D12Resource *source)
{
    for (const auto until = GetTickCount64() + 120000; MzSelected() && g.activePasses == 0;)
    {
        Check(GetTickCount64() < until, "the mochizuki network was not built in two minutes");
        auto *cmd = Begin();
        Check(RecordNetwork(cmd, source, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, true,
            WantedPasses(), [&]() { return SubmitPrivatePass(cmd, g.alloc[0].Get()); }), "record network");
        Submit();
        ++g.status.frame;
        Sleep(100);
    }
}
void Run(const std::filesystem::path &input, int frames)
{
    UINT w = 0, h = 0;
    const auto rgb = ReadPpm(input, w, h);
    Bring(w, h, false);
    ComPtr<ID3D12Resource> source;
    Check(CreateTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, source, "fixture"), "source");
    Upload(source.Get(), rgb.data(), 3, 4);
    Clock clock;
    std::ofstream timing(ExeDirectory() / "timings.csv");
    std::ofstream manifest(ExeDirectory() / "capture.csv");
    timing << "frame,gpu_ms,wall_ms,job,accepted,watchdog_job\n";
    manifest << "file,width,height,dxgi_format\n";
    AwaitMochizuki(source.Get());
    for (int f = 1; f <= frames; ++f)
    {
        const auto start = std::chrono::steady_clock::now();
        UINT64 waited = 0;
        const double gpu = clock.Frame(source.Get(), true, waited);
        const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        timing << f << ',' << gpu << ',' << wall << ',' << g.lastJob << ',' << g.activePasses
               << ',' << WatchdogJob() << '\n';
        timing.flush();
        Check(g.activePasses == WantedPasses(), "runtime refused a pass");
        if (f == 1 || f == frames)
        {
            const auto prefix = "f" + std::to_string(f) + "-";
            Dump(g.netBase.Get(), prefix + "input", manifest);
            Dump(g.netColour.Get(), prefix + "runtime", manifest);
            Dump(g.netResidual.Get(), prefix + "residual", manifest);
            Dump(g.composed.Get(), prefix + "composed", manifest);
        }
        ++g.status.frame;
        std::printf("frame %d: %.3f GPU ms, %.3f wall ms\n", f, gpu, wall);
        std::fflush(stdout);
    }
}
std::filesystem::path Numbered(const std::filesystem::path &dir, const char *stem, int i, const char *ext)
{
    char name[64];
    std::snprintf(name, sizeof(name), "%s%04d%s", stem, i, ext);
    return dir / name;
}
// What a capture kind reads. A kind a later stage adds is one line here.
ID3D12Resource *Capture(const std::string &kind)
{
    static const std::map<std::string, ID3D12Resource *(*)()> kinds {
        { "input", [] { return g.netBase.Get(); } },
        { "runtime", [] { return g.netColour.Get(); } },
        { "residual", [] { return g.netResidual.Get(); } },
        { "composed", [] { return g.composed.Get(); } },
        { "motion", [] { return g.netMotion.Get(); } },
        { "history", [] { return g.history[0].Get(); } },
#if AMDNR_WITH_FFX
        { "flow", [] { return g_flow.dense.Get(); } },
        { "sparse", [] { return g_flow.sparse.Get(); } },
        { "scd", [] { return g_flow.scd.Get(); } },
#endif
    };
    const auto it = kinds.find(kind);
    Check(it != kinds.end(), "unknown dump kind");
    return it->second();
}
void RunSequence(const std::filesystem::path &dir, const std::map<std::string, std::string> &opt)
{
    int count = 0;
    while (std::filesystem::exists(Numbered(dir, "frame", count, ".ppm")))
        ++count;
    Check(count > 0, "no frame0000.ppm in the sequence directory");
    const auto get = [&](const char *k, const char *def) {
        const auto it = opt.find(k);
        return it != opt.end() ? it->second : std::string(def);
    };
    const int frames = std::stoi(get("frames", std::to_string(count).c_str()));
    const int skip = std::stoi(get("skip", "0"));
    const bool truth = get("mv", "off") == "truth";
    Check(frames >= 1 && frames <= 5000 && skip >= 0, "frames must be 1..5000, skip >= 0");
    std::vector<std::string> kinds;
    for (std::string list = get("dump", "runtime,composed,motion,history"), k; !list.empty();)
    {
        const auto comma = list.find(',');
        k = list.substr(0, comma);
        list = comma == std::string::npos ? "" : list.substr(comma + 1);
        if (!k.empty()) kinds.push_back(k);
    }
    const int dumpFrom = std::stoi(get("from", "0"));

    UINT w = 0, h = 0;
    ReadPpm(Numbered(dir, "frame", 0, ".ppm"), w, h);
    Bring(w, h, true);
    ComPtr<ID3D12Resource> source, velocity;
    Check(CreateTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, source, "fixture"), "source");
    AwaitMochizuki(source.Get());
    if (truth)
    {
        Check(std::filesystem::exists(Numbered(dir, "mv", 0, ".f32")), "mv=truth without mv0000.f32");
        Check(CreateTexture(w, h, DXGI_FORMAT_R32G32_FLOAT, velocity, "truth motion"), "truth motion");
        g.guideMotion.local = velocity;
        g.guideMotion.external = true;
        g.gameMotionActive = true;
    }
    for (const auto &k : kinds)
        Capture(k);  // refuse an unknown kind before a long run, not after
    Clock clock;
    std::ofstream csv(ExeDirectory() / "seq.csv");
    std::ofstream manifest(ExeDirectory() / "capture.csv");
    csv << "frame,source,ran,accepted,history_valid,job,watchdog_job,gpu_ms,wall_ms,job_wait_ms\n";
    manifest << "file,width,height,dxgi_format\n";
    for (int f = 0; f < frames; ++f)
    {
        const int s = std::min(f, count - 1);
        UINT fw = 0, fh = 0;
        const auto rgb = ReadPpm(Numbered(dir, "frame", s, ".ppm"), fw, fh);
        Check(fw == w && fh == h, "every frame of a sequence must have the same size");
        Upload(source.Get(), rgb.data(), 3, 4);
        if (truth)
        {
            // Pixels, prev = cur + mv, in; UV out, which is what the velocity path multiplies back
            // up by the raster. A frame past the last file has not moved.
            std::vector<float> mv(static_cast<size_t>(w) * h * 2, 0.0f);
            if (s == f)
            {
                std::ifstream in(Numbered(dir, "mv", s, ".f32"), std::ios::binary);
                in.read(reinterpret_cast<char *>(mv.data()), mv.size() * sizeof(float));
                Check(static_cast<size_t>(in.gcount()) == mv.size() * sizeof(float), "truncated mv file");
            }
            for (size_t i = 0; i < mv.size(); i += 2) { mv[i] /= w; mv[i + 1] /= h; }
            Upload(velocity.Get(), reinterpret_cast<const unsigned char *>(mv.data()), 8, 8);
        }
        const bool run = f % (skip + 1) == 0;
        const auto start = std::chrono::steady_clock::now();
        UINT64 waited = 0;
        const double gpu = clock.Frame(source.Get(), run, waited);
        const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (run)
            Check(g.activePasses == WantedPasses(), "runtime refused a pass");
        csv << f << ',' << s << ',' << (run ? 1 : 0) << ',' << (run ? g.activePasses : 0) << ','
            << g.historyValid.load() << ',' << g.lastJob << ',' << WatchdogJob() << ',' << gpu << ','
            << wall << ',' << waited << '\n';
        csv.flush();
        if (f >= dumpFrom)
            for (const auto &k : kinds)
                Dump(Capture(k), Numbered({}, "s", f, "-").string() + k, manifest);
        ++g.status.frame;
        if (f % 10 == 0 || f + 1 == frames)
        {
            std::printf("frame %d/%d: %s, %.3f GPU ms\n", f, frames, run ? "ran" : "skipped", gpu);
            std::fflush(stdout);
        }
    }
}
} // namespace framecheck

int wmain(int argc, wchar_t **argv)
{
    int result = 0;
    try
    {
        framecheck::Check(argc >= 2, "usage: amd-nr-framecheck.exe input.ppm [frames=20] | --seq <dir> [key=value ...]");
        if (std::wstring(argv[1]) == L"--seq")
        {
            framecheck::Check(argc >= 3, "--seq needs a directory");
            std::map<std::string, std::string> opt;
            for (int i = 3; i < argc; ++i)
            {
                const auto a = std::filesystem::path(argv[i]).string();
                const auto eq = a.find('=');
                framecheck::Check(eq != std::string::npos, "sequence options are key=value");
                opt[a.substr(0, eq)] = a.substr(eq + 1);
            }
            framecheck::RunSequence(argv[2], opt);
        }
        else
        {
            framecheck::Check(argc <= 3, "usage: amd-nr-framecheck.exe input.ppm [frames=20]");
            const int frames = argc == 3 ? std::stoi(argv[2]) : 20;
            framecheck::Check(frames >= 2 && frames <= 120, "frames must be 2..120");
            framecheck::Run(argv[1], frames);
        }
    }
    catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); result = 1; }
    std::fflush(nullptr);
    // The pinned third-party runtime has no shutdown API. Do not destruct its
    // device/resources while its persistent HIP worker still holds references.
    // ExitProcess enters DLL detach handlers; this runtime changes the exit code
    // there even after a completed capture. The isolated harness flushes its own
    // files first and lets the OS release the worker and its resources together.
    TerminateProcess(GetCurrentProcess(), result);
    return result;
}
