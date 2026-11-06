// Shader libraries, functions and pipelines through the Objective-C runtime
// (darwin/metal/metal_objects.cpp, "functions, libraries, pipelines" and
// "argument encoders"; owner: Chrome stage 4 g2), as Chrome's GPU process
// (Dawn, ANGLE) uses them. With a Metal device (skipped without a Vulkan 1.3
// GPU):
//   - -newLibraryWithSource:options:error: with MTLCompileOptions
//     (preprocessorMacros, languageVersion) and its failure (an NSError in
//     MTLLibraryErrorDomain, MTLLibraryErrorCompileFailure);
//   - an argument buffer of buffer pointers (ANGLE's uniform-block argument
//     buffer, design): -newArgumentEncoderWithBufferIndex: gives the
//     [[id(n)]] layout (encodedLength, an array member's later ids), the
//     encoder writes device addresses, and a compute dispatch reads through
//     them.
#include "alchemy/gui/tests/fill_fixture.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <process.h>
#include <fstream>
#include <iterator>

namespace {
using namespace fill_test;

constexpr std::string_view metal = "/System/Library/Frameworks/Metal.framework/Versions/A/Metal";

Address make(Fixture& f, std::string_view cls) { return f.send(f.send(f.cls(cls), "alloc"), "init"); }

Address device(Fixture& f) {
    static Address value = f.call(metal, "_MTLCreateSystemDefaultDevice");
    return value;
}

// The source compiles with a macro from MTLCompileOptions.preprocessorMacros
// (ANGLE passes its configuration that way) and fails without it.
constexpr const char* argument_source = R"(
#include <metal_stdlib>
using namespace metal;
#ifndef SCALE
#error SCALE comes from MTLCompileOptions.preprocessorMacros
#endif
struct Block { float4 value; uint count; };
struct Arguments {
    constant Block* first [[id(0)]];
    device float* out [[id(1)]];
    constant Block* pair[2] [[id(2)]];
};
kernel void read_arguments(constant Arguments& args [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    constant Block& b = *args.pair[i & 1u];
    args.out[i] = (args.first->value.x + b.value.y) * SCALE + float(b.count);
}
)";

void libraries(Fixture& f) {
    const auto d = device(f);
    if (!d) { std::cout << "  no Metal device (no Vulkan 1.3 GPU): skipped\n"; return; }
    const auto error = f.block(8);
    // Without the macro: the compile error comes back as an NSError.
    const auto options = make(f, "MTLCompileOptions");
    require(f.send(options, "languageVersion") != 0, "MTLCompileOptions.languageVersion has a default");
    const auto failed = f.send(d, "newLibraryWithSource:options:error:", {f.cf.string(argument_source), options, error});
    const auto failure = f.engine.read64(error);
    require(failed == 0 && failure && f.send(failure, "code") == 3 &&
                f.cf.utf8(f.send(failure, "domain")) == "MTLLibraryErrorDomain",
            "a compile error is MTLLibraryErrorCompileFailure");
    // With it (an NSDictionary of NSString -> NSString).
    const auto key = f.cf.string("SCALE");
    const auto two = f.cf.string("2.0f");
    const auto keys = f.block(8), values = f.block(8);
    f.engine.write64(keys, key);
    f.engine.write64(values, two);
    const auto dictionary = f.send(f.cls("NSDictionary"), "dictionaryWithObjects:forKeys:count:", {values, keys, 1});
    static_cast<void>(f.send(options, "setPreprocessorMacros:", {dictionary}));
    f.engine.write64(error, 0);
    const auto library = f.send(d, "newLibraryWithSource:options:error:", {f.cf.string(argument_source), options, error});
    require(library != 0 && f.engine.read64(error) == 0,
            "newLibraryWithSource: with preprocessorMacros: " +
                (f.engine.read64(error) ? f.cf.utf8(f.send(f.engine.read64(error), "localizedDescription")) : std::string()));
    const auto function = f.send(library, "newFunctionWithName:", {f.cf.string("read_arguments")});
    require(function != 0 && f.send(function, "functionType") == 3, "newFunctionWithName: finds the kernel");

    // The argument encoder: ids 0, 1, 2 and 3 (pair[1]) at 0, 8, 16, 24.
    const auto encoder = f.send(function, "newArgumentEncoderWithBufferIndex:", {0});
    require(encoder != 0 && f.send(encoder, "encodedLength") == 32 && f.send(encoder, "alignment") == 8,
            "argument encoder length and alignment from the [[id]] layout");
    const auto arguments = f.send(d, "newBufferWithLength:options:", {32, 0});
    auto block = [&](float x, float y, std::uint32_t count) {
        const auto buffer = f.send(d, "newBufferWithLength:options:", {32, 0});
        const auto contents = f.send(buffer, "contents");
        float values4[4] = {x, y, 0, 0};
        std::uint8_t bytes[20]{};
        std::memcpy(bytes, values4, 16);
        std::memcpy(bytes + 16, &count, 4);
        f.engine.write_bytes(contents, std::vector<std::uint8_t>(bytes, bytes + sizeof(bytes)));
        return buffer;
    };
    const auto first = block(1.5f, 0, 0), even = block(0, 10.0f, 100), odd = block(0, 20.0f, 200);
    constexpr std::uint32_t threads = 8;
    const auto out = f.send(d, "newBufferWithLength:options:", {threads * 4, 0});
    static_cast<void>(f.send(encoder, "setArgumentBuffer:offset:", {arguments, 0}));
    static_cast<void>(f.send(encoder, "setBuffer:offset:atIndex:", {first, 0, 0}));
    static_cast<void>(f.send(encoder, "setBuffer:offset:atIndex:", {out, 0, 1}));
    static_cast<void>(f.send(encoder, "setBuffer:offset:atIndex:", {even, 0, 2}));
    static_cast<void>(f.send(encoder, "setBuffer:offset:atIndex:", {odd, 0, 3}));   // pair[1]
    const auto encoded = f.send(arguments, "contents");
    require(f.engine.read64(encoded + 24) == f.send(odd, "gpuAddress") && f.engine.read64(encoded) == f.send(first, "gpuAddress"),
            "the encoder writes device addresses at the member offsets");

    const auto pipeline = f.send(d, "newComputePipelineStateWithFunction:error:", {function, error});
    require(pipeline != 0, "compute pipeline");
    const auto queue = f.send(d, "newCommandQueue");
    const auto commands = f.send(queue, "commandBuffer");
    const auto compute = f.send(commands, "computeCommandEncoder");
    static_cast<void>(f.send(compute, "setComputePipelineState:", {pipeline}));
    static_cast<void>(f.send(compute, "setBuffer:offset:atIndex:", {arguments, 0, 0}));
    for (const auto used : {first, out, even, odd}) static_cast<void>(f.send(compute, "useResource:usage:", {used, 3}));
    const auto groups = f.block(24), size = f.block(24);
    f.engine.write64(groups, 1); f.engine.write64(groups + 8, 1); f.engine.write64(groups + 16, 1);
    f.engine.write64(size, threads); f.engine.write64(size + 8, 1); f.engine.write64(size + 16, 1);
    static_cast<void>(f.send(compute, "dispatchThreadgroups:threadsPerThreadgroup:", {groups, size}));
    static_cast<void>(f.send(compute, "endEncoding"));
    static_cast<void>(f.send(commands, "commit"));
    static_cast<void>(f.send(commands, "waitUntilCompleted"));
    require(f.send(commands, "status") == 4, "the dispatch completes");
    const auto results = f.send(out, "contents");
    for (std::uint32_t i = 0; i < threads; ++i) {
        const auto bits = static_cast<std::uint32_t>(f.engine.read64(results + 4 * i));
        float value;
        std::memcpy(&value, &bits, 4);
        const float want = (1.5f + (i & 1 ? 20.0f : 10.0f)) * 2.0f + (i & 1 ? 200.0f : 100.0f);
        require(value == want, "out[" + std::to_string(i) + "] = " + std::to_string(value) + ", expected " +
                                   std::to_string(want));
    }
}

// Pipeline reflection: MTLArgument and macOS 13's MTLBinding view of a
// kernel's buffer and integer texture.
void reflection(Fixture& f) {
    const auto d = device(f);
    if (!d) { std::cout << "  no Metal device (no Vulkan 1.3 GPU): skipped\n"; return; }
    const auto error = f.block(8);
    const auto library = f.send(d, "newLibraryWithSource:options:error:", {f.cf.string(R"(
#include <metal_stdlib>
using namespace metal;
kernel void reflected(device float* out [[buffer(2)]], texture2d<uint, access::read> counts [[texture(1)]],
                      uint2 gid [[thread_position_in_grid]]) {
    out[gid.x] = float(counts.read(gid).x);
}
)"), 0, error});
    require(library != 0, "reflection kernel compiles");
    const auto function = f.send(library, "newFunctionWithName:", {f.cf.string("reflected")});
    const auto out = f.block(8);
    // MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo
    const auto pipeline = f.send(d, "newComputePipelineStateWithFunction:options:reflection:error:", {function, 3, out, error});
    const auto reflected = f.engine.read64(out);
    require(pipeline != 0 && reflected != 0, "a pipeline with reflection");
    const auto bindings = f.send(reflected, "bindings");
    const auto arguments = f.send(reflected, "arguments");
    require(bindings == arguments && f.cf.length(bindings) == 2, "bindings and arguments list the buffer and the texture");
    bool buffer = false, texture = false;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto binding = f.cf.element(bindings, i);
        const auto type = f.send(binding, "type");
        const auto name = f.cf.utf8(f.send(binding, "name"));
        require(f.send(binding, "isUsed") == 1 && f.send(binding, "isArgument") == 0, "used, not an argument buffer member");
        if (type == 0) buffer = name == "out" && f.send(binding, "index") == 2 && f.send(binding, "access") == 1;
        if (type == 2) texture = name == "counts" && f.send(binding, "index") == 1 && f.send(binding, "textureType") == 2 &&
                                 f.send(binding, "textureDataType") == 33;   // MTLDataTypeUInt
    }
    require(buffer && texture, "MTLBindingTypeBuffer out at 2 (read-write), MTLBindingTypeTexture counts at 1 (uint)");
}

// A precompiled library (newLibraryWithData:, ANGLE's built-in shaders) when
// the AIR translator and a dumped metallib are present (ALCHEMY_M2V_CAPI,
// ALCHEMY_M2V_TEST_LIBRARY; air_translator.hpp): its functions, function
// constants, and render and compute pipeline states. Without them the call
// fails explicitly, as Metal reports an unsupported library.
void precompiled(Fixture& f) {
    const auto d = device(f);
    if (!d) { std::cout << "  no Metal device (no Vulkan 1.3 GPU): skipped\n"; return; }
    const char* path = std::getenv("ALCHEMY_M2V_TEST_LIBRARY");
    std::vector<std::uint8_t> bytes;
    if (path && *path) {
        std::ifstream file(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    if (bytes.empty()) {
        // Any bytes: without a translator (or a library) the call fails.
        bytes = {'M', 'T', 'L', 'B', 0, 0, 0, 0};
    }
    const auto source = f.block(bytes.size());
    f.engine.write_bytes(source, bytes);
    const auto data = f.call("/usr/lib/libSystem.B.dylib", "_dispatch_data_create", {source, bytes.size(), 0, 0});
    const auto error = f.block(8);
    const auto library = f.send(d, "newLibraryWithData:error:", {data, error});
    const bool translator = std::getenv("ALCHEMY_M2V_CAPI") && *std::getenv("ALCHEMY_M2V_CAPI") &&
                            std::filesystem::exists(std::getenv("ALCHEMY_M2V_CAPI"));
    if (!translator || !path || !*path) {
        const auto failure = f.engine.read64(error);
        require(library == 0 && failure && f.cf.utf8(f.send(failure, "domain")) == "MTLLibraryErrorDomain",
                "without the AIR translator and a library, newLibraryWithData: fails in MTLLibraryErrorDomain");
        std::cout << "  no AIR translator or dumped library: only the failure checked\n";
        return;
    }
    require(library != 0, "newLibraryWithData: through the AIR translator: " +
                              (f.engine.read64(error) ? f.cf.utf8(f.send(f.engine.read64(error), "localizedDescription")) : std::string()));
    require(f.cf.length(f.send(library, "functionNames")) == 40, "ANGLE's library has 40 functions");
    const auto missing = f.send(library, "newFunctionWithName:", {f.cf.string("noSuchFunction")});
    require(missing == 0, "an unknown function name gives nil");

    // Function constants: MTLFunctionConstantValues by index.
    const auto constants = make(f, "MTLFunctionConstantValues");
    const auto value = f.block(4);
    const auto set = [&](std::uint32_t bits, std::uint64_t type, std::uint64_t index) {
        f.engine.write64(value, bits);
        static_cast<void>(f.send(constants, "setConstantValue:type:atIndex:", {value, type, index}));
    };
    set(1, 33, 0);   // kNumColorOutputs: uint 1
    set(0, 53, 1);   // kPremultiplyAlpha: bool
    set(0, 53, 2);   // kUnmultiplyAlpha
    set(0, 53, 3);   // kTransformLinearToSrgb
    set(0, 29, 4);   // kSourceTextureType: int (2D)
    const auto vertex = f.send(library, "newFunctionWithName:", {f.cf.string("blitVS")});
    f.engine.write64(error, 0);
    const auto fragment = f.send(library, "newFunctionWithName:constantValues:error:", {f.cf.string("blitFloatFS"), constants, error});
    require(vertex != 0 && f.send(vertex, "functionType") == 1 && fragment != 0 && f.send(fragment, "functionType") == 2,
            "blitVS (vertex) and blitFloatFS (fragment, specialized)");
    require(f.cf.utf8(f.send(fragment, "name")) == "blitFloatFS", "a precompiled function's name");
    const auto unspecialized = f.send(library, "newFunctionWithName:constantValues:error:",
                                      {f.cf.string("convertIndexU16"), make(f, "MTLFunctionConstantValues"), error});
    require(unspecialized == 0 && f.engine.read64(error) != 0, "a required function constant without a value is an error");

    const auto descriptor = make(f, "MTLRenderPipelineDescriptor");
    static_cast<void>(f.send(descriptor, "setVertexFunction:", {vertex}));
    static_cast<void>(f.send(descriptor, "setFragmentFunction:", {fragment}));
    const auto color = f.send(f.send(descriptor, "colorAttachments"), "objectAtIndexedSubscript:", {0});
    static_cast<void>(f.send(color, "setPixelFormat:", {70}));   // RGBA8Unorm
    f.engine.write64(error, 0);
    const auto render = f.send(d, "newRenderPipelineStateWithDescriptor:error:", {descriptor, error});
    require(render != 0, "a render pipeline state of precompiled functions: " +
                             (f.engine.read64(error) ? f.cf.utf8(f.send(f.engine.read64(error), "localizedDescription")) : std::string()));

    const auto kernel = f.send(library, "newFunctionWithName:", {f.cf.string("generate2DMipmaps")});
    require(kernel != 0 && f.send(kernel, "functionType") == 3, "generate2DMipmaps (kernel)");
    f.engine.write64(error, 0);
    const auto compute = f.send(d, "newComputePipelineStateWithFunction:error:", {kernel, error});
    require(compute != 0 && f.send(compute, "maxTotalThreadsPerThreadgroup") >= 32 && f.send(compute, "threadExecutionWidth") == 32,
            "a compute pipeline state of a precompiled kernel answers its thread limits");
}

// MTLBinaryArchive: adding a compute pipeline creates it (into the device's
// pipeline cache), tile/mesh pipelines and functions are refused with
// MTLBinaryArchiveErrorCompilationFailure, serializeToURL: writes a file an
// archive made from that URL loads, and a missing file fails InvalidFile.
void archives(Fixture& f) {
    const auto d = device(f);
    if (!d) { std::cout << "  no Metal device (no Vulkan 1.3 GPU): skipped\n"; return; }
    const auto error = f.block(8);
    const auto library = f.send(d, "newLibraryWithSource:options:error:", {f.cf.string(R"(
#include <metal_stdlib>
using namespace metal;
kernel void archived(device uint* out [[buffer(0)]], uint i [[thread_position_in_grid]]) { out[i] = i * 3u; }
)"), 0, error});
    require(library != 0, "archive kernel compiles");
    const auto pipeline = make(f, "MTLComputePipelineDescriptor");
    static_cast<void>(f.send(pipeline, "setComputeFunction:", {f.send(library, "newFunctionWithName:", {f.cf.string("archived")})}));
    const auto empty = make(f, "MTLBinaryArchiveDescriptor");
    const auto archive = f.send(d, "newBinaryArchiveWithDescriptor:error:", {empty, error});
    require(archive != 0, "an empty archive");
    require(f.send(archive, "addComputePipelineFunctionsWithDescriptor:error:", {pipeline, error}) == 1, "a compute pipeline is added");
    f.engine.write64(error, 0);
    require(f.send(archive, "addMeshRenderPipelineFunctionsWithDescriptor:error:", {make(f, "MTLBinaryArchiveDescriptor"), error}) == 0,
            "a mesh pipeline is refused");
    const auto refusal = f.engine.read64(error);
    require(refusal != 0 && f.cf.utf8(f.send(refusal, "domain")) == "MTLBinaryArchiveDomain" && f.send(refusal, "code") == 3,
            "MTLBinaryArchiveDomain / CompilationFailure");
    const auto host = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()) /
                      ("alchemy_metal_archive_" + std::to_string(_getpid()) + ".bin");
    std::filesystem::remove(host);
    // The guest path of the host file: as is without a mapping, else
    // /Volumes/<drive>/<rest>, checked against the mapping.
    std::string guest = host.generic_string();
    if (f.process.map_guest_path) {
        const auto drive = host.root_name().string();
        guest = "/Volumes/" + drive.substr(0, 1) + "/" + host.relative_path().generic_string();
        std::error_code ignored;
        if (std::filesystem::weakly_canonical(f.process.map_guest_path(guest), ignored) != host) {
            std::cout << "  " << guest << " does not map to " << host.string() << ": file round trip skipped\n";
            return;
        }
    }
    const auto url = f.send(f.cls("NSURL"), "fileURLWithPath:isDirectory:", {f.cf.string(guest), 0});
    require(f.send(archive, "serializeToURL:error:", {url, error}) == 1 && std::filesystem::file_size(host) > 25,
            "serializeToURL: writes the archive file");
    const auto loading = make(f, "MTLBinaryArchiveDescriptor");
    static_cast<void>(f.send(loading, "setUrl:", {url}));
    require(f.send(d, "newBinaryArchiveWithDescriptor:error:", {loading, error}) != 0, "the written archive loads");
    std::filesystem::remove(host);
    f.engine.write64(error, 0);
    require(f.send(d, "newBinaryArchiveWithDescriptor:error:", {loading, error}) == 0 && f.engine.read64(error) != 0 &&
                f.send(f.engine.read64(error), "code") == 1,
            "a missing archive file fails with MTLBinaryArchiveErrorInvalidFile");
}

// Tier 2 argument buffers: an encoder made from a function's argument
// buffer of a texture, a sampler and a buffer writes the texture's and the
// sampler's gpuResourceID (bindless slots) and the buffer's address at the
// members' offsets; one made from MTLArgumentDescriptors lays a texture out
// the same way.
void tier2_arguments(Fixture& f) {
    const auto d = device(f);
    if (!d) { std::cout << "  no Metal device (no Vulkan 1.3 GPU): skipped\n"; return; }
    if (f.send(d, "argumentBuffersSupport") != 1) { std::cout << "  argumentBuffersSupport is Tier1: skipped\n"; return; }
    const auto error = f.block(8);
    const auto library = f.send(d, "newLibraryWithSource:options:error:", {f.cf.string(R"(
#include <metal_stdlib>
using namespace metal;
struct Material { texture2d<float> image [[id(0)]]; sampler nearest [[id(1)]]; device float* out [[id(2)]]; };
kernel void tier2(constant Material& m [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    m.out[i] = m.image.sample(m.nearest, float2(0.5), level(0)).x;
}
)"), 0, error});
    require(library != 0, "tier 2 kernel compiles");
    const auto function = f.send(library, "newFunctionWithName:", {f.cf.string("tier2")});
    const auto encoder = f.send(function, "newArgumentEncoderWithBufferIndex:", {0});
    require(encoder != 0 && f.send(encoder, "encodedLength") == 24, "a 24-byte argument buffer (three 8-byte members)");
    const auto arguments = f.send(d, "newBufferWithLength:options:", {24, 0});
    static_cast<void>(f.send(encoder, "setArgumentBuffer:offset:", {arguments, 0}));
    const auto texture_descriptor = make(f, "MTLTextureDescriptor");
    static_cast<void>(f.send(texture_descriptor, "setWidth:", {4}));
    static_cast<void>(f.send(texture_descriptor, "setHeight:", {4}));
    const auto texture = f.send(d, "newTextureWithDescriptor:", {texture_descriptor});
    const auto sampler = f.send(d, "newSamplerStateWithDescriptor:", {make(f, "MTLSamplerDescriptor")});
    const auto out = f.send(d, "newBufferWithLength:options:", {64, 0});
    static_cast<void>(f.send(encoder, "setTexture:atIndex:", {texture, 0}));
    static_cast<void>(f.send(encoder, "setSamplerState:atIndex:", {sampler, 1}));
    static_cast<void>(f.send(encoder, "setBuffer:offset:atIndex:", {out, 0, 2}));
    const auto contents = f.send(arguments, "contents");
    const auto texture_id = f.send(texture, "gpuResourceID"), sampler_id = f.send(sampler, "gpuResourceID");
    require(texture_id != 0 && sampler_id != 0, "the texture and the sampler have MTLResourceIDs");
    require(f.engine.read64(contents) == texture_id && f.engine.read64(contents + 8) == sampler_id &&
                f.engine.read64(contents + 16) == f.send(out, "gpuAddress"),
            "setTexture:/setSamplerState:/setBuffer: write the resource IDs and the address");
    // MTLArgumentDescriptor: a texture member is an 8-byte MTLResourceID too.
    const auto texture_argument = make(f, "MTLArgumentDescriptor");
    static_cast<void>(f.send(texture_argument, "setDataType:", {58}));   // MTLDataTypeTexture
    static_cast<void>(f.send(texture_argument, "setIndex:", {0}));
    const std::array<Address, 1> descriptors{texture_argument};
    const auto by_descriptor = f.send(d, "newArgumentEncoderWithArguments:", {f.cf.array(descriptors)});
    require(by_descriptor != 0 && f.send(by_descriptor, "encodedLength") == 8, "an MTLArgumentDescriptor texture takes 8 bytes");
    static_cast<void>(f.send(by_descriptor, "setArgumentBuffer:offset:", {arguments, 0}));
    static_cast<void>(f.send(by_descriptor, "setTexture:atIndex:", {texture, 0}));
    require(f.engine.read64(contents) == texture_id, "the descriptor-made encoder writes the same ID");
    for (const auto object : {encoder, by_descriptor, arguments, texture, sampler, out, function, library})
        static_cast<void>(f.send(object, "release"));
}

// Documented refusals: a dynamic library (supportsDynamicLibraries is NO)
// fails with MTLLibraryErrorUnsupported, and a texture of a pixel format the
// device has no format for (ASTC) is nil.
void refusals(Fixture& f) {
    const auto d = device(f);
    if (!d) { std::cout << "  no Metal device (no Vulkan 1.3 GPU): skipped\n"; return; }
    const auto error = f.block(8);
    const auto options = make(f, "MTLCompileOptions");
    static_cast<void>(f.send(options, "setLibraryType:", {1}));   // MTLLibraryTypeDynamic
    const auto library = f.send(d, "newLibraryWithSource:options:error:",
                                {f.cf.string("kernel void k(device uint* p [[buffer(0)]]) { p[0] = 1; }\n"), options, error});
    require(library == 0 && f.engine.read64(error) != 0 && f.send(f.engine.read64(error), "code") == 1,
            "a dynamic library fails with MTLLibraryErrorUnsupported");
    const auto descriptor = make(f, "MTLTextureDescriptor");
    static_cast<void>(f.send(descriptor, "setPixelFormat:", {204}));   // MTLPixelFormatASTC_4x4_LDR
    static_cast<void>(f.send(descriptor, "setWidth:", {16}));
    static_cast<void>(f.send(descriptor, "setHeight:", {16}));
    require(f.send(d, "newTextureWithDescriptor:", {descriptor}) == 0, "an ASTC texture is nil");
}

} // namespace

int main() {
    return run("metal libraries", {{"libraries and argument buffers", libraries}, {"reflection", reflection},
                                    {"precompiled libraries", precompiled}, {"binary archives", archives},
                                    {"refusals", refusals}, {"tier 2 argument buffers", tier2_arguments}});
}
