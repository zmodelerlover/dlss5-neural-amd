// RUNG 3: can a HIP kernel of ours read AND write the very same VRAM a D3D12 resource
// lives in, with no round trip through system RAM?
//
// D3D12 writes the input (upload heap -> CopyBufferRegion into a SHARED DEFAULT-heap buffer).
// HIP imports that buffer's NT handle and runs v[i] = v[i]*3 + i over it.
// D3D12 copies the result out (READBACK heap) and the CPU asserts the exact bytes.
// If the transformed values arrive through the D3D12 side, it is one allocation.
//
// Ordering is done on the GPU: a SHARED ID3D12Fence imported as a HIP external semaphore.
// No hipDeviceSynchronize between the D3D12 write and our kernel, none between our kernel
// and the D3D12 readback. (--cpusync forces the CPU-sync fallback instead, for comparison.)
//
// argv: --break   skip the kernel launch (teeth check: must FAIL)
//       --cpusync use hipDeviceSynchronize + CPU fence wait instead of external semaphores
//       --copy    also time the copy path (hipMemcpy staging) for a 1920x1080 RGBA16F frame
// exit 0 = claim holds. ponytail: no RAII, process exit frees it.
#include <hip/hip_runtime.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <chrono>

#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")

static int fails = 0;
#define CK(c) do{ hipError_t e=(c); printf("  %-58s -> %d %s\n",#c,(int)e,hipGetErrorName(e)); if(e!=hipSuccess)fails++; }while(0)
#define CKQ(c) do{ hipError_t e=(c); if(e!=hipSuccess){ printf("  %-58s -> %d %s\n",#c,(int)e,hipGetErrorName(e)); fails++; } }while(0)
#define HR(c) do{ HRESULT r=(c); if(FAILED(r)){ printf("  %-58s -> 0x%08lx FAILED\n",#c,(unsigned long)r); return 2;} }while(0)

// the only thing a kernel can do that a copy cannot: transform in place, in VRAM.
__global__ void transform(unsigned *p, unsigned n)
{
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) p[i] = p[i] * 3u + i;
}

// writes pixel (x,y) = {x, y, 0xBEEF, 0x1234} as raw 16-bit lanes through a surface object.
__global__ void surfstamp(hipSurfaceObject_t surf, unsigned w, unsigned h)
{
    unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    ushort4 v = make_ushort4((unsigned short)x, (unsigned short)y, 0xBEEF, 0x1234);
    surf2Dwrite<ushort4>(v, surf, (int)(x * 8), (int)y);   // x is in bytes
}

int main(int argc, char **argv)
{
    bool breakIt = false, cpuSync = false, doCopy = false, doTex = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--break"))   breakIt = true;
        if (!strcmp(argv[i], "--cpusync")) cpuSync = true;
        if (!strcmp(argv[i], "--copy"))    doCopy  = true;
        if (!strcmp(argv[i], "--tex"))     doTex   = true;
    }

    // ---- 1. pick the D3D12 adapter, then find the HIP device by LUID the way neural.cpp does.
    // neural.cpp InitHip(): LoadLibraryExW(amdhip64_7.dll), hipGetDevicePropertiesR0600 into an
    // 8192-byte blob, memcmp(blob+272, &device->GetAdapterLuid(), 8).
    printf("[1] adapter + LUID match (same logic as neural.cpp InitHip)\n");
    IDXGIFactory4 *fac = nullptr; HR(CreateDXGIFactory1(IID_PPV_ARGS(&fac)));
    IDXGIAdapter1 *ad = nullptr; ID3D12Device *dev = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 dd{}; ad->GetDesc1(&dd);
        if (dd.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { ad->Release(); ad = nullptr; continue; }
        printf("  adapter %u vendor=0x%04x vram=%zu MiB\n", i, dd.VendorId, (size_t)(dd.DedicatedVideoMemory >> 20));
        break;
    }
    if (!ad) { printf("FAIL: no hardware DXGI adapter\n"); return 1; }
    HR(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
    const LUID luid = dev->GetAdapterLuid();
    printf("  d3d12 device LUID = %08lx%08lx\n", (unsigned long)luid.HighPart, (unsigned long)luid.LowPart);

    HMODULE hip = LoadLibraryExW(L"amdhip64_7.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!hip) { printf("FAIL: amdhip64_7.dll not loadable (%lu)\n", GetLastError()); return 1; }
    auto count = reinterpret_cast<int (*)(int *)>(GetProcAddress(hip, "hipGetDeviceCount"));
    auto props = reinterpret_cast<int (*)(void *, int)>(GetProcAddress(hip, "hipGetDevicePropertiesR0600"));
    if (!count || !props) { printf("FAIL: R0600 API unavailable\n"); return 1; }
    int n = 0; if (count(&n) != 0 || n == 0) { printf("FAIL: no HIP devices\n"); return 1; }
    int hipDevice = -1;
    for (int i = 0; i < n; ++i) {
        alignas(16) std::vector<unsigned char> p(8192, 0);
        if (props(p.data(), i) == 0 && memcmp(p.data() + 272, &luid, 8) == 0) {
            hipDevice = i;
            printf("  HIP device %d '%s' matches the D3D12 LUID (blob+272)\n", i, (char *)p.data());
            break;
        }
    }
    if (hipDevice < 0) { printf("FAIL: no HIP device matches the D3D12 LUID\n"); return 1; }
    CK(hipSetDevice(hipDevice));

    // ---- 2. D3D12 side: shared buffer + shared fence
    printf("\n[2] D3D12 shared committed buffer + shared fence\n");
    const UINT ELEMS = 1024, N = ELEMS * 4;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = N; rd.Height = 1;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;   // required alongside HEAP_FLAG_SHARED
    ID3D12Resource *shared = nullptr;
    HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&shared)));
    HANDLE hBuf = nullptr;
    HR(dev->CreateSharedHandle(shared, nullptr, GENERIC_ALL, nullptr, &hBuf));
    D3D12_RESOURCE_ALLOCATION_INFO ai = dev->GetResourceAllocationInfo(0, 1, &rd);
    printf("  buffer handle=%p allocSize=%llu (buffer %u B)\n", hBuf, (unsigned long long)ai.SizeInBytes, N);

    ID3D12Fence *fence = nullptr;
    HR(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
    HANDLE hFence = nullptr;
    HR(dev->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &hFence));
    printf("  fence  handle=%p (D3D12_FENCE_FLAG_SHARED)\n", hFence);

    ID3D12CommandQueue *q = nullptr; D3D12_COMMAND_QUEUE_DESC qd{};
    HR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    ID3D12CommandAllocator *al = nullptr;
    HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)));
    ID3D12GraphicsCommandList *cl = nullptr;
    HR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl)));

    // ---- 3. import buffer (and fence, unless --cpusync) into HIP
    printf("\n[3] import into HIP\n");
    hipExternalMemory_t em = nullptr; hipExternalMemoryHandleDesc ed{};
    ed.type = hipExternalMemoryHandleTypeD3D12Resource;
    ed.handle.win32.handle = hBuf; ed.size = ai.SizeInBytes; ed.flags = hipExternalMemoryDedicated;
    CK(hipImportExternalMemory(&em, &ed));
    void *dptr = nullptr; hipExternalMemoryBufferDesc bd{}; bd.offset = 0; bd.size = N;
    CK(hipExternalMemoryGetMappedBuffer(&dptr, em, &bd));
    printf("  mapped device ptr = %p\n", dptr);
    if (!dptr || fails) { printf("FAIL: import failed\n"); return 1; }

    hipExternalSemaphore_t sem = nullptr;
    bool semOk = false;
    if (!cpuSync) {
        hipExternalSemaphoreHandleDesc sd{};
        sd.type = hipExternalSemaphoreHandleTypeD3D12Fence;
        sd.handle.win32.handle = hFence; sd.flags = 0;
        hipError_t e = hipImportExternalSemaphore(&sem, &sd);
        printf("  %-58s -> %d %s\n", "hipImportExternalSemaphore(D3D12Fence)", (int)e, hipGetErrorName(e));
        semOk = (e == hipSuccess && sem != nullptr);
        if (!semOk) printf("  -> falling back to CPU sync\n");
    }
    hipStream_t s = nullptr; CK(hipStreamCreateWithFlags(&s, hipStreamNonBlocking));

    // ---- 4. D3D12 writes the input
    printf("\n[4] D3D12 writes the input pattern (upload heap -> shared buffer)\n");
    D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC urd = rd; urd.Flags = D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *upload = nullptr;
    HR(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &urd,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    unsigned *um = nullptr; D3D12_RANGE none{0, 0};
    HR(upload->Map(0, &none, (void **)&um));
    for (unsigned i = 0; i < ELEMS; ++i) um[i] = 0x1000u + i * 7u;   // the input D3D12 owns
    upload->Unmap(0, nullptr);

    auto barrier = [&](ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = r; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = a; br.Transition.StateAfter = b; cl->ResourceBarrier(1, &br);
    };
    barrier(shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyBufferRegion(shared, 0, upload, 0, N);
    barrier(shared, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    HR(cl->Close());
    ID3D12CommandList *l1[] = {cl}; q->ExecuteCommandLists(1, l1);
    HR(q->Signal(fence, 1));                       // "input is in VRAM"
    printf("  wrote %u uints, queue->Signal(fence,1)\n", ELEMS);

    // ---- 5. our kernel, ordered against D3D12 on the GPU
    printf("\n[5] our HIP kernel: v[i] = v[i]*3 + i   (sync = %s)\n",
           semOk ? "GPU, imported D3D12 fence" : "CPU, hipDeviceSynchronize + fence event");
    if (semOk) {
        hipExternalSemaphoreWaitParams wp{}; wp.params.fence.value = 1;
        CKQ(hipWaitExternalSemaphoresAsync(&sem, &wp, 1, s));       // wait for the D3D12 write
        if (!breakIt) transform<<<(ELEMS + 255) / 256, 256, 0, s>>>((unsigned *)dptr, ELEMS);
        hipExternalSemaphoreSignalParams sp{}; sp.params.fence.value = 2;
        CKQ(hipSignalExternalSemaphoresAsync(&sem, &sp, 1, s));     // tell D3D12 we are done
        printf("  hipWait(1) -> kernel%s -> hipSignal(2), no host sync\n", breakIt ? " SKIPPED" : "");
    } else {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        HR(fence->SetEventOnCompletion(1, ev));
        if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) { printf("FAIL: d3d12 fence 1 timeout\n"); return 1; }
        if (!breakIt) transform<<<(ELEMS + 255) / 256, 256, 0, s>>>((unsigned *)dptr, ELEMS);
        CKQ(hipStreamSynchronize(s)); CKQ(hipDeviceSynchronize());
        printf("  CPU waited fence=1, kernel%s, hipDeviceSynchronize\n", breakIt ? " SKIPPED" : "");
    }

    // ---- 6. D3D12 reads it back
    printf("\n[6] D3D12 reads it back (readback heap), CPU asserts\n");
    D3D12_HEAP_PROPERTIES rbp{}; rbp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC brd = rd; brd.Flags = D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *back = nullptr;
    HR(dev->CreateCommittedResource(&rbp, D3D12_HEAP_FLAG_NONE, &brd,
                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&back)));
    HR(al->Reset()); HR(cl->Reset(al, nullptr));
    barrier(shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyBufferRegion(back, 0, shared, 0, N);
    barrier(shared, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    HR(cl->Close());
    if (semOk) HR(q->Wait(fence, 2));              // GPU-side wait on the HIP signal
    ID3D12CommandList *l2[] = {cl}; q->ExecuteCommandLists(1, l2);
    HR(q->Signal(fence, 3));
    HANDLE ev2 = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HR(fence->SetEventOnCompletion(3, ev2));
    if (WaitForSingleObject(ev2, 10000) != WAIT_OBJECT_0) {
        printf("FAIL: readback fence timeout -- HIP never signalled the imported fence\n");
        return 1;
    }
    unsigned *mp = nullptr; D3D12_RANGE all{0, (SIZE_T)N};
    HR(back->Map(0, &all, (void **)&mp));
    printf("  in[0..3]  = %08x %08x %08x %08x\n", 0x1000u, 0x1000u + 7, 0x1000u + 14, 0x1000u + 21);
    printf("  got[0..3] = %08x %08x %08x %08x\n", mp[0], mp[1], mp[2], mp[3]);
    unsigned e0 = (0x1000u) * 3, e1 = (0x1000u + 7) * 3 + 1, e2 = (0x1000u + 14) * 3 + 2, e3 = (0x1000u + 21) * 3 + 3;
    printf("  exp[0..3] = %08x %08x %08x %08x\n", e0, e1, e2, e3);
    for (unsigned i = 0; i < ELEMS; ++i) {
        unsigned want = (0x1000u + i * 7u) * 3u + i;
        if (mp[i] != want) { printf("  MISMATCH at %u: got %08x want %08x\n", i, mp[i], want); fails++; break; }
    }
    if (!fails) printf("  MATCH on all %u elements: our kernel transformed D3D12-owned VRAM in place\n", ELEMS);

    // ---- 7. the alternative, for the record: what a copy would cost per frame
    if (doCopy) {
        printf("\n[7] copy path cost, 1920x1080 RGBA16F = %u bytes\n", 1920u * 1080u * 8u);
        const size_t FB = 1920ull * 1080ull * 8ull;
        void *d = nullptr, *pinned = nullptr;
        CKQ(hipMalloc(&d, FB)); CKQ(hipHostMalloc(&pinned, FB, 0));
        void *paged = malloc(FB);
        auto bench = [&](const char *what, void *host, hipMemcpyKind k) {
            void *dst = (k == hipMemcpyDeviceToHost) ? host : d;
            const void *src = (k == hipMemcpyDeviceToHost) ? d : host;
            for (int i = 0; i < 3; ++i) hipMemcpy(dst, src, FB, k);
            hipDeviceSynchronize();
            auto t0 = std::chrono::high_resolution_clock::now();
            const int R = 50;
            for (int i = 0; i < R; ++i) hipMemcpy(dst, src, FB, k);
            hipDeviceSynchronize();
            double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::high_resolution_clock::now() - t0).count() / R;
            printf("  %-28s %7.3f ms  %6.2f GB/s\n", what, ms, FB / (ms * 1e6));
        };
        bench("D2H pinned",   pinned, hipMemcpyDeviceToHost);
        bench("H2D pinned",   pinned, hipMemcpyHostToDevice);
        bench("D2H pageable", paged,  hipMemcpyDeviceToHost);
        bench("H2D pageable", paged,  hipMemcpyHostToDevice);
        printf("  round trip (D2H+H2D) is the per-frame cost if we cannot import.\n");
    }

    // ---- 8. the add-on's real resources are RGBA16F TEXTURES, not buffers. Is a shared
    // texture addressable as linear memory from a kernel, or is it swizzled in VRAM?
    if (doTex) {
        printf("\n[8] shared D3D12 TEXTURE (RGBA16F 256x64) imported into HIP\n");
        const UINT TW = 256, TH = 64, RP = TW * 8, TBYTES = RP * TH;   // rowPitch 2048 = 256-aligned
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = TW; td.Height = TH;
        td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource *tex = nullptr;
        HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &td,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex)));
        HANDLE hTex = nullptr;
        HR(dev->CreateSharedHandle(tex, nullptr, GENERIC_ALL, nullptr, &hTex));
        D3D12_RESOURCE_ALLOCATION_INFO tai = dev->GetResourceAllocationInfo(0, 1, &td);
        printf("  texture allocSize=%llu, row-major bytes would be %u\n",
               (unsigned long long)tai.SizeInBytes, TBYTES);

        hipExternalMemory_t tem = nullptr; hipExternalMemoryHandleDesc ted{};
        ted.type = hipExternalMemoryHandleTypeD3D12Resource;
        ted.handle.win32.handle = hTex; ted.size = tai.SizeInBytes; ted.flags = hipExternalMemoryDedicated;
        hipError_t te = hipImportExternalMemory(&tem, &ted);
        printf("  hipImportExternalMemory(texture)                            -> %d %s\n", (int)te, hipGetErrorName(te));
        void *tp = nullptr; hipExternalMemoryBufferDesc tbd{}; tbd.offset = 0; tbd.size = tai.SizeInBytes;
        hipError_t tb = hipExternalMemoryGetMappedBuffer(&tp, tem, &tbd);
        printf("  hipExternalMemoryGetMappedBuffer(texture)                   -> %d %s  ptr=%p\n",
               (int)tb, hipGetErrorName(tb), tp);
        hipMipmappedArray_t arr = nullptr; hipExternalMemoryMipmappedArrayDesc mad{};
        mad.offset = 0; mad.numLevels = 1; mad.flags = 0;
        mad.extent = make_hipExtent(TW, TH, 0);
        mad.formatDesc = hipCreateChannelDesc(16, 16, 16, 16, hipChannelFormatKindFloat);
        hipError_t ma = hipExternalMemoryGetMappedMipmappedArray(&arr, tem, &mad);
        printf("  hipExternalMemoryGetMappedMipmappedArray(texture)           -> %d %s\n", (int)ma, hipGetErrorName(ma));

        if (tb == hipSuccess && tp) {
            // kernel writes a linear ramp; D3D12 copies the texture out row-major.
            // ramp survives => the kernel addresses the texture linearly. It does not => swizzled.
            transform<<<1, 1, 0, s>>>((unsigned *)tp, 0);   // no-op, keeps the stream alive
            hipMemsetD32(reinterpret_cast<hipDeviceptr_t>(tp), 0, TBYTES / 4);
            CKQ(hipDeviceSynchronize());
            std::vector<unsigned> ramp(TBYTES / 4);
            for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = 0xC0DE0000u + (unsigned)i;
            CKQ(hipMemcpy(tp, ramp.data(), TBYTES, hipMemcpyHostToDevice));
            CKQ(hipDeviceSynchronize());

            ID3D12Resource *tback = nullptr;
            D3D12_RESOURCE_DESC bufd = rd; bufd.Width = TBYTES; bufd.Flags = D3D12_RESOURCE_FLAG_NONE;
            HR(dev->CreateCommittedResource(&rbp, D3D12_HEAP_FLAG_NONE, &bufd,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tback)));
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0;
            dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
            printf("  GetCopyableFootprints: rowPitch=%u totalBytes=%llu\n",
                   fp.Footprint.RowPitch, (unsigned long long)total);
            HR(al->Reset()); HR(cl->Reset(al, nullptr));
            barrier(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = tex;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = tback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            HR(cl->Close());
            ID3D12CommandList *l3[] = {cl}; q->ExecuteCommandLists(1, l3);
            HR(q->Signal(fence, 10));
            HANDLE ev3 = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            HR(fence->SetEventOnCompletion(10, ev3));
            WaitForSingleObject(ev3, 10000);
            unsigned *tm = nullptr; D3D12_RANGE tr{0, (SIZE_T)TBYTES};
            HR(tback->Map(0, &tr, (void **)&tm));
            size_t bad = 0, firstBad = 0;
            for (size_t i = 0; i < TBYTES / 4; ++i)
                if (tm[i] != 0xC0DE0000u + (unsigned)i) { if (!bad) firstBad = i; ++bad; }
            printf("  linear-ramp survives the texture round trip? %s (%zu/%zu dwords differ)\n",
                   bad ? "NO" : "YES", bad, (size_t)(TBYTES / 4));
            printf("  first 4 dwords back: %08x %08x %08x %08x  (wrote %08x %08x %08x %08x)\n",
                   tm[0], tm[1], tm[2], tm[3], 0xC0DE0000u, 0xC0DE0001u, 0xC0DE0002u, 0xC0DE0003u);
            if (bad) printf("  first mismatch at dword %zu: %08x\n", firstBad, tm[firstBad]);
            printf("  => a shared TEXTURE %s be treated as linear memory by a kernel.\n",
                   bad ? "CANNOT" : "can");

            // the array route: does a surface object over the imported texture write where
            // D3D12 says pixel (x,y) lives? This is the only zero-copy path for a texture.
            if (ma == hipSuccess && arr) {
                hipArray_t lvl = nullptr;
                hipError_t gl = hipGetMipmappedArrayLevel(&lvl, arr, 0);
                hipResourceDesc res{}; res.resType = hipResourceTypeArray; res.res.array.array = lvl;
                hipSurfaceObject_t so = 0;
                hipError_t cs = hipCreateSurfaceObject(&so, &res);
                printf("  hipGetMipmappedArrayLevel -> %d %s ; hipCreateSurfaceObject -> %d %s\n",
                       (int)gl, hipGetErrorName(gl), (int)cs, hipGetErrorName(cs));
                if (gl == hipSuccess && cs == hipSuccess) {
                    surfstamp<<<dim3(TW / 32, TH), dim3(32, 1), 0, s>>>(so, TW, TH);
                    hipError_t ke = hipDeviceSynchronize();
                    printf("  surf2Dwrite kernel -> %d %s\n", (int)ke, hipGetErrorName(ke));
                    HR(al->Reset()); HR(cl->Reset(al, nullptr));
                    barrier(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    barrier(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                    HR(cl->Close());
                    ID3D12CommandList *l4[] = {cl}; q->ExecuteCommandLists(1, l4);
                    HR(q->Signal(fence, 11));
                    HANDLE ev4 = CreateEventA(nullptr, FALSE, FALSE, nullptr);
                    HR(fence->SetEventOnCompletion(11, ev4));
                    WaitForSingleObject(ev4, 10000);
                    tback->Unmap(0, nullptr);                    // re-map so the read range is fresh
                    unsigned *tm2 = nullptr;
                    HR(tback->Map(0, &tr, (void **)&tm2));
                    // control: does the BEEF pattern exist ANYWHERE in the imported allocation?
                    std::vector<unsigned short> lin(TBYTES / 2);
                    hipMemcpy(lin.data(), tp, TBYTES, hipMemcpyDeviceToHost);
                    size_t beefLin = 0, beefTex = 0;
                    size_t rampLin = 0, zeroLin = 0;
                    for (size_t i = 0; i < lin.size(); ++i) { if (lin[i] == 0xBEEF) ++beefLin; if ((lin[i] & 0xFF00) == 0xC000 || lin[i] == 0xC0DE) ++rampLin; if (!lin[i]) ++zeroLin; }
                    printf("  control: of %zu 16-bit lanes, %zu are zero, %zu still carry the old ramp\n",
                           lin.size(), zeroLin, rampLin);
                    for (size_t i = 0; i < TBYTES / 2; ++i) if (((unsigned short *)tm2)[i] == 0xBEEF) ++beefTex;
                    printf("  control: 0xBEEF lanes found -> %zu via the linear HIP pointer, "
                           "%zu via the D3D12 texture copy (expect %u each if the write landed)\n",
                           beefLin, beefTex, TW * TH);
                    unsigned short *sm = (unsigned short *)tm2;
                    size_t sbad = 0, sfirst = 0;
                    for (unsigned y = 0; y < TH; ++y)
                        for (unsigned x = 0; x < TW; ++x) {
                            unsigned short *px = sm + (size_t)y * (RP / 2) + x * 4;
                            if (px[0] != x || px[1] != y || px[2] != 0xBEEF) {
                                if (!sbad) sfirst = (size_t)y * TW + x;
                                ++sbad;
                            }
                        }
                    printf("  surface writes land at D3D12's (x,y)? %s (%zu/%u pixels wrong)\n",
                           sbad ? "NO" : "YES", sbad, TW * TH);
                    printf("  pixel(0,0)=%04x %04x %04x  pixel(5,3)=%04x %04x %04x (want 0,0,beef / 5,3,beef)\n",
                           sm[0], sm[1], sm[2], sm[3 * (RP / 2) + 5 * 4], sm[3 * (RP / 2) + 5 * 4 + 1],
                           sm[3 * (RP / 2) + 5 * 4 + 2]);
                    if (sbad) printf("  first wrong pixel index %zu\n", sfirst);
                    printf("  => a shared TEXTURE %s be written through a HIP surface object.\n",
                           sbad ? "CANNOT" : "CAN");
                }
            }
        }
    }

    printf("\n==== %s (%d failures) ====\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
