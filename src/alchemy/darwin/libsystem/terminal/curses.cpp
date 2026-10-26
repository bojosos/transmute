#include "alchemy/engine_adapter.hpp"
#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// libncurses' curses API (/usr/lib/libncurses.5.4.dylib) over an xterm-compatible terminal on the
// guest's standard input and output, as Python's curses module uses it. terminfo itself keeps
// no database (mach/machsys.cpp: setupterm fails): curses screens use a built-in
// xterm-256color description instead.
//
// Windows, subwindows (sharing their parent's cells), pads and the virtual and physical screens
// are cell grids (a character, its attributes and colour pair). doupdate writes the differences
// between them as ANSI/xterm sequences: cursor movement, SGR attributes, 256-colour pairs and
// UTF-8 text, the alternate character set as Unicode line drawing, in the alternate screen
// (smcup/rmcup) while curses is active. Input is read from fd 0: cbreak, raw, echo, nl and the
// delays are real (termios through tcgetattr/tcsetattr; echo is curses' own, as in ncurses),
// keypad decodes the xterm cursor, editing and function keys, mousemask turns on xterm SGR
// mouse reports, which getmouse returns, and a change of the terminal size (TIOCGWINSZ) reads
// as KEY_RESIZE. initscr needs a terminal on fd 0 and 1 whose attributes can be read (a pseudo-
// terminal; Alchemy's host console has none, so initscr stops there, as tcgetattr does).
// Colours cannot be redefined (can_change_color is FALSE).
namespace alchemy::export_fill {
namespace {

using Address = std::uint64_t;

constexpr std::string_view ncurses = "/usr/lib/libncurses.5.4.dylib";
constexpr std::uint64_t ok = 0, err = static_cast<std::uint64_t>(-1);
// chtype / attr_t bits (ncurses' NCURSES_BITS(mask, shift) = mask << (shift + 8)).
constexpr std::uint32_t a_chartext = 0xff, a_color = 0xff00, a_standout = 1u << 16, a_underline = 1u << 17, a_reverse = 1u << 18,
    a_blink = 1u << 19, a_dim = 1u << 20, a_bold = 1u << 21, a_altcharset = 1u << 22, a_invis = 1u << 23, a_protect = 1u << 24,
    a_attributes = 0xffffff00;
// Key codes (KEY_* in <curses.h>).
constexpr std::int32_t key_down = 0402, key_up = 0403, key_left = 0404, key_right = 0405, key_home = 0406, key_backspace = 0407,
    key_f0 = 0410, key_dc = 0512, key_ic = 0513, key_npage = 0522, key_ppage = 0523, key_enter = 0527, key_btab = 0541, key_end = 0550,
    key_mouse = 0631, key_resize = 0632;
// Mouse masks (NCURSES_MOUSE_VERSION 1).
constexpr std::uint64_t button_released = 01, button_pressed = 02, button_clicked = 04, button_shift = 0200000000, button_ctrl = 0100000000,
    button_alt = 0400000000, report_mouse_position = 01000000000;

struct Cell { char32_t ch{U' '}; std::uint32_t attrs{}; bool operator==(const Cell&) const = default; };

struct Grid {
    int rows{}, cols{};
    std::vector<Cell> cells;
    Grid(int r, int c) : rows(r), cols(c), cells(static_cast<std::size_t>(std::max(r, 0) * std::max(c, 0))) {}
    Cell& at(int y, int x) { return cells[static_cast<std::size_t>(y * cols + x)]; }
};

struct Window {
    Address guest{};                       // the WINDOW* (its first fields mirror struct _win_st)
    std::shared_ptr<Grid> grid;            // shared with the parent for subwindows
    int origin_y{}, origin_x{};            // where this window starts within `grid`
    int rows{}, cols{}, begy{}, begx{};    // size; screen position (pads: 0)
    int cury{}, curx{};
    int top{}, bottom{};                   // scrolling region
    std::uint32_t attrs{}, background{U' '};
    Window* parent{};
    int pary{-1}, parx{-1};
    bool pad{}, scroll{}, clear{}, leave{}, keypad{}, nodelay{}, immediate{}, sync{}, idl{true}, idc{true}, notimeout{};
    int delay{-1};                         // wtimeout: -1 blocking, 0 non-blocking, else milliseconds
    std::vector<bool> touched;
    Cell& cell(int y, int x) { return grid->at(origin_y + y, origin_x + x); }
};

struct MouseEvent { std::int16_t id{}; std::int32_t x{}, y{}, z{}; std::uint64_t state{}; };

class Curses {
public:
    explicit Curses(Provider& provider) : p(provider) {}
    Provider p;
    std::recursive_mutex mutex;
    std::map<Address, std::unique_ptr<Window>> windows;
    Window* stdscr{};
    std::unique_ptr<Grid> virtual_screen, physical;   // newscr, curscr contents
    Address curscr_window{}, newscr_window{};
    int lines{24}, columns{80};
    bool active{}, ended{}, cbreak_mode{}, raw_mode{}, echo_mode{true}, nl_mode{true}, cursor_dirty{true}, alternate{};
    int halfdelay_tenths{};
    int cursor_visibility{1};
    int vcury{}, vcurx{};                  // where doupdate leaves the cursor
    std::array<std::uint8_t, 72> shell_mode{}, program_mode{}, saved_tty{};
    bool shell_saved{}, program_saved{}, tty_saved{};
    std::deque<std::int32_t> pushback;     // ungetch / unget_wch
    std::deque<MouseEvent> mouse_events;
    std::uint64_t mouse_mask{};
    int mouse_interval{166};
    bool colors_started{}, default_colors{};
    std::array<std::pair<int, int>, 256> pairs{};
    // Cells: the variables guests read.
    Address stdscr_cell{}, curscr_cell{}, newscr_cell{}, lines_cell{}, cols_cell{}, colors_cell{}, pairs_cell{}, escdelay_cell{}, tabsize_cell{},
        acs_cell{};
    std::string out;                       // pending terminal output

    GuestMemory& memory() { return p.process().calling_engine(); }
    std::uint64_t sys(const char* name, std::initializer_list<std::uint64_t> arguments) { return p.call(libsystem_library, name, arguments); }
    std::int32_t escdelay() { return static_cast<std::int32_t>(memory().read32(escdelay_cell)); }
    std::int32_t tabsize() { const auto value = static_cast<std::int32_t>(memory().read32(tabsize_cell)); return value > 0 ? value : 8; }

    // ---- the terminal ----
    void flush() {
        if (out.empty()) return;
        const auto buffer = p.malloc(out.size());
        memory().write_bytes(buffer, std::span(reinterpret_cast<const std::uint8_t*>(out.data()), out.size()));
        for (std::size_t written = 0; written < out.size();) {
            const auto count = static_cast<std::int64_t>(sys("_write", {1, buffer + written, out.size() - written}));
            if (count <= 0) break;
            written += static_cast<std::size_t>(count);
        }
        static_cast<void>(sys("_free", {buffer}));
        out.clear();
    }
    bool get_mode(std::array<std::uint8_t, 72>& mode) {
        const auto buffer = p.malloc(72);
        const bool done = static_cast<std::int32_t>(sys("_tcgetattr", {0, buffer})) == 0;
        if (done) memory().read_bytes(buffer, mode);
        static_cast<void>(sys("_free", {buffer}));
        return done;
    }
    bool set_mode(const std::array<std::uint8_t, 72>& mode) {
        const auto buffer = p.malloc(72);
        memory().write_bytes(buffer, mode);
        const bool done = static_cast<std::int32_t>(sys("_tcsetattr", {0, 1 /* TCSADRAIN */, buffer})) == 0;
        static_cast<void>(sys("_free", {buffer}));
        return done;
    }
    // struct termios: c_iflag (8 bytes) at 0, c_oflag 8, c_cflag 16, c_lflag 24, c_cc[20] at 32.
    static std::uint64_t flag(const std::array<std::uint8_t, 72>& mode, std::size_t offset) {
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(mode[offset + index]) << (8 * index);
        return value;
    }
    static void set_flag(std::array<std::uint8_t, 72>& mode, std::size_t offset, std::uint64_t value) {
        for (std::size_t index = 0; index < 8; ++index) mode[offset + index] = static_cast<std::uint8_t>(value >> (8 * index));
    }
    // The terminal mode curses' settings call for: echo is curses' own; ICANON unless cbreak/raw;
    // ISIG and IXON unless raw; ICRNL with nl.
    void apply_mode() {
        if (!program_saved) return;
        auto mode = program_mode;
        constexpr std::uint64_t echo = 0x8, echoe = 0x2, echok = 0x4, echonl = 0x10, icanon = 0x100, isig = 0x80, iexten = 0x400;
        constexpr std::uint64_t icrnl = 0x100, ixon = 0x200, onlcr = 0x2;
        auto lflag = flag(mode, 24) & ~(echo | echoe | echok | echonl);
        auto iflag = flag(mode, 0);
        auto oflag = flag(mode, 8);
        if (cbreak_mode || raw_mode || halfdelay_tenths) lflag &= ~icanon; else lflag |= icanon;
        if (raw_mode) { lflag &= ~(isig | iexten); iflag &= ~ixon; } else lflag |= isig;
        if (nl_mode) iflag |= icrnl; else iflag &= ~icrnl;
        oflag &= ~onlcr;   // curses positions the cursor itself
        set_flag(mode, 24, lflag); set_flag(mode, 0, iflag); set_flag(mode, 8, oflag);
        mode[32 + 16] = 1;   // VMIN
        mode[32 + 17] = static_cast<std::uint8_t>(halfdelay_tenths);   // VTIME
        static_cast<void>(set_mode(mode));
    }
    // TIOCGWINSZ on fd 1: {rows, columns} when known.
    std::optional<std::pair<int, int>> terminal_size() {
        const auto buffer = p.malloc(8);
        memory().write64(buffer, 0);
        CallRegs registers{};
        registers.x[0] = 1; registers.x[1] = 0x40087468;
        registers.push_variadic(buffer);
        const auto result = static_cast<std::int32_t>(p.call_regs(libsystem_library, "_ioctl", registers));
        const auto rows = static_cast<int>(memory().read32(buffer) & 0xffff), cols = static_cast<int>(memory().read32(buffer) >> 16);
        static_cast<void>(sys("_free", {buffer}));
        if (result != 0 || rows <= 0 || cols <= 0) return std::nullopt;
        return std::pair{rows, cols};
    }
    std::optional<int> environment_number(const char* name) {
        const auto text = p.malloc(32);
        put_string(memory(), text, name);
        const auto value = sys("_getenv", {text});
        static_cast<void>(sys("_free", {text}));
        if (!value) return std::nullopt;
        const auto digits = guest_string(memory(), value, 32);
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        return std::stoi(digits);
    }

    // ---- windows ----
    void mirror(Window& w) {
        // struct _win_st: short _cury, _curx, _maxy, _maxx, _begy, _begx, _flags; attr_t _attrs; chtype _bkgd.
        auto& m = memory();
        const std::array<int, 6> fields{w.cury, w.curx, w.rows - 1, w.cols - 1, w.begy, w.begx};
        for (std::size_t index = 0; index < fields.size(); ++index) {
            m.write8(w.guest + index * 2, static_cast<std::uint8_t>(fields[index]));
            m.write8(w.guest + index * 2 + 1, static_cast<std::uint8_t>(static_cast<std::uint16_t>(fields[index]) >> 8));
        }
        m.write32(w.guest + 16, w.attrs);
        m.write32(w.guest + 20, w.background);
    }
    Window* make_window(int rows, int cols, int begy, int begx, std::shared_ptr<Grid> grid, int origin_y, int origin_x) {
        auto value = std::make_unique<Window>();
        value->guest = p.process().allocate_guest(128);
        memory().fill_bytes(value->guest, 0, 128);
        value->grid = grid ? std::move(grid) : std::make_shared<Grid>(rows, cols);
        value->origin_y = origin_y; value->origin_x = origin_x;
        value->rows = rows; value->cols = cols; value->begy = begy; value->begx = begx;
        value->bottom = rows - 1;
        value->touched.assign(static_cast<std::size_t>(rows), true);
        auto* raw = value.get();
        mirror(*raw);
        windows[raw->guest] = std::move(value);
        return raw;
    }
    Window* window(Address guest) {
        const auto found = windows.find(guest);
        return found == windows.end() ? nullptr : found->second.get();
    }
    Window& require_window(Address guest, std::string_view what) {
        auto* value = window(guest);
        if (!value) throw std::runtime_error("Alchemy curses: " + std::string(what) + " on something that is not a curses window");
        return *value;
    }
    void touch(Window& w, int y) {
        if (y >= 0 && y < w.rows) w.touched[static_cast<std::size_t>(y)] = true;
        if (w.parent && w.sync) touch(*w.parent, y + w.pary);
    }
    void touch_all(Window& w) { for (int y = 0; y < w.rows; ++y) touch(w, y); }
    Cell blank(const Window& w) const { return Cell{static_cast<char32_t>(w.background & a_chartext ? w.background & a_chartext : ' '), w.background & a_attributes}; }
    void scroll_window(Window& w, int lines_up) {
        const int top = w.top, bottom = w.bottom;
        if (lines_up == 0 || top > bottom) return;
        const int height = bottom - top + 1;
        std::vector<std::vector<Cell>> rows(static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < w.cols; ++x) rows[static_cast<std::size_t>(y)].push_back(w.cell(top + y, x));
        for (int y = 0; y < height; ++y) {
            const int source = y + lines_up;
            for (int x = 0; x < w.cols; ++x)
                w.cell(top + y, x) = source >= 0 && source < height ? rows[static_cast<std::size_t>(source)][static_cast<std::size_t>(x)] : blank(w);
            touch(w, top + y);
        }
    }
    void newline(Window& w) {
        w.curx = 0;
        if (w.cury == w.bottom) { if (w.scroll) scroll_window(w, 1); }
        else if (w.cury < w.rows - 1) ++w.cury;
    }
    // waddch's rules: control characters, tabs, wrapping and scrolling. ERR at the bottom right of
    // a window that does not scroll (after the character is written there).
    bool put(Window& w, char32_t ch, std::uint32_t attrs) {
        if (ch == U'\n') { for (int x = w.curx; x < w.cols; ++x) w.cell(w.cury, x) = blank(w); touch(w, w.cury); newline(w); return true; }
        if (ch == U'\r') { w.curx = 0; return true; }
        if (ch == U'\b') { if (w.curx > 0) --w.curx; return true; }
        if (ch == U'\t') {
            const int target = (w.curx / tabsize() + 1) * tabsize();
            while (w.curx < target && w.curx < w.cols) if (!put(w, U' ', attrs)) return false;
            return true;
        }
        if (ch < 0x20 || ch == 0x7f) {
            const auto text = unctrl_text(static_cast<std::uint32_t>(ch));
            for (const auto c : text) if (!put(w, static_cast<char32_t>(c), attrs)) return false;
            return true;
        }
        auto combined = (attrs | w.attrs) & a_attributes;
        if (!(combined & a_color)) combined |= w.background & a_color;
        w.cell(w.cury, w.curx) = Cell{ch, combined};
        touch(w, w.cury);
        if (++w.curx >= w.cols) {
            if (w.cury == w.bottom && !w.scroll) { w.curx = w.cols - 1; return false; }
            if (w.cury == w.rows - 1 && !w.scroll) { w.curx = w.cols - 1; return false; }
            newline(w);
        }
        return true;
    }
    static std::string unctrl_text(std::uint32_t c) {
        c &= 0xff;
        if (c < 0x20) return std::string("^") + static_cast<char>(c + 64);
        if (c == 0x7f) return "^?";
        if (c >= 0x80 && c < 0xa0) return std::string("~") + static_cast<char>(c - 0x80 + 64);
        return std::string(1, static_cast<char>(c));
    }
    void after_change(Window& w) {
        mirror(w);
        if (w.immediate) refresh_window(w);
    }

    // ---- output ----
    // DEC special graphics (the ACS characters) as Unicode.
    static char32_t acs_unicode(std::uint32_t c) {
        switch (c & 0xff) {
        case 'l': return U'\u250C'; case 'm': return U'\u2514'; case 'k': return U'\u2510'; case 'j': return U'\u2518';
        case 't': return U'\u251C'; case 'u': return U'\u2524'; case 'v': return U'\u2534'; case 'w': return U'\u252C';
        case 'q': return U'\u2500'; case 'x': return U'\u2502'; case 'n': return U'\u253C'; case 'o': return U'\u23BA';
        case 's': return U'\u23BD'; case '`': return U'\u25C6'; case 'a': return U'\u2592'; case 'f': return U'\u00B0';
        case 'g': return U'\u00B1'; case '~': return U'\u00B7'; case ',': return U'\u2190'; case '+': return U'\u2192';
        case '.': return U'\u2193'; case '-': return U'\u2191'; case 'h': return U'\u2592'; case 'i': return U'\u240B';
        case '0': return U'\u2588'; case 'p': return U'\u23BB'; case 'r': return U'\u23BC'; case 'y': return U'\u2264';
        case 'z': return U'\u2265'; case '{': return U'\u03C0'; case '|': return U'\u2260'; case '}': return U'\u00A3';
        default: return static_cast<char32_t>(c & 0xff);
        }
    }
    static void utf8(std::string& text, char32_t c) {
        if (c < 0x80) text.push_back(static_cast<char>(c));
        else if (c < 0x800) { text.push_back(static_cast<char>(0xc0 | (c >> 6))); text.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
        else if (c < 0x10000) { text.push_back(static_cast<char>(0xe0 | (c >> 12))); text.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f))); text.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
        else { text.push_back(static_cast<char>(0xf0 | (c >> 18))); text.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f))); text.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f))); text.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
    }
    std::string sgr(std::uint32_t attrs) {
        std::string code = "\x1b[0";
        if (attrs & a_bold) code += ";1";
        if (attrs & a_dim) code += ";2";
        if (attrs & a_underline) code += ";4";
        if (attrs & a_blink) code += ";5";
        if (attrs & (a_reverse | a_standout)) code += ";7";
        if (attrs & a_invis) code += ";8";
        const auto pair = (attrs & a_color) >> 8;
        if (pair && colors_started) {
            const auto [fg, bg] = pairs[pair];
            if (fg >= 0) code += fg < 8 ? ";" + std::to_string(30 + fg) : ";38;5;" + std::to_string(fg);
            if (bg >= 0) code += bg < 8 ? ";" + std::to_string(40 + bg) : ";48;5;" + std::to_string(bg);
        }
        return code + "m";
    }
    // Writes the differences between the virtual and the physical screen.
    void update() {
        if (!virtual_screen) return;
        std::uint32_t current = 0xffffffff;
        int at_y = -1, at_x = -1;
        const bool redraw = std::exchange(cursor_dirty, false);
        if (redraw) { out += "\x1b[0m\x1b[H\x1b[2J"; physical = std::make_unique<Grid>(lines, columns); for (auto& c : physical->cells) c = Cell{U' ', 0}; current = 0; at_y = at_x = 0; }
        for (int y = 0; y < lines; ++y)
            for (int x = 0; x < columns; ++x) {
                const auto& want = virtual_screen->at(y, x);
                auto& have = physical->at(y, x);
                if (want == have && !(redraw && want.ch != U' ')) continue;
                if (y != at_y || x != at_x) out += "\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
                if (want.attrs != current) { out += sgr(want.attrs); current = want.attrs; }
                utf8(out, want.attrs & a_altcharset ? acs_unicode(static_cast<std::uint32_t>(want.ch)) : want.ch);
                have = want;
                at_y = y; at_x = x + 1;
            }
        if (current != 0) out += "\x1b[0m";
        out += "\x1b[" + std::to_string(vcury + 1) + ";" + std::to_string(vcurx + 1) + "H";
        flush();
    }
    // wnoutrefresh: the window's touched lines onto the virtual screen.
    void stage(Window& w, int pminrow = 0, int pmincol = 0, int sminrow = -1, int smincol = -1, int smaxrow = -1, int smaxcol = -1) {
        if (!virtual_screen) return;
        if (w.clear) { cursor_dirty = true; w.clear = false; }
        const int top = sminrow >= 0 ? sminrow : w.begy, left = smincol >= 0 ? smincol : w.begx;
        const int height = smaxrow >= 0 ? smaxrow - sminrow + 1 : w.rows, width = smaxcol >= 0 ? smaxcol - smincol + 1 : w.cols;
        for (int y = 0; y < height; ++y) {
            const int source = pminrow + y;
            if (source < 0 || source >= w.rows) continue;
            for (int x = 0; x < width; ++x) {
                const int sx = pmincol + x, ty = top + y, tx = left + x;
                if (sx < 0 || sx >= w.cols || ty < 0 || ty >= lines || tx < 0 || tx >= columns) continue;
                virtual_screen->at(ty, tx) = w.cell(source, sx);
            }
            w.touched[static_cast<std::size_t>(source)] = false;
        }
        if (!w.leave) { vcury = top + w.cury - pminrow; vcurx = left + w.curx - pmincol; }
    }
    void refresh_window(Window& w) { stage(w); update(); }

    // ---- input ----
    // One byte from fd 0 within `timeout` ms (-1: wait); nullopt when none came.
    std::optional<std::uint8_t> read_byte(int timeout) {
        if (timeout >= 0) {
            const auto poll = p.malloc(8);
            memory().write32(poll, 0); memory().write32(poll + 4, 1);   // {fd 0, events POLLIN}
            const auto ready = static_cast<std::int32_t>(sys("_poll", {poll, 1, static_cast<std::uint64_t>(timeout)}));
            static_cast<void>(sys("_free", {poll}));
            if (ready <= 0) return std::nullopt;
        }
        const auto cell = p.malloc(1);
        const auto count = static_cast<std::int64_t>(sys("_read", {0, cell, 1}));
        const auto byte = memory().read8(cell);
        static_cast<void>(sys("_free", {cell}));
        if (count != 1) return std::nullopt;
        return byte;
    }
    // A decoded key (keypad on): an escape sequence, or the bytes read so far.
    std::vector<std::int32_t> decode(std::uint8_t first, bool keypad, bool no_timeout) {
        if (first != 0x1b || !keypad) {
            if (keypad && first == 0x7f) return {key_backspace};
            return {first};
        }
        std::string sequence;
        const int wait = no_timeout ? -1 : escdelay();
        const auto next = read_byte(wait);
        if (!next) return {0x1b};
        sequence.push_back(static_cast<char>(*next));
        if (*next != '[' && *next != 'O') return {0x1b, *next};
        // CSI / SS3: parameters, then a final byte (0x40-0x7e); SGR mouse reports start "[<".
        for (int count = 0; count < 32; ++count) {
            const auto byte = read_byte(wait);
            if (!byte) break;
            sequence.push_back(static_cast<char>(*byte));
            if (*byte >= 0x40 && *byte <= 0x7e && !(sequence.size() == 2 && *byte == '<')) break;
        }
        if (sequence.starts_with("[<")) {
            // SGR mouse: "[<b;x;yM" (press) or "...m" (release).
            int values[3]{}; int index = 0;
            for (std::size_t at = 2; at < sequence.size() - 1 && index < 3; ++at) {
                if (sequence[at] == ';') ++index;
                else if (sequence[at] >= '0' && sequence[at] <= '9') values[index] = values[index] * 10 + (sequence[at] - '0');
            }
            const bool release = sequence.back() == 'm';
            const int button = values[0] & 3;
            if (values[0] & 64) return {};   // wheel: not reported
            std::uint64_t state = release ? button_released : button_pressed;
            state <<= button * 6;
            if (values[0] & 4) state |= button_shift;
            if (values[0] & 8) state |= button_alt;
            if (values[0] & 16) state |= button_ctrl;
            if ((values[0] & 32) && !release) state = report_mouse_position;
            if (!(state & mouse_mask)) return {};
            mouse_events.push_back(MouseEvent{0, values[1] - 1, values[2] - 1, 0, state});
            return {key_mouse};
        }
        static const std::map<std::string, std::int32_t> keys{
            {"[A", key_up}, {"[B", key_down}, {"[C", key_right}, {"[D", key_left}, {"[H", key_home}, {"[F", key_end},
            {"OA", key_up}, {"OB", key_down}, {"OC", key_right}, {"OD", key_left}, {"OH", key_home}, {"OF", key_end}, {"OM", key_enter},
            {"[1~", key_home}, {"[2~", key_ic}, {"[3~", key_dc}, {"[4~", key_end}, {"[5~", key_ppage}, {"[6~", key_npage}, {"[Z", key_btab},
            {"OP", key_f0 + 1}, {"OQ", key_f0 + 2}, {"OR", key_f0 + 3}, {"OS", key_f0 + 4}, {"[15~", key_f0 + 5}, {"[17~", key_f0 + 6},
            {"[18~", key_f0 + 7}, {"[19~", key_f0 + 8}, {"[20~", key_f0 + 9}, {"[21~", key_f0 + 10}, {"[23~", key_f0 + 11}, {"[24~", key_f0 + 12}};
        if (const auto found = keys.find(sequence); found != keys.end()) return {found->second};
        std::vector<std::int32_t> raw{0x1b};
        for (const auto c : sequence) raw.push_back(static_cast<std::uint8_t>(c));
        return raw;
    }
    // wgetch: ERR when no key came within the window's delay.
    std::int32_t get_key(Window& w) {
        if (!pushback.empty()) { const auto key = pushback.front(); pushback.pop_front(); return key; }
        if (const auto size = terminal_size(); size && (size->first != lines || size->second != columns)) {
            resize(size->first, size->second);
            return key_resize;
        }
        refresh_window(w);
        int timeout = w.nodelay ? 0 : w.delay;
        if (halfdelay_tenths && timeout < 0) timeout = halfdelay_tenths * 100;
        const auto byte = read_byte(timeout);
        if (!byte) return static_cast<std::int32_t>(err);
        auto keys = decode(*byte, w.keypad, w.notimeout);
        if (keys.empty()) return get_key(w);
        for (std::size_t index = 1; index < keys.size(); ++index) pushback.push_back(keys[index]);
        const auto key = keys.front();
        if (echo_mode && key < 0x100 && key >= 0x20 && key != 0x7f) { static_cast<void>(put(w, static_cast<char32_t>(key), 0)); refresh_window(w); }
        if (!nl_mode && key == '\r') return '\r';
        return key == '\r' && nl_mode ? '\n' : key;
    }
    void resize(int rows, int cols) {
        lines = rows; columns = cols;
        memory().write32(lines_cell, static_cast<std::uint32_t>(rows));
        memory().write32(cols_cell, static_cast<std::uint32_t>(cols));
        virtual_screen = std::make_unique<Grid>(rows, cols);
        physical = std::make_unique<Grid>(rows, cols);
        cursor_dirty = true;
        if (stdscr) resize_window(*stdscr, rows, cols);
    }
    void resize_window(Window& w, int rows, int cols) {
        auto grid = std::make_shared<Grid>(rows, cols);
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < cols; ++x) grid->at(y, x) = y < w.rows && x < w.cols ? w.cell(y, x) : blank(w);
        w.grid = std::move(grid); w.origin_y = w.origin_x = 0;
        w.rows = rows; w.cols = cols; w.top = 0; w.bottom = rows - 1;
        w.cury = std::min(w.cury, rows - 1); w.curx = std::min(w.curx, cols - 1);
        w.touched.assign(static_cast<std::size_t>(rows), true);
        mirror(w);
    }
    // initscr: the screens, stdscr, the program mode and the alternate screen.
    Address start() {
        if (active) return stdscr->guest;
        std::array<std::uint8_t, 72> mode{};
        if (!get_mode(mode)) throw std::runtime_error("Alchemy curses: initscr needs a terminal whose attributes can be read on fd 0 (tcgetattr failed)");
        shell_mode = mode; shell_saved = true;
        program_mode = mode; program_saved = true;
        int rows = 24, cols = 80;
        if (const auto size = terminal_size()) { rows = size->first; cols = size->second; }
        if (const auto value = environment_number("LINES")) rows = *value;
        if (const auto value = environment_number("COLUMNS")) cols = *value;
        lines = rows; columns = cols;
        memory().write32(lines_cell, static_cast<std::uint32_t>(rows));
        memory().write32(cols_cell, static_cast<std::uint32_t>(cols));
        virtual_screen = std::make_unique<Grid>(rows, cols);
        physical = std::make_unique<Grid>(rows, cols);
        stdscr = make_window(rows, cols, 0, 0, nullptr, 0, 0);
        memory().write64(stdscr_cell, stdscr->guest);
        if (!curscr_window) {
            curscr_window = make_window(rows, cols, 0, 0, nullptr, 0, 0)->guest;
            newscr_window = make_window(rows, cols, 0, 0, nullptr, 0, 0)->guest;
        }
        memory().write64(curscr_cell, curscr_window);
        memory().write64(newscr_cell, newscr_window);
        active = true; ended = false; cursor_dirty = true;
        out += "\x1b[?1049h";
        apply_mode();
        update();
        return stdscr->guest;
    }
    void stop() {
        if (!active || ended) return;
        out += "\x1b[0m\x1b[" + std::to_string(lines) + ";1H";
        if (mouse_mask) out += "\x1b[?1006l\x1b[?1000l\x1b[?1002l";
        if (cursor_visibility != 1) out += "\x1b[?25h";
        out += "\x1b[?1049l";
        flush();
        if (shell_saved) static_cast<void>(set_mode(shell_mode));
        ended = true;
    }
};

} // namespace

void install_ncurses(Provider& p) {
    auto curses = std::make_shared<Curses>(p);
    auto& c = *curses;
    const auto cell32 = [&p](const char* name, std::uint32_t value) {
        std::array<std::uint8_t, 4> bytes{};
        for (std::size_t index = 0; index < 4; ++index) bytes[index] = static_cast<std::uint8_t>(value >> (8 * index));
        return p.data(ncurses, name, 4, bytes);
    };
    c.stdscr_cell = p.data(ncurses, "_stdscr", 8);
    c.curscr_cell = p.data(ncurses, "_curscr", 8);
    c.newscr_cell = p.data(ncurses, "_newscr", 8);
    c.lines_cell = cell32("_LINES", 0);
    c.cols_cell = cell32("_COLS", 0);
    c.colors_cell = cell32("_COLORS", 0);
    c.pairs_cell = cell32("_COLOR_PAIRS", 0);
    c.escdelay_cell = cell32("_ESCDELAY", 1000);
    c.tabsize_cell = cell32("_TABSIZE", 8);
    // chtype acs_map[128]: the VT100 line-drawing characters, A_ALTCHARSET (doupdate writes them as Unicode).
    {
        std::array<std::uint8_t, 128 * 4> bytes{};
        for (std::uint32_t index = 0; index < 128; ++index) {
            const std::uint32_t value = std::string_view("lmkjtuvwqxnos`afg~,+.-hi0pryz{|}").find(static_cast<char>(index)) != std::string_view::npos
                                            ? (index | a_altcharset) : 0;
            for (std::size_t b = 0; b < 4; ++b) bytes[index * 4 + b] = static_cast<std::uint8_t>(value >> (8 * b));
        }
        c.acs_cell = p.data(ncurses, "_acs_map", bytes.size(), bytes);
    }
    // Handlers lock the curses state; `win(x)` is the window at x.
    using Handler = std::function<std::uint64_t(Curses&, CpuState&, GuestMemory&)>;
    const auto add = [&p, curses](const char* name, Handler handler, std::uint64_t failure = err) {
        p.function(ncurses, name, [curses, handler](CpuState& s, GuestMemory& m) -> std::uint64_t {
            std::lock_guard lock(curses->mutex);
            return handler(*curses, s, m);
        }, failure);
    };
    const auto i32 = [](std::uint64_t value) { return static_cast<int>(static_cast<std::int32_t>(value)); };

    // ---- screens ----
    add("_initscr", [](Curses& k, CpuState&, GuestMemory&) { return k.start(); }, 0);
    add("_endwin", [](Curses& k, CpuState&, GuestMemory&) { k.stop(); return ok; });
    add("_isendwin", [](Curses& k, CpuState&, GuestMemory&) -> std::uint64_t { return k.active && k.ended ? 1 : 0; }, 0);
    add("_doupdate", [](Curses& k, CpuState&, GuestMemory&) {
        if (k.ended && k.active) { k.ended = false; k.out += "\x1b[?1049h"; k.cursor_dirty = true; k.apply_mode(); }
        k.update();
        return ok;
    });
    add("_wrefresh", [](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "wrefresh");
        if (s.x[0] == k.curscr_window) { k.cursor_dirty = true; k.update(); return ok; }
        if (k.ended) { k.ended = false; k.out += "\x1b[?1049h"; k.cursor_dirty = true; k.apply_mode(); }
        k.refresh_window(w);
        return ok;
    });
    add("_wnoutrefresh", [](Curses& k, CpuState& s, GuestMemory&) { k.stage(k.require_window(s.x[0], "wnoutrefresh")); return ok; });
    add("_redrawwin", [](Curses& k, CpuState& s, GuestMemory&) { auto& w = k.require_window(s.x[0], "redrawwin"); k.touch_all(w); k.cursor_dirty = true; return ok; });
    add("_wredrawln", [i32](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "wredrawln");
        for (int y = i32(s.x[1]); y < i32(s.x[1]) + i32(s.x[2]); ++y) k.touch(w, y);
        k.cursor_dirty = true;
        return ok;
    });
    // resizeterm / resize_term(lines, columns); is_term_resized(lines, columns)
    for (const auto* name : {"_resizeterm", "_resize_term"})
        add(name, [i32](Curses& k, CpuState& s, GuestMemory&) {
            if (i32(s.x[0]) <= 0 || i32(s.x[1]) <= 0) return err;
            k.resize(i32(s.x[0]), i32(s.x[1]));
            return ok;
        });
    add("_is_term_resized", [i32](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t { return i32(s.x[0]) != k.lines || i32(s.x[1]) != k.columns ? 1 : 0; }, 0);
    add("_use_env", [](Curses&, CpuState&, GuestMemory&) { return ok; });
    add("_filter", [](Curses&, CpuState&, GuestMemory&) { return ok; });

    // ---- windows ----
    add("_newwin", [i32](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t {
        int rows = i32(s.x[0]), cols = i32(s.x[1]);
        const int y = i32(s.x[2]), x = i32(s.x[3]);
        if (rows == 0) rows = k.lines - y;
        if (cols == 0) cols = k.columns - x;
        if (rows <= 0 || cols <= 0 || y < 0 || x < 0) return 0;
        return k.make_window(rows, cols, y, x, nullptr, 0, 0)->guest;
    }, 0);
    add("_newpad", [i32](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t {
        if (i32(s.x[0]) <= 0 || i32(s.x[1]) <= 0) return 0;
        auto* w = k.make_window(i32(s.x[0]), i32(s.x[1]), 0, 0, nullptr, 0, 0);
        w->pad = true;
        return w->guest;
    }, 0);
    // subwin(parent, lines, cols, begin_y, begin_x) (screen coordinates); derwin (parent's); subpad (pad's)
    for (const auto* name : {"_subwin", "_derwin", "_subpad"})
        add(name, [i32, relative = std::string_view(name) != "_subwin"](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t {
            auto& parent = k.require_window(s.x[0], "subwin/derwin/subpad");
            int rows = i32(s.x[1]), cols = i32(s.x[2]);
            const int py = relative ? i32(s.x[3]) : i32(s.x[3]) - parent.begy, px = relative ? i32(s.x[4]) : i32(s.x[4]) - parent.begx;
            if (rows == 0) rows = parent.rows - py;
            if (cols == 0) cols = parent.cols - px;
            if (py < 0 || px < 0 || rows <= 0 || cols <= 0 || py + rows > parent.rows || px + cols > parent.cols) return 0;
            auto* w = k.make_window(rows, cols, parent.begy + py, parent.begx + px, parent.grid, parent.origin_y + py, parent.origin_x + px);
            w->parent = &parent; w->pary = py; w->parx = px; w->pad = parent.pad;
            w->attrs = parent.attrs; w->background = parent.background;
            k.mirror(*w);
            return w->guest;
        }, 0);
    add("_delwin", [](Curses& k, CpuState& s, GuestMemory&) {
        auto* w = k.window(s.x[0]);
        if (!w || w == k.stdscr) return err;
        for (const auto& [address, other] : k.windows) if (other->parent == w) return err;   // a window with subwindows
        k.p.process().release_guest(w->guest);
        k.windows.erase(s.x[0]);
        return ok;
    });
    add("_mvwin", [i32](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "mvwin");
        const int y = i32(s.x[1]), x = i32(s.x[2]);
        if (w.pad || y < 0 || x < 0 || y + w.rows > k.lines || x + w.cols > k.columns) return err;
        w.begy = y; w.begx = x;
        k.touch_all(w); k.mirror(w);
        return ok;
    });
    add("_mvderwin", [i32](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "mvderwin");
        if (!w.parent) return err;
        const int y = i32(s.x[1]), x = i32(s.x[2]);
        if (y < 0 || x < 0 || y + w.rows > w.parent->rows || x + w.cols > w.parent->cols) return err;
        w.pary = y; w.parx = x; w.origin_y = w.parent->origin_y + y; w.origin_x = w.parent->origin_x + x;
        k.touch_all(w); k.mirror(w);
        return ok;
    });
    add("_wresize", [i32](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "wresize");
        if (i32(s.x[1]) <= 0 || i32(s.x[2]) <= 0 || w.parent) return err;
        k.resize_window(w, i32(s.x[1]), i32(s.x[2]));
        return ok;
    });
    // prefresh / pnoutrefresh(pad, pminrow, pmincol, sminrow, smincol, smaxrow, smaxcol)
    for (const auto* name : {"_prefresh", "_pnoutrefresh"})
        add(name, [i32, now = std::string_view(name) == "_prefresh"](Curses& k, CpuState& s, GuestMemory&) {
            auto& w = k.require_window(s.x[0], "prefresh");
            if (!w.pad) return err;
            const int sminrow = std::max(0, i32(s.x[3])), smincol = std::max(0, i32(s.x[4]));
            const int smaxrow = std::min(k.lines - 1, i32(s.x[5])), smaxcol = std::min(k.columns - 1, i32(s.x[6]));
            if (smaxrow < sminrow || smaxcol < smincol) return err;
            k.stage(w, std::max(0, i32(s.x[1])), std::max(0, i32(s.x[2])), sminrow, smincol, smaxrow, smaxcol);
            if (now) k.update();
            return ok;
        });
    add("_is_pad", [](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t { auto* w = k.window(s.x[0]); return w && w->pad ? 1 : 0; }, 0);
    // copywin(src, dst, sminrow, smincol, dminrow, dmincol, dmaxrow, dmaxcol, overlay)
    add("_copywin", [i32](Curses& k, CpuState& s, GuestMemory& m) {
        auto& from = k.require_window(s.x[0], "copywin");
        auto& to = k.require_window(s.x[1], "copywin");
        const int sminrow = i32(s.x[2]), smincol = i32(s.x[3]), dminrow = i32(s.x[4]), dmincol = i32(s.x[5]), dmaxrow = i32(s.x[6]), dmaxcol = i32(s.x[7]);
        const bool overlay = (m.read32(s.sp) & 0xff) != 0;
        for (int y = dminrow; y <= dmaxrow; ++y)
            for (int x = dmincol; x <= dmaxcol; ++x) {
                const int fy = sminrow + y - dminrow, fx = smincol + x - dmincol;
                if (fy < 0 || fx < 0 || fy >= from.rows || fx >= from.cols || y < 0 || x < 0 || y >= to.rows || x >= to.cols) return err;
                const auto source = from.cell(fy, fx);
                if (overlay && source.ch == U' ') continue;
                to.cell(y, x) = source;
            }
        for (int y = std::max(0, dminrow); y <= std::min(to.rows - 1, dmaxrow); ++y) k.touch(to, y);
        return ok;
    });
    // overlay / overwrite(src, dst): where the windows overlap on the screen.
    for (const auto* name : {"_overlay", "_overwrite"})
        add(name, [skip_blanks = std::string_view(name) == "_overlay"](Curses& k, CpuState& s, GuestMemory&) {
            auto& from = k.require_window(s.x[0], "overlay");
            auto& to = k.require_window(s.x[1], "overlay");
            const int top = std::max(from.begy, to.begy), left = std::max(from.begx, to.begx);
            const int bottom = std::min(from.begy + from.rows, to.begy + to.rows), right = std::min(from.begx + from.cols, to.begx + to.cols);
            for (int y = top; y < bottom; ++y) {
                for (int x = left; x < right; ++x) {
                    const auto source = from.cell(y - from.begy, x - from.begx);
                    if (skip_blanks && source.ch == U' ') continue;
                    to.cell(y - to.begy, x - to.begx) = source;
                }
                k.touch(to, y - to.begy);
            }
            return ok;
        });

    // ---- window state ----
    add("_wmove", [i32](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "wmove");
        const int y = i32(s.x[1]), x = i32(s.x[2]);
        if (y < 0 || x < 0 || y >= w.rows || x >= w.cols) return err;
        w.cury = y; w.curx = x;
        k.mirror(w);
        return ok;
    });
    const auto field = [&add](const char* name, auto get) {
        add(name, [get](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t {
            auto* w = k.window(s.x[0]);
            return w ? static_cast<std::uint64_t>(static_cast<std::int64_t>(get(*w))) : err;
        });
    };
    field("_getcury", [](Window& w) { return w.cury; });
    field("_getcurx", [](Window& w) { return w.curx; });
    field("_getmaxy", [](Window& w) { return w.rows; });
    field("_getmaxx", [](Window& w) { return w.cols; });
    field("_getbegy", [](Window& w) { return w.begy; });
    field("_getbegx", [](Window& w) { return w.begx; });
    field("_getpary", [](Window& w) { return w.pary; });
    field("_getparx", [](Window& w) { return w.parx; });
    field("_getattrs", [](Window& w) { return static_cast<int>(w.attrs); });
    add("_getbkgd", [](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t { auto* w = k.window(s.x[0]); return w ? w->background : 0; }, 0);
    const auto option = [&add](const char* name, bool Window::* member) {
        add(name, [member](Curses& k, CpuState& s, GuestMemory&) {
            k.require_window(s.x[0], "a window option").*member = (s.x[1] & 0xff) != 0;
            return ok;
        });
    };
    option("_scrollok", &Window::scroll);
    option("_leaveok", &Window::leave);
    option("_clearok", &Window::clear);
    option("_idlok", &Window::idl);
    option("_keypad", &Window::keypad);
    option("_nodelay", &Window::nodelay);
    option("_notimeout", &Window::notimeout);
    option("_syncok", &Window::sync);
    add("_idcok", [](Curses& k, CpuState& s, GuestMemory&) { k.require_window(s.x[0], "idcok").idc = (s.x[1] & 0xff) != 0; return ok; });
    add("_immedok", [](Curses& k, CpuState& s, GuestMemory&) { k.require_window(s.x[0], "immedok").immediate = (s.x[1] & 0xff) != 0; return ok; });
    add("_is_leaveok", [](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t { return k.require_window(s.x[0], "is_leaveok").leave ? 1 : 0; }, 0);
    add("_wtimeout", [i32](Curses& k, CpuState& s, GuestMemory&) { k.require_window(s.x[0], "wtimeout").delay = std::max(-1, i32(s.x[1])); return ok; });
    add("_wsetscrreg", [i32](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "wsetscrreg");
        const int top = i32(s.x[1]), bottom = i32(s.x[2]);
        if (top < 0 || bottom >= w.rows || top > bottom) return err;
        w.top = top; w.bottom = bottom;
        return ok;
    });
    add("_wscrl", [i32](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "wscrl");
        if (!w.scroll) return err;
        k.scroll_window(w, i32(s.x[1]));
        k.after_change(w);
        return ok;
    });
    // Synchronization with parents (subwindows share cells, so only the touched flags move).
    add("_wsyncup", [](Curses& k, CpuState& s, GuestMemory&) {
        for (auto* w = &k.require_window(s.x[0], "wsyncup"); w->parent; w = w->parent)
            for (int y = 0; y < w->rows; ++y) if (w->touched[static_cast<std::size_t>(y)]) k.touch(*w->parent, y + w->pary);
        return ok;
    });
    add("_wsyncdown", [](Curses& k, CpuState& s, GuestMemory&) {
        // The window shares its ancestors' cells, so it is up to date; it is touched for its next refresh.
        k.touch_all(k.require_window(s.x[0], "wsyncdown"));
        return ok;
    });
    add("_wcursyncup", [](Curses& k, CpuState& s, GuestMemory&) {
        for (auto* w = &k.require_window(s.x[0], "wcursyncup"); w->parent; w = w->parent) {
            w->parent->cury = w->cury + w->pary; w->parent->curx = w->curx + w->parx;
            k.mirror(*w->parent);
        }
        return ok;
    });
    // wtouchln(win, y, n, changed); is_linetouched(win, line); is_wintouched(win)
    add("_wtouchln", [i32](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "wtouchln");
        for (int y = i32(s.x[1]); y < i32(s.x[1]) + i32(s.x[2]) && y < w.rows; ++y)
            if (y >= 0) w.touched[static_cast<std::size_t>(y)] = (s.x[3] & 0xff) != 0;
        return ok;
    });
    add("_is_linetouched", [i32](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t {
        auto& w = k.require_window(s.x[0], "is_linetouched");
        const int y = i32(s.x[1]);
        if (y < 0 || y >= w.rows) return err;
        return w.touched[static_cast<std::size_t>(y)] ? 1 : 0;
    });
    add("_is_wintouched", [](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t {
        auto& w = k.require_window(s.x[0], "is_wintouched");
        return std::any_of(w.touched.begin(), w.touched.end(), [](bool t) { return t; }) ? 1 : 0;
    }, 0);

    // ---- attributes ----
    add("_wattr_on", [](Curses& k, CpuState& s, GuestMemory&) { auto& w = k.require_window(s.x[0], "wattr_on"); w.attrs |= static_cast<std::uint32_t>(s.x[1]); k.mirror(w); return ok; });
    add("_wattr_off", [](Curses& k, CpuState& s, GuestMemory&) { auto& w = k.require_window(s.x[0], "wattr_off"); w.attrs &= ~static_cast<std::uint32_t>(s.x[1]); k.mirror(w); return ok; });
    add("_wattrset", [](Curses& k, CpuState& s, GuestMemory&) { auto& w = k.require_window(s.x[0], "wattrset"); w.attrs = static_cast<std::uint32_t>(s.x[1]); k.mirror(w); return ok; });
    add("_wbkgdset", [](Curses& k, CpuState& s, GuestMemory&) { auto& w = k.require_window(s.x[0], "wbkgdset"); w.background = static_cast<std::uint32_t>(s.x[1]); k.mirror(w); return ok; });
    // wbkgd: the new background replaces the old one on every cell.
    add("_wbkgd", [](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "wbkgd");
        const auto previous = w.background;
        w.background = static_cast<std::uint32_t>(s.x[1]);
        for (int y = 0; y < w.rows; ++y) {
            for (int x = 0; x < w.cols; ++x) {
                auto& cell = w.cell(y, x);
                if (cell.ch == static_cast<char32_t>(previous & a_chartext ? previous & a_chartext : ' ')) cell.ch = k.blank(w).ch;
                cell.attrs = (cell.attrs & ~(previous & a_attributes)) | (w.background & a_attributes);
            }
            k.touch(w, y);
        }
        k.after_change(w);
        return ok;
    });
    // wchgat(win, n, attr, pair, opts): n characters from the cursor (-1: to the end of the line).
    add("_wchgat", [i32](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "wchgat");
        const int count = i32(s.x[1]) < 0 ? w.cols - w.curx : std::min(i32(s.x[1]), w.cols - w.curx);
        const auto attrs = (static_cast<std::uint32_t>(s.x[2]) & ~a_color) | ((static_cast<std::uint32_t>(s.x[3]) & 0xff) << 8);
        for (int x = w.curx; x < w.curx + count; ++x) w.cell(w.cury, x).attrs = attrs;
        k.touch(w, w.cury);
        k.after_change(w);
        return ok;
    });
    add("_termattrs", [](Curses&, CpuState&, GuestMemory&) -> std::uint64_t {
        return a_standout | a_underline | a_reverse | a_blink | a_dim | a_bold | a_altcharset | a_invis | a_color;
    }, 0);

    // ---- output ----
    add("_waddch", [](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "waddch");
        const auto ch = static_cast<std::uint32_t>(s.x[1]);
        const bool done = k.put(w, static_cast<char32_t>(ch & a_chartext), ch & a_attributes);
        k.after_change(w);
        return done ? ok : err;
    });
    for (const auto* name : {"_wechochar", "_pechochar"})
        add(name, [pad = std::string_view(name) == "_pechochar"](Curses& k, CpuState& s, GuestMemory&) {
            auto& w = k.require_window(s.x[0], "wechochar");
            const auto ch = static_cast<std::uint32_t>(s.x[1]);
            const bool done = k.put(w, static_cast<char32_t>(ch & a_chartext), ch & a_attributes);
            k.mirror(w);
            if (!pad) k.refresh_window(w);
            return done ? ok : err;
        });
    // waddnstr(win, str, n) (n -1: all) / waddnwstr(win, wchar_t*, n)
    add("_waddnstr", [i32](Curses& k, CpuState& s, GuestMemory& m) {
        auto& w = k.require_window(s.x[0], "waddnstr");
        const auto limit = i32(s.x[2]) < 0 ? 1 << 20 : i32(s.x[2]);
        const auto text = guest_string(m, s.x[1], static_cast<std::size_t>(limit));
        // UTF-8 text: decode to characters.
        bool done = true;
        for (std::size_t at = 0; at < text.size() && done;) {
            auto c = static_cast<std::uint8_t>(text[at]);
            char32_t value = c; std::size_t length = 1;
            if (c >= 0xc0 && c < 0xe0 && at + 1 < text.size()) { value = ((c & 0x1fu) << 6) | (text[at + 1] & 0x3fu); length = 2; }
            else if (c >= 0xe0 && c < 0xf0 && at + 2 < text.size()) { value = ((c & 0x0fu) << 12) | ((text[at + 1] & 0x3fu) << 6) | (text[at + 2] & 0x3fu); length = 3; }
            else if (c >= 0xf0 && at + 3 < text.size()) { value = ((c & 0x07u) << 18) | ((text[at + 1] & 0x3fu) << 12) | ((text[at + 2] & 0x3fu) << 6) | (text[at + 3] & 0x3fu); length = 4; }
            done = k.put(w, value, 0);
            at += length;
        }
        k.after_change(w);
        return done ? ok : err;
    });
    add("_waddnwstr", [i32](Curses& k, CpuState& s, GuestMemory& m) {
        auto& w = k.require_window(s.x[0], "waddnwstr");
        const int limit = i32(s.x[2]) < 0 ? 1 << 20 : i32(s.x[2]);
        bool done = true;
        for (int index = 0; index < limit && done; ++index) {
            const auto c = m.read32(s.x[1] + static_cast<std::uint64_t>(index) * 4);
            if (!c) break;
            done = k.put(w, static_cast<char32_t>(c), 0);
        }
        k.after_change(w);
        return done ? ok : err;
    });
    // cchar_t {attr_t attr; wchar_t chars[5]}: setcchar(cchar_t*, const wchar_t*, attr_t, short pair, void*)
    add("_setcchar", [](Curses&, CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return err;
        m.fill_bytes(s.x[0], 0, 24);
        m.write32(s.x[0], (static_cast<std::uint32_t>(s.x[2]) & ~a_color) | ((static_cast<std::uint32_t>(s.x[3]) & 0xff) << 8));
        for (std::uint64_t index = 0; index < 5 && s.x[1]; ++index) {
            const auto c = m.read32(s.x[1] + index * 4);
            if (!c) break;
            m.write32(s.x[0] + 4 + index * 4, c);
        }
        return ok;
    });
    add("_wadd_wch", [](Curses& k, CpuState& s, GuestMemory& m) {
        auto& w = k.require_window(s.x[0], "wadd_wch");
        const auto attrs = m.read32(s.x[1]);
        const bool done = k.put(w, static_cast<char32_t>(m.read32(s.x[1] + 4)), attrs);
        k.after_change(w);
        return done ? ok : err;
    });
    // winsch / winsnstr / wins_nwstr: insert before the cursor, shifting the line right.
    const auto insert = [](Curses& k, Window& w, const std::u32string& text, std::uint32_t attrs) {
        for (auto c = text.rbegin(); c != text.rend(); ++c) {
            for (int x = w.cols - 1; x > w.curx; --x) w.cell(w.cury, x) = w.cell(w.cury, x - 1);
            w.cell(w.cury, w.curx) = Cell{*c, (attrs | w.attrs) & a_attributes};
        }
        k.touch(w, w.cury);
        k.after_change(w);
    };
    add("_winsch", [insert](Curses& k, CpuState& s, GuestMemory&) {
        const auto ch = static_cast<std::uint32_t>(s.x[1]);
        insert(k, k.require_window(s.x[0], "winsch"), std::u32string(1, static_cast<char32_t>(ch & a_chartext)), ch & a_attributes);
        return ok;
    });
    add("_winsnstr", [insert, i32](Curses& k, CpuState& s, GuestMemory& m) {
        const auto text = guest_string(m, s.x[1], i32(s.x[2]) < 0 ? 1 << 16 : static_cast<std::size_t>(i32(s.x[2])));
        insert(k, k.require_window(s.x[0], "winsnstr"), std::u32string(text.begin(), text.end()), 0);
        return ok;
    });
    add("_wins_nwstr", [insert, i32](Curses& k, CpuState& s, GuestMemory& m) {
        std::u32string text;
        for (int index = 0; index < (i32(s.x[2]) < 0 ? 1 << 16 : i32(s.x[2])); ++index) {
            const auto c = m.read32(s.x[1] + static_cast<std::uint64_t>(index) * 4);
            if (!c) break;
            text.push_back(static_cast<char32_t>(c));
        }
        insert(k, k.require_window(s.x[0], "wins_nwstr"), text, 0);
        return ok;
    });
    add("_wdelch", [](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "wdelch");
        for (int x = w.curx; x < w.cols - 1; ++x) w.cell(w.cury, x) = w.cell(w.cury, x + 1);
        w.cell(w.cury, w.cols - 1) = k.blank(w);
        k.touch(w, w.cury);
        k.after_change(w);
        return ok;
    });
    // winsdelln(win, n): n > 0 inserts blank lines above the cursor's line, n < 0 deletes lines.
    add("_winsdelln", [i32](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "winsdelln");
        const auto top = w.top, bottom = w.bottom;
        w.top = w.cury; w.bottom = w.rows - 1;
        k.scroll_window(w, -i32(s.x[1]));
        w.top = top; w.bottom = bottom;
        k.after_change(w);
        return ok;
    });
    add("_wclrtoeol", [](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "wclrtoeol");
        for (int x = w.curx; x < w.cols; ++x) w.cell(w.cury, x) = k.blank(w);
        k.touch(w, w.cury);
        k.after_change(w);
        return ok;
    });
    add("_wclrtobot", [](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "wclrtobot");
        for (int y = w.cury; y < w.rows; ++y) {
            for (int x = y == w.cury ? w.curx : 0; x < w.cols; ++x) w.cell(y, x) = k.blank(w);
            k.touch(w, y);
        }
        k.after_change(w);
        return ok;
    });
    for (const auto* name : {"_werase", "_wclear"})
        add(name, [clear = std::string_view(name) == "_wclear"](Curses& k, CpuState& s, GuestMemory&) {
            auto& w = k.require_window(s.x[0], "werase");
            for (int y = 0; y < w.rows; ++y) { for (int x = 0; x < w.cols; ++x) w.cell(y, x) = k.blank(w); k.touch(w, y); }
            w.cury = w.curx = 0;
            if (clear) w.clear = true;
            k.after_change(w);
            return ok;
        });
    // wborder(win, ls, rs, ts, bs, tl, tr, bl, br): 0 picks the ACS default; the eighth argument is on the stack.
    add("_wborder", [](Curses& k, CpuState& s, GuestMemory& m) {
        auto& w = k.require_window(s.x[0], "wborder");
        const std::array<std::uint32_t, 8> defaults{'x' | a_altcharset, 'x' | a_altcharset, 'q' | a_altcharset, 'q' | a_altcharset,
                                                    'l' | a_altcharset, 'k' | a_altcharset, 'm' | a_altcharset, 'j' | a_altcharset};
        std::array<std::uint32_t, 8> parts{};
        for (std::size_t index = 0; index < 7; ++index) parts[index] = static_cast<std::uint32_t>(s.x[index + 1]);
        parts[7] = m.read32(s.sp);
        for (std::size_t index = 0; index < 8; ++index) if (!(parts[index] & a_chartext)) parts[index] = defaults[index] | (parts[index] & a_attributes);
        const auto set = [&](int y, int x, std::uint32_t ch) { w.cell(y, x) = Cell{static_cast<char32_t>(ch & a_chartext), (ch | w.attrs) & a_attributes}; };
        for (int x = 1; x < w.cols - 1; ++x) { set(0, x, parts[2]); set(w.rows - 1, x, parts[3]); }
        for (int y = 1; y < w.rows - 1; ++y) { set(y, 0, parts[0]); set(y, w.cols - 1, parts[1]); }
        set(0, 0, parts[4]); set(0, w.cols - 1, parts[5]); set(w.rows - 1, 0, parts[6]); set(w.rows - 1, w.cols - 1, parts[7]);
        k.touch_all(w);
        k.after_change(w);
        return ok;
    });
    // whline / wvline(win, ch, n): from the cursor, without moving it.
    for (const auto* name : {"_whline", "_wvline"})
        add(name, [i32, vertical = std::string_view(name) == "_wvline"](Curses& k, CpuState& s, GuestMemory&) {
            auto& w = k.require_window(s.x[0], "whline");
            auto ch = static_cast<std::uint32_t>(s.x[1]);
            if (!(ch & a_chartext)) ch |= (vertical ? 'x' : 'q') | a_altcharset;
            for (int index = 0; index < i32(s.x[2]); ++index) {
                const int y = w.cury + (vertical ? index : 0), x = w.curx + (vertical ? 0 : index);
                if (y >= w.rows || x >= w.cols) break;
                w.cell(y, x) = Cell{static_cast<char32_t>(ch & a_chartext), (ch | w.attrs) & a_attributes};
                k.touch(w, y);
            }
            k.after_change(w);
            return ok;
        });

    // ---- reading back ----
    add("_winch", [](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t {
        auto& w = k.require_window(s.x[0], "winch");
        const auto& cell = w.cell(w.cury, w.curx);
        return (static_cast<std::uint32_t>(cell.ch) & a_chartext) | cell.attrs;
    });
    // winnstr(win, str, n): up to n characters from the cursor; their number.
    add("_winnstr", [i32](Curses& k, CpuState& s, GuestMemory& m) -> std::uint64_t {
        auto& w = k.require_window(s.x[0], "winnstr");
        const int count = i32(s.x[2]) < 0 ? w.cols - w.curx : std::min(i32(s.x[2]), w.cols - w.curx);
        std::string text;
        for (int x = w.curx; x < w.curx + count; ++x) text.push_back(static_cast<char>(static_cast<std::uint32_t>(w.cell(w.cury, x).ch) & 0xff));
        put_string(m, s.x[1], text);
        return text.size();
    });

    // ---- input ----
    add("_wgetch", [](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(k.get_key(k.require_window(s.x[0], "wgetch"))));
    });
    // wget_wch(win, wint_t*): KEY_CODE_YES (0400) for a key code, else OK; UTF-8 input is decoded.
    add("_wget_wch", [](Curses& k, CpuState& s, GuestMemory& m) -> std::uint64_t {
        auto& w = k.require_window(s.x[0], "wget_wch");
        const auto key = k.get_key(w);
        if (key == static_cast<std::int32_t>(err)) return err;
        if (key >= 0x100) { m.write32(s.x[1], static_cast<std::uint32_t>(key)); return 0400; }
        std::uint32_t value = static_cast<std::uint32_t>(key);
        int extra = value >= 0xf0 ? 3 : value >= 0xe0 ? 2 : value >= 0xc0 ? 1 : 0;
        if (extra) value &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : 0x1f;
        for (; extra > 0; --extra) {
            const auto next = k.read_byte(-1);
            if (!next) break;
            value = (value << 6) | (*next & 0x3fu);
        }
        m.write32(s.x[1], value);
        return ok;
    });
    // wgetnstr(win, str, n): a line with the window's echo; erase and kill edit it.
    add("_wgetnstr", [i32](Curses& k, CpuState& s, GuestMemory& m) {
        auto& w = k.require_window(s.x[0], "wgetnstr");
        const int limit = i32(s.x[2]) < 0 ? 1 << 16 : i32(s.x[2]);
        std::string line;
        const bool echoing = k.echo_mode;
        k.echo_mode = false;
        for (;;) {
            const auto key = k.get_key(w);
            if (key == static_cast<std::int32_t>(err)) { k.echo_mode = echoing; return err; }
            if (key == '\n' || key == '\r' || key == key_enter) break;
            if (key == 0x7f || key == '\b' || key == key_backspace) {
                if (!line.empty()) { line.pop_back(); if (echoing && w.curx > 0) { --w.curx; w.cell(w.cury, w.curx) = k.blank(w); k.touch(w, w.cury); } }
            } else if (key < 0x100 && static_cast<int>(line.size()) < limit) {
                line.push_back(static_cast<char>(key));
                if (echoing) static_cast<void>(k.put(w, static_cast<char32_t>(key), 0));
            }
            if (echoing) k.refresh_window(w);
        }
        k.echo_mode = echoing;
        put_string(m, s.x[1], line);
        return ok;
    });
    add("_ungetch", [](Curses& k, CpuState& s, GuestMemory&) { k.pushback.push_front(static_cast<std::int32_t>(s.x[0])); return ok; });
    add("_unget_wch", [](Curses& k, CpuState& s, GuestMemory&) { k.pushback.push_front(static_cast<std::int32_t>(s.x[0])); return ok; });
    // flushinp: discards typed-ahead input.
    add("_flushinp", [](Curses& k, CpuState&, GuestMemory&) {
        k.pushback.clear();
        while (k.read_byte(0)) {}
        return ok;
    });
    add("_has_key", [](Curses&, CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto key = static_cast<std::int32_t>(s.x[0]);
        return key == key_up || key == key_down || key == key_left || key == key_right || key == key_home || key == key_end || key == key_backspace ||
               key == key_dc || key == key_ic || key == key_npage || key == key_ppage || key == key_btab || key == key_enter ||
               (key > key_f0 && key <= key_f0 + 12) ? 1 : 0;
    }, 0);
    add("_typeahead", [](Curses&, CpuState&, GuestMemory&) { return ok; });

    // ---- modes ----
    const auto mode = [&add](const char* name, auto change) {
        add(name, [change](Curses& k, CpuState& s, GuestMemory&) { change(k, s); k.apply_mode(); return ok; });
    };
    mode("_cbreak", [](Curses& k, CpuState&) { k.cbreak_mode = true; k.halfdelay_tenths = 0; });
    mode("_nocbreak", [](Curses& k, CpuState&) { k.cbreak_mode = false; k.halfdelay_tenths = 0; });
    mode("_raw", [](Curses& k, CpuState&) { k.raw_mode = true; });
    mode("_noraw", [](Curses& k, CpuState&) { k.raw_mode = false; });
    mode("_echo", [](Curses& k, CpuState&) { k.echo_mode = true; });
    mode("_noecho", [](Curses& k, CpuState&) { k.echo_mode = false; });
    mode("_nl", [](Curses& k, CpuState&) { k.nl_mode = true; });
    mode("_nonl", [](Curses& k, CpuState&) { k.nl_mode = false; });
    add("_halfdelay", [i32](Curses& k, CpuState& s, GuestMemory&) {
        if (i32(s.x[0]) < 1 || i32(s.x[0]) > 255) return err;
        k.halfdelay_tenths = i32(s.x[0]);
        k.apply_mode();
        return ok;
    });
    for (const auto* name : {"_intrflush", "_meta", "_qiflush", "_noqiflush"})
        add(name, [](Curses&, CpuState&, GuestMemory&) { return ok; });
    // The saved modes (def_prog_mode, def_shell_mode, savetty and their resets).
    add("_def_prog_mode", [](Curses& k, CpuState&, GuestMemory&) { k.program_saved = k.get_mode(k.program_mode); return k.program_saved ? ok : err; });
    add("_def_shell_mode", [](Curses& k, CpuState&, GuestMemory&) { k.shell_saved = k.get_mode(k.shell_mode); return k.shell_saved ? ok : err; });
    add("_reset_prog_mode", [](Curses& k, CpuState&, GuestMemory&) { return k.program_saved && k.set_mode(k.program_mode) ? ok : err; });
    add("_reset_shell_mode", [](Curses& k, CpuState&, GuestMemory&) { return k.shell_saved && k.set_mode(k.shell_mode) ? ok : err; });
    add("_savetty", [](Curses& k, CpuState&, GuestMemory&) { k.tty_saved = k.get_mode(k.saved_tty); return k.tty_saved ? ok : err; });
    add("_resetty", [](Curses& k, CpuState&, GuestMemory&) { return k.tty_saved && k.set_mode(k.saved_tty) ? ok : err; });
    // curs_set(visibility): the previous visibility.
    add("_curs_set", [i32](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t {
        const int visibility = i32(s.x[0]);
        if (visibility < 0 || visibility > 2) return err;
        const auto previous = std::exchange(k.cursor_visibility, visibility);
        k.out += visibility ? "\x1b[?25h" : "\x1b[?25l";
        k.flush();
        return static_cast<std::uint64_t>(previous);
    });
    add("_beep", [](Curses& k, CpuState&, GuestMemory&) { k.out += "\a"; k.flush(); return ok; });
    add("_flash", [](Curses& k, CpuState&, GuestMemory&) { k.out += "\x1b[?5h"; k.flush(); static_cast<void>(k.sys("_usleep", {100000})); k.out += "\x1b[?5l"; k.flush(); return ok; });
    for (const auto* name : {"_napms", "_delay_output"})
        add(name, [i32](Curses& k, CpuState& s, GuestMemory&) { static_cast<void>(k.sys("_usleep", {static_cast<std::uint64_t>(std::max(0, i32(s.x[0]))) * 1000})); return ok; });
    add("_set_escdelay", [](Curses& k, CpuState& s, GuestMemory& m) { m.write32(k.escdelay_cell, static_cast<std::uint32_t>(s.x[0])); return ok; });
    add("_set_tabsize", [](Curses& k, CpuState& s, GuestMemory& m) { m.write32(k.tabsize_cell, static_cast<std::uint32_t>(s.x[0])); return ok; });

    // ---- the terminal's description (built in: xterm-256color) ----
    add("_baudrate", [](Curses&, CpuState&, GuestMemory&) -> std::uint64_t { return 38400; });
    // erasechar / killchar: the terminal's VERASE and VKILL.
    for (const auto& [name, index] : std::initializer_list<std::pair<const char*, std::size_t>>{{"_erasechar", 3}, {"_killchar", 5}})
        add(name, [index = index](Curses& k, CpuState&, GuestMemory&) -> std::uint64_t {
            std::array<std::uint8_t, 72> mode{};
            if (!k.get_mode(mode)) return err;
            return mode[32 + index];
        });
    add("_has_ic", [](Curses&, CpuState&, GuestMemory&) -> std::uint64_t { return 1; }, 0);
    add("_has_il", [](Curses&, CpuState&, GuestMemory&) -> std::uint64_t { return 1; }, 0);
    const auto description = [&p](std::string_view text) {
        const auto address = p.process().allocate_guest(text.size() + 1);
        put_string(p.process().main_engine(), address, text);
        return address;
    };
    const auto long_name = description("xterm with 256 colors"), term_name = description("xterm-256color");
    add("_longname", [long_name](Curses&, CpuState&, GuestMemory&) -> std::uint64_t { return long_name; }, 0);
    add("_termname", [term_name](Curses&, CpuState&, GuestMemory&) -> std::uint64_t { return term_name; }, 0);
    // putp(str): the string to the terminal.
    add("_putp", [](Curses& k, CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return err;
        k.out += guest_string(m, s.x[0], 1 << 16);
        k.flush();
        return ok;
    });
    // char* tparm(const char* str, ...): nine long parameters (variadic, so on the stack); terminfo
    // parameters (%p1-%p9, %d, %c, %s, %i, arithmetic, %{n}, %'c', %%), into a buffer reused by each call.
    const auto tparm_buffer = p.process().allocate_guest(1024);
    add("_tparm", [tparm_buffer](Curses&, CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0]) return 0;
        const auto format = guest_string(m, s.x[0], 1000);
        std::array<std::int64_t, 9> parameters{};
        for (std::size_t index = 0; index < 9; ++index)
            parameters[index] = static_cast<std::int64_t>(m.read64(s.sp + index * 8));
        std::vector<std::int64_t> stack;
        const auto pop = [&stack] { if (stack.empty()) return std::int64_t{0}; const auto value = stack.back(); stack.pop_back(); return value; };
        std::string result;
        for (std::size_t at = 0; at < format.size(); ++at) {
            if (format[at] != '%' || at + 1 >= format.size()) { result.push_back(format[at]); continue; }
            const auto op = format[++at];
            switch (op) {
            case '%': result.push_back('%'); break;
            case 'd': result += std::to_string(pop()); break;
            case 'c': result.push_back(static_cast<char>(pop())); break;
            case 's': result += std::to_string(pop()); break;
            case 'i': ++parameters[0]; ++parameters[1]; break;
            case 'p': if (at + 1 < format.size()) { const int index = format[++at] - '1'; if (index >= 0 && index < 9) stack.push_back(parameters[static_cast<std::size_t>(index)]); } break;
            case '{': { std::int64_t value = 0; while (at + 1 < format.size() && format[at + 1] != '}') value = value * 10 + (format[++at] - '0'); ++at; stack.push_back(value); break; }
            case '\'': if (at + 2 < format.size()) { stack.push_back(format[at + 1]); at += 2; } break;
            case '+': { const auto b = pop(), a = pop(); stack.push_back(a + b); break; }
            case '-': { const auto b = pop(), a = pop(); stack.push_back(a - b); break; }
            case '*': { const auto b = pop(), a = pop(); stack.push_back(a * b); break; }
            case '/': { const auto b = pop(), a = pop(); stack.push_back(b ? a / b : 0); break; }
            case 'm': { const auto b = pop(), a = pop(); stack.push_back(b ? a % b : 0); break; }
            default: result.push_back('%'); result.push_back(op); break;
            }
        }
        if (result.size() > 1023) result.resize(1023);
        put_string(m, tparm_buffer, result);
        return tparm_buffer;
    }, 0);
    // unctrl(chtype) / keyname(int): printable names, in buffers reused by each call.
    const auto name_buffer = p.process().allocate_guest(64);
    add("_unctrl", [name_buffer](Curses&, CpuState& s, GuestMemory& m) -> std::uint64_t {
        put_string(m, name_buffer, Curses::unctrl_text(static_cast<std::uint32_t>(s.x[0])));
        return name_buffer;
    }, 0);
    add("_keyname", [name_buffer](Curses&, CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto key = static_cast<std::int32_t>(s.x[0]);
        static const std::map<std::int32_t, std::string> names{
            {key_down, "KEY_DOWN"}, {key_up, "KEY_UP"}, {key_left, "KEY_LEFT"}, {key_right, "KEY_RIGHT"}, {key_home, "KEY_HOME"},
            {key_backspace, "KEY_BACKSPACE"}, {key_dc, "KEY_DC"}, {key_ic, "KEY_IC"}, {key_npage, "KEY_NPAGE"}, {key_ppage, "KEY_PPAGE"},
            {key_enter, "KEY_ENTER"}, {key_btab, "KEY_BTAB"}, {key_end, "KEY_END"}, {key_mouse, "KEY_MOUSE"}, {key_resize, "KEY_RESIZE"}};
        std::string text;
        if (const auto found = names.find(key); found != names.end()) text = found->second;
        else if (key > key_f0 && key <= key_f0 + 63) text = "KEY_F(" + std::to_string(key - key_f0) + ")";
        else if (key >= 0 && key < 0x80) text = Curses::unctrl_text(static_cast<std::uint32_t>(key));
        else if (key >= 0x80 && key < 0x100) text = "M-" + Curses::unctrl_text(static_cast<std::uint32_t>(key - 0x80));
        else return 0;
        put_string(m, name_buffer, text);
        return name_buffer;
    }, 0);

    // ---- colours ----
    add("_has_colors", [](Curses&, CpuState&, GuestMemory&) -> std::uint64_t { return 1; }, 0);
    add("_can_change_color", [](Curses&, CpuState&, GuestMemory&) -> std::uint64_t { return 0; }, 0);
    add("_start_color", [](Curses& k, CpuState&, GuestMemory& m) {
        k.colors_started = true;
        m.write32(k.colors_cell, 256);
        m.write32(k.pairs_cell, 256);
        k.pairs[0] = {7, 0};
        return ok;
    });
    add("_use_default_colors", [](Curses& k, CpuState&, GuestMemory&) { k.default_colors = true; k.pairs[0] = {-1, -1}; return ok; });
    // init_pair(pair, fg, bg): -1 is the default colour after use_default_colors.
    add("_init_pair", [](Curses& k, CpuState& s, GuestMemory&) {
        const auto pair = static_cast<std::int16_t>(s.x[0]), fg = static_cast<std::int16_t>(s.x[1]), bg = static_cast<std::int16_t>(s.x[2]);
        const int least = k.default_colors ? -1 : 0;
        if (!k.colors_started || pair < 1 || pair > 255 || fg < least || fg > 255 || bg < least || bg > 255) return err;
        k.pairs[static_cast<std::size_t>(pair)] = {fg, bg};
        k.cursor_dirty = true;   // cells already drawn in that pair change colour
        return ok;
    });
    add("_pair_content", [](Curses& k, CpuState& s, GuestMemory& m) {
        const auto pair = static_cast<std::int16_t>(s.x[0]);
        if (pair < 0 || pair > 255) return err;
        if (s.x[1]) { m.write8(s.x[1], static_cast<std::uint8_t>(k.pairs[static_cast<std::size_t>(pair)].first)); m.write8(s.x[1] + 1, static_cast<std::uint8_t>(static_cast<std::uint16_t>(k.pairs[static_cast<std::size_t>(pair)].first) >> 8)); }
        if (s.x[2]) { m.write8(s.x[2], static_cast<std::uint8_t>(k.pairs[static_cast<std::size_t>(pair)].second)); m.write8(s.x[2] + 1, static_cast<std::uint8_t>(static_cast<std::uint16_t>(k.pairs[static_cast<std::size_t>(pair)].second) >> 8)); }
        return ok;
    });
    // color_content(color, r, g, b): xterm's palette, in curses' 0-1000 scale.
    add("_color_content", [](Curses&, CpuState& s, GuestMemory& m) {
        const auto color = static_cast<std::int16_t>(s.x[0]);
        if (color < 0 || color > 255) return err;
        std::array<int, 3> rgb{};
        if (color < 16) {
            static constexpr std::array<std::array<int, 3>, 16> base{{{0, 0, 0}, {205, 0, 0}, {0, 205, 0}, {205, 205, 0}, {0, 0, 238}, {205, 0, 205},
                {0, 205, 205}, {229, 229, 229}, {127, 127, 127}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0}, {92, 92, 255}, {255, 0, 255},
                {0, 255, 255}, {255, 255, 255}}};
            rgb = base[static_cast<std::size_t>(color)];
        } else if (color < 232) {
            const int index = color - 16;
            const auto level = [](int v) { return v ? 55 + v * 40 : 0; };
            rgb = {level(index / 36), level((index / 6) % 6), level(index % 6)};
        } else rgb.fill(8 + (color - 232) * 10);
        for (std::size_t index = 0; index < 3; ++index)
            if (s.x[index + 1]) {
                const auto value = static_cast<std::uint16_t>(rgb[index] * 1000 / 255);
                m.write8(s.x[index + 1], static_cast<std::uint8_t>(value)); m.write8(s.x[index + 1] + 1, static_cast<std::uint8_t>(value >> 8));
            }
        return ok;
    });
    add("_init_color", [](Curses&, CpuState&, GuestMemory&) { return err; });   // can_change_color is FALSE

    // ---- the mouse ----
    // mmask_t mousemask(mmask_t newmask, mmask_t* oldmask): the events reported (xterm SGR mouse mode).
    add("_mousemask", [](Curses& k, CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (s.x[1]) m.write64(s.x[1], k.mouse_mask);
        k.mouse_mask = s.x[0];
        k.out += s.x[0] ? ((s.x[0] & report_mouse_position) ? "\x1b[?1002h\x1b[?1006h" : "\x1b[?1000h\x1b[?1006h") : "\x1b[?1006l\x1b[?1000l\x1b[?1002l";
        k.flush();
        return s.x[0];
    }, 0);
    // getmouse(MEVENT*) / ungetmouse(MEVENT*): MEVENT {short id; int x, y, z; mmask_t bstate}.
    add("_getmouse", [](Curses& k, CpuState& s, GuestMemory& m) {
        if (k.mouse_events.empty() || !s.x[0]) return err;
        const auto event = k.mouse_events.front();
        k.mouse_events.pop_front();
        m.write8(s.x[0], static_cast<std::uint8_t>(event.id)); m.write8(s.x[0] + 1, static_cast<std::uint8_t>(static_cast<std::uint16_t>(event.id) >> 8));
        m.write32(s.x[0] + 4, static_cast<std::uint32_t>(event.x));
        m.write32(s.x[0] + 8, static_cast<std::uint32_t>(event.y));
        m.write32(s.x[0] + 12, static_cast<std::uint32_t>(event.z));
        m.write64(s.x[0] + 16, event.state);
        return ok;
    });
    add("_ungetmouse", [](Curses& k, CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return err;
        MouseEvent event;
        event.id = static_cast<std::int16_t>(m.read8(s.x[0]) | (m.read8(s.x[0] + 1) << 8));
        event.x = static_cast<std::int32_t>(m.read32(s.x[0] + 4));
        event.y = static_cast<std::int32_t>(m.read32(s.x[0] + 8));
        event.z = static_cast<std::int32_t>(m.read32(s.x[0] + 12));
        event.state = m.read64(s.x[0] + 16);
        k.mouse_events.push_front(event);
        k.pushback.push_front(key_mouse);
        return ok;
    });
    add("_mouseinterval", [](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto previous = k.mouse_interval;
        if (static_cast<std::int32_t>(s.x[0]) >= 0) k.mouse_interval = static_cast<std::int32_t>(s.x[0]);
        return static_cast<std::uint64_t>(previous);
    });
    // wenclose(win, y, x): whether screen position (y, x) is inside the window.
    add("_wenclose", [](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t {
        auto* w = k.window(s.x[0]);
        const auto y = static_cast<std::int32_t>(s.x[1]), x = static_cast<std::int32_t>(s.x[2]);
        return w && y >= w->begy && y < w->begy + w->rows && x >= w->begx && x < w->begx + w->cols ? 1 : 0;
    }, 0);

    // ---- window files: putwin(win, FILE*) / getwin(FILE*), Alchemy's own text format ----
    add("_putwin", [](Curses& k, CpuState& s, GuestMemory&) {
        auto& w = k.require_window(s.x[0], "putwin");
        if (!s.x[1]) return err;
        std::string text = "ALCHEMY-CURSES-WINDOW 1 " + std::to_string(w.rows) + " " + std::to_string(w.cols) + " " + std::to_string(w.begy) + " " +
                           std::to_string(w.begx) + " " + std::to_string(w.cury) + " " + std::to_string(w.curx) + " " + std::to_string(w.attrs) + " " +
                           std::to_string(w.background) + "\n";
        for (int y = 0; y < w.rows; ++y)
            for (int x = 0; x < w.cols; ++x) text += std::to_string(static_cast<std::uint32_t>(w.cell(y, x).ch)) + " " + std::to_string(w.cell(y, x).attrs) + "\n";
        const auto buffer = k.p.malloc(text.size());
        k.memory().write_bytes(buffer, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        const auto written = k.sys("_fwrite", {buffer, 1, text.size(), s.x[1]});
        static_cast<void>(k.sys("_free", {buffer}));
        return written == text.size() ? ok : err;
    });
    add("_getwin", [](Curses& k, CpuState& s, GuestMemory&) -> std::uint64_t {
        if (!s.x[0]) return 0;
        std::string text;
        const auto buffer = k.p.malloc(4096);
        for (;;) {
            const auto count = k.sys("_fread", {buffer, 1, 4096, s.x[0]});
            if (!count) break;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
            k.memory().read_bytes(buffer, bytes);
            text.append(bytes.begin(), bytes.end());
        }
        static_cast<void>(k.sys("_free", {buffer}));
        std::vector<std::uint64_t> numbers;
        std::size_t at = 0;
        if (!text.starts_with("ALCHEMY-CURSES-WINDOW 1 ")) return 0;
        at = 24;
        while (at < text.size()) {
            while (at < text.size() && (text[at] == ' ' || text[at] == '\n')) ++at;
            std::uint64_t value = 0; bool digits = false;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9') { value = value * 10 + static_cast<std::uint64_t>(text[at++] - '0'); digits = true; }
            if (!digits) break;
            numbers.push_back(value);
        }
        if (numbers.size() < 8) return 0;
        const int rows = static_cast<int>(numbers[0]), cols = static_cast<int>(numbers[1]);
        if (rows <= 0 || cols <= 0 || numbers.size() < 8 + static_cast<std::size_t>(rows * cols) * 2) return 0;
        auto* w = k.make_window(rows, cols, static_cast<int>(numbers[2]), static_cast<int>(numbers[3]), nullptr, 0, 0);
        w->cury = static_cast<int>(numbers[4]); w->curx = static_cast<int>(numbers[5]);
        w->attrs = static_cast<std::uint32_t>(numbers[6]); w->background = static_cast<std::uint32_t>(numbers[7]);
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < cols; ++x) {
                const auto index = 8 + static_cast<std::size_t>(y * cols + x) * 2;
                w->cell(y, x) = Cell{static_cast<char32_t>(numbers[index]), static_cast<std::uint32_t>(numbers[index + 1])};
            }
        k.mirror(*w);
        return w->guest;
    }, 0);
}

} // namespace alchemy::export_fill
