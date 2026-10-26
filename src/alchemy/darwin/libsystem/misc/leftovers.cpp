#include "alchemy/gui/fill_provider.hpp"
#include "alchemy/darwin/compression/install.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

// libSystem, libbsm and libbz2 names Google Chrome's images import that no
// other provider exports (docs/ALCHEMY_CHROME_GAPS.md, "libSystem"; Mach and
// VM names belong to other providers). Fill-only: installed after every
// provider, a name is added only when it is still missing.
//
// Real:
// - getopt, getopt_long and their globals (optarg, optind, opterr, optopt,
//   optreset), BSD semantics: getopt_long permutes non-options to the end
//   unless the option string starts with '+' (or '-': non-options are
//   returned as argument 1); a leading ':' reports a missing argument as ':'
//   without a message. crashpad_handler parses its command line this way.
// - getprogname/setprogname (over __progname),
//   nan/nanf/fmaf/nearbyintf, __math_errhandling (MATH_ERREXCEPT: Darwin's
//   libm does not set errno), ___mb_cur_max/___mb_cur_max_l and
//   __maskrune_l (over the locale provider), tmpnam, _dirhelper (over
//   confstr's _CS_DARWIN_USER_* names), dispatch_queue_create_with_target
//   (create, then set the target queue), the Mach-O byte-swap helpers of
//   libmacho (swap_*), mbr_uid_to_uuid (Open Directory's compatibility UUID
//   FFFFEEEE-DDDD-CCCC-BBBB-AAAA<uid>), audit_token_to_* (libbsm), QoS
//   queries and overrides (QoS is a scheduling hint: an override is a real
//   token that changes nothing on the host, like pthread_set_qos_class_self_np),
//   csr_get_active_config (no System Integrity Protection flags disabled),
//   and csops(CS_OPS_STATUS) (the process is not code-signed: flags 0).
//   libbz2's streaming decompressor is darwin/compression/bzip2_stream.cpp's,
//   installed first here.
// Documented failures:
// - ACLs: files carry no extended ACL. acl_get_link_np returns NULL with
//   ENOENT (macOS's answer for a file without one); the entry and permset
//   accessors fail with EINVAL, as no acl_t can exist.
// - __mac_syscall: no MAC policy module is loaded (ENOSYS).
// - getattrlist: not provided by the host filesystem layer (ENOTSUP).
// - csops operations other than CS_OPS_STATUS: ENOTSUP (no code signature).
namespace alchemy::gui::fill {
namespace {

constexpr std::uint32_t esrch = 3, einval = 22, enoent = 2, enosys = 78, enotsup = 45;
constexpr Address minus_one = static_cast<Address>(-1);

std::uint32_t swap32(std::uint32_t v) { return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24); }

// Byte-swaps `words32` 32-bit words at `address`, then `words64` 64-bit words.
void swap_words(GuestMemory& m, Address address, unsigned words32, unsigned words64 = 0) {
    for (unsigned index = 0; index < words32; ++index) m.write32(address + index * 4ULL, swap32(m.read32(address + index * 4ULL)));
    const auto wide = address + words32 * 4ULL;
    for (unsigned index = 0; index < words64; ++index) {
        const auto value = m.read64(wide + index * 8ULL);
        m.write64(wide + index * 8ULL, (static_cast<std::uint64_t>(swap32(static_cast<std::uint32_t>(value))) << 32) | swap32(static_cast<std::uint32_t>(value >> 32)));
    }
}

class Leftovers final : public Provider {
public:
    using Provider::Provider;
    void install();

private:
    // ---- getopt ----
    struct Options {
        Address optarg{}, optind{}, opterr{}, optopt{}, optreset{};
        Address place{};                       // next short option character inside argv[optind]
        std::uint32_t first_nonopt{1}, last_nonopt{1};
        std::mutex mutex;
    } options_;
    std::int64_t getopt(GuestMemory& m, std::uint32_t argc, Address argv, Address optstring, Address longopts, Address longindex, bool long_form,
                        bool long_only = false);
    void exchange(GuestMemory& m, Address argv, std::uint32_t optind);
    void option_error(GuestMemory& m, Address optstring, const std::string& message);
    std::string progname(GuestMemory& m);

    void install_getopt();
    void install_swap();
};

std::string Leftovers::progname(GuestMemory& m) {
    if (const auto cell = process.resolve(libsystem, "___progname"))
        if (const auto name = m.read64(cell->address)) return read_cstring(m, name, 1024);
    return "alchemy";
}

void Leftovers::option_error(GuestMemory& m, Address optstring, const std::string& message) {
    if (!m.read32(options_.opterr) || (optstring && m.read8(optstring) == ':')) return;
    const auto line = progname(m) + ": " + message + "\n";
    const auto text = strdup(line);
    static_cast<void>(call(libsystem, "_write", {2, text, line.size()}));
    free(text);
}

// Moves the non-options [first_nonopt, last_nonopt) after the options
// [last_nonopt, optind) that followed them.
void Leftovers::exchange(GuestMemory& m, Address argv, std::uint32_t optind) {
    auto& o = options_;
    std::vector<Address> items;
    for (auto index = o.first_nonopt; index < optind; ++index) items.push_back(m.read64(argv + index * 8ULL));
    std::rotate(items.begin(), items.begin() + (o.last_nonopt - o.first_nonopt), items.end());
    for (std::size_t index = 0; index < items.size(); ++index) m.write64(argv + (o.first_nonopt + index) * 8ULL, items[index]);
    o.first_nonopt += optind - o.last_nonopt;
    o.last_nonopt = optind;
}

std::int64_t Leftovers::getopt(GuestMemory& m, std::uint32_t argc, Address argv, Address optstring_address, Address longopts,
                               Address longindex, bool long_form, bool long_only) {
    auto& o = options_;
    std::lock_guard lock(o.mutex);
    const auto spec_text = read_cstring(m, optstring_address);
    std::string_view spec = spec_text;
    enum class Order { permute, require, in_order } order = long_form ? Order::permute : Order::require;
    if (!spec.empty() && (spec[0] == '+' || spec[0] == '-')) { order = spec[0] == '+' ? Order::require : Order::in_order; spec.remove_prefix(1); }
    const bool colon = !spec.empty() && spec[0] == ':';
    auto optind = static_cast<std::uint32_t>(m.read32(o.optind));
    if (optind == 0 || m.read32(o.optreset)) {
        if (optind == 0) optind = 1;
        m.write32(o.optreset, 0);
        o.place = 0;
        o.first_nonopt = o.last_nonopt = optind;
    }
    m.write64(o.optarg, 0);
    const auto arg = [&](std::uint32_t index) { return m.read64(argv + index * 8ULL); };
    const auto is_option = [&](std::uint32_t index) {
        const auto text = arg(index);
        return text && m.read8(text) == '-' && m.read8(text + 1) != 0;
    };
    const auto finish = [&](std::int64_t value) { m.write32(o.optind, optind); return value; };
    if (!o.place || !m.read8(o.place)) {
        o.place = 0;
        if (o.last_nonopt > optind) o.last_nonopt = optind;
        if (o.first_nonopt > optind) o.first_nonopt = optind;
        if (order == Order::permute) {
            if (o.first_nonopt != o.last_nonopt && o.last_nonopt != optind) exchange(m, argv, optind);
            else if (o.last_nonopt != optind) o.first_nonopt = optind;
            while (optind < argc && !is_option(optind)) ++optind;
            o.last_nonopt = optind;
        }
        if (optind < argc && read_cstring(m, arg(optind), 3) == "--") {
            ++optind;
            if (order == Order::permute) {
                if (o.first_nonopt != o.last_nonopt && o.last_nonopt != optind) exchange(m, argv, optind);
                else if (o.first_nonopt == o.last_nonopt) o.first_nonopt = optind;
                o.last_nonopt = argc;
                optind = argc;
            }
        }
        if (optind >= argc) {
            if (order == Order::permute && o.first_nonopt != o.last_nonopt) optind = o.first_nonopt;
            return finish(-1);
        }
        if (!is_option(optind)) {
            if (order == Order::in_order) { m.write64(o.optarg, arg(optind++)); return finish(1); }
            return finish(-1);
        }
        const auto text = arg(optind);
        // getopt_long_only: "-name" is a long option too. When its first letter is a short
        // option, it is one only if a long option is named name (or, for longer names,
        // starts with it); otherwise it is short options ("-v", "-vd").
        const bool double_dash = m.read8(text + 1) == '-';
        bool long_option = longopts && double_dash;
        if (longopts && !double_dash && long_only) {
            const auto body = read_cstring(m, text + 1);
            const auto name = body.substr(0, body.find('='));
            const bool short_too = !name.empty() && name[0] != ':' && spec.find(name[0]) != std::string_view::npos;
            long_option = !short_too;
            for (std::uint32_t index = 0; !long_option; ++index) {
                const auto entry_name = m.read64(longopts + index * 32ULL);
                if (!entry_name) break;
                const auto candidate = read_cstring(m, entry_name);
                long_option = candidate == name || (name.size() > 1 && candidate.starts_with(name));
            }
        }
        if (long_option) {
            // --name[=value] (or -name[=value] for getopt_long_only)
            const auto body = read_cstring(m, text + (double_dash ? 2 : 1));
            const auto equals = body.find('=');
            const auto name = body.substr(0, equals);
            ++optind;
            std::int64_t match = -1;
            bool exact = false, ambiguous = false;
            for (std::uint32_t index = 0;; ++index) {
                const auto entry = longopts + index * 32ULL;
                const auto entry_name = m.read64(entry);
                if (!entry_name) break;
                const auto candidate = read_cstring(m, entry_name);
                if (candidate == name) { match = index; exact = true; break; }
                if (candidate.starts_with(name)) { if (match < 0) match = index; else ambiguous = true; }
            }
            if (match < 0 || (ambiguous && !exact)) {
                m.write32(o.optopt, 0);
                option_error(m, optstring_address, (ambiguous ? "option `--" : "unrecognized option `--") + name + (ambiguous ? "' is ambiguous" : "'"));
                return finish('?');
            }
            const auto entry = longopts + static_cast<Address>(match) * 32ULL;
            const auto has_arg = static_cast<std::int32_t>(m.read32(entry + 8));
            const auto flag = m.read64(entry + 16);
            const auto value = static_cast<std::int32_t>(m.read32(entry + 24));
            const auto full_name = read_cstring(m, m.read64(entry));
            if (equals != std::string::npos) {
                if (has_arg == 0) {
                    m.write32(o.optopt, flag ? 0 : static_cast<std::uint32_t>(value));
                    option_error(m, optstring_address, "option `--" + full_name + "' doesn't allow an argument");
                    return finish('?');
                }
                m.write64(o.optarg, text + (double_dash ? 2 : 1) + equals + 1);
            } else if (has_arg == 1) {
                if (optind >= argc) {
                    m.write32(o.optopt, flag ? 0 : static_cast<std::uint32_t>(value));
                    option_error(m, optstring_address, "option `--" + full_name + "' requires an argument");
                    return finish(colon ? ':' : '?');
                }
                m.write64(o.optarg, arg(optind++));
            }
            if (longindex) m.write32(longindex, static_cast<std::uint32_t>(match));
            if (flag) { m.write32(flag, static_cast<std::uint32_t>(value)); return finish(0); }
            return finish(value);
        }
        o.place = text + 1;
    }
    // A short option character.
    const auto c = static_cast<char>(m.read8(o.place++));
    const auto found = c == ':' ? std::string_view::npos : spec.find(c, colon ? 1 : 0);
    const bool last = !m.read8(o.place);
    if (found == std::string_view::npos) {
        if (last) { ++optind; o.place = 0; }
        m.write32(o.optopt, static_cast<std::uint8_t>(c));
        option_error(m, optstring_address, std::string("illegal option -- ") + c);
        return finish('?');
    }
    const bool takes = found + 1 < spec.size() && spec[found + 1] == ':';
    const bool optional = takes && found + 2 < spec.size() && spec[found + 2] == ':';
    if (!takes) {
        if (last) { ++optind; o.place = 0; }
        return finish(static_cast<std::uint8_t>(c));
    }
    if (!last) m.write64(o.optarg, o.place);
    else if (!optional) {
        if (optind + 1 >= argc) {
            ++optind; o.place = 0;
            m.write32(o.optopt, static_cast<std::uint8_t>(c));
            option_error(m, optstring_address, std::string("option requires an argument -- ") + c);
            return finish(colon ? ':' : '?');
        }
        m.write64(o.optarg, arg(++optind));
    }
    ++optind; o.place = 0;
    return finish(static_cast<std::uint8_t>(c));
}

void Leftovers::install_getopt() {
    const std::array<std::uint8_t, 8> null{};
    const std::array<std::uint8_t, 4> one{1, 0, 0, 0}, zero{};
    options_.optarg = data(libsystem, "_optarg", null, 8);
    options_.optind = data(libsystem, "_optind", one, 4);
    options_.opterr = data(libsystem, "_opterr", one, 4);
    options_.optopt = data(libsystem, "_optopt", zero, 4);
    options_.optreset = data(libsystem, "_optreset", zero, 4);
    // getopt(argc, argv, optstring); getopt_long(argc, argv, optstring, longopts, longindex)
    fn(libsystem, "_getopt", [this](CpuState& s, GuestMemory& m) {
        return static_cast<Address>(getopt(m, static_cast<std::uint32_t>(s.x[0]), s.x[1], s.x[2], 0, 0, false));
    });
    fn(libsystem, "_getopt_long", [this](CpuState& s, GuestMemory& m) {
        return static_cast<Address>(getopt(m, static_cast<std::uint32_t>(s.x[0]), s.x[1], s.x[2], s.x[3], s.x[4], true));
    });
    fn(libsystem, "_getopt_long_only", [this](CpuState& s, GuestMemory& m) {
        return static_cast<Address>(getopt(m, static_cast<std::uint32_t>(s.x[0]), s.x[1], s.x[2], s.x[3], s.x[4], true, true));
    });
}

// libmacho's byte-swap helpers (mach-o/swap.h): every field of these
// structures is a 32- or 64-bit integer except segment names, so a swap is
// the same in either direction (the target byte order is not needed).
void Leftovers::install_swap() {
    fn(libsystem, "_swap_fat_header", [](CpuState& s, GuestMemory& m) { swap_words(m, s.x[0], 2); return Address{0}; });
    fn(libsystem, "_swap_fat_arch", [](CpuState& s, GuestMemory& m) {
        for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(s.x[1]); ++index) swap_words(m, s.x[0] + index * 20ULL, 5);
        return Address{0};
    });
    fn(libsystem, "_swap_mach_header", [](CpuState& s, GuestMemory& m) { swap_words(m, s.x[0], 7); return Address{0}; });
    fn(libsystem, "_swap_mach_header_64", [](CpuState& s, GuestMemory& m) { swap_words(m, s.x[0], 8); return Address{0}; });
    fn(libsystem, "_swap_load_command", [](CpuState& s, GuestMemory& m) { swap_words(m, s.x[0], 2); return Address{0}; });
    // cmd, cmdsize, segname[16], then 8 x uint32 (32-bit) or 4 x uint64 + 4 x uint32 (64-bit).
    fn(libsystem, "_swap_segment_command", [](CpuState& s, GuestMemory& m) {
        swap_words(m, s.x[0], 2); swap_words(m, s.x[0] + 24, 8); return Address{0};
    });
    fn(libsystem, "_swap_segment_command_64", [](CpuState& s, GuestMemory& m) {
        swap_words(m, s.x[0], 2); swap_words(m, s.x[0] + 24, 0, 4); swap_words(m, s.x[0] + 56, 4); return Address{0};
    });
    fn(libsystem, "_swap_symtab_command", [](CpuState& s, GuestMemory& m) { swap_words(m, s.x[0], 6); return Address{0}; });
    fn(libsystem, "_swap_linkedit_data_command", [](CpuState& s, GuestMemory& m) { swap_words(m, s.x[0], 4); return Address{0}; });
}

// bz_stream: next_in 0, avail_in 8, total_in_lo32 12, total_in_hi32 16,
// next_out 24, avail_out 32, total_out_lo32 36, total_out_hi32 40, state 48.
void Leftovers::install() {
    install_getopt();
    install_swap();

    // ---- program name and err(3) ----
    fn(libsystem, "_getprogname", [this](CpuState&, GuestMemory& m) -> Address {
        const auto cell = process.resolve(libsystem, "___progname");
        return cell ? m.read64(cell->address) : 0;
    });
    fn(libsystem, "_setprogname", [this](CpuState& s, GuestMemory& m) -> Address {
        const auto cell = process.resolve(libsystem, "___progname");
        if (cell && s.x[0]) {
            const auto name = read_cstring(m, s.x[0], 4096);
            const auto slash = name.find_last_of('/');
            m.write64(cell->address, s.x[0] + (slash == std::string::npos ? 0 : slash + 1));
        }
        return 0;
    });

    // ---- math ----
    raw(libsystem, "_nan", [](CpuState& s, GuestMemory&) { return_real(s, std::numeric_limits<double>::quiet_NaN()); });
    raw(libsystem, "_nanf", [](CpuState& s, GuestMemory&) { return_real32(s, std::numeric_limits<float>::quiet_NaN()); });
    raw(libsystem, "_fmaf", [](CpuState& s, GuestMemory&) { return_real32(s, std::fmaf(real32(s, 0), real32(s, 1), real32(s, 2))); });
    // Round to nearest, ties to even (the default rounding mode), without raising inexact.
    raw(libsystem, "_nearbyintf", [](CpuState& s, GuestMemory&) { return_real32(s, std::nearbyintf(real32(s, 0))); });
    constant(libsystem, "___math_errhandling", 2);   // MATH_ERREXCEPT

    // ---- locale (over the locale provider) ----
    fn(libsystem, "____mb_cur_max", [this](CpuState&, GuestMemory&) { return call(libsystem, "___mb_cur_max", {}); });
    fn(libsystem, "____mb_cur_max_l", [this](CpuState& s, GuestMemory&) { return call(libsystem, "___mb_cur_max_l", {s.x[0]}); });
    // __maskrune_l(c, mask, locale): the locale provider has one character table.
    fn(libsystem, "___maskrune_l", [this](CpuState& s, GuestMemory&) { return call(libsystem, "___maskrune", {s.x[0], s.x[1]}); });

    // ---- files ----
    // char *tmpnam(char *s): "/tmp/tmp.XXXXXX" names that do not exist yet (L_tmpnam 1024).
    fn(libsystem, "_tmpnam", [this](CpuState& s, GuestMemory& m) -> Address {
        static Address shared{};
        auto target = s.x[0];
        if (!target) { if (!shared) shared = process.allocate_guest(1024); target = shared; }
        const auto pattern = std::string("/tmp/tmp.XXXXXX");
        m.write_bytes(target, std::span(reinterpret_cast<const std::uint8_t*>(pattern.c_str()), pattern.size() + 1));
        return call(libsystem, "_mktemp", {target});
    });
    // char *_dirhelper(which, char *path, size_t length): DIRHELPER_USER_LOCAL (0),
    // _TEMP (1), _CACHE (2) are confstr's _CS_DARWIN_USER_DIR/_TEMP_DIR/_CACHE_DIR.
    // (Private interface: the enum values are from the open-source Libc, unverified.)
    fn(libsystem, "__dirhelper", [this](CpuState& s, GuestMemory&) -> Address {
        if (s.x[0] > 2 || !s.x[1]) { set_errno(einval); return 0; }
        const auto length = call(libsystem, "_confstr", {65536 + s.x[0], s.x[1], s.x[2]});
        return length && length <= s.x[2] ? s.x[1] : 0;
    });
    // Attribute lists (getattrlist(2) family): no volume here offers them, as a file system
    // without attribute-list support answers.
    for (const auto name : {"_getattrlist", "_fgetattrlist", "_getattrlistat", "_setattrlist", "_fsetattrlist", "_setattrlistat"})
        fn(libsystem, name, [this](CpuState&, GuestMemory&) { set_errno(enotsup); return minus_one; });
    fn(libsystem, "_posix_spawn_file_actions_addchdir", [this](CpuState& s, GuestMemory&) {
        return call(libsystem, "_posix_spawn_file_actions_addchdir_np", {s.x[0], s.x[1]});
    });

    // ---- ACLs: no file carries one ----
    for (const auto name : {"_acl_get_link_np", "_acl_get_file", "_acl_get_fd", "_acl_get_fd_np"})
        fn(libsystem, name, [this](CpuState&, GuestMemory&) -> Address { set_errno(enoent); return 0; });
    // acl_valid(acl): no acl_t is ever handed out, so none is valid.
    fn(libsystem, "_acl_valid", [this](CpuState&, GuestMemory&) { set_errno(einval); return minus_one; });
    fn(libsystem, "_acl_free", [this](CpuState& s, GuestMemory&) -> Address {
        if (!s.x[0]) { set_errno(einval); return minus_one; }
        free(s.x[0]);   // text from acl_to_text is malloc'd; no acl_t is ever handed out
        return 0;
    });
    for (const auto name : {"_acl_get_entry", "_acl_get_tag_type", "_acl_get_permset_mask_np"})
        fn(libsystem, name, [this](CpuState&, GuestMemory&) { set_errno(einval); return minus_one; });
    fn(libsystem, "_acl_get_qualifier", [this](CpuState&, GuestMemory&) -> Address { set_errno(einval); return 0; });

    // ---- security and code signing ----
    fn(libsystem, "___mac_syscall", [this](CpuState&, GuestMemory&) { set_errno(enosys); return minus_one; });
    // int csops(pid, ops, void *useraddr, size_t usersize): CS_OPS_STATUS (0) reports
    // flags 0 (not signed, not hardened) for this process; no other operation.
    fn(libsystem, "_csops", [this](CpuState& s, GuestMemory& m) -> Address {
        const auto pid = static_cast<std::uint32_t>(s.x[0]);
        if (pid && pid != static_cast<std::uint32_t>(call(libsystem, "_getpid", {}))) { set_errno(esrch); return minus_one; }
        if (s.x[1] != 0) { set_errno(enotsup); return minus_one; }
        if (!s.x[2] || s.x[3] < 4) { set_errno(einval); return minus_one; }
        m.write32(s.x[2], 0);
        return 0;
    });
    fn(libsystem, "_csr_get_active_config", [](CpuState& s, GuestMemory& m) -> Address {
        if (!s.x[0]) return einval;
        m.write32(s.x[0], 0);
        return 0;
    });
    // int mbr_uid_to_uuid(uid_t, uuid_t): the compatibility UUID FFFFEEEE-DDDD-CCCC-BBBB-AAAA<uid>.
    fn(libsystem, "_mbr_uid_to_uuid", [](CpuState& s, GuestMemory& m) -> Address {
        const std::array<std::uint8_t, 12> prefix{0xff, 0xff, 0xee, 0xee, 0xdd, 0xdd, 0xcc, 0xcc, 0xbb, 0xbb, 0xaa, 0xaa};
        m.write_bytes(s.x[1], prefix);
        const auto uid = static_cast<std::uint32_t>(s.x[0]);
        for (unsigned index = 0; index < 4; ++index) m.write8(s.x[1] + 12 + index, static_cast<std::uint8_t>(uid >> (24 - index * 8)));
        return 0;
    });
    // libbsm: audit_token_t {auid, euid, egid, ruid, rgid, pid, asid, pidversion}, by reference.
    const std::array<std::pair<const char*, unsigned>, 7> token_fields{{{"_audit_token_to_auid", 0}, {"_audit_token_to_euid", 1},
        {"_audit_token_to_egid", 2}, {"_audit_token_to_ruid", 3}, {"_audit_token_to_rgid", 4}, {"_audit_token_to_asid", 6},
        {"_audit_token_to_pidversion", 7}}};
    for (const auto& [name, field] : token_fields)
        fn(libbsm, name, [field](CpuState& s, GuestMemory& m) -> Address { return m.read32(s.x[0] + field * 4ULL); });

    // ---- threads and queues ----
    // QoS is a scheduling hint the host scheduler does not take.
    fn(libsystem, "_pthread_get_qos_class_np", [](CpuState& s, GuestMemory& m) -> Address {
        if (s.x[1]) m.write32(s.x[1], 0x15);   // QOS_CLASS_DEFAULT
        if (s.x[2]) m.write32(s.x[2], 0);
        return 0;
    });
    fn(libsystem, "_pthread_override_qos_class_start_np", [this](CpuState& s, GuestMemory&) -> Address {
        const auto qos = static_cast<std::uint32_t>(s.x[1]);
        if (qos != 0x21 && qos != 0x19 && qos != 0x15 && qos != 0x11 && qos != 0x09) { set_errno(einval); return 0; }
        return process.allocate_guest(16);   // pthread_override_t
    });
    fn(libsystem, "_pthread_override_qos_class_end_np", [this](CpuState& s, GuestMemory&) -> Address {
        if (!s.x[0]) return einval;
        process.release_guest(s.x[0]);
        return 0;
    });
    fn(libsystem, "_dispatch_queue_create_with_target$V2", [this](CpuState& s, GuestMemory&) {
        const auto queue = call(libsystem, "_dispatch_queue_create", {s.x[0], s.x[1]});
        if (queue && s.x[2]) static_cast<void>(call(libsystem, "_dispatch_set_target_queue", {queue, s.x[2]}));
        return queue;
    });
}

} // namespace

void install_libsystem_leftovers(InstallContext& context) {
    alchemy::compression::install_bzip2_stream(context);   // libbz2 streams (darwin/compression)
    std::make_shared<Leftovers>(context)->install();
}

} // namespace alchemy::gui::fill
