#include "alchemy/msl/compiler.hpp"
#include "spirv_builder.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

// The vertex and tessellation control stages in front of a post-tessellation
// vertex function (M5). See tessellation_stages in
// include/alchemy/msl/compiler.hpp for the contract.
namespace alchemy::msl {
namespace {

namespace sop = spv::op;
using Numeric = TessellationSetup::Attribute::Numeric;

constexpr std::uint32_t spirv_1_5 = 0x00010500;
constexpr std::uint32_t addressing_physical64 = 5348, memory_glsl450 = 1;
constexpr std::uint32_t model_vertex = 0, model_control = 1;
constexpr std::uint32_t mode_spacing_equal = 1, mode_spacing_fractional_even = 2, mode_spacing_fractional_odd = 3;
constexpr std::uint32_t mode_vertex_order_cw = 4, mode_vertex_order_ccw = 5, mode_triangles = 22, mode_quads = 24;
constexpr std::uint32_t mode_output_vertices = 26;
constexpr std::uint32_t memory_aligned = 2;

// A function body in one module: SSA values, one level of structured
// selection (enough for optional buffer reads).
class Builder {
public:
    spv::Module m;
    std::uint32_t u32() { return m.type_int(32, false); }
    std::uint32_t i32() { return m.type_int(32, true); }
    std::uint32_t u64() { return m.type_int(64, false); }
    std::uint32_t f32() { return m.type_float(32); }
    std::uint32_t vec(std::uint32_t element, std::uint32_t count) { return m.type_vector(element, count); }
    std::uint32_t cu(std::uint32_t value) { return m.constant_u32(value); }
    std::uint32_t cf(float value) { return m.constant_f32(value); }
    std::uint32_t op(std::uint32_t opcode, std::uint32_t type, std::vector<std::uint32_t> operands) {
        return m.emit_value(opcode, type, operands);
    }
    std::uint32_t ext(std::uint32_t type, std::uint32_t instruction, std::vector<std::uint32_t> operands) {
        operands.insert(operands.begin(), {m.glsl(), instruction});
        return m.emit_value(sop::ExtInst, type, operands);
    }
    std::uint32_t variable(std::uint32_t storage, std::uint32_t type) {
        const auto id = m.variable(m.type_pointer(storage, type), storage);
        interface.push_back(id);
        return id;
    }
    std::uint32_t load(std::uint32_t type, std::uint32_t pointer) { return op(sop::Load, type, {pointer}); }
    // Element `index` (a constant or id) of an arrayed variable.
    std::uint32_t element(std::uint32_t storage, std::uint32_t type, std::uint32_t variable, std::uint32_t index) {
        return op(sop::AccessChain, m.type_pointer(storage, type), {variable, index});
    }
    // A 32-bit word at a (4-byte aligned) buffer device address.
    std::uint32_t load_word(std::uint32_t address) {
        const auto pointer = op(sop::ConvertUToPtr, m.type_pointer(spv::storage::PhysicalStorageBuffer, u32()), {address});
        return op(sop::Load, u32(), {pointer, memory_aligned, 4});
    }
    std::uint32_t add64(std::uint32_t address, std::uint32_t bytes32) {
        return op(sop::IAdd, u64(), {address, op(sop::UConvert, u64(), {bytes32})});
    }
    // `condition ? then() : otherwise`, evaluating then() only when needed.
    std::uint32_t when(std::uint32_t condition, std::uint32_t type, const std::function<std::uint32_t()>& then,
                       std::uint32_t otherwise) {
        const auto then_label = m.id(), merge = m.id();
        const auto from = block_;
        m.emit(sop::SelectionMerge, {merge, 0});
        m.emit(sop::BranchConditional, {condition, then_label, merge});
        label(then_label);
        const auto value = then();
        const auto then_end = block_;
        m.emit(sop::Branch, {merge});
        label(merge);
        return op(sop::Phi, type, {value, then_end, otherwise, from});
    }
    void begin() {
        function = m.id();
        m.emit(sop::Function, {m.type_void(), function, 0, m.type_function(m.type_void())});
        label(m.id());
    }
    std::vector<std::uint32_t> end(std::uint32_t model, bool physical = true) {
        m.emit(sop::Return, {});
        m.emit(sop::FunctionEnd, {});
        m.capability(spv::capability::Shader);
        m.entry_point(model, function, "main", interface);
        return m.finish(spirv_1_5, physical ? addressing_physical64 : 0U, memory_glsl450);
    }
    std::uint32_t function{};
    std::vector<std::uint32_t> interface;

private:
    void label(std::uint32_t id) {
        m.emit(sop::Label, {id});
        block_ = id;
    }
    std::uint32_t block_{};
};

// The 32-bit interface type of a reflected attribute: element kind ('f', 'i',
// 'u') and component count.
struct InterfaceShape { char kind{'f'}; std::uint32_t components{1}; };
std::optional<InterfaceShape> shape_of(const Reflection& reflection, std::uint32_t type_index) {
    if (type_index >= reflection.types.size()) return std::nullopt;
    const auto& type = reflection.types[type_index];
    if (type.kind != Type::Kind::scalar && type.kind != Type::Kind::vector) return std::nullopt;
    InterfaceShape shape;
    shape.components = type.kind == Type::Kind::vector ? type.components : 1;
    switch (type.scalar) {
    case Scalar::half: case Scalar::bfloat: case Scalar::float32: shape.kind = 'f'; break;
    case Scalar::int8: case Scalar::int16: case Scalar::int32: shape.kind = 'i'; break;
    case Scalar::boolean: case Scalar::uint8: case Scalar::uint16: case Scalar::uint32: shape.kind = 'u'; break;
    default: return std::nullopt;
    }
    return shape;
}

std::vector<std::uint32_t> vertex_stage() {
    Builder b;
    const auto u2 = b.vec(b.u32(), 2);
    const auto vertex_index = b.variable(spv::storage::Input, b.u32());
    b.m.decorate(vertex_index, spv::decoration::BuiltIn, {spv::builtin::VertexIndex});
    const auto instance_index = b.variable(spv::storage::Input, b.u32());
    b.m.decorate(instance_index, spv::decoration::BuiltIn, {spv::builtin::InstanceIndex});
    const auto out = b.variable(spv::storage::Output, u2);
    b.m.decorate(out, spv::decoration::Location, {0});
    b.begin();
    const auto value = b.op(sop::CompositeConstruct, u2, {b.load(b.u32(), vertex_index), b.load(b.u32(), instance_index)});
    b.m.emit(sop::Store, {out, value});
    return b.end(model_vertex, false);
}

} // namespace

TessellationStages tessellation_stages(const Reflection& post, const TessellationSetup& setup) {
    TessellationStages result;
    if (!post.patch) {
        result.error = "the vertex function is not a post-tessellation ([[patch]]) function";
        return result;
    }
    const auto kind = post.patch->kind;
    if (kind != setup.kind) {
        result.error = "the tessellation setup's patch kind does not match the [[patch]] function";
        return result;
    }
    const auto count = setup.control_points;
    if (count < 1 || count > 32) {
        result.error = "a patch needs 1 to 32 control points";
        return result;
    }
    if (setup.control_point_index_bytes != 0 && setup.control_point_index_bytes != 2 && setup.control_point_index_bytes != 4) {
        result.error = "the control point index type must be none, UInt16 or UInt32";
        return result;
    }
    result.vertex = vertex_stage();

    Builder b;
    auto& m = b.m;
    m.capability(spv::capability::Tessellation);
    m.capability(spv::capability::Int64);
    m.capability(spv::capability::PhysicalStorageBufferAddresses);
    const auto u32 = b.u32(), i32 = b.i32(), u64 = b.u64(), f32 = b.f32();
    const auto u2 = b.vec(u32, 2);

    // Push constants (TessellationPushConstants).
    const auto buffers_type = m.type_array(u64, b.cu(tessellation_vertex_buffers), 8);
    const auto block = m.type_struct({u64, u64, u64, u32, f32, buffers_type}, true,
        {static_cast<std::uint32_t>(offsetof(TessellationPushConstants, factors)),
         static_cast<std::uint32_t>(offsetof(TessellationPushConstants, patch_indices)),
         static_cast<std::uint32_t>(offsetof(TessellationPushConstants, control_point_indices)),
         static_cast<std::uint32_t>(offsetof(TessellationPushConstants, instance_stride)),
         static_cast<std::uint32_t>(offsetof(TessellationPushConstants, scale)),
         static_cast<std::uint32_t>(offsetof(TessellationPushConstants, buffers))});
    const auto push = b.variable(spv::storage::PushConstant, block);
    const auto vertex_in = b.variable(spv::storage::Input, m.type_array(u2, b.cu(32)));
    m.decorate(vertex_in, spv::decoration::Location, {0});
    const auto invocation = b.variable(spv::storage::Input, u32);
    m.decorate(invocation, spv::decoration::BuiltIn, {spv::builtin::InvocationId});
    const auto outer = b.variable(spv::storage::Output, m.type_array(f32, b.cu(4)));
    m.decorate(outer, spv::decoration::BuiltIn, {spv::builtin::TessLevelOuter});
    m.decorate(outer, spv::decoration::Patch);
    const auto inner = b.variable(spv::storage::Output, m.type_array(f32, b.cu(2)));
    m.decorate(inner, spv::decoration::BuiltIn, {spv::builtin::TessLevelInner});
    m.decorate(inner, spv::decoration::Patch);
    const auto info_out = b.variable(spv::storage::Output, u2);
    m.decorate(info_out, spv::decoration::Location, {patch_info_location});
    m.decorate(info_out, spv::decoration::Patch);

    // The outputs: one per attribute the post-tessellation function reads.
    struct Output {
        const TessellationSetup::Attribute* attribute{};
        InterfaceShape shape;
        bool per_patch{};
        std::uint32_t variable{}, type{};
    };
    std::vector<Output> outputs;
    for (const auto& input : post.stage_in) {
        if (!input.used) continue;
        const TessellationSetup::Attribute* found = nullptr;
        for (const auto& attribute : setup.attributes) if (attribute.location == input.index) found = &attribute;
        if (!found) {
            result.error = "the vertex descriptor has no attribute " + std::to_string(input.index) + " ('" + input.name + "')";
            return result;
        }
        const auto shape = shape_of(post, input.type);
        if (!shape) {
            result.error = "attribute " + std::to_string(input.index) + " has a type a vertex format cannot fill";
            return result;
        }
        const bool float_format = found->numeric == Numeric::float_ || found->numeric == Numeric::unorm ||
                                  found->numeric == Numeric::snorm;
        if (float_format != (shape->kind == 'f')) {
            result.error = "the vertex format of attribute " + std::to_string(input.index) + " does not match its type";
            return result;
        }
        if (found->step == 1 || found->step > 4 || found->buffer >= tessellation_vertex_buffers ||
            (found->bits != 8 && found->bits != 16 && found->bits != 32) || found->components < 1 || found->components > 4) {
            result.error = "attribute " + std::to_string(input.index) + " cannot be fetched for a post-tessellation function";
            return result;
        }
        Output output;
        output.attribute = found;
        output.shape = *shape;
        output.per_patch = input.per_patch;
        const auto element = shape->kind == 'f' ? f32 : shape->kind == 'i' ? i32 : u32;
        output.type = b.vec(element, shape->components);
        output.variable = b.variable(spv::storage::Output,
            output.per_patch ? output.type : m.type_array(output.type, b.cu(count)));
        m.decorate(output.variable, spv::decoration::Location, {input.index});
        if (output.per_patch) m.decorate(output.variable, spv::decoration::Patch);
        outputs.push_back(output);
    }

    b.begin();
    const auto push_member = [&](std::uint32_t index, std::uint32_t type) {
        return b.load(type, b.element(spv::storage::PushConstant, type, push, b.cu(index)));
    };
    const auto vertex = b.load(u2, b.element(spv::storage::Input, u2, vertex_in, b.cu(0)));
    const auto vertex_index = b.op(sop::CompositeExtract, u32, {vertex, 0});
    const auto instance = b.op(sop::CompositeExtract, u32, {vertex, 1});
    const auto i = b.load(u32, invocation);
    const auto zero64 = m.constant_u64(0);

    // Patch id: the patch index buffer's element, or the vertex index (patchStart + n).
    const auto patch_indices = push_member(1, u64);
    const auto patch = b.when(b.op(sop::INotEqual, m.type_bool(), {patch_indices, zero64}), u32, [&] {
        return b.load_word(b.add64(patch_indices, b.op(sop::IMul, u32, {vertex_index, b.cu(4)})));
    }, vertex_index);
    // Control point index.
    auto control_point = b.op(sop::IAdd, u32, {b.op(sop::IMul, u32, {patch, b.cu(count)}), i});
    if (setup.control_point_index_bytes) {
        const auto indices = push_member(2, u64);
        const auto bytes = setup.control_point_index_bytes;
        control_point = b.when(b.op(sop::INotEqual, m.type_bool(), {indices, zero64}), u32, [&] {
            const auto at = b.op(sop::IMul, u32, {control_point, b.cu(bytes)});
            if (bytes == 4) return b.load_word(b.add64(indices, at));
            const auto word = b.load_word(b.add64(indices, b.op(sop::BitwiseAnd, u32, {at, b.cu(~3U)})));
            const auto shift = b.op(sop::IMul, u32, {b.op(sop::BitwiseAnd, u32, {at, b.cu(3)}), b.cu(8)});
            return b.op(sop::BitFieldUExtract, u32, {word, shift, b.cu(16)});
        }, control_point);
    }

    for (const auto& output : outputs) {
        const auto& a = *output.attribute;
        std::uint32_t index;
        switch (a.step) {
        case 0: index = b.cu(0); break;
        case 2: index = a.step_rate ? b.op(sop::UDiv, u32, {instance, b.cu(a.step_rate)}) : b.cu(0); break;
        case 3: index = patch; break;
        default: index = control_point; break;
        }
        const auto buffer = b.load(u64, b.op(sop::AccessChain, m.type_pointer(spv::storage::PushConstant, u64),
                                             {push, b.cu(5), b.cu(a.buffer)}));
        const auto element_offset = b.op(sop::IAdd, u32, {b.op(sop::IMul, u32, {index, b.cu(a.stride)}), b.cu(a.offset)});
        const auto address = b.add64(buffer, element_offset);
        const bool is_signed = a.numeric == Numeric::sint || a.numeric == Numeric::snorm;
        std::vector<std::uint32_t> values;   // decoded components (f32 or u32/i32 bits)
        for (std::uint32_t c = 0; c < a.components; ++c) {
            std::uint32_t raw;
            if (a.bits == 32) {
                raw = b.load_word(b.op(sop::IAdd, u64, {address, m.constant_u64(4ULL * c)}));
            } else {
                const auto byte = b.op(sop::IAdd, u64, {address, m.constant_u64(static_cast<std::uint64_t>(c) * a.bits / 8)});
                const auto word = b.load_word(b.op(sop::BitwiseAnd, u64, {byte, m.constant_u64(~3ULL)}));
                const auto low = b.op(sop::UConvert, u32, {b.op(sop::BitwiseAnd, u64, {byte, m.constant_u64(3)})});
                const auto shift = b.op(sop::IMul, u32, {low, b.cu(8)});
                raw = b.op(is_signed ? sop::BitFieldSExtract : sop::BitFieldUExtract, u32, {word, shift, b.cu(a.bits)});
            }
            std::uint32_t value = raw;
            switch (a.numeric) {
            case Numeric::float_:
                value = a.bits == 32 ? b.op(sop::Bitcast, f32, {raw})
                                     : b.op(sop::CompositeExtract, f32, {b.ext(b.vec(f32, 2), spv::glsl::UnpackHalf2x16, {raw}), 0});
                break;
            case Numeric::unorm:
                value = b.op(sop::FMul, f32, {b.op(sop::ConvertUToF, f32, {raw}),
                                              b.cf(1.0F / static_cast<float>((1ULL << a.bits) - 1))});
                break;
            case Numeric::snorm:
                value = b.ext(f32, spv::glsl::FMax, {b.op(sop::FMul, f32, {b.op(sop::ConvertSToF, f32, {b.op(sop::Bitcast, i32, {raw})}),
                    b.cf(1.0F / static_cast<float>((1ULL << (a.bits - 1)) - 1))}), b.cf(-1.0F)});
                break;
            case Numeric::uint: case Numeric::sint: break;
            }
            values.push_back(value);
        }
        if (a.bgra && values.size() >= 3) std::swap(values[0], values[2]);
        const char kind = output.shape.kind;
        std::vector<std::uint32_t> parts;
        for (std::uint32_t c = 0; c < output.shape.components; ++c) {
            if (c < values.size()) {
                parts.push_back(kind == 'i' ? b.op(sop::Bitcast, i32, {values[c]}) : values[c]);
            } else {
                const bool one = c == 3;
                parts.push_back(kind == 'f' ? b.cf(one ? 1.0F : 0.0F) : kind == 'i' ? m.constant_i32(one ? 1 : 0) : b.cu(one ? 1 : 0));
            }
        }
        const auto value = parts.size() == 1 ? parts[0] : b.op(sop::CompositeConstruct, output.type, parts);
        const auto target = output.per_patch ? output.variable
                                             : b.element(spv::storage::Output, output.type, output.variable, i);
        m.emit(sop::Store, {target, value});
    }
    m.emit(sop::Store, {info_out, b.op(sop::CompositeConstruct, u2, {patch, instance})});

    // Tessellation factors (MTLTriangle/QuadTessellationFactorsHalf).
    const bool quad = kind == Patch::Kind::quad;
    auto factor_address = push_member(0, u64);
    const std::uint32_t factor_size = quad ? 12 : 8;
    if (setup.factor_step & 1U) factor_address = b.add64(factor_address, b.op(sop::IMul, u32, {patch, b.cu(factor_size)}));
    if (setup.factor_step & 2U)
        factor_address = b.add64(factor_address, b.op(sop::IMul, u32, {instance, push_member(3, u32)}));
    const auto scale = setup.factor_scale ? push_member(4, f32) : 0U;
    std::vector<std::uint32_t> factors;
    for (std::uint32_t w = 0; w < factor_size / 4; ++w) {
        const auto word = b.load_word(b.op(sop::IAdd, u64, {factor_address, m.constant_u64(4ULL * w)}));
        const auto pair = b.ext(b.vec(f32, 2), spv::glsl::UnpackHalf2x16, {word});
        for (std::uint32_t h = 0; h < 2; ++h) {
            auto f = b.op(sop::CompositeExtract, f32, {pair, h});
            if (scale) f = b.op(sop::FMul, f32, {f, scale});
            // Clamped to the maximum; NaN and 0 stay (both discard the patch).
            const auto maximum = b.cf(setup.max_factor);
            f = b.op(sop::Select, f32, {b.op(sop::FOrdGreaterThan, m.type_bool(), {f, maximum}), maximum, f});
            if (setup.partition == TessellationSetup::Partition::pow2) {
                const auto rounded = b.ext(f32, spv::glsl::Exp2, {b.ext(f32, spv::glsl::Ceil, {b.ext(f32, spv::glsl::Log2, {f})})});
                f = b.op(sop::Select, f32, {b.op(sop::FOrdGreaterThan, m.type_bool(), {f, b.cf(1.0F)}), rounded, f});
            }
            factors.push_back(f);
        }
    }
    const auto edges = quad ? 4U : 3U;
    for (std::uint32_t e = 0; e < edges; ++e)
        m.emit(sop::Store, {b.element(spv::storage::Output, f32, outer, b.cu(e)), factors[e]});
    for (std::uint32_t k = 0; k < (quad ? 2U : 1U); ++k)
        m.emit(sop::Store, {b.element(spv::storage::Output, f32, inner, b.cu(k)), factors[edges + k]});

    m.execution_mode(b.function, mode_output_vertices, {count});
    m.execution_mode(b.function, quad ? mode_quads : mode_triangles);
    switch (setup.partition) {
    case TessellationSetup::Partition::pow2: case TessellationSetup::Partition::integer:
        m.execution_mode(b.function, mode_spacing_equal); break;
    case TessellationSetup::Partition::fractional_odd: m.execution_mode(b.function, mode_spacing_fractional_odd); break;
    case TessellationSetup::Partition::fractional_even: m.execution_mode(b.function, mode_spacing_fractional_even); break;
    }
    m.execution_mode(b.function, setup.clockwise ? mode_vertex_order_cw : mode_vertex_order_ccw);
    result.control = b.end(model_control);
    return result;
}

} // namespace alchemy::msl
