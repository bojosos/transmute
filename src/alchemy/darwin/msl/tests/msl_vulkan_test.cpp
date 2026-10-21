// MSL compiler end-to-end test (owner: A6): compiles
// fixtures/vulkan_compute.metal, runs it on a Vulkan 1.3 device under the
// fixed resource model (argument table of buffer device addresses at set 0
// binding 0, function constants and the workgroup/threadgroup sizes as
// specialization constants) and compares the results with a host reference.
// A second kernel checks the kernel grid push constants: a non-uniform
// dispatchThreads: grid (threads past it must not run) and an indirect-style
// dispatch without a pushed grid ([[threads_per_grid]] from the workgroups).
// vulkan-1.dll is loaded dynamically; without a suitable device the test
// prints why and passes (skipped), as the GUI tests do headless.
#include "msl_test_support.hpp"

#include <cmath>
#include <cstring>
#include <deque>
#include <memory>

#if defined(_WIN32) && defined(ALCHEMY_MSL_HAVE_VULKAN_HEADERS)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#define ALCHEMY_MSL_VULKAN_TEST 1
#endif

namespace alchemy::msl::test {

#ifndef ALCHEMY_MSL_VULKAN_TEST
int run_vulkan_test() {
    std::cout << "vulkan: SKIPPED (built without Vulkan headers or not on Windows)\n";
    return 0;
}
#else
namespace {

struct Skip : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed (VkResult " + std::to_string(result) + ")");
}

struct Api {
    PFN_vkGetInstanceProcAddr get_instance_proc{};
    PFN_vkCreateInstance create_instance{};
#define ALCHEMY_VK_INSTANCE(F) PFN_##F F{};
#define ALCHEMY_VK_INSTANCE_FUNCTIONS(X)                                                                          \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                           \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties)                                   \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
    ALCHEMY_VK_INSTANCE_FUNCTIONS(ALCHEMY_VK_INSTANCE)
#define ALCHEMY_VK_DEVICE_FUNCTIONS(X)                                                                            \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements)  \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkGetBufferDeviceAddress)          \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout)                               \
    X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout)                          \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool)         \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkDestroyCommandPool)          \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdBindPipeline)                \
    X(vkCmdBindDescriptorSets) X(vkCmdDispatch) X(vkCmdPushConstants) X(vkQueueSubmit) X(vkQueueWaitIdle)
    ALCHEMY_VK_DEVICE_FUNCTIONS(ALCHEMY_VK_INSTANCE)
#undef ALCHEMY_VK_INSTANCE
};

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* data{};
    VkDeviceAddress address{};
};

class Harness {
public:
    Harness() {
        library_ = LoadLibraryW(L"vulkan-1.dll");
        if (!library_) throw Skip("vulkan-1.dll not found");
        api_.get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library_, "vkGetInstanceProcAddr"));
        if (!api_.get_instance_proc) throw Skip("vulkan-1.dll lacks vkGetInstanceProcAddr");
        api_.create_instance = reinterpret_cast<PFN_vkCreateInstance>(api_.get_instance_proc(nullptr, "vkCreateInstance"));
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "alchemy_msl_tests";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        if (api_.create_instance(&info, nullptr, &instance_) != VK_SUCCESS) throw Skip("vkCreateInstance failed");
#define ALCHEMY_VK_LOAD(F) api_.F = reinterpret_cast<PFN_##F>(api_.get_instance_proc(instance_, #F));
        ALCHEMY_VK_INSTANCE_FUNCTIONS(ALCHEMY_VK_LOAD)
#undef ALCHEMY_VK_LOAD
        pick_device();
#define ALCHEMY_VK_LOAD(F) api_.F = reinterpret_cast<PFN_##F>(api_.vkGetDeviceProcAddr(device_, #F));
        ALCHEMY_VK_DEVICE_FUNCTIONS(ALCHEMY_VK_LOAD)
#undef ALCHEMY_VK_LOAD
        api_.vkGetDeviceQueue(device_, family_, 0, &queue_);
    }
    ~Harness() {
        if (device_) {
            for (auto& b : buffers_) {
                api_.vkDestroyBuffer(device_, b.buffer, nullptr);
                api_.vkFreeMemory(device_, b.memory, nullptr);
            }
            api_.vkDestroyDevice(device_, nullptr);
        }
        if (instance_) api_.vkDestroyInstance(instance_, nullptr);
    }
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    const Api& api() const { return api_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    std::uint32_t family() const { return family_; }
    const std::string& name() const { return name_; }

    Buffer& buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
        Buffer b;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        check(api_.vkCreateBuffer(device_, &info, nullptr, &b.buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        api_.vkGetBufferMemoryRequirements(device_, b.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory{};
        api_.vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
        std::uint32_t type = ~0U;
        const auto wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1U << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
                type = i;
                break;
            }
        require(type != ~0U, "no host-visible coherent memory type");
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = &flags;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        check(api_.vkAllocateMemory(device_, &allocate, nullptr, &b.memory), "vkAllocateMemory");
        check(api_.vkBindBufferMemory(device_, b.buffer, b.memory, 0), "vkBindBufferMemory");
        check(api_.vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.data), "vkMapMemory");
        std::memset(b.data, 0, static_cast<std::size_t>(size));
        VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        address.buffer = b.buffer;
        b.address = api_.vkGetBufferDeviceAddress(device_, &address);
        buffers_.push_back(b);
        return buffers_.back();
    }

private:
    void pick_device() {
        std::uint32_t count = 0;
        api_.vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        api_.vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        std::string reasons;
        for (auto* candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            api_.vkGetPhysicalDeviceProperties(candidate, &properties);
            VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &v12;
            api_.vkGetPhysicalDeviceFeatures2(candidate, &features);
            if (properties.apiVersion < VK_API_VERSION_1_3 || !v12.bufferDeviceAddress || !features.features.shaderInt64) {
                reasons += std::string(" ") + properties.deviceName + " (needs Vulkan 1.3, bufferDeviceAddress, shaderInt64);";
                continue;
            }
            std::uint32_t families = 0;
            api_.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> family_properties(families);
            api_.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, family_properties.data());
            for (std::uint32_t i = 0; i < families; ++i) {
                if (!(family_properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
                physical_ = candidate;
                family_ = i;
                name_ = properties.deviceName;
                break;
            }
            if (physical_) break;
        }
        if (!physical_) throw Skip("no suitable Vulkan device:" + (reasons.empty() ? std::string(" none") : reasons));
        float priority = 1.0F;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = family_;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        v12.bufferDeviceAddress = VK_TRUE;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &v12;
        features.features.shaderInt64 = VK_TRUE;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.pNext = &features;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue;
        check(api_.vkCreateDevice(physical_, &info, nullptr, &device_), "vkCreateDevice");
    }

    HMODULE library_{};
    Api api_;
    VkInstance instance_{};
    VkPhysicalDevice physical_{};
    VkDevice device_{};
    VkQueue queue_{};
    std::uint32_t family_{};
    std::string name_;
    std::deque<Buffer> buffers_;   // buffer() hands out references
};

constexpr std::uint32_t element_count = 1000, group_width = 64;
constexpr float scale = 1.5F, offset_bias = 0.25F;

void run(Harness& vk) {
    const auto& compiled = compile_fixture("vulkan_compute.metal");
    const auto& function = compiled.functions.at(0);
    validate_spirv(function.spirv, "vulkan_compute.saxpy_reduce");
    const auto& reflection = function.reflection;
    require(reflection.argument_table.set == 0 && reflection.argument_table.binding == 0, "argument table location");
    const auto& api = vk.api();
    const auto device = vk.device();
    const std::uint32_t groups = (element_count + group_width - 1) / group_width;

    auto& x = vk.buffer(element_count * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto& y = vk.buffer(element_count * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto& out = vk.buffer(element_count * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto& params = vk.buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto& sums = vk.buffer(groups * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto& odd = vk.buffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto& table = vk.buffer(8 * maximum_buffers + 8, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    auto* xs = static_cast<float*>(x.data);
    auto* ys = static_cast<float*>(y.data);
    for (std::uint32_t i = 0; i < element_count; ++i) {
        xs[i] = static_cast<float>(i % 97) * 0.03125F - 1.0F;
        ys[i] = static_cast<float>(i % 13) * 0.75F;
    }
    std::memcpy(params.data, &scale, 4);
    std::memcpy(static_cast<char*>(params.data) + 4, &element_count, 4);
    // Argument table: buffer(n)'s device address at byte 8 * n.
    auto* slots = static_cast<VkDeviceAddress*>(table.data);
    const Buffer* bound[] = {&x, &y, &out, &params, &sums, &odd};
    for (std::uint32_t n = 0; n < 6; ++n) slots[n] = bound[n]->address;
    for (const auto& b : reflection.buffers) require(b.table_offset() == 8 * b.index && b.index < 6, "buffer slots");

    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = function.spirv.size() * 4;
    module_info.pCode = function.spirv.data();
    VkShaderModule module{};
    check(api.vkCreateShaderModule(device, &module_info, nullptr, &module), "vkCreateShaderModule");
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = argument_table_binding;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    VkDescriptorSetLayout set_layout{};
    check(api.vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &set_layout), "vkCreateDescriptorSetLayout");
    // Every kernel pipeline layout carries the grid push constants (A5 does the same).
    const VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, kernel_push_constant_size};
    VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    VkPipelineLayout pipeline_layout{};
    check(api.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout), "vkCreatePipelineLayout");

    // Specialization: workgroup size, threadgroup(0) length, function constants.
    struct Spec {
        std::uint32_t wx, wy, wz, tg0;
        float bias;
        VkBool32 use_sin;
    } spec{group_width, 1, 1, group_width * 4, offset_bias, VK_TRUE};
    const VkSpecializationMapEntry entries[] = {
        {reserved_spec_id::workgroup_size_x, offsetof(Spec, wx), 4},
        {reserved_spec_id::workgroup_size_y, offsetof(Spec, wy), 4},
        {reserved_spec_id::workgroup_size_z, offsetof(Spec, wz), 4},
        {reserved_spec_id::threadgroup_memory_base + 0, offsetof(Spec, tg0), 4},
        {0, offsetof(Spec, bias), 4},
        {1, offsetof(Spec, use_sin), 4},
        // is_function_constant_defined(offset_bias), as Metal's pipeline sets it.
        {reserved_spec_id::function_constant_defined_base + 0, offsetof(Spec, use_sin), 4},
    };
    VkSpecializationInfo specialization{6, entries, sizeof(spec), &spec};
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = function.name.c_str();
    pipeline_info.stage.pSpecializationInfo = &specialization;
    pipeline_info.layout = pipeline_layout;
    VkPipeline pipeline{};
    check(api.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline),
          "vkCreateComputePipelines");

    // grid_guard: workgroup size only.
    const auto& guard = compiled.functions.at(1);
    require(guard.name == "grid_guard", "second fixture kernel is grid_guard");
    validate_spirv(guard.spirv, "vulkan_compute.grid_guard");
    VkShaderModuleCreateInfo guard_module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    guard_module_info.codeSize = guard.spirv.size() * 4;
    guard_module_info.pCode = guard.spirv.data();
    VkShaderModule guard_module{};
    check(api.vkCreateShaderModule(device, &guard_module_info, nullptr, &guard_module), "vkCreateShaderModule");
    VkSpecializationInfo guard_specialization{3, entries, sizeof(spec), &spec};
    pipeline_info.stage.module = guard_module;
    pipeline_info.stage.pName = guard.name.c_str();
    pipeline_info.stage.pSpecializationInfo = &guard_specialization;
    VkPipeline guard_pipeline{};
    check(api.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &guard_pipeline),
          "vkCreateComputePipelines (grid_guard)");
    // derived_constants: program-scope constants computed from the
    // specialized function constants (offset_bias defined, use_sin true).
    const auto& derived = compiled.functions.at(2);
    require(derived.name == "derived_constants", "third fixture kernel is derived_constants");
    validate_spirv(derived.spirv, "vulkan_compute.derived_constants");
    VkShaderModuleCreateInfo derived_module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    derived_module_info.codeSize = derived.spirv.size() * 4;
    derived_module_info.pCode = derived.spirv.data();
    VkShaderModule derived_module{};
    check(api.vkCreateShaderModule(device, &derived_module_info, nullptr, &derived_module), "vkCreateShaderModule");
    pipeline_info.stage.module = derived_module;
    pipeline_info.stage.pName = derived.name.c_str();
    VkSpecializationInfo derived_specialization{7, entries, sizeof(spec), &spec};
    pipeline_info.stage.pSpecializationInfo = &derived_specialization;
    VkPipeline derived_pipeline{};
    check(api.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &derived_pipeline),
          "vkCreateComputePipelines (derived_constants)");
    auto& derived_out = vk.buffer(group_width * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto& derived_table = vk.buffer(8 * maximum_buffers + 8, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    static_cast<VkDeviceAddress*>(derived_table.data)[0] = derived_out.address;

    const std::uint32_t padded = groups * group_width;
    auto& guarded = vk.buffer(padded * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto& unguarded = vk.buffer(padded * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(guarded.data, 0xff, padded * 4);
    std::memset(unguarded.data, 0xff, padded * 4);
    auto& guarded_table = vk.buffer(8 * maximum_buffers + 8, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    auto& unguarded_table = vk.buffer(8 * maximum_buffers + 8, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    static_cast<VkDeviceAddress*>(guarded_table.data)[0] = guarded.address;
    static_cast<VkDeviceAddress*>(unguarded_table.data)[0] = unguarded.address;

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 4;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool pool{};
    check(api.vkCreateDescriptorPool(device, &pool_info, nullptr, &pool), "vkCreateDescriptorPool");
    const VkDescriptorSetLayout set_layouts[] = {set_layout, set_layout, set_layout, set_layout};
    VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_info.descriptorPool = pool;
    set_info.descriptorSetCount = 4;
    set_info.pSetLayouts = set_layouts;
    VkDescriptorSet sets[4]{};
    check(api.vkAllocateDescriptorSets(device, &set_info, sets), "vkAllocateDescriptorSets");
    const VkDescriptorSet set = sets[0];
    const Buffer* tables[] = {&table, &guarded_table, &unguarded_table, &derived_table};
    for (int k = 0; k < 4; ++k) {
        VkDescriptorBufferInfo table_info{tables[k]->buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sets[k];
        write.dstBinding = argument_table_binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &table_info;
        api.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool_info.queueFamilyIndex = vk.family();
    VkCommandPool command_pool{};
    check(api.vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer commands{};
    check(api.vkAllocateCommandBuffers(device, &command_info, &commands), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(api.vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
    api.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    api.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                stage_descriptor_set(Stage::kernel), 1, &set, 0, nullptr);
    // dispatchThreadgroups: the grid is groups x threads, uniform.
    const std::uint32_t uniform_grid[4] = {padded, 1, 1, 0};
    api.vkCmdPushConstants(commands, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uniform_grid), uniform_grid);
    api.vkCmdDispatch(commands, groups, 1, 1);
    // grid_guard twice: dispatchThreads: of element_count threads (non-uniform),
    // then an indirect-style dispatch with no pushed grid.
    api.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, guard_pipeline);
    const std::uint32_t non_uniform_grid[4] = {element_count, 1, 1, 1};
    api.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                stage_descriptor_set(Stage::kernel), 1, &sets[1], 0, nullptr);
    api.vkCmdPushConstants(commands, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(non_uniform_grid),
                           non_uniform_grid);
    api.vkCmdDispatch(commands, groups, 1, 1);
    const std::uint32_t no_grid[4] = {0, 0, 0, 0};
    api.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                stage_descriptor_set(Stage::kernel), 1, &sets[2], 0, nullptr);
    api.vkCmdPushConstants(commands, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(no_grid), no_grid);
    api.vkCmdDispatch(commands, groups, 1, 1);
    api.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, derived_pipeline);
    api.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                stage_descriptor_set(Stage::kernel), 1, &sets[3], 0, nullptr);
    api.vkCmdPushConstants(commands, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(no_grid), no_grid);
    api.vkCmdDispatch(commands, 1, 1, 1);
    check(api.vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    check(api.vkQueueSubmit(vk.queue(), 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    check(api.vkQueueWaitIdle(vk.queue()), "vkQueueWaitIdle");

    // Host reference.
    const auto* results = static_cast<const float*>(out.data);
    std::uint32_t odd_expected = 0;
    double max_error = 0;
    std::vector<double> group_sums(groups, 0.0);
    for (std::uint32_t i = 0; i < element_count; ++i) {
        float expected = scale * xs[i] + ys[i] + offset_bias + std::sin(xs[i]);
        expected += 0.5F * static_cast<float>(i & 3U);
        const double error = std::fabs(static_cast<double>(results[i]) - expected);
        max_error = std::max(max_error, error);
        require(error < 2e-3, "out[" + std::to_string(i) + "] = " + std::to_string(results[i]) + ", expected " +
                                  std::to_string(expected));
        if (static_cast<std::int32_t>(results[i]) & 1) ++odd_expected;   // the GPU's own values decide parity
        group_sums[i / group_width] += results[i];
    }
    const auto* gpu_sums = static_cast<const float*>(sums.data);
    for (std::uint32_t g = 0; g < groups; ++g)
        require(std::fabs(gpu_sums[g] - group_sums[g]) < 1e-2 + 1e-4 * std::fabs(group_sums[g]),
                "threadgroup sum " + std::to_string(g) + " = " + std::to_string(gpu_sums[g]) + ", expected " +
                    std::to_string(group_sums[g]));
    const auto odd_count = *static_cast<const std::uint32_t*>(odd.data);
    require(odd_count == odd_expected, "atomic count " + std::to_string(odd_count) + ", expected " +
                                           std::to_string(odd_expected));
    // Grid guard: threads at or past element_count never ran; threads_per_grid
    // is the pushed grid, or the whole workgroup grid without one.
    const auto* g = static_cast<const std::uint32_t*>(guarded.data);
    const auto* u = static_cast<const std::uint32_t*>(unguarded.data);
    for (std::uint32_t i = 0; i < padded; ++i) {
        const std::uint32_t want = i < element_count ? element_count * 4096U + i : 0xffffffffU;
        require(g[i] == want, "non-uniform grid: out[" + std::to_string(i) + "] = " + std::to_string(g[i]) +
                                  ", expected " + std::to_string(want));
        require(u[i] == padded * 4096U + i, "indirect-style grid: out[" + std::to_string(i) + "] = " +
                                                std::to_string(u[i]) + ", expected " + std::to_string(padded * 4096U + i));
    }
    // derived_bias = offset_bias * 4 (defined), derived_flag = use_sin && derived_bias > 0.5.
    const auto* d = static_cast<const float*>(derived_out.data);
    for (std::uint32_t i = 0; i < group_width; ++i) {
        const float want = offset_bias * 4.0F + static_cast<float>(i);
        require(d[i] == want, "derived function constants: out[" + std::to_string(i) + "] = " + std::to_string(d[i]) +
                                  ", expected " + std::to_string(want));
    }

    api.vkDestroyCommandPool(device, command_pool, nullptr);
    api.vkDestroyDescriptorPool(device, pool, nullptr);
    api.vkDestroyPipeline(device, pipeline, nullptr);
    api.vkDestroyPipeline(device, guard_pipeline, nullptr);
    api.vkDestroyShaderModule(device, guard_module, nullptr);
    api.vkDestroyPipeline(device, derived_pipeline, nullptr);
    api.vkDestroyShaderModule(device, derived_module, nullptr);
    api.vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    api.vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    api.vkDestroyShaderModule(device, module, nullptr);
    std::cout << "vulkan: ok on " << vk.name() << " (" << element_count << " elements, " << groups
              << " threadgroups, max |error| " << max_error << ", " << odd_count << " odd; grid guard "
              << element_count << "/" << padded << " threads)\n";
}

} // namespace

int run_vulkan_test() {
    std::unique_ptr<Harness> vk;   // outlives the handlers below
    try {
        vk = std::make_unique<Harness>();
        run(*vk);
    } catch (const Skip& skip) {
        std::cout << "vulkan: SKIPPED (" << skip.what() << ")\n";
    } catch (const std::exception& error) {
        // Exit without tearing the device down: objects of the failed run
        // may still be alive, and a driver crash would hide the message.
        std::cerr << "FAILED (vulkan): " << error.what() << std::endl;
        std::_Exit(1);
    }
    return 0;
}
#endif

} // namespace alchemy::msl::test
