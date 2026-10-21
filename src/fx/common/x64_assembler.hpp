#pragma once

// In-house x64 assembler for the fx engine (translator, runtime stubs, thunks).
//
// Owner: integration. Components may add wrappers in their own files using the
// public generic encoders (emit_rr/emit_rm, sse_raw, vex_rr/vex_rm) and the
// SseOp/VexOp descriptors; only add to this header for new encoding forms.
//
// Conventions: `bits` is the operand size (8, 16, 32 or 64). 32-bit results
// zero-extend into the full register, matching A64 W-register writes.
// Label references are resolved by finalize() (or bind(), for labels bound
// after their uses); call finalize() once before copying the bytes.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <vector>

#if defined(_MSC_VER)
#define FX_X64_INLINE __forceinline
#else
#define FX_X64_INLINE inline __attribute__((always_inline))
#endif

namespace transmute::fx::x64 {

enum class Gp : std::uint8_t {
    rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi,
    r8, r9, r10, r11, r12, r13, r14, r15,
    none = 0xff,
};
enum class Xmm : std::uint8_t {
    xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7,
    xmm8, xmm9, xmm10, xmm11, xmm12, xmm13, xmm14, xmm15,
};
enum class Cond : std::uint8_t {
    o = 0, no = 1, b = 2, ae = 3, e = 4, ne = 5, be = 6, a = 7,
    s = 8, ns = 9, p = 10, np = 11, l = 12, ge = 13, le = 14, g = 15,
    c = b, nc = ae, z = e, nz = ne,
};
constexpr Cond invert(Cond c) noexcept { return static_cast<Cond>(static_cast<unsigned>(c) ^ 1u); }
constexpr unsigned id(Gp r) noexcept { return static_cast<unsigned>(r); }
constexpr unsigned id(Xmm r) noexcept { return static_cast<unsigned>(r); }
constexpr Gp gp(unsigned n) noexcept { return static_cast<Gp>(n); }
constexpr Xmm xmm(unsigned n) noexcept { return static_cast<Xmm>(n); }

inline constexpr std::uint32_t kNoLabel = UINT32_MAX;

struct Label {
    std::uint32_t id{kNoLabel};
    [[nodiscard]] bool valid() const noexcept { return id != kNoLabel; }
};

// Memory operand: [base + index*scale + disp] or [rip + label + disp].
struct Mem {
    Gp base{Gp::none};
    Gp index{Gp::none};
    std::uint8_t scale{1};
    std::int32_t disp{};
    std::uint32_t label{kNoLabel};
};
constexpr Mem ptr(Gp base, std::int32_t disp = 0) noexcept {
    Mem m; m.base = base; m.disp = disp; return m;
}
constexpr Mem ptr(Gp base, Gp index, unsigned scale, std::int32_t disp = 0) noexcept {
    Mem m; m.base = base; m.index = index; m.scale = static_cast<std::uint8_t>(scale); m.disp = disp;
    return m;
}
constexpr Mem ptr_index(Gp index, unsigned scale, std::int32_t disp = 0) noexcept {
    Mem m; m.index = index; m.scale = static_cast<std::uint8_t>(scale); m.disp = disp; return m;
}
constexpr Mem rip(Label label, std::int32_t addend = 0) noexcept {
    Mem m; m.label = label.id; m.disp = addend; return m;
}

enum class Alu : std::uint8_t { add = 0, or_ = 1, adc = 2, sbb = 3, and_ = 4, sub = 5, xor_ = 6, cmp = 7 };
enum class Shift : std::uint8_t { rol = 0, ror = 1, rcl = 2, rcr = 3, shl = 4, shr = 5, sar = 7 };

// Legacy SSE opcode: mandatory prefix (0, 0x66, 0xF2, 0xF3), map (1 = 0F,
// 2 = 0F38, 3 = 0F3A) and opcode byte.
struct SseOp {
    std::uint8_t prefix;
    std::uint8_t map;
    std::uint8_t opcode;
};
// VEX opcode: pp (0 none, 1 = 66, 2 = F3, 3 = F2), map (1 0F, 2 0F38, 3 0F3A),
// opcode byte, and VEX.W.
struct VexOp {
    std::uint8_t pp;
    std::uint8_t map;
    std::uint8_t opcode;
    bool w;
};

class Assembler {
public:
    explicit Assembler(std::size_t reserve = 1024) { bytes_.resize(reserve < 64 ? 64 : reserve); }

    // The buffer runs ahead of the code (bytes_.size() >= size_), so emitting
    // a byte is a store and an increment; bytes() trims it to the code. Code
    // that changes the vector bytes() returns (or moves it out) must not emit
    // through this assembler afterwards.
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { bytes_.resize(size_); return bytes_; }
    [[nodiscard]] std::vector<std::uint8_t>& bytes() noexcept { bytes_.resize(size_); return bytes_; }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return bytes_.data(); }

    // ---- raw data ----------------------------------------------------------
    // (Forced inline, multi-byte values appended at once: byte-wise calls were
    // a tenth of the JIT's time. x64 is little-endian, like the encoding.)
    FX_X64_INLINE void db(std::uint8_t v) {
        if (size_ == bytes_.size()) grow(1);
        bytes_.data()[size_++] = v;
    }
    FX_X64_INLINE void dw(std::uint16_t v) { append(&v, 2); }
    FX_X64_INLINE void dd(std::uint32_t v) { append(&v, 4); }
    FX_X64_INLINE void dq(std::uint64_t v) { append(&v, 8); }
    void put32(std::size_t at, std::uint32_t v) { std::memcpy(bytes_.data() + at, &v, 4); }
    void put64(std::size_t at, std::uint64_t v) { std::memcpy(bytes_.data() + at, &v, 8); }

    // ---- labels -------------------------------------------------------------
    Label new_label() {
        labels_.push_back(kUnbound);
        return Label{static_cast<std::uint32_t>(labels_.size() - 1)};
    }
    void bind(Label label) {
        assert(label.valid() && labels_[label.id] == kUnbound);
        labels_[label.id] = size_;
        ++control_flow_;
    }
    Label bound_label() { Label l = new_label(); bind(l); return l; }
    // Pre-sizes the label and label-reference tables (no reallocation while emitting).
    void reserve_labels(std::size_t labels, std::size_t refs) { labels_.reserve(labels); refs_.reserve(refs); }
    [[nodiscard]] bool is_bound(Label label) const { return labels_.at(label.id) != kUnbound; }
    // Number of labels bound and label-relative jumps emitted so far: callers
    // can tell whether a code sequence contains internal control flow.
    [[nodiscard]] std::uint32_t control_flow_count() const noexcept { return control_flow_; }
    [[nodiscard]] std::size_t offset_of(Label label) const {
        const auto at = labels_.at(label.id);
        if (at == kUnbound) throw std::logic_error("x64 label is not bound");
        return at;
    }
    // Resolves every label reference. Throws if a referenced label is unbound
    // or a rel8 reference is out of range.
    void finalize() {
        for (const auto& ref : refs_) {
            const auto target = offset_of(Label{ref.label});
            const auto value = static_cast<std::int64_t>(target) + ref.addend -
                               static_cast<std::int64_t>(ref.end);
            if (ref.width == 1) {
                if (value < -128 || value > 127) throw std::logic_error("x64 rel8 out of range");
                bytes_[ref.at] = static_cast<std::uint8_t>(static_cast<std::int8_t>(value));
            } else {
                if (value < INT32_MIN || value > INT32_MAX) throw std::logic_error("x64 rel32 out of range");
                put32(ref.at, static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
            }
        }
        finalized_refs_ = refs_.size();
    }

    // Pads with NOPs so that the next `opcode_length` bytes end on a 4-byte
    // boundary: a following rel32 field is then atomically patchable.
    void align_rel32_after(unsigned opcode_length) {
        while ((size_ + opcode_length) % 4 != 0) db(0x90);
    }
    void align(unsigned alignment) {
        const auto pad = (alignment - size_ % alignment) % alignment;
        nop(static_cast<unsigned>(pad));
    }

    // ---- generic encoders --------------------------------------------------
    // Register/register form. `reg` is the ModRM.reg field (register number or
    // opcode extension /digit), `rm` the ModRM.rm register. `byte_regs`
    // forces a REX prefix so 4..7 name spl/bpl/sil/dil.
    void emit_rr(std::uint8_t prefix, bool w, std::initializer_list<std::uint8_t> opcode,
                 unsigned reg, unsigned rm, bool byte_regs = false) {
        if (prefix != 0) db(prefix);
        const bool force = byte_regs && ((reg >= 4 && reg < 8) || (rm >= 4 && rm < 8));
        rex(w, reg, 0, rm, force);
        for (auto b : opcode) db(b);
        db(static_cast<std::uint8_t>(0xc0 | ((reg & 7) << 3) | (rm & 7)));
    }
    // Register/memory form. `imm_bytes` = immediate bytes that follow the
    // ModRM/displacement (needed for rip-relative operands).
    void emit_rm(std::uint8_t prefix, bool w, std::initializer_list<std::uint8_t> opcode,
                 unsigned reg, const Mem& m, unsigned imm_bytes = 0, bool byte_reg = false) {
        if (prefix != 0) db(prefix);
        const bool force = byte_reg && reg >= 4 && reg < 8;
        rex(w, reg, mem_index(m), mem_base(m), force);
        for (auto b : opcode) db(b);
        modrm_mem(reg, m, imm_bytes);
    }
    // Legacy SSE with optional REX.W and raw register numbers (GPR or XMM).
    void sse_raw(SseOp op, bool w, unsigned reg, unsigned rm) {
        if (op.prefix != 0) db(op.prefix);
        rex(w, reg, 0, rm, false);
        sse_opcode(op);
        db(static_cast<std::uint8_t>(0xc0 | ((reg & 7) << 3) | (rm & 7)));
    }
    void sse_raw(SseOp op, bool w, unsigned reg, const Mem& m, unsigned imm_bytes = 0) {
        if (op.prefix != 0) db(op.prefix);
        rex(w, reg, mem_index(m), mem_base(m), false);
        sse_opcode(op);
        modrm_mem(reg, m, imm_bytes);
    }
    // VEX forms with raw register numbers. `vvvv` is the extra source (0 if unused).
    void vex_rr(VexOp op, bool l256, unsigned reg, unsigned vvvv, unsigned rm) {
        vex_prefix(op, l256, reg, vvvv, 0, rm);
        db(op.opcode);
        db(static_cast<std::uint8_t>(0xc0 | ((reg & 7) << 3) | (rm & 7)));
    }
    void vex_rm(VexOp op, bool l256, unsigned reg, unsigned vvvv, const Mem& m,
                unsigned imm_bytes = 0) {
        vex_prefix(op, l256, reg, vvvv, mem_index(m), mem_base(m));
        db(op.opcode);
        modrm_mem(reg, m, imm_bytes);
    }

    // ---- integer moves -----------------------------------------------------
    void mov(unsigned bits, Gp dst, Gp src) {
        size_prefix(bits);
        emit_rr(0, bits == 64, {static_cast<std::uint8_t>(bits == 8 ? 0x88 : 0x89)}, id(src), id(dst), bits == 8);
    }
    void mov(unsigned bits, Gp dst, const Mem& src) {   // 8/16-bit loads merge; prefer movzx
        size_prefix(bits);
        emit_rm(0, bits == 64, {static_cast<std::uint8_t>(bits == 8 ? 0x8a : 0x8b)}, id(dst), src, 0, bits == 8);
    }
    void mov(unsigned bits, const Mem& dst, Gp src) {
        size_prefix(bits);
        emit_rm(0, bits == 64, {static_cast<std::uint8_t>(bits == 8 ? 0x88 : 0x89)}, id(src), dst, 0, bits == 8);
    }
    // Store an immediate (sign-extended to 64 bits for bits == 64).
    void mov(unsigned bits, const Mem& dst, std::int32_t imm) {
        size_prefix(bits);
        const unsigned imm_bytes = bits == 8 ? 1 : bits == 16 ? 2 : 4;
        emit_rm(0, bits == 64, {static_cast<std::uint8_t>(bits == 8 ? 0xc6 : 0xc7)}, 0, dst, imm_bytes);
        emit_imm(imm, imm_bytes);
    }
    // Shortest flag-preserving immediate load (never uses xor).
    void mov_imm(Gp dst, std::uint64_t value) {
        if (value <= 0xffffffffULL) {
            rex(false, 0, 0, id(dst), false);
            db(static_cast<std::uint8_t>(0xb8 + (id(dst) & 7)));
            dd(static_cast<std::uint32_t>(value));
        } else if (static_cast<std::int64_t>(value) >= INT32_MIN &&
                   static_cast<std::int64_t>(value) <= INT32_MAX) {
            emit_rr(0, true, {0xc7}, 0, id(dst));
            dd(static_cast<std::uint32_t>(value));
        } else {
            mov_imm64(dst, value);
        }
    }
    // Always the 10-byte movabs form. Returns the offset of the imm64 (fixup site).
    std::size_t mov_imm64(Gp dst, std::uint64_t value) {
        rex(true, 0, 0, id(dst), false);
        db(static_cast<std::uint8_t>(0xb8 + (id(dst) & 7)));
        const auto at = size_;
        dq(value);
        return at;
    }
    void movzx(unsigned dst_bits, Gp dst, Gp src, unsigned src_bits) {
        emit_rr(0, dst_bits == 64, {0x0f, static_cast<std::uint8_t>(src_bits == 8 ? 0xb6 : 0xb7)},
                id(dst), id(src), src_bits == 8);
    }
    void movzx(unsigned dst_bits, Gp dst, const Mem& src, unsigned src_bits) {
        emit_rm(0, dst_bits == 64, {0x0f, static_cast<std::uint8_t>(src_bits == 8 ? 0xb6 : 0xb7)},
                id(dst), src);
    }
    void movsx(unsigned dst_bits, Gp dst, Gp src, unsigned src_bits) {
        if (src_bits == 32) { emit_rr(0, true, {0x63}, id(dst), id(src)); return; }
        emit_rr(0, dst_bits == 64, {0x0f, static_cast<std::uint8_t>(src_bits == 8 ? 0xbe : 0xbf)},
                id(dst), id(src), src_bits == 8);
    }
    void movsx(unsigned dst_bits, Gp dst, const Mem& src, unsigned src_bits) {
        if (src_bits == 32) { emit_rm(0, true, {0x63}, id(dst), src); return; }
        emit_rm(0, dst_bits == 64, {0x0f, static_cast<std::uint8_t>(src_bits == 8 ? 0xbe : 0xbf)},
                id(dst), src);
    }
    void lea(unsigned bits, Gp dst, const Mem& m) {
        emit_rm(0, bits == 64, {0x8d}, id(dst), m);
    }
    void xchg(unsigned bits, Gp a, Gp b) {
        size_prefix(bits);
        emit_rr(0, bits == 64, {static_cast<std::uint8_t>(bits == 8 ? 0x86 : 0x87)}, id(a), id(b), bits == 8);
    }
    // Implicitly locked.
    void xchg(unsigned bits, const Mem& m, Gp r) {
        size_prefix(bits);
        emit_rm(0, bits == 64, {static_cast<std::uint8_t>(bits == 8 ? 0x86 : 0x87)}, id(r), m, 0, bits == 8);
    }
    void movbe(unsigned bits, Gp dst, const Mem& src) {
        size_prefix(bits);
        emit_rm(0, bits == 64, {0x0f, 0x38, 0xf0}, id(dst), src);
    }
    void movbe(unsigned bits, const Mem& dst, Gp src) {
        size_prefix(bits);
        emit_rm(0, bits == 64, {0x0f, 0x38, 0xf1}, id(src), dst);
    }

    // ---- integer arithmetic ------------------------------------------------
    void alu(Alu op, unsigned bits, Gp dst, Gp src) {
        size_prefix(bits);
        emit_rr(0, bits == 64, {static_cast<std::uint8_t>(static_cast<unsigned>(op) * 8 + (bits == 8 ? 0 : 1))},
                id(src), id(dst), bits == 8);
    }
    void alu(Alu op, unsigned bits, Gp dst, const Mem& src) {
        size_prefix(bits);
        emit_rm(0, bits == 64, {static_cast<std::uint8_t>(static_cast<unsigned>(op) * 8 + (bits == 8 ? 2 : 3))},
                id(dst), src, 0, bits == 8);
    }
    void alu(Alu op, unsigned bits, const Mem& dst, Gp src) {
        size_prefix(bits);
        emit_rm(0, bits == 64, {static_cast<std::uint8_t>(static_cast<unsigned>(op) * 8 + (bits == 8 ? 0 : 1))},
                id(src), dst, 0, bits == 8);
    }
    void alu(Alu op, unsigned bits, Gp dst, std::int32_t imm) {
        size_prefix(bits);
        const auto ext = static_cast<unsigned>(op);
        if (bits == 8) { emit_rr(0, false, {0x80}, ext, id(dst), true); db(static_cast<std::uint8_t>(imm)); return; }
        if (imm >= -128 && imm <= 127) { emit_rr(0, bits == 64, {0x83}, ext, id(dst)); db(static_cast<std::uint8_t>(imm)); return; }
        emit_rr(0, bits == 64, {0x81}, ext, id(dst));
        emit_imm(imm, bits == 16 ? 2 : 4);
    }
    void alu(Alu op, unsigned bits, const Mem& dst, std::int32_t imm) {
        size_prefix(bits);
        const auto ext = static_cast<unsigned>(op);
        if (bits == 8) { emit_rm(0, false, {0x80}, ext, dst, 1); db(static_cast<std::uint8_t>(imm)); return; }
        if (imm >= -128 && imm <= 127) { emit_rm(0, bits == 64, {0x83}, ext, dst, 1); db(static_cast<std::uint8_t>(imm)); return; }
        const unsigned n = bits == 16 ? 2 : 4;
        emit_rm(0, bits == 64, {0x81}, ext, dst, n);
        emit_imm(imm, n);
    }
    void add(unsigned bits, Gp d, Gp s) { alu(Alu::add, bits, d, s); }
    void add(unsigned bits, Gp d, std::int32_t i) { alu(Alu::add, bits, d, i); }
    void sub(unsigned bits, Gp d, Gp s) { alu(Alu::sub, bits, d, s); }
    void sub(unsigned bits, Gp d, std::int32_t i) { alu(Alu::sub, bits, d, i); }
    void and_(unsigned bits, Gp d, Gp s) { alu(Alu::and_, bits, d, s); }
    void and_(unsigned bits, Gp d, std::int32_t i) { alu(Alu::and_, bits, d, i); }
    void or_(unsigned bits, Gp d, Gp s) { alu(Alu::or_, bits, d, s); }
    void or_(unsigned bits, Gp d, std::int32_t i) { alu(Alu::or_, bits, d, i); }
    void xor_(unsigned bits, Gp d, Gp s) { alu(Alu::xor_, bits, d, s); }
    void xor_(unsigned bits, Gp d, std::int32_t i) { alu(Alu::xor_, bits, d, i); }
    void cmp(unsigned bits, Gp a, Gp b) { alu(Alu::cmp, bits, a, b); }
    void cmp(unsigned bits, Gp a, std::int32_t i) { alu(Alu::cmp, bits, a, i); }
    void cmp(unsigned bits, Gp a, const Mem& b) { alu(Alu::cmp, bits, a, b); }
    void cmp(unsigned bits, const Mem& a, std::int32_t i) { alu(Alu::cmp, bits, a, i); }
    void adc(unsigned bits, Gp d, Gp s) { alu(Alu::adc, bits, d, s); }
    void sbb(unsigned bits, Gp d, Gp s) { alu(Alu::sbb, bits, d, s); }

    void test(unsigned bits, Gp a, Gp b) {
        size_prefix(bits);
        emit_rr(0, bits == 64, {static_cast<std::uint8_t>(bits == 8 ? 0x84 : 0x85)}, id(b), id(a), bits == 8);
    }
    void test(unsigned bits, Gp a, std::int32_t imm) {
        size_prefix(bits);
        if (bits == 8) { emit_rr(0, false, {0xf6}, 0, id(a), true); db(static_cast<std::uint8_t>(imm)); return; }
        emit_rr(0, bits == 64, {0xf7}, 0, id(a));
        emit_imm(imm, bits == 16 ? 2 : 4);
    }
    void test(unsigned bits, const Mem& a, std::int32_t imm) {
        size_prefix(bits);
        const unsigned n = bits == 8 ? 1 : bits == 16 ? 2 : 4;
        emit_rm(0, bits == 64, {static_cast<std::uint8_t>(bits == 8 ? 0xf6 : 0xf7)}, 0, a, n);
        emit_imm(imm, n);
    }
    void unary(unsigned ext, unsigned bits, Gp r) {   // F7 /ext
        size_prefix(bits);
        emit_rr(0, bits == 64, {static_cast<std::uint8_t>(bits == 8 ? 0xf6 : 0xf7)}, ext, id(r), bits == 8);
    }
    void unary(unsigned ext, unsigned bits, const Mem& m) {
        size_prefix(bits);
        emit_rm(0, bits == 64, {static_cast<std::uint8_t>(bits == 8 ? 0xf6 : 0xf7)}, ext, m);
    }
    void not_(unsigned bits, Gp r) { unary(2, bits, r); }
    void neg(unsigned bits, Gp r) { unary(3, bits, r); }
    void mul(unsigned bits, Gp r) { unary(4, bits, r); }    // rdx:rax = rax * r (unsigned)
    void imul1(unsigned bits, Gp r) { unary(5, bits, r); }  // rdx:rax = rax * r (signed)
    void div(unsigned bits, Gp r) { unary(6, bits, r); }
    void idiv(unsigned bits, Gp r) { unary(7, bits, r); }
    void imul(unsigned bits, Gp dst, Gp src) {
        size_prefix(bits);
        emit_rr(0, bits == 64, {0x0f, 0xaf}, id(dst), id(src));
    }
    void imul(unsigned bits, Gp dst, const Mem& src) {
        size_prefix(bits);
        emit_rm(0, bits == 64, {0x0f, 0xaf}, id(dst), src);
    }
    void imul(unsigned bits, Gp dst, Gp src, std::int32_t imm) {
        size_prefix(bits);
        if (imm >= -128 && imm <= 127) { emit_rr(0, bits == 64, {0x6b}, id(dst), id(src)); db(static_cast<std::uint8_t>(imm)); return; }
        emit_rr(0, bits == 64, {0x69}, id(dst), id(src));
        emit_imm(imm, bits == 16 ? 2 : 4);
    }
    void cdq() { db(0x99); }
    void cqo() { db(0x48); db(0x99); }

    void shift(Shift op, unsigned bits, Gp r, std::uint8_t count) {
        size_prefix(bits);
        if (count == 1) { emit_rr(0, bits == 64, {static_cast<std::uint8_t>(bits == 8 ? 0xd0 : 0xd1)}, static_cast<unsigned>(op), id(r), bits == 8); return; }
        emit_rr(0, bits == 64, {static_cast<std::uint8_t>(bits == 8 ? 0xc0 : 0xc1)}, static_cast<unsigned>(op), id(r), bits == 8);
        db(count);
    }
    void shift_cl(Shift op, unsigned bits, Gp r) {
        size_prefix(bits);
        emit_rr(0, bits == 64, {static_cast<std::uint8_t>(bits == 8 ? 0xd2 : 0xd3)}, static_cast<unsigned>(op), id(r), bits == 8);
    }
    void shl(unsigned bits, Gp r, std::uint8_t n) { shift(Shift::shl, bits, r, n); }
    void shr(unsigned bits, Gp r, std::uint8_t n) { shift(Shift::shr, bits, r, n); }
    void sar(unsigned bits, Gp r, std::uint8_t n) { shift(Shift::sar, bits, r, n); }
    void rol(unsigned bits, Gp r, std::uint8_t n) { shift(Shift::rol, bits, r, n); }
    void ror(unsigned bits, Gp r, std::uint8_t n) { shift(Shift::ror, bits, r, n); }
    void shld(unsigned bits, Gp dst, Gp src, std::uint8_t n) {
        size_prefix(bits); emit_rr(0, bits == 64, {0x0f, 0xa4}, id(src), id(dst)); db(n);
    }
    void shrd(unsigned bits, Gp dst, Gp src, std::uint8_t n) {
        size_prefix(bits); emit_rr(0, bits == 64, {0x0f, 0xac}, id(src), id(dst)); db(n);
    }
    void shld_cl(unsigned bits, Gp dst, Gp src) { size_prefix(bits); emit_rr(0, bits == 64, {0x0f, 0xa5}, id(src), id(dst)); }
    void shrd_cl(unsigned bits, Gp dst, Gp src) { size_prefix(bits); emit_rr(0, bits == 64, {0x0f, 0xad}, id(src), id(dst)); }
    void bswap(unsigned bits, Gp r) {
        rex(bits == 64, 0, 0, id(r), false);
        db(0x0f); db(static_cast<std::uint8_t>(0xc8 + (id(r) & 7)));
    }
    // bt/bts/btr/btc with immediate: ext 4/5/6/7.
    void bt_imm(unsigned ext, unsigned bits, Gp r, std::uint8_t bit) {
        size_prefix(bits); emit_rr(0, bits == 64, {0x0f, 0xba}, ext, id(r)); db(bit);
    }
    void bt(unsigned bits, Gp r, std::uint8_t bit) { bt_imm(4, bits, r, bit); }
    void bts(unsigned bits, Gp r, std::uint8_t bit) { bt_imm(5, bits, r, bit); }
    void btr(unsigned bits, Gp r, std::uint8_t bit) { bt_imm(6, bits, r, bit); }
    void btc(unsigned bits, Gp r, std::uint8_t bit) { bt_imm(7, bits, r, bit); }
    void bt(unsigned bits, Gp r, Gp bit) { size_prefix(bits); emit_rr(0, bits == 64, {0x0f, 0xa3}, id(bit), id(r)); }

    void setcc(Cond c, Gp r8) {
        emit_rr(0, false, {0x0f, static_cast<std::uint8_t>(0x90 + static_cast<unsigned>(c))}, 0, id(r8), true);
    }
    void cmov(Cond c, unsigned bits, Gp dst, Gp src) {
        size_prefix(bits);
        emit_rr(0, bits == 64, {0x0f, static_cast<std::uint8_t>(0x40 + static_cast<unsigned>(c))}, id(dst), id(src));
    }
    void cmov(Cond c, unsigned bits, Gp dst, const Mem& src) {
        size_prefix(bits);
        emit_rm(0, bits == 64, {0x0f, static_cast<std::uint8_t>(0x40 + static_cast<unsigned>(c))}, id(dst), src);
    }

    // ---- atomics (emit lock() first where required) ------------------------
    void lock() { db(0xf0); }
    void cmpxchg(unsigned bits, const Mem& m, Gp src) {   // compares with rax/eax
        size_prefix(bits);
        emit_rm(0, bits == 64, {0x0f, static_cast<std::uint8_t>(bits == 8 ? 0xb0 : 0xb1)}, id(src), m, 0, bits == 8);
    }
    void xadd(unsigned bits, const Mem& m, Gp src) {
        size_prefix(bits);
        emit_rm(0, bits == 64, {0x0f, static_cast<std::uint8_t>(bits == 8 ? 0xc0 : 0xc1)}, id(src), m, 0, bits == 8);
    }
    void cmpxchg16b(const Mem& m) { emit_rm(0, true, {0x0f, 0xc7}, 1, m); }

    // ---- control flow ------------------------------------------------------
    // jmp/jcc/call to a label always use rel32 (patchable). Returns the
    // offset of the rel32 field.
    std::size_t jmp(Label target) { db(0xe9); ++control_flow_; return rel32_ref(target); }
    std::size_t jcc(Cond c, Label target) {
        ++control_flow_;
        db(0x0f); db(static_cast<std::uint8_t>(0x80 + static_cast<unsigned>(c)));
        return rel32_ref(target);
    }
    std::size_t call(Label target) { db(0xe8); return rel32_ref(target); }
    // Short forms; finalize() throws if the target is out of rel8 range.
    void jmp_short(Label target) { db(0xeb); ++control_flow_; rel8_ref(target); }
    void jcc_short(Cond c, Label target) { db(static_cast<std::uint8_t>(0x70 + static_cast<unsigned>(c))); ++control_flow_; rel8_ref(target); }
    void jrcxz_short(Label target) { db(0xe3); ++control_flow_; rel8_ref(target); }
    // rel32 to an absolute host address, resolved against `code_address`, the
    // final host address of byte 0 of this buffer (JIT-only, not relocatable).
    std::size_t jmp_abs(std::uint64_t code_address, const void* target) {
        db(0xe9); return rel32_abs(code_address, target);
    }
    std::size_t call_abs(std::uint64_t code_address, const void* target) {
        db(0xe8); return rel32_abs(code_address, target);
    }
    void jmp(Gp r) { emit_rr(0, false, {0xff}, 4, id(r)); }
    void jmp(const Mem& m) { emit_rm(0, false, {0xff}, 4, m); }
    void call(Gp r) { emit_rr(0, false, {0xff}, 2, id(r)); }
    void call(const Mem& m) { emit_rm(0, false, {0xff}, 2, m); }
    void ret() { db(0xc3); }
    void push(Gp r) { rex(false, 0, 0, id(r), false); db(static_cast<std::uint8_t>(0x50 + (id(r) & 7))); }
    void pop(Gp r) { rex(false, 0, 0, id(r), false); db(static_cast<std::uint8_t>(0x58 + (id(r) & 7))); }

    // ---- misc --------------------------------------------------------------
    void nop(unsigned count = 1) {
        static constexpr std::uint8_t forms[9][9] = {
            {}, {0x90}, {0x66, 0x90}, {0x0f, 0x1f, 0x00}, {0x0f, 0x1f, 0x40, 0x00},
            {0x0f, 0x1f, 0x44, 0x00, 0x00}, {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00},
            {0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00},
            {0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
        };
        while (count > 0) {
            const unsigned n = count > 8 ? 8 : count;
            for (unsigned i = 0; i < n; ++i) db(forms[n][i]);
            count -= n;
        }
    }
    void int3() { db(0xcc); }
    void ud2() { db(0x0f); db(0x0b); }
    void mfence() { db(0x0f); db(0xae); db(0xf0); }
    void lfence() { db(0x0f); db(0xae); db(0xe8); }
    void sfence() { db(0x0f); db(0xae); db(0xf8); }
    void pause() { db(0xf3); db(0x90); }
    void lahf() { db(0x9f); }
    void sahf() { db(0x9e); }
    void pushfq() { db(0x9c); }
    void popfq() { db(0x9d); }
    void clc() { db(0xf8); }
    void stc() { db(0xf9); }
    void cmc() { db(0xf5); }
    void cpuid() { db(0x0f); db(0xa2); }
    void rdtsc() { db(0x0f); db(0x31); }
    void ldmxcsr(const Mem& m) { emit_rm(0, false, {0x0f, 0xae}, 2, m); }
    void stmxcsr(const Mem& m) { emit_rm(0, false, {0x0f, 0xae}, 3, m); }

    // ---- BMI1/BMI2/LZCNT/POPCNT/CRC32 --------------------------------------
    void lzcnt(unsigned bits, Gp dst, Gp src) { size_prefix(bits); emit_rr(0xf3, bits == 64, {0x0f, 0xbd}, id(dst), id(src)); }
    void tzcnt(unsigned bits, Gp dst, Gp src) { size_prefix(bits); emit_rr(0xf3, bits == 64, {0x0f, 0xbc}, id(dst), id(src)); }
    void popcnt(unsigned bits, Gp dst, Gp src) { size_prefix(bits); emit_rr(0xf3, bits == 64, {0x0f, 0xb8}, id(dst), id(src)); }
    // crc32 dst32/64, src (src_bits 8/16/32/64). CRC32C polynomial only.
    void crc32(Gp dst, Gp src, unsigned src_bits) {
        if (src_bits == 16) db(0x66);
        emit_rr(0xf2, src_bits == 64, {0x0f, 0x38, static_cast<std::uint8_t>(src_bits == 8 ? 0xf0 : 0xf1)},
                id(dst), id(src), src_bits == 8);
    }
    void andn(unsigned bits, Gp dst, Gp src1, Gp src2) { vex_rr({0, 2, 0xf2, bits == 64}, false, id(dst), id(src1), id(src2)); }
    void bextr(unsigned bits, Gp dst, Gp src, Gp control) { vex_rr({0, 2, 0xf7, bits == 64}, false, id(dst), id(control), id(src)); }
    void bzhi(unsigned bits, Gp dst, Gp src, Gp index) { vex_rr({0, 2, 0xf5, bits == 64}, false, id(dst), id(index), id(src)); }
    void blsr(unsigned bits, Gp dst, Gp src) { vex_rr({0, 2, 0xf3, bits == 64}, false, 1, id(dst), id(src)); }
    void blsmsk(unsigned bits, Gp dst, Gp src) { vex_rr({0, 2, 0xf3, bits == 64}, false, 2, id(dst), id(src)); }
    void blsi(unsigned bits, Gp dst, Gp src) { vex_rr({0, 2, 0xf3, bits == 64}, false, 3, id(dst), id(src)); }
    void shlx(unsigned bits, Gp dst, Gp src, Gp count) { vex_rr({1, 2, 0xf7, bits == 64}, false, id(dst), id(count), id(src)); }
    void sarx(unsigned bits, Gp dst, Gp src, Gp count) { vex_rr({2, 2, 0xf7, bits == 64}, false, id(dst), id(count), id(src)); }
    void shrx(unsigned bits, Gp dst, Gp src, Gp count) { vex_rr({3, 2, 0xf7, bits == 64}, false, id(dst), id(count), id(src)); }
    void rorx(unsigned bits, Gp dst, Gp src, std::uint8_t imm) { vex_rr({3, 3, 0xf0, bits == 64}, false, id(dst), 0, id(src)); db(imm); }
    // hi:lo = rdx * src (unsigned), flags unchanged.
    void mulx(unsigned bits, Gp hi, Gp lo, Gp src) { vex_rr({3, 2, 0xf6, bits == 64}, false, id(hi), id(lo), id(src)); }
    void pdep(unsigned bits, Gp dst, Gp src, Gp mask) { vex_rr({3, 2, 0xf5, bits == 64}, false, id(dst), id(src), id(mask)); }
    void pext(unsigned bits, Gp dst, Gp src, Gp mask) { vex_rr({2, 2, 0xf5, bits == 64}, false, id(dst), id(src), id(mask)); }

    // ---- SSE ---------------------------------------------------------------
    void sse(SseOp op, Xmm dst, Xmm src) { sse_raw(op, false, id(dst), id(src)); }
    void sse(SseOp op, Xmm dst, const Mem& src) { sse_raw(op, false, id(dst), src); }
    void sse(SseOp op, const Mem& dst, Xmm src) { sse_raw(op, false, id(src), dst); }   // store forms
    void sse_imm(SseOp op, Xmm dst, Xmm src, std::uint8_t imm) { sse_raw(op, false, id(dst), id(src)); db(imm); }
    void sse_imm(SseOp op, Xmm dst, const Mem& src, std::uint8_t imm) { sse_raw(op, false, id(dst), src, 1); db(imm); }
    // Shift-by-immediate group (66 0F 71/72/73 /ext ib).
    void sse_shift_imm(std::uint8_t opcode, unsigned ext, Xmm r, std::uint8_t imm) {
        sse_raw({0x66, 1, opcode}, false, ext, id(r)); db(imm);
    }
    void movd(Xmm dst, Gp src) { sse_raw({0x66, 1, 0x6e}, false, id(dst), id(src)); }
    void movq(Xmm dst, Gp src) { sse_raw({0x66, 1, 0x6e}, true, id(dst), id(src)); }
    void movd(Gp dst, Xmm src) { sse_raw({0x66, 1, 0x7e}, false, id(src), id(dst)); }
    void movq(Gp dst, Xmm src) { sse_raw({0x66, 1, 0x7e}, true, id(src), id(dst)); }
    void movq(Xmm dst, Xmm src) { sse_raw({0xf3, 1, 0x7e}, false, id(dst), id(src)); }   // zeroes high
    void movq(Xmm dst, const Mem& src) { sse_raw({0xf3, 1, 0x7e}, false, id(dst), src); }
    void movq(const Mem& dst, Xmm src) { sse_raw({0x66, 1, 0xd6}, false, id(src), dst); }
    void movd(Xmm dst, const Mem& src) { sse_raw({0x66, 1, 0x6e}, false, id(dst), src); }
    void movd(const Mem& dst, Xmm src) { sse_raw({0x66, 1, 0x7e}, false, id(src), dst); }
    void movdqu(Xmm dst, const Mem& src) { sse_raw({0xf3, 1, 0x6f}, false, id(dst), src); }
    void movdqu(const Mem& dst, Xmm src) { sse_raw({0xf3, 1, 0x7f}, false, id(src), dst); }
    void movdqa(Xmm dst, const Mem& src) { sse_raw({0x66, 1, 0x6f}, false, id(dst), src); }
    void movdqa(const Mem& dst, Xmm src) { sse_raw({0x66, 1, 0x7f}, false, id(src), dst); }
    void movaps(Xmm dst, Xmm src) { sse_raw({0, 1, 0x28}, false, id(dst), id(src)); }
    void movups(Xmm dst, const Mem& src) { sse_raw({0, 1, 0x10}, false, id(dst), src); }
    void movups(const Mem& dst, Xmm src) { sse_raw({0, 1, 0x11}, false, id(src), dst); }
    void movss(Xmm dst, const Mem& src) { sse_raw({0xf3, 1, 0x10}, false, id(dst), src); }  // zeroes 127:32
    void movss(const Mem& dst, Xmm src) { sse_raw({0xf3, 1, 0x11}, false, id(src), dst); }
    void movss(Xmm dst, Xmm src) { sse_raw({0xf3, 1, 0x10}, false, id(dst), id(src)); }      // merges
    void movsd(Xmm dst, const Mem& src) { sse_raw({0xf2, 1, 0x10}, false, id(dst), src); }  // zeroes 127:64
    void movsd(const Mem& dst, Xmm src) { sse_raw({0xf2, 1, 0x11}, false, id(src), dst); }
    void movsd(Xmm dst, Xmm src) { sse_raw({0xf2, 1, 0x10}, false, id(dst), id(src)); }      // merges
    void pxor(Xmm dst, Xmm src) { sse_raw({0x66, 1, 0xef}, false, id(dst), id(src)); }
    // pinsr/pextr with a GPR (bits 8/16/32/64)
    void pinsr(unsigned bits, Xmm dst, Gp src, std::uint8_t lane) {
        if (bits == 16) { sse_raw({0x66, 1, 0xc4}, false, id(dst), id(src)); db(lane); return; }
        sse_raw({0x66, 3, static_cast<std::uint8_t>(bits == 8 ? 0x20 : 0x22)}, bits == 64, id(dst), id(src)); db(lane);
    }
    void pextr(unsigned bits, Gp dst, Xmm src, std::uint8_t lane) {
        if (bits == 16) { sse_raw({0x66, 1, 0xc5}, false, id(dst), id(src)); db(lane); return; }
        sse_raw({0x66, 3, static_cast<std::uint8_t>(bits == 8 ? 0x14 : 0x16)}, bits == 64, id(src), id(dst)); db(lane);
    }
    // cvtsi2ss/sd (src_bits 32/64), cvt(t)ss2si/sd2si (dst_bits 32/64)
    void cvtsi2ss(Xmm dst, Gp src, unsigned src_bits) { sse_raw({0xf3, 1, 0x2a}, src_bits == 64, id(dst), id(src)); }
    void cvtsi2sd(Xmm dst, Gp src, unsigned src_bits) { sse_raw({0xf2, 1, 0x2a}, src_bits == 64, id(dst), id(src)); }
    void cvttss2si(Gp dst, Xmm src, unsigned dst_bits) { sse_raw({0xf3, 1, 0x2c}, dst_bits == 64, id(dst), id(src)); }
    void cvttsd2si(Gp dst, Xmm src, unsigned dst_bits) { sse_raw({0xf2, 1, 0x2c}, dst_bits == 64, id(dst), id(src)); }
    void cvtss2si(Gp dst, Xmm src, unsigned dst_bits) { sse_raw({0xf3, 1, 0x2d}, dst_bits == 64, id(dst), id(src)); }
    void cvtsd2si(Gp dst, Xmm src, unsigned dst_bits) { sse_raw({0xf2, 1, 0x2d}, dst_bits == 64, id(dst), id(src)); }
    void movmskps(Gp dst, Xmm src) { sse_raw({0, 1, 0x50}, false, id(dst), id(src)); }
    void movmskpd(Gp dst, Xmm src) { sse_raw({0x66, 1, 0x50}, false, id(dst), id(src)); }
    void pmovmskb(Gp dst, Xmm src) { sse_raw({0x66, 1, 0xd7}, false, id(dst), id(src)); }

    // ---- VEX (AVX/AVX2/FMA3) ----------------------------------------------
    void vex(VexOp op, Xmm dst, Xmm src1, Xmm src2, bool l256 = false) { vex_rr(op, l256, id(dst), id(src1), id(src2)); }
    void vex(VexOp op, Xmm dst, Xmm src1, const Mem& src2, bool l256 = false) { vex_rm(op, l256, id(dst), id(src1), src2); }
    void vex_imm(VexOp op, Xmm dst, Xmm src1, Xmm src2, std::uint8_t imm, bool l256 = false) {
        vex_rr(op, l256, id(dst), id(src1), id(src2)); db(imm);
    }
    // 4-operand blends (vblendvps/vpblendvb): mask register in imm[7:4].
    void vex_is4(VexOp op, Xmm dst, Xmm src1, Xmm src2, Xmm mask, bool l256 = false) {
        vex_rr(op, l256, id(dst), id(src1), id(src2)); db(static_cast<std::uint8_t>(id(mask) << 4));
    }

    // ---- patching (on live code) --------------------------------------------
    // Rewrites the rel32 at `field` (4-byte aligned) to reach `target`. The
    // instruction ends at field + 4. Atomic with respect to executing threads.
    static void patch_rel32(void* field, const void* target) {
        const auto end = reinterpret_cast<std::intptr_t>(field) + 4;
        const auto value = reinterpret_cast<std::intptr_t>(target) - end;
        if (value < INT32_MIN || value > INT32_MAX) throw std::logic_error("patch_rel32 out of range");
        assert((reinterpret_cast<std::uintptr_t>(field) & 3) == 0);
        *static_cast<volatile std::int32_t*>(field) = static_cast<std::int32_t>(value);
    }

private:
    static constexpr std::size_t kUnbound = SIZE_MAX;
    struct Ref {
        std::size_t at;       // field offset
        std::size_t end;      // end of instruction (relative base)
        std::uint32_t label;
        std::int32_t addend;
        std::uint8_t width;   // 1 or 4
    };

    void emit_imm(std::int32_t imm, unsigned bytes) {
        if (bytes == 1) db(static_cast<std::uint8_t>(imm));
        else if (bytes == 2) dw(static_cast<std::uint16_t>(imm));
        else dd(static_cast<std::uint32_t>(imm));
    }
    void size_prefix(unsigned bits) { if (bits == 16) db(0x66); }
    static unsigned mem_base(const Mem& m) { return m.base == Gp::none ? 0 : id(m.base); }
    static unsigned mem_index(const Mem& m) { return m.index == Gp::none ? 0 : id(m.index); }
    void rex(bool w, unsigned reg, unsigned index, unsigned base, bool force) {
        const auto value = static_cast<std::uint8_t>(0x40 | (w ? 8 : 0) | (((reg >> 3) & 1) << 2) |
                                                     (((index >> 3) & 1) << 1) | ((base >> 3) & 1));
        if (value != 0x40 || force) db(value);
    }
    void sse_opcode(SseOp op) {
        db(0x0f);
        if (op.map == 2) db(0x38);
        else if (op.map == 3) db(0x3a);
        db(op.opcode);
    }
    void vex_prefix(VexOp op, bool l256, unsigned reg, unsigned vvvv, unsigned index, unsigned base) {
        const unsigned r = (~reg >> 3) & 1, x = (~index >> 3) & 1, b = (~base >> 3) & 1;
        const unsigned v = (~vvvv) & 15;
        if (op.map == 1 && !op.w && x == 1 && b == 1) {
            db(0xc5);
            db(static_cast<std::uint8_t>((r << 7) | (v << 3) | ((l256 ? 1 : 0) << 2) | op.pp));
        } else {
            db(0xc4);
            db(static_cast<std::uint8_t>((r << 7) | (x << 6) | (b << 5) | op.map));
            db(static_cast<std::uint8_t>(((op.w ? 1 : 0) << 7) | (v << 3) | ((l256 ? 1 : 0) << 2) | op.pp));
        }
    }
    static unsigned scale_bits(unsigned scale) {
        switch (scale) {
        case 1: return 0; case 2: return 1; case 4: return 2; case 8: return 3;
        default: throw std::logic_error("x64 scale must be 1, 2, 4 or 8");
        }
    }
    void modrm_mem(unsigned reg, const Mem& m, unsigned imm_bytes) {
        reg &= 7;
        if (m.label != kNoLabel) {
            db(static_cast<std::uint8_t>(0x05 | (reg << 3)));
            const auto at = size_;
            dd(0);
            refs_.push_back({at, at + 4 + imm_bytes, m.label, m.disp, 4});
            return;
        }
        if (m.index == Gp::rsp) throw std::logic_error("rsp cannot be an index register");
        if (m.base == Gp::none) {
            db(static_cast<std::uint8_t>(0x04 | (reg << 3)));
            const unsigned idx = m.index == Gp::none ? 4 : (id(m.index) & 7);
            db(static_cast<std::uint8_t>((scale_bits(m.index == Gp::none ? 1 : m.scale) << 6) | (idx << 3) | 5));
            dd(static_cast<std::uint32_t>(m.disp));
            return;
        }
        const unsigned b = id(m.base) & 7;
        const bool sib = m.index != Gp::none || b == 4;
        unsigned mod;
        if (m.disp == 0 && b != 5) mod = 0;
        else if (m.disp >= -128 && m.disp <= 127) mod = 1;
        else mod = 2;
        if (sib) {
            db(static_cast<std::uint8_t>((mod << 6) | (reg << 3) | 4));
            const unsigned idx = m.index == Gp::none ? 4 : (id(m.index) & 7);
            db(static_cast<std::uint8_t>((scale_bits(m.index == Gp::none ? 1 : m.scale) << 6) | (idx << 3) | b));
        } else {
            db(static_cast<std::uint8_t>((mod << 6) | (reg << 3) | b));
        }
        if (mod == 1) db(static_cast<std::uint8_t>(static_cast<std::int8_t>(m.disp)));
        else if (mod == 2) dd(static_cast<std::uint32_t>(m.disp));
    }
    std::size_t rel32_ref(Label target) {
        const auto at = size_;
        dd(0);
        refs_.push_back({at, at + 4, target.id, 0, 4});
        return at;
    }
    void rel8_ref(Label target) {
        const auto at = size_;
        db(0);
        refs_.push_back({at, at + 1, target.id, 0, 1});
    }
    std::size_t rel32_abs(std::uint64_t code_address, const void* target) {
        const auto at = size_;
        const auto end = static_cast<std::int64_t>(code_address + at + 4);
        const auto value = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(target)) - end;
        if (value < INT32_MIN || value > INT32_MAX) throw std::logic_error("x64 absolute rel32 out of range");
        dd(static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
        return at;
    }

    FX_X64_INLINE void append(const void* data, std::size_t n) {
        if (bytes_.size() - size_ < n) grow(n);
        std::memcpy(bytes_.data() + size_, data, n);
        size_ += n;
    }
    void grow(std::size_t n) {
        const std::size_t want = size_ + n + 256;
        bytes_.resize(want > bytes_.size() * 2 ? want : bytes_.size() * 2);
    }

    mutable std::vector<std::uint8_t> bytes_;   // [0, size_) is the code
    std::size_t size_{};
    std::vector<std::size_t> labels_;
    std::vector<Ref> refs_;
    std::size_t finalized_refs_{};
    std::uint32_t control_flow_{};
};

// Common legacy-SSE opcodes. Add more in your own files as SseOp constants.
namespace sse {
inline constexpr SseOp movaps{0, 1, 0x28}, movups{0, 1, 0x10}, movdqa{0x66, 1, 0x6f}, movdqu{0xf3, 1, 0x6f};
inline constexpr SseOp addss{0xf3, 1, 0x58}, addsd{0xf2, 1, 0x58}, addps{0, 1, 0x58}, addpd{0x66, 1, 0x58};
inline constexpr SseOp mulss{0xf3, 1, 0x59}, mulsd{0xf2, 1, 0x59}, mulps{0, 1, 0x59}, mulpd{0x66, 1, 0x59};
inline constexpr SseOp subss{0xf3, 1, 0x5c}, subsd{0xf2, 1, 0x5c}, subps{0, 1, 0x5c}, subpd{0x66, 1, 0x5c};
inline constexpr SseOp divss{0xf3, 1, 0x5e}, divsd{0xf2, 1, 0x5e}, divps{0, 1, 0x5e}, divpd{0x66, 1, 0x5e};
inline constexpr SseOp minss{0xf3, 1, 0x5d}, minsd{0xf2, 1, 0x5d}, minps{0, 1, 0x5d}, minpd{0x66, 1, 0x5d};
inline constexpr SseOp maxss{0xf3, 1, 0x5f}, maxsd{0xf2, 1, 0x5f}, maxps{0, 1, 0x5f}, maxpd{0x66, 1, 0x5f};
inline constexpr SseOp sqrtss{0xf3, 1, 0x51}, sqrtsd{0xf2, 1, 0x51}, sqrtps{0, 1, 0x51}, sqrtpd{0x66, 1, 0x51};
inline constexpr SseOp andps{0, 1, 0x54}, andnps{0, 1, 0x55}, orps{0, 1, 0x56}, xorps{0, 1, 0x57};
inline constexpr SseOp andpd{0x66, 1, 0x54}, andnpd{0x66, 1, 0x55}, orpd{0x66, 1, 0x56}, xorpd{0x66, 1, 0x57};
inline constexpr SseOp cvtss2sd{0xf3, 1, 0x5a}, cvtsd2ss{0xf2, 1, 0x5a}, cvtps2pd{0, 1, 0x5a}, cvtpd2ps{0x66, 1, 0x5a};
inline constexpr SseOp cvtdq2ps{0, 1, 0x5b}, cvttps2dq{0xf3, 1, 0x5b}, cvtps2dq{0x66, 1, 0x5b};
inline constexpr SseOp ucomiss{0, 1, 0x2e}, ucomisd{0x66, 1, 0x2e}, comiss{0, 1, 0x2f}, comisd{0x66, 1, 0x2f};
inline constexpr SseOp unpcklps{0, 1, 0x14}, unpckhps{0, 1, 0x15}, unpcklpd{0x66, 1, 0x14}, unpckhpd{0x66, 1, 0x15};
inline constexpr SseOp shufps{0, 1, 0xc6}, shufpd{0x66, 1, 0xc6};
inline constexpr SseOp cmpps{0, 1, 0xc2}, cmppd{0x66, 1, 0xc2}, cmpss{0xf3, 1, 0xc2}, cmpsd{0xf2, 1, 0xc2};
inline constexpr SseOp paddb{0x66, 1, 0xfc}, paddw{0x66, 1, 0xfd}, paddd{0x66, 1, 0xfe}, paddq{0x66, 1, 0xd4};
inline constexpr SseOp psubb{0x66, 1, 0xf8}, psubw{0x66, 1, 0xf9}, psubd{0x66, 1, 0xfa}, psubq{0x66, 1, 0xfb};
inline constexpr SseOp pand{0x66, 1, 0xdb}, pandn{0x66, 1, 0xdf}, por{0x66, 1, 0xeb}, pxor{0x66, 1, 0xef};
inline constexpr SseOp pcmpeqb{0x66, 1, 0x74}, pcmpeqw{0x66, 1, 0x75}, pcmpeqd{0x66, 1, 0x76}, pcmpeqq{0x66, 2, 0x29};
inline constexpr SseOp pcmpgtb{0x66, 1, 0x64}, pcmpgtw{0x66, 1, 0x65}, pcmpgtd{0x66, 1, 0x66}, pcmpgtq{0x66, 2, 0x37};
inline constexpr SseOp pmullw{0x66, 1, 0xd5}, pmulld{0x66, 2, 0x40}, pmuludq{0x66, 1, 0xf4}, pmulhw{0x66, 1, 0xe5}, pmulhuw{0x66, 1, 0xe4};
inline constexpr SseOp punpcklbw{0x66, 1, 0x60}, punpcklwd{0x66, 1, 0x61}, punpckldq{0x66, 1, 0x62}, punpcklqdq{0x66, 1, 0x6c};
inline constexpr SseOp punpckhbw{0x66, 1, 0x68}, punpckhwd{0x66, 1, 0x69}, punpckhdq{0x66, 1, 0x6a}, punpckhqdq{0x66, 1, 0x6d};
inline constexpr SseOp packsswb{0x66, 1, 0x63}, packuswb{0x66, 1, 0x67}, packssdw{0x66, 1, 0x6b}, packusdw{0x66, 2, 0x2b};
inline constexpr SseOp pshufd{0x66, 1, 0x70}, pshuflw{0xf2, 1, 0x70}, pshufhw{0xf3, 1, 0x70}, pshufb{0x66, 2, 0x00};
inline constexpr SseOp palignr{0x66, 3, 0x0f}, ptest{0x66, 2, 0x17}, pblendvb{0x66, 2, 0x10}, blendvps{0x66, 2, 0x14}, blendvpd{0x66, 2, 0x15};
inline constexpr SseOp roundps{0x66, 3, 0x08}, roundpd{0x66, 3, 0x09}, roundss{0x66, 3, 0x0a}, roundsd{0x66, 3, 0x0b};
inline constexpr SseOp blendps{0x66, 3, 0x0c}, blendpd{0x66, 3, 0x0d}, pblendw{0x66, 3, 0x0e}, insertps{0x66, 3, 0x21};
inline constexpr SseOp psrlw{0x66, 1, 0xd1}, psrld{0x66, 1, 0xd2}, psrlq{0x66, 1, 0xd3}, psraw{0x66, 1, 0xe1}, psrad{0x66, 1, 0xe2};
inline constexpr SseOp psllw{0x66, 1, 0xf1}, pslld{0x66, 1, 0xf2}, psllq{0x66, 1, 0xf3};
inline constexpr SseOp pclmulqdq{0x66, 3, 0x44}, aesenc{0x66, 2, 0xdc}, aesenclast{0x66, 2, 0xdd}, aesdec{0x66, 2, 0xde}, aesdeclast{0x66, 2, 0xdf}, aesimc{0x66, 2, 0xdb};
} // namespace sse

// Common VEX opcodes (128-bit unless l256). FMA3 "231" forms: dst += src1*src2.
namespace vex {
inline constexpr VexOp vfmadd231ps{1, 2, 0xb8, false}, vfmadd231pd{1, 2, 0xb8, true};
inline constexpr VexOp vfmadd231ss{1, 2, 0xb9, false}, vfmadd231sd{1, 2, 0xb9, true};
inline constexpr VexOp vfmsub231ps{1, 2, 0xba, false}, vfmsub231pd{1, 2, 0xba, true};
inline constexpr VexOp vfmsub231ss{1, 2, 0xbb, false}, vfmsub231sd{1, 2, 0xbb, true};
inline constexpr VexOp vfnmadd231ps{1, 2, 0xbc, false}, vfnmadd231pd{1, 2, 0xbc, true};
inline constexpr VexOp vfnmadd231ss{1, 2, 0xbd, false}, vfnmadd231sd{1, 2, 0xbd, true};
inline constexpr VexOp vfnmsub231ps{1, 2, 0xbe, false}, vfnmsub231pd{1, 2, 0xbe, true};
inline constexpr VexOp vfnmsub231ss{1, 2, 0xbf, false}, vfnmsub231sd{1, 2, 0xbf, true};
inline constexpr VexOp vaddps{0, 1, 0x58, false}, vmulps{0, 1, 0x59, false}, vsubps{0, 1, 0x5c, false};
inline constexpr VexOp vpaddd{1, 1, 0xfe, false}, vpxor{1, 1, 0xef, false}, vpand{1, 1, 0xdb, false}, vpor{1, 1, 0xeb, false};
inline constexpr VexOp vbroadcastss{1, 2, 0x18, false}, vpbroadcastb{1, 2, 0x78, false}, vpbroadcastw{1, 2, 0x79, false};
inline constexpr VexOp vpbroadcastd{1, 2, 0x58, false}, vpbroadcastq{1, 2, 0x59, false};
inline constexpr VexOp vpblendvb{1, 3, 0x4c, false}, vblendvps{1, 3, 0x4a, false}, vblendvpd{1, 3, 0x4b, false};
inline constexpr VexOp vpermilps_imm{1, 3, 0x04, false}, vpsllvd{1, 2, 0x47, false}, vpsllvq{1, 2, 0x47, true};
inline constexpr VexOp vpsrlvd{1, 2, 0x45, false}, vpsrlvq{1, 2, 0x45, true}, vpsravd{1, 2, 0x46, false};
} // namespace vex

} // namespace transmute::fx::x64
