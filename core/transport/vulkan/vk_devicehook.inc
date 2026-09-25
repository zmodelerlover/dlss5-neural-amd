// The Vulkan route's vkCreateDevice hook. Included by vk_route.inc, in namespace vkroute.

// ---------------------------------------------------------------------------------------------
// Making the host's device interop-capable, which it is not by default.
//
// Measured on RPCS3: ReShade's own vkCreateDevice does NOT enable the interop extensions. The
// route's first run stood down at `vkImportSemaphoreWin32HandleKHR is missing`, which is the exact
// symptom vk_route.inc's header predicted -- so the simplification does not hold and plan B is
// needed.
//
// Prefer swapping the executable's vkCreateDevice import: RPCS3, Detroit and DOOM use that simple
// path, which patches no code. RDR2 instead loads Vulkan with LoadLibrary/GetProcAddress and keeps
// the returned function pointers outside its import table. For that class of host the fallback
// trampolines vulkan-1!vkCreateDevice itself, so pointers cached before this add-on loaded are
// covered too. The fallback is installed on a worker after DllMain releases the loader lock;
// MinHook briefly suspends threads while enabling a trampoline and must never do that in DllMain.
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

enum class InstallKind : uint8_t
{
    none,
    staticIat,
    inlinePending,
    inlineHook,
    failed,
};

PFN_vkCreateDevice g_original = nullptr;
void **g_slot = nullptr;
void *g_inlineTarget = nullptr;
std::atomic<InstallKind> g_installKind { InstallKind::none };
std::atomic_bool g_ran { false };
std::atomic_bool g_reportReady { false };
std::atomic_bool g_stopping { false };
char g_report[512] = "the host never created a Vulkan device through the patched entry";
char g_installReport[512] = "vulkan: no device hook was installed";
// The LUID of the GPU the host created its device on, 0 until seen. FindPhysicalDevice picks by it.
// ponytail: the last device the host creates wins; key by VkDevice if a host renders on two GPUs.
std::atomic<uint64_t> g_hostLuid { 0 };

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
    g_ran.store(true, std::memory_order_relaxed);
    if (g_original == nullptr || info == nullptr)
        return -1;

    const HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
    // Read here, the one place the host's own VkPhysicalDevice is in hand; a handle kept for later
    // would dangle if the host recreated its instance. A missing entry or invalid LUID is unknown.
    if (auto props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            GetProcAddress(vulkan, "vkGetPhysicalDeviceProperties2")))
    {
        VkPhysicalDeviceIDProperties ids {};
        ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props {};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &ids;
        props2(gpu, &props);
        uint64_t luid = 0;
        std::memcpy(&luid, ids.deviceLUID, sizeof(luid));
        if (ids.deviceLUIDValid != 0)
            g_hostLuid.store(luid, std::memory_order_relaxed);
    }
    auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        GetProcAddress(vulkan, "vkEnumerateDeviceExtensionProperties"));

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
        g_reportReady.store(true, std::memory_order_release);
        return result;
    }

    // Nothing about the game's own rendering depends on what was added here, so a refusal must
    // cost the route and not the session. Hand the host exactly what it asked for and let it run.
    std::snprintf(g_report, sizeof(g_report),
                  "vulkan: the host's device REFUSED the added extensions (VkResult %d). Retrying "
                  "with the host's own request untouched -- the game will run and this route will "
                  "not.", result);
    result = g_original(gpu, info, alloc, out);
    g_reportReady.store(true, std::memory_order_release);
    return result;
}

// Swap one pointer in the executable's import address table. This remains the preferred route:
// it is tiny, reversible, and has no effect outside that one call site.
bool InstallStaticImport()
{
    auto *base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
    if (base == nullptr)
        return false;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0)
        return false;

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
                return false;
            g_original = reinterpret_cast<PFN_vkCreateDevice>(addrs->u1.Function);
            g_slot = reinterpret_cast<void **>(&addrs->u1.Function);
            *g_slot = reinterpret_cast<void *>(&CreateDevice);
            VirtualProtect(&addrs->u1.Function, sizeof(addrs->u1.Function), old, &old);
            g_installKind.store(InstallKind::staticIat, std::memory_order_release);
            return true;
        }
    }
    return false;
}

// Dynamic Vulkan loaders can cache vkCreateDevice before the add-on exists, so patching
// GetProcAddress or vkGetInstanceProcAddr would still miss them. Hook the final function entry
// instead. This runs after DllMain: enabling MinHook suspends process threads very briefly.
DWORD WINAPI InstallInline(void *moduleReference)
{
    HMODULE self = static_cast<HMODULE>(moduleReference);
    HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
    void *target = vulkan != nullptr
        ? reinterpret_cast<void *>(GetProcAddress(vulkan, "vkCreateDevice"))
        : nullptr;
    if (target == nullptr || g_stopping.load(std::memory_order_acquire))
    {
        std::snprintf(g_installReport, sizeof(g_installReport),
                      "vulkan: dynamic loader detected, but vulkan-1!vkCreateDevice could not be "
                      "resolved before device creation.");
        g_installKind.store(InstallKind::failed, std::memory_order_release);
        FreeLibraryAndExitThread(self, 0);
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK)
    {
        std::snprintf(g_installReport, sizeof(g_installReport),
                      "vulkan: dynamic vkCreateDevice hook initialization failed: %s.",
                      MH_StatusToString(status));
        g_installKind.store(InstallKind::failed, std::memory_order_release);
        FreeLibraryAndExitThread(self, 0);
    }

    status = MH_CreateHook(target, reinterpret_cast<void *>(&CreateDevice),
                           reinterpret_cast<void **>(&g_original));
    if (status != MH_OK)
    {
        std::snprintf(g_installReport, sizeof(g_installReport),
                      "vulkan: dynamic vkCreateDevice trampoline creation failed: %s.",
                      MH_StatusToString(status));
        MH_Uninitialize();
        g_installKind.store(InstallKind::failed, std::memory_order_release);
        FreeLibraryAndExitThread(self, 0);
    }

    status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        std::snprintf(g_installReport, sizeof(g_installReport),
                      "vulkan: dynamic vkCreateDevice trampoline enable failed: %s.",
                      MH_StatusToString(status));
        MH_RemoveHook(target);
        MH_Uninitialize();
        g_original = nullptr;
        g_installKind.store(InstallKind::failed, std::memory_order_release);
        FreeLibraryAndExitThread(self, 0);
    }

    g_inlineTarget = target;
    std::snprintf(g_installReport, sizeof(g_installReport),
                  "vulkan: dynamic loader detected; installed an inline vkCreateDevice hook "
                  "before the host created its device.");
    g_installKind.store(InstallKind::inlineHook, std::memory_order_release);
    FreeLibraryAndExitThread(self, 0);
}

void Install()
{
    if (g_installKind.load(std::memory_order_acquire) != InstallKind::none)
        return;
    if (InstallStaticImport())
        return;

    // Hold our own module reference until the worker returns, so an explicit ReShade unload
    // cannot invalidate its code while it waits for the loader lock held by this DllMain.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&g_installKind), &self))
    {
        std::snprintf(g_installReport, sizeof(g_installReport),
                      "vulkan: dynamic loader detected, but the add-on could not retain itself "
                      "while installing the vkCreateDevice hook.");
        g_installKind.store(InstallKind::failed, std::memory_order_release);
        return;
    }

    g_installKind.store(InstallKind::inlinePending, std::memory_order_release);
    HANDLE worker = CreateThread(nullptr, 0, InstallInline, self, 0, nullptr);
    if (worker == nullptr)
    {
        std::snprintf(g_installReport, sizeof(g_installReport),
                      "vulkan: dynamic loader detected, but the vkCreateDevice hook worker could "
                      "not be started (Win32 error %lu).", GetLastError());
        g_installKind.store(InstallKind::failed, std::memory_order_release);
        FreeLibrary(self);
        return;
    }
    CloseHandle(worker);
}

void Remove()
{
    g_stopping.store(true, std::memory_order_release);
    const InstallKind kind = g_installKind.load(std::memory_order_acquire);
    if (kind == InstallKind::staticIat && g_slot != nullptr)
    {
        DWORD old = 0;
        if (VirtualProtect(g_slot, sizeof(void *), PAGE_READWRITE, &old))
        {
            *g_slot = reinterpret_cast<void *>(g_original);
            VirtualProtect(g_slot, sizeof(void *), old, &old);
        }
    }
    else if (kind == InstallKind::inlineHook && g_inlineTarget != nullptr)
    {
        MH_DisableHook(g_inlineTarget);
        MH_RemoveHook(g_inlineTarget);
        MH_Uninitialize();
    }
    g_installKind.store(InstallKind::none, std::memory_order_release);
}

// Said once, on the first Vulkan present: DllMain runs before the log file exists.
void ReportOnce()
{
    static bool said = false;
    if (said)
        return;
    said = true;
    const InstallKind kind = g_installKind.load(std::memory_order_acquire);
    if (kind == InstallKind::none || kind == InstallKind::inlinePending ||
        kind == InstallKind::failed)
    {
        Log("%s", kind == InstallKind::inlinePending
                      ? "vulkan: the dynamic vkCreateDevice hook had not finished before the "
                        "host created its device."
                      : g_installReport);
        return;
    }
    if (kind == InstallKind::inlineHook)
        Log("%s", g_installReport);
    if (g_ran.load(std::memory_order_acquire) &&
        g_reportReady.load(std::memory_order_acquire))
        Log("%s", g_report);
    else
        Log("vulkan: vkCreateDevice was patched but the host had already created its device "
            "before this add-on loaded, so nothing was added to it.");
}

}  // namespace devicehook
