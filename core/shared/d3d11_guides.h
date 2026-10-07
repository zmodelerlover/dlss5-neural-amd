#pragma once
// What the game's D3D11 device does for a frame before the frame crosses to the network: the
// private staging around the back buffer, the copies of the game's own depth and motion, and the
// CPU-side drain that makes those copies safe to read. Shared by the 64-bit add-on's D3D11 route
// and the 32-bit bridge, which do exactly this on the same kind of device. They used to be two
// copies, and the 32-bit one had drifted: it had no size floor on depth and neither of the gates
// the 64-bit observation has, so the two routes could settle on different buffers in one game.
//
// Whatever differs between the routes comes in as an argument: the device and context, the cached
// compute shader, the compiler (resolved by hand on the 64-bit side, linked on the 32-bit one), how
// a guide's shared texture is made, and the log. Nothing here may need more than D3D11 itself,
// because the 32-bit build is checked for the imports it must not have.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <reshade_api.hpp>

#include <atomic>
#include <unordered_map>

#include "guide_choice.h"
#include "../shaders/guide_depth.h"

namespace d3d11guides {

using Microsoft::WRL::ComPtr;
using TallyMap = std::unordered_map<void*, guides::Tallied>;

// Poll an event query until it lands, for 2 s at most: hanging here would be a black screen
// instead of an error. The query lands only once all the game's queued GPU work has, so a GPU-bound
// game waits here for several ms every frame. It yields for the first millisecond, then waits a
// millisecond at a time on a high-resolution timer. A Sleep(0) spin kept a core busy for the whole
// wait, one the game's own threads want, and Sleep(1) rounds up to the system timer tick, 15.6 ms
// unless the game asked for finer; it is only the fallback where that timer cannot be made (before
// Windows 10 1803). `poll` returns S_FALSE while the query is pending. The 32-bit bridge's D3D9
// drain polls through this too.
template <class Poll>
HRESULT PollQuery(Poll poll) {
    LARGE_INTEGER hz{}, start{}, now{}, due{};
    QueryPerformanceFrequency(&hz);
    QueryPerformanceCounter(&start);
    const ULONGLONG deadline = GetTickCount64() + 2000;
    due.QuadPart = -10000;  // 1 ms after it is set, in 100 ns units
    HANDLE timer = nullptr;
    HRESULT hr;
    while ((hr = poll()) == S_FALSE) {
        if (GetTickCount64() > deadline) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            break;
        }
        QueryPerformanceCounter(&now);
        if (now.QuadPart - start.QuadPart < hz.QuadPart / 1000) {
            Sleep(0);
            continue;
        }
        if (timer == nullptr)
            timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
        if (timer == nullptr || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) ||
            WaitForSingleObject(timer, 100) != WAIT_OBJECT_0)
            Sleep(1);
    }
    if (timer != nullptr)
        CloseHandle(timer);
    return hr;
}

// Submit everything recorded on the game's context and wait, on the CPU, for the GPU to finish
// it. An event query does not report until every command submitted before it has completed.
//
// This replaces the cross-device fences the bridge used to synchronise with. Those queued a
// GPU-side Wait on the game's immediate context, and everything the runtime submitted after it
// -- including ReShade's own overlay pass, which draws into the back buffer -- then sat pending
// behind it. DXGI refuses ResizeBuffers while a pending command references a back buffer, and
// NFS reports that refusal as a DirectX error and quits. Measured: the failure survived not
// touching the back buffer at all from this add-on, which is what ruled the copies out and left
// the queued wait as the only candidate.
//
// Bounded by PollQuery. The network takes about 16 ms, so this normally returns at once. A
// removed device never completes the query, so it is not waited out either.
inline bool FlushAndWait11(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (dev == nullptr || ctx == nullptr)
        return false;
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> done;
    if (FAILED(dev->CreateQuery(&qd, &done))) {
        ctx->Flush();
        return false;
    }
    ctx->End(done.Get());
    ctx->Flush();
    const HRESULT hr = PollQuery([&] {
        const HRESULT got = ctx->GetData(done.Get(), nullptr, 0, 0);
        return got == S_FALSE && FAILED(dev->GetDeviceRemovedReason()) ? E_FAIL : got;
    });
    return SUCCEEDED(hr) && SUCCEEDED(dev->GetDeviceRemovedReason());
}

// The two private textures that keep the back buffer away from anything shared. S is whichever
// struct holds them on the route: stageIn11, stageOut11, and the size and format they were made at.
template <class S, class LogFn>
bool EnsureStage(ID3D11Device* dev, S& s, UINT w, UINT h, DXGI_FORMAT fmt, LogFn Log) {
    if (s.stageIn11 != nullptr && s.stageW == w && s.stageH == h && s.stageFmt == fmt)
        return true;
    s.stageIn11.Reset();
    s.stageOut11.Reset();
    s.stageW = s.stageH = 0;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &s.stageIn11)) ||
        FAILED(dev->CreateTexture2D(&td, nullptr, &s.stageOut11))) {
        Log("bridge: could not create the private staging textures %ux%u fmt %u.", w, h,
            static_cast<unsigned>(fmt));
        s.stageIn11.Reset();
        s.stageOut11.Reset();
        return false;
    }
    s.stageW = w;
    s.stageH = h;
    s.stageFmt = fmt;
    Log("bridge: private staging %ux%u fmt %u, so the back buffer never meets a shared resource.",
        w, h, static_cast<unsigned>(fmt));
    return true;
}

// Draws since the last bind, credited by ObserveD3D11 to the depth-stencil that bind left bound: what
// the D3D11 depth pick ranks by (guide_choice.h, Tallied::draws). Per recording thread, as a deferred
// context binds and draws on the thread recording it (GTA V), so its draws go to its own bind and no
// add is shared between threads. Nothing is counted until a D3D11 bind has been observed: the draw
// events are registered on every API (Events bit 2, on by default), and elsewhere this is one load.
// ponytail: two deferred contexts recorded in turn on one thread share its count.
inline std::atomic<bool> countDraws{false};
inline thread_local UINT drawsSinceBind = 0;
inline thread_local void* boundDepth = nullptr;  // identity only, never dereferenced
inline void CountDraw() {
    if (countDraws.load(std::memory_order_relaxed))
        ++drawsSinceBind;
}

// D3D11 half of the observation. ReShade hands the render targets and the depth-stencil of
// every bind; on D3D12 it hands the add-on only the swapchain (measured: zero depth binds in
// 600 frames), which is why this path exists at all and why PCSX2 had to be moved to D3D11
// before it could show a depth buffer.
//
// Only what a bind looks like is decided here. Whether to look at all -- the GameGuides switch, and
// not while ReShade draws its own effect chain -- stays with each route, which keeps those two
// switches in its own place; both routes apply both.
inline void ObserveD3D11(reshade::api::device* dev, const reshade::api::resource_view* rtvs,
                         uint32_t count, reshade::api::resource depthRes, UINT screenW,
                         UINT screenH, TallyMap& depthTally, TallyMap& motionTally) {
    auto record = [](TallyMap& tally, ID3D11Resource* native, const D3D11_TEXTURE2D_DESC& d) -> guides::Tallied& {
        guides::Tallied& slot = tally[native];
        if (slot.res == nullptr) {
            slot.res = native;
            slot.width = d.Width;
            slot.height = d.Height;
            slot.format = d.Format;
        }
        ++slot.binds;
        return slot;
    };
    if (!countDraws.load(std::memory_order_relaxed))
        countDraws.store(true, std::memory_order_relaxed);
    if (const auto it = depthTally.find(boundDepth); it != depthTally.end())
        it->second.draws += drawsSinceBind;
    drawsSinceBind = 0;
    boundDepth = nullptr;
    if (depthRes.handle != 0) {
        auto* native = reinterpret_cast<ID3D11Resource*>(depthRes.handle);
        ComPtr<ID3D11Texture2D> tex;
        D3D11_TEXTURE2D_DESC d{};
        if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&tex)))) {
            tex->GetDesc(&d);
            // The same two floors LooksLikeMotion applies, for the same reason.
            if (d.SampleDesc.Count == 1 && d.ArraySize == 1 && d.Width >= guides::kGuideFloor &&
                d.Height >= guides::kGuideFloor &&
                guides::GuideDepthSrvFormat(d.Format) != DXGI_FORMAT_UNKNOWN &&
                (screenW == 0 || screenH == 0 ||
                 (d.Width * 2 >= screenW && d.Height * 2 >= screenH))) {
                record(depthTally, native, d).screenShaped =
                    guides::ScreenShaped(d.Width, d.Height, screenW, screenH);
                boundDepth = native;
            }
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (rtvs[i].handle == 0)
            continue;
        const reshade::api::resource res = dev->get_resource_from_view(rtvs[i]);
        if (res.handle == 0)
            continue;
        auto* native = reinterpret_cast<ID3D11Resource*>(res.handle);
        ComPtr<ID3D11Texture2D> tex;
        D3D11_TEXTURE2D_DESC d{};
        if (FAILED(native->QueryInterface(IID_PPV_ARGS(&tex))))
            continue;
        tex->GetDesc(&d);
        if (guides::LooksLikeMotion(d, screenW, screenH))
            record(motionTally, native, d);
    }
}

// One frame of one guide, on the game's own device and context. The game's buffer is live and
// keeps being rewritten, so the value has to be taken now and taken by copy.
//
// Motion is the cheap case: an engine's velocity buffer is already R16G16_FLOAT, which opens on
// a second device, so it is a single CopyResource straight into the shared texture. Depth is
// not -- R32G8X24_TYPELESS and R24G8_TYPELESS refuse to open across devices -- so it goes
// through a private snapshot, a compute pass that reads the single float channel, and a shared
// R32_FLOAT. Both of those steps are ported from session.cpp, where the transport is measured.
//
// ensure(w, h, format, uav) makes or keeps guide.bridge's shared texture: the 64-bit route opens
// it on its own device, the 32-bit one exports it to the helper.
template <class Guide, class EnsureFn, class LogFn>
bool PrepareGuide(ID3D11Device* dev, ID3D11DeviceContext* ctx, Guide& guide, bool isDepth,
                  ComPtr<ID3D11ComputeShader>& cs, bool& csFailed, pD3DCompile compile,
                  EnsureFn ensure, LogFn Log) {
    if (guide.failed || guide.chosen == nullptr || dev == nullptr || ctx == nullptr)
        return false;
    const UINT w = guide.width, h = guide.height;
    if (w == 0 || h == 0)
        return false;

    if (!isDepth) {
        if (!ensure(w, h, guide.format, false)) {
            guide.failed = true;
            Log("guide %s: format %u will not share between the devices; giving up on it.",
                guide.name, static_cast<unsigned>(guide.format));
            return false;
        }
        ctx->CopyResource(guide.bridge.on11.Get(), guide.chosen.Get());
        guide.ready = true;
        if (!guide.logged) {
            guide.logged = true;
            Log("guide %s: %ux%u format %u crossing to the network device, one copy per frame.",
                guide.name, w, h, static_cast<unsigned>(guide.format));
        }
        return true;
    }

    if (cs == nullptr) {
        if (csFailed)
            return false;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(compile(shaders::kGuideDepthCs, sizeof(shaders::kGuideDepthCs) - 1,
                           "guide-depth", nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob,
                           &err)) ||
            FAILED(dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                            nullptr, &cs))) {
            csFailed = true;
            Log("guide depth: compute shader failed: %s",
                err != nullptr ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
    }

    // The snapshot is never bound as a depth-stencil, so an SRV over it stays valid while the
    // game goes on binding and clearing the original.
    if (guide.snap == nullptr || guide.snapW != w || guide.snapH != h ||
        guide.snapFmt != guide.format) {
        guide.srv.Reset();
        guide.srvOf = nullptr;
        guide.snap.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = guide.format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        // BIND_DEPTH_STENCIL as well, for a depth format, so this is the same kind of resource as
        // the one about to be copied into it. A depth-stencil surface is planar and compressed;
        // the same typeless format without the flag is neither, and CopyResource between the two
        // is a copy across layouts. The D3D12 half of this add-on made exactly this mistake with
        // its pre-clear snapshot and every probe of the result came back min -3e38, max 2e36,
        // mean NaN -- garbage that was then handed to the network as depth. This is that same
        // copy, on the D3D11 bridge, and Darksiders 3 takes it: R24G8_TYPELESS at 2560x1440.
        //
        // Kept to depth formats: a motion guide is an ordinary two-channel colour target and the
        // flag would only make its creation fail.
        const bool depthFormat = guides::GuideDepthSrvFormat(guide.format) != DXGI_FORMAT_UNKNOWN;
        td.BindFlags = depthFormat ? (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL)
                                   : D3D11_BIND_SHADER_RESOURCE;
        HRESULT made = dev->CreateTexture2D(&td, nullptr, &guide.snap);
        if (FAILED(made) && depthFormat) {
            // Some formats reach here that no driver will give a depth-stencil view of. Falling
            // back leaves the old behaviour rather than losing the guide outright, and says so,
            // because a copy on this path is then the suspect for anything odd downstream.
            Log("guide depth: %ux%u format %u was refused as a depth-stencil copy (0x%08lX); "
                "falling back to a plain shader-resource copy, which may not read correctly.",
                w, h, static_cast<unsigned>(guide.format), static_cast<unsigned long>(made));
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            made = dev->CreateTexture2D(&td, nullptr, &guide.snap);
        }
        if (FAILED(made)) {
            guide.failed = true;
            Log("guide depth: private %ux%u copy of format %u could not be created.", w, h,
                static_cast<unsigned>(guide.format));
            return false;
        }
        guide.snapW = w;
        guide.snapH = h;
        guide.snapFmt = guide.format;
    }
    // The frame's depth is copied now, unless SnapshotDepthBeforeClear already copied it just
    // before the game's last clear of the buffer; a frame that cleared nothing is copied now.
    const bool beforeClear = guide.snapFresh;
    guide.snapFresh = false;
    if (!beforeClear)
        ctx->CopyResource(guide.snap.Get(), guide.chosen.Get());

    if (!ensure(w, h, DXGI_FORMAT_R32_FLOAT, true)) {
        guide.failed = true;
        return false;
    }
    // Ensure builds a new texture on a resize, and a view left over from the old one writes into
    // an orphan while every read of the new one comes back zero. Rebuild whenever the resource
    // underneath changed, not just when the view is null.
    if (guide.uavOf != guide.bridge.on11.Get()) {
        guide.uav.Reset();
        guide.uavOf = nullptr;
    }
    if (guide.uav == nullptr) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        const HRESULT hr = dev->CreateUnorderedAccessView(guide.bridge.on11.Get(), &ud, &guide.uav);
        if (FAILED(hr)) {
            guide.failed = true;
            // A lost device refuses every view; its reason is what tells a GPU hang from a bad desc.
            Log("guide depth: UAV over the shared texture failed (0x%08lX, device removed reason 0x%08lX).",
                static_cast<unsigned long>(hr), static_cast<unsigned long>(dev->GetDeviceRemovedReason()));
            return false;
        }
        guide.uavOf = guide.bridge.on11.Get();
    }
    if (guide.srvOf != guide.snap.Get()) {
        guide.srv.Reset();
        guide.srvOf = nullptr;
    }
    if (guide.srv == nullptr) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = guides::GuideDepthSrvFormat(guide.format);
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(guide.snap.Get(), &sd, &guide.srv))) {
            guide.failed = true;
            Log("guide depth: SRV over the snapshot failed (fmt %u read as %u).",
                static_cast<unsigned>(guide.format), static_cast<unsigned>(sd.Format));
            return false;
        }
        guide.srvOf = guide.snap.Get();
    }

    // Save exactly the three compute bindings this touches and put them back: the context
    // belongs to the game and it is mid-frame.
    ID3D11ComputeShader* oldCs = nullptr;
    ID3D11ShaderResourceView* oldSrv = nullptr;
    ID3D11UnorderedAccessView* oldUav = nullptr;
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &oldSrv);
    ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);

    UINT keep = static_cast<UINT>(-1);
    ID3D11ShaderResourceView* srv = guide.srv.Get();
    ID3D11UnorderedAccessView* uav = guide.uav.Get();
    ctx->CSSetShader(cs.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
    ctx->CSSetShaderResources(0, 1, &nullSrv);
    ctx->CSSetShader(oldCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &oldSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
    if (oldCs != nullptr)
        oldCs->Release();
    if (oldSrv != nullptr)
        oldSrv->Release();
    if (oldUav != nullptr)
        oldUav->Release();

    guide.ready = true;
    // Once switched, said when the first copy taken before a clear is used: none means no clear of
    // the buffer is seen, and every frame is still copied at present.
    if (!guide.logged && (beforeClear || !guide.preClear)) {
        guide.logged = true;
        Log("guide depth: %ux%u format %u -> shared R32_FLOAT, one snapshot and one dispatch per "
            "frame, the snapshot taken %s. PS2 depth tops out near 0.002; a modern engine's fills "
            "the range.",
            w, h, static_cast<unsigned>(guide.format),
            beforeClear ? "just before the game's last clear" : "at present");
    }
    return true;
}

// The probe withheld the depth this guide copied at present -- JUNK, or FLAT in a moving scene: a
// game that clears its depth before Present (Tomb Raider on the 32-bit bridge) leaves a constant
// plane there. The copy is then taken just before the game's clears instead, and probed again. At
// first only on trial: an engine that clears at the start of a frame hands over the previous
// frame's depth there, where the copy at present is this frame's, and a menu that moves is withheld
// just the same before the scene arrives in that buffer. So the first reading that varies before a
// clear (`varied`: the probe's last reading of fed depth did) goes back to the copy at present,
// once, and the probe looks again; withheld there a second time, the copy before the clears stays
// until the guide moves to another buffer (SettleGuide). A copy before a clear that reads no better
// is withheld as before, not swapped back. True on either switch, for the caller to re-probe.
template <class Guide, class LogFn>
bool FallBackToPreClear(Guide& guide, bool withheld, bool varied, LogFn Log) {
    if (guide.external || guide.chosen == nullptr)
        return false;
    if (guide.preClear && varied && !guide.presentRetried) {
        guide.preClear = guide.snapFresh = false;
        guide.presentRetried = true;
        guide.logged = false;  // PrepareGuide says the copy at present is in use again
        Log("guide depth: reads right as copied just before the game clears it, so copied at "
            "present again, once, as that is this frame's if the game clears at the start of one, "
            "and probed again.");
        return true;
    }
    if (!withheld || guide.preClear)
        return false;
    guide.preClear = true;
    guide.logged = false;  // PrepareGuide says when the first copy before a clear is in use
    Log("guide depth: withheld as copied at present, so it is copied just before the game clears "
        "it%s, and probed again.",
        guide.presentRetried ? " until the guide takes another buffer" : "");
    return true;
}

// Just before the game clears a depth-stencil (the 64-bit route's OnDepthCleared, the 32-bit
// bridge's own clear event): once FallBackToPreClear switched the guide, copy its buffer while the
// frame's depth is still in it, on the clearing context, deferred or not, so the copy lands ahead
// of the clear. The last clear before a present wins. ponytail: a game that clears the scene depth
// again for its HUD and draws into it hands over the HUD's, which the probe withholds as before.
template <class Guide>
void SnapshotDepthBeforeClear(reshade::api::command_list* cmd, reshade::api::resource_view dsv,
                              ID3D11Device* game, Guide& guide) {
    // A snapshot of another size or format than the buffer is PrepareGuide's to rebuild first.
    if (!guide.preClear || guide.external || guide.failed || guide.snap == nullptr ||
        guide.snapW != guide.width || guide.snapH != guide.height ||
        guide.snapFmt != guide.format || cmd == nullptr || dsv.handle == 0 || game == nullptr)
        return;
    reshade::api::device* dev = cmd->get_device();
    if (dev == nullptr || reinterpret_cast<ID3D11Device*>(dev->get_native()) != game ||
        reinterpret_cast<ID3D11Resource*>(dev->get_resource_from_view(dsv).handle) !=
            guide.chosen.Get())
        return;
    reinterpret_cast<ID3D11DeviceContext*>(cmd->get_native())
        ->CopyResource(guide.snap.Get(), guide.chosen.Get());
    guide.snapFresh = true;
}

} // namespace d3d11guides
