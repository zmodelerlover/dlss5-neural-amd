// Raw Vulkan, declared here instead of pulled from a Vulkan SDK.
//
// There is no SDK on the build machine and this needs about forty of the several thousand
// symbols in it. vulkan-1.dll is in System32 wherever a Vulkan driver is installed and exports
// every core entry point, so the loader is the dependency, not the headers.
//
// Everything below is transcribed from the specification. The structures are declared with
// natural alignment and no hand-inserted padding on purpose: the Vulkan ABI is the platform C
// ABI, so what the compiler lays out is what the driver reads. The one place that rule is bent
// is VkPhysicalDeviceProperties, which carries ~500 bytes of limits this code never touches --
// that is a sized byte block with the two read fields taken at their spec offsets.
//
// Used by src/vkbridge (the transport harness) and by the add-on's Vulkan route.

#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace vkraw
{

using VkFlags = uint32_t;
using VkBool32 = uint32_t;
using VkDeviceSize = uint64_t;

#define VKRAW_HANDLE(name) struct name##_T; using name = name##_T *
VKRAW_HANDLE(VkInstance);
VKRAW_HANDLE(VkPhysicalDevice);
VKRAW_HANDLE(VkDevice);
VKRAW_HANDLE(VkQueue);
VKRAW_HANDLE(VkCommandBuffer);
VKRAW_HANDLE(VkCommandPool);
VKRAW_HANDLE(VkImage);
VKRAW_HANDLE(VkBuffer);
VKRAW_HANDLE(VkDeviceMemory);
VKRAW_HANDLE(VkSemaphore);
VKRAW_HANDLE(VkFence);
#undef VKRAW_HANDLE

enum VkStructureType : int
{
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
    VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8,
    VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9,
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12,
    VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14,
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 = 1000059001,
    VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 = 1000059003,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 = 1000059004,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO = 1000071000,
    VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES = 1000071001,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES = 1000071004,
    VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO = 1000072001,
    VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR = 1000073000,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO = 1000076000,
    VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES = 1000076001,
    VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR = 1000078000,
    VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO = 1000127001,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES = 1000207000,
    VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO = 1000207002,
    VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO = 1000207003,
};

// VkExternalMemoryHandleTypeFlagBits / VkExternalSemaphoreHandleTypeFlagBits. D3D12_RESOURCE and
// D3D12_FENCE are the two the transport uses; the rest are here so a probe can report what else
// a driver would have accepted, which turns "no" into "no, but".
enum : VkFlags
{
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT = 0x002,
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT = 0x004,
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT = 0x008,
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT = 0x010,
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT = 0x020,
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT = 0x040,

    VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT = 0x1,
    VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT = 0x2,
    VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT = 0x4,

    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT = 0x02,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT = 0x04,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT = 0x08,

    VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT = 0x1,
    VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT = 0x2,
};

enum : VkFlags
{
    VK_QUEUE_GRAPHICS_BIT = 0x1,
    VK_QUEUE_COMPUTE_BIT = 0x2,
    VK_QUEUE_TRANSFER_BIT = 0x4,

    VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 0x01,
    VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x02,
    VK_IMAGE_USAGE_SAMPLED_BIT = 0x04,
    VK_IMAGE_USAGE_STORAGE_BIT = 0x08,
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x10,

    VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 0x01,
    VK_BUFFER_USAGE_TRANSFER_DST_BIT = 0x02,

    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x01,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x02,
    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x04,

    VK_IMAGE_ASPECT_COLOR_BIT = 0x1,

    VK_ACCESS_TRANSFER_READ_BIT = 0x0800,
    VK_ACCESS_TRANSFER_WRITE_BIT = 0x1000,
    VK_ACCESS_MEMORY_READ_BIT = 0x8000,
    VK_ACCESS_MEMORY_WRITE_BIT = 0x10000,

    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 0x00001,
    VK_PIPELINE_STAGE_TRANSFER_BIT = 0x01000,
    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT = 0x02000,
    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT = 0x10000,

    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 0x2,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x1,
};

// Queue-family transfers are not what the barriers here are for; both sides keep ownership.
constexpr uint32_t VK_QUEUE_FAMILY_IGNORED_VALUE = ~0u;

enum : int
{
    VK_IMAGE_TYPE_2D = 1,
    VK_IMAGE_TILING_OPTIMAL = 0,
    VK_SHARING_MODE_EXCLUSIVE = 0,
    VK_SAMPLE_COUNT_1_BIT = 1,
    VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0,
    VK_SEMAPHORE_TYPE_TIMELINE = 1,

    VK_IMAGE_LAYOUT_UNDEFINED = 0,
    VK_IMAGE_LAYOUT_GENERAL = 1,
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7,

    // The handful of VkFormat values this project carries. The add-on's textures are
    // R16G16B16A16_FLOAT colour, R32_FLOAT depth and R16G16_FLOAT motion, which is also what the
    // reference Vulkan implementation feeds its engine.
    VK_FORMAT_R8G8B8A8_UNORM = 37,
    VK_FORMAT_B8G8R8A8_UNORM = 44,
    VK_FORMAT_R16G16_SFLOAT = 83,
    VK_FORMAT_R16G16B16A16_SFLOAT = 97,
    VK_FORMAT_R32_SFLOAT = 100,
};

struct VkExtent3D
{
    uint32_t width, height, depth;
};
struct VkOffset3D
{
    int32_t x, y, z;
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

struct VkDeviceQueueCreateInfo
{
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float *pQueuePriorities;
};

struct VkDeviceCreateInfo
{
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
};

struct VkPhysicalDeviceTimelineSemaphoreFeatures
{
    VkStructureType sType;
    void *pNext;
    VkBool32 timelineSemaphore;
};

// See the note at the top: only vendorID (+8) and deviceName (+20) are read out of this.
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

struct VkQueueFamilyProperties
{
    VkFlags queueFlags;
    uint32_t queueCount;
    uint32_t timestampValidBits;
    VkExtent3D minImageTransferGranularity;
};

struct VkMemoryType
{
    VkFlags propertyFlags;
    uint32_t heapIndex;
};
struct VkMemoryHeap
{
    VkDeviceSize size;
    VkFlags flags;
};
struct VkPhysicalDeviceMemoryProperties
{
    uint32_t memoryTypeCount;
    VkMemoryType memoryTypes[32];
    uint32_t memoryHeapCount;
    VkMemoryHeap memoryHeaps[16];
};

struct VkExtensionProperties
{
    char extensionName[256];
    uint32_t specVersion;
};

struct VkPhysicalDeviceExternalImageFormatInfo
{
    VkStructureType sType;
    const void *pNext;
    VkFlags handleType;
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

struct VkExternalMemoryImageCreateInfo
{
    VkStructureType sType;
    const void *pNext;
    VkFlags handleTypes;
};

struct VkImageCreateInfo
{
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    int imageType;
    int format;
    VkExtent3D extent;
    uint32_t mipLevels;
    uint32_t arrayLayers;
    int samples;
    int tiling;
    VkFlags usage;
    int sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
    int initialLayout;
};

struct VkBufferCreateInfo
{
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    VkDeviceSize size;
    VkFlags usage;
    int sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
};

struct VkMemoryRequirements
{
    VkDeviceSize size;
    VkDeviceSize alignment;
    uint32_t memoryTypeBits;
};

// Windows hands the imported handle to the driver; the name field is for named shares, which
// this does not use. The handle stays owned by the caller when importing by handle -- the
// driver duplicates it -- so it still has to be closed.
struct VkImportMemoryWin32HandleInfoKHR
{
    VkStructureType sType;
    const void *pNext;
    VkFlags handleType;
    HANDLE handle;
    LPCWSTR name;
};

// The AMD driver reports DEDICATED_ONLY for every format this project carries, so the import is
// not optional about this: the allocation must name the image it backs.
struct VkMemoryDedicatedAllocateInfo
{
    VkStructureType sType;
    const void *pNext;
    VkImage image;
    VkBuffer buffer;
};

struct VkMemoryAllocateInfo
{
    VkStructureType sType;
    const void *pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeIndex;
};

struct VkSemaphoreTypeCreateInfo
{
    VkStructureType sType;
    const void *pNext;
    int semaphoreType;
    uint64_t initialValue;
};

struct VkSemaphoreCreateInfo
{
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
};

struct VkImportSemaphoreWin32HandleInfoKHR
{
    VkStructureType sType;
    const void *pNext;
    VkSemaphore semaphore;
    VkFlags flags;
    VkFlags handleType;
    HANDLE handle;
    LPCWSTR name;
};

struct VkCommandPoolCreateInfo
{
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
};

struct VkCommandBufferAllocateInfo
{
    VkStructureType sType;
    const void *pNext;
    VkCommandPool commandPool;
    int level;
    uint32_t commandBufferCount;
};

struct VkCommandBufferBeginInfo
{
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    const void *pInheritanceInfo;
};

struct VkImageSubresourceRange
{
    VkFlags aspectMask;
    uint32_t baseMipLevel, levelCount, baseArrayLayer, layerCount;
};

struct VkImageSubresourceLayers
{
    VkFlags aspectMask;
    uint32_t mipLevel, baseArrayLayer, layerCount;
};

struct VkImageMemoryBarrier
{
    VkStructureType sType;
    const void *pNext;
    VkFlags srcAccessMask;
    VkFlags dstAccessMask;
    int oldLayout;
    int newLayout;
    uint32_t srcQueueFamilyIndex;
    uint32_t dstQueueFamilyIndex;
    VkImage image;
    VkImageSubresourceRange subresourceRange;
};

struct VkBufferImageCopy
{
    VkDeviceSize bufferOffset;
    uint32_t bufferRowLength;
    uint32_t bufferImageHeight;
    VkImageSubresourceLayers imageSubresource;
    VkOffset3D imageOffset;
    VkExtent3D imageExtent;
};

struct VkImageCopy
{
    VkImageSubresourceLayers srcSubresource;
    VkOffset3D srcOffset;
    VkImageSubresourceLayers dstSubresource;
    VkOffset3D dstOffset;
    VkExtent3D extent;
};

struct VkSubmitInfo
{
    VkStructureType sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore *pWaitSemaphores;
    const VkFlags *pWaitDstStageMask;
    uint32_t commandBufferCount;
    const VkCommandBuffer *pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const VkSemaphore *pSignalSemaphores;
};

struct VkTimelineSemaphoreSubmitInfo
{
    VkStructureType sType;
    const void *pNext;
    uint32_t waitSemaphoreValueCount;
    const uint64_t *pWaitSemaphoreValues;
    uint32_t signalSemaphoreValueCount;
    const uint64_t *pSignalSemaphoreValues;
};

// ---- entry points ---------------------------------------------------------------------------

using PFN_vkVoidFunction = void(__stdcall *)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(__stdcall *)(VkInstance, const char *);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction(__stdcall *)(VkDevice, const char *);

#define VKRAW_FN(ret, name, ...) using PFN_##name = ret(__stdcall *)(__VA_ARGS__)

VKRAW_FN(int, vkCreateInstance, const VkInstanceCreateInfo *, const void *, VkInstance *);
VKRAW_FN(void, vkDestroyInstance, VkInstance, const void *);
VKRAW_FN(int, vkEnumeratePhysicalDevices, VkInstance, uint32_t *, VkPhysicalDevice *);
VKRAW_FN(void, vkGetPhysicalDeviceProperties2, VkPhysicalDevice, VkPhysicalDeviceProperties2 *);
VKRAW_FN(void, vkGetPhysicalDeviceQueueFamilyProperties, VkPhysicalDevice, uint32_t *,
         VkQueueFamilyProperties *);
VKRAW_FN(void, vkGetPhysicalDeviceMemoryProperties, VkPhysicalDevice,
         VkPhysicalDeviceMemoryProperties *);
VKRAW_FN(int, vkEnumerateDeviceExtensionProperties, VkPhysicalDevice, const char *, uint32_t *,
         VkExtensionProperties *);
VKRAW_FN(int, vkGetPhysicalDeviceImageFormatProperties2, VkPhysicalDevice,
         const VkPhysicalDeviceImageFormatInfo2 *, VkImageFormatProperties2 *);
VKRAW_FN(void, vkGetPhysicalDeviceExternalSemaphoreProperties, VkPhysicalDevice,
         const VkPhysicalDeviceExternalSemaphoreInfo *, VkExternalSemaphoreProperties *);
VKRAW_FN(int, vkCreateDevice, VkPhysicalDevice, const VkDeviceCreateInfo *, const void *,
         VkDevice *);
VKRAW_FN(void, vkDestroyDevice, VkDevice, const void *);
VKRAW_FN(void, vkGetDeviceQueue, VkDevice, uint32_t, uint32_t, VkQueue *);
VKRAW_FN(int, vkCreateImage, VkDevice, const VkImageCreateInfo *, const void *, VkImage *);
VKRAW_FN(void, vkDestroyImage, VkDevice, VkImage, const void *);
VKRAW_FN(void, vkGetImageMemoryRequirements, VkDevice, VkImage, VkMemoryRequirements *);
VKRAW_FN(int, vkAllocateMemory, VkDevice, const VkMemoryAllocateInfo *, const void *,
         VkDeviceMemory *);
VKRAW_FN(void, vkFreeMemory, VkDevice, VkDeviceMemory, const void *);
VKRAW_FN(int, vkBindImageMemory, VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
VKRAW_FN(int, vkCreateBuffer, VkDevice, const VkBufferCreateInfo *, const void *, VkBuffer *);
VKRAW_FN(void, vkDestroyBuffer, VkDevice, VkBuffer, const void *);
VKRAW_FN(void, vkGetBufferMemoryRequirements, VkDevice, VkBuffer, VkMemoryRequirements *);
VKRAW_FN(int, vkBindBufferMemory, VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
VKRAW_FN(int, vkMapMemory, VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void **);
VKRAW_FN(void, vkUnmapMemory, VkDevice, VkDeviceMemory);
VKRAW_FN(int, vkCreateSemaphore, VkDevice, const VkSemaphoreCreateInfo *, const void *,
         VkSemaphore *);
VKRAW_FN(void, vkDestroySemaphore, VkDevice, VkSemaphore, const void *);
VKRAW_FN(int, vkImportSemaphoreWin32HandleKHR, VkDevice,
         const VkImportSemaphoreWin32HandleInfoKHR *);
VKRAW_FN(int, vkCreateCommandPool, VkDevice, const VkCommandPoolCreateInfo *, const void *,
         VkCommandPool *);
VKRAW_FN(void, vkDestroyCommandPool, VkDevice, VkCommandPool, const void *);
VKRAW_FN(int, vkAllocateCommandBuffers, VkDevice, const VkCommandBufferAllocateInfo *,
         VkCommandBuffer *);
VKRAW_FN(int, vkBeginCommandBuffer, VkCommandBuffer, const VkCommandBufferBeginInfo *);
VKRAW_FN(int, vkEndCommandBuffer, VkCommandBuffer);
VKRAW_FN(void, vkCmdPipelineBarrier, VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t,
         const void *, uint32_t, const void *, uint32_t, const VkImageMemoryBarrier *);
VKRAW_FN(void, vkCmdCopyImageToBuffer, VkCommandBuffer, VkImage, int, VkBuffer, uint32_t,
         const VkBufferImageCopy *);
VKRAW_FN(void, vkCmdCopyBufferToImage, VkCommandBuffer, VkBuffer, VkImage, int, uint32_t,
         const VkBufferImageCopy *);
VKRAW_FN(void, vkCmdCopyImage, VkCommandBuffer, VkImage, int, VkImage, int, uint32_t,
         const VkImageCopy *);
VKRAW_FN(int, vkQueueSubmit, VkQueue, uint32_t, const VkSubmitInfo *, VkFence);
VKRAW_FN(int, vkQueueWaitIdle, VkQueue);
VKRAW_FN(int, vkDeviceWaitIdle, VkDevice);

#undef VKRAW_FN

// Everything the transport calls, resolved once. Core entry points come straight off
// vulkan-1.dll, which exports them as loader trampolines; the KHR ones have to come through
// vkGetDeviceProcAddr because only the device's driver has them.
struct Api
{
    HMODULE loader = nullptr;
    PFN_vkGetInstanceProcAddr getInstanceProc = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProc = nullptr;

    PFN_vkCreateInstance createInstance = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensions = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 getImageFormatProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties getExternalSemaphoreProperties = nullptr;
    PFN_vkCreateDevice createDevice = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;

    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory bindBufferMemory = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
    PFN_vkCreateSemaphore createSemaphore = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    PFN_vkImportSemaphoreWin32HandleKHR importSemaphoreWin32 = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage cmdCopyBufferToImage = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkQueueWaitIdle queueWaitIdle = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;

    template <class T> T Core(const char *name)
    {
        return reinterpret_cast<T>(GetProcAddress(loader, name));
    }

    // Loader-level: everything that can be resolved before any device exists.
    bool LoadCore()
    {
        loader = LoadLibraryW(L"vulkan-1.dll");
        if (loader == nullptr)
            return false;
        getInstanceProc = Core<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
        getDeviceProc = Core<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        createInstance = Core<PFN_vkCreateInstance>("vkCreateInstance");
        destroyInstance = Core<PFN_vkDestroyInstance>("vkDestroyInstance");
        enumeratePhysicalDevices =
            Core<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        getPhysicalDeviceProperties2 =
            Core<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2");
        getQueueFamilyProperties = Core<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            "vkGetPhysicalDeviceQueueFamilyProperties");
        getMemoryProperties =
            Core<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties");
        enumerateDeviceExtensions = Core<PFN_vkEnumerateDeviceExtensionProperties>(
            "vkEnumerateDeviceExtensionProperties");
        getImageFormatProperties2 = Core<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
            "vkGetPhysicalDeviceImageFormatProperties2");
        getExternalSemaphoreProperties = Core<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
            "vkGetPhysicalDeviceExternalSemaphoreProperties");
        createDevice = Core<PFN_vkCreateDevice>("vkCreateDevice");
        destroyDevice = Core<PFN_vkDestroyDevice>("vkDestroyDevice");
        return getInstanceProc != nullptr && getDeviceProc != nullptr && createInstance != nullptr;
    }

    template <class T> T Dev(VkDevice device, const char *name)
    {
        return reinterpret_cast<T>(getDeviceProc(device, name));
    }

    // Device-level. Called once a VkDevice exists -- ours in the harness, the host's in the
    // add-on. importSemaphoreWin32 is the one that is allowed to be null on a device created
    // without the interop extensions, and it is the tell that the vkCreateDevice hook did not
    // land.
    bool LoadDevice(VkDevice device)
    {
        getDeviceQueue = Dev<PFN_vkGetDeviceQueue>(device, "vkGetDeviceQueue");
        createImage = Dev<PFN_vkCreateImage>(device, "vkCreateImage");
        destroyImage = Dev<PFN_vkDestroyImage>(device, "vkDestroyImage");
        getImageMemoryRequirements =
            Dev<PFN_vkGetImageMemoryRequirements>(device, "vkGetImageMemoryRequirements");
        allocateMemory = Dev<PFN_vkAllocateMemory>(device, "vkAllocateMemory");
        freeMemory = Dev<PFN_vkFreeMemory>(device, "vkFreeMemory");
        bindImageMemory = Dev<PFN_vkBindImageMemory>(device, "vkBindImageMemory");
        createBuffer = Dev<PFN_vkCreateBuffer>(device, "vkCreateBuffer");
        destroyBuffer = Dev<PFN_vkDestroyBuffer>(device, "vkDestroyBuffer");
        getBufferMemoryRequirements =
            Dev<PFN_vkGetBufferMemoryRequirements>(device, "vkGetBufferMemoryRequirements");
        bindBufferMemory = Dev<PFN_vkBindBufferMemory>(device, "vkBindBufferMemory");
        mapMemory = Dev<PFN_vkMapMemory>(device, "vkMapMemory");
        unmapMemory = Dev<PFN_vkUnmapMemory>(device, "vkUnmapMemory");
        createSemaphore = Dev<PFN_vkCreateSemaphore>(device, "vkCreateSemaphore");
        destroySemaphore = Dev<PFN_vkDestroySemaphore>(device, "vkDestroySemaphore");
        importSemaphoreWin32 =
            Dev<PFN_vkImportSemaphoreWin32HandleKHR>(device, "vkImportSemaphoreWin32HandleKHR");
        createCommandPool = Dev<PFN_vkCreateCommandPool>(device, "vkCreateCommandPool");
        destroyCommandPool = Dev<PFN_vkDestroyCommandPool>(device, "vkDestroyCommandPool");
        allocateCommandBuffers =
            Dev<PFN_vkAllocateCommandBuffers>(device, "vkAllocateCommandBuffers");
        beginCommandBuffer = Dev<PFN_vkBeginCommandBuffer>(device, "vkBeginCommandBuffer");
        endCommandBuffer = Dev<PFN_vkEndCommandBuffer>(device, "vkEndCommandBuffer");
        cmdPipelineBarrier = Dev<PFN_vkCmdPipelineBarrier>(device, "vkCmdPipelineBarrier");
        cmdCopyImageToBuffer = Dev<PFN_vkCmdCopyImageToBuffer>(device, "vkCmdCopyImageToBuffer");
        cmdCopyBufferToImage = Dev<PFN_vkCmdCopyBufferToImage>(device, "vkCmdCopyBufferToImage");
        cmdCopyImage = Dev<PFN_vkCmdCopyImage>(device, "vkCmdCopyImage");
        queueSubmit = Dev<PFN_vkQueueSubmit>(device, "vkQueueSubmit");
        queueWaitIdle = Dev<PFN_vkQueueWaitIdle>(device, "vkQueueWaitIdle");
        deviceWaitIdle = Dev<PFN_vkDeviceWaitIdle>(device, "vkDeviceWaitIdle");
        return getDeviceQueue != nullptr && createImage != nullptr && allocateMemory != nullptr &&
               importSemaphoreWin32 != nullptr && queueSubmit != nullptr;
    }
};

// The device extensions the transport needs enabled at vkCreateDevice. A host such as RPCS3
// never asks for these on its own, which is why the add-on has to append them from a hook on
// vulkan-1!vkCreateDevice -- once the device exists it is too late, and the only symptom is
// vkImportSemaphoreWin32HandleKHR resolving to null.
constexpr const char *kInteropExtensions[] {
    "VK_KHR_external_memory",      "VK_KHR_external_memory_win32",
    "VK_KHR_external_semaphore",   "VK_KHR_external_semaphore_win32",
    "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2",
    "VK_KHR_timeline_semaphore",
};

// Which memory type to allocate an imported image from. The bits come from the image's own
// requirements; among those, the device-local one is the right answer, because the memory is
// already resident on the GPU -- it belongs to the D3D12 resource.
inline uint32_t PickMemoryType(const VkPhysicalDeviceMemoryProperties &props, uint32_t allowed,
                               VkFlags wanted)
{
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((allowed & (1u << i)) != 0 && (props.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    return UINT32_MAX;
}

}  // namespace vkraw
