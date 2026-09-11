// The Vulkan route: the same bridge as D3D11, with the crossing running the other way.
//
// On D3D11 the shared textures are created on the game's device and opened on ours, because
// D3D11 and D3D12 both speak the same shared-handle dialect. Vulkan does not: a handle exported
// from Vulkan is opaque memory, and D3D12 cannot open it. So the direction flips -- the textures
// and the fences are created on OUR D3D12 device, exported as NT handles, and imported into the
// host's VkDevice as VkImage. That is not a preference, it is the only way round that works, and
// it is what every working DLSS 5 Vulkan add-on does.
//
// Two things make this far smaller than the reference implementations:
//
//   * They hook vulkan-1!vkCreateDevice to append the interop extensions, because a host never
//     asks for them and after the device exists it is too late. We do not have to: ReShade is
//     itself a Vulkan layer and its own vkCreateDevice already enables VK_KHR_external_memory,
//     _win32, external_semaphore, dedicated_allocation and timeline_semaphore -- the strings are
//     in ReShade64.dll. If that ever stops being true the symptom is exact and is checked for
//     below: vkImportSemaphoreWin32HandleKHR resolves to null.
//   * They own the submission. We record into ReShade's own immediate command buffer and let
//     ReShade submit it, so there is no command pool, no queue family to match, and no second
//     submission racing the host's.
//
// The synchronisation is the CPU-side stall the D3D11 route already uses (FlushAndWait11):
// flush, wait for idle, then hand over. Slower than a fence pair and a great deal easier to be
// sure of, and the D3D11 route has run ten thousand frames on it. The imported fences are built
// anyway because the async version needs them and because their import is the single best test
// that the interop is real.
//
// The layout rule, which is silent when broken: the imported image leaves VK_IMAGE_LAYOUT_
// UNDEFINED exactly once, at build time, and stays VK_IMAGE_LAYOUT_GENERAL forever. A transition
// out of UNDEFINED may discard the contents, so doing it per frame would throw away the frame
// that was just copied in and leave no error anywhere.

namespace vkroute
{

using namespace vkraw;

// ---------------------------------------------------------------------------------------------
// Making the host's device interop-capable, which it is not by default.
//
// Measured on RPCS3: ReShade's own vkCreateDevice does NOT enable the interop extensions. The
// route's first run stood down at `vkImportSemaphoreWin32HandleKHR is missing`, which is the exact
// symptom the note above predicted -- so the simplification does not hold and plan B is needed.
//
// The reference implementations reach for MinHook and trampoline vulkan-1!vkCreateDevice. This
// does not have to: RPCS3 imports vkCreateDevice **by name, statically**, so the address it calls
// lives in its own import address table and swapping one pointer there is the whole hook. No code
// is patched, nothing is disassembled, and undoing it is writing the old pointer back.
//
// A host that resolves Vulkan dynamically has no such entry and nothing is patched, which is
// reported rather than assumed -- on such a host the route stands down exactly as it does now.
namespace devicehook
{

// The seven the transport needs, in the order vkprobe proved them on this GPU. Only ones the
// physical device actually advertises are added: asking for an unsupported extension does not
// degrade gracefully, it fails the whole device creation and takes the game's renderer with it.
const char *const kWanted[] = {
    "VK_KHR_external_memory",       "VK_KHR_external_memory_win32",
    "VK_KHR_external_semaphore",    "VK_KHR_external_semaphore_win32",
    "VK_KHR_dedicated_allocation",  "VK_KHR_get_memory_requirements2",
    "VK_KHR_timeline_semaphore",
};

PFN_vkCreateDevice g_original = nullptr;
void **g_slot = nullptr;
bool g_installed = false;
bool g_ran = false;
char g_report[512] = "the host never created a Vulkan device through the patched entry";

// VkPhysicalDeviceVulkan12Features, when the host chains one, carries timelineSemaphore itself and
// the two must not both be present. Its layout is fixed by the spec: sType, four bytes of padding,
// pNext, then 47 VkBool32 in declaration order, of which timelineSemaphore is the 38th --
// samplerMirrorClampToEdge, drawIndirectCount, storageBuffer8BitAccess,
// uniformAndStorageBuffer8BitAccess, storagePushConstant8, shaderBufferInt64Atomics,
// shaderSharedInt64Atomics, shaderFloat16, shaderInt8, descriptorIndexing, then the ten
// shader*Array*Indexing, then the ten descriptorBinding*, runtimeDescriptorArray,
// samplerFilterMinmax, scalarBlockLayout, imagelessFramebuffer, uniformBufferStandardLayout,
// shaderSubgroupExtendedTypes, separateDepthStencilLayouts, hostQueryReset, timelineSemaphore.
constexpr uint32_t kVulkan12Features = 49;
constexpr size_t kTimelineSemaphoreOffset = 16 + 37 * 4;

int __stdcall CreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *info, const void *alloc,
                           VkDevice *out)
{
    g_ran = true;
    if (g_original == nullptr || info == nullptr)
        return -1;

    auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"), "vkEnumerateDeviceExtensionProperties"));

    std::vector<VkExtensionProperties> supported;
    if (enumerate != nullptr)
    {
        uint32_t count = 0;
        enumerate(gpu, nullptr, &count, nullptr);
        supported.resize(count);
        if (count != 0)
            enumerate(gpu, nullptr, &count, supported.data());
    }
    auto advertised = [&](const char *name) {
        for (const auto &e : supported)
            if (std::strcmp(e.extensionName, name) == 0)
                return true;
        return false;
    };

    std::vector<const char *> names(info->ppEnabledExtensionNames,
                                    info->ppEnabledExtensionNames + info->enabledExtensionCount);
    auto already = [&](const char *name) {
        for (const char *n : names)
            if (n != nullptr && std::strcmp(n, name) == 0)
                return true;
        return false;
    };

    int added = 0, missing = 0;
    bool addedTimeline = false;
    for (const char *want : kWanted)
    {
        if (already(want))
            continue;
        if (enumerate != nullptr && !advertised(want))
        {
            ++missing;
            continue;
        }
        names.push_back(want);
        ++added;
        if (std::strcmp(want, "VK_KHR_timeline_semaphore") == 0)
            addedTimeline = true;
    }

    VkDeviceCreateInfo patched = *info;
    patched.enabledExtensionCount = static_cast<uint32_t>(names.size());
    patched.ppEnabledExtensionNames = names.data();

    // The feature, which is separate from the extension: enabling VK_KHR_timeline_semaphore
    // without turning timelineSemaphore on creates a device that has the entry points and refuses
    // to make a timeline.
    const bool wantTimeline = addedTimeline || already("VK_KHR_timeline_semaphore");
    bool patchedInPlace = false;
    if (wantTimeline)
    {
        for (auto *node = static_cast<const uint8_t *>(info->pNext); node != nullptr;)
        {
            uint32_t sType = 0;
            std::memcpy(&sType, node, sizeof(sType));
            if (sType == kVulkan12Features)
            {
                auto *bits = const_cast<uint8_t *>(node) + kTimelineSemaphoreOffset;
                uint32_t on = 0;
                std::memcpy(&on, bits, sizeof(on));
                if (on == 0)
                {
                    on = 1;
                    std::memcpy(bits, &on, sizeof(on));
                }
                patchedInPlace = true;
                break;
            }
            const void *next = nullptr;
            std::memcpy(&next, node + 8, sizeof(next));
            node = static_cast<const uint8_t *>(next);
        }
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline {};
    if (wantTimeline && !patchedInPlace)
    {
        timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        timeline.pNext = const_cast<void *>(info->pNext);
        timeline.timelineSemaphore = 1;
        patched.pNext = &timeline;
    }

    int result = g_original(gpu, &patched, alloc, out);
    if (result == 0)
    {
        std::snprintf(g_report, sizeof(g_report),
                      "vulkan: the host's device was created with %d extension(s) added by this "
                      "add-on (%d of the seven were not advertised by the driver), timeline "
                      "feature %s. Without this the host creates a device that cannot import a "
                      "D3D12 texture and the route stands down.",
                      added, missing,
                      !wantTimeline      ? "not needed"
                      : patchedInPlace   ? "set on the host's own Vulkan 1.2 features"
                                         : "chained");
        return result;
    }

    // Nothing about the game's own rendering depends on what was added here, so a refusal must
    // cost the route and not the session. Hand the host exactly what it asked for and let it run.
    std::snprintf(g_report, sizeof(g_report),
                  "vulkan: the host's device REFUSED the added extensions (VkResult %d). Retrying "
                  "with the host's own request untouched -- the game will run and this route will "
                  "not.", result);
    return g_original(gpu, info, alloc, out);
}

// Swap one pointer in the executable's import address table.
void Install()
{
    if (g_installed)
        return;
    auto *base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
    if (base == nullptr)
        return;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0)
        return;

    for (auto *desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);
         desc->Name != 0; ++desc)
    {
        const char *dll = reinterpret_cast<const char *>(base + desc->Name);
        if (_stricmp(dll, "vulkan-1.dll") != 0)
            continue;
        // OriginalFirstThunk carries the names, FirstThunk the addresses the process calls. Some
        // linkers omit the first, in which case the second still holds names before binding.
        const auto *names = reinterpret_cast<const IMAGE_THUNK_DATA *>(
            base + (desc->OriginalFirstThunk != 0 ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto *addrs = reinterpret_cast<IMAGE_THUNK_DATA *>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++addrs)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const auto *import =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (std::strcmp(import->Name, "vkCreateDevice") != 0)
                continue;
            DWORD old = 0;
            if (!VirtualProtect(&addrs->u1.Function, sizeof(addrs->u1.Function),
                                PAGE_READWRITE, &old))
                return;
            g_original = reinterpret_cast<PFN_vkCreateDevice>(addrs->u1.Function);
            g_slot = reinterpret_cast<void **>(&addrs->u1.Function);
            *g_slot = reinterpret_cast<void *>(&CreateDevice);
            VirtualProtect(&addrs->u1.Function, sizeof(addrs->u1.Function), old, &old);
            g_installed = true;
            return;
        }
    }
}

void Remove()
{
    if (!g_installed || g_slot == nullptr)
        return;
    DWORD old = 0;
    if (VirtualProtect(g_slot, sizeof(void *), PAGE_READWRITE, &old))
    {
        *g_slot = reinterpret_cast<void *>(g_original);
        VirtualProtect(g_slot, sizeof(void *), old, &old);
    }
    g_installed = false;
}

// Said once, on the first Vulkan present: DllMain runs before the log file exists.
void ReportOnce()
{
    static bool said = false;
    if (said)
        return;
    said = true;
    if (!g_installed)
    {
        Log("vulkan: this host does not import vkCreateDevice by name, so the interop extensions "
            "could not be added to its device. If the route stands down on missing entry points, "
            "that is why.");
        return;
    }
    Log("%s", g_ran ? g_report
                    : "vulkan: vkCreateDevice was patched but the host had already created its "
                      "device before this add-on loaded, so nothing was added to it.");
}

}  // namespace devicehook

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

Route g_route;

// Our own VkInstance, only ever used to reach a VkPhysicalDevice and ask it two questions: which
// GPU is this (LUID, so the D3D12 device lands on the same one) and what memory types does it
// have. Memory types are a property of the physical GPU and its driver, not of an instance, so
// the answer is the same one the host's device would give.
bool FindPhysicalDevice(Route &r)
{
    VkApplicationInfo app {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "dlss5-neural";
    app.apiVersion = (1u << 22) | (1u << 12);  // 1.1: the properties2 queries are core there

    VkInstanceCreateInfo ici {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    // This runs inside a ReShade callback, and ReShade is itself a Vulkan layer that hooks
    // vkCreateInstance -- so without this our private instance would re-enter ReShade's own
    // instance setup from inside its present. Its layer manifest publishes the variable that
    // turns it off, so use the documented switch rather than hoping the re-entry is harmless.
    // Restored immediately: the host's next instance, if it makes one, must still get ReShade.
    SetEnvironmentVariableW(L"DISABLE_VK_LAYER_reshade_1", L"1");
    const int made = r.api.createInstance(&ici, nullptr, &r.ownInstance);
    SetEnvironmentVariableW(L"DISABLE_VK_LAYER_reshade_1", nullptr);
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
    HRESULT hr = g.workDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&c.on12));
    if (FAILED(hr))
    {
        Log("vulkan: creating the shared %s texture failed 0x%08lX.", what, hr);
        return false;
    }
    hr = g.workDevice->CreateSharedHandle(c.on12.Get(), nullptr, GENERIC_ALL, nullptr, &c.handle);
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

// Everything that only has to happen once, plus the resize path. Returns false with a reason
// already logged.
bool Ensure(Route &r, device *dev, command_queue *queue, UINT w, UINT h, DXGI_FORMAT fmt)
{
    if (r.failed)
        return false;

    if (r.device == nullptr)
    {
        r.device = reinterpret_cast<VkDevice>(dev->get_native());
        if (r.device == nullptr)
        {
            r.Stop("ReShade reported no native VkDevice.");
            return false;
        }
        if (!r.api.LoadCore())
        {
            r.Stop("vulkan-1.dll or its core entry points are missing.");
            return false;
        }
        if (!r.api.LoadDevice(r.device))
        {
            // The exact symptom of a device created without the interop extensions. ReShade's own
            // Vulkan layer enables them, so this means either ReShade is not the layer in front of
            // this device or that changed in a newer build.
            r.Stop("the host's VkDevice has no external-memory/semaphore entry points -- "
                   "vkImportSemaphoreWin32HandleKHR is missing. The device was created without "
                   "the KHR interop extensions, and after creation that cannot be fixed.");
            return false;
        }
        Log("vulkan: host VkDevice %p, interop entry points resolved.",
            static_cast<void *>(r.device));
        if (!FindPhysicalDevice(r))
        {
            r.Stop("could not identify the physical device.");
            return false;
        }
    }

    if (g.workDevice == nullptr)
    {
        ComPtr<IDXGIAdapter> adapter;
        if (!AdapterForLuid(r.luid, adapter))
        {
            r.Stop("no DXGI adapter carries the Vulkan device's LUID. D3D12 and Vulkan would be "
                   "different GPUs, and nothing can be shared between them.");
            return false;
        }
        if (!CreateWorkDevice(adapter.Get(), r.luid))
        {
            r.Stop("the work D3D12 device did not come up.");
            return false;
        }
        g.device = g.workDevice;
        g.queue = g.workQueue;
        // The completion fence, the one this route waits on before copying the result back.
        // CreateWorkDevice makes the ring fence and the two shared ones; this one is created by
        // the D3D11 route inside its own frame function, so the Vulkan route never had it -- and
        // Signal on a null fence is not a wait, it is nothing, so the copy back would have raced
        // the network with no error anywhere to say so.
        if (FAILED(g.workDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
        {
            r.Stop("could not create the completion fence.");
            return false;
        }
        if (!ImportFence(r, g.crossHandle, r.semToVk, "D3D12 -> Vulkan") ||
            !ImportFence(r, g.backHandle, r.semToD3D, "Vulkan -> D3D12"))
        {
            r.Stop("the shared fences would not import.");
            return false;
        }
    }

    if (r.ready && r.width == w && r.height == h && r.format == fmt)
        return true;

    const int vkFmt = VkFormatFor(fmt);
    if (vkFmt == 0)
    {
        Log("vulkan: swapchain format DXGI %d has no Vulkan equivalent here. Guessing one would "
            "reorder channels silently, so the route stops instead.", static_cast<int>(fmt));
        r.Stop("unsupported swapchain format.");
        return false;
    }

    r.ready = false;
    auto *cmd = reinterpret_cast<VkCommandBuffer>(
        queue->get_immediate_command_list()->get_native());
    if (cmd == nullptr)
    {
        r.Stop("ReShade gave no immediate command buffer.");
        return false;
    }
    if (!BuildCrossing(r, r.in, w, h, fmt, vkFmt, cmd, "colour-in") ||
        !BuildCrossing(r, r.out, w, h, fmt, vkFmt, cmd, "result-out"))
    {
        r.Stop("the shared textures would not cross.");
        return false;
    }
    queue->flush_immediate_command_list();
    queue->wait_idle();

    // Our private copy, on our device, so the network never reads a shared resource directly --
    // same reason the D3D11 route keeps crossLocal.
    g.crossLocal.Reset();
    if (!CreateTexture(w, h, fmt, g.crossLocal, "crossLocal"))
    {
        r.Stop("could not create the private copy of the frame.");
        return false;
    }

    r.width = w;
    r.height = h;
    r.format = fmt;
    r.vkFormat = vkFmt;
    r.ready = true;
    if (!r.loggedReady)
    {
        r.loggedReady = true;
        Log("vulkan: bridge up at %ux%u. Textures cross as D3D12 resources imported into the "
            "host's VkDevice; ReShade's own command buffer carries the copies.", w, h);
    }
    return true;
}

// One frame. Copy the host's back buffer into the shared texture, run the network on our D3D12
// queue, copy the result back over the back buffer.
void Present(command_queue *queue, swapchain *sc)
{
    Route &r = g_route;
    devicehook::ReportOnce();
    device *dev = sc->get_device();

    const resource back = sc->get_current_back_buffer();
    if (back.handle == 0)
        return;
    const resource_desc bd = dev->get_resource_desc(back);
    if (bd.texture.samples != 1)
        return;

    // Resolved once, here, so the D3D12 side of the crossing, the private copy, the working
    // textures and compose all agree on one concrete format. A typeless swapchain is legal on the
    // D3D12 side and impossible on the Vulkan one; carrying it further would mean the two sides of
    // the same memory disagreeing about what is in it.
    if (!Ensure(r, dev, queue, bd.texture.width, bd.texture.height,
                ColourReadFormat(static_cast<DXGI_FORMAT>(bd.texture.format))))
        return;

    // The working textures. Both of the other routes call this and this one did not, so
    // netColour, netBase, netResidual, netMotion, netDepth and composed were all null while
    // RecordNetwork wrote descriptors through them -- an access violation, not a failure. This
    // is the other half of the first Vulkan crash; bringing the engine up alone was never going
    // to be enough.
    if (!EnsureResources(r.width, r.height, r.format, g.scale.load()))
    {
        g.unavailable = true;
        if (*g.reason == 0)
            g.reason = "could not create the working textures; see dlss5-neural.log";
        Log("vulkan: %s", g.reason);
        return;
    }

    // Same order the other two routes use, and the reason the first Vulkan run crashed: without
    // it g.runtime is null and RecordNetwork dereferences it at a fixed offset, which is an
    // access violation rather than a failure. The work device exists by here, which is what
    // InitPipeline and InitEngine need.
    UINT wanted = WantedPasses();
    if (!BringUpEngines(wanted))
    {
        g.unavailable = true;
        g.reason = "could not bring the engine up on the Vulkan bridge";
        Log("vulkan: %s", g.reason);
        return;
    }
    g.loadedPasses = wanted;

    // The same guard the D3D11 route carries. Our own queue is drained every frame further down,
    // but the engine keeps its own timeline in async mode, and starting a second evaluation on
    // top of one still running is how this hangs rather than skips.
    bool runNetwork = true;
    const bool jobPending =
        g.fence->GetCompletedValue() < g.completion ||
        static_cast<UINT>(InterlockedCompareExchange(
            reinterpret_cast<volatile LONG *>(&At<UINT>(g.runtime, 0x8d6f4)), 0, 0)) < g.lastJob;
    if (jobPending && GetTickCount64() - g.lastJobAt < 500)
    {
        runNetwork = false;
        if (++g.skipped % 120 == 1)
            Log("vulkan: network skipped, previous evaluation still pending (%llu skipped, %llu "
                "done)", static_cast<unsigned long long>(g.skipped),
                static_cast<unsigned long long>(g.frame));
    }

    auto *backImage = reinterpret_cast<VkImage>(back.handle);
    VkImageCopy region {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { r.width, r.height, 1 };

    // --- 1. the host's frame comes over ---------------------------------------------------
    // ReShade's barrier() moves the host's own image, because ReShade is what knows the layout
    // it is in; the copy itself is raw Vulkan on the same command buffer.
    {
        command_list *list = queue->get_immediate_command_list();
        auto *cmd = reinterpret_cast<VkCommandBuffer>(list->get_native());
        if (cmd == nullptr)
            return;
        list->barrier(back, resource_usage::present, resource_usage::copy_source);
        r.api.cmdCopyImage(cmd, backImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.in.image,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        list->barrier(back, resource_usage::copy_source, resource_usage::present);
        queue->flush_immediate_command_list();
        queue->wait_idle();
    }

    // --- 2. our device does the work ------------------------------------------------------
    const UINT slot = static_cast<UINT>(g.serial % State::kRing);
    if (g.ringValue[slot] != 0 && g.ringFence->GetCompletedValue() < g.ringValue[slot])
    {
        g.ringFence->SetEventOnCompletion(g.ringValue[slot], g.ringEvent);
        WaitForSingleObject(g.ringEvent, 2000);
    }
    if (FAILED(g.alloc[slot]->Reset()) || FAILED(g.list[slot]->Reset(g.alloc[slot].Get(), nullptr)))
    {
        Log("vulkan: command list reset failed.");
        return;
    }
    auto *cmd12 = g.list[slot].Get();
    cmd12->CopyResource(g.crossLocal.Get(), r.in.on12.Get());
    Barrier(cmd12, g.crossLocal.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool ok = RecordNetwork(cmd12, g.crossLocal.Get(), r.format, nullptr, runNetwork,
                                  g.loadedPasses,
                                  [&]() { return SubmitPrivatePass(cmd12, g.alloc[slot].Get()); });
    if (!ok && g.failed)
        return;
    Barrier(cmd12, g.crossLocal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    // Freshness gate, same as the other two routes: on a frame where the network was skipped the
    // residual belongs to an older picture, and the host is better served by its own image than
    // by a correction aimed at where things used to be.
    const bool fresh = ok && CompositionIsFresh(runNetwork);
    if (fresh)
    {
        Barrier(cmd12, g.composed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd12->CopyResource(r.out.on12.Get(), g.composed.Get());
        Barrier(cmd12, g.composed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (FAILED(cmd12->Close()))
    {
        Log("vulkan: closing the command list failed.");
        return;
    }
    ID3D12CommandList *lists[] { cmd12 };
    g.workQueue->ExecuteCommandLists(1, lists);
    if (g.activePasses != 0)
        reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(g.runtime) + 0x9170)(
            g.workQueue.Get(), 1, lists);
    g.ringValue[slot] = ++g.ringSerial;
    g.workQueue->Signal(g.ringFence.Get(), g.ringSerial);
    g.completion = ++g.serial;
    g.workQueue->Signal(g.fence.Get(), g.completion);
    WaitForWorkQueue(g.completion);

    // --- 3. and the finished image goes back over the host's -------------------------------
    if (fresh && !g.noBackBuffer.load())
    {
        command_list *list = queue->get_immediate_command_list();
        auto *cmd = reinterpret_cast<VkCommandBuffer>(list->get_native());
        if (cmd != nullptr)
        {
            list->barrier(back, resource_usage::present, resource_usage::copy_dest);
            r.api.cmdCopyImage(cmd, r.out.image, VK_IMAGE_LAYOUT_GENERAL, backImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            list->barrier(back, resource_usage::copy_dest, resource_usage::present);
            queue->flush_immediate_command_list();
            queue->wait_idle();
        }
    }

    DrainReadbacks(g.netWidth, g.netHeight);

    if (!r.loggedFirstFrame)
    {
        r.loggedFirstFrame = true;
        Log("vulkan: first full round trip done at %ux%u.", r.width, r.height);
    }
    if (++g.frame <= 3 || g.frame % 120 == 0)
        Log("frame %llu processed (%llu skipped)", static_cast<unsigned long long>(g.frame),
            static_cast<unsigned long long>(g.skipped));
}

}  // namespace vkroute
