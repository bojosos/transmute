#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

// NSData file reading and writing (QLab saves archives and settings), added
// when NSData lacks them. Files go through the guest's libSystem (open, read,
// write, rename), so paths resolve in the root as for the application.
//   Writing: -writeToFile:atomically:, -writeToURL:atomically:,
//   -writeToFile:options:error:, -writeToURL:options:error: (NSDataWritingAtomic
//   1: a temporary file renamed over the target; NSDataWritingWithoutOverwriting
//   2: O_EXCL). Reading: +dataWithContentsOfFile:[options:error:],
//   +dataWithContentsOfURL:[options:error:] and the -initWithContentsOf...
//   forms (an initializer returns a new object, as class clusters do).
// URLs must be file URLs (network URLs fail explicitly). Errors are
// NSCocoaErrorDomain: reading 260 (no such file), 257 (no permission), 256;
// writing 4 (no such directory), 513 (no permission), 516 (exists), 512.
namespace alchemy::gui::completion_kit {
namespace {

constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
constexpr Address o_rdonly = 0, o_wronly = 1, o_creat = 0x200, o_trunc = 0x400, o_excl = 0x800;
constexpr int e_noent = 2, e_acces = 13, e_exist = 17, e_perm = 1;

struct Files {
    Support& s;

    std::int64_t signed_result(Address value) { return static_cast<std::int64_t>(value); }
    int errno_value() { const auto at = s.call(libsystem, "___error", {}); return at ? static_cast<int>(s.memory().read32(at)) : 0; }
    Address guest_text(const std::string& text) {
        const auto block = s.process.allocate_guest(text.size() + 1);
        s.memory().write_bytes(block, std::span(reinterpret_cast<const std::uint8_t*>(text.c_str()), text.size() + 1));
        return block;
    }
    std::optional<std::string> path_of(Address object) {
        if (!object) return std::nullopt;
        if (s.cf.kind(object) == ObjectKind::string) return s.cf.utf8(object);
        if (s.cf.kind(object) == ObjectKind::url) {
            const auto scheme = s.send_id(object, "scheme");
            if (scheme && s.cf.utf8(scheme) != "file") unsupported("NSData with a non-file URL (" + s.cf.utf8(scheme) + ")");
            return s.cf.url_path(object);
        }
        if (s.objc.responds(object, s.objc.selector("isFileURL"))) {
            if (!(s.send(object, "isFileURL").x0 & 0xff)) unsupported("NSData with a non-file URL");
            return s.cf.utf8(s.send_id(object, "path"));
        }
        fail("NSData: a path that is neither a string nor a URL");
    }
    Address error(std::int64_t code, const std::string& path) {
        const auto error_class = s.objc.find_class("NSError");
        if (!error_class) return 0;
        std::vector<std::pair<Address, Address>> entries{{s.cf.constant_string("NSFilePath"), s.send_id(s.cf.string(path), "autorelease")}};
        const auto info = s.send_id(s.cf.dictionary(entries), "autorelease");
        CallRegs registers;
        registers.x[2] = s.cf.constant_string("NSCocoaErrorDomain");
        registers.x[3] = static_cast<Address>(code);
        registers.x[4] = info;
        return s.send(error_class, "errorWithDomain:code:userInfo:", registers).x0;
    }
    // The file's bytes, or the errno.
    std::optional<std::vector<std::uint8_t>> read(const std::string& path, int& error_number) {
        const auto text = guest_text(path);
        const auto fd = signed_result(s.call(libsystem, "_open", {text, o_rdonly}));
        s.process.release_guest(text);
        if (fd < 0) { error_number = errno_value(); return std::nullopt; }
        std::vector<std::uint8_t> bytes;
        const auto chunk = s.process.allocate_guest(65536);
        try {
            for (;;) {
                const auto got = signed_result(s.call(libsystem, "_read", {static_cast<Address>(fd), chunk, 65536}));
                if (got < 0) { error_number = errno_value(); bytes.clear(); s.process.release_guest(chunk); s.call(libsystem, "_close", {static_cast<Address>(fd)}); return std::nullopt; }
                if (got == 0) break;
                const auto at = bytes.size();
                bytes.resize(at + static_cast<std::size_t>(got));
                s.memory().read_bytes(chunk, std::span(bytes.data() + at, static_cast<std::size_t>(got)));
            }
        } catch (...) { s.process.release_guest(chunk); s.call(libsystem, "_close", {static_cast<Address>(fd)}); throw; }
        s.process.release_guest(chunk);
        s.call(libsystem, "_close", {static_cast<Address>(fd)});
        return bytes;
    }
    // Writes the data object's bytes; 0 or the errno.
    int write(Address data, const std::string& path, bool atomic, bool exclusive) {
        const auto target = atomic ? path + ".alchemy-" + std::to_string(s.call(libsystem, "_getpid", {})) + ".tmp" : path;
        if (atomic && exclusive) {
            // Without overwriting: fail early when the target exists.
            const auto check = guest_text(path);
            const auto st = s.process.allocate_guest(144);
            const auto exists = signed_result(s.call(libsystem, "_stat", {check, st})) == 0;
            s.process.release_guest(st);
            s.process.release_guest(check);
            if (exists) return e_exist;
        }
        const auto text = guest_text(target);
        const auto flags = o_wronly | o_creat | (exclusive || atomic ? o_excl : o_trunc);
        // open(path, flags, mode): the mode is variadic (on the stack on Darwin arm64).
        CallRegs open_call;
        open_call.x[0] = text;
        open_call.x[1] = flags;
        open_call.push_variadic(std::uint64_t{0644});
        const auto fd = signed_result(s.call(libsystem, "_open", open_call).x0);
        if (fd < 0) { const auto e = errno_value(); s.process.release_guest(text); return e; }
        const auto length = s.cf.length(data);
        const auto bytes = length ? s.send(data, "bytes").x0 : 0;
        Address written = 0;
        int failure = 0;
        while (written < length) {
            const auto put = signed_result(s.call(libsystem, "_write", {static_cast<Address>(fd), bytes + written, length - written}));
            if (put <= 0) { failure = errno_value(); break; }
            written += static_cast<Address>(put);
        }
        s.call(libsystem, "_close", {static_cast<Address>(fd)});
        if (failure) { s.call(libsystem, "_unlink", {text}); s.process.release_guest(text); return failure; }
        if (atomic) {
            const auto final_path = guest_text(path);
            const auto renamed = signed_result(s.call(libsystem, "_rename", {text, final_path})) == 0;
            if (!renamed) { failure = errno_value(); s.call(libsystem, "_unlink", {text}); }
            s.process.release_guest(final_path);
        }
        s.process.release_guest(text);
        return failure;
    }
    Address make_data(const std::vector<std::uint8_t>& bytes, Address cls) {
        const auto block = s.process.allocate_guest(std::max<std::size_t>(bytes.size(), 1));
        if (!bytes.empty()) s.memory().write_bytes(block, bytes);
        CallRegs make;
        make.x[2] = block;
        make.x[3] = bytes.size();
        Address result{};
        try { result = s.send(cls, "dataWithBytes:length:", make).x0; } catch (...) { s.process.release_guest(block); throw; }
        s.process.release_guest(block);
        return result;   // autoreleased
    }
};

int read_error_code(int e) { return e == e_noent ? 260 : (e == e_acces || e == e_perm) ? 257 : 256; }
int write_error_code(int e) { return e == e_noent ? 4 : (e == e_acces || e == e_perm) ? 513 : e == e_exist ? 516 : 512; }

} // namespace

void install_data_files(Support& support) {
    const auto data_class = support.objc.find_class("NSData");
    if (!data_class) return;
    auto files = std::make_shared<Files>(Files{support});
    auto* sp = &support;
    const auto has = [sp, data_class](const char* selector, bool class_method) {
        const auto target = class_method ? sp->process.main_engine().read64(data_class) : data_class;   // metaclass for class methods
        return sp->call("/usr/lib/libobjc.A.dylib", "_class_getInstanceMethod", {target, sp->objc.selector(selector)}) != 0;
    };
    // Writing.
    const auto write_with = [files](Address self, Address destination, bool atomic, bool exclusive, Address error_out) {
        const auto path = files->path_of(destination);
        if (!path) return false;
        const auto e = files->write(self, *path, atomic, exclusive);
        if (e && error_out) files->s.memory().write64(error_out, files->error(write_error_code(e), *path));
        return e == 0;
    };
    for (const auto* name : {"writeToFile:atomically:", "writeToURL:atomically:"})
        if (!has(name, false))
            support.method(data_class, name, [write_with](CpuState& s, GuestMemory&) { s.x[0] = write_with(s.x[0], s.x[2], (s.x[3] & 0xff) != 0, false, 0); }, "B@:@B");
    for (const auto* name : {"writeToFile:options:error:", "writeToURL:options:error:"})
        if (!has(name, false))
            support.method(data_class, name, [write_with](CpuState& s, GuestMemory& m) {
                if (s.x[4]) m.write64(s.x[4], 0);
                s.x[0] = write_with(s.x[0], s.x[2], (s.x[3] & 1) != 0, (s.x[3] & 2) != 0, s.x[4]);
            }, "B@:@Q^@");
    // Reading: class factories return autoreleased data of the receiver class.
    const auto read_into = [files](Address cls, Address source, Address error_out) -> Address {
        const auto path = files->path_of(source);
        if (!path) return 0;
        int e = 0;
        const auto bytes = files->read(*path, e);
        if (!bytes) {
            if (error_out) files->s.memory().write64(error_out, files->error(read_error_code(e), *path));
            return 0;
        }
        return files->make_data(*bytes, cls);
    };
    for (const auto* name : {"dataWithContentsOfFile:", "dataWithContentsOfURL:"})
        if (!has(name, true))
            support.method(data_class, name, [read_into](CpuState& s, GuestMemory&) { s.x[0] = read_into(s.x[0], s.x[2], 0); }, "@@:@", true);
    for (const auto* name : {"dataWithContentsOfFile:options:error:", "dataWithContentsOfURL:options:error:"})
        if (!has(name, true))
            support.method(data_class, name, [read_into](CpuState& s, GuestMemory& m) {
                if (s.x[4]) m.write64(s.x[4], 0);
                s.x[0] = read_into(s.x[0], s.x[2], s.x[4]);
            }, "@@:@Q^@", true);
    // Initializers: the receiver is replaced by a new data object.
    const auto init_from = [sp, read_into](Address self, Address source, Address error_out) -> Address {
        const auto cls = sp->objc.object_class(self);
        const auto made = read_into(cls, source, error_out);
        sp->send_id(self, "release");
        return made ? sp->send_id(made, "retain") : 0;
    };
    for (const auto* name : {"initWithContentsOfFile:", "initWithContentsOfURL:"})
        if (!has(name, false))
            support.method(data_class, name, [init_from](CpuState& s, GuestMemory&) { s.x[0] = init_from(s.x[0], s.x[2], 0); }, "@@:@");
    for (const auto* name : {"initWithContentsOfFile:options:error:", "initWithContentsOfURL:options:error:"})
        if (!has(name, false))
            support.method(data_class, name, [init_from](CpuState& s, GuestMemory& m) {
                if (s.x[4]) m.write64(s.x[4], 0);
                s.x[0] = init_from(s.x[0], s.x[2], s.x[4]);
            }, "@@:@Q^@");
}

} // namespace alchemy::gui::completion_kit
