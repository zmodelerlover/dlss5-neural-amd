#pragma once
// What the panel reads and writes, and nothing else. The panel is one implementation shared by the
// 64-bit add-on and the 32-bit bridge; each side fills these from what it has -- the engine's
// atomics on one, the shadow of the helper's state on the other -- and applies back what changed.
// Nothing in core/ui/ may know which side it is on except through the data here.

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

// Which optional controls have a widget. One bit each, saved as HiddenShown in the ini, and carried
// over the wire as-is, so the same bit means the same control whichever route wrote the key.
//
// This replaces a single "show everything" switch. That switch had the shape of the problem the
// rebuild was fixing: the answer to "this panel has too much on it" is not a second panel with all
// of it back. Here a person turns on the two controls they actually want and the rest stays off,
// which is a panel that grows by what somebody asked for rather than by what exists.
//
// Table-driven (kOpts in view_logic.h) so the cascade that toggles a bit and the gate that reads it
// cannot drift apart: a new control is one enumerator, one row, and one Shown() at the call site.
// The numbers are saved: amd-nr.ini keeps them as HiddenShown on both routes, so a new control takes
// the next free bit and none is ever renumbered or reused.
enum Opt : uint32_t
{
    kOptCompose      = 1u << 0,
    kOptGuard        = 1u << 1,
    kOptGuardPerPass = 1u << 2,
    kOptLocalTone    = 1u << 3,
    kOptTaper        = 1u << 4,
    kOptPerPass      = 1u << 5,
    kOptBicubic      = 1u << 6,
    kOptFeed         = 1u << 7,
    kOptGameGuides   = 1u << 8,
    kOptDepth        = 1u << 9,
    kOptDepthInv     = 1u << 10,
    kOptDepthStretch = 1u << 11,
    kOptHistory      = 1u << 12,
    kOptMotion       = 1u << 13,
    kOptMotionScale  = 1u << 14,
    kOptFlowGate     = 1u << 15,
    kOptFlowAccept   = 1u << 16,
    kOptMask         = 1u << 17,
    kOptTemporal     = 1u << 18,
    kOptTonemap      = 1u << 19,
    kOptToneChannels = 1u << 20,
    kOptOutputScale  = 1u << 21,
    kOptMeasure      = 1u << 22,
};
constexpr uint32_t kOptAll = (1u << 23) - 1u;

constexpr int kMaxPasses = 3;

// The settings, as plain values. The field list is the bridge's X-macro, so a setting added there
// is a setting the panel has, with the type it travels in: flags are uint32_t, not bool.
struct PanelSettings
{
#define X(type, name, low, high) type name = 0;
#include "../x86bridge/settings_fields.inc"
#undef X
    uint32_t passOverride[kMaxPasses] {};
    float passStructure[kMaxPasses] {}, passTone[kMaxPasses] {}, passSkin[kMaxPasses] {};
    // Not in the X-macro on purpose: the companion effect is read through ReShade's effect runtime,
    // which on the 32-bit route is in the game's process while the network is in the helper, so the
    // flag has nothing to reach there. Drawn only when PanelStatus::hasFeedEffect says so.
    uint32_t useFeedEffect = 0;

    bool operator==(const PanelSettings &) const = default;
};

// The one line beside Enabled, most serious first. The bridge has more ways of not running than
// the add-on -- the engine is a second process -- so some of these only ever come from one side.
enum class RunState
{
    Unavailable,     // the add-on gave up; PanelStatus::reason says why
    NoHelper,        // bridge: nothing on the other end of the pipe
    Error,
    TransportOnly,   // bridge: the helper copies frames across with no network at all
    EngineNotReady,
    Ready,
};

// Where a guide is coming from, for the status column.
enum class GuideSource
{
    None,
    Game,       // the game's own buffer
    Effect,     // AMD_Neural_Feed.fx
    Snapshot,   // depth copied before the game cleared it
    Estimated,  // motion only: this add-on's own block matcher
};

// What the panel only shows. Where the two routes really differ, the difference is a field here,
// never an #ifdef in core/ui/.
struct PanelStatus
{
    RunState run = RunState::Ready;
    std::string reason;
    uint64_t processed = 0, skipped = 0;

    // First line of the right column: the per-game profile on the add-on, what the bridge is doing
    // on the bridge.
    std::string routeNote;
    uint32_t outWidth = 0, outHeight = 0, netWidth = 0, netHeight = 0;
    // What the network is really held to when the card's own limit fired. 0 is no cap.
    float scaleCap = 0.0f;

    GuideSource depthSource = GuideSource::None, motionSource = GuideSource::Estimated;
    bool gameMotionActive = false;
    int stillPct = -1;  // -1 is no probe reading yet
    float depthMin = 0.0f, depthMax = 0.0f;

    // Restart-only diagnostics, echoed from the ini.
    int stage = 0, events = 0;
    bool noBridge = false, noBackBuffer = false;

    std::string hotkeyName;
    bool hotkeyArmed = false;

    std::wstring exportedPath;
    bool exportFailed = false;

    // Capabilities. Each is something one route has and the other does not.
    bool hasFeedEffect = false;   // the companion effect can reach the network
    std::string feedStatus;       // what the companion effect is handing over, when it is
    // The engine runs in a helper process: Timing switches the bridge's pipelining rather than the
    // engine's inline mode, the ini and the logs are the helper's, and anything that needs the
    // network is out of reach while the helper is only copying frames (transportOnly).
    bool helperProcess = false;
    bool transportOnly = false;
    // Lines the route adds under Debug, already worded: the bridge has two processes, two logs and
    // a guide detector whose pick is worth seeing, and none of that is the panel's to interpret.
    std::vector<std::string> routeDiagnostics;
};

// What the panel asks for and cannot do itself: the owner of each is on the adapter's side --
// the engine, the ini, the filesystem, the pipe.
enum class PanelAction : uint32_t
{
    Save                = 1u << 0,
    Reload              = 1u << 1,
    FactoryDefaults     = 1u << 2,
    ExportLogs          = 1u << 3,
    MeasureResidual     = 1u << 4,
    ToggleHotkeyCapture = 1u << 5,
    // Letting go of the Scale slider, changed or not: the person is overruling the cap the card's
    // own limit put on, and a cap only lifts when somebody asks.
    LiftScaleCap        = 1u << 6,
};

struct PanelActions
{
    uint32_t bits = 0;
    void Add(PanelAction a) { bits |= static_cast<uint32_t>(a); }
    bool Has(PanelAction a) const { return (bits & static_cast<uint32_t>(a)) != 0; }
};

} // namespace ui
