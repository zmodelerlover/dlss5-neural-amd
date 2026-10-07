#pragma once
// Which game buffer each guide reads from -- depth, and motion -- and how a depth format is read.
// Shared by the 64-bit add-on's D3D11 route and the 32-bit bridge, which observe the same D3D11
// binds and must pick the same buffer. They used to be two copies kept in step by a test that
// compared their text; now there is one.
#include <d3d11.h>
#include <wrl/client.h>

#include <unordered_map>

namespace guides {

using Microsoft::WRL::ComPtr;

// True for the depth-stencil formats whose single-float alias can be read through an SRV.
inline DXGI_FORMAT GuideDepthSrvFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// Bind tallies for the current frame, one entry per resource. Most-bound wins, which is what
// separates the main scene pass from shadow maps, reflection probes and the dozens of
// downsample steps a modern engine also renders into -- the probe log for NFS lists 45 render
// targets in a frame, and the two that matter are bound two orders of magnitude more than the
// rest. Cleared every present.
// The value holds a reference. Keying on the raw pointer is only an identity test; keeping one
// is not safe, because a target the game releases between the bind and the present leaves a
// dangling pointer that the next QueryInterface walks into -- measured, as an access violation
// on the first frame. The description is read here too, while the resource is provably alive.
struct Tallied
{
    ComPtr<ID3D11Resource> res;
    UINT binds = 0;
    // Draws while it was the bound depth-stencil (d3d11guides::CountDraw), which rank first; binds
    // break a tie, and are all a motion target has. Issue #19: three same-size D32S8 buffers, and the
    // most-bound was not the one the scene was drawn into -- ReShade's own pick counts draws.
    UINT draws = 0;
    // A depth candidate the backbuffer's shape (ObserveD3D11, ScreenShaped). Only these rank by draws:
    // a shadow cascade or atlas (2048x2048, 1024x4096) passes the size floor and out-draws the scene.
    bool screenShaped = false;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

// No guide buffer worth having is smaller than this. It exists because the relative floors below
// are measured against a swapchain size that is zero until the effect has been switched on once --
// a Darksiders 3 log has "guide motion: taking 1x1 format 16, bound 2928 times this frame", taken
// in exactly that window, with CreateTexture2D failing on it the next line.
constexpr UINT kGuideFloor = 256;

// ReShade's aspect test: w/h within 10% of the backbuffer's, as |w*H - h*W| <= W*H/10. False while the
// backbuffer's size is not known yet.
inline bool ScreenShaped(UINT w, UINT h, UINT screenW, UINT screenH)
{
    const long long d = static_cast<long long>(w) * screenH - static_cast<long long>(h) * screenW;
    return screenW != 0 && screenH != 0 && (d < 0 ? -d : d) <= static_cast<long long>(screenW) * screenH / 10;
}

// Depth buffers the probe read FLAT twice in a moving scene, which SettleGuide passes over for the
// next candidate (D3D11Transport.inc): a set, as issue #19 had three of one size, so a third is tried
// rather than the first again. Identity only, never dereferenced; one set per process, as there is one
// depth guide, emptied when the route lets them back in.
inline const void *passedOver[4] {};
inline bool PassedOver(const void *p)
{
    for (const void *q : passedOver)
        if (p != nullptr && q == p)
            return true;
    return false;
}
inline void PassOver(const void *p)
{
    for (const void *&q : passedOver)
        if (q == nullptr)
            return void(q = p);
}

// A motion-vector target as an engine writes it: two float channels, no more, at something
// close to render resolution. The dozens of small two-channel buffers an engine also produces
// are excluded by the size floor rather than by name, because names are not available here.
inline bool LooksLikeMotion(const D3D11_TEXTURE2D_DESC &d, UINT screenW, UINT screenH)
{
    if (d.SampleDesc.Count != 1 || d.ArraySize != 1)
        return false;
    if (d.Format != DXGI_FORMAT_R16G16_FLOAT && d.Format != DXGI_FORMAT_R32G32_FLOAT &&
        d.Format != DXGI_FORMAT_R16G16_SNORM)
        return false;
    // An absolute floor first, because the relative one below is measured against a swapchain
    // size that is zero until the effect has been enabled once -- and "anything passes while the
    // size is unknown" is how a 1x1 buffer became the motion guide.
    if (d.Width < kGuideFloor || d.Height < kGuideFloor)
        return false;
    if (screenW == 0 || screenH == 0)
        return true;
    return d.Width * 2 >= screenW && d.Height * 2 >= screenH;
}

// Takes the frame's tallies and settles which resource each guide reads from. A guide that
// changes resource mid-run drops its snapshot and views, which Ensure rebuilds -- the same
// orphaned-view trap the depth path already paid for once. True when it took a buffer, which the
// caller hands to RearmGuideProbe: the probe's last reading was of another one.
//
// A template because the two routes' Guide structs are different types -- the add-on's also holds
// the D3D12 copy -- that share every member this reads and writes; and each route has its own log.
template <class Guide, class LogFn>
bool SettleGuide(Guide &guide, std::unordered_map<void *, Tallied> &tally, LogFn Log)
{
    // Screen-shaped depth candidates rank by draws, binds breaking a tie; with none of that shape, and
    // for motion, which has no draws, most-bound wins as it always did.
    bool shaped = false;
    for (const auto &entry : tally)
        shaped = shaped || (entry.second.screenShaped && !PassedOver(entry.first));
    const Tallied *best = nullptr;
    for (const auto &entry : tally)
    {
        const Tallied &t = entry.second;
        if (PassedOver(entry.first) || (shaped && !t.screenShaped))
            continue;
        if (best == nullptr || (shaped ? t.draws > best->draws || (t.draws == best->draws && t.binds > best->binds)
                                       : t.binds > best->binds))
            best = &t;
    }
    if (best == nullptr)
    {
        tally.clear();
        return false;
    }
    if (guide.chosen.Get() == best->res.Get())
    {
        guide.chosenBinds = best->binds;
        guide.challenger = nullptr;
        guide.challengerFrames = 0;
        tally.clear();
        return false;
    }

    // Nothing chosen yet: add three presents up, then take the leader.
    //
    // The streak rule below exists to protect an incumbent, and with no incumbent there is
    // nothing to protect -- only the question of having seen enough. Asking one candidate to win
    // three presents *running* is a rule this case cannot always satisfy: an engine that rotates
    // two or three depth targets never presents the same one three times in a row, so the
    // challenger changed every present, the streak reset every present, and the guide was never
    // taken at all. No depth, in a game that has depth, for as long as it runs. Measured against
    // tools/guide_switch_check.py: a rotating pair leaves the slot empty after a hundred presents.
    //
    // So the tally is left standing rather than cleared, and three presents of binds add up
    // before the leader is taken. Rotating targets each keep their own share and one of them
    // wins; the scene pass still outbinds a shadow map by an order of magnitude; and a single odd
    // frame still cannot decide it alone.
    const bool cold = guide.chosen == nullptr;
    if (cold)
    {
        if (++guide.coldFrames < 3)
            return false;  // deliberately NOT cleared -- leaving it standing is what accumulates
        guide.coldFrames = 0;
    }
    // A challenger has to win more than one frame.
    //
    // The tally is cleared every present, so "most-bound" was decided by a single frame, and a
    // single frame is not always a representative one. The GTA San Andreas log has the depth
    // guide walk away from a buffer bound 96440 times to one bound *9 times*, on the frame the
    // game changed resolution and stopped drawing its scene pass: the incumbent simply was not
    // in that frame's tally, so a nine-bind buffer won by being the only thing there. Every frame
    // after that fed the network the wrong depth, and nothing demotes a chosen guide, so it never
    // recovered.
    //
    // Three frames is enough to outlast a resolution change, a loading screen or an alt-tab, and
    // short enough that a real switch costs nothing anyone can see.
    else if (guide.challenger != best->res.Get())
    {
        guide.challenger = best->res.Get();
        guide.challengerFrames = 1;
        tally.clear();
        return false;
    }
    else if (++guide.challengerFrames < 3)
    {
        tally.clear();
        return false;
    }
    guide.challenger = nullptr;
    guide.challengerFrames = 0;
    guide.chosenBinds = best->binds;
    guide.chosen = best->res;  // takes a reference; the tally's is about to go
    guide.width = best->width;
    guide.height = best->height;
    guide.format = best->format;
    guide.ready = false;
    guide.logged = false;
    guide.failed = false;
    // A new buffer is copied at present until the probe withholds that copy (d3d11_guides.h).
    guide.preClear = guide.snapFresh = guide.presentRetried = false;
    // Whatever the companion effect had here, the game has just outbid it with a buffer it
    // renders itself. That is not a guess at motion, so it wins.
    guide.external = false;
    Log("guide %s: taking %p, %ux%u format %u, bound %u times with %u draws %s", guide.name,
        static_cast<void *>(best->res.Get()), best->width, best->height,
        static_cast<unsigned>(best->format), best->binds, best->draws,
        cold ? "over the first three presents" : "a frame for three frames running");
    tally.clear();
    return true;
}

} // namespace guides
