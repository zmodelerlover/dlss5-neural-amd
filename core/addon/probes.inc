// The probes' read side: the guide probe, the flow probe and the residual measurement, mapped
// and logged once their copies have landed. Part of the core's unity build, included by
// neural.cpp.

// The flow probe and the residual measurement copy into readback buffers during recording; this
// is where the results are actually read back and logged. It lived inline in OnPresent, which
// meant the bridge path -- which never reaches that tail -- recorded the copies and then threw
// the numbers away, so a D3D11 run produced no measurement at all and there was nothing to check
// the transport against. Both paths call it now.
float HalfToFloat(uint16_t h)
{
    const int e = (h >> 10) & 0x1f;
    const int m = h & 0x3ff;
    const float v = e == 0 ? m / 1024.0f * 6.103515625e-5f : std::ldexpf(1.0f + m / 1024.0f, e - 15);
    return (h & 0x8000) ? -v : v;
}

// Whether the network is handed depth (kUseDepth, RecordNetwork: every route), from what the probe
// read of it. netDepth is written either way, so a later reading can hand it back. A change is a
// change of the network's inputs, as the Depth switch is, so the history goes with it. A reading
// that varies resets the FLAT count even when nothing changes.
void SetDepthUsable(bool usable, const char *why)
{
    if (usable)
        g.flatProbes = 0;
    if (g.depthUsable.exchange(usable) == usable)
        return;
    Log("guide depth: %s, so the network is %s handed depth.", why, usable ? "again" : "no longer");
    ResetTemporal(usable ? "the network is handed depth again" : "the guide probe withheld depth");
}

// A guide went to another buffer -- the game's pick (SettleGuide, the D3D12 depth settle, the 32-bit
// bridge's FRAME) or the companion effect's feed: the last reading was of a buffer no longer read,
// so look again once it has settled. The junk count starts over too, or five readings latched on
// the old buffer would end the new search after one. ponytail: takes that keep coming (the D3D12
// depth pick has no streak) keep the probe going, at most one per 120 presents, past the five-junk
// cap; a count of re-arms since the last real reading would bound it.
//
// `replaced`: the take put one buffer where another was in use, and history built on the old one
// goes too. Filling an empty slot does not -- the first take, or the same buffer found again three
// presents after a swapchain rebuild let it go, which kept the history on purpose (history.inc) --
// nor does a take of a guide the network is not fed (its Depth or Motion switch off). ponytail: the
// D3D12 depth challenger wins on one present with a 25% bind margin, no streak, so a game that
// ping-pongs two depth buffers whose binds swing that much present to present re-takes, and now
// drops history, every few dozen presents (a model at +-20% gave one in 23); SettleGuide's
// three-present challenger streak in SettleD3D12Depth is the fix once a log shows it.
void RearmGuideProbe(bool replaced)
{
    if (replaced)
        ResetTemporal("a guide moved to another buffer");
    g.probeGuides.store(true);
    g.nextGuideProbe = g.status.frame + 120;
    g.junkProbes = 0;
    // A buffer not read yet is fed. ponytail: so is withheld depth on a re-arm that took no new
    // depth buffer (motion alone, the effect's feed, the same buffer after a rebuild): the network
    // gets it back, and the history goes twice (history.inc), until two readings withhold it again;
    // passing whether the take was of new depth down to here is the fix.
    SetDepthUsable(true, "the guide probe looks again");
}

void DemoteMotion(double meanPx);  // core/temporal/motion_feed.inc
float FeedCap();
void NoteFlicker(const void *in, const void *out, UINT rowPitch, UINT nw, UINT nh, bool first);  // stats.inc
void DrainReadbacks(UINT nw, UINT nh)
{
    if (g.pendingGuides)
    {
        g.pendingGuides = false;
        ComPtr<ID3D12Fence> f;
        if (SUCCEEDED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))) &&
            SUCCEEDED(g.queue->Signal(f.Get(), 1)))
        {
            for (int spin = 0; spin < 2000 && f->GetCompletedValue() < 1; ++spin)
                Sleep(1);
            const UINT pitch = (nw * 4 + 255) & ~255u;
            void *a = nullptr, *b = nullptr;
            if (f->GetCompletedValue() >= 1 && SUCCEEDED(g.guideReadDepth->Map(0, nullptr, &a)) &&
                SUCCEEDED(g.guideReadMotion->Map(0, nullptr, &b)))
            {
                // Counted over samples that are actually depth. A depth buffer read correctly
                // is in 0..1 everywhere; the numbers that arrive when the read is wrong are
                // 1e38, -3e38 and NaN, and "min is not max" called every one of those a real
                // depth buffer. NaN fails both comparisons below, so it lands outside the range
                // on its own and never reaches the mean.
                double lo = 1e30, hi = -1e30, sum = 0.0;
                UINT64 inRange = 0;
                const UINT64 samples = static_cast<UINT64>(nw) * nh;
                for (UINT y = 0; y < nh; ++y)
                {
                    auto *row = reinterpret_cast<const float *>(static_cast<const char *>(a) +
                                                                static_cast<size_t>(y) * pitch);
                    for (UINT x = 0; x < nw; ++x)
                    {
                        const float v = row[x];
                        if (!(v >= 0.0f && v <= 1.0f))
                            continue;
                        lo = std::min<double>(lo, v);
                        hi = std::max<double>(hi, v);
                        sum += v;
                        ++inRange;
                    }
                }
                const double inPct = samples == 0 ? 0.0 : 100.0 * static_cast<double>(inRange) /
                                                              static_cast<double>(samples);
                const bool depthJunk = inPct < 90.0;
                const bool depthReal = !depthJunk && inRange > 0 && (hi - lo) > 1e-6;
                if (inRange == 0)
                    lo = hi = 0.0;
                g.probeDepthMin.store(static_cast<float>(lo));
                g.probeDepthMax.store(static_cast<float>(hi));
                // Only from a reading that is actually depth: under JUNK, hi is the maximum of
                // whatever few samples happened to land in 0..1, which is noise.
                if (!depthJunk && hi > 1e-6)
                    g.depthDebugScale.store(static_cast<float>(1.0 / hi));
                // The guide the network reads, from the same measurement. Gated on depthReal so
                // a flat buffer cannot latch a scale, and on hi < 0.5 so a game whose depth
                // already fills the range is left exactly as it was. Clamped because a max that
                // measures near zero would otherwise turn rounding noise into the whole signal.
                // The copy is of netDepth, already multiplied by the scale in force, so the
                // buffer's own top is hi over that scale; taking hi itself, a re-probe after a
                // take would set 1/old of the right scale. ponytail: a reading the old scale
                // saturated (a small range's x500 on a buffer that fills 0..1) still keeps that
                // scale; telling it from a right scale's own top at 1.0 needs a raw copy.
                const double top =
                    hi / (g.settings.depthNormalise.load() ? g.depthScale.load() : 1.0f);
                if (depthReal && top < 0.5)
                {
                    const float s = std::clamp(static_cast<float>(1.0 / top), 1.0f, 4096.0f);
                    g.depthScale.store(s);
                    Log("guide depth: range tops out at %.6f, so the buffer fills %.2f%% of 0..1 "
                        "-- scaling the guide by %.1fx to give the network the range it was "
                        "trained on. DepthNormalise=0 turns this off.",
                        top, 100.0 * top, static_cast<double>(s));
                }
                Log("guide probe, depth %ux%u: min %.6f max %.6f mean %.6f, %.1f%% in 0..1%s", nw,
                    nh, lo, hi, inRange == 0 ? 0.0 : sum / static_cast<double>(inRange), inPct,
                    depthJunk ? "  <-- JUNK. Most of this is not in 0..1, so it is not being read "
                                "as depth: wrong resource, or a copy between two layouts that do "
                                "not match. The network is not handed it."
                    : !depthReal ? "  <-- FLAT. A constant, so either this is a menu with no scene "
                                   "or the guide picked the wrong resource."
                                 : "  <-- varies, so this is a real depth buffer.");

                // What the feed will zero (motion_feed.inc): all ones in the exponent is inf or NaN,
                // which HalfToFloat reads as 65536 and up, and the rest past the cap.
                const float cap = FeedCap();
                const auto zeroed = [&](uint16_t h) { return (h & 0x7c00) == 0x7c00 || std::abs(HalfToFloat(h)) > cap; };
                UINT64 zero = 0, n = 0, past = 0;
                double mag = 0.0, biggest = 0.0;
                for (UINT y = 0; y < nh; ++y)
                {
                    auto *row = reinterpret_cast<const uint16_t *>(static_cast<const char *>(b) +
                                                                   static_cast<size_t>(y) * pitch);
                    for (UINT x = 0; x < nw; ++x)
                    {
                        const double dx = HalfToFloat(row[x * 2]), dy = HalfToFloat(row[x * 2 + 1]);
                        if (dx == 0.0 && dy == 0.0)
                            ++zero;
                        past += zeroed(row[x * 2]) || zeroed(row[x * 2 + 1]) ? 1 : 0;
                        mag += std::abs(dx) + std::abs(dy);
                        biggest = std::max(biggest, std::abs(dx) + std::abs(dy));
                        ++n;
                    }
                }
                g.probeMotionMean.store(static_cast<float>(mag / (2.0 * n)));
                g.probeMotionMax.store(static_cast<float>(biggest));
                g.probeStillPct.store(static_cast<int>(100 * zero / n));
                // Naming the source is the difference between a number and a comparison: two
                // readings of "8% still" mean nothing unless it is known which of the three
                // things producing the field was running for each.
                Log("guide probe, motion %ux%u (%s): %llu%% exactly still, mean |d| %.3f px, "
                    "max %.3f px, %llu%% past the feed's %.0f px, so zeroed%s", nw, nh,
                    g.guideMotion.external  ? "from the effect"
                    : g.gameMotionActive    ? "the game's own"
                    : g.settings.opticalFlow.load() != 0 ? "optical flow"
                                            : "estimated",
                    static_cast<unsigned long long>(100 * zero / n), mag / (2.0 * n), biggest,
                    static_cast<unsigned long long>(100 * past / n), static_cast<double>(cap),
                    zero == n ? "  <-- ALL ZERO. Either nothing is moving, or the guide picked a "
                                "buffer the engine does not write velocity into."
                              : "");
                // The effect's field all zero in two readings of a moving scene (issue #18). A still menu reads
                // all zero honestly, so a reading counts only when the depth fed with it changed (ponytail: no
                // real depth, never counts). Retried 1200 presents on, doubling to 9600, or on new inputs.
                static double lastDepthSum = -1.0;
                const bool sceneMoved = g.probedDepth && depthReal && lastDepthSum >= 0.0 && sum != lastDepthSum;
                lastDepthSum = g.probedDepth && depthReal ? sum : -1.0;
                if (!g.guideMotion.external || zero < n)
                    g.feedZeroProbes = 0;
                else if (sceneMoved)
                    ++g.feedZeroProbes;
                if (g.feedZeroProbes == 2 && !g.feedMotionZero)
                {
                    const UINT64 wait = 1200ull << std::min(g.feedZeroLatches++, 3u);
                    g.feedMotionZero = true, g.feedRetryAt = g.status.frame + wait;
                    Log("guide probe: the effect's motion field is all zero in a moving scene; %s takes motion back, and the "
                        "effect is tried again in %llu presents", g.settings.opticalFlow.load() != 0 ? "FidelityFX optical flow" : "the estimator", static_cast<unsigned long long>(wait));
                }
                // Only a reading of depth that frame fed says anything: with none, netDepth holds
                // whatever it last did. JUNK is never depth. FLAT is a menu as often as a wrong
                // buffer, so it counts only in a scene -- something moved -- and only the second
                // time. A menu that moves (a spinner, a video) reads just the same, so depth
                // withheld as FLAT is looked at again past the five-reading cap, below.
                if (g.probedDepth && depthJunk)
                    SetDepthUsable(false, "JUNK");
                else if (g.probedDepth && !depthReal && zero < n && ++g.flatProbes >= 2)
                    SetDepthUsable(false, "FLAT in two readings of a moving scene");
                else if (g.probedDepth && depthReal)
                    SetDepthUsable(true, "it varies");
                g.depthVaried.store(g.probedDepth && depthReal);  // FallBackToPreClear's cue
                // Once a real scene has been seen there is nothing left to answer; until then
                // keep looking, because the first few hundred frames are menus and loading -- and
                // because junk must never end the search. It used to: the first probe fired on
                // depth that was 1e38 and motion that was uninitialised, both passed, and the
                // add-on stopped asking for the rest of the run.
                if (g.gameMotionActive && mag / (2.0 * n) > std::max(nw, nh))
                    DemoteMotion(mag / (2.0 * n));
                if (depthReal && zero < n && !g.guideMotion.failed)
                {
                    g.guidesLookReal.store(true);
                    g.probeGuides.store(false);
                    Log("guide probe: both guides carry real data. Not probing again until a "
                        "guide moves to another buffer.");
                }
                else if (++g.junkProbes >= 5 && g.flatProbes < 2)
                {
                    // Junk used to end the search, which was the bug. Never ending it is the
                    // other one: each probe allocates two readback buffers and spins on a fence
                    // on the present thread, and in a game whose depth reads as junk that is the
                    // one thing guaranteed to keep happening. Five is enough to outlast menus
                    // and loading screens; past that the answer is not going to change.
                    g.probeGuides.store(false);
                    Log("guide probe: five readings and the guides still do not carry usable "
                        "data. Not probing again until a guide moves to another buffer -- the "
                        "lines above say what was wrong with each one.");
                }
                else if (g.junkProbes >= 5)
                {
                    // Depth withheld as FLAT (flatProbes is zeroed whenever it is handed back).
                    // Stopping here would withhold it for the session when the FLAT was a menu
                    // that moved and the scene comes later in the same buffer, so the first
                    // reading that varies hands it back. JUNK keeps the cap: a menu never reads
                    // as JUNK. Each look stalls the present on a fence and walks both copies on
                    // the CPU, so the gap doubles to at most 4800 presents.
                    g.nextGuideProbe = g.status.frame + (600ull << std::min(g.junkProbes - 4, 3u));
                    if (g.junkProbes == 5)
                        Log("guide probe: five readings and depth is still withheld as FLAT. "
                            "Looking again, less often each time, until a reading varies.");
                }
                g.guideReadDepth->Unmap(0, nullptr);
                g.guideReadMotion->Unmap(0, nullptr);
            }
        }
        // A copy the GPU has not finished in 2 s is still writing into these: park them rather than
        // free them. ponytail: two per timed-out probe, freed at the next raster change.
        if (f == nullptr || f->GetCompletedValue() < 1)
            g.parked.insert(g.parked.end(), { g.guideReadDepth, g.guideReadMotion });
        g.guideReadDepth.Reset();
        g.guideReadMotion.Reset();
    }
    if (g.pendingFlow)
    {
        g.pendingFlow = false;
        ComPtr<ID3D12Fence> f;
        if (SUCCEEDED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))) &&
            SUCCEEDED(g.queue->Signal(f.Get(), 1)))
        {
            for (int spin = 0; spin < 2000 && f->GetCompletedValue() < 1; ++spin)
                Sleep(1);
            const UINT fw = g.flowWidth, fh = g.flowHeight;
            const UINT lumaPitch = (fw * 2 + 255) & ~255u;
            const UINT flowPitch = (fw * 4 + 255) & ~255u;
            void *a = nullptr, *b = nullptr;
            if (f->GetCompletedValue() >= 1 && SUCCEEDED(g.flowReadLuma->Map(0, nullptr, &a)) &&
                SUCCEEDED(g.flowReadFlow->Map(0, nullptr, &b)))
            {
                double lo = 1e30, hi = -1e30, sum = 0.0;
                for (UINT y = 0; y < fh; ++y)
                {
                    auto *row = reinterpret_cast<const uint16_t *>(static_cast<const char *>(a) +
                                                                   static_cast<size_t>(y) * lumaPitch);
                    for (UINT x = 0; x < fw; ++x)
                    {
                        const double v = HalfToFloat(row[x]);
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                        sum += v;
                    }
                }
                Log("flow probe, luminance %ux%u: min %.6f max %.6f mean %.6f%s", fw, fh, lo, hi,
                    sum / (fw * fh),
                    (hi - lo) < 1e-6 ? "  <-- flat, so there is nothing for a match to lock onto"
                                     : "  <-- has detail, so matching had something to work with");

                UINT64 zero = 0, atLimit = 0, n = 0;
                double mag = 0.0;
                for (UINT y = 0; y < fh; ++y)
                {
                    auto *row = reinterpret_cast<const uint16_t *>(static_cast<const char *>(b) +
                                                                   static_cast<size_t>(y) * flowPitch);
                    for (UINT x = 0; x < fw; ++x)
                    {
                        const double dx = HalfToFloat(row[x * 2]), dy = HalfToFloat(row[x * 2 + 1]);
                        if (dx == 0.0 && dy == 0.0)
                            ++zero;
                        if (std::abs(dx) >= 12.0 || std::abs(dy) >= 12.0)
                            ++atLimit;
                        mag += std::abs(dx) + std::abs(dy);
                        ++n;
                    }
                }
                Log("flow probe, field: %llu%% of blocks still, %llu%% pinned at the search limit, "
                    "mean |d| %.3f coarse pixels", static_cast<unsigned long long>(100 * zero / n),
                    static_cast<unsigned long long>(100 * atLimit / n), mag / (2.0 * n));
                Log("  a healthy field is mostly still with a minority moving; nearly all pinned at "
                    "the limit means the match is following noise, not the picture.");
                g.flowReadLuma->Unmap(0, nullptr);
                g.flowReadFlow->Unmap(0, nullptr);
            }
        }
        if (f == nullptr || f->GetCompletedValue() < 1)
            g.parked.insert(g.parked.end(), { g.flowReadLuma, g.flowReadFlow });
        g.flowReadLuma.Reset();
        g.flowReadFlow.Reset();
    }
    if (g.pendingMeasure)
    {
        g.pendingMeasure = false;
        ComPtr<ID3D12Fence> f;
        if (SUCCEEDED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))) &&
            SUCCEEDED(g.queue->Signal(f.Get(), 1)))
        {
            for (int spin = 0; spin < 2000 && f->GetCompletedValue() < 1; ++spin)
                Sleep(1);
            void *a = nullptr, *b = nullptr;
            D3D12_RANGE all { 0, static_cast<SIZE_T>((nw * 8 + 255) & ~255u) * nh };
            if (f->GetCompletedValue() >= 1 &&
                SUCCEEDED(g.readbackBase->Map(0, &all, &a)) &&
                SUCCEEDED(g.readbackNr->Map(0, &all, &b)))
            {
                const UINT rowPitch = (nw * 8 + 255) & ~255u;
                if (g.settings.capturePair)
                {
                    g.settings.capturePair = false;
                    std::error_code ec;
                    const auto dir = ExeDirectory() / L"amd-nr-captures";
                    std::filesystem::create_directories(dir, ec);
                    const std::string prefix = "frame-" + std::to_string(g.status.frame) + "-" +
                                               std::to_string(GetTickCount64());
                    auto save = [&](const char *kind, const void *data) {
                        std::ofstream file(dir / (prefix + kind + ".raw"), std::ios::binary);
                        for (UINT y = 0; y < nh && file; ++y)
                            file.write(static_cast<const char *>(data) +
                                static_cast<size_t>(y) * rowPitch, static_cast<size_t>(nw) * 8);
                        return file.good();
                    };
                    if (!ec && save("-input", a) && save("-runtime", b))
                    {
                        std::ofstream meta(dir / (prefix + ".txt"));
                        meta << "width=" << nw << "\nheight=" << nh << "\nformat=RGBA16F_LE\n"
                             << "encoding=" << g.settings.encoding.load() << "\npasses=" << g.settings.passes.load()
                             << "\ntonemap=" << RuntimeTonemap() << "\ninline=" << g.settings.inlineMode.load()
                             << "\njob=" << g.lastJob << "\n";
                        Log("capture pair saved: %ls / %s (input and runtime; not the raw neural tensor)",
                            dir.c_str(), prefix.c_str());
                    }
                    else Log("capture pair could not be written to %ls", dir.c_str());
                }
                double sum = 0.0, peak = 0.0;
                UINT64 n = 0, nonFinite = 0;
                for (UINT y = 0; y < nh; y += 4)
                {
                    auto *pa = reinterpret_cast<const uint16_t *>(static_cast<const char *>(a) +
                                                                  static_cast<size_t>(y) * rowPitch);
                    auto *pb = reinterpret_cast<const uint16_t *>(static_cast<const char *>(b) +
                                                                  static_cast<size_t>(y) * rowPitch);
                    for (UINT x = 0; x < nw * 4; x += 4)
                    {
                        // An fp16 exponent of all ones is inf or NaN, which HalfToFloat reads as 65536+.
                        nonFinite += (pb[x] & 0x7c00) == 0x7c00 || (pb[x + 1] & 0x7c00) == 0x7c00 ||
                                     (pb[x + 2] & 0x7c00) == 0x7c00;
                        const double d = std::abs(HalfToFloat(pa[x]) - HalfToFloat(pb[x]));
                        sum += d;
                        if (d > peak)
                            peak = d;
                        ++n;
                    }
                }
                // A mean residual says the network changed something; it does not say *what*.
                // A flat exposure or saturation shift and a genuine texture edit produce the same
                // mean. They differ in how the correction varies pixel to pixel: a level shift is
                // smooth, so its horizontal gradient is near zero, while structure follows the
                // image and its gradient is a real fraction of the image's own. Measure both and
                // report the ratio -- that is the difference between "it recoloured the frame"
                // and "it worked on the texture", and it is not a judgement call.
                double meanBase = 0.0, gradRes = 0.0, gradIn = 0.0;
                UINT64 nb = 0, ng = 0;
                for (UINT y = 0; y < nh; y += 4)
                {
                    auto *pa = reinterpret_cast<const uint16_t *>(static_cast<const char *>(a) +
                                                                  static_cast<size_t>(y) * rowPitch);
                    auto *pb = reinterpret_cast<const uint16_t *>(static_cast<const char *>(b) +
                                                                  static_cast<size_t>(y) * rowPitch);
                    double prevIn = 0.0, prevRes = 0.0;
                    bool have = false;
                    for (UINT x = 0; x < nw * 4; x += 4)
                    {
                        const double in = HalfToFloat(pa[x]);
                        const double res = HalfToFloat(pb[x]) - in;
                        meanBase += std::abs(in);
                        ++nb;
                        if (have)
                        {
                            gradIn += std::abs(in - prevIn);
                            gradRes += std::abs(res - prevRes);
                            ++ng;
                        }
                        prevIn = in;
                        prevRes = res;
                        have = true;
                    }
                }
                const double inputMean = nb ? meanBase / nb : 0.0;
                const double gIn = ng ? gradIn / ng : 0.0;
                const double gRes = ng ? gradRes / ng : 0.0;
                if (inputMean <= 0.0 && g.measureTries < 12)
                {
                    ++g.measureTries;
                    Log("measure: the network was handed a black frame (attempt %u); the game is "
                        "probably still on a loading screen. Retrying in 240 frames.",
                        g.measureTries);
                }
                else
                {
                    const double meanRes = n ? sum / n : 0.0;
                    const bool first = !g.measured;
                    g.measured = true;
                    NoteFlicker(a, b, rowPitch, nw, nh, first);
                    // Whether the runtime emits them at all is unproven, and only OutputSmooth=0 says:
                    // this reads netColour after the output smooth, which replaces them wherever it
                    // runs, so with it on a 0 proves nothing. At most once in 1800 frames.
                    if (first || nonFinite != 0)
                        Log("measure, non-finite: %llu of %llu sampled pixels NaN or inf%s",
                            static_cast<unsigned long long>(nonFinite), static_cast<unsigned long long>(n),
                            g.settings.outputSmooth.load() > 0.0f
                                ? ", read after the output smooth, which replaces them: only OutputSmooth=0 measures the runtime"
                                : "; compose takes them as no correction");
                    if (first)
                    {
                        Log("measure, network input: mean absolute %.6f (%llu samples)",
                            inputMean, static_cast<unsigned long long>(nb));
                        Log("  0.000000 means a black image was handed to the network.");
                        Log("measure, residual at %ux%u: mean %.6f, max %.6f (%llu samples)",
                            nw, nh, meanRes, peak, static_cast<unsigned long long>(n));
                        Log("  mean 0.000000 means the network returned its input unchanged.");
                        Log("measure, residual detail: local variation of the correction %.6f "
                            "against %.6f in the image itself -- ratio %.3f", gRes, gIn,
                            gIn > 0.0 ? gRes / gIn : 0.0);
                        Log("  Near 0.000 means the correction is smooth across the frame: a "
                            "colour, exposure or saturation shift, with nothing done to texture. "
                            "Rising toward and past 0.100 means the correction follows the "
                            "image's own detail, which is the network working on structure.");
                    }
                    // The watchdog. A working run measures a residual of a few percent of the
                    // input (God of War: 0.021 on a 0.13 mean; a run that had broken measured
                    // 0.00024 on 0.45). One part in a thousand is far below any working reading
                    // and far above readback noise.
                    const bool inert = meanRes < inputMean * 1e-3;
                    if (inert != g.inert.load() || (first && inert))
                    {
                        g.inert.store(inert);
                        if (inert)
                            Log("WARNING: the network is returning its input unchanged (residual "
                                "mean %.6f against an input mean of %.6f). The picture will not "
                                "change with the effect on, whatever the log says about frames. "
                                "Checks, in order: output Scale %.5f (the runtime's default "
                                "is 0.03125); intensity %.2f; structure %.2f (0 removes the "
                                "effect); the runtime log for GPU errors or 'output stores are "
                                "being dropped'.",
                                meanRes, inputMean, static_cast<double>(g.settings.engineScale.load()),
                                static_cast<double>(g.settings.intensity.load()),
                                static_cast<double>(g.settings.structure.load()));
                        else
                            Log("measure: the network is changing the frame again (residual mean "
                                "%.6f against %.6f).", meanRes, inputMean);
                    }
                }
            }
            if (a != nullptr) g.readbackBase->Unmap(0, nullptr);
            if (b != nullptr) g.readbackNr->Unmap(0, nullptr);
        }
        if (f == nullptr || f->GetCompletedValue() < 1)
            g.parked.insert(g.parked.end(), { g.readbackBase, g.readbackNr });
        g.readbackBase.Reset();
        g.readbackNr.Reset();
    }
}
