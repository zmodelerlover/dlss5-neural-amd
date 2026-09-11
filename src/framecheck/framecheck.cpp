// Repeat one lossless SDR frame through the actual add-on pipeline, without a game.
// Build: ./build.ps1 -Target framecheck -Exe
// Run in a private directory containing the runtime, weights and dlss5-neural.ini:
//   dlss5-framecheck.exe input.ppm [frames=20]
// PPM must be binary P6, RGB8, with no comments. Outputs are tightly packed raw
// textures plus capture.csv (formats/dimensions) and timings.csv (GPU and wall ms).
// This includes the production implementation so experiments cannot silently use
// a second copy of its shaders or runtime ABI. It never registers with ReShade.
#include "../neural/neural.cpp"
#include <chrono>
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
void Submit(bool notify = false)
{
    Hr(g.list[0]->Close(), "list close");
    ID3D12CommandList *lists[] {g.list[0].Get()};
    g.queue->ExecuteCommandLists(1, lists);
    if (notify)
        reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(g.runtime) + 0x9170)(
            g.queue.Get(), 1, lists);
    const auto value = ++g.ringSerial;
    Hr(g.queue->Signal(g.ringFence.Get(), value), "signal");
    Hr(g.ringFence->SetEventOnCompletion(value, g.ringEvent), "fence event");
    Check(WaitForSingleObject(g.ringEvent, 30000) == WAIT_OBJECT_0, "GPU timed out");
    Hr(g.device->GetDeviceRemovedReason(), "device removed");
    if (notify)
    {
        // A GPU fence alone can complete on the timeout fallback while HIP is
        // still writing. Match the host routes' additional job-completion check
        // before reusing the fixture/output or starting the next evaluation.
        const auto deadline = GetTickCount64() + 30000;
        while (static_cast<UINT>(InterlockedCompareExchange(
            reinterpret_cast<volatile LONG *>(&At<UINT>(g.runtime, 0x8d6f4)), 0, 0)) < g.lastJob)
        {
            Check(GetTickCount64() < deadline, "runtime job did not complete");
            Sleep(1);
        }
    }
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
void Run(const std::filesystem::path &input, int frames)
{
    g_log = _wfopen((ExeDirectory() / L"dlss5-neural.log").c_str(), L"w");
    Check(g_log != nullptr, "open log");
    LoadSettings();
    Check(g.inlineMode && g.passes >= 1 && g.passes <= 3 && !g.useMotion && !g.useHistory &&
          !g.useDepth && !g.useGameGuides && g.temporalMode == 1 && g.encoding == 0,
          "requires inline, 1..3 passes, SDR, temporal forced off and all guides/history off");
    std::ifstream in(input, std::ios::binary);
    std::string magic; UINT w = 0, h = 0, maxValue = 0;
    in >> magic >> w >> h >> maxValue;
    Check(in.good() && magic == "P6" && maxValue == 255 && w >= 64 && h >= 64 &&
          w <= 8192 && h <= 8192, "expected P6 RGB8 input (64..8192 pixels)");
    Check(in.get() == '\n', "PPM header must end with LF");
    std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
    in.read(reinterpret_cast<char *>(rgb.data()), rgb.size());
    Check(static_cast<size_t>(in.gcount()) == rgb.size(), "truncated PPM");
    // The harness has no ReShade hooks; use the system D3D12 module directly.
    Check(LoadLibraryW(L"d3d12.dll") != nullptr && LoadGraphicsApi(), "graphics API");
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
    g.device = g.workDevice; g.queue = g.workQueue;
    Hr(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)), "pass fence");
    Check(InitPipeline() && EnsureResources(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, g.scale), "pipeline/resources");
    ComPtr<ID3D12Resource> source;
    Check(CreateTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, source, "fixture"), "source");
    const Layout l(source.Get());
    auto upload = Buffer(l.bytes, D3D12_HEAP_TYPE_UPLOAD);
    unsigned char *dst = nullptr;
    D3D12_RANGE none {0, 0};
    Hr(upload->Map(0, &none, reinterpret_cast<void **>(&dst)), "upload map");
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            auto *p = dst + l.footprint.Offset + y * l.footprint.Footprint.RowPitch + x * 4;
            std::memcpy(p, rgb.data() + (static_cast<size_t>(y) * w + x) * 3, 3); p[3] = 255;
        }
    upload->Unmap(0, nullptr);
    auto *cmd = Begin();
    Barrier(cmd, source.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION from {}, to {};
    from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = l.footprint;
    to.pResource = source.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(cmd, source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Submit();
    Check(InitEngine(), "engine initialization");
    ComPtr<ID3D12QueryHeap> queries;
    D3D12_QUERY_HEAP_DESC qd {}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = 2;
    Hr(g.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries)), "query heap");
    auto times = Buffer(16, D3D12_HEAP_TYPE_READBACK);
    UINT64 frequency = 0; Hr(g.queue->GetTimestampFrequency(&frequency), "timestamp frequency");
    std::ofstream timing(ExeDirectory() / "timings.csv");
    std::ofstream manifest(ExeDirectory() / "capture.csv");
    timing << "frame,gpu_ms,wall_ms,job,accepted,watchdog_job\n";
    manifest << "file,width,height,dxgi_format\n";
    for (int f = 1; f <= frames; ++f)
    {
        const auto start = std::chrono::steady_clock::now();
        cmd = Begin();
        cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        Check(RecordNetwork(cmd, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, true,
            WantedPasses(), [&]() { return SubmitPrivatePass(cmd, g.alloc[0].Get()); }), "record network");
        cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, times.Get(), 0);
        Submit(g.activePasses != 0);
        const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        UINT64 *ticks = nullptr; D3D12_RANGE range {0, 16};
        Hr(times->Map(0, &range, reinterpret_cast<void **>(&ticks)), "timestamp map");
        const double gpu = (ticks[1] - ticks[0]) * 1000.0 / frequency;
        times->Unmap(0, &none);
        const UINT watchdog = static_cast<UINT>(InterlockedCompareExchange(
            reinterpret_cast<volatile LONG *>(&At<UINT>(g.runtime, 0x8d80c)), 0, 0));
        timing << f << ',' << gpu << ',' << wall << ',' << g.lastJob << ',' << g.activePasses
               << ',' << watchdog << '\n';
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
        ++g.frame;
        std::printf("frame %d: %.3f GPU ms, %.3f wall ms\n", f, gpu, wall);
        std::fflush(stdout);
    }
}
} // namespace framecheck

int wmain(int argc, wchar_t **argv)
{
    int result = 0;
    try
    {
        framecheck::Check(argc >= 2 && argc <= 3, "usage: dlss5-framecheck.exe input.ppm [frames=20]");
        const int frames = argc == 3 ? std::stoi(argv[2]) : 20;
        framecheck::Check(frames >= 2 && frames <= 120, "frames must be 2..120");
        framecheck::Run(argv[1], frames);
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
