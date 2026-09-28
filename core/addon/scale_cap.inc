// The scale cap: what the network runs at, held a step below the person's Scale once this card has
// shown it cannot carry it, and the stand-down at the lowest. Two notes step it, NoteWatchdog and
// NoteJobCost, both taken by JobGate (runtimes.inc) as it retires an evaluation. Part of the core's
// unity build, included by neural.cpp before runtimes.inc.

// What the network is actually run at: the person's Scale, held down when this card has already
// shown it cannot carry it.
//
// The engine's inline mode makes the game's own queue wait for the network, and the watchdog this
// add-on writes into dlssnr_on_amd.ini (InlineWaitMs=100) only stops the CPU from waiting -- it
// cannot cancel a dispatch already on the GPU. A Conan Exiles log from an RX 9070 has jobs of
// 277 ms and then 2711 ms at 1920x1080, which is Windows TDR territory: the driver resets, the
// device is removed, and the game goes with it. The person reporting it had the game and then the
// whole PC go down, and found by hand that 0.50 was the setting that survived.
//
// So the cost is measured and the scale is held one step below whatever produced it. Their own
// setting is never overwritten -- the cap is separate, and the overlay says it is in force.
float EffectiveScale()
{
    return ui::EffectiveScale(g.settings.scale.load(), g.scaleCap.load());
}

// One step down the scale for a network this card cannot carry at this one, logged after `why`.
// The person's Scale is untouched: the cap is separate, and the overlay says it is in force. At
// the lowest there is no step left, so the add-on stands down for the session with `reason`, the
// same on every route, rather than risk the driver. NoteJobCost counts again from here, and
// NoteWatchdog's streak starts again on the new scale.
void StepScaleDown(const char *why, const char *reason)
{
    const float now = EffectiveScale();
    const float next = std::max(0.25f, now - 0.25f);
    g.longJobs = 0;
    if (next >= now)
    {
        g.status.unavailable = true;
        g.status.reason = reason;
        Log("%s at scale %.2f, the lowest. Standing down: %s", why, static_cast<double>(now),
            reason);
        return;
    }
    g.scaleCap.store(next);
    Log("%s at scale %.2f, so the scale is held at %.2f. Your own Scale setting is untouched; "
        "raise the cap by setting Scale again in the overlay.", why, static_cast<double>(now),
        static_cast<double>(next));
}

// The runtime's watchdog lets the game's queue go when a job outlasts InlineWaitMs, and counts each
// time it does (kWatchdogFires, one count per module). One is a hitch. Every job tripping it holds
// the game's queue for the whole budget on every evaluation: a network too slow for this scale
// (between InlineWaitMs and 250 ms NoteJobCost never counts it), or a GPU that faults every time.
// So WatchdogStandDown retired evaluations in a row that each tripped it (8; 0 never) take the
// scale down a step, and at the lowest stand the add-on down. A streak is one scale on one raster:
// a step, the person's Scale or a resize starts it again. A count that went down is a module that
// recreated its staging and zeroed it: only what it counted since then is a fire. Returns whether
// this evaluation tripped it.
bool NoteWatchdog()
{
    static UINT seen[State::kMaxPasses] {};
    static int streak = 0;
    static float atScale = 0.0f;
    static UINT atWidth = 0, atHeight = 0;
    bool fired = false;
    for (UINT m = 0; m < State::kMaxPasses; ++m)
    {
        const HMODULE r = g.runtimes[m];
        const UINT now = r != nullptr ? At<volatile UINT>(r, rt::B->kWatchdogFires) : 0;
        fired |= now != seen[m] && now != 0;
        seen[m] = now;
    }
    const float scale = EffectiveScale();
    if (scale != atScale || g.netWidth != atWidth || g.netHeight != atHeight)
        streak = 0;
    atScale = scale;
    atWidth = g.netWidth;
    atHeight = g.netHeight;
    streak = fired ? streak + 1 : 0;
    const int limit = g.settings.watchdogStandDown.load();
    if (limit == 0 || streak < limit)
        return fired;
    char why[128];
    std::snprintf(why, sizeof(why), "the network outlasted InlineWaitMs on %d evaluations in a row "
                  "(WatchdogStandDown=%d; 0 turns this off)", streak, limit);
    StepScaleDown(why, "the network kept outlasting InlineWaitMs at the lowest scale; restart the "
                       "game to re-enable");
    return fired;
}

// One finished evaluation, timed from this side rather than read out of the engine's log. Three
// dangerous jobs and the scale comes down a step: one is a shader compile or an alt-tab, three is
// this card at this resolution. `fired`: the runtime's watchdog stopped this one (NoteWatchdog).
void NoteJobCost(UINT64 ms, bool fired)
{
    constexpr UINT64 kDanger = 250;  // an order of magnitude past a frame, far short of TDR
    // A reading this large is not a job. The GPU cannot hold one for half a minute -- Windows
    // resets the driver long before -- so it is a pause that slipped past the latch: a route that
    // skipped frames without a reset (an MSAA or missing back buffer). Counting it would cap the
    // scale for something that never ran.
    if (ms > 30000)
        return;
    if (ms > g.worstJobMs.load())
        g.worstJobMs.store(ms);
    if (ms < kDanger)
    {
        if (ms < kDanger / 2)
            g.longJobs = 0;  // comfortably back inside budget: the streak was a hitch
        return;
    }
    // At the lowest scale the next step is off for the session, and a present gap (a loading
    // screen) reads as long too: there only a job the watchdog also stopped counts, as any real
    // one past 250 ms does under the engine's 200 ms default.
    if ((!fired && EffectiveScale() <= 0.25f) || ++g.longJobs < 3)
        return;
    char why[64];  // a dispatch that long resets the display driver and takes the game with it
    std::snprintf(why, sizeof(why), "the network took %llu ms three times",
                  static_cast<unsigned long long>(ms));
    StepScaleDown(why, "the network took over 250 ms three times at the lowest scale; restart the "
                       "game to re-enable");
}
