// DarwinServices' files: *at calls, links, permissions, statvfs/statfs,
// file times, copyfile, temporary files, directories, extended attributes,
// shm_open/msync, and the stdio and wide-character calls installed with them.
#include "alchemy/darwin/libsystem/services_impl.hpp"

namespace alchemy {

void DarwinServices::Impl::install_extra() {
    static_cast<void>(context.install_function("___assert_rtn", [](auto& s, auto& m) {
        try {
            const auto function = s.x[0] ? text(m, s.x[0]) : std::string("(unknown)");
            const auto filename = s.x[1] ? text(m, s.x[1]) : std::string("(unknown)");
            const auto expression = s.x[3] ? text(m, s.x[3]) : std::string("(unknown)");
            throw std::runtime_error("Darwin assertion failed: (" + expression + "), function " + function +
                ", file " + filename + ", line " + std::to_string(static_cast<std::uint32_t>(s.x[2])));
        } catch (const ServiceError&) {
            throw std::runtime_error("Darwin assertion failed with an invalid guest diagnostic string");
        }
    }));
    install("_openat", [this](auto& s, auto& m) { return static_cast<std::uint64_t>(open_path(path_at(m, s.x[0], s.x[1]), s.x[2], (s.x[2] & 0x200) ? m.read64(s.sp) : 0666)); });
    install("_unlinkat", [this](auto& s, auto& m) {
        if (s.x[2] != 0 && s.x[2] != 0x80) throw ServiceError{einval};
        const auto name = filesystem_path_at(m, s.x[0], s.x[1]);
        std::error_code error;
        const auto directory = std::filesystem::is_directory(name, error);
        if (error) throw ServiceError{host_error(error)};
        if (directory && !s.x[2]) throw ServiceError{eisdir};
        if (!directory && s.x[2]) throw ServiceError{enotdir};
        if (!std::filesystem::remove(name, error)) throw ServiceError{error ? host_error(error) : enoent};
        return std::uint64_t{0};
    });
    install("_fdopendir", [this](auto& s, auto& m) { const auto record = file(static_cast<int>(s.x[0])); if (!record->directory) throw ServiceError{enotdir}; return new_directory(m, record->path, static_cast<int>(s.x[0])); }, 0);
    install("_lstat", [this](auto& s, auto& m) {
        const auto name = path(m, s.x[0]);
        if (is_random_device(name)) return random_device_stat(m, s.x[1], name);
        if (const auto* node = device_node(name)) return device_stat(m, s.x[1], *node, current_credentials()[0]);
        if (const auto record = libsystem::symlink_record(name)) {
            validate(m, s.x[1], 144, true);
            m.fill_bytes(s.x[1], 0, 144);
            m.write8(s.x[1] + 4, 0xff); m.write8(s.x[1] + 5, 0xa1);   // S_IFLNK | 0777
            m.write8(s.x[1] + 6, 1);
            m.write32(s.x[1] + 16, current_credentials()[1]);
            m.write32(s.x[1] + 20, current_credentials()[3]);
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            for (unsigned index = 0; index < 4; ++index) {
                m.write64(s.x[1] + 32 + index * 16, static_cast<std::uint64_t>(now / 1000000000));
                m.write64(s.x[1] + 40 + index * 16, static_cast<std::uint64_t>(now % 1000000000));
            }
            m.write64(s.x[1] + 96, record->size());
            m.write32(s.x[1] + 112, 4096);
            return std::uint64_t{0};
        }
        std::error_code error;
        const auto details = host_metadata(name, true);
        const auto link = details.symlink;
        if (error) throw ServiceError{host_error(error)};
        // Installed bundles record their macOS links (junction/hard-link hosts).
        const auto recorded = installed_link(name);
        if (!link && !recorded) return stat(m, s.x[0], s.x[1], false);
        validate(m, s.x[1], 144, true);
        std::size_t length{};
        if (recorded) length = recorded->target.size();
        else {
            const auto target = std::filesystem::read_symlink(name, error);
            if (error) throw ServiceError{host_error(error)};
            length = (target.is_absolute() ? guest_path_string(target) : path_text(target)).size();
        }
        m.fill_bytes(s.x[1], 0, 144);
        m.write8(s.x[1] + 4, 0xff); m.write8(s.x[1] + 5, 0xa1);
        m.write8(s.x[1] + 6, 1);
        m.write64(s.x[1] + 96, length);
        m.write32(s.x[1], details.device); m.write64(s.x[1] + 8, details.inode);
        for (std::size_t index = 0; index < details.times.size(); ++index) {
            m.write64(s.x[1] + 32 + index * 16, details.times[index][0]);
            m.write64(s.x[1] + 40 + index * 16, details.times[index][1]);
        }
        m.write64(s.x[1] + 104, (details.allocated + 511) / 512); m.write32(s.x[1] + 112, 4096);
        return std::uint64_t{0};
    });
    install("_ftruncate", [this](auto& s, auto&) {
        if (s.x[1] > INT64_MAX) throw ServiceError{einval};
        const auto record = file(static_cast<int>(s.x[0]));
        if (record->directory) throw ServiceError{eisdir};
        if (record->socket || record->queue) throw ServiceError{einval};
        if (!record->writable) throw ServiceError{ebadf};
        std::lock_guard lock(record->mutex);
        if (record->closed) throw ServiceError{ebadf};
        // The end of file moves without touching the file position. Growing
        // works while sections view the file; shrinking a mapped file may be
        // refused by the host (Darwin allows it).
        if (const auto resized = host::files::set_size(record->native, s.x[1]); !resized) {
            if (resized.error() == host::Error::busy) {
                notice("ftruncate cannot shrink a file that is mapped with MAP_SHARED (EBUSY)");
                throw ServiceError{16};
            }
            throw ServiceError{file_failure(resized.error())};
        }
        return std::uint64_t{0};
    });
    // int shm_open(const char* name, int oflag, ... /* mode_t mode */): a host
    // file in the root's shm namespace (shm_directory), close-on-exec.
    install("_shm_open", [this](auto& s, auto& m) {
        constexpr std::uint64_t o_rdwr = 2, o_creat = 0x200, o_trunc = 0x400, o_excl = 0x800, o_cloexec = 0x1000000,
            o_nonblock = 4;
        const auto flags = s.x[1];
        if ((flags & ~(3 | o_creat | o_trunc | o_excl | o_cloexec | o_nonblock)) || (flags & 3) == 1 || (flags & 3) == 3 ||
            ((flags & o_trunc) && (flags & 3) != o_rdwr)) throw ServiceError{einval};
        const auto name = shm_path(m, s.x[0]);
        const auto mode = (flags & o_creat) ? m.read64(s.sp) & 0777 : 0;
        return static_cast<std::uint64_t>(open_path(name, (flags & (3 | o_creat | o_trunc | o_excl)) | o_cloexec, mode));
    });
    install("_shm_unlink", [this](auto& s, auto& m) {
        std::error_code error;
        if (!std::filesystem::remove(shm_path(m, s.x[0]), error)) throw ServiceError{error ? host_error(error) : enoent};
        return std::uint64_t{0};
    });
    // int msync(void* address, size_t length, int flags): shared file views
    // start writing their dirty pages back to the file (MS_SYNC and MS_ASYNC
    // alike: other processes see the pages at once anyway); private copies
    // have nothing to write back.
    install("_msync", [this](auto& s, auto& m) {
        constexpr std::uint64_t ms_async = 1, ms_invalidate = 2, ms_sync = 0x10;
        const auto flags = s.x[2];
        if ((flags & ~(ms_async | ms_invalidate | ms_sync)) || ((flags & ms_async) && (flags & ms_sync)) ||
            s.x[0] % darwin_arm64_page_size) throw ServiceError{einval};
        if (!s.x[1]) return std::uint64_t{0};
        bool section = false;
        std::uint64_t flush = 0;
        {
            std::lock_guard lock(mutex);
            auto found = mappings.upper_bound(s.x[0]);
            if (found != mappings.begin()) {
                --found;
                if (s.x[0] - found->first < found->second.size) {
                    if (s.x[1] > found->second.size - (s.x[0] - found->first)) throw ServiceError{enomem};
                    section = found->second.section;
                    const auto into = s.x[0] - found->first;
                    flush = into < found->second.view ? std::min(s.x[1], found->second.view - into) : 0;
                }
            }
        }
        if (section) {
            if (flush && !(flags & ms_invalidate)) {
                if (const auto flushed = host::sections::flush(s.x[0], static_cast<std::size_t>(flush)); !flushed)
                    throw ServiceError{file_failure(flushed.error())};
            }
            return std::uint64_t{0};
        }
        try { static_cast<void>(m.read8(s.x[0])); static_cast<void>(m.read8(s.x[0] + s.x[1] - 1)); }
        catch (...) { throw ServiceError{enomem}; }
        return std::uint64_t{0};
    });
    install("_truncate", [this](auto& s, auto& m) {
        if (s.x[1] > INT64_MAX) throw ServiceError{einval};
        std::error_code error; std::filesystem::resize_file(filesystem_path(m, s.x[0]), s.x[1], error);
        if (error) throw ServiceError{host_error(error)};
        return std::uint64_t{0};
    });
    install("_remove", [this](auto& s, auto& m) { std::error_code error; if (!std::filesystem::remove(filesystem_path(m, s.x[0]), error)) throw ServiceError{error ? host_error(error) : enoent}; return std::uint64_t{0}; });
    install("_link", [this](auto& s, auto& m) { std::error_code error; std::filesystem::create_hard_link(filesystem_path(m, s.x[0]), filesystem_path(m, s.x[1]), error); if (error) throw ServiceError{host_error(error)}; return std::uint64_t{0}; });
    install("_symlink", [this](auto& s, auto& m) {
        const auto target = text(m, s.x[0]);
        auto value = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(target.data()), target.size()));
#ifdef _WIN32
        if (value.has_root_directory() && !value.has_root_name())
            value = translate_path(target);
#endif
        if (is_random_device(value)) throw ServiceError{enosys};
        const auto link = filesystem_path(m, s.x[1]);
        if (target.empty()) throw ServiceError{enoent};
        if (libsystem::symlink_record(link)) throw ServiceError{eexist};
        std::error_code error; std::filesystem::create_symlink(value, link, error);
        if (!error) return std::uint64_t{0};
        { std::error_code existing; if (std::filesystem::exists(std::filesystem::symlink_status(link, existing))) throw ServiceError{eexist}; }
        // Without host symlink privilege a link inside the Alchemy root is a
        // recorded run-time link (junction / hard link; see alchemy_root.hpp).
        // Links those cannot represent (a dangling target, a link or target
        // outside the root) are symlink records (symlink_records.hpp).
        const auto record = [&] {
            { std::error_code parent; if (!std::filesystem::is_directory(link.parent_path(), parent)) throw ServiceError{enoent}; }
            if (!libsystem::add_symlink_record(link, target)) throw ServiceError{eexist};
            return std::uint64_t{0};
        };
        if (!configured_alchemy_root()) return record();
        auto relative = target;
        if (!target.empty() && target.front() == '/') {
            const auto host_target = translate_path(target);
            relative = path_text(host_target.lexically_relative(link.parent_path()));
            if (relative.empty()) return record();
        }
        try { create_runtime_link(link, relative); }
        catch (const std::system_error& failure) {
            const auto code = failure.code().category() == std::generic_category() ? failure.code().value() : -1;
            if (code == ENOENT || code == EXDEV || code == EINVAL) return record();
            if (code >= 0) throw ServiceError{host_error(code)};
            throw ServiceError{host_error(failure.code())};
        }
        return std::uint64_t{0};
    });
    install("_readlink", [this](auto& s, auto& m) {
        if (!s.x[2]) throw ServiceError{einval};
        if (is_random_device(path(m, s.x[0]))) throw ServiceError{einval};
        std::string value;
        if (const auto record = libsystem::symlink_record(path(m, s.x[0]))) value = *record;
        else if (const auto recorded = installed_link(path(m, s.x[0]))) value = recorded->target;
        else {
            std::error_code error; const auto target = std::filesystem::read_symlink(path(m, s.x[0]), error);
            if (error) throw ServiceError{host_error(error)};
            value = target.is_absolute() ? guest_path_string(target) : path_text(target);
        }
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(value.size(), s.x[2]));
        m.write_bytes(s.x[1], std::span(reinterpret_cast<const std::uint8_t*>(value.data()), count));
        return static_cast<std::uint64_t>(count);
    });
    install("_realpath", [this](auto& s, auto& m) {
        const auto name = path(m, s.x[0]);
        std::error_code error; const auto canonical = is_random_device(name) ? name : std::filesystem::canonical(name, error);
        if (error) throw ServiceError{host_error(error)};
        const auto value = guest_path_string(is_random_device(name) ? canonical : resolve_installed_links(canonical));
        if (value.size() >= 1024 && s.x[1]) throw ServiceError{erange};
        const auto address = s.x[1] ? s.x[1] : allocate(value.size() + 1);
        put_text(m, address, value);
        return address;
    }, 0);
    // Windows ACLs cannot represent arbitrary Unix permission bits. Readonly is
    // supported; unsupported ownership/group distinctions fail explicitly.
    install("_fchmod", [this](auto& s, auto&) {
        const auto record = file(static_cast<int>(s.x[0]));
        if (record->random_device) throw ServiceError{eperm};
        if (record->socket || record->queue) throw ServiceError{45};
        const auto mode = s.x[1] & 07777;   // chmod(2) ignores the file-type bits (libuv passes st_mode)
        if (mode != 0444 && mode != 0666 && mode != 0400 && mode != 0600) throw ServiceError{enosys};
        if (const auto changed = host::files::set_read_only(path_text(record->path), !(mode & 0200)); !changed)
            throw ServiceError{crt_failure(changed.error())};
        return std::uint64_t{0};
    });
    install("_fchmodat", [this](auto& s, auto& m) {
        if (s.x[3]) throw ServiceError{einval};
        const auto name = filesystem_path_at(m, s.x[0], s.x[1]);
        // The host's attributes hold write permission or none.
        const auto mode = s.x[2] & 07777;
        if (mode != 0444 && mode != 0666 && mode != 0400 && mode != 0600) throw ServiceError{enosys};
        if (const auto changed = host::files::set_read_only(path_text(name), !(mode & 0200)); !changed) throw ServiceError{crt_failure(changed.error())};
        return std::uint64_t{0};
    });
    install("_statvfs", [this](auto& s, auto& m) {
        if (is_random_device(path(m, s.x[0]))) throw ServiceError{enosys};
        std::error_code error; const auto information = std::filesystem::space(path(m, s.x[0]), error);
        if (error) throw ServiceError{host_error(error)};
        // Darwin arm64 struct statvfs (<sys/statvfs.h>, 64 bytes): unsigned long
        // f_bsize, f_frsize; fsblkcnt_t (uint32) f_blocks, f_bfree, f_bavail;
        // fsfilcnt_t (uint32) f_files, f_ffree, f_favail; unsigned long f_fsid,
        // f_flag, f_namemax. Block counts are in f_frsize units; a volume too
        // large for 32-bit counts reports a larger f_frsize so sizes stay exact.
        constexpr std::uint64_t statvfs_size = 64;
        validate(m, s.x[1], statvfs_size, true);
        m.fill_bytes(s.x[1], 0, statvfs_size);
        std::uint64_t fragment = 4096;
        while (information.capacity / fragment > UINT32_MAX) fragment *= 2;
        m.write64(s.x[1], 4096); m.write64(s.x[1] + 8, fragment);
        m.write32(s.x[1] + 16, static_cast<std::uint32_t>(information.capacity / fragment));
        m.write32(s.x[1] + 20, static_cast<std::uint32_t>(information.free / fragment));
        m.write32(s.x[1] + 24, static_cast<std::uint32_t>(information.available / fragment));
        for (const std::uint64_t offset : {28, 32, 36}) m.write32(s.x[1] + offset, UINT32_MAX);   // no inode limit
        m.write64(s.x[1] + 56, 255);   // f_namemax
        return std::uint64_t{0};
    });
    install("_utimes", [this](auto& s, auto& m) {
        const auto name = filesystem_path(m, s.x[0]);
        std::optional<std::int64_t> access = host::time::realtime_ns(), modified = access;
        if (s.x[1]) {
            const auto convert = [&](std::uint64_t address) {
                const auto seconds = std::bit_cast<std::int64_t>(m.read64(address));
                const auto micros = m.read32(address + 8);
                if (micros >= 1000000 || seconds > INT64_MAX / 1'000'000'000 - 1 || seconds < INT64_MIN / 1'000'000'000 + 1) throw ServiceError{einval};
                return seconds * 1'000'000'000 + static_cast<std::int64_t>(micros) * 1000;
            };
            access = convert(s.x[1]); modified = convert(s.x[1] + 16);
        }
        // EACCES when the file cannot be opened for its attributes, EINVAL for
        // an unrepresentable time, ENOENT for a missing path, and EIO when
        // the times cannot be written to a file that exists (utimes(2)).
        if (const auto set = host::files::set_times(path_text(name), access, modified); !set)
            throw ServiceError{file_failure(set.error())};
        return std::uint64_t{0};
    });
    install("_copyfile_state_alloc", [this](auto&, auto&) { const auto address = context.allocate_guest(8); std::lock_guard lock(mutex); copy_states.emplace(address, CopyState{}); return address; }, 0);
    install("_copyfile_state_free", [this](auto& s, auto&) {
        if (!s.x[0]) return std::uint64_t{0};
        CopyState state;
        { std::lock_guard lock(mutex); const auto found = copy_states.find(s.x[0]); if (found == copy_states.end()) throw ServiceError{einval}; state = found->second; copy_states.erase(found); }
        for (const auto name : {state.source_name, state.destination_name}) if (name) context.release_guest(name);
        context.release_guest(s.x[0]);
        return std::uint64_t{0};
    });
    // copyfile_state_get/_set(state, flag, value): COPYFILE_STATE_SRC_FD 1, SRC_FILENAME 2, DST_FD 3,
    // DST_FILENAME 4, QUARANTINE 5, STATUS_CB 6, STATUS_CTX 7, COPIED 8, XATTRNAME 9, WAS_CLONED 10.
    // Descriptors are passed by address, the other values directly; get writes through `value`.
    // Alchemy copies with the host's copy calls, so no status callback is called and nothing is cloned.
    install("_copyfile_state_get", [this](auto& s, auto& m) {
        const auto out = s.x[2];
        if (!out) throw ServiceError{einval};
        std::lock_guard lock(mutex);
        const auto found = copy_states.find(s.x[0]);
        if (found == copy_states.end()) throw ServiceError{einval};
        const auto& state = found->second;
        switch (static_cast<std::uint32_t>(s.x[1])) {
        case 1: validate(m, out, 4, true); m.write32(out, static_cast<std::uint32_t>(state.source_fd)); break;
        case 3: validate(m, out, 4, true); m.write32(out, static_cast<std::uint32_t>(state.destination_fd)); break;
        case 2: validate(m, out, 8, true); m.write64(out, state.source_name); break;
        case 4: validate(m, out, 8, true); m.write64(out, state.destination_name); break;
        case 5: validate(m, out, 8, true); m.write64(out, state.quarantine); break;
        case 6: validate(m, out, 8, true); m.write64(out, state.status_callback); break;
        case 7: validate(m, out, 8, true); m.write64(out, state.status_context); break;
        case 8: validate(m, out, 8, true); m.write64(out, static_cast<std::uint64_t>(state.copied)); break;
        case 9: validate(m, out, 8, true); m.write64(out, 0); break;   // no extended attribute is being copied
        case 10: validate(m, out, 1, true); m.write8(out, state.cloned ? 1 : 0); break;
        default: throw ServiceError{einval};
        }
        return std::uint64_t{0};
    });
    install("_copyfile_state_set", [this](auto& s, auto& m) {
        const auto flag = static_cast<std::uint32_t>(s.x[1]);
        const std::uint64_t value = s.x[2];
        std::string name;
        if (flag == 2 || flag == 4) name = text(m, value);
        std::int32_t descriptor = -1;
        if (flag == 1 || flag == 3) { validate(m, value, 4, false); descriptor = static_cast<std::int32_t>(m.read32(value)); }
        std::lock_guard lock(mutex);
        const auto found = copy_states.find(s.x[0]);
        if (found == copy_states.end()) throw ServiceError{einval};
        auto& state = found->second;
        const auto keep = [&](std::uint64_t& slot) {
            const auto address = context.allocate_guest(name.size() + 1);
            put_text(m, address, name);
            if (slot) context.release_guest(slot);
            slot = address;
        };
        switch (flag) {
        case 1: state.source_fd = descriptor; break;
        case 3: state.destination_fd = descriptor; break;
        case 2: keep(state.source_name); break;
        case 4: keep(state.destination_name); break;
        case 5: state.quarantine = value; break;
        case 6: state.status_callback = value; break;
        case 7: state.status_context = value; break;
        default: throw ServiceError{einval};
        }
        return std::uint64_t{0};
    });
    install("_fcopyfile", [this](auto& s, auto& m) {
        if (s.x[0] == s.x[1] || s.x[3] != 8) throw ServiceError{einval};
        if (s.x[2]) {
            std::lock_guard lock(mutex);
            const auto found = copy_states.find(s.x[2]);
            if (found == copy_states.end()) throw ServiceError{einval};
            found->second.source_fd = static_cast<std::int32_t>(s.x[0]); found->second.destination_fd = static_cast<std::int32_t>(s.x[1]);
            found->second.copied = 0; found->second.cloned = false;
        }
        static_cast<void>(seek(static_cast<int>(s.x[0]), 0, 0)); static_cast<void>(seek(static_cast<int>(s.x[1]), 0, 0));
        const auto buffer = context.allocate_guest(65536);
        try {
            for (;;) {
                const auto count = read(m, static_cast<int>(s.x[0]), buffer, 65536);
                if (!count) break;
                std::uint64_t written = 0;
                while (written < count) { const auto done = write(m, static_cast<int>(s.x[1]), buffer + written, count - written); if (!done) throw ServiceError{eio}; written += done; }
            }
            const auto length = seek(static_cast<int>(s.x[1]), 0, 1);
            if (s.x[2]) { std::lock_guard lock(mutex); if (const auto found = copy_states.find(s.x[2]); found != copy_states.end()) found->second.copied = static_cast<std::int64_t>(length); }
            const auto record = file(static_cast<int>(s.x[1]));
            std::lock_guard lock(record->mutex);
            if (record->closed) throw ServiceError{ebadf};
            if (const auto resized = host::files::set_size(record->native, length); !resized) {
                // As ftruncate: the host may refuse to shrink a mapped file (EBUSY).
                if (resized.error() == host::Error::busy) {
                    notice("fcopyfile cannot shrink a destination that is mapped with MAP_SHARED (EBUSY)");
                    throw ServiceError{16};
                }
                throw ServiceError{crt_failure(resized.error())};
            }
            context.release_guest(buffer);
        } catch (...) { context.release_guest(buffer); throw; }
        return std::uint64_t{0};
    });
    for (const auto& [alias, original] : std::array<std::pair<const char*, const char*>, 3>{{{"_fseeko", "_fseek"}, {"_ftello", "_ftell"}, {"_lstat$INODE64", "_lstat"}}})
        context.install_export(std::string(system_library), alias, *context.resolve(system_library, original));
    install("_setbuf", [this](auto& s, auto&) { static_cast<void>(stream(s.x[0])); if (s.x[1]) throw std::runtime_error("Darwin setbuf buffering is unsupported"); return std::uint64_t{0}; });
    install("_setvbuf", [this](auto& s, auto&) { static_cast<void>(stream(s.x[0])); if (s.x[2] != 2) throw ServiceError{enosys}; return std::uint64_t{0}; });
    // isatty: fs/terminal.cpp (is_terminal).
    install("_isatty", [this](auto& s, auto&) {
        if (s.x[0] > INT32_MAX) throw ServiceError{ebadf};
        if (!is_terminal(static_cast<int>(s.x[0]))) throw ServiceError{25};
        return std::uint64_t{1};
    }, 0);
    install("_ungetc", [this](auto& s, auto&) {
        const auto record = stream(s.x[1]); std::lock_guard stream_lock(*record.lock);
        if (static_cast<std::uint32_t>(s.x[0]) == UINT32_MAX) return UINT64_MAX;
        std::lock_guard lock(mutex); auto& current = streams.at(s.x[1]);
        if (current.pushed != -1) return UINT64_MAX;
        current.pushed = static_cast<int>(s.x[0] & 255); current.eof = false;
        return s.x[0] & 255;
    });
    install("_flockfile", [this](auto& s, auto&) { stream(s.x[0]).lock->lock(); return std::uint64_t{0}; });
    install("_funlockfile", [this](auto& s, auto&) { stream(s.x[0]).lock->unlock(); return std::uint64_t{0}; });
    install("_ftrylockfile", [this](auto& s, auto&) { return stream(s.x[0]).lock->try_lock() ? std::uint64_t{0} : std::uint64_t{1}; });
    install("_fputwc", [this](auto& s, auto& m) { if (s.x[0] > 127) throw ServiceError{92}; const char character = static_cast<char>(s.x[0]); static_cast<void>(output_stream(m, s.x[1], std::string_view(&character, 1))); return s.x[0]; }, UINT32_MAX);
    install("_getwc", [this](auto& s, auto& m) {
        const auto byte = context.allocate_guest(1);
        try { const auto count = read_stream(m, s.x[0], byte, 1); const auto result = count ? static_cast<std::uint64_t>(m.read8(byte)) : UINT32_MAX; context.release_guest(byte); if (!count) { std::lock_guard lock(mutex); streams.at(s.x[0]).eof = true; } if (count && result > 127) throw ServiceError{92}; return result; }
        catch (...) { if (m.query_region(byte)) context.release_guest(byte); throw; }
    }, UINT32_MAX);
    install("_ungetwc", [this](auto& s, auto&) { if (s.x[0] > 127) return static_cast<std::uint64_t>(UINT32_MAX); const auto value = stream(s.x[1]); if (value.pushed != -1) return static_cast<std::uint64_t>(UINT32_MAX); std::lock_guard lock(mutex); streams.at(s.x[1]).pushed = static_cast<int>(s.x[0]); streams.at(s.x[1]).eof = false; return s.x[0]; }, UINT32_MAX);
    context.install_export(std::string(system_library), "_fgetwc", *context.resolve(system_library, "_getwc"));
    install("_snprintf_l", [this](auto& s, auto& m) { validate_locale(s.x[2]); const auto value = format(m, s.x[3], s.sp); if (s.x[1]) put_text(m, s.x[0], std::string_view(value.data(), static_cast<std::size_t>(std::min<std::uint64_t>(value.size(), s.x[1] - 1)))); return static_cast<std::uint64_t>(value.size()); }, UINT32_MAX);
    install("_asprintf_l", [this](auto& s, auto& m) { validate_locale(s.x[1]); const auto value = format(m, s.x[2], s.sp); const auto address = allocate(value.size() + 1); put_text(m, address, value); m.write64(s.x[0], address); return static_cast<std::uint64_t>(value.size()); }, UINT32_MAX);
    // strerror_r: install_darwin_support (misc/support.cpp).
    for (const auto localized : {false, true}) install(localized ? "_sscanf_l" : "_sscanf", [this, localized](auto& s, auto& m) { if (localized) validate_locale(s.x[1]); return scan(m, s.x[0], s.x[localized ? 2 : 1], s.sp); }, UINT32_MAX);
    for (const auto localized : {false, true}) install(localized ? "_strftime_l" : "_strftime", [this, localized](auto& s, auto& m) {
        if (localized) validate_locale(s.x[4]);
        // %z and %Z come from the Darwin tm's tm_gmtoff (+40) and tm_zone (+48), as
        // in wcsftime (misc/support.cpp); the host's zone does not describe a guest tm.
        std::string specification;
        {
            const auto source = text(m, s.x[2]);
            for (std::size_t at = 0; at < source.size(); ++at) {
                if (source[at] != '%' || at + 1 == source.size()) { specification += source[at]; continue; }
                const auto conversion = source[++at];
                if (conversion == 'z') {
                    const auto offset = std::bit_cast<std::int64_t>(m.read64(s.x[3] + 40));
                    if (offset < -86400 || offset > 86400) throw ServiceError{einval};
                    const auto absolute = offset < 0 ? -offset : offset;
                    std::array<char, 16> buffer{};
                    std::snprintf(buffer.data(), buffer.size(), "%c%02lld%02lld", offset < 0 ? '-' : '+',
                        static_cast<long long>(absolute / 3600), static_cast<long long>((absolute / 60) % 60));
                    specification += buffer.data();
                } else if (conversion == 'Z') {
                    if (const auto zone = m.read64(s.x[3] + 48))
                        for (const auto c : text(m, zone)) specification += c == '%' ? std::string("%%") : std::string(1, c);
                } else { specification += '%'; specification += conversion; }
            }
        }
        std::tm value{};
        const auto integer = [&](std::uint64_t at) { return std::bit_cast<std::int32_t>(m.read32(s.x[3] + at)); };
        value.tm_sec = integer(0); value.tm_min = integer(4); value.tm_hour = integer(8);
        value.tm_mday = integer(12); value.tm_mon = integer(16); value.tm_year = integer(20);
        value.tm_wday = integer(24); value.tm_yday = integer(28); value.tm_isdst = integer(32);
        if (value.tm_sec < 0 || value.tm_sec > 60 || value.tm_min < 0 || value.tm_min > 59 ||
            value.tm_hour < 0 || value.tm_hour > 23 || value.tm_mday < 1 || value.tm_mday > 31 ||
            value.tm_mon < 0 || value.tm_mon > 11 || value.tm_wday < 0 || value.tm_wday > 6 ||
            value.tm_yday < 0 || value.tm_yday > 365 || value.tm_year < -1900 || value.tm_year > 8099 ||
            value.tm_isdst < -1 || value.tm_isdst > 1) throw ServiceError{einval};
        const auto count = bounded_size(s.x[1]);
        if (!count) return std::uint64_t{0};
        std::vector<char> buffer(count);
        const auto written = std::strftime(buffer.data(), buffer.size(), specification.c_str(), &value);
        if (written) put_text(m, s.x[0], std::string_view(buffer.data(), written));
        return static_cast<std::uint64_t>(written);
    }, 0);
    install("_swprintf", [](auto& s, auto& m) {
        const auto value = wide_format_text(m, s.x[2], s.sp);
        if (s.x[1] <= value.size()) return static_cast<std::uint64_t>(UINT32_MAX);
        for (std::size_t at = 0; at < value.size(); ++at) m.write32(add(s.x[0], at * 4), static_cast<std::uint32_t>(value[at]));
        m.write32(add(s.x[0], value.size() * 4), 0);
        return static_cast<std::uint64_t>(value.size());
    }, UINT32_MAX);
    // Wide-character printf: wide_format_text. swscanf over the narrow
    // implementation: formats and scanned input must be ASCII (C locale), and
    // wide string/character conversions (%ls, %lc, %S, %C, %l[) fail explicitly.
    const auto narrow_wide = [this](auto& m, std::uint64_t address, const char* function) {
        std::string narrow;
        for (std::uint64_t at = 0;; ++at) {
            if (at == string_limit) throw std::runtime_error(std::string("Darwin ") + function + " string exceeds limit");
            const auto value = m.read32(add(address, at * 4));
            if (!value) break;
            if (value > 127) throw std::runtime_error(std::string("Darwin ") + function + " currently requires ASCII text");
            narrow += static_cast<char>(value);
        }
        return narrow;
    };
    const auto check_wide_conversions = [](const std::string& narrow, const char* function) {
        for (std::size_t at = 0; at + 1 < narrow.size(); ++at) {
            if (narrow[at] != '%') continue;
            auto next = at + 1;
            while (next < narrow.size() && std::string_view("0123456789.-+ #*'hjztqL").find(narrow[next]) != std::string_view::npos) ++next;
            if (next < narrow.size() && (narrow[next] == 'S' || narrow[next] == 'C' ||
                (narrow[next] == 'l' && next + 1 < narrow.size() && (narrow[next + 1] == 's' || narrow[next + 1] == 'c' || narrow[next + 1] == '['))))
                throw std::runtime_error(std::string("Darwin ") + function + " wide string arguments are unsupported");
            if (next < narrow.size() && narrow[next] == '%') at = next;
        }
    };
    install("_vswprintf", [](auto& s, auto& m) {
        const auto value = wide_format_text(m, s.x[2], s.x[3]);
        if (s.x[1] <= value.size()) return static_cast<std::uint64_t>(UINT32_MAX);
        for (std::size_t at = 0; at < value.size(); ++at) m.write32(add(s.x[0], at * 4), static_cast<std::uint32_t>(value[at]));
        m.write32(add(s.x[0], value.size() * 4), 0);
        return static_cast<std::uint64_t>(value.size());
    }, UINT32_MAX);
    install("_fwprintf", [this](auto& s, auto& m) {
        const auto value = utf32_to_utf8(wide_format_text(m, s.x[1], s.sp));
        static_cast<void>(output_stream(m, s.x[0], value));
        return static_cast<std::uint64_t>(value.size());
    }, UINT32_MAX);
    install("_swscanf", [this, narrow_wide, check_wide_conversions](auto& s, auto& m) {
        const auto input = narrow_wide(m, s.x[0], "swscanf"), specification = narrow_wide(m, s.x[1], "swscanf");
        check_wide_conversions(specification, "swscanf");
        const auto input_address = context.allocate_guest(input.size() + 1);
        const auto format_address = context.allocate_guest(specification.size() + 1);
        try {
            put_text(m, input_address, input); put_text(m, format_address, specification);
            const auto result = scan(m, input_address, format_address, s.sp);
            context.release_guest(input_address); context.release_guest(format_address);
            return result;
        } catch (...) { context.release_guest(input_address); context.release_guest(format_address); throw; }
    }, UINT32_MAX);
}

void DarwinServices::Impl::install_python_services() {
    for (const auto target : {"_fopen", "_fdopen", "_realpath"})
        context.install_export(std::string(system_library), std::string(target) + "$DARWIN_EXTSN", *context.resolve(system_library, target));
    install("___srget", [this](auto& s, auto& m) {
        const auto temporary = context.allocate_guest(1);
        try { const auto count = read_stream(m, s.x[0], temporary, 1);
            const auto result = count ? static_cast<std::uint64_t>(m.read8(temporary)) : UINT64_MAX;
            m.write32(s.x[0] + 8, 0); context.release_guest(temporary); return result;
        } catch (...) { context.release_guest(temporary); throw; }
    });
    install("___swbuf", [this](auto& s, auto& m) {
        const char byte = static_cast<char>(s.x[0]); static_cast<void>(output_stream(m, s.x[1], std::string_view(&byte, 1)));
        m.write32(s.x[1] + 12, 0); return s.x[0] & 255;
    });
    // fseeko and ftello: aliases of fseek and ftell (install_extra).
    install("_rewind", [this](auto& s, auto& m) {
        const auto value = stream(s.x[0]);
        std::lock_guard stream_lock(*value.lock);
        static_cast<void>(seek(value.descriptor, 0, 0));
        { std::lock_guard lock(mutex); auto& current = streams.at(s.x[0]); current.eof = false; current.error = false; current.pushed = -1; }
        m.write32(s.x[0] + 8, 0); m.write8(s.x[0] + 16, static_cast<std::uint8_t>(m.read8(s.x[0] + 16) & ~0x60U));
        return std::uint64_t{0};
    });
    install("_dup", [this](auto& s, auto&) { return static_cast<std::uint64_t>(duplicate(static_cast<int>(s.x[0]), 0)); });
    install("_dup2", [this](auto& s, auto&) { return static_cast<std::uint64_t>(duplicate(static_cast<int>(s.x[0]), 0, static_cast<int>(s.x[1]))); });
    install("_fsync", [this](auto& s, auto&) {
        const auto record = file(static_cast<int>(s.x[0]));
        std::lock_guard lock(record->mutex);
        if (record->closed) throw ServiceError{ebadf};
        if (record->directory) { sync_directory(record->path); return std::uint64_t{0}; }   // LevelDB's SyncParent
        if (record->standard >= 0 || record->random_device || record->socket || record->queue) throw ServiceError{einval};
        if (!record->native) throw ServiceError{ebadf};
        if (const auto flushed = host::files::flush(record->native); !flushed) throw ServiceError{crt_failure(flushed.error())};
        return std::uint64_t{0};
    });
    install("_fcntl", [this](auto& s, auto& m) {
        const auto descriptor = static_cast<int>(s.x[0]);
        const auto command = static_cast<std::uint32_t>(s.x[1]);
        const auto record = descriptor_file(descriptor);
        if (command == 0 || command == 67)
            return static_cast<std::uint64_t>(duplicate(descriptor, static_cast<std::int32_t>(m.read32(s.sp)), {}, command == 67));
        if (command == 1) return static_cast<std::uint64_t>(descriptor_cloexec(descriptor));
        if (command == 3) { if (!record) return std::uint64_t{descriptor == 0 ? 0ULL : 1ULL}; std::lock_guard lock(record->mutex); if (record->closed) throw ServiceError{ebadf}; if (record->socket) return static_cast<std::uint64_t>(record->socket->status_flags.load());
            if (record->pipe) return (record->flags & 3) | record->pipe_status->load();
            return record->random_device ? *record->random_flags : record->flags & ~0x1000000ULL; }
        // Apple ARM64 variadic ints occupy a stack slot whose upper padding is
        // unspecified. Pointer-valued F_GETPATH arguments use the full word.
        const auto argument = command == 50 || (command >= 7 && command <= 9) ? m.read64(s.sp) : static_cast<std::uint64_t>(m.read32(s.sp));
        if (command == 2) {
            if (argument & ~1ULL) throw ServiceError{einval};
            set_descriptor_cloexec(descriptor, argument != 0);
            return std::uint64_t{0};
        }
        if (command == 4) {
            if (record && record->socket) {
                std::lock_guard lock(record->mutex); if (record->closed) throw ServiceError{ebadf};
                if (argument & ~(2ULL | 4ULL | 0x1000000ULL)) throw ServiceError{einval};
                record->socket->status_flags = 2U | static_cast<std::uint32_t>(argument & 4); return std::uint64_t{0};
            }
            if (record && record->random_device) {
                std::lock_guard lock(record->mutex);
                if (argument & ~(4ULL | 0x1000000ULL)) throw ServiceError{einval};
                *record->random_flags = argument & 4; return std::uint64_t{0};
            }
            if (record && record->pipe) {
                // O_NONBLOCK is honoured by pipe reads (PeekNamedPipe); O_APPEND has no meaning.
                std::lock_guard lock(record->mutex); if (record->closed) throw ServiceError{ebadf};
                record->pipe_status->store(static_cast<std::uint32_t>(argument & 4)); return std::uint64_t{0};
            }
            if (record && record->null_device) {
                std::lock_guard lock(record->mutex);
                record->flags = (record->flags & ~4ULL) | (argument & 4); return std::uint64_t{0};
            }
            // Shared open-file status flags need a backend update; do not
            // advertise a successful append/nonblocking change we cannot make.
            const auto current = record ? record->flags : (descriptor == 0 ? 0ULL : 1ULL);
            if ((argument & (4 | 8)) != (current & (4 | 8))) throw ServiceError{enosys};
            return std::uint64_t{0};
        }
        if (command >= 7 && command <= 9) {
            // F_GETLK, F_SETLK, F_SETLKW: struct flock {off_t l_start, l_len;
            // pid_t l_pid; short l_type, l_whence;} (24 bytes); POSIX record
            // locks shared by every Alchemy process (record_locks.hpp).
            validate(m, argument, 24, command == 7);
            auto start = static_cast<std::int64_t>(m.read64(argument));
            auto length = static_cast<std::int64_t>(m.read64(argument + 8));
            const auto packed = m.read32(argument + 20);
            const auto type = static_cast<std::int16_t>(packed & 0xffff), whence = static_cast<std::int16_t>(packed >> 16);
            if (!record) throw ServiceError{descriptor >= 0 && descriptor <= 2 ? einval : ebadf};
            std::string key;
            bool readable{}, writable{};
            std::int64_t base = 0;
            {
                std::lock_guard lock(record->mutex);
                if (record->closed) throw ServiceError{ebadf};
                if (record->socket || record->pipe || record->queue) throw ServiceError{ebadf};   // not a vnode
                if (!record->native || !record->flock_owner) throw ServiceError{einval};
                readable = record->readable; writable = record->writable;
                key = record_locks::file_key(static_cast<std::intptr_t>(record->native.value));
                if (whence == 1) {
                    const auto position = host::files::seek(record->native, 0, host::files::Whence::current);
                    if (!position) throw ServiceError{crt_failure(position.error())};
                    base = static_cast<std::int64_t>(*position);
                } else if (whence == 2) base = static_cast<std::int64_t>(file_size(record->native, file_failure));
            }
            if (type < 1 || type > 3 || whence < 0 || whence > 2 || base < 0) throw ServiceError{einval};
            if (command != 7 && ((type == record_locks::read_lock && !readable) || (type == record_locks::write_lock && !writable))) throw ServiceError{ebadf};
            if (key.empty()) throw ServiceError{45};
            if ((base > 0 && start > INT64_MAX - base)) throw ServiceError{84};   // EOVERFLOW
            start += base;
            if (length < 0) { if (start < -length) throw ServiceError{einval}; start += length; length = -length; }
            if (start < 0) throw ServiceError{einval};
            if (length && start > INT64_MAX - length) throw ServiceError{84};
            const auto end = length ? start + length : record_locks::to_end;
            if (command == 7) {
                const auto conflict = type == record_locks::unlock ? std::nullopt : record_locks::test(key, start, end, type);
                if (!conflict) { m.write32(argument + 20, record_locks::unlock | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(whence)) << 16)); return std::uint64_t{0}; }
                m.write64(argument, static_cast<std::uint64_t>(conflict->start));
                m.write64(argument + 8, conflict->end == record_locks::to_end ? 0 : static_cast<std::uint64_t>(conflict->end - conflict->start));
                m.write32(argument + 16, conflict->pid);
                m.write32(argument + 20, static_cast<std::uint32_t>(conflict->type));   // l_whence SEEK_SET
                return std::uint64_t{0};
            }
            const auto error = record_locks::set(key, start, end, type, command == 9, [this]() -> bool { return context.requested_exit().has_value(); });
            if (error) throw ServiceError{static_cast<std::uint32_t>(error)};
            return std::uint64_t{0};
        }
        if (command == 50 && record) {
            if (record->socket || record->queue) throw ServiceError{45};
            const auto value = guest_path_string(record->path);
            if (value.size() >= 1024) throw ServiceError{63};
            validate(m, argument, value.size() + 1, true); put_text(m, argument, value);
            return std::uint64_t{0};
        }
        if (command == 51 && record) {
            std::lock_guard lock(record->mutex);
            if (!record->closed && record->directory) { sync_directory(record->path); return std::uint64_t{0}; }
            if (record->closed || !record->native) throw ServiceError{ebadf};
            if (const auto flushed = host::files::flush(record->native); !flushed) throw ServiceError{file_failure(flushed.error())};
            return std::uint64_t{0};
        }
        throw ServiceError{enosys};
    });
    for (const auto writing : {false, true}) {
        install(writing ? "_pwrite" : "_pread", [this, writing](auto& s, auto& m) {
            return positioned_io(m, static_cast<int>(s.x[0]), s.x[1], s.x[2], s.x[3], writing);
        });
        for (const auto positioned : {false, true}) install(writing ? (positioned ? "_pwritev" : "_writev") :
            (positioned ? "_preadv" : "_readv"), [this, writing, positioned](auto& s, auto& m) { return vector_io(s, m, writing, positioned); });
    }
    const auto check_access = [this](const std::filesystem::path& name, std::uint64_t mode) {
        if (mode & ~7ULL) throw ServiceError{einval};
        if (is_random_device(name)) { if (mode & 3) throw ServiceError{eacces}; return; }
        if (is_null_device(name)) { if (mode & 1) throw ServiceError{eacces}; return; }
        if (const auto* node = device_node(name)) {
            // Permission bits of the node for this process's effective user.
            const auto ids = current_credentials();
            if (ids[1] == 0) { if (mode & 1) throw ServiceError{eacces}; return; }
            const auto bits = node->console && ids[1] == ids[0] ? (node->mode >> 6) & 7 : node->mode & 7;
            if ((mode & 7) & ~bits) throw ServiceError{eacces};
            return;
        }
        if (const auto allowed = host::files::access(path_text(name), (mode & 2) != 0); !allowed) throw ServiceError{crt_failure(allowed.error())};
        if (mode & 1) {
            std::error_code error;
            const auto info = std::filesystem::status(name, error);
            if (error) throw ServiceError{host_error(error)};
            // The host has no Darwin execute bit: directories are traversable;
            // an ordinary file is executable when exec can run it (a Mach-O
            // program or bundle, or a #! script).
            if (!std::filesystem::is_directory(info) && !guest_executable(guest_path_string(name), exec_environment()))
                throw ServiceError{eacces};
        }
    };
    install("_access", [this, check_access](auto& s, auto& m) {
        // The built-in /bin/sh, /usr/bin/env and the guest Python alias exist for exec.
        if (const auto value = text(m, s.x[0]); !(s.x[1] & 2) && is_virtual_program(value)) return std::uint64_t{0};
        check_access(path(m, s.x[0]), s.x[1]); return std::uint64_t{0};
    });
    install("_faccessat", [this, check_access](auto& s, auto& m) {
        if (s.x[3] != 0 && s.x[3] != 0x10) throw ServiceError{einval};
        check_access(path_at(m, s.x[0], s.x[1]), s.x[2]); return std::uint64_t{0};
    });
    install("_chmod", [this](auto& s, auto& m) {
        const auto name = filesystem_path(m, s.x[0]);
        const auto mode = s.x[1] & 07777;
        if (mode != 0444 && mode != 0666 && mode != 0400 && mode != 0600) throw ServiceError{enosys};
        if (const auto changed = host::files::set_read_only(path_text(name), !(mode & 0200)); !changed) throw ServiceError{crt_failure(changed.error())};
        return std::uint64_t{0};
    });
    install("_fchdir", [this](auto& s, auto&) {
        const auto record = file(static_cast<int>(s.x[0]));
        std::lock_guard file_lock(record->mutex);
        if (record->closed) throw ServiceError{ebadf};
        if (!record->directory) throw ServiceError{enotdir};
        if (overlay) { overlay->cwd = record->path; return std::uint64_t{0}; }
        std::lock_guard lock(mutex); context.working_directory = record->path; return std::uint64_t{0};
    });
    install("_mkdirat", [this](auto& s, auto& m) {
        std::error_code error; const auto created = std::filesystem::create_directory(filesystem_path_at(m, s.x[0], s.x[1]), error);
        if (error) throw ServiceError{host_error(error)};
        if (!created) throw ServiceError{eexist};
        return std::uint64_t{0};
    });
    install("_renameat", [this](auto& s, auto& m) {
        rename_path(filesystem_path_at(m, s.x[0], s.x[1]), filesystem_path_at(m, s.x[2], s.x[3]));
        return std::uint64_t{0};
    });
    install("_linkat", [this](auto& s, auto& m) {
        if (s.x[4]) throw ServiceError{enosys};
        std::error_code error; std::filesystem::create_hard_link(filesystem_path_at(m, s.x[0], s.x[1]), filesystem_path_at(m, s.x[2], s.x[3]), error);
        if (error) throw ServiceError{host_error(error)}; return std::uint64_t{0};
    });
    install("_readlinkat", [this](auto& s, auto& m) {
        if (!s.x[3]) throw ServiceError{einval};
        if (is_random_device(path_at(m, s.x[0], s.x[1]))) throw ServiceError{einval};
        std::string value;
        if (const auto record = libsystem::symlink_record(path_at(m, s.x[0], s.x[1]))) value = *record;
        else {
            std::error_code error; const auto target = std::filesystem::read_symlink(path_at(m, s.x[0], s.x[1]), error);
            if (error) throw ServiceError{host_error(error)};
            value = target.is_absolute() ? guest_path_string(target) : path_text(target);
        }
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(value.size(), s.x[3]));
        validate(m, s.x[2], count, true);
        m.write_bytes(s.x[2], std::span(reinterpret_cast<const std::uint8_t*>(value.data()), count)); return static_cast<std::uint64_t>(count);
    });
    install("_symlinkat", [this](auto& s, auto& m) {
        const auto value = text(m, s.x[0]);
        auto target = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
        if (target.has_root_directory() || target.has_root_name()) target = translate_path(value);
        if (is_random_device(target)) throw ServiceError{enosys};
        const auto link = filesystem_path_at(m, s.x[1], s.x[2]);
        if (libsystem::symlink_record(link)) throw ServiceError{eexist};
        std::error_code error; std::filesystem::create_symlink(target, link, error);
        if (!error) return std::uint64_t{0};
        // As symlink(2): a link the host cannot create is a symlink record.
        { std::error_code existing; if (std::filesystem::exists(std::filesystem::symlink_status(link, existing))) throw ServiceError{eexist}; }
        { std::error_code parent; if (!std::filesystem::is_directory(link.parent_path(), parent)) throw ServiceError{enoent}; }
        if (value.empty() || !libsystem::add_symlink_record(link, value)) throw ServiceError{host_error(error)};
        return std::uint64_t{0};
    });
    // Reuse the established checked Darwin layouts by calling the existing
    // implementations with a temporary guest pathname, without metadata locks.
    const auto call_path = [this](auto& memory, std::string_view target, const std::filesystem::path& name,
        std::uint64_t argument) {
        const auto value = guest_path_string(name);
        const auto temporary = context.allocate_guest(value.size() + 1);
        try {
            put_text(memory, temporary, value);
            const std::array<std::uint64_t, 2> arguments{temporary, argument};
            const auto result = EngineAdapter(context.calling_engine()).call(context.resolve(system_library, target)->address, arguments);
            context.release_guest(temporary); return result;
        } catch (...) { context.release_guest(temporary); throw; }
    };
    install("_fstatat", [this, call_path](auto& s, auto& m) {
        if (s.x[3] != 0 && s.x[3] != 0x20) throw ServiceError{einval};
        return call_path(m, s.x[3] ? "_lstat" : "_stat", path_at(m, s.x[0], s.x[1]), s.x[2]);
    });
    install("_fstatvfs", [this, call_path](auto& s, auto& m) { return call_path(m, "_statvfs", file(static_cast<int>(s.x[0]))->path, s.x[1]); });
    install("_futimes", [this, call_path](auto& s, auto& m) { return call_path(m, "_utimes", file(static_cast<int>(s.x[0]))->path, s.x[1]); });
}

void DarwinServices::Impl::install_file_services() {
    install("_mkstemp", [this](auto& s, auto& m) { return create_temporary(m, s.x[0], 0, false); });
    install("_mkstemps", [this](auto& s, auto& m) {
        const auto suffix = static_cast<std::int32_t>(s.x[1]);
        if (suffix < 0) throw ServiceError{einval};
        return create_temporary(m, s.x[0], static_cast<std::uint64_t>(suffix), false);
    });
    install("_mkdtemp", [this](auto& s, auto& m) { return create_temporary(m, s.x[0], 0, true); }, 0);
    install("_tmpfile", [this](auto&, auto&) {
        const auto directory = translate_path(temporary_directory());
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            int descriptor;
            try { descriptor = private_temporary_file(directory / ("alchemy-tmp-" + temporary_suffix()), true); }
            catch (const ServiceError& error) { if (error.value == eexist) continue; throw; }
            try { return new_stream(descriptor); }
            catch (...) { static_cast<void>(close(descriptor)); throw; }
        }
        throw ServiceError{eexist};
    }, 0);
    install("_dirfd", [this](auto& s, auto&) {
        int descriptor;
        { std::lock_guard lock(mutex); const auto found = directories.find(s.x[0]);
          if (found == directories.end() || !found->second.descriptor) throw ServiceError{ebadf};
          descriptor = *found->second.descriptor;
        }
        const auto record = file(descriptor);
        std::lock_guard lock(record->mutex);
        if (record->closed) throw ServiceError{ebadf};
        return static_cast<std::uint64_t>(descriptor);
    });
    install("_flock", [this](auto& s, auto&) { return flock(static_cast<int>(s.x[0]), static_cast<std::uint32_t>(s.x[1])); });
    // Actual guest signal delivery is a separate capability. Shutdown cancels
    // waits with ECANCELED; it must not be reported as a delivered signal.
    install("_sleep", [this](auto& s, auto&) {
        const auto seconds = static_cast<std::uint32_t>(s.x[0]);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        for (;;) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) return std::uint64_t{0};
            if (context.requested_exit()) {
                context.set_errno(89);
                return static_cast<std::uint64_t>(std::chrono::ceil<std::chrono::seconds>(remaining).count());
            }
            precise_sleep_for(std::min(remaining,
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(10))));
        }
    });
    install("_usleep", [this](auto& s, auto&) {
        perf::Scope timed(perf::guest_sleep_ticks, perf::guest_sleeps);
        if (perf::enabled() && perf::is_main_thread())
            perf::count_key("usleep on the main thread (us requested)", std::to_string(static_cast<std::uint32_t>(s.x[0])));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<std::uint32_t>(s.x[0]));
        for (;;) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) return std::uint64_t{0};
            if (context.requested_exit()) throw ServiceError{89};
            precise_sleep_for(std::min(remaining,
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(10))));
        }
    });
}

// statfs/fstatfs, file times, copyfile/renamex_np, readdir_r, extended
// attributes and the default malloc zone.
void DarwinServices::Impl::install_libsystem_files() {
    // struct statfs (64-bit inodes): f_bsize @0, f_iosize @4, f_blocks @8, f_bfree @16, f_bavail @24,
    // f_files @32, f_ffree @40, f_fsid @48, f_owner @56, f_type @60, f_flags @64, f_fssubtype @68,
    // f_fstypename[16] @72, f_mntonname[1024] @88, f_mntfromname[1024] @1112, f_flags_ext @2136; 2168 bytes.
    const auto statfs = [this](transmute::GuestMemory& m, const std::filesystem::path& name, std::uint64_t output) {
        if (is_random_device(name)) throw ServiceError{enosys};
        validate(m, output, 2168, true);
        std::error_code error;
        const auto space = std::filesystem::space(name, error);
        if (error) throw ServiceError{host_error(error)};
        m.fill_bytes(output, 0, 2168);
        m.write32(output, 4096); m.write32(output + 4, 1024 * 1024);
        m.write64(output + 8, space.capacity / 4096); m.write64(output + 16, space.free / 4096); m.write64(output + 24, space.available / 4096);
        m.write64(output + 32, UINT32_MAX); m.write64(output + 40, UINT32_MAX); m.write32(output + 56, 1000);
        std::string type = "ntfs", from, mounted = "/";
        std::uint32_t flags = 0x1000; // MNT_LOCAL
        if (const auto volume = host::files::volume(path_text(name))) {
            type.clear();
            for (const auto c : volume->filesystem) type += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : (static_cast<unsigned char>(c) < 128 ? c : '?'));
            m.write32(output + 48, volume->serial);
            if (volume->read_only) flags |= 0x1; // MNT_RDONLY
            if (volume->kind == host::files::VolumeKind::network) flags &= ~0x1000U;
            const auto root_path = utf8_path(volume->root);
            from = path_text(root_path);
            mounted = root_path.root_path() == root.root_path() ? "/" : guest_path_string(root_path);
            while (mounted.size() > 1 && mounted.back() == '/') mounted.pop_back();
        }
        m.write32(output + 64, flags);
        put_text(m, output + 72, type.substr(0, 15));
        put_text(m, output + 88, mounted.substr(0, 1023));
        put_text(m, output + 1112, from.substr(0, 1023));
        return std::uint64_t{0};
    };
    install("_statfs", [this, statfs](auto& s, auto& m) { return statfs(m, path(m, s.x[0]), s.x[1]); });
    install("_fstatfs", [this, statfs](auto& s, auto& m) { return statfs(m, file(static_cast<int>(s.x[0]))->path, s.x[1]); });
    for (const auto alias : {"_statfs$INODE64", "_fstatfs$INODE64"})
        context.install_export(std::string(system_library), alias, *context.resolve(system_library, std::string_view(alias).substr(0, std::string_view(alias).find('$'))));
    // The mounted file systems: the host volume that holds "/" first, then every other host
    // volume (each mounted on its /Volumes/<name> guest path). Volumes that statfs cannot
    // read (an empty card reader) are left out, as an unmounted device is.
    const auto mounted = [this] {
        std::vector<std::filesystem::path> result{root};
        if (const auto roots = host::files::volume_roots())
            for (const auto& value : *roots) {
                const auto volume_root = utf8_path(value);
                if (volume_root.root_path() != root.root_path()) result.push_back(volume_root);
            }
        return result;
    };
    // Fills `count` statfs records at `output`; returns how many were written.
    const auto fill_mounts = [this, statfs, mounted](transmute::GuestMemory& m, std::uint64_t output, std::uint64_t count) {
        std::uint64_t written = 0;
        for (const auto& volume_root : mounted()) {
            if (written == count) break;
            try { statfs(m, volume_root, output + written * 2168); ++written; }
            catch (const ServiceError&) {}
        }
        return written;
    };
    const auto mount_count = [this, statfs, mounted](transmute::GuestMemory& m) {
        const auto scratch = context.allocate_guest(2168);
        std::uint64_t count = 0;
        for (const auto& volume_root : mounted()) {
            try { statfs(m, volume_root, scratch); ++count; } catch (const ServiceError&) {}
        }
        context.release_guest(scratch);
        return count;
    };
    // int getfsstat(struct statfs* buf, int bufsize, int mode): NULL counts the file systems.
    install("_getfsstat", [fill_mounts, mount_count](auto& s, auto& m) {
        const auto size = static_cast<std::int32_t>(s.x[1]);
        if (!s.x[0]) return mount_count(m);
        if (size < 0) throw ServiceError{einval};
        return fill_mounts(m, s.x[0], static_cast<std::uint64_t>(size) / 2168);
    });
    // int getmntinfo(struct statfs** mntbufp, int flags): one buffer per process, replaced by the
    // next call (as Darwin's, which callers must not free); getmntinfo_r_np's is the caller's to free.
    const auto mount_buffer = std::make_shared<std::pair<std::uint64_t, std::uint64_t>>();   // address, capacity in records
    const auto mount_info = [this, fill_mounts, mount_count](transmute::GuestMemory& m, std::uint64_t out, bool owned,
                                                              std::shared_ptr<std::pair<std::uint64_t, std::uint64_t>> shared) {
        validate(m, out, 8, true);
        const auto count = mount_count(m);
        std::uint64_t buffer{};
        if (owned) {
            buffer = allocate(std::max<std::uint64_t>(count, 1) * 2168);   // the caller free()s it
        } else {
            std::lock_guard lock(mutex);
            if (shared->second < count) {
                if (shared->first) context.release_guest(shared->first);
                shared->first = context.allocate_guest(std::max<std::uint64_t>(count, 1) * 2168);
                shared->second = std::max<std::uint64_t>(count, 1);
            }
            buffer = shared->first;
        }
        const auto written = fill_mounts(m, buffer, count);
        m.write64(out, buffer);
        return written;
    };
    install("_getmntinfo", [mount_info, mount_buffer](auto& s, auto& m) { return mount_info(m, s.x[0], false, mount_buffer); }, 0);
    install("_getmntinfo_r_np", [mount_info](auto& s, auto& m) { return mount_info(m, s.x[0], true, {}); }, 0);

    // Times are nanoseconds since the Unix epoch; empty keeps the existing value.
    const auto set_times = [](const std::filesystem::path& name, std::optional<std::int64_t> access, std::optional<std::int64_t> modified) {
        if (const auto set = host::files::set_times(path_text(name), access, modified); !set) throw ServiceError{file_failure(set.error())};
    };
    const auto now = [] { return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); };
    // timespec[2] with UTIME_NOW (-1) and UTIME_OMIT (-2) in tv_nsec.
    const auto timespecs = [now](transmute::GuestMemory& m, std::uint64_t times) {
        std::array<std::optional<std::int64_t>, 2> result{now(), now()};
        if (!times) return result;
        validate(m, times, 32, false);
        for (unsigned index = 0; index < 2; ++index) {
            const auto seconds = std::bit_cast<std::int64_t>(m.read64(times + index * 16));
            const auto nanos = std::bit_cast<std::int64_t>(m.read64(times + index * 16 + 8));
            if (nanos == -2) result[index] = std::nullopt;
            else if (nanos == -1) result[index] = now();
            else if (nanos < 0 || nanos >= 1'000'000'000 || seconds > INT64_MAX / 1'000'000'000 - 1 || seconds < INT64_MIN / 1'000'000'000 + 1) throw ServiceError{einval};
            else result[index] = seconds * 1'000'000'000 + nanos;
        }
        return result;
    };
    install("_futimens", [this, set_times, timespecs](auto& s, auto& m) {
        const auto times = timespecs(m, s.x[1]);
        set_times(file(static_cast<int>(s.x[0]))->path, times[0], times[1]);
        return std::uint64_t{0};
    });
    install("_utimensat", [this, set_times, timespecs](auto& s, auto& m) {
        if (s.x[3] & ~0x20ULL) throw ServiceError{einval}; // AT_SYMLINK_NOFOLLOW only
        const auto times = timespecs(m, s.x[2]);
        set_times(path_at(m, s.x[0], s.x[1]), times[0], times[1]);
        return std::uint64_t{0};
    });
    install("_utime", [this, set_times, now](auto& s, auto& m) {
        // struct utimbuf {time_t actime; time_t modtime}; NULL means now.
        std::optional<std::int64_t> access = now(), modified = access;
        if (s.x[1]) {
            validate(m, s.x[1], 16, false);
            const auto first = std::bit_cast<std::int64_t>(m.read64(s.x[1])), second = std::bit_cast<std::int64_t>(m.read64(s.x[1] + 8));
            if (first > INT64_MAX / 1'000'000'000 || second > INT64_MAX / 1'000'000'000 || first < INT64_MIN / 1'000'000'000 || second < INT64_MIN / 1'000'000'000) throw ServiceError{einval};
            access = first * 1'000'000'000; modified = second * 1'000'000'000;
        }
        set_times(filesystem_path(m, s.x[0]), access, modified);
        return std::uint64_t{0};
    });

    install("_renamex_np", [this](auto& s, auto& m) {
        const auto flags = s.x[2];
        if (flags & 2) throw ServiceError{45};          // RENAME_SWAP: no atomic exchange on the host
        if (flags & ~0x4ULL) throw ServiceError{einval}; // RENAME_EXCL is the only other flag
        const auto from = filesystem_path(m, s.x[0]), to = filesystem_path(m, s.x[1]);
        if (!(flags & 4)) { rename_path(from, to); return std::uint64_t{0}; }
        // Without replacing, the host refuses an existing target atomically.
        if (const auto renamed = host::files::rename(path_text(from), path_text(to), false); !renamed) throw ServiceError{file_failure(renamed.error())};
        return std::uint64_t{0};
    });
    install("_renameatx_np", [this](auto& s, auto& m) {
        if (s.x[4] & ~0x4ULL) throw ServiceError{(s.x[4] & 2) ? 45U : einval};
        const auto from = path_at(m, s.x[0], s.x[1]), to = path_at(m, s.x[2], s.x[3]);
        std::error_code error;
        if ((s.x[4] & 4) && std::filesystem::exists(std::filesystem::symlink_status(to, error))) throw ServiceError{eexist};
        rename_path(from, to);
        return std::uint64_t{0};
    });
    // copyfile(from, to, state, flags): data and metadata copies of regular files and trees.
    install("_copyfile", [this](auto& s, auto& m) {
        constexpr std::uint64_t data = 1 << 3, recursive = 1 << 15, exclusive = 1 << 17, move = 1 << 20, unlink = 1 << 21,
            clone_force = 1 << 25, check = 1 << 16;
        const auto flags = s.x[3];
        if (s.x[2]) { std::lock_guard lock(mutex); if (!copy_states.contains(s.x[2])) throw ServiceError{einval}; }
        if (!s.x[0] || !s.x[1]) throw ServiceError{einval}; // descriptor-only forms use fcopyfile
        const auto from = filesystem_path(m, s.x[0]), to = filesystem_path(m, s.x[1]);
        if (s.x[2]) {
            // The state names the paths and counts the bytes copied (copyfile_state_get).
            const auto source = text(m, s.x[0]), destination = text(m, s.x[1]);
            std::lock_guard lock(mutex);
            auto& state = copy_states.at(s.x[2]);
            for (auto [slot, value] : {std::pair{&state.source_name, &source}, std::pair{&state.destination_name, &destination}}) {
                const auto address = context.allocate_guest(value->size() + 1);
                put_text(m, address, *value);
                if (*slot) context.release_guest(*slot);
                *slot = address;
            }
            state.copied = 0; state.cloned = false;
        }
        const auto count_copied = [&] {
            if (!s.x[2]) return;
            std::error_code size_error;
            const auto size = std::filesystem::is_regular_file(to, size_error) ? std::filesystem::file_size(to, size_error) : 0;
            std::lock_guard lock(mutex);
            if (const auto found = copy_states.find(s.x[2]); found != copy_states.end() && !size_error) found->second.copied = static_cast<std::int64_t>(size);
        };
        if (flags & check) throw ServiceError{45};
        if (flags & clone_force) throw ServiceError{45};   // no copy-on-write clones on the host
        std::error_code error;
        const auto source = std::filesystem::status(from, error);
        if (error) throw ServiceError{host_error(error)};
        if ((flags & exclusive) && std::filesystem::exists(std::filesystem::symlink_status(to, error))) throw ServiceError{eexist};
        if (flags & move) { std::filesystem::rename(from, to, error); if (error) throw ServiceError{host_error(error)}; return std::uint64_t{0}; }
        if (flags & unlink) { std::filesystem::remove(to, error); error.clear(); }
        if (std::filesystem::is_directory(source)) {
            if (!(flags & recursive)) throw ServiceError{eisdir};
            std::filesystem::copy(from, to, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
        } else if (flags & data) {
            std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, error);
            if (!error) count_copied();
        } else if (std::filesystem::exists(to, error)) {
            // Metadata only: carry the modification time.
            const auto time = std::filesystem::last_write_time(from, error);
            if (!error) std::filesystem::last_write_time(to, time, error);
        } else throw ServiceError{enoent};
        if (error) throw ServiceError{host_error(error)};
        return std::uint64_t{0};
    });

    install("_readdir_r", [this](auto& s, auto& m) {
        validate(m, s.x[2], 8, true);
        validate(m, s.x[1], 1048, true);
        std::lock_guard lock(mutex);
        const auto found = directories.find(s.x[0]);
        if (found == directories.end()) return static_cast<std::uint64_t>(ebadf);
        auto& directory = found->second;
        if (directory.at == directory.entries.size()) { m.write64(s.x[2], 0); return std::uint64_t{0}; }
        const auto& entry = directory.entries[directory.at++];
        const auto name = path_text(entry.path().filename());
        if (name.size() > 1023) return static_cast<std::uint64_t>(erange);
        const auto address = s.x[1];
        m.fill_bytes(address, 0, 1048);
        m.write64(address, directory.at); m.write64(address + 8, directory.at);
        const auto record_size = static_cast<std::uint16_t>((22 + name.size() + 3) & ~3ULL);
        m.write8(address + 16, static_cast<std::uint8_t>(record_size)); m.write8(address + 17, static_cast<std::uint8_t>(record_size >> 8));
        m.write8(address + 18, static_cast<std::uint8_t>(name.size())); m.write8(address + 19, static_cast<std::uint8_t>(name.size() >> 8));
        std::error_code error;
        const auto status = entry.symlink_status(error);
        m.write8(address + 20, error ? 0 : (std::filesystem::is_directory(status) ? 4 : (std::filesystem::is_symlink(status) ? 10 : 8)));
        put_text(m, address + 21, name);
        m.write64(s.x[2], address);
        return std::uint64_t{0};
    });
    context.install_export(std::string(system_library), "_readdir_r$INODE64", *context.resolve(system_library, "_readdir_r"));

    // Extended attributes are the host's (host::files attributes; on Windows
    // NTFS alternate data streams). Names the host cannot hold (empty, '/',
    // '\\', longer than XATTR_MAXNAMELEN 127) fail with EINVAL / ENAMETOOLONG;
    // a missing attribute is ENOATTR (93). Only position 0 exists (no
    // resource fork); XATTR_NOFOLLOW is accepted (the host link
    // representations carry no attributes of their own), and XATTR_CREATE /
    // XATTR_REPLACE are honoured. Descriptor variants use the file's path;
    // sockets, pipes and queues have none (EPERM). A host without attributes
    // fails with ENOTSUP.
    constexpr std::uint32_t enoattr = 93;
    const auto xattr_file = [this](auto& m, bool descriptor, std::uint64_t argument) -> std::filesystem::path {
        if (!descriptor) return filesystem_path(m, argument);
        const auto record = file(static_cast<int>(argument));
        if (record->socket || record->pipe || record->queue || record->standard >= 0 || record->random_device || record->path.empty()) throw ServiceError{eperm};
        return record->path;
    };
    // The attribute name, checked; the file must exist.
    const auto xattr_name = [](auto& m, const std::filesystem::path& host, std::uint64_t name_address) {
        const auto name = text(m, name_address);
        if (name.empty() || name.find_first_of("/\\") != std::string::npos) throw ServiceError{einval};
        if (name.size() > 127) throw ServiceError{63};
        std::error_code error;
        if (!std::filesystem::exists(host, error)) throw ServiceError{enoent};
        return name;
    };
    const auto attribute_failure = [](host::Error error) { return error == host::Error::not_found ? enoattr : file_failure(error); };
    for (const auto descriptor : {false, true}) {
        // getxattr(path, name, value, size, position, options)
        install(descriptor ? "_fgetxattr" : "_getxattr", [this, descriptor, xattr_file, xattr_name, attribute_failure](auto& s, auto& m) {
            if (static_cast<std::uint32_t>(s.x[4])) throw ServiceError{einval};
            const auto host = xattr_file(m, descriptor, s.x[0]);
            const auto value = host::files::get_attribute(path_text(host), xattr_name(m, host, s.x[1]));
            if (!value) throw ServiceError{attribute_failure(value.error())};
            const auto bytes = static_cast<std::uint64_t>(value->size());
            if (!s.x[2]) return bytes;
            if (bytes > s.x[3]) throw ServiceError{erange};
            validate(m, s.x[2], bytes, true);
            m.write_bytes(s.x[2], std::span(reinterpret_cast<const std::uint8_t*>(value->data()), value->size()));
            return bytes;
        });
        // setxattr(path, name, value, size, position, options)
        install(descriptor ? "_fsetxattr" : "_setxattr", [this, descriptor, xattr_file, xattr_name](auto& s, auto& m) {
            if (static_cast<std::uint32_t>(s.x[4])) throw ServiceError{einval};
            const auto options = static_cast<std::uint32_t>(s.x[5]);
            if (options & ~(1U | 2U | 4U | 0x20U)) throw ServiceError{einval};
            if ((options & 2) && (options & 4)) throw ServiceError{einval};
            const auto host = xattr_file(m, descriptor, s.x[0]);
            const auto name = xattr_name(m, host, s.x[1]);
            // An attribute the host cannot query counts as absent, as before.
            const auto exists = host::files::has_attribute(path_text(host), name).value_or(false);
            if ((options & 2) && exists) throw ServiceError{eexist};
            if ((options & 4) && !exists) throw ServiceError{enoattr};
            if (s.x[3] > transfer_limit) throw ServiceError{7};   // E2BIG
            std::vector<std::uint8_t> value(static_cast<std::size_t>(s.x[3]));
            if (!value.empty()) { validate(m, s.x[2], s.x[3], false); m.read_bytes(s.x[2], value); }
            if (const auto set = host::files::set_attribute(path_text(host), name, std::as_bytes(std::span(value.data(), value.size()))); !set)
                throw ServiceError{file_failure(set.error())};
            return std::uint64_t{0};
        });
        install(descriptor ? "_fremovexattr" : "_removexattr", [this, descriptor, xattr_file, xattr_name, attribute_failure](auto& s, auto& m) {
            const auto host = xattr_file(m, descriptor, s.x[0]);
            if (const auto removed = host::files::remove_attribute(path_text(host), xattr_name(m, host, s.x[1])); !removed)
                throw ServiceError{attribute_failure(removed.error())};
            return std::uint64_t{0};
        });
        // listxattr(path, namebuf, size, options): NUL-separated names.
        install(descriptor ? "_flistxattr" : "_listxattr", [this, descriptor, xattr_file](auto& s, auto& m) {
            const auto host = xattr_file(m, descriptor, s.x[0]);
            const auto listed = host::files::list_attributes(path_text(host));
            if (!listed) throw ServiceError{file_failure(listed.error())};
            std::string names;
            for (const auto& name : *listed) { names += name; names.push_back('\0'); }
            if (!s.x[1]) return static_cast<std::uint64_t>(names.size());
            if (names.size() > s.x[2]) throw ServiceError{erange};
            validate(m, s.x[1], names.size(), true);
            m.write_bytes(s.x[1], std::span(reinterpret_cast<const std::uint8_t*>(names.data()), names.size()));
            return static_cast<std::uint64_t>(names.size());
        });
    }

    install("_malloc_good_size", [](auto& s, auto&) { return std::max<std::uint64_t>(16, (s.x[0] + 15) & ~15ULL); });
}

} // namespace alchemy
