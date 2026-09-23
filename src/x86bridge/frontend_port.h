#pragma once
// The frontend's door for the panel adapter (panel32.cpp): the control traffic the two share, and
// the few things the adapter reads or switches on the frontend. Everything else the frontend owns
// stays private to frontend32.cpp.
#include <reshade.hpp>

#include "bridge_ipc.h"
#include "../neural/hotkey_capture.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace frontend32 {

// What the overlay asked for and what the helper last said. The overlay callback only reads this
// and raises flags; OnPresent, which owns the pipe, carries them across.
struct Controls32 {
    x86bridge::WireSettings shadow{};x86bridge::WireStatus status{};
    bool synced=false,syncRequested=false,save=false,reload=false,factory=false,measure=false,liftCap=false,preSyncEnableChanged=false;
    // The log export the panel's button asks for, and what to tell the person afterwards. The
    // copying itself happens on the present path with the other control traffic: the overlay
    // callback owns no transaction, here or anywhere else.
    bool exportLogs=false,exportFailed=false;std::wstring exportedPath;
    hotkey::Capture capture;
    // Stamped by the overlay callback, the only place ReShade hands a runtime over. The key
    // scan runs on the present path and needs it to read ReShade's own key state.
    reshade::api::effect_runtime* runtime=nullptr;
    uint64_t sentRevision=0,savedRevision=0,commandId=0,lastStatusAt=0,overlayAt=0;
};
extern Controls32 controls;

// The lock the present path holds for its whole frame.
std::mutex& FrameLock();
// The helper could not be reached; clearing it asks the present path to try again.
bool HostFailed();
void RetryHost();
// Whether presentation is pipelined (async) -- the frontend's own flag, not a shadow field.
bool Pipelined();
void SwitchPipelining(bool on);
// Push the operational fields of the shadow (enabled, hotkey, alt-tab) into the frontend.
void ApplyOperational();
// One line per guide the detector is holding, for the panel's Debug section.
std::vector<std::string> GuideCandidates();

// The overlay callback, registered by the frontend's DllMain.
void OnOverlay32(reshade::api::effect_runtime* runtime);

} // namespace frontend32
