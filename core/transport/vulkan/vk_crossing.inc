// The Vulkan route's state and its shared textures. Included by vk_route.inc, in namespace vkroute.

// ReShade's format enum carries DXGI's numbering, so the DXGI side of this is a cast. The Vulkan
// side is not: only the formats a swapchain actually appears in are here, and an unknown one is
// reported rather than guessed, because guessing produces a copy that silently reorders channels.
int VkFormatFor(DXGI_FORMAT typedOrTypeless)
{
    // Through ColourReadFormat first. RPCS3's swapchain arrives from ReShade as DXGI 90, which is
    // B8G8R8A8_TYPELESS -- a real answer, not a broken one, and there is no typeless anything in
    // Vulkan to map it to. The table below wants the concrete format, and resolving a typeless to
    // its UNORM member is a thing this add-on already knows how to do everywhere else.
    const DXGI_FORMAT f = ColourReadFormat(typedOrTypeless);
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:      return VK_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 43;  // VK_FORMAT_R8G8B8A8_SRGB
    case DXGI_FORMAT_B8G8R8A8_UNORM:      return VK_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 50;  // VK_FORMAT_B8G8R8A8_SRGB
    case DXGI_FORMAT_R16G16B16A16_FLOAT:  return VK_FORMAT_R16G16B16A16_SFLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM:   return 64;  // VK_FORMAT_A2B10G10R10_UNORM_PACK32
    default:                              return 0;   // VK_FORMAT_UNDEFINED
    }
}

// One texture living on both APIs: the D3D12 resource owns the memory, the VkImage is a view of
// that same memory on the host's device.
struct Crossing
{
    ComPtr<ID3D12Resource> on12;
    HANDLE handle = nullptr;
    VkImage image = nullptr;
    VkDeviceMemory memory = nullptr;

    void Release(Api &api, VkDevice device)
    {
        if (image != nullptr)
            api.destroyImage(device, image, nullptr);
        if (memory != nullptr)
            api.freeMemory(device, memory, nullptr);
        if (handle != nullptr)
            CloseHandle(handle);
        image = nullptr;
        memory = nullptr;
        handle = nullptr;
        on12.Reset();
    }
};

struct Route
{
    bool failed = false;
    bool ready = false;
    bool loggedReady = false;
    bool loggedFirstFrame = false;
    bool loggedQueue = false;
    bool loggedUnsupportedQueue = false;

    Api api {};
    VkDevice device = nullptr;
    VkPhysicalDevice gpu = nullptr;         // ours, matched by LUID, for memory properties only
    VkInstance ownInstance = nullptr;       // ours, solely to reach a VkPhysicalDevice
    VkPhysicalDeviceMemoryProperties memProps {};
    LUID luid {};

    VkSemaphore semToVk = nullptr, semToD3D = nullptr;

    Crossing in, out;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    int vkFormat = 0;

    void Stop(const char *why)
    {
        failed = true;
        ready = false;
        Log("vulkan: %s Standing down and leaving the host's own image alone.", why);
    }
};

Route &g_route = *new Route;  // never destroyed, as State g is: see neural.cpp

// ReShade's stock Vulkan manifest uses the _1 kill switch. DOOM Eternal's launcher sets that
// exact variable, so the compatible global manifest on this machine uses _2. Preserve both while
// the add-on creates its private discovery instance, otherwise that instance recursively enters
// ReShade again.
std::wstring SaveEnvironmentVariable(const wchar_t *name, bool &present)
{
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    present = required != 0;
    if (!present)
        return {};

    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    value.resize(copied);
    return value;
}

void RestoreEnvironmentVariable(const wchar_t *name, bool present, const std::wstring &value)
{
    SetEnvironmentVariableW(name, present ? value.c_str() : nullptr);
}

// The host VkDevice outlives a swapchain, but the imported images do not: they have the old
// swapchain's size and are paired with D3D12 resources released by the common teardown. Make the
// route visibly not ready before those resources go away, including when the replacement
// swapchain happens to have exactly the same dimensions and format.
void ReleaseSwapchainSized()
{
    Route &r = g_route;
    r.ready = false;

    if (r.device != nullptr && r.api.deviceWaitIdle != nullptr)
    {
        const int result = r.api.deviceWaitIdle(r.device);
        if (result != 0)
            Log("vulkan: vkDeviceWaitIdle during swapchain teardown returned %d.", result);
    }
    if (r.device != nullptr)
    {
        r.in.Release(r.api, r.device);
        r.out.Release(r.api, r.device);
    }

    r.width = 0;
    r.height = 0;
    r.format = DXGI_FORMAT_UNKNOWN;
    r.vkFormat = 0;
    r.loggedReady = false;
    r.loggedFirstFrame = false;
}

// Our own VkInstance, only ever used to reach a VkPhysicalDevice and ask it two questions: which
// GPU is this (LUID, so the D3D12 device lands on the same one) and what memory types does it
// have. Memory types are a property of the physical GPU and its driver, not of an instance, so
// the answer is the same one the host's device would give.
bool FindPhysicalDevice(Route &r)
{
    VkApplicationInfo app {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "amd-nr";
    app.apiVersion = (1u << 22) | (1u << 12);  // 1.1: the properties2 queries are core there

    VkInstanceCreateInfo ici {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    // This runs inside a ReShade callback, and ReShade is itself a Vulkan layer that hooks
    // vkCreateInstance -- so without this our private instance would re-enter ReShade's own
    // instance setup from inside its present. Its layer manifest publishes the variable that
    // turns it off, so use the documented switch rather than hoping the re-entry is harmless.
    // Restored immediately: the host's next instance, if it makes one, must retain the exact
    // environment it had before this callback. This matters on DOOM, where _1 is already set.
    bool hadDisable1 = false, hadDisable2 = false;
    const std::wstring disable1 =
        SaveEnvironmentVariable(L"DISABLE_VK_LAYER_reshade_1", hadDisable1);
    const std::wstring disable2 =
        SaveEnvironmentVariable(L"DISABLE_VK_LAYER_reshade_2", hadDisable2);
    SetEnvironmentVariableW(L"DISABLE_VK_LAYER_reshade_1", L"1");
    SetEnvironmentVariableW(L"DISABLE_VK_LAYER_reshade_2", L"1");
    const int made = r.api.createInstance(&ici, nullptr, &r.ownInstance);
    RestoreEnvironmentVariable(L"DISABLE_VK_LAYER_reshade_2", hadDisable2, disable2);
    RestoreEnvironmentVariable(L"DISABLE_VK_LAYER_reshade_1", hadDisable1, disable1);
    if (made != 0 || r.ownInstance == nullptr)
    {
        Log("vulkan: could not create a private VkInstance to identify the GPU (VkResult %d).",
            made);
        return false;
    }

    uint32_t count = 0;
    r.api.enumeratePhysicalDevices(r.ownInstance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    if (count != 0)
        r.api.enumeratePhysicalDevices(r.ownInstance, &count, gpus.data());
    if (count == 0)
    {
        Log("vulkan: no physical devices.");
        return false;
    }

    // With more than one, there is no way from here to tell which the host chose -- ReShade does
    // not expose the VkPhysicalDevice. The first is taken and the choice is logged, so a wrong
    // one is visible in the log rather than showing up as an import that fails for no reason.
    for (uint32_t i = 0; i < count; ++i)
    {
        VkPhysicalDeviceIDProperties ids {};
        ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props {};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &ids;
        r.api.getPhysicalDeviceProperties2(gpus[i], &props);
        const char *name = reinterpret_cast<const char *>(props.properties + kDeviceNameOffset);
        uint32_t vendor = 0;
        std::memcpy(&vendor, props.properties + kVendorIdOffset, sizeof(vendor));
        LUID luid {};
        std::memcpy(&luid, ids.deviceLUID, sizeof(luid));
        Log("vulkan: physical device %u: %s (vendor %04X) LUID %08lX:%08lX valid=%u%s", i, name,
            vendor, static_cast<unsigned long>(luid.HighPart), luid.LowPart, ids.deviceLUIDValid,
            i == 0 ? "  <- taken" : "");
        if (i == 0)
        {
            r.gpu = gpus[i];
            r.luid = luid;
            r.api.getMemoryProperties(r.gpu, &r.memProps);
        }
    }
    if (count > 1)
        Log("vulkan: more than one physical device. ReShade does not say which one the host is "
            "rendering on, so the first was taken. If nothing imports, that is the first thing "
            "to suspect.");
    return r.gpu != nullptr;
}

// The DXGI adapter that IS that Vulkan device. Not "an AMD adapter": this machine reports three
// adapters with the same name and different LUIDs, so anything but a LUID match lands on a
// different device and every import fails with nothing to explain it.
bool AdapterForLuid(LUID luid, ComPtr<IDXGIAdapter> &out)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart)
        {
            out = adapter;
            return true;
        }
        adapter.Reset();
    }
    return false;
}

// A D3D12 fence, shared, imported on the host's device as a timeline semaphore. A D3D12 fence is
// a 64-bit monotonic counter and so is a timeline, which is why the two map onto each other with
// no translation. Not on the per-frame path yet -- but if this refuses, nothing else will work
// either, and it says so here rather than in a copy that silently reads nothing.
bool ImportFence(Route &r, HANDLE handle, VkSemaphore &out, const char *what)
{
    VkSemaphoreTypeCreateInfo type {};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci {};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &type;
    if (r.api.createSemaphore(r.device, &sci, nullptr, &out) != 0)
    {
        Log("vulkan: vkCreateSemaphore (timeline) failed for %s.", what);
        return false;
    }
    VkImportSemaphoreWin32HandleInfoKHR imp {};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
    imp.semaphore = out;
    imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    imp.handle = handle;
    const int result = r.api.importSemaphoreWin32(r.device, &imp);
    Log("vulkan: %s fence -> timeline semaphore %s", what,
        result == 0 ? "imported" : "REFUSED by the driver");
    return result == 0;
}

// One shared texture: created on our D3D12 device, exported, imported as a VkImage, and left in
// VK_IMAGE_LAYOUT_GENERAL for good.
bool BuildCrossing(Route &r, Crossing &c, UINT w, UINT h, DXGI_FORMAT fmt, int vkFmt,
                   VkCommandBuffer cmd, const char *what)
{
    c.Release(r.api, r.device);

    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    HRESULT hr = g.bridge.workDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&c.on12));
    if (FAILED(hr))
    {
        Log("vulkan: creating the shared %s texture failed 0x%08lX.", what, hr);
        return false;
    }
    hr = g.bridge.workDevice->CreateSharedHandle(c.on12.Get(), nullptr, GENERIC_ALL, nullptr, &c.handle);
    if (FAILED(hr))
    {
        Log("vulkan: exporting the shared %s handle failed 0x%08lX.", what, hr);
        return false;
    }

    VkExternalMemoryImageCreateInfo emi {};
    emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

    VkImageCreateInfo ici {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vkFmt;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (r.api.createImage(r.device, &ici, nullptr, &c.image) != 0)
    {
        Log("vulkan: vkCreateImage for %s failed.", what);
        return false;
    }

    VkMemoryRequirements mr {};
    r.api.getImageMemoryRequirements(r.device, c.image, &mr);
    uint32_t type = PickMemoryType(r.memProps, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        // The memory belongs to a D3D12 resource, so it is device-local by construction; if the
        // properties came back in a shape this did not expect, the lowest allowed type is a
        // better move than giving up.
        for (uint32_t i = 0; i < 32; ++i)
            if ((mr.memoryTypeBits & (1u << i)) != 0)
            {
                type = i;
                break;
            }
        Log("vulkan: no device-local memory type matched for %s; falling back to type %u.", what,
            type);
    }
    if (type == UINT32_MAX)
    {
        Log("vulkan: the %s image accepts no memory type at all.", what);
        return false;
    }

    // The driver reports DEDICATED_ONLY for every format this carries, so the allocation has to
    // name the image it backs. Without it the allocation is refused and the message says nothing
    // about dedication.
    VkMemoryDedicatedAllocateInfo ded {};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.image = c.image;
    VkImportMemoryWin32HandleInfoKHR imp {};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    imp.pNext = &ded;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    imp.handle = c.handle;

    VkMemoryAllocateInfo mai {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &imp;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type;

    const int allocated = r.api.allocateMemory(r.device, &mai, nullptr, &c.memory);
    if (allocated != 0)
    {
        Log("vulkan: importing the %s texture into the host's device failed (VkResult %d).", what,
            allocated);
        return false;
    }
    if (r.api.bindImageMemory(r.device, c.image, c.memory, 0) != 0)
    {
        Log("vulkan: vkBindImageMemory for %s failed.", what);
        return false;
    }

    // The one and only move out of UNDEFINED, here, while the image holds nothing worth keeping.
    VkImageMemoryBarrier toGeneral {};
    toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneral.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED_VALUE;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED_VALUE;
    toGeneral.image = c.image;
    toGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    r.api.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toGeneral);

    Log("vulkan: %s %ux%u DXGI %d -> VkImage imported (%llu bytes, memory type %u)", what, w, h,
        static_cast<int>(fmt), static_cast<unsigned long long>(mr.size), type);
    return true;
}
