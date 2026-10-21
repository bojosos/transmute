#include "alchemy/darwin/metal/bridge.hpp"

#include "alchemy/hash_map.hpp"

#include <cfloat>
#include <cstring>
#include <map>

// Metal descriptor classes generated from one property table (owner: A5).
//
// Each entry names a class, its superclass and its own properties with type
// and default value (the documented defaults of the Metal interface). From
// the table every class gets: a getter (plus an `is...` alias for BOOL
// properties declared with one), a setter, -copy/-copyWithZone: (deep copy,
// as Metal descriptors conform to NSCopying), -dealloc and, for arrays,
// -objectAtIndexedSubscript: / -setObject:atIndexedSubscript: (the setter
// copies, like Metal's). Child descriptors (colorAttachments, depthAttachment,
// vertexDescriptor, frontFaceStencil, ...) are created on first access and
// owned by their parent; setting one copies the argument. Class factories
// (+renderPassDescriptor, +vertexDescriptor, ...) return autoreleased
// defaults. The existing headless shells of darwin_frameworks.cpp gain the
// methods; classes the shells lack are registered and published.
//
// The C++ side of the Metal provider reads descriptors through the
// descriptor_* functions of bridge.hpp; untouched descriptors answer defaults.
namespace alchemy::metal::bridge {
namespace {

enum class Kind : std::uint8_t { u64, boolean, f32, f64, object, color, swizzle, child, array, options };

struct Property {
    std::string_view name;
    Kind kind{Kind::u64};
    std::uint64_t value{};          // default (f32/f64 as bit patterns)
    std::string_view type;          // child/array class
    std::string_view getter;        // extra getter name (isBlendingEnabled, sampleCount)
    std::string_view setter;        // extra setter name (setSampleCount:)
    bool readonly{};
};

struct ClassSpec {
    std::string_view name, superclass;
    std::vector<Property> properties;
    std::string_view element;       // arrays: element class
    std::uint32_t capacity{};
    std::vector<std::string_view> factories;   // +factory returning an autoreleased default instance
    bool reset{};                    // -reset restores the defaults
};

constexpr std::uint64_t f64(double value) { return std::bit_cast<std::uint64_t>(value); }
constexpr std::uint64_t f32(float value) { return std::bit_cast<std::uint32_t>(value); }
constexpr std::uint64_t dont_sample = ~std::uint64_t{0};   // MTLCounterDontSample

Property u(std::string_view name, std::uint64_t value = 0) { return {name, Kind::u64, value}; }
Property b(std::string_view name, bool value = false, std::string_view getter = {}) {
    return {name, Kind::boolean, value ? 1U : 0U, {}, getter};
}
Property object(std::string_view name) { return {name, Kind::object}; }
Property child(std::string_view name, std::string_view type, bool readonly = false) {
    return {name, Kind::child, 0, type, {}, {}, readonly};
}
Property array(std::string_view name, std::string_view type) { return {name, Kind::array, 0, type, {}, {}, true}; }

std::vector<ClassSpec> specifications() {
    const auto attachment_base = std::vector<Property>{object("texture"), u("level"), u("slice"), u("depthPlane"),
        object("resolveTexture"), u("resolveLevel"), u("resolveSlice"), u("resolveDepthPlane"), u("loadAction"),
        u("storeAction"), u("storeActionOptions")};
    const auto sample_attachment = std::vector<Property>{object("sampleBuffer"), u("startOfEncoderSampleIndex", dont_sample),
        u("endOfEncoderSampleIndex", dont_sample)};
    return {
        {"MTLTextureDescriptor", "NSObject",
         {u("textureType", 2), u("pixelFormat", 70), u("width", 1), u("height", 1), u("depth", 1), u("mipmapLevelCount", 1),
          u("sampleCount", 1), u("arrayLength", 1), u("cpuCacheMode"), u("storageMode", 1), u("hazardTrackingMode"),
          {"resourceOptions", Kind::options, 1U << 4}, u("usage", 1), b("allowGPUOptimizedContents", true),
          u("compressionType"), {"swizzle", Kind::swizzle, 0x05040302}, u("placementSparsePageSize")}},
        {"MTLSamplerDescriptor", "NSObject",
         {u("minFilter"), u("magFilter"), u("mipFilter"), u("maxAnisotropy", 1), u("sAddressMode"), u("tAddressMode"),
          u("rAddressMode"), u("borderColor"), b("normalizedCoordinates", true), {"lodMinClamp", Kind::f32, f32(0)},
          {"lodMaxClamp", Kind::f32, f32(FLT_MAX)}, b("lodAverage"), u("compareFunction"), b("supportArgumentBuffers"),
          object("label")}},
        {"MTLRenderPassAttachmentDescriptor", "NSObject", attachment_base},
        {"MTLRenderPassColorAttachmentDescriptor", "MTLRenderPassAttachmentDescriptor", {{"clearColor", Kind::color}}},
        {"MTLRenderPassDepthAttachmentDescriptor", "MTLRenderPassAttachmentDescriptor",
         {u("loadAction", 2), {"clearDepth", Kind::f64, f64(1.0)}, u("depthResolveFilter")}},
        {"MTLRenderPassStencilAttachmentDescriptor", "MTLRenderPassAttachmentDescriptor",
         {u("loadAction", 2), u("clearStencil"), u("stencilResolveFilter")}},
        {"MTLRenderPassColorAttachmentDescriptorArray", "NSObject", {}, "MTLRenderPassColorAttachmentDescriptor", 8},
        {"MTLRenderPassSampleBufferAttachmentDescriptor", "NSObject",
         {object("sampleBuffer"), u("startOfVertexSampleIndex", dont_sample), u("endOfVertexSampleIndex", dont_sample),
          u("startOfFragmentSampleIndex", dont_sample), u("endOfFragmentSampleIndex", dont_sample)}},
        {"MTLRenderPassSampleBufferAttachmentDescriptorArray", "NSObject", {}, "MTLRenderPassSampleBufferAttachmentDescriptor", 4},
        {"MTLRenderPassDescriptor", "NSObject",
         {array("colorAttachments", "MTLRenderPassColorAttachmentDescriptorArray"),
          child("depthAttachment", "MTLRenderPassDepthAttachmentDescriptor"),
          child("stencilAttachment", "MTLRenderPassStencilAttachmentDescriptor"), object("visibilityResultBuffer"),
          u("renderTargetArrayLength"), u("renderTargetWidth"), u("renderTargetHeight"), u("defaultRasterSampleCount"),
          u("imageblockSampleLength"), u("threadgroupMemoryLength"), u("tileWidth"), u("tileHeight"),
          object("rasterizationRateMap"),
          array("sampleBufferAttachments", "MTLRenderPassSampleBufferAttachmentDescriptorArray")},
         {}, 0, {"renderPassDescriptor"}},
        {"MTLRenderPipelineColorAttachmentDescriptor", "NSObject",
         {u("pixelFormat"), b("blendingEnabled", false, "isBlendingEnabled"), u("sourceRGBBlendFactor", 1),
          u("destinationRGBBlendFactor"), u("rgbBlendOperation"), u("sourceAlphaBlendFactor", 1),
          u("destinationAlphaBlendFactor"), u("alphaBlendOperation"), u("writeMask", 0xf)}},
        {"MTLRenderPipelineColorAttachmentDescriptorArray", "NSObject", {}, "MTLRenderPipelineColorAttachmentDescriptor", 8},
        {"MTLPipelineBufferDescriptor", "NSObject", {u("mutability")}},
        {"MTLPipelineBufferDescriptorArray", "NSObject", {}, "MTLPipelineBufferDescriptor", 31},
        {"MTLVertexBufferLayoutDescriptor", "NSObject", {u("stride"), u("stepFunction", 1), u("stepRate", 1)}},
        {"MTLVertexBufferLayoutDescriptorArray", "NSObject", {}, "MTLVertexBufferLayoutDescriptor", 31},
        {"MTLVertexAttributeDescriptor", "NSObject", {u("format"), u("offset"), u("bufferIndex")}},
        {"MTLVertexAttributeDescriptorArray", "NSObject", {}, "MTLVertexAttributeDescriptor", 31},
        {"MTLVertexDescriptor", "NSObject",
         {array("layouts", "MTLVertexBufferLayoutDescriptorArray"), array("attributes", "MTLVertexAttributeDescriptorArray")},
         {}, 0, {"vertexDescriptor"}, true},
        {"MTLLinkedFunctions", "NSObject",
         {object("functions"), object("binaryFunctions"), object("groups"), object("privateFunctions")},
         {}, 0, {"linkedFunctions"}},
        {"MTLRenderPipelineDescriptor", "NSObject",
         {object("label"), object("vertexFunction"), object("fragmentFunction"),
          child("vertexDescriptor", "MTLVertexDescriptor"),
          {"rasterSampleCount", Kind::u64, 1, {}, "sampleCount", "setSampleCount:"},
          b("alphaToCoverageEnabled", false, "isAlphaToCoverageEnabled"), b("alphaToOneEnabled", false, "isAlphaToOneEnabled"),
          b("rasterizationEnabled", true, "isRasterizationEnabled"), u("inputPrimitiveTopology"),
          u("depthAttachmentPixelFormat"), u("stencilAttachmentPixelFormat"),
          array("colorAttachments", "MTLRenderPipelineColorAttachmentDescriptorArray"), u("maxVertexAmplificationCount", 1),
          b("supportIndirectCommandBuffers"), object("binaryArchives"),
          array("vertexBuffers", "MTLPipelineBufferDescriptorArray"), array("fragmentBuffers", "MTLPipelineBufferDescriptorArray"),
          u("maxTessellationFactor", 16), b("tessellationFactorScaleEnabled", false, "isTessellationFactorScaleEnabled"),
          u("tessellationFactorFormat"), u("tessellationControlPointIndexType"), u("tessellationFactorStepFunction"),
          u("tessellationOutputWindingOrder"), u("tessellationPartitionMode"), u("shaderValidation"),
          child("vertexLinkedFunctions", "MTLLinkedFunctions"), child("fragmentLinkedFunctions", "MTLLinkedFunctions"),
          b("supportAddingVertexBinaryFunctions"), b("supportAddingFragmentBinaryFunctions"),
          u("maxVertexCallStackDepth", 1), u("maxFragmentCallStackDepth", 1)},
         {}, 0, {}, true},
        {"MTLComputePipelineDescriptor", "NSObject",
         {object("label"), object("computeFunction"), b("threadGroupSizeIsMultipleOfThreadExecutionWidth"),
          u("maxTotalThreadsPerThreadgroup"), u("maxCallStackDepth", 1), object("stageInputDescriptor"),
          array("buffers", "MTLPipelineBufferDescriptorArray"), b("supportIndirectCommandBuffers"),
          child("linkedFunctions", "MTLLinkedFunctions"), b("supportAddingBinaryFunctions"), object("binaryArchives"),
          object("preloadedLibraries"), u("shaderValidation")},
         {}, 0, {}, true},
        {"MTLStencilDescriptor", "NSObject",
         {u("stencilCompareFunction", 7), u("stencilFailureOperation"), u("depthFailureOperation"),
          u("depthStencilPassOperation"), u("readMask", 0xffffffff), u("writeMask", 0xffffffff)}},
        {"MTLDepthStencilDescriptor", "NSObject",
         {u("depthCompareFunction", 7), b("depthWriteEnabled", false, "isDepthWriteEnabled"),
          child("frontFaceStencil", "MTLStencilDescriptor"), child("backFaceStencil", "MTLStencilDescriptor"), object("label")}},
        {"MTLCompileOptions", "NSObject",
         {object("preprocessorMacros"), b("fastMathEnabled", true), u("mathMode", 2), u("mathFloatingPointFunctions"),
          u("languageVersion", 0x30001), u("libraryType"), object("installName"), object("libraries"),
          b("preserveInvariance"), u("optimizationLevel"), u("compileSymbolVisibility"),
          b("allowReferencingUndefinedSymbols"), u("maxTotalThreadsPerThreadgroup"), b("enableLogging")}},
        {"MTLFunctionDescriptor", "NSObject",
         {object("name"), object("specializedName"), object("constantValues"), u("options"), object("binaryArchives")},
         {}, 0, {"functionDescriptor"}},
        {"MTLIntersectionFunctionDescriptor", "MTLFunctionDescriptor", {}},
        {"MTLArgumentDescriptor", "NSObject",
         {u("dataType"), u("index"), u("arrayLength"), u("access"), u("textureType", 2), u("constantBlockAlignment")},
         {}, 0, {"argumentDescriptor"}},
        {"MTLCommandBufferDescriptor", "NSObject", {b("retainedReferences", true), u("errorOptions"), object("logState")}},
        // macOS 15. maxCommandBufferCount defaults to 64, as -newCommandQueue's
        // queue; MTLLogStateDescriptor's level (MTLLogLevelUndefined) and
        // bufferSize defaults of 0 are not documented values (unverified).
        {"MTLCommandQueueDescriptor", "NSObject", {u("maxCommandBufferCount", 64), object("logState")}},
        {"MTLLogStateDescriptor", "NSObject", {u("level"), u("bufferSize")}},
        {"MTLHeapDescriptor", "NSObject",
         {u("size"), u("storageMode", 2), u("cpuCacheMode"), u("hazardTrackingMode"), {"resourceOptions", Kind::options, 2U << 4},
          u("type"), u("sparsePageSize"), u("maxCompatiblePlacementSparsePageSize")}},
        {"MTLComputePassSampleBufferAttachmentDescriptor", "NSObject", sample_attachment},
        {"MTLComputePassSampleBufferAttachmentDescriptorArray", "NSObject", {}, "MTLComputePassSampleBufferAttachmentDescriptor", 4},
        {"MTLComputePassDescriptor", "NSObject",
         {u("dispatchType"), array("sampleBufferAttachments", "MTLComputePassSampleBufferAttachmentDescriptorArray")},
         {}, 0, {"computePassDescriptor"}},
        {"MTLBlitPassSampleBufferAttachmentDescriptor", "NSObject", sample_attachment},
        {"MTLBlitPassSampleBufferAttachmentDescriptorArray", "NSObject", {}, "MTLBlitPassSampleBufferAttachmentDescriptor", 4},
        {"MTLBlitPassDescriptor", "NSObject",
         {array("sampleBufferAttachments", "MTLBlitPassSampleBufferAttachmentDescriptorArray")}, {}, 0, {"blitPassDescriptor"}},
        {"MTLBinaryArchiveDescriptor", "NSObject", {object("url")}},
        {"MTLCaptureDescriptor", "NSObject", {object("captureObject"), u("destination", 1), object("outputURL")}},
        {"MTLCounterSampleBufferDescriptor", "NSObject",
         {object("counterSet"), object("label"), u("storageMode"), u("sampleCount")}},
        {"MTLIndirectCommandBufferDescriptor", "NSObject",
         {u("commandTypes"), b("inheritPipelineState", true), b("inheritBuffers", true), u("maxVertexBufferBindCount"),
          u("maxFragmentBufferBindCount"), u("maxKernelBufferBindCount")}},
        {"MTLIntersectionFunctionTableDescriptor", "NSObject", {u("functionCount")}, {}, 0,
         {"intersectionFunctionTableDescriptor"}},
    };
}

// A class from the table with every inherited property flattened in.
struct Resolved {
    Address cls{};
    std::string name;
    std::vector<Property> properties;
    hash_map<std::string, std::size_t, string_hash, string_equal> index;   // property name -> slot
    const Resolved* element{};
    std::uint32_t capacity{};
    [[nodiscard]] std::size_t find(std::string_view property) const {
        const auto found = index.find(property);
        if (found == index.end())
            throw std::runtime_error("Alchemy Metal: " + name + " has no property " + std::string(property));
        return found->second;
    }
};

struct Value { std::array<std::uint64_t, 4> bits{}; };

struct DescriptorState final : ObjectPayload {
    const Resolved* spec{};
    std::vector<Value> values;
    std::vector<Address> elements;   // arrays: +1 element objects, 0 = not created yet
};

struct ConstantState final : ObjectPayload {
    ConstantValues values;
};

struct Registry {
    std::vector<std::unique_ptr<Resolved>> classes;
    hash_map<Address, const Resolved*, address_hash> by_class;
    std::map<std::string, const Resolved*, std::less<>> by_name;
    Address constant_values_class{};
};

Registry& registry() {
    static Registry value;
    return value;
}

Value default_value(const Property& property) {
    Value value;
    switch (property.kind) {
    case Kind::color:
        value.bits = {f64(0), f64(0), f64(0), f64(1)};
        break;
    default: value.bits[0] = property.value; break;
    }
    return value;
}

// The spec of an object's class, walking up for guest subclasses.
const Resolved* spec_of(Address object) {
    auto& r = runtime();
    auto cls = r.objc.object_class(object);
    for (int depth = 0; cls && depth < 64; ++depth) {
        if (const auto found = registry().by_class.find(cls); found != registry().by_class.end()) return found->second;
        cls = r.objc.send(cls, "superclass").x0;
    }
    throw std::runtime_error("Alchemy Metal: object is not a Metal descriptor");
}

std::shared_ptr<DescriptorState> descriptor(Address object) {
    const auto* spec = spec_of(object);
    return runtime().objc.host_state<DescriptorState>(object, [spec] {
        auto state = std::make_shared<DescriptorState>();
        state->spec = spec;
        for (const auto& property : spec->properties) state->values.push_back(default_value(property));
        state->elements.assign(spec->capacity, 0);
        return state;
    });
}

std::shared_ptr<DescriptorState> existing(Address object) {
    return object ? runtime().objc.find_host_state<DescriptorState>(object) : nullptr;
}

Address new_instance(const Resolved& spec) {
    auto& r = runtime();
    const auto object = r.objc.allocate(spec.cls);
    static_cast<void>(descriptor(object));
    return object;
}

// Releases every guest reference a descriptor holds and restores defaults.
void clear(DescriptorState& state) {
    auto& r = runtime();
    std::vector<Address> released;
    for (std::size_t index = 0; index < state.values.size(); ++index) {
        const auto kind = state.spec->properties[index].kind;
        if ((kind == Kind::object || kind == Kind::child || kind == Kind::array) && state.values[index].bits[0])
            released.push_back(state.values[index].bits[0]);
        state.values[index] = default_value(state.spec->properties[index]);
    }
    for (auto& element : state.elements) { if (element) released.push_back(element); element = 0; }
    for (const auto object : released) r.release(object);
}

void copy_into(Address destination, Address source);

// A child or array object of `state` at `index`, created on first use.
Address ensure_child(DescriptorState& state, std::size_t index) {
    auto& value = state.values[index].bits[0];
    if (!value) {
        const auto& property = state.spec->properties[index];
        const auto found = registry().by_name.find(property.type);
        if (found == registry().by_name.end()) throw std::runtime_error("Alchemy Metal: unknown descriptor class " + std::string(property.type));
        value = new_instance(*found->second);
    }
    return value;
}

Address ensure_element(DescriptorState& state, std::size_t index) {
    if (index >= state.elements.size())
        throw std::out_of_range("Alchemy Metal: " + state.spec->name + " index " + std::to_string(index) + " exceeds " +
                                std::to_string(state.elements.size() - 1));
    if (!state.elements[index]) state.elements[index] = new_instance(*state.spec->element);
    return state.elements[index];
}

// Deep copy of every value of `source` into `destination` (same table).
void copy_into(Address destination, Address source) {
    auto& r = runtime();
    auto target = descriptor(destination);
    clear(*target);
    const auto from = existing(source);
    if (!from) return;   // untouched source: defaults
    if (from->spec != target->spec && from->spec->properties.size() != target->spec->properties.size())
        throw std::runtime_error("Alchemy Metal: cannot copy a " + from->spec->name + " into a " + target->spec->name);
    for (std::size_t index = 0; index < from->values.size(); ++index) {
        const auto kind = from->spec->properties[index].kind;
        const auto value = from->values[index];
        if (kind == Kind::object) target->values[index].bits[0] = r.retain(value.bits[0]);
        else if (kind == Kind::child || kind == Kind::array) {
            if (value.bits[0]) copy_into(ensure_child(*target, index), value.bits[0]);
        } else target->values[index] = value;
    }
    for (std::size_t index = 0; index < from->elements.size(); ++index)
        if (from->elements[index]) copy_into(ensure_element(*target, index), from->elements[index]);
}

std::uint64_t resource_options(const DescriptorState& state) {
    const auto& spec = *state.spec;
    return (state.values[spec.find("storageMode")].bits[0] << 4) | state.values[spec.find("cpuCacheMode")].bits[0] |
           (state.values[spec.find("hazardTrackingMode")].bits[0] << 8);
}

std::string setter_name(std::string_view property) {
    std::string result = "set";
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(property[0])));
    result.append(property.substr(1));
    result += ':';
    return result;
}

void install_property(Runtime& r, const Resolved& spec, std::size_t index) {
    const auto& property = spec.properties[index];
    const auto getter = [&r, index, kind = property.kind](CpuState& s, GuestMemory&) {
        auto state = descriptor(s.x[0]);
        auto& value = state->values[index].bits;
        switch (kind) {
        case Kind::u64: case Kind::object: s.x[0] = value[0]; break;
        case Kind::boolean: s.x[0] = value[0] ? 1 : 0; break;
        case Kind::f32: s.v[0] = {value[0] & 0xffffffffU, 0}; break;
        case Kind::f64: s.v[0] = {value[0], 0}; break;
        case Kind::color: for (int c = 0; c < 4; ++c) s.v[c] = {value[c], 0}; break;
        case Kind::swizzle: s.x[0] = value[0] & 0xffffffffU; break;
        case Kind::child: case Kind::array: s.x[0] = ensure_child(*state, index); break;
        case Kind::options: s.x[0] = resource_options(*state); break;
        }
        static_cast<void>(r);
    };
    const auto setter = [&r, index, kind = property.kind](CpuState& s, GuestMemory&) {
        auto state = descriptor(s.x[0]);
        auto& value = state->values[index].bits;
        switch (kind) {
        case Kind::u64: value[0] = s.x[2]; break;
        case Kind::boolean: value[0] = (s.x[2] & 0xff) ? 1 : 0; break;
        case Kind::f32: value[0] = s.v[0][0] & 0xffffffffU; break;
        case Kind::f64: value[0] = s.v[0][0]; break;
        case Kind::color: for (int c = 0; c < 4; ++c) value[c] = s.v[c][0]; break;
        case Kind::swizzle: value[0] = s.x[2] & 0xffffffffU; break;
        case Kind::object: {
            const auto previous = value[0];
            value[0] = r.retain(s.x[2]);
            r.release(previous);
            break;
        }
        case Kind::child:
            // Metal copies descriptors assigned to a descriptor property; nil
            // removes it (vertexDescriptor = nil: no vertex fetch).
            if (!s.x[2]) { const auto previous = value[0]; value[0] = 0; r.release(previous); }
            else copy_into(ensure_child(*state, index), s.x[2]);
            break;
        case Kind::array: throw std::runtime_error("Alchemy Metal: descriptor arrays are read-only properties");
        case Kind::options: {
            const auto& spec = *state->spec;
            state->values[spec.find("storageMode")].bits[0] = (s.x[2] >> 4) & 0xf;
            state->values[spec.find("cpuCacheMode")].bits[0] = s.x[2] & 0xf;
            state->values[spec.find("hazardTrackingMode")].bits[0] = (s.x[2] >> 8) & 0x3;
            break;
        }
        }
    };
    const char* get_types = "Q@:";
    switch (property.kind) {
    case Kind::boolean: get_types = "B@:"; break;
    case Kind::f32: get_types = "f@:"; break;
    case Kind::f64: get_types = "d@:"; break;
    case Kind::object: case Kind::child: case Kind::array: get_types = "@@:"; break;
    case Kind::color: get_types = "{MTLClearColor=dddd}@:"; break;
    default: break;
    }
    r.method(spec.cls, property.name, getter, false, get_types);
    if (!property.getter.empty()) r.method(spec.cls, property.getter, getter, false, get_types);
    if (!property.readonly) {
        r.method(spec.cls, setter_name(property.name), setter, false, "v@:Q");
        if (!property.setter.empty()) r.method(spec.cls, property.setter, setter, false, "v@:Q");
    }
}

} // namespace

// ---- read API --------------------------------------------------------------------------

namespace {
const Value* value_of(Address object, std::string_view property, Value& fallback) {
    if (!object) throw std::runtime_error("Alchemy Metal: nil descriptor");
    if (const auto state = existing(object)) {
        const auto index = state->spec->find(property);
        fallback = state->values[index];
        return &fallback;
    }
    const auto* spec = spec_of(object);
    fallback = default_value(spec->properties[spec->find(property)]);
    return &fallback;
}
} // namespace

std::uint64_t descriptor_u64(Address object, std::string_view property) {
    if (!object) throw std::runtime_error("Alchemy Metal: nil descriptor");
    // One state lookup (each is an object-store probe) and one name lookup.
    if (const auto state = existing(object)) {
        const auto index = state->spec->find(property);
        if (state->spec->properties[index].kind == Kind::options) return resource_options(*state);
        return state->values[index].bits[0];
    }
    Value value;
    return value_of(object, property, value)->bits[0];
}
void descriptor_read(Address object, std::span<const std::string_view> properties, std::span<std::array<std::uint64_t, 4>> out) {
    if (!object) throw std::runtime_error("Alchemy Metal: nil descriptor");
    if (out.size() < properties.size()) throw std::logic_error("descriptor_read: output too small");
    // One object-store lookup for every property.
    const auto state = existing(object);
    const auto* spec = state ? state->spec : spec_of(object);
    for (std::size_t i = 0; i < properties.size(); ++i) {
        const auto index = spec->find(properties[i]);
        if (spec->properties[index].kind == Kind::options) throw std::logic_error("descriptor_read: options property");
        out[i] = state ? state->values[index].bits : default_value(spec->properties[index]).bits;
    }
}
bool descriptor_bool(Address object, std::string_view property) { return descriptor_u64(object, property) != 0; }
float descriptor_f32(Address object, std::string_view property) {
    Value value;
    return std::bit_cast<float>(static_cast<std::uint32_t>(value_of(object, property, value)->bits[0]));
}
double descriptor_f64(Address object, std::string_view property) {
    Value value;
    return std::bit_cast<double>(value_of(object, property, value)->bits[0]);
}
Address descriptor_object(Address object, std::string_view property) { return descriptor_u64(object, property); }
std::array<double, 4> descriptor_color(Address object, std::string_view property) {
    Value value;
    const auto& bits = value_of(object, property, value)->bits;
    return {std::bit_cast<double>(bits[0]), std::bit_cast<double>(bits[1]), std::bit_cast<double>(bits[2]),
            std::bit_cast<double>(bits[3])};
}
std::array<std::uint8_t, 4> descriptor_swizzle(Address object, std::string_view property) {
    const auto packed = static_cast<std::uint32_t>(descriptor_u64(object, property));
    return {static_cast<std::uint8_t>(packed), static_cast<std::uint8_t>(packed >> 8), static_cast<std::uint8_t>(packed >> 16),
            static_cast<std::uint8_t>(packed >> 24)};
}
Address descriptor_child(Address object, std::string_view property) {
    const auto state = existing(object);
    return state ? state->values[state->spec->find(property)].bits[0] : 0;
}
Address descriptor_element(Address array, std::uint32_t index) {
    const auto state = existing(array);
    return state && index < state->elements.size() ? state->elements[index] : 0;
}

ConstantValues constant_values(Address object) {
    if (!object) return {};
    const auto state = runtime().objc.find_host_state<ConstantState>(object);
    return state ? state->values : ConstantValues{};
}

std::vector<ArgumentEntry> argument_entries(Address array) {
    std::vector<ArgumentEntry> result;
    if (!array) return result;
    for (const auto item : runtime().cf.array_values(array))
        result.push_back({static_cast<std::uint32_t>(descriptor_u64(item, "index")),
                          static_cast<std::uint32_t>(descriptor_u64(item, "dataType")),
                          static_cast<std::uint32_t>(descriptor_u64(item, "arrayLength"))});
    return result;
}

// ---- installation -----------------------------------------------------------------------

void install_descriptors(Runtime& r) {
    auto& reg = registry();
    const auto specs = specifications();
    // Classes first (superclasses precede subclasses in the table).
    for (const auto& spec : specs) {
        auto resolved = std::make_unique<Resolved>();
        resolved->name = spec.name;
        resolved->cls = r.define_class(spec.name, spec.superclass);
        if (const auto parent = reg.by_name.find(spec.superclass); parent != reg.by_name.end())
            resolved->properties = parent->second->properties;
        for (const auto& property : spec.properties) {
            const auto existing_property = std::find_if(resolved->properties.begin(), resolved->properties.end(),
                [&](const auto& item) { return item.name == property.name; });
            if (existing_property != resolved->properties.end()) existing_property->value = property.value;   // new default
            else resolved->properties.push_back(property);
        }
        for (std::size_t index = 0; index < resolved->properties.size(); ++index)
            resolved->index.emplace(std::string(resolved->properties[index].name), index);
        resolved->capacity = spec.capacity;
        reg.by_class[resolved->cls] = resolved.get();
        reg.by_name[resolved->name] = resolved.get();
        reg.classes.push_back(std::move(resolved));
    }
    for (const auto& spec : specs) {
        auto& resolved = *const_cast<Resolved*>(reg.by_name.at(std::string(spec.name)));
        if (!spec.element.empty()) resolved.element = reg.by_name.at(std::string(spec.element));
        const auto own_begin = reg.by_name.count(std::string(spec.superclass))
                                   ? reg.by_name.at(std::string(spec.superclass))->properties.size() : 0;
        for (std::size_t index = own_begin; index < resolved.properties.size(); ++index) install_property(r, resolved, index);
        const auto cls = resolved.cls;
        const auto* pointer = &resolved;
        r.method(cls, "init", [](CpuState& s, GuestMemory&) { static_cast<void>(descriptor(s.x[0])); });
        const auto copy = [&r, pointer](CpuState& s, GuestMemory&) {
            const auto* spec_value = spec_of(s.x[0]);
            const auto result = new_instance(*spec_value);
            copy_into(result, s.x[0]);
            s.x[0] = result;
            static_cast<void>(r);
            static_cast<void>(pointer);
        };
        r.method(cls, "copy", copy);
        r.method(cls, "copyWithZone:", copy, false, "@@:^v");
        r.method(cls, "dealloc", [&r, cls](CpuState& s, GuestMemory&) {
            const auto object = s.x[0];
            if (const auto state = existing(object)) clear(*state);
            static_cast<void>(r.objc.send_super(object, cls, r.objc.selector("dealloc")));
        }, false, "v@:");
        if (spec.reset)
            r.method(cls, "reset", [](CpuState& s, GuestMemory&) { clear(*descriptor(s.x[0])); }, false, "v@:");
        if (resolved.capacity) {
            r.method(cls, "objectAtIndexedSubscript:", [](CpuState& s, GuestMemory&) {
                s.x[0] = ensure_element(*descriptor(s.x[0]), s.x[2]);
            }, false, "@@:Q");
            r.method(cls, "setObject:atIndexedSubscript:", [&r](CpuState& s, GuestMemory&) {
                auto state = descriptor(s.x[0]);
                const auto element = ensure_element(*state, s.x[3]);
                if (s.x[2]) copy_into(element, s.x[2]);
                else clear(*descriptor(element));
                static_cast<void>(r);
            }, false, "v@:@Q");
        }
        for (const auto factory : spec.factories)
            r.method(cls, factory, [&r, pointer](CpuState& s, GuestMemory&) {
                // The receiver may be a guest subclass.
                const auto* target = pointer;
                if (const auto found = registry().by_class.find(s.x[0]); found != registry().by_class.end()) target = found->second;
                s.x[0] = r.autorelease(new_instance(*target));
            }, true);
    }

    // MTLTextureDescriptor convenience constructors.
    const auto texture_class = reg.by_name.at("MTLTextureDescriptor")->cls;
    const auto make_texture = [&r](std::uint32_t type, std::uint64_t format, std::uint64_t width, std::uint64_t height,
                                   std::uint64_t levels) {
        const auto object = new_instance(*registry().by_name.at("MTLTextureDescriptor"));
        auto state = descriptor(object);
        const auto& spec = *state->spec;
        state->values[spec.find("textureType")].bits[0] = type;
        state->values[spec.find("pixelFormat")].bits[0] = format;
        state->values[spec.find("width")].bits[0] = width;
        state->values[spec.find("height")].bits[0] = height;
        state->values[spec.find("mipmapLevelCount")].bits[0] = levels;
        return std::pair{object, state};
    };
    const auto level_count = [](std::uint64_t width, std::uint64_t height) {
        std::uint64_t levels = 1, size = std::max(width, height);
        while (size > 1) { size >>= 1; ++levels; }
        return levels;
    };
    r.method(texture_class, "texture2DDescriptorWithPixelFormat:width:height:mipmapped:",
        [&r, make_texture, level_count](CpuState& s, GuestMemory&) {
            const auto levels = (s.x[5] & 0xff) ? level_count(s.x[3], s.x[4]) : 1;
            s.x[0] = r.autorelease(make_texture(2, s.x[2], s.x[3], s.x[4], levels).first);
        }, true, "@@:QQQB");
    r.method(texture_class, "textureCubeDescriptorWithPixelFormat:size:mipmapped:",
        [&r, make_texture, level_count](CpuState& s, GuestMemory&) {
            const auto levels = (s.x[4] & 0xff) ? level_count(s.x[3], s.x[3]) : 1;
            s.x[0] = r.autorelease(make_texture(5, s.x[2], s.x[3], s.x[3], levels).first);
        }, true, "@@:QQB");
    r.method(texture_class, "textureBufferDescriptorWithPixelFormat:width:resourceOptions:usage:",
        [&r, make_texture](CpuState& s, GuestMemory&) {
            auto [object, state] = make_texture(9, s.x[2], s.x[3], 1, 1);
            const auto& spec = *state->spec;
            state->values[spec.find("storageMode")].bits[0] = (s.x[4] >> 4) & 0xf;
            state->values[spec.find("cpuCacheMode")].bits[0] = s.x[4] & 0xf;
            state->values[spec.find("hazardTrackingMode")].bits[0] = (s.x[4] >> 8) & 0x3;
            state->values[spec.find("usage")].bits[0] = s.x[5];
            s.x[0] = r.autorelease(object);
        }, true, "@@:QQQQ");

    // MTLFunctionConstantValues.
    const auto constants = r.define_class("MTLFunctionConstantValues");
    reg.constant_values_class = constants;
    const auto constant_state = [](Address object) {
        return runtime().objc.host_state<ConstantState>(object);
    };
    const auto read_value = [](GuestMemory& m, Address pointer, std::uint64_t type) {
        const auto size = data_type_size(static_cast<std::uint32_t>(type));
        if (!size)
            throw std::runtime_error("Alchemy Metal: function constants of MTLDataType " + std::to_string(type) +
                                     " (vectors, structs) are not translated");
        ConstantValue value;
        value.data_type = static_cast<std::uint32_t>(type);
        value.bytes.resize(size);
        m.read_bytes(pointer, value.bytes);
        return value;
    };
    r.method(constants, "init", [constant_state](CpuState& s, GuestMemory&) { static_cast<void>(constant_state(s.x[0])); });
    r.method(constants, "setConstantValue:type:atIndex:", [constant_state, read_value](CpuState& s, GuestMemory& m) {
        constant_state(s.x[0])->values.by_index[static_cast<std::uint32_t>(s.x[4])] = read_value(m, s.x[2], s.x[3]);
    }, false, "v@:^vQQ");
    r.method(constants, "setConstantValue:type:withName:", [&r, constant_state, read_value](CpuState& s, GuestMemory& m) {
        constant_state(s.x[0])->values.by_name[r.utf8(s.x[4])] = read_value(m, s.x[2], s.x[3]);
    }, false, "v@:^vQ@");
    r.method(constants, "setConstantValues:type:withRange:", [constant_state, read_value](CpuState& s, GuestMemory& m) {
        const auto size = data_type_size(static_cast<std::uint32_t>(s.x[3]));
        auto state = constant_state(s.x[0]);
        for (std::uint64_t index = 0; index < s.x[5]; ++index)
            state->values.by_index[static_cast<std::uint32_t>(s.x[4] + index)] = read_value(m, s.x[2] + index * size, s.x[3]);
    }, false, "v@:^vQ{_NSRange=QQ}");
    r.method(constants, "reset", [constant_state](CpuState& s, GuestMemory&) { constant_state(s.x[0])->values = {}; }, false, "v@:");
    const auto copy_constants = [&r, constants](CpuState& s, GuestMemory&) {
        const auto source = runtime().objc.find_host_state<ConstantState>(s.x[0]);
        const auto object = r.objc.allocate(constants);
        auto state = runtime().objc.host_state<ConstantState>(object);
        if (source) state->values = source->values;
        s.x[0] = object;
    };
    r.method(constants, "copy", copy_constants);
    r.method(constants, "copyWithZone:", copy_constants, false, "@@:^v");
}

} // namespace alchemy::metal::bridge
