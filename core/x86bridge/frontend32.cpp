// Native D3D9/D3D11 x86 frontend. Guide/staging algorithms adapted from upstream neural.cpp.
// See the upstream LICENSE; no engine or private runtime ABI lives in this translation unit.
#include <imgui.h>
#include <reshade.hpp>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <shlobj.h>
#include <unordered_map>
#include <mutex>
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#include <string>
#include "bridge_io.h"
#include "control_state.h"
#include "../shared/hotkey_capture.h"
#include "../shared/ini_text.h"
#include "frontend_port.h"
#include "../shaders/guide_depth.h"
#include "../shared/log_export.h"
#include "../shared/guide_choice.h"
#include <cstring>
using Microsoft::WRL::ComPtr;
using namespace reshade::api;
namespace {
FILE* logFile=nullptr;
HMODULE addonModule=nullptr;
void Log(const char* fmt,...){if(!logFile)return;va_list a;va_start(a,fmt);vfprintf(logFile,fmt,a);va_end(a);fputc('\n',logFile);fflush(logFile);}
std::filesystem::path Directory(){wchar_t b[32768]{};GetModuleFileNameW(addonModule,b,32768);return std::filesystem::path(b).parent_path();}
bool DropRemote();
struct Bridge {
    const char* name="";ComPtr<ID3D11Texture2D> on11;x86bridge::Handle handle;
    UINT width=0,height=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    void Destroy(){handle.reset();on11.Reset();width=height=0;format=DXGI_FORMAT_UNKNOWN;}
    bool Ensure(ID3D11Device* dev,UINT w,UINT h,DXGI_FORMAT fmt,bool uav=false){
        if(on11&&width==w&&height==h&&format==fmt)return true;
        if(!DropRemote())return false;
        Destroy();D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=fmt;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|(uav?D3D11_BIND_UNORDERED_ACCESS:0);
        d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
        if(FAILED(dev->CreateTexture2D(&d,nullptr,&on11)))return false;
        ComPtr<IDXGIResource1> res;HANDLE raw=nullptr;
        if(FAILED(on11.As(&res))||FAILED(res->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&raw))){Destroy();return false;}
        handle.reset(raw);width=w;height=h;format=fmt;return true;
    }
};
struct Guide
{
    const char *name = "";

    ComPtr<ID3D11Resource> chosen;
    UINT chosenBinds = 0;

    ID3D11Resource *challenger = nullptr;
    UINT challengerFrames = 0;
    UINT coldFrames = 0;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D11Texture2D> snap;  
    UINT snapW = 0, snapH = 0;
    DXGI_FORMAT snapFmt = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11ShaderResourceView> srv;
    ID3D11Texture2D *srvOf = nullptr;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ID3D11Texture2D *uavOf = nullptr;

    Bridge bridge;
  
    bool ready = false;
    bool logged = false;
    bool failed = false;
    // Set when the companion effect supplies this guide instead of the game. Mirrored from
    // upstream's Guide so SettleGuide stays an executable copy rather than a lookalike.
    bool external = false;
};

// The guide selection is guide_choice.h, the same code the 64-bit add-on runs.
using namespace guides;
std::unordered_map<void *, Tallied> g_depthTally, g_motionTally;

struct Front {
    std::mutex lock;ComPtr<ID3D11Device> game11;ComPtr<ID3D11DeviceContext> game11ctx;
    ComPtr<ID3D11Texture2D> stageIn11,stageOut11;UINT stageW=0,stageH=0,outWidth=0,outHeight=0;
    DXGI_FORMAT stageFmt=DXGI_FORMAT_UNKNOWN;
    ComPtr<IDirect3DDevice9> game9;ComPtr<IDirect3DTexture9> stageIn9,stageOut9;
    ComPtr<IDirect3DSurface9> stageInSurface9,stageOutSurface9,readback9,upload9;
    ComPtr<ID3D11Texture2D> sharedIn11,sharedOut11,cpuIn11,cpuOut11;
    UINT stage9W=0,stage9H=0;D3DFORMAT stage9Fmt=D3DFMT_UNKNOWN;
    bool nativeD3D9=false,d3d9Shared=false;
    Guide guideDepth,guideMotion;Bridge colour,output;
    ComPtr<ID3D11ComputeShader> guideDepthCs;bool guideDepthCsFailed=false;
    x86bridge::Handle process,pipe,job;DWORD hostPid=0;LUID luid{};
    swapchain* active=nullptr;bool settings=false,enabled=false,failed=false,built=false,reset=true,hidden=false,keyDown=false,transport=false;
    int toggleKey=VK_END,toggleMods=1;bool disableAltTab=false;uint64_t generation=0,frame=0;
    // Pipelined presentation, off unless the ini asks. When on, a frame is posted at the end of one
    // present and its answer collected at the start of the next, so the helper works while the game
    // builds its next frame instead of while the game waits. pendingFrame is what the outstanding
    // answer belongs to, and the raster it was captured at, because an answer that outlived a resize
    // describes a back buffer that no longer exists and must be dropped rather than composed.
    bool async=false,pending=false;x86bridge::Frame pendingFrame{};UINT pendingWidth=0,pendingHeight=0;
} g;
// Opt-in per-stage measurement, off unless AMDNR_X86BRIDGE_TIMING=1.
//
// Tuning the classic D3D9 route blind is what produced the rejected raster experiment: the
// requested raster was altered before anyone knew whether the cost was the CPU round trip, the
// network or the game's own pacing. This separates the three and changes nothing else.
//
// It never issues a query, a flush or a wait of its own. Every boundary it reads is a
// synchronisation the frame already performs, so an enabled probe measures the same frame that
// would have run without it. Keep it that way: a wait added here would land inside the D3D9
// reset window this frontend works hard to keep clear.
struct StageProbe {
    bool on=false;double toMs=0.0;long long mark=0;
    double input=0.0,host=0.0,output=0.0;unsigned frames=0;
    void Arm(bool enabled){
        LARGE_INTEGER f{};
        on=enabled&&QueryPerformanceFrequency(&f)&&f.QuadPart>0;
        if(on)toMs=1000.0/static_cast<double>(f.QuadPart);
    }
    void Begin(){if(!on)return;LARGE_INTEGER c{};if(QueryPerformanceCounter(&c))mark=c.QuadPart;}
    double Split(){
        if(!on)return 0.0;LARGE_INTEGER c{};if(!QueryPerformanceCounter(&c))return 0.0;
        const double ms=static_cast<double>(c.QuadPart-mark)*toMs;mark=c.QuadPart;return ms;
    }
    // Only whole frames that reached the game again are averaged; an abandoned frame is no sample.
    void Keep(double in,double h,double out){if(!on)return;input+=in;host+=h;output+=out;++frames;}
    bool Due() const {return on&&frames>=120;}
    void Drop(){frames=0;input=host=output=0.0;}
    // The game's own pace, measured present to present.
    //
    // The stage numbers above say what the bridge costs. They cannot say what the game costs, and
    // the pipelining estimate needs both: pipelining replaces a frame time of game+bridge with
    // max(game+transport, network), so without the game term the predicted gain is a shape rather
    // than a number. Toggling the effect off in a fixed scene and comparing these two lines
    // supplies it.
    //
    // Sampled before the effect's own early-outs, so it keeps measuring while the effect is off,
    // and it still issues no wait of its own. A window that spans a toggle is discarded rather
    // than averaged, because half of it would be measuring the other thing.
    //
    // A gap past kPeriodOutlierMs is not a rendered frame -- alt-tab, a loading screen, a
    // breakpoint -- and one of them swamps an average of 120, so it is dropped. That biases the
    // result slightly optimistic, which is worth knowing when reading it.
    long long presentMark=0;double period=0.0;unsigned periodFrames=0;
    bool periodEffect=false,periodPipelined=false;
    static constexpr double kPeriodOutlierMs=250.0;
    // Both accumulators are thrown away when the effect is toggled or the presentation mode is
    // switched, because a window spanning either would average two different things and read as
    // one. The mode can now change while the game runs, so this is no longer hypothetical.
    void Present(bool effectOn,bool pipelined){
        if(!on)return;
        LARGE_INTEGER c{};if(!QueryPerformanceCounter(&c))return;
        if(presentMark!=0&&effectOn==periodEffect&&pipelined==periodPipelined){
            const double ms=static_cast<double>(c.QuadPart-presentMark)*toMs;
            if(ms<=kPeriodOutlierMs){period+=ms;++periodFrames;}
        }else{
            if(pipelined!=periodPipelined)Drop();
            period=0.0;periodFrames=0;periodEffect=effectOn;periodPipelined=pipelined;
        }
        presentMark=c.QuadPart;
    }
    bool PeriodDue() const {return on&&periodFrames>=120;}
    void PeriodDrop(){periodFrames=0;period=0.0;}
} probe;
// How stale the overlay stamp may get before a pending rebind is abandoned. It has to
// outlast the gap between the overlay's draw callbacks, which is far longer than a frame:
// 672 ms was measured with the panel open, against the 500 ms this used to allow.
constexpr uint64_t kCaptureIdleMs=3000;
// The control traffic lives in frontend_port.h, shared with the panel adapter in panel32.cpp.
using frontend32::Controls32;using frontend32::controls;
void OperationalSettings(){
    const auto& s=controls.shadow;
    if(g.toggleKey!=s.toggleKey||g.toggleMods!=s.toggleMods)g.keyDown=true;
    if(g.enabled!=(s.enabled!=0)){g.reset=true;if(s.enabled)g.failed=false;}
    g.enabled=s.enabled!=0;g.toggleKey=s.toggleKey;g.toggleMods=s.toggleMods;g.disableAltTab=s.disableOnAltTab!=0;
}
void OperationalChanged(){
    if(controls.synced&&controls.shadow.enabled!=static_cast<uint32_t>(g.enabled)){
        controls.shadow.enabled=g.enabled;++controls.shadow.settings_revision;
    }
}
void StopHost(){
    // A fatal partial operation is never followed by texture reuse in a new frame.
    controls.synced=false;controls.status={};controls.save=controls.reload=controls.factory=controls.measure=controls.liftCap=false;controls.capture.Cancel();
    g.pipe.reset();if(g.process&&WaitForSingleObject(g.process.value,0)!=WAIT_OBJECT_0){
        TerminateProcess(g.process.value,7);WaitForSingleObject(g.process.value,x86bridge::IpcTimeoutMs);
    }
    // The pipe is gone, so the outstanding answer is gone with it. Clearing this here is what
    // makes every Fault path safe without each one remembering to.
    g.pending=false;
    g.job.reset();g.process.reset();g.built=false;g.reset=true;
}
void Fault(const char* reason){Log("x86bridge ORIGINAL: %s (win32=%lu)",reason,GetLastError());g.failed=true;StopHost();}
void FaultHresult(const char* reason,HRESULT hr){Log("x86bridge ORIGINAL: %s (hr=0x%08lX)",reason,static_cast<unsigned long>(hr));g.failed=true;StopHost();}
bool DeferD3D9Failure(const char* operation,HRESULT operationHr){
    if(!g.game9)return false;
    const HRESULT cooperativeHr=g.game9->TestCooperativeLevel();
    const bool resetting=operationHr==D3DERR_DEVICELOST||operationHr==D3DERR_DEVICENOTRESET||
        cooperativeHr==D3DERR_DEVICELOST||cooperativeHr==D3DERR_DEVICENOTRESET;
    if(!resetting)return false;
    Log("x86bridge D3D9 %s skipped during device reset (operation=0x%08lX cooperative=0x%08lX)",
        operation,static_cast<unsigned long>(operationHr),static_cast<unsigned long>(cooperativeHr));
    // This is the normal exclusive-fullscreen Alt+Tab lifecycle, not a bridge failure. Keep the
    // host connected, discard this interrupted frame and make the next generation reset history.
    // destroy_swapchain releases the default-pool resources; the next stable present retires the
    // old remote generation and rebuilds it.
    g.reset=true;
    return true;
}
bool DropRemote(){
    if(!g.built)return !g.failed;
    x86bridge::Ack a;
    if(!x86bridge::Request(g.pipe.value,g.process.value,x86bridge::Kind::Drop,nullptr,0,a)||a.result!=x86bridge::Result::Ready||a.generation!=g.generation){Fault("DROP failed");return false;}
    g.built=false;g.reset=true;return true;
}
bool PrepareGuide(Guide &guide, bool isDepth)
{
    if (guide.failed || guide.chosen == nullptr || g.game11 == nullptr)
        return false;
    const UINT w = guide.width, h = guide.height;
    if (w == 0 || h == 0)
        return false;

    if (!isDepth)
    {
        if (!guide.bridge.Ensure(g.game11.Get(), w, h, guide.format))
        {
            guide.failed = true;
            Log("guide %s: format %u will not share between the devices; giving up on it.",
                guide.name, static_cast<unsigned>(guide.format));
            return false;
        }
        g.game11ctx->CopyResource(guide.bridge.on11.Get(), guide.chosen.Get());
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
        if (FAILED(D3DCompile(shaders::kGuideDepthCs, sizeof(shaders::kGuideDepthCs) - 1, "guide-depth", nullptr,
                              nullptr, "main", "cs_5_0", 0, 0, &blob, &err)) ||
            FAILED(g.game11->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 nullptr, &g.guideDepthCs)))
        {
            g.guideDepthCsFailed = true;
            Log("guide depth: compute shader failed: %s",
                err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "?");
            return false;
        }
    }

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
        // BIND_DEPTH_STENCIL for a depth format, so the CopyResource below is between two
        // resources of the same kind. Same defect and same reasoning as the 64-bit path -- see
        // the comment on this in neural.cpp's PrepareGuide -- and this route had been left with
        // the broken half of it: a planar depth-stencil copied into a plain shader-resource
        // texture comes back as garbage the network is then fed as depth.
        const bool isDepth = GuideDepthSrvFormat(guide.format) != DXGI_FORMAT_UNKNOWN;
        td.BindFlags = isDepth ? (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL)
                               : D3D11_BIND_SHADER_RESOURCE;
        HRESULT made = g.game11->CreateTexture2D(&td, nullptr, &guide.snap);
        if (FAILED(made) && isDepth)
        {
            Log("guide depth: %ux%u format %u was refused as a depth-stencil copy (0x%08lX); "
                "falling back to a plain shader-resource copy, which may not read correctly.",
                w, h, static_cast<unsigned>(guide.format), static_cast<unsigned long>(made));
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            made = g.game11->CreateTexture2D(&td, nullptr, &guide.snap);
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
    g.game11ctx->CopyResource(guide.snap.Get(), guide.chosen.Get());

    if (!guide.bridge.Ensure(g.game11.Get(), w, h, DXGI_FORMAT_R32_FLOAT, true))
    {
        guide.failed = true;
        return false;
    }

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
        if (FAILED(g.game11->CreateUnorderedAccessView(guide.bridge.on11.Get(), &ud, &guide.uav)))
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
        if (FAILED(g.game11->CreateShaderResourceView(guide.snap.Get(), &sd, &guide.srv)))
        {
            guide.failed = true;
            Log("guide depth: SRV over the snapshot failed (fmt %u read as %u).",
                static_cast<unsigned>(guide.format), static_cast<unsigned>(sd.Format));
            return false;
        }
        guide.srvOf = guide.snap.Get();
    }

    ID3D11ComputeShader *oldCs = nullptr;
    ID3D11ShaderResourceView *oldSrv = nullptr;
    ID3D11UnorderedAccessView *oldUav = nullptr;
    g.game11ctx->CSGetShader(&oldCs, nullptr, nullptr);
    g.game11ctx->CSGetShaderResources(0, 1, &oldSrv);
    g.game11ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);

    UINT keep = static_cast<UINT>(-1);
    ID3D11ShaderResourceView *srv = guide.srv.Get();
    ID3D11UnorderedAccessView *uav = guide.uav.Get();
    g.game11ctx->CSSetShader(g.guideDepthCs.Get(), nullptr, 0);
    g.game11ctx->CSSetShaderResources(0, 1, &srv);
    g.game11ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
    g.game11ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    ID3D11ShaderResourceView *nullSrv = nullptr;
    ID3D11UnorderedAccessView *nullUav = nullptr;
    g.game11ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
    g.game11ctx->CSSetShaderResources(0, 1, &nullSrv);
    g.game11ctx->CSSetShader(oldCs, nullptr, 0);
    g.game11ctx->CSSetShaderResources(0, 1, &oldSrv);
    g.game11ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
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
            "frame.",
            w, h, static_cast<unsigned>(guide.format));
    }
    return true;
}

bool EnsureStage(UINT w, UINT h, DXGI_FORMAT fmt)
{
    if (g.stageIn11 != nullptr && g.stageW == w && g.stageH == h && g.stageFmt == fmt)
        return true;
    g.stageIn11.Reset();
    g.stageOut11.Reset();
    g.stageW = g.stageH = 0;
    D3D11_TEXTURE2D_DESC td {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.game11->CreateTexture2D(&td, nullptr, &g.stageIn11)) ||
        FAILED(g.game11->CreateTexture2D(&td, nullptr, &g.stageOut11)))
    {
        Log("bridge: could not create the private staging textures %ux%u fmt %u.", w, h,
            static_cast<unsigned>(fmt));
        g.stageIn11.Reset();
        g.stageOut11.Reset();
        return false;
    }
    g.stageW = w;
    g.stageH = h;
    g.stageFmt = fmt;
    Log("bridge: private staging %ux%u fmt %u, so the back buffer never meets a shared resource.",
        w, h, static_cast<unsigned>(fmt));
    return true;
}

void ReleaseD3D9Stage()
{
    g.cpuIn11.Reset();
    g.cpuOut11.Reset();
    g.sharedIn11.Reset();
    g.sharedOut11.Reset();
    g.readback9.Reset();
    g.upload9.Reset();
    g.stageInSurface9.Reset();
    g.stageOutSurface9.Reset();
    g.stageIn9.Reset();
    g.stageOut9.Reset();
    g.stage9W = g.stage9H = 0;
    g.stage9Fmt = D3DFMT_UNKNOWN;
    g.d3d9Shared = false;
}

// The transport format for the classic CPU route. The X8 pair is carried as its A8 twin, which
// is the same four bytes a pixel with one channel the game does not read -- and the reason
// is not tidiness: B8G8R8X8_UNORM has no typed UAV at all on this hardware, so the x64 host had
// nowhere to write the corrected image and stopped rather than draw garbage. Measured on a Radeon
// RX 9070 XT, driver 32.0.31041: format 88 reports uav=0 typed_store=0, format 87 reports both.
// That is what a D3D9 game with an X8R8G8B8 back buffer -- Oblivion, and most of its generation --
// ran into. The X8B8G8R8 pair below was already carried as R8G8B8A8_UNORM for the same reason.
DXGI_FORMAT D3D9CpuFormat(D3DFORMAT format)
{
    switch (format)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool FindD3D9Adapter(IDirect3DDevice9 *device9, ComPtr<IDXGIAdapter1> &match)
{
    D3DDEVICE_CREATION_PARAMETERS creation {};
    ComPtr<IDirect3D9> d3d9;
    if (FAILED(device9->GetCreationParameters(&creation)) ||
        FAILED(device9->GetDirect3D(&d3d9)))
        return false;
    const HMONITOR monitor = d3d9->GetAdapterMonitor(creation.AdapterOrdinal);
    if (monitor == nullptr)
        return false;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        for (UINT outputIndex = 0;; ++outputIndex)
        {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_OUTPUT_DESC desc {};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
            {
                match = adapter;
                return true;
            }
        }
    }
    return false;
}

bool InitD3D9Bridge(device *reshadeDevice)
{
    // ReShade's API object already exposes the original D3D9 device from get_native(). Querying
    // the private proxy-only unwrapped IID here fails with E_NOINTERFACE on that original object.
    auto *native = reinterpret_cast<IDirect3DDevice9 *>(reshadeDevice->get_native());
    if (native == nullptr)
        return false;
    g.game9 = native;

    ComPtr<IDXGIAdapter1> adapter;
    if (!FindD3D9Adapter(native, adapter))
        return false;
    DXGI_ADAPTER_DESC1 adapterDesc {};
    if (FAILED(adapter->GetDesc1(&adapterDesc)))
        return false;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL level {};
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION, &g.game11, &level, &g.game11ctx);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1,
            static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
            &g.game11, &level, &g.game11ctx);
    if (FAILED(hr) || g.game11 == nullptr || g.game11ctx == nullptr)
        return false;

    g.luid = adapterDesc.AdapterLuid;
    g.nativeD3D9 = true;
    g.guideDepth.name = "depth";
    g.guideMotion.name = "motion";
    Log("x86bridge native D3D9 interop on LUID=%08lX:%08lX feature=%04X",
        g.luid.HighPart, g.luid.LowPart, static_cast<unsigned>(level));
    return true;
}

HRESULT FlushAndWait9();
bool FlushAndWait11();

bool EnsureD3D9Stage(UINT width, UINT height, D3DFORMAT format, DXGI_FORMAT &dxgiFormat)
{
    if (g.stageIn9 != nullptr && g.stage9W == width && g.stage9H == height &&
        g.stage9Fmt == format)
    {
        D3D11_TEXTURE2D_DESC desc {};
        auto *texture = g.d3d9Shared ? g.sharedIn11.Get() : g.cpuIn11.Get();
        if (texture == nullptr)
            return false;
        texture->GetDesc(&desc);
        dxgiFormat = desc.Format;
        return true;
    }

    ReleaseD3D9Stage();
    HANDLE inputHandle = nullptr, outputHandle = nullptr;
    const bool shared = SUCCEEDED(g.game9->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                            format, D3DPOOL_DEFAULT, &g.stageIn9, &inputHandle)) &&
        inputHandle != nullptr &&
        SUCCEEDED(g.game9->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format,
                            D3DPOOL_DEFAULT, &g.stageOut9, &outputHandle)) &&
        outputHandle != nullptr &&
        SUCCEEDED(g.stageIn9->GetSurfaceLevel(0, &g.stageInSurface9)) &&
        SUCCEEDED(g.stageOut9->GetSurfaceLevel(0, &g.stageOutSurface9)) &&
        SUCCEEDED(g.game11->OpenSharedResource(inputHandle, IID_PPV_ARGS(&g.sharedIn11))) &&
        SUCCEEDED(g.game11->OpenSharedResource(outputHandle, IID_PPV_ARGS(&g.sharedOut11)));
    if (shared)
    {
        D3D11_TEXTURE2D_DESC inputDesc {}, outputDesc {};
        g.sharedIn11->GetDesc(&inputDesc);
        g.sharedOut11->GetDesc(&outputDesc);
        if (inputDesc.Width == width && inputDesc.Height == height &&
            inputDesc.SampleDesc.Count == 1 && inputDesc.Format != DXGI_FORMAT_UNKNOWN &&
            outputDesc.Width == width && outputDesc.Height == height &&
            outputDesc.SampleDesc.Count == 1 && outputDesc.Format == inputDesc.Format)
        {
            g.d3d9Shared = true;
            dxgiFormat = inputDesc.Format;
        }
        else
            ReleaseD3D9Stage();
    }

    if (!g.d3d9Shared)
    {
        ReleaseD3D9Stage();
        dxgiFormat = D3D9CpuFormat(format);
        if (dxgiFormat == DXGI_FORMAT_UNKNOWN ||
            FAILED(g.game9->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format,
                D3DPOOL_DEFAULT, &g.stageIn9, nullptr)) ||
            FAILED(g.game9->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format,
                D3DPOOL_DEFAULT, &g.stageOut9, nullptr)) ||
            FAILED(g.stageIn9->GetSurfaceLevel(0, &g.stageInSurface9)) ||
            FAILED(g.stageOut9->GetSurfaceLevel(0, &g.stageOutSurface9)) ||
            FAILED(g.game9->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_SYSTEMMEM,
                &g.readback9, nullptr)) ||
            FAILED(g.game9->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_SYSTEMMEM,
                &g.upload9, nullptr)))
        {
            ReleaseD3D9Stage();
            return false;
        }
        D3D11_TEXTURE2D_DESC cpu {};
        cpu.Width = width;
        cpu.Height = height;
        cpu.MipLevels = cpu.ArraySize = cpu.SampleDesc.Count = 1;
        cpu.Format = dxgiFormat;
        cpu.Usage = D3D11_USAGE_STAGING;
        cpu.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g.game11->CreateTexture2D(&cpu, nullptr, &g.cpuIn11)))
        {
            ReleaseD3D9Stage();
            return false;
        }
        cpu.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g.game11->CreateTexture2D(&cpu, nullptr, &g.cpuOut11)))
        {
            ReleaseD3D9Stage();
            return false;
        }
    }
    g.stage9W = width;
    g.stage9H = height;
    g.stage9Fmt = format;
    Log("bridge: native D3D9 %ux%u fmt %u <-> D3D11 fmt %u using %s staging",
        width, height, static_cast<unsigned>(format), static_cast<unsigned>(dxgiFormat),
        g.d3d9Shared ? "shared GPU" : "classic CPU-compatible");
    return true;
}

HRESULT UploadD3D9Frame(IDirect3DSurface9 *backBuffer)
{
    HRESULT hr = g.game9->StretchRect(backBuffer, nullptr, g.stageInSurface9.Get(), nullptr,
        D3DTEXF_NONE);
    if (FAILED(hr))
        return hr;
    if (g.d3d9Shared)
    {
        if (FAILED(hr = FlushAndWait9()))
            return hr;
        g.game11ctx->CopyResource(g.stageIn11.Get(), g.sharedIn11.Get());
        return S_OK;
    }
    if (FAILED(hr = g.game9->GetRenderTargetData(g.stageInSurface9.Get(), g.readback9.Get())))
        return hr;
    D3DLOCKED_RECT source {};
    D3D11_MAPPED_SUBRESOURCE destination {};
    if (FAILED(hr = g.readback9->LockRect(&source, nullptr, D3DLOCK_READONLY)))
        return hr;
    const HRESULT mapped = g.game11ctx->Map(g.cpuIn11.Get(), 0, D3D11_MAP_WRITE, 0, &destination);
    const size_t rowBytes = static_cast<size_t>(g.stage9W) * 4;
    if (FAILED(mapped) || source.Pitch <= 0 || static_cast<size_t>(source.Pitch) < rowBytes ||
        (SUCCEEDED(mapped) && destination.RowPitch < rowBytes))
    {
        if (SUCCEEDED(mapped))
            g.game11ctx->Unmap(g.cpuIn11.Get(), 0);
        g.readback9->UnlockRect();
        return FAILED(mapped) ? mapped : E_UNEXPECTED;
    }
    for (UINT y = 0; y < g.stage9H; ++y)
        std::memcpy(static_cast<unsigned char *>(destination.pData) + destination.RowPitch * y,
            static_cast<const unsigned char *>(source.pBits) + source.Pitch * y, rowBytes);
    g.game11ctx->Unmap(g.cpuIn11.Get(), 0);
    g.readback9->UnlockRect();
    g.game11ctx->CopyResource(g.stageIn11.Get(), g.cpuIn11.Get());
    return S_OK;
}

HRESULT DownloadD3D9Frame(IDirect3DSurface9 *backBuffer)
{
    if (g.d3d9Shared)
    {
        g.game11ctx->CopyResource(g.sharedOut11.Get(), g.stageOut11.Get());
        if (!FlushAndWait11())
            return E_FAIL;
    }
    else
    {
        g.game11ctx->CopyResource(g.cpuOut11.Get(), g.stageOut11.Get());
        if (!FlushAndWait11())
            return E_FAIL;
        D3D11_MAPPED_SUBRESOURCE source {};
        D3DLOCKED_RECT destination {};
        HRESULT hr = g.game11ctx->Map(g.cpuOut11.Get(), 0, D3D11_MAP_READ, 0, &source);
        if (FAILED(hr))
            return hr;
        const HRESULT locked = g.upload9->LockRect(&destination, nullptr, 0);
        const size_t rowBytes = static_cast<size_t>(g.stage9W) * 4;
        if (FAILED(locked) || destination.Pitch <= 0 ||
            (SUCCEEDED(locked) && static_cast<size_t>(destination.Pitch) < rowBytes) ||
            source.RowPitch < rowBytes)
        {
            if (SUCCEEDED(locked))
                g.upload9->UnlockRect();
            g.game11ctx->Unmap(g.cpuOut11.Get(), 0);
            return FAILED(locked) ? locked : E_UNEXPECTED;
        }
        for (UINT y = 0; y < g.stage9H; ++y)
            std::memcpy(static_cast<unsigned char *>(destination.pBits) + destination.Pitch * y,
                static_cast<const unsigned char *>(source.pData) + source.RowPitch * y, rowBytes);
        g.upload9->UnlockRect();
        g.game11ctx->Unmap(g.cpuOut11.Get(), 0);
        if (FAILED(hr = g.game9->UpdateSurface(g.upload9.Get(), nullptr, g.stageOutSurface9.Get(), nullptr)))
            return hr;
    }
    HRESULT hr = g.game9->StretchRect(g.stageOutSurface9.Get(), nullptr, backBuffer, nullptr,
        D3DTEXF_NONE);
    return FAILED(hr) ? hr : FlushAndWait9();
}

HRESULT FlushAndWait9()
{
    if (g.game9 == nullptr)
        return E_POINTER;
    ComPtr<IDirect3DQuery9> query;
    HRESULT hr = g.game9->CreateQuery(D3DQUERYTYPE_EVENT, &query);
    if (FAILED(hr))
        return hr;
    if (query == nullptr)
        return E_UNEXPECTED;
    if (FAILED(hr = query->Issue(D3DISSUE_END)))
        return hr;
    const ULONGLONG end = GetTickCount64() + 2000;
    hr = S_FALSE;
    while ((hr = query->GetData(nullptr, 0, D3DGETDATA_FLUSH)) == S_FALSE)
    {
        if (GetTickCount64() > end)
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        Sleep(0);
    }
    return hr;
}

void ObserveD3D11(device *dev, const resource_view *rtvs, uint32_t count, resource depthRes)
{
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
            if (d.SampleDesc.Count == 1 && d.ArraySize == 1 &&
                GuideDepthSrvFormat(d.Format) != DXGI_FORMAT_UNKNOWN &&
                (screenW == 0 || (d.Width * 2 >= screenW && d.Height * 2 >= screenH)))
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

bool FlushAndWait11(){
    if(!g.game11||!g.game11ctx)return false;
    D3D11_QUERY_DESC d{};d.Query=D3D11_QUERY_EVENT;ComPtr<ID3D11Query> q;
    if(FAILED(g.game11->CreateQuery(&d,&q)))return false;
    g.game11ctx->End(q.Get());g.game11ctx->Flush();
    const ULONGLONG end=GetTickCount64()+2000;
    HRESULT hr;
    while((hr=g.game11ctx->GetData(q.Get(),nullptr,0,0))==S_FALSE){
        if(FAILED(g.game11->GetDeviceRemovedReason())||GetTickCount64()>end)return false;
        Sleep(0);
    }
    return SUCCEEDED(hr)&&SUCCEEDED(g.game11->GetDeviceRemovedReason());
}
void Settings(){
    if(g.settings)return;g.settings=true;
    const auto dir=Directory();logFile=_wfopen((dir/L"amd-nr-x86.log").c_str(),L"w");
    const auto ini=(dir/L"amd-nr.ini").wstring();
    if(ini_text::StripUtf8Bom(ini))Log("x86bridge: removed a UTF-8 byte-order mark from amd-nr.ini; every setting in it was reading as its default");
    g.enabled=GetPrivateProfileIntW(L"amd-nr",L"StartOn",0,ini.c_str())!=0;
    g.toggleKey=std::clamp(static_cast<int>(GetPrivateProfileIntW(L"amd-nr",L"ToggleKey",VK_END,ini.c_str())),0,255);
    g.toggleMods=std::clamp(static_cast<int>(GetPrivateProfileIntW(L"amd-nr",L"ToggleMods",1,ini.c_str())),0,7);
    g.disableAltTab=GetPrivateProfileIntW(L"amd-nr",L"DisableOnAltTab",0,ini.c_str())!=0;
    wchar_t flag[8]{};g.transport=GetEnvironmentVariableW(L"AMDNR_X86BRIDGE_TRANSPORT_ONLY",flag,8)==1&&flag[0]==L'1';
    // The environment variable alone is not enough for every game. One that re-launches itself
    // through its own launcher ends up loading this add-on into a process that is not a child of
    // whatever set the variable, so it inherits nothing and run-with-timing.cmd reports probe=off
    // there no matter what. GTA IV does exactly that.
    // The ini is already open on the line above and travels with the install, so it always arrives.
    // Pipelining gives up the same-frame guarantee, and it is the default because the measurements
    // say the trade is one-sided: +16% to +41% across the three games measured, for
    // one frame of lag and no image cost. The back buffer is replaced whole, so a pipelined frame is
    // the previous one finished rather than a mix of two, and all three were checked in both modes
    // with no difference seen. Async=0 restores the old behaviour. Ini rather than environment, for
    // the same reason Timing is.
    g.async=GetPrivateProfileIntW(L"amd-nr",L"Async",1,ini.c_str())!=0;
    wchar_t timingFlag[8]{};
    probe.Arm((GetEnvironmentVariableW(L"AMDNR_X86BRIDGE_TIMING",timingFlag,8)==1&&timingFlag[0]==L'1')
              ||GetPrivateProfileIntW(L"amd-nr",L"Timing",0,ini.c_str())!=0);
    Log("x86bridge native x86 protocol=%u mode=%s StartOn=%d ToggleKey=%d ToggleMods=%d probe=%s present=%s",x86bridge::Version,g.transport?"TRANSPORT_ONLY":"NEURAL",g.enabled,g.toggleKey,g.toggleMods,probe.on?"on":"off",g.async?"pipelined":"same-frame");
}
bool StartHost(){
    if(g.process)return WaitForSingleObject(g.process.value,0)==WAIT_TIMEOUT;
    const auto dir=Directory(),exe=dir/L"amd-nr-host64.exe";
    if(GetFileAttributesW(exe.c_str())==INVALID_FILE_ATTRIBUTES)return false;
    LARGE_INTEGER ticks{};QueryPerformanceCounter(&ticks);
    const std::wstring name=L"\\\\.\\pipe\\amd-nr-x86bridge-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(ticks.QuadPart);
    g.pipe.reset(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr));
    if(!g.pipe)return false;
    x86bridge::Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event)return false;
    OVERLAPPED ov{};ov.hEvent=event.value;
    const BOOL connected=ConnectNamedPipe(g.pipe.value,&ov);const DWORD connectionError=connected?ERROR_SUCCESS:GetLastError();
    if(!connected&&connectionError!=ERROR_IO_PENDING&&connectionError!=ERROR_PIPE_CONNECTED)return false;
    const bool pending=!connected&&connectionError==ERROR_IO_PENDING;
    auto cancel=[&](){if(pending){DWORD n=0;CancelIoEx(g.pipe.value,&ov);GetOverlappedResult(g.pipe.value,&ov,&n,TRUE);}};
    g.job.reset(CreateJobObjectW(nullptr,nullptr));JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!g.job||!SetInformationJobObject(g.job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){cancel();return false;}
    std::wstring command=L"\""+exe.wstring()+L"\" \""+name+L"\" "+std::to_wstring(GetCurrentProcessId())+(g.transport?L" --transport-only":L"");
    STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,dir.c_str(),&si,&pi)){cancel();return false;}
    g.process.reset(pi.hProcess);g.hostPid=pi.dwProcessId;x86bridge::Handle thread(pi.hThread);
    if(!AssignProcessToJobObject(g.job.value,g.process.value)||ResumeThread(thread.value)==DWORD(-1)){cancel();return false;}
    if(pending){
        HANDLE waits[]={event.value,g.process.value};DWORD n=0;
        const DWORD connected=WaitForMultipleObjects(2,waits,FALSE,x86bridge::StartupTimeoutMs);
        if(connected!=WAIT_OBJECT_0){
            const DWORD error=connected==WAIT_TIMEOUT?ERROR_TIMEOUT:
                connected==WAIT_OBJECT_0+1?ERROR_BROKEN_PIPE:GetLastError();
            cancel();SetLastError(error);return false;
        }
        if(!GetOverlappedResult(g.pipe.value,&ov,&n,FALSE))return false;
    }
    ULONG client=0;if(!GetNamedPipeClientProcessId(g.pipe.value,&client)||client!=g.hostPid)return false;
    x86bridge::Hello h;h.pid=GetCurrentProcessId();h.luidLow=g.luid.LowPart;h.luidHigh=g.luid.HighPart;
    x86bridge::Ack a;
    const bool ok=x86bridge::Request(g.pipe.value,g.process.value,x86bridge::Kind::Hello,&h,sizeof(h),a,x86bridge::StartupTimeoutMs)&&a.result==x86bridge::Result::Ready&&a.luidLow==h.luidLow&&a.luidHigh==h.luidHigh;
    Log("x86bridge HELLO host_pid=%lu LUID=%08lX:%08lX %s",g.hostPid,g.luid.HighPart,g.luid.LowPart,ok?"MATCH":"FAILED");
    return ok;
}
bool Export(Bridge& source,x86bridge::Texture& t){
    if(!source.on11)return true;HANDLE remote=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),source.handle.value,g.process.value,&remote,0,FALSE,DUPLICATE_SAME_ACCESS))return false;
    t.valid=1;t.width=source.width;t.height=source.height;t.format=source.format;
    t.handle=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(remote));return true;
}
bool BuildRemote(){
    if(g.built)return true;
    x86bridge::Build b;b.generation=++g.generation;
    if(!Export(g.colour,b.colour)||!Export(g.output,b.output)||!Export(g.guideDepth.bridge,b.depth)||!Export(g.guideMotion.bridge,b.motion))return false;
    x86bridge::Ack a;
    if(!x86bridge::Request(g.pipe.value,g.process.value,x86bridge::Kind::Build,&b,sizeof(b),a)||a.result!=x86bridge::Result::Ready||a.generation!=b.generation)return false;
    g.built=true;g.reset=true;return true;
}
bool StateRequest(x86bridge::Kind kind,const void* body=nullptr,uint32_t bytes=0,bool replace=false){
    x86bridge::Ack ack;x86bridge::StateSnapshot snapshot;
    if(!x86bridge::Request(g.pipe.value,g.process.value,kind,body,bytes,ack)||
       !x86bridge::Receive(g.pipe.value,g.process.value,&snapshot,sizeof(snapshot)))return false;
    if(ack.result!=x86bridge::Result::Ready)return false;
    controls.status=snapshot.status;
    if(replace){controls.shadow=snapshot.settings;controls.sentRevision=snapshot.settings.settings_revision;controls.synced=true;OperationalSettings();}
    return true;
}
// The same export the 64-bit panel has (log_export.h), with this route's two logs, looked for both
// beside the add-on and beside the game.
std::wstring ExportBridgeLogs(){
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
    const auto r=logexport::ToDesktop({Directory(),std::filesystem::path(exe).parent_path()},
        {L"amd-nr-x86.log",L"amd-nr-x86-host.log",L"dlssnr_on_amd.log",L"ReShade.log",L"amd-nr.ini"});
    if(r.copied!=0)Log("menu: exported %d file(s) to %ls",r.copied,r.folder.c_str());
    return r.folder.wstring();
}
// Called only from OnPresent, with g.lock held. Never from ImGui or a worker.
bool SyncControls(){
    using x86bridge::Kind;
    if(!controls.synced){
        // Preserve an explicit pre-sync enable/hotkey action; all persistent values still come from host.
        const bool requestedEnable=g.enabled;
        if(!StateRequest(Kind::GetState,nullptr,0,true))return false;
        if(controls.preSyncEnableChanged&&requestedEnable!=g.enabled){g.enabled=requestedEnable;g.reset=true;OperationalChanged();}
        controls.syncRequested=false;controls.preSyncEnableChanged=false;controls.savedRevision=controls.sentRevision;
    }
    if(controls.shadow.settings_revision!=controls.sentRevision){
        if(!StateRequest(Kind::SetState,&controls.shadow,sizeof(controls.shadow),true))return false;
    }
    // savedRevision marks what is on disk. The SetState above has already pushed this revision to the
    // host, so a save that succeeds here writes exactly it -- and Factory Defaults, which only changes
    // memory, deliberately leaves this behind so the overlay's autosave picks it up next frame.
    if(controls.save){if(!StateRequest(Kind::SaveSettings))return false;controls.save=false;controls.savedRevision=controls.sentRevision;}
    if(controls.reload){if(!StateRequest(Kind::ReloadSettings,nullptr,0,true))return false;controls.reload=false;controls.savedRevision=controls.sentRevision;}
    // After the save above, never before it: a log without the settings that produced it cannot be
    // compared against anything, and the panel arms both flags in the same click.
    if(controls.exportLogs&&!controls.save){
        controls.exportedPath=ExportBridgeLogs();controls.exportFailed=controls.exportedPath.empty();controls.exportLogs=false;
    }
    if(controls.factory){x86bridge::WireCommand c;c.id=++controls.commandId;c.code=x86bridge::CommandCode::FactoryDefaults;
        if(!StateRequest(Kind::Command,&c,sizeof(c),true))return false;controls.factory=false;}
    if(controls.measure){x86bridge::WireCommand c;c.id=++controls.commandId;
        if(!StateRequest(Kind::Command,&c,sizeof(c)))return false;controls.measure=false;}
    // Letting go of Scale on the value it already had. A helper older than this command refuses it,
    // and that costs the lift and nothing else, so a refusal here is not a fault: the status request
    // below is what finds a pipe that has really gone.
    if(controls.liftCap){x86bridge::WireCommand c;c.id=++controls.commandId;c.code=x86bridge::CommandCode::LiftScaleCap;
        StateRequest(Kind::Command,&c,sizeof(c));controls.liftCap=false;}
    const uint64_t now=GetTickCount64();
    if(now-controls.lastStatusAt>=250){if(!StateRequest(Kind::Status))return false;controls.lastStatusAt=now;}
    return true;
}
// Switch presentation mode while the game runs.
//
// Safe in both directions, and not by accident: CollectPending runs unconditionally at the top of
// every present, before anything else touches the pipe, so whichever mode the next present picks it
// starts with nothing outstanding. Turning pipelining off collects the answer in flight and drops
// it, costing one corrected frame. Turning it on leaves the first present with no previous answer
// to compose, so that one frame shows the game's own image; at these frame rates it is one frame in
// sixty and has not been visible in testing.
//
// Callers hold g.lock -- the overlay takes it for its whole draw and OnPresent for its whole frame
// -- so the flag cannot change underneath a present that is already running.
//
// Persisted the same way the helper persists its own settings, one key at a time, which leaves
// every other line in the file alone. Rewriting the whole ini from here would drop Timing and
// anything else the helper owns.
void SetAsync(bool async){
    if(g.async==async)return;
    g.async=async;
    probe.Present(g.enabled,g.async);
    Log("x86bridge presentation switched to %s",async?"pipelined":"same-frame");
    WritePrivateProfileStringW(L"amd-nr",L"Async",async?L"1":L"0",
        (Directory()/L"amd-nr.ini").wstring().c_str());
}
void ClearGuide(Guide& v){
    v.chosen.Reset();v.snap.Reset();v.srv.Reset();v.uav.Reset();v.bridge.Destroy();
    v.challenger=nullptr;v.challengerFrames=0;v.chosenBinds=0;v.width=v.height=v.snapW=v.snapH=0;
    v.srvOf=nullptr;v.uavOf=nullptr;v.snapFmt=v.format=DXGI_FORMAT_UNKNOWN;v.ready=v.failed=v.logged=false;
}
// Collect the answer to a frame posted by an earlier present.
//
// The pipe carries one conversation and SyncControls talks on it every present, so this has to run
// before anything else touches the pipe. An uncollected frame answer would be delivered into the
// next unrelated call and the protocol would never recover.
//
// This is also where the pipelined path spends whatever is left of the helper's work. If the game's
// own frame outlasted the network there is nothing left and this returns at once, which is the
// entire point of posting a present early.
//
// got says an answer arrived and matched. The return value says the pipe is still trustworthy;
// false means the caller must fault, because a mismatched or missing answer leaves the conversation
// out of step and no later request on this pipe can be believed.
bool CollectPending(x86bridge::Ack& a,bool& got){
    got=false;
    if(!g.pending)return true;
    g.pending=false;
    if(!x86bridge::Collect(g.pipe.value,g.process.value,x86bridge::Kind::Frame,a))return false;
    if(a.generation!=g.pendingFrame.generation||a.frame!=g.pendingFrame.id)return false;
    got=true;return true;
}
void ReleaseLocal(){
    g_depthTally.clear();g_motionTally.clear();ClearGuide(g.guideDepth);ClearGuide(g.guideMotion);
    ReleaseD3D9Stage();
    g.colour.Destroy();g.output.Destroy();g.stageIn11.Reset();g.stageOut11.Reset();g.stageW=g.stageH=0;
    g.stageFmt=DXGI_FORMAT_UNKNOWN;g.reset=true;
}
void OnBind(command_list* cmd,uint32_t count,const resource_view* targets,resource_view depth){
    if(!cmd)return;auto* dev=cmd->get_device();if(!dev||dev->get_api()!=device_api::d3d11)return;
    std::lock_guard lock(g.lock);
    if(!g.game11||reinterpret_cast<ID3D11Device*>(dev->get_native())!=g.game11.Get())return;
    const auto resource=depth.handle?dev->get_resource_from_view(depth):reshade::api::resource{0};
    ObserveD3D11(dev,targets,count,resource);
}
bool OnDraw(command_list*,uint32_t,uint32_t,uint32_t,uint32_t){return false;}
bool OnDrawIndexed(command_list*,uint32_t,uint32_t,uint32_t,int32_t,uint32_t){return false;}
void OnInit(swapchain* sc,bool){
    if(!sc)return;const auto api=sc->get_device()->get_api();
    if(api!=device_api::d3d11&&api!=device_api::d3d9)return;
    std::lock_guard lock(g.lock);if(g.active==sc)g.reset=true;
}
// Released after g.lock is let go: the D3D9 route's private D3D11 device re-enters OnDestroyDevice on
// its last release, which takes g.lock again; std::mutex throws, and D3D9 games went down on exit.
struct Retired{ComPtr<ID3D11ComputeShader> cs;ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> ctx;ComPtr<IDirect3DDevice9> d9;};void Retire(Retired& r){r.cs.Swap(g.guideDepthCs);r.d11.Swap(g.game11);r.ctx.Swap(g.game11ctx);r.d9.Swap(g.game9);}
void OnDestroy(swapchain* sc,bool resize){
    Retired retired;std::lock_guard lock(g.lock);if(sc!=g.active)return;
    Log("x86bridge retiring swapchain resize=%d",resize);
    // A pipelined frame may still be outstanding. Collect it before anything below touches the pipe
    // or releases a resource the helper is still writing into. This is a bounded pipe read, not a
    // GPU wait and not a driver call, so it does not re-enter the display driver that the rest of
    // this function is written to stay out of.
    if(g.pending){
        x86bridge::Ack drop{};bool got=false;
        if(!CollectPending(drop,got))StopHost();
    }

    // ReShade calls destroy_swapchain from inside IDirect3DDevice9::Reset. At that point the
    // native D3D9 device is already transitioning through its lost/reset state. Submitting an
    // event query here (or waiting on either private GPU device) can re-enter the display driver
    // while it is resetting. GTA IV's exclusive-fullscreen Alt+Tab path fails fast in amdxx32.dll
    // when that happens.
    //
    // Every successful D3D9 presentation has already drained the D3D11 copies and the final
    // StretchRect before returning, so there is no work left to wait for here. Release the D3D9
    // default-pool resources immediately, as Reset requires, and leave the host's imported shared
    // resources alive. The first stable presentation after Reset calls Bridge::Ensure, which sends
    // DROP before allocating the replacement generation. This keeps all IPC and GPU waits outside
    // the driver's reset callback.
    if(resize&&g.nativeD3D9){
        ReleaseLocal();
        Log("x86bridge swapchain retired resize=1 (D3D9 reset; remote retirement deferred)");
        return;
    }

    DropRemote();
    if(g.game11ctx){FlushAndWait11();g.game11ctx->Flush();} // no ClearState: it emptied the game's cache
    if(g.nativeD3D9)FlushAndWait9();
    ReleaseLocal();
    if(!resize){
        if(g.process&&!g.failed){x86bridge::Ack a;x86bridge::Request(g.pipe.value,g.process.value,x86bridge::Kind::Quit,nullptr,0,a);}
        StopHost();g.active=nullptr;controls.runtime=nullptr;Retire(retired);g.nativeD3D9=false;g.guideDepthCsFailed=false;
    }
    Log("x86bridge swapchain retired resize=%d",resize);
}
// Release point for a game that never delivers destroy_swapchain.
//
// Everything this add-on holds is released in OnDestroy, and only there. A game that leaves through
// ExitProcess rather than shutting its renderer down never delivers that callback, which is why
// such a log never carries "retiring swapchain resize=0" -- Log() flushes every line, so an absent
// line is code that did not run rather than a buffer that was lost. Handoff 4.6 names the games. ReShade then finds its device still referenced and writes "Reference count
// for IDirect3DDevice9 ... is inconsistent! Leaking resources".
//
// The reference is real and it is ours. ComPtr AddRefs the game's own device on both routes --
// g.game9 on D3D9, and on D3D11 g.game11 is the game's device rather than a private one -- and on
// D3D9 every staging texture and surface made from it holds one as well.
//
// This does NOT silence ReShade's "Reference count ... is inconsistent" warning, and nothing in an
// add-on can. Measured 2026-09-16 in GTA IV and Half-Life 2 with an unconditional log at the top of
// this function: it printed nothing in either, so ReShade never delivers the event when a game
// leaves through ExitProcess. Its own log shows why that cannot be worked around -- it releases the
// device and warns at 13:17:36, then unloads the add-on at 13:17:38, so the check runs while we are
// still loaded and still holding the reference, and DLL_PROCESS_DETACH is later still. None of
// destroy_device, destroy_swapchain or destroy_effect_runtime arrive on that path. The warning is
// cosmetic: it is written as the process dies and the OS reclaims everything regardless.
//
// The handler is kept because it is correct and free for any game that does shut its renderer down.
//
// destroy_device is the last callback before the device goes, and unlike DLL_PROCESS_DETACH it does
// not run under the loader lock, so COM releases are safe here. No GPU work is issued: the device is
// already on its way out, which is the rule the Reset branch above follows for the same reason. The
// helper is stopped rather than asked to quit, because a bounded IPC round trip is not worth the
// risk at teardown and the kill-on-close job would take it anyway.
//
// Idempotent on purpose. A clean shutdown reaches OnDestroy first and this then finds nothing left.
void OnDestroyDevice(device* dev){
    if(dev==nullptr)return;
    Retired retired;std::lock_guard lock(g.lock);
    const uint64_t native=dev->get_native();
    const uint64_t ours=g.nativeD3D9?reinterpret_cast<uint64_t>(g.game9.Get())
                                    :reinterpret_cast<uint64_t>(g.game11.Get());
    if(ours==0||native!=ours)return;
    Log("x86bridge releasing on device destroy (no destroy_swapchain arrived)");
    StopHost();
    ReleaseLocal();
    g.active=nullptr;controls.runtime=nullptr;Retire(retired);g.nativeD3D9=false;g.guideDepthCsFailed=false;
}
void OnPresent(command_queue*,swapchain* sc,const rect*,const rect*,uint32_t,const rect*){
    if(!sc)return;auto* dev=sc->get_device();const auto api=dev->get_api();
    if(api!=device_api::d3d11&&api!=device_api::d3d9)return;
    std::lock_guard lock(g.lock);Settings();
    probe.Present(g.enabled,g.async);
    if(probe.PeriodDue()){
        const double avg=probe.period/static_cast<double>(probe.periodFrames);
        if(avg>0.0)Log("x86bridge frame period over %u frames: %.2f ms (%.1f FPS) with the effect %s, %s",
            probe.periodFrames,avg,1000.0/avg,probe.periodEffect?"on":"off",
            probe.periodPipelined?"pipelined":"same-frame");
        probe.PeriodDrop();
    }
    struct ClearFrameTallies {~ClearFrameTallies(){g_depthTally.clear();g_motionTally.clear();}} clearFrameTallies;
    if(g.active&&g.active!=sc)return; // one active swapchain per process, never mix resource owners
    if(!g.active){g.active=sc;g.reset=true;}
    HWND hwnd=static_cast<HWND>(sc->get_hwnd());
    const bool foreground=!hwnd||GetForegroundWindow()==hwnd;
    const int mods=((GetAsyncKeyState(VK_CONTROL)&0x8000)?1:0)|((GetAsyncKeyState(VK_MENU)&0x8000)?2:0)|((GetAsyncKeyState(VK_SHIFT)&0x8000)?4:0);
    // Rebinding runs here, on the present path, and not inside the overlay's draw callback.
    // The draw callback is not called every frame -- measured at 672 ms between calls with the
    // panel open -- while this path is. With the scan living in the callback and the 500 ms
    // cancel living here, the cancel always won and a key was never once scanned for.
    // One line per arm/disarm, so a rebind that goes nowhere still says where it stopped.
    static bool wasArmed=false;
    if(controls.capture.armed!=wasArmed){
        wasArmed=controls.capture.armed;
        Log("x86bridge: rebind %s (overlay stamp %llu ms old)",controls.capture.armed?"armed":"disarmed",
            static_cast<unsigned long long>(GetTickCount64()-controls.overlayAt));
    }
    if(controls.capture.armed){
        // A rebind that outlives the panel is abandoned: with the overlay closed the game gets its
        // keyboard back, and the next key pressed in play would silently become the binding.
        if(!controls.runtime||GetTickCount64()-controls.overlayAt>kCaptureIdleMs){
            Log("x86bridge: rebind gave up, overlay stamp %llu ms old",
                static_cast<unsigned long long>(GetTickCount64()-controls.overlayAt));
            controls.capture.Cancel();
        }else{
            // ReShade's key state, never GetAsyncKeyState: it hooks that one and answers 0 for
            // every key while the overlay blocks the keyboard, which is the whole time this panel
            // is open. See hotkey_capture.h.
            auto* const runtime=controls.runtime;
            int boundKey=0,boundMods=0;
            if(controls.capture.Poll([runtime](int vk){return runtime->is_key_down(static_cast<uint32_t>(vk));},boundKey,boundMods)){
                // The binding belongs to the synchronised copy, not to g alone: writing g left the
                // old value on the panel, never reached the host, was never written to the ini, and
                // was overwritten by the next state snapshot. That is what "the bind cannot be
                // changed" looked like even on the one occasion a key was captured.
                controls.shadow.toggleKey=boundKey;controls.shadow.toggleMods=boundMods;
                ++controls.shadow.settings_revision;
                OperationalSettings();
                Log("x86bridge: toggle bound to key %d mods %d",boundKey,boundMods);
            }
        }
    }
    const bool key=!controls.capture.armed&&foreground&&g.toggleKey!=0&&(GetAsyncKeyState(g.toggleKey)&0x8000)&&(mods&g.toggleMods)==g.toggleMods;
    if(key&&!g.keyDown){if(!controls.synced)controls.preSyncEnableChanged=true;g.enabled=!g.enabled;g.reset=true;if(g.enabled)g.failed=false;Log("x86bridge enabled=%d",g.enabled);}
    g.keyDown=key;
    if(g.disableAltTab&&!foreground){if(!controls.synced&&g.enabled)controls.preSyncEnableChanged=true;g.enabled=false;g.reset=true;}
    OperationalChanged();
    if(hwnd&&IsIconic(hwnd)){g.hidden=true;g.reset=true;g_depthTally.clear();g_motionTally.clear();return;}
    if(g.hidden){g.hidden=false;g.reset=true;}
    if((!g.enabled&&!controls.syncRequested&&!controls.synced)||g.failed){g.reset=true;g_depthTally.clear();g_motionTally.clear();return;}
    if(!g.game11){
        if(api==device_api::d3d9){
            if(!InitD3D9Bridge(dev)){Fault("native D3D9/D3D11 interop initialization failed");return;}
        }else{
            g.game11=reinterpret_cast<ID3D11Device*>(dev->get_native());
            if(!g.game11){Fault("D3D11 device absent");return;}g.game11->GetImmediateContext(&g.game11ctx);
            ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC d{};
            if(!g.game11ctx||FAILED(g.game11.As(&dxgi))||FAILED(dxgi->GetAdapter(&adapter))||FAILED(adapter->GetDesc(&d))){Fault("adapter unavailable");return;}
            g.luid=d.AdapterLuid;g.guideDepth.name="depth";g.guideMotion.name="motion";
        }
    }
    if(g.nativeD3D9!=(api==device_api::d3d9)){Fault("graphics API changed for active bridge");return;}
    if(!StartHost()){Fault("helper missing, launch failed, or host died");return;}
    // Before SyncControls, which uses the same pipe. In same-frame mode nothing is ever pending and
    // this is a no-op; the split is measured either way so a log says how much of the helper's work
    // the game's own frame managed to cover.
    probe.Begin();
    x86bridge::Ack pendingAck{};bool havePending=false;
    if(!CollectPending(pendingAck,havePending)){Fault("pipelined frame reply failed or mismatched");return;}
    const double collectMs=probe.Split();
    if(!SyncControls()){Fault("control protocol v2 synchronization failed");return;}
    if(!g.enabled){g.reset=true;return;}
    ComPtr<ID3D11Texture2D> bb;
    ComPtr<IDirect3DSurface9> bb9;
    UINT width=0,height=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    if(g.nativeD3D9){
        D3DSURFACE_DESC d9{};
        if(FAILED(g.game9->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&bb9))||
            FAILED(bb9->GetDesc(&d9))||!d9.Width||!d9.Height){g.reset=true;return;}
        width=d9.Width;height=d9.Height;
        if(!EnsureD3D9Stage(width,height,d9.Format,format)){Fault("D3D9 staging unavailable or unsupported back-buffer format");return;}
    }else{
        const auto back=sc->get_current_back_buffer();if(!back.handle){g.reset=true;return;}
        if(FAILED(reinterpret_cast<ID3D11Resource*>(back.handle)->QueryInterface(IID_PPV_ARGS(&bb)))){Fault("backbuffer texture unavailable");return;}
        D3D11_TEXTURE2D_DESC d{};bb->GetDesc(&d);
        if(!d.Width||!d.Height||d.SampleDesc.Count!=1||d.ArraySize!=1||d.MipLevels!=1){g.reset=true;return;}
        width=d.Width;height=d.Height;format=d.Format;
    }
    g.outWidth=width;g.outHeight=height;
    if(!g.colour.Ensure(g.game11.Get(),width,height,format)||!g.output.Ensure(g.game11.Get(),width,height,format)||!EnsureStage(width,height,format)){Fault("colour/staging resources unavailable");return;}
    probe.Begin();
    if(g.nativeD3D9){
        const HRESULT uploadHr=UploadD3D9Frame(bb9.Get());
        if(FAILED(uploadHr)){if(DeferD3D9Failure("input copy",uploadHr))return;FaultHresult("D3D9 input copy did not complete",uploadHr);return;}
    }else g.game11ctx->CopyResource(g.stageIn11.Get(),bb.Get());
    g.game11ctx->CopyResource(g.colour.on11.Get(),g.stageIn11.Get());
    SettleGuide(g.guideDepth,g_depthTally,Log);SettleGuide(g.guideMotion,g_motionTally,Log);
    g.guideDepth.ready=g.guideMotion.ready=false;
    PrepareGuide(g.guideDepth,true);PrepareGuide(g.guideMotion,false);
    if(g.failed)return;
    if(!FlushAndWait11()){Fault("input D3D11 queue not drained");return;}
    if(!BuildRemote()){Fault("resource export/BUILD failed");return;}
    x86bridge::Frame f;f.generation=g.generation;f.id=++g.frame;f.resetHistory=g.reset;
    f.depthValid=g.guideDepth.ready;f.motionValid=g.guideMotion.ready;
    const double inputMs=probe.Split();
    // Same-frame mode asks and waits right here, so the answer belongs to the frame just captured.
    // Pipelined mode collected the previous present's answer before SyncControls and posts this
    // frame further down, after that answer has been composed -- the helper writes into one output
    // texture, so it must not be given new work until the last result has left it.
    x86bridge::Ack a{};x86bridge::Frame answeredFrame{};bool answered=false;
    if(g.async){
        // Two ways an answer can outlive what it describes, and Confirmed catches neither, because
        // the answer agrees with the frame that asked -- it is the world underneath that moved.
        // A resize leaves it describing a back buffer that no longer exists. A rebuild in
        // BuildRemote above, which happens between the post and here, increments the generation and
        // replaces g.output, so the result was written into the texture that has just been retired.
        // Drop it either way: the cost is one original frame, against composing a stale or
        // mismatched surface.
        if(havePending&&g.pendingFrame.generation==g.generation&&g.pendingWidth==width&&g.pendingHeight==height){
            a=pendingAck;answeredFrame=g.pendingFrame;answered=true;
        }else if(havePending)Log("x86bridge pipelined frame %llu dropped: generation %llu->%llu raster %ux%u->%ux%u",
            static_cast<unsigned long long>(g.pendingFrame.id),
            static_cast<unsigned long long>(g.pendingFrame.generation),static_cast<unsigned long long>(g.generation),
            g.pendingWidth,g.pendingHeight,width,height);
    }else{
        if(!x86bridge::Request(g.pipe.value,g.process.value,x86bridge::Kind::Frame,&f,sizeof(f),a)||a.generation!=f.generation||a.frame!=f.id){Fault("frame reply failed/mismatched");return;}
        answeredFrame=f;answered=true;
    }
    const double requestMs=probe.Split();
    g.reset=false;
    if(answered&&a.result==x86bridge::Result::Error){Fault("host error");return;}
    if(answered&&x86bridge::Confirmed(a,answeredFrame,g.transport)){
        if(FAILED(g.game11->GetDeviceRemovedReason())){Fault("D3D11 device removed");return;}
        g.game11ctx->CopyResource(g.stageOut11.Get(),g.output.on11.Get());
        if(g.nativeD3D9){
            const HRESULT downloadHr=DownloadD3D9Frame(bb9.Get());
            if(FAILED(downloadHr)){if(DeferD3D9Failure("output copy",downloadHr))return;FaultHresult("D3D9 output copy did not complete",downloadHr);return;}
        }else{
            g.game11ctx->CopyResource(bb.Get(),g.stageOut11.Get());
            if(!FlushAndWait11()){Fault("return D3D11 queue not drained");return;}
        }
        g.game11ctx->Flush();
        probe.Keep(inputMs,collectMs+requestMs,probe.Split());
    }else if(answered&&a.result!=x86bridge::Result::Original){Fault("unexpected presentation status");return;}
    // Hand this frame over last, so the helper starts on it while the game builds the next one.
    // Everything the helper reads was drained above and everything it writes has just been consumed,
    // so a single set of textures is enough and no extra copy is introduced to allow the overlap.
    if(g.async){
        if(!x86bridge::Post(g.pipe.value,g.process.value,x86bridge::Kind::Frame,&f,sizeof(f))){Fault("pipelined frame post failed");return;}
        g.pendingFrame=f;g.pendingWidth=width;g.pendingHeight=height;g.pending=true;
    }
    if(g.frame<=3||g.frame%120==0)Log("x86bridge frame=%llu result=%u same_frame=1 depth=%u motion=%u",f.id,static_cast<unsigned>(a.result),f.depthValid,f.motionValid);
    if(probe.Due()){
        const double n=static_cast<double>(probe.frames);
        Log("x86bridge stage probe over %u frames: input+prepare %.2f ms, host %.2f ms, output %.2f ms, bridge total %.2f ms (%s, %s)",
            probe.frames,probe.input/n,probe.host/n,probe.output/n,(probe.input+probe.host+probe.output)/n,
            g.nativeD3D9?(g.d3d9Shared?"D3D9 shared GPU staging":"D3D9 classic CPU-compatible staging"):"D3D11 direct",
            g.async?"pipelined":"same-frame");
        probe.Drop();
    }
}
}
extern "C" __declspec(dllexport) const char* NAME="AMD Neural Rendering (32-bit)";
extern "C" __declspec(dllexport) const char* DESCRIPTION="Native D3D9/D3D11 x86 to original x64 neural engine; same-frame CPU barriers.";
// The frontend's side of frontend_port.h: the few things the panel adapter reads or switches.
namespace frontend32 {
Controls32 controls;
std::mutex& FrameLock(){return g.lock;}
bool HostFailed(){return g.failed;}
void RetryHost(){g.failed=false;}
bool Pipelined(){return g.async;}
void SwitchPipelining(bool on){SetAsync(on);}
void ApplyOperational(){OperationalSettings();}
std::vector<std::string> GuideCandidates(){
    std::vector<std::string> lines;char line[128];
    for(const Guide* v:{&g.guideDepth,&g.guideMotion}){
        std::snprintf(line,sizeof(line),"%s candidate=%d %ux%u format=%u last_capture_valid=%d",
                      v==&g.guideDepth?"Depth":"Motion",v->chosen.Get()!=nullptr,v->width,v->height,
                      static_cast<unsigned>(v->format),v->ready);
        lines.push_back(line);
    }
    return lines;
}
} // namespace frontend32
using frontend32::OnOverlay32;
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){
        addonModule=module;
        if(!reshade::register_addon(module))return FALSE;
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBind);
        reshade::register_event<reshade::addon_event::draw>(OnDraw);
        reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
        reshade::register_event<reshade::addon_event::init_swapchain>(OnInit);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroy);
        reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
        reshade::register_overlay("AMD Neural Rendering (32-bit)",OnOverlay32);
    }else if(reason==DLL_PROCESS_DETACH){
        reshade::unregister_overlay("AMD Neural Rendering (32-bit)",OnOverlay32);
        reshade::unregister_addon(module);
        // No waits or graphics calls while holding the loader lock. Normal retirement is
        // in destroy_swapchain. The private kill-on-close job isolates forced unload/exit.
        g.job.reset();g.pipe.reset();g.process.reset();if(logFile){fclose(logFile);logFile=nullptr;}
    }
    return TRUE;
}
