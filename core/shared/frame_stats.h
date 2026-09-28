// The 'stats:' line: what the presents of one window did, as the difference of two readings of the
// running totals the add-on keeps (core/addon/stats.inc). Pure, so tools/stats_check.py runs it.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// presents: through JobGate, which every route passes once a present. evaluated: the engine took at
// least one pass. skipped: the network sat the present out because the GPU still had the last one,
// JobGate's pending job or, as heapHeld, a list still reading the descriptor heap. refused: the
// engine turned a pass down. bindWaits: a game's bind or clear found the present holding g.lock.
struct FrameCounts {
    uint64_t presents = 0, evaluated = 0, skipped = 0, heapHeld = 0, refused = 0, bindWaits = 0;
};

inline FrameCounts operator-(const FrameCounts& a, const FrameCounts& b) {
    return {a.presents - b.presents, a.evaluated - b.evaluated, a.skipped - b.skipped,
            a.heapHeld - b.heapHeld, a.refused - b.refused, a.bindWaits - b.bindWaits};
}

// jobMs: the longest evaluation that retired in the window, 0 when none did.
inline std::string FormatStats(const FrameCounts& d, double seconds, uint64_t jobMs) {
    char line[256];
    const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    const int n =
        std::snprintf(line, sizeof(line),
                      "stats: %.2f s | presents %llu eval %llu skip %llu (heap %llu) refused %llu"
                      " | bind waits %llu | job max ",
                      seconds, u(d.presents), u(d.evaluated), u(d.skipped), u(d.heapHeld),
                      u(d.refused), u(d.bindWaits));
    if (jobMs == 0)
        std::snprintf(line + n, sizeof(line) - n, "none");
    else
        std::snprintf(line + n, sizeof(line) - n, "%llu ms", u(jobMs));
    return line;
}
