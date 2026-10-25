/* m2v-capi: a C ABI over metal2vulkan (Metal AIR to Vulkan SPIR-V).
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (C) 2026 the Transmute authors. This header is part of m2v-capi, free software under
 * the GNU Lesser General Public License, version 3 or (at your option) any later version; see
 * COPYING.LESSER and COPYING. It is distributed WITHOUT ANY WARRANTY.
 *
 * ABI revision 1 (m2v_abi_version). Conventions:
 *   - Handles are opaque. Each has its destroy function; a pipeline or plan never outlives the
 *     device and library it was made from.
 *   - Plain data is UTF-8 JSON text, NUL-terminated. Strings the library returns are freed with
 *     m2v_string_free. Metal enumerations in descriptors are their raw values.
 *   - A function that fails returns NULL and leaves {"kind", "detail"} JSON in m2v_last_error()
 *     for the calling thread. Kinds: "refused" (a typed refusal of the input), "needs_input"
 *     (a pipeline input the compile asks for, e.g. a function-constant value), "invalid",
 *     "bind", "internal".
 *   - Every function is reentrant; handles may be used from several threads at once.
 */
#ifndef M2V_CAPI_H
#define M2V_CAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct M2vDevice M2vDevice;
typedef struct M2vLibrary M2vLibrary;
typedef struct M2vPipeline M2vPipeline;
typedef struct M2vPlan M2vPlan;
/* PFN_vkGetInstanceProcAddr, without requiring vulkan.h. */
typedef void (*M2vVoidFunction)(void);
typedef M2vVoidFunction (*M2vGetInstanceProcAddr)(void* instance, const char* name);

uint32_t m2v_abi_version(void);
const char* m2v_last_error(void);
void m2v_string_free(char* s);

/* A device as the caller created it: the VkPhysicalDeviceFeatures2 chain and extension names it
 * passed to vkCreateDevice (the description records exactly those), on `instance` (created with
 * `instance_api_version`). `policy_json` is a serialized metal2vulkan DevicePolicy or NULL. Only
 * physical-device queries run. */
M2vDevice* m2v_device_create_vulkan(M2vGetInstanceProcAddr get_instance_proc_addr, void* instance,
                                    void* physical_device, uint32_t instance_api_version,
                                    const void* enabled_features, const char* const* enabled_extensions,
                                    uint32_t enabled_extension_count, const char* policy_json);
/* A device from a Khronos profiles JSON (no GPU needed); `profile_name` may be NULL. */
M2vDevice* m2v_device_create_profile(const char* profile_json, const char* profile_name);
/* {"description", "required_enables", "neutral_kinds", "metal_properties"} */
char* m2v_device_json(const M2vDevice* device);
void m2v_device_destroy(M2vDevice* device);

/* A metallib or AIR bitcode; the bytes are copied. */
M2vLibrary* m2v_library_parse(const uint8_t* bytes, size_t size);
/* [{"name", "stage", "constants": [{"name", "index", "type" (MTLDataType), "required"}]}] */
char* m2v_library_json(const M2vLibrary* library);
void m2v_library_destroy(M2vLibrary* library);

/* Compiles a pipeline.
 *   compute: {"kind": "compute", "function", "constants", "stage_input",
 *             "max_total_threads_per_threadgroup", "threadgroup_size_is_multiple_of_execution_width",
 *             "variant"}
 *   render:  {"kind": "render", "vertex", "vertex_constants", "fragment", "fragment_constants",
 *             "color_attachments": [{"index", "format", "blending", "source_rgb", "destination_rgb",
 *             "rgb_operation", "source_alpha", "destination_alpha", "alpha_operation", "write_mask"}],
 *             "depth_format", "stencil_format", "raster_sample_count", "rasterization_enabled",
 *             "alpha_to_coverage", "alpha_to_one", "input_primitive_topology", "vertex_descriptor",
 *             "variant"}
 * A constant is {"index" or "name", "type" (MTLDataType), "bytes" (hex, little-endian)}.
 * "stage_input" and "vertex_descriptor" are serialized metal2vulkan descriptors; "variant" is a
 * plan's "variant", to compile the variant it asks for. */
M2vPipeline* m2v_pipeline_compile(const M2vDevice* device, const M2vLibrary* library, const char* descriptor_json);
/* {"kind", "key", "stages": [{"stage", "entry_point", "words", "specialization",
 *  "required_subgroup_size", "stage_flags", "constant_blob"}], "layout", "interface",
 *  "dependencies", "variant_bits", for a compute pipeline "metal_info" (maxTotalThreadsPerThreadgroup,
 *  threadExecutionWidth, ...), and for a render pipeline "vertex_input" and "graphics"} */
char* m2v_pipeline_json(const M2vPipeline* pipeline);
/* Stage `stage`'s SPIR-V words, owned by the pipeline; *words receives their count. */
const uint32_t* m2v_pipeline_spirv(const M2vPipeline* pipeline, uint32_t stage, size_t* words);
/* Stage `stage`'s constant blob, or NULL; *size receives its byte count. */
const uint8_t* m2v_pipeline_constant_blob(const M2vPipeline* pipeline, uint32_t stage, size_t* size);
void m2v_pipeline_destroy(M2vPipeline* pipeline);

/* The serialized form (a JSON string) of raw value `raw` of Metal enumeration `type_name`
 * ("PixelFormat", "TextureType", "SamplerAddressMode", ...: the name without "MTL"), for building
 * the plain data below from raw Metal values. */
char* m2v_metal_enum_name(const char* type_name, uint32_t raw);

/* A serialized default of a plain-data type, to fill in: "BindState", "StageTable" (empty),
 * "SamplerDescriptor" (Metal's default sampler), "TextureFacts" (a one-level 2D RGBA8Unorm texture),
 * "VariantKey" (the empty variant: a plan whose "variant" differs asks for that variant). */
char* m2v_template(const char* type_name);

/* The shape (serialized BindShape) of a serialized BindState: the key to memoize plans by. */
char* m2v_bind_shape(const char* state_json);
/* Plans a pipeline for the shape of a serialized BindState. */
M2vPlan* m2v_plan_create(const M2vPipeline* pipeline, const M2vDevice* device, const char* state_json);
/* The serialized BindingPlan. */
char* m2v_plan_json(const M2vPlan* plan);
/* Serialized DispatchCommands for a BindState and a Dispatch. */
char* m2v_plan_dispatch(const M2vPlan* plan, const char* state_json, const char* dispatch_json);
/* Serialized DrawCommands for a BindState and a DrawCall. */
char* m2v_plan_draw(const M2vPlan* plan, const char* state_json, const char* draw_json);
void m2v_plan_destroy(M2vPlan* plan);

/* The Vulkan sampler (serialized vk::SamplerDesc) of a plan's sampler write: the bound sampler (a
 * serialized SamplerDescriptor), the write's use ({"compare", "integer", "cube_gradient",
 * "opaque_black"}), and the VkFormat of the image it samples, negative for none. */
char* m2v_sampler_desc(const M2vDevice* device, const char* descriptor_json, const char* use_json, int64_t image_format);

#ifdef __cplusplus
}
#endif

#endif /* M2V_CAPI_H */
