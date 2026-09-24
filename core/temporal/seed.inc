// The pre-block's noise seed, pinned. Part of the core's unity build, included by neural.cpp.
//
// The runtime hands its pre-block kernel the engine's evaluation count (rt::kFrameCounter, read at
// launch in sub_18002D2D0 and bumped after every job), and the network's output moves with it: on a
// still frame with nothing temporal on, consecutive outputs differ by 0.44/255 on average and up to
// 15/255 (E5). NVIDIA cycles its seed 0..7 per frame; the reference pins it at 0
// (native_game_oneshot.h:179). FixedSeed=1 does what the reference does. The worker's one-time
// self-check fires on the count it is set to, which starts at 0, so once the first job has run it is
// moved out of reach; otherwise a pinned count would read the pre-block back on every frame.

void PinSeed(HMODULE r)
{
    // Said once per change, so the log answers "is it on" without a line per job.
    static int logged = -1;
    if (const int on = g.settings.fixedSeed.load(); on != logged)
    {
        logged = on;
        Log("noise seed: %s", on ? "pinned at 0 (FixedSeed=1, as the reference does)"
                                 : "the engine's own count, one step per job (FixedSeed=0)");
    }
    if (g.settings.fixedSeed.load() == 0)
        return;
    if (At<int>(r, rt::kFrameCounter) > 0)
        At<int>(r, rt::kSelfCheckFrame) = -1;
    At<int>(r, rt::kFrameCounter) = 0;
}
