#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <algorithm>

// NSFileManager additions and NSDirectoryEnumerator, over the guest's own
// libSystem (stat, opendir/readdir, symlink, rename, read): the guest's view
// of the file system, as Foundation's.
//   -contentsAtPath:, -contentsEqualAtPath:andPath: (files byte for byte,
//   directories recursively by names and contents, symbolic links by target),
//   -contentsOfDirectoryAtURL:includingPropertiesForKeys:options:error:,
//   -subpathsOfDirectoryAtPath:error:, -enumeratorAtPath:,
//   -enumeratorAtURL:includingPropertiesForKeys:options:errorHandler:
//   (pre-order; skipsSubdirectoryDescendants, skipsPackageDescendants,
//   skipsHiddenFiles, producesRelativePathURLs; -skipDescendants, -level,
//   -fileAttributes, -directoryAttributes),
//   -createSymbolicLinkAtPath:withDestinationPath:error:,
//   -URLForDirectory:inDomain:appropriateForURL:create:error: (including
//   NSItemReplacementDirectory, a fresh directory under the temporary folder),
//   -getRelationship:ofDirectory:inDomain:toItemAtURL:error: and
//   -getRelationship:ofDirectoryAtURL:toItemAtURL:error: (by standardized
//   paths), -replaceItemAtURL:withItemAtURL:backupItemName:options:resultingItemURL:error:
//   (rename(2) into place, optionally keeping the backup).
// Errors are NSCocoaErrorDomain errors with NSUnderlyingError POSIX codes
// where Foundation reports them (260 no such file, 513 no permission, 512
// other, 516 file exists).
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

constexpr std::uint32_t enoent = 2, eacces = 13, eexist = 17, eperm = 1, enotdir = 20;
constexpr std::uint64_t skips_subdirectory_descendants = 1, skips_package_descendants = 2, skips_hidden_files = 4,
                        produces_relative_path_urls = 1 << 4;

struct Entry { std::string name; bool directory{}, link{}; };

struct EnumeratorState final : ObjectPayload {
    std::mutex mutex;
    std::string root;                        // the enumerated directory
    bool urls{}, relative_urls{};
    std::uint64_t options{};
    Address error_handler{};                 // copied block
    Address base_url{};                      // +1 (relative URLs)
    // Directories being walked: their relative path and remaining entries.
    struct Level { std::string relative; std::vector<Entry> entries; std::size_t next{}; };
    std::vector<Level> stack;
    std::string last;                        // relative path of the last object returned
    bool last_directory{}, skip{};
};

struct Files {
    explicit Files(Support& value) : support(value) {}
    Support& support;
    Address enumerator_class{};

    std::int64_t sys(std::string_view symbol, std::initializer_list<Address> arguments) {
        return static_cast<std::int64_t>(support.call(libsystem, symbol, arguments));
    }
    std::string join(const std::string& directory, const std::string& name) {
        if (directory.empty()) return name;
        return directory.back() == '/' ? directory + name : directory + "/" + name;
    }
    // stat/lstat: {mode} or errno.
    std::optional<std::uint32_t> mode(const std::string& path, bool follow, std::uint32_t& code) {
        GuestBlock text_path(support, path), buffer(support, 144);
        if (sys(follow ? "_stat" : "_lstat", {text_path, buffer}) != 0) { code = guest_errno(support); return std::nullopt; }
        return support.memory().read32(buffer + 4) & 0xffff;
    }
    static bool is_directory(std::uint32_t value) { return (value & 0xf000) == 0x4000; }
    static bool is_link(std::uint32_t value) { return (value & 0xf000) == 0xa000; }
    // The entries of a directory (without . and ..), sorted; nullopt with errno.
    std::optional<std::vector<Entry>> list(const std::string& path, std::uint32_t& code) {
        GuestBlock text_path(support, path);
        const auto directory = support.call(libsystem, "_opendir", {text_path});
        if (!directory) { code = guest_errno(support); return std::nullopt; }
        std::vector<Entry> entries;
        for (;;) {
            const auto entry = support.call(libsystem, "_readdir", {directory});
            if (!entry) break;
            const auto name_length = support.memory().read32(entry + 16) >> 16;   // d_namlen
            std::string name(name_length, '\0');
            if (name_length) support.memory().read_bytes(entry + 21, std::span(reinterpret_cast<std::uint8_t*>(name.data()), name.size()));
            if (name == "." || name == "..") continue;
            const auto type = support.memory().read8(entry + 20);
            Entry value{name, type == 4, type == 10};
            if (type == 0) {   // DT_UNKNOWN: stat it
                std::uint32_t ignored{};
                if (const auto found = mode(join(path, name), false, ignored)) { value.directory = is_directory(*found); value.link = is_link(*found); }
            }
            entries.push_back(std::move(value));
        }
        static_cast<void>(sys("_closedir", {directory}));
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
        return entries;
    }
    std::optional<std::vector<std::uint8_t>> contents(const std::string& path) {
        GuestBlock text_path(support, path);
        const auto fd = sys("_open", {text_path, 0x1000000, 0});
        if (fd < 0) return std::nullopt;
        std::vector<std::uint8_t> bytes;
        GuestBlock buffer(support, 65536, false);
        for (;;) {
            const auto count = sys("_read", {static_cast<Address>(fd), buffer, 65536});
            if (count < 0) { static_cast<void>(sys("_close", {static_cast<Address>(fd)})); return std::nullopt; }
            if (count == 0) break;
            const auto start = bytes.size();
            bytes.resize(start + static_cast<std::size_t>(count));
            support.memory().read_bytes(buffer, std::span(bytes.data() + start, static_cast<std::size_t>(count)));
        }
        static_cast<void>(sys("_close", {static_cast<Address>(fd)}));
        return bytes;
    }
    std::optional<std::string> link_target(const std::string& path) {
        GuestBlock text_path(support, path), buffer(support, 4096);
        const auto count = sys("_readlink", {text_path, buffer, 4095});
        if (count < 0) return std::nullopt;
        std::string target(static_cast<std::size_t>(count), '\0');
        if (count) support.memory().read_bytes(buffer, std::span(reinterpret_cast<std::uint8_t*>(target.data()), target.size()));
        return target;
    }
    bool equal(const std::string& left, const std::string& right, int depth = 0) {
        if (depth > 256) return false;
        std::uint32_t code{};
        const auto a = mode(left, false, code), b = mode(right, false, code);
        if (!a || !b || (*a & 0xf000) != (*b & 0xf000)) return false;
        if (is_link(*a)) return link_target(left) == link_target(right);
        if (is_directory(*a)) {
            const auto first = list(left, code), second = list(right, code);
            if (!first || !second || first->size() != second->size()) return false;
            for (std::size_t index = 0; index < first->size(); ++index) {
                if ((*first)[index].name != (*second)[index].name) return false;
                if (!equal(join(left, (*first)[index].name), join(right, (*second)[index].name), depth + 1)) return false;
            }
            return true;
        }
        const auto x = contents(left), y = contents(right);
        return x && y && *x == *y;
    }
    Address cocoa_error(std::uint32_t code, const std::string& path, bool writing = false) {
        const std::int64_t cocoa = code == enoent || code == enotdir ? (writing ? 4 : 260) : code == eacces || code == eperm ? (writing ? 513 : 257)
                                   : code == eexist ? 516 : (writing ? 512 : 256);
        const auto underlying = error(support, "NSPOSIXErrorDomain", code);
        return error(support, "NSCocoaErrorDomain", cocoa, {}, {{"NSFilePath", string(support, path)}, {"NSUnderlyingError", underlying}});
    }
    std::string path_of_url(Address url) { return url ? support.cf.url_path(url) : std::string{}; }
    Address url(const std::string& path, bool directory) { return autoreleased(support, support.cf.file_url(path, directory)); }

    // ---- enumeration ----
    Address make_enumerator(const std::string& root, bool urls, std::uint64_t options, Address handler, Address base_url) {
        auto state = std::make_shared<EnumeratorState>();
        state->root = root;
        state->urls = urls;
        state->options = options;
        state->relative_urls = urls && (options & produces_relative_path_urls);
        state->error_handler = block_copy(support, handler);
        state->base_url = objc_retain(support, base_url);
        std::uint32_t code{};
        if (auto entries = list(root, code)) state->stack.push_back({"", std::move(*entries), 0});
        else if (handler) {
            const auto keep = invoke_block(support, handler, {url(root, true), cocoa_error(code, root)});
            static_cast<void>(keep);
        }
        return autoreleased(support, make_object(support, enumerator_class, state));
    }
    bool package(const std::string& name) {
        for (const auto* extension : {".app", ".bundle", ".framework", ".plugin", ".kext", ".xpc", ".qlab5", ".qlab4"})
            if (name.size() > std::string_view(extension).size() && name.ends_with(extension)) return true;
        return false;
    }
    Address next(Address enumerator) {
        const auto state = support.objc.find_host_state<EnumeratorState>(enumerator);
        if (!state) fail("NSDirectoryEnumerator without its state");
        std::unique_lock lock(state->mutex);
        // Descend into the last directory returned unless skipped.
        if (state->last_directory && !state->skip) {
            const auto relative = state->last;
            const auto name = relative.substr(relative.find_last_of('/') + 1);
            const bool descend = !(state->options & skips_subdirectory_descendants) && !((state->options & skips_package_descendants) && package(name));
            if (descend) {
                std::uint32_t code{};
                lock.unlock();
                const auto full = join(state->root, relative);
                auto entries = list(full, code);
                bool proceed = true;
                if (!entries && state->error_handler) {
                    proceed = (invoke_block(support, state->error_handler, {url(full, true), cocoa_error(code, full)}) & 0xff) != 0;
                }
                lock.lock();
                if (!proceed) { state->stack.clear(); state->last_directory = false; return 0; }
                if (entries) state->stack.push_back({relative, std::move(*entries), 0});
            }
        }
        state->last_directory = false;
        state->skip = false;
        while (!state->stack.empty()) {
            auto& level = state->stack.back();
            if (level.next >= level.entries.size()) { state->stack.pop_back(); continue; }
            const auto entry = level.entries[level.next++];
            if ((state->options & skips_hidden_files) && entry.name.starts_with('.')) continue;
            state->last = join(level.relative, entry.name);
            state->last_directory = entry.directory && !entry.link;
            const auto relative = state->last;
            const bool directory = state->last_directory;
            lock.unlock();
            if (!state->urls) return string(support, relative);
            return url(join(state->root, relative), directory);
        }
        return 0;
    }
};

} // namespace

void install_files(Support& support) {
    auto self = std::make_shared<Files>(support);
    const auto manager = class_named(support, "NSFileManager");
    auto enumerator = support.objc.find_class("NSDirectoryEnumerator");
    if (!enumerator) {
        const auto base = support.objc.find_class("NSEnumerator") ? "NSEnumerator" : "NSObject";
        enumerator = support.declare_class("NSDirectoryEnumerator", base, foundation_library);
    }
    self->enumerator_class = enumerator;

    support.method(manager, "contentsAtPath:", [self](CpuState& s, GuestMemory&) {
        const auto bytes = s.x[2] ? self->contents(text(self->support, s.x[2])) : std::nullopt;
        if (!bytes) { s.x[0] = 0; return; }
        GuestBlock buffer(self->support, bytes->size(), false);
        if (!bytes->empty()) self->support.memory().write_bytes(buffer, *bytes);
        s.x[0] = autoreleased(self->support, self->support.cf.invoke("CFDataCreate", std::array<Address, 3>{0, buffer, bytes->size()}));
    }, "@@:@");
    support.method(manager, "contentsEqualAtPath:andPath:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] && s.x[3] && self->equal(text(self->support, s.x[2]), text(self->support, s.x[3]));
    }, "B@:@@");
    support.method(manager, "contentsOfDirectoryAtURL:includingPropertiesForKeys:options:error:", [self](CpuState& s, GuestMemory&) {
        const auto path = self->path_of_url(s.x[2]);
        std::uint32_t code{};
        const auto entries = self->list(path, code);
        if (!entries) { store(self->support, s.x[5], self->cocoa_error(code, path)); s.x[0] = 0; return; }
        std::vector<Address> values;
        for (const auto& entry : *entries) {
            if ((s.x[4] & skips_hidden_files) && entry.name.starts_with('.')) continue;
            values.push_back(self->url(self->join(path, entry.name), entry.directory && !entry.link));
        }
        s.x[0] = array(self->support, values);
    }, "@@:@@Q^@");
    support.method(manager, "subpathsOfDirectoryAtPath:error:", [self](CpuState& s, GuestMemory&) {
        const auto path = text(self->support, s.x[2]);
        std::uint32_t code{};
        if (!self->list(path, code)) { store(self->support, s.x[3], self->cocoa_error(code, path)); s.x[0] = 0; return; }
        const auto walker = self->make_enumerator(path, false, 0, 0, 0);
        std::vector<Address> values;
        for (auto value = self->next(walker); value; value = self->next(walker)) values.push_back(value);
        s.x[0] = array(self->support, values);
    }, "@@:@^@");
    support.method(manager, "subpathsAtPath:", [self](CpuState& s, GuestMemory&) {
        const auto path = text(self->support, s.x[2]);
        std::uint32_t code{};
        if (!self->list(path, code)) { s.x[0] = 0; return; }
        const auto walker = self->make_enumerator(path, false, 0, 0, 0);
        std::vector<Address> values;
        for (auto value = self->next(walker); value; value = self->next(walker)) values.push_back(value);
        s.x[0] = array(self->support, values);
    }, "@@:@");
    support.method(manager, "enumeratorAtPath:", [self](CpuState& s, GuestMemory&) {
        const auto path = text(self->support, s.x[2]);
        std::uint32_t code{};
        const auto found = self->mode(path, true, code);
        s.x[0] = found && Files::is_directory(*found) ? self->make_enumerator(path, false, 0, 0, 0) : 0;
    }, "@@:@");
    support.method(manager, "enumeratorAtURL:includingPropertiesForKeys:options:errorHandler:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] ? self->make_enumerator(self->path_of_url(s.x[2]), true, s.x[4], s.x[5], s.x[2]) : 0;
    }, "@@:@@Q@?");
    support.method(manager, "createSymbolicLinkAtPath:withDestinationPath:error:", [self](CpuState& s, GuestMemory&) {
        const auto path = text(self->support, s.x[2]), destination = text(self->support, s.x[3]);
        GuestBlock link_path(self->support, path), target(self->support, destination);
        const bool ok = self->sys("_symlink", {target, link_path}) == 0;
        if (!ok) store(self->support, s.x[4], self->cocoa_error(guest_errno(self->support), path, true));
        s.x[0] = ok;
    }, "B@:@@^@");
    support.method(manager, "createSymbolicLinkAtURL:withDestinationURL:error:", [self](CpuState& s, GuestMemory&) {
        const auto path = self->path_of_url(s.x[2]), destination = self->path_of_url(s.x[3]);
        GuestBlock link_path(self->support, path), target(self->support, destination);
        const bool ok = self->sys("_symlink", {target, link_path}) == 0;
        if (!ok) store(self->support, s.x[4], self->cocoa_error(guest_errno(self->support), path, true));
        s.x[0] = ok;
    }, "B@:@@^@");
    support.method(manager, "destinationOfSymbolicLinkAtPath:error:", [self](CpuState& s, GuestMemory&) {
        const auto path = text(self->support, s.x[2]);
        const auto target = self->link_target(path);
        if (!target) { store(self->support, s.x[3], self->cocoa_error(guest_errno(self->support), path)); s.x[0] = 0; return; }
        s.x[0] = string(self->support, *target);
    }, "@@:@^@");

    // NSSearchPathDirectory in a domain (via -URLsForDirectory:inDomains:).
    const auto directory_url = [self](Address receiver, std::uint64_t directory, std::uint64_t domain) -> Address {
        const auto list = self->support.send_id(receiver, "URLsForDirectory:inDomains:", {directory, domain});
        return list && self->support.send_id(list, "count") ? self->support.send_id(list, "firstObject") : 0;
    };
    support.method(manager, "URLForDirectory:inDomain:appropriateForURL:create:error:", [self, directory_url](CpuState& s, GuestMemory&) {
        constexpr std::uint64_t item_replacement = 99;
        std::string path;
        if (s.x[2] == item_replacement) {
            // A fresh directory for a safe-save replacement, under the temporary folder.
            const auto temporary = self->path_of_url(self->support.send_id(s.x[0], "temporaryDirectory"));
            const auto base = self->join(temporary, "TemporaryItems");
            GuestBlock base_text(self->support, base);
            static_cast<void>(self->sys("_mkdir", {base_text, 0700}));
            for (unsigned attempt = 0; attempt < 1000 && path.empty(); ++attempt) {
                const auto candidate = self->join(base, "(A Document Being Saved By Alchemy" + (attempt ? " " + std::to_string(attempt + 1) : std::string{}) + ")");
                GuestBlock candidate_text(self->support, candidate);
                if (self->sys("_mkdir", {candidate_text, 0700}) == 0) path = candidate;
                else if (guest_errno(self->support) != eexist) break;
            }
            if (path.empty()) { store(self->support, s.x[6], self->cocoa_error(guest_errno(self->support), base, true)); s.x[0] = 0; return; }
            s.x[0] = self->url(path, true);
            return;
        }
        const auto found = directory_url(s.x[0], s.x[2], s.x[3]);
        if (!found) {
            store(self->support, s.x[6], error(self->support, "NSCocoaErrorDomain", 4, "The requested directory is not available in this domain."));
            s.x[0] = 0;
            return;
        }
        if (s.x[5] & 0xff) {
            CallRegs create;
            create.x[2] = found;
            create.x[3] = 1;
            create.x[4] = 0;
            create.x[5] = s.x[6];
            if (!(self->support.send(s.x[0], "createDirectoryAtURL:withIntermediateDirectories:attributes:error:", create).x0 & 0xff)) { s.x[0] = 0; return; }
        }
        s.x[0] = found;
    }, "@@:QQ@B^@");
    // NSURLRelationship: Contains 0, Same 1, Other 2.
    const auto relationship = [self](const std::string& directory, const std::string& item) -> std::int64_t {
        auto normalize = [](std::string value) { while (value.size() > 1 && value.back() == '/') value.pop_back(); return value; };
        const auto a = normalize(directory), b = normalize(item);
        if (a == b) return 1;
        if (b.size() > a.size() && b.compare(0, a.size(), a) == 0 && (a == "/" || b[a.size()] == '/')) return 0;
        return 2;
    };
    support.method(manager, "getRelationship:ofDirectory:inDomain:toItemAtURL:error:", [self, directory_url, relationship](CpuState& s, GuestMemory& m) {
        const auto found = directory_url(s.x[0], s.x[3], s.x[4]);
        if (!found) {
            store(self->support, s.x[6], error(self->support, "NSCocoaErrorDomain", 4, "The requested directory is not available in this domain."));
            s.x[0] = 0;
            return;
        }
        if (s.x[2]) m.write64(s.x[2], static_cast<Address>(relationship(self->path_of_url(found), self->path_of_url(s.x[5]))));
        s.x[0] = 1;
    }, "B@:^qQQ@^@");
    support.method(manager, "getRelationship:ofDirectoryAtURL:toItemAtURL:error:", [self, relationship](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], static_cast<Address>(relationship(self->path_of_url(s.x[3]), self->path_of_url(s.x[4]))));
        s.x[0] = 1;
    }, "B@:^q@@^@");
    support.method(manager, "replaceItemAtURL:withItemAtURL:backupItemName:options:resultingItemURL:error:", [self](CpuState& s, GuestMemory& m) {
        constexpr std::uint64_t without_deleting_backup = 2;
        const auto original = self->path_of_url(s.x[2]), replacement = self->path_of_url(s.x[3]);
        const auto failed = [&](std::uint32_t code, const std::string& path) {
            store(self->support, s.x[7], self->cocoa_error(code, path, true));
            s.x[0] = 0;
        };
        std::uint32_t code{};
        const bool original_exists = self->mode(original, false, code).has_value();
        std::string backup;
        if (original_exists && s.x[4]) {
            backup = self->join(original.substr(0, original.find_last_of('/')), text(self->support, s.x[4]));
            GuestBlock from(self->support, original), to(self->support, backup);
            if (self->sys("_rename", {from, to}) != 0) { failed(guest_errno(self->support), original); return; }
        }
        GuestBlock from(self->support, replacement), to(self->support, original);
        if (self->sys("_rename", {from, to}) != 0) {
            const auto error_code = guest_errno(self->support);
            if (!backup.empty()) {   // put the original back
                GuestBlock undo_from(self->support, backup), undo_to(self->support, original);
                static_cast<void>(self->sys("_rename", {undo_from, undo_to}));
            }
            failed(error_code, replacement);
            return;
        }
        if (!backup.empty() && !(s.x[5] & without_deleting_backup)) {
            CallRegs remove;
            remove.x[2] = string(self->support, backup);
            remove.x[3] = 0;
            static_cast<void>(self->support.send(s.x[0], "removeItemAtPath:error:", remove));
        }
        if (s.x[6]) m.write64(s.x[6], s.x[2]);
        s.x[0] = 1;
    }, "B@:@@@Q^@^@");

    // ---- NSDirectoryEnumerator ----
    support.method(enumerator, "nextObject", [self](CpuState& s, GuestMemory&) { s.x[0] = self->next(s.x[0]); }, "@@:");
    support.method(enumerator, "allObjects", [self](CpuState& s, GuestMemory&) {
        std::vector<Address> values;
        for (auto value = self->next(s.x[0]); value; value = self->next(s.x[0])) values.push_back(value);
        s.x[0] = array(self->support, values);
    }, "@@:");
    for (const auto* name : {"skipDescendants", "skipDescendents"})
        support.method(enumerator, name, [self](CpuState& s, GuestMemory&) {
            const auto state = self->support.objc.find_host_state<EnumeratorState>(s.x[0]);
            if (!state) return;
            std::lock_guard lock(state->mutex);
            state->skip = true;
        }, "v@:");
    support.method(enumerator, "level", [self](CpuState& s, GuestMemory&) {
        const auto state = self->support.objc.find_host_state<EnumeratorState>(s.x[0]);
        if (!state) { s.x[0] = 0; return; }
        std::lock_guard lock(state->mutex);
        s.x[0] = state->last.empty() ? 0 : static_cast<Address>(std::count(state->last.begin(), state->last.end(), '/') + 1);
    }, "Q@:");
    const auto attributes = [self](Address enumerator_object, bool directory) -> Address {
        const auto state = self->support.objc.find_host_state<EnumeratorState>(enumerator_object);
        if (!state || state->urls) return 0;
        std::string path;
        {
            std::lock_guard lock(state->mutex);
            path = directory ? state->root : self->join(state->root, state->last);
        }
        const auto manager_object = self->support.send_id(class_named(self->support, "NSFileManager"), "defaultManager");
        return self->support.send_id(manager_object, "attributesOfItemAtPath:error:", {string(self->support, path), 0});
    };
    support.method(enumerator, "fileAttributes", [attributes](CpuState& s, GuestMemory&) { s.x[0] = attributes(s.x[0], false); }, "@@:");
    support.method(enumerator, "directoryAttributes", [attributes](CpuState& s, GuestMemory&) { s.x[0] = attributes(s.x[0], true); }, "@@:");
    support.method(enumerator, "isEnumeratingDirectoryPostOrder", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    // Fast enumeration over the remaining objects (one at a time).
    support.method(enumerator, "countByEnumeratingWithState:objects:count:", [self](CpuState& s, GuestMemory& m) {
        // NSFastEnumerationState { state, itemsPtr, mutationsPtr, extra[5] }; objects buffer of `count`.
        const auto state_address = s.x[2], buffer = s.x[3];
        if (!s.x[4]) { s.x[0] = 0; return; }
        const auto value = self->next(s.x[0]);
        if (!value) { s.x[0] = 0; return; }
        m.write64(buffer, value);
        m.write64(state_address, 1);
        m.write64(state_address + 8, buffer);
        m.write64(state_address + 16, state_address + 24);   // a mutations word that never changes
        s.x[0] = 1;
    }, "Q@:^v^@Q");
    support.method(enumerator, "dealloc", [self, enumerator](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<EnumeratorState>(s.x[0])) {
            block_release(self->support, std::exchange(state->error_handler, 0));
            objc_release(self->support, std::exchange(state->base_url, 0));
        }
        static_cast<void>(self->support.objc.send_super(s.x[0], enumerator, self->support.objc.selector("dealloc")));
    }, "v@:");
}

} // namespace alchemy::gui::completion_kit
