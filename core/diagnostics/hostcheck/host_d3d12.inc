// hostcheck's D3D12 host: the frame sits in an upload buffer and is copied onto the current back
// buffer before every present, one frame in flight.
#include <d3d12.h>
#include <dxgi1_4.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

int hostcheck::RunD3D12(const Image &img)
{
    using Microsoft::WRL::ComPtr;
    auto fail = [](const char *what) {
        std::fprintf(stderr, "%s failed\n", what);
        return 3;
    };
    HWND wnd = MakeWindow(img.w, img.h, false);
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd {};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) ||
        FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
        return fail("device");
    DXGI_SWAP_CHAIN_DESC1 sd {};
    sd.Width = img.w;
    sd.Height = img.h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc1;
    ComPtr<IDXGISwapChain3> sc;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), wnd, &sd, nullptr, nullptr, &sc1)) ||
        FAILED(sc1.As(&sc)))
        return fail("swapchain");

    ComPtr<ID3D12Resource> back0;
    sc->GetBuffer(0, IID_PPV_ARGS(&back0));
    const D3D12_RESOURCE_DESC bd = back0->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 bytes = 0;
    dev->GetCopyableFootprints(&bd, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ud {};
    ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ud.Width = bytes;
    ud.Height = ud.DepthOrArraySize = ud.MipLevels = 1;
    ud.SampleDesc.Count = 1;
    ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    unsigned char *mapped = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ud,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&upload))) ||
        FAILED(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped))))
        return fail("upload buffer");
    for (UINT y = 0; y < img.h; ++y)
        std::memcpy(mapped + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
                    &img.rgba[static_cast<size_t>(y) * img.w * 4], static_cast<size_t>(img.w) * 4);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                      IID_PPV_ARGS(&list))) ||
        FAILED(list->Close()) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return fail("command list");
    UINT64 serial = 0;
    back0.Reset();

    auto barrier = [&](ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    };
    return Loop([&] {
        ComPtr<ID3D12Resource> back;
        if (FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back))) ||
            FAILED(alloc->Reset()) || FAILED(list->Reset(alloc.Get(), nullptr)))
            return false;
        barrier(back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to {}, from {};
        to.pResource = back.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = upload.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = fp;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        if (FAILED(list->Close()))
            return false;
        ID3D12CommandList *lists[] { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(sc->Present(1, 0)) || FAILED(queue->Signal(fence.Get(), ++serial)) ||
            FAILED(fence->SetEventOnCompletion(serial, done)))
            return false;
        return WaitForSingleObject(done, 10000) == WAIT_OBJECT_0;
    });
}
