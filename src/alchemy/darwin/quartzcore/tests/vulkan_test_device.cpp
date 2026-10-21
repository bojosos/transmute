// The test device queries Win32 presentation support itself (tests may use
// OS headers; QuartzCore leaves the platform surface to host::gpu).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_USE_PLATFORM_WIN32_KHR 1
#include "alchemy/darwin/quartzcore/tests/vulkan_test_device.hpp"

#include <Windows.h>

#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace alchemy::gui::quartzcore::test {
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
namespace {
void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed (VkResult " + std::to_string(result) + ")");
}
template <class F> F load(PFN_vkGetInstanceProcAddr get, VkInstance instance, const char* name) {
    return reinterpret_cast<F>(get(instance, name));
}
} // namespace

std::shared_ptr<VulkanTestDevice> VulkanTestDevice::create(std::string& why) {
    std::shared_ptr<VulkanTestDevice> self(new VulkanTestDevice());
    const auto library = LoadLibraryW(L"vulkan-1.dll");
    if (!library) { why = "vulkan-1.dll is not installed"; return nullptr; }
    self->library_ = library;
    self->get_proc_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr"));
    if (!self->get_proc_) { why = "vulkan-1.dll lacks vkGetInstanceProcAddr"; return nullptr; }
    const auto get = self->get_proc_;
    const auto create_instance = load<PFN_vkCreateInstance>(get, nullptr, "vkCreateInstance");
    const auto enumerate_layers = load<PFN_vkEnumerateInstanceLayerProperties>(get, nullptr, "vkEnumerateInstanceLayerProperties");

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "alchemy-quartzcore-tests";
    application.apiVersion = VK_API_VERSION_1_1;
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    std::vector<const char*> layers;
    // ALCHEMY_QC_VALIDATION=1 enables the Khronos validation layer when installed.
    if (const char* validation = std::getenv("ALCHEMY_QC_VALIDATION"); validation && *validation == '1') {
        std::uint32_t count{};
        enumerate_layers(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        enumerate_layers(&count, available.data());
        for (const auto& layer : available)
            if (std::string(layer.layerName) == "VK_LAYER_KHRONOS_validation") layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    if (create_instance(&info, nullptr, &self->instance_) != VK_SUCCESS) {
        why = "no Vulkan instance with VK_KHR_win32_surface";
        return nullptr;
    }
    const auto instance = self->instance_;
    const auto enumerate = load<PFN_vkEnumeratePhysicalDevices>(get, instance, "vkEnumeratePhysicalDevices");
    const auto families = load<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(get, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    const auto presentation = load<PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR>(get, instance,
        "vkGetPhysicalDeviceWin32PresentationSupportKHR");
    const auto properties = load<PFN_vkGetPhysicalDeviceProperties>(get, instance, "vkGetPhysicalDeviceProperties");
    const auto create_device = load<PFN_vkCreateDevice>(get, instance, "vkCreateDevice");
    const auto device_proc = load<PFN_vkGetDeviceProcAddr>(get, instance, "vkGetDeviceProcAddr");
    std::uint32_t count{};
    enumerate(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    enumerate(instance, &count, devices.data());
    for (const auto candidate : devices) {
        std::uint32_t family_count{};
        families(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> list(family_count);
        families(candidate, &family_count, list.data());
        for (std::uint32_t index = 0; index < family_count; ++index) {
            if ((list[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentation(candidate, index)) {
                self->physical_ = candidate;
                self->family_ = index;
                break;
            }
        }
        if (self->physical_) break;
    }
    if (!self->physical_) { why = "no Vulkan device can present to Win32 windows"; return nullptr; }
    VkPhysicalDeviceProperties described{};
    properties(self->physical_, &described);
    self->device_name = described.deviceName;
    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = self->family_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queue;
    device.enabledExtensionCount = 1;
    device.ppEnabledExtensionNames = device_extensions;
    check(create_device(self->physical_, &device, nullptr, &self->device_), "vkCreateDevice");
    const auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(device_proc(self->device_, "vkGetDeviceQueue"));
    get_queue(self->device_, self->family_, 0, &self->queue_);
    return self;
}

VulkanTestDevice::~VulkanTestDevice() {
    if (device_) {
        wait_idle();
        load<PFN_vkDestroyDevice>(get_proc_, instance_, "vkDestroyDevice")(device_, nullptr);
    }
    if (instance_) load<PFN_vkDestroyInstance>(get_proc_, instance_, "vkDestroyInstance")(instance_, nullptr);
    if (library_) FreeLibrary(static_cast<HMODULE>(library_));
}

void VulkanTestDevice::with_queue(const std::function<void(VkQueue)>& work) {
    std::lock_guard lock(queue_mutex_);
    work(queue_);
}

std::uint64_t VulkanTestDevice::wrap_drawable_texture(const std::shared_ptr<DrawableImage>& image) {
    if (wrap) return wrap(image);
    std::lock_guard lock(queue_mutex_);
    return next_fake_ += 16;
}

void VulkanTestDevice::wait_idle() {
    const auto idle = load<PFN_vkDeviceWaitIdle>(get_proc_, instance_, "vkDeviceWaitIdle");
    std::lock_guard lock(queue_mutex_);
    check(idle(device_), "vkDeviceWaitIdle");
}

void VulkanTestDevice::clear(DrawableImage& image, std::array<float, 4> rgba) {
    const auto proc = load<PFN_vkGetDeviceProcAddr>(get_proc_, instance_, "vkGetDeviceProcAddr");
#define QC_TEST_FN(name) const auto name = reinterpret_cast<PFN_##name>(proc(device_, #name));
    QC_TEST_FN(vkCreateCommandPool) QC_TEST_FN(vkDestroyCommandPool) QC_TEST_FN(vkAllocateCommandBuffers)
    QC_TEST_FN(vkBeginCommandBuffer) QC_TEST_FN(vkEndCommandBuffer) QC_TEST_FN(vkCmdPipelineBarrier)
    QC_TEST_FN(vkCmdClearColorImage) QC_TEST_FN(vkQueueSubmit) QC_TEST_FN(vkQueueWaitIdle)
#undef QC_TEST_FN
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = family_;
    VkCommandPool pool{};
    check(vkCreateCommandPool(device_, &pool_info, nullptr, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer commands{};
    check(vkAllocateCommandBuffers(device_, &allocate, &commands), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;   // clear load action: previous contents discarded
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = range;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    VkClearColorValue color{};
    for (int index = 0; index < 4; ++index) color.float32[index] = rgba[static_cast<std::size_t>(index)];
    vkCmdClearColorImage(commands, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    // Metal's submit order: the layout is recorded at submission, the
    // present that follows is ordered after it on the same queue. The pool
    // is destroyed only after the queue drained (test simplicity).
    with_queue([&](VkQueue queue) {
        check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        image.layout.store(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    });
    with_queue([&](VkQueue queue) { check(vkQueueWaitIdle(queue), "vkQueueWaitIdle"); });
    vkDestroyCommandPool(device_, pool, nullptr);
}
#endif

namespace {
LRESULT CALLBACK test_window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND) return 1;   // Vulkan owns the client area
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

std::unique_ptr<TestWindow> TestWindow::create(int width, int height, std::string& why) {
    static const ATOM atom = [] {
        WNDCLASSW type{};
        type.lpfnWndProc = test_window_procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"AlchemyQuartzCoreTest";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassW(&type);
    }();
    if (!atom) { why = "RegisterClass failed"; return nullptr; }
    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    const auto window = CreateWindowExW(0, L"AlchemyQuartzCoreTest", L"Alchemy QuartzCore test", WS_OVERLAPPEDWINDOW,
        40, 40, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) { why = "CreateWindow failed (no interactive desktop?)"; return nullptr; }
    ShowWindow(window, SW_SHOWNOACTIVATE);
    std::unique_ptr<TestWindow> result(new TestWindow());
    result->window_ = window;
    result->pump();
    return result;
}

TestWindow::~TestWindow() {
    if (window_) DestroyWindow(static_cast<HWND>(window_));
    pump();
}

void TestWindow::resize(int width, int height) {
    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(static_cast<HWND>(window_), nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    pump();
}

void TestWindow::pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

} // namespace alchemy::gui::quartzcore::test
