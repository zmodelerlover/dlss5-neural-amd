// The mochizuki runtime: MochizukiNrRuntime.dll, mochizuki0323's Vulkan port of the network behind
// the C ABI the OptiScaler fork drives it with (3rdparty/mochizuki). NrBackend=mochizuki in
// amd-nr.ini picks it over dlssnr_amd_pass1.dll, read once, so a change needs a restart.
//
// It runs on a Vulkan device of its own on the same GPU and reaches our D3D12 device through
// shared buffers and a shared fence. The list is cut in two around it: the inputs are copied out,
// the first half is submitted, the runtime queues the network between the halves on our queue,
// and the second half copies the answer back into netColour. From there on the frame is the one
// the danielblnc runtime leaves: residual, compose and measurement read netColour as before.
// Passes and temporal history live inside the runtime, so none of the per-module copies apply.
//
// Included inside neural.cpp's anonymous namespace, after TuningFor.

#include "../../3rdparty/mochizuki/LmxxfNrApi.h"
#include "../../3rdparty/mochizuki/MochizukiNrControls.h"
#include "../shared/runtime_choice.h"

struct Mochizuki {
    int selected = -1; // -1 until amd-nr.ini is read; then the runtime this run uses
    int chosen = 0;    // what amd-nr.ini asks for now, for the next launch
    bool installed[runtime_choice::kCount]{};
    HMODULE dll = nullptr;
    LmxxfNrApi api{};
    PFN_MochizukiNrSetControls setControls = nullptr;
    PFN_MochizukiNrGetInfo getInfo = nullptr;
    MochizukiNrControls defaults{}, sent{};
    bool sentValid = false;
    void* session = nullptr;
    ID3D12Device* device = nullptr; // the device the session was created on
    void* lastJob = nullptr;
    std::string reason, said;
} mz;

bool MzSelected() {
    if (mz.selected < 0) {
        mz.chosen = runtime_choice::Read(ExeDirectory());
        for (int r = 0; r < runtime_choice::kCount; ++r)
            mz.installed[r] = runtime_choice::Installed(ExeDirectory(), r);
        // The installer takes mochizuki's files out when it is unticked and leaves amd-nr.ini to the
        // person, so an ini can still name it here: run danielblnc then, rather than nothing.
        mz.selected =
            mz.chosen == runtime_choice::kMochizuki && mz.installed[runtime_choice::kMochizuki] ? 1
                                                                                                : 0;
        if (mz.selected == 1)
            Log("runtime: mochizuki (NrBackend=mochizuki in amd-nr.ini)");
        else if (mz.chosen == runtime_choice::kMochizuki)
            Log("runtime: amd-nr.ini names mochizuki, but MochizukiNrRuntime.dll or dlssnr-amd\\dlssnr.bin "
                "is not here; running danielblnc");
    }
    return mz.selected == 1;
}

// The runtime's own words for what went wrong, logged when they change: a network build says
// the same thing for every frame it takes.
void MzSay(const char* what) {
    char err[256]{};
    if (mz.api.GetLastError != nullptr)
        mz.api.GetLastError(err, sizeof err);
    std::string line = std::string(what) + ": " + err;
    if (line != mz.said) {
        mz.said = line;
        Log("mochizuki: %s", line.c_str());
    }
}

bool MzOff(const char* what) {
    MzSay(what);
    mz.reason = "mochizuki runtime: " + mz.said + "; see amd-nr.log and mochizuki_nr.log";
    g.status.reason = mz.reason.c_str();
    return false;
}

bool MzLoad() {
    if (mz.dll != nullptr)
        return true;
    const auto file = ExeDirectory() / L"MochizukiNrRuntime.dll";
    HMODULE h = LoadLibraryExW(file.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (h == nullptr) {
        Log("mochizuki: %ls did not load (error %lu). It needs vulkan-1.dll, which the AMD driver "
            "installs.",
            file.c_str(), GetLastError());
        mz.reason = "MochizukiNrRuntime.dll did not load; see amd-nr.log";
        g.status.reason = mz.reason.c_str();
        return false;
    }
    // Never unloaded: its build thread and the Vulkan driver can outlive any point we could
    // free it at.
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(h), &pinned);
    auto getApi =
        reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi*)>(GetProcAddress(h, "LmxxfNrGetApi"));
    mz.api.struct_size = sizeof mz.api;
    if (getApi == nullptr || getApi(LMXXF_NR_ABI_VERSION, &mz.api) != LMXXF_NR_OK) {
        Log("mochizuki: %ls does not offer ABI %u", file.c_str(), LMXXF_NR_ABI_VERSION);
        mz.reason = "MochizukiNrRuntime.dll is not a build this add-on can drive";
        g.status.reason = mz.reason.c_str();
        return false;
    }
    mz.setControls =
        reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(h, "MochizukiNrSetControls"));
    mz.getInfo = reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(h, "MochizukiNrGetInfo"));
    mz.defaults.struct_size = sizeof mz.defaults;
    if (auto defaults = reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(
            GetProcAddress(h, "MochizukiNrGetControlDefaults")))
        defaults(&mz.defaults);
    mz.dll = h;
    return true;
}

// A session per D3D12 device. The one on a device that went away is drained and dropped first.
bool MzInit() {
    if (mz.session != nullptr && mz.device == g.device.Get())
        return true;
    if (!MzLoad())
        return false;
    if (mz.session != nullptr) {
        mz.api.Drain(mz.session);
        mz.api.Destroy(mz.session);
        mz.session = nullptr;
        mz.lastJob = nullptr;
        mz.sentValid = false;
    }
    const std::wstring dir = ExeDirectory().wstring();
    LmxxfNrCreateInfo ci{sizeof ci, g.device.Get(), g.queue.Get(), dir.c_str(),
                         LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK};
    void* s = nullptr;
    if (mz.api.Create(&ci, &s) != LMXXF_NR_OK)
        return MzOff("Create");
    // The runtime makes its own Vulkan instance here. ReShade's Vulkan layer, when it is installed
    // system-wide, would wrap that instance as if it were a game's and report its device to every
    // add-on, this one included. The layer's manifest names this variable as its off switch.
    constexpr wchar_t kNoLayer[] = L"DISABLE_VK_LAYER_reshade_1";
    wchar_t old[8]{};
    const DWORD had = GetEnvironmentVariableW(kNoLayer, old, 8);
    SetEnvironmentVariableW(kNoLayer, L"1");
    const int32_t rc = mz.api.PrepareSession(s);
    SetEnvironmentVariableW(kNoLayer, had != 0 && had < 8 ? old : nullptr);
    if (rc != LMXXF_NR_OK) {
        MzOff("PrepareSession");
        mz.api.Destroy(s);
        return false;
    }
    mz.session = s;
    mz.device = g.device.Get();
    Log("mochizuki: session ready. The first frames go out without the network while it is "
        "built; the first time in each game that takes up to a minute (pipeline compilation).");
    return true;
}

// The model's controls, from the same settings the danielblnc runtime reads: it is the same
// network, so Structure, Tone, Skin, the character mask and the per-pass profiles mean the same.
// Intensity, the limit, colour strength and the grade stay in compose, so the runtime's own detail
// and colour strengths stay at 1. The OptiScaler host sends colour 0 instead; on the framecheck
// frame Colour Strength 0 in compose gives the same mean change to 0.0001. Sent only on a change.
void MzSendControls() {
    if (mz.setControls == nullptr)
        return;
    MochizukiNrControls c = mz.defaults;
    c.struct_size = sizeof c;
    const PassTune first = TuningFor(0);
    c.local_structure = first.structure;
    c.local_tone = first.tone;
    c.skin_structure = first.skin;
    c.automatic_mask = g.settings.autoMask.load() != 0 ? 1u : 0u;
    // Encoding 0 hands the network the game's own sRGB values; 1 and 2 hand it linear light.
    c.linear_input = g.settings.encoding.load() == 0 ? 2u : 1u;
    for (UINT i = 1; i < State::kMaxPasses; ++i) {
        const PassTune t = TuningFor(i);
        MochizukiNrPassControls& p = c.pass[i - 1];
        p.used = 1;
        p.style = c.style;
        p.intensity = c.intensity;
        p.local_tone = t.tone;
        p.local_structure = t.structure;
        p.skin_structure = t.skin;
        p.automatic_mask = c.automatic_mask;
    }
    if (mz.sentValid && std::memcmp(&c, &mz.sent, sizeof c) == 0)
        return;
    if (mz.setControls(mz.session, &c) != LMXXF_NR_OK) {
        MzSay("SetControls");
        return;
    }
    mz.sent = c;
    mz.sentValid = true;
    Log("mochizuki: controls structure %.2f, tone %.2f, skin %.2f, mask %u, linear input %s",
        static_cast<double>(c.local_structure), static_cast<double>(c.local_tone),
        static_cast<double>(c.skin_structure), c.automatic_mask,
        c.linear_input == 1 ? "on" : "off");
}

// Runs the network on netColour in place. `accepted` stays 0 on a frame that goes without it (the
// network still building, a refused format). False only when the list could not be carried on.
bool MzRecord(ID3D12GraphicsCommandList*& cmd, bool haveMotion, UINT passes,
              const SubmitPassFn& submitPass, UINT& accepted) {
    if (mz.lastJob != nullptr) {
        mz.api.Retire(mz.session, mz.lastJob);
        mz.lastJob = nullptr;
    }
    MzSendControls();
    LmxxfNrFrameInfo fi{};
    fi.struct_size = sizeof fi;
    fi.frame_id = g.status.frame;
    fi.command_list = cmd;
    fi.color_width = g.netWidth;
    fi.color_height = g.netHeight;
    fi.color = g.netColour.Get();
    fi.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    fi.model_scale = 1.0f;
    fi.passes = passes;
    // netMotion is in pixels of the network's raster, pointing to where the pixel was: the NGX
    // convention the runtime takes, at a scale of one.
    if (haveMotion && g.settings.useHistory.load() && g.netMotion != nullptr) {
        fi.flags = LMXXF_NR_FRAME_FLAG_TEMPORAL;
        fi.motion = g.netMotion.Get();
        fi.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        fi.motion_scale_x = 1.0f;
        fi.motion_scale_y = 1.0f;
    }
    fi.reset = (g.historyValid.load() & 1u) == 0 ? 1u : 0u;
    LmxxfNrJob job{sizeof job, nullptr, nullptr};
    if (mz.api.PrepareFrame(mz.session, &fi, &job) != LMXXF_NR_OK) {
        MzSay("frame without the network");
        return true;
    }
    if (mz.api.RecordInputs(mz.session, job.handle, cmd) != LMXXF_NR_OK) {
        MzSay("RecordInputs");
        mz.api.CancelUnsubmitted(mz.session, job.handle);
        return true;
    }
    // The network has to find the copies done, so the first half goes to the queue now.
    if (!submitPass || !submitPass()) {
        mz.api.CancelUnsubmitted(mz.session, job.handle);
        g.status.failed = true;
        Log("mochizuki: could not submit the list ahead of the network. Stopping.");
        return false;
    }
    mz.api.EnqueueHip(mz.session, job.handle, g.queue.Get());
    if (mz.api.RecordOutputs(mz.session, job.handle, cmd) != LMXXF_NR_OK) {
        MzSay("RecordOutputs");
        mz.lastJob = job.handle;
        return true;
    }
    auto* result = static_cast<ID3D12Resource*>(job.private_output);
    Barrier(cmd, result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, g.netColour.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(g.netColour.Get(), result);
    Barrier(cmd, g.netColour.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, result, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    mz.lastJob = job.handle;
    if (!mz.said.empty()) {
        mz.said.clear();
        Log("mochizuki: the network is running");
    }
    g.historyValid.fetch_or(1u);
    accepted = passes;
    return true;
}

// For the panel: the runtime choice, and the mochizuki status column line.
void MzStatus(PanelStatus& st) {
    const bool selected = MzSelected();
    st.runtimeActive = mz.selected;
    st.runtimeChosen = mz.chosen;
    for (int r = 0; r < runtime_choice::kCount; ++r)
        st.runtimeInstalled[r] = mz.installed[r];
    if (!selected)
        return;
    st.mochizuki = 1;
    MochizukiNrInfo info{};
    info.struct_size = sizeof info;
    if (mz.session == nullptr || mz.getInfo == nullptr ||
        mz.getInfo(mz.session, &info) != LMXXF_NR_OK)
        return;
    st.mochizuki = info.failed != 0 ? 3 : info.building != 0 || info.frames == 0 ? 1 : 2;
    st.mochizukiMs = info.gpu_ms_median;
}

void MzChoose(const PanelActions& actions) {
    if (actions.runtime < 0 || actions.runtime >= runtime_choice::kCount ||
        actions.runtime == mz.chosen)
        return;
    mz.chosen = actions.runtime;
    runtime_choice::Write(ExeDirectory(), mz.chosen);
    Log("menu: NR runtime %s from the next launch",
        mz.chosen == runtime_choice::kMochizuki ? "mochizuki" : "danielblnc");
}
