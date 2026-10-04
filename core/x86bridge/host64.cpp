// Additive x64 process boundary. The upstream engine is compiled verbatim in this TU.
#define AMDNR_WITH_VULKAN 0
#include "../addon/neural.cpp"
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
    uint64_t settingsRevision=1,lastCommand=0;
    StandDown standDown=StandDown::Other;  // what Neural() saw stand the helper down, for WireStatus
    void ForceInline(){g.settings.inlineMode.store(true);}  // the helper runs the engine same-frame whatever Inline says; the frontend has its own Async
    // The fence waits end before the frontend's IpcTimeoutMs, or the frontend kills this process
    // mid-wait with our work still on the GPU: FenceWaitCapMs is held to 4 s, 0 (for ever) included.
    // A wait that gives up stands the helper down (WaitFence) and the frame is answered Original.
    // Not bounded yet: the 5 s RuntimeBusy drains (EnsureResources, FinishSubmittedPass) and the
    // engine bring-up on the first Frame can outlast it; they live on the frontend letting one late
    // answer pass, and a second in a row still faults the bridge.
    void BoundWaits(){const int cap=g.settings.fenceWaitCapMs.load(),most=static_cast<int>(IpcTimeoutMs)-1000;
        g.settings.fenceWaitCapMs.store(cap==0||cap>most?most:cap);}
    WireSettings ExportSettings(){
        WireSettings s;s.settings_revision=settingsRevision;
#define X(type,name,key,def,low,high) s.name=static_cast<type>(g.settings.name.load());
#include "settings_fields.inc"
#undef X
        for(unsigned i=0;i<3;++i){s.passOverride[i]=g.settings.passOverride[i].load();s.passStructure[i]=g.settings.passStructure[i].load();s.passTone[i]=g.settings.passTone[i].load();s.passSkin[i]=g.settings.passSkin[i].load();}
        return s;
    }
    bool ApplySettings(WireSettings s){
        if(!NewRevision(s.settings_revision,settingsRevision))return false;
        if(!NormalizeSettings(s))return false;
        const bool historyChanged=SettingsInvalidateHistory(ExportSettings(),s);  // the 64-bit panel's list
#define X(type,name,key,def,low,high) g.settings.name.store(s.name);
#include "settings_fields.inc"
#undef X
        for(unsigned i=0;i<3;++i){g.settings.passOverride[i].store(s.passOverride[i]!=0);g.settings.passStructure[i].store(s.passStructure[i]);g.settings.passTone[i].store(s.passTone[i]);g.settings.passSkin[i].store(s.passSkin[i]);}
        if(historyChanged)ResetTemporal("a setting from the 32-bit panel changed");
        settingsRevision=s.settings_revision;return true;
    }
    WireStatus ExportStatus(){
        WireStatus s;s.connected=1;s.transportOnly=transport;
        s.processed=g.status.frame-std::min(g.status.frame,g.status.skipped);s.skipped=g.status.skipped;  // as the 64-bit panel reads them
        s.engineReady=!transport&&(g.runtime!=nullptr||mz.session!=nullptr)&&!g.status.unavailable&&!g.status.failed;
        s.unavailable=g.status.unavailable;s.failed=g.status.failed;
        s.outWidth=spec.colour.width;s.outHeight=spec.colour.height;s.netWidth=g.netWidth;s.netHeight=g.netHeight;
        s.loadedPasses=PassesAvailable();s.activePasses=g.activePasses;
        s.depthActive=!transport&&built&&g.gameDepthActive?(!g.depthUsable.load()?2u:g.depthVaried.load()?3u:1u):0u;s.motionActive=!transport&&built&&g.gameMotionActive;
        s.probeValid=!transport&&built&&g.probeStillPct.load()>=0;
        s.depthMin=g.probeDepthMin.load();s.depthMax=g.probeDepthMax.load();s.motionMean=g.probeMotionMean.load();s.motionMax=g.probeMotionMax.load();s.stillPct=g.probeStillPct.load();
        s.stage=g.stage.load();s.events=g.events;s.noBridge=g.noBridge.load();s.noBackBuffer=g.noBackBuffer.load();
        s.scaleCap=g.scaleCap.load();
        const char* why=g.status.reason;  // the core's stand-downs, by the string each sets
        s.reason=static_cast<uint32_t>(!g.status.unavailable?StandDown::None:g.loggedDeviceLost?StandDown::DeviceLost
            :why==kFenceGaveUp?StandDown::FenceWait:why==kSlowWatchdog||why==kSlowJobs?StandDown::TooSlow
            :why==kNotSameFrame?StandDown::NotSameFrame:standDown);
        ui::PanelStatus p;MzStatus(p);
        s.mochizuki=p.mochizuki;s.danielblnc=p.danielblnc!=nullptr?static_cast<uint32_t>(rt::B-rt::kBuilds)+1:0;s.networkMs=p.mochizuki!=0?p.mochizukiMs:p.danielblncMs;
        s.stallMs=g.standDownMs.load();
        return s;
    }
    void Snapshot(Kind kind,Result result=Result::Ready){
        StateSnapshot s{ExportSettings(),ExportStatus()};Reply(kind,result);
        Require(Send(pipe.value,parent.value,&s,sizeof(s)),"state snapshot write failed");
    }

    // The table's defaults (settings_fields.inc), as g is constructed with them, BEFORE LoadSettings.
    // A fresh ini is the 64-bit one (EnsureNeuralIni) and Factory Defaults the 64-bit Factory
    // Defaults: this route used to ship Colour Strength 0.25 in both.
    void CaptureFactoryDefaults(){factoryDefaults=ExportSettings();}
    void RestoreFactoryDefaults(){
        // No INI access. Restore the captured defaults, keeping the preferences (FactorySettings).
        // ApplySettings drops the history once if anything in SettingsInvalidateHistory moved.
        Require(ApplySettings(FactorySettings(factoryDefaults,ExportSettings())),"factory defaults rejected");
    }
    void Init(const Hello& h){
        Require(h.pid==GetProcessId(parent.value),"parent PID mismatch");
        CaptureFactoryDefaults();EnsureNeuralIni();LoadSettings();ForceInline();BoundWaits();Require(LoadGraphicsApi(),"graphics API load failed");
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
        Idle();ReleaseSwapchainSized();built=false;  // history stays, as on the 64-bit route (history.inc)
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
        generation=b.generation;spec=b;built=true;
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
        if(g.status.failed||g.status.unavailable||DeviceLost())return Result::Original;
        if(g.noBridge.load()||g.stage.load()<3)return Result::Original;
        UINT wanted=WantedPasses();if(!BringUpEngines(wanted)){g.status.unavailable=true;standDown=StandDown::EngineInit;return Result::Original;}
        g.loadedPasses=wanted;
        const UINT w=spec.colour.width,h=spec.colour.height;const auto fmt=static_cast<DXGI_FORMAT>(spec.colour.format);
        if(!EnsureResources(w,h,fmt,EffectiveScale())){g.status.unavailable=true;standDown=StandDown::Resources;return Result::Original;}
        if(!g.bridge.crossLocal && !CreateTexture(w,h,ColourReadFormat(fmt),g.bridge.crossLocal,"crossLocal",D3D12_RESOURCE_STATE_COPY_DEST))return Result::Original;
    const bool runNetwork = JobGate();

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
    if (!ok && g.status.failed)
        throw std::runtime_error("engine recording failed");
    Barrier(cmd, g.bridge.crossLocal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    if (ok && CompositionIsFresh(runNetwork) && !g.noBackBuffer.load())
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
        RetireUnsubmitted(g.bridge.workQueue.Get(), cmd);
        if (!RecreateWorkSlot(i))
            g.bridge.failed = true;
        return x86bridge::Result::Original;
    }
    ID3D12CommandList *lists[] { cmd };
    g.bridge.workQueue->ExecuteCommandLists(1, lists);
    if (g.activePasses != 0)  // every module that recorded: the last pass's copy runs nothing until told
        NotifyRuntimes(g.bridge.workQueue.Get(), 1, lists);
    g.ringValue[i] = ++g.ringSerial;
    Check(g.bridge.workQueue->Signal(g.ringFence.Get(), g.ringSerial), "ring signal");
    g.completion = ++g.serial;
    Check(g.bridge.workQueue->Signal(g.fence.Get(), g.completion), "completion signal");
    ArmStallWatch();

        // A wait that gave up (BoundWaits) stood the helper down: this frame is dropped, answered
        // Original, and its readbacks and textures stay where the GPU may still be using them.
        ++g.bridge.backValue;if(!WaitForWorkQueue(g.completion)&&!DeviceLost())return Result::Original;
        Require(!DeviceLost()&&g.fence->GetCompletedValue()!=UINT64_MAX&&g.fence->GetCompletedValue()>=g.completion,"output completion failed");
        const bool fresh=ok&&CompositionIsFresh(runNetwork)&&!g.noBackBuffer.load();
        DrainReadbacks(g.netWidth,g.netHeight);
        return fresh?Result::Neural:Result::Original;
    }
    Result FrameWork(const Frame& f){
        Require(built&&f.generation==generation&&f.id>lastFrame&&f.depthValid<=1&&f.motionValid<=1&&f.resetHistory<=1&&f.guideTaken<=7,"invalid frame/generation");
        Require((!f.depthValid||spec.depth.valid)&&(!f.motionValid||spec.motion.valid),"unbuilt guide requested");
        lastFrame=f.id;
        if(f.resetHistory){ResetTemporal("the 32-bit side paused, switched back on or answered late");g.jobRunning=false;}  // a pause is no network job
        // The frontend's SettleGuide took another buffer: as on the 64-bit route, the probe looks again and a new motion buffer is no longer demoted.
        if(f.guideTaken){if(f.guideTaken&2)g.guideMotion.failed=false;RearmGuideProbe((f.guideTaken&4)!=0);}
        g.guideDepth.ready=f.depthValid&&g.settings.useGameGuides.load()&&g.settings.useDepth.load();
        g.guideMotion.ready=f.motionValid&&g.settings.useGameGuides.load()&&g.settings.useMotion.load()&&!g.guideMotion.failed;
        const auto result=transport?CopyOnly():Neural();
        ++g.status.frame;
        if(g.status.frame<=3||g.status.frame%120==0)Log("x86bridge frame=%llu engine_frame=%llu result=%u",f.id,g.status.frame,static_cast<unsigned>(result));
        return result;
    }
    void Reply(Kind kind,Result result,uint64_t frame=0){
        Ack a;a.header.kind=kind;a.header.bytes=sizeof(a)-sizeof(a.header);a.result=result;a.generation=generation;a.frame=frame;
        if(g.bridge.workDevice){const auto luid=g.bridge.workDevice->GetAdapterLuid();a.luidLow=luid.LowPart;a.luidHigh=luid.HighPart;}
        Require(Send(pipe.value,parent.value,&a,sizeof(a)),"ACK write failed");
    }
    void Run(){
        Header h;const bool got=Receive(pipe.value,parent.value,&h,sizeof(h));
        if(got&&h.magic==Magic&&h.version!=Version)Log("x86bridge: the frontend speaks protocol v%u, this helper v%u: install amd-nr.addon32 and amd-nr-host64.exe together",h.version,Version);
        Require(got&&ValidHeader(h)&&h.kind==Kind::Hello,"HELLO header");
        Hello hello;Require(Receive(pipe.value,parent.value,&hello,sizeof(hello)),"HELLO body");Init(hello);Reply(Kind::Hello,Result::Ready);
        for(;;){
            // Waiting for the next request while the game is idle is intentionally unbounded.
            // Every operation after a header, and every frontend wait, remains bounded.
            Require(Receive(pipe.value,parent.value,&h,sizeof(h),INFINITE)&&ValidHeader(h),"IPC header/peer closed");
            switch(h.kind){
            case Kind::GetState:case Kind::Status:Snapshot(h.kind);break;
            case Kind::SetState:{WireSettings s;Require(Receive(pipe.value,parent.value,&s,sizeof(s)),"SET_STATE body");
                const float wanted=g.settings.scale.load();
                const bool applied=ApplySettings(s);
                // A new Scale arriving is the person overruling the cap NoteJobCost put on. Letting go
                // of the slider on an unchanged value sends no revision; that one arrives as the
                // LiftScaleCap command below, as it does on the 64-bit route.
                if(applied&&g.settings.scale.load()!=wanted){g.scaleCap.store(0.0f);g.longJobs=0;}
                Snapshot(h.kind,applied?Result::Ready:Result::Error);break;}
            case Kind::SaveSettings:SaveSettings();Snapshot(h.kind);break;
            case Kind::ReloadSettings:{LoadSettings();ForceInline();BoundWaits();++settingsRevision;Snapshot(h.kind);break;}  // LoadSettings drops the history
            case Kind::Command:{WireCommand c;Require(Receive(pipe.value,parent.value,&c,sizeof(c)),"COMMAND body");
                const bool accepted=NewCommand(c,lastCommand)&&(c.code==CommandCode::FactoryDefaults||!transport);
                if(accepted){lastCommand=c.id;if(c.code==CommandCode::FactoryDefaults)RestoreFactoryDefaults();
                    else if(c.code==CommandCode::LiftScaleCap){g.scaleCap.store(0.0f);g.longJobs=0;}
                    else {g.measured=false;g.measureTries=0;g.settings.measureNow.store(true);}}
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
