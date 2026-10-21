#include "cache.hpp"

#include "frontend.hpp"

#include "alchemy/perf.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <type_traits>

#include "alchemy/host/process.hpp"

// MSL compile-result cache (owner: A6). See cache.hpp.
namespace alchemy::msl::detail {
namespace {

constexpr std::uint32_t format_magic = 0x4c534d41;   // "AMSL"
constexpr std::uint32_t format_version = 2;   // 2: BufferBinding::arguments

// One description of every field, used by both directions.
struct Writer {
    std::vector<std::uint8_t> bytes;
    static constexpr bool reading = false;
    bool ok{true};
    void raw(void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
};

struct Reader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t at{};
    static constexpr bool reading = true;
    bool ok{true};
    void raw(void* data, std::size_t size) {
        if (!ok || bytes.size() - at < size) {
            ok = false;
            std::memset(data, 0, size);
            return;
        }
        std::memcpy(data, bytes.data() + at, size);
        at += size;
    }
};

template<typename A, typename T> void io(A& a, T& value) requires std::is_arithmetic_v<T> || std::is_enum_v<T> {
    a.raw(&value, sizeof(value));
}
template<typename A> void io(A& a, std::string& value) {
    auto size = static_cast<std::uint32_t>(value.size());
    io(a, size);
    if constexpr (A::reading) {
        if (!a.ok || size > a.bytes.size()) { a.ok = false; return; }
        value.resize(size);
    }
    if (size) a.raw(value.data(), size);
}
template<typename A, typename T> void io(A& a, std::optional<T>& value) {
    bool present = value.has_value();
    io(a, present);
    if constexpr (A::reading) {
        if (present) value.emplace();
        else value.reset();
    }
    if (present) io(a, *value);
}
template<typename A, typename T, std::size_t N> void io(A& a, std::array<T, N>& value) {
    for (auto& element : value) io(a, element);
}
template<typename A, typename T> void io(A& a, std::vector<T>& value) {
    auto size = static_cast<std::uint32_t>(value.size());
    io(a, size);
    if constexpr (A::reading) {
        if (!a.ok || size > a.bytes.size()) { a.ok = false; return; }
        value.resize(size);
    }
    if constexpr (std::is_arithmetic_v<T>) {
        if (size) a.raw(value.data(), size * sizeof(T));
    } else {
        for (auto& element : value) io(a, element);
    }
}

template<typename A> void io(A& a, Type::Member& v) { io(a, v.name); io(a, v.offset); io(a, v.type); }
template<typename A> void io(A& a, Type& v) {
    io(a, v.kind); io(a, v.scalar); io(a, v.components); io(a, v.columns); io(a, v.array_length); io(a, v.size);
    io(a, v.alignment); io(a, v.packed); io(a, v.name); io(a, v.members); io(a, v.element);
}
template<typename A> void io(A& a, Descriptor& v) { io(a, v.set); io(a, v.binding); io(a, v.type); io(a, v.count); }
template<typename A> void io(A& a, BufferBinding::Argument& v) {
    io(a, v.name); io(a, v.id); io(a, v.count); io(a, v.offset); io(a, v.stride); io(a, v.pointer); io(a, v.type);
}
template<typename A> void io(A& a, BufferBinding& v) {
    io(a, v.index); io(a, v.name); io(a, v.space); io(a, v.access); io(a, v.type); io(a, v.reference);
    io(a, v.argument_buffer); io(a, v.used); io(a, v.function_constant); io(a, v.index_function_constant);
    io(a, v.arguments); io(a, v.argument_length); io(a, v.argument_alignment);
}
template<typename A> void io(A& a, TextureBinding& v) {
    io(a, v.index); io(a, v.name); io(a, v.texture_type); io(a, v.sample_scalar); io(a, v.access);
    io(a, v.array_length); io(a, v.descriptor); io(a, v.used); io(a, v.function_constant);
}
template<typename A> void io(A& a, SamplerBinding& v) { io(a, v.index); io(a, v.name); io(a, v.descriptor); io(a, v.used); }
template<typename A> void io(A& a, ConstexprSampler& v) {
    io(a, v.descriptor); io(a, v.min_filter); io(a, v.mag_filter); io(a, v.mip_filter); io(a, v.address);
    io(a, v.compare_function); io(a, v.border_color); io(a, v.max_anisotropy); io(a, v.lod_min); io(a, v.lod_max);
    io(a, v.normalized_coordinates);
}
template<typename A> void io(A& a, ArgumentBufferSamplers::Member& v) { io(a, v.name); io(a, v.id); io(a, v.count); }
template<typename A> void io(A& a, ArgumentBufferSamplers& v) {
    io(a, v.index); io(a, v.index_function_constant); io(a, v.name); io(a, v.type); io(a, v.members); io(a, v.count);
    io(a, v.descriptor); io(a, v.used);
}
template<typename A> void io(A& a, ColorInput& v) { io(a, v.color); io(a, v.type); io(a, v.name); io(a, v.descriptor); }
template<typename A> void io(A& a, VertexAttribute& v) { io(a, v.index); io(a, v.name); io(a, v.type); io(a, v.used); }
template<typename A> void io(A& a, Varying& v) { io(a, v.name); io(a, v.type); io(a, v.location); io(a, v.interpolation); }
template<typename A> void io(A& a, FragmentOutput& v) { io(a, v.color); io(a, v.index); io(a, v.type); }
template<typename A> void io(A& a, FunctionConstant& v) { io(a, v.index); io(a, v.name); io(a, v.type); io(a, v.required); }
template<typename A> void io(A& a, ThreadgroupMemory& v) { io(a, v.index); io(a, v.name); io(a, v.type); io(a, v.spec_id); }
template<typename A> void io(A& a, Reflection& v) {
    io(a, v.stage); io(a, v.types); io(a, v.argument_table); io(a, v.argument_table_size); io(a, v.buffers);
    io(a, v.textures); io(a, v.samplers); io(a, v.constexpr_samplers); io(a, v.argument_buffers);
    io(a, v.function_constants);
    io(a, v.builtins); io(a, v.stage_in); io(a, v.outputs); io(a, v.inputs); io(a, v.color_outputs); io(a, v.color_inputs); io(a, v.depth);
    io(a, v.writes_sample_mask); io(a, v.writes_stencil); io(a, v.early_fragment_tests); io(a, v.discards);
    io(a, v.max_total_threads_per_threadgroup); io(a, v.threadgroup_memory); io(a, v.uses_simdgroup_operations);
    io(a, v.notes);
}
template<typename A> void io(A& a, Function& v) { io(a, v.name); io(a, v.stage); io(a, v.spirv); io(a, v.reflection); }
template<typename A> void io(A& a, Diagnostic& v) { io(a, v.severity); io(a, v.message); io(a, v.line); io(a, v.column); }
template<typename A> void io(A& a, CompileResult& v) { io(a, v.functions); io(a, v.diagnostics); }

std::string hex(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (auto index = text.size(); index-- > 0; value >>= 4) text[index] = digits[value & 15];
    return text;
}

std::mutex memory_mutex;
std::map<std::string, CompileResult, std::less<>> memory;
std::atomic<std::uint64_t> memory_hits{}, disk_hits{}, misses{}, seed_hits{};

// Read-only seed results (set_seed_directory): <seed>/results/<name>.bin.
std::mutex seed_mutex;
std::filesystem::path seed_root;

std::string result_name(const std::string& key) {
    // The key is long; its two independent hashes name the file and the full
    // key is stored inside and compared on load.
    return hex(fnv1a(key)) + hex(fnv1a(key, 0x84222325cbf29ce4ULL)) + ".bin";
}

std::filesystem::path result_path(const std::string& key) {
    const auto& root = cache_root();
    if (root.empty()) return {};
    return root / "results" / result_name(key);
}

} // namespace

std::string cache_key(std::string_view source, const CompileOptions& options) {
    std::string key = compiler_version();
    auto field = [&](std::string_view text) {
        key += '\x1f';
        key += text;
    };
    field(std::to_string(options.language_major) + "." + std::to_string(options.language_minor));
    for (const auto& [name, value] : options.macros) field(name + "=" + value);
    field(options.fast_math ? "fast" : "precise");
    field(options.preserve_invariance ? "invariant" : "");
    field(std::to_string(options.vulkan_major) + "." + std::to_string(options.vulkan_minor));
    field(std::to_string(options.subgroup_size));
    field(options.debug_info ? "debug" : "");
    field(hex(fnv1a(source)) + ":" + std::to_string(source.size()));
    return key;
}

std::vector<std::uint8_t> serialize(const CompileResult& result) {
    Writer writer;
    auto magic = format_magic;
    auto version = format_version;
    io(writer, magic);
    io(writer, version);
    io(writer, const_cast<CompileResult&>(result));
    return std::move(writer.bytes);
}

std::optional<CompileResult> deserialize(const std::vector<std::uint8_t>& bytes) {
    Reader reader{bytes};
    std::uint32_t magic{}, version{};
    io(reader, magic);
    io(reader, version);
    if (!reader.ok || magic != format_magic || version != format_version) return std::nullopt;
    CompileResult result;
    io(reader, result);
    if (!reader.ok || reader.at != bytes.size()) return std::nullopt;
    return result;
}

// The whole file in one sized read (empty when it cannot be read).
std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size <= 0) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return {};
    return bytes;
}

// A result file: key length, key, serialised result. The key is checked by
// the caller (`expected`, or any key starting with `prefix`).
std::optional<std::pair<std::string, CompileResult>> parse_result_file(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() <= 4) return std::nullopt;
    std::uint32_t length{};
    std::memcpy(&length, bytes.data(), 4);
    if (bytes.size() < 4ULL + length) return std::nullopt;
    std::string key(reinterpret_cast<const char*>(bytes.data() + 4), length);
    std::vector<std::uint8_t> body(bytes.begin() + 4 + length, bytes.end());
    auto result = deserialize(body);
    if (!result) return std::nullopt;
    return std::pair{std::move(key), std::move(*result)};
}

std::optional<CompileResult> cache_lookup(const std::string& key) {
    {
        std::lock_guard lock(memory_mutex);
        const auto found = memory.find(key);
        if (found != memory.end()) {
            ++memory_hits;
            return found->second;
        }
    }
    const auto path = result_path(key);
    if (!path.empty()) {
        if (auto entry = parse_result_file(read_file(path)); entry && entry->first == key) {
            ++disk_hits;
            std::lock_guard lock(memory_mutex);
            memory.emplace(key, entry->second);
            return std::move(entry->second);
        }
    }
    std::filesystem::path seed;
    {
        std::lock_guard lock(seed_mutex);
        seed = seed_root;
    }
    if (!seed.empty()) {
        const auto bytes = read_file(seed / "results" / result_name(key));
        if (auto entry = parse_result_file(bytes); entry && entry->first == key) {
            ++seed_hits;
            perf::add(perf::msl_seed_hits);
            {
                std::lock_guard lock(memory_mutex);
                memory.emplace(key, entry->second);
            }
            // Into the user cache as well: later starts read it from there.
            if (!path.empty()) {
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
                auto staging = path;
                staging += ".seed.tmp";
                {
                    std::ofstream file(staging, std::ios::binary);
                    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                }
                std::filesystem::rename(staging, path, error);
                if (error) std::filesystem::remove(staging, error);
            }
            return std::move(entry->second);
        }
    }
    ++misses;
    return std::nullopt;
}

void set_seed_directory(const std::filesystem::path& directory) {
    std::lock_guard lock(seed_mutex);
    seed_root = directory;
}

std::size_t cache_preload() {
    const auto& root = cache_root();
    if (root.empty()) return 0;
    const auto prefix = compiler_version();
    std::size_t loaded = 0;
    std::error_code error;
    for (const auto& item : std::filesystem::directory_iterator(root / "results", error)) {
        if (item.path().extension() != ".bin") continue;
        auto entry = parse_result_file(read_file(item.path()));
        if (!entry || !std::string_view(entry->first).starts_with(prefix)) continue;
        std::lock_guard lock(memory_mutex);
        if (memory.emplace(std::move(entry->first), std::move(entry->second)).second) ++loaded;
    }
    return loaded;
}

void cache_store(const std::string& key, const CompileResult& result) {
    {
        std::lock_guard lock(memory_mutex);
        memory.insert_or_assign(key, result);
    }
    if (!result.success()) return;   // errors may depend on a missing toolchain or include; recompute
    const auto path = result_path(key);
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    auto body = serialize(result);
    std::vector<std::uint8_t> bytes(4);
    const auto length = static_cast<std::uint32_t>(key.size());
    std::memcpy(bytes.data(), &length, 4);
    bytes.insert(bytes.end(), key.begin(), key.end());
    bytes.insert(bytes.end(), body.begin(), body.end());
    // Write privately and rename, so readers never see a partial file.
    auto staging = path;
    staging += ".tmp" + std::to_string(alchemy::host::process::current_pid()) + "-" +
               std::to_string(alchemy::host::process::current_thread_id());
    {
        std::ofstream file(staging, std::ios::binary);
        if (!file) return;
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            file.close();
            std::filesystem::remove(staging, error);
            return;
        }
    }
    std::filesystem::rename(staging, path, error);
    if (error) std::filesystem::remove(staging, error);
}

CacheStatistics cache_statistics() { return {memory_hits.load(), disk_hits.load(), misses.load(), seed_hits.load()}; }

void clear_memory_cache() {
    std::lock_guard lock(memory_mutex);
    memory.clear();
}

} // namespace alchemy::msl::detail
