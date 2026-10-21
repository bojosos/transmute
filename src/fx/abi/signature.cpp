#include "fx/abi/signature.hpp"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>

namespace transmute::fx::abi {
namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Signature parse_root() {
        Signature sig;
        if (s_ == "?") {
            sig.unknown = true;
            sig.ret.kind = TypeKind::i64;
            sig.text = "?";
            pos_ = 1;
            return sig;
        }
        sig = parse_signature_body();
        if (pos_ != s_.size()) fail("trailing characters");
        sig.text = std::string(s_);
        return sig;
    }

private:
    [[noreturn]] void fail(const char* what) const {
        throw std::invalid_argument(std::string("sigdb signature '") + std::string(s_) + "': " + what +
                                    " at " + std::to_string(pos_));
    }
    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    char take() {
        if (pos_ >= s_.size()) fail("unexpected end");
        return s_[pos_++];
    }
    std::uint32_t number() {
        if (!(peek() >= '0' && peek() <= '9')) fail("expected a number");
        std::uint32_t value = 0;
        while (peek() >= '0' && peek() <= '9') {
            value = value * 10 + static_cast<std::uint32_t>(take() - '0');
            if (value > (1u << 24)) fail("number too large");
        }
        return value;
    }

    Signature parse_signature_body() {
        Signature sig;
        sig.ret = parse_type(true);
        if (take() != '(') fail("expected '('");
        while (peek() != ')') {
            if (peek() == '.') {
                ++pos_;
                sig.variadic = true;
                if (peek() != ')') fail("'.' must be last");
                break;
            }
            sig.params.push_back(parse_type(false));
        }
        take();
        return sig;
    }

    TypeDesc parse_type(bool allow_void) {
        TypeDesc t;
        const char c = take();
        switch (c) {
        case 'v':
            if (!allow_void) fail("void parameter");
            t.kind = TypeKind::void_; t.size = 0; break;
        case 'c': t.kind = TypeKind::i8; t.size = 1; break;
        case 'C': t.kind = TypeKind::u8; t.size = 1; break;
        case 's': t.kind = TypeKind::i16; t.size = 2; break;
        case 'S': t.kind = TypeKind::u16; t.size = 2; break;
        case 'i': t.kind = TypeKind::i32; t.size = 4; break;
        case 'I': t.kind = TypeKind::u32; t.size = 4; break;
        case 'l': t.kind = TypeKind::i64; t.size = 8; break;
        case 'p': t.kind = TypeKind::ptr; t.size = 8; break;
        case 'f': t.kind = TypeKind::f32; t.size = 4; break;
        case 'd': t.kind = TypeKind::f64; t.size = 8; break;
        case '{': {
            if (peek() == 'f' || peek() == 'd') {
                t.kind = TypeKind::hfa;
                t.hfa_double = take() == 'd';
                const auto count = number();
                if (count < 1 || count > 4) fail("HFA member count must be 1..4");
                t.hfa_count = static_cast<std::uint8_t>(count);
                t.size = count * t.hfa_element_size();
                t.align = static_cast<std::uint8_t>(t.hfa_element_size());
            } else {
                t.kind = TypeKind::record;
                t.size = number();
                if (t.size == 0) fail("empty record");
                t.align = 8;
                if (peek() == ':') {
                    ++pos_;
                    const auto align = number();
                    if (align != 1 && align != 2 && align != 4 && align != 8 && align != 16)
                        fail("bad record alignment");
                    t.align = static_cast<std::uint8_t>(align);
                }
            }
            if (take() != '}') fail("expected '}'");
            break;
        }
        case '@': {
            const auto start = pos_;
            Parser nested(s_.substr(start));
            (void)nested.parse_signature_body();   // finds the end; interning re-parses
            pos_ = start + nested.pos_;
            t.kind = TypeKind::fnptr;
            t.size = 8;
            t.fn = intern_signature(s_.substr(start, pos_ - start));
            if (!t.fn) fail("bad nested signature");
            break;
        }
        default:
            fail("unknown type code");
        }
        return t;
    }

    std::string_view s_;
    std::size_t pos_{};
};

struct Interner {
    std::shared_mutex mutex;
    std::unordered_map<std::string, std::unique_ptr<Signature>> map;
};

Interner& interner() {
    static Interner* instance = new Interner();   // never destroyed: pointers handed out live forever
    return *instance;
}

} // namespace

Signature parse_signature(std::string_view text) {
    return Parser(text).parse_root();
}

const Signature* intern_signature(std::string_view text) {
    auto& in = interner();
    {
        std::shared_lock lock(in.mutex);
        const auto it = in.map.find(std::string(text));
        if (it != in.map.end()) return it->second.get();
    }
    std::unique_ptr<Signature> parsed;
    try {
        parsed = std::make_unique<Signature>(parse_signature(text));
    } catch (const std::invalid_argument&) {
        return nullptr;
    }
    std::unique_lock lock(in.mutex);
    auto [it, inserted] = in.map.emplace(std::string(text), std::move(parsed));
    return it->second.get();
}

const Signature* unknown_signature() {
    static const Signature* sig = intern_signature("?");
    return sig;
}

CallPlan plan_call(const Signature& sig) {
    CallPlan plan;
    plan.variadic = sig.variadic;
    plan.ret = sig.ret;
    switch (sig.ret.kind) {
    case TypeKind::void_: plan.ret_kind = ReturnKind::none; break;
    case TypeKind::f32:
    case TypeKind::f64: plan.ret_kind = ReturnKind::fp; break;
    case TypeKind::record:
        plan.ret_kind = sig.ret.size > 16 ? ReturnKind::record_memory : ReturnKind::record_regs;
        break;
    case TypeKind::hfa: plan.ret_kind = ReturnKind::hfa_regs; break;
    default: plan.ret_kind = ReturnKind::integer; break;
    }
    plan.win_hidden_return = sig.ret.is_aggregate() && !win_by_value_size(sig.ret.size);

    unsigned pos = plan.win_hidden_return ? 1u : 0u;
    unsigned ngrn = 0, nsrn = 0, nsaa = 0;   // AAPCS64 allocation state
    unsigned slot = 0;                       // variadic imaginary stack (8-byte slots)
    const auto round_up = [](unsigned v, unsigned a) { return (v + a - 1) & ~(a - 1); };
    const auto slot_piece = [](unsigned s) {
        return s < 8 ? ArmPiece{ArmPiece::gpr, static_cast<std::uint8_t>(s), 8, 0}
                     : ArmPiece{ArmPiece::stack, 0, 8, (s - 8) * 8};
    };
    const auto stack_pieces = [&](ArgPlan& a, unsigned size, unsigned align) {
        nsaa = round_up(nsaa, align >= 16 ? 16u : 8u);
        for (unsigned off = 0; off < size; off += 8) a.arm.push_back({ArmPiece::stack, 0, 8, nsaa + off});
        nsaa += round_up(size, 8);
    };

    for (const auto& t : sig.params) {
        ArgPlan a;
        a.type = t;
        a.win_pos = static_cast<std::uint8_t>(pos++);
        a.win_by_reference = t.is_aggregate() && !win_by_value_size(t.size);
        a.win_fp = t.is_fp();
        if (sig.variadic) {
            // Windows ARM64 variadic convention: every argument (named or not)
            // goes to the imaginary stack whose first 64 bytes are x0-x7. No
            // FP/SIMD registers, no special HFA treatment.
            if (t.is_aggregate() && t.size > 16) {
                a.arm_indirect = true;
                a.arm.push_back(slot_piece(slot++));
            } else if (t.is_aggregate()) {
                if (t.align >= 16) slot = round_up(slot, 2);
                for (unsigned w = 0; w < (t.size + 7) / 8; ++w) a.arm.push_back(slot_piece(slot++));
            } else {
                a.arm.push_back(slot_piece(slot++));
            }
        } else if (t.kind == TypeKind::hfa) {
            if (nsrn + t.hfa_count <= 8) {
                for (unsigned k = 0; k < t.hfa_count; ++k)
                    a.arm.push_back({ArmPiece::vreg, static_cast<std::uint8_t>(nsrn++),
                                     static_cast<std::uint8_t>(t.hfa_element_size()), 0});
            } else {
                nsrn = 8;
                stack_pieces(a, t.size, t.align);
            }
        } else if (t.is_fp()) {
            if (nsrn < 8) {
                a.arm.push_back({ArmPiece::vreg, static_cast<std::uint8_t>(nsrn++),
                                 static_cast<std::uint8_t>(t.size), 0});
            } else {
                a.arm.push_back({ArmPiece::stack, 0, 8, nsaa});
                nsaa += 8;
            }
        } else if (t.kind == TypeKind::record && t.size > 16) {
            a.arm_indirect = true;
            if (ngrn < 8) a.arm.push_back({ArmPiece::gpr, static_cast<std::uint8_t>(ngrn++), 8, 0});
            else { a.arm.push_back({ArmPiece::stack, 0, 8, nsaa}); nsaa += 8; }
        } else if (t.kind == TypeKind::record) {
            const unsigned words = (t.size + 7) / 8;
            if (t.align >= 16) ngrn = round_up(ngrn, 2);
            if (ngrn + words <= 8) {
                for (unsigned w = 0; w < words; ++w)
                    a.arm.push_back({ArmPiece::gpr, static_cast<std::uint8_t>(ngrn++), 8, 0});
            } else {
                ngrn = 8;
                stack_pieces(a, t.size, t.align);
            }
        } else {
            if (ngrn < 8) a.arm.push_back({ArmPiece::gpr, static_cast<std::uint8_t>(ngrn++), 8, 0});
            else { a.arm.push_back({ArmPiece::stack, 0, 8, nsaa}); nsaa += 8; }
        }
        plan.args.push_back(std::move(a));
    }
    plan.win_slots = pos;
    if (sig.variadic) {
        plan.arm_named_slots = slot;
        plan.arm_stack_bytes = slot > 8 ? (slot - 8) * 8 : 0;
    } else {
        plan.arm_stack_bytes = nsaa;
    }
    return plan;
}

} // namespace transmute::fx::abi
