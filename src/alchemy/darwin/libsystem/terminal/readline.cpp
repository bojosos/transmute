#include "alchemy/engine_adapter.hpp"
#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// libedit's readline API (/usr/lib/libedit.3.dylib), as Python's readline module uses it: the
// callback interface over rl_instream's descriptor, history, completion and key bindings.
//
// Input is read a byte at a time from the descriptor in whatever mode the terminal is in (the
// terminal's own line discipline echoes and edits a cooked line), so it works on consoles,
// pipes and pseudo-terminals alike; rl_prep_terminal changes no terminal mode. A line ends at
// "\n" or "\r"; end of input on an empty line delivers NULL. A Tab bound to rl_complete (the
// default, and Python's "tab: complete") completes the current word through
// rl_attempted_completion_function, as GNU readline does; other bindings are kept (rl_bind_key,
// rl_parse_and_bind, rl_variable_bind, rl_read_init_file) and do not change how bytes are read.
// History: entries are malloc'd libedit HIST_ENTRY {line, data} (the structures are
// libedit's, the library's ABI), history_base is 1, and history files are one line per entry. rl_library_version reports GNU
// readline 8.1, whose semantics these are (Python's libedit emulation is not needed).
namespace alchemy::export_fill {
namespace {

using Address = std::uint64_t;

constexpr std::string_view libedit = "/usr/lib/libedit.3.dylib";
constexpr std::uint32_t enoent_value = 2;

struct State {
    std::mutex mutex;
    std::vector<Address> history;           // HIST_ENTRY* (guest malloc), oldest first
    std::string line;                       // the line being read
    std::string prompt;
    Address handler{};                      // rl_vcpfunc_t (the callback handler)
    Address line_cell{}, line_buffer{};     // rl_line_buffer and its storage
    std::size_t line_capacity{};
    std::array<Address, 256> bindings{};    // rl_bind_key: key -> rl_command_func_t
    std::vector<std::string> settings;      // rl_parse_and_bind / rl_variable_bind lines, kept
    // The variables (guest cells, read at the time of use).
    Address instream{}, outstream{}, attempted_completion{}, attempted_over{}, completer_breaks{},
        append_character{}, display_hook{}, startup_hook{}, pre_input_hook{}, complete_function{};
};

class Readline {
public:
    explicit Readline(Provider& provider) : p(provider) {}
    Provider p;
    std::shared_ptr<State> state = std::make_shared<State>();

    GuestMemory& memory() { return p.process().calling_engine(); }
    std::uint64_t sys(const char* name, std::initializer_list<std::uint64_t> arguments) { return p.call(libsystem_library, name, arguments); }
    std::uint64_t call(Address function, std::initializer_list<std::uint64_t> arguments) {
        return EngineAdapter(p.process().calling_engine()).call(function, std::span(arguments.begin(), arguments.size()));
    }
    Address strdup(std::string_view text) {
        const auto copy = p.malloc(text.size() + 1);
        put_string(memory(), copy, text);
        return copy;
    }
    // A FILE*'s descriptor (NULL: the standard stream).
    std::int32_t descriptor(Address cell, std::int32_t fallback) {
        const auto file = memory().read64(cell);
        return file ? static_cast<std::int32_t>(sys("_fileno", {file})) : fallback;
    }
    void write_out(std::string_view text) {
        if (text.empty()) return;
        const auto fd = descriptor(state->outstream, 1);
        const auto buffer = strdup(text);
        static_cast<void>(sys("_write", {static_cast<std::uint64_t>(fd), buffer, text.size()}));
        static_cast<void>(sys("_free", {buffer}));
    }
    // rl_line_buffer mirrors `line` (caller holds no lock).
    void publish_line() {
        std::string text;
        { std::lock_guard lock(state->mutex); text = state->line; }
        if (text.size() + 1 > state->line_capacity) {
            const auto capacity = std::max<std::size_t>(256, (text.size() + 1) * 2);
            const auto buffer = p.malloc(capacity);
            const auto previous = std::exchange(state->line_buffer, buffer);
            state->line_capacity = capacity;
            memory().write64(state->line_cell, buffer);
            if (previous) static_cast<void>(sys("_free", {previous}));
        }
        put_string(memory(), state->line_buffer, text);
    }
    // libedit's HIST_ENTRY {const char* line; histdata_t data}.
    Address make_entry(std::string_view line, Address data) {
        const auto entry = p.malloc(16);
        memory().write64(entry, strdup(line));
        memory().write64(entry + 8, data);
        return entry;
    }
    void free_entry(Address entry) {
        if (!entry) return;
        static_cast<void>(sys("_free", {memory().read64(entry)}));
        static_cast<void>(sys("_free", {entry}));
    }
    std::string entry_line(Address entry) { return guest_string(memory(), memory().read64(entry), 1 << 20); }
    // A whole file through the guest's open/read (NULL path: ~/.history).
    std::optional<std::string> read_file(const std::string& path) {
        const auto name = strdup(path);
        const auto fd = static_cast<std::int32_t>(sys("_open", {name, 0}));
        static_cast<void>(sys("_free", {name}));
        if (fd < 0) return std::nullopt;
        std::string text;
        const auto buffer = p.malloc(4096);
        for (;;) {
            const auto count = static_cast<std::int64_t>(sys("_read", {static_cast<std::uint64_t>(fd), buffer, 4096}));
            if (count <= 0) break;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
            memory().read_bytes(buffer, bytes);
            text.append(bytes.begin(), bytes.end());
        }
        static_cast<void>(sys("_free", {buffer}));
        static_cast<void>(sys("_close", {static_cast<std::uint64_t>(fd)}));
        return text;
    }
    // 0, or the errno open reported.
    std::uint64_t write_file(const std::string& path, const std::string& text) {
        const auto name = strdup(path);
        // open is variadic: the mode is a stack argument.
        CallRegs registers{};
        registers.x[0] = name; registers.x[1] = 0x601;   // O_WRONLY | O_CREAT | O_TRUNC
        registers.push_variadic(std::uint64_t{0600});
        const auto fd = static_cast<std::int32_t>(p.call_regs(libsystem_library, "_open", registers));
        static_cast<void>(sys("_free", {name}));
        if (fd < 0) return p.guest_errno();
        const auto buffer = strdup(text);
        static_cast<void>(sys("_write", {static_cast<std::uint64_t>(fd), buffer, text.size()}));
        static_cast<void>(sys("_free", {buffer}));
        static_cast<void>(sys("_close", {static_cast<std::uint64_t>(fd)}));
        return 0;
    }
    std::string history_path(Address path) {
        if (path) return guest_string(memory(), path, 4096);
        const auto name = strdup("HOME");
        const auto home = sys("_getenv", {name});
        static_cast<void>(sys("_free", {name}));
        return (home ? guest_string(memory(), home, 4096) : std::string()) + "/.history";
    }
    static std::vector<std::string> lines(const std::string& text) {
        std::vector<std::string> result;
        std::size_t at = 0;
        while (at < text.size()) {
            const auto end = text.find('\n', at);
            result.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return result;
    }

    // rl_completion_matches(text, generator): generator(text, state) until NULL; [0] is the
    // matches' common prefix (or the one match), then the matches, then NULL; NULL for none.
    Address completion_matches(Address text, Address generator) {
        std::vector<Address> matches;
        for (std::uint64_t index = 0;; ++index) {
            const auto match = call(generator, {text, index});
            if (!match) break;
            matches.push_back(match);
        }
        if (matches.empty()) return 0;
        const auto result = p.malloc((matches.size() + 2) * 8);
        if (matches.size() == 1) {
            memory().write64(result, matches.front());
            memory().write64(result + 8, 0);
            return result;
        }
        auto common = guest_string(memory(), matches.front(), 1 << 16);
        for (const auto match : matches) {
            const auto other = guest_string(memory(), match, 1 << 16);
            std::size_t length = 0;
            while (length < common.size() && length < other.size() && common[length] == other[length]) ++length;
            common.resize(length);
        }
        memory().write64(result, strdup(common));
        for (std::size_t index = 0; index < matches.size(); ++index) memory().write64(result + 8 * (index + 1), matches[index]);
        memory().write64(result + 8 * (matches.size() + 1), 0);
        return result;
    }
    // rl_complete: the word before the end of the line, completed as GNU readline does.
    void complete() {
        std::string line;
        { std::lock_guard lock(state->mutex); line = state->line; }
        const auto breaks_text = memory().read64(state->completer_breaks);
        const auto breaks = breaks_text ? guest_string(memory(), breaks_text, 256) : std::string(" \t\n\"\\'`@$><=;|&{(");
        std::size_t start = line.size();
        while (start > 0 && breaks.find(line[start - 1]) == std::string::npos) --start;
        const auto function = memory().read64(state->attempted_completion);
        if (!function) return;   // no default (file name) completion
        memory().write32(state->attempted_over, 0);
        const auto word = strdup(line.substr(start));
        const auto matches = call(function, {word, start, line.size()});
        static_cast<void>(sys("_free", {word}));
        if (!matches) return;
        std::vector<std::string> found;
        for (std::uint64_t index = 0;; ++index) {
            const auto match = memory().read64(matches + index * 8);
            if (!match) break;
            found.push_back(guest_string(memory(), match, 1 << 16));
            static_cast<void>(sys("_free", {match}));
        }
        static_cast<void>(sys("_free", {matches}));
        if (found.empty()) return;
        auto replacement = found.front();
        if (found.size() == 1) {
            const auto append = static_cast<char>(memory().read32(state->append_character) & 0xff);
            if (append) replacement.push_back(append);
        } else {
            std::string listing = "\n";
            for (std::size_t index = 1; index < found.size(); ++index) listing += found[index] + (index + 1 < found.size() ? "  " : "\n");
            write_out(listing);
        }
        std::string prompt;
        {
            std::lock_guard lock(state->mutex);
            state->line = line.substr(0, start) + replacement;
            prompt = state->prompt;
            line = state->line;
        }
        publish_line();
        write_out("\r" + prompt + line);
    }
    // rl_callback_read_char: one byte of input.
    void read_char() {
        const auto fd = descriptor(state->instream, 0);
        const auto cell = p.malloc(1);
        const auto count = static_cast<std::int64_t>(sys("_read", {static_cast<std::uint64_t>(fd), cell, 1}));
        const auto byte = count == 1 ? memory().read8(cell) : 0;
        static_cast<void>(sys("_free", {cell}));
        Address handler{};
        std::string line;
        bool deliver = false, eof = false;
        {
            std::lock_guard lock(state->mutex);
            handler = state->handler;
            if (count <= 0) { eof = state->line.empty(); deliver = true; line = std::exchange(state->line, {}); }
            else if (byte == '\n' || byte == '\r') { deliver = true; line = std::exchange(state->line, {}); }
        }
        if (!deliver) {
            if (byte == '\t' && state->bindings['\t'] == state->complete_function) { complete(); return; }
            if (const auto binding = state->bindings[byte]; binding && binding != state->complete_function) {
                static_cast<void>(call(binding, {1, static_cast<std::uint64_t>(byte)}));
                return;
            }
            { std::lock_guard lock(state->mutex); state->line.push_back(static_cast<char>(byte)); }
            publish_line();
            return;
        }
        publish_line();
        if (handler) static_cast<void>(call(handler, {eof ? 0 : strdup(line)}));
    }
};

} // namespace

void install_libedit_readline(Provider& p) {
    ProcessContext::add_provider_library(std::string(libedit));
    auto readline = std::make_shared<Readline>(p);
    auto& state = *readline->state;
    const auto pointer_cell = [&p](const char* name, Address value) {
        std::array<std::uint8_t, 8> bytes{};
        for (std::size_t index = 0; index < 8; ++index) bytes[index] = static_cast<std::uint8_t>(value >> (8 * index));
        return p.data(libedit, name, 8, bytes);
    };
    const auto int_cell = [&p](const char* name, std::int32_t value) {
        std::array<std::uint8_t, 4> bytes{};
        for (std::size_t index = 0; index < 4; ++index) bytes[index] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) >> (8 * index));
        return p.data(libedit, name, 4, bytes);
    };
    const auto string_value = [&p](std::string_view text) {
        const auto address = p.process().allocate_guest(text.size() + 1);
        put_string(p.process().main_engine(), address, text);
        return address;
    };
    static constexpr std::string_view word_breaks = " \t\n\"\\'`@$><=;|&{(";
    state.instream = pointer_cell("_rl_instream", 0);
    state.outstream = pointer_cell("_rl_outstream", 0);
    state.line_cell = pointer_cell("_rl_line_buffer", 0);
    state.attempted_completion = pointer_cell("_rl_attempted_completion_function", 0);
    state.attempted_over = int_cell("_rl_attempted_completion_over", 0);
    state.completer_breaks = pointer_cell("_rl_completer_word_break_characters", string_value(word_breaks));
    static_cast<void>(pointer_cell("_rl_basic_word_break_characters", string_value(word_breaks)));
    state.append_character = int_cell("_rl_completion_append_character", ' ');
    state.display_hook = pointer_cell("_rl_completion_display_matches_hook", 0);
    static_cast<void>(int_cell("_rl_completion_type", 0));
    state.startup_hook = pointer_cell("_rl_startup_hook", 0);
    state.pre_input_hook = pointer_cell("_rl_pre_input_hook", 0);
    static_cast<void>(pointer_cell("_rl_library_version", string_value("8.1")));
    static_cast<void>(int_cell("_rl_readline_version", 0x0801));
    static_cast<void>(pointer_cell("_rl_readline_name", string_value("other")));
    // Keymap: 256 KEYMAP_ENTRY {char type; rl_command_func_t* function} (the meta map, unused here).
    static_cast<void>(p.data(libedit, "_emacs_meta_keymap", 256 * 16, std::array<std::uint8_t, 0>{}));

    // ---- initialization and terminal ----
    p.function(libedit, "_rl_initialize", [readline](CpuState&, GuestMemory&) -> std::uint64_t { readline->publish_line(); return 0; });
    p.function(libedit, "_using_history", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
    // The terminal keeps its own mode (the reading above needs none).
    p.function(libedit, "_rl_prep_terminal", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
    p.function(libedit, "_rl_cleanup_after_signal", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
    p.function(libedit, "_rl_free_line_state", [readline](CpuState&, GuestMemory&) -> std::uint64_t {
        { std::lock_guard lock(readline->state->mutex); readline->state->line.clear(); }
        readline->publish_line();
        return 0;
    });

    // ---- the callback interface ----
    // rl_callback_handler_install(prompt, handler): the startup and pre-input hooks run, then the prompt is written.
    p.function(libedit, "_rl_callback_handler_install", [readline](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto prompt = s.x[0] ? guest_string(m, s.x[0], 4096) : std::string();
        {
            std::lock_guard lock(readline->state->mutex);
            readline->state->prompt = prompt;
            readline->state->handler = s.x[1];
            readline->state->line.clear();
        }
        readline->publish_line();
        for (const auto cell : {readline->state->startup_hook, readline->state->pre_input_hook})
            if (const auto hook = m.read64(cell)) static_cast<void>(readline->call(hook, {}));
        readline->write_out(prompt);
        return 0;
    });
    p.function(libedit, "_rl_callback_handler_remove", [readline](CpuState&, GuestMemory&) -> std::uint64_t {
        std::lock_guard lock(readline->state->mutex);
        readline->state->handler = 0;
        return 0;
    });
    p.function(libedit, "_rl_callback_read_char", [readline](CpuState&, GuestMemory&) -> std::uint64_t { readline->read_char(); return 0; });
    // int rl_insert_text(const char*) / rl_insert(int count, int key): at the end of the line.
    p.function(libedit, "_rl_insert_text", [readline](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto text = s.x[0] ? guest_string(m, s.x[0], 1 << 20) : std::string();
        { std::lock_guard lock(readline->state->mutex); readline->state->line += text; }
        readline->publish_line();
        return text.size();
    });
    p.function(libedit, "_rl_insert", [readline](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto count = static_cast<std::int32_t>(s.x[0]);
        { std::lock_guard lock(readline->state->mutex); if (count > 0) readline->state->line.append(static_cast<std::size_t>(count), static_cast<char>(s.x[1] & 0xff)); }
        readline->publish_line();
        return 0;
    });
    // rl_redisplay: the prompt and the line again, from the start of the terminal line.
    p.function(libedit, "_rl_redisplay", [readline](CpuState&, GuestMemory&) -> std::uint64_t {
        std::string text;
        { std::lock_guard lock(readline->state->mutex); text = "\r" + readline->state->prompt + readline->state->line; }
        readline->write_out(text);
        return 0;
    });

    // ---- completion ----
    p.function(libedit, "_rl_completion_matches", [readline](CpuState& s, GuestMemory&) -> std::uint64_t {
        return s.x[1] ? readline->completion_matches(s.x[0], s.x[1]) : 0;
    }, 0);
    // int rl_complete(int ignore, int key)
    p.function(libedit, "_rl_complete", [readline](CpuState&, GuestMemory&) -> std::uint64_t { readline->complete(); return 0; });
    state.complete_function = p.process().resolve(libedit, "_rl_complete")->address;
    state.bindings['\t'] = state.complete_function;

    // ---- bindings and settings (kept) ----
    for (const auto* name : {"_rl_bind_key", "_rl_bind_key_in_map"})
        p.function(libedit, name, [readline](CpuState& s, GuestMemory&) -> std::uint64_t {
            const auto key = static_cast<std::uint32_t>(s.x[0]);
            if (key > 255) return static_cast<std::uint64_t>(-1);
            readline->state->bindings[key] = s.x[1];
            return 0;
        });
    // int rl_parse_and_bind(char* line): "tab: complete" binds Tab to rl_complete; the rest is kept.
    p.function(libedit, "_rl_parse_and_bind", [readline](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto line = s.x[0] ? guest_string(m, s.x[0], 4096) : std::string();
        std::string lowered;
        for (const auto c : line) if (c != ' ') lowered.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
        if (lowered == "tab:complete") readline->state->bindings['\t'] = readline->state->complete_function;
        std::lock_guard lock(readline->state->mutex);
        readline->state->settings.push_back(line);
        return 0;
    });
    p.function(libedit, "_rl_variable_bind", [readline](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto line = (s.x[0] ? guest_string(m, s.x[0], 256) : std::string()) + " " + (s.x[1] ? guest_string(m, s.x[1], 256) : std::string());
        std::lock_guard lock(readline->state->mutex);
        readline->state->settings.push_back("set " + line);
        return 0;
    });
    // int rl_read_init_file(const char* file): its lines are kept as settings; errno when it cannot be read.
    p.function(libedit, "_rl_read_init_file", [readline](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::string path;
        if (s.x[0]) path = guest_string(m, s.x[0], 4096);
        else {
            const auto name = readline->strdup("HOME");
            const auto home = readline->sys("_getenv", {name});
            static_cast<void>(readline->sys("_free", {name}));
            path = (home ? guest_string(m, home, 4096) : std::string()) + "/.inputrc";
        }
        const auto text = readline->read_file(path);
        if (!text) return readline->p.guest_errno() ? readline->p.guest_errno() : enoent_value;
        std::lock_guard lock(readline->state->mutex);
        for (auto& line : Readline::lines(*text)) readline->state->settings.push_back(std::move(line));
        return 0;
    });

    // ---- history ----
    p.function(libedit, "_add_history", [readline](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto entry = readline->make_entry(s.x[0] ? guest_string(m, s.x[0], 1 << 20) : std::string(), 0);
        std::lock_guard lock(readline->state->mutex);
        readline->state->history.push_back(entry);
        return 0;
    });
    // HIST_ENTRY* history_get(int offset): offset counts from history_base (1).
    p.function(libedit, "_history_get", [readline](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto offset = static_cast<std::int32_t>(s.x[0]);
        std::lock_guard lock(readline->state->mutex);
        if (offset < 1 || static_cast<std::size_t>(offset) > readline->state->history.size()) return 0;
        return readline->state->history[static_cast<std::size_t>(offset - 1)];
    }, 0);
    // HIST_ENTRY* remove_history(int which) / replace_history_entry(int which, line, data): the
    // removed or replaced entry, which the caller frees.
    p.function(libedit, "_remove_history", [readline](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto which = static_cast<std::int32_t>(s.x[0]);
        std::lock_guard lock(readline->state->mutex);
        auto& history = readline->state->history;
        if (which < 0 || static_cast<std::size_t>(which) >= history.size()) return 0;
        const auto entry = history[static_cast<std::size_t>(which)];
        history.erase(history.begin() + which);
        return entry;
    }, 0);
    p.function(libedit, "_replace_history_entry", [readline](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto which = static_cast<std::int32_t>(s.x[0]);
        {
            std::lock_guard lock(readline->state->mutex);
            if (which < 0 || static_cast<std::size_t>(which) >= readline->state->history.size()) return 0;
        }
        const auto entry = readline->make_entry(s.x[1] ? guest_string(m, s.x[1], 1 << 20) : std::string(), s.x[2]);
        std::lock_guard lock(readline->state->mutex);
        return std::exchange(readline->state->history[static_cast<std::size_t>(which)], entry);
    }, 0);
    p.function(libedit, "_clear_history", [readline](CpuState&, GuestMemory&) -> std::uint64_t {
        std::vector<Address> entries;
        { std::lock_guard lock(readline->state->mutex); entries.swap(readline->state->history); }
        for (const auto entry : entries) readline->free_entry(entry);
        return 0;
    });
    // HISTORY_STATE* history_get_history_state(void): libedit's state, whose first field is the
    // number of entries (int length); malloc'd, the caller frees it.
    p.function(libedit, "_history_get_history_state", [readline](CpuState&, GuestMemory& m) -> std::uint64_t {
        std::size_t length{};
        { std::lock_guard lock(readline->state->mutex); length = readline->state->history.size(); }
        const auto result = readline->p.malloc(16);
        m.fill_bytes(result, 0, 16);
        m.write32(result, static_cast<std::uint32_t>(length));
        return result;
    }, 0);
    // int read_history(const char*) / write_history(const char*): one entry per line (NULL: ~/.history).
    p.function(libedit, "_read_history", [readline](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto text = readline->read_file(readline->history_path(s.x[0]));
        if (!text) return readline->p.guest_errno() ? readline->p.guest_errno() : enoent_value;
        for (const auto& line : Readline::lines(*text)) {
            const auto entry = readline->make_entry(line, 0);
            std::lock_guard lock(readline->state->mutex);
            readline->state->history.push_back(entry);
        }
        return 0;
    });
    p.function(libedit, "_write_history", [readline](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::vector<Address> entries;
        { std::lock_guard lock(readline->state->mutex); entries = readline->state->history; }
        std::string text;
        for (const auto entry : entries) text += readline->entry_line(entry) + "\n";
        return readline->write_file(readline->history_path(s.x[0]), text);
    });
    // int history_truncate_file(const char*, int lines): the file keeps its last `lines` lines.
    p.function(libedit, "_history_truncate_file", [readline](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto path = readline->history_path(s.x[0]);
        const auto text = readline->read_file(path);
        if (!text) return readline->p.guest_errno() ? readline->p.guest_errno() : enoent_value;
        auto all = Readline::lines(*text);
        const auto keep = static_cast<std::size_t>(std::max<std::int32_t>(0, static_cast<std::int32_t>(s.x[1])));
        if (all.size() > keep) all.erase(all.begin(), all.end() - static_cast<std::ptrdiff_t>(keep));
        std::string result;
        for (const auto& line : all) result += line + "\n";
        return readline->write_file(path, result);
    });
}

} // namespace alchemy::export_fill
