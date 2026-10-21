// Pseudo-terminals: posix_openpt (and /dev/ptmx), grantpt, unlockpt,
// ptsname, /dev/ttys<n>, openpty, ttyname, isatty, tcgetattr/tcsetattr,
// tcgetpgrp/tcsetpgrp and the terminal ioctls (attributes, window size,
// TIOCPTYGNAME, TIOCSCTTY, the foreground group).
//
// A terminal is a connected stream pair (connected_stream_pair): what the
// master writes the slave reads, and the other way. Both ends carry the
// Terminal, whose attributes are in the named section "alchemy-pty-<n>", so
// every process with an end (a child's inherited slave, a reopened
// /dev/ttys<n>) sees one termios and one window size. Of the line discipline
// only the byte mappings apply: output OPOST|ONLCR (the slave's "\n" reads
// "\r\n" on the master) and input ICRNL, INLCR and IGNCR (the master's
// writes). There is no echo, canonical-mode line editing, or signal
// generation (ISIG, SIGWINCH): programs that put the terminal in raw mode, as
// shells and REPLs do, do not need them.
#include "alchemy/darwin/libsystem/services_impl.hpp"

#include <cstddef>

namespace alchemy {

static_assert(offsetof(TerminalShared, cc) == 32 && offsetof(TerminalShared, ispeed) == 56 &&
              offsetof(TerminalShared, rows) == 72, "TerminalShared starts with Darwin's struct termios and struct winsize");

namespace {
constexpr std::uint32_t enotty = 25, enxio = 6;
constexpr std::uint32_t terminal_limit = 512;   // ttys000-ttys511
constexpr std::size_t termios_size = 72, winsize_size = 8;

std::string section_name(std::uint32_t number) { return "alchemy-pty-" + std::to_string(number); }

std::string device_name(std::uint32_t number) {
    auto digits = std::to_string(number);
    if (digits.size() < 3) digits.insert(0, 3 - digits.size(), '0');
    return "/dev/ttys" + digits;
}

// /dev/ttys<digits>: its number.
std::optional<std::uint32_t> device_number(std::string_view path) {
    constexpr std::string_view prefix = "/dev/ttys";
    if (!path.starts_with(prefix) || path.size() == prefix.size() || path.size() > prefix.size() + 6) return std::nullopt;
    std::uint32_t value = 0;
    for (const auto c : path.substr(prefix.size())) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return value;
}

// A new terminal's attributes (<sys/ttydefaults.h>; a pty's cflag is CS8 | CREAD | HUPCL).
void initialize(TerminalShared& shared) {
    shared = {};
    shared.iflag = 0x2b02;   // BRKINT | ICRNL | IXON | IXANY | IMAXBEL
    shared.oflag = 0x3;      // OPOST | ONLCR
    shared.cflag = 0x4b00;   // CREAD | CS8 | HUPCL
    shared.lflag = 0x5cb;    // ECHOKE | ECHOE | ECHO | ECHOCTL | ISIG | ICANON | IEXTEN
    constexpr std::uint8_t cc[20] = {4, 0xff, 0xff, 0x7f, 0x17, 0x15, 0x12, 0xff, 3, 0x1c, 0x1a, 0x19, 0x11, 0x13, 0x16, 0x0f, 1, 0, 0x14, 0xff};
    std::memcpy(shared.cc, cc, sizeof(cc));
    shared.ispeed = shared.ospeed = 38400;
}

// TIOCSWINSZ, openpty: the struct winsize at `address`.
void set_window_size(const transmute::GuestMemory& memory, std::uint64_t address, TerminalShared& shared) {
    std::array<std::uint8_t, winsize_size> value{};
    memory.read_bytes(address, value);
    std::memcpy(reinterpret_cast<std::uint8_t*>(&shared) + termios_size, value.data(), value.size());
}

// The bytes `input` becomes on the way to the other end.
std::vector<std::uint8_t> mapped(std::span<const std::uint8_t> input, const TerminalShared& shared, bool master) {
    std::vector<std::uint8_t> out;
    out.reserve(input.size() + input.size() / 8);
    if (master) {
        const auto iflag = shared.iflag;
        for (auto byte : input) {
            if (byte == '\r') {
                if (iflag & 0x80) continue;          // IGNCR
                if (iflag & 0x100) byte = '\n';      // ICRNL
            } else if (byte == '\n' && (iflag & 0x40)) byte = '\r';   // INLCR
            out.push_back(byte);
        }
    } else {
        const bool newline = (shared.oflag & 0x3) == 0x3;   // OPOST | ONLCR
        for (const auto byte : input) {
            if (byte == '\n' && newline) out.push_back('\r');
            out.push_back(byte);
        }
    }
    return out;
}
} // namespace

Terminal::~Terminal() {
    if (section) {
        if (shared && shared != local.get()) static_cast<void>(host::sections::unmap(reinterpret_cast<std::uintptr_t>(shared)));
        host::close(section);
    }
}

std::shared_ptr<Terminal> DarwinServices::Impl::terminal_for(std::uint32_t number) {
    std::lock_guard lock(mutex);
    const auto found = terminals.find(number);
    return found == terminals.end() ? nullptr : found->second.lock();
}

std::shared_ptr<NetworkSocket> DarwinServices::Impl::terminal_socket(int descriptor) {
    const auto record = descriptor_file(descriptor);
    if (!record || !record->socket || !record->socket->terminal) return nullptr;
    if (record->closed) throw ServiceError{ebadf};
    return record->socket;
}

bool DarwinServices::Impl::is_terminal(int descriptor) {
    // The process's own standard streams (also when duplicated) are the
    // host's: a terminal when that is a console. Any other descriptor, also
    // one dup2'd or spawned onto 0-2 (a socket, a pipe), is itself.
    const auto standard = descriptor_file(descriptor);
    const auto which = !standard ? descriptor : standard->standard;
    if (which >= 0 && which < 3) {
        using host::process::Standard;
        const auto handle = host::process::standard_handle(which == 0 ? Standard::input : which == 1 ? Standard::output : Standard::error);
        return handle && host::files::is_console(handle);
    }
    const auto record = file(descriptor);
    std::lock_guard lock(record->mutex);
    if (record->closed) throw ServiceError{ebadf};
    if (record->socket) return record->socket->terminal != nullptr;
    if (record->directory || record->queue) return false;
    return record->native && host::files::is_character_device(record->native);
}

int DarwinServices::Impl::open_master_terminal(std::uint64_t flags) {
    if ((flags & 3) != 2) throw ServiceError{einval};   // O_RDWR
    auto terminal = std::make_shared<Terminal>();
    // The first number no process has a section for (the section ends with
    // the last process that has an end of the terminal).
    for (std::uint32_t number = 0; number < terminal_limit && !terminal->section; ++number) {
        const auto named = host::sections::open_named(section_name(number), sizeof(TerminalShared));
        if (!named) throw ServiceError{file_failure(named.error())};
        if (!named->created) { host::close(named->handle); continue; }
        terminal->number = number; terminal->section = named->handle;
    }
    if (!terminal->section) throw ServiceError{35};   // EAGAIN: every number is in use
    const auto view = host::sections::map(terminal->section, 0, sizeof(TerminalShared), true);
    if (!view) throw ServiceError{file_failure(view.error())};
    terminal->shared = reinterpret_cast<TerminalShared*>(*view);
    initialize(*terminal->shared);
    auto [master, slave] = connected_stream_pair();
    master->terminal = terminal; master->terminal_master = true;
    if (flags & 4) master->status_flags |= 4U;   // O_NONBLOCK
    terminal->unopened = std::move(slave);
    { std::lock_guard lock(mutex); terminals[terminal->number] = terminal; }
    const auto descriptor = add_socket(std::move(master));
    if (flags & 0x1000000) set_descriptor_cloexec(descriptor, true);   // O_CLOEXEC
    return descriptor;
}

void DarwinServices::Impl::attach_terminal(const std::shared_ptr<NetworkSocket>& socket, std::uint32_t number, bool master) {
    auto terminal = terminal_for(number);
    if (!terminal) {
        terminal = std::make_shared<Terminal>();
        terminal->number = number;
        if (const auto section = host::sections::open_existing(section_name(number), true)) {
            terminal->section = *section;
            if (const auto view = host::sections::map(terminal->section, 0, sizeof(TerminalShared), true))
                terminal->shared = reinterpret_cast<TerminalShared*>(*view);
        }
        if (!terminal->shared) {
            // Every other end is gone: this process keeps the attributes alone.
            terminal->local = std::make_unique<TerminalShared>();
            initialize(*terminal->local);
            terminal->shared = terminal->local.get();
        }
        std::lock_guard lock(mutex);
        terminals[number] = terminal;
    }
    socket->terminal = std::move(terminal);
    socket->terminal_master = master;
}

std::optional<int> DarwinServices::Impl::open_pseudo_terminal(const std::filesystem::path& name, std::uint64_t flags) {
    if (!name.has_root_directory() || name.has_root_name()) return std::nullopt;
    const auto text = name.generic_u8string();
    const std::string_view path(reinterpret_cast<const char*>(text.data()), text.size());
    if (path == "/dev/ptmx") return open_master_terminal(flags);
    const auto number = device_number(path);
    if (!number) return std::nullopt;
    if (flags & 0x100000) throw ServiceError{enotdir};   // O_DIRECTORY
    // A slave end this process already has (one it inherited), else the
    // master's slave end on its first open here.
    std::shared_ptr<NetworkSocket> slave;
    const auto consider = [&](const std::shared_ptr<File>& record) {
        if (slave || !record || record->closed || !record->socket || record->socket->closed) return;
        const auto& socket = record->socket;
        if (socket->terminal && socket->terminal->number == *number && !socket->terminal_master) slave = socket;
    };
    if (overlay) for (const auto& [descriptor, slot] : overlay->slots) consider(slot.file);
    { std::lock_guard lock(mutex); for (const auto& [descriptor, record] : files) consider(record); }
    if (slave) ++slave->aliases;
    else if (const auto terminal = terminal_for(*number)) {
        std::lock_guard lock(terminal->gate);
        if (terminal->unopened) {
            slave = std::move(terminal->unopened);
            slave->terminal = terminal; slave->terminal_master = false;
        }
    }
    if (!slave) throw ServiceError{enxio};
    // Without O_NOCTTY a session leader takes the terminal as its controlling
    // terminal, its group the foreground one.
    if (!(flags & 0x20000) && own_sid == own_pid && !slave->terminal->shared->foreground)
        slave->terminal->shared->foreground = own_pgid;
    if (flags & 4) slave->status_flags |= 4U;   // O_NONBLOCK
    auto record = std::make_shared<File>();
    record->socket = std::move(slave);
    record->readable = (flags & 3) != 1; record->writable = (flags & 3) != 0;
    record->flags = flags & 3; record->cloexec = (flags & 0x1000000) != 0;
    return add_file(std::move(record));
}

std::uint64_t DarwinServices::Impl::terminal_write(transmute::GuestMemory& memory, const std::shared_ptr<File>& record,
    std::uint64_t address, std::uint64_t size) {
    const auto& socket = *record->socket;
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size, 65536));   // a short write, as a terminal's
    validate(memory, address, count, false);
    std::vector<std::uint8_t> input(count);
    memory.read_bytes(address, input);
    const auto output = mapped(input, *socket.terminal->shared, socket.terminal_master);
    if (output == input) return network_io(memory, record, address, count, 0, true);
    const auto temporary = context.allocate_guest(std::max<std::size_t>(output.size(), 1));
    std::uint64_t sent;
    try {
        memory.write_bytes(temporary, output);
        sent = network_io(memory, record, temporary, output.size(), 0, true);
    } catch (...) { context.release_guest(temporary); throw; }
    context.release_guest(temporary);
    if (sent == output.size()) return count;
    // A short send: the input bytes whose mapping went out whole (at least one).
    std::uint64_t consumed = 0, covered = 0;
    for (const auto byte : input) {
        const auto width = mapped(std::span(&byte, 1), *socket.terminal->shared, socket.terminal_master).size();
        if (covered + width > sent) break;
        covered += width; ++consumed;
    }
    return std::max<std::uint64_t>(consumed, sent ? 1 : 0);
}

std::uint64_t DarwinServices::Impl::terminal_stat(transmute::GuestMemory& memory, std::uint64_t result, const NetworkSocket& socket) {
    // A character device, ttys major 16 (the master: ptc, major 15).
    const auto number = socket.terminal->number;
    const DeviceNode node{"", 020620, false, ((socket.terminal_master ? 15U : 16U) << 24) | number,
                          (socket.terminal_master ? 0x50544300U : 0x54545900U) | number};
    return device_stat(memory, result, node, 0);
}

std::optional<std::uint64_t> DarwinServices::Impl::terminal_ioctl(transmute::GuestMemory& memory, const std::shared_ptr<File>& record,
    std::uint64_t request, std::uint64_t stack) {
    const auto& socket = *record->socket;
    auto& shared = *socket.terminal->shared;
    const auto argument = [&](std::uint64_t size, bool out) { const auto value = memory.read64(stack); validate(memory, value, size, out); return value; };
    auto* bytes = reinterpret_cast<std::uint8_t*>(&shared);
    switch (request) {
    case 0x40487413: {   // TIOCGETA (tcgetattr)
        const auto at = argument(termios_size, true);
        memory.write_bytes(at, std::span<const std::uint8_t>(bytes, termios_size));
        return 0;
    }
    case 0x80487414: case 0x80487415: case 0x80487416: {   // TIOCSETA, TIOCSETAW, TIOCSETAF (tcsetattr)
        const auto at = argument(termios_size, false);
        std::array<std::uint8_t, termios_size> value{};
        memory.read_bytes(at, value);
        std::memcpy(bytes, value.data(), value.size());
        return 0;
    }
    case 0x40087468: {   // TIOCGWINSZ
        memory.write_bytes(argument(winsize_size, true), std::span<const std::uint8_t>(bytes + termios_size, winsize_size));
        return 0;
    }
    case 0x80087467: {   // TIOCSWINSZ
        set_window_size(memory, argument(winsize_size, false), shared);
        return 0;
    }
    case 0x40807453: {   // TIOCPTYGNAME: the master's slave name (libuv tells the ends apart by it)
        if (!socket.terminal_master) return std::nullopt;
        put_text(memory, argument(128, true), device_name(socket.terminal->number));
        return 0;
    }
    case 0x20007454: case 0x20007452:   // TIOCPTYGRANT, TIOCPTYUNLK
        if (!socket.terminal_master) return std::nullopt;
        if (request == 0x20007452) shared.unlocked = 1;
        return 0;
    case 0x20007461:   // TIOCSCTTY
        if (socket.terminal_master) throw ServiceError{einval};
        shared.foreground = own_pgid;
        return 0;
    case 0x40047477: {   // TIOCGPGRP
        if (!shared.foreground) throw ServiceError{enotty};
        memory.write32(argument(4, true), shared.foreground);
        return 0;
    }
    case 0x80047476:   // TIOCSPGRP
        shared.foreground = memory.read32(argument(4, false));
        return 0;
    case 0x40047473:   // TIOCOUTQ: written bytes are already sent
        memory.write32(argument(4, true), 0);
        return 0;
    case 0x80047410: case 0x2000740d: case 0x2000740e: case 0x20007471:   // TIOCFLUSH, TIOCEXCL, TIOCNXCL, TIOCNOTTY
        return 0;
    default:
        return std::nullopt;
    }
}

void DarwinServices::Impl::install_terminals() {
    const auto master = [this](std::uint64_t descriptor) {
        if (descriptor > INT32_MAX) throw ServiceError{ebadf};
        const auto socket = terminal_socket(static_cast<int>(descriptor));
        if (!socket || !socket->terminal_master) throw ServiceError{einval};
        return socket;
    };
    const auto name_buffer = [this] {
        std::lock_guard lock(mutex);
        if (!terminal_name) terminal_name = context.allocate_guest(128);
        return terminal_name;
    };
    install("_posix_openpt", [this](auto& s, auto&) { return static_cast<std::uint64_t>(open_master_terminal(s.x[0])); });
    install("_grantpt", [master](auto& s, auto&) { static_cast<void>(master(s.x[0])); return std::uint64_t{0}; });
    install("_unlockpt", [master](auto& s, auto&) { master(s.x[0])->terminal->shared->unlocked = 1; return std::uint64_t{0}; });
    install("_ptsname", [master, name_buffer](auto& s, auto& m) {
        const auto name = device_name(master(s.x[0])->terminal->number);
        const auto buffer = name_buffer();
        put_text(m, buffer, name);
        return buffer;
    }, 0);
    install("_ptsname_r", [master](auto& s, auto& m) {
        const auto name = device_name(master(s.x[0])->terminal->number);
        if (!s.x[1]) throw ServiceError{einval};
        if (s.x[2] < name.size() + 1) throw ServiceError{erange};
        validate(m, s.x[1], name.size() + 1, true);
        put_text(m, s.x[1], name);
        return std::uint64_t{0};
    });
    // openpty(amaster, aslave, name, termp, winp).
    install("_openpty", [this](auto& s, auto& m) {
        validate(m, s.x[0], 4, true); validate(m, s.x[1], 4, true);
        const auto first = open_master_terminal(2);
        try {
            const auto socket = terminal_socket(first);
            const auto name = device_name(socket->terminal->number);
            auto& shared = *socket->terminal->shared;
            if (s.x[3]) {
                validate(m, s.x[3], termios_size, false);
                std::array<std::uint8_t, termios_size> value{};
                m.read_bytes(s.x[3], value);
                std::memcpy(&shared, value.data(), value.size());
            }
            if (s.x[4]) { validate(m, s.x[4], winsize_size, false); set_window_size(m, s.x[4], shared); }
            const auto second = *open_pseudo_terminal(std::filesystem::path(name), 2 | 0x20000);   // O_RDWR | O_NOCTTY
            if (s.x[2]) { validate(m, s.x[2], name.size() + 1, true); put_text(m, s.x[2], name); }
            m.write32(s.x[0], static_cast<std::uint32_t>(first)); m.write32(s.x[1], static_cast<std::uint32_t>(second));
        } catch (...) { static_cast<void>(close(first)); throw; }
        return std::uint64_t{0};
    });
    // ttyname: a pseudo-terminal end is /dev/ttys<n>; the console is /dev/tty.
    install("_ttyname", [this, name_buffer](auto& s, auto& m) {
        if (s.x[0] > INT32_MAX) throw ServiceError{ebadf};
        const auto descriptor = static_cast<int>(s.x[0]);
        const auto socket = terminal_socket(descriptor);
        if (!socket && !is_terminal(descriptor)) throw ServiceError{enotty};
        const auto buffer = name_buffer();
        put_text(m, buffer, socket ? device_name(socket->terminal->number) : std::string("/dev/tty"));
        return buffer;
    }, 0);
    // ttyname_r returns the error number.
    install("_ttyname_r", [this](auto& s, auto& m) -> std::uint64_t {
        try {
            if (s.x[0] > INT32_MAX) return ebadf;
            const auto descriptor = static_cast<int>(s.x[0]);
            const auto socket = terminal_socket(descriptor);
            if (!socket && !is_terminal(descriptor)) return enotty;
            const auto name = socket ? device_name(socket->terminal->number) : std::string("/dev/tty");
            if (s.x[2] < name.size() + 1) return erange;
            validate(m, s.x[1], name.size() + 1, true);
            put_text(m, s.x[1], name);
            return 0;
        } catch (const ServiceError& error) { return error.value; }
    });
    const auto console_unsupported = [](const char* name) -> std::uint64_t {
        // A console terminal would need echo/canonical-mode mapping, which is not implemented.
        throw std::runtime_error(std::string("Alchemy: ") + name + " on a console terminal is unsupported");
    };
    install("_tcgetattr", [this, console_unsupported](auto& s, auto& m) {
        if (s.x[0] > INT32_MAX) throw ServiceError{ebadf};
        const auto socket = terminal_socket(static_cast<int>(s.x[0]));
        if (!socket) { if (!is_terminal(static_cast<int>(s.x[0]))) throw ServiceError{enotty}; return console_unsupported("tcgetattr"); }
        validate(m, s.x[1], termios_size, true);
        m.write_bytes(s.x[1], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(socket->terminal->shared), termios_size));
        return std::uint64_t{0};
    });
    install("_tcsetattr", [this, console_unsupported](auto& s, auto& m) {
        if (s.x[0] > INT32_MAX) throw ServiceError{ebadf};
        const auto socket = terminal_socket(static_cast<int>(s.x[0]));
        if (!socket) { if (!is_terminal(static_cast<int>(s.x[0]))) throw ServiceError{enotty}; return console_unsupported("tcsetattr"); }
        if ((s.x[1] & ~0x10ULL) > 2) throw ServiceError{einval};   // TCSANOW, TCSADRAIN, TCSAFLUSH (| TCSASOFT)
        validate(m, s.x[2], termios_size, false);
        std::array<std::uint8_t, termios_size> value{};
        m.read_bytes(s.x[2], value);
        std::memcpy(socket->terminal->shared, value.data(), value.size());
        return std::uint64_t{0};
    });
    install("_tcgetpgrp", [this](auto& s, auto&) {
        if (s.x[0] > INT32_MAX) throw ServiceError{ebadf};
        const auto socket = terminal_socket(static_cast<int>(s.x[0]));
        if (!socket || !socket->terminal->shared->foreground) throw ServiceError{enotty};
        return static_cast<std::uint64_t>(socket->terminal->shared->foreground);
    });
    install("_tcsetpgrp", [this](auto& s, auto&) {
        if (s.x[0] > INT32_MAX) throw ServiceError{ebadf};
        const auto socket = terminal_socket(static_cast<int>(s.x[0]));
        if (!socket) throw ServiceError{enotty};
        if (static_cast<std::int32_t>(s.x[1]) <= 0) throw ServiceError{einval};
        socket->terminal->shared->foreground = static_cast<std::uint32_t>(s.x[1]);
        return std::uint64_t{0};
    });
}

} // namespace alchemy
