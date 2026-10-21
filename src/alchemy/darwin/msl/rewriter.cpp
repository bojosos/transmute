#include "rewriter.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <set>
#include <sstream>

// MSL source rewriter (owner: A6). See rewriter.hpp. The parser is
// deliberately shallow: it splits the preprocessed token stream into
// namespace-, class- and function-scope statements by brace and parenthesis
// balance, which is all MSL's attribute syntax and Blender's generated
// sources need; clang does the real C++ parsing afterwards.
namespace alchemy::msl::detail {
namespace {

// ---- tokens ------------------------------------------------------------------------
struct Token {
    enum class Kind { space, newline, identifier, number, string, character, punct, directive };
    Kind kind{Kind::space};
    std::string text;
    std::uint32_t line{};      // line in the original MSL source
    std::uint32_t file{};      // index into the tokenizer's file names (line markers)
    std::string out;           // replacement text
    std::string before, after;
    bool removed{};
};

const char* const punctuators[] = {">>=", "<<=", "...", "->*", "::", "->", "++", "--", "<<", ">>", "<=", ">=",
    "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##", ".*"};

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }
bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }

// `files`: the file names of the GNU line markers, as written between the
// quotes ("<stdin>" until the first marker).
std::vector<Token> tokenize(std::string_view s, std::vector<std::string>& files) {
    std::vector<Token> tokens;
    std::uint32_t line = 1;
    files.assign(1, "<stdin>");
    std::uint32_t file = 0;
    bool line_start = true;
    std::size_t i = 0;
    auto push = [&](Token::Kind kind, std::size_t begin, std::size_t end) {
        Token token;
        token.kind = kind;
        token.text = std::string(s.substr(begin, end - begin));
        token.out = token.text;
        token.line = line;
        token.file = file;
        tokens.push_back(std::move(token));
    };
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\n') {
            push(Token::Kind::newline, i, i + 1);
            ++line;
            ++i;
            line_start = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            std::size_t j = i;
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\f' || s[j] == '\v')) ++j;
            push(Token::Kind::space, i, j);
            i = j;
            continue;
        }
        if (c == '#' && line_start) {
            std::size_t j = i;
            while (j < s.size() && s[j] != '\n') {
                if (s[j] == '\\' && j + 1 < s.size() && s[j + 1] == '\n') ++j;
                ++j;
            }
            push(Token::Kind::directive, i, j);
            // GNU line marker "# N "file" flags": the next line is line N.
            const std::string_view text = s.substr(i, j - i);
            std::size_t k = 1;
            while (k < text.size() && text[k] == ' ') ++k;
            if (text.substr(k, 4) == "line") k += 4;
            while (k < text.size() && text[k] == ' ') ++k;
            std::uint32_t value = 0;
            const auto parsed = std::from_chars(text.data() + k, text.data() + text.size(), value);
            if (parsed.ec == std::errc{} && parsed.ptr != text.data() + k) {
                line = value - 1;
                const auto open = text.find('"', static_cast<std::size_t>(parsed.ptr - text.data()));
                const auto close = open == std::string_view::npos ? open : text.find('"', open + 1);
                if (close != std::string_view::npos) {
                    const auto name = text.substr(open + 1, close - open - 1);
                    if (files[file] != name) {
                        const auto found = std::find(files.begin(), files.end(), name);
                        file = static_cast<std::uint32_t>(found - files.begin());
                        if (found == files.end()) files.emplace_back(name);
                    }
                }
            }
            i = j;
            continue;
        }
        line_start = false;
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            std::size_t j = i;
            while (j < s.size() && s[j] != '\n') ++j;
            push(Token::Kind::space, i, j);
            i = j;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            std::size_t j = i + 2;
            const auto start_line = line;
            while (j + 1 < s.size() && !(s[j] == '*' && s[j + 1] == '/')) {
                if (s[j] == '\n') ++line;
                ++j;
            }
            j = std::min(s.size(), j + 2);
            Token token;
            token.kind = Token::Kind::space;
            // Keep the newlines of a block comment so line numbers stay put.
            token.text = std::string(s.substr(i, j - i));
            for (const auto ch : token.text) if (ch == '\n') token.out += '\n';
            if (token.out.empty()) token.out = " ";
            token.line = start_line;
            tokens.push_back(std::move(token));
            i = j;
            continue;
        }
        if (ident_start(c)) {
            std::size_t j = i;
            while (j < s.size() && ident_char(s[j])) ++j;
            push(Token::Kind::identifier, i, j);
            i = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            std::size_t j = i + 1;
            while (j < s.size()) {
                const char d = s[j];
                if ((d == '+' || d == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E' || s[j - 1] == 'p' || s[j - 1] == 'P')) {
                    ++j;
                    continue;
                }
                if (!ident_char(d) && d != '.' && d != '\'') break;
                ++j;
            }
            push(Token::Kind::number, i, j);
            i = j;
            continue;
        }
        if (c == '"' || c == '\'') {
            std::size_t j = i + 1;
            while (j < s.size() && s[j] != c && s[j] != '\n') {
                if (s[j] == '\\') ++j;
                ++j;
            }
            j = std::min(s.size(), j + 1);
            push(c == '"' ? Token::Kind::string : Token::Kind::character, i, j);
            i = j;
            continue;
        }
        std::size_t length = 1;
        for (const auto* p : punctuators) {
            const std::string_view candidate(p);
            if (s.substr(i, candidate.size()) == candidate) {
                length = candidate.size();
                break;
            }
        }
        push(Token::Kind::punct, i, i + length);
        i += length;
    }
    return tokens;
}

// ---- attribute and scope model ------------------------------------------------------
struct Attribute {
    std::string name;
    std::vector<std::string> args;            // top-level comma separated, trimmed
    std::vector<std::vector<std::size_t>> arg_tokens;   // significant token indices per arg
    std::uint32_t line{};
};

struct Member {
    std::string name;
    std::string type;                          // renamed type text
    std::string array_suffix;                  // "[6]" etc.
    std::vector<Attribute> attributes;
    std::uint32_t line{};
};

struct StructDef {
    std::string name;
    std::string qualified;                     // A::B::name
    std::vector<Member> members;
};

bool is_msl_attribute(std::string_view name) {
    static const std::set<std::string_view> names = {
        "buffer", "texture", "sampler", "threadgroup", "stage_in", "position", "point_size", "clip_distance",
        "vertex_id", "instance_id", "base_vertex", "base_instance", "front_facing", "point_coord", "sample_id",
        "sample_mask", "primitive_id", "render_target_array_index", "viewport_array_index",
        "thread_position_in_grid", "thread_position_in_threadgroup", "threadgroup_position_in_grid",
        "threads_per_threadgroup", "threads_per_grid", "threadgroups_per_grid", "thread_index_in_threadgroup",
        "thread_index_in_simdgroup", "simdgroup_index_in_threadgroup", "threads_per_simdgroup",
        "simdgroups_per_threadgroup", "thread_execution_width", "thread_index_in_quadgroup",
        "quadgroup_index_in_threadgroup", "quadgroups_per_threadgroup", "dispatch_threads_per_threadgroup",
        "attribute", "user", "flat", "center_perspective", "center_no_perspective", "centroid_perspective",
        "centroid_no_perspective", "sample_perspective", "sample_no_perspective", "color", "index", "depth",
        "stencil", "raster_order_group", "invariant", "function_constant", "id", "early_fragment_tests",
        "max_total_threads_per_threadgroup", "vertex", "fragment", "kernel", "host_name", "visible",
        "stitchable", "patch", "barycentric_coord", "payload", "object", "mesh", "amplification_id",
        "amplification_count", "shared", "intersection", "imageblock_data", "access", "raster_order_group",
        "required_threads_per_threadgroup", "instance_id"};
    return names.count(name) != 0;
}

const std::map<std::string_view, Builtin>& builtin_attributes() {
    static const std::map<std::string_view, Builtin> map = {
        {"vertex_id", Builtin::vertex_id}, {"instance_id", Builtin::instance_id},
        {"base_vertex", Builtin::base_vertex}, {"base_instance", Builtin::base_instance},
        {"position", Builtin::position}, {"point_size", Builtin::point_size},
        {"clip_distance", Builtin::clip_distance}, {"front_facing", Builtin::front_facing},
        {"point_coord", Builtin::point_coord}, {"sample_id", Builtin::sample_id},
        {"sample_mask", Builtin::sample_mask}, {"primitive_id", Builtin::primitive_id},
        {"render_target_array_index", Builtin::render_target_array_index},
        {"viewport_array_index", Builtin::viewport_array_index},
        {"thread_position_in_grid", Builtin::thread_position_in_grid},
        {"thread_position_in_threadgroup", Builtin::thread_position_in_threadgroup},
        {"threadgroup_position_in_grid", Builtin::threadgroup_position_in_grid},
        {"threads_per_threadgroup", Builtin::threads_per_threadgroup},
        {"dispatch_threads_per_threadgroup", Builtin::threads_per_threadgroup},
        {"threads_per_grid", Builtin::threads_per_grid}, {"threadgroups_per_grid", Builtin::threadgroups_per_grid},
        {"thread_index_in_threadgroup", Builtin::thread_index_in_threadgroup},
        {"thread_index_in_simdgroup", Builtin::thread_index_in_simdgroup},
        {"thread_execution_width", Builtin::threads_per_simdgroup},
        {"simdgroup_index_in_threadgroup", Builtin::simdgroup_index_in_threadgroup},
        {"threads_per_simdgroup", Builtin::threads_per_simdgroup},
        {"simdgroups_per_threadgroup", Builtin::simdgroups_per_threadgroup}};
    return map;
}

// The natural (Vulkan) type a builtin input is read as.
const char* builtin_input_type(Builtin builtin) {
    switch (builtin) {
    case Builtin::position: return "float4";
    case Builtin::point_coord: return "float2";
    case Builtin::front_facing: return "bool";
    case Builtin::thread_position_in_grid: case Builtin::thread_position_in_threadgroup:
    case Builtin::threadgroup_position_in_grid: case Builtin::threads_per_threadgroup:
    case Builtin::threads_per_grid: case Builtin::threadgroups_per_grid: return "uint3";
    default: return "uint";
    }
}
std::string raw_type(const char* type) {
    const std::string_view t(type);
    if (t == "float4") return "metal::__ext<float, 4>";
    if (t == "float2") return "metal::__ext<float, 2>";
    if (t == "uint3") return "metal::__ext<uint, 3>";
    return std::string(t);
}

bool valid_builtin(Stage stage, Builtin builtin, bool input) {
    switch (builtin) {
    case Builtin::vertex_id: case Builtin::instance_id: case Builtin::base_vertex: case Builtin::base_instance:
        return stage == Stage::vertex && input;
    case Builtin::position: return (stage == Stage::fragment && input) || (stage == Stage::vertex && !input);
    case Builtin::point_size: case Builtin::clip_distance: return stage == Stage::vertex && !input;
    case Builtin::front_facing: case Builtin::point_coord: case Builtin::sample_id: case Builtin::primitive_id:
        return stage == Stage::fragment && input;
    case Builtin::sample_mask: return stage == Stage::fragment;
    case Builtin::render_target_array_index: case Builtin::viewport_array_index:
        return (stage == Stage::vertex && !input) || (stage == Stage::fragment && input);
    // SIMD-group lane and width are fragment inputs too (MSL 2.2).
    case Builtin::thread_index_in_simdgroup: case Builtin::threads_per_simdgroup:
        return (stage == Stage::kernel || stage == Stage::fragment) && input;
    default: return stage == Stage::kernel && input;
    }
}

class Rewriter {
public:
    explicit Rewriter(std::string_view source) : tokens_(tokenize(source, files_)) {
        for (std::size_t i = 0; i < tokens_.size(); ++i) {
            const auto kind = tokens_[i].kind;
            if (kind != Token::Kind::space && kind != Token::Kind::newline && kind != Token::Kind::directive)
                sig_.push_back(i);
        }
    }

    RewriteResult run() {
        parse_scope(0, sig_.size(), {}, true);
        rename_derived_uses();
        for (auto& entry : pending_entries_) build_entry(entry);
        rename_tokens();
        wrap_conditionals();
        RewriteResult result;
        std::string out;
        out.reserve(tokens_.size() * 4 + 4096);
        // User code stays in the global namespace, as in Metal: wrapping it in
        // a namespace would let user overloads of stdlib names (Blender's
        // `mix(vec, vec, bool)` templates) hide metal::mix and friends from
        // unqualified lookup. Internal linkage comes from a pragma instead, so
        // functions the entry points do not reach are dropped at -O2 and only
        // the wrappers stay external. User functions are always_inline: the
        // lowering needs one function per entry point, and -O2's inliner
        // leaves large shader-class methods (Blender's vertex_main) as calls.
        // Function constants are declared where the source declares them,
        // outside that region (function_constant()), so they keep external
        // linkage and the output up to any split point does not depend on
        // the code after it (prelude PCHs, prelude.hpp).
        out += "# 1 \"<alchemy-msl-prologue>\"\n";
        out += user_code_begin;
        std::size_t next_split = 0;
        for (std::size_t i = 0; i < tokens_.size(); ++i) {
            const auto& token = tokens_[i];
            if (next_split < split_tokens_.size() && split_tokens_[next_split] == i) {
                SplitPoint split;
                split.offset = out.size();
                split.line = token.line;
                split.file = files_[token.file];
                result.split_points.push_back(std::move(split));
                ++next_split;
            }
            out += token.before;
            if (!token.removed) out += token.out;
            else if (token.kind == Token::Kind::newline) out += '\n';
            out += token.after;
        }
        out += "\n";
        out += user_code_end;
        out += "# 1 \"<alchemy-msl-entry>\"\n";
        out += wrappers_.str();
        result.source = std::move(out);
        result.entries = std::move(entries_);
        result.function_constants = std::move(constants_);
        result.threadgroup_variables = threadgroup_ids_;
        result.diagnostics = std::move(diagnostics_);
        return result;
    }

private:
    struct Scope {
        std::vector<std::string> path;     // enclosing namespaces/classes
        bool in_class{};
    };
    struct PendingEntry {
        Stage stage{};
        std::string name;
        std::vector<std::string> path;
        std::vector<Attribute> attributes;
        std::string return_type;
        std::vector<std::vector<std::size_t>> params;   // significant indices per parameter (attributes removed)
        std::vector<std::vector<Attribute>> param_attributes;
        std::uint32_t line{};
    };

    std::vector<std::string> files_;           // before tokens_: filled by tokenize
    std::vector<Token> tokens_;
    std::vector<std::size_t> sig_;
    std::vector<std::size_t> split_tokens_;    // raw token indices starting a top-level line (split points)
    std::vector<StructDef> structs_;
    std::vector<FunctionConstantDecl> constants_;
    std::vector<PendingEntry> pending_entries_;
    std::vector<EntryPoint> entries_;
    std::vector<Diagnostic> diagnostics_;
    std::ostringstream wrappers_;
    std::uint32_t threadgroup_ids_{};
    std::set<std::size_t> keep_identifier_;     // tokens exempt from keyword renaming
    // Type aliases at namespace and class scope (`using ivec2 = int2;`,
    // `typedef uint uint32_t;`): name -> aliased spelling. Names aliased to
    // two different spellings (in different scopes) are not resolved.
    std::map<std::string, std::string> aliases_;
    std::set<std::string> ambiguous_aliases_;

    void record_alias(std::size_t s, std::size_t e) {
        std::string name, type;
        auto join = [&](std::size_t from, std::size_t to) {
            std::string out;
            for (auto k = from; k < to; ++k) {
                if (is(k, "(") || is(k, "[") || is(k, "{")) return std::string();   // function/array types
                out += (out.empty() ? "" : " ") + rename_word(text(k));
            }
            return out;
        };
        if (is(s, "using")) {
            if (s + 3 >= e || T(s + 1).kind != Token::Kind::identifier || !is(s + 2, "=")) return;
            name = text(s + 1);
            type = join(s + 3, e);
        } else if (is(s, "typedef")) {
            if (e < s + 3 || T(e - 1).kind != Token::Kind::identifier) return;
            name = text(e - 1);
            type = join(s + 1, e - 1);
        }
        if (name.empty() || type.empty() || ambiguous_aliases_.count(name)) return;
        auto clean = [](std::string value) {
            value.erase(std::remove(value.begin(), value.end(), ' '), value.end());
            return value;
        };
        const auto [at, inserted] = aliases_.emplace(name, type);
        if (!inserted && clean(at->second) != clean(type)) {
            aliases_.erase(at);
            ambiguous_aliases_.insert(name);
        }
    }

    // parse_type_spelling through the aliases: a spelling that names no MSL
    // scalar/vector/matrix/texture/sampler type is looked up (also without
    // its qualifier) and the aliased spelling parsed instead, transitively.
    TypeSpelling resolve_spelling(std::string_view input) const {
        auto spelling = parse_type_spelling(input);
        for (int depth = 0; depth < 16; ++depth) {
            if (spelling.kind != Type::Kind::structure && spelling.kind != Type::Kind::unknown) break;
            auto key = spelling.text;
            auto found = aliases_.find(key);
            if (found == aliases_.end()) {
                const auto colon = key.rfind("::");
                if (colon == std::string::npos) break;
                found = aliases_.find(key.substr(colon + 2));
                if (found == aliases_.end()) break;
            }
            spelling = parse_type_spelling(found->second);
        }
        return spelling;
    }

    Token& T(std::size_t s) { return tokens_[sig_[s]]; }
    const std::string& text(std::size_t s) const { return tokens_[sig_[s]].text; }
    bool is(std::size_t s, std::string_view value) const { return s < sig_.size() && text(s) == value; }
    std::uint32_t line_of(std::size_t s) const { return s < sig_.size() ? tokens_[sig_[s]].line : 0; }

    void error(std::uint32_t line, std::string message) {
        Diagnostic diagnostic;
        diagnostic.severity = Diagnostic::Severity::error;
        diagnostic.message = std::move(message);
        diagnostic.line = line;
        diagnostic.column = 1;
        diagnostics_.push_back(std::move(diagnostic));
    }

    // Removes significant tokens [b, e) and the whitespace between them
    // (newlines are kept so line numbers stay correct).
    void remove(std::size_t b, std::size_t e) {
        if (b >= e) return;
        const auto first = sig_[b], last = sig_[e - 1];
        for (auto i = first; i <= last; ++i)
            if (tokens_[i].kind != Token::Kind::newline && tokens_[i].kind != Token::Kind::directive) tokens_[i].removed = true;
    }

    std::size_t match(std::size_t s, std::string_view open, std::string_view close) const {
        int depth = 0;
        for (auto i = s; i < sig_.size(); ++i) {
            if (text(i) == open) ++depth;
            else if (text(i) == close && --depth == 0) return i;
        }
        return sig_.size();
    }

    // [[ ... ]] starting at s (s and s+1 are '['): returns the index after ]].
    std::size_t parse_attribute_group(std::size_t s, std::vector<Attribute>& out) {
        std::size_t i = s + 2;
        while (i < sig_.size() && !(is(i, "]") && is(i + 1, "]"))) {
            if (is(i, ",")) { ++i; continue; }
            Attribute attribute;
            attribute.line = line_of(i);
            while (i < sig_.size() && (T(i).kind == Token::Kind::identifier || is(i, "::"))) attribute.name += text(i++);
            if (is(i, "(")) {
                const auto close = match(i, "(", ")");
                std::vector<std::size_t> current;
                int depth = 0;
                for (auto k = i + 1; k < close; ++k) {
                    if (is(k, "(") || is(k, "[") || is(k, "{")) ++depth;
                    if (is(k, ")") || is(k, "]") || is(k, "}")) --depth;
                    if (depth == 0 && is(k, ",")) {
                        attribute.arg_tokens.push_back(std::move(current));
                        current.clear();
                        continue;
                    }
                    current.push_back(k);
                }
                attribute.arg_tokens.push_back(std::move(current));
                for (const auto& arg : attribute.arg_tokens) {
                    std::string value;
                    for (const auto k : arg) value += (value.empty() ? "" : " ") + text(k);
                    attribute.args.push_back(value);
                }
                i = close + 1;
            }
            if (attribute.name.empty()) { ++i; continue; }
            out.push_back(std::move(attribute));
        }
        return std::min(sig_.size(), i + 2);
    }

    // Collects and removes the MSL attribute groups in [b, e); returns the
    // significant indices left. Groups holding only non-MSL attributes stay.
    std::vector<std::size_t> strip_attributes(std::size_t b, std::size_t e, std::vector<Attribute>& attributes) {
        std::vector<std::size_t> rest;
        for (auto i = b; i < e;) {
            if (is(i, "[") && is(i + 1, "[")) {
                std::vector<Attribute> group;
                const auto end = std::min(e, parse_attribute_group(i, group));
                bool msl = false;
                for (const auto& attribute : group) msl = msl || is_msl_attribute(attribute.name);
                if (msl) {
                    for (const auto& attribute : group)
                        if (is_msl_attribute(attribute.name)) attributes.push_back(attribute);
                    remove(i, end);
                } else {
                    for (auto k = i; k < end; ++k) rest.push_back(k);
                }
                i = end;
                continue;
            }
            rest.push_back(i++);
        }
        return rest;
    }

    // ---- scopes ---------------------------------------------------------------------
    // `top`: the translation unit's own scope, where every statement start
    // at the beginning of a line is a split point.
    void parse_scope(std::size_t b, std::size_t e, Scope scope, bool top = false) {
        std::size_t i = b;
        while (i < e) {
            const auto s = i;
            if (top && s > 0) {
                for (auto raw = sig_[s]; raw-- > sig_[s - 1] + 1;) {
                    if (tokens_[raw].kind != Token::Kind::newline) continue;
                    if (split_tokens_.empty() || split_tokens_.back() < raw + 1) split_tokens_.push_back(raw + 1);
                    break;
                }
            }
            int paren = 0, bracket = 0;
            std::size_t j = s;
            for (; j < e; ++j) {
                const auto& t = text(j);
                if (t == "(") ++paren;
                else if (t == ")") --paren;
                else if (t == "[") ++bracket;
                else if (t == "]") --bracket;
                else if (paren == 0 && bracket == 0 && (t == ";" || t == "{")) break;
                else if (scope.in_class && paren == 0 && bracket == 0 && t == ":" && j == s + 1 &&
                         (text(s) == "public" || text(s) == "private" || text(s) == "protected")) break;
            }
            if (j >= e) break;
            if (is(j, ":")) { i = j + 1; continue; }
            if (j == s && is(j, "{")) {
                // A block without a head (e.g. after a ctor-initializer): skip it.
                i = match(j, "{", "}") + 1;
                continue;
            }
            if (is(j, ";")) {
                simple_statement(s, j, scope);
                i = j + 1;
                continue;
            }
            const auto close = match(j, "{", "}");
            if (close >= e) {
                error(line_of(j), "unbalanced braces");
                return;
            }
            const auto kind = classify(s, j);
            if (kind == 'n') {
                Scope inner = scope;
                for (auto k = s; k < j; ++k)
                    if (T(k).kind == Token::Kind::identifier && text(k) != "namespace" && text(k) != "inline")
                        inner.path.push_back(text(k));
                inner.in_class = false;
                parse_scope(j + 1, close, inner);
                i = close + 1;
            } else if (kind == 'l') {
                parse_scope(j + 1, close, scope);
                i = close + 1;
            } else if (kind == 'c' || kind == 'e') {
                // Class or enum: the statement continues after the body.
                std::size_t end = close + 1;
                while (end < e && !is(end, ";")) ++end;
                if (kind == 'c') class_definition(s, j, close, end, scope);
                i = std::min(e, end + 1);
            } else if (kind == 'f') {
                function_definition(s, j, close, scope);
                i = close + 1;
                if (is(i, ";")) ++i;
            } else {
                // Brace initializer: part of a simple statement.
                std::size_t end = close + 1;
                int depth = 0;
                while (end < e) {
                    if (is(end, "{") || is(end, "(")) ++depth;
                    if (is(end, "}") || is(end, ")")) --depth;
                    if (depth == 0 && is(end, ";")) break;
                    ++end;
                }
                simple_statement(s, end, scope);
                i = std::min(e, end + 1);
            }
        }
    }

    // 'n' namespace, 'l' linkage block, 'c' class, 'e' enum, 'f' function, 'i' initializer.
    char classify(std::size_t s, std::size_t brace) const {
        std::size_t k = s;
        if (is(k, "inline")) ++k;
        if (is(k, "namespace")) return 'n';
        if (is(s, "extern") && s + 1 < brace && T_kind(s + 1) == Token::Kind::string && s + 2 == brace) return 'l';
        // A depth-0 '(' that is not alignas/__attribute__/decltype makes a function.
        int angle = 0, paren = 0;
        bool function = false, assignment = false;
        for (auto i = s; i < brace; ++i) {
            const auto& t = text(i);
            if (t == "[" && is(i + 1, "[")) {
                i = match_attribute_end(i);
                continue;
            }
            if (t == "<") ++angle;
            else if (t == ">") angle = std::max(0, angle - 1);
            else if (t == ">>") angle = std::max(0, angle - 2);
            else if (t == "=" && paren == 0 && angle == 0) assignment = true;
            else if (t == "(") {
                const bool special = i > s && (text(i - 1) == "alignas" || text(i - 1) == "__attribute__" ||
                    text(i - 1) == "decltype" || text(i - 1) == "__declspec" || text(i - 1) == "sizeof");
                if (paren == 0 && !special && angle == 0) function = true;
                if (special) { i = match_const(i, "(", ")"); continue; }
                ++paren;
            } else if (t == ")") --paren;
        }
        if (assignment && !function) return 'i';
        if (function) return assignment ? 'i' : 'f';
        // Class key after an optional template header and typedef.
        for (auto i = s; i < brace; ++i) {
            const auto& t = text(i);
            if (t == "template" && is(i + 1, "<")) {
                i = match_angles(i + 1);
                continue;
            }
            if (t == "typedef" || t == "static" || t == "const" || t == "constexpr") continue;
            if (t == "struct" || t == "class" || t == "union") return 'c';
            if (t == "enum") return 'e';
            break;
        }
        return 'i';
    }
    Token::Kind T_kind(std::size_t s) const { return tokens_[sig_[s]].kind; }
    std::size_t match_const(std::size_t s, std::string_view open, std::string_view close) const {
        return match(s, open, close);
    }
    std::size_t match_attribute_end(std::size_t s) const {
        for (auto i = s + 2; i + 1 < sig_.size(); ++i)
            if (is(i, "]") && is(i + 1, "]")) return i + 1;
        return sig_.size() - 1;
    }
    std::size_t match_angles(std::size_t s) const {
        int depth = 0;
        for (auto i = s; i < sig_.size(); ++i) {
            if (is(i, "<")) ++depth;
            else if (is(i, ">")) { if (--depth == 0) return i; }
            else if (is(i, ">>")) { depth -= 2; if (depth <= 0) return i; }
            else if (is(i, "(")) i = match(i, "(", ")");
        }
        return sig_.size();
    }

    // ---- classes ------------------------------------------------------------------------
    void class_definition(std::size_t s, std::size_t open, std::size_t close, std::size_t end, const Scope& scope) {
        StructDef def;
        bool is_typedef = false;
        for (auto i = s; i < open; ++i) {
            const auto& t = text(i);
            if (t == "typedef") is_typedef = true;
            if (t == "template" && is(i + 1, "<")) { i = match_angles(i + 1); continue; }
            if ((t == "struct" || t == "class" || t == "union") && def.name.empty()) {
                for (auto k = i + 1; k < open; ++k) {
                    if (is(k, "[") && is(k + 1, "[")) { k = match_attribute_end(k); continue; }
                    if (is(k, "alignas") || is(k, "__attribute__") || is(k, "__declspec")) {
                        k = match(k + 1, "(", ")");
                        continue;
                    }
                    if (T(k).kind == Token::Kind::identifier && text(k) != "final") def.name = text(k);
                    break;
                }
            }
        }
        if (is_typedef || def.name.empty()) {
            for (auto i = end; i > close; --i)
                if (T(i).kind == Token::Kind::identifier) { if (def.name.empty() || is_typedef) def.name = text(i); break; }
        }
        Scope inner = scope;
        inner.in_class = true;
        if (!def.name.empty()) inner.path.push_back(def.name);
        for (const auto& part : scope.path) def.qualified += part + "::";
        def.qualified += def.name;
        current_members_.emplace_back();
        parse_scope(open + 1, close, inner);
        def.members = std::move(current_members_.back());
        current_members_.pop_back();
        if (!def.name.empty()) structs_.push_back(std::move(def));
    }
    std::vector<std::vector<Member>> current_members_;

    void simple_statement(std::size_t s, std::size_t e, const Scope& scope) {
        if (s >= e) return;
        const auto& first = text(s);
        if (first == "using" || first == "typedef") {
            record_alias(s, e);
            return;
        }
        if (first == "friend" || first == "static_assert" || first == "template" || first == "enum") return;
        bool has_attribute = false;
        for (auto i = s; i + 1 < e; ++i) has_attribute = has_attribute || (is(i, "[") && is(i + 1, "["));
        if (scope.in_class) {
            if (!current_members_.empty()) member_declaration(s, e);
            return;
        }
        // Namespace scope: function prototypes, function constants and
        // program-scope constants.
        declaration_params(s, e);
        if (has_attribute) {
            std::vector<Attribute> attributes;
            const auto rest = strip_attributes(s, e, attributes);
            for (const auto& attribute : attributes) {
                if (attribute.name == "function_constant") {
                    function_constant(rest, attribute);
                    return;
                }
                error(attribute.line, "attribute '" + attribute.name + "' is not valid on this declaration");
            }
            return;
        }
        if (first == "constant") {
            if (derived_constant(s, e)) return;
            bool constant = false;
            for (auto i = s; i < e; ++i) constant = constant || is(i, "const") || is(i, "constexpr");
            if (!constant) T(s).after += " const";
        }
    }

    // ---- derived function constants -------------------------------------------------------
    // `constant bool C = A || B;` with A and B function constants (ANGLE's
    // ANGLESampleMaskWriteEnabled) is a program-scope constant whose value
    // exists only at pipeline creation. As a global it would get a dynamic
    // initializer that no entry point runs, so it becomes an inline function,
    // `inline bool __msl_fc_derived_C() { return (A || B); }`, and every later
    // use of C calls it; [[function_constant(C)]] conditions use it too.
    // Declarations depending on one another chain the same way.
    std::map<std::string, std::string> derived_;                 // name -> function
    std::vector<std::pair<std::size_t, std::string>> derived_uses_;   // first token after the declaration, name

    bool derived_constant(std::size_t s, std::size_t e) {
        std::size_t equals = e;
        int depth = 0;
        for (auto i = s; i < e; ++i) {
            const auto& t = text(i);
            if (depth == 0 && t == "[") return false;   // arrays keep their (constant) initializers
            if (t == "(" || t == "{" || t == "[") ++depth;
            else if (t == ")" || t == "}" || t == "]") --depth;
            else if (depth == 0 && t == "=") { equals = i; break; }
        }
        if (equals == e || equals <= s + 1 || T(equals - 1).kind != Token::Kind::identifier) return false;
        bool dependent = false;
        depth = 0;
        for (auto i = equals + 1; i < e; ++i) {
            const auto& t = text(i);
            if (t == "(" || t == "{" || t == "[") ++depth;
            else if (t == ")" || t == "}" || t == "]") --depth;
            else if (depth == 0 && t == ",") return false;   // several declarators
            if (T(i).kind == Token::Kind::identifier &&
                (t == "is_function_constant_defined" || constant_index(t) || derived_.count(t)))
                dependent = true;
        }
        if (!dependent) return false;
        const auto name = text(equals - 1);
        const auto function = "__msl_fc_derived_" + name;
        T(s).out = "inline";
        keep_identifier_.insert(sig_[s]);
        T(equals - 1).out = function + "() { return (";
        keep_identifier_.insert(sig_[equals - 1]);
        T(equals).removed = true;
        T(e).out = "); }";
        derived_[name] = function;
        derived_uses_.emplace_back(e + 1, name);
        return true;
    }

    void rename_derived_uses() {
        for (const auto& [from, name] : derived_uses_) {
            const auto call = derived_.at(name) + "()";
            for (auto i = from; i < sig_.size(); ++i) {
                if (T(i).kind != Token::Kind::identifier || text(i) != name || T(i).removed) continue;
                if (keep_identifier_.count(sig_[i])) continue;
                if (i > 0 && (is(i - 1, ".") || is(i - 1, "->") || is(i - 1, "::"))) continue;
                T(i).out = call;
                keep_identifier_.insert(sig_[i]);
            }
        }
    }

    void member_declaration(std::size_t s, std::size_t e) {
        // Skip member functions declared without bodies, using, etc.
        std::vector<Attribute> attributes;
        auto rest = strip_attributes(s, e, attributes);
        // Declarators: split at depth-0 commas.
        std::vector<std::vector<std::size_t>> parts(1);
        int depth = 0, angle = 0;
        for (const auto k : rest) {
            const auto& t = text(k);
            if (t == "(" || t == "[" || t == "{") ++depth;
            if (t == ")" || t == "]" || t == "}") --depth;
            if (t == "<") ++angle;
            if (t == ">") angle = std::max(0, angle - 1);
            if (depth == 0 && angle == 0 && t == ",") { parts.emplace_back(); continue; }
            parts.back().push_back(k);
        }
        if (rest.empty()) return;
        for (const auto k : rest) {
            if (!is(k, "(")) continue;
            declaration_params(s, e);   // member function declaration or ctor
            return;
        }
        std::string type;
        for (std::size_t p = 0; p < parts.size(); ++p) {
            const auto& part = parts[p];
            // Name: last identifier before '[', '=', ':' or the end.
            std::size_t name_at = part.size();
            for (std::size_t q = 0; q < part.size(); ++q) {
                const auto& t = text(part[q]);
                if (t == "[" || t == "=" || t == ":") break;
                if (T(part[q]).kind == Token::Kind::identifier) name_at = q;
            }
            if (name_at == part.size()) continue;
            if (p == 0) {
                for (std::size_t q = 0; q < name_at; ++q) type += (type.empty() ? "" : " ") + rename_word(text(part[q]));
            }
            Member member;
            member.name = text(part[name_at]);
            member.type = type;
            member.line = line_of(part[name_at]);
            for (std::size_t q = name_at + 1; q < part.size(); ++q) {
                if (is(part[q], "=")) break;
                member.array_suffix += text(part[q]);
            }
            // Attributes apply to the first declarator (MSL members declare one).
            if (p == 0) member.attributes = attributes;
            current_members_.back().push_back(std::move(member));
        }
    }

    void function_constant(const std::vector<std::size_t>& rest, const Attribute& attribute) {
        // constant <type> NAME [[function_constant(N)]];
        FunctionConstantDecl decl;
        if (attribute.args.size() != 1) {
            error(attribute.line, "function_constant takes one index");
            return;
        }
        const auto index = evaluate_integer(attribute.args[0]);
        if (!index) {
            error(attribute.line, "function_constant index must be an integer constant");
            return;
        }
        decl.index = *index;
        std::string type;
        std::size_t name_at = rest.size();
        for (std::size_t q = 0; q < rest.size(); ++q)
            if (T(rest[q]).kind == Token::Kind::identifier && !is(rest[q], "constant") && !is(rest[q], "const"))
                name_at = q;
        if (name_at == rest.size()) {
            error(attribute.line, "malformed function constant declaration");
            return;
        }
        for (std::size_t q = 0; q < name_at; ++q) {
            const auto& t = text(rest[q]);
            if (t == "constant" || t == "const") continue;
            type += (type.empty() ? "" : " ") + t;
        }
        decl.name = text(rest[name_at]);
        decl.type = resolve_spelling(type);
        if (decl.type.kind != Type::Kind::scalar) {
            error(attribute.line, "function constant '" + decl.name + "' of type '" + type +
                  "' is unsupported (only scalar function constants are implemented)");
            return;
        }
        for (const auto k : rest) T(k).removed = true;
        // Remove the trailing ';' too: the declaration is replaced, in place,
        // by an extern declaration outside the internal-linkage region,
        // followed by a line marker so later lines keep their numbers.
        const auto after = rest.back() + 1;
        if (is(after, ";")) T(after).removed = true;
        auto& first = T(rest.front());
        first.before += std::string("\n") + user_code_end + "extern __global const " + type + ' ' +
                        decl.name + " __asm__(\"msl.fc." + std::to_string(decl.index) + "\");\n" + user_code_begin +
                        "# " + std::to_string(first.line) + " \"" + files_[first.file] + "\"\n";
        constants_.push_back(std::move(decl));
    }

    std::optional<std::uint32_t> evaluate_integer(std::string_view value) const {
        std::string digits;
        for (const auto c : value) if (c != ' ') digits += c;
        while (!digits.empty() && (digits.back() == 'u' || digits.back() == 'U' || digits.back() == 'l' ||
                                   digits.back() == 'L')) digits.pop_back();
        if (digits.size() > 2 && digits[0] == '(' && digits.back() == ')') digits = digits.substr(1, digits.size() - 2);
        int base = 10;
        std::size_t offset = 0;
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
            base = 16;
            offset = 2;
        }
        std::uint32_t result = 0;
        const auto parsed = std::from_chars(digits.data() + offset, digits.data() + digits.size(), result, base);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || digits.empty()) return std::nullopt;
        return result;
    }

    // ---- functions ----------------------------------------------------------------------
    void function_definition(std::size_t s, std::size_t open, std::size_t close, const Scope& scope) {
        // Locate the parameter list: the first depth-0 '(' that is not alignas etc.
        std::size_t lparen = open;
        int angle = 0;
        for (auto i = s; i < open; ++i) {
            if (is(i, "[") && is(i + 1, "[")) { i = match_attribute_end(i); continue; }
            if (is(i, "<")) ++angle;
            else if (is(i, ">")) angle = std::max(0, angle - 1);
            else if (is(i, ">>")) angle = std::max(0, angle - 2);
            else if (is(i, "(") && angle == 0) {
                if (i > s && (is(i - 1, "alignas") || is(i - 1, "__attribute__") || is(i - 1, "decltype"))) {
                    i = match(i, "(", ")");
                    continue;
                }
                lparen = i;
                break;
            }
        }
        const auto rparen = lparen < open ? match(lparen, "(", ")") : open;
        // Stage keyword or attribute.
        std::optional<Stage> stage;
        std::vector<Attribute> attributes;
        std::size_t stage_token = sig_.size();
        if (!scope.in_class) {
            for (auto i = s; i < lparen; ++i) {
                if (is(i, "[") && is(i + 1, "[")) { i = match_attribute_end(i); continue; }
                if (T(i).kind != Token::Kind::identifier) continue;
                if (text(i) == "vertex") { stage = Stage::vertex; stage_token = i; }
                else if (text(i) == "fragment") { stage = Stage::fragment; stage_token = i; }
                else if (text(i) == "kernel") { stage = Stage::kernel; stage_token = i; }
                if (stage) break;
                if (text(i) != "static" && text(i) != "inline" && text(i) != "extern") break;
            }
        }
        // Function-level attributes in the prefix ([[vertex]], [[early_fragment_tests]], ...).
        auto prefix = strip_attributes(s, lparen, attributes);
        for (const auto& attribute : attributes) {
            if (attribute.name == "vertex") stage = Stage::vertex;
            else if (attribute.name == "fragment") stage = Stage::fragment;
            else if (attribute.name == "kernel") stage = Stage::kernel;
        }
        // Attributes after the parameter list ([[...]] before the body).
        if (rparen < open) strip_attributes(rparen + 1, open, attributes);
        if (stage) {
            PendingEntry entry;
            entry.stage = *stage;
            entry.path = scope.path;
            entry.line = line_of(s);
            entry.attributes = attributes;
            if (stage_token < sig_.size()) T(stage_token).removed = true;
            std::size_t name_at = lparen;
            for (auto k = prefix.rbegin(); k != prefix.rend(); ++k)
                if (*k < lparen && T(*k).kind == Token::Kind::identifier) { name_at = *k; break; }
            if (name_at == lparen) {
                error(entry.line, "cannot find the entry point name");
                return;
            }
            entry.name = text(name_at);
            for (const auto k : prefix) {
                if (k >= name_at || k == stage_token) continue;
                const auto& t = text(k);
                if (t == "static" || t == "inline") continue;
                entry.return_type += (entry.return_type.empty() ? "" : " ") + rename_word(t);
            }
            // Parameters.
            std::vector<std::size_t> current;
            std::vector<Attribute> current_attributes;
            int depth = 0, angles = 0;
            for (auto k = lparen + 1; k < rparen;) {
                if (is(k, "[") && is(k + 1, "[")) {
                    std::vector<Attribute> group;
                    const auto end = parse_attribute_group(k, group);
                    for (auto& attribute : group) current_attributes.push_back(std::move(attribute));
                    remove(k, end);
                    k = end;
                    continue;
                }
                const auto& t = text(k);
                if (t == "(" || t == "[" || t == "{") ++depth;
                if (t == ")" || t == "]" || t == "}") --depth;
                if (t == "<") ++angles;
                if (t == ">") angles = std::max(0, angles - 1);
                if (t == ">>") angles = std::max(0, angles - 2);
                if (depth == 0 && angles == 0 && t == ",") {
                    entry.params.push_back(std::move(current));
                    entry.param_attributes.push_back(std::move(current_attributes));
                    current.clear();
                    current_attributes.clear();
                    ++k;
                    continue;
                }
                current.push_back(k++);
            }
            if (!current.empty()) {
                entry.params.push_back(std::move(current));
                entry.param_attributes.push_back(std::move(current_attributes));
            }
            pending_entries_.push_back(std::move(entry));
        } else if (lparen < open) {
            if (duplicate_overload(s, open, close, scope)) return;
            array_params_to_pointers(lparen, rparen);
        }
        function_body(open + 1, close);
    }

    // ---- constant/device overloads ----------------------------------------------------------
    // MSL's constant and device address spaces both become __global, so
    // overloads that differ only in them (Tint's tint_array::operator[] pair
    // `const constant` / `const device`, a helper written once per address
    // space) would be redefinitions. A later definition whose head equals an
    // earlier one's after renaming is dropped when both bodies are the same
    // tokens: it would do the same thing on the same memory. Different bodies
    // are rejected explicitly.
    std::map<std::string, std::string> definitions_;

    bool duplicate_overload(std::size_t s, std::size_t open, std::size_t close, const Scope& scope) {
        std::string head, body;
        bool address_space = false;
        for (const auto& part : scope.path) head += part + "::";
        head += '|';
        for (auto k = s; k < open; ++k) {
            const auto& t = text(k);
            if (t == "constant" || t == "device") address_space = true;
            head += rename_word(t) + ' ';
        }
        if (!address_space) return false;
        for (auto k = open; k <= close; ++k) body += text(k) + ' ';
        const auto [found, inserted] = definitions_.emplace(std::move(head), body);
        if (inserted) return false;
        if (found->second != body) {
            error(line_of(s), "overloads that differ only in the constant and device address spaces have different "
                              "bodies (both address spaces are device memory here)");
            return true;
        }
        remove(s, close + 1);
        return true;
    }

    // ---- array parameters -------------------------------------------------------------------
    // `float4 offset[3]` as a parameter is `float4* offset` in C++, but clang's
    // OpenCL C++ decays it to a `__private` pointer, so an array member of a
    // shader class (reached through the generic `this`) cannot be passed
    // (Blender's SMAA). Array parameters become pointers (generic, as MSL's
    // `thread T*` parameters are): `T a[N]` -> `T* a`, `T a[N][M]` -> `T (*a)[M]`.
    void array_params_to_pointers(std::size_t lparen, std::size_t rparen) {
        std::size_t start = lparen + 1;
        int depth = 0, angle = 0;
        for (auto k = lparen + 1; k <= rparen && k < sig_.size(); ++k) {
            if (k == rparen || (depth == 0 && angle == 0 && is(k, ","))) {
                array_param_to_pointer(start, k);
                start = k + 1;
                continue;
            }
            const auto& t = text(k);
            if (t == "(" || t == "[" || t == "{") ++depth;
            else if (t == ")" || t == "]" || t == "}") --depth;
            else if (depth == 0 && t == "<") ++angle;
            else if (depth == 0 && t == ">") angle = std::max(0, angle - 1);
            else if (depth == 0 && t == ">>") angle = std::max(0, angle - 2);
        }
    }
    void array_param_to_pointer(std::size_t b, std::size_t e) {
        int depth = 0;
        for (auto k = b; k < e; ++k) {
            const auto& t = text(k);
            if (depth == 0 && t == "=") return;   // default argument
            if (t == "[" && is(k + 1, "[")) { k = match_attribute_end(k); continue; }
            if (t == "(" || t == "{") { ++depth; continue; }
            if (t == ")" || t == "}") { --depth; continue; }
            if (depth != 0 || t != "[" || k == b || T(k - 1).kind != Token::Kind::identifier || T(k).removed) continue;
            const auto close = match(k, "[", "]");
            if (close >= e) return;
            auto& name = T(k - 1);
            if (is(close + 1, "[")) {
                name.before = "(*" + name.before;
                name.after += ")";
            } else {
                name.before = "*" + name.before;
            }
            remove(k, close + 1);
            return;
        }
    }
    // A function declaration (prototype) statement: its parameter list.
    void declaration_params(std::size_t s, std::size_t e) {
        int angle = 0;
        for (auto i = s; i < e; ++i) {
            const auto& t = text(i);
            if (t == "=" ) return;   // a variable with an initializer
            if (t == "[" && is(i + 1, "[")) { i = match_attribute_end(i); continue; }
            if (t == "<") ++angle;
            else if (t == ">") angle = std::max(0, angle - 1);
            else if (t == ">>") angle = std::max(0, angle - 2);
            else if (t == "(" && angle == 0) {
                if (i == s || T(i - 1).kind != Token::Kind::identifier || is(i - 1, "alignas") ||
                    is(i - 1, "__attribute__") || is(i - 1, "decltype") || is(i - 1, "sizeof") ||
                    is(i - 1, "__declspec"))
                    return;
                const auto close = match(i, "(", ")");
                if (close < e) array_params_to_pointers(i, close);
                return;
            }
        }
    }

    // Function-scope threadgroup variables: `threadgroup T name[N];`.
    void function_body(std::size_t b, std::size_t e) {
        for (auto i = b; i < e; ++i) {
            if (!is(i, "threadgroup")) continue;
            const bool statement_start = i == b || is(i - 1, ";") || is(i - 1, "{") || is(i - 1, "}") ||
                                         is(i - 1, "static");
            if (!statement_start) continue;
            std::size_t end = i;
            int depth = 0;
            while (end < e) {
                if (is(end, "(") || is(end, "[") || is(end, "{")) ++depth;
                if (is(end, ")") || is(end, "]") || is(end, "}")) --depth;
                if (depth == 0 && is(end, ";")) break;
                ++end;
            }
            if (end >= e) return;
            // Declarator name: the last identifier at bracket depth 0 before '[' or ';'.
            std::size_t name_at = end;
            bool pointer = false, initialized = false, multiple = false;
            int angle = 0;
            for (auto k = i + 1; k < end; ++k) {
                const auto& t = text(k);
                if (t == "<") ++angle;
                else if (t == ">") angle = std::max(0, angle - 1);
                else if (t == ">>") angle = std::max(0, angle - 2);
                if (angle > 0) continue;
                if (t == "*" || t == "&") pointer = true;
                if (t == "=") initialized = true;
                if (t == ",") multiple = true;
                if (t == "[") break;
                if (T(k).kind == Token::Kind::identifier) name_at = k;
            }
            if (pointer || name_at == end) { i = end; continue; }
            if (initialized) {
                error(line_of(i), "threadgroup variables cannot have initializers");
                i = end;
                continue;
            }
            if (multiple) {
                error(line_of(i), "declare one threadgroup variable per statement");
                i = end;
                continue;
            }
            const auto id = threadgroup_ids_++;
            const std::string alias = "__msl_tg_t" + std::to_string(id);
            std::string type, suffix;
            for (auto k = i + 1; k < name_at; ++k) type += rename_word(text(k)) + " ";
            for (auto k = name_at + 1; k < end; ++k) suffix += text(k);
            const auto name = text(name_at);
            remove(i, end);
            T(i).removed = false;
            T(i).out = "typedef " + type + alias + suffix + "; __local " + alias + "& " + name + " = *(__local " + alias +
                       "*)__msl_threadgroup_storage(" + std::to_string(id) + "u, sizeof(" + alias + "), alignof(" +
                       alias + "))";
            keep_identifier_.insert(sig_[i]);
            i = end;
        }
    }

    // ---- entry points -------------------------------------------------------------------
    const StructDef* find_struct(std::string type) const {
        // Strip qualifiers to the (qualified) name.
        for (const auto* word : {"const ", "struct ", "__global ", "volatile "}) {
            std::size_t at;
            while ((at = type.find(word)) != std::string::npos) type.erase(at, std::string_view(word).size());
        }
        std::string clean;
        for (const auto c : type) if (c != ' ' && c != '&') clean += c;
        if (clean.rfind("::", 0) == 0) clean = clean.substr(2);
        const StructDef* best = nullptr;
        for (const auto& def : structs_) {
            if (def.qualified == clean) return &def;
            const auto suffix = "::" + clean;
            if (def.name == clean || (def.qualified.size() > suffix.size() &&
                                      def.qualified.compare(def.qualified.size() - suffix.size(), suffix.size(), suffix) == 0)) {
                if (!best) best = &def;
            }
            const auto last = clean.rfind("::");
            if (!best && last != std::string::npos && def.name == clean.substr(last + 2)) best = &def;
        }
        return best;
    }

    // The members of an argument buffer of samplers: a struct whose members
    // all carry [[id(n)]] and are `sampler`, `array<sampler, N>` or
    // `sampler name[N]` (`access`: 0, 1, 2 per member). nullopt when the
    // struct is no such argument buffer (no sampler member with an [[id]]);
    // samplers mixed with other members are an error (`failed`).
    std::optional<std::vector<ArgumentBufferSamplers::Member>> sampler_argument_buffer(
        const StructDef& def, std::uint32_t line, std::vector<std::pair<int, std::string>>& access, bool& failed) {
        std::vector<ArgumentBufferSamplers::Member> members;
        std::vector<std::string> others;
        for (const auto& member : def.members) {
            std::string type;
            for (const auto c : member.type) if (c != ' ') type += c;
            for (std::size_t at; (at = type.find("metal::")) != std::string::npos;) type.erase(at, 7);
            std::uint32_t count = 0;
            int kind = 0;
            if (type == "sampler") {
                count = 1;
                if (member.array_suffix.size() > 2 && member.array_suffix.front() == '[' && member.array_suffix.back() == ']') {
                    count = evaluate_integer(member.array_suffix.substr(1, member.array_suffix.size() - 2)).value_or(0);
                    kind = 2;
                }
            } else if (type.rfind("array<sampler,", 0) == 0 && type.back() == '>' && member.array_suffix.empty()) {
                count = evaluate_integer(type.substr(14, type.size() - 15)).value_or(0);
                kind = 1;
            }
            const auto* id = find_attribute(member.attributes, "id");
            if (!count || !id || id->args.empty() || !evaluate_integer(id->args[0])) {
                others.push_back(member.name);
                continue;
            }
            members.push_back({member.name, *evaluate_integer(id->args[0]), count});
            access.emplace_back(kind, member.name);
        }
        if (members.empty()) return std::nullopt;
        if (!others.empty()) {
            error(line, "argument buffer '" + def.name + "' holds samplers and '" + others.front() +
                  "': only argument buffers of samplers ([[id(n)]] sampler / array<sampler, N> members) are supported");
            failed = true;
            return std::nullopt;
        }
        return members;
    }

    // An argument buffer of buffer pointers and constant data (ANGLE's
    // uniform-block argument buffer, design): every member carries [[id(n)]]
    // and is a pointer (`constant Block* p [[id(0)]]`, arrays `[N]` or
    // `array<T*, N>`) or a scalar/vector/matrix. The shader reads the struct
    // from the buffer as it is; the reflection gives MTLArgumentEncoder the
    // layout, computed here with the MSL size rules and checked against
    // clang's by static_asserts in the wrappers. false when the struct has no
    // [[id]] member; anything else in it is an error (`failed`).
    bool buffer_argument_buffer(const StructDef& def, InterfaceBuffer& buffer, bool& failed) {
        bool any = false;
        for (const auto& member : def.members) any = any || find_attribute(member.attributes, "id");
        if (!any) return false;
        auto scalar_size = [](Scalar s) -> std::uint32_t {
            switch (s) {
            case Scalar::boolean: case Scalar::int8: case Scalar::uint8: return 1;
            case Scalar::int16: case Scalar::uint16: case Scalar::half: case Scalar::bfloat: return 2;
            case Scalar::int64: case Scalar::uint64: return 8;
            case Scalar::none: return 0;
            default: return 4;
            }
        };
        std::uint32_t offset = 0, alignment = 1;
        std::ostringstream checks;
        for (const auto& member : def.members) {
            const auto* id = find_attribute(member.attributes, "id");
            const auto value = id && !id->args.empty() ? evaluate_integer(id->args[0]) : std::nullopt;
            auto fail = [&](const std::string& why) {
                error(member.line, "argument buffer '" + def.name + "' member '" + member.name + "' " + why);
                failed = true;
            };
            if (!value) { fail("needs a constant [[id(n)]]"); return false; }
            std::string type;
            for (const auto c : member.type) if (c != ' ') type += c;
            for (std::size_t at; (at = type.find("metal::")) != std::string::npos;) type.erase(at, 7);
            InterfaceBuffer::Argument argument;
            argument.name = member.name;
            argument.id = *value;
            if (member.array_suffix.size() > 2 && member.array_suffix.front() == '[' && member.array_suffix.back() == ']') {
                const auto count = evaluate_integer(member.array_suffix.substr(1, member.array_suffix.size() - 2));
                if (!count || !*count) { fail("needs a constant array length"); return false; }
                argument.count = *count;
            } else if (!member.array_suffix.empty()) {
                fail("has an unsupported declarator '" + member.array_suffix + "'");
                return false;
            } else if (type.rfind("array<", 0) == 0 && type.back() == '>') {
                const auto comma = type.rfind(',');
                const auto count = comma == std::string::npos ? std::nullopt
                    : evaluate_integer(type.substr(comma + 1, type.size() - comma - 2));
                if (!count || !*count) { fail("needs a constant array length"); return false; }
                argument.count = *count;
                type = type.substr(6, comma - 6);
            }
            std::uint32_t size = 8, align = 8;
            argument.pointer = !type.empty() && type.back() == '*';
            if (!argument.pointer) {
                argument.type = resolve_spelling(member.type);
                const auto& t = argument.type;
                const auto element = scalar_size(t.scalar);
                if (t.kind == Type::Kind::scalar) {
                    size = align = element;
                } else if (t.kind == Type::Kind::vector) {
                    align = t.packed ? element : element * (t.components == 3 ? 4 : t.components);
                    size = t.packed ? element * t.components : align;
                } else if (t.kind == Type::Kind::matrix) {
                    align = element * (t.components == 3 ? 4 : t.components);
                    size = align * t.columns;
                } else {
                    fail("of type '" + member.type + "' is unsupported (argument buffers hold buffer pointers and "
                         "scalar/vector/matrix data here; textures and samplers need bindless descriptors)");
                    return false;
                }
                if (!element) { fail("has an unknown scalar type"); return false; }
            }
            offset = (offset + align - 1) / align * align;
            argument.offset = offset;
            argument.stride = size;
            offset += size * argument.count;
            alignment = std::max(alignment, align);
            checks << "static_assert(__builtin_offsetof(::" << def.qualified << ", " << member.name << ") == "
                   << argument.offset << ", \"argument buffer layout: " << def.name << "::" << member.name << "\");\n";
            buffer.arguments.push_back(std::move(argument));
        }
        buffer.argument_buffer = true;
        buffer.argument_alignment = alignment;
        buffer.argument_length = (offset + alignment - 1) / alignment * alignment;
        checks << "static_assert(sizeof(::" << def.qualified << ") == " << buffer.argument_length
               << ", \"argument buffer size: " << def.name << "\");\n";
        wrappers_ << checks.str();
        return true;
    }

    static const Attribute* find_attribute(const std::vector<Attribute>& attributes, std::string_view name) {
        for (const auto& attribute : attributes) if (attribute.name == name) return &attribute;
        return nullptr;
    }

    std::optional<Interpolation> interpolation_of(const std::vector<Attribute>& attributes) const {
        static const std::map<std::string_view, Interpolation> map = {
            {"flat", Interpolation::flat}, {"center_perspective", Interpolation::perspective},
            {"center_no_perspective", Interpolation::no_perspective},
            {"centroid_perspective", Interpolation::centroid_perspective},
            {"centroid_no_perspective", Interpolation::centroid_no_perspective},
            {"sample_perspective", Interpolation::sample_perspective},
            {"sample_no_perspective", Interpolation::sample_no_perspective}};
        for (const auto& attribute : attributes) {
            const auto found = map.find(attribute.name);
            if (found != map.end()) return found->second;
        }
        return std::nullopt;
    }

    std::optional<std::uint32_t> constant_index(const std::string& name) const {
        for (const auto& constant : constants_) if (constant.name == name) return constant.index;
        return std::nullopt;
    }

    // C++ condition for a [[function_constant(name)]] member or parameter.
    std::string constant_condition(const Attribute& attribute, std::optional<std::uint32_t>& index) {
        const auto name = attribute.args.empty() ? std::string{} : attribute.args[0];
        if (const auto derived = derived_.find(name); derived != derived_.end()) {
            index.reset();
            return "(" + derived->second + "())";
        }
        index = constant_index(name);
        if (!index) {
            error(attribute.line, "unknown function constant '" + name + "'");
            return "false";
        }
        for (const auto& constant : constants_)
            if (constant.index == *index && constant.type.scalar == Scalar::boolean)
                return "(__msl_fc_defined(" + std::to_string(*index) + "u) && " + name + ")";
        return "__msl_fc_defined(" + std::to_string(*index) + "u)";
    }

    std::string param_text(const std::vector<std::size_t>& tokens, std::size_t from, std::size_t to) const {
        std::string out;
        for (auto q = from; q < to; ++q) {
            const auto word = rename_word(text(tokens[q]));
            if (word.empty()) continue;
            out += (out.empty() ? "" : " ") + word;
        }
        return out;
    }

    void build_entry(PendingEntry& pending) {
        EntryPoint entry;
        entry.stage = pending.stage;
        entry.name = pending.name;
        entry.line = pending.line;
        entry.wrapper = "__msl_entry_" + std::to_string(entries_.size());
        for (const auto& attribute : pending.attributes) {
            if (attribute.name == "early_fragment_tests") entry.early_fragment_tests = true;
            else if (attribute.name == "max_total_threads_per_threadgroup" && !attribute.args.empty())
                entry.max_total_threads_per_threadgroup = evaluate_integer(attribute.args[0]).value_or(0);
            else if (attribute.name == "host_name" && !attribute.args.empty()) {
                auto name = attribute.args[0];
                name.erase(std::remove(name.begin(), name.end(), '"'), name.end());
                entry.name = name;
            } else if (attribute.name == "visible" || attribute.name == "stitchable" || attribute.name == "patch" ||
                       attribute.name == "object" || attribute.name == "mesh" || attribute.name == "intersection")
                error(attribute.line, "[[" + attribute.name + "]] functions are unsupported");
            else if (attribute.name == "required_threads_per_threadgroup")
                entry.notes.push_back("required_threads_per_threadgroup is ignored");
        }
        std::string qualified;
        for (const auto& part : pending.path) qualified += part + "::";
        std::ostringstream body;
        // Kernels: Vulkan runs whole workgroups, so the threads past a
        // non-uniform dispatchThreads: grid return before anything else.
        if (entry.stage == Stage::kernel) body << "    if (__msl_kernel_outside_grid()) return;\n";
        std::vector<std::string> args;
        std::uint32_t next_input = 0;
        for (std::size_t p = 0; p < pending.params.size(); ++p) {
            const auto& tokens = pending.params[p];
            const auto& attributes = pending.param_attributes[p];
            const auto line = tokens.empty() ? pending.line : line_of(tokens.front());
            if (tokens.empty()) continue;
            if (tokens.size() == 1 && is(tokens[0], "void")) continue;
            // Name: last identifier (before an array suffix or default value).
            std::size_t name_at = tokens.size(), stop = tokens.size();
            for (std::size_t q = 0; q < tokens.size(); ++q) {
                if (is(tokens[q], "=") || is(tokens[q], "[")) { stop = q; break; }
                if (T(tokens[q]).kind == Token::Kind::identifier) name_at = q;
            }
            if (name_at == tokens.size()) {
                error(line, "unnamed entry point parameter");
                continue;
            }
            const auto name = text(tokens[name_at]);
            const auto type = param_text(tokens, 0, name_at);
            const auto var = "__p" + std::to_string(p);
            std::optional<std::uint32_t> condition_index;
            std::string condition;
            if (const auto* fc = find_attribute(attributes, "function_constant")) condition = constant_condition(*fc, condition_index);
            (void)stop;
            bool handled = false;
            for (const auto& attribute : attributes) {
                const auto& kind = attribute.name;
                if (kind == "function_constant" || kind == "flat" || kind == "center_perspective" ||
                    kind == "center_no_perspective")
                    continue;
                handled = true;
                if (kind == "buffer") {
                    InterfaceBuffer buffer;
                    buffer.name = name;
                    buffer.function_constant = condition_index;
                    const auto expression = attribute.args.empty() ? std::string{} : attribute.args[0];
                    if (const auto literal = evaluate_integer(expression)) {
                        buffer.index = *literal;
                    } else {
                        // name, name + n, n + name
                        const auto& arg = attribute.arg_tokens.empty() ? std::vector<std::size_t>{} : attribute.arg_tokens[0];
                        std::optional<std::uint32_t> constant;
                        std::uint32_t offset = 0;
                        bool ok = !arg.empty();
                        for (std::size_t q = 0; q < arg.size() && ok; ++q) {
                            const auto& t = text(arg[q]);
                            if (t == "+") continue;
                            if (t == "(" || t == ")") continue;
                            if (T(arg[q]).kind == Token::Kind::identifier && !constant) {
                                constant = constant_index(t);
                                ok = constant.has_value();
                            } else if (const auto n = evaluate_integer(t)) {
                                offset += *n;
                            } else {
                                ok = false;
                            }
                        }
                        if (!ok || !constant) {
                            error(attribute.line, "buffer index '" + expression +
                                  "' must be an integer or function constant + integer");
                            continue;
                        }
                        buffer.index = offset;
                        buffer.index_constant = constant;
                    }
                    if (buffer.index >= maximum_buffers) {
                        error(attribute.line, "buffer index " + std::to_string(buffer.index) + " out of range");
                        continue;
                    }
                    std::string pointee = type;
                    buffer.reference = !pointee.empty() && pointee.back() == '&';
                    const bool pointer = !pointee.empty() && pointee.back() == '*';
                    if (!buffer.reference && !pointer) {
                        error(attribute.line, "buffer parameter '" + name + "' must be a pointer or reference");
                        continue;
                    }
                    pointee.pop_back();
                    while (!pointee.empty() && pointee.back() == ' ') pointee.pop_back();
                    // Address space and constness from the original spelling.
                    for (std::size_t q = 0; q < name_at; ++q) {
                        if (is(tokens[q], "constant")) buffer.space = BufferBinding::Space::constant;
                        if (is(tokens[q], "const")) buffer.is_const = true;
                    }
                    auto clean = pointee;
                    for (const auto* word : {"__global ", "const ", "volatile "}) {
                        std::size_t at;
                        while ((at = clean.find(word)) != std::string::npos) clean.erase(at, std::string_view(word).size());
                    }
                    buffer.pointee = clean;
                    if (const auto* def = find_struct(clean)) {
                        std::vector<std::pair<int, std::string>> access;   // member kind, name
                        bool failed = false;
                        if (auto members = sampler_argument_buffer(*def, attribute.line, access, failed)) {
                            // The samplers come from the k-th argument-buffer
                            // binding: the wrapper passes a static struct whose
                            // sampler handles name its elements.
                            InterfaceArgumentBuffer ab;
                            ab.index = buffer.index;
                            ab.index_constant = buffer.index_constant;
                            ab.name = name;
                            ab.type = def->qualified;
                            ab.members = std::move(*members);
                            for (const auto& member : ab.members) ab.count = std::max(ab.count, member.id + member.count);
                            const auto k = static_cast<std::uint64_t>(entry.argument_buffers.size());
                            const auto global = "__msl_ab_" + std::to_string(entries_.size()) + "_" + std::to_string(k);
                            wrappers_ << "static constexpr ::" << def->qualified << ' ' << global << "_make() {\n    ::"
                                      << def->qualified << " s{};\n";
                            for (std::size_t m = 0; m < ab.members.size(); ++m) {
                                const auto& member = ab.members[m];
                                for (std::uint32_t j = 0; j < member.count; ++j) {
                                    const auto handle = argument_buffer_sampler_flag | (k << 32) | (member.id + j);
                                    wrappers_ << "    s." << member.name;
                                    if (access[m].first == 1) wrappers_ << ".__elems[" << j << ']';
                                    if (access[m].first == 2) wrappers_ << '[' << j << ']';
                                    wrappers_ << ".__h = " << handle << "ul;\n";
                                }
                            }
                            wrappers_ << "    return s;\n}\nstatic __global const ::" << def->qualified << ' ' << global
                                      << " = " << global << "_make();\n";
                            args.push_back(buffer.reference ? "const_cast<" + pointee + "&>(" + global + ")"
                                                            : "const_cast<" + pointee + "*>(&" + global + ")");
                            entry.argument_buffers.push_back(std::move(ab));
                            continue;
                        }
                        if (failed) continue;
                        buffer_argument_buffer(*def, buffer, failed);
                        if (failed) continue;
                    }
                    const auto address = "__msl_buffer(" + (buffer.index_constant ? "(uint)(" + expression + ")"
                        : std::to_string(buffer.index) + "u") + ")";
                    body << "    " << pointee << "* " << var << " = (" << pointee << "*)" << address << ";\n";
                    args.push_back(buffer.reference ? "*" + var : var);
                    entry.buffers.push_back(std::move(buffer));
                } else if (kind == "texture") {
                    InterfaceTexture texture;
                    texture.name = name;
                    texture.function_constant = condition_index;
                    texture.index = attribute.args.empty() ? 0 : evaluate_integer(attribute.args[0]).value_or(~0u);
                    texture.type = resolve_spelling(type);
                    if (texture.index >= maximum_textures || texture.type.kind != Type::Kind::texture) {
                        error(attribute.line, "texture parameter '" + name + "' needs a texture type and a constant index");
                        continue;
                    }
                    if (texture.type.array_length) {
                        body << "    " << type << ' ' << var << ";\n    for (uint __i = 0; __i < " << texture.type.array_length
                             << "u; ++__i) " << var << "[__i].__h = (" << texture.index << "u << 16) | __i;\n";
                    } else {
                        body << "    " << type << ' ' << var << " = metal::__msl_texture<metal::remove_cv_t<" << type
                             << ">>(" << texture.index << "u);\n";
                    }
                    args.push_back(var);
                    entry.textures.push_back(std::move(texture));
                } else if (kind == "sampler") {
                    InterfaceSampler sampler;
                    sampler.name = name;
                    sampler.index = attribute.args.empty() ? 0 : evaluate_integer(attribute.args[0]).value_or(~0u);
                    if (sampler.index >= maximum_samplers) {
                        error(attribute.line, "sampler index out of range");
                        continue;
                    }
                    body << "    metal::sampler " << var << " = metal::__msl_sampler(" << sampler.index << "u);\n";
                    args.push_back(var);
                    entry.samplers.push_back(std::move(sampler));
                } else if (kind == "threadgroup") {
                    if (entry.stage != Stage::kernel) {
                        error(attribute.line, "[[threadgroup]] parameters are only valid in kernels");
                        continue;
                    }
                    InterfaceThreadgroup group;
                    group.name = name;
                    group.index = attribute.args.empty() ? 0 : evaluate_integer(attribute.args[0]).value_or(0);
                    std::string pointee = type;
                    if (pointee.empty() || (pointee.back() != '*' && pointee.back() != '&')) {
                        error(attribute.line, "threadgroup parameter '" + name + "' must be a pointer or reference");
                        continue;
                    }
                    const bool reference = pointee.back() == '&';
                    pointee.pop_back();
                    group.element = pointee;
                    body << "    " << pointee << "* " << var << " = (" << pointee << "*)__msl_threadgroup_param("
                         << group.index << "u);\n";
                    args.push_back(reference ? "*" + var : var);
                    entry.threadgroups.push_back(std::move(group));
                } else if (kind == "stage_in") {
                    if (entry.stage == Stage::kernel) {
                        error(attribute.line, "kernel [[stage_in]] is unsupported");
                        continue;
                    }
                    const auto* def = find_struct(type);
                    if (!def) {
                        error(attribute.line, "cannot find the [[stage_in]] struct '" + type + "'");
                        continue;
                    }
                    body << "    " << type << ' ' << var << ";\n";
                    stage_in(entry, *def, var, body, next_input);
                    args.push_back(var);
                } else if (const auto found = builtin_attributes().find(kind); found != builtin_attributes().end()) {
                    if (!valid_builtin(entry.stage, found->second, true)) {
                        error(attribute.line, "[[" + kind + "]] is not an input of this stage");
                        continue;
                    }
                    const auto* natural = builtin_input_type(found->second);
                    body << "    auto " << var << " = metal::__msl_builtin_in<" << type << ", " << raw_type(natural)
                         << ">(" << static_cast<std::uint32_t>(found->second) << "u);\n";
                    args.push_back(var);
                    entry.builtin_inputs.push_back({found->second, name});
                } else if (kind == "color" && entry.stage == Stage::fragment) {
                    // Framebuffer fetch: `float4 c [[color(n)]]`.
                    const auto color = attribute.args.empty() ? std::optional<std::uint32_t>{} : evaluate_integer(attribute.args[0]);
                    if (!color || *color >= 8) {
                        error(attribute.line, "[[color(n)]] input '" + name + "' needs a constant n < 8");
                        continue;
                    }
                    body << "    " << type << ' ' << var << " = metal::__msl_color_in<" << type << ">(" << *color << "u);\n";
                    args.push_back(var);
                    entry.color_inputs.push_back({*color, 0, resolve_spelling(type), name});
                } else if (kind == "raster_order_group") {
                    handled = false;
                    continue;
                } else {
                    error(attribute.line, "[[" + kind + "]] parameters are unsupported");
                }
                break;
            }
            if (!handled && entry.stage == Stage::fragment) {
                // A struct of [[color(n)]] members (Blender's FragmentTileIn):
                // framebuffer fetch of each.
                if (const auto* def = find_struct(type); def && !def->members.empty()) {
                    bool all = true;
                    for (const auto& member : def->members) all = all && find_attribute(member.attributes, "color");
                    if (all) {
                        handled = true;
                        body << "    " << type << ' ' << var << ";\n";
                        for (const auto& member : def->members) {
                            const auto* attribute = find_attribute(member.attributes, "color");
                            const auto color = attribute->args.empty() ? std::optional<std::uint32_t>{}
                                                                       : evaluate_integer(attribute->args[0]);
                            if (!color || *color >= 8 || !member.array_suffix.empty()) {
                                error(member.line, "[[color(n)]] input member '" + member.name + "' needs a constant n < 8");
                                continue;
                            }
                            body << "    " << var << '.' << member.name << " = metal::__msl_color_in<decltype(" << var << '.'
                                 << member.name << ")>(" << *color << "u);\n";
                            entry.color_inputs.push_back({*color, 0, resolve_spelling(member.type), member.name});
                        }
                        args.push_back(var);
                    }
                }
            }
            if (!handled) error(line, "entry point parameter '" + name + "' has no MSL attribute");
        }
        // Call and outputs.
        const bool returns = !pending.return_type.empty() && pending.return_type != "void";
        body << "    " << (returns ? "auto __r = " : "") << qualified << pending.name << '(';
        for (std::size_t a = 0; a < args.size(); ++a) body << (a ? ", " : "") << args[a];
        body << ");\n";
        if (returns) outputs(entry, pending.return_type, body);
        wrappers_ << "extern \"C\" void " << entry.wrapper << "() {\n" << body.str() << "}\n";
        entries_.push_back(std::move(entry));
    }

    void stage_in(EntryPoint& entry, const StructDef& def, const std::string& var, std::ostringstream& body,
                  std::uint32_t& next_input) {
        for (const auto& member : def.members) {
            const auto access = var + "." + member.name;
            std::optional<std::uint32_t> condition_index;
            std::string condition;
            if (const auto* fc = find_attribute(member.attributes, "function_constant"))
                condition = constant_condition(*fc, condition_index);
            const auto guarded = [&](const std::string& statement) {
                body << "    " << (condition.empty() ? statement : "if (" + condition + ") " + statement) << '\n';
            };
            if (entry.stage == Stage::vertex) {
                const auto* attribute = find_attribute(member.attributes, "attribute");
                if (!attribute) {
                    error(member.line, "vertex [[stage_in]] member '" + member.name + "' has no [[attribute(n)]]");
                    continue;
                }
                InterfaceVarying varying;
                varying.name = member.name;
                varying.member = member.name;
                varying.location = attribute->args.empty() ? 0 : evaluate_integer(attribute->args[0]).value_or(0);
                varying.type = resolve_spelling(member.type);
                varying.function_constant = condition_index;
                if (!member.array_suffix.empty()) {
                    error(member.line, "array vertex attributes are unsupported");
                    continue;
                }
                guarded(access + " = metal::__msl_in<decltype(" + access + ")>(" + std::to_string(varying.location) + "u, 0u);");
                entry.attributes.push_back(std::move(varying));
                continue;
            }
            // Fragment: builtins or varyings.
            bool builtin = false;
            for (const auto& attribute : member.attributes) {
                const auto found = builtin_attributes().find(attribute.name);
                if (found == builtin_attributes().end()) continue;
                builtin = true;
                if (found->second == Builtin::point_size || found->second == Builtin::clip_distance) break;   // vertex-only
                if (!valid_builtin(Stage::fragment, found->second, true)) {
                    error(attribute.line, "[[" + attribute.name + "]] is not a fragment input");
                    break;
                }
                const auto* natural = builtin_input_type(found->second);
                guarded(access + " = metal::__msl_builtin_in<decltype(" + access + "), " + raw_type(natural) + ">(" +
                        std::to_string(static_cast<std::uint32_t>(found->second)) + "u);");
                entry.builtin_inputs.push_back({found->second, member.name});
                break;
            }
            if (builtin) continue;
            if (find_attribute(member.attributes, "color")) {
                error(member.line, "framebuffer fetch ([[color(n)]] fragment inputs) is unsupported");
                continue;
            }
            if (find_attribute(member.attributes, "barycentric_coord")) {
                error(member.line, "[[barycentric_coord]] is unsupported");
                continue;
            }
            if (!member.array_suffix.empty()) {
                error(member.line, "array varyings are unsupported");
                continue;
            }
            InterfaceVarying varying;
            varying.member = member.name;
            varying.name = member.name;
            if (const auto* user = find_attribute(member.attributes, "user"); user && !user->args.empty())
                varying.name = user->args[0];
            varying.location = next_input++;
            varying.type = resolve_spelling(member.type);
            varying.interpolation = interpolation_of(member.attributes).value_or(Interpolation::perspective);
            varying.function_constant = condition_index;
            guarded(access + " = metal::__msl_in<decltype(" + access + ")>(" + std::to_string(varying.location) + "u, " +
                    std::to_string(static_cast<std::uint32_t>(varying.interpolation)) + "u);");
            entry.inputs.push_back(std::move(varying));
        }
    }

    void outputs(EntryPoint& entry, const std::string& return_type, std::ostringstream& body) {
        const auto spelling = resolve_spelling(return_type);
        if (entry.stage == Stage::kernel) {
            error(entry.line, "kernel functions must return void");
            return;
        }
        if (spelling.kind == Type::Kind::scalar || spelling.kind == Type::Kind::vector) {
            if (entry.stage == Stage::vertex) {
                body << "    metal::__msl_builtin_out(" << static_cast<std::uint32_t>(Builtin::position) << "u, 0u, __r);\n";
                entry.builtin_outputs.push_back({Builtin::position, "return"});
            } else {
                body << "    metal::__msl_color(0u, 0u, __r);\n";
                entry.colors.push_back({0, 0, spelling, "return"});
            }
            return;
        }
        const auto* def = find_struct(return_type);
        if (!def) {
            error(entry.line, "cannot find the return struct '" + return_type + "' of '" + entry.name + "'");
            return;
        }
        std::uint32_t next_output = 0;
        for (const auto& member : def->members) {
            const auto access = "__r." + member.name;
            std::optional<std::uint32_t> condition_index;
            std::string condition;
            if (const auto* fc = find_attribute(member.attributes, "function_constant"))
                condition = constant_condition(*fc, condition_index);
            bool done = false;
            for (const auto& attribute : member.attributes) {
                const auto& kind = attribute.name;
                const auto found = builtin_attributes().find(kind);
                if (found != builtin_attributes().end()) {
                    done = true;
                    const auto builtin = found->second;
                    if (entry.stage == Stage::fragment && builtin == Builtin::sample_mask) {
                        // Absent (its function constant false): all samples
                        // stay covered, as when Metal has no such output.
                        entry.writes_sample_mask = true;
                        body << "    metal::__msl_builtin_out(" << builtin_sample_mask_out << "u, 0u, "
                             << (condition.empty() ? access : "(" + condition + ") ? uint(" + access + ") : 0xffffffffu")
                             << ");\n";
                        break;
                    }
                    if (!valid_builtin(entry.stage, builtin, false)) {
                        error(attribute.line, "[[" + kind + "]] is not an output of this stage");
                        break;
                    }
                    auto id = static_cast<std::uint32_t>(builtin);
                    if (builtin == Builtin::position && find_attribute(member.attributes, "invariant")) {
                        id |= builtin_invariant_flag;
                        entry.invariant_position = true;
                    }
                    const auto neutral = builtin == Builtin::clip_distance ? "1.0f" : builtin == Builtin::point_size
                        ? "1.0f" : "0";
                    if (!member.array_suffix.empty()) {
                        body << "    for (uint __i = 0; __i < sizeof(" << access << ") / sizeof(" << access
                             << "[0]); ++__i) metal::__msl_builtin_out(" << id << "u, __i, "
                             << (condition.empty() ? access + "[__i]" : "(" + condition + ") ? " + access +
                                 "[__i] : decltype(" + access + "[0])(" + neutral + ")") << ");\n";
                    } else if (builtin == Builtin::clip_distance) {
                        // A scalar, or an array object (metal::array, Tint's
                        // tint_array<float, N>): element by element.
                        const auto statement = "metal::__msl_builtin_out_elements(" + std::to_string(id) + "u, " + access + ");";
                        body << "    " << (condition.empty() ? statement : "if (" + condition + ") " + statement +
                                 " else metal::__msl_builtin_out_neutral(" + std::to_string(id) + "u, " + access + ", " +
                                 neutral + ");") << '\n';
                    } else {
                        body << "    metal::__msl_builtin_out(" << id << "u, 0u, "
                             << (condition.empty() ? access : "(" + condition + ") ? " + access + " : decltype(" +
                                 access + ")(" + neutral + ")") << ");\n";
                    }
                    entry.builtin_outputs.push_back({builtin, member.name});
                    break;
                }
                if (kind == "color" && entry.stage == Stage::fragment) {
                    done = true;
                    EntryPoint::Color color;
                    color.color = attribute.args.empty() ? 0 : evaluate_integer(attribute.args[0]).value_or(0);
                    if (const auto* index = find_attribute(member.attributes, "index"); index && !index->args.empty())
                        color.index = evaluate_integer(index->args[0]).value_or(0);
                    color.type = resolve_spelling(member.type);
                    color.name = member.name;
                    const auto statement = "metal::__msl_color(" + std::to_string(color.color) + "u, " +
                                           std::to_string(color.index) + "u, " + access + ");";
                    body << "    " << (condition.empty() ? statement : "if (" + condition + ") " + statement) << '\n';
                    entry.colors.push_back(std::move(color));
                    break;
                }
                if (kind == "depth" && entry.stage == Stage::fragment) {
                    done = true;
                    const auto mode = attribute.args.empty() ? std::string("any") : attribute.args[0];
                    entry.depth = mode == "greater" ? DepthOutput::greater : mode == "less" ? DepthOutput::less
                                                                                             : DepthOutput::any;
                    // Absent (its function constant false): the fragment's
                    // own depth, as when Metal has no depth output. Vulkan
                    // needs FragDepth written on every path once declared.
                    const auto value = condition.empty() ? access
                        : "(" + condition + ") ? float(" + access + ") : metal::__msl_builtin_in<metal::float4, " +
                          raw_type(builtin_input_type(Builtin::position)) + ">(" +
                          std::to_string(static_cast<std::uint32_t>(Builtin::position)) + "u).z";
                    body << "    metal::__msl_builtin_out(" << builtin_frag_depth << "u, "
                         << static_cast<std::uint32_t>(entry.depth) << "u, " << value << ");\n";
                    break;
                }
                if (kind == "stencil" && entry.stage == Stage::fragment) {
                    done = true;
                    if (!condition.empty()) {
                        error(member.line, "a [[stencil]] output with [[function_constant]] is unsupported (the "
                                           "reference value an absent output keeps is not visible to the shader)");
                        break;
                    }
                    entry.writes_stencil = true;
                    body << "    metal::__msl_builtin_out(" << builtin_stencil << "u, 0u, " << access << ");\n";
                    break;
                }
            }
            if (done) continue;
            if (entry.stage == Stage::fragment) {
                error(member.line, "fragment output '" + member.name + "' has no [[color(n)]], [[depth]] or [[stencil]]");
                continue;
            }
            if (!member.array_suffix.empty()) {
                error(member.line, "array varyings are unsupported");
                continue;
            }
            InterfaceVarying varying;
            varying.member = member.name;
            varying.name = member.name;
            if (const auto* user = find_attribute(member.attributes, "user"); user && !user->args.empty())
                varying.name = user->args[0];
            varying.location = next_output++;
            varying.type = resolve_spelling(member.type);
            varying.interpolation = interpolation_of(member.attributes).value_or(Interpolation::perspective);
            varying.function_constant = condition_index;
            const auto statement = "metal::__msl_out(" + std::to_string(varying.location) + "u, " + access + ");";
            body << "    " << (condition.empty() ? statement : "if (" + condition + ") " + statement) << '\n';
            entry.outputs.push_back(std::move(varying));
        }
    }

    // ---- keyword renames ---------------------------------------------------------------
    static std::string rename_word(const std::string& word) {
        static const std::map<std::string_view, std::string_view> renames = {
            {"device", "__global"}, {"constant", "__global"}, {"threadgroup", "__local"}, {"thread", ""},
            {"global", "__msl_id_global"}, {"local", "__msl_id_local"}, {"generic", "__msl_id_generic"},
            {"read_only", "__msl_id_read_only"}, {"write_only", "__msl_id_write_only"},
            {"read_write", "__msl_id_read_write"}, {"pipe", "__msl_id_pipe"}, {"kernel", "__msl_id_kernel"},
            {"__kernel", "__msl_id___kernel"}, {"image1d_t", "__msl_id_image1d_t"}, {"image2d_t", "__msl_id_image2d_t"},
            {"image3d_t", "__msl_id_image3d_t"}, {"sampler_t", "__msl_id_sampler_t"}, {"event_t", "__msl_id_event_t"},
            {"queue_t", "__msl_id_queue_t"}, {"clk_event_t", "__msl_id_clk_event_t"},
            {"reserve_id_t", "__msl_id_reserve_id_t"}, {"ndrange_t", "__msl_id_ndrange_t"},
            // OpenCL 2.0 builtin atomic typedefs (metal_stdlib declares the
            // renamed MSL ones).
            {"atomic_int", "__msl_id_atomic_int"}, {"atomic_uint", "__msl_id_atomic_uint"},
            {"atomic_float", "__msl_id_atomic_float"}, {"atomic_long", "__msl_id_atomic_long"},
            {"atomic_ulong", "__msl_id_atomic_ulong"}, {"atomic_bool", "__msl_id_atomic_bool"},
            {"atomic_double", "__msl_id_atomic_double"}, {"atomic_flag", "__msl_id_atomic_flag"}};
        const auto found = renames.find(word);
        return found == renames.end() ? word : std::string(found->second);
    }

    // ---- conditional operands --------------------------------------------------------------
    // Clang's OpenCL C++ types a class prvalue (`float2(0.0f)`, a call result)
    // without an address space but a local lvalue as `__private T`, and then
    // rejects `c ? float2(0.0f) : local[i]` ("incompatible operand types"),
    // which is ordinary MSL. Each operand of a conditional becomes
    // `(metal::__msl_rv{}, (operand))`: an overloaded comma that returns a
    // prvalue copy (void operands fall back to the built-in comma), so both
    // operands are prvalues of one type. Conditionals are never lvalues in
    // GLSL-derived code; an MSL source assigning through one gets a clang
    // error rather than silent miscompilation. A conditional whose third
    // operand has a top-level '<' or '>' (a template-id or comparison whose
    // extent the token-level scan cannot know) is left alone.
    void wrap_conditionals() {
        static constexpr const char* open = "(metal::__msl_rv{}, (";
        static constexpr const char* close = "))";
        for (std::size_t q = 0; q < sig_.size(); ++q) {
            if (!is(q, "?") || T(q).removed) continue;
            // Second operand: up to the ':' that matches this '?'.
            std::size_t colon = sig_.size();
            int depth = 0, pending = 0;
            for (auto k = q + 1; k < sig_.size(); ++k) {
                const auto& t = text(k);
                if (t == "(" || t == "[" || t == "{") ++depth;
                else if (t == ")" || t == "]" || t == "}") { if (--depth < 0) break; }
                else if (depth == 0 && t == ";") break;
                else if (depth == 0 && t == "?") ++pending;
                else if (depth == 0 && t == ":" && pending-- == 0) { colon = k; break; }
            }
            if (colon >= sig_.size() || colon == q + 1) continue;
            // Third operand: an assignment-expression, ending at a top-level
            // ',', ';', closing bracket or the ':' of an enclosing conditional.
            std::size_t end = colon + 1;
            bool angle = false;
            depth = 0;
            pending = 0;
            for (; end < sig_.size(); ++end) {
                const auto& t = text(end);
                if (t == "(" || t == "[" || t == "{") { ++depth; continue; }
                if (t == ")" || t == "]" || t == "}") { if (depth-- == 0) break; continue; }
                if (depth != 0) continue;
                if (t == "," || t == ";") break;
                if (t == "?") ++pending;
                else if (t == ":" && pending-- == 0) break;
                else if (t == "<" || t == ">" || t == ">>") angle = true;
            }
            if (angle || end == colon + 1 || end >= sig_.size()) continue;
            if (T(q + 1).removed || T(colon - 1).removed || T(colon + 1).removed || T(end - 1).removed) continue;
            T(q + 1).before = open + T(q + 1).before;
            T(colon - 1).after += close;
            T(colon + 1).before = open + T(colon + 1).before;
            T(end - 1).after += close;
        }
    }

    void rename_tokens() {
        for (std::size_t s = 0; s < sig_.size(); ++s) {
            auto& token = T(s);
            if (token.removed || keep_identifier_.count(sig_[s])) continue;
            if (token.kind == Token::Kind::identifier) {
                if (token.text == "is_function_constant_defined" && is(s + 1, "(") && s + 3 < sig_.size() && is(s + 3, ")")) {
                    const auto index = constant_index(text(s + 2));
                    if (!index) {
                        error(token.line, "unknown function constant '" + text(s + 2) + "'");
                        continue;
                    }
                    token.out = "__msl_fc_defined(" + std::to_string(*index) + "u)";
                    remove(s + 1, s + 4);
                    s += 3;
                    continue;
                }
                if (token.text == "threadgroup_imageblock" || token.text == "ray_data" || token.text == "object_data") {
                    error(token.line, "the '" + token.text + "' address space is unsupported");
                    continue;
                }
                token.out = rename_word(token.text);
                continue;
            }
            if (token.kind == Token::Kind::number) {
                // MSL half literals: 1.0h / 2h.
                const auto& t = token.text;
                const bool hex = t.size() > 1 && (t[1] == 'x' || t[1] == 'X');
                if (!hex && t.size() > 1 && (t.back() == 'h' || t.back() == 'H')) {
                    const auto body = t.substr(0, t.size() - 1);
                    const bool real = body.find_first_of(".eE") != std::string::npos;
                    token.out = "((half)" + body + (real ? "f" : "") + ")";
                }
            }
        }
    }
};

} // namespace

TypeSpelling parse_type_spelling(std::string_view input) {
    TypeSpelling result;
    // Clean: drop address spaces, cv, references, 'metal::', 'struct'.
    std::string text;
    {
        std::string word;
        std::vector<std::string> words;
        std::string current;
        for (const auto c : input) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!current.empty()) words.push_back(current), current.clear();
                continue;
            }
            current += c;
        }
        if (!current.empty()) words.push_back(current);
        for (auto& w : words) {
            if (w == "const" || w == "volatile" || w == "__global" || w == "__local" || w == "device" ||
                w == "constant" || w == "thread" || w == "threadgroup" || w == "struct" || w == "&" || w == "*")
                continue;
            text += w;
        }
    }
    std::size_t at;
    while ((at = text.find("metal::")) != std::string::npos) text.erase(at, 7);
    while (!text.empty() && (text.back() == '&' || text.back() == '*')) text.pop_back();
    if (text.rfind("::", 0) == 0) text = text.substr(2);
    result.text = text;
    static const std::map<std::string_view, Scalar> scalars = {
        {"bool", Scalar::boolean}, {"char", Scalar::int8}, {"uchar", Scalar::uint8}, {"int8_t", Scalar::int8},
        {"uint8_t", Scalar::uint8}, {"short", Scalar::int16}, {"ushort", Scalar::uint16}, {"int16_t", Scalar::int16},
        {"uint16_t", Scalar::uint16}, {"int", Scalar::int32}, {"uint", Scalar::uint32}, {"int32_t", Scalar::int32},
        {"uint32_t", Scalar::uint32}, {"unsignedint", Scalar::uint32}, {"long", Scalar::int64},
        {"ulong", Scalar::uint64}, {"int64_t", Scalar::int64}, {"uint64_t", Scalar::uint64}, {"half", Scalar::half},
        {"float", Scalar::float32}, {"bfloat", Scalar::bfloat}};
    if (const auto found = scalars.find(text); found != scalars.end()) {
        result.kind = Type::Kind::scalar;
        result.scalar = found->second;
        return result;
    }
    static const std::pair<std::string_view, Scalar> prefixes[] = {
        {"packed_uchar", Scalar::uint8}, {"packed_char", Scalar::int8}, {"packed_ushort", Scalar::uint16},
        {"packed_short", Scalar::int16}, {"packed_uint", Scalar::uint32}, {"packed_int", Scalar::int32},
        {"packed_half", Scalar::half}, {"packed_float", Scalar::float32}, {"bool", Scalar::boolean},
        {"uchar", Scalar::uint8}, {"char", Scalar::int8}, {"ushort", Scalar::uint16}, {"short", Scalar::int16},
        {"uint", Scalar::uint32}, {"int", Scalar::int32}, {"ulong", Scalar::uint64}, {"long", Scalar::int64},
        {"half", Scalar::half}, {"float", Scalar::float32}};
    for (const auto& [prefix, scalar] : prefixes) {
        if (text.rfind(prefix, 0) != 0) continue;
        const auto rest = std::string_view(text).substr(prefix.size());
        if (rest.size() == 1 && rest[0] >= '2' && rest[0] <= '4') {
            result.kind = Type::Kind::vector;
            result.scalar = scalar;
            result.components = static_cast<std::uint32_t>(rest[0] - '0');
            result.packed = prefix.rfind("packed_", 0) == 0;
            return result;
        }
        if (rest.size() == 3 && rest[1] == 'x' && rest[0] >= '2' && rest[0] <= '4' && rest[2] >= '2' && rest[2] <= '4') {
            result.kind = Type::Kind::matrix;
            result.scalar = scalar;
            result.columns = static_cast<std::uint32_t>(rest[0] - '0');
            result.components = static_cast<std::uint32_t>(rest[2] - '0');
            return result;
        }
    }
    if (text == "sampler") {
        result.kind = Type::Kind::sampler;
        return result;
    }
    // array<texture2d<float>, N>
    std::string inner = text;
    if (inner.rfind("array<", 0) == 0 && inner.back() == '>') {
        const auto comma = inner.rfind(',');
        if (comma != std::string::npos) {
            std::uint32_t count = 0;
            const auto digits = inner.substr(comma + 1, inner.size() - comma - 2);
            std::from_chars(digits.data(), digits.data() + digits.size(), count);
            result.array_length = count;
            inner = inner.substr(6, comma - 6);
        }
    }
    static const std::pair<std::string_view, TextureType> textures[] = {
        {"texture1d_array", TextureType::texture1d_array}, {"texture1d", TextureType::texture1d},
        {"texture2d_ms_array", TextureType::texture2d_ms_array}, {"texture2d_ms", TextureType::texture2d_ms},
        {"texture2d_array", TextureType::texture2d_array}, {"texture2d", TextureType::texture2d},
        {"texture3d", TextureType::texture3d}, {"texturecube_array", TextureType::texture_cube_array},
        {"texturecube", TextureType::texture_cube}, {"texture_buffer", TextureType::texture_buffer},
        {"depth2d_ms_array", TextureType::depth2d_ms_array}, {"depth2d_ms", TextureType::depth2d_ms},
        {"depth2d_array", TextureType::depth2d_array}, {"depth2d", TextureType::depth2d},
        {"depthcube_array", TextureType::depth_cube_array}, {"depthcube", TextureType::depth_cube}};
    for (const auto& [name, type] : textures) {
        if (inner.rfind(name, 0) != 0) continue;
        const auto rest = std::string_view(inner).substr(name.size());
        if (!rest.empty() && rest[0] != '<') continue;
        result.kind = Type::Kind::texture;
        result.texture = type;
        const bool depth = name.rfind("depth", 0) == 0;
        const bool ms = name.find("_ms") != std::string_view::npos || type == TextureType::texture_buffer;
        result.scalar = Scalar::float32;
        result.access = ms ? Access::read : Access::sample;
        if (rest.size() > 2) {
            const auto args = rest.substr(1, rest.size() - 2);
            const auto comma = args.find(',');
            const auto element = args.substr(0, comma);
            if (const auto found = scalars.find(element); found != scalars.end()) result.scalar = found->second;
            if (comma != std::string_view::npos) {
                const auto access = args.substr(comma + 1);
                if (access.find("read_write") != std::string_view::npos) result.access = Access::read_write;
                else if (access.find("write") != std::string_view::npos) result.access = Access::write;
                else if (access.find("read") != std::string_view::npos) result.access = Access::read;
                else if (access.find("sample") != std::string_view::npos) result.access = Access::sample;
            }
        }
        (void)depth;
        return result;
    }
    result.kind = Type::Kind::structure;
    return result;
}

RewriteResult rewrite(std::string_view preprocessed) {
    return Rewriter(preprocessed).run();
}

} // namespace alchemy::msl::detail
