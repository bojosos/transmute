// The built-in POSIX shell subset of Alchemy children (/bin/sh, system(),
// popen()) and the env tool. Project-authored; it never runs a host shell.
// Unsupported syntax fails explicitly with status 2 (docs/ALCHEMY_PROCESS.md).
#define _CRT_SECURE_NO_WARNINGS
#include "alchemy/darwin_process.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/host/net.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sync.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

namespace alchemy {
namespace {

struct SyntaxError { std::string message; };

// ---- syntax -----------------------------------------------------------------------------------
struct Part {
    enum class Kind { literal, parameter } kind{Kind::literal};
    std::string text;         // literal text or parameter name
    std::string fallback;     // ${NAME:-word} / ${NAME-word}
    bool quoted{};            // inside quotes: no field splitting
    bool has_fallback{}, fallback_if_empty{};
};
struct Word {
    std::vector<Part> parts;
    bool any_quoted{};
    [[nodiscard]] bool plain(std::string_view value) const {
        return parts.size() == 1 && parts[0].kind == Part::Kind::literal && !parts[0].quoted && parts[0].text == value;
    }
    [[nodiscard]] std::optional<std::string> literal() const {
        std::string result;
        for (const auto& part : parts) { if (part.kind != Part::Kind::literal) return std::nullopt; result += part.text; }
        return result;
    }
};
struct Redirect { int fd{}; std::string op; Word target; };
struct Command { std::vector<std::pair<std::string, Word>> assignments; std::vector<Word> words; std::vector<Redirect> redirects; };
struct Pipeline { std::vector<Command> commands; bool negate{}; };
struct AndOr { std::vector<Pipeline> pipelines; std::vector<std::string> operators; };
using Script = std::vector<AndOr>;

struct Token {
    enum class Kind { word, op, io_number, end } kind{Kind::end};
    std::string op;
    Word word;
    int number{};
};

bool name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool name_char(char c) { return name_start(c) || (c >= '0' && c <= '9'); }

class Lexer {
public:
    explicit Lexer(std::string_view value) : input(value) {}
    std::vector<Token> tokens() {
        std::vector<Token> result;
        for (;;) {
            while (at < input.size() && (input[at] == ' ' || input[at] == '\t' || (input[at] == '\\' && at + 1 < input.size() && input[at + 1] == '\n'))) at += input[at] == '\\' ? 2 : 1;
            if (at >= input.size()) break;
            const char c = input[at];
            if (c == '#') { while (at < input.size() && input[at] != '\n') ++at; continue; }
            if (c == '\n' || c == ';') {
                if (c == ';' && at + 1 < input.size() && input[at + 1] == ';') throw SyntaxError{"case clauses (;;) are not supported"};
                ++at; result.push_back(op(";")); continue;
            }
            if (c == '&') {
                if (at + 1 < input.size() && input[at + 1] == '&') { at += 2; result.push_back(op("&&")); continue; }
                throw SyntaxError{"background jobs (&) are not supported"};
            }
            if (c == '|') {
                if (at + 1 < input.size() && input[at + 1] == '|') { at += 2; result.push_back(op("||")); continue; }
                ++at; result.push_back(op("|")); continue;
            }
            if (c == '(' || c == ')') throw SyntaxError{"subshells and functions are not supported"};
            if (c == '<' || c == '>') { result.push_back(redirection(-1)); continue; }
            // An all-digit word directly followed by a redirection is its descriptor.
            if (c >= '0' && c <= '9') {
                auto end = at;
                while (end < input.size() && input[end] >= '0' && input[end] <= '9') ++end;
                if (end < input.size() && (input[end] == '<' || input[end] == '>') && end - at < 4) {
                    const int fd = std::stoi(std::string(input.substr(at, end - at)));
                    at = end;
                    result.push_back(redirection(fd));
                    continue;
                }
            }
            Token token; token.kind = Token::Kind::word; token.word = word();
            result.push_back(std::move(token));
        }
        Token end; end.kind = Token::Kind::end;
        result.push_back(end);
        return result;
    }

private:
    static Token op(std::string value) { Token token; token.kind = Token::Kind::op; token.op = std::move(value); return token; }
    Token redirection(int fd) {
        std::string value(1, input[at++]);
        if (at < input.size()) {
            const char next = input[at];
            if (value == "<" && next == '<') throw SyntaxError{"here-documents are not supported"};
            if ((value == ">" && (next == '>' || next == '&' || next == '|')) || (value == "<" && (next == '&' || next == '>'))) { value += next; ++at; }
        }
        Token token; token.kind = Token::Kind::io_number; token.op = value;
        token.number = fd >= 0 ? fd : (value[0] == '<' ? 0 : 1);
        return token;
    }
    void literal(Word& result, std::string value, bool quoted) {
        if (!result.parts.empty() && result.parts.back().kind == Part::Kind::literal && result.parts.back().quoted == quoted)
            result.parts.back().text += value;
        else { Part part; part.text = std::move(value); part.quoted = quoted; result.parts.push_back(std::move(part)); }
    }
    void parameter(Word& result, bool quoted) {
        ++at;   // '$'
        if (at >= input.size()) { literal(result, "$", quoted); return; }
        Part part; part.kind = Part::Kind::parameter; part.quoted = quoted;
        const char c = input[at];
        if (c == '(') throw SyntaxError{"command substitution and arithmetic $(...) are not supported"};
        if (c == '{') {
            const auto end = input.find('}', at);
            if (end == std::string_view::npos) throw SyntaxError{"unterminated ${"};
            const auto body = std::string(input.substr(at + 1, end - at - 1));
            at = end + 1;
            std::size_t n = 0;
            if (!body.empty() && (body[0] == '?' || body[0] == '#' || body[0] == '@' || body[0] == '*' || body[0] == '$' || (body[0] >= '0' && body[0] <= '9'))) n = 1;
            else while (n < body.size() && name_char(body[n])) ++n;
            if (n == 0) throw SyntaxError{"unsupported parameter expansion ${" + body + "}"};
            part.text = body.substr(0, n);
            if (n < body.size()) {
                const auto rest = body.substr(n);
                if (rest.starts_with(":-")) { part.has_fallback = true; part.fallback_if_empty = true; part.fallback = rest.substr(2); }
                else if (rest.starts_with("-")) { part.has_fallback = true; part.fallback = rest.substr(1); }
                else throw SyntaxError{"unsupported parameter expansion ${" + body + "}"};
                if (part.fallback.find_first_of("$`\"'\\") != std::string::npos) throw SyntaxError{"unsupported parameter expansion ${" + body + "}"};
            }
        } else if (c == '?' || c == '#' || c == '@' || c == '*' || c == '$' || (c >= '0' && c <= '9')) {
            part.text = std::string(1, c); ++at;
        } else if (c == '!' || c == '-') {
            throw SyntaxError{std::string("unsupported special parameter $") + c};
        } else if (name_start(c)) {
            const auto begin = at;
            while (at < input.size() && name_char(input[at])) ++at;
            part.text = std::string(input.substr(begin, at - begin));
        } else { literal(result, "$", quoted); return; }
        result.parts.push_back(std::move(part));
    }
    Word word() {
        Word result;
        bool first = true;
        while (at < input.size()) {
            const char c = input[at];
            if (c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '<' || c == '>' || c == '(' || c == ')') break;
            if (c == '\'') {
                const auto end = input.find('\'', at + 1);
                if (end == std::string_view::npos) throw SyntaxError{"unterminated single quote"};
                literal(result, std::string(input.substr(at + 1, end - at - 1)), true);
                result.any_quoted = true;
                at = end + 1;
            } else if (c == '"') {
                ++at; result.any_quoted = true;
                literal(result, "", true);
                for (;;) {
                    if (at >= input.size()) throw SyntaxError{"unterminated double quote"};
                    const char d = input[at];
                    if (d == '"') { ++at; break; }
                    if (d == '`') throw SyntaxError{"command substitution (backquotes) is not supported"};
                    if (d == '\\' && at + 1 < input.size() && std::string_view("$`\"\\\n").find(input[at + 1]) != std::string_view::npos) {
                        if (input[at + 1] != '\n') literal(result, std::string(1, input[at + 1]), true);
                        at += 2; continue;
                    }
                    if (d == '$') { parameter(result, true); continue; }
                    literal(result, std::string(1, d), true); ++at;
                }
            } else if (c == '\\') {
                if (at + 1 < input.size()) { if (input[at + 1] != '\n') literal(result, std::string(1, input[at + 1]), true); at += 2; }
                else { literal(result, "\\", false); ++at; }
            } else if (c == '$') {
                parameter(result, false);
            } else if (c == '`') {
                throw SyntaxError{"command substitution (backquotes) is not supported"};
            } else if (c == '*' || c == '?') {
                throw SyntaxError{std::string("pathname expansion (") + c + ") is not supported; quote the character"};
            } else if (c == '~' && first && (at + 1 >= input.size() || input[at + 1] == '/' || input[at + 1] == ' ' || input[at + 1] == '\t' || input[at + 1] == '\n' || input[at + 1] == ';')) {
                Part part; part.kind = Part::Kind::parameter; part.text = "HOME"; part.quoted = true;
                result.parts.push_back(std::move(part)); ++at;
            } else {
                literal(result, std::string(1, c), false); ++at;
            }
            first = false;
        }
        return result;
    }
    std::string_view input;
    std::size_t at{};
};

class Parser {
public:
    explicit Parser(std::vector<Token> value) : tokens(std::move(value)) {}
    Script script() {
        Script result;
        for (;;) {
            while (is_op(";")) ++at;
            if (peek().kind == Token::Kind::end) break;
            result.push_back(and_or());
            if (peek().kind == Token::Kind::end) break;
            if (!is_op(";")) throw SyntaxError{"unexpected " + describe(peek())};
        }
        return result;
    }

private:
    const Token& peek() const { return tokens[at]; }
    bool is_op(std::string_view value) const { return peek().kind == Token::Kind::op && peek().op == value; }
    static std::string describe(const Token& token) {
        if (token.kind == Token::Kind::op || token.kind == Token::Kind::io_number) return "'" + token.op + "'";
        if (token.kind == Token::Kind::end) return "end of input";
        return "word";
    }
    AndOr and_or() {
        AndOr result;
        result.pipelines.push_back(pipeline());
        while (is_op("&&") || is_op("||")) {
            result.operators.push_back(peek().op); ++at;
            while (is_op(";")) {   // a newline may follow && and ||
                if (tokens[at].kind == Token::Kind::op && tokens[at].op == ";") ++at;
            }
            result.pipelines.push_back(pipeline());
        }
        return result;
    }
    Pipeline pipeline() {
        Pipeline result;
        if (peek().kind == Token::Kind::word && peek().word.plain("!")) { result.negate = true; ++at; }
        result.commands.push_back(command());
        while (is_op("|")) { ++at; while (is_op(";")) ++at; result.commands.push_back(command()); }
        return result;
    }
    Command command() {
        static const std::array<std::string_view, 17> reserved{"if", "then", "else", "elif", "fi", "case", "esac", "for",
            "while", "until", "do", "done", "in", "function", "{", "}", "[["};
        Command result;
        bool assignments = true;
        for (;;) {
            const auto& token = peek();
            if (token.kind == Token::Kind::io_number) {
                Redirect redirect; redirect.fd = token.number; redirect.op = token.op; ++at;
                if (peek().kind != Token::Kind::word) throw SyntaxError{"missing redirection target after '" + redirect.op + "'"};
                redirect.target = peek().word; ++at;
                result.redirects.push_back(std::move(redirect));
                continue;
            }
            if (token.kind != Token::Kind::word) break;
            const auto& word = token.word;
            if (result.words.empty()) {
                for (const auto name : reserved)
                    if (word.plain(name)) throw SyntaxError{"shell control structure '" + std::string(name) + "' is not supported"};
            }
            if (assignments && !word.parts.empty() && word.parts[0].kind == Part::Kind::literal && !word.parts[0].quoted) {
                const auto& head = word.parts[0].text;
                const auto equals = head.find('=');
                if (equals != std::string::npos && equals > 0 && name_start(head[0]) &&
                    std::all_of(head.begin(), head.begin() + static_cast<std::ptrdiff_t>(equals), name_char)) {
                    Word value = word;
                    value.parts[0].text = head.substr(equals + 1);
                    result.assignments.emplace_back(head.substr(0, equals), std::move(value));
                    ++at;
                    continue;
                }
            }
            assignments = false;
            result.words.push_back(word);
            ++at;
        }
        if (result.words.empty() && result.assignments.empty() && result.redirects.empty())
            throw SyntaxError{"missing command before " + describe(peek())};
        return result;
    }
    std::vector<Token> tokens;
    std::size_t at{};
};

Script parse(std::string_view text) { return Parser(Lexer(text).tokens()).script(); }

// ---- execution ---------------------------------------------------------------------------------
struct Slot {
    host::Handle handle;
    std::uint64_t flags{};   // Darwin O_ACCMODE
    std::shared_ptr<void> owner;   // closes a handle this shell opened
    // A socket descriptor (a Node parent's stdio is a socket pair): how it
    // was handed over; `handle` is the socket imported here.
    std::shared_ptr<const ChildFd> socket;
};
using FdTable = std::map<int, Slot>;

std::shared_ptr<void> own(host::Handle handle) {
    return std::shared_ptr<void>(reinterpret_cast<void*>(handle.value), [](void* value) {
        if (value) host::close(host::Handle{reinterpret_cast<std::uintptr_t>(value)});
    });
}

// A descriptor handed over in the launch specification: a host handle
// value, or a socket opened here from its export (the handle is closed with
// the slot's owner).
std::optional<Slot> launch_slot(const ChildFd& fd) {
    if (fd.kind == ChildFdKind::handle) return Slot{host::Handle{static_cast<std::uintptr_t>(fd.handle)}, fd.flags & 3, {}, {}};
    if (fd.kind != ChildFdKind::socket || !host::net::initialize()) return std::nullopt;
    const auto imported = host::net::import_socket(fd.blob);
    if (!imported) return std::nullopt;
    auto described = std::make_shared<ChildFd>(fd);
    described->blob.clear();
    described->handle = static_cast<std::uint64_t>(imported->value);
    const auto owner = std::shared_ptr<void>(reinterpret_cast<void*>(imported->value), [](void* value) {
        host::net::close_socket(host::Handle{reinterpret_cast<std::uintptr_t>(value)});
    });
    return Slot{*imported, 2, owner, std::move(described)};
}

// Host sockets are non-blocking: wait for room or data instead of failing.
bool socket_ready(host::Handle socket, std::uint16_t events) {
    const auto ready = host::net::ready(socket, events, 1'000'000'000ULL);
    return ready && !(*ready & (host::net::ready_error | host::net::ready_invalid));
}

void write_all(const Slot& slot, std::string_view text) {
    while (!text.empty()) {
        const auto chunk = std::as_bytes(std::span(text.data(), std::min<std::size_t>(text.size(), 1U << 20)));
        const auto written = slot.socket ? host::net::send(slot.handle, chunk) : host::files::write(slot.handle, chunk);
        if (slot.socket && !written && written.error() == host::Error::would_block) {
            if (!socket_ready(slot.handle, host::net::ready_writable)) return;
            continue;
        }
        if (!written || !*written) return;
        text.remove_prefix(*written);
    }
}

std::string read_all(const Slot& slot) {
    std::string result;
    std::array<std::byte, 65536> buffer{};
    for (;;) {
        const auto count = slot.socket ? host::net::receive(slot.handle, buffer) : host::files::read(slot.handle, buffer);
        if (slot.socket && !count && count.error() == host::Error::would_block) {
            if (!socket_ready(slot.handle, host::net::ready_readable)) break;
            continue;
        }
        if (!count || !*count) break;
        result.append(reinterpret_cast<const char*>(buffer.data()), *count);
    }
    return result;
}

class Shell {
public:
    Shell(const ChildLaunch& launch, std::vector<std::string> options) : launch_options(std::move(options)) {
        env = launch.envp;
        cwd = launch.cwd.empty() ? std::filesystem::current_path() : launch.cwd;
        pid = launch.pid ? launch.pid : host::process::current_pid();
        ppid = launch.ppid; pgid = launch.pgid ? launch.pgid : pid; sid = launch.sid ? launch.sid : pid;
        umask = launch.umask; ignored = launch.ignored_signals;
        for (const auto& fd : launch.fds)
            if (auto slot = launch_slot(fd)) fds[fd.fd] = std::move(*slot);
        exec_environment.main_executable = launch.host_program;
        exec_environment.host_environment = host::process::environment_block();
        exec_environment.map_path = [this](std::string_view value) { return map(value); };
    }

    std::vector<std::string> positional;   // $0, $1, ...
    bool errexit{};

    int run_script(std::string_view text) {
        Script script;
        try { script = parse(text); }
        catch (const SyntaxError& error) { diagnose(error.message); return 2; }
        try { return execute(script); }
        catch (const SyntaxError& error) { diagnose(error.message); return 2; }
        catch (const ExitRequest& request) { return request.status; }
    }
    // The tools' own output and the script read from standard input.
    void output(int fd, std::string_view text) { write_to(fds, fd, text); }
    std::string standard_input() { const auto found = fds.find(0); return found == fds.end() ? std::string() : read_all(found->second); }
    void diagnose(std::string_view message) {
        const auto found = fds.find(2);
        if (found != fds.end()) write_all(found->second, "sh: " + std::string(message) + "\n");
    }
    std::filesystem::path map(std::string_view value) {
        if (value == "/dev/null") return std::filesystem::path(L"NUL");
        auto result = utf8_path(value).lexically_normal();
        if (value.find('\\') != std::string_view::npos) throw ProcessError(22, std::string(value) + ": invalid guest path");
        if (result.has_root_directory() && !result.has_root_name()) {
            const auto normalized = std::string(value);
            if (normalized == "/tmp" || normalized.starts_with("/tmp/")) {
                result = std::filesystem::temp_directory_path();
                if (normalized.size() > 5) result /= utf8_path(normalized.substr(5));
            } else if (normalized.starts_with("/Volumes/") && normalized.size() >= 10) {
                std::string native{normalized[9], ':', '/'};
                if (normalized.size() > 11) native += normalized.substr(11);
                result = utf8_path(native);
            } else if (auto in_root = guest_root_host_path(text_of(result))) {
                result = std::move(*in_root);
            } else result = cwd.root_path() / result.relative_path();
        }
        if (result.is_relative()) result = cwd / result;
        return result.lexically_normal();
    }

private:
    struct ExitRequest { int status; };

    static std::string text_of(const std::filesystem::path& value) {
        const auto bytes = value.generic_u8string();
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
    std::optional<std::string> variable(std::string_view name) const {
        if (name == "?") return std::to_string(status);
        if (name == "$") return std::to_string(pid);
        if (name == "#") return std::to_string(positional.empty() ? 0 : positional.size() - 1);
        if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
            const auto index = static_cast<std::size_t>(name[0] - '0');
            if (index < positional.size()) return positional[index];
            return std::nullopt;
        }
        if (name == "@" || name == "*") {
            std::string joined;
            for (std::size_t i = 1; i < positional.size(); ++i) { if (i > 1) joined += ' '; joined += positional[i]; }
            return joined;
        }
        for (const auto& entry : env)
            if (entry.size() > name.size() && entry.compare(0, name.size(), name) == 0 && entry[name.size()] == '=') return entry.substr(name.size() + 1);
        return std::nullopt;
    }
    // Expands a word into fields (unquoted parameter values split on blanks).
    std::vector<std::string> expand(const Word& word) const {
        std::vector<std::string> fields;
        std::string current;
        bool have = false;
        for (const auto& part : word.parts) {
            if (part.kind == Part::Kind::literal) { current += part.text; have = have || !part.text.empty() || part.quoted; continue; }
            if (part.text == "@" && part.quoted) {
                // "$@": one field per positional parameter.
                for (std::size_t i = 1; i < positional.size(); ++i) {
                    current += positional[i];
                    if (i + 1 < positional.size()) { fields.push_back(current); current.clear(); }
                    have = true;
                }
                continue;
            }
            auto value = variable(part.text);
            if (part.has_fallback && (!value || (part.fallback_if_empty && value->empty()))) value = part.fallback;
            const auto text = value.value_or("");
            if (part.quoted) { current += text; have = true; continue; }
            std::size_t start = 0;
            while (start <= text.size()) {
                const auto blank = text.find_first_of(" \t\n", start);
                const auto piece = text.substr(start, blank == std::string::npos ? std::string::npos : blank - start);
                current += piece;
                if (!piece.empty()) have = true;
                if (blank == std::string::npos) break;
                if (have) { fields.push_back(current); current.clear(); have = false; }
                start = blank + 1;
            }
        }
        if (have || word.any_quoted) fields.push_back(current);
        return fields;
    }
    std::string expand_one(const Word& word) const {
        auto fields = expand(word);
        std::string result;
        for (std::size_t i = 0; i < fields.size(); ++i) { if (i) result += ' '; result += fields[i]; }
        return result;
    }

    int execute(const Script& script) {
        for (const auto& list : script) {
            status = run_and_or(list);
        }
        return status;
    }
    int run_and_or(const AndOr& list) {
        auto result = run_pipeline(list.pipelines[0]);
        for (std::size_t i = 0; i < list.operators.size(); ++i) {
            status = result;
            if ((list.operators[i] == "&&") == (result == 0)) result = run_pipeline(list.pipelines[i + 1]);
        }
        if (errexit && result != 0 && list.operators.empty()) throw ExitRequest{result};
        return result;
    }

    int run_pipeline(const Pipeline& pipeline) {
        int result = 0;
        if (pipeline.commands.size() == 1) result = run_command(pipeline.commands[0], fds, false);
        else {
            // Every stage runs at once, connected by host pipes. Built-in stages
            // run on threads of this shell.
            std::vector<FdTable> tables(pipeline.commands.size(), fds);
            for (std::size_t i = 0; i + 1 < pipeline.commands.size(); ++i) {
                const auto pipe = host::files::create_pipe();
                if (!pipe) throw ProcessError(5, "pipe creation failed");
                tables[i][1] = Slot{pipe->write, 1, own(pipe->write), {}};
                tables[i + 1][0] = Slot{pipe->read, 0, own(pipe->read), {}};
            }
            std::vector<std::thread> threads;
            std::vector<int> results(pipeline.commands.size());
            for (std::size_t i = 0; i < pipeline.commands.size(); ++i) {
                threads.emplace_back([&, i] {
                    try { results[i] = run_command(pipeline.commands[i], tables[i], true); }
                    catch (const ExitRequest& request) { results[i] = request.status; }
                    catch (const std::exception& error) { diagnose(error.what()); results[i] = 2; }
                    tables[i].clear();   // close this stage's pipe ends so its neighbours see EOF
                });
            }
            for (auto& thread : threads) thread.join();
            result = results.back();
        }
        return pipeline.negate ? (result == 0 ? 1 : 0) : result;
    }

    void apply_redirects(const std::vector<Redirect>& redirects, FdTable& table) {
        for (const auto& redirect : redirects) {
            const auto target = expand_one(redirect.target);
            if (redirect.op == ">&" || redirect.op == "<&") {
                if (target == "-") { table.erase(redirect.fd); continue; }
                if (target.empty() || !std::all_of(target.begin(), target.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    throw SyntaxError{"redirection " + redirect.op + target + " needs a descriptor number"};
                const auto found = table.find(std::stoi(target));
                if (found == table.end()) throw ProcessError(9, target + ": bad file descriptor");
                table[redirect.fd] = found->second;
                continue;
            }
            const auto host = map(target);
            using host::files::Access;
            using host::files::Create;
            auto access = Access::read;
            auto create = Create::open_existing;
            std::uint32_t options = 0;
            std::uint64_t flags = 0;
            if (redirect.op == ">" || redirect.op == ">|") { access = Access::write; create = Create::create_or_truncate; flags = 1; }
            else if (redirect.op == ">>") { access = Access::write; create = Create::open_or_create; options = host::files::open_append; flags = 1 | 8; }
            else if (redirect.op == "<>") { access = Access::read_write; create = Create::open_or_create; flags = 2; }
            // A directory is not a redirection target (the host could open one).
            std::error_code status_error;
            if (std::filesystem::is_directory(host, status_error)) throw ProcessError(2, target + ": cannot open");
            const auto native = host.u8string();
            const auto handle = host::files::open(std::string(reinterpret_cast<const char*>(native.data()), native.size()), access, create, options);
            if (!handle) throw ProcessError(2, target + ": cannot open");
            table[redirect.fd] = Slot{*handle, flags, own(*handle), {}};
        }
    }

    int wait_for(SpawnedProcess child) {
        const std::array processes{host::Handle{reinterpret_cast<std::uintptr_t>(child.process)}};
        static_cast<void>(host::sync::wait_any(processes, std::nullopt));
        const auto code = host::process::exit_code(processes[0]).value_or(0);
        host::close(processes[0]);
        const auto value = darwin_wait_status(code);
        return (value & 0x7f) ? 128 + static_cast<int>(value & 0x7f) : static_cast<int>((value >> 8) & 0xff);
    }

    int run_command(const Command& command, FdTable& base, bool in_pipeline) {
        FdTable table = base;
        std::vector<std::string> argv;
        for (const auto& word : command.words) for (auto& field : expand(word)) argv.push_back(std::move(field));
        std::vector<std::pair<std::string, std::string>> assignments;
        for (const auto& [name, value] : command.assignments) assignments.emplace_back(name, expand_one(value));
        try { apply_redirects(command.redirects, table); }
        catch (const ProcessError& error) { write_to(table, 2, std::string("sh: ") + error.what() + "\n"); return 1; }
        if (argv.empty()) {
            for (const auto& [name, value] : assignments) set_variable(name, value);
            return 0;
        }
        const auto& name = argv[0];
        // Built-ins.
        if (name == ":" || name == "true") return 0;
        if (name == "false") return 1;
        if (name == "exit") {
            const int code = argv.size() > 1 ? std::atoi(argv[1].c_str()) & 0xff : status;
            if (in_pipeline) return code;
            throw ExitRequest{code};
        }
        if (name == "echo") {
            std::size_t first = 1; bool newline = true;
            if (argv.size() > 1 && argv[1] == "-n") { newline = false; first = 2; }
            std::string text;
            for (std::size_t i = first; i < argv.size(); ++i) { if (i > first) text += ' '; text += argv[i]; }
            if (newline) text += '\n';
            write_to(table, 1, text);
            return 0;
        }
        if (name == "cd") {
            const auto target = argv.size() > 1 ? argv[1] : variable("HOME").value_or("/");
            const auto host = map(target);
            std::error_code error;
            if (!std::filesystem::is_directory(host, error)) { write_to(table, 2, "sh: cd: " + target + ": No such directory\n"); return 1; }
            cwd = host;
            set_variable("PWD", guest_path_string(host));
            return 0;
        }
        if (name == "export" || name == "unset") {
            for (std::size_t i = 1; i < argv.size(); ++i) {
                const auto equals = argv[i].find('=');
                if (name == "unset") { std::erase_if(env, [&](const std::string& item) { return item.starts_with(argv[i] + "="); }); continue; }
                if (equals != std::string::npos) set_variable(argv[i].substr(0, equals), argv[i].substr(equals + 1));
            }
            return 0;
        }
        if (name == "command") {
            // command -v NAME: built-ins print their name, programs their path.
            if (argv.size() != 3 || argv[1] != "-v") { write_to(table, 2, "sh: command: only 'command -v NAME' is supported\n"); return 2; }
            static const std::array<std::string_view, 12> builtins{":", "true", "false", "exit", "echo", "cd", "export",
                "unset", "exec", "command", "getconf", "ps"};
            if (std::find(builtins.begin(), builtins.end(), argv[2]) != builtins.end()) {
                write_to(table, 1, argv[2] + "\n"); return 0;
            }
            try { write_to(table, 1, search_path(argv[2], env, exec_environment) + "\n"); return 0; }
            catch (const ProcessError&) { return 1; }
        }
        if (name == "getconf") {
            if (argv.size() == 2 && argv[1] == "ARG_MAX") { write_to(table, 1, "1048576\n"); return 0; }   // Darwin's ARG_MAX
            write_to(table, 2, "getconf: unsupported variable (Alchemy built-in shell)\n");
            return 1;
        }
        if (name == "ps") {
            // No process list is visible to a guest: only the header.
            write_to(table, 1, "  PID COMMAND\n");
            return 0;
        }
        bool replace = false;
        if (name == "exec") {
            if (argv.size() == 1) { if (!in_pipeline) fds = table; return 0; }
            argv.erase(argv.begin());
            replace = true;
        }
        static const std::array<std::string_view, 12> unsupported{"[", "test", "read", "set", "shift", "eval", ".", "source",
            "trap", "wait", "printf", "local"};
        if (std::find(unsupported.begin(), unsupported.end(), argv[0]) != unsupported.end())
            throw SyntaxError{"built-in '" + argv[0] + "' is not supported"};
        auto command_env = env;
        for (const auto& [variable_name, value] : assignments) set_in(command_env, variable_name, value);
        SpawnRequest request;
        try {
            const auto path = search_path(argv[0], command_env, exec_environment);
            request.target = resolve_exec(path, argv, command_env, exec_environment);
        } catch (const ProcessError& error) {
            write_to(table, 2, "sh: " + argv[0] + ": " + (error.error() == 2 ? std::string("command not found") : std::string(error.what())) + "\n");
            return error.error() == 2 ? 127 : 126;
        }
        if (request.target.tool == "env") {
            std::string text;
            for (const auto& entry : command_env) text += entry + "\n";
            write_to(table, 1, text);
            return 0;
        }
        request.envp = std::move(command_env);
        request.cwd = cwd;
        request.ppid = replace ? ppid : pid;
        request.pid = replace ? pid : 0;
        request.pgid = pgid; request.sid = sid;
        request.umask = umask; request.ignored_signals = ignored;
        request.launch_options = launch_options;
        for (const auto& [fd, slot] : table) {
            ChildFd entry = slot.socket ? *slot.socket : ChildFd{};
            entry.fd = fd; entry.kind = slot.socket ? ChildFdKind::socket : ChildFdKind::handle;
            entry.handle = static_cast<std::uint64_t>(slot.handle.value);
            if (!slot.socket) entry.flags = slot.flags;
            request.fds.push_back(std::move(entry));
        }
        if (request.target.tool == "sh") request.target.host_program = exec_environment.main_executable;
        int result;
        try { result = wait_for(spawn_child(request)); }
        catch (const ProcessError& error) { write_to(table, 2, "sh: " + argv[0] + ": " + error.what() + "\n"); result = 126; }
        if (replace && !in_pipeline) throw ExitRequest{result};
        return result;
    }

    void write_to(const FdTable& table, int fd, std::string_view text) {
        const auto found = table.find(fd);
        if (found != table.end()) write_all(found->second, text);
    }
    static void set_in(std::vector<std::string>& values, std::string_view name, std::string_view value) {
        const auto entry = std::string(name) + "=" + std::string(value);
        for (auto& item : values) if (item.size() > name.size() && item.compare(0, name.size(), name) == 0 && item[name.size()] == '=') { item = entry; return; }
        values.push_back(entry);
    }
    void set_variable(std::string_view name, std::string_view value) { set_in(env, name, value); }

    std::vector<std::string> launch_options;
    ExecEnvironment exec_environment;
    std::vector<std::string> env;
    std::filesystem::path cwd;
    FdTable fds;
    int status{};
    std::uint32_t pid{}, ppid{}, pgid{}, sid{}, umask{022}, ignored{};
};

} // namespace

std::optional<std::string> shell_syntax_error(std::string_view script) {
    try { static_cast<void>(parse(script)); return std::nullopt; }
    catch (const SyntaxError& error) { return error.message; }
}

int run_child_tool(const ChildLaunch& launch, const std::vector<std::string>& launch_options) {
    Shell shell(launch, launch_options);
    if (launch.tool == "env") {
        std::string text;
        for (const auto& entry : launch.envp) text += entry + "\n";
        shell.output(1, text);
        return 0;
    }
    if (launch.tool == "socketfilterfw") {
        // The macOS application firewall does not exist under Alchemy:
        // incoming connections of guest programs are not filtered by it, so
        // adding, unblocking or removing an application is a successful no-op.
        const auto output = [&](int which, std::string_view text) { shell.output(which, text); };
        const auto& argv = launch.argv;
        if (argv.size() >= 2) {
            const auto& option = argv[1];
            if (option == "--getglobalstate") { output(1, "Firewall is disabled. (State = 0)\n"); return 0; }
            if (option == "--listapps") { output(1, "ALF: total number of apps = 0 \n"); return 0; }
            if ((option == "--add" || option == "--remove" || option == "--unblockapp" || option == "--blockapp") && argv.size() >= 3) {
                output(1, "Alchemy: no application firewall; nothing to change for " + argv[2] + "\n");
                return 0;
            }
        }
        output(2, "socketfilterfw: unsupported option (Alchemy has no application firewall)\n");
        return 1;
    }
    if (launch.tool != "sh") throw std::runtime_error("unknown Alchemy child tool " + launch.tool);
    const auto& argv = launch.argv;
    std::size_t index = 1;
    std::optional<std::string> command;
    for (; index < argv.size(); ++index) {
        const auto& argument = argv[index];
        if (argument == "--") { ++index; break; }
        if (argument.size() < 2 || argument[0] != '-') break;
        for (std::size_t i = 1; i < argument.size(); ++i) {
            switch (argument[i]) {
            case 'c': command = std::string(); break;
            case 'e': shell.errexit = true; break;
            case 'u': case 'x': case 'f': case 'h': break;   // accepted, no effect in this subset
            // Login and interactive shells read startup files and prompt; the
            // subset has neither, so with -c they run the command as any shell
            // (VS Code resolves the user's environment with `$SHELL -i -l -c`).
            case 'i': case 'l': break;
            default: shell.diagnose(std::string("unsupported option -") + argument[i]); return 2;
            }
        }
    }
    std::string script;
    if (command) {
        if (index >= argv.size()) { shell.diagnose("-c requires a command string"); return 2; }
        script = argv[index++];
        shell.positional.push_back(index < argv.size() ? argv[index++] : (argv.empty() ? std::string("sh") : argv[0]));
    } else if (index < argv.size()) {
        const auto file = argv[index++];
        std::ifstream input(shell.map(file), std::ios::binary);
        if (!input) { shell.diagnose(file + ": cannot open"); return 127; }
        std::ostringstream content; content << input.rdbuf();
        script = content.str();
        if (script.starts_with("#!")) { const auto end = script.find('\n'); script = end == std::string::npos ? std::string() : script.substr(end + 1); }
        shell.positional.push_back(file);
    } else {
        script = shell.standard_input();
        shell.positional.push_back(argv.empty() ? std::string("sh") : argv[0]);
    }
    for (; index < argv.size(); ++index) shell.positional.push_back(argv[index]);
    return shell.run_script(script);
}

} // namespace alchemy
