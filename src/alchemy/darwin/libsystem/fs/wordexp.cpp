#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// wordexp(3) and wordfree(3) (fill-only): the shell's word expansions, done here in the shell's order:
// tilde expansion, parameter expansion (the ${...} forms, with the environment as the variables),
// command substitution (the command runs in /bin/sh through the guest's popen; its standard error goes to
// /dev/null unless WRDE_SHOWERR), arithmetic expansion (C integer expressions), field splitting on IFS,
// pathname expansion (the guest's glob) and quote removal. As in a fresh shell, the positional
// parameters are empty, $# and $? are 0, $0 is "sh" and $$ is the process ID; ${name=word} assignments
// last for the call. An unquoted | & ; < > ( ) { } or newline is WRDE_BADCHAR, as wordexp has no
// shell grammar beyond words.
//
// wordexp_t: we_wordc @0, we_wordv @8, we_offs @16. we_wordv and each word are malloc'd; wordfree
// frees the words after the we_offs reserved slots and the vector.
namespace alchemy::export_fill {
namespace {

constexpr std::uint32_t wrde_append = 0x01, wrde_dooffs = 0x02, wrde_nocmd = 0x04, wrde_reuse = 0x08,
    wrde_showerr = 0x10, wrde_undef = 0x20;
// <wordexp.h> results; WRDE_NOSYS when a command cannot be started (no shell).
constexpr std::uint64_t wrde_badchar = 1, wrde_badval = 2, wrde_cmdsub = 3, wrde_nospace = 4, wrde_nosys = 5,
    wrde_syntax = 6;
constexpr std::size_t limit = 1U << 20;   // bytes of input, expansion and command output

struct Failure { std::uint64_t code; };

// One character of an expanded word: quoted characters are not split or globbed; expanded ones come
// from an expansion (only those are split on IFS).
struct Char { char c; bool quoted; bool expanded; };
struct Word { std::vector<Char> text; bool quotes{}; };

bool name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool name_char(char c) { return name_start(c) || (c >= '0' && c <= '9'); }

class Expander {
public:
    Expander(const Provider& p, GuestMemory& m, std::uint32_t flags) : p_(p), m_(m), flags_(flags) {}

    std::vector<std::string> run(std::string_view input) {
        if (input.size() > limit) throw Failure{wrde_nospace};
        std::vector<Word> words;
        Word current;
        bool started{};
        const auto finish = [&] {
            if (started) words.push_back(std::move(current));
            current = {};
            started = false;
        };
        for (std::size_t i = 0; i < input.size();) {
            const char c = input[i];
            if (c == ' ' || c == '\t') { finish(); ++i; continue; }
            if (c == '\n' || std::string_view("|&;<>(){}").find(c) != std::string_view::npos) throw Failure{wrde_badchar};
            started = true;
            if (c == '~' && current.text.empty() && !current.quotes) { i = tilde(input, i, current); continue; }
            i = item(input, i, current, false);
        }
        finish();
        std::vector<std::string> result;
        for (auto& word : words)
            for (auto& field : split(word)) glob(field, result);
        return result;
    }

private:
    // One quoting unit or expansion at `i` (outside quotes, or inside double quotes when `quoted`).
    std::size_t item(std::string_view s, std::size_t i, Word& word, bool quoted) {
        const char c = s[i];
        if (c == '\\') {
            if (i + 1 >= s.size()) { add(word, '\\', quoted, false); return i + 1; }
            const char next = s[i + 1];
            if (quoted && std::string_view("$`\"\\\n").find(next) == std::string_view::npos) {
                add(word, '\\', true, false);
                add(word, next, true, false);
            } else if (next != '\n') add(word, next, true, false);
            return i + 2;
        }
        if (c == '\'' && !quoted) {
            const auto end = s.find('\'', i + 1);
            if (end == std::string_view::npos) throw Failure{wrde_syntax};
            for (auto k = i + 1; k < end; ++k) add(word, s[k], true, false);
            word.quotes = true;
            return end + 1;
        }
        if (c == '"' && !quoted) {
            word.quotes = true;
            for (++i; i < s.size() && s[i] != '"';) i = item(s, i, word, true);
            if (i >= s.size()) throw Failure{wrde_syntax};
            return i + 1;
        }
        if (c == '`') {
            std::string command;
            for (++i; i < s.size() && s[i] != '`'; ++i) {
                if (s[i] == '\\' && i + 1 < s.size() && std::string_view("$`\\").find(s[i + 1]) != std::string_view::npos) ++i;
                command += s[i];
            }
            if (i >= s.size()) throw Failure{wrde_syntax};
            append(word, substitute(command), quoted);
            return i + 1;
        }
        if (c == '$') return dollar(s, i, word, quoted);
        add(word, c, quoted, false);
        return i + 1;
    }

    std::size_t dollar(std::string_view s, std::size_t i, Word& word, bool quoted) {
        if (i + 1 >= s.size()) { add(word, '$', quoted, false); return i + 1; }
        const char next = s[i + 1];
        if (next == '(') {
            if (i + 2 < s.size() && s[i + 2] == '(') {
                const auto end = closing(s, i + 3, 2);
                const auto expression = text(expand_string(s.substr(i + 3, end - (i + 3))));
                append(word, std::to_string(arithmetic(expression)), quoted);
                return end + 2;
            }
            const auto end = closing(s, i + 2, 1);
            append(word, substitute(std::string(s.substr(i + 2, end - (i + 2)))), quoted);
            return end + 1;
        }
        if (next == '{') {
            const auto end = brace_end(s, i + 2);
            parameter(s.substr(i + 2, end - (i + 2)), word, quoted);
            return end + 1;
        }
        if (name_start(next)) {
            auto end = i + 1;
            while (end < s.size() && name_char(s[end])) ++end;
            const auto value = variable(std::string(s.substr(i + 1, end - (i + 1))));
            if (!value && (flags_ & wrde_undef)) throw Failure{wrde_badval};
            append(word, value.value_or(""), quoted);
            return end;
        }
        if ((next >= '0' && next <= '9') || std::string_view("@*#?-$!").find(next) != std::string_view::npos) {
            const auto value = special(next);
            if (!value && (flags_ & wrde_undef)) throw Failure{wrde_badval};
            append(word, value.value_or(""), quoted);
            return i + 2;
        }
        add(word, '$', quoted, false);
        return i + 1;
    }

    // ${...}: name, #name, or name followed by one of the operators.
    void parameter(std::string_view body, Word& word, bool quoted) {
        if (body.size() > 1 && body[0] == '#') {
            const auto value = lookup(body.substr(1));
            if (!value && (flags_ & wrde_undef)) throw Failure{wrde_badval};
            append(word, std::to_string(value.value_or("").size()), quoted);
            return;
        }
        std::size_t end = 0;
        if (!body.empty() && name_start(body[0])) { while (end < body.size() && name_char(body[end])) ++end; }
        else if (!body.empty() && ((body[0] >= '0' && body[0] <= '9') || std::string_view("@*#?-$!").find(body[0]) != std::string_view::npos)) end = 1;
        if (end == 0) throw Failure{wrde_syntax};
        const auto name = body.substr(0, end);
        const auto value = lookup(name);
        auto rest = body.substr(end);
        if (rest.empty()) {
            if (!value && (flags_ & wrde_undef)) throw Failure{wrde_badval};
            append(word, value.value_or(""), quoted);
            return;
        }
        const bool colon = rest[0] == ':';
        if (colon) rest.remove_prefix(1);
        if (rest.empty()) throw Failure{wrde_syntax};
        const char op = rest[0];
        const bool doubled = !colon && rest.size() > 1 && (op == '#' || op == '%') && rest[1] == op;
        const auto argument = rest.substr(doubled ? 2 : 1);
        const bool set = value && (!colon || !value->empty());
        switch (op) {
        case '-': if (set) append(word, *value, quoted); else append_text(word, expand_string(argument), quoted); return;
        case '=':
            if (set) { append(word, *value, quoted); return; }
            if (!name_start(name[0])) throw Failure{wrde_syntax};
            assigned_[std::string(name)] = text(expand_string(argument));
            append(word, assigned_[std::string(name)], quoted);
            return;
        case '?': if (set) { append(word, *value, quoted); return; } throw Failure{wrde_badval};
        case '+': if (set) append_text(word, expand_string(argument), quoted); return;
        case '#': case '%': {
            if (colon) throw Failure{wrde_syntax};
            if (!value && (flags_ & wrde_undef)) throw Failure{wrde_badval};
            append(word, trim(value.value_or(""), pattern(expand_string(argument)), op == '#', doubled), quoted);
            return;
        }
        default: throw Failure{wrde_syntax};
        }
    }

    std::optional<std::string> lookup(std::string_view name) {
        if (!name.empty() && name_start(name[0])) return variable(std::string(name));
        return name.size() == 1 ? special(name[0]) : std::nullopt;
    }
    std::optional<std::string> special(char c) {
        switch (c) {
        case '0': return "sh";
        case '#': case '?': return "0";
        case '$': return std::to_string(p_.call(libsystem_library, "_getpid", {}) & 0xffffffffU);
        case '-': case '@': case '*': return "";
        default: return std::nullopt;   // $1..$9 and $! are unset
        }
    }
    std::optional<std::string> variable(const std::string& name) {
        if (const auto found = assigned_.find(name); found != assigned_.end()) return found->second;
        const auto key = guest_string(name);
        const auto value = p_.call(libsystem_library, "_getenv", {key});
        p_.call(libsystem_library, "_free", {key});
        if (!value) return std::nullopt;
        return read(value);
    }

    // A word expanded as one string (an operand of ${...} or an arithmetic expression): quotes and
    // expansions, no splitting.
    std::vector<Char> expand_string(std::string_view s) {
        Word word;
        for (std::size_t i = 0; i < s.size();) {
            if (s[i] == '~' && i == 0) { i = tilde(s, i, word); continue; }
            i = item(s, i, word, false);
        }
        return word.text;
    }

    std::size_t tilde(std::string_view s, std::size_t i, Word& word) {
        auto end = i + 1;
        while (end < s.size() && s[end] != '/' && s[end] != ' ' && s[end] != '\t') {
            if (!name_char(s[end]) && s[end] != '.' && s[end] != '-') { add(word, '~', false, false); return i + 1; }
            ++end;
        }
        const auto user = s.substr(i + 1, end - (i + 1));
        std::optional<std::string> home;
        if (user.empty()) {
            home = variable("HOME");
            if (!home) {
                if (const auto entry = p_.call(libsystem_library, "_getpwuid", {p_.call(libsystem_library, "_getuid", {})})) home = read(m_.read64(entry + 48));
            }
        } else {
            const auto name = guest_string(std::string(user));
            const auto entry = p_.call(libsystem_library, "_getpwnam", {name});
            p_.call(libsystem_library, "_free", {name});
            if (entry) home = read(m_.read64(entry + 48));
        }
        if (!home) { add(word, '~', false, false); return i + 1; }   // an unknown user stays as written
        for (const char c : *home) add(word, c, true, true);
        return end;
    }

    // The command's output, trailing newlines removed.
    std::string substitute(const std::string& command) {
        if (flags_ & wrde_nocmd) throw Failure{wrde_cmdsub};
        if (command.find_first_not_of(" \t\n") == std::string::npos) return {};
        // exec with only a redirection applies it to the shell itself (no subshell needed).
        const auto line = guest_string((flags_ & wrde_showerr) ? command : "exec 2>/dev/null; " + command);
        const auto mode = guest_string("r");
        const auto stream = p_.call(libsystem_library, "_popen", {line, mode});
        p_.call(libsystem_library, "_free", {line});
        p_.call(libsystem_library, "_free", {mode});
        if (!stream) throw Failure{wrde_nosys};
        constexpr std::uint64_t chunk = 4096;
        const auto buffer = p_.malloc(chunk);
        std::string output;
        for (;;) {
            const auto count = p_.call(libsystem_library, "_fread", {buffer, 1, chunk, stream});
            if (!count) break;
            if (output.size() + count > limit) { p_.call(libsystem_library, "_free", {buffer}); p_.call(libsystem_library, "_pclose", {stream}); throw Failure{wrde_nospace}; }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
            m_.read_bytes(buffer, bytes);
            output.append(bytes.begin(), bytes.end());
        }
        p_.call(libsystem_library, "_free", {buffer});
        p_.call(libsystem_library, "_pclose", {stream});
        while (!output.empty() && output.back() == '\n') output.pop_back();
        return output;
    }

    // ${name#pattern} and the other removals: the shortest or longest matching prefix or suffix.
    std::string trim(const std::string& value, const std::string& glob, bool prefix, bool longest) {
        const auto pattern = guest_string(glob);
        const auto matches = [&](const std::string& candidate) {
            const auto text_address = guest_string(candidate);
            const auto result = p_.call(libsystem_library, "_fnmatch", {pattern, text_address, 0});
            p_.call(libsystem_library, "_free", {text_address});
            return result == 0;
        };
        std::string result = value;
        const auto n = value.size();
        for (std::size_t step = 0; step <= n; ++step) {
            const auto length = longest ? n - step : step;
            if (prefix ? matches(value.substr(0, length)) : matches(value.substr(n - length))) {
                result = prefix ? value.substr(length) : value.substr(0, n - length);
                break;
            }
        }
        p_.call(libsystem_library, "_free", {pattern});
        return result;
    }

    // Field splitting of the expanded, unquoted characters on IFS.
    std::vector<std::vector<Char>> split(const Word& word) {
        const auto ifs = variable("IFS").value_or(" \t\n");
        const auto white = [](char c) { return c == ' ' || c == '\t' || c == '\n'; };
        std::vector<std::vector<Char>> fields;
        std::vector<Char> field;
        // A leading non-white IFS character starts with an empty field.
        bool split_any{}, pending{true};
        for (std::size_t k = 0; k < word.text.size(); ++k) {
            const auto& ch = word.text[k];
            if (ch.expanded && !ch.quoted && ifs.find(ch.c) != std::string::npos) {
                split_any = true;
                if (white(ch.c)) { if (!field.empty()) { fields.push_back(std::move(field)); field.clear(); } pending = false; continue; }
                // A non-white IFS character ends a field, which may be empty.
                if (!field.empty() || pending) fields.push_back(std::move(field));
                field.clear();
                pending = true;
                continue;
            }
            field.push_back(ch);
            pending = false;
        }
        if (!field.empty()) fields.push_back(std::move(field));
        if (fields.empty() && !split_any && word.quotes) fields.emplace_back();   // "" is one empty word
        return fields;
    }

    // Pathname expansion of a field with unquoted pattern characters; without a match it stays as is.
    void glob(const std::vector<Char>& field, std::vector<std::string>& out) {
        bool magic{};
        std::string pattern, plain;
        for (const auto& ch : field) {
            plain += ch.c;
            if (!ch.quoted && (ch.c == '*' || ch.c == '?' || ch.c == '[')) magic = true;
            if (ch.quoted && std::string_view("\\*?[]").find(ch.c) != std::string_view::npos) pattern += '\\';
            pattern += ch.c;
        }
        if (!magic) { out.push_back(plain); return; }
        const auto address = guest_string(pattern);
        const auto result = p_.malloc(128);
        m_.fill_bytes(result, 0, 128);
        const auto status = static_cast<std::int32_t>(p_.call(libsystem_library, "_glob", {address, 0, 0, result}));
        p_.call(libsystem_library, "_free", {address});
        if (status == 0) {
            const auto count = m_.read64(result), paths = m_.read64(result + 32);
            for (std::uint64_t k = 0; k < count; ++k) out.push_back(read(m_.read64(paths + k * 8)));
            p_.call(libsystem_library, "_globfree", {result});
        } else out.push_back(plain);   // GLOB_NOMATCH, or an unreadable directory
        p_.call(libsystem_library, "_free", {result});
    }

    // $(( )): C integer arithmetic; names are variables read as integers (unset or empty is 0).
    std::int64_t arithmetic(const std::string& expression) {
        std::size_t at = 0;
        const auto value = conditional(expression, at);
        skip(expression, at);
        if (at != expression.size()) throw Failure{wrde_syntax};
        return value;
    }
    static void skip(const std::string& e, std::size_t& at) { while (at < e.size() && (e[at] == ' ' || e[at] == '\t' || e[at] == '\n')) ++at; }
    static bool take(const std::string& e, std::size_t& at, std::string_view op) {
        skip(e, at);
        if (e.compare(at, op.size(), op) != 0) return false;
        // "<" is not the start of "<<" or "<=", "&" not of "&&", "|" not of "||", "!" not of "!=".
        if (op.size() == 1 && at + 1 < e.size()) {
            const char next = e[at + 1];
            if ((op[0] == '<' || op[0] == '>') && (next == op[0] || next == '=')) return false;
            if ((op[0] == '&' || op[0] == '|') && next == op[0]) return false;
            if (op[0] == '!' && next == '=') return false;
        }
        at += op.size();
        return true;
    }
    std::int64_t conditional(const std::string& e, std::size_t& at) {
        const auto test = binary(e, at, 0);
        if (!take(e, at, "?")) return test;
        const auto yes = conditional(e, at);
        if (!take(e, at, ":")) throw Failure{wrde_syntax};
        const auto no = conditional(e, at);
        return test ? yes : no;
    }
    std::int64_t binary(const std::string& e, std::size_t& at, int level) {
        static const std::vector<std::vector<std::string_view>> levels{
            {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="}, {"<=", ">=", "<", ">"}, {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"}};
        if (level == static_cast<int>(levels.size())) return unary(e, at);
        auto left = binary(e, at, level + 1);
        for (;;) {
            std::string_view found;
            for (const auto op : levels[static_cast<std::size_t>(level)]) if (take(e, at, op)) { found = op; break; }
            if (found.empty()) return left;
            const auto right = binary(e, at, level + 1);
            const auto l = static_cast<std::uint64_t>(left), r = static_cast<std::uint64_t>(right);
            if (found == "||") left = (left || right) ? 1 : 0;
            else if (found == "&&") left = (left && right) ? 1 : 0;
            else if (found == "|") left = static_cast<std::int64_t>(l | r);
            else if (found == "^") left = static_cast<std::int64_t>(l ^ r);
            else if (found == "&") left = static_cast<std::int64_t>(l & r);
            else if (found == "==") left = left == right;
            else if (found == "!=") left = left != right;
            else if (found == "<=") left = left <= right;
            else if (found == ">=") left = left >= right;
            else if (found == "<") left = left < right;
            else if (found == ">") left = left > right;
            else if (found == "<<") left = static_cast<std::int64_t>(l << (r & 63));
            else if (found == ">>") left >>= (r & 63);
            else if (found == "+") left = static_cast<std::int64_t>(l + r);
            else if (found == "-") left = static_cast<std::int64_t>(l - r);
            else if (found == "*") left = static_cast<std::int64_t>(l * r);
            else {
                if (right == 0 || (left == INT64_MIN && right == -1)) throw Failure{wrde_syntax};
                left = found == "/" ? left / right : left % right;
            }
        }
    }
    std::int64_t unary(const std::string& e, std::size_t& at) {
        if (take(e, at, "!")) return !unary(e, at);
        if (take(e, at, "~")) return ~unary(e, at);
        if (take(e, at, "-")) return static_cast<std::int64_t>(0 - static_cast<std::uint64_t>(unary(e, at)));
        if (take(e, at, "+")) return unary(e, at);
        if (take(e, at, "(")) {
            const auto value = conditional(e, at);
            if (!take(e, at, ")")) throw Failure{wrde_syntax};
            return value;
        }
        skip(e, at);
        if (at < e.size() && name_start(e[at])) {
            const auto start = at;
            while (at < e.size() && name_char(e[at])) ++at;
            const auto value = variable(e.substr(start, at - start)).value_or("");
            std::size_t inner = 0;
            if (value.empty()) return 0;
            const auto number = arithmetic_number(value, inner);
            if (inner != value.size()) throw Failure{wrde_syntax};
            return number;
        }
        return arithmetic_number(e, at);
    }
    static std::int64_t arithmetic_number(const std::string& e, std::size_t& at) {
        skip(e, at);
        if (at >= e.size() || e[at] < '0' || e[at] > '9') throw Failure{wrde_syntax};
        unsigned base = 10;
        if (e[at] == '0' && at + 1 < e.size() && (e[at + 1] == 'x' || e[at + 1] == 'X')) { base = 16; at += 2; }
        else if (e[at] == '0') base = 8;
        std::uint64_t value{};
        bool digits{};
        for (; at < e.size(); ++at) {
            const char c = e[at];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else break;
            if (digit >= base) throw Failure{wrde_syntax};
            value = value * base + digit;
            digits = true;
        }
        if (!digits && base == 16) throw Failure{wrde_syntax};
        return static_cast<std::int64_t>(value);
    }

    // The index of the parenthesis that closes $( (depth 1) or $(( (depth 2), skipping quotes.
    static std::size_t closing(std::string_view s, std::size_t i, int depth) {
        int open = depth;
        for (; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '\\') { ++i; continue; }
            if (c == '\'') { i = s.find('\'', i + 1); if (i == std::string_view::npos) break; continue; }
            if (c == '"') { for (++i; i < s.size() && s[i] != '"'; ++i) if (s[i] == '\\') ++i; if (i >= s.size()) break; continue; }
            if (c == '(') ++open;
            else if (c == ')' && --open < depth) {
                if (depth == 2) { if (i + 1 < s.size() && s[i + 1] == ')' && open == 1) return i; throw Failure{wrde_syntax}; }
                return i;
            }
        }
        throw Failure{wrde_syntax};
    }
    static std::size_t brace_end(std::string_view s, std::size_t i) {
        int open = 1;
        for (; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '\\') { ++i; continue; }
            if (c == '\'') { i = s.find('\'', i + 1); if (i == std::string_view::npos) break; continue; }
            if (c == '{') ++open;
            else if (c == '}' && --open == 0) return i;
        }
        throw Failure{wrde_syntax};
    }

    void add(Word& word, char c, bool quoted, bool expanded) {
        if (word.text.size() >= limit) throw Failure{wrde_nospace};
        word.text.push_back({c, quoted, expanded});
    }
    void append(Word& word, const std::string& value, bool quoted) {
        if (quoted) word.quotes = true;
        for (const char c : value) add(word, c, quoted, true);
    }
    void append_text(Word& word, const std::vector<Char>& value, bool quoted) {
        if (quoted) word.quotes = true;
        for (const auto& ch : value) add(word, ch.c, quoted || ch.quoted, true);
    }
    static std::string text(const std::vector<Char>& value) {
        std::string result;
        for (const auto& ch : value) result += ch.c;
        return result;
    }
    // A pattern operand: quoted characters match themselves.
    static std::string pattern(const std::vector<Char>& value) {
        std::string result;
        for (const auto& ch : value) {
            if (ch.quoted && std::string_view("\\*?[]").find(ch.c) != std::string_view::npos) result += '\\';
            result += ch.c;
        }
        return result;
    }
    std::string read(std::uint64_t address) const {
        if (!address) return {};
        const auto bytes = m_.read_string(address, 1, limit);
        return {bytes.begin(), bytes.end()};
    }
    std::uint64_t guest_string(const std::string& value) const {
        const auto address = p_.malloc(value.size() + 1);
        m_.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
        m_.write8(address + value.size(), 0);
        return address;
    }

    const Provider& p_;
    GuestMemory& m_;
    std::uint32_t flags_;
    std::map<std::string, std::string> assigned_;
};

void free_words(const Provider& p, GuestMemory& m, std::uint64_t we) {
    const auto vector = m.read64(we + 8);
    if (vector) {
        const auto count = m.read64(we), offsets = m.read64(we + 16);
        for (std::uint64_t k = 0; k < count; ++k) p.call(libsystem_library, "_free", {m.read64(vector + (offsets + k) * 8)});
        p.call(libsystem_library, "_free", {vector});
    }
    m.write64(we, 0);
    m.write64(we + 8, 0);
}

} // namespace

void install_libsystem_wordexp(Provider& p) {
    // int wordexp(const char* words, wordexp_t* we, int flags)
    p.libsystem("_wordexp", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto we = s.x[1];
        const auto flags = static_cast<std::uint32_t>(s.x[2]);
        if (!s.x[0] || !we) return wrde_syntax;
        std::vector<std::string> words;
        try {
            const auto bytes = m.read_string(s.x[0], 1, limit + 1);
            words = Expander(p, m, flags).run(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        } catch (const Failure& failure) {
            return failure.code;
        }
        if ((flags & wrde_reuse) && !(flags & wrde_append)) free_words(p, m, we);
        const bool append = (flags & wrde_append) && m.read64(we + 8);
        const auto offsets = (flags & wrde_dooffs) ? m.read64(we + 16) : 0;
        const auto old_count = append ? m.read64(we) : 0;
        const auto slots = offsets + old_count + words.size() + 1;
        if (offsets > limit || slots > limit) return wrde_nospace;
        const auto vector = append ? p.call(libsystem_library, "_realloc", {m.read64(we + 8), slots * 8}) : p.call(libsystem_library, "_malloc", {slots * 8});
        if (!vector) return wrde_nospace;
        if (!append) for (std::uint64_t k = 0; k < offsets; ++k) m.write64(vector + k * 8, 0);
        auto index = offsets + old_count;
        for (const auto& word : words) {
            const auto address = p.call(libsystem_library, "_malloc", {word.size() + 1});
            if (!address) { m.write64(vector + index * 8, 0); m.write64(we + 8, vector); m.write64(we, index - offsets); return wrde_nospace; }
            m.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(word.data()), word.size()));
            m.write8(address + word.size(), 0);
            m.write64(vector + index++ * 8, address);
        }
        m.write64(vector + index * 8, 0);
        m.write64(we, index - offsets);
        m.write64(we + 8, vector);
        m.write64(we + 16, offsets);
        return 0;
    }, wrde_syntax);
    // void wordfree(wordexp_t* we)
    p.libsystem("_wordfree", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (s.x[0]) free_words(p, m, s.x[0]);
        return 0;
    }, 0);
}

} // namespace alchemy::export_fill
