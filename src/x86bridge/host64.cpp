// Additive x64 process boundary. The upstream engine is compiled verbatim in this TU.
#define AMDNR_WITH_VULKAN 0
#include "../neural/neural.cpp"
#include "bridge_io.h"
#include "control_state.h"
#include <stdexcept>

namespace x86host {
using namespace x86bridge;
void Check(HRESULT hr,const char* what){if(FAILED(hr))throw std::runtime_error(what);}
void Require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
struct Host {
    Handle parent,pipe;
    bool transport=false,built=false;uint64_t generation=0,lastFrame=0;
    Build spec{};
    WireSettings factoryDefaults{};
    uint64_t settingsRevision=1,lastCommand=0,processedFrames=0,skippedFrames=0;
    void ForceInline(){
        if(!g.inlineMode.load())Log("x86bridge forced same-frame: Inline=0 is unsupported on this route");
        g.inlineMode.store(true);
    }
    WireSettings ExportSettings(){
        WireSettings s;s.settings_revision=settingsRevision;
#define X(type,name,low,high) s.name=static_cast<type>(g.name.load());
#include "settings_fields.inc"
#undef X
        for(unsigned i=0;i<3;++i){s.passOverride[i]=g.passOverride[i].load();s.passStructure[i]=g.passStructure[i].load();s.passTone[i]=g.passTone[i].load();s.passSkin[i]=g.passSkin[i].load();}
        return s;
    }
    bool ApplySettings(WireSettings s){
        if(!NewRevision(s.settings_revision,settingsRevision))return false;
        if(!NormalizeSettings(s))return false;
        const bool historyChanged=g.useHistory.load()!=(s.useHistory!=0);
#define X(type,name,low,high) g.name.store(s.name);
#include "settings_fields.inc"
#undef X
        for(unsigned i=0;i<3;++i){g.passOverride[i].store(s.passOverride[i]!=0);g.passStructure[i].store(s.passStructure[i]);g.passTone[i].store(s.passTone[i]);g.passSkin[i].store(s.passSkin[i]);}
        if(historyChanged)g.historyValid.store(false);
        settingsRevision=s.settings_revision;return true;
    }
    WireStatus ExportStatus(){
        WireStatus s;s.connected=1;s.transportOnly=transport;
        s.processed=processedFrames;s.skipped=skippedFrames;
        s.engineReady=!transport&&g.runtime!=nullptr&&!g.unavailable&&!g.failed;
        s.unavailable=g.unavailable;s.failed=g.failed;
        s.outWidth=spec.colour.width;s.outHeight=spec.colour.height;s.netWidth=g.netWidth;s.netHeight=g.netHeight;
        s.loadedPasses=g.loadedPasses;s.activePasses=g.activePasses;
        s.depthActive=!transport&&built&&g.gameDepthActive;s.motionActive=!transport&&built&&g.gameMotionActive;
        s.probeValid=!transport&&built&&g.probeStillPct.load()>=0;
        s.depthMin=g.probeDepthMin.load();s.depthMax=g.probeDepthMax.load();s.motionMean=g.probeMotionMean.load();s.motionMax=g.probeMotionMax.load();s.stillPct=g.probeStillPct.load();
        s.stage=g.stage.load();s.events=g.events;s.noBridge=g.noBridge.load();s.noBackBuffer=g.noBackBuffer.load();
        s.scaleCap=g.scaleCap.load();return s;
    }
    void Snapshot(Kind kind,Result result=Result::Ready){
        StateSnapshot s{ExportSettings(),ExportStatus()};Reply(kind,result);
        Require(Send(pipe.value,parent.value,&s,sizeof(s)),"state snapshot write failed");
    }

    void CaptureFactoryDefaults(){
        factoryDefaults=ExportSettings(); // g's constructed defaults, BEFORE LoadSettings.
        factoryDefaults=FactorySettings(factoryDefaults,factoryDefaults);
    }
    void EnsureX86Ini(){
        const auto ini=ExeDirectory()/L"amd-nr.ini";
        std::error_code ec;const bool existed=std::filesystem::exists(ini,ec);
        EnsureNeuralIni();
        if(!existed&&!ec){
            g.colourStrength.store(0.25f);g.structure.store(1);g.skin.store(-1);g.passes.store(1);
            WritePrivateProfileStringW(L"amd-nr",L"ColourStrength",L"0.25",ini.c_str());
            WritePrivateProfileStringW(L"amd-nr",L"Structure",L"1",ini.c_str());
            // -1 is the engine's automatic. A fresh ini used to write 1 here, which switched it off
            // before anybody had touched a control -- the panel's Auto skin box then came up
            // unticked on this route and ticked on the other, for the same shipped defaults.
            WritePrivateProfileStringW(L"amd-nr",L"Skin",L"-1",ini.c_str());
            WritePrivateProfileStringW(L"amd-nr",L"Passes",L"1",ini.c_str());
        }
    }
    void RestoreFactoryDefaults(){
        // No INI access. Restore constructed upstream defaults plus the five x86 overrides.
        auto s=FactorySettings(factoryDefaults,ExportSettings());
        const bool historyChanged=(s.useHistory!=0)!=g.useHistory.load();
        const bool guidesChanged=historyChanged||(s.useMotion!=0)!=g.useMotion.load()||
            (s.useDepth!=0)!=g.useDepth.load()||(s.useGameGuides!=0)!=g.useGameGuides.load()||s.temporalMode!=g.temporalMode.load()||
            s.motionScale!=g.motionScale.load()||s.flowGate!=g.flowGate.load()||s.flowRatio!=g.flowRatio.load();
        Require(ApplySettings(s),"factory defaults rejected");
        if(guidesChanged&&!historyChanged)g.historyValid.store(false);
    }
    void Init(const Hello& h){
        Require(h.pid==GetProcessId(parent.value),"parent PID mismatch");
        CaptureFactoryDefaults();EnsureX86Ini();LoadSettings();ForceInline();Require(LoadGraphicsApi(),"graphics API load failed");
        ComPtr<IDXGIFactory4> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");
        LUID luid{h.luidLow,h.luidHigh};ComPtr<IDXGIAdapter1> adapter;
        Check(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter)),"exact LUID adapter unavailable");
        Require(CreateWorkDevice(adapter.Get(),luid),"work device creation failed");
        const LUID got=g.bridge.workDevice->GetAdapterLuid();
        const bool match=got.LowPart==luid.LowPart&&got.HighPart==luid.HighPart;
        Log("x86bridge game LUID=%08lX:%08lX host LUID=%08lX:%08lX %s",luid.HighPart,luid.LowPart,got.HighPart,got.LowPart,match?"MATCH":"MISMATCH");
        Require(match,"LUID mismatch");g.device=g.bridge.workDevice;g.queue=g.bridge.workQueue;
        Check(g.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&g.fence)),"completion fence");
        Require(g.ringEvent&&g.completionEvent,"fence events unavailable");
        Log("x86bridge mode=%s; no IPC GPU fences",transport?"TRANSPORT_ONLY (not neural)":"ORIGINAL_ENGINE");
    }
    void Idle(){
        if(!g.bridge.workQueue)return;
        g.completion=++g.serial;Check(g.bridge.workQueue->Signal(g.fence.Get(),g.completion),"idle signal");
        WaitForWorkQueue(g.completion);
        Require(!DeviceLost()&&g.fence->GetCompletedValue()!=UINT64_MAX&&g.fence->GetCompletedValue()>=g.completion,"queue did not finish");
    }
    void Drop(){
        Idle();ReleaseSwapchainSized();built=false;g.historyValid.store(false);
        Require(!g.bridge.failed,"resource retirement failed");
    }
    void Import(const Texture& t,ComPtr<ID3D12Resource>& out){
        out.Reset();if(!t.valid)return;
        Handle handle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(t.handle)));
        Check(g.device->OpenSharedHandle(handle.value,IID_PPV_ARGS(&out)),"OpenSharedHandle failed");
        const auto d=out->GetDesc();
        Require(d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width==t.width&&d.Height==t.height&&d.Format==static_cast<DXGI_FORMAT>(t.format)&&d.MipLevels==1&&d.DepthOrArraySize==1&&d.SampleDesc.Count==1,"import description mismatch");
    }
    void Resources(const Build& b){
        Require(!built&&ValidBuild(b)&&b.generation>generation,"invalid resource generation/shape");
        Require(!b.depth.valid||b.depth.format==DXGI_FORMAT_R32_FLOAT,"invalid depth transport format");
        Require(!b.motion.valid||b.motion.format==DXGI_FORMAT_R16G16_FLOAT||b.motion.format==DXGI_FORMAT_R32G32_FLOAT||b.motion.format==DXGI_FORMAT_R16G16_SNORM,"invalid motion transport format");
        Import(b.colour,g.bridge.in.on12);Import(b.output,g.bridge.out.on12);
        Import(b.depth,g.guideDepth.bridge.on12);Import(b.motion,g.guideMotion.bridge.on12);
        g.guideDepth.name="depth";g.guideMotion.name="motion";
        generation=b.generation;spec=b;built=true;g.historyValid.store(false);
        Log("x86bridge BUILD generation=%llu %ux%u depth=%u motion=%u",generation,b.colour.width,b.colour.height,b.depth.valid,b.motion.valid);
    }
    Result CopyOnly(){
        const UINT i=static_cast<UINT>(g.bridge.backValue%State::kRing);
        Require(WaitFence(g.ringFence.Get(),g.ringValue[i],g.ringEvent,"transport slot"),"transport ring wait");
        Check(g.alloc[i]->Reset(),"transport allocator");Check(g.list[i]->Reset(g.alloc[i].Get(),nullptr),"transport list");
        if(!g.bridge.crossLocal){
            auto d=g.bridge.in.on12->GetDesc();d.Flags=D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
            Check(g.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&g.bridge.crossLocal)),"transport local texture");
        }
        auto* cmd=g.list[i].Get();cmd->CopyResource(g.bridge.crossLocal.Get(),g.bridge.in.on12.Get());
        Barrier(cmd,g.bridge.crossLocal.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(g.bridge.out.on12.Get(),g.bridge.crossLocal.Get());
        Barrier(cmd,g.bridge.crossLocal.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        Check(cmd->Close(),"transport close");ID3D12CommandList* lists[]={cmd};g.bridge.workQueue->ExecuteCommandLists(1,lists);
        g.ringValue[i]=++g.ringSerial;Check(g.bridge.workQueue->Signal(g.ringFence.Get(),g.ringSerial),"transport ring signal");
        ++g.bridge.backValue;Idle();return Result::Transport;
    }
    Result Neural(){
        if(g.bridge.failed)throw std::runtime_error("work slot unavailable");
        if(g.failed||g.unavailable||DeviceLost())return Result::Original;
        if(g.noBridge.load()||g.stage.load()<3)return Result::Original;
        UINT wanted=WantedPasses();if(!BringUpEngines(wanted)){g.unavailable=true;return Result::Original;}
        g.loadedPasses=wanted;
        const UINT w=spec.colour.width,h=spec.colour.height;const auto fmt=static_cast<DXGI_FORMAT>(spec.colour.format);
        if(!EnsureResources(w,h,fmt,g.scale.load())){g.unavailable=true;return Result::Original;}
        if(!g.bridge.crossLocal && !CreateTexture(w,h,fmt,g.bridge.crossLocal,"crossLocal",D3D12_RESOURCE_STATE_COPY_DEST))return Result::Original;
    bool runNetwork = true;
    const bool jobPending = g.fence->GetCompletedValue() < g.completion || RuntimeBusy();
    if (!jobPending && g.jobRunning)
    {
        g.jobRunning = false;
        NoteJobCost(GetTickCount64() - g.lastJobAt);
    }
    if (jobPending && GetTickCount64() - g.lastJobAt < 500)
    {
        runNetwork = false;
        if (++g.skipped % 120 == 1)
            Log("network skipped: previous evaluation still pending (%llu skipped, %llu done). Those "
                    "frames go out as the game drew them; a correction aimed at an older picture "
                    "reads as a trail, not as detail.",
                static_cast<unsigned long long>(g.skipped),
                static_cast<unsigned long long>(g.frame));
    }
    else if (jobPending)
    {
        ResetJobs();
    }

    const UINT i = static_cast<UINT>(g.bridge.backValue % State::kRing);

    if (g.ringValue[i] != 0 &&
        !WaitFence(g.ringFence.Get(), g.ringValue[i], g.ringEvent, "the ring slot to come free"))
        return x86bridge::Result::Original;
    const HRESULT allocatorHr = g.alloc[i]->Reset();
    const HRESULT listHr = SUCCEEDED(allocatorHr)
                               ? g.list[i]->Reset(g.alloc[i].Get(), nullptr)
                               : allocatorHr;
    if (FAILED(allocatorHr) || FAILED(listHr))
    {
        Log("bridge: work slot %u reset failed (allocator 0x%08lX, list 0x%08lX); "
            "recreating the pair.", i, allocatorHr, listHr);
        if (!RecreateWorkSlot(i))
            g.bridge.failed = true;
        return x86bridge::Result::Original;
    }
    auto *cmd = g.list[i].Get();
    cmd->CopyResource(g.bridge.crossLocal.Get(), g.bridge.in.on12.Get());

    auto localise = [&](Guide &guide) {
        if (!guide.ready || guide.bridge.on12 == nullptr)
            return false;
        const auto want = guide.bridge.on12->GetDesc();
        if (guide.local == nullptr || guide.local->GetDesc().Width != want.Width ||
            guide.local->GetDesc().Height != want.Height ||
            guide.local->GetDesc().Format != want.Format)
        {
            guide.local.Reset();
            if (!CreateTexture(static_cast<UINT>(want.Width), want.Height, want.Format,
                               guide.local, guide.name))
                return false;
        }
        Barrier(cmd, guide.local.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(guide.local.Get(), guide.bridge.on12.Get());
        Barrier(cmd, guide.local.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return true;
    };
    g.gameDepthActive = localise(g.guideDepth);
    g.gameMotionActive = localise(g.guideMotion);
    Barrier(cmd, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool ok =
        RecordNetwork(cmd, g.bridge.crossLocal.Get(), fmt, nullptr, runNetwork, g.loadedPasses,
            [&]() { return SubmitPrivatePass(cmd, g.alloc[i].Get()); });
    if (!ok && g.failed)
        throw std::runtime_error("engine recording failed");
    Barrier(cmd, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    if (ok && CompositionIsFresh(runNetwork) && runNetwork && g.activePasses != 0 && !g.noBackBuffer.load())
    {
        Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(g.bridge.out.on12.Get(), g.composed.Get());
        Barrier(cmd, g.composed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (const HRESULT hr = cmd->Close(); FAILED(hr))
    {

        Log("bridge: closing the command list failed (0x%08lX); rebuilding.", hr);
        g.lastJob = 0;
        g.activePasses = 0;
        g.historyValid.store(false);
        if (!RecreateWorkSlot(i))
            g.bridge.failed = true;
        return x86bridge::Result::Original;
    }
    ID3D12CommandList *lists[] { cmd };
    g.bridge.workQueue->ExecuteCommandLists(1, lists);
    if (g.activePasses != 0)
        reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(g.runtime) + rt::kNotifyFn)(
            g.bridge.workQueue.Get(), 1, lists);
    g.ringValue[i] = ++g.ringSerial;
    Check(g.bridge.workQueue->Signal(g.ringFence.Get(), g.ringSerial), "ring signal");
    g.completion = ++g.serial;
    Check(g.bridge.workQueue->Signal(g.fence.Get(), g.completion), "completion signal");

        ++g.bridge.backValue;WaitForWorkQueue(g.completion);
        Require(!DeviceLost()&&g.fence->GetCompletedValue()!=UINT64_MAX&&g.fence->GetCompletedValue()>=g.completion,"output completion failed");
        const bool fresh=ok&&CompositionIsFresh(runNetwork)&&runNetwork&&g.activePasses!=0&&!g.noBackBuffer.load();
        DrainReadbacks(g.netWidth,g.netHeight);
        return fresh?Result::Neural:Result::Original;
    }
    Result FrameWork(const Frame& f){
        Require(built&&f.generation==generation&&f.id>lastFrame&&f.depthValid<=1&&f.motionValid<=1&&f.resetHistory<=1,"invalid frame/generation");
        Require((!f.depthValid||spec.depth.valid)&&(!f.motionValid||spec.motion.valid),"unbuilt guide requested");
        lastFrame=f.id;
        if(f.resetHistory)g.historyValid.store(false);
        g.guideDepth.ready=f.depthValid&&g.useGameGuides.load()&&g.useDepth.load();
        g.guideMotion.ready=f.motionValid&&g.useGameGuides.load()&&g.useMotion.load();
        const auto result=transport?CopyOnly():Neural();
        if(result==Result::Neural||result==Result::Transport)++processedFrames;else ++skippedFrames;
        ++g.frame;
        if(g.frame<=3||g.frame%120==0)Log("x86bridge frame=%llu engine_frame=%llu result=%u",f.id,g.frame,static_cast<unsigned>(result));
        return result;
    }
    void Reply(Kind kind,Result result,uint64_t frame=0){
        Ack a;a.header.kind=kind;a.header.bytes=sizeof(a)-sizeof(a.header);a.result=result;a.generation=generation;a.frame=frame;
        if(g.bridge.workDevice){const auto luid=g.bridge.workDevice->GetAdapterLuid();a.luidLow=luid.LowPart;a.luidHigh=luid.HighPart;}
        Require(Send(pipe.value,parent.value,&a,sizeof(a)),"ACK write failed");
    }
    void Run(){
        Header h;Require(Receive(pipe.value,parent.value,&h,sizeof(h))&&ValidHeader(h)&&h.kind==Kind::Hello,"HELLO header");
        Hello hello;Require(Receive(pipe.value,parent.value,&hello,sizeof(hello)),"HELLO body");Init(hello);Reply(Kind::Hello,Result::Ready);
        for(;;){
            // Waiting for the next request while the game is idle is intentionally unbounded.
            // Every operation after a header, and every frontend wait, remains bounded.
            Require(Receive(pipe.value,parent.value,&h,sizeof(h),INFINITE)&&ValidHeader(h),"IPC header/peer closed");
            switch(h.kind){
            case Kind::GetState:case Kind::Status:Snapshot(h.kind);break;
            case Kind::SetState:{WireSettings s;Require(Receive(pipe.value,parent.value,&s,sizeof(s)),"SET_STATE body");
                const float wanted=g.scale.load();
                const bool applied=ApplySettings(s);
                // A new Scale arriving is the person overruling the cap NoteJobCost put on. Letting go
                // of the slider on an unchanged value sends no revision; that one arrives as the
                // LiftScaleCap command below, as it does on the 64-bit route.
                if(applied&&g.scale.load()!=wanted){g.scaleCap.store(0.0f);g.longJobs=0;}
                Snapshot(h.kind,applied?Result::Ready:Result::Error);break;}
            case Kind::SaveSettings:SaveSettings();Snapshot(h.kind);break;
            case Kind::ReloadSettings:{const bool history=g.useHistory.load();LoadSettings();ForceInline();if(history!=g.useHistory.load())g.historyValid.store(false);++settingsRevision;Snapshot(h.kind);break;}
            case Kind::Command:{WireCommand c;Require(Receive(pipe.value,parent.value,&c,sizeof(c)),"COMMAND body");
                const bool accepted=NewCommand(c,lastCommand)&&(c.code==CommandCode::FactoryDefaults||!transport);
                if(accepted){lastCommand=c.id;if(c.code==CommandCode::FactoryDefaults)RestoreFactoryDefaults();
                    else if(c.code==CommandCode::LiftScaleCap){g.scaleCap.store(0.0f);g.longJobs=0;}
                    else {g.measured=false;g.measureTries=0;g.measureNow.store(true);}}
                Snapshot(h.kind,accepted?Result::Ready:Result::Error);break;}
            case Kind::Build:{Build b;Require(Receive(pipe.value,parent.value,&b,sizeof(b)),"BUILD body");Resources(b);Reply(h.kind,Result::Ready);break;}
            case Kind::Frame:{Frame f;Require(Receive(pipe.value,parent.value,&f,sizeof(f)),"FRAME body");try{Result result=FrameWork(f);Reply(h.kind,result,f.id);}
                catch(...){Reply(h.kind,Result::Error,f.id);throw;}break;}
            case Kind::Drop:Drop();Reply(h.kind,Result::Ready);break;
            case Kind::Quit:Drop();Reply(h.kind,Result::Ready);return;
            default:throw std::runtime_error("unexpected IPC kind");
            }
        }
    }
};
}
int wmain(int argc,wchar_t** argv){
    // No DllMain call and no ReShade API call in the host startup path.
    if(argc<3||argc>4)return 2;
    x86host::Host host;host.transport=argc==4&&wcscmp(argv[3],L"--transport-only")==0;
    if(argc==4&&!host.transport)return 2;
    const DWORD pid=wcstoul(argv[2],nullptr,10);
    host.parent.reset(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));if(!host.parent)return 3;
    host.pipe.reset(CreateFileW(argv[1],GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));if(!host.pipe)return 4;
    ULONG server=0;if(!GetNamedPipeServerProcessId(host.pipe.value,&server)||server!=pid)return 5;
    g_log=_wfopen((ExeDirectory()/L"amd-nr-x86-host.log").c_str(),L"w");
    try{host.Run();if(g_log){fclose(g_log);g_log=nullptr;}return 0;}
    catch(const std::exception& e){
        Log("x86bridge HOST_ERROR: %s; original frame only",e.what());
        // Fatal partial GPU work belongs solely to this helper. Do not free its live
        // resources under the queue, or attempt a new frame with uncertain ownership.
        TerminateProcess(GetCurrentProcess(),6);return 6;
    }
}
