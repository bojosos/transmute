#include "alchemy/darwin/foundation/invocation_core.hpp"

#include <algorithm>
#include <stdexcept>

namespace alchemy::gui::completion_kit::invocation {
namespace {

[[noreturn]] void malformed(std::string_view why) {
    throw std::runtime_error("Alchemy completion kit: Objective-C type encoding: " + std::string(why));
}

void skip_qualifiers(std::string_view& c) {
    while (!c.empty() && std::string_view("rnNoORVA").find(c.front()) != std::string_view::npos) c.remove_prefix(1);
}

void skip_offset(std::string_view& c) {
    while (!c.empty() && ((c.front() >= '0' && c.front() <= '9') || c.front() == '-' || c.front() == '+')) c.remove_prefix(1);
}

Type scalar(std::string encoding, Kind kind, std::size_t size, bool is_signed = false) {
    Type type;
    type.encoding = std::move(encoding);
    type.kind = kind;
    type.size = size;
    type.alignment = size ? size : 1;
    type.is_signed = is_signed;
    return type;
}

std::size_t round_up(std::size_t value, std::size_t alignment) { return (value + alignment - 1) / alignment * alignment; }

// Floating-point members of a composite flattened: {base size, count}, or
// {0, 0} when any member is not a float or double.
struct Flat { std::size_t member{}; unsigned count{}; bool valid{true}; };

Type parse(std::string_view& c, Flat* flat);

// Members of a struct or union up to `close`.
std::vector<Type> members(std::string_view& c, char close, Flat* flat) {
    std::vector<Type> result;
    while (!c.empty() && c.front() != close) {
        if (c.front() == '"') {   // a member name
            const auto end = c.find('"', 1);
            if (end == std::string_view::npos) malformed("unterminated member name");
            c.remove_prefix(end + 1);
            continue;
        }
        result.push_back(parse(c, flat));
    }
    if (c.empty()) malformed("unterminated aggregate");
    c.remove_prefix(1);
    return result;
}

void note_member(Flat* flat, const Type& type) {
    if (!flat || type.kind == Kind::composite) return;
    if (type.kind != Kind::floating) { flat->valid = false; return; }
    if (flat->member && flat->member != type.size) flat->valid = false;
    flat->member = type.size;
    ++flat->count;
}

Type parse(std::string_view& c, Flat* flat) {
    skip_qualifiers(c);
    if (c.empty()) malformed("missing type");
    const auto start = c;
    const char k = c.front();
    c.remove_prefix(1);
    Type type;
    switch (k) {
    case 'c': type = scalar("c", Kind::integer, 1, true); break;
    case 'C': type = scalar("C", Kind::integer, 1); break;
    case 's': type = scalar("s", Kind::integer, 2, true); break;
    case 'S': type = scalar("S", Kind::integer, 2); break;
    case 'i': type = scalar("i", Kind::integer, 4, true); break;
    case 'I': type = scalar("I", Kind::integer, 4); break;
    case 'l': type = scalar("l", Kind::integer, 4, true); break;   // 'l' is 32-bit in 64-bit programs
    case 'L': type = scalar("L", Kind::integer, 4); break;
    case 'q': type = scalar("q", Kind::integer, 8, true); break;
    case 'Q': type = scalar("Q", Kind::integer, 8); break;
    case 'B': type = scalar("B", Kind::integer, 1); break;
    case 'f': type = scalar("f", Kind::floating, 4); break;
    case 'd': case 'D': type = scalar(std::string(1, k), Kind::floating, 8); break;   // long double is double on arm64
    case 'v': type = scalar("v", Kind::void_type, 0); break;
    case '*': type = scalar("*", Kind::integer, 8); type.c_string = true; break;
    case '#': case ':': type = scalar(std::string(1, k), Kind::integer, 8); break;
    case '?': type = scalar("?", Kind::integer, 8); break;   // unknown (function) type behind a pointer
    case '@':
        type = scalar("@", Kind::integer, 8);
        type.object = true;
        if (!c.empty() && c.front() == '?') {
            c.remove_prefix(1);
            type.block = true;
            if (!c.empty() && c.front() == '<') {   // block signature <...>
                const auto end = c.find('>');
                if (end == std::string_view::npos) malformed("unterminated block signature");
                c.remove_prefix(end + 1);
            }
        } else if (!c.empty() && c.front() == '"') {
            const auto end = c.find('"', 1);
            // A quoted class name; an unterminated quote belongs to the next member name.
            if (end != std::string_view::npos) c.remove_prefix(end + 1);
        }
        break;
    case '^': {
        static_cast<void>(parse(c, nullptr));   // the pointee
        type = scalar("^", Kind::integer, 8);
        break;
    }
    case '{': case '(': {
        const char close = k == '{' ? '}' : ')';
        const auto name_end = c.find_first_of(std::string("=") + close);
        if (name_end == std::string_view::npos) malformed("unterminated aggregate name");
        const bool opaque = c[name_end] == close;
        c.remove_prefix(name_end + 1);
        type.kind = Kind::composite;
        Flat own;
        if (!opaque) {
            const auto list = members(c, close, &own);
            std::size_t size{}, alignment{1};
            for (const auto& member : list) {
                alignment = std::max(alignment, member.alignment);
                if (k == '{') size = round_up(size, member.alignment) + member.size;
                else size = std::max(size, member.size);
            }
            type.size = round_up(size, alignment);
            type.alignment = alignment;
            if (k == '(') own.valid = false;   // unions are never passed as HFAs here
        } else {
            own.valid = false;
        }
        if (own.valid && own.count >= 1 && own.count <= 4 && own.count * own.member == type.size) {
            type.hfa_count = own.count;
            type.hfa_member = own.member;
        }
        if (flat) {
            if (own.valid && own.count) { for (unsigned i = 0; i < own.count; ++i) note_member(flat, scalar("", Kind::floating, own.member)); }
            else flat->valid = false;
        }
        type.encoding.assign(start.substr(0, start.size() - c.size()));
        skip_offset(c);
        return type;
    }
    case '[': {
        std::size_t count{};
        while (!c.empty() && c.front() >= '0' && c.front() <= '9') { count = count * 10 + static_cast<std::size_t>(c.front() - '0'); c.remove_prefix(1); }
        Flat own;
        const auto element = parse(c, &own);
        if (c.empty() || c.front() != ']') malformed("unterminated array");
        c.remove_prefix(1);
        type.kind = Kind::composite;
        type.size = element.size * count;
        type.alignment = element.alignment;
        if (flat) {
            if (own.valid && own.count) for (std::size_t i = 0; i < count * own.count; ++i) note_member(flat, scalar("", Kind::floating, own.member));
            else flat->valid = false;
        }
        if (own.valid && own.count && count * own.count >= 1 && count * own.count <= 4) {
            type.hfa_count = static_cast<unsigned>(count * own.count);
            type.hfa_member = own.member;
        }
        type.encoding.assign(start.substr(0, start.size() - c.size()));
        skip_offset(c);
        return type;
    }
    case 'b': malformed("bitfields are not supported");
    default: malformed(std::string("unsupported type '") + k + "'");
    }
    note_member(flat, type);
    skip_offset(c);
    return type;
}

} // namespace

Type parse_type(std::string_view& cursor) { return parse(cursor, nullptr); }

Signature parse_signature(std::string_view types) {
    Signature signature;
    signature.types = std::string(types);
    auto cursor = types;
    signature.result = parse(cursor, nullptr);
    while (!cursor.empty()) signature.arguments.push_back(parse(cursor, nullptr));
    if (signature.arguments.size() < 2) malformed("a method signature needs self and _cmd: " + std::string(types));
    for (const auto& argument : signature.arguments)
        if (argument.kind == Kind::void_type) malformed("void argument in " + std::string(types));
    // Foundation's arm64 frame: the register save area (x0-x7 and q0-q7,
    // 192 bytes), the indirect result/link words (32 bytes), then the stack
    // arguments.
    signature.frame_length = 224 + assign(signature).stack_size;
    return signature;
}

Layout assign(const Signature& signature) {
    Layout layout;
    unsigned ngrn = 0, nsrn = 0;
    std::size_t nsaa = 0;
    const auto& result = signature.result;
    layout.indirect_result = result.kind == Kind::composite && !result.hfa_count && result.size > 16;
    const auto on_stack = [&](std::size_t size, std::size_t alignment) {
        nsaa = round_up(nsaa, alignment);
        const auto offset = nsaa;
        nsaa += size;
        return offset;
    };
    for (const auto& type : signature.arguments) {
        Location location;
        switch (type.kind) {
        case Kind::void_type:
            malformed("void argument");
        case Kind::integer:
            if (ngrn < 8) { location.where = Location::Where::x; location.first = ngrn++; location.count = 1; }
            else { location.where = Location::Where::stack; location.offset = on_stack(type.size, type.alignment); }
            break;
        case Kind::floating:
            if (nsrn < 8) { location.where = Location::Where::v; location.first = nsrn++; location.count = 1; }
            else { location.where = Location::Where::stack; location.offset = on_stack(type.size, type.alignment); }
            break;
        case Kind::composite:
            if (type.hfa_count) {
                if (nsrn + type.hfa_count <= 8) {
                    location.where = Location::Where::v; location.first = nsrn; location.count = type.hfa_count;
                    nsrn += type.hfa_count;
                } else {
                    nsrn = 8;
                    location.where = Location::Where::stack;
                    location.offset = on_stack(type.size, std::max<std::size_t>(8, type.alignment));
                }
            } else if (type.size > 16) {
                location.where = Location::Where::reference;
                if (ngrn < 8) { location.first = ngrn++; location.count = 1; }
                else { location.stack_pointer = true; location.offset = on_stack(8, 8); }
            } else {
                const auto needed = static_cast<unsigned>((type.size + 7) / 8);
                if (type.alignment == 16 && (ngrn & 1)) ++ngrn;
                if (needed && ngrn + needed <= 8) {
                    location.where = Location::Where::x; location.first = ngrn; location.count = needed;
                    ngrn += needed;
                } else {
                    ngrn = 8;
                    location.where = Location::Where::stack;
                    location.offset = on_stack(round_up(std::max<std::size_t>(type.size, 1), 8), 8);
                }
            }
            break;
        }
        layout.arguments.push_back(location);
    }
    layout.stack_size = round_up(nsaa, 16);
    return layout;
}

} // namespace alchemy::gui::completion_kit::invocation
