// vkbridge -- the D3D12 <-> Vulkan transport, proved by round-tripping known bytes.
//
// vkprobe answered "will this driver allow it". This answers the next one: does the data
// actually survive the crossing, in both directions, with the synchronisation holding. It is
// the same transport the add-on's Vulkan route needs, so it is written here first, where a
// failure prints a diff instead of hanging a game.
//
// The shape, which is the shape every working DLSS 5 Vulkan add-on uses:
//
//   * the textures and the two fences are created on the D3D12 side (D3D12_HEAP_FLAG_SHARED,
//     D3D12_FENCE_FLAG_SHARED) and exported as NT handles
//   * Vulkan imports them -- VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT for the image,
//     VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT for the fences, which become timeline
//     semaphores because a D3D12 fence is a monotonic counter and that is what a timeline is
//   * two fences, not one: a D3D12 fence can be signalled from one side and waited on the
//     other, so one carries D3D12 -> Vulkan and the other Vulkan -> D3D12
//
// The layout rule is the part that is easy to get wrong and silent when wrong: the imported
// image is moved out of VK_IMAGE_LAYOUT_UNDEFINED exactly once, before anything writes it, and
// stays in VK_IMAGE_LAYOUT_GENERAL forever after. A transition out of UNDEFINED is allowed to
// discard the contents, so doing it per frame would throw away whatever D3D12 had just written
// and the symptom would be an empty image with no error anywhere.
//
// Build:  .\build.ps1 -Target vkbridge -Exe      Run:  .\build\dlss5-vkbridge.exe

#include "../vkshared/vk_raw.inc"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace vkraw;

namespace
{

int g_failures = 0;

void Fail(const char *what, long code = 0)
{
    ++g_failures;
    if (code != 0)
        std::printf("  FAIL: %s (0x%08lX)\n", what, static_cast<unsigned long>(code));
    else
        std::printf("  FAIL: %s\n", what);
}

struct Case
{
    const char *name;
    DXGI_FORMAT dxgi;
    int vk;
    UINT bytesPerPixel;
};

// 256 wide keeps every row pitch a multiple of D3D12's 256-byte copy alignment, so the readback
// footprint is tightly packed and matches Vulkan's default (bufferRowLength 0 = width). One
// less thing between a byte mismatch and its cause.
constexpr UINT kWidth = 256, kHeight = 64;
constexpr Case kCases[] {
    { "R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 4 },
    { "R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8 },
    { "R32_FLOAT", DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, 4 },
    { "R16G16_FLOAT", DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT, 4 },
};

// Two different fills so a stale buffer cannot pass as a successful copy: if direction 2 read
// back direction 1's bytes, the compare has to notice.
void FillPattern(unsigned char *dst, size_t bytes, unsigned seed)
{
    for (size_t i = 0; i < bytes; ++i)
        dst[i] = static_cast<unsigned char>((i * 31u + seed * 97u + (i >> 8)) & 0xFF);
}

size_t FirstDifference(const unsigned char *a, const unsigned char *b, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i)
        if (a[i] != b[i])
            return i;
    return bytes;
}

}  // namespace

int main()
{
    std::printf("vkbridge -- round-tripping bytes between D3D12 and Vulkan\n");

    Api vk {};
    if (!vk.LoadCore())
    {
        std::printf("vulkan-1.dll or its core entry points are missing.\n");
        return 2;
    }

    // --- Vulkan instance and the physical device, first, because its LUID chooses the adapter --
    VkApplicationInfo app {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vkbridge";
    app.apiVersion = (1u << 22) | (2u << 12);  // 1.2: timeline semaphores are core there

    VkInstanceCreateInfo ici {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance instance = nullptr;
    if (vk.createInstance(&ici, nullptr, &instance) != 0)
    {
        std::printf("vkCreateInstance(1.2) failed.\n");
        return 2;
    }

    uint32_t gpuCount = 0;
    vk.enumeratePhysicalDevices(instance, &gpuCount, nullptr);
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    if (gpuCount != 0)
        vk.enumeratePhysicalDevices(instance, &gpuCount, gpus.data());
    if (gpuCount == 0)
    {
        std::printf("no Vulkan physical devices.\n");
        return 2;
    }

    VkPhysicalDevice gpu = gpus[0];
    VkPhysicalDeviceIDProperties ids {};
    ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 props {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &ids;
    vk.getPhysicalDeviceProperties2(gpu, &props);
    const char *gpuName = reinterpret_cast<const char *>(props.properties + kDeviceNameOffset);
    LUID vkLuid {};
    std::memcpy(&vkLuid, ids.deviceLUID, sizeof(vkLuid));
    std::printf("Vulkan device: %s   LUID %08lX%08lX   (valid=%u)\n", gpuName,
                static_cast<unsigned long>(vkLuid.HighPart),
                static_cast<unsigned long>(vkLuid.LowPart), ids.deviceLUIDValid);

    // --- the D3D12 device, on the adapter that IS that Vulkan device --------------------------
    // Not "an AMD adapter": three DXGI adapters on this machine report the same name and
    // different LUIDs, so picking by name or index would land on a different device and every
    // import would fail with nothing to explain it.
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        std::printf("no DXGI factory.\n");
        return 2;
    }
    ComPtr<IDXGIAdapter1> adapter, chosen;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart == vkLuid.LowPart &&
            desc.AdapterLuid.HighPart == vkLuid.HighPart)
        {
            chosen = adapter;
            std::printf("D3D12 adapter: %ls  (LUID matched)\n", desc.Description);
        }
        adapter.Reset();
    }
    if (chosen == nullptr)
    {
        std::printf("no DXGI adapter has the Vulkan device's LUID. They are different GPUs and\n"
                    "nothing can be shared between them.\n");
        return 2;
    }

    ComPtr<ID3D12Device> dev12;
    if (FAILED(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12))))
    {
        std::printf("D3D12CreateDevice failed on that adapter.\n");
        return 2;
    }

    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12CommandAllocator> alloc12;
    ComPtr<ID3D12GraphicsCommandList> list12;
    if (FAILED(dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue12))) ||
        FAILED(dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&alloc12))) ||
        FAILED(dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc12.Get(), nullptr,
                                        IID_PPV_ARGS(&list12))))
    {
        std::printf("D3D12 queue/allocator/list creation failed.\n");
        return 2;
    }
    list12->Close();

    // --- the two shared fences ----------------------------------------------------------------
    // toVk: D3D12 signals it, Vulkan waits on it. toD3D: the other way. A D3D12 fence is a
    // 64-bit monotonic counter and so is a Vulkan timeline semaphore, which is why the two map
    // onto each other with no translation.
    ComPtr<ID3D12Fence> fenceToVk, fenceToD3D;
    HANDLE hFenceToVk = nullptr, hFenceToD3D = nullptr;
    if (FAILED(dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fenceToVk))) ||
        FAILED(dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fenceToD3D))) ||
        FAILED(dev12->CreateSharedHandle(fenceToVk.Get(), nullptr, GENERIC_ALL, nullptr,
                                         &hFenceToVk)) ||
        FAILED(dev12->CreateSharedHandle(fenceToD3D.Get(), nullptr, GENERIC_ALL, nullptr,
                                         &hFenceToD3D)))
    {
        std::printf("shared fence creation or export failed.\n");
        return 2;
    }

    // --- our own VkDevice, created the way the hook will have to create the host's ------------
    uint32_t famCount = 0;
    vk.getQueueFamilyProperties(gpu, &famCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(famCount);
    vk.getQueueFamilyProperties(gpu, &famCount, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < famCount; ++i)
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            family = i;
            break;
        }
    if (family == UINT32_MAX)
    {
        std::printf("no graphics queue family.\n");
        return 2;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline {};
    timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timeline.timelineSemaphore = 1;

    VkDeviceCreateInfo dci {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &timeline;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(std::size(kInteropExtensions));
    dci.ppEnabledExtensionNames = kInteropExtensions;

    VkDevice device = nullptr;
    const int madeDevice = vk.createDevice(gpu, &dci, nullptr, &device);
    if (madeDevice != 0)
    {
        std::printf("vkCreateDevice failed (VkResult %d) with the interop extensions.\n",
                    madeDevice);
        return 2;
    }
    if (!vk.LoadDevice(device))
    {
        std::printf("device entry points missing -- vkImportSemaphoreWin32HandleKHR in\n"
                    "particular. That is what a device created without the interop extensions\n"
                    "looks like.\n");
        return 2;
    }
    VkQueue queue = nullptr;
    vk.getDeviceQueue(device, family, 0, &queue);

    VkPhysicalDeviceMemoryProperties memProps {};
    vk.getMemoryProperties(gpu, &memProps);

    // --- import the two fences as timeline semaphores ------------------------------------------
    auto importFence = [&](HANDLE handle, VkSemaphore &out, const char *what) {
        VkSemaphoreTypeCreateInfo type {};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;
        VkSemaphoreCreateInfo sci {};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sci.pNext = &type;
        if (vk.createSemaphore(device, &sci, nullptr, &out) != 0)
        {
            Fail("vkCreateSemaphore (timeline)");
            return false;
        }
        VkImportSemaphoreWin32HandleInfoKHR imp {};
        imp.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
        imp.semaphore = out;
        imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        imp.handle = handle;
        const int r = vk.importSemaphoreWin32(device, &imp);
        std::printf("  %s: D3D12 fence -> timeline semaphore %s\n", what,
                    r == 0 ? "imported" : "REFUSED");
        if (r != 0)
            Fail("vkImportSemaphoreWin32HandleKHR");
        return r == 0;
    };

    std::printf("fences:\n");
    VkSemaphore semToVk = nullptr, semToD3D = nullptr;
    if (!importFence(hFenceToVk, semToVk, "D3D12 -> Vulkan") ||
        !importFence(hFenceToD3D, semToD3D, "Vulkan -> D3D12"))
        return 1;

    // One command buffer, reset and reused; the harness never has two submissions in flight.
    VkCommandPoolCreateInfo pci {};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    VkCommandPool pool = nullptr;
    if (vk.createCommandPool(device, &pci, nullptr, &pool) != 0)
    {
        Fail("vkCreateCommandPool");
        return 1;
    }
    VkCommandBufferAllocateInfo cbai {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = nullptr;
    if (vk.allocateCommandBuffers(device, &cbai, &cmd) != 0)
    {
        Fail("vkAllocateCommandBuffers");
        return 1;
    }

    // Submit `cmd` waiting for `waitValue` on `waitSem` and signalling `signalValue` on
    // `signalSem`; either side may be null. This is the whole synchronisation surface.
    auto submit = [&](VkSemaphore waitSem, uint64_t waitValue, VkSemaphore signalSem,
                      uint64_t signalValue) {
        const VkFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo tl {};
        tl.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        tl.waitSemaphoreValueCount = waitSem != nullptr ? 1u : 0u;
        tl.pWaitSemaphoreValues = &waitValue;
        tl.signalSemaphoreValueCount = signalSem != nullptr ? 1u : 0u;
        tl.pSignalSemaphoreValues = &signalValue;

        VkSubmitInfo si {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.pNext = &tl;
        si.waitSemaphoreCount = tl.waitSemaphoreValueCount;
        si.pWaitSemaphores = &waitSem;
        si.pWaitDstStageMask = &stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = tl.signalSemaphoreValueCount;
        si.pSignalSemaphores = &signalSem;
        return vk.queueSubmit(queue, 1, &si, nullptr);
    };

    auto beginCmd = [&]() {
        VkCommandBufferBeginInfo bi {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.beginCommandBuffer(cmd, &bi);
    };

    // --- the fence values advance across the whole run, not per case, so a case that quietly
    // --- did nothing cannot be masked by the next one starting from zero again.
    uint64_t toVkValue = 0, toD3DValue = 0;
    HANDLE cpuEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    for (const Case &c : kCases)
    {
        std::printf("\n%s  %ux%u  %u bytes/pixel\n", c.name, kWidth, kHeight, c.bytesPerPixel);
        const size_t bytes = static_cast<size_t>(kWidth) * kHeight * c.bytesPerPixel;
        const UINT rowPitch = kWidth * c.bytesPerPixel;  // already 256-aligned by construction

        // ---- the shared texture, created on D3D12 and exported ----
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = kWidth;
        rd.Height = kHeight;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = c.dxgi;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ComPtr<ID3D12Resource> shared;
        HRESULT hr = dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&shared));
        if (FAILED(hr))
        {
            Fail("CreateCommittedResource(SHARED)", hr);
            continue;
        }
        HANDLE hTexture = nullptr;
        hr = dev12->CreateSharedHandle(shared.Get(), nullptr, GENERIC_ALL, nullptr, &hTexture);
        if (FAILED(hr))
        {
            Fail("CreateSharedHandle(texture)", hr);
            continue;
        }

        // ---- import it as a VkImage ----
        VkExternalMemoryImageCreateInfo emi {};
        emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

        VkImageCreateInfo iciImg {};
        iciImg.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iciImg.pNext = &emi;
        iciImg.imageType = VK_IMAGE_TYPE_2D;
        iciImg.format = c.vk;
        iciImg.extent = { kWidth, kHeight, 1 };
        iciImg.mipLevels = 1;
        iciImg.arrayLayers = 1;
        iciImg.samples = VK_SAMPLE_COUNT_1_BIT;
        iciImg.tiling = VK_IMAGE_TILING_OPTIMAL;
        iciImg.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT;
        iciImg.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        iciImg.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage image = nullptr;
        if (vk.createImage(device, &iciImg, nullptr, &image) != 0)
        {
            Fail("vkCreateImage (external)");
            CloseHandle(hTexture);
            continue;
        }

        VkMemoryRequirements mr {};
        vk.getImageMemoryRequirements(device, image, &mr);
        const uint32_t typeIndex =
            PickMemoryType(memProps, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (typeIndex == UINT32_MAX)
        {
            Fail("no device-local memory type accepts this image");
            CloseHandle(hTexture);
            continue;
        }

        // The driver reported DEDICATED_ONLY for these formats, so the allocation names the
        // image it backs. Without this the allocation is refused and the message says nothing
        // about dedication.
        VkMemoryDedicatedAllocateInfo ded {};
        ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        ded.image = image;
        VkImportMemoryWin32HandleInfoKHR imp {};
        imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
        imp.pNext = &ded;
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        imp.handle = hTexture;

        VkMemoryAllocateInfo mai {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext = &imp;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = typeIndex;

        VkDeviceMemory memory = nullptr;
        const int allocated = vk.allocateMemory(device, &mai, nullptr, &memory);
        if (allocated != 0 || vk.bindImageMemory(device, image, memory, 0) != 0)
        {
            Fail("importing the D3D12 texture into Vulkan");
            std::printf("        vkAllocateMemory -> %d\n", allocated);
            CloseHandle(hTexture);
            continue;
        }
        std::printf("  texture: D3D12 resource -> VkImage imported (%llu bytes, memory type %u)\n",
                    static_cast<unsigned long long>(mr.size), typeIndex);

        // A staging buffer on the Vulkan side, host visible, for both reading and writing.
        VkBufferCreateInfo bci {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer staging = nullptr;
        VkDeviceMemory stagingMem = nullptr;
        void *stagingPtr = nullptr;
        if (vk.createBuffer(device, &bci, nullptr, &staging) != 0)
        {
            Fail("vkCreateBuffer (staging)");
            continue;
        }
        VkMemoryRequirements bmr {};
        vk.getBufferMemoryRequirements(device, staging, &bmr);
        VkMemoryAllocateInfo bmai {};
        bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bmai.allocationSize = bmr.size;
        bmai.memoryTypeIndex =
            PickMemoryType(memProps, bmr.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (bmai.memoryTypeIndex == UINT32_MAX ||
            vk.allocateMemory(device, &bmai, nullptr, &stagingMem) != 0 ||
            vk.bindBufferMemory(device, staging, stagingMem, 0) != 0 ||
            vk.mapMemory(device, stagingMem, 0, bytes, 0, &stagingPtr) != 0)
        {
            Fail("staging buffer allocation");
            continue;
        }

        // ---- the one and only transition out of UNDEFINED, before anything holds data -------
        beginCmd();
        VkImageMemoryBarrier toGeneral {};
        toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toGeneral.srcAccessMask = 0;
        toGeneral.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED_VALUE;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED_VALUE;
        toGeneral.image = image;
        toGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vk.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &toGeneral);
        vk.endCommandBuffer(cmd);
        if (submit(nullptr, 0, nullptr, 0) != 0)
        {
            Fail("submit of the initial layout transition");
            continue;
        }
        vk.queueWaitIdle(queue);

        // =========================== direction 1: D3D12 -> Vulkan ===========================
        std::vector<unsigned char> sent(bytes), got(bytes);
        FillPattern(sent.data(), bytes, 1);

        ComPtr<ID3D12Resource> upload;
        D3D12_HEAP_PROPERTIES up {};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev12->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&upload))))
        {
            Fail("upload buffer");
            continue;
        }
        void *uploadPtr = nullptr;
        upload->Map(0, nullptr, &uploadPtr);
        std::memcpy(uploadPtr, sent.data(), bytes);
        upload->Unmap(0, nullptr);

        alloc12->Reset();
        list12->Reset(alloc12.Get(), nullptr);
        D3D12_RESOURCE_BARRIER toCopyDest {};
        toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopyDest.Transition.pResource = shared.Get();
        toCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list12->ResourceBarrier(1, &toCopyDest);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = shared.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = { c.dxgi, kWidth, kHeight, 1, rowPitch };
        list12->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        // Back to COMMON before handing over: COMMON is the state a shared resource has to be in
        // for the other API to touch it.
        std::swap(toCopyDest.Transition.StateBefore, toCopyDest.Transition.StateAfter);
        list12->ResourceBarrier(1, &toCopyDest);
        list12->Close();
        ID3D12CommandList *lists[] { list12.Get() };
        queue12->ExecuteCommandLists(1, lists);
        queue12->Signal(fenceToVk.Get(), ++toVkValue);

        // Vulkan waits for exactly that value, then reads the image out to the staging buffer.
        beginCmd();
        VkBufferImageCopy region {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { kWidth, kHeight, 1 };
        vk.cmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, staging, 1, &region);
        vk.endCommandBuffer(cmd);
        if (submit(semToVk, toVkValue, semToD3D, ++toD3DValue) != 0)
        {
            Fail("submit waiting on the D3D12 fence");
            continue;
        }
        if (vk.queueWaitIdle(queue) != 0)
        {
            Fail("vkQueueWaitIdle -- the wait on the imported fence never completed");
            continue;
        }
        std::memcpy(got.data(), stagingPtr, bytes);
        size_t diff = FirstDifference(sent.data(), got.data(), bytes);
        if (diff == bytes)
            std::printf("  D3D12 -> Vulkan: %zu bytes identical\n", bytes);
        else
        {
            Fail("D3D12 -> Vulkan bytes differ");
            std::printf("        first difference at %zu: sent %02X, got %02X\n", diff,
                        sent[diff], got[diff]);
        }

        // =========================== direction 2: Vulkan -> D3D12 ===========================
        std::vector<unsigned char> sentBack(bytes), gotBack(bytes);
        FillPattern(sentBack.data(), bytes, 2);
        std::memcpy(stagingPtr, sentBack.data(), bytes);

        beginCmd();
        vk.cmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        vk.endCommandBuffer(cmd);
        if (submit(nullptr, 0, semToD3D, ++toD3DValue) != 0)
        {
            Fail("submit of the Vulkan write");
            continue;
        }

        ComPtr<ID3D12Resource> readback;
        D3D12_HEAP_PROPERTIES rb {};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(dev12->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&readback))))
        {
            Fail("readback buffer");
            continue;
        }

        // The GPU-side wait: D3D12 blocks its own queue on the fence Vulkan is signalling. This
        // is the direction that would silently read stale bytes if the import were not real.
        queue12->Wait(fenceToD3D.Get(), toD3DValue);
        alloc12->Reset();
        list12->Reset(alloc12.Get(), nullptr);
        D3D12_RESOURCE_BARRIER toCopySrc = toCopyDest;
        toCopySrc.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toCopySrc.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list12->ResourceBarrier(1, &toCopySrc);
        D3D12_TEXTURE_COPY_LOCATION rdst {}, rsrc {};
        rdst.pResource = readback.Get();
        rdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rdst.PlacedFootprint.Footprint = { c.dxgi, kWidth, kHeight, 1, rowPitch };
        rsrc.pResource = shared.Get();
        rsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list12->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, nullptr);
        std::swap(toCopySrc.Transition.StateBefore, toCopySrc.Transition.StateAfter);
        list12->ResourceBarrier(1, &toCopySrc);
        list12->Close();
        queue12->ExecuteCommandLists(1, lists);
        queue12->Signal(fenceToVk.Get(), ++toVkValue);

        // CPU-side wait, so the mapping below sees a finished copy.
        fenceToVk->SetEventOnCompletion(toVkValue, cpuEvent);
        if (WaitForSingleObject(cpuEvent, 5000) != WAIT_OBJECT_0)
        {
            Fail("D3D12 never reached the fence value -- its wait on the Vulkan signal is stuck");
            continue;
        }
        void *readPtr = nullptr;
        readback->Map(0, nullptr, &readPtr);
        std::memcpy(gotBack.data(), readPtr, bytes);
        readback->Unmap(0, nullptr);

        diff = FirstDifference(sentBack.data(), gotBack.data(), bytes);
        if (diff == bytes)
            std::printf("  Vulkan -> D3D12: %zu bytes identical\n", bytes);
        else
        {
            Fail("Vulkan -> D3D12 bytes differ");
            std::printf("        first difference at %zu: sent %02X, got %02X\n", diff,
                        sentBack[diff], gotBack[diff]);
        }

        vk.unmapMemory(device, stagingMem);
        vk.destroyBuffer(device, staging, nullptr);
        vk.freeMemory(device, stagingMem, nullptr);
        vk.destroyImage(device, image, nullptr);
        vk.freeMemory(device, memory, nullptr);
        CloseHandle(hTexture);
    }

    vk.deviceWaitIdle(device);
    std::printf("\n%s\n", g_failures == 0
                              ? "Transport works. Both directions, every format the add-on "
                                "carries, bytes identical."
                              : "Transport is NOT working. See the FAIL lines above.");
    return g_failures == 0 ? 0 : 1;
}
