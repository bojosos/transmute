#include "lower.hpp"

#include "spirv_builder.hpp"
#include "structurize.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

// IR -> SPIR-V lowering (owner: A6). See lower.hpp for the memory and
// register model.
namespace alchemy::msl::detail {

std::uint32_t add_reflection_type(Reflection& reflection, const TypeSpelling& spelling) {
    Type type;
    type.kind = spelling.kind;
    type.scalar = spelling.scalar;
    type.components = spelling.components;
    type.columns = spelling.columns;
    type.packed = spelling.packed;
    type.name = spelling.text;
    auto scalar_size = [](Scalar s) -> std::uint32_t {
        switch (s) {
        case Scalar::boolean: case Scalar::int8: case Scalar::uint8: return 1;
        case Scalar::int16: case Scalar::uint16: case Scalar::half: case Scalar::bfloat: return 2;
        case Scalar::int64: case Scalar::uint64: return 8;
        case Scalar::none: return 0;
        default: return 4;
        }
    };
    const auto element = scalar_size(spelling.scalar);
    switch (spelling.kind) {
    case Type::Kind::scalar: type.size = type.alignment = element; break;
    case Type::Kind::vector:
        type.alignment = spelling.packed ? element : element * (spelling.components == 3 ? 4 : spelling.components);
        type.size = spelling.packed ? element * spelling.components : type.alignment;
        break;
    case Type::Kind::matrix: {
        const auto column = element * (spelling.components == 3 ? 4 : spelling.components);
        type.alignment = column;
        type.size = column * spelling.columns;
        break;
    }
    case Type::Kind::texture: case Type::Kind::sampler: type.size = 8; type.alignment = 8; break;
    default: break;
    }
    for (std::size_t i = 0; i < reflection.types.size(); ++i) {
        const auto& t = reflection.types[i];
        if (t.kind == type.kind && t.name == type.name && t.scalar == type.scalar) return static_cast<std::uint32_t>(i);
    }
    reflection.types.push_back(std::move(type));
    return static_cast<std::uint32_t>(reflection.types.size() - 1);
}

namespace {

using ir::Opcode;
using ir::Value;
namespace sop = spv::op;

struct LoweringError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void fail(const std::string& message) { throw LoweringError(message); }

enum class Space { none, physical, stack, workgroup, constdata, constant };

struct Ptr {
    Space space{Space::none};
    std::uint32_t id{};      // 0: constant offset only (word arrays)
    std::int64_t offset{};
};

struct Lowered {
    std::uint32_t id{};
    Ptr ptr;
    bool is_ptr{};
};

// SPIR-V memory semantics / scopes.
constexpr std::uint32_t scope_device = 1, scope_workgroup = 2, scope_subgroup = 3;
constexpr std::uint32_t sem_acq_rel = 0x8, sem_uniform = 0x40, sem_workgroup = 0x100, sem_image = 0x800;

class Lowerer {
public:
    Lowerer(ir::Module& module, const EntryPoint& entry, const std::vector<FunctionConstantDecl>& constants,
            const CompileOptions& options)
        : irm_(module), entry_(entry), constants_(constants), options_(options) {}

    LowerResult run() {
        LowerResult result;
        try {
            fn_ = irm_.function(entry_.wrapper);
            if (!fn_ || fn_->declaration) fail("entry wrapper " + entry_.wrapper + " missing from the IR");
            for (auto* block : fn_->blocks)
                for (auto* v : block->instructions)
                    if (v->op == Opcode::call && v->callee && !v->callee->declaration)
                        fail("call to '" + v->callee->name + "' was not inlined (recursion or a non-inlinable function)");
            cfg_ = structurize(irm_, *fn_);
            infer_spaces();
            plan_memory();
            prescan();
            emit_function();
            finish_entry();
            reflection_.stage = entry_.stage;
            build_reflection();
            const auto version = options_.vulkan_major > 1 || options_.vulkan_minor >= 3 ? 0x00010600U : 0x00010500U;
            result.spirv = m_.finish(version, uses_physical_ ? 5348U : 0U, 1U);
            result.reflection = std::move(reflection_);
        } catch (const LoweringError& error) {
            Diagnostic diagnostic;
            diagnostic.message = "in '" + entry_.name + "': " + error.what();
            diagnostic.line = entry_.line;
            result.diagnostics.push_back(std::move(diagnostic));
        } catch (const std::runtime_error& error) {
            Diagnostic diagnostic;
            diagnostic.message = "in '" + entry_.name + "': " + error.what();
            diagnostic.line = entry_.line;
            result.diagnostics.push_back(std::move(diagnostic));
        } catch (const std::logic_error& error) {
            // A lowering bug (a missing map entry and the like): report it
            // as an internal error rather than crashing Metal's caller.
            Diagnostic diagnostic;
            diagnostic.message = "in '" + entry_.name + "': internal compiler error: " + error.what() +
                (at_ ? " (lowering %" + at_->name + ", opcode " + std::to_string(static_cast<int>(at_->op)) + ")" : std::string());
            diagnostic.line = entry_.line;
            result.diagnostics.push_back(std::move(diagnostic));
        }
        return result;
    }

private:
    ir::Module& irm_;
    const EntryPoint& entry_;
    const std::vector<FunctionConstantDecl>& constants_;
    const CompileOptions& options_;
    ir::Function* fn_{};
    Value* at_{};   // the instruction being lowered (internal error reports)
    StructuredCfg cfg_;
    spv::Module m_;
    Reflection reflection_;
    bool uses_physical_{};
    std::vector<std::uint32_t> interface_;
    std::uint32_t function_id_{};

    // ---- types ----------------------------------------------------------------------------
    std::uint32_t t_void() { return m_.type_void(); }
    std::uint32_t t_bool() { return m_.type_bool(); }
    std::uint32_t t_u32() { return m_.type_int(32, false); }
    std::uint32_t t_i32() { return m_.type_int(32, true); }
    std::uint32_t t_u64() { m_.capability(spv::capability::Int64); return m_.type_int(64, false); }
    std::uint32_t t_f32() { return m_.type_float(32); }
    std::uint32_t t_vec(std::uint32_t element, std::uint32_t n) { return m_.type_vector(element, n); }
    std::uint32_t c_u32(std::uint32_t v) { return m_.constant_u32(v); }
    std::uint32_t c_i32(std::int32_t v) { return m_.constant_i32(v); }
    std::uint32_t c_u64(std::uint64_t v) { t_u64(); return m_.constant_u64(v); }

    static std::uint32_t scalar_bits(const ir::Type* t) {
        t = t->scalar();
        switch (t->kind) {
        case ir::Type::Kind::integer: return t->bits;
        case ir::Type::Kind::half: return 16;
        case ir::Type::Kind::float_: return 32;
        case ir::Type::Kind::double_: return 64;
        case ir::Type::Kind::pointer: return 64;
        default: return 0;
        }
    }

    std::uint32_t reg(const ir::Type* t) {
        switch (t->kind) {
        case ir::Type::Kind::void_: return t_void();
        case ir::Type::Kind::integer:
            if (t->bits == 1) return t_bool();
            if (t->bits <= 32) return t_u32();
            if (t->bits == 64) return t_u64();
            fail("integer type " + ir::to_string(t) + " is unsupported");
        case ir::Type::Kind::half: case ir::Type::Kind::float_: return t_f32();
        case ir::Type::Kind::double_: fail("64-bit floating point (double) is unsupported");
        case ir::Type::Kind::bfloat: fail("bfloat is unsupported");
        case ir::Type::Kind::vector: {
            if (t->count > 4) fail("vector type " + ir::to_string(t) + " is wider than 4 components");
            if (t->element->is_pointer()) fail("vectors of pointers are unsupported");
            return t_vec(reg(t->element), static_cast<std::uint32_t>(t->count));
        }
        case ir::Type::Kind::array:
            return m_.type_array(reg(t->element), c_u32(static_cast<std::uint32_t>(t->count)));
        case ir::Type::Kind::structure: {
            std::vector<std::uint32_t> members;
            for (const auto* member : t->members) members.push_back(reg(member));
            return m_.type_struct(members);
        }
        case ir::Type::Kind::pointer: fail("aggregate containing a pointer");
        default: fail("unsupported type " + ir::to_string(t));
        }
    }
    std::uint32_t rep_type(Space space) { return space == Space::physical ? t_u64() : t_u32(); }

    // Splat a 32-bit constant to the shape of `t` (scalar or vector of u32).
    std::uint32_t splat_u32(const ir::Type* t, std::uint32_t value) {
        const auto scalar = c_u32(value);
        if (!t->is_vector()) return scalar;
        std::vector<std::uint32_t> parts(t->count, scalar);
        return m_.constant_composite(reg(t), parts);
    }
    std::uint32_t splat(std::uint32_t type, std::uint32_t scalar, std::uint32_t lanes) {
        if (lanes == 1) return scalar;
        return m_.constant_composite(type, std::vector<std::uint32_t>(lanes, scalar));
    }

    std::uint32_t value_op(std::uint32_t opcode, std::uint32_t type, std::vector<std::uint32_t> operands) {
        return m_.emit_value(opcode, type, operands);
    }
    std::uint32_t ext(std::uint32_t type, std::uint32_t instruction, std::vector<std::uint32_t> operands) {
        std::vector<std::uint32_t> words = {m_.glsl(), instruction};
        words.insert(words.end(), operands.begin(), operands.end());
        return m_.emit_value(sop::ExtInst, type, words);
    }

    // Narrow integers (i8/i16) are kept zero-extended in 32-bit registers.
    std::uint32_t normalise(std::uint32_t id, const ir::Type* t) {
        const auto bits = scalar_bits(t);
        if (!t->scalar()->is_int() || bits == 1 || bits >= 32) return id;
        return value_op(sop::BitwiseAnd, reg(t), {id, splat_u32(t, (1U << bits) - 1)});
    }
    std::uint32_t sign_extend(std::uint32_t id, const ir::Type* t) {
        const auto bits = scalar_bits(t);
        if (!t->scalar()->is_int() || bits == 1 || bits >= 32) return id;
        return value_op(sop::BitFieldSExtract, reg(t), {id, c_u32(0), c_u32(bits)});
    }

    // ---- values ------------------------------------------------------------------------------
    std::unordered_map<Value*, Lowered> values_;
    std::unordered_map<ir::BasicBlock*, std::uint32_t> labels_;
    std::unordered_map<Value*, std::uint32_t> demoted_vars_;
    ir::BasicBlock* current_{};

    std::uint32_t label(ir::BasicBlock* block) {
        auto& id = labels_[block];
        if (!id) id = m_.id();
        return id;
    }

    bool fc_global(const Value* v, std::uint32_t& index) const {
        if (v->kind != Value::Kind::global) return false;
        std::string name = v->name;
        if (!name.empty() && name[0] == '\x01') name = name.substr(1);
        if (name.rfind("msl.fc.", 0) != 0) return false;
        index = static_cast<std::uint32_t>(std::stoul(name.substr(7)));
        return true;
    }

    std::uint32_t constant(Value* c, const ir::Type* t) {
        switch (c->kind) {
        case Value::Kind::constant_int: {
            const auto bits = scalar_bits(t);
            if (t->is_vector()) {
                const auto element = constant(c, t->element);
                return splat(reg(t), element, static_cast<std::uint32_t>(t->count));
            }
            if (bits == 1) return m_.constant_bool(c->int_value != 0);
            if (bits <= 32) return c_u32(static_cast<std::uint32_t>(c->int_value));
            if (bits == 64) return c_u64(c->int_value);
            fail("integer constant of " + std::to_string(bits) + " bits");
        }
        case Value::Kind::constant_fp: {
            if (t->is_vector()) return splat(reg(t), constant(c, t->element), static_cast<std::uint32_t>(t->count));
            if (t->kind == ir::Type::Kind::double_) fail("64-bit floating point (double) is unsupported");
            return m_.constant_f32(static_cast<float>(c->fp_value));
        }
        case Value::Kind::constant_null: {
            if (t->is_int() && t->bits == 1) return m_.constant_bool(false);
            if (t->is_int() && t->bits <= 32) return c_u32(0);
            if (t->is_int()) return c_u64(0);
            if (t->is_float()) return m_.constant_f32(0);
            return m_.constant_null(reg(t));
        }
        case Value::Kind::constant_undef: case Value::Kind::constant_poison:
            return m_.undef(reg(t));
        case Value::Kind::constant_aggregate: {
            std::vector<std::uint32_t> parts;
            for (std::size_t i = 0; i < c->operands.size(); ++i) {
                const ir::Type* element = t->kind == ir::Type::Kind::structure ? t->members[i] : t->element;
                auto* part = c->operands[i];
                // Composite constants take constants only: poison lanes become zero.
                if (part->is_undef()) parts.push_back(constant_null_of(element));
                else parts.push_back(constant(part, element));
            }
            return m_.constant_composite(reg(t), parts);
        }
        case Value::Kind::constant_expr:
            if (c->op == Opcode::bitcast && !t->is_pointer()) return bitcast(constant(c->operands[0], c->operands[0]->type),
                c->operands[0]->type, t);
            fail("constant expression in a non-pointer context");
        default: fail("unexpected constant");
        }
    }
    std::uint32_t constant_null_of(const ir::Type* t) {
        Value zero;
        zero.kind = Value::Kind::constant_null;
        return constant(&zero, t);
    }

    // The register id of an operand (loading demoted values in other blocks).
    std::uint32_t id_of(Value* v) {
        if (v->is_constant() && !v->type->is_pointer()) return constant(v, v->type);
        if (v->type->is_pointer()) return materialise(ptr_of(v));
        if (wide_.count(v))
            fail(std::to_string(v->type->bits) + "-bit integer values are unsupported (only loads split by shifts "
                 "and truncations)");
        return lowered(v).id;
    }
    const Lowered& lowered(Value* v) {
        auto* key = demotion_key(v);
        if (cfg_.demoted.count(key) && v->parent != current_) {
            const auto cache_key = std::pair(key, current_);
            auto found = demoted_cache_.find(cache_key);
            if (found == demoted_cache_.end()) {
                // The definition (or a tail-duplicated copy of it) may be
                // emitted after this use in block order: derive the
                // variable's type from the IR, not from the lowered value.
                Lowered loaded;
                loaded.is_ptr = v->type->is_pointer();
                Space space = Space::none;
                if (loaded.is_ptr) {
                    const auto def = values_.find(key);
                    if (def != values_.end() && def->second.is_ptr) space = def->second.ptr.space;
                    else if (spaces_.count(key)) space = spaces_.at(key);
                    else fail("pointer %" + v->name + " has no inferred address space");
                }
                const auto type = loaded.is_ptr ? rep_type(space) : reg(v->type);
                const auto id = value_op(sop::Load, type, {demoted_var(key, type)});
                if (loaded.is_ptr) loaded.ptr = {space, id, 0};
                else loaded.id = id;
                found = demoted_cache_.emplace(cache_key, loaded).first;
            }
            return found->second;
        }
        const auto found = values_.find(v);
        if (found == values_.end()) fail("value %" + v->name + " used before its definition");
        return found->second;
    }
    std::uint32_t demoted_var(Value* key, std::uint32_t type) {
        auto& var = demoted_vars_[key];
        if (!var) var = m_.variable(m_.type_pointer(spv::storage::Function, type), spv::storage::Function);
        return var;
    }
    std::map<std::pair<Value*, ir::BasicBlock*>, Lowered> demoted_cache_;

    void bind(Value* v, std::uint32_t id) {
        Lowered l;
        l.id = id;
        values_[v] = l;
        store_demoted(v);
    }
    void bind_ptr(Value* v, Ptr p) {
        Lowered l;
        l.is_ptr = true;
        l.ptr = p;
        if (cfg_.demoted.count(demotion_key(v)) && p.offset) l.ptr = {p.space, materialise(p), 0};
        values_[v] = l;
        store_demoted(v);
    }
    // A tail-duplicated copy of a demoted value shares the original's variable.
    Value* demotion_key(Value* v) const {
        const auto found = cfg_.clone_of.find(v);
        return found == cfg_.clone_of.end() ? v : found->second;
    }
    void store_demoted(Value* v) {
        auto* key = demotion_key(v);
        if (!cfg_.demoted.count(key)) return;
        auto& def = values_.at(v);
        std::uint32_t type, id;
        if (def.is_ptr) {
            def.ptr = {def.ptr.space, materialise(def.ptr), 0};
            type = rep_type(def.ptr.space);
            id = def.ptr.id;
        } else {
            type = reg(v->type);
            id = def.id;
        }
        m_.emit(sop::Store, {demoted_var(key, type), id});
    }

    // ---- pointer spaces ---------------------------------------------------------------------
    std::unordered_map<Value*, Space> spaces_;

    Space space_of_operand(Value* v) {
        if (v->kind == Value::Kind::global) {
            std::uint32_t index;
            if (fc_global(v, index)) return Space::constant;
            return Space::constdata;
        }
        if (v->kind == Value::Kind::constant_expr) return space_of_operand(v->operands[0]);
        if (v->is_constant()) return Space::none;
        const auto found = spaces_.find(v);
        return found == spaces_.end() ? Space::none : found->second;
    }
    static Space join(Space a, Space b, bool& conflict) {
        if (a == Space::none) return b;
        if (b == Space::none || a == b) return a;
        conflict = true;
        return a;
    }
    void infer_spaces() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto* block : fn_->blocks) {
                for (auto* v : block->instructions) {
                    if (!v->type || !v->type->is_pointer()) continue;
                    Space space = Space::none;
                    bool conflict = false;
                    switch (v->op) {
                    case Opcode::alloca: space = Space::stack; break;
                    case Opcode::getelementptr: case Opcode::addrspacecast: case Opcode::bitcast: case Opcode::freeze:
                        space = space_of_operand(v->operands[0]);
                        break;
                    case Opcode::phi:
                        for (auto* o : v->operands) space = join(space, space_of_operand(o), conflict);
                        break;
                    case Opcode::select:
                        space = join(space_of_operand(v->operands[1]), space_of_operand(v->operands[2]), conflict);
                        break;
                    case Opcode::load: {
                        const auto as = v->type->address_space;
                        space = as == 1 || as == 2 ? Space::physical : as == 3 ? Space::workgroup : as == 0 ? Space::stack
                            : Space::none;
                        if (as == 4) fail("a generic pointer loaded from memory is unsupported");
                        break;
                    }
                    case Opcode::inttoptr: space = Space::physical; break;
                    case Opcode::call: {
                        const auto name = v->callee ? ir::demangled_identifier(v->callee->name) : std::string{};
                        if (name == "__msl_buffer") space = Space::physical;
                        else if (name == "__msl_threadgroup_storage" || name == "__msl_threadgroup_param") space = Space::workgroup;
                        else fail("call returning a pointer: " + (v->callee ? v->callee->name : std::string("?")));
                        break;
                    }
                    default: fail("unsupported pointer-producing instruction");
                    }
                    if (conflict) fail("a pointer that may point into different address spaces (generic pointer "
                                       "joining device, thread or threadgroup memory) is unsupported");
                    auto& slot = spaces_[v];
                    if (slot != space) {
                        slot = space;
                        changed = true;
                    }
                }
            }
        }
    }

    // ---- memory planning ------------------------------------------------------------------
    std::unordered_map<Value*, std::uint64_t> alloca_offsets_;
    std::uint64_t stack_size_{};
    std::uint32_t stack_var_{};
    std::map<std::uint32_t, std::uint64_t> threadgroup_storage_offsets_;   // by id
    std::uint64_t threadgroup_static_{};
    std::uint32_t workgroup_var_{}, workgroup_words_{};
    std::map<std::uint32_t, std::uint32_t> threadgroup_param_offsets_;     // index -> spec constant offset id
    std::unordered_map<Value*, std::uint64_t> constdata_offsets_;
    std::vector<std::uint8_t> constdata_;
    std::uint32_t constdata_var_{};

    static std::uint64_t round_up(std::uint64_t v, std::uint64_t a) { return a ? (v + a - 1) / a * a : v; }

    void plan_memory() {
        for (auto* block : fn_->blocks) {
            for (auto* v : block->instructions) {
                if (v->op == Opcode::alloca) {
                    const auto align = std::max<std::uint64_t>(4, std::max(v->alignment, ir::align_of(v->aux_type)));
                    stack_size_ = round_up(stack_size_, align);
                    alloca_offsets_[v] = stack_size_;
                    stack_size_ += std::max<std::uint64_t>(1, ir::size_of(v->aux_type));
                }
                if (v->op == Opcode::call && v->callee && ir::demangled_identifier(v->callee->name) == "__msl_threadgroup_storage") {
                    const auto id = static_cast<std::uint32_t>(const_int(v->operands[0]));
                    if (threadgroup_storage_offsets_.count(id)) continue;
                    const auto size = const_int(v->operands[1]);
                    const auto align = std::max<std::uint64_t>(4, const_int(v->operands[2]));
                    threadgroup_static_ = round_up(threadgroup_static_, align);
                    threadgroup_storage_offsets_[id] = threadgroup_static_;
                    threadgroup_static_ += size;
                }
                for (auto* operand : v->operands) collect_constdata(operand);
            }
        }
        stack_size_ = round_up(stack_size_, 4);
        threadgroup_static_ = round_up(threadgroup_static_, 16);
    }
    // A program-scope variable whose initializer is not a constant expression
    // is written by a global constructor (clang's _GLOBAL__sub_I_* /
    // __cxx_global_var_init functions, run through llvm.global_ctors), which
    // no entry point runs: its constant data would read as zeros.
    bool dynamically_initialized(const ir::Global* g) {
        if (!dynamic_globals_) {
            dynamic_globals_.emplace();
            for (const auto* f : irm_.functions) {
                if (f->declaration || (f->name.rfind("_GLOBAL__sub_I", 0) != 0 && f->name.rfind("__cxx_global_var_init", 0) != 0))
                    continue;
                for (const auto* block : f->blocks)
                    for (const auto* i : block->instructions) {
                        const Value* target = nullptr;
                        if (i->op == Opcode::store && i->operands.size() > 1) target = i->operands[1];
                        else if (i->op == Opcode::call && i->callee && !i->operands.empty() &&
                                 (i->callee->name.rfind("llvm.memcpy", 0) == 0 || i->callee->name.rfind("llvm.memset", 0) == 0))
                            target = i->operands[0];
                        while (target && !target->operands.empty() &&
                               (target->kind == Value::Kind::constant_expr ||
                                (target->kind == Value::Kind::instruction && target->op == Opcode::getelementptr)))
                            target = target->operands[0];
                        if (target && target->kind == Value::Kind::global) dynamic_globals_->insert(target);
                    }
            }
        }
        return dynamic_globals_->count(g) != 0;
    }
    std::optional<std::set<const Value*>> dynamic_globals_;

    void collect_constdata(Value* v) {
        if (v->kind == Value::Kind::constant_expr) {
            for (auto* o : v->operands) collect_constdata(o);
            return;
        }
        if (v->kind != Value::Kind::global) return;
        std::uint32_t index;
        if (fc_global(v, index) || constdata_offsets_.count(v)) return;
        auto* g = static_cast<ir::Global*>(v);
        if (!g->initializer) fail("external global @" + g->name + " is unsupported");
        if (dynamically_initialized(g))
            fail("program-scope variable @" + g->name + " has a dynamic initializer (not a constant expression), "
                 "which is unsupported");
        const auto align = std::max<std::uint64_t>(4, std::max(g->alignment, ir::align_of(g->value_type)));
        constdata_.resize(round_up(constdata_.size(), align), 0);
        constdata_offsets_[v] = constdata_.size();
        const auto start = constdata_.size();
        constdata_.resize(start + ir::size_of(g->value_type), 0);
        serialise(g->initializer, g->value_type, constdata_.data() + start);
    }
    void serialise(Value* c, const ir::Type* t, std::uint8_t* out) {
        switch (c->kind) {
        case Value::Kind::constant_null: case Value::Kind::constant_undef: case Value::Kind::constant_poison: return;
        case Value::Kind::constant_int: {
            if (t->is_vector()) {
                for (std::uint64_t i = 0; i < t->count; ++i) serialise(c, t->element, out + i * ir::size_of(t->element));
                return;
            }
            const auto bytes = std::max<std::uint64_t>(1, (scalar_bits(t) + 7) / 8);
            for (std::uint64_t i = 0; i < bytes && i < 8; ++i) out[i] = static_cast<std::uint8_t>(c->int_value >> (8 * i));
            return;
        }
        case Value::Kind::constant_fp: {
            if (t->is_vector()) {
                for (std::uint64_t i = 0; i < t->count; ++i) serialise(c, t->element, out + i * ir::size_of(t->element));
                return;
            }
            if (t->kind == ir::Type::Kind::float_) {
                const float f = static_cast<float>(c->fp_value);
                std::memcpy(out, &f, 4);
            } else if (t->kind == ir::Type::Kind::half) {
                const auto h = to_half(static_cast<float>(c->fp_value));
                std::memcpy(out, &h, 2);
            } else {
                std::memcpy(out, &c->fp_value, 8);
            }
            return;
        }
        case Value::Kind::constant_aggregate: {
            for (std::size_t i = 0; i < c->operands.size(); ++i) {
                if (t->kind == ir::Type::Kind::structure)
                    serialise(c->operands[i], t->members[i], out + ir::member_offset(t, i));
                else
                    serialise(c->operands[i], t->element, out + i * ir::size_of(t->element));
            }
            return;
        }
        default: fail("global initializer with relocations (pointers to globals) is unsupported");
        }
    }
    static std::uint16_t to_half(float f) {
        std::uint32_t x;
        std::memcpy(&x, &f, 4);
        const std::uint32_t sign = (x >> 16) & 0x8000;
        const int exponent = static_cast<int>((x >> 23) & 0xff) - 127 + 15;
        std::uint32_t mantissa = x & 0x7fffff;
        if (((x >> 23) & 0xff) == 0xff) return static_cast<std::uint16_t>(sign | 0x7c00 | (mantissa ? 0x200 : 0));
        if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7c00);
        if (exponent <= 0) {
            if (exponent < -10) return static_cast<std::uint16_t>(sign);
            mantissa |= 0x800000;
            return static_cast<std::uint16_t>(sign | (mantissa >> (14 - exponent)));
        }
        return static_cast<std::uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
    }

    std::uint64_t const_int(Value* v) {
        if (v->kind != Value::Kind::constant_int)
            fail("expected a constant operand" + (at_ && at_->callee ? " in a call to " + at_->callee->name : std::string()));
        return v->int_value;
    }
    std::optional<std::uint64_t> maybe_const(Value* v) {
        if (v->kind == Value::Kind::constant_int) return v->int_value;
        if (v->kind == Value::Kind::constant_null) return 0;
        return std::nullopt;
    }

    std::uint32_t word_array(Space space) {
        switch (space) {
        case Space::stack: {
            if (!stack_var_) {
                const auto words = static_cast<std::uint32_t>(stack_size_ / 4 + 1);
                const auto array = m_.type_array(t_u32(), c_u32(words));
                stack_var_ = m_.variable(m_.type_pointer(spv::storage::Function, array), spv::storage::Function);
            }
            return stack_var_;
        }
        case Space::workgroup: {
            if (!workgroup_var_) {
                std::uint32_t length;
                if (entry_.threadgroups.empty()) {
                    // No pad word: a kernel using exactly the device's limit
                    // (Blender's 2 x 4096 uint zbins = 32 KB) must fit it.
                    length = c_u32(static_cast<std::uint32_t>(std::max<std::uint64_t>(1, (threadgroup_static_ + 3) / 4)));
                } else {
                    // static words + parameter lengths (spec constants, bytes); a pad
                    // word only when nothing is static (the array is never empty).
                    std::uint32_t total = c_u32(static_cast<std::uint32_t>(threadgroup_static_));
                    for (const auto& group : entry_.threadgroups) {
                        threadgroup_param_offsets_[group.index] = total;
                        const auto length_bytes = m_.spec_constant(t_u32(),
                            reserved_spec_id::threadgroup_memory_base + group.index, {16});
                        const auto rounded = m_.spec_constant_op(t_u32(), sop::BitwiseAnd,
                            {m_.spec_constant_op(t_u32(), sop::IAdd, {length_bytes, c_u32(15)}), c_u32(~15U)});
                        total = m_.spec_constant_op(t_u32(), sop::IAdd, {total, rounded});
                    }
                    length = m_.spec_constant_op(t_u32(), sop::ShiftRightLogical, {total, c_u32(2)});
                    if (threadgroup_static_ == 0) length = m_.spec_constant_op(t_u32(), sop::IAdd, {length, c_u32(1)});
                }
                const auto array = m_.type_array(t_u32(), length);
                workgroup_var_ = m_.variable(m_.type_pointer(spv::storage::Workgroup, array), spv::storage::Workgroup);
                interface_.push_back(workgroup_var_);
            }
            return workgroup_var_;
        }
        case Space::constdata: {
            if (!constdata_var_) {
                const auto words = static_cast<std::uint32_t>(round_up(constdata_.size(), 4) / 4 + 1);
                constdata_.resize(words * 4, 0);
                std::vector<std::uint32_t> parts;
                for (std::uint32_t i = 0; i < words; ++i) {
                    std::uint32_t word;
                    std::memcpy(&word, constdata_.data() + i * 4, 4);
                    parts.push_back(c_u32(word));
                }
                const auto array = m_.type_array(t_u32(), c_u32(words));
                const auto init = m_.constant_composite(array, parts);
                constdata_var_ = m_.variable(m_.type_pointer(spv::storage::Private, array), spv::storage::Private, init);
                interface_.push_back(constdata_var_);
            }
            return constdata_var_;
        }
        default: fail("no word array for this pointer");
        }
    }
    std::uint32_t storage_class(Space space) {
        return space == Space::stack ? spv::storage::Function : space == Space::workgroup ? spv::storage::Workgroup
                                                                                          : spv::storage::Private;
    }

    std::uint32_t materialise(Ptr p) {
        if (p.space == Space::physical) {
            if (!p.offset) return p.id;
            return value_op(sop::IAdd, t_u64(), {p.id, c_u64(static_cast<std::uint64_t>(p.offset))});
        }
        if (!p.id) return c_u32(static_cast<std::uint32_t>(p.offset));
        if (!p.offset) return p.id;
        return value_op(sop::IAdd, t_u32(), {p.id, c_u32(static_cast<std::uint32_t>(p.offset))});
    }

    Ptr ptr_of(Value* v) {
        switch (v->kind) {
        case Value::Kind::global: {
            std::uint32_t index;
            if (fc_global(v, index)) return {Space::constant, index, 0};
            return {Space::constdata, 0, static_cast<std::int64_t>(constdata_offsets_.at(v))};
        }
        case Value::Kind::constant_null: case Value::Kind::constant_undef: case Value::Kind::constant_poison: {
            const auto as = v->type->address_space;
            if (as == 1 || as == 2) return {Space::physical, c_u64(0), 0};
            return {as == 3 ? Space::workgroup : Space::stack, 0, 0};
        }
        case Value::Kind::constant_expr: {
            if (v->op == Opcode::addrspacecast || v->op == Opcode::bitcast) return ptr_of(v->operands[0]);
            if (v->op == Opcode::getelementptr) return gep(v);
            if (v->op == Opcode::inttoptr) return {Space::physical, id_of(v->operands[0]), 0};
            fail("unsupported pointer constant expression");
        }
        default: {
            const auto& l = lowered(v);
            if (!l.is_ptr) fail("value %" + v->name + " is not a pointer");
            return l.ptr;
        }
        }
    }

    // GEP: base + index0 * size(source) + member/element offsets.
    Ptr gep(Value* v) {
        Ptr p = ptr_of(v->operands[0]);
        if (p.space == Space::constant) fail("address arithmetic on a function constant");
        const bool physical = p.space == Space::physical;
        const ir::Type* current = v->aux_type;
        for (std::size_t i = 1; i < v->operands.size(); ++i) {
            auto* index = v->operands[i];
            std::uint64_t stride = 0;
            if (i == 1) {
                stride = ir::size_of(current);
            } else if (current->kind == ir::Type::Kind::structure) {
                const auto member = static_cast<std::size_t>(const_int(index));
                p.offset += static_cast<std::int64_t>(ir::member_offset(current, member));
                current = current->members.at(member);
                continue;
            } else {
                current = current->element;
                stride = ir::size_of(current);
            }
            if (const auto constant_index = maybe_const(index)) {
                const auto bits = scalar_bits(index->type);
                std::int64_t signed_index = static_cast<std::int64_t>(*constant_index);
                if (bits < 64 && (*constant_index >> (bits - 1)) & 1) signed_index -= static_cast<std::int64_t>(1ULL << bits);
                p.offset += signed_index * static_cast<std::int64_t>(stride);
                continue;
            }
            if (index->type->is_vector()) fail("vector GEP indices are unsupported");
            // Dynamic index, sign-extended to the address width.
            auto idx = id_of(index);
            const auto bits = scalar_bits(index->type);
            std::uint32_t scaled;
            if (physical) {
                if (bits < 64) idx = value_op(sop::SConvert, t_u64(), {sign_extend(idx, index->type)});
                scaled = stride == 1 ? idx : value_op(sop::IMul, t_u64(), {idx, c_u64(stride)});
                p.id = value_op(sop::IAdd, t_u64(), {p.id, scaled});
            } else {
                if (bits == 64) idx = value_op(sop::UConvert, t_u32(), {idx});
                else idx = sign_extend(idx, index->type);
                scaled = stride == 1 ? idx : value_op(sop::IMul, t_u32(), {idx, c_u32(static_cast<std::uint32_t>(stride))});
                p.id = p.id ? value_op(sop::IAdd, t_u32(), {p.id, scaled}) : scaled;
            }
        }
        return p;
    }

    // ---- word-array memory ---------------------------------------------------------------
    struct Address {
        std::uint32_t word{};               // u32 word index id
        std::optional<std::uint32_t> sub;   // byte within the word, when known
        std::uint32_t sub_id{};             // byte within the word (dynamic)
    };
    Address address(Ptr p, std::uint64_t at, std::uint64_t align) {
        Address a;
        const auto total = p.offset + static_cast<std::int64_t>(at);
        if (!p.id) {
            a.word = c_u32(static_cast<std::uint32_t>(total / 4));
            a.sub = static_cast<std::uint32_t>(total % 4);
            return a;
        }
        const auto byte = total ? value_op(sop::IAdd, t_u32(), {p.id, c_u32(static_cast<std::uint32_t>(total))}) : p.id;
        a.word = value_op(sop::ShiftRightLogical, t_u32(), {byte, c_u32(2)});
        if (align >= 4 && total % 4 == 0) a.sub = 0;
        else a.sub_id = value_op(sop::BitwiseAnd, t_u32(), {byte, c_u32(3)});
        return a;
    }
    std::uint32_t word_pointer(Space space, std::uint32_t word) {
        return value_op(sop::AccessChain, m_.type_pointer(storage_class(space), t_u32()), {word_array(space), word});
    }
    std::uint32_t load_word(Space space, std::uint32_t word) {
        return value_op(sop::Load, t_u32(), {word_pointer(space, word)});
    }
    // `bytes` (1, 2 or 4) little-endian bytes at `a` as a u32.
    std::uint32_t load_bytes(Space space, const Address& a, std::uint32_t bytes) {
        if (a.sub && *a.sub + bytes <= 4) {
            const auto w = load_word(space, a.word);
            auto r = *a.sub ? value_op(sop::ShiftRightLogical, t_u32(), {w, c_u32(*a.sub * 8)}) : w;
            if (bytes < 4) r = value_op(sop::BitwiseAnd, t_u32(), {r, c_u32((1U << (bytes * 8)) - 1)});
            return r;
        }
        // Possibly straddling two words.
        const auto sub = a.sub ? c_u32(*a.sub) : a.sub_id;
        const auto w0 = load_word(space, a.word);
        const auto w1 = load_word(space, value_op(sop::IAdd, t_u32(), {a.word, c_u32(1)}));
        const auto shift = value_op(sop::ShiftLeftLogical, t_u32(), {sub, c_u32(3)});
        const auto lo = value_op(sop::ShiftRightLogical, t_u32(), {w0, shift});
        const auto hi = value_op(sop::ShiftLeftLogical, t_u32(), {w1, value_op(sop::ISub, t_u32(), {c_u32(32), shift})});
        const auto zero = value_op(sop::IEqual, t_bool(), {shift, c_u32(0)});
        auto r = value_op(sop::Select, t_u32(), {zero, w0, value_op(sop::BitwiseOr, t_u32(), {lo, hi})});
        if (bytes < 4) r = value_op(sop::BitwiseAnd, t_u32(), {r, c_u32((1U << (bytes * 8)) - 1)});
        return r;
    }
    void store_bytes(Space space, const Address& a, std::uint32_t bytes, std::uint32_t value) {
        if (space == Space::constdata) fail("store to constant data");
        if (a.sub && *a.sub == 0 && bytes == 4) {
            m_.emit(sop::Store, {word_pointer(space, a.word), value});
            return;
        }
        if (a.sub && *a.sub + bytes <= 4) {
            const auto mask = bytes == 4 ? 0xffffffffU : ((1U << (bytes * 8)) - 1) << (*a.sub * 8);
            const auto shifted = *a.sub ? value_op(sop::ShiftLeftLogical, t_u32(), {value, c_u32(*a.sub * 8)}) : value;
            rmw(space, a.word, c_u32(~mask), shifted);
            return;
        }
        // Unknown or straddling position: byte by byte.
        const auto sub = a.sub ? c_u32(*a.sub) : a.sub_id;
        for (std::uint32_t i = 0; i < bytes; ++i) {
            const auto position = value_op(sop::IAdd, t_u32(), {sub, c_u32(i)});
            const auto word = value_op(sop::IAdd, t_u32(), {a.word, value_op(sop::ShiftRightLogical, t_u32(), {position, c_u32(2)})});
            const auto shift = value_op(sop::ShiftLeftLogical, t_u32(), {value_op(sop::BitwiseAnd, t_u32(), {position, c_u32(3)}), c_u32(3)});
            const auto byte = value_op(sop::BitwiseAnd, t_u32(), {value_op(sop::ShiftRightLogical, t_u32(), {value, c_u32(i * 8)}), c_u32(0xff)});
            const auto mask = value_op(sop::Not, t_u32(), {value_op(sop::ShiftLeftLogical, t_u32(), {c_u32(0xff), shift})});
            rmw(space, word, mask, value_op(sop::ShiftLeftLogical, t_u32(), {byte, shift}));
        }
    }
    // word = (word & keep) | set; atomic for workgroup memory (other threads
    // may own the neighbouring bytes).
    void rmw(Space space, std::uint32_t word, std::uint32_t keep, std::uint32_t set) {
        const auto pointer = word_pointer(space, word);
        if (space == Space::workgroup) {
            value_op(sop::AtomicAnd, t_u32(), {pointer, c_u32(scope_workgroup), c_u32(0), keep});
            value_op(sop::AtomicOr, t_u32(), {pointer, c_u32(scope_workgroup), c_u32(0), set});
            return;
        }
        const auto old = value_op(sop::Load, t_u32(), {pointer});
        const auto next = value_op(sop::BitwiseOr, t_u32(), {value_op(sop::BitwiseAnd, t_u32(), {old, keep}), set});
        m_.emit(sop::Store, {pointer, next});
    }

    // Raw u32 bits of a leaf (<= 4 bytes) to a register value and back.
    std::uint32_t from_raw(std::uint32_t raw, const ir::Type* leaf) {
        switch (leaf->kind) {
        case ir::Type::Kind::float_: return value_op(sop::Bitcast, t_f32(), {raw});
        case ir::Type::Kind::half: {
            const auto pair = ext(t_vec(t_f32(), 2), spv::glsl::UnpackHalf2x16, {raw});
            return value_op(sop::CompositeExtract, t_f32(), {pair, 0});
        }
        case ir::Type::Kind::integer:
            if (leaf->bits == 1) return value_op(sop::INotEqual, t_bool(), {raw, c_u32(0)});
            return raw;
        default: fail("unexpected memory leaf type " + ir::to_string(leaf));
        }
    }
    std::uint32_t to_raw(std::uint32_t value, const ir::Type* leaf) {
        switch (leaf->kind) {
        case ir::Type::Kind::float_: return value_op(sop::Bitcast, t_u32(), {value});
        case ir::Type::Kind::half: {
            const auto pair = value_op(sop::CompositeConstruct, t_vec(t_f32(), 2), {value, m_.constant_f32(0)});
            return ext(t_u32(), spv::glsl::PackHalf2x16, {pair});
        }
        case ir::Type::Kind::integer:
            if (leaf->bits == 1) return value_op(sop::Select, t_u32(), {value, c_u32(1), c_u32(0)});
            return value;
        default: fail("unexpected memory leaf type " + ir::to_string(leaf));
        }
    }

    // Loads `t` from a word array.
    std::uint32_t load_words(Ptr p, const ir::Type* t, std::uint64_t at, std::uint64_t align) {
        switch (t->kind) {
        case ir::Type::Kind::vector: case ir::Type::Kind::array: case ir::Type::Kind::structure: {
            std::vector<std::uint32_t> parts;
            const auto count = t->kind == ir::Type::Kind::structure ? t->members.size() : t->count;
            for (std::uint64_t i = 0; i < count; ++i) {
                const ir::Type* element = t->kind == ir::Type::Kind::structure ? t->members[i] : t->element;
                const auto offset = t->kind == ir::Type::Kind::structure ? ir::member_offset(t, i) : i * ir::size_of(element);
                parts.push_back(load_words(p, element, at + offset, std::min(align, ir::align_of(element))));
            }
            return value_op(sop::CompositeConstruct, reg(t), parts);
        }
        case ir::Type::Kind::pointer: case ir::Type::Kind::integer: case ir::Type::Kind::double_: {
            const auto bytes = t->is_pointer() ? 8 : std::max<std::uint64_t>(1, (t->bits + 7) / 8);
            if (bytes == 8) {
                const auto lo = load_bytes(p.space, address(p, at, align), 4);
                const auto hi = load_bytes(p.space, address(p, at + 4, align), 4);
                if (t->is_pointer()) return lo;   // callers rebuild the Ptr
                if (t->kind == ir::Type::Kind::double_) fail("64-bit floating point (double) is unsupported");
                const auto pair = value_op(sop::CompositeConstruct, t_vec(t_u32(), 2), {lo, hi});
                return value_op(sop::Bitcast, t_u64(), {pair});
            }
            if (bytes > 8) fail("integer wider than 64 bits");
            return from_raw(load_bytes(p.space, address(p, at, align), static_cast<std::uint32_t>(bytes)), t);
        }
        case ir::Type::Kind::half: return from_raw(load_bytes(p.space, address(p, at, align), 2), t);
        case ir::Type::Kind::float_: return from_raw(load_bytes(p.space, address(p, at, align), 4), t);
        default: fail("cannot load " + ir::to_string(t));
        }
    }
    void store_words(Ptr p, const ir::Type* t, std::uint32_t value, std::uint64_t at, std::uint64_t align) {
        switch (t->kind) {
        case ir::Type::Kind::vector: case ir::Type::Kind::array: case ir::Type::Kind::structure: {
            const auto count = t->kind == ir::Type::Kind::structure ? t->members.size() : t->count;
            for (std::uint64_t i = 0; i < count; ++i) {
                const ir::Type* element = t->kind == ir::Type::Kind::structure ? t->members[i] : t->element;
                const auto offset = t->kind == ir::Type::Kind::structure ? ir::member_offset(t, i) : i * ir::size_of(element);
                const auto part = value_op(sop::CompositeExtract, reg(element), {value, static_cast<std::uint32_t>(i)});
                store_words(p, element, part, at + offset, std::min(align, ir::align_of(element)));
            }
            return;
        }
        case ir::Type::Kind::integer: {
            const auto bytes = std::max<std::uint32_t>(1, (t->bits + 7) / 8);
            if (bytes == 8) {
                const auto pair = value_op(sop::Bitcast, t_vec(t_u32(), 2), {value});
                store_bytes(p.space, address(p, at, align), 4, value_op(sop::CompositeExtract, t_u32(), {pair, 0}));
                store_bytes(p.space, address(p, at + 4, align), 4, value_op(sop::CompositeExtract, t_u32(), {pair, 1}));
                return;
            }
            store_bytes(p.space, address(p, at, align), bytes, to_raw(value, t));
            return;
        }
        case ir::Type::Kind::half: store_bytes(p.space, address(p, at, align), 2, to_raw(value, t)); return;
        case ir::Type::Kind::float_: store_bytes(p.space, address(p, at, align), 4, to_raw(value, t)); return;
        default: fail("cannot store " + ir::to_string(t));
        }
    }

    // ---- physical memory -------------------------------------------------------------------
    // The type a leaf has in buffer memory (8/16-bit types need the storage capabilities).
    std::uint32_t memory_type(const ir::Type* t) {
        switch (t->kind) {
        case ir::Type::Kind::integer:
            if (t->bits == 8) {
                m_.capability(spv::capability::StorageBuffer8BitAccess);
                return m_.type_int(8, false);
            }
            if (t->bits == 16) {
                m_.capability(spv::capability::StorageBuffer16BitAccess);
                return m_.type_int(16, false);
            }
            if (t->bits == 32) return t_u32();
            if (t->bits == 64) return t_u64();
            fail("integer of " + std::to_string(t->bits) + " bits in buffer memory");
        case ir::Type::Kind::half:
            m_.capability(spv::capability::StorageBuffer16BitAccess);
            return m_.type_float(16);
        case ir::Type::Kind::float_: return t_f32();
        case ir::Type::Kind::vector: return t_vec(memory_type(t->element), static_cast<std::uint32_t>(t->count));
        default: fail("cannot access " + ir::to_string(t) + " in buffer memory");
        }
    }
    std::uint32_t physical_pointer(std::uint32_t address_id, std::uint32_t type) {
        uses_physical_ = true;
        m_.capability(spv::capability::PhysicalStorageBufferAddresses);
        return value_op(sop::ConvertUToPtr, m_.type_pointer(spv::storage::PhysicalStorageBuffer, type), {address_id});
    }
    static std::uint32_t pow2_align(std::uint64_t align) {
        std::uint32_t a = 1;
        while (a * 2 <= align && a < 256) a *= 2;
        return a;
    }
    std::uint32_t load_physical(std::uint32_t address_id, const ir::Type* t, std::uint64_t align, bool is_volatile) {
        if (t->kind == ir::Type::Kind::array || t->kind == ir::Type::Kind::structure ||
            (t->is_vector() && (t->count > 4 || t->element->is_int() && t->element->bits == 1))) {
            std::vector<std::uint32_t> parts;
            const auto count = t->kind == ir::Type::Kind::structure ? t->members.size() : t->count;
            for (std::uint64_t i = 0; i < count; ++i) {
                const ir::Type* element = t->kind == ir::Type::Kind::structure ? t->members[i] : t->element;
                const auto offset = t->kind == ir::Type::Kind::structure ? ir::member_offset(t, i) : i * ir::size_of(element);
                const auto at = offset ? value_op(sop::IAdd, t_u64(), {address_id, c_u64(offset)}) : address_id;
                parts.push_back(load_physical(at, element, std::min(align, ir::align_of(element)), is_volatile));
            }
            return value_op(sop::CompositeConstruct, reg(t), parts);
        }
        if (t->is_pointer()) return load_physical(address_id, irm_.integer(64), align, is_volatile);
        if (t->is_int() && t->bits == 1) {
            const auto byte = load_physical(address_id, irm_.integer(8), align, is_volatile);
            return value_op(sop::INotEqual, t_bool(), {byte, c_u32(0)});
        }
        if (scalar_bits(t) == 64 && align < 8) {
            // A 64-bit access with less than 8-byte alignment (clang merges
            // two adjacent 32-bit loads, e.g. a uint2 member, into one i64):
            // Vulkan requires Aligned >= the scalar size, so it is two
            // 32-bit accesses.
            if (t->is_vector()) {
                std::vector<std::uint32_t> parts;
                for (std::uint64_t i = 0; i < t->count; ++i) {
                    const auto at = i ? value_op(sop::IAdd, t_u64(), {address_id, c_u64(i * 8)}) : address_id;
                    parts.push_back(load_physical(at, t->element, align, is_volatile));
                }
                return value_op(sop::CompositeConstruct, reg(t), parts);
            }
            const auto lo = load_physical(address_id, irm_.integer(32), align, is_volatile);
            const auto hi = load_physical(value_op(sop::IAdd, t_u64(), {address_id, c_u64(4)}), irm_.integer(32), align,
                                          is_volatile);
            const auto pair = value_op(sop::CompositeConstruct, t_vec(t_u32(), 2), {lo, hi});
            return value_op(sop::Bitcast, reg(t), {pair});
        }
        const auto memory = memory_type(t);
        const auto pointer = physical_pointer(address_id, memory);
        std::vector<std::uint32_t> operands = {pointer, (is_volatile ? 1U : 0U) | 2U, pow2_align(std::max<std::uint64_t>(1, align))};
        auto loaded = value_op(sop::Load, memory, operands);
        const auto bits = scalar_bits(t);
        if (t->scalar()->is_int() && bits < 32) loaded = value_op(sop::UConvert, reg(t), {loaded});
        if (t->scalar()->kind == ir::Type::Kind::half) loaded = value_op(sop::FConvert, reg(t), {loaded});
        return loaded;
    }
    void store_physical(std::uint32_t address_id, const ir::Type* t, std::uint32_t value, std::uint64_t align, bool is_volatile) {
        if (t->kind == ir::Type::Kind::array || t->kind == ir::Type::Kind::structure ||
            (t->is_vector() && (t->count > 4 || t->element->is_int() && t->element->bits == 1))) {
            const auto count = t->kind == ir::Type::Kind::structure ? t->members.size() : t->count;
            for (std::uint64_t i = 0; i < count; ++i) {
                const ir::Type* element = t->kind == ir::Type::Kind::structure ? t->members[i] : t->element;
                const auto offset = t->kind == ir::Type::Kind::structure ? ir::member_offset(t, i) : i * ir::size_of(element);
                const auto at = offset ? value_op(sop::IAdd, t_u64(), {address_id, c_u64(offset)}) : address_id;
                const auto part = value_op(sop::CompositeExtract, reg(element), {value, static_cast<std::uint32_t>(i)});
                store_physical(at, element, part, std::min(align, ir::align_of(element)), is_volatile);
            }
            return;
        }
        if (t->is_int() && t->bits == 1) {
            store_physical(address_id, irm_.integer(8), value_op(sop::Select, t_u32(), {value, c_u32(1), c_u32(0)}), align, is_volatile);
            return;
        }
        if (scalar_bits(t) == 64 && align < 8) {
            // See load_physical: two 32-bit stores.
            if (t->is_vector()) {
                for (std::uint64_t i = 0; i < t->count; ++i) {
                    const auto at = i ? value_op(sop::IAdd, t_u64(), {address_id, c_u64(i * 8)}) : address_id;
                    const auto part = value_op(sop::CompositeExtract, reg(t->element), {value, static_cast<std::uint32_t>(i)});
                    store_physical(at, t->element, part, align, is_volatile);
                }
                return;
            }
            const auto pair = value_op(sop::Bitcast, t_vec(t_u32(), 2), {value});
            store_physical(address_id, irm_.integer(32), value_op(sop::CompositeExtract, t_u32(), {pair, 0}), align,
                           is_volatile);
            store_physical(value_op(sop::IAdd, t_u64(), {address_id, c_u64(4)}), irm_.integer(32),
                           value_op(sop::CompositeExtract, t_u32(), {pair, 1}), align, is_volatile);
            return;
        }
        const auto memory = memory_type(t);
        auto converted = value;
        const auto bits = scalar_bits(t);
        if (t->scalar()->is_int() && bits < 32) converted = value_op(sop::UConvert, memory, {value});
        if (t->scalar()->kind == ir::Type::Kind::half) converted = value_op(sop::FConvert, memory, {value});
        const auto pointer = physical_pointer(address_id, memory);
        m_.emit(sop::Store, {pointer, converted, (is_volatile ? 1U : 0U) | 2U, pow2_align(std::max<std::uint64_t>(1, align))});
    }

    // ---- typed load/store dispatch ---------------------------------------------------------
    std::uint32_t load(Ptr p, const ir::Type* t, std::uint64_t align, bool is_volatile, Ptr* pointer_result = nullptr,
                       Space loaded_space = Space::none) {
        if (p.space == Space::physical) {
            const auto id = load_physical(materialise(p), t, align, is_volatile);
            if (pointer_result) {
                auto rep = id;
                if (loaded_space != Space::physical) rep = value_op(sop::UConvert, t_u32(), {id});
                *pointer_result = {loaded_space, rep, 0};
            }
            return id;
        }
        const auto id = load_words(p, t, 0, align);
        if (pointer_result) *pointer_result = {loaded_space, loaded_space == Space::physical
            ? value_op(sop::Bitcast, t_u64(), {value_op(sop::CompositeConstruct, t_vec(t_u32(), 2),
                {id, load_bytes(p.space, address(p, 4, align), 4)})}) : id, 0};
        return id;
    }
    void store(Ptr p, const ir::Type* t, std::uint32_t value, std::uint64_t align, bool is_volatile) {
        if (p.space == Space::physical) {
            store_physical(materialise(p), t, value, align, is_volatile);
            return;
        }
        store_words(p, t, value, 0, align);
    }
    void store_pointer(Ptr p, Ptr value, std::uint64_t align, bool is_volatile) {
        // Pointers are 8 bytes in memory: physical addresses as is, offsets zero-extended.
        auto rep = materialise(value);
        if (value.space != Space::physical) rep = value_op(sop::UConvert, t_u64(), {rep});
        store(p, irm_.integer(64), rep, align, is_volatile);
    }

    // ---- interface variables -----------------------------------------------------------------
    std::map<std::string, std::uint32_t> interface_vars_;
    std::set<std::uint32_t> used_attributes_, used_inputs_, used_outputs_;
    std::set<Builtin> used_builtins_;
    bool discards_{}, uses_simd_{}, writes_depth_{}, writes_stencil_{}, writes_mask_{};

    std::uint32_t interface_var(const std::string& key, std::uint32_t storage, std::uint32_t type,
                                const std::function<void(std::uint32_t)>& decorate) {
        auto& id = interface_vars_[key];
        if (!id) {
            id = m_.variable(m_.type_pointer(storage, type), storage);
            decorate(id);
            interface_.push_back(id);
        }
        return id;
    }

    // The 32-bit interface type for a register type and declared scalar.
    // Interface variable type. `is_signed` comes from the declared MSL type
    // (clang's __io_sign: aliases resolved), never from the IR, which has
    // no signedness: Vulkan matches the variable's signedness against SINT/
    // UINT vertex formats (VUID-VkGraphicsPipelineCreateInfo-Input-08733),
    // the other stage's flat varyings and integer colour attachments.
    // (__io_sign is 0 for floating-point types.)
    std::uint32_t interface_type(const ir::Type* t, bool is_signed) {
        const auto* s = t->scalar();
        std::uint32_t element;
        if (s->is_float()) element = t_f32();
        else if (s->is_int() && s->bits <= 32) element = is_signed ? t_i32() : t_u32();
        else fail("interface variable of type " + ir::to_string(t) + " is unsupported");
        return t_vec(element, t->lanes());
    }
    // The shape of each interface variable as lowered (kind 'a' attribute,
    // 'i' fragment input, 'o' vertex output, 'c' colour 2 * color + index):
    // the reflection's type when the declared spelling is not a plain
    // scalar/vector name the rewriter resolved.
    void note_interface_shape(char kind, std::uint32_t location, const ir::Type* t, bool is_signed) {
        TypeSpelling spelling;
        const auto* s = t->scalar();
        const auto lanes = t->lanes();
        spelling.kind = lanes > 1 ? Type::Kind::vector : Type::Kind::scalar;
        spelling.components = lanes;
        if (s->is_float()) spelling.scalar = s->bits == 16 ? Scalar::half : Scalar::float32;
        else if (s->bits == 1) spelling.scalar = Scalar::boolean;
        else if (s->bits == 8) spelling.scalar = is_signed ? Scalar::int8 : Scalar::uint8;
        else if (s->bits == 16) spelling.scalar = is_signed ? Scalar::int16 : Scalar::uint16;
        else spelling.scalar = is_signed ? Scalar::int32 : Scalar::uint32;
        static const char* const names[] = {"", "bool", "char", "uchar", "short", "ushort", "int", "uint", "long",
                                            "ulong", "half", "bfloat", "float"};
        spelling.text = names[static_cast<std::uint32_t>(spelling.scalar)];
        if (lanes > 1) spelling.text += std::to_string(lanes);
        interface_shapes_[{kind, location}] = std::move(spelling);
    }
    const TypeSpelling& reflected_spelling(char kind, std::uint32_t location, const TypeSpelling& declared) const {
        const bool plain = declared.kind == Type::Kind::scalar || declared.kind == Type::Kind::vector ||
                           declared.kind == Type::Kind::matrix;
        if (plain && declared.scalar != Scalar::none) return declared;
        const auto found = interface_shapes_.find({kind, location});
        return found == interface_shapes_.end() ? declared : found->second;
    }
    std::map<std::pair<char, std::uint32_t>, TypeSpelling> interface_shapes_;
    std::uint32_t from_interface(std::uint32_t id, const ir::Type* t, bool is_signed) {
        if (is_signed) {
            id = value_op(sop::Bitcast, reg(t), {id});
            return normalise(id, t);
        }
        return id;
    }
    std::uint32_t to_interface(std::uint32_t id, const ir::Type* t, std::uint32_t type, bool is_signed) {
        if (is_signed) return value_op(sop::Bitcast, type, {sign_extend(id, t)});
        return id;
    }

    const InterfaceVarying* find_varying(const std::vector<InterfaceVarying>& list, std::uint32_t location) const {
        for (const auto& v : list) if (v.location == location) return &v;
        return nullptr;
    }

    void decorate_interpolation(std::uint32_t var, Interpolation interpolation, bool integer) {
        using namespace spv::decoration;
        if (integer || interpolation == Interpolation::flat) {
            m_.decorate(var, Flat);
            return;
        }
        switch (interpolation) {
        case Interpolation::no_perspective: m_.decorate(var, NoPerspective); break;
        case Interpolation::centroid_perspective: m_.decorate(var, Centroid); break;
        case Interpolation::centroid_no_perspective: m_.decorate(var, NoPerspective); m_.decorate(var, Centroid); break;
        case Interpolation::sample_perspective:
            m_.capability(spv::capability::SampleRateShading);
            m_.decorate(var, Sample);
            break;
        case Interpolation::sample_no_perspective:
            m_.capability(spv::capability::SampleRateShading);
            m_.decorate(var, NoPerspective);
            m_.decorate(var, Sample);
            break;
        default: break;
        }
    }

    std::uint32_t builtin_spirv(Builtin b) {
        using namespace spv::builtin;
        switch (b) {
        case Builtin::vertex_id: return VertexIndex;
        case Builtin::instance_id: return InstanceIndex;
        case Builtin::base_vertex: m_.capability(spv::capability::DrawParameters); return BaseVertex;
        case Builtin::base_instance: m_.capability(spv::capability::DrawParameters); return BaseInstance;
        case Builtin::position: return entry_.stage == Stage::fragment ? FragCoord : Position;
        case Builtin::point_size: return PointSize;
        case Builtin::clip_distance: m_.capability(spv::capability::ClipDistance); return ClipDistance;
        case Builtin::front_facing: return FrontFacing;
        case Builtin::point_coord: return PointCoord;
        case Builtin::sample_id: m_.capability(spv::capability::SampleRateShading); return SampleId;
        case Builtin::sample_mask: return SampleMask;
        case Builtin::primitive_id: m_.capability(spv::capability::Geometry); return PrimitiveId;
        case Builtin::render_target_array_index:
            m_.capability(entry_.stage == Stage::fragment ? spv::capability::Geometry : spv::capability::ShaderLayer);
            return Layer;
        case Builtin::viewport_array_index:
            m_.capability(entry_.stage == Stage::fragment ? spv::capability::Geometry : spv::capability::ShaderViewportIndex);
            return ViewportIndex;
        case Builtin::thread_position_in_grid: return GlobalInvocationId;
        case Builtin::thread_position_in_threadgroup: return LocalInvocationId;
        case Builtin::threadgroup_position_in_grid: return WorkgroupId;
        case Builtin::threadgroups_per_grid: return NumWorkgroups;
        case Builtin::thread_index_in_threadgroup: return LocalInvocationIndex;
        case Builtin::thread_index_in_simdgroup: m_.capability(spv::capability::GroupNonUniform); return SubgroupLocalInvocationId;
        case Builtin::simdgroup_index_in_threadgroup: m_.capability(spv::capability::GroupNonUniform); return SubgroupId;
        case Builtin::threads_per_simdgroup: m_.capability(spv::capability::GroupNonUniform); return SubgroupSize;
        case Builtin::simdgroups_per_threadgroup: m_.capability(spv::capability::GroupNonUniform); return NumSubgroups;
        default: fail("builtin has no SPIR-V variable");
        }
    }

    std::uint32_t workgroup_size_id_{};
    std::uint32_t workgroup_size() {
        if (!workgroup_size_id_) {
            const auto x = m_.spec_constant(t_u32(), reserved_spec_id::workgroup_size_x, {1});
            const auto y = m_.spec_constant(t_u32(), reserved_spec_id::workgroup_size_y, {1});
            const auto z = m_.spec_constant(t_u32(), reserved_spec_id::workgroup_size_z, {1});
            workgroup_size_id_ = m_.spec_constant_composite(t_vec(t_u32(), 3), {x, y, z});
            m_.decorate(workgroup_size_id_, spv::decoration::BuiltIn, {spv::builtin::WorkgroupSize});
        }
        return workgroup_size_id_;
    }

    // Kernel grid push constants (compiler.hpp, kernel_push_constant_size):
    // struct { uint3 threads_per_grid; uint non_uniform; } at offset 0.
    std::uint32_t grid_var_{};
    std::uint32_t grid_member(std::uint32_t member) {
        if (!grid_var_) {
            const auto block = m_.type_struct({t_vec(t_u32(), 3), t_u32()}, true, {0, 12});
            grid_var_ = m_.variable(m_.type_pointer(spv::storage::PushConstant, block), spv::storage::PushConstant);
            m_.name(grid_var_, "alchemy_kernel_grid");
            interface_.push_back(grid_var_);
        }
        const auto type = member == 0 ? t_vec(t_u32(), 3) : t_u32();
        return value_op(sop::Load, type,
            {value_op(sop::AccessChain, m_.type_pointer(spv::storage::PushConstant, type), {grid_var_, c_u32(member)})});
    }
    // threads_per_grid: the pushed grid (dispatchThreads: / dispatchThreadgroups:),
    // or threadgroups x threads per threadgroup when the host pushed none
    // (indirect dispatch: the grid lives in GPU memory).
    std::uint32_t threads_per_grid() {
        const auto uvec3 = t_vec(t_u32(), 3);
        const auto groups = builtin_input(Builtin::threadgroups_per_grid, irm_.vector(irm_.integer(32), 3));
        const auto computed = value_op(sop::IMul, uvec3, {groups, workgroup_size()});
        const auto pushed = grid_member(0);
        const auto x = value_op(sop::CompositeExtract, t_u32(), {pushed, 0});
        const auto known = value_op(sop::INotEqual, t_bool(), {x, c_u32(0)});
        return value_op(sop::Select, uvec3, {known, pushed, computed});
    }
    // True for the threads of a non-uniform dispatchThreads: grid that Metal
    // never runs (some thread_position_in_grid component >= threads_per_grid).
    std::uint32_t kernel_outside_grid() {
        if (entry_.stage != Stage::kernel) fail("kernel grid guard outside a kernel");
        const auto uvec3 = t_vec(t_u32(), 3);
        const auto position = builtin_input(Builtin::thread_position_in_grid, irm_.vector(irm_.integer(32), 3));
        const auto outside = value_op(sop::Any, t_bool(),
            {value_op(sop::UGreaterThanEqual, t_vec(t_bool(), 3), {position, grid_member(0)})});
        const auto non_uniform = value_op(sop::INotEqual, t_bool(), {grid_member(1), c_u32(0)});
        return value_op(sop::LogicalAnd, t_bool(), {non_uniform, outside});
    }

    std::uint32_t builtin_input(Builtin b, const ir::Type* t) {
        used_builtins_.insert(b);
        if (b == Builtin::threads_per_threadgroup) return convert_builtin(workgroup_size(), t_vec(t_u32(), 3), t);
        if (b == Builtin::threads_per_grid) return convert_builtin(threads_per_grid(), t_vec(t_u32(), 3), t);
        const auto spirv = builtin_spirv(b);
        std::uint32_t type;
        bool array = false;
        switch (b) {
        case Builtin::position: type = t_vec(t_f32(), 4); break;
        case Builtin::point_coord: type = t_vec(t_f32(), 2); break;
        case Builtin::front_facing: type = t_bool(); break;
        case Builtin::thread_position_in_grid: case Builtin::thread_position_in_threadgroup:
        case Builtin::threadgroup_position_in_grid: case Builtin::threadgroups_per_grid: type = t_vec(t_u32(), 3); break;
        case Builtin::sample_mask: type = t_u32(); array = true; break;
        default: type = t_u32(); break;
        }
        const auto var_type = array ? m_.type_array(type, c_u32(1)) : type;
        const auto var = interface_var("bi.in." + std::to_string(static_cast<int>(b)), spv::storage::Input, var_type,
            [&](std::uint32_t id) {
                m_.decorate(id, spv::decoration::BuiltIn, {spirv});
                if (entry_.stage == Stage::fragment && type == t_u32()) m_.decorate(id, spv::decoration::Flat);
            });
        std::uint32_t loaded;
        if (array) loaded = value_op(sop::Load, type, {value_op(sop::AccessChain, m_.type_pointer(spv::storage::Input, type), {var, c_u32(0)})});
        else loaded = value_op(sop::Load, type, {var});
        return convert_builtin(loaded, type, t);
    }
    std::uint32_t convert_builtin(std::uint32_t id, std::uint32_t type, const ir::Type* t) {
        const auto target = reg(t);
        if (target == type) return id;
        if (t->is_int() && t->bits == 1) return value_op(sop::INotEqual, t_bool(), {id, c_u32(0)});
        if (t->is_int() && type == t_bool()) return value_op(sop::Select, target, {id, c_u32(1), c_u32(0)});
        fail("builtin read with an unexpected type " + ir::to_string(t));
    }

    std::map<std::uint32_t, std::uint32_t> clip_sizes_;   // builtin id -> array length
    std::set<std::uint32_t> atomic_textures_;

    void prescan() {
        for (auto* block : fn_->blocks) {
            for (auto* v : block->instructions) {
                if (v->op != Opcode::call || !v->callee) continue;
                const auto name = ir::demangled_identifier(v->callee->name);
                if (name == "__msl_builtin_output") {
                    const auto id = static_cast<std::uint32_t>(const_int(v->operands[0])) & 0xff;
                    if (auto element = maybe_const(v->operands[1]))
                        clip_sizes_[id] = std::max<std::uint32_t>(clip_sizes_[id], static_cast<std::uint32_t>(*element) + 1);
                    else fail("dynamic builtin output index");
                }
                if (name == "__msl_tex_atomic") {
                    if (auto handle = maybe_const(v->operands[0])) atomic_textures_.insert(static_cast<std::uint32_t>(*handle >> 16));
                }
            }
        }
    }

    void builtin_output(std::uint32_t id, std::uint32_t element, std::uint32_t value, const ir::Type* t) {
        const bool invariant = (id & builtin_invariant_flag) != 0;
        id &= 0xff;
        std::uint32_t spirv, type;
        bool array = false;
        std::uint32_t length = 1;
        if (id == builtin_frag_depth) {
            writes_depth_ = true;
            spirv = spv::builtin::FragDepth;
            type = t_f32();
        } else if (id == builtin_stencil) {
            writes_stencil_ = true;
            m_.capability(spv::capability::StencilExportEXT);
            m_.extension("SPV_EXT_shader_stencil_export");
            spirv = spv::builtin::FragStencilRefEXT;
            type = t_i32();
        } else if (id == builtin_sample_mask_out) {
            writes_mask_ = true;
            spirv = spv::builtin::SampleMask;
            type = t_u32();
            array = true;
        } else {
            const auto b = static_cast<Builtin>(id);
            used_builtins_.insert(b);
            spirv = builtin_spirv(b);
            switch (b) {
            case Builtin::position: type = t_vec(t_f32(), 4); break;
            case Builtin::point_size: type = t_f32(); break;
            case Builtin::clip_distance: type = t_f32(); array = true; length = clip_sizes_[id]; break;
            default: type = t_u32(); break;
            }
        }
        const auto var_type = array ? m_.type_array(type, c_u32(length)) : type;
        const auto var = interface_var("bi.out." + std::to_string(id), spv::storage::Output, var_type, [&](std::uint32_t v) {
            m_.decorate(v, spv::decoration::BuiltIn, {spirv});
            if (invariant) m_.decorate(v, spv::decoration::Invariant);
        });
        auto converted = value;
        if (type == t_i32()) converted = value_op(sop::Bitcast, t_i32(), {value});
        else if (t->is_int() && t->bits == 1) converted = value_op(sop::Select, type, {value, c_u32(1), c_u32(0)});
        if (array) {
            const auto pointer = value_op(sop::AccessChain, m_.type_pointer(spv::storage::Output, type), {var, c_u32(element)});
            m_.emit(sop::Store, {pointer, converted});
        } else {
            m_.emit(sop::Store, {var, converted});
        }
    }

    // ---- resources ------------------------------------------------------------------------
    std::uint32_t table_var_{};
    std::set<std::uint32_t> used_buffer_slots_;
    bool dynamic_buffer_index_{};
    std::uint32_t argument_table() {
        if (!table_var_) {
            const auto uvec4 = t_vec(t_u32(), 4);
            const auto array = m_.type_array(uvec4, c_u32(16), 16);
            const auto block = m_.type_struct({array}, true, {0});
            table_var_ = m_.variable(m_.type_pointer(spv::storage::Uniform, block), spv::storage::Uniform);
            m_.decorate(table_var_, spv::decoration::DescriptorSet, {stage_descriptor_set(entry_.stage)});
            m_.decorate(table_var_, spv::decoration::Binding, {argument_table_binding});
            m_.name(table_var_, "alchemy_argument_table");
            interface_.push_back(table_var_);
        }
        return table_var_;
    }
    std::uint32_t buffer_address(Value* index) {
        const auto table = argument_table();
        const auto uvec4 = t_vec(t_u32(), 4);
        const auto pointer_type = m_.type_pointer(spv::storage::Uniform, uvec4);
        if (const auto constant_index = maybe_const(index)) {
            const auto n = static_cast<std::uint32_t>(*constant_index);
            used_buffer_slots_.insert(n);
            const auto slot = value_op(sop::Load, uvec4, {value_op(sop::AccessChain, pointer_type, {table, c_u32(0), c_u32(n / 2)})});
            const auto pair = value_op(sop::VectorShuffle, t_vec(t_u32(), 2), {slot, slot, (n % 2) * 2, (n % 2) * 2 + 1});
            return value_op(sop::Bitcast, t_u64(), {pair});
        }
        dynamic_buffer_index_ = true;
        const auto n = id_of(index);
        const auto half = value_op(sop::ShiftRightLogical, t_u32(), {n, c_u32(1)});
        const auto slot = value_op(sop::Load, uvec4, {value_op(sop::AccessChain, pointer_type, {table, c_u32(0), half})});
        const auto lane = value_op(sop::ShiftLeftLogical, t_u32(), {value_op(sop::BitwiseAnd, t_u32(), {n, c_u32(1)}), c_u32(1)});
        const auto lo = value_op(sop::VectorExtractDynamic, t_u32(), {slot, lane});
        const auto hi = value_op(sop::VectorExtractDynamic, t_u32(), {slot, value_op(sop::IAdd, t_u32(), {lane, c_u32(1)})});
        const auto pair = value_op(sop::CompositeConstruct, t_vec(t_u32(), 2), {lo, hi});
        return value_op(sop::Bitcast, t_u64(), {pair});
    }

    const InterfaceTexture& texture_info(std::uint32_t binding) {
        for (const auto& t : entry_.textures) if (t.index == binding) return t;
        fail("texture handle for binding " + std::to_string(binding) + " has no [[texture]] parameter");
    }
    struct ImageInfo {
        std::uint32_t image_type{}, sampled_type{}, var{};
        bool storage{}, depth{}, arrayed{}, ms{}, buffer{}, cube{};
        std::uint32_t dims{};     // coordinate components without the layer
        Scalar scalar{};
    };
    std::map<std::uint32_t, ImageInfo> images_;
    std::set<std::uint32_t> used_textures_, used_samplers_;
    ImageInfo& image(std::uint32_t binding) {
        auto found = images_.find(binding);
        if (found != images_.end()) return found->second;
        const auto& info = texture_info(binding);
        ImageInfo image;
        const auto type = info.type.texture;
        std::uint32_t dim = 1;
        using T = TextureType;
        switch (type) {
        case T::texture1d: case T::texture1d_array: dim = 0; image.dims = 1; break;
        case T::texture3d: dim = 2; image.dims = 3; break;
        case T::texture_cube: case T::texture_cube_array: case T::depth_cube: case T::depth_cube_array:
            dim = 3; image.dims = 3; image.cube = true; break;
        case T::texture_buffer: dim = 5; image.dims = 1; image.buffer = true; break;
        default: dim = 1; image.dims = 2; break;
        }
        image.arrayed = type == T::texture1d_array || type == T::texture2d_array || type == T::texture2d_ms_array ||
                        type == T::texture_cube_array || type == T::depth2d_array || type == T::depth2d_ms_array ||
                        type == T::depth_cube_array;
        image.ms = type == T::texture2d_ms || type == T::texture2d_ms_array || type == T::depth2d_ms ||
                   type == T::depth2d_ms_array;
        image.depth = type == T::depth2d || type == T::depth2d_array || type == T::depth2d_ms ||
                      type == T::depth2d_ms_array || type == T::depth_cube || type == T::depth_cube_array;
        image.storage = info.type.access == Access::write || info.type.access == Access::read_write;
        image.scalar = info.type.scalar;
        const bool is_signed = info.type.scalar == Scalar::int32 || info.type.scalar == Scalar::int16 ||
                               info.type.scalar == Scalar::int8;
        const bool is_int = is_signed || info.type.scalar == Scalar::uint32 || info.type.scalar == Scalar::uint16 ||
                            info.type.scalar == Scalar::uint8;
        image.sampled_type = is_int ? (is_signed ? t_i32() : t_u32()) : t_f32();
        std::uint32_t format = 0;
        if (atomic_textures_.count(binding)) {
            if (!is_int) fail("texture atomics need an int or uint texture");
            format = is_signed ? 24 : 33;   // R32i / R32ui
            image.storage = true;
        }
        if (dim == 0) m_.capability(image.storage ? spv::capability::Image1D : spv::capability::Sampled1D);
        if (dim == 5) m_.capability(image.storage ? spv::capability::ImageBuffer : spv::capability::SampledBuffer);
        if (image.cube && image.arrayed) m_.capability(spv::capability::SampledCubeArray);
        if (image.ms && image.storage) m_.capability(spv::capability::StorageImageMultisample);
        if (image.ms && image.arrayed && image.storage) m_.capability(spv::capability::ImageMSArray);
        if (image.storage && format == 0) {
            if (info.type.access == Access::read_write) m_.capability(spv::capability::StorageImageReadWithoutFormat);
            m_.capability(spv::capability::StorageImageWriteWithoutFormat);
        }
        image.image_type = m_.type_image(image.sampled_type, dim, image.depth ? 1 : 0, image.arrayed ? 1 : 0,
                                         image.ms ? 1 : 0, image.storage ? 2 : 1, format);
        auto var_type = image.image_type;
        if (info.type.array_length) var_type = m_.type_array(image.image_type, c_u32(info.type.array_length));
        image.var = m_.variable(m_.type_pointer(spv::storage::UniformConstant, var_type), spv::storage::UniformConstant);
        m_.decorate(image.var, spv::decoration::DescriptorSet, {stage_descriptor_set(entry_.stage)});
        m_.decorate(image.var, spv::decoration::Binding, {texture_binding_base + binding});
        m_.name(image.var, info.name);
        interface_.push_back(image.var);
        return images_.emplace(binding, image).first->second;
    }
    // Resolves a texture handle operand to (binding, image id loaded from the variable).
    std::pair<ImageInfo*, std::uint32_t> texture(Value* handle) {
        std::uint32_t binding;
        std::uint32_t element = 0;
        bool dynamic = false;
        if (const auto value = maybe_const(handle)) {
            binding = static_cast<std::uint32_t>(*value >> 16);
            element = static_cast<std::uint32_t>(*value & 0xffff);
        } else if (handle->kind == Value::Kind::instruction && (handle->op == Opcode::or_ || handle->op == Opcode::add) &&
                   maybe_const(handle->operands[0])) {
            binding = static_cast<std::uint32_t>(*maybe_const(handle->operands[0]) >> 16);
            element = id_of(handle->operands[1]);
            dynamic = true;
        } else {
            fail("a texture whose binding is not known at compile time (texture selected at run time) is unsupported");
        }
        used_textures_.insert(binding);
        auto& img = image(binding);
        std::uint32_t pointer = img.var;
        if (texture_info(binding).type.array_length) {
            pointer = value_op(sop::AccessChain, m_.type_pointer(spv::storage::UniformConstant, img.image_type),
                {img.var, dynamic ? element : c_u32(element)});
        } else if (dynamic || element) {
            fail("indexing a texture that is not an array");
        }
        return {&img, value_op(sop::Load, img.image_type, {pointer})};
    }

    std::map<std::uint32_t, std::uint32_t> sampler_vars_;
    std::map<std::uint32_t, std::uint32_t> color_input_vars_;   // [[color(n)]] input -> variable
    std::set<std::uint32_t> used_argument_buffers_;
    std::map<std::uint64_t, std::uint32_t> constexpr_samplers_;   // state -> slot
    std::uint32_t sampler(Value* handle) {
        const auto value = maybe_const(handle);
        if (!value) fail("a sampler not known at compile time is unsupported");
        const auto sampler_type = m_.type_sampler();
        std::uint32_t var;
        if (!(*value >> 63) && (*value & argument_buffer_sampler_flag)) {
            // Element `element` of the k-th argument buffer of samplers.
            const auto k = static_cast<std::uint32_t>((*value >> 32) & 0xffff);
            const auto element = static_cast<std::uint32_t>(*value & 0xffffffffu);
            if (k >= entry_.argument_buffers.size() || element >= entry_.argument_buffers[k].count)
                fail("sampler handle names no argument buffer element");
            used_argument_buffers_.insert(k);
            auto& id = sampler_vars_[2000 + k];
            const auto array_type = m_.type_array(sampler_type, c_u32(entry_.argument_buffers[k].count));
            if (!id) {
                id = m_.variable(m_.type_pointer(spv::storage::UniformConstant, array_type), spv::storage::UniformConstant);
                m_.decorate(id, spv::decoration::DescriptorSet, {stage_descriptor_set(entry_.stage)});
                m_.decorate(id, spv::decoration::Binding, {argument_buffer_sampler_binding_base + k});
                m_.name(id, entry_.argument_buffers[k].name);
                interface_.push_back(id);
            }
            const auto pointer = value_op(sop::AccessChain, m_.type_pointer(spv::storage::UniformConstant, sampler_type),
                                          {id, c_u32(element)});
            return value_op(sop::Load, sampler_type, {pointer});
        }
        if (*value >> 63) {
            auto found = constexpr_samplers_.find(*value);
            if (found == constexpr_samplers_.end()) {
                const auto slot = static_cast<std::uint32_t>(constexpr_samplers_.size());
                found = constexpr_samplers_.emplace(*value, slot).first;
            }
            auto& id = sampler_vars_[1000 + found->second];
            if (!id) {
                id = m_.variable(m_.type_pointer(spv::storage::UniformConstant, sampler_type), spv::storage::UniformConstant);
                m_.decorate(id, spv::decoration::DescriptorSet, {stage_descriptor_set(entry_.stage)});
                m_.decorate(id, spv::decoration::Binding, {constexpr_sampler_binding_base + found->second});
                interface_.push_back(id);
            }
            var = id;
        } else {
            const auto binding = static_cast<std::uint32_t>(*value);
            used_samplers_.insert(binding);
            auto& id = sampler_vars_[binding];
            if (!id) {
                id = m_.variable(m_.type_pointer(spv::storage::UniformConstant, sampler_type), spv::storage::UniformConstant);
                m_.decorate(id, spv::decoration::DescriptorSet, {stage_descriptor_set(entry_.stage)});
                m_.decorate(id, spv::decoration::Binding, {sampler_binding_base + binding});
                interface_.push_back(id);
            }
            var = id;
        }
        return value_op(sop::Load, sampler_type, {var});
    }

    // Texel results: vec4 of the sampled type -> the IR's register type.
    std::uint32_t texel_result(std::uint32_t id, const ImageInfo& img, const ir::Type* t) {
        const auto target = reg(t);
        if (img.sampled_type == t_i32()) {
            id = value_op(sop::Bitcast, t_vec(t_u32(), 4), {id});
            if (t->is_vector() && scalar_bits(t) < 32) id = normalise(id, t);
        }
        if (target != t_vec(img.sampled_type == t_i32() ? t_u32() : img.sampled_type, 4)) {
            if (t->lanes() == 1) id = value_op(sop::CompositeExtract, target, {id, 0});
        }
        return id;
    }
    std::uint32_t texel_operand(std::uint32_t id, const ImageInfo& img, const ir::Type* t) {
        if (img.sampled_type == t_i32()) return value_op(sop::Bitcast, t_vec(t_i32(), 4), {sign_extend(id, t)});
        (void)t;
        return id;
    }
    // Float coordinates (+ layer as float) for sampling.
    std::uint32_t sample_coordinate(const ImageInfo& img, Value* coord, Value* layer) {
        auto c = id_of(coord);
        if (!img.arrayed) return c;
        const auto lanes = coord->type->lanes();
        const auto l = value_op(sop::ConvertUToF, t_f32(), {id_of(layer)});
        std::vector<std::uint32_t> parts;
        if (lanes == 1) parts = {c, l};
        else {
            for (std::uint32_t i = 0; i < lanes; ++i) parts.push_back(value_op(sop::CompositeExtract, t_f32(), {c, i}));
            parts.push_back(l);
        }
        return value_op(sop::CompositeConstruct, t_vec(t_f32(), lanes + 1), parts);
    }
    // Integer coordinates (+ layer) for fetch/read/write.
    std::uint32_t texel_coordinate(const ImageInfo& img, Value* coord, Value* layer) {
        auto c = id_of(coord);
        const auto lanes = coord->type->lanes();
        std::vector<std::uint32_t> parts;
        if (lanes == 1) parts = {c};
        else for (std::uint32_t i = 0; i < lanes; ++i) parts.push_back(value_op(sop::CompositeExtract, t_u32(), {c, i}));
        if (img.arrayed || img.cube) parts.push_back(id_of(layer));
        for (auto& part : parts) part = value_op(sop::Bitcast, t_i32(), {part});
        if (parts.size() == 1) return parts[0];
        return value_op(sop::CompositeConstruct, t_vec(t_i32(), static_cast<std::uint32_t>(parts.size())), parts);
    }
    // Offset image operand: none for a constant zero, ConstOffset or Offset.
    void offset_operand(Value* offset, std::uint32_t& mask, std::vector<std::uint32_t>& operands) {
        if (offset->kind == Value::Kind::constant_null) return;
        if (offset->kind == Value::Kind::constant_int && offset->int_value == 0) return;
        if (offset->is_constant()) {
            // ConstOffset must be a constant instruction: build the signed
            // vector directly (an OpBitcast would be a computed value).
            mask |= 0x8;
            const auto lanes = offset->type->lanes();
            std::vector<std::uint32_t> parts;
            for (std::uint32_t lane = 0; lane < lanes; ++lane) {
                Value* element = offset;
                if (offset->kind == Value::Kind::constant_aggregate) element = offset->operands.at(lane);
                if (element->kind != Value::Kind::constant_int && element->kind != Value::Kind::constant_null &&
                    !element->is_undef())
                    fail("texture offset is not an integer constant");
                const auto value = element->kind == Value::Kind::constant_int ? element->int_value : 0;
                parts.push_back(m_.constant_i32(static_cast<std::int32_t>(static_cast<std::uint32_t>(value))));
            }
            operands.push_back(lanes == 1 ? parts[0] : m_.constant_composite(t_vec(t_i32(), lanes), parts));
            return;
        }
        mask |= 0x10;
        m_.capability(spv::capability::ImageGatherExtended);
        operands.push_back(value_op(sop::Bitcast, t_vec(t_i32(), offset->type->lanes()), {id_of(offset)}));
    }

    // A constexpr sampler with pixel coordinates and nearest filtering
    // reads exactly one texel: Blender's texelFetch() is
    // sample(constexpr sampler(coord::pixel, filter::nearest,
    // address::clamp_to_zero), coord, level(lod), offset). Vulkan restricts
    // unnormalized-coordinate samplers (no lod, offsets or arrays), so such
    // samples become OpImageFetch with the address mode applied here.
    static bool pixel_fetch_sampler(std::uint64_t state, const ImageInfo& img) {
        if (!(state >> 63) || !((state >> 23) & 1) || (state & 3) != 0) return false;
        if (img.cube || img.ms || img.buffer || img.storage) return false;
        const auto s = (state >> 4) & 7;
        return s == 0 || s == 4 || s == 5;   // clamp_to_edge, clamp_to_zero, clamp_to_border
    }
    std::uint32_t lower_pixel_fetch(const ImageInfo& img, std::uint32_t image_id, std::uint64_t state, Value* v) {
        const auto& ops = v->operands;
        const auto mode = static_cast<std::uint32_t>(const_int(ops[4]));
        if (mode != 0 && mode != 2) fail("a pixel-coordinate sampler with bias, gradient or min_lod_clamp");
        for (const auto shift : {4, 7, 10})
            if (((state >> shift) & 7) != ((state >> 4) & 7) && img.dims > static_cast<std::uint32_t>((shift - 4) / 3))
                fail("a pixel-coordinate sampler with per-axis address modes");
        const bool zero_outside = ((state >> 4) & 7) != 0;
        if (((state >> 4) & 7) == 5 && ((state >> 16) & 3) != 0)
            fail("a pixel-coordinate sampler with an opaque border colour");
        m_.capability(spv::capability::ImageQuery);
        const auto lod = mode == 2 ? value_op(sop::ConvertFToS, t_i32(), {id_of(ops[5])}) : m_.constant_i32(0);
        // Integer texel coordinates: floor(coord) + offset, then the layer.
        const auto lanes = ops[2]->type->lanes();
        const auto fvec = lanes == 1 ? t_f32() : t_vec(t_f32(), lanes);
        const auto ivec = lanes == 1 ? t_i32() : t_vec(t_i32(), lanes);
        auto texel = value_op(sop::ConvertFToS, ivec, {ext(fvec, spv::glsl::Floor, {id_of(ops[2])})});
        const auto* offset = ops[8];
        if (!(offset->kind == Value::Kind::constant_null ||
              (offset->kind == Value::Kind::constant_int && offset->int_value == 0))) {
            if (offset->type->lanes() != lanes) fail("texture offset does not match the coordinate");
            texel = value_op(sop::IAdd, ivec, {texel, value_op(sop::Bitcast, ivec, {id_of(ops[8])})});
        }
        std::vector<std::uint32_t> parts;
        if (lanes == 1) parts = {texel};
        else for (std::uint32_t i = 0; i < lanes; ++i) parts.push_back(value_op(sop::CompositeExtract, t_i32(), {texel, i}));
        if (img.arrayed) parts.push_back(value_op(sop::Bitcast, t_i32(), {id_of(ops[3])}));
        const auto components = static_cast<std::uint32_t>(parts.size());
        const auto cvec = components == 1 ? t_i32() : t_vec(t_i32(), components);
        const auto coord = components == 1 ? parts[0] : value_op(sop::CompositeConstruct, cvec, parts);
        // Clamp into the level (and the layer into the array, as Metal does).
        const auto size = value_op(sop::Bitcast, cvec, {value_op(sop::ImageQuerySizeLod,
            components == 1 ? t_u32() : t_vec(t_u32(), components), {image_id, lod})});
        const auto zero = components == 1 ? m_.constant_i32(0) : splat(cvec, m_.constant_i32(0), components);
        const auto one = components == 1 ? m_.constant_i32(1) : splat(cvec, m_.constant_i32(1), components);
        const auto last = value_op(sop::ISub, cvec, {size, one});
        const auto clamped = ext(cvec, spv::glsl::SClamp, {coord, zero, last});
        const auto vec4 = t_vec(img.sampled_type, 4);
        auto result = value_op(sop::ImageFetch, vec4, {image_id, clamped, 0x2, lod});
        if (zero_outside) {
            // Only the texel coordinates decide (the layer is clamped).
            const auto bvec = components == 1 ? t_bool() : t_vec(t_bool(), components);
            auto inside = value_op(sop::LogicalAnd, bvec, {value_op(sop::SGreaterThanEqual, bvec, {coord, zero}),
                                                            value_op(sop::SLessThan, bvec, {coord, size})});
            if (img.arrayed) {
                std::vector<std::uint32_t> keep;
                for (std::uint32_t i = 0; i < lanes; ++i) keep.push_back(value_op(sop::CompositeExtract, t_bool(), {inside, i}));
                inside = lanes == 1 ? keep[0] : value_op(sop::CompositeConstruct, t_vec(t_bool(), lanes), keep);
            }
            if (lanes > 1) inside = value_op(sop::All, t_bool(), {inside});
            const auto zero_texel = img.sampled_type == t_f32() ? splat(vec4, m_.constant_f32(0), 4)
                                                                : splat(vec4, m_.constant(img.sampled_type, {0}), 4);
            result = value_op(sop::Select, vec4, {inside, result, zero_texel});
        }
        return texel_result(result, img, v->type);
    }

    std::uint32_t lower_texture(const std::string& name, Value* v) {
        const auto& ops = v->operands;
        if (name == "__msl_tex_query") {
            auto [img, id] = texture(ops[0]);
            const auto what = static_cast<std::uint32_t>(const_int(ops[1]));
            m_.capability(spv::capability::ImageQuery);
            if (what == 4) return value_op(sop::ImageQueryLevels, t_u32(), {id});
            if (what == 5) return value_op(sop::ImageQuerySamples, t_u32(), {id});
            const auto components = (img->cube ? 2 : img->dims) + (img->arrayed ? 1 : 0);
            const auto type = t_vec(t_u32(), components);
            std::uint32_t size;
            if (img->storage || img->ms || img->buffer) size = value_op(sop::ImageQuerySize, type, {id});
            else size = value_op(sop::ImageQuerySizeLod, type, {id, id_of(ops[2])});
            std::uint32_t component = what == 3 ? (img->cube ? 2 : img->dims) : what;
            if (component >= components) fail("texture size query for a dimension the texture does not have");
            if (components == 1) return size;
            return value_op(sop::CompositeExtract, t_u32(), {size, component});
        }
        if (name == "__msl_tex_fence") {
            m_.emit(sop::MemoryBarrier, {c_u32(scope_device), c_u32(sem_acq_rel | sem_image)});
            return 0;
        }
        auto [img, image_id] = texture(ops[0]);
        const bool fragment = entry_.stage == Stage::fragment;
        if (name == "__msl_tex_sample") {
            if (const auto state = maybe_const(ops[1]); state && pixel_fetch_sampler(*state, *img))
                return lower_pixel_fetch(*img, image_id, *state, v);
        }
        if (name == "__msl_tex_sample" || name == "__msl_tex_sample_compare" || name == "__msl_tex_gather" ||
            name == "__msl_tex_gather_compare") {
            if (img->storage) fail("sampling a texture declared with write or read_write access");
            const auto s = sampler(ops[1]);
            const auto sampled_image = value_op(sop::SampledImage, m_.type_sampled_image(img->image_type), {image_id, s});
            const auto coord = sample_coordinate(*img, ops[2], ops[3]);
            const auto vec4 = t_vec(img->sampled_type, 4);
            std::uint32_t mask = 0;
            std::vector<std::uint32_t> extra;
            if (name == "__msl_tex_sample") {
                const auto mode = static_cast<std::uint32_t>(const_int(ops[4]));
                std::uint32_t opcode = sop::ImageSampleExplicitLod;
                switch (mode) {
                case 0:
                    if (fragment) opcode = sop::ImageSampleImplicitLod;
                    else { mask |= 0x2; extra.push_back(m_.constant_f32(0)); }
                    break;
                case 1:
                    if (fragment) { opcode = sop::ImageSampleImplicitLod; mask |= 0x1; extra.push_back(id_of(ops[5])); }
                    else { mask |= 0x2; extra.push_back(m_.constant_f32(0)); }
                    break;
                case 2: mask |= 0x2; extra.push_back(id_of(ops[5])); break;
                case 3: mask |= 0x4; extra.push_back(id_of(ops[6])); extra.push_back(id_of(ops[7])); break;
                case 4:
                    m_.capability(spv::capability::MinLod);
                    if (fragment) opcode = sop::ImageSampleImplicitLod;
                    else { mask |= 0x2; extra.push_back(m_.constant_f32(0)); }
                    break;
                default: fail("unknown sample mode");
                }
                std::vector<std::uint32_t> offset;
                std::uint32_t offset_mask = 0;
                if (!img->cube) offset_operand(ops[8], offset_mask, offset);
                mask |= offset_mask;
                // Image operands appear in mask bit order: Bias, Lod, Grad, ConstOffset, Offset, MinLod.
                extra.insert(extra.end(), offset.begin(), offset.end());
                if (mode == 4) { mask |= 0x80; extra.push_back(id_of(ops[5])); }
                std::vector<std::uint32_t> operands = {sampled_image, coord};
                if (mask) {
                    operands.push_back(mask);
                    operands.insert(operands.end(), extra.begin(), extra.end());
                }
                if (opcode == sop::ImageSampleImplicitLod && !fragment) fail("implicit-lod sampling outside a fragment function");
                const auto result = value_op(opcode, vec4, operands);
                return texel_result(result, *img, v->type);
            }
            if (name == "__msl_tex_sample_compare") {
                if (!img->depth) fail("sample_compare on a non-depth texture");
                const auto dref = id_of(ops[4]);
                const auto mode = static_cast<std::uint32_t>(const_int(ops[5]));
                std::uint32_t opcode = sop::ImageSampleDrefExplicitLod;
                if (mode == 0 && fragment) opcode = sop::ImageSampleDrefImplicitLod;
                else { mask |= 0x2; extra.push_back(mode == 2 ? id_of(ops[6]) : m_.constant_f32(0)); }
                std::vector<std::uint32_t> offset;
                std::uint32_t offset_mask = 0;
                if (!img->cube) offset_operand(ops[7], offset_mask, offset);
                mask |= offset_mask;
                extra.insert(extra.end(), offset.begin(), offset.end());
                std::vector<std::uint32_t> operands = {sampled_image, coord, dref};
                if (mask) {
                    operands.push_back(mask);
                    operands.insert(operands.end(), extra.begin(), extra.end());
                }
                return value_op(opcode, t_f32(), operands);
            }
            if (name == "__msl_tex_gather") {
                std::vector<std::uint32_t> operands = {sampled_image, coord, c_u32(img->depth ? 0 : static_cast<std::uint32_t>(const_int(ops[5])))};
                if (!img->cube) offset_operand(ops[4], mask, extra);
                if (mask) {
                    operands.push_back(mask);
                    operands.insert(operands.end(), extra.begin(), extra.end());
                }
                return texel_result(value_op(sop::ImageGather, vec4, operands), *img, v->type);
            }
            // gather_compare
            std::vector<std::uint32_t> operands = {sampled_image, coord, id_of(ops[4])};
            if (!img->cube) offset_operand(ops[5], mask, extra);
            if (mask) {
                operands.push_back(mask);
                operands.insert(operands.end(), extra.begin(), extra.end());
            }
            return texel_result(value_op(sop::ImageDrefGather, vec4, operands), *img, v->type);
        }
        if (name == "__msl_tex_read") {
            if (img->cube && !img->storage) fail("read() on cube textures is unsupported");
            const auto coord = texel_coordinate(*img, ops[1], ops[2]);
            const auto vec4 = t_vec(img->sampled_type, 4);
            std::vector<std::uint32_t> operands = {image_id, coord};
            const auto lod_or_sample = id_of(ops[3]);
            std::uint32_t result;
            if (img->storage) {
                if (img->ms) { operands.push_back(0x40); operands.push_back(lod_or_sample); }
                result = value_op(sop::ImageRead, vec4, operands);
            } else {
                if (img->ms) { operands.push_back(0x40); operands.push_back(lod_or_sample); }
                else if (!img->buffer) { operands.push_back(0x2); operands.push_back(lod_or_sample); }
                result = value_op(sop::ImageFetch, vec4, operands);
            }
            return texel_result(result, *img, v->type);
        }
        if (name == "__msl_tex_write") {
            if (!img->storage) fail("write() to a texture without write or read_write access");
            const auto coord = texel_coordinate(*img, ops[1], ops[2]);
            const auto texel = texel_operand(id_of(ops[4]), *img, ops[4]->type);
            if (!(ops[3]->kind == Value::Kind::constant_int && ops[3]->int_value == 0)) fail("write() to a mip level other than 0");
            m_.emit(sop::ImageWrite, {image_id, coord, texel});
            return 0;
        }
        if (name == "__msl_tex_atomic") {
            const auto coord = texel_coordinate(*img, ops[1], ops[2]);
            const auto op = static_cast<std::uint32_t>(const_int(ops[3]));
            const bool is_signed = img->sampled_type == t_i32();
            const auto scalar = img->sampled_type;
            // The texel pointer needs the image variable itself.
            std::uint32_t pointer_var = img->var;
            if (texture_info(static_cast<std::uint32_t>(*maybe_const(ops[0]) >> 16)).type.array_length)
                fail("atomics on arrays of textures are unsupported");
            const auto pointer = value_op(sop::ImageTexelPointer, m_.type_pointer(spv::storage::Image, scalar),
                {pointer_var, coord, c_u32(0)});
            static constexpr std::uint32_t opcodes[] = {sop::AtomicIAdd, sop::AtomicISub, sop::AtomicAnd, sop::AtomicOr,
                sop::AtomicXor, 0, 0, sop::AtomicExchange};
            std::uint32_t opcode = op < 8 ? opcodes[op] : 0;
            if (op == 5) opcode = is_signed ? sop::AtomicSMin : sop::AtomicUMin;
            if (op == 6) opcode = is_signed ? sop::AtomicSMax : sop::AtomicUMax;
            if (!opcode) fail("unknown texture atomic");
            auto operand = id_of(ops[4]);
            if (is_signed) operand = value_op(sop::Bitcast, t_i32(), {operand});
            auto result = value_op(opcode, scalar, {pointer, c_u32(scope_device), c_u32(0), operand});
            if (is_signed) result = value_op(sop::Bitcast, t_u32(), {result});
            return result;
        }
        fail("unknown texture intrinsic " + name);
    }

    // ---- function constants -----------------------------------------------------------------
    std::map<std::uint32_t, std::uint32_t> spec_values_, spec_defined_;
    std::set<std::uint32_t> fc_value_used_, fc_defined_used_;
    const FunctionConstantDecl* constant_decl(std::uint32_t index) const {
        for (const auto& c : constants_) if (c.index == index) return &c;
        return nullptr;
    }
    std::uint32_t function_constant(std::uint32_t index, const ir::Type* t) {
        fc_value_used_.insert(index);
        auto& id = spec_values_[index];
        const auto* decl = constant_decl(index);
        const auto scalar = decl ? decl->type.scalar : Scalar::int32;
        if (!id) {
            switch (scalar) {
            case Scalar::boolean: id = m_.spec_constant_bool(index, false); break;
            case Scalar::float32: id = m_.spec_constant(t_f32(), index, {0}); break;
            case Scalar::half:
                m_.capability(spv::capability::Float16);
                id = value_op(sop::FConvert, t_f32(), {m_.spec_constant(m_.type_float(16), index, {0})});
                break;
            case Scalar::int16: case Scalar::uint16:
                m_.capability(spv::capability::Int16);
                id = m_.spec_constant(m_.type_int(16, false), index, {0});
                break;
            case Scalar::int8: case Scalar::uint8:
                m_.capability(spv::capability::Int8);
                id = m_.spec_constant(m_.type_int(8, false), index, {0});
                break;
            case Scalar::int64: case Scalar::uint64: id = m_.spec_constant(t_u64(), index, {0, 0}); break;
            default: id = m_.spec_constant(t_u32(), index, {0}); break;
            }
        }
        // Convert to the load's register type.
        if (scalar == Scalar::boolean) {
            if (t->is_int() && t->bits == 1) return id;
            return value_op(sop::Select, reg(t), {id, c_u32(1), c_u32(0)});
        }
        if (scalar == Scalar::int16 || scalar == Scalar::uint16 || scalar == Scalar::int8 || scalar == Scalar::uint8)
            return value_op(sop::UConvert, t_u32(), {id});
        return id;
    }
    std::uint32_t function_constant_defined(std::uint32_t index) {
        fc_defined_used_.insert(index);
        auto& id = spec_defined_[index];
        if (!id) id = m_.spec_constant_bool(reserved_spec_id::function_constant_defined_base + index, false);
        return id;
    }

    // ---- bitcasts --------------------------------------------------------------------------------
    // Register value -> its memory bytes as u32 words (little endian).
    std::vector<std::uint32_t> to_bytes(std::uint32_t id, const ir::Type* t) {
        // Returns one u32 per byte.
        std::vector<std::uint32_t> bytes;
        if (t->is_vector()) {
            for (std::uint64_t i = 0; i < t->count; ++i) {
                auto lane = value_op(sop::CompositeExtract, reg(t->element), {id, static_cast<std::uint32_t>(i)});
                auto part = to_bytes(lane, t->element);
                bytes.insert(bytes.end(), part.begin(), part.end());
            }
            return bytes;
        }
        const auto bits = scalar_bits(t);
        if (bits == 64) {
            const auto pair = value_op(sop::Bitcast, t_vec(t_u32(), 2), {id});
            for (std::uint32_t w = 0; w < 2; ++w) {
                const auto word = value_op(sop::CompositeExtract, t_u32(), {pair, w});
                for (std::uint32_t b = 0; b < 4; ++b)
                    bytes.push_back(value_op(sop::BitFieldUExtract, t_u32(), {word, c_u32(b * 8), c_u32(8)}));
            }
            return bytes;
        }
        const auto raw = to_raw(id, t);
        const auto count = std::max<std::uint32_t>(1, bits / 8);
        for (std::uint32_t b = 0; b < count; ++b)
            bytes.push_back(value_op(sop::BitFieldUExtract, t_u32(), {raw, c_u32(b * 8), c_u32(8)}));
        return bytes;
    }
    std::uint32_t from_bytes(const std::vector<std::uint32_t>& bytes, std::size_t at, const ir::Type* t) {
        if (t->is_vector()) {
            std::vector<std::uint32_t> lanes;
            const auto size = scalar_bits(t->element) / 8;
            for (std::uint64_t i = 0; i < t->count; ++i) lanes.push_back(from_bytes(bytes, at + i * size, t->element));
            return value_op(sop::CompositeConstruct, reg(t), lanes);
        }
        const auto bits = scalar_bits(t);
        auto word = [&](std::size_t first, std::uint32_t count) {
            std::uint32_t result = c_u32(0);
            for (std::uint32_t b = 0; b < count; ++b) {
                const auto shifted = b ? value_op(sop::ShiftLeftLogical, t_u32(), {bytes[first + b], c_u32(b * 8)}) : bytes[first + b];
                result = value_op(sop::BitwiseOr, t_u32(), {result, shifted});
            }
            return result;
        };
        if (bits == 64) {
            const auto pair = value_op(sop::CompositeConstruct, t_vec(t_u32(), 2), {word(at, 4), word(at + 4, 4)});
            return value_op(sop::Bitcast, t_u64(), {pair});
        }
        return from_raw(word(at, std::max<std::uint32_t>(1, bits / 8)), t);
    }
    std::uint32_t bitcast(std::uint32_t id, const ir::Type* from, const ir::Type* to) {
        const auto from_reg = reg(from), to_reg = reg(to);
        if (from_reg == to_reg) return id;
        // <N x i1> <-> iN bit masks.
        if (from->is_vector() && from->element->is_int() && from->element->bits == 1) {
            std::uint32_t mask = c_u32(0);
            for (std::uint64_t i = 0; i < from->count; ++i) {
                const auto lane = value_op(sop::CompositeExtract, t_bool(), {id, static_cast<std::uint32_t>(i)});
                mask = value_op(sop::BitwiseOr, t_u32(), {mask, value_op(sop::Select, t_u32(), {lane, c_u32(1U << i), c_u32(0)})});
            }
            if (to->is_int() && to->bits <= 32) return mask;
            fail("unsupported bitcast of a bool vector");
        }
        if (to->is_vector() && to->element->is_int() && to->element->bits == 1) {
            std::vector<std::uint32_t> lanes;
            for (std::uint64_t i = 0; i < to->count; ++i) {
                const auto bit = value_op(sop::BitwiseAnd, t_u32(), {id, c_u32(1U << i)});
                lanes.push_back(value_op(sop::INotEqual, t_bool(), {bit, c_u32(0)}));
            }
            return value_op(sop::CompositeConstruct, to_reg, lanes);
        }
        // Same register width per component and no narrow types: OpBitcast.
        const bool narrow = (scalar_bits(from) < 32) || (scalar_bits(to) < 32);
        if (!narrow && from->scalar()->kind != ir::Type::Kind::half && to->scalar()->kind != ir::Type::Kind::half)
            return value_op(sop::Bitcast, to_reg, {id});
        return from_bytes(to_bytes(id, from), 0, to);
    }

    // ---- instruction lowering ------------------------------------------------------------------
    void lower_binary(Value* v) {
        const auto* t = v->type;
        if (wide_integer(t)) {
            // Only a whole-byte right shift of a deferred wide load (moves it).
            const auto shift = maybe_const(v->operands[1]);
            if (v->op != Opcode::lshr || !shift || *shift % 8)
                fail(std::to_string(t->bits) + "-bit integer arithmetic is unsupported");
            auto wide = wide_of(v->operands[0]);
            wide.ptr.offset += static_cast<std::int64_t>(*shift / 8);
            wide.align = offset_alignment(wide.align, static_cast<std::int64_t>(*shift / 8));
            wide_[v] = wide;
            return;
        }
        const auto type = reg(t);
        auto a = id_of(v->operands[0]), b = id_of(v->operands[1]);
        const bool boolean = t->scalar()->is_int() && t->scalar()->bits == 1;
        std::uint32_t r;
        switch (v->op) {
        case Opcode::add: r = boolean ? value_op(sop::LogicalNotEqual, type, {a, b}) : normalise(value_op(sop::IAdd, type, {a, b}), t); break;
        case Opcode::sub: r = boolean ? value_op(sop::LogicalNotEqual, type, {a, b}) : normalise(value_op(sop::ISub, type, {a, b}), t); break;
        case Opcode::mul: r = boolean ? value_op(sop::LogicalAnd, type, {a, b}) : normalise(value_op(sop::IMul, type, {a, b}), t); break;
        case Opcode::udiv: r = value_op(sop::UDiv, type, {a, b}); break;
        case Opcode::urem: r = value_op(sop::UMod, type, {a, b}); break;
        case Opcode::sdiv: r = normalise(value_op(sop::SDiv, type, {sign_extend(a, t), sign_extend(b, t)}), t); break;
        case Opcode::srem: r = normalise(value_op(sop::SRem, type, {sign_extend(a, t), sign_extend(b, t)}), t); break;
        case Opcode::shl: r = normalise(value_op(sop::ShiftLeftLogical, type, {a, b}), t); break;
        case Opcode::lshr: r = value_op(sop::ShiftRightLogical, type, {a, b}); break;
        case Opcode::ashr: r = normalise(value_op(sop::ShiftRightArithmetic, type, {sign_extend(a, t), b}), t); break;
        case Opcode::and_: r = value_op(boolean ? sop::LogicalAnd : sop::BitwiseAnd, type, {a, b}); break;
        case Opcode::or_: r = value_op(boolean ? sop::LogicalOr : sop::BitwiseOr, type, {a, b}); break;
        case Opcode::xor_: r = value_op(boolean ? sop::LogicalNotEqual : sop::BitwiseXor, type, {a, b}); break;
        case Opcode::fadd: r = value_op(sop::FAdd, type, {a, b}); break;
        case Opcode::fsub: r = value_op(sop::FSub, type, {a, b}); break;
        case Opcode::fmul: r = value_op(sop::FMul, type, {a, b}); break;
        case Opcode::fdiv: r = value_op(sop::FDiv, type, {a, b}); break;
        case Opcode::frem: r = value_op(sop::FRem, type, {a, b}); break;
        default: fail("unexpected binary op");
        }
        bind(v, r);
    }

    void lower_icmp(Value* v) {
        auto* lhs = v->operands[0];
        auto* rhs = v->operands[1];
        const auto* t = lhs->type;
        const auto result_type = reg(v->type);
        std::uint32_t a, b;
        if (t->is_pointer()) {
            a = id_of(lhs);
            b = id_of(rhs);
        } else {
            a = id_of(lhs);
            b = id_of(rhs);
        }
        const bool boolean = t->scalar()->is_int() && t->scalar()->bits == 1;
        if (boolean) {
            if (v->predicate == ir::Predicate::eq) return bind(v, value_op(sop::LogicalEqual, result_type, {a, b}));
            if (v->predicate == ir::Predicate::ne) return bind(v, value_op(sop::LogicalNotEqual, result_type, {a, b}));
            // Order comparisons of i1: compare as 0/1 (signed: true = -1).
            const auto u = t->is_vector() ? t_vec(t_u32(), t->lanes()) : t_u32();
            a = value_op(sop::Select, u, {a, splat(u, c_u32(1), t->lanes()), splat(u, c_u32(0), t->lanes())});
            b = value_op(sop::Select, u, {b, splat(u, c_u32(1), t->lanes()), splat(u, c_u32(0), t->lanes())});
        }
        const bool is_signed = v->predicate == ir::Predicate::sgt || v->predicate == ir::Predicate::sge ||
                               v->predicate == ir::Predicate::slt || v->predicate == ir::Predicate::sle;
        if (is_signed && !boolean) {
            a = sign_extend(a, t);
            b = sign_extend(b, t);
        }
        std::uint32_t opcode = 0;
        switch (v->predicate) {
        case ir::Predicate::eq: opcode = sop::IEqual; break;
        case ir::Predicate::ne: opcode = sop::INotEqual; break;
        case ir::Predicate::ugt: opcode = sop::UGreaterThan; break;
        case ir::Predicate::uge: opcode = sop::UGreaterThanEqual; break;
        case ir::Predicate::ult: opcode = sop::ULessThan; break;
        case ir::Predicate::ule: opcode = sop::ULessThanEqual; break;
        case ir::Predicate::sgt: opcode = boolean ? sop::ULessThan : sop::SGreaterThan; break;
        case ir::Predicate::sge: opcode = boolean ? sop::ULessThanEqual : sop::SGreaterThanEqual; break;
        case ir::Predicate::slt: opcode = boolean ? sop::UGreaterThan : sop::SLessThan; break;
        case ir::Predicate::sle: opcode = boolean ? sop::UGreaterThanEqual : sop::SLessThanEqual; break;
        default: fail("bad icmp predicate");
        }
        bind(v, value_op(opcode, result_type, {a, b}));
    }

    void lower_fcmp(Value* v) {
        const auto result_type = reg(v->type);
        const auto a = id_of(v->operands[0]), b = id_of(v->operands[1]);
        using P = ir::Predicate;
        const auto lanes = v->type->lanes();
        switch (v->predicate) {
        case P::f_false: return bind(v, splat(result_type, m_.constant_bool(false), lanes));
        case P::f_true: return bind(v, splat(result_type, m_.constant_bool(true), lanes));
        case P::f_ord: case P::f_uno: {
            const auto na = value_op(sop::IsNan, result_type, {a}), nb = value_op(sop::IsNan, result_type, {b});
            const auto any_nan = value_op(sop::LogicalOr, result_type, {na, nb});
            return bind(v, v->predicate == P::f_uno ? any_nan : value_op(sop::LogicalNot, result_type, {any_nan}));
        }
        default: break;
        }
        static const std::map<P, std::uint32_t> table = {{P::f_oeq, sop::FOrdEqual}, {P::f_ogt, sop::FOrdGreaterThan},
            {P::f_oge, sop::FOrdGreaterThanEqual}, {P::f_olt, sop::FOrdLessThan}, {P::f_ole, sop::FOrdLessThanEqual},
            {P::f_one, sop::FOrdNotEqual}, {P::f_ueq, sop::FUnordEqual}, {P::f_ugt, sop::FUnordGreaterThan},
            {P::f_uge, sop::FUnordGreaterThanEqual}, {P::f_ult, sop::FUnordLessThan},
            {P::f_ule, sop::FUnordLessThanEqual}, {P::f_une, sop::FUnordNotEqual}};
        bind(v, value_op(table.at(v->predicate), result_type, {a, b}));
    }

    void lower_cast(Value* v) {
        auto* source = v->operands[0];
        const auto* from = source->type;
        const auto* to = v->type;
        if (v->op == Opcode::addrspacecast || (v->op == Opcode::bitcast && to->is_pointer())) {
            Ptr p = ptr_of(source);
            const auto target = spaces_.count(v) ? spaces_.at(v) : p.space;
            if (target != p.space && p.space != Space::none) fail("address space cast between unrelated memories");
            return bind_ptr(v, p);
        }
        if (v->op == Opcode::inttoptr) {
            auto id = id_of(source);
            if (scalar_bits(from) < 64) id = value_op(sop::UConvert, t_u64(), {id});
            const auto space = spaces_.count(v) ? spaces_.at(v) : Space::physical;
            if (space != Space::physical) fail("inttoptr to a thread or threadgroup pointer");
            return bind_ptr(v, {Space::physical, id, 0});
        }
        if (v->op == Opcode::ptrtoint) {
            const auto p = ptr_of(source);
            auto id = materialise(p);
            const auto from_bits = p.space == Space::physical ? 64U : 32U;
            const auto to_bits = scalar_bits(to);
            if (to_bits == 64 && from_bits == 32) id = value_op(sop::UConvert, t_u64(), {id});
            else if (to_bits <= 32 && from_bits == 64) id = normalise(value_op(sop::UConvert, t_u32(), {id}), to);
            else if (to_bits < 32) id = normalise(id, to);
            return bind(v, id);
        }
        if (wide_integer(from)) {
            // trunc / bitcast of a deferred wide load: load the low bytes as `to`.
            if (v->op != Opcode::trunc && v->op != Opcode::bitcast)
                fail(std::to_string(from->bits) + "-bit integer conversion is unsupported");
            if (to->is_int() && to->bits == 1) {
                const auto byte = load_wide_part(source, irm_.integer(8), 0);
                return bind(v, value_op(sop::INotEqual, t_bool(), {value_op(sop::BitwiseAnd, t_u32(), {byte, c_u32(1)}), c_u32(0)}));
            }
            return bind(v, load_wide_part(source, to, 0));
        }
        if (wide_integer(to)) fail(std::to_string(to->bits) + "-bit integer values are unsupported");
        const auto id = id_of(source);
        const auto type = reg(to);
        const auto from_bits = scalar_bits(from), to_bits = scalar_bits(to);
        const auto lanes = to->lanes();
        switch (v->op) {
        case Opcode::trunc: {
            if (to_bits == 1) {
                const auto u = reg(from);
                const auto bit = value_op(sop::BitwiseAnd, u, {id, from_bits == 64 ? splat(u, c_u64(1), lanes) : splat(u, c_u32(1), lanes)});
                return bind(v, value_op(sop::INotEqual, type, {bit, from_bits == 64 ? splat(u, c_u64(0), lanes) : splat(u, c_u32(0), lanes)}));
            }
            auto r = id;
            if (from_bits == 64) r = value_op(sop::UConvert, type, {id});
            return bind(v, normalise(r, to));
        }
        case Opcode::zext: {
            if (from_bits == 1) {
                const auto one = to_bits == 64 ? c_u64(1) : c_u32(1), zero = to_bits == 64 ? c_u64(0) : c_u32(0);
                return bind(v, value_op(sop::Select, type, {id, splat(type, one, lanes), splat(type, zero, lanes)}));
            }
            if (to_bits == 64 && from_bits < 64) return bind(v, value_op(sop::UConvert, type, {id}));
            return bind(v, id);
        }
        case Opcode::sext: {
            if (from_bits == 1) {
                const auto ones = to_bits == 64 ? c_u64(~0ULL) : c_u32(to_bits >= 32 ? ~0U : (1U << to_bits) - 1);
                const auto zero = to_bits == 64 ? c_u64(0) : c_u32(0);
                return bind(v, value_op(sop::Select, type, {id, splat(type, ones, lanes), splat(type, zero, lanes)}));
            }
            auto r = sign_extend(id, from);
            if (to_bits == 64) r = value_op(sop::SConvert, type, {r});
            return bind(v, normalise(r, to));
        }
        case Opcode::fptrunc:
            if (from->scalar()->kind == ir::Type::Kind::double_) fail("64-bit floating point (double) is unsupported");
            if (to->scalar()->kind == ir::Type::Kind::half) return bind(v, value_op(sop::QuantizeToF16, type, {id}));
            return bind(v, id);
        case Opcode::fpext:
            if (to->scalar()->kind == ir::Type::Kind::double_) fail("64-bit floating point (double) is unsupported");
            return bind(v, id);
        case Opcode::fptoui: {
            if (to_bits == 1) return bind(v, value_op(sop::FOrdNotEqual, type, {id, splat(reg(from), m_.constant_f32(0), lanes)}));
            const auto convert_type = to_bits == 64 ? type : t_vec(t_u32(), lanes);
            return bind(v, normalise(value_op(sop::ConvertFToU, convert_type, {id}), to));
        }
        case Opcode::fptosi: {
            const auto signed_type = to_bits == 64 ? (lanes == 1 ? m_.type_int(64, true) : t_vec(m_.type_int(64, true), lanes))
                                                   : t_vec(t_i32(), lanes);
            auto r = value_op(sop::ConvertFToS, signed_type, {id});
            r = value_op(sop::Bitcast, type, {r});
            return bind(v, normalise(r, to));
        }
        case Opcode::uitofp: {
            auto source_id = id;
            if (from_bits == 1) source_id = value_op(sop::Select, t_vec(t_u32(), lanes), {id, splat(t_vec(t_u32(), lanes), c_u32(1), lanes),
                splat(t_vec(t_u32(), lanes), c_u32(0), lanes)});
            return bind(v, value_op(sop::ConvertUToF, type, {source_id}));
        }
        case Opcode::sitofp: {
            auto source_id = sign_extend(id, from);
            if (from_bits == 1) source_id = value_op(sop::Select, t_vec(t_u32(), lanes), {id, splat(t_vec(t_u32(), lanes), c_u32(~0U), lanes),
                splat(t_vec(t_u32(), lanes), c_u32(0), lanes)});
            const auto signed_type = from_bits == 64 ? (lanes == 1 ? m_.type_int(64, true) : t_vec(m_.type_int(64, true), lanes))
                                                     : t_vec(t_i32(), lanes);
            source_id = value_op(sop::Bitcast, signed_type, {source_id});
            return bind(v, value_op(sop::ConvertSToF, type, {source_id}));
        }
        case Opcode::bitcast: return bind(v, bitcast(id, from, to));
        default: fail("unsupported cast");
        }
    }

    void lower_gep(Value* v) { bind_ptr(v, gep(v)); }

    // ---- wide integers ----------------------------------------------------------------------
    // SROA turns aggregate copies into integer loads wider than 64 bits
    // (`load i128` of a 16-byte struct, then lshr + trunc per field). Such a
    // load is not emitted: it stays a pointer, `lshr` by a whole number of
    // bytes moves it, and `trunc`/`bitcast` (or a store of the whole value)
    // loads the bytes they need. Any other use is an explicit error.
    struct WideLoad {
        Ptr ptr;
        std::uint64_t align{};
        bool is_volatile{};
    };
    std::unordered_map<Value*, WideLoad> wide_;
    static bool wide_integer(const ir::Type* t) { return t->kind == ir::Type::Kind::integer && t->bits > 64; }
    const WideLoad& wide_of(Value* v) {
        const auto found = wide_.find(v);
        if (found == wide_.end() || cfg_.demoted.count(demotion_key(v)))
            fail(std::to_string(v->type->bits) + "-bit integer values are unsupported (only loads split by "
                 "shifts and truncations)");
        return found->second;
    }
    static std::uint64_t offset_alignment(std::uint64_t align, std::int64_t offset) {
        std::uint64_t a = align ? align : 1;
        while (a > 1 && offset % static_cast<std::int64_t>(a) != 0) a /= 2;
        return a;
    }
    // Loads `t` from byte `offset` of a deferred wide load.
    std::uint32_t load_wide_part(Value* source, const ir::Type* t, std::int64_t offset) {
        const auto& wide = wide_of(source);
        Ptr p = wide.ptr;
        p.offset += offset;
        return load(p, t, offset_alignment(wide.align, offset), wide.is_volatile);
    }

    void lower_load(Value* v) {
        auto* pointer = v->operands[0];
        const auto p = ptr_of(pointer);
        if (wide_integer(v->type)) {
            if (v->atomic || p.space == Space::constant) fail(std::to_string(v->type->bits) + "-bit atomic or constant load");
            if (v->type->bits % 8) fail(std::to_string(v->type->bits) + "-bit integer load");
            wide_[v] = {p, v->alignment, v->is_volatile};
            return;
        }
        if (p.space == Space::constant) {
            return bind(v, function_constant(p.id, v->type));
        }
        if (v->type->is_pointer()) {
            Ptr result;
            load(p, irm_.integer(64), v->alignment, v->is_volatile, &result, spaces_.at(v));
            if (result.space != Space::physical && p.space == Space::physical) {}   // UConvert done in load()
            return bind_ptr(v, result);
        }
        if (v->atomic) return bind(v, atomic_load(p, v->type));
        bind(v, load(p, v->type, v->alignment, v->is_volatile));
    }
    void lower_store(Value* v) {
        auto* value = v->operands[0];
        const auto p = ptr_of(v->operands[1]);
        if (p.space == Space::constant) fail("store to a function constant");
        if (value->type->is_pointer()) return store_pointer(p, ptr_of(value), v->alignment, v->is_volatile);
        if (wide_integer(value->type)) {
            // A copy: move the bytes in 32-bit words (or one 64-bit word).
            const auto bits = value->type->bits;
            if (bits % 32) fail(std::to_string(bits) + "-bit integer store");
            for (std::uint32_t word = 0; word < bits / 32; ++word) {
                const auto offset = static_cast<std::int64_t>(word) * 4;
                const auto part = load_wide_part(value, irm_.integer(32), offset);
                Ptr q = p;
                q.offset += offset;
                store(q, irm_.integer(32), part, offset_alignment(v->alignment, offset), v->is_volatile);
            }
            return;
        }
        if (v->atomic) return atomic_store(p, value->type, id_of(value));
        store(p, value->type, id_of(value), v->alignment, v->is_volatile);
    }

    // Atomic access: a pointer to the 32/64-bit scalar plus its scope.
    std::pair<std::uint32_t, std::uint32_t> atomic_pointer(Ptr p, const ir::Type* t) {
        const auto bits = scalar_bits(t);
        if (bits != 32 && bits != 64) fail("atomics on " + std::to_string(bits) + "-bit values are unsupported");
        if (bits == 64) m_.capability(spv::capability::Int64Atomics);
        const auto type = t->is_float() ? t_f32() : bits == 64 ? t_u64() : t_u32();
        if (p.space == Space::physical) return {physical_pointer(materialise(p), type), scope_device};
        if (p.space == Space::workgroup) {
            if (bits != 32 || t->is_float()) fail("threadgroup atomics are implemented for 32-bit integers only");
            const auto a = address(p, 0, 4);
            return {word_pointer(Space::workgroup, a.word), scope_workgroup};
        }
        return {0, 0};   // thread memory: plain read-modify-write
    }
    std::uint32_t atomic_load(Ptr p, const ir::Type* t) {
        auto [pointer, scope] = atomic_pointer(p, t);
        if (!pointer) return load(p, t, 4, false);
        return value_op(sop::AtomicLoad, reg(t), {pointer, c_u32(scope), c_u32(0)});
    }
    void atomic_store(Ptr p, const ir::Type* t, std::uint32_t value) {
        auto [pointer, scope] = atomic_pointer(p, t);
        if (!pointer) return store(p, t, value, 4, false);
        m_.emit(sop::AtomicStore, {pointer, c_u32(scope), c_u32(0), value});
    }
    void lower_atomicrmw(Value* v) {
        const auto p = ptr_of(v->operands[0]);
        const auto* t = v->type;
        auto operand = id_of(v->operands[1]);
        auto [pointer, scope] = atomic_pointer(p, t);
        const auto type = reg(t);
        using A = ir::AtomicOp;
        if (!pointer) {
            // Thread memory: not shared, emulate.
            const auto old = load(p, t, 4, false);
            std::uint32_t next = 0;
            switch (v->atomic_op) {
            case A::xchg: next = operand; break;
            case A::add: next = value_op(sop::IAdd, type, {old, operand}); break;
            case A::sub: next = value_op(sop::ISub, type, {old, operand}); break;
            case A::and_: next = value_op(sop::BitwiseAnd, type, {old, operand}); break;
            case A::or_: next = value_op(sop::BitwiseOr, type, {old, operand}); break;
            case A::xor_: next = value_op(sop::BitwiseXor, type, {old, operand}); break;
            case A::umax: next = ext(type, spv::glsl::UMax, {old, operand}); break;
            case A::umin: next = ext(type, spv::glsl::UMin, {old, operand}); break;
            case A::max: next = ext(type, spv::glsl::SMax, {old, operand}); break;
            case A::min: next = ext(type, spv::glsl::SMin, {old, operand}); break;
            case A::fadd: next = value_op(sop::FAdd, type, {old, operand}); break;
            default: fail("unsupported atomic operation");
            }
            store(p, t, next, 4, false);
            return bind(v, old);
        }
        std::uint32_t opcode = 0;
        switch (v->atomic_op) {
        case A::xchg: opcode = sop::AtomicExchange; break;
        case A::add: opcode = sop::AtomicIAdd; break;
        case A::sub: opcode = sop::AtomicISub; break;
        case A::and_: opcode = sop::AtomicAnd; break;
        case A::or_: opcode = sop::AtomicOr; break;
        case A::xor_: opcode = sop::AtomicXor; break;
        case A::max: opcode = sop::AtomicSMax; break;
        case A::min: opcode = sop::AtomicSMin; break;
        case A::umax: opcode = sop::AtomicUMax; break;
        case A::umin: opcode = sop::AtomicUMin; break;
        case A::fadd:
            m_.capability(spv::capability::AtomicFloat32AddEXT);
            m_.extension("SPV_EXT_shader_atomic_float_add");
            opcode = sop::AtomicFAddEXT;
            break;
        default: fail("unsupported atomic operation");
        }
        bind(v, value_op(opcode, type, {pointer, c_u32(scope), c_u32(0), operand}));
    }
    void lower_cmpxchg(Value* v) {
        const auto p = ptr_of(v->operands[0]);
        const auto* t = v->operands[1]->type;
        const auto expected = id_of(v->operands[1]), desired = id_of(v->operands[2]);
        auto [pointer, scope] = atomic_pointer(p, t);
        const auto type = reg(t);
        std::uint32_t old;
        if (!pointer) {
            old = load(p, t, 4, false);
            const auto equal = value_op(sop::IEqual, t_bool(), {old, expected});
            store(p, t, value_op(sop::Select, type, {equal, desired, old}), 4, false);
        } else {
            old = value_op(sop::AtomicCompareExchange, type, {pointer, c_u32(scope), c_u32(0), c_u32(0), desired, expected});
        }
        const auto success = value_op(sop::IEqual, t_bool(), {old, expected});
        bind(v, value_op(sop::CompositeConstruct, reg(v->type), {old, success}));
    }

    void lower_select(Value* v) {
        if (v->type->is_pointer()) {
            const auto a = ptr_of(v->operands[1]), b = ptr_of(v->operands[2]);
            const auto space = spaces_.at(v);
            const auto type = rep_type(space);
            const auto id = value_op(sop::Select, type, {id_of(v->operands[0]), materialise(a), materialise(b)});
            return bind_ptr(v, {space, id, 0});
        }
        bind(v, value_op(sop::Select, reg(v->type), {id_of(v->operands[0]), id_of(v->operands[1]), id_of(v->operands[2])}));
    }

    void lower_vector(Value* v) {
        switch (v->op) {
        case Opcode::extractelement: {
            const auto* vt = v->operands[0]->type;
            const auto vec = id_of(v->operands[0]);
            if (vt->count == 1) return bind(v, vec);
            if (const auto index = maybe_const(v->operands[1]))
                return bind(v, value_op(sop::CompositeExtract, reg(v->type), {vec, static_cast<std::uint32_t>(*index)}));
            return bind(v, value_op(sop::VectorExtractDynamic, reg(v->type), {vec, id_of(v->operands[1])}));
        }
        case Opcode::insertelement: {
            const auto* vt = v->type;
            const auto vec = id_of(v->operands[0]), element = id_of(v->operands[1]);
            if (vt->count == 1) return bind(v, element);
            if (const auto index = maybe_const(v->operands[2]))
                return bind(v, value_op(sop::CompositeInsert, reg(vt), {element, vec, static_cast<std::uint32_t>(*index)}));
            return bind(v, value_op(sop::VectorInsertDynamic, reg(vt), {vec, element, id_of(v->operands[2])}));
        }
        case Opcode::shufflevector: {
            const auto* a = v->operands[0]->type;
            const auto* b = v->operands[1]->type;
            const auto ida = id_of(v->operands[0]), idb = id_of(v->operands[1]);
            const auto lanes_a = static_cast<std::int64_t>(a->count);
            if (v->type->count == 1) {
                const auto index = v->indices[0];
                if (index < 0) return bind(v, m_.undef(reg(v->type)));
                const auto* source = index < lanes_a ? a : b;
                const auto id = index < lanes_a ? ida : idb;
                const auto lane = static_cast<std::uint32_t>(index < lanes_a ? index : index - lanes_a);
                return bind(v, source->count == 1 ? id : value_op(sop::CompositeExtract, reg(v->type), {id, lane}));
            }
            if (a->count == 1 || b->count == 1) {
                // Scalars standing in for 1-lane vectors: build the result lane by lane.
                std::vector<std::uint32_t> lanes;
                for (const auto index : v->indices) {
                    if (index < 0) { lanes.push_back(m_.undef(reg(v->type->element))); continue; }
                    const auto* source = index < lanes_a ? a : b;
                    const auto id = index < lanes_a ? ida : idb;
                    const auto lane = static_cast<std::uint32_t>(index < lanes_a ? index : index - lanes_a);
                    lanes.push_back(source->count == 1 ? id : value_op(sop::CompositeExtract, reg(v->type->element), {id, lane}));
                }
                return bind(v, value_op(sop::CompositeConstruct, reg(v->type), lanes));
            }
            std::vector<std::uint32_t> operands = {ida, idb};
            for (const auto index : v->indices) operands.push_back(index < 0 ? 0xffffffffU : static_cast<std::uint32_t>(index));
            return bind(v, value_op(sop::VectorShuffle, reg(v->type), operands));
        }
        case Opcode::extractvalue: {
            std::vector<std::uint32_t> operands = {id_of(v->operands[0])};
            for (const auto index : v->indices) operands.push_back(static_cast<std::uint32_t>(index));
            return bind(v, value_op(sop::CompositeExtract, reg(v->type), operands));
        }
        case Opcode::insertvalue: {
            std::vector<std::uint32_t> operands = {id_of(v->operands[1]), id_of(v->operands[0])};
            for (const auto index : v->indices) operands.push_back(static_cast<std::uint32_t>(index));
            return bind(v, value_op(sop::CompositeInsert, reg(v->type), operands));
        }
        default: fail("unexpected vector op");
        }
    }

    // ---- calls ----------------------------------------------------------------------------------
    std::uint32_t group_scope() { return c_u32(scope_subgroup); }

    void lower_call(Value* v) {
        const auto& full = v->callee->name;
        const auto name = ir::demangled_identifier(full);
        const auto& ops = v->operands;
        const auto type = v->type->kind == ir::Type::Kind::void_ || v->type->is_pointer() ? 0U : reg(v->type);
        if (full.rfind("llvm.", 0) == 0) return lower_llvm_intrinsic(v, full);
        if (name == "__msl_glsl1" || name == "__msl_glsl2" || name == "__msl_glsl3") {
            const auto op = static_cast<std::uint32_t>(const_int(ops[0]));
            std::vector<std::uint32_t> args;
            const bool is_signed = op == spv::glsl::SAbs || op == spv::glsl::SSign || op == spv::glsl::SMin ||
                                   op == spv::glsl::SMax || op == spv::glsl::SClamp || op == spv::glsl::FindSMsb;
            for (std::size_t i = 1; i < ops.size(); ++i) {
                auto a = id_of(ops[i]);
                if (is_signed) a = sign_extend(a, ops[i]->type);
                args.push_back(a);
            }
            auto r = ext(type, op, args);
            if (is_signed || op == spv::glsl::FindUMsb || op == spv::glsl::FindILsb) r = normalise(r, v->type);
            return bind(v, r);
        }
        if (name == "__msl_spv1" || name == "__msl_spv2" || name == "__msl_spv3" || name == "__msl_spv4") {
            const auto op = static_cast<std::uint32_t>(const_int(ops[0]));
            std::vector<std::uint32_t> args;
            for (std::size_t i = 1; i < ops.size(); ++i) args.push_back(id_of(ops[i]));
            const auto bits = scalar_bits(v->type);
            switch (op) {
            case sop::FRem: case sop::Dot: case sop::BitCount: return bind(v, value_op(op, type, args));
            case sop::BitReverse: {
                auto r = value_op(op, type, args);
                if (bits < 32) r = value_op(sop::ShiftRightLogical, type, {r, splat_u32(v->type, 32 - bits)});
                return bind(v, r);
            }
            case sop::BitFieldUExtract: return bind(v, value_op(op, type, args));
            case sop::BitFieldSExtract:
                args[0] = sign_extend(args[0], ops[1]->type);
                return bind(v, normalise(value_op(op, type, args), v->type));
            case sop::BitFieldInsert: return bind(v, normalise(value_op(op, type, args), v->type));
            default: fail("unsupported SPIR-V opcode intrinsic " + std::to_string(op));
            }
        }
        if (name == "__msl_spvb1") {
            const auto op = static_cast<std::uint32_t>(const_int(ops[0]));
            const auto lanes = v->type->lanes();
            const auto bool_type = t_vec(t_bool(), lanes);
            const auto r = value_op(op, bool_type, {id_of(ops[1])});
            if (v->type->scalar()->bits == 1) return bind(v, r);
            return bind(v, value_op(sop::Select, type, {r, splat(type, c_u32(1), lanes), splat(type, c_u32(0), lanes)}));
        }
        if (name == "__msl_deriv") {
            if (entry_.stage != Stage::fragment) fail("derivatives (dfdx/dfdy/fwidth) outside a fragment function");
            const auto op = static_cast<std::uint32_t>(const_int(ops[0]));
            if (op >= sop::DPdxFine) m_.capability(spv::capability::DerivativeControl);
            return bind(v, value_op(op, type, {id_of(ops[1])}));
        }
        if (name.rfind("__msl_tex_", 0) == 0) {
            const auto r = lower_texture(name, v);
            if (type) bind(v, r);
            return;
        }
        if (name == "__msl_barrier") {
            const auto scope = static_cast<std::uint32_t>(const_int(ops[0]));
            const auto flags = static_cast<std::uint32_t>(const_int(ops[1]));
            std::uint32_t semantics = 0;
            if (flags & 1) semantics |= sem_uniform;
            if (flags & 2) semantics |= sem_workgroup;
            if (flags & 4) semantics |= sem_image;
            if (semantics) semantics |= sem_acq_rel;
            if (scope >= 0x100) {
                const auto memory_scope = scope == 0x102 ? scope_workgroup : scope == 0x103 ? scope_subgroup : scope_device;
                m_.emit(sop::MemoryBarrier, {c_u32(memory_scope), c_u32(semantics | sem_acq_rel)});
                return;
            }
            if (entry_.stage != Stage::kernel && scope == scope_workgroup)
                fail("threadgroup_barrier outside a kernel function");
            if (scope == scope_subgroup) uses_simd_ = true;
            m_.emit(sop::ControlBarrier, {c_u32(scope), c_u32(scope), c_u32(semantics)});
            return;
        }
        if (name == "__msl_group0" || name == "__msl_group1" || name == "__msl_group2") {
            uses_simd_ = true;
            m_.capability(spv::capability::GroupNonUniform);
            const auto op = static_cast<std::uint32_t>(const_int(ops[0]));
            return bind(v, lower_group(op, v, type));
        }
        if (name == "__msl_discard") {
            if (entry_.stage != Stage::fragment) fail("discard_fragment outside a fragment function");
            discards_ = true;
            const bool spirv16 = options_.vulkan_major > 1 || options_.vulkan_minor >= 3;
            m_.emit(spirv16 ? sop::TerminateInvocation : sop::Kill, {});
            block_terminated_ = true;
            return;
        }
        if (name == "__msl_buffer") return bind_ptr(v, {Space::physical, buffer_address(ops[0]), 0});
        if (name == "__msl_threadgroup_storage") {
            const auto id = static_cast<std::uint32_t>(const_int(ops[0]));
            return bind_ptr(v, {Space::workgroup, 0, static_cast<std::int64_t>(threadgroup_storage_offsets_.at(id))});
        }
        if (name == "__msl_threadgroup_param") {
            const auto index = static_cast<std::uint32_t>(const_int(ops[0]));
            word_array(Space::workgroup);
            const auto found = threadgroup_param_offsets_.find(index);
            if (found == threadgroup_param_offsets_.end()) fail("unknown [[threadgroup]] parameter");
            return bind_ptr(v, {Space::workgroup, found->second, 0});
        }
        if (name == "__msl_fc_defined") return bind(v, function_constant_defined(static_cast<std::uint32_t>(const_int(ops[0]))));
        if (name == "__msl_kernel_outside_grid") {
            const auto outside = kernel_outside_grid();
            if (v->type->is_int() && v->type->bits == 1) return bind(v, outside);
            return bind(v, value_op(sop::Select, reg(v->type), {outside, c_u32(1), c_u32(0)}));
        }
        if (name == "__msl_stage_input") {
            const auto location = static_cast<std::uint32_t>(const_int(ops[0]));
            const auto interpolation = static_cast<Interpolation>(const_int(ops[1]));
            const auto& list = entry_.stage == Stage::vertex ? entry_.attributes : entry_.inputs;
            const auto* varying = find_varying(list, location);
            const bool is_signed = const_int(ops[2]) != 0;
            const auto var_type = interface_type(v->type, is_signed);
            note_interface_shape(entry_.stage == Stage::vertex ? 'a' : 'i', location, v->type, is_signed);
            const auto var = interface_var("in." + std::to_string(location), spv::storage::Input, var_type, [&](std::uint32_t id) {
                m_.decorate(id, spv::decoration::Location, {location});
                if (entry_.stage == Stage::fragment) decorate_interpolation(id, interpolation, !v->type->scalar()->is_float());
                if (varying) m_.name(id, varying->name);
            });
            (entry_.stage == Stage::vertex ? used_attributes_ : used_inputs_).insert(location);
            return bind(v, from_interface(value_op(sop::Load, var_type, {var}), v->type, is_signed));
        }
        if (name == "__msl_stage_output") {
            const auto location = static_cast<std::uint32_t>(const_int(ops[0]));
            const auto* varying = find_varying(entry_.outputs, location);
            const bool is_signed = const_int(ops[1]) != 0;
            const auto var_type = interface_type(ops[2]->type, is_signed);
            note_interface_shape('o', location, ops[2]->type, is_signed);
            const auto var = interface_var("out." + std::to_string(location), spv::storage::Output, var_type, [&](std::uint32_t id) {
                m_.decorate(id, spv::decoration::Location, {location});
                if (varying) m_.name(id, varying->name);
            });
            used_outputs_.insert(location);
            m_.emit(sop::Store, {var, to_interface(id_of(ops[2]), ops[2]->type, var_type, is_signed)});
            return;
        }
        if (name == "__msl_color_output") {
            const auto color = static_cast<std::uint32_t>(const_int(ops[0]));
            const auto index = static_cast<std::uint32_t>(const_int(ops[1]));
            const bool is_signed = const_int(ops[2]) != 0;
            const auto var_type = interface_type(ops[3]->type, is_signed);
            note_interface_shape('c', color * 2 + (index ? 1 : 0), ops[3]->type, is_signed);
            const auto var = interface_var("color." + std::to_string(color) + "." + std::to_string(index),
                spv::storage::Output, var_type, [&](std::uint32_t id) {
                    m_.decorate(id, spv::decoration::Location, {color});
                    if (index) m_.decorate(id, spv::decoration::Index, {index});
                });
            m_.emit(sop::Store, {var, to_interface(id_of(ops[3]), ops[3]->type, var_type, is_signed)});
            return;
        }
        if (name == "__msl_color_input") {
            // Framebuffer fetch: OpImageRead of a SubpassData input attachment.
            if (entry_.stage != Stage::fragment) fail("[[color(n)]] inputs outside a fragment function");
            const auto color = static_cast<std::uint32_t>(const_int(ops[0]));
            const bool is_signed = const_int(ops[1]) != 0;
            const auto* s = v->type->scalar();
            if (!(s->is_float() || (s->is_int() && s->bits == 32)))
                fail("[[color(n)]] input of type " + ir::to_string(v->type) + " is unsupported");
            const auto sampled = s->is_float() ? t_f32() : is_signed ? t_i32() : t_u32();
            const auto image_type = m_.type_image(sampled, 6, 0, 0, 0, 2, 0);   // SubpassData
            auto& var = color_input_vars_[color];
            if (!var) {
                m_.capability(spv::capability::InputAttachment);
                var = m_.variable(m_.type_pointer(spv::storage::UniformConstant, image_type), spv::storage::UniformConstant);
                m_.decorate(var, spv::decoration::DescriptorSet, {stage_descriptor_set(entry_.stage)});
                m_.decorate(var, spv::decoration::Binding, {color_input_binding_base + color});
                m_.decorate(var, spv::decoration::InputAttachmentIndex, {color});
                interface_.push_back(var);
                note_interface_shape('f', color, v->type, is_signed);
            }
            const auto image = value_op(sop::Load, image_type, {var});
            const auto origin = m_.constant_composite(t_vec(t_i32(), 2), {c_i32(0), c_i32(0)});
            auto texel = value_op(sop::ImageRead, t_vec(sampled, 4), {image, origin});
            if (is_signed && !s->is_float()) texel = value_op(sop::Bitcast, t_vec(t_u32(), 4), {texel});
            const auto element = s->is_float() ? t_f32() : t_u32();
            const auto lanes = v->type->lanes();
            std::uint32_t result;
            if (lanes == 1) result = value_op(sop::CompositeExtract, element, {texel, 0});
            else if (lanes == 4) result = texel;
            else {
                std::vector<std::uint32_t> operands = {texel, texel};
                for (std::uint32_t i = 0; i < lanes; ++i) operands.push_back(i);
                result = value_op(sop::VectorShuffle, t_vec(element, lanes), operands);
            }
            return bind(v, result);
        }
        if (name == "__msl_builtin_input") {
            const auto builtin = static_cast<Builtin>(const_int(ops[0]));
            return bind(v, builtin_input(builtin, v->type));
        }
        if (name == "__msl_builtin_output") {
            builtin_output(static_cast<std::uint32_t>(const_int(ops[0])), static_cast<std::uint32_t>(const_int(ops[1])),
                id_of(ops[2]), ops[2]->type);
            return;
        }
        fail("call to unsupported function '" + full + "'");
    }

    std::uint32_t lower_group(std::uint32_t op, Value* v, std::uint32_t type) {
        using namespace spv::capability;
        const auto& ops = v->operands;
        switch (op) {
        case sop::GroupNonUniformElect: return value_op(op, t_bool(), {group_scope()});
        case sop::GroupNonUniformAll: case sop::GroupNonUniformAny:
            m_.capability(GroupNonUniformVote);
            return value_op(op, t_bool(), {group_scope(), id_of(ops[2])});
        case sop::GroupNonUniformBallot: {
            m_.capability(GroupNonUniformBallot);
            const auto ballot = value_op(op, t_vec(t_u32(), 4), {group_scope(), id_of(ops[2])});
            const auto pair = value_op(sop::VectorShuffle, t_vec(t_u32(), 2), {ballot, ballot, 0, 1});
            return value_op(sop::Bitcast, t_u64(), {pair});
        }
        case sop::GroupNonUniformBroadcastFirst:
            m_.capability(GroupNonUniformBallot);
            return value_op(op, type, {group_scope(), id_of(ops[2])});
        case sop::GroupNonUniformBroadcast:
            m_.capability(GroupNonUniformBallot);
            return value_op(op, type, {group_scope(), id_of(ops[1]), id_of(ops[2])});
        case sop::GroupNonUniformShuffle: case sop::GroupNonUniformShuffleXor:
            m_.capability(GroupNonUniformShuffle);
            return value_op(op, type, {group_scope(), id_of(ops[1]), id_of(ops[2])});
        case sop::GroupNonUniformShuffleUp: case sop::GroupNonUniformShuffleDown:
            m_.capability(GroupNonUniformShuffleRelative);
            return value_op(op, type, {group_scope(), id_of(ops[1]), id_of(ops[2])});
        case sop::GroupNonUniformQuadBroadcast:
            m_.capability(GroupNonUniformQuad);
            return value_op(op, type, {group_scope(), id_of(ops[1]), id_of(ops[2])});
        case sop::GroupNonUniformQuadSwap: {
            m_.capability(GroupNonUniformQuad);
            const auto mask = maybe_const(ops[2]);
            if (!mask || *mask < 1 || *mask > 3) fail("quad_shuffle_xor needs a constant mask of 1, 2 or 3");
            return value_op(op, type, {group_scope(), id_of(ops[1]), c_u32(static_cast<std::uint32_t>(*mask - 1))});
        }
        default: break;
        }
        if (op >= sop::GroupNonUniformIAdd && op <= sop::GroupNonUniformBitwiseXor) {
            m_.capability(GroupNonUniformArithmetic);
            const auto group = static_cast<std::uint32_t>(const_int(ops[1]));
            auto operand = id_of(ops[2]);
            const bool is_signed = op == sop::GroupNonUniformSMin || op == sop::GroupNonUniformSMax;
            if (is_signed) operand = sign_extend(operand, ops[2]->type);
            const auto r = value_op(op, type, {group_scope(), group, operand});
            return normalise(r, v->type);
        }
        fail("unsupported SIMD-group operation " + std::to_string(op));
    }

    void lower_llvm_intrinsic(Value* v, const std::string& full) {
        auto base = full.substr(5);
        const auto dot = base.find('.');
        // Names like "vector.reduce.add.v4i32" keep their dotted family.
        auto starts = [&](std::string_view prefix) { return base.rfind(prefix, 0) == 0; };
        const auto& ops = v->operands;
        const auto type = v->type->kind == ir::Type::Kind::void_ ? 0U : reg(v->type);
        if (starts("lifetime.") || starts("assume") || starts("experimental.noalias.scope.decl") || starts("dbg.") ||
            starts("invariant.") || starts("sideeffect") || starts("pseudoprobe"))
            return;
        if (starts("memcpy.") || starts("memmove.")) return lower_memcpy(v);
        if (starts("memset.")) return lower_memset(v);
        auto glsl1 = [&](std::uint32_t op) { bind(v, ext(type, op, {id_of(ops[0])})); };
        auto glsl2 = [&](std::uint32_t op) { bind(v, ext(type, op, {id_of(ops[0]), id_of(ops[1])})); };
        const auto family = base.substr(0, dot);
        using namespace spv::glsl;
        if (family == "fmuladd") {
            const auto product = value_op(sop::FMul, type, {id_of(ops[0]), id_of(ops[1])});
            return bind(v, value_op(sop::FAdd, type, {product, id_of(ops[2])}));
        }
        if (family == "fma") return bind(v, ext(type, Fma, {id_of(ops[0]), id_of(ops[1]), id_of(ops[2])}));
        if (family == "fabs") return glsl1(FAbs);
        if (family == "sqrt") return glsl1(Sqrt);
        if (family == "sin") return glsl1(Sin);
        if (family == "cos") return glsl1(Cos);
        if (family == "tan") return glsl1(Tan);
        if (family == "exp") return glsl1(Exp);
        if (family == "exp2") return glsl1(Exp2);
        if (family == "log") return glsl1(Log);
        if (family == "log2") return glsl1(Log2);
        if (family == "floor") return glsl1(Floor);
        if (family == "ceil") return glsl1(Ceil);
        if (family == "trunc") return glsl1(Trunc);
        if (family == "round") return glsl1(Round);
        if (family == "rint" || family == "nearbyint" || family == "roundeven") return glsl1(RoundEven);
        if (family == "pow") return glsl2(Pow);
        if (family == "minnum" || family == "minimum") return glsl2(FMin);
        if (family == "maxnum" || family == "maximum") return glsl2(FMax);
        if (family == "exp10") {
            const auto scaled = value_op(sop::FMul, type, {id_of(ops[0]), splat(type, m_.constant_f32(3.32192809488736234787f), v->type->lanes())});
            return bind(v, ext(type, Exp2, {scaled}));
        }
        if (family == "log10") {
            const auto l = ext(type, Log2, {id_of(ops[0])});
            return bind(v, value_op(sop::FMul, type, {l, splat(type, m_.constant_f32(0.30102999566398119521f), v->type->lanes())}));
        }
        if (family == "ldexp") return bind(v, ext(type, Ldexp, {id_of(ops[0]), id_of(ops[1])}));
        if (family == "copysign") {
            const auto lanes = v->type->lanes();
            const auto u = t_vec(t_u32(), lanes);
            const auto a = value_op(sop::Bitcast, u, {id_of(ops[0])}), b = value_op(sop::Bitcast, u, {id_of(ops[1])});
            const auto magnitude = value_op(sop::BitwiseAnd, u, {a, splat(u, c_u32(0x7fffffff), lanes)});
            const auto sign = value_op(sop::BitwiseAnd, u, {b, splat(u, c_u32(0x80000000), lanes)});
            return bind(v, value_op(sop::Bitcast, type, {value_op(sop::BitwiseOr, u, {magnitude, sign})}));
        }
        const auto* t = v->type;
        if (family == "smax" || family == "smin") {
            auto r = ext(type, family == "smax" ? SMax : SMin, {sign_extend(id_of(ops[0]), t), sign_extend(id_of(ops[1]), t)});
            return bind(v, normalise(r, t));
        }
        if (family == "umax") return glsl2(UMax);
        if (family == "umin") return glsl2(UMin);
        if (family == "abs") return bind(v, normalise(ext(type, SAbs, {sign_extend(id_of(ops[0]), t)}), t));
        if (family == "ctpop") return bind(v, value_op(sop::BitCount, type, {id_of(ops[0])}));
        if (family == "bitreverse") {
            auto r = value_op(sop::BitReverse, type, {id_of(ops[0])});
            const auto bits = scalar_bits(t);
            if (bits < 32) r = value_op(sop::ShiftRightLogical, type, {r, splat_u32(t, 32 - bits)});
            return bind(v, r);
        }
        if (family == "ctlz" || family == "cttz") {
            const auto bits = scalar_bits(t);
            if (bits > 32) fail("64-bit " + family + " is unsupported");
            const auto x = id_of(ops[0]);
            if (family == "ctlz") {
                const auto msb = ext(type, FindUMsb, {x});   // -1 for 0
                return bind(v, normalise(value_op(sop::ISub, type, {splat_u32(t, bits - 1), msb}), t));
            }
            const auto lsb = ext(type, FindILsb, {x});
            const auto zero = value_op(sop::IEqual, t_vec(t_bool(), t->lanes()), {x, splat_u32(t, 0)});
            return bind(v, value_op(sop::Select, type, {zero, splat_u32(t, bits), lsb}));
        }
        if (family == "bswap") {
            const auto bits = scalar_bits(t);
            if (bits != 32 && bits != 16) fail("bswap of " + std::to_string(bits) + " bits");
            auto x = id_of(ops[0]);
            std::uint32_t r = splat_u32(t, 0);
            for (std::uint32_t b = 0; b < bits / 8; ++b) {
                const auto byte = value_op(sop::BitFieldUExtract, type, {x, c_u32(b * 8), c_u32(8)});
                r = value_op(sop::BitFieldInsert, type, {r, byte, c_u32(bits - 8 - b * 8), c_u32(8)});
            }
            return bind(v, r);
        }
        if (family == "fshl" || family == "fshr") {
            const auto bits = scalar_bits(t);
            if (bits != 32) fail("funnel shift of " + std::to_string(bits) + " bits");
            const auto a = id_of(ops[0]), b = id_of(ops[1]);
            const auto amount = value_op(sop::BitwiseAnd, type, {id_of(ops[2]), splat_u32(t, 31)});
            const auto inverse = value_op(sop::ISub, type, {splat_u32(t, 32), amount});
            std::uint32_t r;
            if (family == "fshl") r = value_op(sop::BitwiseOr, type, {value_op(sop::ShiftLeftLogical, type, {a, amount}),
                                        value_op(sop::ShiftRightLogical, type, {b, inverse})});
            else r = value_op(sop::BitwiseOr, type, {value_op(sop::ShiftRightLogical, type, {b, amount}),
                                        value_op(sop::ShiftLeftLogical, type, {a, inverse})});
            const auto zero = value_op(sop::IEqual, t_vec(t_bool(), t->lanes()), {amount, splat_u32(t, 0)});
            return bind(v, value_op(sop::Select, type, {zero, family == "fshl" ? a : b, r}));
        }
        if (family == "uadd" && starts("uadd.sat")) {
            const auto sum = value_op(sop::IAdd, type, {id_of(ops[0]), id_of(ops[1])});
            const auto bits = scalar_bits(t);
            if (bits < 32) {
                const auto limit = splat_u32(t, (1U << bits) - 1);
                return bind(v, ext(type, UMin, {sum, limit}));
            }
            const auto overflow = value_op(sop::ULessThan, t_vec(t_bool(), t->lanes()), {sum, id_of(ops[0])});
            return bind(v, value_op(sop::Select, type, {overflow, splat_u32(t, ~0U), sum}));
        }
        if (family == "usub" && starts("usub.sat")) {
            const auto a = id_of(ops[0]), b = id_of(ops[1]);
            const auto under = value_op(sop::ULessThan, t_vec(t_bool(), t->lanes()), {a, b});
            return bind(v, value_op(sop::Select, type, {under, splat_u32(t, 0), value_op(sop::ISub, type, {a, b})}));
        }
        if (starts("vector.reduce.")) {
            const auto op = base.substr(14, base.find('.', 14) - 14);
            const auto* vt = ops.back()->type;
            const auto vec = id_of(ops.back());
            const auto element = reg(vt->element);
            std::uint32_t acc = 0;
            for (std::uint64_t i = 0; i < vt->count; ++i) {
                const auto lane = value_op(sop::CompositeExtract, element, {vec, static_cast<std::uint32_t>(i)});
                if (!acc) {
                    acc = lane;
                    if (op == "fadd" || op == "fmul") {
                        acc = value_op(op == "fadd" ? sop::FAdd : sop::FMul, element, {id_of(ops[0]), lane});
                    }
                    continue;
                }
                const bool boolean = vt->element->is_int() && vt->element->bits == 1;
                if (op == "add") acc = normalise(value_op(sop::IAdd, element, {acc, lane}), vt->element);
                else if (op == "mul") acc = normalise(value_op(sop::IMul, element, {acc, lane}), vt->element);
                else if (op == "and") acc = value_op(boolean ? sop::LogicalAnd : sop::BitwiseAnd, element, {acc, lane});
                else if (op == "or") acc = value_op(boolean ? sop::LogicalOr : sop::BitwiseOr, element, {acc, lane});
                else if (op == "xor") acc = value_op(boolean ? sop::LogicalNotEqual : sop::BitwiseXor, element, {acc, lane});
                else if (op == "umax") acc = ext(element, UMax, {acc, lane});
                else if (op == "umin") acc = ext(element, UMin, {acc, lane});
                else if (op == "smax") acc = normalise(ext(element, SMax, {sign_extend(acc, vt->element), sign_extend(lane, vt->element)}), vt->element);
                else if (op == "smin") acc = normalise(ext(element, SMin, {sign_extend(acc, vt->element), sign_extend(lane, vt->element)}), vt->element);
                else if (op == "fadd") acc = value_op(sop::FAdd, element, {acc, lane});
                else if (op == "fmul") acc = value_op(sop::FMul, element, {acc, lane});
                else if (op == "fmax") acc = ext(element, FMax, {acc, lane});
                else if (op == "fmin") acc = ext(element, FMin, {acc, lane});
                else fail("unsupported vector reduction " + op);
            }
            return bind(v, acc);
        }
        fail("unsupported LLVM intrinsic " + full);
    }

    void lower_memcpy(Value* v) {
        const auto dst = ptr_of(v->operands[0]), src = ptr_of(v->operands[1]);
        const auto length = maybe_const(v->operands[2]);
        if (!length) fail("memcpy with a run-time length is unsupported");
        const auto dst_align = v->arg_alignment.size() > 0 && v->arg_alignment[0] ? v->arg_alignment[0] : 1;
        const auto src_align = v->arg_alignment.size() > 1 && v->arg_alignment[1] ? v->arg_alignment[1] : 1;
        const auto align = std::min(dst_align, src_align);
        const auto chunk = align >= 4 ? 4U : 1U;
        const auto* chunk_type = irm_.integer(chunk * 8);
        for (std::uint64_t at = 0; at < *length; at += chunk) {
            const auto size = std::min<std::uint64_t>(chunk, *length - at);
            const auto* t = size == chunk ? chunk_type : irm_.integer(8);
            Ptr s = src, d = dst;
            s.offset += static_cast<std::int64_t>(at);
            d.offset += static_cast<std::int64_t>(at);
            const auto value = load(s, t, std::min<std::uint64_t>(src_align, chunk), false);
            store(d, t, value, std::min<std::uint64_t>(dst_align, chunk), false);
            if (size != chunk) {
                for (std::uint64_t b = 1; b < size; ++b) {
                    Ptr sb = s, db = d;
                    sb.offset += static_cast<std::int64_t>(b);
                    db.offset += static_cast<std::int64_t>(b);
                    store(db, irm_.integer(8), load(sb, irm_.integer(8), 1, false), 1, false);
                }
            }
        }
    }
    void lower_memset(Value* v) {
        const auto dst = ptr_of(v->operands[0]);
        const auto length = maybe_const(v->operands[2]);
        if (!length) fail("memset with a run-time length is unsupported");
        const auto align = v->arg_alignment.size() > 0 && v->arg_alignment[0] ? v->arg_alignment[0] : 1;
        const auto byte = id_of(v->operands[1]);
        std::uint32_t word = byte;
        const bool constant_byte = v->operands[1]->kind == Value::Kind::constant_int;
        if (constant_byte) {
            const auto b = static_cast<std::uint32_t>(v->operands[1]->int_value & 0xff);
            word = c_u32(b | (b << 8) | (b << 16) | (b << 24));
        } else {
            word = value_op(sop::IMul, t_u32(), {byte, c_u32(0x01010101)});
        }
        for (std::uint64_t at = 0; at < *length;) {
            Ptr d = dst;
            d.offset += static_cast<std::int64_t>(at);
            if (align >= 4 && *length - at >= 4) {
                store(d, irm_.integer(32), word, 4, false);
                at += 4;
            } else {
                store(d, irm_.integer(8), byte, 1, false);
                at += 1;
            }
        }
    }

    // ---- function emission ---------------------------------------------------------------------
    bool block_terminated_{};
    struct PhiPatch {
        std::size_t position;     // word index of the value operand in code
        Value* operand;
        ir::BasicBlock* pred;
    };
    std::vector<PhiPatch> phi_patches_;

    void emit_function() {
        function_id_ = m_.id();
        m_.emit(sop::Function, {t_void(), function_id_, 0, m_.type_function(t_void())});
        std::size_t variables_at = 0;
        for (auto* block : cfg_.order) label(block);
        bool first = true;
        for (auto* block : cfg_.order) {
            current_ = block;
            block_terminated_ = false;
            m_.emit(sop::Label, {label(block)});
            if (first) {
                variables_at = m_.code().size();
                first = false;
            }
            // Phis first.
            std::size_t i = 0;
            for (; i < block->instructions.size(); ++i) {
                auto* v = block->instructions[i];
                if (v->op != Opcode::phi) break;
                emit_phi(v);
            }
            for (auto* v : block->instructions) if (v->op == Opcode::phi) store_demoted_phi(v);
            for (; i + 1 < block->instructions.size(); ++i) {
                if (block_terminated_) break;
                at_ = block->instructions[i];
                lower_instruction(at_);
            }
            if (!block_terminated_) emit_terminator(block);
        }
        m_.emit(sop::FunctionEnd, {});
        // Patch phi value operands.
        for (const auto& patch : phi_patches_) {
            const auto found = patch_values_.find({patch.operand, patch.pred});
            if (found == patch_values_.end()) fail("phi operand %" + patch.operand->name + " has no value on its edge");
            m_.code()[patch.position] = found->second;
        }
        auto& vars = m_.function_variables();
        m_.code().insert(m_.code().begin() + static_cast<std::ptrdiff_t>(variables_at), vars.begin(), vars.end());
    }
    std::map<std::pair<Value*, ir::BasicBlock*>, std::uint32_t> patch_values_;

    void emit_phi(Value* v) {
        std::uint32_t type;
        Space space = Space::none;
        if (v->type->is_pointer()) {
            space = spaces_.at(v);
            type = rep_type(space);
        } else {
            type = reg(v->type);
        }
        const auto result = m_.id();
        std::vector<std::uint32_t> words = {type, result};
        std::vector<std::pair<std::size_t, std::size_t>> positions;
        for (std::size_t k = 0; k < v->operands.size(); ++k) {
            positions.push_back({words.size(), k});
            words.push_back(0);
            words.push_back(label(v->targets[k]));
        }
        const auto base = m_.code().size() + 1;   // after the instruction word
        m_.emit(sop::Phi, words);
        for (const auto& [offset, k] : positions) phi_patches_.push_back({base + offset, v->operands[k], v->targets[k]});
        if (v->type->is_pointer()) {
            Lowered l;
            l.is_ptr = true;
            l.ptr = {space, result, 0};
            values_[v] = l;
        } else {
            Lowered l;
            l.id = result;
            values_[v] = l;
        }
    }
    void store_demoted_phi(Value* v) { store_demoted(v); }

    // Values the successors' phis need from this block, computed before its terminator.
    void prepare_phi_operands(ir::BasicBlock* block) {
        for (auto* successor : block->successors) {
            for (auto* phi : successor->instructions) {
                if (phi->op != Opcode::phi) break;
                for (std::size_t k = 0; k < phi->operands.size(); ++k) {
                    if (phi->targets[k] != block) continue;
                    auto* operand = phi->operands[k];
                    std::uint32_t id;
                    if (phi->type->is_pointer()) {
                        if (operand->is_undef()) id = m_.undef(rep_type(spaces_.at(phi)));
                        else id = materialise(ptr_of(operand));
                    } else {
                        id = id_of(operand);
                    }
                    patch_values_[{operand, block}] = id;
                }
            }
        }
    }

    void emit_terminator(ir::BasicBlock* block) {
        auto* t = block->instructions.back();
        prepare_phi_operands(block);
        const auto merge = cfg_.merges.find(block);
        switch (t->op) {
        case Opcode::br:
            if (t->targets.size() == 1) {
                if (merge != cfg_.merges.end() && merge->second.kind == Merge::Kind::loop)
                    m_.emit(sop::LoopMerge, {label(merge->second.merge), label(merge->second.continue_target), 0});
                m_.emit(sop::Branch, {label(t->targets[0])});
            } else {
                const auto condition = id_of(t->operands[0]);
                if (merge != cfg_.merges.end()) {
                    if (merge->second.kind == Merge::Kind::loop)
                        m_.emit(sop::LoopMerge, {label(merge->second.merge), label(merge->second.continue_target), 0});
                    else
                        m_.emit(sop::SelectionMerge, {label(merge->second.merge), 0});
                }
                m_.emit(sop::BranchConditional, {condition, label(t->targets[0]), label(t->targets[1])});
            }
            return;
        case Opcode::ret:
            m_.emit(sop::Return, {});
            return;
        case Opcode::unreachable:
            m_.emit(sop::Unreachable, {});
            return;
        default: fail("unexpected terminator");
        }
    }

    void lower_instruction(Value* v) {
        switch (v->op) {
        case Opcode::add: case Opcode::sub: case Opcode::mul: case Opcode::udiv: case Opcode::sdiv: case Opcode::urem:
        case Opcode::srem: case Opcode::shl: case Opcode::lshr: case Opcode::ashr: case Opcode::and_: case Opcode::or_:
        case Opcode::xor_: case Opcode::fadd: case Opcode::fsub: case Opcode::fmul: case Opcode::fdiv: case Opcode::frem:
            return lower_binary(v);
        case Opcode::fneg: return bind(v, value_op(sop::FNegate, reg(v->type), {id_of(v->operands[0])}));
        case Opcode::icmp: return lower_icmp(v);
        case Opcode::fcmp: return lower_fcmp(v);
        case Opcode::trunc: case Opcode::zext: case Opcode::sext: case Opcode::fptrunc: case Opcode::fpext:
        case Opcode::fptoui: case Opcode::fptosi: case Opcode::uitofp: case Opcode::sitofp: case Opcode::ptrtoint:
        case Opcode::inttoptr: case Opcode::bitcast: case Opcode::addrspacecast:
            return lower_cast(v);
        case Opcode::select: return lower_select(v);
        case Opcode::freeze:
            if (v->type->is_pointer()) return bind_ptr(v, ptr_of(v->operands[0]));
            return bind(v, id_of(v->operands[0]));
        case Opcode::alloca: return bind_ptr(v, {Space::stack, 0, static_cast<std::int64_t>(alloca_offsets_.at(v))});
        case Opcode::getelementptr: return lower_gep(v);
        case Opcode::load: return lower_load(v);
        case Opcode::store: return lower_store(v);
        case Opcode::extractelement: case Opcode::insertelement: case Opcode::shufflevector:
        case Opcode::extractvalue: case Opcode::insertvalue:
            return lower_vector(v);
        case Opcode::atomicrmw: return lower_atomicrmw(v);
        case Opcode::cmpxchg: return lower_cmpxchg(v);
        case Opcode::fence:
            m_.emit(sop::MemoryBarrier, {c_u32(scope_device), c_u32(sem_acq_rel | sem_uniform | sem_workgroup | sem_image)});
            return;
        case Opcode::call: return lower_call(v);
        default: fail("unsupported instruction in the entry point body");
        }
    }

    // ---- entry point and reflection ----------------------------------------------------------
    void finish_entry() {
        std::uint32_t model = 0;
        switch (entry_.stage) {
        case Stage::vertex: model = 0; break;
        case Stage::fragment: model = 4; break;
        case Stage::kernel: model = 5; break;
        }
        m_.capability(spv::capability::Shader);
        m_.entry_point(model, function_id_, entry_.name, interface_);
        if (entry_.stage == Stage::fragment) {
            m_.execution_mode(function_id_, 7);   // OriginUpperLeft
            if (entry_.early_fragment_tests) m_.execution_mode(function_id_, 9);
            if (writes_depth_) {
                m_.execution_mode(function_id_, 12);   // DepthReplacing
                if (entry_.depth == DepthOutput::greater) m_.execution_mode(function_id_, 14);
                if (entry_.depth == DepthOutput::less) m_.execution_mode(function_id_, 15);
            }
        }
        if (entry_.stage == Stage::kernel) {
            m_.execution_mode(function_id_, 17, {1, 1, 1});   // LocalSize, overridden by WorkgroupSize
            workgroup_size();
        }
        m_.name(function_id_, entry_.name);
    }

    void build_reflection() {
        auto& r = reflection_;
        const auto set = stage_descriptor_set(entry_.stage);
        if (!entry_.buffers.empty()) {
            r.argument_table = {set, argument_table_binding, DescriptorType::uniform_buffer, 1};
            std::uint32_t highest = 0;
            for (const auto& b : entry_.buffers) highest = std::max(highest, b.index);
            r.argument_table_size = dynamic_buffer_index_ ? 8 * 32 : 8 * (highest + 1);
        }
        for (const auto& b : entry_.buffers) {
            BufferBinding binding;
            binding.index = b.index;
            binding.name = b.name;
            binding.space = b.space;
            binding.access = b.space == BufferBinding::Space::constant || b.is_const ? Access::read : Access::read_write;
            binding.type = add_reflection_type(r, parse_type_spelling(b.pointee));
            binding.reference = b.reference;
            binding.function_constant = b.function_constant;
            binding.index_function_constant = b.index_constant;
            binding.used = b.index_constant ? dynamic_buffer_index_ : used_buffer_slots_.count(b.index) != 0;
            binding.argument_buffer = b.argument_buffer;
            for (const auto& a : b.arguments) {
                BufferBinding::Argument argument;
                argument.name = a.name;
                argument.id = a.id;
                argument.count = a.count;
                argument.offset = a.offset;
                argument.stride = a.stride;
                argument.pointer = a.pointer;
                if (!a.pointer) argument.type = add_reflection_type(r, a.type);
                binding.arguments.push_back(std::move(argument));
            }
            binding.argument_length = b.argument_length;
            binding.argument_alignment = b.argument_alignment;
            r.buffers.push_back(std::move(binding));
        }
        for (const auto& t : entry_.textures) {
            TextureBinding binding;
            binding.index = t.index;
            binding.name = t.name;
            binding.texture_type = t.type.texture;
            binding.sample_scalar = t.type.scalar;
            binding.access = t.type.access;
            binding.array_length = t.type.array_length ? t.type.array_length : 1;
            const bool storage = t.type.access == Access::write || t.type.access == Access::read_write ||
                                 atomic_textures_.count(t.index);
            DescriptorType type = storage ? DescriptorType::storage_image : DescriptorType::sampled_image;
            if (t.type.texture == TextureType::texture_buffer)
                type = storage ? DescriptorType::storage_texel_buffer : DescriptorType::uniform_texel_buffer;
            binding.descriptor = {set, texture_binding_base + t.index, type, binding.array_length};
            binding.used = used_textures_.count(t.index) != 0;
            binding.function_constant = t.function_constant;
            r.textures.push_back(std::move(binding));
        }
        for (const auto& s : entry_.samplers) {
            SamplerBinding binding;
            binding.index = s.index;
            binding.name = s.name;
            binding.descriptor = {set, sampler_binding_base + s.index, DescriptorType::sampler, 1};
            binding.used = used_samplers_.count(s.index) != 0;
            r.samplers.push_back(std::move(binding));
        }
        for (std::size_t k = 0; k < entry_.argument_buffers.size(); ++k) {
            const auto& ab = entry_.argument_buffers[k];
            ArgumentBufferSamplers binding;
            binding.index = ab.index;
            binding.index_function_constant = ab.index_constant;
            binding.name = ab.name;
            binding.type = ab.type;
            binding.members = ab.members;
            binding.count = ab.count;
            binding.descriptor = {set, argument_buffer_sampler_binding_base + static_cast<std::uint32_t>(k),
                                  DescriptorType::sampler, ab.count};
            binding.used = used_argument_buffers_.count(static_cast<std::uint32_t>(k)) != 0;
            r.argument_buffers.push_back(std::move(binding));
        }
        std::vector<std::pair<std::uint32_t, std::uint64_t>> by_slot;
        for (const auto& [state, slot] : constexpr_samplers_) by_slot.push_back({slot, state});
        std::sort(by_slot.begin(), by_slot.end());
        for (const auto& [slot, state] : by_slot) {
            ConstexprSampler s;
            s.descriptor = {set, constexpr_sampler_binding_base + slot, DescriptorType::sampler, 1};
            s.min_filter = state & 1;
            s.mag_filter = (state >> 1) & 1;
            s.mip_filter = (state >> 2) & 3;
            s.address = {static_cast<std::uint32_t>((state >> 4) & 7), static_cast<std::uint32_t>((state >> 7) & 7),
                         static_cast<std::uint32_t>((state >> 10) & 7)};
            s.compare_function = (state >> 13) & 7;
            s.border_color = (state >> 16) & 3;
            s.max_anisotropy = static_cast<std::uint32_t>(((state >> 18) & 31) + 1);
            s.normalized_coordinates = ((state >> 23) & 1) == 0;
            s.lod_min = half_to_float(static_cast<std::uint16_t>((state >> 32) & 0xffff));
            s.lod_max = half_to_float(static_cast<std::uint16_t>((state >> 48) & 0x7fff));
            r.constexpr_samplers.push_back(s);
        }
        std::set<std::uint32_t> constants_used = fc_value_used_;
        constants_used.insert(fc_defined_used_.begin(), fc_defined_used_.end());
        // A [[buffer(fc + n)]] index constant is reflected even when the
        // stage never reads that buffer: Metal resolves the index on the
        // host (newArgumentEncoderWithBufferIndex:, argument buffers of
        // samplers), which needs the constant's value, so its name must be
        // known to match MTLFunctionConstantValues set by name.
        for (const auto& b : entry_.buffers) if (b.index_constant) constants_used.insert(*b.index_constant);
        for (const auto& ab : entry_.argument_buffers) if (ab.index_constant) constants_used.insert(*ab.index_constant);
        for (const auto index : constants_used) {
            const auto* decl = constant_decl(index);
            FunctionConstant c;
            c.index = index;
            if (decl) {
                c.name = decl->name;
                c.type = add_reflection_type(r, decl->type);
            }
            c.required = fc_value_used_.count(index) && !fc_defined_used_.count(index);
            r.function_constants.push_back(std::move(c));
        }
        r.builtins.assign(used_builtins_.begin(), used_builtins_.end());
        for (const auto& a : entry_.attributes) {
            VertexAttribute attribute;
            attribute.index = a.location;
            attribute.name = a.name;
            attribute.type = add_reflection_type(r, reflected_spelling('a', a.location, a.type));
            attribute.used = used_attributes_.count(a.location) != 0;
            r.stage_in.push_back(std::move(attribute));
        }
        for (const auto& o : entry_.outputs) {
            Varying varying;
            varying.name = o.name;
            varying.type = add_reflection_type(r, reflected_spelling('o', o.location, o.type));
            varying.location = o.location;
            varying.interpolation = o.interpolation;
            r.outputs.push_back(std::move(varying));
        }
        for (const auto& i : entry_.inputs) {
            if (!used_inputs_.count(i.location)) continue;
            Varying varying;
            varying.name = i.name;
            varying.type = add_reflection_type(r, reflected_spelling('i', i.location, i.type));
            varying.location = i.location;
            varying.interpolation = i.interpolation;
            r.inputs.push_back(std::move(varying));
        }
        for (const auto& c : entry_.colors) {
            FragmentOutput output;
            output.color = c.color;
            output.index = c.index;
            output.type = add_reflection_type(r, reflected_spelling('c', c.color * 2 + (c.index ? 1 : 0), c.type));
            r.color_outputs.push_back(output);
        }
        for (const auto& c : entry_.color_inputs) {
            if (!color_input_vars_.count(c.color)) continue;   // never read
            ColorInput input;
            input.color = c.color;
            input.name = c.name;
            input.type = add_reflection_type(r, reflected_spelling('f', c.color, c.type));
            input.descriptor = {set, color_input_binding_base + c.color, DescriptorType::input_attachment, 1};
            r.color_inputs.push_back(std::move(input));
        }
        r.depth = writes_depth_ ? entry_.depth : DepthOutput::none;
        r.writes_sample_mask = writes_mask_;
        r.writes_stencil = writes_stencil_;
        r.early_fragment_tests = entry_.early_fragment_tests;
        r.discards = discards_;
        r.max_total_threads_per_threadgroup = entry_.max_total_threads_per_threadgroup;
        for (const auto& g : entry_.threadgroups) {
            ThreadgroupMemory memory;
            memory.index = g.index;
            memory.name = g.name;
            memory.type = add_reflection_type(r, parse_type_spelling(g.element));
            memory.spec_id = reserved_spec_id::threadgroup_memory_base + g.index;
            r.threadgroup_memory.push_back(std::move(memory));
        }
        r.uses_simdgroup_operations = uses_simd_;
        r.notes = entry_.notes;
    }
    static float half_to_float(std::uint16_t h) {
        const std::uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
        float value;
        if (exponent == 0) value = std::ldexp(static_cast<float>(mantissa), -24);
        else if (exponent == 31) value = mantissa ? NAN : INFINITY;
        else value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
        return sign ? -value : value;
    }
};

} // namespace

LowerResult lower(ir::Module& module, const EntryPoint& entry, const std::vector<FunctionConstantDecl>& constants,
                  const CompileOptions& options) {
    return Lowerer(module, entry, constants, options).run();
}

} // namespace alchemy::msl::detail
