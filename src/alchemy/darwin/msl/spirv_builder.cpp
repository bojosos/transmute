#include "spirv_builder.hpp"

#include <cstring>

// SPIR-V module writer (owner: A6). See spirv_builder.hpp.
namespace alchemy::msl::spv {

namespace {
std::uint32_t instruction_word(std::uint32_t opcode, std::size_t count) {
    return static_cast<std::uint32_t>(count << 16) | opcode;
}
void push(Words& into, std::uint32_t opcode, const std::vector<std::uint32_t>& operands) {
    into.push_back(instruction_word(opcode, operands.size() + 1));
    into.insert(into.end(), operands.begin(), operands.end());
}
} // namespace

void Module::append_string(Words& words, std::string_view text) {
    const auto count = text.size() / 4 + 1;
    const auto start = words.size();
    words.resize(start + count, 0);
    std::memcpy(words.data() + start, text.data(), text.size());
}

std::uint32_t Module::glsl() {
    if (!glsl_id_) glsl_id_ = id();
    return glsl_id_;
}

// Types and constants share one deduplicating table: the key is the
// instruction's words without the result id.
std::uint32_t Module::type(Words key, bool) {
    const auto found = types_.find(key);
    if (found != types_.end()) return found->second;
    const auto result = id();
    Words operands;
    const auto opcode = key[0];
    // Constants have (type, result, ...); types have (result, ...).
    const bool is_constant = opcode == op::ConstantTrue || opcode == op::ConstantFalse || opcode == op::Constant ||
                             opcode == op::ConstantComposite || opcode == op::ConstantNull || opcode == op::Undef;
    if (is_constant) {
        operands.push_back(key[1]);
        operands.push_back(result);
        operands.insert(operands.end(), key.begin() + 2, key.end());
    } else {
        operands.push_back(result);
        operands.insert(operands.end(), key.begin() + 1, key.end());
    }
    push(globals_, opcode, operands);
    types_.emplace(std::move(key), result);
    return result;
}

std::uint32_t Module::type_array(std::uint32_t element, std::uint32_t length_id, std::uint32_t stride) {
    // The stride is part of the key only (a decoration, not an operand):
    // differently strided arrays are distinct types.
    const Words key = {op::TypeArray, element, length_id, 0xffffffffU, stride};
    const auto found = types_.find(key);
    if (found != types_.end()) return found->second;
    const auto result = id();
    push(globals_, op::TypeArray, {result, element, length_id});
    types_.emplace(key, result);
    if (stride) decorate(result, decoration::ArrayStride, {stride});
    return result;
}

std::uint32_t Module::type_struct(const std::vector<std::uint32_t>& members, bool block,
                                  const std::vector<std::uint32_t>& offsets) {
    // Decorated structs are distinct types: key them by layout.
    Words key = {op::TypeStruct};
    key.insert(key.end(), members.begin(), members.end());
    if (block || !offsets.empty()) {
        key.push_back(0xffffffffU);
        key.push_back(block ? 1U : 0U);
        key.insert(key.end(), offsets.begin(), offsets.end());
    }
    const auto found = types_.find(key);
    if (found != types_.end()) return found->second;
    const auto result = id();
    Words operands = {result};
    operands.insert(operands.end(), members.begin(), members.end());
    push(globals_, op::TypeStruct, operands);
    types_.emplace(std::move(key), result);
    if (block) decorate(result, decoration::Block);
    for (std::size_t i = 0; i < offsets.size(); ++i)
        member_decorate(result, static_cast<std::uint32_t>(i), decoration::Offset, {offsets[i]});
    return result;
}

std::uint32_t Module::type_function(std::uint32_t result_type, const std::vector<std::uint32_t>& params) {
    Words key = {op::TypeFunction, result_type};
    key.insert(key.end(), params.begin(), params.end());
    return type(std::move(key));
}

std::uint32_t Module::constant_f32(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return constant(type_float(32), {bits});
}

std::uint32_t Module::constant(std::uint32_t t, const std::vector<std::uint32_t>& words) {
    Words key = {op::Constant, t};
    key.insert(key.end(), words.begin(), words.end());
    return type(std::move(key), true);
}

std::uint32_t Module::constant_composite(std::uint32_t t, const std::vector<std::uint32_t>& parts) {
    Words key = {op::ConstantComposite, t};
    key.insert(key.end(), parts.begin(), parts.end());
    return type(std::move(key), true);
}

std::uint32_t Module::constant_null(std::uint32_t t) { return type({op::ConstantNull, t}, true); }
std::uint32_t Module::undef(std::uint32_t t) { return type({op::Undef, t}, true); }

std::uint32_t Module::spec_constant(std::uint32_t t, std::uint32_t spec_id, const std::vector<std::uint32_t>& default_words) {
    const auto result = id();
    Words operands = {t, result};
    operands.insert(operands.end(), default_words.begin(), default_words.end());
    push(globals_, op::SpecConstant, operands);
    decorate(result, decoration::SpecId, {spec_id});
    return result;
}

std::uint32_t Module::spec_constant_bool(std::uint32_t spec_id, bool default_value) {
    const auto result = id();
    push(globals_, default_value ? op::SpecConstantTrue : op::SpecConstantFalse, {type_bool(), result});
    decorate(result, decoration::SpecId, {spec_id});
    return result;
}

std::uint32_t Module::spec_constant_op(std::uint32_t t, std::uint32_t opcode, const std::vector<std::uint32_t>& operands) {
    const auto result = id();
    Words words = {t, result, opcode};
    words.insert(words.end(), operands.begin(), operands.end());
    push(globals_, op::SpecConstantOp, words);
    return result;
}

std::uint32_t Module::spec_constant_composite(std::uint32_t t, const std::vector<std::uint32_t>& parts) {
    const auto result = id();
    Words words = {t, result};
    words.insert(words.end(), parts.begin(), parts.end());
    push(globals_, op::SpecConstantComposite, words);
    return result;
}

std::uint32_t Module::variable(std::uint32_t pointer_type, std::uint32_t storage_class, std::uint32_t initializer) {
    const auto result = id();
    Words operands = {pointer_type, result, storage_class};
    if (initializer) operands.push_back(initializer);
    if (storage_class == storage::Function) push(function_variables_, op::Variable, operands);
    else push(globals_, op::Variable, operands);
    return result;
}

void Module::decorate(std::uint32_t target, std::uint32_t decoration_id, std::initializer_list<std::uint32_t> args) {
    Words operands = {target, decoration_id};
    operands.insert(operands.end(), args.begin(), args.end());
    push(annotations_, op::Decorate, operands);
}

void Module::member_decorate(std::uint32_t t, std::uint32_t member, std::uint32_t decoration_id,
                             std::initializer_list<std::uint32_t> args) {
    Words operands = {t, member, decoration_id};
    operands.insert(operands.end(), args.begin(), args.end());
    push(annotations_, op::MemberDecorate, operands);
}

void Module::name(std::uint32_t target, std::string_view text) {
    Words operands = {target};
    append_string(operands, text);
    push(debug_, op::Name, operands);
}

void Module::entry_point(std::uint32_t model, std::uint32_t function, std::string_view entry_name,
                         const std::vector<std::uint32_t>& interface) {
    Words operands = {model, function};
    append_string(operands, entry_name);
    operands.insert(operands.end(), interface.begin(), interface.end());
    push(entry_points_, op::EntryPoint, operands);
}

void Module::execution_mode(std::uint32_t function, std::uint32_t mode, std::initializer_list<std::uint32_t> args) {
    Words operands = {function, mode};
    operands.insert(operands.end(), args.begin(), args.end());
    push(execution_modes_, op::ExecutionMode, operands);
}

void Module::execution_mode_id(std::uint32_t function, std::uint32_t mode, std::initializer_list<std::uint32_t> args) {
    Words operands = {function, mode};
    operands.insert(operands.end(), args.begin(), args.end());
    push(execution_modes_, op::ExecutionModeId, operands);
}

void Module::emit(std::uint32_t opcode, const std::vector<std::uint32_t>& operands) { push(code_, opcode, operands); }

std::uint32_t Module::emit_value(std::uint32_t opcode, std::uint32_t result_type, const std::vector<std::uint32_t>& operands) {
    const auto result = id();
    Words words = {result_type, result};
    words.insert(words.end(), operands.begin(), operands.end());
    push(code_, opcode, words);
    return result;
}

Words Module::finish(std::uint32_t version, std::uint32_t addressing, std::uint32_t memory_model) {
    Words out = {0x07230203U, version, 0x00410000U /* generator: unregistered, Alchemy */, next_id_, 0};
    for (const auto value : capabilities_) push(out, op::Capability, {value});
    for (const auto& name : extensions_) {
        Words operands;
        append_string(operands, name);
        push(out, op::Extension, operands);
    }
    if (glsl_id_) {
        Words operands = {glsl_id_};
        append_string(operands, "GLSL.std.450");
        push(out, op::ExtInstImport, operands);
    }
    push(out, op::MemoryModel, {addressing, memory_model});
    out.insert(out.end(), entry_points_.begin(), entry_points_.end());
    out.insert(out.end(), execution_modes_.begin(), execution_modes_.end());
    out.insert(out.end(), debug_.begin(), debug_.end());
    out.insert(out.end(), annotations_.begin(), annotations_.end());
    out.insert(out.end(), globals_.begin(), globals_.end());
    out.insert(out.end(), code_.begin(), code_.end());
    out[3] = next_id_;
    return out;
}

} // namespace alchemy::msl::spv
