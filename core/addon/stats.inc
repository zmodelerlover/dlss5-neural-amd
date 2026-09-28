// The per-second stats: what the presents of each second did, counted where it happens and read as
// one line (core/shared/frame_stats.h). Logged with Diagnostics=2, and shown under the 64-bit panel's
// Debug whatever that is set to. Part of the core's unity build, included by neural.cpp before
// runtimes.inc, whose JobGate ticks it once a present on every route.

#include "../shared/frame_stats.h"

struct Stats
{
    FrameCounts now, last;                  // the running totals, and the reading the last line took
    ULONGLONG at = GetTickCount64();        // when that reading was taken
    std::atomic<uint64_t> bindWaits { 0 };  // counted off the lock, on the game's own threads
    std::string line;                       // the last line made, for the panel
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

// Once a present, from JobGate. A line covers the time since the last one, which is longer than a
// second across a pause: nothing ticks while the effect is off, and the line says how long it was.
void TickStats()
{
    ++g_stats.now.presents;
    const ULONGLONG now = GetTickCount64();
    if (now - g_stats.at < 1000)
        return;
    g_stats.now.skipped = g.status.skipped;
    g_stats.now.bindWaits = g_stats.bindWaits.load();
    g_stats.line = FormatStats(g_stats.now - g_stats.last, static_cast<double>(now - g_stats.at) / 1000.0,
                               g.worstJobMs.exchange(0));
    g_stats.last = g_stats.now;
    g_stats.at = now;
    if (g.settings.diagnostics & 2)
        Log("%s", g_stats.line.c_str());
}
