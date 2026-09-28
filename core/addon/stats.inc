// The per-second stats: what the presents of each second did, counted where it happens and read as
// one line (core/shared/frame_stats.h), logged with Diagnostics=2; and the temporal line, what the
// evaluations handed the engine, logged when it changes. Both are under the 64-bit panel's Debug
// whatever Diagnostics says. Part of the core's unity build, included by neural.cpp before
// runtimes.inc, whose JobGate ticks it once a present on every route.

#include "../shared/frame_stats.h"

struct Stats
{
    FrameCounts now, last;                  // the running totals, and the reading the last line took
    ULONGLONG at = GetTickCount64();        // when that reading was taken
    std::atomic<uint64_t> bindWaits { 0 };  // counted off the lock, on the game's own threads
    std::string line;                       // the last line made, for the panel
    double holdMaxMs = 0.0;                 // the longest hold since that reading
    // What this window's evaluations handed the engine (NoteTemporal). The passes handed history or
    // smoothed are ORed over the window: a pass whose history a skip dropped would flip it each second.
    unsigned passes = 0, handed = 0, smoothed = 0;
    uint8_t temporal = 0, sameFrame = 0, depth = 0;
    GuideSource motion = GuideSource::None;  // the last evaluation's, which the 64-bit panel shows too
    std::string temporalLine;
};
Stats &g_stats = *new Stats;  // never destroyed, as g is

// g.lock for a hook on one of the game's own binds or clears, counting each time the present held
// it. The guide tallies take it on every bind and clear the game makes, and this is the number to
// have before they get a lock of their own.
std::unique_lock<std::mutex> LockForHook()
{
    std::unique_lock hold(g.lock, std::try_to_lock);
    if (!hold.owns_lock())
    {
        ++g_stats.bindWaits;
        hold.lock();
    }
    return hold;
}

HMODULE RuntimeFor(UINT slot);  // runtimes.inc
int Outstanding(HMODULE m);
bool MzSelected();

// Why the record entry turned pass `r` down, as far as the runtime's own state says: it refuses when
// its init never finished and at four jobs in flight (runtime_offsets.h, kReady and kJobId). The
// text lasts until the next call.
const char *RefusalReason(HMODULE r)
{
    static char why[128];
    if (At<uint8_t>(r, rt::B->kReady) == 0)
        return "the engine is not ready";
    std::snprintf(why, sizeof(why), "%s: job %u, %u retired",
                  Outstanding(r) >= 4 ? "four jobs already in flight" : "no reason the runtime's state shows",
                  At<UINT>(r, rt::B->kJobId), At<UINT>(r, rt::B->kJobCounter));
    return why;
}

// After an evaluation: what the engine was handed, read back out of the danielblnc runtime rather
// than kept beside each write, so the line says what the engine holds. mochizuki keeps its own.
void NoteTemporal(GuideSource motion)
{
    g_stats.motion = motion;
    if (MzSelected())
        return;
    for (UINT i = 0; i < g.activePasses; ++i)
        g_stats.handed |= At<uint8_t>(RuntimeFor(i), rt::B->kHistoryOn) != 0 ? 1u << i : 0u;
    const HMODULE r = RuntimeFor(0);
    g_stats.passes = g.activePasses;
    g_stats.temporal = At<uint8_t>(r, rt::B->kTemporal);
    g_stats.sameFrame = At<uint8_t>(r, rt::B->kInlineActive);
    g_stats.depth |= At<uint8_t>(r, rt::B->kUseDepth);
}

// The window's temporal line: logged when it is not the last one logged, so a line means something
// changed, and at most one a second.
void TemporalLine()
{
    const GuideSource m = g_stats.motion;
    const std::string line = FormatTemporal({
        g_stats.temporal, g.settings.temporalMode.load(), g_stats.sameFrame == 1,
        g.settings.fixedSeed.load() != 0, g_stats.depth != 0, g_stats.passes, g_stats.handed,
        g_stats.smoothed, g.settings.outputSmooth.load(), g.settings.outputSmoothLimit.load(),
        m == GuideSource::Game ? "the game's" : m == GuideSource::Effect ? "Feed.fx's"
        : m == GuideSource::Flow ? "optical flow" : m == GuideSource::Estimated ? "estimated" : "none" });
    g_stats.handed = g_stats.smoothed = g_stats.depth = 0;
    if (line == g_stats.temporalLine)
        return;
    g_stats.temporalLine = line;
    Log("%s", line.c_str());
}

// One hold of WaitForPreviousJob: how long it held the present, whether it ran out at 500 ms with
// the last list still on the GPU, and whether the job counter was still behind after its spin.
void NoteHold(double ms, bool late, bool spunOut)
{
    static bool said = false;
    if (!said)
    {
        said = true;
        Log("hold: each present waits here for the last evaluation, as the same-frame engine needs "
            "on D3D12 and on OpenGL's fences; the stats line has its cost, and D3D12Wait=0 turns it off.");
    }
    ++g_stats.now.holds;
    g_stats.now.holdMs += ms;
    g_stats.holdMaxMs = std::max(g_stats.holdMaxMs, ms);
    g_stats.now.deadlines += late ? 1 : 0;
    g_stats.now.spinOuts += spunOut ? 1 : 0;
}

// Once a present, from JobGate. A line covers the time since the last one, which is longer than a
// second across a pause: nothing ticks while the effect is off, and the line says how long it was.
// It ends on each module's work: its last job, and how many it has retired.
void TickStats()
{
    ++g_stats.now.presents;
    const ULONGLONG now = GetTickCount64();
    if (now - g_stats.at < 1000)
        return;
    g_stats.now.skipped = g.status.skipped;
    g_stats.now.bindWaits = g_stats.bindWaits.load();
    // A module zeroes its count when it rebuilds its staging, so a count that went down is all new.
    static UINT fires[State::kMaxPasses] {};
    char work[192] = "";  // three modules at their longest take 135
    for (UINT m = 0, n = 0; m < State::kMaxPasses; ++m)
        if (const HMODULE r = g.runtimes[m]; r != nullptr)
        {
            const UINT seen = At<volatile UINT>(r, rt::B->kWatchdogFires);
            g_stats.now.timeouts += seen >= fires[m] ? seen - fires[m] : seen;
            fires[m] = seen;
            n += std::snprintf(work + n, sizeof(work) - n, " | module %u job %u retired %u", m + 1,
                               At<UINT>(r, rt::B->kJobId), At<UINT>(r, rt::B->kJobCounter));
        }
    g_stats.line = FormatStats(g_stats.now - g_stats.last, static_cast<double>(now - g_stats.at) / 1000.0,
                               g.worstJobMs.exchange(0), std::exchange(g_stats.holdMaxMs, 0.0)) + work;
    if (g_stats.now.evaluated != g_stats.last.evaluated && g_stats.passes != 0)
        TemporalLine();
    g_stats.last = g_stats.now;
    g_stats.at = now;
    if (g.settings.diagnostics & 2)
        Log("%s", g_stats.line.c_str());
}
