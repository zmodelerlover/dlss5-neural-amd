// The 32-bit side of the shared panel in core/ui/. The panel draws from a copy of the helper's
// settings and reports what the helper said; this fills both, and turns what the person did into
// requests the present path carries across. The overlay never talks on the pipe.
#include <imgui.h>

#include "frontend_port.h"
#include "panel_wire.h"
#include "../ui/i18n.h"
#include "../ui/panel.h"
#include "../shared/runtime_choice.h"

#include <cstdio>
#include <cstring>

namespace frontend32 {

namespace {

// The helper runs the network and reads NrBackend from the ini beside this add-on when it starts,
// which is before this panel can first be drawn; so what is read here first is what it took.
struct Runtime{int active=-1,chosen=0;bool installed[runtime_choice::kCount]{};} runtime;
std::filesystem::path Here(){
    HMODULE m{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(&Here),&m);
    wchar_t b[32768]{};GetModuleFileNameW(m,b,32768);return std::filesystem::path(b).parent_path();
}
void ReadRuntime(ui::PanelStatus& s){
    if(runtime.active<0){
        runtime.active=runtime.chosen=runtime_choice::Read(Here());
        for(int r=0;r<runtime_choice::kCount;++r)runtime.installed[r]=runtime_choice::Installed(Here(),r);
    }
    s.runtimeActive=runtime.active;s.runtimeChosen=runtime.chosen;
    for(int r=0;r<runtime_choice::kCount;++r)s.runtimeInstalled[r]=runtime.installed[r];
}

ui::PanelStatus BridgeStatus(){
    const auto& w=controls.status;
    ui::PanelStatus s;
    s.run=!w.connected?ui::RunState::NoHelper:w.failed?ui::RunState::Error:w.transportOnly?ui::RunState::TransportOnly
         :!w.engineReady?ui::RunState::EngineNotReady:ui::RunState::Ready;
    s.processed=w.processed;s.skipped=w.skipped;
    // Where the 64-bit panel prints the profile it picked for this game. This frontend has no
    // profile table and is not getting one -- what it can say instead is which of the two things it
    // does it is doing, which is the answer to "why is this faster than it was".
    s.routeNote=Pipelined()?ui::T("32-bit bridge, async","ponte 32 bits, assíncrona")
                           :ui::T("32-bit bridge, same frame","ponte 32 bits, mesmo quadro");
    s.outWidth=w.outWidth;s.outHeight=w.outHeight;s.netWidth=w.netWidth;s.netHeight=w.netHeight;
    s.scaleCap=w.scaleCap;
    s.depthSource=w.depthActive?ui::GuideSource::Game:ui::GuideSource::None;
    s.motionSource=w.motionActive?ui::GuideSource::Game:ui::GuideSource::Estimated;
    s.gameMotionActive=w.motionActive!=0;
    s.stillPct=w.probeValid?w.stillPct:-1;s.depthMin=w.depthMin;s.depthMax=w.depthMax;
    s.stage=w.stage;s.events=w.events;s.noBridge=w.noBridge!=0;s.noBackBuffer=w.noBackBuffer!=0;
    s.hotkeyName=hotkey::Name(controls.shadow.toggleKey,controls.shadow.toggleMods);
    s.hotkeyArmed=controls.capture.armed;
    s.exportedPath=controls.exportedPath;s.exportFailed=controls.exportFailed;
    s.helperProcess=true;s.transportOnly=w.transportOnly!=0;
    // What the old Status section carried that this route has and the 64-bit one does not: two
    // processes, two logs, and a guide detector whose pick is worth seeing.
    char line[96];
    std::snprintf(line,sizeof(line),"Protocol v%u | %s | %s",x86bridge::Version,Pipelined()?"async":"same frame",
                  w.transportOnly?"TRANSPORT_ONLY":"neural");
    s.routeDiagnostics.push_back(line);
    std::snprintf(line,sizeof(line),"passes loaded/active=%u/%u",w.loadedPasses,w.activePasses);
    s.routeDiagnostics.push_back(line);
    for(std::string& candidate:GuideCandidates())s.routeDiagnostics.push_back(std::move(candidate));
    s.routeDiagnostics.push_back("Logs: amd-nr-x86.log / amd-nr-x86-host.log");
    ReadRuntime(s);
    return s;
}

// Requests, not calls: the ini and the engine belong to the helper, and nothing in this callback
// is allowed to talk on the pipe. OnPresent posts them with the rest of the control traffic.
void PostRequests(const ui::PanelActions& actions){
    if(actions.Has(ui::PanelAction::Save))controls.save=true;
    if(actions.Has(ui::PanelAction::Reload))controls.reload=true;
    if(actions.Has(ui::PanelAction::FactoryDefaults))controls.factory=true;
    if(actions.Has(ui::PanelAction::MeasureResidual))controls.measure=true;
    if(actions.Has(ui::PanelAction::LiftScaleCap))controls.liftCap=true;
    // The save goes with the export so the ini beside the logs is the run they describe.
    if(actions.Has(ui::PanelAction::ExportLogs)){controls.save=true;controls.exportLogs=true;}
    // The scan itself runs on the present path: this callback is not called every frame, and a scan
    // that only advanced inside it never outlived the cancel timer. The button only arms it.
    if(actions.Has(ui::PanelAction::ToggleHotkeyCapture))controls.capture.Toggle();
    // Not a helper setting: it only matters when the helper next starts, and it reads the file then.
    if(actions.runtime>=0&&actions.runtime<runtime_choice::kCount){
        runtime.chosen=actions.runtime;runtime_choice::Write(Here(),runtime.chosen);
    }
}

} // namespace

void OnOverlay32(reshade::api::effect_runtime* runtime){
    // Never block on Present, IPC, or GPU. Present also serializes teardown against this state.
    std::unique_lock<std::mutex> guard(FrameLock(),std::try_to_lock);
    if(!guard.owns_lock()){ImGui::TextDisabled("x86 bridge is processing the current frame...");return;}
    controls.overlayAt=GetTickCount64();
    // The only place ReShade hands a runtime over, and the key scan on the present path needs
    // one to read its key state.
    controls.runtime=runtime;
    controls.syncRequested=true;
    if(!controls.synced){
        ImGui::TextDisabled(HostFailed()?"x64 host unavailable. See amd-nr-x86.log.":"Synchronizing with x64 host...");
        if(HostFailed()&&ImGui::Button("Retry host synchronization"))RetryHost();
        return;
    }
    ui::PanelSettings panel=x86bridge::ToPanel(controls.shadow);
    // Timing is this process's own flag, not a shadow field: inside the helper the network always
    // runs same-frame, and what the control switches here is whether the frontend waits for the
    // answer or composes the previous one. Reading the real flag means the panel cannot drift from
    // what the bridge is doing.
    panel.inlineMode=Pipelined()?0u:1u;
    const ui::PanelSettings before=panel;
    ui::SetLanguage(panel.language);
    const ui::PanelActions actions=ui::DrawPanel(panel,BridgeStatus());
    if(panel.inlineMode!=before.inlineMode)SwitchPipelining(panel.inlineMode==0);
    panel.inlineMode=controls.shadow.inlineMode;
    auto edited=x86bridge::FromPanel(controls.shadow,panel);
    if(std::memcmp(&edited,&controls.shadow,sizeof(edited))!=0){
        edited.settings_revision=controls.shadow.settings_revision+1;controls.shadow=edited;ApplyOperational();
    }
    PostRequests(actions);
    // Autosave. Armed when a control settles rather than while it is being dragged: the host rewrites
    // the whole ini once per key, so a slider held down would be a file rewrite per frame. The Save
    // button stays -- it is what puts the line in the host's log -- but nothing is lost without it.
    if(controls.shadow.settings_revision!=controls.savedRevision&&!ImGui::IsAnyItemActive())
        controls.save=true;
}

} // namespace frontend32
