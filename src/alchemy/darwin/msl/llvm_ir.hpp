#pragma once

// In-memory model and parser for the textual LLVM IR subset clang 19 emits
// for the MSL compiler's spir64 C++-for-OpenCL modules (owner: A6). Only what
// the lowering needs is modelled; anything else is a parse error naming the
// construct, never silently skipped.

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::msl::ir {

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- types ---------------------------------------------------------------------------
struct Type {
    enum class Kind { void_, integer, half, bfloat, float_, double_, pointer, vector, array, structure, label, metadata,
        token, function };
    Kind kind{Kind::void_};
    std::uint32_t bits{};             // integer width
    std::uint64_t count{};            // vector/array length
    const Type* element{};            // vector/array element
    std::vector<const Type*> members; // structure members / function params (members[0] = return)
    bool packed{};
    std::uint32_t address_space{};    // pointer
    std::string name;                 // named structure

    [[nodiscard]] bool is_int() const { return kind == Kind::integer; }
    [[nodiscard]] bool is_float() const { return kind == Kind::half || kind == Kind::float_ || kind == Kind::double_ || kind == Kind::bfloat; }
    [[nodiscard]] bool is_vector() const { return kind == Kind::vector; }
    [[nodiscard]] bool is_pointer() const { return kind == Kind::pointer; }
    [[nodiscard]] bool is_aggregate() const { return kind == Kind::array || kind == Kind::structure; }
    [[nodiscard]] const Type* scalar() const { return kind == Kind::vector ? element : this; }
    [[nodiscard]] std::uint32_t lanes() const { return kind == Kind::vector ? static_cast<std::uint32_t>(count) : 1; }
};

// Data layout of spir64 ("e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024").
[[nodiscard]] std::uint64_t size_of(const Type* type);       // alloc size in bytes
[[nodiscard]] std::uint64_t align_of(const Type* type);      // ABI alignment
[[nodiscard]] std::uint64_t member_offset(const Type* structure, std::size_t member);
[[nodiscard]] std::string to_string(const Type* type);

// ---- values -----------------------------------------------------------------------------
struct BasicBlock;
struct Function;

enum class Opcode {
    none,
    // binary
    add, sub, mul, udiv, sdiv, urem, srem, shl, lshr, ashr, and_, or_, xor_, fadd, fsub, fmul, fdiv, frem,
    fneg,
    icmp, fcmp,
    // casts
    trunc, zext, sext, fptrunc, fpext, fptoui, fptosi, uitofp, sitofp, ptrtoint, inttoptr, bitcast, addrspacecast,
    select, phi, call, load, store, alloca, getelementptr, extractelement, insertelement, shufflevector,
    extractvalue, insertvalue, freeze, atomicrmw, cmpxchg, fence,
    br, switch_, ret, unreachable,
};

enum class Predicate {
    none, eq, ne, ugt, uge, ult, ule, sgt, sge, slt, sle,
    f_false, f_oeq, f_ogt, f_oge, f_olt, f_ole, f_one, f_ord, f_ueq, f_ugt, f_uge, f_ult, f_ule, f_une, f_uno, f_true,
};

enum class AtomicOp { xchg, add, sub, and_, nand, or_, xor_, max, min, umax, umin, fadd, fsub, fmax, fmin };

struct Value {
    enum class Kind { constant_int, constant_fp, constant_null, constant_undef, constant_poison, constant_aggregate,
        constant_expr, global, function, argument, instruction, block, placeholder };
    virtual ~Value() = default;
    Kind kind{Kind::placeholder};
    const Type* type{};
    std::string name;

    // constant_int (zero-extended bits), constant_fp (as double).
    std::uint64_t int_value{};
    double fp_value{};
    // constant_aggregate elements / constant_expr and instruction operands.
    std::vector<Value*> operands;

    // instruction / constant_expr
    Opcode op{Opcode::none};
    Predicate predicate{Predicate::none};
    AtomicOp atomic_op{AtomicOp::xchg};
    const Type* aux_type{};          // alloca/load/gep source/cast destination type
    std::vector<std::int64_t> indices;   // extractvalue/insertvalue indices, shufflevector mask (-1 = poison)
    std::vector<BasicBlock*> targets;    // br/switch targets, phi incoming blocks (parallel to operands)
    std::uint64_t alignment{};
    std::vector<std::uint64_t> arg_alignment;   // call: "align N" parameter attributes (0 = none)
    bool is_volatile{}, inbounds{}, atomic{}, nsw{}, nuw{}, exact{}, weak{};
    BasicBlock* parent{};
    Function* callee{};              // direct call target
    std::uint32_t index{};           // argument number / instruction ordinal in its function

    [[nodiscard]] bool is_constant() const {
        return kind == Kind::constant_int || kind == Kind::constant_fp || kind == Kind::constant_null ||
               kind == Kind::constant_undef || kind == Kind::constant_poison || kind == Kind::constant_aggregate ||
               kind == Kind::constant_expr;
    }
    [[nodiscard]] bool is_undef() const { return kind == Kind::constant_undef || kind == Kind::constant_poison; }
};

struct BasicBlock : Value {
    std::vector<Value*> instructions;
    Function* function{};
    std::vector<BasicBlock*> predecessors, successors;   // filled by Function::compute_cfg
};

struct Global : Value {
    const Type* value_type{};
    Value* initializer{};            // null for external globals
    bool constant{};
    std::uint32_t address_space{};
};

struct Function : Value {
    const Type* return_type{};
    std::vector<Value*> arguments;
    std::vector<BasicBlock*> blocks;
    bool declaration{};
    void compute_cfg();
};

class Module {
public:
    std::vector<Global*> globals;
    std::vector<Function*> functions;

    [[nodiscard]] Function* function(std::string_view name) const;
    [[nodiscard]] Global* global(std::string_view name) const;

    // Arena allocation (stable addresses for the module's lifetime).
    template<typename T> T* make() {
        auto owned = std::make_unique<T>();
        auto* raw = owned.get();
        storage_.push_back(std::unique_ptr<Value>(std::move(owned)));
        return raw;
    }
    const Type* intern(Type type);
    const Type* integer(std::uint32_t bits);
    const Type* primitive(Type::Kind kind);
    const Type* pointer(std::uint32_t address_space);
    const Type* vector(const Type* element, std::uint64_t count);
    const Type* array(const Type* element, std::uint64_t count);
    Value* constant_int(const Type* type, std::uint64_t value);
    Value* constant_fp(const Type* type, double value);
    Value* special(Value::Kind kind, const Type* type);

    std::map<std::string, Type*, std::less<>> named_types;
    std::deque<Type> named_storage;

private:
    std::vector<std::unique_ptr<Value>> storage_;
    std::deque<Type> types_;
    std::map<std::string, const Type*> interned_;
};

// Parses a whole module. Throws ParseError with the line number on input it
// does not model.
[[nodiscard]] std::unique_ptr<Module> parse(std::string_view text);

// Helpers.
[[nodiscard]] std::string demangled_identifier(std::string_view mangled);   // "_Z10__msl_spv2I..." -> "__msl_spv2"
[[nodiscard]] bool is_terminator(Opcode op);

} // namespace alchemy::msl::ir
