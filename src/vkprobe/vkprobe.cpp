// vkprobe -- does this AMD driver let Vulkan import a D3D12 texture and a D3D12 fence?
//
// That one question decides whether the network can ever run under a Vulkan host such as RPCS3,
// and it can be answered without writing a transport, without a game and without ReShade. The
// route every working DLSS 5 Vulkan add-on uses is the same one:
//
//   create the shared textures and two shared fences on a private D3D12 device, export NT
//   handles, and import those handles into the game's own VkDevice as VkImage and VkSemaphore
//   -- VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT and
//   VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT.
//
// Every one of those was developed and verified on NVIDIA. The same projects' OpenGL route says
// outright that if GL_EXT_memory_object_win32 is missing "the frame is not being rendered on an
// NVIDIA GPU", so vendor-neutrality is not something to assume here -- it is something to check.
// The Vulkan driver reports what it accepts before anything is created, so the check is a query,
// not a round trip: vkGetPhysicalDeviceImageFormatProperties2 chained with
// VkPhysicalDeviceExternalImageFormatInfo, and vkGetPhysicalDeviceExternalSemaphoreProperties.
// IMPORTABLE in the returned feature bits is the answer.
//
// ponytail: a capability query, not a round-trip harness. If the driver says no, the round trip
// was never going to work and the transport is a different design. If it says yes, the round
// trip is worth writing -- and it is the next step, not this one.
//
// There is no Vulkan SDK on this machine and the probe does not need one: vulkan-1.dll is in
// System32 on any Windows with a Vulkan driver, and the handful of structures used here are
// declared below against the spec. They are laid out with natural alignment on purpose -- the
// Vulkan ABI is the C ABI, so letting the compiler insert the padding is what matches it.
//
// Build:  .\build.ps1 -Target vkprobe -Exe      Run:  .\build\dlss5-vkprobe.exe

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{

// ---- the slice of Vulkan this needs, from the spec -----------------------------------------

using VkFlags = uint32_t;
using VkBool32 = uint32_t;
using VkDeviceSize = uint64_t;
struct VkInstance_T;
struct VkPhysicalDevice_T;
using VkInstance = VkInstance_T *;
using VkPhysicalDevice = VkPhysicalDevice_T *;

enum VkStructureType : int
{
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 = 1000059001,
    VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 = 1000059003,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 = 1000059004,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO = 1000071000,
    VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES = 1000071001,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES = 1000071004,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO = 1000076000,
    VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES = 1000076001,
};

// VkExternalMemoryHandleTypeFlagBits. D3D12_RESOURCE is the one that matters; the others are
// printed beside it because "AMD supports none of this" and "AMD supports it under a different
// handle type" are different findings with different amounts of work behind them.
enum : VkFlags
{
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT = 0x002,
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT = 0x004,
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT = 0x008,
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT = 0x010,
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT = 0x020,
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT = 0x040,
};
enum : VkFlags
{
    VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT = 0x1,
    VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT = 0x2,
    VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT = 0x4,
};
// VkExternalSemaphoreHandleTypeFlagBits. D3D11_FENCE is an alias of D3D12_FENCE, same bit.
enum : VkFlags
{
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT = 0x02,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT = 0x04,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT = 0x08,
};
enum : VkFlags
{
    VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT = 0x1,
    VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT = 0x2,
};

enum : VkFlags
{
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 0x01,
    VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x02,
    VK_IMAGE_USAGE_SAMPLED_BIT = 0x04,
    VK_IMAGE_USAGE_STORAGE_BIT = 0x08,
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x10,
};

struct VkApplicationInfo
{
    VkStructureType sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};

struct VkInstanceCreateInfo
{
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
};

// VkPhysicalDeviceProperties carries VkPhysicalDeviceLimits inline, ~500 bytes of fields none of
// which this probe reads. Transcribing it would be five hundred lines that only have to be
// correct in their total size, so it is a byte block instead, over-sized, with the two fields
// that are read taken at their spec offsets: vendorID at +8, deviceName at +20.
constexpr size_t kPropsBlock = 1024;
constexpr size_t kVendorIdOffset = 8;
constexpr size_t kDeviceNameOffset = 20;

struct VkPhysicalDeviceProperties2
{
    VkStructureType sType;
    void *pNext;
    unsigned char properties[kPropsBlock];
};

struct VkPhysicalDeviceIDProperties
{
    VkStructureType sType;
    void *pNext;
    uint8_t deviceUUID[16];
    uint8_t driverUUID[16];
    uint8_t deviceLUID[8];
    uint32_t deviceNodeMask;
    VkBool32 deviceLUIDValid;
};

struct VkPhysicalDeviceExternalImageFormatInfo
{
    VkStructureType sType;
    const void *pNext;
    VkFlags handleType;  // one bit of VkExternalMemoryHandleTypeFlagBits
};

struct VkPhysicalDeviceImageFormatInfo2
{
    VkStructureType sType;
    const void *pNext;
    int format;
    int type;
    int tiling;
    VkFlags usage;
    VkFlags flags;
};

struct VkExternalMemoryProperties
{
    VkFlags externalMemoryFeatures;
    VkFlags exportFromImportedHandleTypes;
    VkFlags compatibleHandleTypes;
};

struct VkExternalImageFormatProperties
{
    VkStructureType sType;
    void *pNext;
    VkExternalMemoryProperties externalMemoryProperties;
};

struct VkExtent3D
{
    uint32_t width, height, depth;
};

struct VkImageFormatProperties
{
    VkExtent3D maxExtent;
    uint32_t maxMipLevels;
    uint32_t maxArrayLayers;
    VkFlags sampleCounts;
    VkDeviceSize maxResourceSize;
};

struct VkImageFormatProperties2
{
    VkStructureType sType;
    void *pNext;
    VkImageFormatProperties imageFormatProperties;
};

struct VkPhysicalDeviceExternalSemaphoreInfo
{
    VkStructureType sType;
    const void *pNext;
    VkFlags handleType;
};

struct VkExternalSemaphoreProperties
{
    VkStructureType sType;
    void *pNext;
    VkFlags exportFromImportedHandleTypes;
    VkFlags compatibleHandleTypes;
    VkFlags externalSemaphoreFeatures;
};

struct VkExtensionProperties
{
    char extensionName[256];
    uint32_t specVersion;
};

using PFN_vkVoidFunction = void(__stdcall *)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(__stdcall *)(VkInstance, const char *);
using PFN_vkCreateInstance = int(__stdcall *)(const VkInstanceCreateInfo *, const void *,
                                              VkInstance *);
using PFN_vkDestroyInstance = void(__stdcall *)(VkInstance, const void *);
using PFN_vkEnumeratePhysicalDevices = int(__stdcall *)(VkInstance, uint32_t *,
                                                        VkPhysicalDevice *);
using PFN_vkGetPhysicalDeviceProperties2 = void(__stdcall *)(VkPhysicalDevice,
                                                             VkPhysicalDeviceProperties2 *);
using PFN_vkEnumerateDeviceExtensionProperties =
    int(__stdcall *)(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
using PFN_vkGetPhysicalDeviceImageFormatProperties2 =
    int(__stdcall *)(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2 *,
                     VkImageFormatProperties2 *);
using PFN_vkGetPhysicalDeviceExternalSemaphoreProperties =
    void(__stdcall *)(VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo *,
                      VkExternalSemaphoreProperties *);

// ---- what the bridge would actually carry ---------------------------------------------------

// The three textures the engine is handed, in the formats this project already uses on the
// D3D11 route: colour R16G16B16A16_FLOAT, depth R32_FLOAT, motion R16G16_FLOAT. The reference
// Vulkan implementation lands on exactly the same two for its guides, which is a small
// confirmation that the D3D11 side of this project is feeding the right things.
struct Probe
{
    const char *what;
    int vkFormat;
    const char *vkName;
};
constexpr Probe kProbes[] {
    { "colour", 97, "R16G16B16A16_SFLOAT" },
    { "colour", 44, "B8G8R8A8_UNORM" },
    { "colour", 37, "R8G8B8A8_UNORM" },
    { "depth", 100, "R32_SFLOAT" },
    { "motion", 83, "R16G16_SFLOAT" },
};

struct HandleKind
{
    VkFlags bit;
    const char *name;
};
constexpr HandleKind kMemoryHandles[] {
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, "D3D12_RESOURCE" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT, "D3D12_HEAP" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, "D3D11_TEXTURE" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, "D3D11_TEXTURE_KMT" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT, "OPAQUE_WIN32" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT, "OPAQUE_WIN32_KMT" },
};
constexpr HandleKind kSemaphoreHandles[] {
    { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT, "D3D12_FENCE" },
    { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT, "OPAQUE_WIN32" },
    { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT, "OPAQUE_WIN32_KMT" },
};

// The device extensions the reference add-on appends to every vkCreateDevice the host makes. A
// host such as RPCS3 will not ask for these on its own, so on the real route they are injected
// by hooking vulkan-1!vkCreateDevice. Here they are only checked for presence: a driver that
// does not advertise them cannot be made to import anything, hook or no hook.
constexpr const char *kNeeded[] {
    "VK_KHR_external_memory",     "VK_KHR_external_memory_win32",
    "VK_KHR_external_semaphore",  "VK_KHR_external_semaphore_win32",
    "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2",
    "VK_KHR_timeline_semaphore",
};

bool g_anyBlocker = false;

void Line()
{
    std::printf("--------------------------------------------------------------------------\n");
}

// Every DXGI adapter and its LUID. The bridge picks the adapter the host is already rendering
// on, and shared handles only cross between devices on one physical GPU -- so a Vulkan device
// whose LUID matches no adapter here would be a different GPU, and nothing would import even
// with every extension present.
void ListDxgiAdapters(std::vector<uint64_t> &luids)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        std::printf("DXGI: no factory. Cannot correlate LUIDs.\n");
        return;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        uint64_t luid = 0;
        std::memcpy(&luid, &desc.AdapterLuid, sizeof(luid));
        luids.push_back(luid);
        std::printf("  DXGI %u: %-40ls vendor %04x  vram %llu MB  luid %016llx%s\n", i,
                    desc.Description, desc.VendorId,
                    static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20),
                    static_cast<unsigned long long>(luid),
                    (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "  (software)" : "");
        adapter.Reset();
    }
}

}  // namespace

int main()
{
    std::printf("vkprobe -- can Vulkan import D3D12 memory and fences on this driver?\n");
    Line();

    std::vector<uint64_t> dxgiLuids;
    ListDxgiAdapters(dxgiLuids);
    Line();

    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (loader == nullptr)
    {
        std::printf("vulkan-1.dll did not load. No Vulkan driver on this machine, so a Vulkan\n"
                    "host could not run here in the first place.\n");
        return 2;
    }
    auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(loader, "vkGetInstanceProcAddr"));
    if (getProc == nullptr)
    {
        std::printf("vulkan-1.dll exports no vkGetInstanceProcAddr.\n");
        return 2;
    }

    auto createInstance =
        reinterpret_cast<PFN_vkCreateInstance>(getProc(nullptr, "vkCreateInstance"));
    if (createInstance == nullptr)
    {
        std::printf("no vkCreateInstance.\n");
        return 2;
    }

    // 1.1, because vkGetPhysicalDeviceImageFormatProperties2 and
    // vkGetPhysicalDeviceExternalSemaphoreProperties are core there. Asking for 1.1 is what
    // makes them resolvable without enabling the 1.0 capability extensions by hand.
    VkApplicationInfo app {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vkprobe";
    app.pEngineName = "dlss5-neural-amd";
    app.apiVersion = (1u << 22) | (1u << 12);  // VK_API_VERSION_1_1

    VkInstanceCreateInfo ci {};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;

    VkInstance instance = nullptr;
    const int created = createInstance(&ci, nullptr, &instance);
    if (created != 0 || instance == nullptr)
    {
        std::printf("vkCreateInstance failed (VkResult %d). A driver older than Vulkan 1.1\n"
                    "would do this, and so would a broken loader install.\n", created);
        return 2;
    }

    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        getProc(instance, "vkEnumeratePhysicalDevices"));
    auto getProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        getProc(instance, "vkGetPhysicalDeviceProperties2"));
    auto enumExt = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        getProc(instance, "vkEnumerateDeviceExtensionProperties"));
    auto imageProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
        getProc(instance, "vkGetPhysicalDeviceImageFormatProperties2"));
    auto semProps = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
        getProc(instance, "vkGetPhysicalDeviceExternalSemaphoreProperties"));
    auto destroyInstance =
        reinterpret_cast<PFN_vkDestroyInstance>(getProc(instance, "vkDestroyInstance"));

    if (enumerate == nullptr || getProps2 == nullptr || enumExt == nullptr ||
        imageProps2 == nullptr || semProps == nullptr)
    {
        std::printf("a core 1.1 entry point is missing; cannot query anything.\n");
        return 2;
    }

    uint32_t count = 0;
    enumerate(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    if (count != 0)
        enumerate(instance, &count, devices.data());
    std::printf("Vulkan physical devices: %u\n", count);
    Line();

    for (VkPhysicalDevice gpu : devices)
    {
        VkPhysicalDeviceIDProperties ids {};
        ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props {};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &ids;
        getProps2(gpu, &props);

        uint32_t vendor = 0;
        std::memcpy(&vendor, props.properties + kVendorIdOffset, sizeof(vendor));
        const char *name = reinterpret_cast<const char *>(props.properties + kDeviceNameOffset);

        uint64_t luid = 0;
        std::memcpy(&luid, ids.deviceLUID, sizeof(luid));
        bool luidMatches = false;
        for (uint64_t known : dxgiLuids)
            if (known == luid)
                luidMatches = true;

        std::printf("%s   (vendor %04x%s)\n", name, vendor,
                    vendor == 0x1002   ? ", AMD"
                    : vendor == 0x10de ? ", NVIDIA"
                    : vendor == 0x8086 ? ", Intel"
                                       : "");
        if (ids.deviceLUIDValid)
            std::printf("  LUID %016llx -- %s\n", static_cast<unsigned long long>(luid),
                        luidMatches ? "matches a DXGI adapter, so D3D12 and Vulkan can be the "
                                      "same physical GPU"
                                    : "MATCHES NO DXGI ADAPTER; sharing with D3D12 is not "
                                      "possible with this device");
        else
            std::printf("  LUID not reported. Without it there is no way to prove D3D12 and "
                        "Vulkan are on one GPU.\n");

        // --- the extensions the transport needs the host's device to have been created with ---
        uint32_t extCount = 0;
        enumExt(gpu, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        if (extCount != 0)
            enumExt(gpu, nullptr, &extCount, exts.data());

        std::printf("  extensions (%u advertised):\n", extCount);
        bool missingExt = false;
        for (const char *needed : kNeeded)
        {
            bool found = false;
            for (const auto &e : exts)
                if (std::strcmp(e.extensionName, needed) == 0)
                    found = true;
            if (!found)
                missingExt = true;
            std::printf("    %-34s %s\n", needed, found ? "yes" : "MISSING");
        }

        // --- can a D3D12 fence become a Vulkan semaphore? ---
        std::printf("  fence import (D3D12 fence -> Vulkan semaphore):\n");
        bool fenceOk = false;
        for (const auto &h : kSemaphoreHandles)
        {
            VkPhysicalDeviceExternalSemaphoreInfo info {};
            info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
            info.handleType = h.bit;
            VkExternalSemaphoreProperties out {};
            out.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
            semProps(gpu, &info, &out);
            const bool importable =
                (out.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0;
            if (h.bit == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT)
                fenceOk = importable;
            std::printf("    %-18s %s\n", h.name, importable ? "IMPORTABLE" : "no");
        }

        // --- can a D3D12 texture become a VkImage, in the formats this project carries? ---
        std::printf("  texture import, per format and handle type:\n");
        std::printf("    %-8s %-22s", "use", "format");
        for (const auto &h : kMemoryHandles)
            std::printf(" %-18s", h.name);
        std::printf("\n");

        bool d3d12ResourceOk = false;
        for (const auto &p : kProbes)
        {
            std::printf("    %-8s %-22s", p.what, p.vkName);
            for (const auto &h : kMemoryHandles)
            {
                VkPhysicalDeviceExternalImageFormatInfo ext {};
                ext.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
                ext.handleType = h.bit;

                VkPhysicalDeviceImageFormatInfo2 info {};
                info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
                info.pNext = &ext;
                info.format = p.vkFormat;
                info.type = 1;    // VK_IMAGE_TYPE_2D
                info.tiling = 0;  // VK_IMAGE_TILING_OPTIMAL
                // What the transport actually does with these: copy in, copy out, and sample.
                // Asking with the real usage is the point -- a handle type the driver accepts
                // for a bare image and refuses for a sampled one would be a false yes.
                info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT;

                VkExternalImageFormatProperties extOut {};
                extOut.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
                VkImageFormatProperties2 out {};
                out.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
                out.pNext = &extOut;

                const int r = imageProps2(gpu, &info, &out);
                const VkFlags features = extOut.externalMemoryProperties.externalMemoryFeatures;
                const bool importable =
                    r == 0 && (features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
                const bool dedicated =
                    (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
                if (h.bit == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT && importable)
                    d3d12ResourceOk = true;
                std::printf(" %-18s", r != 0            ? "unsupported"
                                      : !importable     ? "no"
                                      : dedicated       ? "IMPORT (dedicated)"
                                                        : "IMPORT");
            }
            std::printf("\n");
        }

        // --- the verdict for this device ---
        if (vendor == 0x1002)
        {
            std::printf("  VERDICT: ");
            if (d3d12ResourceOk && fenceOk && !missingExt && luidMatches)
                std::printf("the Vulkan route is open on this AMD GPU. D3D12 textures import\n"
                            "           as VkImage and a D3D12 fence imports as a semaphore, so\n"
                            "           the transport the reference add-ons use can be built.\n");
            else
            {
                g_anyBlocker = true;
                std::printf("BLOCKED. ");
                if (!luidMatches)
                    std::printf("The LUID matches no DXGI adapter. ");
                if (missingExt)
                    std::printf("An interop extension is not advertised. ");
                if (!d3d12ResourceOk)
                    std::printf("D3D12_RESOURCE textures cannot be imported. ");
                if (!fenceOk)
                    std::printf("D3D12 fences cannot be imported. ");
                std::printf("\n           Read the D3D11_TEXTURE and OPAQUE_WIN32 columns before\n"
                            "           concluding anything: a different handle type being\n"
                            "           importable means a different transport, not no transport.\n");
            }
        }
        Line();
    }

    if (destroyInstance != nullptr)
        destroyInstance(instance, nullptr);

    std::printf("Read the AMD rows. IMPORT under D3D12_RESOURCE plus IMPORTABLE under\n"
                "D3D12_FENCE is the whole precondition; everything else in a Vulkan port is\n"
                "work, not risk.\n");
    return g_anyBlocker ? 1 : 0;
}
