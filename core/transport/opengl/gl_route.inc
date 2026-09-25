// The OpenGL route: the Vulkan route's shape, with the host's half rebuilt around a framebuffer
// blit, because in OpenGL there is no back buffer to copy.
//
// What is the same, and is the same for the same reason: the network runs on our own private
// D3D12 device (CreateWorkDevice), so the shared textures are created THERE, exported as NT
// handles and imported into the host -- never the other way round. A handle exported from OpenGL
// is not something D3D12 can open.
//
//   D3D12 CreateCommittedResource(D3D12_HEAP_FLAG_SHARED) -> CreateSharedHandle
//     -> glCreateMemoryObjectsEXT, GL_DEDICATED_MEMORY_OBJECT_EXT = 1
//     -> glImportMemoryWin32HandleEXT(GL_HANDLE_TYPE_D3D12_RESOURCE_EXT)
//     -> glTexStorageMem2DEXT, then an FBO wrapping that texture
//
// What is different, measured under Luanti with the glinfo add-on rather than assumed:
//
//   * ReShade hands this route a back buffer whose handle is GL_FRAMEBUFFER_DEFAULT with the name
//     GL_BACK, and whose resource_type is `surface`, not texture_2d. There is no texture to copy,
//     so the crossing is glBlitFramebuffer between FBO 0 and an FBO of ours. Every copy in this
//     route is a blit for that reason.
//   * There is no command buffer. ReShade reports an immediate command list whose native handle
//     is a synthetic value; GL commands go straight into the current context on the calling
//     thread. So the route issues its own GL calls and puts back every piece of state it touched.
//   * The context can be replaced under us. Under SDL -- Luanti, and a great many OpenGL games --
//     contexts are created and destroyed during start-up, and ReShade loads and unloads its
//     add-ons once per context: seven times in 230 ms, measured. Every GL object here belongs to
//     one context and dies with it, so the route keys everything on wglGetCurrentContext() and
//     rebuilds when that changes.
//
// The three rules glprobe paid for, which are not in any specification:
//
//   1. The shared NT handle is NOT ours to close. Vulkan takes its own reference and lets the
//      application close the handle it exported; EXT_memory_object_win32 is written the same way
//      and this driver does not honour it. Closing a handle after the import faults inside the
//      ICD, from a driver thread, some time later -- so the crash lands in an unrelated GL call
//      and looks like anything but its cause. Measured: 3 runs in 8 died with closing, 0 in 8
//      without. Handles therefore go into a list and stay open for the life of the process. A
//      resize leaks two kernel handles, deliberately.
//   2. The memory object must be declared dedicated BEFORE the import (D3D12 resource handles
//      back exactly one resource), the same rule the Vulkan route meets as
//      VkMemoryDedicatedAllocateInfo.
//   3. GL_HANDLE_TYPE_OPAQUE_WIN32_EXT is ACCEPTED for a D3D12 resource handle on this driver and
//      is the wrong type. "The import succeeded" proves nothing about the handle type, so this
//      route names D3D12_RESOURCE and stands down if that is refused rather than falling back.
//
// Channel order, which is the one thing here that would silently look like a bug in the network:
// the crossing is created as R8G8B8A8_UNORM even though ReShade reports the GL back buffer as
// B8G8R8A8_UNORM. That report is ReShade's own description of the default framebuffer, not a
// statement about memory. The blit writes our texture through GL's view of it, which is RGBA8 --
// byte 0 is red -- and D3D12 has to read the same bytes the same way round. Creating the crossing
// as BGRA would put blue where the network expects red, with no error anywhere and a picture that
// looks plausible until you notice the sky is orange.
//
// Synchronisation is the CPU stall the D3D11 route has run ten thousand frames on: glFinish, then
// hand over; wait for our own queue, then blit back. glprobe proved that the imported D3D12
// fences work here as GL semaphores -- glWaitSemaphoreEXT on an imported fence completed -- so a
// GPU-side handover is available and is the obvious next step. It is not the first version: this
// route has enough new surface without also being the first one in the project not to stall.

namespace glroute
{

#include "gl_api.inc"
#include "gl_crossing.inc"

// The GL side of a teardown. Called from ReleaseSwapchainSized, which may run on a thread with no
// context at all -- a swapchain can be destroyed from anywhere -- so it checks before touching a
// single GL object. What it cannot delete it forgets: those objects die with their context.
void ReleaseSwapchainSized()
{
    Route &r = g_route;
    r.ready = false;

    SelfIssued selfIssued;
    const bool contextAlive = r.api.coreLoaded && r.context != nullptr &&
                              r.api.getCurrentContext() == r.context;
    if (r.api.coreLoaded && !contextAlive && (r.in.texture != 0 || r.out.texture != 0))
        Log("opengl: releasing without the route's context current; the GL objects are left to "
            "die with it. This is not a leak of anything the driver still tracks.");
    if (contextAlive)
        r.api.finish();
    r.in.ReleaseGl(r.api, contextAlive);
    r.out.ReleaseGl(r.api, contextAlive);
    if (r.resolveFbo != 0 || r.resolveTexture != 0)
    {
        if (contextAlive)
        {
            if (r.resolveFbo != 0)
                r.api.deleteFramebuffers(1, &r.resolveFbo);
            if (r.resolveTexture != 0)
                r.api.deleteTextures(1, &r.resolveTexture);
            r.api.Drain();
        }
        r.resolveFbo = 0;
        r.resolveTexture = 0;
    }
    if (r.in.handle != nullptr)
    {
        r.keptHandles.push_back(r.in.handle);
        r.in.handle = nullptr;
    }
    if (r.out.handle != nullptr)
    {
        r.keptHandles.push_back(r.out.handle);
        r.out.handle = nullptr;
    }
    r.in.on12.Reset();
    r.out.on12.Reset();
    r.width = 0;
    r.height = 0;
    r.loggedReady = false;
    r.loggedFirstFrame = false;
}

// ---- the imported fences ----------------------------------------------------------------------
//
// A D3D12 fence is a 64-bit monotonic counter and EXT_semaphore_win32 carries the value to wait
// for as a parameter on the semaphore, which is how a counter fits through an interface built for
// binary semaphores. Two of them, one per direction, exactly as the Vulkan route imports them:
// crossHandle is D3D12 signalling the host, backHandle is the host signalling D3D12.
//
// These replace two CPU stalls a frame -- a glFinish and a fence wait -- with two queue-side
// waits. The route does not trust them on the strength of the specification: GL's first signal is
// seen on the CPU before the queue is made to wait on it, and if it does not land, the route falls
// back to stalling and says so. A wait that never completes is a hung GPU and a game that has to be
// killed, which is not a failure mode to discover in someone else's session.
bool ImportFences(Route &r)
{
    if (r.api.importSemaphoreWin32 == nullptr || r.api.genSemaphores == nullptr ||
        r.api.semaphoreParameterui64v == nullptr || r.api.waitSemaphore == nullptr ||
        r.api.signalSemaphore == nullptr)
        return false;
    if (g.bridge.crossHandle == nullptr || g.bridge.backHandle == nullptr)
        return false;

    GLuint pair[2] { 0, 0 };
    r.api.Drain();
    r.api.genSemaphores(2, pair);
    if (!r.api.Clean() || pair[0] == 0 || pair[1] == 0)
        return false;
    r.api.importSemaphoreWin32(pair[0], GL_HANDLE_TYPE_D3D12_FENCE_EXT_, g.bridge.crossHandle);
    const bool first = r.api.Clean();
    r.api.importSemaphoreWin32(pair[1], GL_HANDLE_TYPE_D3D12_FENCE_EXT_, g.bridge.backHandle);
    const bool second = r.api.Clean();
    if (!first || !second)
    {
        if (r.api.deleteSemaphores != nullptr)
            r.api.deleteSemaphores(2, pair);
        r.api.Drain();
        return false;
    }
    r.semToGl = pair[0];
    r.semToD3D = pair[1];
    // The fences carry what the D3D11 route or an old context signalled, some maybe in flight;
    // never step below either, so every wait below is for a value that has not been reached yet.
    r.toGlValue = std::max(r.toGlValue, g.bridge.crossFence->GetCompletedValue());
    r.toD3DValue = std::max(r.toD3DValue, g.bridge.backFence->GetCompletedValue());
    return true;
}

// The default framebuffer's sample count, read from GL. ReShade reports the back buffer as one
// sample whatever the window actually has, so this is the only honest source. A multisampled
// default framebuffer needs one more step on the way in, because a blit that resolves samples and
// mirrors Y in the same call is not something every driver accepts.
void ReadDefaultSamples(Route &r)
{
    SavedState saved {};
    Save(r.api, saved);
    r.api.bindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    r.api.Drain();
    GLint samples = 1;
    r.api.getIntegerv(GL_SAMPLES_, &samples);
    if (!r.api.Clean() || samples < 1)
        samples = 1;
    r.defaultSamples = samples;
    Restore(r.api, saved);
}

// An ordinary single-sample texture the resolve lands in before the flip. Only built when the
// default framebuffer is multisampled.
bool BuildResolveTarget(Route &r, UINT w, UINT h)
{
    if (r.api.texStorage2D == nullptr)
    {
        Log("opengl: the default framebuffer is multisampled and this context has no "
            "glTexStorage2D, so there is nowhere to resolve it to.");
        return false;
    }
    if (r.resolveFbo != 0)
        r.api.deleteFramebuffers(1, &r.resolveFbo);
    if (r.resolveTexture != 0)
        r.api.deleteTextures(1, &r.resolveTexture);
    r.resolveFbo = 0;
    r.resolveTexture = 0;
    r.api.Drain();

    r.api.genTextures(1, &r.resolveTexture);
    r.api.bindTexture(GL_TEXTURE_2D_, r.resolveTexture);
    r.api.texStorage2D(GL_TEXTURE_2D_, 1, kCrossingGlFormat, static_cast<GLsizei>(w),
                       static_cast<GLsizei>(h));
    r.api.bindTexture(GL_TEXTURE_2D_, 0);
    if (!r.api.Clean())
    {
        Log("opengl: could not create the resolve texture.");
        return false;
    }
    r.api.genFramebuffers(1, &r.resolveFbo);
    r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, r.resolveFbo);
    r.api.framebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_,
                               r.resolveTexture, 0);
    const GLenum status = r.api.checkFramebufferStatus(GL_DRAW_FRAMEBUFFER_);
    r.api.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    r.api.Drain();
    if (status != GL_FRAMEBUFFER_COMPLETE_)
    {
        Log("opengl: the resolve FBO is incomplete (status 0x%X).", status);
        return false;
    }
    Log("opengl: the default framebuffer has %d samples; resolving through an intermediate "
        "texture on the way in.", r.defaultSamples);
    return true;
}

// ---- everything that happens once, plus the resize path --------------------------------------

bool Ensure(Route &r, UINT w, UINT h)
{
    if (r.failed)
        return false;

    HGLRC current = r.api.getCurrentContext();
    if (current == nullptr)
    {
        if (!r.loggedNoContext)
        {
            r.loggedNoContext = true;
            Log("opengl: present arrived on a thread with no current GL context. Nothing can be "
                "done from here, so these frames are left alone.");
        }
        return false;
    }
    if (current != r.context)
    {
        // A new context owns none of the old one's objects, and the old one may already be gone.
        // Forget rather than delete, and rebuild against what is current now.
        if (r.context != nullptr && !r.loggedContextChange)
        {
            r.loggedContextChange = true;
            Log("opengl: the host replaced its GL context. Rebuilding the crossing against the "
                "new one; the old objects died with it.");
        }
        // The D3D12 halves and the handles are left to BuildCrossing, after our queue is drained.
        r.in.ReleaseGl(r.api, false);
        r.out.ReleaseGl(r.api, false);
        r.resolveFbo = r.resolveTexture = r.semToGl = r.semToD3D = 0;  // never delete these here
        // The fences are imported into this context again and proven again before a queue waits.
        r.semaphores = r.semaphoresProven = r.loggedSemaphores = false;
        r.context = current;
        r.ready = false;
        r.width = 0;
        r.height = 0;
        if (!r.api.LoadForContext())
        {
            r.Stop("this GL context has no external-memory entry points. The driver advertises "
                   "GL_EXT_memory_object_win32 or it does not, and after context creation that "
                   "cannot be changed.");
            return false;
        }
        r.luidKnown = false;
    }

    // Which GPU is this context on? EXT_memory_object reports the LUID, which is the identifier
    // D3D12 uses, so the two can be matched exactly rather than by adapter name -- and this
    // machine has two adapters reporting the same name.
    if (!r.luidKnown && r.api.getUnsignedBytev != nullptr)
    {
        GLubyte bytes[8] {};
        r.api.Drain();
        r.api.getUnsignedBytev(GL_DEVICE_LUID_EXT_, bytes);
        if (r.api.Clean())
        {
            std::memcpy(&r.luid, bytes, sizeof(r.luid));
            r.luidKnown = true;
        }
    }

    if (g.bridge.workDevice == nullptr)
    {
        if (!r.luidKnown)
        {
            r.Stop("the GL context reports no device LUID, so there is no way to prove the work "
                   "device would land on the same GPU.");
            return false;
        }
        ComPtr<IDXGIAdapter> adapter;
        if (!AdapterForLuid(r.luid, adapter))
        {
            r.Stop("no DXGI adapter carries this GL context's LUID. OpenGL and D3D12 would be "
                   "different GPUs, and nothing can be shared between them.");
            return false;
        }
        if (!CreateWorkDevice(adapter.Get(), r.luid))
        {
            r.Stop("the work D3D12 device did not come up.");
            return false;
        }
        g.device = g.bridge.workDevice;
        g.queue = g.bridge.workQueue;
        // The completion fence this route waits on before blitting the result back. The D3D11
        // route creates it inside its own frame function, so a route that does not go through
        // there has to create it here -- and Signal on a null fence is not a wait, it is nothing.
        if (FAILED(g.bridge.workDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
        {
            r.Stop("could not create the completion fence.");
            return false;
        }
    }

    if (!r.semaphores && !r.loggedSemaphores)
    {
        SelfIssued selfIssued;
        r.semaphores = g.settings.glSemaphores.load() && ImportFences(r);
        r.loggedSemaphores = true;
        Log("opengl: imported fences %s. %s", r.semaphores ? "in use"
                                                 : g.settings.glSemaphores.load() ? "unavailable"
                                                                         : "off by GlSemaphores=0",
            r.semaphores ? "The hand-over between the two APIs runs on the GPU; GL's first "
                           "signal is still confirmed on the CPU before the stall is dropped."
                         : "Falling back to the CPU stall -- glFinish and a fence wait -- which "
                           "is what the D3D11 route has always used.");
    }

    const bool built = r.in.texture != 0 && r.in.on12 != nullptr && r.out.texture != 0 &&
                       r.out.on12 != nullptr && g.bridge.crossLocal != nullptr;
    if (r.ready && r.width == w && r.height == h && built)
        return true;

    if (r.ready && r.width == w && r.height == h)
        Log("opengl: the route was ready but a resource is gone; rebuilding it.");

    r.ready = false;
    // Nothing below may be released while our queue can still be reading it, and with the fences
    // in use no present waits for the queue. A new result texture also holds no result yet. A wait
    // that gave up has stood the add-on down, and what the queue may read stays as it is.
    if (!WaitForWorkQueue(g.completion))
        return false;
    r.haveResult = false;
    {
        SelfIssued selfIssued;
        ReadDefaultSamples(r);
    }
    if (!BuildCrossing(r, r.in, w, h, "colour-in") || !BuildCrossing(r, r.out, w, h, "result-out"))
    {
        r.Stop("the shared textures would not cross.");
        return false;
    }
    if (r.defaultSamples > 1)
    {
        SelfIssued selfIssued;
        if (!BuildResolveTarget(r, w, h))
        {
            r.Stop("the multisampled default framebuffer has nowhere to resolve to.");
            return false;
        }
    }

    // Our private copy on our own device, so the network never reads shared memory directly --
    // the same reason the D3D11 route keeps crossLocal.
    g.bridge.crossLocal.Reset();
    if (!CreateTexture(w, h, kCrossingFormat, g.bridge.crossLocal, "crossLocal",
                       D3D12_RESOURCE_STATE_COPY_DEST))
    {
        r.Stop("could not create the private copy of the frame.");
        return false;
    }

    r.width = w;
    r.height = h;
    r.ready = true;
    if (!r.loggedReady)
    {
        r.loggedReady = true;
        Log("opengl: bridge up at %ux%u, default framebuffer %d sample(s)%s. The frame crosses "
            "by blit between the default framebuffer and a D3D12 texture imported into this "
            "context.", w, h, r.defaultSamples,
            r.defaultSamples > 1 ? ", resolved on the way in" : "");
    }
    return true;
}

#include "gl_frame.inc"

}  // namespace glroute
