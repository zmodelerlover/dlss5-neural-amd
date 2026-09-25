// The runtime modules' job control: which module a pass records through, whether any still has a
// job in flight, the frame-notify, the waits on our own queue, the run/skip gate every route goes
// through and the watchdog run it keeps, whether a list still reads the descriptor heap, and how
// many modules the pass count runs on. Part of the core's unity build, included by neural.cpp.

// The module pass `slot` records through. Without a copy of its own a pass shares the first
// module, which is how every pass ran before the copies existed.
HMODULE RuntimeFor(UINT slot)
{
    return slot < State::kMaxPasses && g.runtimes[slot] != nullptr ? g.runtimes[slot] : g.runtime;
}

// The jobs module m has taken and not finished, by its own count: kJobId moves on each record, and
// the worker moves kJobCounter as it retires them, strictly in order. The record entry refuses a
// job on the same subtraction, at four in flight (0x15383), and teardown zeroes both. Same frame
// only: async never moves kJobCounter, which is why async is retired.
int Outstanding(HMODULE m)
{
    return static_cast<int>(static_cast<UINT>(At<volatile LONG>(m, rt::kJobId)) -
                            static_cast<UINT>(InterlockedCompareExchange(
                                reinterpret_cast<volatile LONG *>(&At<UINT>(m, rt::kJobCounter)), 0, 0)));
}

// Whether any module still has a job in flight. Asked of the runtime, not of what this add-on
// remembers recording: a job it forgot is still in the queue, and the next one retires after it.
bool RuntimeBusy()
{
    for (HMODULE m : g.runtimes)
        if (m != nullptr && Outstanding(m) > 0)
            return true;
    return false;
}

// The frame-notify, to every module that recorded onto the lists being submitted. A module that
// recorded nothing is not told: what its notify does with a list it never saw is not known.
void NotifyRuntimes(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    for (UINT i = 0; i < State::kMaxPasses; ++i)
        if ((g.recordedMask & (1u << i)) != 0 && g.runtimes[i] != nullptr)
            reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(g.runtimes[i]) + rt::kNotifyFn)(
                queue, count, lists);
    g.recordedMask = 0;
}

// A list the runtime recorded onto that will never run, because its Close failed. It is still
// notified, on the queue it would have run on: the worker only takes a job it was told of, so an
// untold one stays outstanding for ever and JobGate skips the network for good. Told, it waits for
// a capture that never lands and retires on the runtime's own timeout. The notify compares the list
// with its marker and never touches it (0x9460).
void RetireUnsubmitted(ID3D12CommandQueue *queue, ID3D12CommandList *list)
{
    NotifyRuntimes(queue, 1, &list);
    g.activePasses = 0;
    ResetTemporal("a command list failed to close");
    g.jobRunning = false;  // that wait is the runtime's timeout, not what the network costs
}

// A removed device fails every call silently from here on: the copies stop landing, the shared
// output keeps whatever it was created with -- black -- and that black is what gets copied into
// the back buffer, every frame, for ever. Worth naming the moment it happens.
bool DeviceLost()
{
    if (g.device == nullptr)
        return false;
    const HRESULT reason = g.device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason))
        return false;
    if (!g.loggedDeviceLost)
    {
        g.loggedDeviceLost = true;
        Log("the D3D12 device was removed (0x%08lX). Nothing this add-on draws will reach the "
            "screen any more, so it is stepping out of the way and leaving the game's own image "
            "alone.", reason);
    }
    g.status.unavailable = true;
    g.status.reason = "the D3D12 device was removed; restart the game to re-enable";
    return true;
}

// Wait for a fence to reach a value, for FenceWaitCapMs at most (10 s; 0 waits for ever).
//
// This used to be a bare WaitForSingleObject with a 2 s cap whose result was thrown away, in both
// of the places that wait on our own queue. Two seconds is generous while the game is in front
// and nowhere near enough the moment it is not: Windows deprioritises a background process's GPU
// work heavily, so on the way into and out of an alt-tab our queue routinely takes longer than
// that. Every caller then carried on as if the GPU were finished -- resetting a command allocator
// whose list is still executing, releasing shared textures the queue still references, reading a
// result that is still being written. All three are undefined behaviour, and all three happen in
// the same few frames users report as "crashes when I alt-tab".
//
// Waiting longer is slow. Carrying on is an access violation. So the wait ends on the device dying
// or, past the cap (or with nothing to wait with), with the add-on standing down: every false from
// here has stood it down, and no caller goes on to reuse or release what the GPU may still read.
// The ring waits and the per-frame copy-back skip the frame, a between-pass finish fails the
// route, and ReleaseSwapchainSized and the OpenGL rebuild return first, leaving it all parked
// where it is for the rest of the session. Once one wait has given up, later ones (each resize,
// the exit) only look: the queue that did not finish then is not waited out again for the whole
// cap on the game's thread.
bool WaitFence(ID3D12Fence *fence, UINT64 value, HANDLE ev, const char *what)
{
    static std::atomic<bool> gaveUp { false };
    if (fence == nullptr || fence->GetCompletedValue() >= value)
        return true;
    if (gaveUp)
        return false;
    if (const HRESULT hr = ev == nullptr ? E_HANDLE : fence->SetEventOnCompletion(value, ev); FAILED(hr))
    {
        Log("bridge: could not wait on %s (0x%08lX). Standing down, and keeping everything the GPU "
            "may still read.", what, static_cast<unsigned long>(hr));
        g.status.unavailable = true;
        g.status.reason = "could not wait for the GPU to finish our work; restart the game to re-enable";
        return false;
    }
    const ULONGLONG start = GetTickCount64();
    const ULONGLONG cap = static_cast<ULONGLONG>(std::max(0, g.settings.fenceWaitCapMs.load()));
    // The value decides, never the wake. These events are auto-reset and shared, so a wake can be
    // a registration some other wait left behind -- or one this wait leaves behind if it gives up.
    for (bool told = false;;)
    {
        const ULONGLONG waited = GetTickCount64() - start;
        if (fence->GetCompletedValue() >= value)
        {
            if (told)
                Log("bridge: %s took %llu ms -- the process was most likely in the background.",
                    what, waited);
            return true;
        }
        if (DeviceLost())
        {
            Log("bridge: %s will never complete; the device is gone.", what);
            return false;
        }
        if (cap != 0 && waited >= cap)
        {
            Log("bridge: gave up on %s after %llu ms (FenceWaitCapMs=%llu; 0 waits for ever). "
                "Standing down, and keeping everything the GPU may still read.", what, waited, cap);
            gaveUp = true;
            g.status.unavailable = true;
            g.status.reason = "the GPU did not finish our work within FenceWaitCapMs; restart the "
                              "game to re-enable";
            return false;
        }
        if (!told && waited >= 2000)
        {
            told = true;
            Log("bridge: still waiting on %s after %llu ms. Holding on rather than reusing what "
                "the GPU still owns.", what, waited);
        }
        WaitForSingleObject(ev, static_cast<DWORD>(cap == 0 ? 2000
                                                            : std::min<ULONGLONG>(2000, cap - waited)));
    }
}

// The other half: wait for our own queue to finish what was just submitted. With no event there is
// nothing to wait with, and the caller goes on as it always has: only native D3D12 lacks one (until
// a between-pass finish makes it), and it never waited at its teardown. ponytail: a teardown there
// that waits would take g.lock-held time on the game's queue; add it once that is measured.
bool WaitForWorkQueue(UINT64 value)
{
    return g.completionEvent == nullptr ||
           WaitFence(g.fence.Get(), value, g.completionEvent, "the network to finish");
}

// Same frame means the game waits for the network, and the runtime only makes the GPU queue wait.
// A route that does not hold the CPU once a frame (D3D12, and OpenGL once its fences are proven)
// holds here instead, at the start of the next present, so the game's CPU work has already
// overlapped the network; a route that does finds the fence landed and only waits out the counter
// below. Keyed on the engine's latched mode: until a job has latched same-frame
// there is nothing to wait for, and a module that latches async stands the add-on down in
// RecordNetwork (its kJobCounter never moves). Bounded to 500 ms after the job was recorded, so a
// stuck job costs raw frames, never a hang, and past that no spin either: a job that old is stuck,
// not a counter a fraction of a millisecond behind its fence.
void WaitForPreviousJob()
{
    if (g.runtime == nullptr || At<uint8_t>(g.runtime, rt::kInlineActive) != 1)
        return;
    // Not g.completionEvent: a timed-out SetEventOnCompletion signals late and would end a later
    // WaitFence early. A stale signal here only costs one more trip round the loop.
    static const HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const UINT64 deadline = g.lastJobAt + 500;
    for (UINT64 now = GetTickCount64();
         ev != nullptr && now < deadline && g.fence->GetCompletedValue() < g.completion;
         now = GetTickCount64())
        if (FAILED(g.fence->SetEventOnCompletion(g.completion, ev)) ||
            WaitForSingleObject(ev, static_cast<DWORD>(deadline - now)) == WAIT_FAILED)
            break;
    if (GetTickCount64() >= deadline)
        return;
    // ponytail: kJobCounter lands ~0.1 ms after the flag releases the spin. A spin the watchdog cut
    // short leaves it behind for the rest of the network: 2 ms, then that frame goes raw as before.
    LARGE_INTEGER hz {}, t0 {}, t {};
    QueryPerformanceFrequency(&hz);
    QueryPerformanceCounter(&t0);
    while (RuntimeBusy() && QueryPerformanceCounter(&t) && t.QuadPart - t0.QuadPart < hz.QuadPart / 500)
        SwitchToThread();
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
        const UINT now = r != nullptr ? At<volatile UINT>(r, rt::kWatchdogFires) : 0;
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

// Whether this frame runs the network: the one run/skip decision, taken once a present by every
// route before it records. A job still pending skips the network, and the frame goes out as the
// game drew it, for as long as the runtime has not retired it. No timer lets it go: this once gave
// up at 500 ms and recorded on top, but the runtime retires strictly in order, so the new job only
// queued behind the late one, until four were in flight and the runtime refused. A job that never
// retires is a device that is gone, and DeviceLost stands the add-on down for that. Our own queue
// fence keeps its 500 ms: past that WaitForPreviousJob no longer waits on it, so a signal a
// GPU-bound game keeps queued would skip every frame for good, and a list recorded behind it on
// the same queue stacks no runtime job. One copy, because five had drifted: Vulkan and OpenGL
// recorded on top of a late job, and never timed the jobs NoteJobCost needs to see.
bool JobGate()
{
    WaitForPreviousJob();
    const bool pending = RuntimeBusy() || (g.fence->GetCompletedValue() < g.completion &&
                                           GetTickCount64() - g.lastJobAt < 500);
    if (!pending && g.jobRunning)
    {
        g.jobRunning = false;
        const bool fired = NoteWatchdog();
        NoteJobCost(GetTickCount64() - g.lastJobAt, fired);
    }
    if (!pending)
        return !g.status.unavailable;  // either note may have just stood the add-on down
    DeviceLost();
    if (++g.status.skipped % 120 == 1)
        Log("network skipped: previous evaluation still pending (%llu skipped, %llu done). "
            "Those frames go out as the game drew them; a correction aimed at an older "
            "picture reads as a trail, not as detail.",
            static_cast<unsigned long long>(g.status.skipped),
            static_cast<unsigned long long>(g.status.frame));
    if (g.status.skipped % 600 == 0)
        for (UINT m = 0; m < State::kMaxPasses; ++m)
            if (g.runtimes[m] != nullptr && Outstanding(g.runtimes[m]) > 0)
                Log("module %u: job %u still outstanding (the runtime has retired %u), %llu ms "
                    "after the last record; the network waits for it.", m,
                    At<UINT>(g.runtimes[m], rt::kJobId), At<UINT>(g.runtimes[m], rt::kJobCounter),
                    static_cast<unsigned long long>(GetTickCount64() - g.lastJobAt));
    return false;
}

// Whether the last list that read the descriptor heap may still be running, in which case
// RecordNetwork records nothing and the route skips the frame as it does when JobGate says no. The
// heap is written on the CPU every present and a list reads its views when it executes, not when it
// was recorded, so rewritten under a list still in flight that list composes from this frame's
// views: another back buffer. Only D3D12, and OpenGL once its fences drive the queue, record ahead
// of the GPU, and only where WaitForPreviousJob did not hold them: past its 500 ms, or before the
// engine latched same-frame. Keyed on the last list that read the heap, not on g.completion: a
// skipped frame still signals a list that reads nothing, and chasing that value would keep a
// GPU-bound game raw for good. `ran`: whether JobGate let this frame run, so a skip counts once.
// ponytail: skip, not a ring of heaps; a table set per frame in flight if the count logged says
// this is common.
bool HeapStillRead(bool ran)
{
    static bool wrote = false;  // whether the last call went on to write the heap
    static UINT64 reader = 0;   // the fence value the last list that read it signals
    static UINT64 held = 0;     // frames this held back
    if (wrote)
        reader = g.completion;
    wrote = g.fence == nullptr || g.fence->GetCompletedValue() >= reader;
    if (wrote)
        return false;
    if (ran)
        ++g.status.skipped;
    if (++held % 600 == 1)
        Log("network skipped: the last list that read the descriptor heap is still on the GPU "
            "(%llu such frames).", static_cast<unsigned long long>(held));
    DropStaleHistory(false);
    return true;
}

// Both sides must finish before the next pass changes the module's tuning.
// The GPU can finish via the timeout fallback while the HIP worker is still
// reading the previous pass's globals, so the queue fence alone is insufficient.
bool FinishSubmittedPass()
{
    if (g.completionEvent == nullptr)
        g.completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g.completionEvent == nullptr || g.fence == nullptr)
        return false;
    g.completion = ++g.serial;
    if (FAILED(g.queue->Signal(g.fence.Get(), g.completion)) ||
        !WaitFence(g.fence.Get(), g.completion, g.completionEvent, "the intermediate pass"))
        return false;
    const UINT64 deadline = GetTickCount64() + 5000;
    while (RuntimeBusy())
    {
        if (GetTickCount64() >= deadline || DeviceLost())
        {
            Log("intermediate pass: job %u did not finish; refusing to overwrite its parameters",
                g.lastJob);
            return false;
        }
        Sleep(1);
    }
    return true;
}

bool SubmitPrivatePass(ID3D12GraphicsCommandList *cmd, ID3D12CommandAllocator *allocator)
{
    if (FAILED(cmd->Close()))
        return false;
    ID3D12CommandList *lists[] {cmd};
    g.queue->ExecuteCommandLists(1, lists);
    NotifyRuntimes(g.queue.Get(), 1, lists);
    if (!FinishSubmittedPass())
        return false;
    return SUCCEEDED(allocator->Reset()) && SUCCEEDED(cmd->Reset(allocator, nullptr));
}

// How many passes are asked for. BringUpEngines decides how many run.
//
// Same frame means the game is blocked on the GPU until every pass has finished, so N passes add
// into one stall of N times a single evaluation. That used to force the count back to 1, so the
// slider moved, saved, and did nothing. The reason for the force was the crash theory, and one
// full engine per pass in VRAM is gone. The cost is framerate, which the overlay colours and says.
UINT WantedPasses()
{
    return static_cast<UINT>(
        std::clamp(g.settings.passes.load(), 1, static_cast<int>(State::kMaxPasses)));
}

// How many passes the modules already loaded can run: pass 1's and each copy after it, or every
// pass on the legacy batch path, which runs them all through pass 1's. 0 until the engine is up.
UINT PassesAvailable()
{
    UINT n = g.runtime == nullptr ? 0 : g.settings.serialPasses.load() ? 1 : State::kMaxPasses;
    while (n != 0 && n < State::kMaxPasses && g.runtimes[n] != nullptr)
        ++n;
    return n;
}

bool InitPipeline();
bool InitEngine();
bool LoadExtraRuntime(UINT slot);

// The first engine, plus one copy per further pass, and the one latch on the pass count: every
// route calls this before JobGate, and the 32-bit host's SET_STATE only stores the request.
//
// Copies only in serial mode: that is the only mode in which each pass is submitted and finished on
// its own, which is what lets each module be told about exactly the list it recorded. The legacy
// batch path keeps the single shared module it always had. A copy that fails to come up clamps the
// pass count to what did, and says so, rather than silently running that pass through pass 1's
// state.
//
// Copies load at the first bring-up only. A module is never unloaded -- about 150 MB of VRAM and a
// worker polling with Sleep(0) each, for the session -- and a copy that failed was tried again on
// every present, a 7 MB file copy and a LoadLibrary a frame. So a later change runs on what is
// loaded, and more passes than that wait for a restart, which the panel says.
//
// A change is taken on a present with no job in flight in any module, and history starts again:
// a pass that sat out holds an old frame, and the chain ends on another pass. The drain is a wait,
// not a skip to the next present, because on D3D12 the last job is still running when the next
// present arrives, every frame. Bounded at 2 s under g.lock, one job in practice and only when
// somebody moved the slider; three in a row that ran out mean the runtime is not retiring jobs,
// and every later change waits for a restart too. A drain that succeeds starts the count over.
bool BringUpEngines(UINT &wanted)
{
    if (!InitPipeline() || !InitEngine())
        return false;
    // The request last seen (0: none yet), the count running, and drains in a row that ran out.
    static UINT asked = 0, live = 0, timeouts = 0;
    if (asked == 0)
        for (UINT slot = 1; slot < wanted && g.settings.serialPasses.load(); ++slot)
            if (!LoadExtraRuntime(slot))
            {
                Log("pass count clamped to %u: no separate runtime for pass %u. See the lines "
                    "above for why.", slot, slot + 1);
                break;
            }
    const UINT next = std::min(wanted, PassesAvailable());
    const bool fresh = wanted != asked;
    if (fresh && next < wanted && asked != 0)
        Log("passes %u: the runtime is loaded for %u this session; the rest start after a restart.",
            wanted, next);
    asked = wanted;
    if (live == 0 || next == live)
        live = next;
    else if (timeouts < 3)
    {
        const UINT64 deadline = GetTickCount64() + 2000;
        while (RuntimeBusy() && GetTickCount64() < deadline && !DeviceLost())
            Sleep(1);
        if (!RuntimeBusy())
        {
            Log("passes %u -> %u, taken with every module drained; history starts again.", live, next);
            live = next;
            timeouts = 0;
            ResetTemporal("Passes changed");
        }
        else
            Log("passes %u -> %u: a job was still in flight after 2 s; %s.", live, next,
                ++timeouts < 3 ? "trying again next frame"
                               : "this change and every later one apply after a restart");
    }
    else if (fresh)
        Log("passes %u -> %u applies after a restart: the runtime did not drain in 2 s, three times "
            "running.", live, next);
    wanted = live;
    return true;
}
