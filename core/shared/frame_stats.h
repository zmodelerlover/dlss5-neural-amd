// The 'stats:' line: what the presents of one window did, as the difference of two readings of the
// running totals the add-on keeps (core/addon/stats.inc). Pure, so tools/stats_check.py runs it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// presents: through JobGate, which every route passes once a present. evaluated: the engine took at
// least one pass. skipped: the network sat the present out because the GPU still had the last one,
// JobGate's pending job or, as heapHeld, a list still reading the descriptor heap. refused: the
// engine turned a pass down. bindWaits: a game's bind or clear found the present holding g.lock.
// holds: WaitForPreviousJob held the present for the last evaluation, holdMs in all; deadlines of
// them ran out at 500 ms with its list still on the GPU, spinOuts left its job counter behind.
// timeouts: the runtime's watchdog let the game's queue go, every module's count added up.
struct FrameCounts {
    uint64_t presents = 0, evaluated = 0, skipped = 0, heapHeld = 0, refused = 0, bindWaits = 0;
    uint64_t holds = 0, deadlines = 0, spinOuts = 0, timeouts = 0;
    double holdMs = 0.0;
};

inline FrameCounts operator-(const FrameCounts& a, const FrameCounts& b) {
    return {a.presents - b.presents, a.evaluated - b.evaluated, a.skipped - b.skipped,
            a.heapHeld - b.heapHeld,   a.refused - b.refused,     a.bindWaits - b.bindWaits,
            a.holds - b.holds,         a.deadlines - b.deadlines, a.spinOuts - b.spinOuts,
            a.timeouts - b.timeouts,   a.holdMs - b.holdMs};
}

// jobMs: the longest evaluation that retired in the window, 0 when none did; holdMaxMs, the longest
// hold. The hold is only there when a route held: D3D12, OpenGL on its fences, unless D3D12Wait=0.
inline std::string FormatStats(const FrameCounts& d, double seconds, uint64_t jobMs,
                               double holdMaxMs) {
    char line[256];
    const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    int n =
        std::snprintf(line, sizeof(line),
                      "stats: %.2f s | presents %llu eval %llu skip %llu (heap %llu) refused %llu"
                      " | bind waits %llu | job max ",
                      seconds, u(d.presents), u(d.evaluated), u(d.skipped), u(d.heapHeld),
                      u(d.refused), u(d.bindWaits));
    if (jobMs == 0)
        n += std::snprintf(line + n, sizeof(line) - n, "none");
    else
        n += std::snprintf(line + n, sizeof(line) - n, "%llu ms", u(jobMs));
    n += std::snprintf(line + n, sizeof(line) - n, " | timeouts %llu", u(d.timeouts));
    if (d.holds != 0)
        std::snprintf(line + n, sizeof(line) - n, " | hold %.1f/%.1f ms, deadline %llu, spin %llu",
                      d.holdMs / static_cast<double>(d.holds), holdMaxMs, u(d.deadlines),
                      u(d.spinOuts));
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

// The flicker between two evaluations, as tools/temporal_metrics.py measures recorded play: how far
// the output moved where the input held still, the largest of R, G and B. Held still is a move of
// 1/255 or less at the sample itself, where the ruler asks it of a 5x5 window. The samples are RGB
// triples in the network's own encoding, the earlier evaluation's first.
struct Flicker {
    double stillShare = 0.0; // of the samples, the ones whose input held still
    double mean = 0.0, p99 = 0.0, over1 = 0.0, over4 = 0.0; // over those: over 1/255, over 4/255
};

inline Flicker FlickerStats(const std::vector<float>& in0, const std::vector<float>& out0,
                            const std::vector<float>& in1, const std::vector<float>& out1) {
    const size_t n = std::min({in0.size(), out0.size(), in1.size(), out1.size()}) / 3;
    std::vector<float> moved; // the output's move at each still sample
    for (size_t i = 0; i < n; ++i) {
        float in = 0.0f, out = 0.0f;
        for (size_t c = i * 3; c < i * 3 + 3; ++c) {
            in = std::max(in, std::abs(in1[c] - in0[c]));
            out = std::max(out, std::abs(out1[c] - out0[c]));
        }
        if (in <= 1.0f / 255.0f)
            moved.push_back(out);
    }
    Flicker f;
    if (moved.empty())
        return f;
    f.stillShare = static_cast<double>(moved.size()) / static_cast<double>(n);
    for (const float v : moved) {
        f.mean += v;
        f.over1 += v > 1.0f / 255.0f ? 1.0 : 0.0;
        f.over4 += v > 4.0f / 255.0f ? 1.0 : 0.0;
    }
    const double count = static_cast<double>(moved.size());
    f.mean /= count;
    f.over1 /= count;
    f.over4 /= count;
    const auto at = moved.begin() + static_cast<std::ptrdiff_t>((moved.size() - 1) * 99 / 100);
    std::nth_element(moved.begin(), at, moved.end());
    f.p99 = *at;
    return f;
}
