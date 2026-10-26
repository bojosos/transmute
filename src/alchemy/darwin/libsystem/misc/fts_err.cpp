#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <atomic>
#include <map>
#include <memory>

// libSystem interfaces QLab's images import (cut from the CoreFoundation
// extras, darwin/corefoundation/cf_file_descriptors.cpp, which still installs
// them at the same point through install_fts_err):
//   _c_locale                  the C locale object (its address is a locale_t
//                              the locale provider accepts)
//   fts_open/fts_read/fts_children/fts_set/fts_close  file hierarchy traversal
//                              over the guest's stat/lstat/opendir (below)
//   warn, warnx, vwarn, vwarnx, err, errx, verr, verrx  "<progname>: message[: strerror(errno)]"
//                              on standard error (err* then exit with the status)
//   warnc, vwarnc, errc, verrc the same with an explicit error code
//   err_set_file, err_set_exit where the messages go (a FILE*; NULL is standard
//                              error) and a function err* call with the status
//                              before exiting
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

// ---- fts ----
// FTSENT (Darwin, 64-bit inodes): fts_cycle 0, fts_parent 8, fts_link 16,
// fts_number 24, fts_pointer 32, fts_accpath 40, fts_path 48, fts_errno 56,
// fts_symfd 60, fts_pathlen 64, fts_namelen 66, fts_ino 72, fts_dev 80,
// fts_nlink 84, fts_level 86, fts_info 88, fts_flags 90, fts_instr 92,
// fts_statp 96, fts_name 104.
constexpr std::uint16_t fts_d = 1, fts_dc = 2, fts_dnr = 4, fts_dot = 5, fts_dp = 6, fts_err = 7, fts_f = 8, fts_ns = 10, fts_nsok = 11,
                        fts_sl = 12, fts_slnone = 13;
constexpr std::uint64_t fts_comfollow = 0x001, fts_logical = 0x002, fts_nostat = 0x008, fts_seedot = 0x020, fts_xdev = 0x040;
constexpr std::uint16_t fts_again = 1, fts_follow = 2, fts_skip = 4;

struct Node {
    Address entry{};             // the guest FTSENT
    std::string path, name;
    std::int16_t level{};
    bool directory{}, visited{};
    std::vector<std::size_t> children;   // node indexes
    std::size_t next_child{};
    std::size_t parent{SIZE_MAX};
    std::uint64_t dev{}, ino{};
};

struct Tree {
    std::uint64_t options{};
    Address compar{};
    std::vector<Node> nodes;
    std::vector<std::size_t> roots;
    std::size_t next_root{};
    std::vector<std::size_t> stack;      // directories entered (pre-order returned), innermost last
    std::optional<std::size_t> last;     // the node fts_read returned last
    std::vector<Address> allocations;    // guest memory freed by fts_close
    Address root_parent{};
};

struct Fts {
    explicit Fts(Support& value) : support(value) {}
    Support& support;
    std::mutex mutex;
    std::map<Address, std::shared_ptr<Tree>> trees;

    std::int64_t sys(std::string_view symbol, std::initializer_list<Address> arguments) {
        return static_cast<std::int64_t>(support.call(libsystem, symbol, arguments));
    }

    Address guest_alloc(Tree& tree, std::size_t size) {
        const auto address = support.process.allocate_guest(size ? size : 1, 16);
        support.memory().fill_bytes(address, 0, size ? size : 1);
        tree.allocations.push_back(address);
        return address;
    }
    Address guest_string(Tree& tree, const std::string& text) {
        const auto address = guest_alloc(tree, text.size() + 1);
        if (!text.empty()) support.memory().write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        return address;
    }
    // Fills a node's FTSENT: stat (or lstat), type, path strings.
    void describe(Tree& tree, std::size_t index, bool follow) {
        auto& node = tree.nodes[index];
        auto& m = support.memory();
        const auto entry = node.entry = guest_alloc(tree, 104 + node.name.size() + 1);
        const auto path = guest_string(tree, node.path);
        m.write64(entry + 40, path);   // fts_accpath: the full path (no chdir)
        m.write64(entry + 48, path);
        m.write32(entry + 64, static_cast<std::uint32_t>(std::min<std::size_t>(node.path.size(), 0xffff)) |
                              (static_cast<std::uint32_t>(std::min<std::size_t>(node.name.size(), 0xffff)) << 16));
        m.write32(entry + 60, static_cast<std::uint32_t>(-1));
        if (!node.name.empty()) m.write_bytes(entry + 104, std::span(reinterpret_cast<const std::uint8_t*>(node.name.data()), node.name.size()));
        m.write64(entry + 8, node.parent == SIZE_MAX ? tree.root_parent : tree.nodes[node.parent].entry);
        std::uint16_t info;
        std::uint32_t error{};
        if (tree.options & fts_nostat) {
            info = node.directory ? fts_d : fts_nsok;
        } else {
            const auto stat_buffer = guest_alloc(tree, 144);
            m.write64(entry + 96, stat_buffer);
            GuestBlock text_path(support, node.path);
            bool ok = sys(follow ? "_stat" : "_lstat", {text_path, stat_buffer}) == 0;
            if (!ok && follow) {   // a dangling link: describe the link itself
                ok = sys("_lstat", {text_path, stat_buffer}) == 0;
                if (ok) follow = false;
                else error = guest_errno(support);
            } else if (!ok) error = guest_errno(support);
            if (!ok) info = fts_ns;
            else {
                const auto mode = m.read32(stat_buffer + 4) & 0xf000;
                node.dev = m.read32(stat_buffer);
                node.ino = m.read64(stat_buffer + 8);
                m.write64(entry + 72, node.ino);
                m.write32(entry + 80, static_cast<std::uint32_t>(node.dev));
                m.write32(entry + 84, (m.read32(stat_buffer + 4) >> 16) | (m.read32(entry + 84) & 0xffff0000u));
                node.directory = mode == 0x4000;
                if (mode == 0xa000) info = follow ? fts_slnone : fts_sl;
                else if (node.directory) info = fts_d;
                else info = fts_f;
                if (node.directory) {   // a directory that is its own ancestor (logical walks)
                    for (auto parent = node.parent; parent != SIZE_MAX; parent = tree.nodes[parent].parent)
                        if (tree.nodes[parent].dev == node.dev && tree.nodes[parent].ino == node.ino) {
                            info = fts_dc;
                            m.write64(entry, tree.nodes[parent].entry);
                            break;
                        }
                }
            }
        }
        if (info != fts_d && info != fts_dc) node.directory = false;
        m.write32(entry + 56, error);
        m.write32(entry + 84, (m.read32(entry + 84) & 0xffffu) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(node.level)) << 16));
        m.write32(entry + 88, info);   // fts_info; fts_flags = 0
        m.write32(entry + 92, 0);      // fts_instr
    }
    std::optional<std::vector<std::string>> list(const std::string& path, std::uint32_t& error) {
        GuestBlock text_path(support, path);
        const auto directory = support.call(libsystem, "_opendir", {text_path});
        if (!directory) { error = guest_errno(support); return std::nullopt; }
        std::vector<std::string> names;
        for (;;) {
            const auto entry = support.call(libsystem, "_readdir", {directory});
            if (!entry) break;
            const auto length = support.memory().read32(entry + 16) >> 16;
            std::string name(length, '\0');
            if (length) support.memory().read_bytes(entry + 21, std::span(reinterpret_cast<std::uint8_t*>(name.data()), name.size()));
            names.push_back(std::move(name));
        }
        static_cast<void>(sys("_closedir", {directory}));
        return names;
    }
    std::string join(const std::string& directory, const std::string& name) {
        return !directory.empty() && directory.back() == '/' ? directory + name : directory + "/" + name;
    }
    // Reads a directory's children into nodes (sorted with the comparison function when given).
    std::uint16_t read_children(Tree& tree, std::size_t index) {
        std::uint32_t error{};
        const auto names = list(tree.nodes[index].path, error);
        if (!names) { support.memory().write32(tree.nodes[index].entry + 56, error); return fts_dnr; }
        std::vector<std::size_t> children;
        const bool logical = tree.options & fts_logical;
        for (const auto& name : *names) {
            if ((name == "." || name == "..") && !(tree.options & fts_seedot)) continue;
            Node node;
            node.name = name;
            node.path = join(tree.nodes[index].path, name);
            node.level = static_cast<std::int16_t>(tree.nodes[index].level + 1);
            node.parent = index;
            tree.nodes.push_back(std::move(node));
            const auto child = tree.nodes.size() - 1;
            describe(tree, child, logical);
            if (name == "." || name == "..") { tree.nodes[child].directory = false; support.memory().write32(tree.nodes[child].entry + 88, fts_dot); }
            if ((tree.options & fts_xdev) && tree.nodes[child].directory && tree.nodes[child].dev != tree.nodes[index].dev) tree.nodes[child].directory = false;
            children.push_back(child);
        }
        sort(tree, children);
        auto& m = support.memory();
        for (std::size_t i = 0; i + 1 < children.size(); ++i) m.write64(tree.nodes[children[i]].entry + 16, tree.nodes[children[i + 1]].entry);
        tree.nodes[index].children = std::move(children);
        tree.nodes[index].next_child = 0;
        return fts_d;
    }
    void sort(Tree& tree, std::vector<std::size_t>& list) {
        if (!tree.compar || list.size() < 2) return;
        // compar(const FTSENT **, const FTSENT **)
        GuestBlock cells(support, 16);
        std::stable_sort(list.begin(), list.end(), [&](std::size_t a, std::size_t b) {
            support.memory().write64(cells, tree.nodes[a].entry);
            support.memory().write64(cells + 8, tree.nodes[b].entry);
            return static_cast<std::int32_t>(call_address(support, tree.compar, {cells, cells + 8})) < 0;
        });
    }
    std::uint16_t info_of(const Tree& tree, std::size_t index) { return static_cast<std::uint16_t>(support.memory().read32(tree.nodes[index].entry + 88)); }
    std::uint16_t instruction_of(const Tree& tree, std::size_t index) { return static_cast<std::uint16_t>(support.memory().read32(tree.nodes[index].entry + 92)); }
    Address read(Tree& tree) {
        auto& m = support.memory();
        // Act on the instruction fts_set left on the last entry.
        if (tree.last) {
            const auto index = *tree.last;
            const auto instruction = instruction_of(tree, index);
            m.write32(tree.nodes[index].entry + 92, 0);
            if (instruction == fts_again) {
                const auto parent = tree.nodes[index].parent;
                describe(tree, index, (tree.options & fts_logical) || (parent == SIZE_MAX && (tree.options & fts_comfollow)));
                return tree.nodes[index].entry;
            }
            if (instruction == fts_follow && (info_of(tree, index) == fts_sl || info_of(tree, index) == fts_slnone)) {
                describe(tree, index, true);
                if (info_of(tree, index) == fts_d) tree.nodes[index].directory = true;
                return tree.nodes[index].entry;
            }
            if (info_of(tree, index) == fts_d && !tree.nodes[index].visited) {
                tree.nodes[index].visited = true;
                if (instruction == fts_skip) tree.nodes[index].directory = false;
                else {
                    const auto result = read_children(tree, index);
                    if (result == fts_dnr) {
                        m.write32(tree.nodes[index].entry + 88, fts_dnr);   // the directory again, unreadable
                        tree.last = index;
                        return tree.nodes[index].entry;
                    }
                    tree.stack.push_back(index);
                }
            }
        }
        for (;;) {
            if (!tree.stack.empty()) {
                auto& directory = tree.nodes[tree.stack.back()];
                if (directory.next_child < directory.children.size()) {
                    const auto child = directory.children[directory.next_child++];
                    tree.last = child;
                    return tree.nodes[child].entry;
                }
                const auto done = tree.stack.back();
                tree.stack.pop_back();
                m.write32(tree.nodes[done].entry + 88, fts_dp);   // post-order
                tree.last = std::nullopt;
                return tree.nodes[done].entry;
            }
            if (tree.next_root >= tree.roots.size()) return 0;
            const auto root = tree.roots[tree.next_root++];
            tree.last = root;
            return tree.nodes[root].entry;
        }
    }
};

std::shared_ptr<Fts> fts_instance;

} // namespace

void install_fts_err(Support& support) {
    auto self = std::make_shared<Fts>(support);
    fts_instance = self;

    // ---- _c_locale ----
    if (!support.process.resolve(libsystem, "__c_locale") && support.process.resolve(libsystem, "_newlocale")) {
        GuestBlock name(support, "C");
        const auto locale = support.call(libsystem, "_newlocale", {63, name, 0});   // LC_ALL_MASK
        if (!locale) fail("the C locale cannot be created");
        support.process.install_export(std::string(libsystem), "__c_locale", {locale, ExportKind::data});
    }

    // ---- fts ----
    // FTS *fts_open(char * const *argv, int options, int (*compar)(const FTSENT **, const FTSENT **))
    support.function(libsystem, "_fts_open", [self](CpuState& s, GuestMemory& m) {
        constexpr std::uint64_t valid_options = 0xfff;
        if (!s.x[0] || (s.x[1] & ~valid_options)) { self->support.process.set_errno(22); s.x[0] = 0; return; }
        auto tree = std::make_shared<Tree>();
        tree->options = s.x[1];
        tree->compar = s.x[2];
        tree->root_parent = self->guest_alloc(*tree, 112);
        m.write32(tree->root_parent + 84, static_cast<std::uint32_t>(static_cast<std::uint16_t>(-1)) << 16);   // fts_level -1
        for (Address at = s.x[0];; at += 8) {
            const auto pointer = m.read64(at);
            if (!pointer) break;
            Node node;
            node.path = c_string(m, pointer, 4096);
            if (node.path.empty()) { self->support.process.set_errno(2); s.x[0] = 0; return; }
            const auto slash = node.path.find_last_of('/');
            node.name = slash == std::string::npos || slash + 1 == node.path.size() ? node.path : node.path.substr(slash + 1);
            node.level = 0;
            tree->nodes.push_back(std::move(node));
            const auto index = tree->nodes.size() - 1;
            self->describe(*tree, index, (tree->options & (fts_logical | fts_comfollow)) != 0);
            tree->roots.push_back(index);
        }
        self->sort(*tree, tree->roots);
        const auto handle = self->guest_alloc(*tree, 64);
        { std::lock_guard lock(self->mutex); self->trees[handle] = tree; }
        s.x[0] = handle;
    });
    const auto tree_of = [self](Address handle) {
        std::lock_guard lock(self->mutex);
        const auto found = self->trees.find(handle);
        if (found == self->trees.end()) fail("fts call with an unknown FTS handle");
        return found->second;
    };
    support.function(libsystem, "_fts_read", [self, tree_of](CpuState& s, GuestMemory&) {
        const auto tree = tree_of(s.x[0]);
        self->support.process.set_errno(0);
        s.x[0] = self->read(*tree);
    });
    // FTSENT *fts_children(FTS *, int options): the current directory's children (a linked list).
    support.function(libsystem, "_fts_children", [self, tree_of](CpuState& s, GuestMemory&) {
        const auto tree = tree_of(s.x[0]);
        if (!tree->last || self->info_of(*tree, *tree->last) != fts_d) { self->support.process.set_errno(0); s.x[0] = 0; return; }
        const auto index = *tree->last;
        if (!tree->nodes[index].visited) {
            tree->nodes[index].visited = true;
            if (self->read_children(*tree, index) == fts_dnr) { s.x[0] = 0; return; }
            tree->stack.push_back(index);
            tree->last = std::nullopt;   // the next fts_read returns the first child
        }
        const auto& children = tree->nodes[index].children;
        s.x[0] = children.empty() ? 0 : tree->nodes[children.front()].entry;
    });
    support.function(libsystem, "_fts_set", [self, tree_of](CpuState& s, GuestMemory& m) {
        static_cast<void>(tree_of(s.x[0]));
        const auto instruction = static_cast<std::uint16_t>(s.x[2]);
        if (!s.x[1] || (instruction != 0 && instruction != fts_again && instruction != fts_follow && instruction != 3 && instruction != fts_skip)) {
            self->support.process.set_errno(22);
            s.x[0] = static_cast<Address>(-1);
            return;
        }
        m.write32(s.x[1] + 92, instruction == 3 ? 0 : instruction);   // FTS_NOINSTR clears
        s.x[0] = 0;
    });
    support.function(libsystem, "_fts_close", [self](CpuState& s, GuestMemory&) {
        std::shared_ptr<Tree> tree;
        {
            std::lock_guard lock(self->mutex);
            const auto found = self->trees.find(s.x[0]);
            if (found == self->trees.end()) { self->support.process.set_errno(22); s.x[0] = static_cast<Address>(-1); return; }
            tree = found->second;
            self->trees.erase(found);
        }
        for (const auto address : tree->allocations) self->support.process.release_guest(address);
        s.x[0] = 0;
    });

    // ---- warn / err ----
    // "<progname>: <message>[: <strerror(errno)>]\n" on standard error (or err_set_file's stream).
    struct ErrHooks {
        std::atomic<Address> file{}, exit_callback{};
    };
    const auto hooks = std::make_shared<ErrHooks>();
    support.function(libsystem, "_err_set_file", [hooks](CpuState& s, GuestMemory&) { hooks->file = s.x[0]; });
    support.function(libsystem, "_err_set_exit", [hooks](CpuState& s, GuestMemory&) { hooks->exit_callback = s.x[0]; });
    const auto report = [&support, hooks](Address format, Address arguments, bool with_errno, std::uint32_t code) {
        std::string line;
        if (const auto progname = support.process.resolve(libsystem, "___progname")) {
            const auto name = support.memory().read64(progname->address);
            if (name) line = c_string(support.memory(), name, 1024) + ": ";
        }
        if (format) {
            GuestBlock buffer(support, 4096);
            static_cast<void>(support.call(libsystem, "_vsnprintf", {buffer, 4096, format, arguments}));
            line += c_string(support.memory(), buffer, 4096);
            if (with_errno) line += ": ";
        }
        if (with_errno) line += c_string(support.memory(), support.call(libsystem, "_strerror", {code}), 1024);
        line += "\n";
        GuestBlock text(support, line);
        if (const auto file = hooks->file.load()) static_cast<void>(support.call(libsystem, "_fputs", {text, file}));
        else static_cast<void>(support.call(libsystem, "_write", {2, text, line.size()}));
    };
    // warn(fmt, ...) / vwarn(fmt, va_list) / err(status, fmt, ...) / verr(status, fmt, va_list)
    support.function(libsystem, "_warn", [&support, report](CpuState& s, GuestMemory&) { report(s.x[0], s.sp, true, guest_errno(support)); });
    support.function(libsystem, "_warnx", [report](CpuState& s, GuestMemory&) { report(s.x[0], s.sp, false, 0); });
    support.function(libsystem, "_vwarn", [&support, report](CpuState& s, GuestMemory&) { report(s.x[0], s.x[1], true, guest_errno(support)); });
    support.function(libsystem, "_vwarnx", [report](CpuState& s, GuestMemory&) { report(s.x[0], s.x[1], false, 0); });
    const auto leave = [&support, hooks](CpuState& s, Address status) {
        // err/errx never return: err_set_exit's function, then continue into exit(status).
        const auto exit_function = support.process.resolve(libsystem, "_exit");
        if (!exit_function) fail("err: exit is not provided");
        if (const auto callback = hooks->exit_callback.load())
            static_cast<void>(call_address(support, callback, {static_cast<std::uint32_t>(status)}));
        s.x[0] = status;
        s.x[30] = exit_function->address;
    };
    support.function(libsystem, "_err", [&support, report, leave](CpuState& s, GuestMemory&) { report(s.x[1], s.sp, true, guest_errno(support)); leave(s, s.x[0]); });
    support.function(libsystem, "_errx", [report, leave](CpuState& s, GuestMemory&) { report(s.x[1], s.sp, false, 0); leave(s, s.x[0]); });
    support.function(libsystem, "_verr", [&support, report, leave](CpuState& s, GuestMemory&) { report(s.x[1], s.x[2], true, guest_errno(support)); leave(s, s.x[0]); });
    support.function(libsystem, "_verrx", [report, leave](CpuState& s, GuestMemory&) { report(s.x[1], s.x[2], false, 0); leave(s, s.x[0]); });
    // errc(status, code, fmt, ...) / verrc(status, code, fmt, va_list) / warnc(code, fmt, ...) / vwarnc(code, fmt, va_list)
    const auto code = [](Address value) { return static_cast<std::uint32_t>(value); };
    support.function(libsystem, "_errc", [report, leave, code](CpuState& s, GuestMemory&) { report(s.x[2], s.sp, true, code(s.x[1])); leave(s, s.x[0]); });
    support.function(libsystem, "_verrc", [report, leave, code](CpuState& s, GuestMemory&) { report(s.x[2], s.x[3], true, code(s.x[1])); leave(s, s.x[0]); });
    support.function(libsystem, "_warnc", [report, code](CpuState& s, GuestMemory&) { report(s.x[1], s.sp, true, code(s.x[0])); });
    support.function(libsystem, "_vwarnc", [report, code](CpuState& s, GuestMemory&) { report(s.x[1], s.x[2], true, code(s.x[0])); });
}

} // namespace alchemy::gui::completion_kit
