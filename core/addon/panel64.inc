// The 64-bit side of the panel in core/ui/: what it reports, filled from the engine under the
// lock, what it changed, written back, and what it asked for, done. The 32-bit bridge has its own
// (panel32.cpp). Part of the core's unity build, included by neural.cpp after OnPresent.

// The Export logs button: this run's log, the runtime's, ReShade's and the ini, all beside the
// game. See log_export.h, shared with the 32-bit bridge.
std::wstring ExportLogs()
{
    const auto r = logexport::ToDesktop({ ExeDirectory() }, { L"amd-nr.log", L"dlssnr_on_amd.log",
                                                              L"mochizuki_nr.log", L"ReShade.log", L"amd-nr.ini" });
    if (r.copied != 0)
        Log("menu: exported %d file(s) to %ls", r.copied, r.folder.c_str());
    return r.folder.wstring();
}

// What the panel shows and does not own, read under the lock the present path holds while it
// changes these. Copied out so drawing never happens with the lock held.
PanelStatus ReadPanelStatus()
{
    PanelStatus st;
    std::lock_guard guard(g.lock);
    st.run = g.status.unavailable ? RunState::Unavailable : g.status.failed ? RunState::Error : RunState::Ready;
    st.reason = g.status.reason;
    st.processed = g.status.frame;
    st.skipped = g.status.skipped;
    st.routeNote = ProfileForThisProcess().note;
    st.outWidth = g.outWidth;
    st.outHeight = g.outHeight;
    st.netWidth = g.netWidth;
    st.netHeight = g.netHeight;
    st.scaleCap = g.scaleCap.load();
    st.passesAvailable = PassesAvailable();
    st.depthSource = !g.gameDepthActive && !g.SnapshotFresh()    ? GuideSource::None
                     : !g.depthUsable.load()                     ? GuideSource::Unusable
                     : g.gameDepthActive && g.guideDepth.external ? GuideSource::Effect
                     : g.gameDepthActive ? GuideSource::Game       : GuideSource::Snapshot;
    st.motionSource = g.guideMotion.external && g.gameMotionActive ? GuideSource::Effect
                      : g.gameMotionActive                         ? GuideSource::Game
                                                                   : GuideSource::Estimated;
    st.gameMotionActive = g.gameMotionActive;
    st.stillPct = g.probeStillPct.load();
    st.depthMin = g.probeDepthMin.load();
    st.depthMax = g.probeDepthMax.load();
    st.stage = g.stage.load();
    st.events = g.events;
    st.noBridge = g.noBridge.load();
    st.noBackBuffer = g.noBackBuffer.load();
    st.hotkeyName = HotkeyName();
    RouteCaps(st);  // Feed.fx, Read from the game and depth, as far as this route reaches them
    st.feedStatus = g.feedStatus;
    MzStatus(st);
    return st;
}

// The panel's plain copy of the settings, filled from the engine's atomics.
PanelSettings ReadPanelSettings()
{
    PanelSettings s;
#define X(type, name, key, def, low, high) s.name = static_cast<type>(g.settings.name.load());
#include "../x86bridge/settings_fields.inc"
#undef X
    for (int i = 0; i < kMaxPasses; ++i)
    {
        s.passOverride[i] = g.settings.passOverride[i].load() ? 1u : 0u;
        s.passStructure[i] = g.settings.passStructure[i].load();
        s.passTone[i] = g.settings.passTone[i].load();
        s.passSkin[i] = g.settings.passSkin[i].load();
    }
    s.useFeedEffect = g.settings.useFeedEffect.load() ? 1u : 0u;
    return s;
}

// The settings as the engine constructs them, before amd-nr.ini is read: the table's defaults, what
// a fresh ini is written with, and so what Factory Defaults restores. Captured once, in DllMain.
PanelSettings g_factory;

// What the panel changed, written back to the engine. Only the fields that moved: the render thread
// writes some of these atomics too -- the scale cap's partner fields, the history flag -- and a
// whole-struct write would put back what the panel read before it did. Clamped to the table's
// ranges, as the 32-bit bridge's wire is: a value typed into a slider can be anything.
void ApplyPanelSettings(const PanelSettings &before, const PanelSettings &after)
{
#define X(type, name, key, def, low, high)                                                         \
    if (after.name != before.name)                                                                 \
    {                                                                                              \
        using T = decltype(g.settings.name.load());                                                \
        g.settings.name.store(SettingValue<T>(after.name, def, low, high));                        \
        Log("menu: " #name " %g -> %g", static_cast<double>(before.name),                          \
            static_cast<double>(g.settings.name.load()));                                          \
    }
#include "../x86bridge/settings_fields.inc"
#undef X
    for (int i = 0; i < kMaxPasses; ++i)
    {
        if (after.passOverride[i] != before.passOverride[i])
        {
            g.settings.passOverride[i].store(after.passOverride[i] != 0);
            Log("menu: pass %d profile %s", i + 1, after.passOverride[i] != 0 ? "on" : "off");
        }
        if (after.passStructure[i] != before.passStructure[i])
            g.settings.passStructure[i].store(after.passStructure[i]);
        if (after.passTone[i] != before.passTone[i])
            g.settings.passTone[i].store(after.passTone[i]);
        if (after.passSkin[i] != before.passSkin[i])
            g.settings.passSkin[i].store(after.passSkin[i]);
    }
    if (after.useFeedEffect != before.useFeedEffect)
    {
        g.settings.useFeedEffect.store(after.useFeedEffect != 0);
        g.feedSignature = -1;  // take whatever the effect hands over next as new, not as a repeat
        Log("menu: companion effect %s", after.useFeedEffect != 0 ? "on" : "off");
    }
    // Last frame's output no longer means what the next frame's motion vectors assume.
    if (SettingsInvalidateHistory(before, after))
        ResetTemporal("a panel setting changed");
}

void HandlePanelActions(const PanelActions &actions, const PanelSettings &after)
{
    MzChoose(actions);
    if (actions.Has(PanelAction::FactoryDefaults))
    {
        ApplyPanelSettings(after, KeepPreferences(g_factory, after));
        Log("menu: factory defaults restored; enabled, startup, hotkey, language and the panel's "
            "arrangement kept");
    }
    if (actions.Has(PanelAction::Save))
        SaveSettings();
    if (actions.Has(PanelAction::Reload))
    {
        LoadSettings();
        Log("menu: settings reloaded from the ini");
    }
    if (actions.Has(PanelAction::LiftScaleCap))
    {
        // Letting go of Scale overrules a cap NoteJobCost put on. If this card still cannot carry
        // it, three long jobs put the cap back.
        const float cap = g.scaleCap.exchange(0.0f);
        g.longJobs = 0;
        if (cap > 0.0f)
            Log("menu: resolution scale %.2f; the automatic cap is lifted and it runs at that until "
                "the network is measured too slow for it again.", static_cast<double>(after.scale));
    }
    if (actions.Has(PanelAction::MeasureResidual))
    {
        g.measured = false;
        g.measureTries = 0;
        g.settings.measureNow.store(true);
        Log("menu: residual measurement re-armed");
    }
}

// The overlay, rebuilt 22/09/2026. It used to carry 47 controls across eight headers; it carries
// fourteen across five now. Nothing was deleted: every atomic and every ini key is untouched, so
// each hidden control still reads and writes its key in amd-nr.ini. What went is the widget, and
// with it the chance of dragging a slider nobody here has measured into a state that looks broken.
//
// The panel itself is core/ui/, shared with the 32-bit bridge. This is the 64-bit side of it: fill
// the panel's copy from the engine, draw, write back what changed, carry out what was asked.
void OnOverlay(effect_runtime *runtime)
{
    // Rebinding by capturing a real keypress. Only advances while the overlay is open, which is
    // where the button is.
    static hotkey::Capture capture;
    static std::wstring exported;
    static bool exportFailed = false;

    PanelStatus status = ReadPanelStatus();
    status.hotkeyArmed = capture.armed;
    status.exportedPath = exported;
    status.exportFailed = exportFailed;
    PanelSettings panel = ReadPanelSettings();
    const PanelSettings before = panel;

    const PanelActions actions = DrawPanel(panel, status);

    ApplyPanelSettings(before, panel);
    HandlePanelActions(actions, panel);
    if (actions.Has(PanelAction::ToggleHotkeyCapture))
        capture.Toggle();
    // ReShade's key state, never GetAsyncKeyState: it hooks that one and answers 0 for every key
    // while the overlay is blocking the keyboard, which is the whole time this panel is open. See
    // hotkey_capture.h.
    int boundKey = 0, boundMods = 0;
    if (capture.Poll([runtime](int vk) { return runtime->is_key_down(static_cast<uint32_t>(vk)); },
                     boundKey, boundMods))
    {
        g.settings.toggleKey.store(boundKey);
        g.settings.toggleMods.store(boundMods);
        Log("menu: toggle bound to %s", HotkeyName().c_str());
    }
    if (actions.Has(PanelAction::ExportLogs))
    {
        SaveSettings(/*quiet=*/true);   // so the ini beside the logs is the run they describe
        exported = ExportLogs();
        exportFailed = exported.empty();
    }

    // Autosave. The Save button stays -- it is still the only thing that says out loud that a write
    // happened -- but nothing should be lost because somebody never scrolled this far. Written when
    // a control settles rather than while it is being dragged: WritePrivateProfileString rewrites
    // the whole file once per key, so a slider held down would be fifty full rewrites a second.
    static uint64_t saved = SettingsFingerprint();
    if (const uint64_t now = SettingsFingerprint(); now != saved && !ImGui::IsAnyItemActive())
    {
        SaveSettings(/*quiet=*/true);
        saved = now;
        static bool said = false;
        if (!said)
        {
            Log("settings autosaved to amd-nr.ini; every later change saves itself the same way.");
            said = true;
        }
    }
}
