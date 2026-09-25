// Temporal history: what each pass is handed as its previous output, and when that stops being
// true. Part of the core's unity build, included by neural.cpp right after State, so every route,
// the settings and the panel can drop it.
//
// The network is a denoiser: without a previous result to carry forward it starts from nothing
// every frame, and a motion vector -- which says where a pixel *was* -- has nothing to point at.
// History is the pair that turns motion from an input the engine merely reports into one it can
// use. It is a raw pointer written into one specific build of the runtime: a wrong one does not
// fail, it hangs the game.
//
// g.historyValid is one word so that every invalidation anywhere in the add-on drops everything at
// once: bit `slot` is set when history[slot] holds pass `slot`'s output of the previous frame.
//
// When it goes, on every route: a raster of a new size (Scale, or a back buffer size that lasted,
// RasterPin -- never a swapchain rebuild by itself, which some games do every few frames), a
// setting in SettingsInvalidateHistory, an ini read, a pass's own controls (ControlsChanged), the
// effect resuming from off or minimised, a Passes change, a guide in use moving to another
// buffer, a new OpenGL context, a list that failed to close, and a frame the chain did not run
// (DropStaleHistory); a pass that sat a frame out loses its own bit after the pass loop.
// tools/temporal_reset_check.py holds every store to this.

// The one way the whole history is dropped, so the log can say why. A line when the reason differs
// from the last drop's, with how often that reason has fired: a skip every other frame is one line,
// not thirty a second. Reasons are literals, keyed by address; the overlay can call this from
// another thread than the present, hence the lock.
void ResetTemporal(const char *why)
{
    static std::mutex m;
    static auto &times = *new std::unordered_map<const char *, unsigned>;  // never destroyed, as g
    static const char *last = nullptr;
    std::lock_guard hold(m);
    g.historyValid.store(0);
    const unsigned n = ++times[why];
    if (why != last)
        Log("temporal history dropped: %s (%u so far)", last = why, n);
}

// Before pass `slot` records: its own previous output, or nothing. See the declaration of
// history[] for why a pass never gets the chain's. Returns whether it was handed one.
bool ArmHistory(HMODULE r, UINT slot)
{
    const bool want = g.settings.useHistory.load() && (g.historyValid.load() & (1u << slot)) != 0 &&
                      g.history[slot] != nullptr;
    At<uint8_t>(r, rt::kHistoryOn) = want ? 1 : 0;
    At<void *>(r, rt::kHistory) = want ? static_cast<void *>(g.history[slot].Get()) : nullptr;
    if (want && !g.loggedHistory)
    {
        g.loggedHistory = true;
        Log("history: handing each pass its own previous output at %ux%u -- pass %u reads "
            "what pass %u wrote last frame, not what the chain ended on. Watch the engine "
            "log: it says history off there when it is ignoring this.",
            g.netWidth, g.netHeight, slot + 1, slot + 1);
    }
    return want;
}

void CopyToHistory(ID3D12GraphicsCommandList *cmd, UINT slot)
{
    Barrier(cmd, g.netColour.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, g.history[slot].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(g.history[slot].Get(), g.netColour.Get());
    Barrier(cmd, g.history[slot].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, g.netColour.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.historyValid.fetch_or(1u << slot);
}

// After pass `slot` recorded, keep what it produced as its history for the next frame.
//
// Same frame is the only mode, so the engine's work is on this same list: a copy recorded straight
// after the pass reads what the pass wrote and lands before the next pass overwrites netColour.
// Doing it once after the loop -- which is what this used to do -- could only ever capture the last
// pass, so every earlier pass was handed a reference belonging to a different stage of the chain.
void KeepHistory(ID3D12GraphicsCommandList *cmd, UINT slot)
{
    if (g.settings.useHistory.load() && g.history[slot] != nullptr)
        CopyToHistory(cmd, slot);
}

// Whether the motion handed with the next evaluation is measured against the last evaluated frame --
// the estimator and the optical flow only run when the network does -- rather than against the
// previous present, as a game's vectors and the companion effect's are. Set by RecordNetwork.
bool g_motionSinceEvaluation = false;

// At the end of every present. A frame the chain did not run -- skipped because the previous
// evaluation was still pending, or refused by the engine -- leaves every history a frame older.
// With motion measured present to present that is a history older than the single frame of motion
// handed with it next time, and every further skip widens the gap; invalidating costs the denoiser
// one frame of accumulation. Motion measured between evaluations spans exactly the gap the history
// does, so there the history stays: measured on the synthetic set, dropping it there cost the whole
// temporal gain on still content (static-dither TI 2.96e-3 -> 5.46e-3) and bought nothing.
//
// This used to test only activePasses, which is written on evaluated frames alone: on a skipped
// one it still held the last evaluation's count and the history was never dropped. With History
// off no bit is ever set (KeepHistory), and switching it off drops them (SettingsInvalidateHistory).
void DropStaleHistory(bool ran)
{
    if (ran && g.activePasses == 0)
        ResetTemporal("no pass ran");
    else if (!ran && !g_motionSinceEvaluation)
        ResetTemporal("a skipped frame, with motion measured present to present");
}
