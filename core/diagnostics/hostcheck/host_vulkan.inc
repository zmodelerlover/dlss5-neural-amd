// hostcheck's Vulkan host: a WSI swapchain whose acquired image gets the frame copied in from a
// host-visible buffer before every present. ReShade arrives as the implicit layer, and amd-nr adds
// its interop extensions from its own vkCreateDevice hook, so this asks for nothing but WSI.
//
// The WSI declarations live here rather than in vk_raw.inc: the add-on never creates a surface.
#include "../../shared/vk_raw.inc"

namespace hostcheck::vkwsi {
using namespace vkraw;
using VkSurfaceKHR = uint64_t;
using VkSwapchainKHR = uint64_t;
constexpr auto kSurfaceInfo = static_cast<VkStructureType>(1000009000);
constexpr auto kSwapchainInfo = static_cast<VkStructureType>(1000001000);
constexpr auto kPresentInfo = static_cast<VkStructureType>(1000001001);
constexpr int kLayoutPresentSrc = 1000001002;
struct VkExtent2D { uint32_t width, height; };
struct VkWin32SurfaceCreateInfoKHR
{
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    HINSTANCE hinstance;
    HWND hwnd;
};
struct VkSurfaceCapabilitiesKHR
{
    uint32_t minImageCount, maxImageCount;
    VkExtent2D currentExtent, minImageExtent, maxImageExtent;
    uint32_t maxImageArrayLayers;
    VkFlags supportedTransforms, currentTransform, supportedCompositeAlpha, supportedUsageFlags;
};
struct VkSurfaceFormatKHR { int format, colorSpace; };
struct VkSwapchainCreateInfoKHR
{
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    VkSurfaceKHR surface;
    uint32_t minImageCount;
    int imageFormat, imageColorSpace;
    VkExtent2D imageExtent;
    uint32_t imageArrayLayers;
    VkFlags imageUsage;
    int imageSharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
    VkFlags preTransform, compositeAlpha;
    int presentMode;
    VkBool32 clipped;
    VkSwapchainKHR oldSwapchain;
};
struct VkPresentInfoKHR
{
    VkStructureType sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore *pWaitSemaphores;
    uint32_t swapchainCount;
    const VkSwapchainKHR *pSwapchains;
    const uint32_t *pImageIndices;
    int *pResults;
};
using PFN_CreateSurface = int(__stdcall *)(VkInstance, const VkWin32SurfaceCreateInfoKHR *,
                                           const void *, VkSurfaceKHR *);
using PFN_SurfaceSupport = int(__stdcall *)(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *);
using PFN_SurfaceCaps = int(__stdcall *)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *);
using PFN_SurfaceFormats = int(__stdcall *)(VkPhysicalDevice, VkSurfaceKHR, uint32_t *,
                                            VkSurfaceFormatKHR *);
using PFN_CreateSwapchain = int(__stdcall *)(VkDevice, const VkSwapchainCreateInfoKHR *,
                                             const void *, VkSwapchainKHR *);
using PFN_SwapchainImages = int(__stdcall *)(VkDevice, VkSwapchainKHR, uint32_t *, VkImage *);
using PFN_Acquire = int(__stdcall *)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence,
                                     uint32_t *);
using PFN_Present = int(__stdcall *)(VkQueue, const VkPresentInfoKHR *);
} // namespace hostcheck::vkwsi

int hostcheck::RunVulkan(const Image &img)
{
    using namespace vkwsi;
    auto fail = [](const char *what) {
        std::fprintf(stderr, "%s failed\n", what);
        return 3;
    };
    Api vk;
    if (!vk.LoadCore())
        return fail("vulkan-1.dll");
    const char *instExt[] { "VK_KHR_surface", "VK_KHR_win32_surface" };
    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "hostcheck";
    app.apiVersion = (1u << 22) | (2u << 12); // 1.2
    VkInstanceCreateInfo ici { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instExt;
    VkInstance inst = nullptr;
    if (vk.createInstance(&ici, nullptr, &inst) != 0)
        return fail("vkCreateInstance");
    auto inst_fn = [&](const char *n) { return vk.getInstanceProc(inst, n); };
    auto createSurface = reinterpret_cast<PFN_CreateSurface>(inst_fn("vkCreateWin32SurfaceKHR"));
    auto support = reinterpret_cast<PFN_SurfaceSupport>(inst_fn("vkGetPhysicalDeviceSurfaceSupportKHR"));
    auto caps = reinterpret_cast<PFN_SurfaceCaps>(inst_fn("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    auto formats = reinterpret_cast<PFN_SurfaceFormats>(inst_fn("vkGetPhysicalDeviceSurfaceFormatsKHR"));

    HWND wnd = MakeWindow(img.w, img.h, false);
    VkWin32SurfaceCreateInfoKHR si { kSurfaceInfo };
    si.hinstance = GetModuleHandleW(nullptr);
    si.hwnd = wnd;
    VkSurfaceKHR surface = 0;
    if (createSurface == nullptr || createSurface(inst, &si, nullptr, &surface) != 0)
        return fail("vkCreateWin32SurfaceKHR");

    // The discrete GPU, as D3D's default adapter would be; an integrated AMD part also says 0x1002.
    uint32_t count = 0;
    vk.enumeratePhysicalDevices(inst, &count, nullptr);
    std::vector<VkPhysicalDevice> pds(count);
    vk.enumeratePhysicalDevices(inst, &count, pds.data());
    VkPhysicalDevice pd = count != 0 ? pds[0] : nullptr;
    for (VkPhysicalDevice p : pds) {
        VkPhysicalDeviceProperties2 props { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        vk.getPhysicalDeviceProperties2(p, &props);
        uint32_t type = 0;
        std::memcpy(&type, props.properties + 16, 4); // deviceType
        if (type == 2)
            pd = p;
    }
    uint32_t qf = UINT32_MAX;
    vk.getQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vk.getQueueFamilyProperties(pd, &count, families.data());
    for (uint32_t i = 0; i < count && qf == UINT32_MAX; ++i) {
        VkBool32 ok = 0;
        support(pd, i, surface, &ok);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && ok)
            qf = i;
    }
    if (pd == nullptr || qf == UINT32_MAX)
        return fail("present queue");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    const char *devExt[] { "VK_KHR_swapchain" };
    VkDeviceCreateInfo dci { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExt;
    VkDevice dev = nullptr;
    if (vk.createDevice(pd, &dci, nullptr, &dev) != 0)
        return fail("vkCreateDevice");
    vk.LoadDevice(dev); // false only for want of the interop extensions, which is amd-nr's business
    auto dev_fn = [&](const char *n) { return vk.getDeviceProc(dev, n); };
    auto createSwapchain = reinterpret_cast<PFN_CreateSwapchain>(dev_fn("vkCreateSwapchainKHR"));
    auto swapImages = reinterpret_cast<PFN_SwapchainImages>(dev_fn("vkGetSwapchainImagesKHR"));
    auto acquire = reinterpret_cast<PFN_Acquire>(dev_fn("vkAcquireNextImageKHR"));
    auto present = reinterpret_cast<PFN_Present>(dev_fn("vkQueuePresentKHR"));
    VkQueue queue = nullptr;
    vk.getDeviceQueue(dev, qf, 0, &queue);

    VkSurfaceCapabilitiesKHR sc {};
    caps(pd, surface, &sc);
    uint32_t nf = 0;
    formats(pd, surface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nf);
    formats(pd, surface, &nf, fmts.data());
    int format = VK_FORMAT_B8G8R8A8_UNORM;
    for (const VkSurfaceFormatKHR &f : fmts)
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM)
            format = f.format;
    VkSwapchainCreateInfoKHR sci { kSwapchainInfo };
    sci.surface = surface;
    sci.minImageCount = std::max(2u, sc.minImageCount);
    sci.imageFormat = format;
    sci.imageColorSpace = 0; // SRGB_NONLINEAR
    sci.imageExtent = { img.w, img.h };
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.preTransform = sc.currentTransform;
    sci.compositeAlpha = 1; // OPAQUE
    sci.presentMode = 2;    // FIFO
    sci.clipped = 1;
    VkSwapchainKHR swapchain = 0;
    if (createSwapchain(dev, &sci, nullptr, &swapchain) != 0)
        return fail("vkCreateSwapchainKHR");
    uint32_t ni = 0;
    swapImages(dev, swapchain, &ni, nullptr);
    std::vector<VkImage> images(ni);
    swapImages(dev, swapchain, &ni, images.data());

    // The frame, in whichever byte order the swapchain took.
    VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = img.rgba.size();
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buffer = nullptr;
    VkMemoryRequirements req {};
    VkPhysicalDeviceMemoryProperties mp {};
    vk.getMemoryProperties(pd, &mp);
    if (vk.createBuffer(dev, &bci, nullptr, &buffer) != 0)
        return fail("vkCreateBuffer");
    vk.getBufferMemoryRequirements(dev, buffer, &req);
    VkMemoryAllocateInfo mai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = PickMemoryType(mp, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory = nullptr;
    unsigned char *mapped = nullptr;
    if (vk.allocateMemory(dev, &mai, nullptr, &memory) != 0 ||
        vk.bindBufferMemory(dev, buffer, memory, 0) != 0 ||
        vk.mapMemory(dev, memory, 0, bci.size, 0, reinterpret_cast<void **>(&mapped)) != 0)
        return fail("staging memory");
    std::memcpy(mapped, img.rgba.data(), img.rgba.size());
    if (format == VK_FORMAT_B8G8R8A8_UNORM)
        for (size_t i = 0; i < img.rgba.size(); i += 4)
            std::swap(mapped[i], mapped[i + 2]);

    VkCommandPoolCreateInfo cpi { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = qf;
    VkCommandPool pool = nullptr;
    VkCommandBuffer cmd = nullptr;
    VkCommandBufferAllocateInfo cai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkSemaphoreCreateInfo semi { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquired = nullptr, copied = nullptr;
    if (vk.createCommandPool(dev, &cpi, nullptr, &pool) != 0 ||
        (cai.commandPool = pool, cai.commandBufferCount = 1,
         vk.allocateCommandBuffers(dev, &cai, &cmd) != 0) ||
        vk.createSemaphore(dev, &semi, nullptr, &acquired) != 0 ||
        vk.createSemaphore(dev, &semi, nullptr, &copied) != 0)
        return fail("command buffer");

    auto barrier = [&](VkImage image, int from, int to, VkFlags src, VkFlags dst) {
        VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED_VALUE;
        b.image = image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vk.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    return Loop([&] {
        uint32_t index = 0;
        if (acquire(dev, swapchain, UINT64_MAX, acquired, nullptr, &index) < 0)
            return false;
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vk.beginCommandBuffer(cmd, &begin) != 0)
            return false;
        barrier(images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { img.w, img.h, 1 };
        vk.cmdCopyBufferToImage(cmd, buffer, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                &region);
        barrier(images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kLayoutPresentSrc,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
        if (vk.endCommandBuffer(cmd) != 0)
            return false;
        const VkFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &copied;
        if (vk.queueSubmit(queue, 1, &submit, nullptr) != 0)
            return false;
        VkPresentInfoKHR pi { kPresentInfo };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &copied;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        // One frame in flight: the command buffer and both semaphores are reused next time round.
        return present(queue, &pi) >= 0 && vk.queueWaitIdle(queue) == 0;
    });
}
