#pragma once

// Precompiled Metal libraries (metallib, Apple's AIR) through an external
// translator, loaded at run time (owner: A5).
//
// newLibraryWithData: hands Metal a metallib, which the MSL compiler cannot
// read: it takes Metal Shading Language source. When the AIR translator
// library is present (m2v_capi.dll: external/m2v-capi, a C ABI over
// metal2vulkan; LGPL-3.0-or-later, built apart and never linked into
// Alchemy; THIRD_PARTY_NOTICES.md), such a library is translated to SPIR-V
// by it; without it, newLibraryWithData: keeps failing explicitly.
//
// The translator's ABI is opaque handles plus JSON (json.hpp): this file
// declares the entry points itself (host::gpu::air_translator_symbol
// resolves them) and wraps the handles. The translator chooses its own
// pipeline layout and binding (not the MSL contract's resource model):
// foreign_pipeline.hpp turns its output into Vulkan objects.
//
// Pure host code, like device.hpp. Thread-safe: the translator's calls are
// reentrant, and the wrappers are immutable after creation.

#include "alchemy/darwin/metal/device.hpp"
#include "alchemy/darwin/metal/json.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace alchemy::metal::air {

// A failure the translator reported: its class ("refused", "needs_input",
// "invalid", "bind", "internal", or "unavailable" when there is no
// translator) and its detail text.
class TranslatorError : public std::runtime_error {
public:
    TranslatorError(std::string kind, const std::string& detail)
        : std::runtime_error(kind + ": " + detail), kind_(std::move(kind)) {}
    [[nodiscard]] const std::string& kind() const noexcept { return kind_; }
private:
    std::string kind_;
};

// Whether the translator library loads and speaks this ABI revision; when
// not, `reason` (if given) says why. Loaded once per process.
[[nodiscard]] bool available(std::string* reason = nullptr);

struct DeviceHandle;
struct LibraryHandle;
struct PipelineHandle;
struct PlanHandle;

// The translator's view of a Device: what it enabled, and the profile the
// translator derives from that. One per Device (Device::extension).
class TranslatorDevice {
public:
    // The device's translator view, created at the first call; throws
    // TranslatorError (kind "unavailable" without the translator), the same
    // error at every call when creation failed.
    [[nodiscard]] static const TranslatorDevice& of(Device& device);
    explicit TranslatorDevice(Device& device);
    ~TranslatorDevice();
    TranslatorDevice(const TranslatorDevice&) = delete;
    TranslatorDevice& operator=(const TranslatorDevice&) = delete;

    [[nodiscard]] DeviceHandle* handle() const noexcept { return handle_; }
    // {"description", "required_enables", "neutral_kinds", "metal_properties"}
    [[nodiscard]] const json::Value& info() const noexcept { return info_; }

private:
    DeviceHandle* handle_{};
    json::Value info_;
};

// A parsed metallib.
class Library {
public:
    explicit Library(std::span<const std::uint8_t> bytes);
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    [[nodiscard]] LibraryHandle* handle() const noexcept { return handle_; }
    // [{"name", "stage": "Kernel"|"Vertex"|"Fragment"|..., "constants":
    //   [{"name", "index", "type" (MTLDataType), "required"}]}]
    [[nodiscard]] const json::Value& functions() const noexcept { return functions_; }
    // The function `name` of functions(), or null.
    [[nodiscard]] const json::Value* function(std::string_view name) const;

private:
    LibraryHandle* handle_{};
    json::Value functions_;
};

// A compiled pipeline: SPIR-V per stage and the facts to create and bind it.
class Pipeline {
public:
    // `descriptor` as m2v_pipeline_compile documents (external/m2v-capi/include/m2v_capi.h).
    Pipeline(const TranslatorDevice& device, const Library& library, const json::Value& descriptor);
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    [[nodiscard]] PipelineHandle* handle() const noexcept { return handle_; }
    // {"kind", "key", "stages": [{"stage" (VkShaderStageFlagBits), "entry_point",
    //   "words", "specialization", "required_subgroup_size", "stage_flags",
    //   "constant_blob"}], "layout", "interface", "dependencies", "variant_bits",
    //   and for a render pipeline "vertex_input" and "graphics"}
    [[nodiscard]] const json::Value& info() const noexcept { return info_; }
    [[nodiscard]] bool compute() const;
    [[nodiscard]] std::span<const std::uint32_t> spirv(std::size_t stage) const;
    [[nodiscard]] std::span<const std::uint8_t> constant_blob(std::size_t stage) const;

private:
    PipelineHandle* handle_{};
    json::Value info_;
};

// A binding plan of one pipeline for one bind shape.
class Plan {
public:
    // `state` is a serialized metal2vulkan BindState.
    Plan(const Pipeline& pipeline, const TranslatorDevice& device, const json::Value& state);
    ~Plan();
    Plan(const Plan&) = delete;
    Plan& operator=(const Plan&) = delete;

    [[nodiscard]] const json::Value& info() const noexcept { return info_; }   // the BindingPlan
    [[nodiscard]] json::Value dispatch(const json::Value& state, const json::Value& dispatch) const;
    [[nodiscard]] json::Value draw(const json::Value& state, const json::Value& draw) const;

private:
    PlanHandle* handle_{};
    json::Value info_;
};

// The serialized form of raw value `raw` of Metal enumeration `type`
// ("PixelFormat", "TextureType", ...), for building a BindState. Cached.
[[nodiscard]] json::Value metal_enum(const char* type, std::uint32_t raw);

// A serialized default to fill in: "BindState", "StageTable",
// "SamplerDescriptor", "TextureFacts", "VariantKey" (m2v_template).
[[nodiscard]] json::Value template_of(const char* type);

// The Vulkan sampler (a serialized vk::SamplerDesc) of a plan's sampler
// write: the bound sampler's serialized SamplerDescriptor, the write's use
// ({"compare", "integer", "cube_gradient", "opaque_black"}), and the VkFormat
// of the image it samples (negative: none).
[[nodiscard]] json::Value sampler_desc(const TranslatorDevice& device, const json::Value& descriptor, const json::Value& use,
                                       std::int64_t image_format);

// The shape of a BindState: the key plans are memoized by.
[[nodiscard]] std::string bind_shape(const json::Value& state);

} // namespace alchemy::metal::air
