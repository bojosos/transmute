// DarwinServices (include/alchemy/darwin_services.hpp): the libSystem core.
// install_all installs everything in its fixed order (the libc core here,
// then fs/files.cpp, process/identity.cpp, net/sockets.cpp,
// process/process_model.cpp and the other libSystem providers).
#include "alchemy/darwin/libsystem/services_impl.hpp"

namespace alchemy {

void DarwinServices::Impl::install_all() {
    if (installed) return;
    installed = true;
    context.map_guest_path = [this](std::string_view value) {
        try { return translate_path(value); }
        catch (const ServiceError& error) {
            throw std::runtime_error("invalid Darwin pathname '" + std::string(value) + "' (errno " + std::to_string(error.value) + ")");
        }
    };
    context.shared_location = [this](std::uint64_t address) -> std::optional<ProcessContext::SharedLocation> {
        if (!section_mappings.load(std::memory_order_relaxed)) return std::nullopt;
        std::lock_guard lock(mutex);
        auto found = mappings.upper_bound(address);
        if (found == mappings.begin()) return std::nullopt;
        --found;
        if (!found->second.section || address - found->first >= found->second.size) return std::nullopt;
        return ProcessContext::SharedLocation{found->second.object, found->second.file_offset + (address - found->first)};
    };
    context.child_exited = [this](std::uint32_t pid) -> std::optional<bool> {
        std::unique_lock lock(children_mutex);
        const auto found = children.find(pid);
        if (found == children.end()) {
            // Another process of the Alchemy tree (process_tree.hpp): running,
            // or gone (no exit status to keep; nobody reaps it here).
            lock.unlock();
            if (pid != process_id() && process_tree::alive(pid)) return false;
            return std::nullopt;
        }
        if (found->second.reaped) return std::nullopt;
        if (found->second.pseudo_status) return true;
        // The child's host process (launch.cpp's opaque handle) is signalled once it ends.
        return found->second.process &&
               static_cast<bool>(host::process::exit_code(host::Handle{reinterpret_cast<std::uintptr_t>(found->second.process)}));
    };
    check_locale = install_darwin_locale(context);
    // Host path variables become guest paths (/Volumes/C/...). Darwin processes always
    // inherit TMPDIR; without one, the host temporary directory supplies it. A
    // Windows PATH means nothing to the guest: it becomes the Darwin default
    // (where exec finds the built-in sh and env). An Alchemy child receives its
    // parent's guest environment unchanged.
    if (!context.child_launch) {
        for (auto& entry : context.environment) {
            if (!entry.starts_with("PATH=")) continue;
            const auto value = std::string_view(entry).substr(5);
            if (value.find(';') != std::string_view::npos || value.find('\\') != std::string_view::npos ||
                (value.size() >= 2 && value[1] == ':')) entry = "PATH=" + std::string(default_guest_path);
        }
        std::optional<std::string> temporary;
        bool has_tmpdir{};
        for (auto& entry : context.environment) {
            const auto equals = entry.find('=');
            if (equals == std::string::npos) continue;
            const auto key = entry.substr(0, equals);
            auto value = entry.substr(equals + 1);
            if (key != "TMPDIR" && key != "TMP" && key != "TEMP" && key != "HOME") continue;
            if (value.size() >= 3 && value[1] == ':' && (value[2] == '\\' || value[2] == '/') && value.find(';') == std::string::npos &&
                ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z'))) {
                value = guest_path_string(utf8_path(value));
                entry = key + "=" + value;
            }
            if (key == "TMPDIR") has_tmpdir = true;
            else if ((key == "TMP" || key == "TEMP") && !temporary && value.starts_with("/")) temporary = value;
        }
        if (!has_tmpdir && temporary) context.environment.push_back("TMPDIR=" + *temporary + (temporary->ends_with("/") ? "" : "/"));
    }
    environment_cell = context.allocate_guest(8);
    update_environment();
    context.install_export(std::string(system_library), "_environ", {environment_cell, ExportKind::data});
    for (int descriptor = 0; descriptor < 3; ++descriptor) {
        const auto stream_address = new_stream(descriptor);
        standard_streams[static_cast<std::size_t>(descriptor)] = stream_address;
        const auto cell = context.allocate_guest(8);
        context.calling_engine().write64(cell, stream_address);
        const std::array names{"___stdinp", "___stdoutp", "___stderrp"};
        context.install_export(std::string(system_library), names[static_cast<std::size_t>(descriptor)], {cell, ExportKind::data});
    }
    install("___error", [this](auto&, auto&) { return context.current_thread()->errno_address; });
    install("__NSGetEnviron", [this](auto&, auto&) { return environment_cell; });
    // The system malloc zone: malloc_zone_t with adapters over the process allocator; malloc(3) and
    // the zone registry over it are malloc/malloc_zones.cpp.
    // Layout: reserved1, reserved2, size, malloc, calloc, valloc, free, realloc, destroy, zone_name,
    // batch_malloc, batch_free, introspect, version, memalign, free_definite_size, pressure_relief,
    // claimed_address. The members after realloc that need only the heap's public operations
    // (destroy, batch_*, introspect, pressure_relief, claimed_address, version) are filled by
    // install_malloc_zones.
    const auto adapter = [this](const char* name, std::function<std::uint64_t(transmute::CpuState&, transmute::GuestMemory&)> operation) {
        return context.install_function(name, [this, operation = std::move(operation)](auto& s, auto& m) {
            try { s.x[0] = operation(s, m); }
            catch (const ServiceError& error) { context.set_errno(error.value); s.x[0] = 0; }
            catch (const std::bad_alloc&) { context.set_errno(enomem); s.x[0] = 0; }
        }, "alchemy.internal");
    };
    // size(zone, ptr): 0 only for pointers the zone does not own (a malloc(0) block has one usable byte).
    const auto zone_size = adapter("__alchemy_zone_size", [this](auto& s, auto&) {
        const auto block = allocations.find(s.x[1]); return block ? std::max<std::uint64_t>(block->size, 1) : 0;
    });
    const auto zone_malloc = adapter("__alchemy_zone_malloc", [this](auto& s, auto&) { return allocate(s.x[1]); });
    const auto zone_calloc = adapter("__alchemy_zone_calloc", [this](auto& s, auto& m) {
        if (s.x[1] && s.x[2] > UINT64_MAX / s.x[1]) throw ServiceError{enomem};
        const auto address = allocate(s.x[1] * s.x[2]); m.fill_bytes(address, 0, s.x[1] * s.x[2]); return address;
    });
    const auto zone_valloc = adapter("__alchemy_zone_valloc", [this](auto& s, auto&) { return allocate(s.x[1], darwin_arm64_page_size); });
    const auto zone_free = adapter("__alchemy_zone_free", [this](auto& s, auto&) { release(s.x[1]); return std::uint64_t{0}; });
    const auto zone_realloc = adapter("__alchemy_zone_realloc", [this](auto& s, auto& m) {
        if (!s.x[1]) return allocate(s.x[2]);
        const auto old_size = allocation_size(s.x[1], "zone realloc");
        const auto address = allocate(std::max<std::uint64_t>(s.x[2], 1));
        m.copy_bytes(address, s.x[1], std::min(old_size, s.x[2])); release(s.x[1]); return address;
    });
    const auto zone_memalign = adapter("__alchemy_zone_memalign", [this](auto& s, auto&) {
        if (s.x[1] < 8 || (s.x[1] & (s.x[1] - 1))) throw ServiceError{einval};
        return allocate(s.x[2], s.x[1]);
    });
    const auto zone_free_size = adapter("__alchemy_zone_free_definite_size", [this](auto& s, auto&) { release(s.x[1]); return std::uint64_t{0}; });
    malloc_zone = context.allocate_guest(256);
    {
        auto& memory = context.calling_engine();
        memory.fill_bytes(malloc_zone, 0, 256);
        const auto zone_name = context.allocate_guest(32);
        put_text(memory, zone_name, "DefaultMallocZone");
        const std::array<std::pair<unsigned, std::uint64_t>, 9> slots{{{2, zone_size}, {3, zone_malloc}, {4, zone_calloc},
            {5, zone_valloc}, {6, zone_free}, {7, zone_realloc}, {9, zone_name}, {14, zone_memalign}, {15, zone_free_size}}};
        for (const auto& [slot, value] : slots) memory.write64(malloc_zone + slot * 8, value);
        memory.write32(malloc_zone + 13 * 8, 8); // version
    }
    libsystem::install_malloc_zones(context, {
        [this](std::uint64_t size, std::uint64_t alignment) -> std::uint64_t {
            try { return allocate(size, alignment); }
            catch (const ServiceError& error) { context.set_errno(error.value); return 0; }
            catch (const std::bad_alloc&) { context.set_errno(enomem); return 0; }
        },
        [this](std::uint64_t address) { release(address); },
        [this](std::uint64_t address) { return allocations.find(address).has_value(); },
        [this](std::uint64_t address) { const auto block = allocations.find(address); return block ? std::max<std::uint64_t>(block->size, 1) : 0; },
        [this](std::uint64_t& blocks, std::uint64_t& bytes, std::uint64_t& peak) {
            blocks = allocations.count(); bytes = allocations.bytes(); peak = allocations.peak();
        }}, malloc_zone);
    install("_memcpy", [](auto& s, auto& m) { m.copy_bytes(s.x[0], s.x[1], s.x[2]); return s.x[0]; });
    install("_memmove", [](auto& s, auto& m) { m.copy_bytes(s.x[0], s.x[1], s.x[2], true); return s.x[0]; });
    install("_memset", [](auto& s, auto& m) { m.fill_bytes(s.x[0], static_cast<std::uint8_t>(s.x[1]), s.x[2]); return s.x[0]; });
    // Apple's bzero/bcopy leave x0 untouched, and clang's Darwin arm64 lowering relies on it:
    // `return memset(p, 0, n)` compiles to a tail call of bzero that returns p.
    install("_bzero", [](auto& s, auto& m) { m.fill_bytes(s.x[0], 0, s.x[1]); return s.x[0]; });
    install("_bcopy", [](auto& s, auto& m) { m.copy_bytes(s.x[1], s.x[0], s.x[2], true); return s.x[0]; });
    context.install_export(std::string(system_library), "___bzero", *context.resolve(system_library, "_bzero"));
    // memcmp/memchr/strlen/strcmp/strncmp read guest bytes in chunks that
    // never cross a 4 KiB page past the bytes a byte-wise loop would read
    // (the same faults), instead of one checked read per byte.
    install("_memcmp", [](auto& s, auto& m) {
        std::uint8_t a[256], b[256];
        for (std::uint64_t at = 0; at < s.x[2];) {
            const auto chunk = page_chunk(add(s.x[0], at), add(s.x[1], at), s.x[2] - at, sizeof(a));
            if (!chunk_read(m, add(s.x[0], at), std::span(a, chunk)) || !chunk_read(m, add(s.x[1], at), std::span(b, chunk))) {
                // Near the end of a mapping: byte-wise, with the same faults as before.
                for (std::size_t i = 0; i < chunk; ++i) {
                    const auto x = m.read8(add(s.x[0], at + i)), y = m.read8(add(s.x[1], at + i));
                    if (x != y) return static_cast<std::uint64_t>(static_cast<std::int64_t>(x) - y);
                }
                at += chunk;
                continue;
            }
            for (std::size_t i = 0; i < chunk; ++i)
                if (a[i] != b[i]) return static_cast<std::uint64_t>(static_cast<std::int64_t>(a[i]) - b[i]);
            at += chunk;
        }
        return std::uint64_t{0};
    });
    install("_memchr", [](auto& s, auto& m) {
        std::uint8_t bytes[256];
        for (std::uint64_t at = 0; at < s.x[2];) {
            const auto chunk = page_chunk(add(s.x[0], at), add(s.x[0], at), s.x[2] - at, sizeof(bytes));
            if (!chunk_read(m, add(s.x[0], at), std::span(bytes, chunk))) {
                for (std::size_t i = 0; i < chunk; ++i)
                    if (m.read8(add(s.x[0], at + i)) == static_cast<std::uint8_t>(s.x[1])) return add(s.x[0], at + i);
                at += chunk;
                continue;
            }
            if (const auto* found = static_cast<const std::uint8_t*>(std::memchr(bytes, static_cast<int>(s.x[1] & 0xff), chunk)))
                return add(s.x[0], at + static_cast<std::uint64_t>(found - bytes));
            at += chunk;
        }
        return std::uint64_t{0};
    });
    install("_strlen", [](auto& s, auto& m) {
        bool terminated = false;
        std::size_t length = 0;
        try { length = m.string_length(s.x[0], 1, string_limit, &terminated); }
        catch (const std::runtime_error&) { throw ServiceError{efault}; }
        if (!terminated) throw std::runtime_error("Darwin string exceeds its checked length limit");
        return static_cast<std::uint64_t>(length);
    });
    // maxlen only bounds the scan and never fails: strnlen(p, SIZE_MAX) is
    // strlen(p) (Dawn measures null-terminated WGPUStringViews this way). A
    // string longer than the checked limit is an explicit stop, as in strlen.
    install("_strnlen", [](auto& s, auto& m) {
        const auto maximum = std::min<std::uint64_t>(s.x[1], transfer_limit);
        bool terminated = false;
        std::size_t length = 0;
        try { length = m.string_length(s.x[0], 1, static_cast<std::size_t>(maximum), &terminated); }
        catch (const std::runtime_error&) { throw ServiceError{efault}; }
        if (!terminated && maximum < s.x[1]) throw std::runtime_error("Darwin strnlen string exceeds its checked length limit");
        return static_cast<std::uint64_t>(length);
    });
    for (const auto bounded : {false, true}) {
        install(bounded ? "_strncmp" : "_strcmp", [bounded](auto& s, auto& m) {
            const auto maximum = bounded ? s.x[2] : string_limit;
            std::uint8_t a[128], b[128];
            for (std::uint64_t at = 0; at < maximum;) {
                const auto chunk = page_chunk(add(s.x[0], at), add(s.x[1], at), maximum - at, sizeof(a));
                if (!chunk_read(m, add(s.x[0], at), std::span(a, chunk)) || !chunk_read(m, add(s.x[1], at), std::span(b, chunk))) {
                    for (std::size_t i = 0; i < chunk; ++i) {
                        const auto x = m.read8(add(s.x[0], at + i)), y = m.read8(add(s.x[1], at + i));
                        if (x != y) return static_cast<std::uint64_t>(static_cast<std::int64_t>(x) - y);
                        if (!x) return std::uint64_t{0};
                    }
                    at += chunk;
                    continue;
                }
                for (std::size_t i = 0; i < chunk; ++i) {
                    if (a[i] != b[i]) return static_cast<std::uint64_t>(static_cast<std::int64_t>(a[i]) - b[i]);
                    if (!a[i]) return std::uint64_t{0};
                }
                at += chunk;
            }
            if (!bounded) throw std::runtime_error("Darwin strcmp string exceeds limit");
            return std::uint64_t{0};
        });
    }
    install("_strcpy", [](auto& s, auto& m) { put_text(m, s.x[0], text(m, s.x[1])); return s.x[0]; });
    install("_strncpy", [](auto& s, auto& m) {
        const auto count = bounded_size(s.x[2]);
        const auto bytes = m.read_string(s.x[1], 1, count);
        m.write_bytes(s.x[0], bytes);
        if (bytes.size() < count) m.fill_bytes(add(s.x[0], bytes.size()), 0, count - bytes.size());
        return s.x[0];
    });
    install("_strcat", [](auto& s, auto& m) { const auto first = text(m, s.x[0]); put_text(m, add(s.x[0], first.size()), text(m, s.x[1])); return s.x[0]; });
    install("_strlcpy", [](auto& s, auto& m) { const auto value = text(m, s.x[1]); if (s.x[2]) put_text(m, s.x[0], std::string_view(value.data(), static_cast<std::size_t>(std::min<std::uint64_t>(value.size(), s.x[2] - 1)))); return static_cast<std::uint64_t>(value.size()); });
    install("_strlcat", [](auto& s, auto& m) { const auto source = text(m, s.x[1]); const auto target = m.read_string(s.x[0], 1, static_cast<std::size_t>(std::min<std::uint64_t>(s.x[2], transfer_limit))); if (target.size() < s.x[2]) put_text(m, add(s.x[0], target.size()), std::string_view(source.data(), static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), s.x[2] - target.size() - 1)))); return static_cast<std::uint64_t>(source.size() + target.size()); });
    for (const auto reverse : {false, true}) install(reverse ? "_strrchr" : "_strchr", [reverse](auto& s, auto& m) {
        const auto value = text(m, s.x[0]);
        if (!(s.x[1] & 255)) return add(s.x[0], value.size());
        const auto found = reverse ? value.rfind(static_cast<char>(s.x[1])) : value.find(static_cast<char>(s.x[1]));
        return found == std::string::npos ? 0 : add(s.x[0], found);
    });
    install("_strstr", [](auto& s, auto& m) { const auto source = text(m, s.x[0]), target = text(m, s.x[1]); const auto found = source.find(target); return found == std::string::npos ? 0 : add(s.x[0], found); });
    install("_strdup", [this](auto& s, auto& m) { const auto value = text(m, s.x[0]); const auto address = allocate(value.size() + 1); put_text(m, address, value); return address; }, 0);
    install("_atoi", [](auto& s, auto& m) { const auto value = text(m, s.x[0]); return static_cast<std::uint64_t>(std::strtoll(value.c_str(), nullptr, 10)); });
    for (const auto unsigned_value : {false, true}) install(unsigned_value ? "_strtoull" : "_strtoll", [this, unsigned_value](auto& s, auto& m) {
        const auto source = text(m, s.x[0]);
        if (s.x[2] != 0 && (s.x[2] < 2 || s.x[2] > 36)) throw ServiceError{einval};
        char* end = nullptr;
        errno = 0;
        const auto value = unsigned_value ? std::strtoull(source.c_str(), &end, static_cast<int>(s.x[2])) : static_cast<unsigned long long>(std::strtoll(source.c_str(), &end, static_cast<int>(s.x[2])));
        if (s.x[1]) m.write64(s.x[1], add(s.x[0], static_cast<std::size_t>(end - source.c_str())));
        if (errno) context.set_errno(host_error(errno));
        return static_cast<std::uint64_t>(value);
    });
    for (const auto& [alias, target] : std::array<std::pair<const char*, const char*>, 4>{{{"_strtol", "_strtoll"}, {"_strtoul", "_strtoull"}, {"_atol", "_atoi"}, {"_atoll", "_atoi"}}})
        context.install_export(std::string(system_library), alias, *context.resolve(system_library, target));
    install("_open", [this](auto& s, auto& m) { return static_cast<std::uint64_t>(open(m, s.x[0], s.x[1], (s.x[1] & 0x200) ? m.read64(s.sp) : 0666)); });
    install("_close", [this](auto& s, auto&) { return close(static_cast<int>(s.x[0])); });
    install("_read", [this](auto& s, auto& m) { return read(m, static_cast<int>(s.x[0]), s.x[1], s.x[2]); });
    install("_write", [this](auto& s, auto& m) { return write(m, static_cast<int>(s.x[0]), s.x[1], s.x[2]); });
    install("_lseek", [this](auto& s, auto&) { return seek(static_cast<int>(s.x[0]), s.x[1], static_cast<int>(s.x[2])); });
    for (const auto target : {"_read", "_write", "_open", "_close", "_lseek"})
        context.install_export(std::string(system_library), std::string(target) + "$NOCANCEL", *context.resolve(system_library, target));
    install("_fstat", [this](auto& s, auto& m) { return stat(m, s.x[0], s.x[1], true); });
    install("_stat", [this](auto& s, auto& m) { return stat(m, s.x[0], s.x[1], false); });
    for (const auto target : {"stat", "fstat"}) for (const auto suffix : {"64", "$INODE64"}) {
        const auto original = std::string("_") + target;
        context.install_export(std::string(system_library), original + suffix, *context.resolve(system_library, original));
    }
    install("_fopen", [this](auto& s, auto& m) {
        const auto mode = text(m, s.x[1], 16);
        if (mode.empty() || std::string_view("rwa").find(mode[0]) == std::string_view::npos) throw ServiceError{einval};
        bool plus = false, exclusive = false, cloexec = false;
        // Darwin (FreeBSD __sflags) stops at the first unknown character, so
        // modes such as "wt" or "rb,ccs=UTF-8" are accepted.
        for (std::size_t at = 1; at < mode.size(); ++at) {
            if (mode[at] == '+') plus = true;
            else if (mode[at] == 'x') exclusive = true;
            else if (mode[at] == 'e') cloexec = true;
            else if (mode[at] != 'b') break;
        }
        if (exclusive && mode[0] != 'w') throw ServiceError{einval};
        std::uint64_t flags = plus ? 2 : (mode[0] == 'r' ? 0 : 1);
        if (mode[0] == 'w') flags |= 0x200 | 0x400;
        if (mode[0] == 'a') flags |= 0x200 | 8;
        if (exclusive) flags |= 0x800;
        if (cloexec) flags |= 0x1000000;
        const auto descriptor = open(m, s.x[0], flags, 0666);
        try { return new_stream(descriptor); }
        catch (...) { static_cast<void>(close(descriptor)); throw; }
    }, 0);
    install("_fdopen", [this](auto& s, auto& m) {
        const auto record = file(static_cast<int>(s.x[0]));
        const auto mode = text(m, s.x[1], 16);
        if (mode.empty() || std::string_view("rwa").find(mode[0]) == std::string_view::npos || (mode[0] == 'r' && !record->readable) ||
            (mode[0] != 'r' && !record->writable)) throw ServiceError{einval};
        return new_stream(static_cast<int>(s.x[0]));
    }, 0);
    install("_fclose", [this](auto& s, auto& m) {
        const auto value = stream(s.x[0]);
        std::lock_guard stream_lock(*value.lock);
        if (value.backend) {
            // The stream goes away even when its close function fails.
            std::optional<ServiceError> failure;
            std::uint64_t result = 0;
            try { value.backend->flush(m); result = value.backend->close(m); } catch (const ServiceError& error) { failure = error; }
            { std::lock_guard lock(mutex); streams.erase(s.x[0]); }
            context.release_guest(s.x[0]);
            if (failure) throw *failure;
            return result;
        }
        if (value.descriptor < 3) throw ServiceError{enosys};
        const auto result = close(value.descriptor);
        { std::lock_guard lock(mutex); streams.erase(s.x[0]); }
        context.release_guest(s.x[0]);
        return result;
    });
    install("_fileno", [this](auto& s, auto&) {
        const auto value = stream(s.x[0]);
        if (value.backend) throw ServiceError{ebadf};
        return static_cast<std::uint64_t>(value.descriptor);
    });
    install("_fread", [this](auto& s, auto& m) {
        if (!s.x[1] || !s.x[2]) return std::uint64_t{0};
        if (s.x[2] > UINT64_MAX / s.x[1]) throw ServiceError{einval};
        try { const auto count = read_stream(m, s.x[3], s.x[0], s.x[1] * s.x[2]);
            if (count < s.x[1] * s.x[2]) { std::lock_guard lock(mutex); streams.at(s.x[3]).eof = true; }
            return count / s.x[1];
        } catch (const ServiceError&) { std::lock_guard lock(mutex); streams.at(s.x[3]).error = true; throw; }
    }, 0);
    install("_fwrite", [this](auto& s, auto& m) {
        if (!s.x[1] || !s.x[2]) return std::uint64_t{0};
        if (s.x[2] > UINT64_MAX / s.x[1]) throw ServiceError{einval};
        const auto value = stream(s.x[3]);
        std::lock_guard stream_lock(*value.lock);
        std::uint64_t total = 0;
        try {
            while (total < s.x[1] * s.x[2]) {
                const auto count = stream_write(m, value, add(s.x[0], total), s.x[1] * s.x[2] - total);
                if (!count) throw ServiceError{eio};
                total += count;
            }
        } catch (const ServiceError& error) {
            { std::lock_guard lock(mutex); streams.at(s.x[3]).error = true; }
            context.set_errno(error.value);
        }
        return total / s.x[1];
    }, 0);
    // Descriptor streams are unbuffered; a backend stream publishes its state (open_memstream's
    // buffer and size). fflush(NULL) does that for every stream.
    install("_fflush", [this](auto& s, auto& m) {
        if (s.x[0]) { const auto value = stream(s.x[0]); if (value.backend) { std::lock_guard stream_lock(*value.lock); value.backend->flush(m); } return std::uint64_t{0}; }
        std::vector<Stream> backends;
        { std::lock_guard lock(mutex); for (const auto& entry : streams) if (entry.second.backend) backends.push_back(entry.second); }
        for (const auto& value : backends) { std::lock_guard stream_lock(*value.lock); value.backend->flush(m); }
        return std::uint64_t{0};
    });
    install("_feof", [this](auto& s, auto&) { return static_cast<std::uint64_t>(stream(s.x[0]).eof); });
    install("_ferror", [this](auto& s, auto&) { return static_cast<std::uint64_t>(stream(s.x[0]).error); });
    install("_clearerr", [this](auto& s, auto& m) { static_cast<void>(stream(s.x[0])); std::lock_guard lock(mutex); streams.at(s.x[0]).eof = false; streams.at(s.x[0]).error = false; m.write8(s.x[0] + 16, static_cast<std::uint8_t>(m.read8(s.x[0] + 16) & ~0x60U)); return std::uint64_t{0}; });
    install("_fseek", [this](auto& s, auto& m) { const auto value = stream(s.x[0]); static_cast<void>(stream_seek(m, value, s.x[1], static_cast<int>(s.x[2]))); std::lock_guard lock(mutex); streams.at(s.x[0]).eof = false; streams.at(s.x[0]).pushed = -1; return std::uint64_t{0}; });
    install("_ftell", [this](auto& s, auto& m) { return stream_seek(m, stream(s.x[0]), 0, 1); });
    install("_fgetc", [this](auto& s, auto& m) {
        const auto byte = context.allocate_guest(1);
        try { const auto count = read_stream(m, s.x[0], byte, 1);
            const auto result = count ? static_cast<std::uint64_t>(m.read8(byte)) : UINT64_MAX;
            context.release_guest(byte);
            if (!count) { std::lock_guard lock(mutex); streams.at(s.x[0]).eof = true; }
            return result;
        } catch (...) { context.release_guest(byte); throw; }
    });
    install("_fputc", [this](auto& s, auto& m) { const char byte = static_cast<char>(s.x[0]); static_cast<void>(output_stream(m, s.x[1], std::string_view(&byte, 1))); return s.x[0] & 255; });
    install("_fputs", [this](auto& s, auto& m) { static_cast<void>(output_stream(m, s.x[1], text(m, s.x[0]))); return std::uint64_t{0}; });
    context.install_export(std::string(system_library), "_getc", *context.resolve(system_library, "_fgetc"));
    context.install_export(std::string(system_library), "_putc", *context.resolve(system_library, "_fputc"));
    install("_fgets", [this](auto& s, auto& m) {
        const auto value = stream(s.x[2]);
        std::lock_guard stream_lock(*value.lock);
        const auto count = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[1]));
        if (count <= 0) throw ServiceError{einval};
        validate(m, s.x[0], static_cast<std::uint64_t>(count), true);
        std::uint64_t position = 0;
        try {
            while (position < static_cast<std::uint64_t>(count - 1)) {
                const auto received = read_stream(m, s.x[2], s.x[0] + position, 1);
                if (!received) { std::lock_guard lock(mutex); streams.at(s.x[2]).eof = true; break; }
                ++position;
                if (m.read8(s.x[0] + position - 1) == '\n') break;
            }
        } catch (const ServiceError&) { std::lock_guard lock(mutex); streams.at(s.x[2]).error = true; throw; }
        if (!position && count != 1) return std::uint64_t{0};
        m.write8(s.x[0] + position, 0);
        return s.x[0];
    }, 0);
    // getdelim(3)/getline(3): the line buffer is the caller's malloc block,
    // grown through the process's realloc export.
    const auto getdelim = [this](transmute::CpuState& s, transmute::GuestMemory& m, std::uint64_t delimiter, std::uint64_t file) -> std::uint64_t {
        const auto line_cell = s.x[0], capacity_cell = s.x[1];
        validate(m, line_cell, 8, true);
        validate(m, capacity_cell, 8, true);
        const auto value = stream(file);
        std::lock_guard stream_lock(*value.lock);
        std::string line;
        try {
            const auto byte = context.allocate_guest(1);
            try {
                for (;;) {
                    if (!read_stream(m, file, byte, 1)) { std::lock_guard lock(mutex); streams.at(file).eof = true; break; }
                    const auto c = static_cast<char>(m.read8(byte));
                    line.push_back(c);
                    if (static_cast<unsigned char>(c) == (delimiter & 255)) break;
                }
            } catch (...) { context.release_guest(byte); throw; }
            context.release_guest(byte);
        } catch (const ServiceError&) { std::lock_guard lock(mutex); streams.at(file).error = true; throw; }
        if (line.empty()) return UINT64_MAX;
        auto buffer = m.read64(line_cell);
        auto capacity = buffer ? m.read64(capacity_cell) : 0;
        if (capacity < line.size() + 1) {
            const auto reallocate = context.resolve(system_library, "_realloc");
            if (!reallocate) throw ServiceError{enomem};
            capacity = std::max<std::uint64_t>(line.size() + 1, 128);
            const std::array<std::uint64_t, 2> arguments{buffer, capacity};
            buffer = context.calling_engine().invoke_callback(reallocate->address, arguments);
            if (!buffer) throw ServiceError{enomem};
            m.write64(line_cell, buffer);
            m.write64(capacity_cell, capacity);
        }
        m.write_bytes(buffer, {reinterpret_cast<const std::uint8_t*>(line.data()), line.size()});
        m.write8(buffer + line.size(), 0);
        return line.size();
    };
    install("_getdelim", [getdelim](auto& s, auto& m) { return getdelim(s, m, s.x[2], s.x[3]); });
    install("_getline", [getdelim](auto& s, auto& m) { return getdelim(s, m, '\n', s.x[2]); });
    install("_fgetpos", [this](auto& s, auto& m) { m.write64(s.x[1], stream_seek(m, stream(s.x[0]), 0, 1)); return std::uint64_t{0}; });
    install("_fsetpos", [this](auto& s, auto& m) { static_cast<void>(stream_seek(m, stream(s.x[0]), m.read64(s.x[1]), 0)); std::lock_guard lock(mutex); streams.at(s.x[0]).eof = false; streams.at(s.x[0]).pushed = -1; return std::uint64_t{0}; });
    install("_puts", [this](auto& s, auto& m) { return output_stream(m, standard_streams[1], text(m, s.x[0]) + '\n'); }, UINT32_MAX);
    install("_putchar", [this](auto& s, auto& m) { const char byte = static_cast<char>(s.x[0]); static_cast<void>(output_stream(m, standard_streams[1], std::string_view(&byte, 1))); return s.x[0] & 255; }, UINT32_MAX);
    install("_printf", [this](auto& s, auto& m) { return output_stream(m, standard_streams[1], format(m, s.x[0], s.sp)); }, UINT32_MAX);
    install("_fprintf", [this](auto& s, auto& m) { return output_stream(m, s.x[0], format(m, s.x[1], s.sp)); }, UINT32_MAX);
    install("_vprintf", [this](auto& s, auto& m) { return output_stream(m, standard_streams[1], format(m, s.x[0], s.x[1])); }, UINT32_MAX);
    install("_vfprintf", [this](auto& s, auto& m) { return output_stream(m, s.x[0], format(m, s.x[1], s.x[2])); }, UINT32_MAX);
    for (const auto va : {false, true}) install(va ? "_vsnprintf" : "_snprintf", [va](auto& s, auto& m) {
        const auto value = format(m, s.x[2], va ? s.x[3] : s.sp);
        if (s.x[1]) { const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(value.size(), s.x[1] - 1)); put_text(m, s.x[0], std::string_view(value.data(), count)); }
        return static_cast<std::uint64_t>(value.size());
    }, UINT32_MAX);
    for (const auto va : {false, true}) install(va ? "_vsprintf" : "_sprintf", [va](auto& s, auto& m) {
        const auto value = format(m, s.x[1], va ? s.x[2] : s.sp);
        put_text(m, s.x[0], value);
        return static_cast<std::uint64_t>(value.size());
    }, UINT32_MAX);
    for (const auto va : {false, true}) install(va ? "_vasprintf" : "_asprintf", [this, va](auto& s, auto& m) {
        validate(m, s.x[0], 8, true);
        const auto value = format(m, s.x[1], va ? s.x[2] : s.sp);
        const auto address = allocate(value.size() + 1);
        put_text(m, address, value);
        m.write64(s.x[0], address);
        return static_cast<std::uint64_t>(value.size());
    }, UINT32_MAX);
    install("_getenv", [this](auto& s, auto& m) {
        const auto key = text(m, s.x[0]);
        if (key.empty() || key.find('=') != std::string::npos) return std::uint64_t{0};
        std::lock_guard lock(mutex);
        const auto* child_environment = fork_child_environment();
        for (const auto& value : child_environment ? *child_environment : context.environment) if (value.starts_with(key + '=')) {
            auto found = environment_strings.find(value);
            if (found == environment_strings.end()) {
                const auto address = context.allocate_guest(value.size() + 1);
                put_text(m, address, value);
                found = environment_strings.emplace(value, address).first;
            }
            return add(found->second, key.size() + 1);
        }
        return std::uint64_t{0};
    }, 0);
    install("_setenv", [this](auto& s, auto& m) {
        const auto key = text(m, s.x[0]), value = text(m, s.x[1]);
        if (key.empty() || key.find('=') != std::string::npos) throw ServiceError{einval};
        std::lock_guard lock(mutex);
        // A fork child's environment is its own (exec passes it on); environ stays the parent's.
        auto* child_environment = fork_child_environment(true);
        auto& environment = child_environment ? *child_environment : context.environment;
        auto found = std::find_if(environment.begin(), environment.end(), [&](const auto& item) { return item.starts_with(key + '='); });
        if (found != environment.end()) { if (!s.x[2]) return std::uint64_t{0}; *found = key + '=' + value; }
        else environment.push_back(key + '=' + value);
        if (!child_environment) update_environment();
        return std::uint64_t{0};
    });
    install("_unsetenv", [this](auto& s, auto& m) {
        const auto key = text(m, s.x[0]);
        if (key.empty() || key.find('=') != std::string::npos) throw ServiceError{einval};
        std::lock_guard lock(mutex);
        auto* child_environment = fork_child_environment(true);
        std::erase_if(child_environment ? *child_environment : context.environment, [&](const auto& value) { return value.starts_with(key + '='); });
        if (!child_environment) update_environment();
        return std::uint64_t{0};
    });
    install("_getcwd", [this](auto& s, auto& m) {
        std::filesystem::path directory;
        if (overlay) directory = overlay->cwd;
        else { std::lock_guard lock(mutex); directory = context.working_directory; }
        const auto value = guest_path_string(directory);
        auto address = s.x[0];
        if (!address) address = allocate(std::max<std::uint64_t>(value.size() + 1, s.x[1]));
        else if (s.x[1] <= value.size()) throw ServiceError{erange};
        put_text(m, address, value);
        return address;
    }, 0);
    install("_chdir", [this](auto& s, auto& m) { const auto value = path(m, s.x[0]); if (is_random_device(value)) throw ServiceError{enotdir}; std::error_code error; if (!std::filesystem::is_directory(value, error)) throw ServiceError{error ? host_error(error) : enotdir};
        if (overlay) { overlay->cwd = std::filesystem::absolute(value); return std::uint64_t{0}; }
        std::lock_guard lock(mutex); context.working_directory = std::filesystem::absolute(value); return std::uint64_t{0}; });
    install("_mkdir", [this](auto& s, auto& m) { std::error_code error; const auto result = std::filesystem::create_directory(filesystem_path(m, s.x[0]), error); if (error) throw ServiceError{host_error(error)}; if (!result) throw ServiceError{eexist}; return std::uint64_t{0}; });
    install("_unlink", [this](auto& s, auto& m) {
        const auto value = filesystem_path(m, s.x[0]); std::error_code error;
        if (libsystem::remove_symlink_record(value)) return std::uint64_t{0};
        remove_unix_socket_file(value);
        // A recorded link (junction or hard link) is removed like a symlink.
        if (installed_link(value)) {
            if (!std::filesystem::remove(value, error)) throw ServiceError{error ? host_error(error) : enoent};
            forget_installed_link(value);
            return std::uint64_t{0};
        }
        if (std::filesystem::is_directory(value, error)) throw ServiceError{eisdir};
        if (!std::filesystem::remove(value, error)) throw ServiceError{error ? host_error(error) : enoent};
        return std::uint64_t{0};
    });
    install("_rmdir", [this](auto& s, auto& m) { const auto value = path(m, s.x[0]); if (is_random_device(value)) throw ServiceError{enotdir}; std::error_code error; if (!std::filesystem::is_directory(value, error)) throw ServiceError{error ? host_error(error) : enotdir}; if (!std::filesystem::remove(value, error)) throw ServiceError{error ? host_error(error) : enoent}; return std::uint64_t{0}; });
    install("_rename", [this](auto& s, auto& m) { rename_path(filesystem_path(m, s.x[0]), filesystem_path(m, s.x[1])); return std::uint64_t{0}; });
    install("_opendir", [this](auto& s, auto& m) { return new_directory(m, path(m, s.x[0])); }, 0);
    install("_readdir", [this](auto& s, auto& m) {
        std::lock_guard lock(mutex);
        const auto found = directories.find(s.x[0]);
        if (found == directories.end()) throw ServiceError{ebadf};
        auto& directory = found->second;
        if (directory.at == directory.entries.size()) return std::uint64_t{0};
        const auto& entry = directory.entries[directory.at++];
        const auto name = path_text(entry.path().filename());
        if (name.size() > 1023) throw ServiceError{erange};
        const auto address = directory.buffer;
        m.fill_bytes(address, 0, 1048);
        m.write64(address, directory.at);
        m.write64(address + 8, directory.at);
        const auto record_size = static_cast<std::uint16_t>((22 + name.size() + 3) & ~3ULL);
        m.write8(address + 16, static_cast<std::uint8_t>(record_size));
        m.write8(address + 17, static_cast<std::uint8_t>(record_size >> 8));
        m.write8(address + 18, static_cast<std::uint8_t>(name.size()));
        m.write8(address + 19, static_cast<std::uint8_t>(name.size() >> 8));
        std::error_code error;
        const auto status = entry.symlink_status(error);
        m.write8(address + 20, error ? 0 : (std::filesystem::is_directory(status) ? 4 : (std::filesystem::is_symlink(status) ? 10 : 8)));
        put_text(m, address + 21, name);
        return address;
    }, 0);
    context.install_export(std::string(system_library), "_readdir$INODE64", *context.resolve(system_library, "_readdir"));
    install("_closedir", [this](auto& s, auto&) { std::uint64_t buffer; std::optional<int> descriptor; { std::lock_guard lock(mutex); const auto found = directories.find(s.x[0]); if (found == directories.end()) throw ServiceError{ebadf}; buffer = found->second.buffer; descriptor = found->second.descriptor; directories.erase(found); } if (descriptor) static_cast<void>(close(*descriptor)); context.release_guest(buffer); context.release_guest(s.x[0]); return std::uint64_t{0}; });
    install("_rewinddir", [this](auto& s, auto&) { std::lock_guard lock(mutex); const auto found = directories.find(s.x[0]); if (found == directories.end()) throw ServiceError{ebadf}; found->second.at = 0; return std::uint64_t{0}; });
    // clock_gettime and clock_gettime_nsec_np: install_darwin_support (misc/support.cpp).
    install("_gettimeofday", [this](auto& s, auto& m) {
        if (s.x[0]) validate(m, s.x[0], 16, true);
        if (s.x[1]) validate(m, s.x[1], 8, true);
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (s.x[1]) {
            // Darwin struct timezone {int tz_minuteswest; int tz_dsttime;}: the
            // standard-time offset west of UTC and whether DST is in effect now,
            // from the guest's own localtime_r (so TZ=UTC gives 0/0).
            const auto local = context.resolve(system_library, "_localtime_r");
            if (!local) throw ServiceError{enosys};
            // Scratch from the guest heap (a guest mapping per call cost ~50 us).
            const auto scratch = allocate(64);
            std::int64_t offset; std::uint32_t summer;
            try {
                m.write64(scratch, static_cast<std::uint64_t>(micros / 1000000));
                const std::array<std::uint64_t, 2> arguments{scratch, scratch + 8};
                if (EngineAdapter(context.calling_engine()).call(local->address, arguments) != scratch + 8) throw ServiceError{eoverflow};
                summer = m.read32(scratch + 8 + 32); offset = std::bit_cast<std::int64_t>(m.read64(scratch + 8 + 40));
                release(scratch);
            } catch (...) { release(scratch); throw; }
            const auto dst = std::bit_cast<std::int32_t>(summer) > 0;
            m.write32(s.x[1], static_cast<std::uint32_t>(static_cast<std::int32_t>(-offset / 60 + (dst ? 60 : 0))));
            m.write32(s.x[1] + 4, dst ? 1U : 0U);
        }
        if (s.x[0]) { m.write64(s.x[0], static_cast<std::uint64_t>(micros / 1000000)); m.write32(s.x[0] + 8, static_cast<std::uint32_t>(micros % 1000000)); m.write32(s.x[0] + 12, 0); }
        return std::uint64_t{0};
    });
    install("_time", [](auto& s, auto& m) { const auto value = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()); if (s.x[0]) m.write64(s.x[0], value); return value; });
    install("_nanosleep", [](auto& s, auto& m) { perf::Scope timed(perf::guest_sleep_ticks, perf::guest_sleeps); const auto seconds = m.read64(s.x[0]), nanos = m.read64(s.x[0] + 8); if (seconds > 86400 || nanos >= 1000000000) throw ServiceError{einval}; if (s.x[1]) validate(m, s.x[1], 16, true); precise_sleep_for(std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(seconds) + std::chrono::nanoseconds(nanos))); if (s.x[1]) m.fill_bytes(s.x[1], 0, 16); return std::uint64_t{0}; });
    install("_getpagesize", [](auto&, auto&) { return darwin_arm64_page_size; });
    install("_sysconf", [](auto& s, auto&) { switch (s.x[0]) { case 29: return darwin_arm64_page_size; case 57: case 58: return static_cast<std::uint64_t>(std::max(1U, std::thread::hardware_concurrency())); case 5: return std::uint64_t{4096}; default: throw ServiceError{einval}; } });
    install("_sysctl", [](auto& s, auto& m) { return numeric_sysctl(s, m); });
    install("_sysctlbyname", [](auto& s, auto& m) {
        const auto value = sysctl_named(text(m, s.x[0]));
        if (!value) throw ServiceError{enoent};
        return sysctl_output(m, *value, s.x[1], s.x[2], s.x[3], s.x[4]);
    });
    install("_sysctlnametomib", [](auto& s, auto& m) { return sysctl_name_to_mib(m, text(m, s.x[0]), s.x[1], s.x[2]); });
    install("_getpid", [this](auto&, auto&) { return static_cast<std::uint64_t>(process_id()); });
    install("_getentropy", [](auto& s, auto& m) { if (s.x[1] > 256) throw ServiceError{eio}; random_guest_bytes(m, s.x[0], s.x[1]); return std::uint64_t{0}; });
    install("_arc4random", [](auto&, auto&) {
        std::array<std::uint8_t, 4> bytes{};
        try { cryptographic_random(bytes); }
        catch (const ServiceError&) { throw std::runtime_error("Darwin arc4random host entropy provider failed"); }
        std::uint32_t value{}; std::memcpy(&value, bytes.data(), sizeof(value)); return static_cast<std::uint64_t>(value);
    });
    install("_arc4random_buf", [](auto& s, auto& m) {
        try { random_guest_bytes(m, s.x[0], bounded_size(s.x[1])); }
        catch (const ServiceError& error) { if (error.value == eio) throw std::runtime_error("Darwin arc4random_buf host entropy provider failed"); throw; }
        return std::uint64_t{0};
    });
    install("_atexit", [this](auto& s, auto&) { context.register_finalizer(s.x[0], 0, 0); return std::uint64_t{0}; });
    install("___cxa_atexit", [this](auto& s, auto&) { context.register_finalizer(s.x[0], s.x[1], s.x[2]); return std::uint64_t{0}; });
    install("___cxa_finalize", [this](auto& s, auto&) { context.run_finalizers(s.x[0] ? std::optional(s.x[0]) : std::nullopt); return std::uint64_t{0}; });
    // These replacement bodies are guest ARM64 code. A real SVC exits the run;
    // synthetic host callbacks deliberately have ordinary return semantics.
    const auto cleanup = context.install_function("__alchemy_exit_cleanup",
        [this](auto&, auto&) {
            // exit() on the child side of an emulated fork must not run the
            // parent's atexit handlers or thread cleanup: it is _exit there.
            if (fork_child) return;
            if (context.cleanup_thread) context.cleanup_thread(*context.current_thread());
            context.run_finalizers();
            if (context.cleanup_objects) context.cleanup_objects(*context.current_thread());
        }, "alchemy.internal");
    const std::array<std::uint32_t, 14> exit_instructions{
        0xd10083ffU, // sub sp, sp, #32
        0xa9017bfdU, // stp x29, x30, [sp, #16]
        0xf90003e0U, // str x0, [sp]
        0xd2800010U | (static_cast<std::uint32_t>(cleanup & 0xffff) << 5),
        0xf2a00010U | (static_cast<std::uint32_t>((cleanup >> 16) & 0xffff) << 5),
        0xf2c00010U | (static_cast<std::uint32_t>((cleanup >> 32) & 0xffff) << 5),
        0xf2e00010U | (static_cast<std::uint32_t>((cleanup >> 48) & 0xffff) << 5),
        0xd63f0200U, // blr x16
        0xf94003e0U, // ldr x0, [sp]
        0xa9417bfdU, // ldp x29, x30, [sp, #16]
        0x910083ffU, // add sp, sp, #32
        0xd2800030U, // mov x16, #1
        0xd4001001U, // svc #0x80
        0xd4200000U};
    const auto exit_address = context.allocate_guest(sizeof(exit_instructions));
    for (std::size_t index = 0; index < exit_instructions.size(); ++index)
        context.calling_engine().write32(exit_address + index * 4, exit_instructions[index]);
    context.calling_engine().set_memory_access(exit_address, true, false, true);
    context.install_export(std::string(system_library), "_exit", {exit_address, ExportKind::function});
    const auto raw_exit = context.allocate_guest(12);
    context.calling_engine().write32(raw_exit, 0xd2800030U);
    context.calling_engine().write32(raw_exit + 4, 0xd4001001U);
    context.calling_engine().write32(raw_exit + 8, 0xd4200000U);
    context.calling_engine().set_memory_access(raw_exit, true, false, true);
    context.install_export(std::string(system_library), "__exit", {raw_exit, ExportKind::function});
    // The raw exit stub under a private name, for exec() without fork().
    context.install_export(std::string(system_library), "__alchemy_raw_exit", {raw_exit, ExportKind::function});
    context.install_export(std::string(system_library), "__Exit", {raw_exit, ExportKind::function});
    install("_abort", [](auto&, auto&) -> std::uint64_t {
        throw std::runtime_error("Darwin abort requested SIGABRT; signal delivery is unsupported");
    });
    install("___stack_chk_fail", [](auto&, auto&) -> std::uint64_t { throw std::runtime_error("Darwin guest stack protector failure"); });
    const auto canary = context.allocate_guest(8);
    context.calling_engine().write64(canary, 0x8a13be027ff30000ULL);
    context.install_export(std::string(system_library), "___stack_chk_guard", {canary, ExportKind::data});
    // libm: install_darwin_numeric (misc/numeric_math.cpp).
    install("_mmap", [this](auto& s, auto& m) {
        constexpr std::uint64_t map_shared = 1, map_private = 2, map_anonymous = 0x1000;
        auto flags = s.x[3];
        const auto protection = s.x[2];
        // Shared anonymous memory is shared only with fork children; the fork
        // emulation shares the whole address space anyway and exec'd children
        // start fresh, so it is ordinary anonymous memory here.
        if ((flags & map_shared) && (flags & map_anonymous) && !(flags & map_private)) flags = (flags & ~map_shared) | map_private;
        else if (flags & map_shared) {
            if (const auto mapped = map_shared_file(s.x[0], s.x[1], protection, flags, static_cast<int>(s.x[4]), s.x[5]))
                return mapped;
            flags = map_private;   // read-only, engine without section views: a private copy
        }
        constexpr std::uint64_t map_fixed = 0x10, map_jit = 0x800, hints = 0x40 | 0x200 | 0x400;
        // MAP_FIXED anonymous memory over pages of the calling thread's own
        // stack below its stack pointer replaces them, as on Darwin: they read
        // as zeros with the requested protection. Rust's runtime remaps the
        // lowest page of the main thread's stack this way for its guard page.
        if ((flags & map_fixed) && (flags & map_anonymous) && (flags & map_private) && s.x[1] &&
            s.x[0] % darwin_arm64_page_size == 0 && s.x[1] <= transfer_limit && !(protection & ~7ULL)) {
            const auto [low, high] = context.calling_engine().stack_bounds();
            const auto size = page_round(s.x[1]);
            if (s.x[0] >= low && s.x[0] <= high && size <= high - s.x[0] && s.x[0] + size <= s.sp) {
                protect_engine_range(context.calling_engine(), s.x[0], size, true, true, false);
                m.fill_bytes(s.x[0], 0, size);
                protect_engine_range(context.calling_engine(), s.x[0], size, (protection & 1) != 0, (protection & 2) != 0,
                                     (protection & 4) != 0);
                return s.x[0];
            }
        }
        if ((flags & map_anonymous) && vm_space.enabled()) {
            if (s.x[5] != 0) throw ServiceError{einval};
            return vm_result([&] { return vm_space.map(s.x[0], s.x[1], protection, flags); });
        }
        // A private file copy on engines with VM ranges is anonymous memory
        // (DarwinVmSpace: hints, MAP_FIXED, partial munmap and mprotect) that
        // the file's bytes are read into.
        const bool ranged_copy = vm_space.enabled() && !(flags & map_anonymous);
        const bool fixed_copy = ranged_copy && (flags & map_fixed);
        if ((flags & map_fixed) && !ranged_copy) {
            notice((flags & map_anonymous) ? "mmap(MAP_FIXED) needs an engine with Darwin VM ranges"
                                           : "mmap(MAP_PRIVATE | MAP_FIXED) of a file is unsupported: file copies are placed by Alchemy");
            throw ServiceError{einval};
        }
        flags &= ~map_fixed;
        // A non-null address without MAP_FIXED is only a hint; MAP_NORESERVE,
        // MAP_HASSEMAPHORE, MAP_NOCACHE and (anonymous) MAP_JIT change nothing here.
        flags &= ~(hints | ((flags & map_anonymous) ? map_jit : 0));
        if (!s.x[1] || (protection & ~7ULL) ||
            (flags != map_private && flags != (map_private | map_anonymous))) throw ServiceError{einval};
        if ((flags & map_anonymous) ? (s.x[5] != 0) : (s.x[5] % darwin_arm64_page_size != 0)) throw ServiceError{einval};
        // Address-space reservations (PROT_NONE or beyond the copy limit) need an
        // engine that can reserve without backing and commit through mprotect.
        if ((flags & map_anonymous) && (protection == 0 || s.x[1] > transfer_limit) && s.x[1] <= (1ULL << 40)) {
            if (const auto reserved = reserve_engine_range(context.main_engine(), page_round(s.x[1]),
                    darwin_arm64_page_size, (protection & 1) != 0, (protection & 2) != 0)) {
                std::lock_guard lock(mutex);
                mappings.emplace(*reserved, Mapping{page_round(s.x[1]), true});
                return *reserved;
            }
        }
        if (s.x[1] > transfer_limit) throw ServiceError{einval};
        const auto size = page_round(s.x[1]);
        const auto address = ranged_copy
            ? vm_result([&] { return vm_space.map(s.x[0], size, 3, map_private | map_anonymous | (fixed_copy ? map_fixed : 0)); })
            : context.allocate_guest(size, darwin_arm64_page_size);
        try {
            if (!(flags & map_anonymous)) {
                const auto record = file(static_cast<int>(s.x[4]));
                if (record->directory) throw ServiceError{eisdir};
                if (record->random_device) throw ServiceError{enosys};
                if (!record->readable) throw ServiceError{ebadf};
                std::lock_guard lock(record->mutex);
                if (record->closed) throw ServiceError{ebadf};
                if (!record->native) throw ServiceError{ebadf};
                const auto file_pages = page_round(file_size(record->native));
                if (s.x[5] > file_pages || size > file_pages - s.x[5])
                    throw std::runtime_error("Darwin mmap beyond file pages requires Transmute fault-backed mappings");
                // Duplicated handles share the position: it is saved and restored around the read.
                const auto previous = host::files::seek(record->native, 0, host::files::Whence::current);
                if (!previous) throw ServiceError{crt_failure(previous.error())};
                if (const auto moved = host::files::seek(record->native, static_cast<std::int64_t>(s.x[5]), host::files::Whence::set); !moved)
                    throw ServiceError{crt_failure(moved.error())};
                std::vector<std::uint8_t> bytes(bounded_size(size));
                const auto read = host::files::read(record->native, std::as_writable_bytes(std::span(bytes.data(), bytes.size())));
                if (const auto restored = host::files::seek(record->native, static_cast<std::int64_t>(*previous), host::files::Whence::set); !restored)
                    throw ServiceError{crt_failure(restored.error())};
                if (!read) throw ServiceError{crt_failure(read.error())};
                const auto received = static_cast<std::int64_t>(*read);
                m.write_bytes(address, std::span(bytes.data(), static_cast<std::size_t>(received)));
            }
            if (ranged_copy) {
                if (protection != 3) vm_result([&] { return vm_space.protect(address, size, protection); });
                return address;
            }
            m.set_memory_access(address, (protection & 1) != 0, (protection & 2) != 0, (protection & 4) != 0);
            std::lock_guard lock(mutex);
            mappings.emplace(address, Mapping{size});
            return address;
        } catch (...) {
            if (ranged_copy) { try { vm_space.unmap(address, size); } catch (...) {} }
            else context.release_guest(address);
            throw;
        }
    });
    install("_munmap", [this](auto& s, auto&) {
        if (vm_space.enabled()) return unmap_ranges(s.x[0], s.x[1]);
        if (!s.x[1] || s.x[1] > transfer_limit || s.x[0] % darwin_arm64_page_size) throw ServiceError{einval};
        const auto length = page_round(s.x[1]);
        { std::lock_guard lock(mutex); const auto found = mappings.find(s.x[0]);
          if (found == mappings.end() || found->second.size != length) EngineAdapter::require("range unmapping for partial Darwin munmap");
          if (found->second.engine_reserved || found->second.section) {
              if (found->second.section) section_mappings.fetch_sub(1);
              mappings.erase(found); context.main_engine().unmap_memory(s.x[0]); return std::uint64_t{0}; }
          mappings.erase(found); }
        context.release_guest(s.x[0]);
        return std::uint64_t{0};
    });
    install("_mprotect", [this](auto& s, auto& m) {
        if (vm_space.enabled() && vm_result([&] { return vm_space.protect(s.x[0], s.x[1], s.x[2]); })) return std::uint64_t{0};
        if ((s.x[2] & ~7ULL) || s.x[0] % darwin_arm64_page_size || s.x[1] > transfer_limit) throw ServiceError{einval};
        if (!s.x[1]) return std::uint64_t{0};
        const auto length = page_round(s.x[1]);
        { std::lock_guard lock(mutex); const auto found = mappings.find(s.x[0]); if (found == mappings.end() || found->second.size != length) {
            if (protect_engine_range(context.main_engine(), s.x[0], length, (s.x[2] & 1) != 0, (s.x[2] & 2) != 0, (s.x[2] & 4) != 0)) return std::uint64_t{0};
            EngineAdapter::require("range protection for partial Darwin mprotect"); }
          // A shared file view: write needs a descriptor that was open for writing; no executable file views.
          if (found->second.section && (((s.x[2] & 2) && !found->second.writable_section) || (s.x[2] & 4))) throw ServiceError{eacces}; }
        m.set_memory_access(s.x[0], (s.x[2] & 1) != 0, (s.x[2] & 2) != 0, (s.x[2] & 4) != 0);
        return std::uint64_t{0};
    });
    install_extra();
    install_python_services();
    install_memory_streams();
    install_process_identity();
    install_file_services();
    install_network_services();
    install_libsystem_files();
    install_terminals();
    install_darwin_support(context);
    install_darwin_libsystem_extra(context, {
        [this] { const auto identity = context.current_thread()->identity; std::lock_guard lock(mutex); return signal_masks[identity]; },
        [this](std::uint32_t value) {
            const auto identity = context.current_thread()->identity;
            std::lock_guard lock(mutex); signal_masks[identity] = value & ~((1U << 8) | (1U << 16)); // SIGKILL/SIGSTOP stay unblocked
        }});
    // Replaces libsystem_extra's madvise: MADV_FREE of anonymous memory lets the host drop its pages.
    install("_madvise", [this](auto& s, auto&) {
        if (vm_space.enabled() && vm_result([&] { return vm_space.advise(s.x[0], s.x[1], s.x[2]); })) return std::uint64_t{0};
        // Advice 0-9 never changes visible contents of other memory (MADV_FREE may keep pages).
        if (s.x[2] > 9) throw ServiceError{einval};
        return std::uint64_t{0};
    });
    // Last: replaces the stubs above (popen, pclose) and the ENOSYS process calls.
    install_process_services();
    install_inherited_descriptors();
}

std::uint64_t DarwinServices::Impl::syscall(std::uint64_t number, transmute::CpuState& s, transmute::GuestMemory& m) {
    switch (number) {
    case 1: fork_child_exit(static_cast<std::uint32_t>(s.x[0]));   // only the child side of an emulated fork
    case 3: return read(m, static_cast<int>(s.x[0]), s.x[1], s.x[2]);
    case 4: return write(m, static_cast<int>(s.x[0]), s.x[1], s.x[2]);
    case 5: return static_cast<std::uint64_t>(open(m, s.x[0], s.x[1], s.x[2]));
    case 6: return close(static_cast<int>(s.x[0]));
    case 20: return process_id();
    case 199: return seek(static_cast<int>(s.x[0]), s.x[1], static_cast<int>(s.x[2]));
    case 202: return numeric_sysctl(s, m);
    case 338: return stat(m, s.x[0], s.x[1], false);
    case 339: return stat(m, s.x[0], s.x[1], true);
    default: throw ServiceError{enosys};
    }
}

DarwinServices::DarwinServices(ProcessContext& context) : impl_(std::make_unique<Impl>(context)) { install(); }
DarwinServices::~DarwinServices() = default;
void DarwinServices::install() { impl_->install_all(); }
bool DarwinServices::handles_syscall(std::uint64_t number) const noexcept {
    switch (number) {
    case 1: return impl_->fork_child_handles_exit();   // _exit on the child side of an emulated fork
    case 3: case 4: case 5: case 6: case 20: case 199: case 202: case 338: case 339: return true;
    default: return false;
    }
}
std::optional<std::uint32_t> DarwinServices::supervisor_call(const transmute::SupervisorCall& call,
    transmute::CpuState& state, transmute::GuestMemory& memory) {
    if (call.immediate != 0x80) throw std::runtime_error("unsupported Darwin supervisor-call immediate");
    try {
        state.x[0] = impl_->syscall(state.x[16], state, memory);
        state.x[1] = 0;
        state.nzcv &= ~carry;
    } catch (const ServiceError& error) {
        state.x[0] = error.value;
        state.x[1] = 0;
        state.nzcv |= carry;
    } catch (const std::bad_alloc&) {
        state.x[0] = enomem;
        state.x[1] = 0;
        state.nzcv |= carry;
    } catch (const std::runtime_error&) {
        // Checked bad paths/buffers fault; explicit capability errors retain their diagnosis.
        throw;
    }
    return std::nullopt;
}

} // namespace alchemy
