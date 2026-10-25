// A precompiled Metal library through the AIR translator (owner: A5): every
// pipeline of ANGLE's built-in library (its 22 kernels, 13 fragment functions
// with their vertex functions, and 2 vertex functions alone: 37) is compiled
// by m2v_capi.dll for this machine's Metal device, planned for a plausible
// binding, and created as real VkPipelines. The library is a run-time dump
// of what Chrome hands -[MTLDevice newLibraryWithData:] (ALCHEMY_MSL_DUMP's
// metallib-*.metallib); it never enters the repository.
//
// Then two of them run through the encoders (commands_foreign.cpp), checked
// against what ANGLE's public shader sources compute: clearFloatFS fills a
// render target with the color its ClearParams hold, and convertIndexU8ToU16
// widens a range of 8-bit indices.
//
// Exit code 77 (ctest SKIP) without a Vulkan 1.3 device, without the
// translator (ALCHEMY_M2V_CAPI) or without the library
// (ALCHEMY_M2V_TEST_LIBRARY).
#include "alchemy/darwin/metal/air_translator.hpp"
#include "alchemy/darwin/metal/commands.hpp"
#include "alchemy/darwin/metal/device.hpp"
#include "alchemy/darwin/metal/foreign_pipeline.hpp"
#include "alchemy/darwin/metal/formats.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace metal = alchemy::metal;
namespace air = alchemy::metal::air;
namespace json = alchemy::metal::json;

namespace {

int failures = 0;

void expect(bool condition, const std::string& what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

// MTLDataType values of the constants below.
constexpr std::uint32_t data_int = 29, data_uint = 33, data_bool = 53;

json::Value constant(std::uint32_t index, std::uint32_t type, std::uint32_t value) {
    auto c = json::Value::object();
    c.set("index", index);
    c.set("type", type);
    std::string hex;
    const int bytes = type == data_bool ? 1 : 4;
    for (int i = 0; i < bytes; ++i) {
        static const char digits[] = "0123456789abcdef";
        const auto byte = (value >> (8 * i)) & 0xff;
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    c.set("bytes", hex);
    return c;
}

json::Value list(std::initializer_list<json::Value> items) {
    auto out = json::Value::array();
    for (const auto& item : items) out.push(item);
    return out;
}

// One pipeline of the library: its descriptor (m2v_pipeline_compile's form)
// with the function-constant values a Metal runtime would pass.
struct Case {
    std::string name;
    json::Value descriptor;
};

json::Value kernel(const std::string& function, json::Value constants = json::Value::array()) {
    auto d = json::Value::object();
    d.set("kind", "compute");
    d.set("function", function);
    d.set("constants", std::move(constants));
    return d;
}

json::Value render(const std::string& vertex, const std::string& fragment, json::Value fragment_constants,
                   std::uint32_t color, std::uint32_t depth = 0, std::uint32_t stencil = 0) {
    auto d = json::Value::object();
    d.set("kind", "render");
    d.set("vertex", vertex);
    if (!fragment.empty()) d.set("fragment", fragment);
    d.set("fragment_constants", std::move(fragment_constants));
    auto colors = json::Value::array();
    if (color) {
        auto c = json::Value::object();
        c.set("index", 0);
        c.set("format", color);
        colors.push(std::move(c));
    }
    d.set("color_attachments", std::move(colors));
    if (depth) d.set("depth_format", depth);
    if (stencil) d.set("stencil_format", stencil);
    if (fragment.empty()) d.set("rasterization_enabled", false);
    return d;
}

std::vector<Case> angle_pipelines() {
    namespace pf = metal::pixel_format;
    std::vector<Case> cases;
    const auto add = [&](std::string name, json::Value d) { cases.push_back({std::move(name), std::move(d)}); };
    for (const char* k : {"convertIndexU8ToU16", "genTriFanIndicesFromArray", "genLineLoopIndicesFromArray", "generate3DMipmaps",
                          "generate2DMipmaps", "generateCubeMipmaps", "generate2DArrayMipmaps", "expandVertexFormatComponentsCS",
                          "linearizeBlocks", "saturateDepth"})
        add(k, kernel(k));
    for (const char* k : {"convertIndexU16", "convertIndexU32"}) add(k, kernel(k, list({constant(100, data_bool, 1)})));
    for (const char* k : {"genTriFanIndicesFromElements", "genLineLoopIndicesFromElements"})
        add(k, kernel(k, list({constant(100, data_bool, 1), constant(200, data_bool, 0), constant(300, data_bool, 1),
                              constant(400, data_bool, 0)})));
    for (const char* k : {"writeFromFloatTextureToBufferCS", "writeFromIntTextureToBufferCS", "writeFromUIntTextureToBufferCS"})
        add(k, kernel(k, list({constant(10, data_int, 1), constant(20, data_int, 0)})));
    add("blitStencilToBufferCS", kernel("blitStencilToBufferCS", list({constant(5, data_int, 0)})));
    add("convertToFloatVertexFormatCS", kernel("convertToFloatVertexFormatCS", list({constant(10, data_int, 1)})));
    add("combineVisibilityResult", kernel("combineVisibilityResult", list({constant(1000, data_bool, 1)})));
    for (const char* k : {"fixIndexBuffer", "genIndexBuffer"}) add(k, kernel(k, list({constant(2000, data_uint, 5)})));

    const auto blit_constants = [] {
        return list({constant(0, data_uint, 1), constant(1, data_bool, 0), constant(2, data_bool, 0), constant(3, data_bool, 0),
                     constant(4, data_int, 0)});
    };
    add("blitVS+blitFloatFS", render("blitVS", "blitFloatFS", blit_constants(), pf::rgba8_unorm));
    add("blitVS+blitIntFS", render("blitVS", "blitIntFS", blit_constants(), pf::rgba32_sint));
    add("blitVS+blitUIntFS", render("blitVS", "blitUIntFS", blit_constants(), pf::rgba32_uint));
    add("blitVS+copyTextureFloatToUIntFS", render("blitVS", "copyTextureFloatToUIntFS", blit_constants(), pf::rgba32_uint));
    add("blitVS+blitDepthFS", render("blitVS", "blitDepthFS", list({constant(4, data_int, 0)}), 0, pf::depth32_float));
    add("blitVS+blitStencilFS", render("blitVS", "blitStencilFS", list({constant(5, data_int, 0)}), 0, 0, pf::stencil8));
    add("blitVS+blitDepthStencilFS", render("blitVS", "blitDepthStencilFS", list({constant(4, data_int, 0), constant(5, data_int, 0)}), 0,
                                            pf::depth32_float_stencil8, pf::depth32_float_stencil8));
    add("clearVS+clearFloatFS", render("clearVS", "clearFloatFS", list({constant(0, data_uint, 1)}), pf::rgba8_unorm));
    add("clearVS+clearIntFS", render("clearVS", "clearIntFS", list({constant(0, data_uint, 1)}), pf::rgba32_sint));
    add("clearVS+clearUIntFS", render("clearVS", "clearUIntFS", list({constant(0, data_uint, 1)}), pf::rgba32_uint));
    add("readFromBufferToTextureVS+readFromBufferToFloatTextureFS",
        render("readFromBufferToTextureVS", "readFromBufferToFloatTextureFS", list({constant(10, data_int, 1)}), pf::rgba8_unorm));
    add("readFromBufferToTextureVS+readFromBufferToIntTextureFS",
        render("readFromBufferToTextureVS", "readFromBufferToIntTextureFS", list({constant(10, data_int, 1)}), pf::rgba32_sint));
    add("readFromBufferToTextureVS+readFromBufferToUIntTextureFS",
        render("readFromBufferToTextureVS", "readFromBufferToUIntTextureFS", list({constant(10, data_int, 1)}), pf::rgba32_uint));
    auto vertex_only = render("convertToFloatVertexFormatVS", "", json::Value::array(), 0);
    vertex_only.set("vertex_constants", list({constant(10, data_int, 1)}));
    add("convertToFloatVertexFormatVS", std::move(vertex_only));
    add("expandVertexFormatComponentsVS", render("expandVertexFormatComponentsVS", "", json::Value::array(), 0));
    return cases;
}

// A plausible binding for every slot the pipeline's interface lists: a
// 64 KiB buffer, a texture of the slot's type and sampled type, Metal's
// default sampler.
json::Value bind_state(const air::Pipeline& pipeline) {
    auto state = air::template_of("BindState");
    auto stages = json::Value::object();
    std::uint64_t address = 0x100000;
    for (const auto& slot : pipeline.info()["interface"]["slots"].items()) {
        const auto& ref = slot[0];
        const auto& use = slot[1];
        const auto* stage = ref["stage"].as_string();
        const auto index = std::to_string(ref["index"].as_u64().value_or(0));
        if (!stage) throw std::runtime_error("interface slot without a stage: " + slot.dump());
        auto* table = stages.find(*stage);
        if (!table) table = &stages.set(*stage, air::template_of("StageTable"));
        if (const auto* buffer = use.find("Buffer")) {
            auto bound = json::Value::object();
            bound.set("length", std::uint64_t{65536});
            bound.set("offset", std::uint64_t{0});
            bound.set("gpu_address", address);
            bound.set("device_address", address);
            bound.set("attribute_stride", nullptr);
            address += 0x10000;
            auto wrapped = json::Value::object();
            wrapped.set("Buffer", std::move(bound));
            table->find("buffers")->set(index, std::move(wrapped));
            static_cast<void>(buffer);
        } else if (const auto* texture = use.find("Texture")) {
            auto facts = air::template_of("TextureFacts");
            const auto* type = (*texture)["texture_type"].as_string();
            const auto* sampled = (*texture)["sampled_type"].as_string();
            facts.set("texture_type", type ? *type : "Type2D");
            std::uint32_t format = metal::pixel_format::rgba8_unorm;
            if ((*texture)["depth_compare"].as_bool().value_or(false)) format = metal::pixel_format::depth32_float;
            else if (sampled && *sampled == "Int") format = metal::pixel_format::rgba32_sint;
            else if (sampled && (*sampled == "UInt" || *sampled == "Uint")) format = metal::pixel_format::rgba32_uint;
            facts.set("pixel_format", air::metal_enum("PixelFormat", format));
            if (type && type->find("Multisample") != std::string::npos) facts.set("sample_count", 4);
            if (type && type->find("Array") != std::string::npos) facts.set("array_length", 2);
            const bool write = (*texture)["access"]["write"].as_bool().value_or(false);
            facts.set("usage", write ? 3 : 1);   // MTLTextureUsageShaderRead (| ShaderWrite)
            auto bound = json::Value::object();
            bound.set("facts", std::move(facts));
            bound.set("resource_id", nullptr);
            auto wrapped = json::Value::object();
            wrapped.set("Texture", std::move(bound));
            table->find("textures")->set(index, std::move(wrapped));
        } else if (use.find("Sampler")) {
            auto bound = json::Value::object();
            bound.set("desc", air::template_of("SamplerDescriptor"));
            bound.set("resource_id", nullptr);
            table->find("samplers")->set(index, std::move(bound));
        } else {
            throw std::runtime_error("interface slot of an unhandled kind: " + slot.dump());
        }
    }
    state.set("stages", std::move(stages));
    state.set("render_area", list({64, 64}));
    return state;
}

// Compiles, plans (compiling the variant a plan asks for) and creates the
// VkPipelines of one case. Returns how many VkPipelines it created.
int create(const std::shared_ptr<metal::Device>& device, const air::TranslatorDevice& translator, const air::Library& library,
           const Case& c) {
    auto descriptor = c.descriptor;
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto pipeline = std::make_shared<const air::Pipeline>(translator, library, descriptor);
        const auto state = bind_state(*pipeline);
        air::Plan plan(*pipeline, translator, state);
        // A plan whose variant is not the empty one asks for that variant.
        static const auto no_variant = air::template_of("VariantKey");
        const auto& variant = plan.info()["variant"];
        if (variant != no_variant && descriptor["variant"] != variant) {
            descriptor.set("variant", variant);
            continue;
        }
        air::ForeignPipeline foreign(device, pipeline);
        int created = 0;
        if (pipeline->compute()) {
            auto dispatch = json::Value::object();
            auto threads = json::Value::object();
            threads.set("threads", list({64, 2, 1}));
            threads.set("threads_per_group", list({32, 1, 1}));
            dispatch.set("Threads", std::move(threads));
            const auto commands = plan.dispatch(state, dispatch);
            for (const auto& region : commands["regions"].items()) {
                expect(foreign.compute(region) != VK_NULL_HANDLE, c.name + ": compute VkPipeline");
                ++created;
            }
        } else {
            auto with_type = state;
            with_type.set("primitive_type", air::metal_enum("PrimitiveType", 4));   // MTLPrimitiveTypeTriangleStrip
            auto draw = json::Value::object();
            auto primitives = json::Value::object();
            primitives.set("vertex_start", 0);
            primitives.set("vertex_count", 4);
            primitives.set("instance_count", 1);
            primitives.set("base_instance", 0);
            draw.set("Primitives", std::move(primitives));
            const auto commands = plan.draw(with_type, draw);
            expect(foreign.render(commands["specialization"], VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) != VK_NULL_HANDLE,
                   c.name + ": render VkPipeline");
            ++created;
        }
        return created;
    }
    throw std::runtime_error("the plan kept asking for another variant");
}

std::shared_ptr<air::ForeignPipeline> foreign(const std::shared_ptr<metal::Device>& device, const air::TranslatorDevice& translator,
                                              const std::shared_ptr<const air::Library>& library, const json::Value& descriptor) {
    auto pipeline = std::make_shared<const air::Pipeline>(translator, *library, descriptor);
    return std::make_shared<air::ForeignPipeline>(device, std::move(pipeline), library, descriptor);
}

std::shared_ptr<metal::Buffer> shared_buffer(const std::shared_ptr<metal::Device>& device, std::size_t length) {
    return metal::Buffer::create(device, length, {metal::StorageMode::shared, 0});
}

// clearVS + clearFloatFS (one color output) over a 16x16 RGBA8Unorm target
// cleared to blue: ANGLE's ClearParams are the color (float4) then the depth.
void test_clear(const std::shared_ptr<metal::Device>& device, const air::TranslatorDevice& translator,
                const std::shared_ptr<const air::Library>& library) {
    const auto pipeline = foreign(device, translator, library,
                                  render("clearVS", "clearFloatFS", list({constant(0, data_uint, 1)}), metal::pixel_format::rgba8_unorm));
    metal::TextureDescriptor target_descriptor;
    target_descriptor.pixel_format = metal::pixel_format::rgba8_unorm;
    target_descriptor.width = target_descriptor.height = 16;
    target_descriptor.usage = metal::texture_usage::render_target | metal::texture_usage::shader_read;
    auto target = metal::Texture::create(device, target_descriptor);
    metal::RenderPass pass;
    pass.colors[0].texture = target;
    pass.colors[0].load_action = 2;    // clear
    pass.colors[0].store_action = 1;   // store
    pass.colors[0].clear_color = {0, 0, 1, 1};
    const float params[8] = {0.25F, 0.5F, 0.75F, 1.0F, 0.5F, 0, 0, 0};
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(params), sizeof(params));
    auto readback = shared_buffer(device, 16 * 64);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_render(pass);
    commands->set_render_pipeline(pipeline);
    commands->set_bytes(metal::Stage::vertex, 0, bytes);
    commands->set_bytes(metal::Stage::fragment, 0, bytes);
    commands->draw(3, 0, 6, 1, 0);   // MTLPrimitiveTypeTriangle: two triangles over the target
    commands->end_encoding();
    commands->begin_blit();
    commands->copy_texture_to_buffer(target, 0, 0, {0, 0, 0, 16, 16, 1}, readback, 0, 64, 16 * 64);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "clear: the command buffer completes");
    const auto* pixels = readback->contents();
    const std::uint8_t want[4] = {64, 128, 191, 255};
    int wrong = 0;
    for (int i = 0; i < 16 * 16; ++i)
        for (int c = 0; c < 4; ++c) wrong += std::abs(int(pixels[i * 4 + c]) - int(want[c])) > 1;
    char line[96];
    std::snprintf(line, sizeof line, "pixel (8,8) = %u %u %u %u", pixels[(8 * 16 + 8) * 4], pixels[(8 * 16 + 8) * 4 + 1],
                  pixels[(8 * 16 + 8) * 4 + 2], pixels[(8 * 16 + 8) * 4 + 3]);
    expect(wrong == 0, std::string("clear: every pixel has ClearParams' color (") + line + ", " + std::to_string(wrong) + " channels off)");
    std::cout << "  clearFloatFS drew " << line << '\n';
}

// convertIndexU8ToU16: IndexConversionParams {srcOffset, indexCount,
// primitiveRestartEnabled} at buffer 0, the 8-bit indices at 1, the 16-bit
// output at 2, one thread per index.
void test_convert(const std::shared_ptr<metal::Device>& device, const air::TranslatorDevice& translator,
                  const std::shared_ptr<const air::Library>& library) {
    const auto pipeline = foreign(device, translator, library, kernel("convertIndexU8ToU16"));
    constexpr std::uint32_t count = 100, offset = 3;
    auto source = shared_buffer(device, 256);
    auto destination = shared_buffer(device, 512);
    for (std::uint32_t i = 0; i < 256; ++i) source->contents()[i] = static_cast<std::uint8_t>(i * 7 + 1);
    std::memset(destination->contents(), 0xab, 512);
    std::uint8_t params[12]{};
    std::memcpy(params, &offset, 4);
    std::memcpy(params + 4, &count, 4);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_compute(false);
    commands->set_compute_pipeline(pipeline);
    commands->set_bytes(metal::Stage::kernel, 0, {params, sizeof params});
    commands->set_buffer(metal::Stage::kernel, 1, source, 0);
    commands->set_buffer(metal::Stage::kernel, 2, destination, 0);
    commands->dispatch_threads({count, 1, 1}, {64, 1, 1});
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "convert: the command buffer completes");
    const auto* out = reinterpret_cast<const std::uint16_t*>(destination->contents());
    int wrong = 0;
    for (std::uint32_t i = 0; i < count; ++i) wrong += out[i] != source->contents()[offset + i];
    for (std::uint32_t i = count; i < 256; ++i) wrong += out[i] != 0xabab;
    expect(wrong == 0, "convert: output[i] = input[offset + i] for the 100 indices, nothing past them (" + std::to_string(wrong) +
                           " wrong)");
    std::cout << "  convertIndexU8ToU16 widened " << count << " indices (" << out[0] << ", " << out[1] << ", ...)\n";
}

std::string library_path() {
    if (const char* path = std::getenv("ALCHEMY_M2V_TEST_LIBRARY"); path && *path) return path;
    return {};
}

} // namespace

int main() {
    std::string reason;
    if (!air::available(&reason)) {
        std::cout << "SKIP: " << reason << '\n';
        return 77;
    }
    const auto path = library_path();
    if (path.empty() || !std::filesystem::exists(path)) {
        std::cout << "SKIP: no dumped metallib (set ALCHEMY_M2V_TEST_LIBRARY to ALCHEMY_MSL_DUMP's metallib-*.metallib)\n";
        return 77;
    }
    std::shared_ptr<metal::Device> device;
    try {
        metal::DeviceOptions options;
        options.pipeline_cache = std::filesystem::path{};   // no disk cache in tests
        options.presentation = false;
        options.validation = std::getenv("ALCHEMY_M2V_VALIDATION") != nullptr;
        device = metal::Device::create(options);
    } catch (const std::exception& error) {
        std::cout << "SKIP: no Vulkan 1.3 device for Metal: " << error.what() << '\n';
        return 77;
    }
    try {
        std::ifstream file(path, std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto& translator = air::TranslatorDevice::of(*device);
        std::cout << "device: " << device->profile().name << ", translator enables "
                  << translator.info()["required_enables"]["features"].size() << " features\n";
        const auto shared_library = std::make_shared<const air::Library>(bytes);
        const auto& library = *shared_library;
        std::cout << "library: " << path << " (" << bytes.size() << " bytes, " << library.functions().size() << " functions)\n";
        int passed = 0, pipelines = 0;
        const auto cases = angle_pipelines();
        for (const auto& c : cases) {
            try {
                const auto created = create(device, translator, library, c);
                pipelines += created;
                ++passed;
                std::cout << "  ok   " << c.name << " (" << created << " VkPipeline" << (created == 1 ? "" : "s") << ")\n";
            } catch (const std::exception& error) {
                expect(false, c.name + ": " + error.what());
            }
        }
        std::cout << passed << " of " << cases.size() << " pipelines created (" << pipelines << " VkPipelines)\n";
        expect(cases.size() == 37, "37 pipelines in ANGLE's library");
        test_clear(device, translator, shared_library);
        test_convert(device, translator, shared_library);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: exception: " << error.what() << '\n';
        return 1;
    }
    if (failures) {
        std::cerr << failures << " AIR translator check(s) failed\n";
        return 1;
    }
    return 0;
}
