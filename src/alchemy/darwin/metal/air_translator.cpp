// The AIR translator through its C ABI (air_translator.hpp). The entry point
// types below are this file's own declarations of the ABI that
// external/m2v-capi documents; nothing of the translator is compiled in.
#include "alchemy/darwin/metal/air_translator.hpp"

#include "alchemy/host/gpu.hpp"

#include <array>
#include <map>
#include <mutex>

namespace alchemy::metal::air {

namespace {

constexpr std::uint32_t abi_version = 1;

struct Api {
    std::uint32_t (*abi_version)();
    const char* (*last_error)();
    void (*string_free)(char*);
    DeviceHandle* (*device_create_vulkan)(PFN_vkGetInstanceProcAddr, void* instance, void* physical, std::uint32_t instance_api_version,
                                          const void* enabled_features, const char* const* extensions, std::uint32_t extension_count,
                                          const char* policy_json);
    char* (*device_json)(const DeviceHandle*);
    void (*device_destroy)(DeviceHandle*);
    LibraryHandle* (*library_parse)(const std::uint8_t*, std::size_t);
    char* (*library_json)(const LibraryHandle*);
    void (*library_destroy)(LibraryHandle*);
    PipelineHandle* (*pipeline_compile)(const DeviceHandle*, const LibraryHandle*, const char* descriptor_json);
    char* (*pipeline_json)(const PipelineHandle*);
    const std::uint32_t* (*pipeline_spirv)(const PipelineHandle*, std::uint32_t stage, std::size_t* words);
    const std::uint8_t* (*pipeline_constant_blob)(const PipelineHandle*, std::uint32_t stage, std::size_t* size);
    void (*pipeline_destroy)(PipelineHandle*);
    char* (*metal_enum_name)(const char* type_name, std::uint32_t raw);
    char* (*template_json)(const char* type_name);
    char* (*sampler_desc)(const DeviceHandle*, const char* descriptor_json, const char* use_json, std::int64_t image_format);
    char* (*bind_shape)(const char* state_json);
    PlanHandle* (*plan_create)(const PipelineHandle*, const DeviceHandle*, const char* state_json);
    char* (*plan_json)(const PlanHandle*);
    char* (*plan_dispatch)(const PlanHandle*, const char* state_json, const char* dispatch_json);
    char* (*plan_draw)(const PlanHandle*, const char* state_json, const char* draw_json);
    void (*plan_destroy)(PlanHandle*);
};

struct Loaded {
    Api api{};
    std::string error;   // empty: loaded
};

const Loaded& loaded() {
    static const Loaded result = [] {
        Loaded out;
        const auto resolve = [&out](auto& slot, const char* name) {
            if (!out.error.empty()) return;
            const auto entry = host::gpu::air_translator_symbol(name);
            if (!entry) {
                out.error = entry.error() == host::Error::not_found
                                ? "no AIR translator (m2v_capi.dll; set ALCHEMY_M2V_CAPI or place it next to the executable)"
                                : std::string("the AIR translator lacks ") + name;
                return;
            }
            slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(*entry);
        };
        auto& a = out.api;
        resolve(a.abi_version, "m2v_abi_version");
        if (out.error.empty() && a.abi_version() != abi_version)
            out.error = "the AIR translator speaks ABI " + std::to_string(a.abi_version()) + ", not " + std::to_string(abi_version);
        resolve(a.last_error, "m2v_last_error");
        resolve(a.string_free, "m2v_string_free");
        resolve(a.device_create_vulkan, "m2v_device_create_vulkan");
        resolve(a.device_json, "m2v_device_json");
        resolve(a.device_destroy, "m2v_device_destroy");
        resolve(a.library_parse, "m2v_library_parse");
        resolve(a.library_json, "m2v_library_json");
        resolve(a.library_destroy, "m2v_library_destroy");
        resolve(a.pipeline_compile, "m2v_pipeline_compile");
        resolve(a.pipeline_json, "m2v_pipeline_json");
        resolve(a.pipeline_spirv, "m2v_pipeline_spirv");
        resolve(a.pipeline_constant_blob, "m2v_pipeline_constant_blob");
        resolve(a.pipeline_destroy, "m2v_pipeline_destroy");
        resolve(a.metal_enum_name, "m2v_metal_enum_name");
        resolve(a.template_json, "m2v_template");
        resolve(a.sampler_desc, "m2v_sampler_desc");
        resolve(a.bind_shape, "m2v_bind_shape");
        resolve(a.plan_create, "m2v_plan_create");
        resolve(a.plan_json, "m2v_plan_json");
        resolve(a.plan_dispatch, "m2v_plan_dispatch");
        resolve(a.plan_draw, "m2v_plan_draw");
        resolve(a.plan_destroy, "m2v_plan_destroy");
        return out;
    }();
    return result;
}

const Api& api() {
    const auto& l = loaded();
    if (!l.error.empty()) throw TranslatorError("unavailable", l.error);
    return l.api;
}

[[noreturn]] void fail(const Api& a, const char* call) {
    const char* text = a.last_error();
    try {
        const auto error = json::Value::parse(text ? text : "");
        const auto* kind = error["kind"].as_string();
        const auto* detail = error["detail"].as_string();
        throw TranslatorError(kind ? *kind : "internal", std::string(call) + ": " + (detail ? *detail : "no detail"));
    } catch (const std::runtime_error& e) {
        if (dynamic_cast<const TranslatorError*>(&e)) throw;
        throw TranslatorError("internal", std::string(call) + ": unreadable error");
    }
}

// Takes a string the translator returned: parsed, then freed.
json::Value take(const Api& a, char* text, const char* call) {
    if (!text) fail(a, call);
    std::string copy(text);
    a.string_free(text);
    return json::Value::parse(copy);
}

} // namespace

bool available(std::string* reason) {
    const auto& l = loaded();
    if (reason) *reason = l.error;
    return l.error.empty();
}

// ---- device --------------------------------------------------------------------------

TranslatorDevice::TranslatorDevice(Device& device) {
    const auto& a = api();
    // The feature chain vkCreateDevice was given (device.cpp), rebuilt from
    // what the Device records.
    VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enabled.features = device.features10;
    auto v11 = device.features11;
    auto v12 = device.features12;
    auto v13 = device.features13;
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT restart{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR local_read{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
    enabled.pNext = &v11;
    v11.pNext = &v12;
    v12.pNext = &v13;
    v13.pNext = nullptr;
    void** tail = &v13.pNext;
    if (device.has_device_extension(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME)) {
        barycentric.fragmentShaderBarycentric = device.fragment_shader_barycentric;
        *tail = &barycentric; tail = &barycentric.pNext;
    }
    if (device.has_device_extension(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME)) {
        restart.primitiveTopologyListRestart = device.primitive_topology_list_restart;
        *tail = &restart; tail = &restart.pNext;
    }
    if (device.dynamic_rendering_local_read) {
        local_read.dynamicRenderingLocalRead = VK_TRUE;
        *tail = &local_read; tail = &local_read.pNext;
    }
    std::vector<const char*> extensions;
    for (const auto& name : device.device_extensions) extensions.push_back(name.c_str());
    handle_ = a.device_create_vulkan(device.vk.vkGetInstanceProcAddr, device.instance, device.physical, VK_API_VERSION_1_3, &enabled,
                                     extensions.data(), static_cast<std::uint32_t>(extensions.size()), nullptr);
    if (!handle_) fail(a, "m2v_device_create_vulkan");
    try {
        info_ = take(a, a.device_json(handle_), "m2v_device_json");
    } catch (...) {
        a.device_destroy(handle_);
        throw;
    }
}

TranslatorDevice::~TranslatorDevice() {
    if (handle_) loaded().api.device_destroy(handle_);
}

const TranslatorDevice& TranslatorDevice::of(Device& device) {
    struct Holder {
        std::shared_ptr<TranslatorDevice> device;
        std::string kind, error;
        void cleanup() { device.reset(); }
    };
    auto& holder = device.extension<Holder>("air-translator", [&device] {
        auto made = std::make_shared<Holder>();
        try {
            made->device = std::make_shared<TranslatorDevice>(device);
        } catch (const TranslatorError& e) {
            made->kind = e.kind();
            made->error = e.what();
        }
        return made;
    });
    if (!holder.device) throw TranslatorError(holder.kind, holder.error);
    return *holder.device;
}

// ---- library -------------------------------------------------------------------------

Library::Library(std::span<const std::uint8_t> bytes) {
    const auto& a = api();
    handle_ = a.library_parse(bytes.data(), bytes.size());
    if (!handle_) fail(a, "m2v_library_parse");
    try {
        functions_ = take(a, a.library_json(handle_), "m2v_library_json");
    } catch (...) {
        a.library_destroy(handle_);
        throw;
    }
}

Library::~Library() {
    if (handle_) loaded().api.library_destroy(handle_);
}

const json::Value* Library::function(std::string_view name) const {
    for (const auto& f : functions_.items()) {
        const auto* n = f["name"].as_string();
        if (n && *n == name) return &f;
    }
    return nullptr;
}

// ---- pipeline ------------------------------------------------------------------------

Pipeline::Pipeline(const TranslatorDevice& device, const Library& library, const json::Value& descriptor) {
    const auto& a = api();
    handle_ = a.pipeline_compile(device.handle(), library.handle(), descriptor.dump().c_str());
    if (!handle_) fail(a, "m2v_pipeline_compile");
    try {
        info_ = take(a, a.pipeline_json(handle_), "m2v_pipeline_json");
    } catch (...) {
        a.pipeline_destroy(handle_);
        throw;
    }
}

Pipeline::~Pipeline() {
    if (handle_) loaded().api.pipeline_destroy(handle_);
}

bool Pipeline::compute() const {
    const auto* kind = info_["kind"].as_string();
    return kind && *kind == "compute";
}

std::span<const std::uint32_t> Pipeline::spirv(std::size_t stage) const {
    const auto& a = api();
    std::size_t words{};
    const auto* data = a.pipeline_spirv(handle_, static_cast<std::uint32_t>(stage), &words);
    if (!data) fail(a, "m2v_pipeline_spirv");
    return {data, words};
}

std::span<const std::uint8_t> Pipeline::constant_blob(std::size_t stage) const {
    const auto& a = api();
    std::size_t size{};
    const auto* data = a.pipeline_constant_blob(handle_, static_cast<std::uint32_t>(stage), &size);
    return data ? std::span<const std::uint8_t>(data, size) : std::span<const std::uint8_t>{};
}

// ---- plan ----------------------------------------------------------------------------

Plan::Plan(const Pipeline& pipeline, const TranslatorDevice& device, const json::Value& state) {
    const auto& a = api();
    handle_ = a.plan_create(pipeline.handle(), device.handle(), state.dump().c_str());
    if (!handle_) fail(a, "m2v_plan_create");
    try {
        info_ = take(a, a.plan_json(handle_), "m2v_plan_json");
    } catch (...) {
        a.plan_destroy(handle_);
        throw;
    }
}

Plan::~Plan() {
    if (handle_) loaded().api.plan_destroy(handle_);
}

json::Value Plan::dispatch(const json::Value& state, const json::Value& dispatch) const {
    const auto& a = api();
    return take(a, a.plan_dispatch(handle_, state.dump().c_str(), dispatch.dump().c_str()), "m2v_plan_dispatch");
}

json::Value Plan::draw(const json::Value& state, const json::Value& draw) const {
    const auto& a = api();
    return take(a, a.plan_draw(handle_, state.dump().c_str(), draw.dump().c_str()), "m2v_plan_draw");
}

json::Value metal_enum(const char* type, std::uint32_t raw) {
    static std::mutex mutex;
    static std::map<std::pair<std::string, std::uint32_t>, json::Value> cache;
    std::lock_guard lock(mutex);
    const auto key = std::make_pair(std::string(type), raw);
    if (const auto found = cache.find(key); found != cache.end()) return found->second;
    const auto& a = api();
    auto value = take(a, a.metal_enum_name(type, raw), "m2v_metal_enum_name");
    cache.emplace(key, value);
    return value;
}

json::Value template_of(const char* type) {
    const auto& a = api();
    return take(a, a.template_json(type), "m2v_template");
}

json::Value sampler_desc(const TranslatorDevice& device, const json::Value& descriptor, const json::Value& use,
                         std::int64_t image_format) {
    const auto& a = api();
    return take(a, a.sampler_desc(device.handle(), descriptor.dump().c_str(), use.dump().c_str(), image_format), "m2v_sampler_desc");
}

std::string bind_shape(const json::Value& state) {
    const auto& a = api();
    char* text = a.bind_shape(state.dump().c_str());
    if (!text) fail(a, "m2v_bind_shape");
    std::string out(text);
    a.string_free(text);
    return out;
}

} // namespace alchemy::metal::air
