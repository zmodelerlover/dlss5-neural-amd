// The runtime modules' job control: which module a pass records through, whether any still has a
// job in flight, the frame-notify, and the waits on our own queue. Part of the core's unity build,
// included by neural.cpp.

// The module pass `slot` records through. Without a copy of its own a pass shares the first
// module, which is how every pass ran before the copies existed.
HMODULE RuntimeFor(UINT slot)
{
    return slot < State::kMaxPasses && g.runtimes[slot] != nullptr ? g.runtimes[slot] : g.runtime;
}

// Whether any module still has a job in flight: its job counter has not reached the id of the
// last job this add-on recorded through it.
bool RuntimeBusy()
{
    for (UINT i = 0; i < State::kMaxPasses; ++i)
    {
        HMODULE m = g.runtimes[i];
        if (m == nullptr)
            continue;
        if (static_cast<UINT>(InterlockedCompareExchange(
                reinterpret_cast<volatile LONG *>(&At<UINT>(m, rt::kJobCounter)), 0, 0)) <
            g.lastJobs[i])
            return true;
    }
    return false;
}

void ResetJobs()
{
    g.lastJob = 0u;
    for (UINT &j : g.lastJobs)
        j = 0;
    g.recordedMask = 0;
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
    g.status.reason = "the D3D12 device was removed; see amd-nr.log";
    return true;
}

// Wait for a fence to reach a value, and do not give up on it.
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
// Waiting longer is slow. Carrying on is an access violation. The only real end to the wait is
// the device dying, which is what the loop checks for.
bool WaitFence(ID3D12Fence *fence, UINT64 value, HANDLE ev, const char *what)
{
    if (fence == nullptr || fence->GetCompletedValue() >= value)
        return true;
    if (ev == nullptr || FAILED(fence->SetEventOnCompletion(value, ev)))
        return false;
    // The value is read again after every wake. These events are auto-reset and shared, so a wake
    // can be a registration some other wait left behind; ours is still pending and is waited for.
    for (UINT waited = 0;; waited += 2)
    {
        if (WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0 && fence->GetCompletedValue() >= value)
        {
            if (waited != 0)
                Log("bridge: %s took %u s -- the process was most likely in the background.",
                    what, waited);
            return true;
        }
        if (DeviceLost())
        {
            Log("bridge: %s will never complete; the device is gone.", what);
            return false;
        }
        if (waited == 0)
            Log("bridge: still waiting on %s after 2 s. Holding on rather than reusing what the "
                "GPU still owns.", what);
    }
}

// The other half: wait for our own queue to finish what was just submitted.
void WaitForWorkQueue(UINT64 value)
{
    WaitFence(g.fence.Get(), value, g.completionEvent, "the network to finish");
}

// Same frame means the game waits for the network, and the runtime only makes the GPU queue wait.
// A route that does not hold the CPU once a frame (D3D12, and OpenGL once its fences are proven)
// holds here instead, at the start of the next present, so the game's CPU work has already
// overlapped the network. Keyed on the engine's latched mode: until a job has latched same-frame
// there is nothing to wait for, and a module that latches async stands the add-on down in
// RecordNetwork (its kJobCounter never moves). Bounded by the skip window, so a stuck job still
// ends in its ResetJobs, never in a hang.
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
    // ponytail: kJobCounter lands ~0.1 ms after the flag releases the spin. A spin the watchdog cut
    // short leaves it behind for the rest of the network: 2 ms, then that frame goes raw as before.
    LARGE_INTEGER hz {}, t0 {}, t {};
    QueryPerformanceFrequency(&hz);
    QueryPerformanceCounter(&t0);
    while (RuntimeBusy() && QueryPerformanceCounter(&t) && t.QuadPart - t0.QuadPart < hz.QuadPart / 500)
        SwitchToThread();
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
