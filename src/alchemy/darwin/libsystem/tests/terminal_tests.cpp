// The terminal libraries (terminal/): libedit's readline API, terminfo's built-in xterm entry,
// curses and panels, over a pseudo-terminal whose slave is the guest's fd 0 and 1.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/call_regs.hpp"
#include "alchemy/path.hpp"

#include <filesystem>
#include <string>

namespace {
using namespace fill_test;

constexpr std::string_view sys = "/usr/lib/libSystem.B.dylib", edit = "/usr/lib/libedit.3.dylib",
    curses = "/usr/lib/libncurses.5.4.dylib", panel = "/usr/lib/libpanel.5.4.dylib";

struct Terminal {
    Fixture& f;
    std::int32_t master{}, slave{}, saved_in{}, saved_out{};
    explicit Terminal(Fixture& fixture) : f(fixture) {
        const auto ends = f.process.allocate_guest(8);
        require(f.call(sys, "_openpty", {ends, ends + 4, 0, 0, 0}) == 0, "openpty");
        master = static_cast<std::int32_t>(f.engine.read32(ends));
        slave = static_cast<std::int32_t>(f.engine.read32(ends + 4));
        // TIOCSWINSZ on the master: 30 rows, 100 columns.
        const auto size = f.process.allocate_guest(8);
        f.engine.write32(size, 30 | (100u << 16));
        f.engine.write32(size + 4, 0);
        alchemy::CallRegs registers{};
        registers.x[0] = static_cast<Address>(master); registers.x[1] = 0x80087467;
        registers.push_variadic(size);
        require(alchemy::engine_call_regs(f.engine, f.symbol(sys, "_ioctl"), registers).x0 == 0, "TIOCSWINSZ");
        saved_in = static_cast<std::int32_t>(f.call(sys, "_dup", {0}));
        saved_out = static_cast<std::int32_t>(f.call(sys, "_dup", {1}));
        require(f.call(sys, "_dup2", {static_cast<Address>(slave), 0}) == 0 && f.call(sys, "_dup2", {static_cast<Address>(slave), 1}) == 1, "the slave is fd 0 and 1");
    }
    ~Terminal() {
        try {
            if (saved_in >= 0) static_cast<void>(f.call(sys, "_dup2", {static_cast<Address>(saved_in), 0}));
            if (saved_out >= 0) static_cast<void>(f.call(sys, "_dup2", {static_cast<Address>(saved_out), 1}));
        } catch (...) {}
    }
    // What the guest wrote to the terminal so far.
    std::string output() {
        std::string text;
        const auto poll = f.process.allocate_guest(8), buffer = f.process.allocate_guest(4096);
        for (;;) {
            f.engine.write32(poll, static_cast<std::uint32_t>(master));
            f.engine.write32(poll + 4, 1);
            if (static_cast<std::int32_t>(f.call(sys, "_poll", {poll, 1, 50})) <= 0) break;
            const auto count = static_cast<std::int64_t>(f.call(sys, "_read", {static_cast<Address>(master), buffer, 4096}));
            if (count <= 0) break;
            for (std::int64_t at = 0; at < count; ++at) text.push_back(static_cast<char>(f.memory().read8(buffer + static_cast<Address>(at))));
        }
        return text;
    }
    void type(std::string_view keys) {
        const auto text = f.text(keys);
        require(f.call(sys, "_write", {static_cast<Address>(master), text, keys.size()}) == keys.size(), "typed");
    }
};

void test_readline(Fixture& f) {
    Terminal terminal(f);
    static std::string received;
    static bool delivered = false;
    static Fixture* fixture = &f;
    const auto handler = f.process.install_function("_terminal_test_line", [](transmute::CpuState& s, transmute::GuestMemory&) {
        received = s.x[0] ? fixture->read_text(s.x[0]) : std::string("(null)");
        delivered = true;
        s.x[0] = 0;
    });
    require(f.call(edit, "_rl_initialize") == 0, "rl_initialize");
    require(f.read_text(f.engine.read64(f.symbol(edit, "_rl_library_version"))) == "8.1", "rl_library_version");
    f.call(edit, "_rl_callback_handler_install", {f.text("> "), handler});
    require(terminal.output().find("> ") != std::string::npos, "the prompt is written");
    terminal.type("abc\n");
    for (int index = 0; index < 4 && !delivered; ++index) f.call(edit, "_rl_callback_read_char");
    require(delivered && received == "abc", "the handler receives the line");
    f.call(edit, "_rl_callback_handler_remove");
    // History.
    f.call(edit, "_add_history", {f.text("one")});
    f.call(edit, "_add_history", {f.text("two")});
    const auto entry = f.call(edit, "_history_get", {2});
    require(entry && f.read_text(f.engine.read64(entry)) == "two" && f.call(edit, "_history_get", {3}) == 0, "history_get counts from 1");
    const auto history_state = f.call(edit, "_history_get_history_state");
    require(f.engine.read32(history_state) == 2, "history length (HISTORY_STATE starts with it)");
    const auto file = std::filesystem::temp_directory_path() / "alchemy-readline-history";
    const auto path = f.text(alchemy::guest_path_string(file));
    require(f.call(edit, "_write_history", {path}) == 0, "write_history");
    f.call(edit, "_clear_history");
    require(f.call(edit, "_history_get", {1}) == 0, "clear_history");
    require(f.call(edit, "_read_history", {path}) == 0 && f.read_text(f.engine.read64(f.call(edit, "_history_get", {1}))) == "one", "read_history");
    require(f.call(edit, "_history_truncate_file", {path, 1}) == 0, "history_truncate_file");
    f.call(edit, "_clear_history");
    require(f.call(edit, "_read_history", {path}) == 0 && f.read_text(f.engine.read64(f.call(edit, "_history_get", {1}))) == "two" &&
            f.call(edit, "_history_get", {2}) == 0, "the truncated file keeps the last line");
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    const auto removed = f.call(edit, "_remove_history", {0});
    require(removed && f.read_text(f.engine.read64(removed)) == "two", "remove_history returns the entry");
    // Completion matches from a generator: [0] is the common prefix.
    static int calls = 0;
    const auto generator = f.process.install_function("_terminal_test_generator", [](transmute::CpuState& s, transmute::GuestMemory&) {
        static const char* words[] = {"apple", "apricot"};
        const auto index = s.x[1];
        if (index >= 2) { s.x[0] = 0; return; }
        ++calls;
        s.x[0] = fixture->text(words[index]);
    });
    const auto matches = f.call(edit, "_rl_completion_matches", {f.text("ap"), generator});
    require(matches && f.read_text(f.engine.read64(matches)) == "ap" && f.read_text(f.engine.read64(matches + 8)) == "apple" &&
            f.read_text(f.engine.read64(matches + 16)) == "apricot" && f.engine.read64(matches + 24) == 0 && calls == 2, "rl_completion_matches");
}

void test_terminfo(Fixture& f) {
    const auto errret = f.process.allocate_guest(4);
    require(f.call(curses, "_setupterm", {f.text("xterm-256color"), 1, errret}) == 0 && f.engine.read32(errret) == 1, "setupterm finds xterm-256color");
    require(f.call(curses, "_tigetnum", {f.text("colors")}) == 256, "256 colours");
    require(f.read_text(f.call(curses, "_tigetstr", {f.text("smcup")})) == "\x1b[?1049h", "tigetstr");
    require(f.call(curses, "_tigetstr", {f.text("colors")}) == static_cast<Address>(-1), "a numeric capability is not a string");
    require(f.call(curses, "_tigetflag", {f.text("am")}) == 1, "tigetflag");
    const auto cup = f.call(curses, "_tigetstr", {f.text("cup")});
    // tparm is variadic: its parameters are on the stack.
    alchemy::CallRegs registers{};
    registers.x[0] = cup;
    for (const Address value : {4, 9, 0, 0, 0, 0, 0, 0, 0}) registers.push_variadic(value);
    require(f.read_text(alchemy::engine_call_regs(f.engine, f.symbol(curses, "_tparm"), registers).x0) == "\x1b[5;10H", "tparm cup");
    require(static_cast<std::int32_t>(f.call(curses, "_setupterm", {f.text("no-such-terminal"), 1, errret})) == -1 && f.engine.read32(errret) == 0,
            "an unknown terminal is not found");
}

void test_curses(Fixture& f) {
    Terminal terminal(f);
    const auto screen = f.call(curses, "_initscr");
    require(screen != 0 && f.engine.read64(f.symbol(curses, "_stdscr")) == screen, "initscr");
    require(f.engine.read32(f.symbol(curses, "_LINES")) == 30 && f.engine.read32(f.symbol(curses, "_COLS")) == 100, "the terminal's size");
    require(terminal.output().find("\x1b[?1049h") != std::string::npos, "the alternate screen");
    f.call(curses, "_cbreak");
    f.call(curses, "_noecho");
    f.call(curses, "_keypad", {screen, 1});
    // Text and colours.
    require(f.call(curses, "_start_color") == 0 && f.call(curses, "_init_pair", {1, 1, 0}) == 0, "colours");
    f.call(curses, "_wmove", {screen, 2, 3});
    f.call(curses, "_waddnstr", {screen, f.text("hello"), static_cast<Address>(-1)});
    require(f.call(curses, "_getcury", {screen}) == 2 && f.call(curses, "_getcurx", {screen}) == 8, "the cursor moves");
    f.call(curses, "_wattr_on", {screen, 1u << 8, 0});
    f.call(curses, "_waddch", {screen, 'X'});
    f.call(curses, "_wrefresh", {screen});
    const auto drawn = terminal.output();
    require(drawn.find("\x1b[3;4H") != std::string::npos && drawn.find("hello") != std::string::npos, "the text at row 3, column 4");
    require(drawn.find(";31") != std::string::npos && drawn.find("X") != std::string::npos, "the colour pair's foreground");
    f.call(curses, "_wmove", {screen, 2, 3});
    require((f.call(curses, "_winch", {screen}) & 0xff) == 'h', "winch reads the cell back");
    // Keys: an xterm cursor key decodes with keypad on; nothing waiting is ERR without a delay.
    terminal.type("\x1b[A");
    require(f.call(curses, "_wgetch", {screen}) == 0403, "KEY_UP");
    terminal.type("q");
    require(f.call(curses, "_wgetch", {screen}) == 'q', "a plain key");
    f.call(curses, "_wtimeout", {screen, 0});
    require(static_cast<std::int32_t>(f.call(curses, "_wgetch", {screen})) == -1, "no key: ERR");
    // A boxed window in a panel over stdscr.
    const auto window = f.call(curses, "_newwin", {5, 20, 10, 10});
    require(window != 0, "newwin");
    f.call(curses, "_wborder", {window, 0, 0, 0, 0, 0, 0, 0});
    const auto top = f.call(panel, "_new_panel", {window});
    require(top && f.call(panel, "_panel_below", {0}) == top, "the panel is on top");
    f.call(panel, "_update_panels");
    f.call(curses, "_doupdate");
    const auto boxed = terminal.output();
    require(boxed.find("\x1b[11;11H") != std::string::npos && boxed.find("\xe2\x94\x8c") != std::string::npos, "the box's corner at row 11, column 11");
    f.call(panel, "_hide_panel", {top});
    require(f.call(panel, "_panel_hidden", {top}) == 1 && f.call(panel, "_panel_below", {0}) == 0, "hide_panel");
    // endwin restores the terminal and leaves the alternate screen.
    f.call(curses, "_endwin");
    require(f.call(curses, "_isendwin") == 1 && terminal.output().find("\x1b[?1049l") != std::string::npos, "endwin");
}

} // namespace

int main() {
    return run("terminal", {{"readline", test_readline}, {"terminfo", test_terminfo}, {"curses", test_curses}});
}
