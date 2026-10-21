#include "llvm_ir.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_map>

// Textual LLVM IR parser (owner: A6). See llvm_ir.hpp.
namespace alchemy::msl::ir {

// ---- layout ------------------------------------------------------------------------------
namespace {
std::uint64_t next_power_of_two(std::uint64_t value) {
    std::uint64_t result = 1;
    while (result < value) result <<= 1;
    return result;
}
std::uint64_t round_up(std::uint64_t value, std::uint64_t alignment) {
    return alignment ? (value + alignment - 1) / alignment * alignment : value;
}
std::uint64_t scalar_bits(const Type* type) {
    switch (type->kind) {
    case Type::Kind::integer: return type->bits;
    case Type::Kind::half: case Type::Kind::bfloat: return 16;
    case Type::Kind::float_: return 32;
    case Type::Kind::double_: return 64;
    case Type::Kind::pointer: return 64;
    default: return 0;
    }
}
} // namespace

std::uint64_t align_of(const Type* type) {
    switch (type->kind) {
    case Type::Kind::integer: return type->bits <= 8 ? 1 : type->bits <= 16 ? 2 : type->bits <= 32 ? 4 : 8;
    case Type::Kind::vector: {
        const auto bits = scalar_bits(type->element) * type->count;
        return std::max<std::uint64_t>(1, next_power_of_two((bits + 7) / 8));
    }
    case Type::Kind::array: return align_of(type->element);
    case Type::Kind::structure: {
        if (type->packed) return 1;
        std::uint64_t alignment = 1;
        for (const auto* member : type->members) alignment = std::max(alignment, align_of(member));
        return alignment;
    }
    default: return std::max<std::uint64_t>(1, scalar_bits(type) / 8);
    }
}

std::uint64_t size_of(const Type* type) {
    switch (type->kind) {
    case Type::Kind::integer: return round_up((type->bits + 7) / 8, align_of(type));
    case Type::Kind::vector: return round_up((scalar_bits(type->element) * type->count + 7) / 8, align_of(type));
    case Type::Kind::array: return type->count * size_of(type->element);
    case Type::Kind::structure: {
        std::uint64_t offset = 0;
        for (const auto* member : type->members) {
            if (!type->packed) offset = round_up(offset, align_of(member));
            offset += size_of(member);
        }
        return round_up(offset, align_of(type));
    }
    default: return scalar_bits(type) / 8;
    }
}

std::uint64_t member_offset(const Type* structure, std::size_t member) {
    std::uint64_t offset = 0;
    for (std::size_t i = 0; i < structure->members.size(); ++i) {
        if (!structure->packed) offset = round_up(offset, align_of(structure->members[i]));
        if (i == member) return offset;
        offset += size_of(structure->members[i]);
    }
    return offset;
}

std::string to_string(const Type* type) {
    if (!type) return "<null>";
    switch (type->kind) {
    case Type::Kind::void_: return "void";
    case Type::Kind::integer: return "i" + std::to_string(type->bits);
    case Type::Kind::half: return "half";
    case Type::Kind::bfloat: return "bfloat";
    case Type::Kind::float_: return "float";
    case Type::Kind::double_: return "double";
    case Type::Kind::pointer: return type->address_space ? "ptr addrspace(" + std::to_string(type->address_space) + ")" : "ptr";
    case Type::Kind::vector: return "<" + std::to_string(type->count) + " x " + to_string(type->element) + ">";
    case Type::Kind::array: return "[" + std::to_string(type->count) + " x " + to_string(type->element) + "]";
    case Type::Kind::structure: {
        if (!type->name.empty()) return "%" + type->name;
        std::string result = type->packed ? "<{ " : "{ ";
        for (std::size_t i = 0; i < type->members.size(); ++i) result += (i ? ", " : "") + to_string(type->members[i]);
        return result + (type->packed ? " }>" : " }");
    }
    case Type::Kind::label: return "label";
    case Type::Kind::metadata: return "metadata";
    case Type::Kind::token: return "token";
    case Type::Kind::function: return "fn";
    }
    return "?";
}

bool is_terminator(Opcode op) {
    return op == Opcode::br || op == Opcode::switch_ || op == Opcode::ret || op == Opcode::unreachable;
}

std::string demangled_identifier(std::string_view mangled) {
    if (mangled.size() < 3 || mangled.substr(0, 2) != "_Z") return std::string(mangled);
    std::size_t i = 2;
    if (i < mangled.size() && mangled[i] == 'L') ++i;   // internal linkage
    std::size_t length = 0;
    while (i < mangled.size() && std::isdigit(static_cast<unsigned char>(mangled[i]))) length = length * 10 + (mangled[i++] - '0');
    if (!length || i + length > mangled.size()) return std::string(mangled);
    return std::string(mangled.substr(i, length));
}

// ---- module helpers ---------------------------------------------------------------------
Function* Module::function(std::string_view name) const {
    for (auto* f : functions) if (f->name == name) return f;
    return nullptr;
}
Global* Module::global(std::string_view name) const {
    for (auto* g : globals) if (g->name == name) return g;
    return nullptr;
}
const Type* Module::intern(Type type) {
    const auto key = to_string(&type) + (type.kind == Type::Kind::function ? std::to_string(types_.size()) : "");
    if (type.kind == Type::Kind::structure && !type.name.empty()) {
        // Named structures are owned by named_types.
    }
    const auto found = interned_.find(key);
    if (found != interned_.end() && type.kind != Type::Kind::function) return found->second;
    types_.push_back(std::move(type));
    interned_[key] = &types_.back();
    return &types_.back();
}
const Type* Module::integer(std::uint32_t bits) {
    Type type;
    type.kind = Type::Kind::integer;
    type.bits = bits;
    return intern(std::move(type));
}
const Type* Module::primitive(Type::Kind kind) {
    Type type;
    type.kind = kind;
    return intern(std::move(type));
}
const Type* Module::pointer(std::uint32_t address_space) {
    Type type;
    type.kind = Type::Kind::pointer;
    type.address_space = address_space;
    return intern(std::move(type));
}
const Type* Module::vector(const Type* element, std::uint64_t count) {
    Type type;
    type.kind = Type::Kind::vector;
    type.element = element;
    type.count = count;
    return intern(std::move(type));
}
const Type* Module::array(const Type* element, std::uint64_t count) {
    Type type;
    type.kind = Type::Kind::array;
    type.element = element;
    type.count = count;
    return intern(std::move(type));
}
Value* Module::constant_int(const Type* type, std::uint64_t value) {
    auto* v = make<Value>();
    v->kind = Value::Kind::constant_int;
    v->type = type;
    const auto bits = type->scalar()->bits;
    v->int_value = bits >= 64 ? value : value & ((1ULL << bits) - 1);
    return v;
}
Value* Module::constant_fp(const Type* type, double value) {
    auto* v = make<Value>();
    v->kind = Value::Kind::constant_fp;
    v->type = type;
    v->fp_value = value;
    return v;
}
Value* Module::special(Value::Kind kind, const Type* type) {
    auto* v = make<Value>();
    v->kind = kind;
    v->type = type;
    return v;
}

void Function::compute_cfg() {
    for (auto* block : blocks) {
        block->predecessors.clear();
        block->successors.clear();
    }
    for (auto* block : blocks) {
        if (block->instructions.empty()) continue;
        auto* last = block->instructions.back();
        for (auto* target : last->targets) {
            if (std::find(block->successors.begin(), block->successors.end(), target) == block->successors.end())
                block->successors.push_back(target);
        }
        for (auto* target : block->successors) target->predecessors.push_back(block);
    }
}

// ---- lexer -------------------------------------------------------------------------------
namespace {

struct Token {
    enum class Kind { end, newline, ident, local, global, meta, attrgroup, integer, fp, string, cstring, punct };
    Kind kind{Kind::end};
    std::string text;
    std::uint32_t line{};
};

class Lexer {
public:
    explicit Lexer(std::string_view text) : s_(text) {}

    std::vector<Token> run() {
        std::vector<Token> tokens;
        while (true) {
            auto token = next();
            const bool end = token.kind == Token::Kind::end;
            tokens.push_back(std::move(token));
            if (end) break;
        }
        return tokens;
    }

private:
    std::string_view s_;
    std::size_t i_{};
    std::uint32_t line_{1};

    static bool name_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$' || c == '-';
    }

    std::string quoted() {
        // At the opening quote.
        std::string out;
        ++i_;
        while (i_ < s_.size() && s_[i_] != '"') {
            if (s_[i_] == '\\' && i_ + 2 < s_.size() && std::isxdigit(static_cast<unsigned char>(s_[i_ + 1]))) {
                const char hex[3] = {s_[i_ + 1], s_[i_ + 2], 0};
                out += static_cast<char>(std::strtol(hex, nullptr, 16));
                i_ += 3;
                continue;
            }
            if (s_[i_] == '\\' && i_ + 1 < s_.size() && s_[i_ + 1] == '\\') {
                out += '\\';
                i_ += 2;
                continue;
            }
            out += s_[i_++];
        }
        ++i_;
        return out;
    }

    Token make(Token::Kind kind, std::string text) {
        Token token;
        token.kind = kind;
        token.text = std::move(text);
        token.line = line_;
        return token;
    }

    Token next() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ';') {
                while (i_ < s_.size() && s_[i_] != '\n') ++i_;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r') {
                ++i_;
                continue;
            }
            break;
        }
        if (i_ >= s_.size()) return make(Token::Kind::end, {});
        const char c = s_[i_];
        if (c == '\n') {
            auto token = make(Token::Kind::newline, "\n");
            ++line_;
            ++i_;
            return token;
        }
        if (c == '%' || c == '@' || c == '!' || c == '#') {
            const auto kind = c == '%' ? Token::Kind::local : c == '@' ? Token::Kind::global
                : c == '!' ? Token::Kind::meta : Token::Kind::attrgroup;
            ++i_;
            if (i_ < s_.size() && s_[i_] == '"') return make(kind, quoted());
            const auto begin = i_;
            while (i_ < s_.size() && name_char(s_[i_])) ++i_;
            if (kind == Token::Kind::meta && begin == i_ && i_ < s_.size() && s_[i_] == '{') return make(kind, "{");
            return make(kind, std::string(s_.substr(begin, i_ - begin)));
        }
        if (c == 'c' && i_ + 1 < s_.size() && s_[i_ + 1] == '"') {
            ++i_;
            return make(Token::Kind::cstring, quoted());
        }
        if (c == '"') return make(Token::Kind::string, quoted());
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            ((c == '-' || c == '+') && i_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_ + 1])))) {
            const auto begin = i_;
            if (c == '-' || c == '+') ++i_;
            if (s_.substr(i_, 2) == "0x") {
                i_ += 2;
                while (i_ < s_.size() && std::isalnum(static_cast<unsigned char>(s_[i_]))) ++i_;
                return make(Token::Kind::fp, std::string(s_.substr(begin, i_ - begin)));
            }
            bool real = false;
            while (i_ < s_.size()) {
                const char d = s_[i_];
                if (std::isdigit(static_cast<unsigned char>(d))) { ++i_; continue; }
                if (d == '.' || d == 'e' || d == 'E') { real = true; ++i_; continue; }
                if ((d == '+' || d == '-') && (s_[i_ - 1] == 'e' || s_[i_ - 1] == 'E')) { ++i_; continue; }
                break;
            }
            return make(real ? Token::Kind::fp : Token::Kind::integer, std::string(s_.substr(begin, i_ - begin)));
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '.') {
            const auto begin = i_;
            while (i_ < s_.size() && (name_char(s_[i_]) && s_[i_] != '-')) ++i_;
            return make(Token::Kind::ident, std::string(s_.substr(begin, i_ - begin)));
        }
        if (s_.substr(i_, 3) == "...") {
            i_ += 3;
            return make(Token::Kind::punct, "...");
        }
        ++i_;
        return make(Token::Kind::punct, std::string(1, c));
    }
};

// ---- parser ---------------------------------------------------------------------------
double parse_float(const std::string& text, const Type* type) {
    if (text.size() > 2 && (text[0] == '0' || text[1] == '0') && text.find("0x") != std::string::npos) {
        const auto at = text.find("0x");
        std::string digits = text.substr(at + 2);
        char form = 0;
        if (!digits.empty() && std::isalpha(static_cast<unsigned char>(digits[0])) && !std::isxdigit(static_cast<unsigned char>(digits[0]))) {
            form = digits[0];
            digits = digits.substr(1);
        } else if (!digits.empty() && (digits[0] == 'H' || digits[0] == 'R' || digits[0] == 'K' || digits[0] == 'L' || digits[0] == 'M')) {
            form = digits[0];
            digits = digits.substr(1);
        }
        std::uint64_t bits = 0;
        std::from_chars(digits.data(), digits.data() + digits.size(), bits, 16);
        double value = 0;
        if (form == 'H') {
            // IEEE half.
            const auto sign = (bits >> 15) & 1, exponent = (bits >> 10) & 0x1f, mantissa = bits & 0x3ff;
            if (exponent == 0) value = std::ldexp(static_cast<double>(mantissa), -24);
            else if (exponent == 31) value = mantissa ? NAN : INFINITY;
            else value = std::ldexp(static_cast<double>(mantissa | 0x400), static_cast<int>(exponent) - 25);
            if (sign) value = -value;
        } else if (form == 'R') {
            const auto widened = static_cast<std::uint32_t>(bits << 16);
            float f;
            std::memcpy(&f, &widened, 4);
            value = f;
        } else if (form == 0) {
            // Doubles (and floats/halves printed as the double with the same value).
            std::memcpy(&value, &bits, 8);
        } else {
            throw ParseError("unsupported floating-point literal " + text);
        }
        if (text[0] == '-') value = -value;
        (void)type;
        return value;
    }
    return std::strtod(text.c_str(), nullptr);
}

class Parser {
public:
    Parser(std::string_view text, Module& module) : tokens_(Lexer(text).run()), m_(module) {}

    void run() {
        prescan();
        while (!at_end()) {
            skip_newlines();
            if (at_end()) break;
            const auto& t = peek();
            if (t.kind == Token::Kind::local && peek(1).text == "=" && peek(2).text == "type") {
                named_type();
            } else if (t.kind == Token::Kind::global && peek(1).text == "=") {
                global_variable();
            } else if (t.kind == Token::Kind::ident && (t.text == "define" || t.text == "declare")) {
                function();
            } else {
                skip_line();
            }
        }
        for (auto* f : m_.functions) {
            if (f->kind == Value::Kind::placeholder) f->kind = Value::Kind::function, f->declaration = true;
        }
    }

private:
    std::vector<Token> tokens_;
    std::size_t p_{};
    Module& m_;
    std::unordered_map<std::string, Value*> globals_;
    std::unordered_map<std::string, Value*> locals_;
    std::unordered_map<std::string, BasicBlock*> blocks_;
    Function* function_{};
    std::uint32_t ordinal_{};
    std::uint64_t last_align_{};

    [[noreturn]] void fail(const std::string& message) const {
        throw ParseError("LLVM IR line " + std::to_string(peek().line) + ": " + message + " (at '" + peek().text + "')");
    }
    const Token& peek(std::size_t ahead = 0) const {
        return tokens_[std::min(tokens_.size() - 1, p_ + ahead)];
    }
    bool at_end() const { return peek().kind == Token::Kind::end; }
    Token take() {
        auto token = peek();
        if (!at_end()) ++p_;
        return token;
    }
    bool accept(std::string_view text) {
        if (peek().text == text && peek().kind != Token::Kind::string && peek().kind != Token::Kind::local &&
            peek().kind != Token::Kind::global) {
            ++p_;
            return true;
        }
        return false;
    }
    void expect(std::string_view text) {
        if (!accept(text)) fail("expected '" + std::string(text) + "'");
    }
    void skip_newlines() {
        while (peek().kind == Token::Kind::newline) ++p_;
    }
    void skip_line() {
        while (!at_end() && peek().kind != Token::Kind::newline) ++p_;
    }
    void skip_parens() {
        if (!accept("(")) return;
        int depth = 1;
        while (!at_end() && depth) {
            if (peek().text == "(") ++depth;
            if (peek().text == ")") --depth;
            ++p_;
        }
    }
    std::uint64_t integer() {
        if (peek().kind != Token::Kind::integer) fail("expected an integer");
        const auto text = take().text;
        if (!text.empty() && text[0] == '-') return static_cast<std::uint64_t>(std::stoll(text));
        return std::stoull(text);
    }

    void prescan() {
        // Create every function and global up front so uses may precede definitions.
        for (std::size_t i = 0; i + 1 < tokens_.size(); ++i) {
            const bool line_start = i == 0 || tokens_[i - 1].kind == Token::Kind::newline;
            if (!line_start) continue;
            if (tokens_[i].kind == Token::Kind::global && tokens_[i + 1].text == "=") {
                auto* g = m_.make<Global>();
                g->kind = Value::Kind::global;
                g->name = tokens_[i].text;
                m_.globals.push_back(g);
                globals_[g->name] = g;
            } else if (tokens_[i].kind == Token::Kind::ident && (tokens_[i].text == "define" || tokens_[i].text == "declare")) {
                for (auto k = i + 1; k < tokens_.size() && tokens_[k].kind != Token::Kind::newline; ++k) {
                    if (tokens_[k].kind == Token::Kind::global && tokens_[k + 1].text == "(") {
                        auto* f = m_.make<Function>();
                        f->kind = Value::Kind::placeholder;
                        f->name = tokens_[k].text;
                        m_.functions.push_back(f);
                        globals_[f->name] = f;
                        break;
                    }
                }
            }
        }
    }

    // ---- types ------------------------------------------------------------------------
    bool at_type() const {
        const auto& t = peek();
        if (t.kind == Token::Kind::local) return true;   // named struct
        if (t.kind == Token::Kind::punct) return t.text == "<" || t.text == "[" || t.text == "{";
        if (t.kind != Token::Kind::ident) return false;
        static const std::set<std::string_view> names = {"void", "half", "bfloat", "float", "double", "ptr",
            "label", "metadata", "token", "x86_fp80", "fp128"};
        if (names.count(t.text)) return true;
        return t.text.size() > 1 && t.text[0] == 'i' && std::all_of(t.text.begin() + 1, t.text.end(), [](char c) {
                   return std::isdigit(static_cast<unsigned char>(c)); });
    }

    const Type* type() {
        const auto t = take();
        const Type* result = nullptr;
        if (t.kind == Token::Kind::local) {
            result = named(t.text);
        } else if (t.text == "void") result = m_.primitive(Type::Kind::void_);
        else if (t.text == "half") result = m_.primitive(Type::Kind::half);
        else if (t.text == "bfloat") result = m_.primitive(Type::Kind::bfloat);
        else if (t.text == "float") result = m_.primitive(Type::Kind::float_);
        else if (t.text == "double") result = m_.primitive(Type::Kind::double_);
        else if (t.text == "label") result = m_.primitive(Type::Kind::label);
        else if (t.text == "metadata") result = m_.primitive(Type::Kind::metadata);
        else if (t.text == "token") result = m_.primitive(Type::Kind::token);
        else if (t.text == "ptr") {
            std::uint32_t space = 0;
            if (peek().text == "addrspace") {
                take();
                expect("(");
                space = static_cast<std::uint32_t>(integer());
                expect(")");
            }
            result = m_.pointer(space);
        } else if (t.kind == Token::Kind::ident && t.text[0] == 'i') {
            result = m_.integer(static_cast<std::uint32_t>(std::stoul(t.text.substr(1))));
        } else if (t.text == "<") {
            if (accept("{")) {
                result = struct_body(true);
                expect(">");
            } else {
                const auto count = integer();
                expect("x");
                const auto* element = type();
                expect(">");
                result = m_.vector(element, count);
            }
        } else if (t.text == "[") {
            const auto count = integer();
            expect("x");
            const auto* element = type();
            expect("]");
            result = m_.array(element, count);
        } else if (t.text == "{") {
            result = struct_body(false);
        } else {
            --p_;
            fail("unsupported type");
        }
        // Function types (only in call syntax for variadic callees).
        if (peek().text == "(" && result) {
            Type fn;
            fn.kind = Type::Kind::function;
            fn.members.push_back(result);
            take();
            while (!accept(")")) {
                if (accept("...")) continue;
                fn.members.push_back(type());
                accept(",");
            }
            result = m_.intern(std::move(fn));
        }
        return result;
    }
    Type* named(const std::string& name) {
        auto& slot = m_.named_types[name];
        if (!slot) {
            slot = &m_.named_storage.emplace_back();
            slot->kind = Type::Kind::structure;
            slot->name = name;
        }
        return slot;
    }

    const Type* struct_body(bool packed) {
        Type s;
        s.kind = Type::Kind::structure;
        s.packed = packed;
        while (!accept("}")) {
            s.members.push_back(type());
            accept(",");
        }
        return m_.intern(std::move(s));
    }

    void named_type() {
        const auto name = take().text;
        expect("=");
        expect("type");
        auto* slot = named(name);
        if (accept("opaque")) {
            skip_line();
            return;
        }
        bool packed = false;
        if (accept("<")) packed = true;
        expect("{");
        slot->packed = packed;
        slot->members.clear();
        while (!accept("}")) {
            slot->members.push_back(type());
            accept(",");
        }
        if (packed) expect(">");
        skip_line();
    }

    // ---- constants and values ---------------------------------------------------------
    Value* value(const Type* type) {
        const auto t = peek();
        switch (t.kind) {
        case Token::Kind::local: {
            take();
            auto& slot = locals_[t.text];
            if (!slot) {
                slot = m_.make<Value>();
                slot->kind = Value::Kind::placeholder;
                slot->name = t.text;
                slot->type = type;
            }
            return slot;
        }
        case Token::Kind::global: {
            take();
            const auto found = globals_.find(t.text);
            if (found == globals_.end()) fail("unknown global @" + t.text);
            return found->second;
        }
        case Token::Kind::integer: {
            take();
            const auto text = t.text;
            if (type && type->scalar()->is_float()) return m_.constant_fp(type, std::strtod(text.c_str(), nullptr));
            const std::uint64_t bits = !text.empty() && text[0] == '-' ? static_cast<std::uint64_t>(std::stoll(text))
                                                                         : std::stoull(text);
            return m_.constant_int(type, bits);
        }
        case Token::Kind::fp: {
            take();
            return m_.constant_fp(type, parse_float(t.text, type));
        }
        case Token::Kind::cstring: {
            take();
            auto* v = m_.make<Value>();
            v->kind = Value::Kind::constant_aggregate;
            v->type = type;
            const auto* i8 = m_.integer(8);
            for (const auto c : t.text) v->operands.push_back(m_.constant_int(i8, static_cast<unsigned char>(c)));
            return v;
        }
        default: break;
        }
        if (t.text == "true" || t.text == "false") {
            take();
            return m_.constant_int(type, t.text == "true" ? 1 : 0);
        }
        if (t.text == "null" || t.text == "zeroinitializer") {
            take();
            return m_.special(Value::Kind::constant_null, type);
        }
        if (t.text == "undef") { take(); return m_.special(Value::Kind::constant_undef, type); }
        if (t.text == "poison") { take(); return m_.special(Value::Kind::constant_poison, type); }
        if (t.text == "splat") {
            take();
            expect("(");
            const auto* element = this->type();
            auto* scalar = value(element);
            expect(")");
            auto* v = m_.make<Value>();
            v->kind = Value::Kind::constant_aggregate;
            v->type = type;
            for (std::uint64_t i = 0; i < (type ? type->count : 1); ++i) v->operands.push_back(scalar);
            return v;
        }
        if (t.text == "<" || t.text == "[" || t.text == "{") {
            take();
            const bool packed_struct = t.text == "<" && peek().text == "{";
            if (packed_struct) take();
            const std::string close = t.text == "<" ? (packed_struct ? "}" : ">") : t.text == "[" ? "]" : "}";
            auto* v = m_.make<Value>();
            v->kind = Value::Kind::constant_aggregate;
            v->type = type;
            while (!accept(close)) {
                const auto* element = this->type();
                v->operands.push_back(value(element));
                accept(",");
            }
            if (packed_struct) expect(">");
            return v;
        }
        if (t.kind == Token::Kind::ident) {
            static const std::set<std::string_view> casts = {"bitcast", "addrspacecast", "ptrtoint", "inttoptr",
                "trunc", "zext", "sext", "getelementptr"};
            if (casts.count(t.text)) return constant_expression(type);
            if (t.text == "asm") fail("inline assembly is unsupported");
        }
        fail("unsupported value");
    }

    Value* constant_expression(const Type* type) {
        const auto name = take().text;
        auto* v = m_.make<Value>();
        v->kind = Value::Kind::constant_expr;
        v->type = type;
        if (name == "getelementptr") {
            v->op = Opcode::getelementptr;
            while (peek().text == "inbounds" || peek().text == "nuw" || peek().text == "nusw" || peek().text == "inrange") {
                if (take().text == "inrange") skip_parens();
            }
            expect("(");
            v->aux_type = this->type();
            expect(",");
            while (!accept(")")) {
                const auto* operand_type = this->type();
                v->operands.push_back(value(operand_type));
                accept(",");
            }
            return v;
        }
        static const std::map<std::string_view, Opcode> ops = {{"bitcast", Opcode::bitcast},
            {"addrspacecast", Opcode::addrspacecast}, {"ptrtoint", Opcode::ptrtoint}, {"inttoptr", Opcode::inttoptr},
            {"trunc", Opcode::trunc}, {"zext", Opcode::zext}, {"sext", Opcode::sext}};
        v->op = ops.at(name);
        expect("(");
        const auto* source = this->type();
        v->operands.push_back(value(source));
        expect("to");
        v->aux_type = this->type();
        expect(")");
        return v;
    }

    // `<type> [attributes] <value>` in argument lists.
    bool at_value() const {
        const auto& t = peek();
        switch (t.kind) {
        case Token::Kind::local: case Token::Kind::global: case Token::Kind::integer: case Token::Kind::fp:
        case Token::Kind::cstring: return true;
        case Token::Kind::punct: return t.text == "<" || t.text == "[" || t.text == "{";
        case Token::Kind::ident: {
            static const std::set<std::string_view> names = {"true", "false", "null", "zeroinitializer", "undef",
                "poison", "splat", "getelementptr", "bitcast", "addrspacecast", "ptrtoint", "inttoptr"};
            return names.count(t.text) != 0;
        }
        default: return false;
        }
    }
    void skip_attributes() {
        while (peek().kind == Token::Kind::ident && !at_value()) {
            const auto word = take().text;
            if (word == "align" || word == "dereferenceable" || word == "addrspace") {
                if (peek().text == "(") skip_parens();
                else if (peek().kind == Token::Kind::integer) {
                    const auto amount = take().text;
                    if (word == "align") last_align_ = std::stoull(amount);
                }
                continue;
            }
            if (peek().text == "(") skip_parens();
        }
    }
    Value* typed_value() {
        const auto* t = type();
        skip_attributes();
        return value(t);
    }

    // ---- globals ---------------------------------------------------------------------------
    void global_variable() {
        const auto name = take().text;
        expect("=");
        auto* g = static_cast<Global*>(globals_.at(name));
        bool external = false;
        while (true) {
            const auto& t = peek();
            if (t.text == "global" || t.text == "constant") break;
            if (t.kind != Token::Kind::ident) fail("unexpected token in global declaration");
            if (t.text == "external" || t.text == "extern_weak") external = true;
            if (t.text == "addrspace") {
                take();
                expect("(");
                g->address_space = static_cast<std::uint32_t>(integer());
                expect(")");
                continue;
            }
            if (t.text == "alias" || t.text == "ifunc") fail("global aliases are unsupported");
            take();
            if (peek().text == "(") skip_parens();
        }
        g->constant = take().text == "constant";
        g->value_type = type();
        g->type = m_.pointer(g->address_space);
        if (!external && peek().kind != Token::Kind::newline && peek().text != ",") g->initializer = value(g->value_type);
        while (accept(",")) {
            if (accept("align")) g->alignment = integer();
            else skip_line_items();
        }
        skip_line();
    }
    void skip_line_items() {
        // Section, comdat, metadata attachments: skip to the next ',' or newline.
        while (!at_end() && peek().kind != Token::Kind::newline && peek().text != ",") {
            if (peek().text == "(") skip_parens();
            else take();
        }
    }

    // ---- functions -----------------------------------------------------------------------
    void function() {
        const bool definition = take().text == "define";
        // Linkage, visibility, calling convention, return attributes: identifiers before the type.
        while (!at_type() || (peek().kind == Token::Kind::ident && peek().text.rfind("i", 0) != 0 &&
                              (peek().text == "noundef" || peek().text == "zeroext"))) {
            if (peek().kind == Token::Kind::newline || at_end()) fail("malformed function header");
            const auto word = take().text;
            if (word == "align" && peek().kind == Token::Kind::integer) take();
            if (peek().text == "(") skip_parens();
        }
        const auto* return_type = type();
        while (peek().kind != Token::Kind::global) {
            if (peek().text == "(") skip_parens();
            else take();
        }
        const auto name = take().text;
        auto* f = static_cast<Function*>(globals_.at(name));
        f->kind = Value::Kind::function;
        f->return_type = return_type;
        f->type = m_.pointer(0);
        f->declaration = !definition;
        locals_.clear();
        blocks_.clear();
        function_ = f;
        ordinal_ = 0;
        expect("(");
        std::uint32_t unnamed = 0;
        while (!accept(")")) {
            if (accept("...")) continue;
            const auto* t = type();
            // Parameter attributes then an optional name.
            while (peek().kind == Token::Kind::ident) {
                const auto word = take().text;
                if ((word == "align" || word == "dereferenceable") && peek().kind == Token::Kind::integer) take();
                if (peek().text == "(") skip_parens();
            }
            auto* argument = m_.make<Value>();
            argument->kind = Value::Kind::argument;
            argument->type = t;
            argument->index = static_cast<std::uint32_t>(f->arguments.size());
            if (peek().kind == Token::Kind::local) {
                argument->name = take().text;
            } else {
                argument->name = std::to_string(unnamed);
            }
            if (std::all_of(argument->name.begin(), argument->name.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
                unnamed = static_cast<std::uint32_t>(std::stoul(argument->name)) + 1;
            locals_[argument->name] = argument;
            f->arguments.push_back(argument);
            accept(",");
        }
        if (!definition) {
            skip_line();
            return;
        }
        while (!accept("{")) {
            if (at_end() || peek().kind == Token::Kind::newline) fail("expected a function body");
            if (peek().text == "(") skip_parens();
            else take();
        }
        skip_newlines();
        BasicBlock* block = nullptr;
        // The entry block is unlabelled: it takes the next unnamed number.
        if (!(peek(1).text == ":" && (peek().kind == Token::Kind::integer || peek().kind == Token::Kind::ident))) {
            block = block_named(std::to_string(unnamed));
            f->blocks.push_back(block);
        }
        while (!accept("}")) {
            skip_newlines();
            if (peek().text == "}") continue;
            if (peek(1).text == ":" && (peek().kind == Token::Kind::integer || peek().kind == Token::Kind::ident ||
                                        peek().kind == Token::Kind::string)) {
                block = block_named(take().text);
                take();
                f->blocks.push_back(block);
                skip_line();
                continue;
            }
            if (!block) fail("instruction outside a block");
            auto* instruction = this->instruction();
            instruction->parent = block;
            instruction->index = ordinal_++;
            block->instructions.push_back(instruction);
            skip_line();
        }
        for (auto& [local, v] : locals_)
            if (v->kind == Value::Kind::placeholder) throw ParseError("undefined value %" + local + " in @" + f->name);
        for (auto* b : f->blocks) {
            b->function = f;
            if (b->instructions.empty() || !is_terminator(b->instructions.back()->op))
                throw ParseError("block %" + b->name + " in @" + f->name + " has no terminator");
        }
        f->compute_cfg();
        function_ = nullptr;
    }

    BasicBlock* block_named(const std::string& name) {
        auto& slot = blocks_[name];
        if (!slot) {
            slot = m_.make<BasicBlock>();
            slot->kind = Value::Kind::block;
            slot->name = name;
            slot->type = m_.primitive(Type::Kind::label);
        }
        return slot;
    }
    BasicBlock* label() {
        expect("label");
        if (peek().kind != Token::Kind::local) fail("expected a block label");
        return block_named(take().text);
    }

    Value* define(const std::string& name) {
        if (name.empty()) {
            auto* v = m_.make<Value>();
            v->kind = Value::Kind::instruction;
            return v;
        }
        auto& slot = locals_[name];
        if (!slot) slot = m_.make<Value>();
        else if (slot->kind != Value::Kind::placeholder) fail("value %" + name + " defined twice");
        slot->kind = Value::Kind::instruction;
        slot->name = name;
        return slot;
    }

    void skip_flags() {
        static const std::set<std::string_view> flags = {"nuw", "nsw", "exact", "disjoint", "nneg", "fast", "nnan",
            "ninf", "nsz", "arcp", "contract", "afn", "reassoc", "inbounds", "nusw", "samesign"};
        while (peek().kind == Token::Kind::ident && flags.count(peek().text)) take();
    }

    Value* instruction() {
        std::string name;
        if (peek().kind == Token::Kind::local && peek(1).text == "=") {
            name = take().text;
            take();
        }
        auto opcode = take().text;
        if (opcode == "tail" || opcode == "musttail" || opcode == "notail") opcode = take().text;
        auto* v = define(name);
        static const std::map<std::string_view, Opcode> binary = {{"add", Opcode::add}, {"sub", Opcode::sub},
            {"mul", Opcode::mul}, {"udiv", Opcode::udiv}, {"sdiv", Opcode::sdiv}, {"urem", Opcode::urem},
            {"srem", Opcode::srem}, {"shl", Opcode::shl}, {"lshr", Opcode::lshr}, {"ashr", Opcode::ashr},
            {"and", Opcode::and_}, {"or", Opcode::or_}, {"xor", Opcode::xor_}, {"fadd", Opcode::fadd},
            {"fsub", Opcode::fsub}, {"fmul", Opcode::fmul}, {"fdiv", Opcode::fdiv}, {"frem", Opcode::frem}};
        static const std::map<std::string_view, Opcode> casts = {{"trunc", Opcode::trunc}, {"zext", Opcode::zext},
            {"sext", Opcode::sext}, {"fptrunc", Opcode::fptrunc}, {"fpext", Opcode::fpext}, {"fptoui", Opcode::fptoui},
            {"fptosi", Opcode::fptosi}, {"uitofp", Opcode::uitofp}, {"sitofp", Opcode::sitofp},
            {"ptrtoint", Opcode::ptrtoint}, {"inttoptr", Opcode::inttoptr}, {"bitcast", Opcode::bitcast},
            {"addrspacecast", Opcode::addrspacecast}};
        if (const auto found = binary.find(opcode); found != binary.end()) {
            v->op = found->second;
            while (peek().kind == Token::Kind::ident && !at_type()) {
                const auto flag = take().text;
                if (flag == "nsw") v->nsw = true;
                if (flag == "nuw") v->nuw = true;
                if (flag == "exact") v->exact = true;
            }
            v->type = type();
            v->operands.push_back(value(v->type));
            expect(",");
            v->operands.push_back(value(v->type));
            return v;
        }
        if (const auto found = casts.find(opcode); found != casts.end()) {
            v->op = found->second;
            skip_flags();
            v->operands.push_back(typed_value());
            expect("to");
            v->type = type();
            v->aux_type = v->type;
            return v;
        }
        if (opcode == "fneg") {
            v->op = Opcode::fneg;
            skip_flags();
            v->type = type();
            v->operands.push_back(value(v->type));
            return v;
        }
        if (opcode == "icmp" || opcode == "fcmp") {
            v->op = opcode == "icmp" ? Opcode::icmp : Opcode::fcmp;
            skip_flags();
            static const std::map<std::string_view, Predicate> icmp = {{"eq", Predicate::eq}, {"ne", Predicate::ne},
                {"ugt", Predicate::ugt}, {"uge", Predicate::uge}, {"ult", Predicate::ult}, {"ule", Predicate::ule},
                {"sgt", Predicate::sgt}, {"sge", Predicate::sge}, {"slt", Predicate::slt}, {"sle", Predicate::sle}};
            static const std::map<std::string_view, Predicate> fcmp = {{"false", Predicate::f_false},
                {"oeq", Predicate::f_oeq}, {"ogt", Predicate::f_ogt}, {"oge", Predicate::f_oge}, {"olt", Predicate::f_olt},
                {"ole", Predicate::f_ole}, {"one", Predicate::f_one}, {"ord", Predicate::f_ord}, {"ueq", Predicate::f_ueq},
                {"ugt", Predicate::f_ugt}, {"uge", Predicate::f_uge}, {"ult", Predicate::f_ult}, {"ule", Predicate::f_ule},
                {"une", Predicate::f_une}, {"uno", Predicate::f_uno}, {"true", Predicate::f_true}};
            const auto predicate = take().text;
            const auto& table = v->op == Opcode::icmp ? icmp : fcmp;
            const auto found = table.find(predicate);
            if (found == table.end()) fail("unknown predicate " + predicate);
            v->predicate = found->second;
            const auto* operand_type = type();
            v->operands.push_back(value(operand_type));
            expect(",");
            v->operands.push_back(value(operand_type));
            v->type = operand_type->is_vector() ? m_.vector(m_.integer(1), operand_type->count) : m_.integer(1);
            return v;
        }
        if (opcode == "select") {
            v->op = Opcode::select;
            skip_flags();
            v->operands.push_back(typed_value());
            expect(",");
            v->operands.push_back(typed_value());
            expect(",");
            v->operands.push_back(typed_value());
            v->type = v->operands[1]->type;
            return v;
        }
        if (opcode == "freeze") {
            v->op = Opcode::freeze;
            v->operands.push_back(typed_value());
            v->type = v->operands[0]->type;
            return v;
        }
        if (opcode == "phi") {
            v->op = Opcode::phi;
            skip_flags();
            v->type = type();
            while (accept("[")) {
                v->operands.push_back(value(v->type));
                expect(",");
                if (peek().kind != Token::Kind::local) fail("expected a phi block");
                v->targets.push_back(block_named(take().text));
                expect("]");
                if (!accept(",")) break;
            }
            return v;
        }
        if (opcode == "call") {
            v->op = Opcode::call;
            skip_flags();
            // Calling convention and return attributes.
            while (!at_type()) {
                const auto word = take().text;
                if ((word == "align" || word == "dereferenceable") && peek().kind == Token::Kind::integer) take();
                if (peek().text == "(") skip_parens();
                if (at_end() || peek().kind == Token::Kind::newline) fail("malformed call");
            }
            const auto* return_type = type();
            if (return_type->kind == Type::Kind::function) return_type = return_type->members[0];
            v->type = return_type;
            if (peek().kind != Token::Kind::global) fail("indirect calls are unsupported");
            auto* callee = globals_.at(take().text);
            v->callee = static_cast<Function*>(callee);
            expect("(");
            while (!accept(")")) {
                if (peek().text == "metadata") {
                    // Metadata operands (llvm.experimental.noalias.scope.decl,
                    // dbg intrinsics) carry no value; the lowering ignores
                    // those intrinsics, so the operand is dropped.
                    take();
                    int depth = 0;
                    while (!at_end() && peek().kind != Token::Kind::newline &&
                           !(depth == 0 && (peek().text == "," || peek().text == ")"))) {
                        if (peek().text == "{" || peek().text == "(") ++depth;
                        if (peek().text == "}" || peek().text == ")") --depth;
                        take();
                    }
                    accept(",");
                    continue;
                }
                const auto* argument_type = type();
                last_align_ = 0;
                skip_attributes();
                v->operands.push_back(value(argument_type));
                v->arg_alignment.push_back(last_align_);
                accept(",");
            }
            // Function attributes / attribute groups / operand bundles follow.
            if (peek().text == "[") fail("operand bundles are unsupported");
            return v;
        }
        if (opcode == "load") {
            v->op = Opcode::load;
            if (accept("atomic")) v->atomic = true;
            if (accept("volatile")) v->is_volatile = true;
            v->type = type();
            v->aux_type = v->type;
            expect(",");
            v->operands.push_back(typed_value());
            memory_tail(v);
            return v;
        }
        if (opcode == "store") {
            v->op = Opcode::store;
            if (accept("atomic")) v->atomic = true;
            if (accept("volatile")) v->is_volatile = true;
            v->operands.push_back(typed_value());
            expect(",");
            v->operands.push_back(typed_value());
            v->type = m_.primitive(Type::Kind::void_);
            memory_tail(v);
            return v;
        }
        if (opcode == "alloca") {
            v->op = Opcode::alloca;
            accept("inalloca");
            v->aux_type = type();
            v->type = m_.pointer(0);
            while (accept(",")) {
                if (accept("align")) v->alignment = integer();
                else if (accept("addrspace")) {
                    expect("(");
                    v->type = m_.pointer(static_cast<std::uint32_t>(integer()));
                    expect(")");
                } else if (at_type()) {
                    auto* count = typed_value();
                    if (count->kind != Value::Kind::constant_int || count->int_value != 1)
                        fail("dynamically sized alloca is unsupported");
                } else {
                    break;
                }
            }
            return v;
        }
        if (opcode == "getelementptr") {
            v->op = Opcode::getelementptr;
            while (peek().text == "inbounds" || peek().text == "nuw" || peek().text == "nusw") {
                if (take().text == "inbounds") v->inbounds = true;
            }
            v->aux_type = type();
            expect(",");
            while (true) {
                if (accept("inrange")) skip_parens();
                v->operands.push_back(typed_value());
                if (!accept(",")) break;
                if (peek().kind == Token::Kind::meta) { --p_; break; }
            }
            v->type = v->operands[0]->type;
            return v;
        }
        if (opcode == "extractelement") {
            v->op = Opcode::extractelement;
            v->operands.push_back(typed_value());
            expect(",");
            v->operands.push_back(typed_value());
            v->type = v->operands[0]->type->element;
            return v;
        }
        if (opcode == "insertelement") {
            v->op = Opcode::insertelement;
            v->operands.push_back(typed_value());
            expect(",");
            v->operands.push_back(typed_value());
            expect(",");
            v->operands.push_back(typed_value());
            v->type = v->operands[0]->type;
            return v;
        }
        if (opcode == "shufflevector") {
            v->op = Opcode::shufflevector;
            v->operands.push_back(typed_value());
            expect(",");
            v->operands.push_back(typed_value());
            expect(",");
            const auto* mask_type = type();
            auto* mask = value(mask_type);
            v->type = m_.vector(v->operands[0]->type->element, mask_type->count);
            for (std::uint64_t i = 0; i < mask_type->count; ++i) {
                if (mask->kind == Value::Kind::constant_null) v->indices.push_back(0);
                else if (mask->is_undef()) v->indices.push_back(-1);
                else {
                    auto* element = mask->operands.at(i);
                    v->indices.push_back(element->kind == Value::Kind::constant_int
                        ? static_cast<std::int64_t>(element->int_value) : -1);
                }
            }
            return v;
        }
        if (opcode == "extractvalue" || opcode == "insertvalue") {
            v->op = opcode == "extractvalue" ? Opcode::extractvalue : Opcode::insertvalue;
            v->operands.push_back(typed_value());
            if (v->op == Opcode::insertvalue) {
                expect(",");
                v->operands.push_back(typed_value());
            }
            const Type* t = v->operands[0]->type;
            while (accept(",")) {
                if (peek().kind != Token::Kind::integer) { --p_; break; }
                const auto index = integer();
                v->indices.push_back(static_cast<std::int64_t>(index));
                t = t->kind == Type::Kind::structure ? t->members.at(index) : t->element;
            }
            v->type = v->op == Opcode::extractvalue ? t : v->operands[0]->type;
            return v;
        }
        if (opcode == "atomicrmw") {
            v->op = Opcode::atomicrmw;
            if (accept("volatile")) v->is_volatile = true;
            static const std::map<std::string_view, AtomicOp> ops = {{"xchg", AtomicOp::xchg}, {"add", AtomicOp::add},
                {"sub", AtomicOp::sub}, {"and", AtomicOp::and_}, {"nand", AtomicOp::nand}, {"or", AtomicOp::or_},
                {"xor", AtomicOp::xor_}, {"max", AtomicOp::max}, {"min", AtomicOp::min}, {"umax", AtomicOp::umax},
                {"umin", AtomicOp::umin}, {"fadd", AtomicOp::fadd}, {"fsub", AtomicOp::fsub}, {"fmax", AtomicOp::fmax},
                {"fmin", AtomicOp::fmin}};
            const auto op = take().text;
            const auto found = ops.find(op);
            if (found == ops.end()) fail("unsupported atomicrmw " + op);
            v->atomic_op = found->second;
            v->operands.push_back(typed_value());
            expect(",");
            v->operands.push_back(typed_value());
            v->type = v->operands[1]->type;
            memory_tail(v);
            return v;
        }
        if (opcode == "cmpxchg") {
            v->op = Opcode::cmpxchg;
            if (accept("weak")) v->weak = true;
            if (accept("volatile")) v->is_volatile = true;
            v->operands.push_back(typed_value());
            expect(",");
            v->operands.push_back(typed_value());
            expect(",");
            v->operands.push_back(typed_value());
            Type pair;
            pair.kind = Type::Kind::structure;
            pair.members = {v->operands[1]->type, m_.integer(1)};
            v->type = m_.intern(std::move(pair));
            memory_tail(v);
            return v;
        }
        if (opcode == "fence") {
            v->op = Opcode::fence;
            v->type = m_.primitive(Type::Kind::void_);
            skip_line_items();
            return v;
        }
        if (opcode == "br") {
            v->op = Opcode::br;
            v->type = m_.primitive(Type::Kind::void_);
            if (peek().text == "label") {
                v->targets.push_back(label());
            } else {
                v->operands.push_back(typed_value());
                expect(",");
                v->targets.push_back(label());
                expect(",");
                v->targets.push_back(label());
            }
            return v;
        }
        if (opcode == "switch") {
            v->op = Opcode::switch_;
            v->type = m_.primitive(Type::Kind::void_);
            v->operands.push_back(typed_value());
            expect(",");
            v->targets.push_back(label());
            expect("[");
            while (true) {
                skip_newlines();
                if (accept("]")) break;
                v->operands.push_back(typed_value());
                expect(",");
                v->targets.push_back(label());
            }
            return v;
        }
        if (opcode == "ret") {
            v->op = Opcode::ret;
            v->type = m_.primitive(Type::Kind::void_);
            if (!accept("void")) v->operands.push_back(typed_value());
            return v;
        }
        if (opcode == "unreachable") {
            v->op = Opcode::unreachable;
            v->type = m_.primitive(Type::Kind::void_);
            return v;
        }
        --p_;
        fail("unsupported instruction '" + opcode + "'");
    }

    void memory_tail(Value* v) {
        // [syncscope("...")] [ordering...] [, align N] [, !metadata ...]
        while (peek().kind == Token::Kind::ident) {
            const auto word = take().text;
            if (word == "syncscope") skip_parens();
        }
        while (accept(",")) {
            if (accept("align")) v->alignment = integer();
            else skip_line_items();
        }
    }
};

} // namespace

std::unique_ptr<Module> parse(std::string_view text) {
    auto module = std::make_unique<Module>();
    Parser(text, *module).run();
    return module;
}

} // namespace alchemy::msl::ir
