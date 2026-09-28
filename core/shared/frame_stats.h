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

// What one window's evaluations handed the engine, for the 'temporal:' line. Bit i of handed and
// smoothed is pass i + 1, of the passes the last evaluation ran.
struct TemporalState {
    int temporal = 0, setting = 0; // the byte the engine read, and the Temporal= that asked for it
    bool sameFrame = false, seedPinned = false, depth = false;
    unsigned passes = 0, handed = 0, smoothed = 0;
    float smooth = 0.0f, smoothLimit = 0.0f;
    const char* motion = "none";
};

inline std::string FormatTemporal(const TemporalState& t) {
    const auto count = [&](unsigned bits) {
        int n = 0;
        for (bits &= (1u << t.passes) - 1; bits != 0; bits &= bits - 1)
            ++n;
        return n;
    };
    char smooth[64] = "not smoothed (OutputSmooth=0)";
    if (t.smooth > 0.0f)
        std::snprintf(smooth, sizeof(smooth), "smoothed %d/%u at %.2f under %.0f/255",
                      count(t.smoothed), t.passes, static_cast<double>(t.smooth),
                      static_cast<double>(t.smoothLimit));
    char line[320];
    std::snprintf(line, sizeof(line),
                  "temporal: byte %d (Temporal=%d), engine %s, history handed %d/%u, %s, seed %s, "
                  "motion %s, depth %s",
                  t.temporal, t.setting, t.sameFrame ? "same-frame" : "NOT same-frame",
                  count(t.handed), t.passes, smooth, t.seedPinned ? "pinned" : "free", t.motion,
                  t.depth ? "handed" : "not handed");
    return line;
}
