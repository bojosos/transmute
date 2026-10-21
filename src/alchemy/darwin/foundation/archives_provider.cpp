#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/compression/bzip2.hpp"
#include "alchemy/darwin/compression/xar.hpp"
#include "alchemy/darwin/security/keys.hpp"

#include <map>
#include <mutex>

// /usr/lib/libbz2.1.0.dylib and /usr/lib/libxar.1.dylib, which Sparkle's
// Autoupdate helper imports for binary-delta updates (QLab itself never
// installs an update under Alchemy; strict binding needs the imports).
//
// libbz2, over bzip2.cpp, with libbzip2's FILE* API semantics and BZ_* codes:
//   BZ2_bzReadOpen / bzRead / bzReadClose: the stream is read from the FILE
//   (through the guest's fread) and decoded at the first bzRead; bzRead
//   returns BZ_STREAM_END with the last bytes. BZ2_bzWriteOpen / bzWrite /
//   bzWriteClose / bzWriteClose64: the data is encoded and written with
//   fwrite at close. BZ2_bzBuffToBuffCompress / Decompress for whole buffers.
//   Errors: BZ_PARAM_ERROR (-2), BZ_DATA_ERROR (-4), BZ_DATA_ERROR_MAGIC (-5),
//   BZ_IO_ERROR (-6), BZ_UNEXPECTED_EOF (-7), BZ_OUTBUFF_FULL (-8).
//
// libxar, over xar.cpp (read, extract and write; files go through the
// guest's libSystem, so paths resolve in the root): xar_open (READ 0 /
// WRITE 1), xar_close (writes the archive), xar_opt_set ("compression":
// gzip, bzip2 or none; other compressions return -1; checksum options are
// recorded, the archive is written with SHA-1), xar_add_frompath (one regular
// file, directory or symbolic link), xar_add_frombuffer, xar_extract_tofile,
// the file iterator, xar_get_path and xar_get_safe_path (malloc'd), xar_prop_get / _set and the
// subdoc functions. Failures return the documented NULL or -1.
namespace alchemy::gui::completion_kit::data {
namespace {

constexpr std::string_view bz2_library = "/usr/lib/libbz2.1.0.dylib";
constexpr std::string_view xar_library = "/usr/lib/libxar.1.dylib";
constexpr std::int32_t bz_ok = 0, bz_stream_end = 4, bz_param_error = -2, bz_data_error = -4, bz_data_error_magic = -5, bz_io_error = -6,
                       bz_unexpected_eof = -7, bz_outbuff_full = -8;
constexpr Address o_rdonly = 0, o_wronly = 1, o_creat = 0x200, o_trunc = 0x400;
constexpr std::uint32_t s_ifmt = 0170000, s_ifdir = 0040000, s_iflnk = 0120000, s_ifreg = 0100000;

struct GuestFiles {
    Support& s;
    Guest g{s};
    std::int64_t signed_result(Address v) { return static_cast<std::int64_t>(v); }
    Address text(const std::string& value, Guest::Scratch& block) {
        g.memory().write_bytes(block.address, std::span(reinterpret_cast<const std::uint8_t*>(value.c_str()), value.size() + 1));
        return block.address;
    }
    std::optional<std::vector<std::uint8_t>> read(const std::string& path) {
        Guest::Scratch name(g, path.size() + 1);
        const auto fd = signed_result(s.call(libsystem_library, "_open", {text(path, name), o_rdonly}));
        if (fd < 0) return std::nullopt;
        std::vector<std::uint8_t> out;
        Guest::Scratch chunk(g, 65536);
        for (;;) {
            const auto got = signed_result(s.call(libsystem_library, "_read", {static_cast<Address>(fd), chunk.address, 65536}));
            if (got < 0) { s.call(libsystem_library, "_close", {static_cast<Address>(fd)}); return std::nullopt; }
            if (got == 0) break;
            const auto at = out.size();
            out.resize(at + static_cast<std::size_t>(got));
            g.memory().read_bytes(chunk.address, std::span(out.data() + at, static_cast<std::size_t>(got)));
        }
        s.call(libsystem_library, "_close", {static_cast<Address>(fd)});
        return out;
    }
    bool write(const std::string& path, std::span<const std::uint8_t> bytes, std::uint32_t mode) {
        Guest::Scratch name(g, path.size() + 1);
        CallRegs open_call;
        open_call.x[0] = text(path, name);
        open_call.x[1] = o_wronly | o_creat | o_trunc;
        open_call.push_variadic(std::uint64_t{mode});
        const auto fd = signed_result(s.call(libsystem_library, "_open", open_call).x0);
        if (fd < 0) return false;
        Guest::Scratch block(g, std::max<std::size_t>(bytes.size(), 1));
        if (!bytes.empty()) g.memory().write_bytes(block.address, bytes);
        std::size_t done = 0;
        bool ok = true;
        while (done < bytes.size()) {
            const auto put = signed_result(s.call(libsystem_library, "_write", {static_cast<Address>(fd), block.address + done, bytes.size() - done}));
            if (put <= 0) { ok = false; break; }
            done += static_cast<std::size_t>(put);
        }
        s.call(libsystem_library, "_close", {static_cast<Address>(fd)});
        if (ok) { Guest::Scratch again(g, path.size() + 1); static_cast<void>(s.call(libsystem_library, "_chmod", {text(path, again), mode})); }
        return ok;
    }
    // st_mode (lstat), or nullopt when the item does not exist.
    std::optional<std::uint32_t> mode_of(const std::string& path) {
        Guest::Scratch name(g, path.size() + 1), st(g, 144);
        if (signed_result(s.call(libsystem_library, "_lstat", {text(path, name), st.address})) != 0) return std::nullopt;
        return g.memory().read32(st.address + 4) & 0xffffu;   // st_mode (16 bits at offset 4)
    }
    std::optional<std::string> link_target(const std::string& path) {
        Guest::Scratch name(g, path.size() + 1), buffer(g, 1025);
        const auto n = signed_result(s.call(libsystem_library, "_readlink", {text(path, name), buffer.address, 1024}));
        if (n < 0) return std::nullopt;
        std::vector<std::uint8_t> raw(static_cast<std::size_t>(n));
        if (n) g.memory().read_bytes(buffer.address, raw);
        return std::string(raw.begin(), raw.end());
    }
    bool mkdir(const std::string& path, std::uint32_t mode) {
        Guest::Scratch name(g, path.size() + 1);
        if (signed_result(s.call(libsystem_library, "_mkdir", {text(path, name), mode})) == 0) return true;
        const auto existing = mode_of(path);
        return existing && (*existing & s_ifmt) == s_ifdir;
    }
    bool symlink(const std::string& target, const std::string& path) {
        Guest::Scratch t(g, target.size() + 1), p(g, path.size() + 1);
        return signed_result(s.call(libsystem_library, "_symlink", {text(target, t), text(path, p)})) == 0;
    }
};

// ---- libbz2 ----
struct BzFile {
    Address file{};
    bool writing{}, decoded{};
    int level{9};
    std::vector<std::uint8_t> data;   // reading: decoded output; writing: input
    std::size_t delivered{};
    std::uint64_t written{};
};

// ---- libxar ----
struct XarArchive {
    std::unique_ptr<xar::Archive> archive;
    bool writing{};
    std::string path;
    std::vector<Address> strings;                    // guest C strings handed out (freed at close)
    std::map<const xar::Node*, Address> files;       // node -> handle
    std::map<const xar::Subdoc*, Address> subdocs;   // subdoc -> handle
};
struct XarIter { XarArchive* archive{}; std::vector<xar::Node*> nodes; std::size_t next{}; };

struct Archives {
    explicit Archives(Support& support) : s(support), files{support} {}
    Support& s;
    GuestFiles files;
    std::mutex mutex;
    std::map<Address, std::unique_ptr<BzFile>> bz;
    std::map<Address, std::unique_ptr<XarArchive>> xars;
    std::map<Address, std::pair<XarArchive*, xar::Node*>> file_handles;
    std::map<Address, std::pair<XarArchive*, xar::Subdoc*>> subdoc_handles;
    std::map<Address, std::unique_ptr<XarIter>> iters;

    Address token() { return s.process.allocate_guest(8); }
    void set_error(Address out, std::int32_t code) { if (out) s.memory().write32(out, static_cast<std::uint32_t>(code)); }
    std::string c_string(Address at) {
        std::string out;
        if (!at) return out;
        for (;; ++at) { const auto c = s.memory().read8(at); if (!c || out.size() > 65536) break; out.push_back(static_cast<char>(c)); }
        return out;
    }
    Address guest_string(XarArchive& x, const std::string& value) {
        const auto at = s.process.allocate_guest(value.size() + 1);
        s.memory().write_bytes(at, std::span(reinterpret_cast<const std::uint8_t*>(value.c_str()), value.size() + 1));
        x.strings.push_back(at);
        return at;
    }
    Address file_handle(XarArchive& x, xar::Node* node) {
        if (!node) return 0;
        if (const auto found = x.files.find(node); found != x.files.end()) return found->second;
        const auto t = token();
        x.files.emplace(node, t);
        file_handles.emplace(t, std::pair{&x, node});
        return t;
    }
    Address subdoc_handle(XarArchive& x, xar::Subdoc* sub) {
        if (!sub) return 0;
        if (const auto found = x.subdocs.find(sub); found != x.subdocs.end()) return found->second;
        const auto t = token();
        x.subdocs.emplace(sub, t);
        subdoc_handles.emplace(t, std::pair{&x, sub});
        return t;
    }
    void close_archive(Address handle) {
        auto found = xars.find(handle);
        if (found == xars.end()) return;
        auto& x = *found->second;
        for (const auto at : x.strings) s.process.release_guest(at);
        for (const auto& [node, t] : x.files) { file_handles.erase(t); s.process.release_guest(t); }
        for (const auto& [sub, t] : x.subdocs) { subdoc_handles.erase(t); s.process.release_guest(t); }
        for (auto it = iters.begin(); it != iters.end(); ++it) if (it->second->archive == &x) it->second->archive = nullptr;
        xars.erase(found);
        s.process.release_guest(handle);
    }
};

std::vector<std::uint8_t> sha1(std::span<const std::uint8_t> bytes) {
    auto digest = security::digest("SHA1", bytes);
    if (!digest) unsupported("libxar: SHA-1 digests (the host digest provider is not available)");
    return *digest;
}

} // namespace

void install_archives(Support& support) {
    auto ar = std::make_shared<Archives>(support);
    ProcessContext::add_provider_library(std::string(bz2_library));
    ProcessContext::add_provider_library(std::string(xar_library));

    // ---- libbz2 ----
    // BZFILE* BZ2_bzReadOpen(int* bzerror, FILE* f, int verbosity, int small, void* unused, int nUnused)
    support.fill_function(bz2_library, "_BZ2_bzReadOpen", [ar](CpuState& s, GuestMemory& m) {
        const auto unused_count = static_cast<std::int32_t>(s.x[5]);
        if (!s.x[1] || static_cast<std::int32_t>(s.x[2]) < 0 || static_cast<std::int32_t>(s.x[2]) > 4 || (s.x[3] & 0xffffffff) > 1 ||
            unused_count < 0 || unused_count > 5000 || (unused_count > 0 && !s.x[4])) {
            ar->set_error(s.x[0], bz_param_error);
            s.x[0] = 0;
            return;
        }
        auto b = std::make_unique<BzFile>();
        b->file = s.x[1];
        if (unused_count > 0) { b->data.resize(static_cast<std::size_t>(unused_count)); m.read_bytes(s.x[4], b->data); }
        const auto handle = ar->token();
        { std::lock_guard lock(ar->mutex); ar->bz.emplace(handle, std::move(b)); }
        ar->set_error(s.x[0], bz_ok);
        s.x[0] = handle;
    });
    // int BZ2_bzRead(int* bzerror, BZFILE* b, void* buf, int len)
    support.fill_function(bz2_library, "_BZ2_bzRead", [ar](CpuState& s, GuestMemory& m) {
        const auto error_out = s.x[0];
        BzFile* b = nullptr;
        { std::lock_guard lock(ar->mutex); if (const auto f = ar->bz.find(s.x[1]); f != ar->bz.end()) b = f->second.get(); }
        const auto len = static_cast<std::int32_t>(s.x[3]);
        if (!b || b->writing || len < 0 || (len > 0 && !s.x[2])) { ar->set_error(error_out, bz_param_error); s.x[0] = 0; return; }
        if (!b->decoded) {
            // The rest of the FILE, then the first stream of it.
            auto input = std::move(b->data);
            Guest::Scratch chunk(ar->files.g, 65536);
            for (;;) {
                const auto got = ar->s.call(libsystem_library, "_fread", {chunk.address, 1, 65536, b->file});
                if (!got) break;
                const auto at = input.size();
                input.resize(at + static_cast<std::size_t>(got));
                m.read_bytes(chunk.address, std::span(input.data() + at, static_cast<std::size_t>(got)));
            }
            if (ar->s.call(libsystem_library, "_ferror", {b->file})) { ar->set_error(error_out, bz_io_error); s.x[0] = 0; return; }
            auto decoded = bzip2::decode(input);
            if (decoded.error != bzip2::Error::none) {
                const auto code = decoded.error == bzip2::Error::magic ? bz_data_error_magic
                                : decoded.error == bzip2::Error::unexpected_end ? bz_unexpected_eof : bz_data_error;
                ar->set_error(error_out, code);
                s.x[0] = 0;
                return;
            }
            b->data = std::move(decoded.output);
            b->decoded = true;
        }
        const auto n = std::min<std::size_t>(static_cast<std::size_t>(len), b->data.size() - b->delivered);
        if (n) m.write_bytes(s.x[2], std::span(b->data.data() + b->delivered, n));
        b->delivered += n;
        ar->set_error(error_out, b->delivered == b->data.size() ? bz_stream_end : bz_ok);
        s.x[0] = n;
    });
    // void BZ2_bzReadClose(int* bzerror, BZFILE* b)
    support.fill_function(bz2_library, "_BZ2_bzReadClose", [ar](CpuState& s, GuestMemory&) {
        bool known = false;
        { std::lock_guard lock(ar->mutex); known = ar->bz.erase(s.x[1]) != 0; }
        if (known) ar->s.process.release_guest(s.x[1]);
        ar->set_error(s.x[0], known ? bz_ok : bz_param_error);
    });
    // BZFILE* BZ2_bzWriteOpen(int* bzerror, FILE* f, int blockSize100k, int verbosity, int workFactor)
    support.fill_function(bz2_library, "_BZ2_bzWriteOpen", [ar](CpuState& s, GuestMemory&) {
        const auto level = static_cast<std::int32_t>(s.x[2]), verbosity = static_cast<std::int32_t>(s.x[3]), work = static_cast<std::int32_t>(s.x[4]);
        if (!s.x[1] || level < 1 || level > 9 || verbosity < 0 || verbosity > 4 || work < 0 || work > 250) {
            ar->set_error(s.x[0], bz_param_error);
            s.x[0] = 0;
            return;
        }
        auto b = std::make_unique<BzFile>();
        b->file = s.x[1];
        b->writing = true;
        b->level = level;
        const auto handle = ar->token();
        { std::lock_guard lock(ar->mutex); ar->bz.emplace(handle, std::move(b)); }
        ar->set_error(s.x[0], bz_ok);
        s.x[0] = handle;
    });
    // void BZ2_bzWrite(int* bzerror, BZFILE* b, void* buf, int len)
    support.fill_function(bz2_library, "_BZ2_bzWrite", [ar](CpuState& s, GuestMemory& m) {
        BzFile* b = nullptr;
        { std::lock_guard lock(ar->mutex); if (const auto f = ar->bz.find(s.x[1]); f != ar->bz.end()) b = f->second.get(); }
        const auto len = static_cast<std::int32_t>(s.x[3]);
        if (!b || !b->writing || len < 0 || (len > 0 && !s.x[2])) { ar->set_error(s.x[0], bz_param_error); return; }
        const auto at = b->data.size();
        b->data.resize(at + static_cast<std::size_t>(len));
        if (len) m.read_bytes(s.x[2], std::span(b->data.data() + at, static_cast<std::size_t>(len)));
        ar->set_error(s.x[0], bz_ok);
    });
    // void BZ2_bzWriteClose64(int* bzerror, BZFILE* b, int abandon, unsigned* in_lo32, unsigned* in_hi32, unsigned* out_lo32, unsigned* out_hi32)
    const auto write_close = [ar](CpuState& s, GuestMemory& m, bool wide) {
        std::unique_ptr<BzFile> b;
        {
            std::lock_guard lock(ar->mutex);
            if (const auto f = ar->bz.find(s.x[1]); f != ar->bz.end() && f->second->writing) { b = std::move(f->second); ar->bz.erase(f); }
        }
        if (!b) { ar->set_error(s.x[0], bz_param_error); return; }
        ar->s.process.release_guest(s.x[1]);
        std::uint64_t out_bytes = 0;
        std::int32_t code = bz_ok;
        if (!(s.x[2] & 0xffffffff)) {
            const auto encoded = bzip2::encode(b->data, b->level);
            Guest::Scratch block(ar->files.g, std::max<std::size_t>(encoded.size(), 1));
            m.write_bytes(block.address, encoded);
            const auto put = ar->s.call(libsystem_library, "_fwrite", {block.address, 1, encoded.size(), b->file});
            if (put != encoded.size()) code = bz_io_error;
            else out_bytes = encoded.size();
        }
        const std::uint64_t in_bytes = b->data.size();
        if (wide) {
            if (s.x[3]) m.write32(s.x[3], static_cast<std::uint32_t>(in_bytes));
            if (s.x[4]) m.write32(s.x[4], static_cast<std::uint32_t>(in_bytes >> 32));
            if (s.x[5]) m.write32(s.x[5], static_cast<std::uint32_t>(out_bytes));
            if (s.x[6]) m.write32(s.x[6], static_cast<std::uint32_t>(out_bytes >> 32));
        } else {
            if (s.x[3]) m.write32(s.x[3], static_cast<std::uint32_t>(in_bytes));
            if (s.x[4]) m.write32(s.x[4], static_cast<std::uint32_t>(out_bytes));
        }
        ar->set_error(s.x[0], code);
    };
    support.fill_function(bz2_library, "_BZ2_bzWriteClose64", [write_close](CpuState& s, GuestMemory& m) { write_close(s, m, true); });
    support.fill_function(bz2_library, "_BZ2_bzWriteClose", [write_close](CpuState& s, GuestMemory& m) { write_close(s, m, false); });
    // int BZ2_bzBuffToBuffCompress(char* dest, unsigned* destLen, char* source, unsigned sourceLen, int blockSize100k, int verbosity, int workFactor)
    support.fill_function(bz2_library, "_BZ2_bzBuffToBuffCompress", [](CpuState& s, GuestMemory& m) {
        const auto level = static_cast<std::int32_t>(s.x[4]);
        if (!s.x[0] || !s.x[1] || (!s.x[2] && s.x[3]) || level < 1 || level > 9) { s.x[0] = static_cast<std::uint32_t>(bz_param_error); return; }
        std::vector<std::uint8_t> input(static_cast<std::uint32_t>(s.x[3]));
        if (!input.empty()) m.read_bytes(s.x[2], input);
        const auto encoded = bzip2::encode(input, level);
        if (encoded.size() > m.read32(s.x[1])) { s.x[0] = static_cast<std::uint32_t>(bz_outbuff_full); return; }
        m.write_bytes(s.x[0], encoded);
        m.write32(s.x[1], static_cast<std::uint32_t>(encoded.size()));
        s.x[0] = bz_ok;
    });
    // int BZ2_bzBuffToBuffDecompress(char* dest, unsigned* destLen, char* source, unsigned sourceLen, int small, int verbosity)
    support.fill_function(bz2_library, "_BZ2_bzBuffToBuffDecompress", [](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1] || (!s.x[2] && s.x[3])) { s.x[0] = static_cast<std::uint32_t>(bz_param_error); return; }
        std::vector<std::uint8_t> input(static_cast<std::uint32_t>(s.x[3]));
        if (!input.empty()) m.read_bytes(s.x[2], input);
        const auto decoded = bzip2::decode(input);
        if (decoded.error != bzip2::Error::none) {
            const auto code = decoded.error == bzip2::Error::magic ? bz_data_error_magic : decoded.error == bzip2::Error::unexpected_end ? bz_unexpected_eof : bz_data_error;
            s.x[0] = static_cast<std::uint32_t>(code);
            return;
        }
        if (decoded.output.size() > m.read32(s.x[1])) { s.x[0] = static_cast<std::uint32_t>(bz_outbuff_full); return; }
        if (!decoded.output.empty()) m.write_bytes(s.x[0], decoded.output);
        m.write32(s.x[1], static_cast<std::uint32_t>(decoded.output.size()));
        s.x[0] = bz_ok;
    });

    // ---- libxar ----
    const auto archive_of = [ar](Address handle) -> XarArchive* {
        const auto f = ar->xars.find(handle);
        return f == ar->xars.end() ? nullptr : f->second.get();
    };
    const auto file_of = [ar](Address handle) -> std::pair<XarArchive*, xar::Node*> {
        const auto f = ar->file_handles.find(handle);
        return f == ar->file_handles.end() ? std::pair<XarArchive*, xar::Node*>{} : f->second;
    };
    const auto subdoc_of = [ar](Address handle) -> std::pair<XarArchive*, xar::Subdoc*> {
        const auto f = ar->subdoc_handles.find(handle);
        return f == ar->subdoc_handles.end() ? std::pair<XarArchive*, xar::Subdoc*>{} : f->second;
    };
    // xar_t xar_open(const char* file, int32_t flags)
    support.fill_function(xar_library, "_xar_open", [ar](CpuState& s, GuestMemory&) {
        const auto path = ar->c_string(s.x[0]);
        const auto flags = static_cast<std::int32_t>(s.x[1]);
        auto x = std::make_unique<XarArchive>();
        x->path = path;
        if (flags == 1) { x->writing = true; x->archive = std::make_unique<xar::Archive>(); }
        else if (flags == 0) {
            const auto bytes = ar->files.read(path);
            if (!bytes) { s.x[0] = 0; return; }
            std::string error;
            x->archive = xar::read(*bytes, error);
            if (!x->archive) { s.x[0] = 0; return; }
        } else { s.x[0] = 0; return; }
        const auto handle = ar->token();
        std::lock_guard lock(ar->mutex);
        ar->xars.emplace(handle, std::move(x));
        s.x[0] = handle;
    });
    // int xar_close(xar_t)
    support.fill_function(xar_library, "_xar_close", [ar, archive_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(ar->mutex);
        auto* x = archive_of(s.x[0]);
        if (!x) { s.x[0] = static_cast<std::uint32_t>(-1); return; }
        bool ok = true;
        if (x->writing) ok = ar->files.write(x->path, xar::write(*x->archive, sha1), 0644);
        ar->close_archive(s.x[0]);
        s.x[0] = ok ? 0 : static_cast<std::uint32_t>(-1);
    });
    // int32_t xar_opt_set(xar_t, const char* option, const char* value)
    support.fill_function(xar_library, "_xar_opt_set", [ar, archive_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(ar->mutex);
        auto* x = archive_of(s.x[0]);
        const auto option = ar->c_string(s.x[1]), value = ar->c_string(s.x[2]);
        if (!x) { s.x[0] = static_cast<std::uint32_t>(-1); return; }
        if (option == "compression") {
            if (value != "gzip" && value != "bzip2" && value != "none") { s.x[0] = static_cast<std::uint32_t>(-1); return; }
            x->archive->compression = value;
        }
        s.x[0] = 0;
    });
    // xar_file_t xar_add_frompath(xar_t, xar_file_t parent, const char* name, const char* realpath)
    support.fill_function(xar_library, "_xar_add_frompath", [ar, archive_of, file_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(ar->mutex);
        auto* x = archive_of(s.x[0]);
        if (!x || !x->writing) { s.x[0] = 0; return; }
        xar::Node* parent = nullptr;
        if (s.x[1]) { const auto p = file_of(s.x[1]); if (p.first != x) { s.x[0] = 0; return; } parent = p.second; }
        const auto name = ar->c_string(s.x[2]), real = ar->c_string(s.x[3]);
        const auto mode = ar->files.mode_of(real);
        if (!mode) { s.x[0] = 0; return; }
        std::string type;
        std::optional<std::vector<std::uint8_t>> contents;
        std::string link;
        if ((*mode & s_ifmt) == s_ifreg) { type = "file"; contents = ar->files.read(real); if (!contents) { s.x[0] = 0; return; } }
        else if ((*mode & s_ifmt) == s_ifdir) type = "directory";
        else if ((*mode & s_ifmt) == s_iflnk) { type = "symlink"; const auto t = ar->files.link_target(real); if (!t) { s.x[0] = 0; return; } link = *t; }
        else { s.x[0] = 0; return; }   // devices, sockets and FIFOs are not archived
        auto& node = x->archive->add(parent, name);
        node.set("type", type);
        char octal[16];
        std::snprintf(octal, sizeof octal, "%04o", *mode & 07777);
        node.set("mode", octal);
        if (!link.empty()) node.set("link", link);
        node.contents = std::move(contents);
        s.x[0] = ar->file_handle(*x, &node);
    });
    // xar_file_t xar_add_frombuffer(xar_t, xar_file_t parent, const char* name, char* buffer, size_t length)
    support.fill_function(xar_library, "_xar_add_frombuffer", [ar, archive_of, file_of](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(ar->mutex);
        auto* x = archive_of(s.x[0]);
        if (!x || !x->writing || (!s.x[3] && s.x[4])) { s.x[0] = 0; return; }
        xar::Node* parent = nullptr;
        if (s.x[1]) { const auto p = file_of(s.x[1]); if (p.first != x) { s.x[0] = 0; return; } parent = p.second; }
        auto& node = x->archive->add(parent, ar->c_string(s.x[2]));
        node.set("type", "file");
        node.set("mode", "0644");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.x[4]));
        if (!bytes.empty()) m.read_bytes(s.x[3], bytes);
        node.contents = std::move(bytes);
        s.x[0] = ar->file_handle(*x, &node);
    });
    // int32_t xar_extract_tofile(xar_t, xar_file_t, const char* path)
    support.fill_function(xar_library, "_xar_extract_tofile", [ar, archive_of, file_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(ar->mutex);
        auto* x = archive_of(s.x[0]);
        const auto [owner, node] = file_of(s.x[1]);
        if (!x || owner != x || !node) { s.x[0] = static_cast<std::uint32_t>(-1); return; }
        const auto path = ar->c_string(s.x[2]);
        const auto* type = node->property("type");
        const auto* mode_text = node->property("mode");
        const auto mode = mode_text ? static_cast<std::uint32_t>(std::strtoul(mode_text->c_str(), nullptr, 8)) : 0644u;
        const std::string kind = type ? *type : "file";
        bool ok = false;
        if (kind == "directory") ok = ar->files.mkdir(path, mode ? mode : 0755);
        else if (kind == "symlink") { const auto* link = node->property("link"); ok = link && ar->files.symlink(*link, path); }
        else if (kind == "file") {
            std::string error;
            bool not_provided = false;
            const auto data = xar::extract(*x->archive, *node, error, not_provided);
            if (not_provided) unsupported("libxar: " + error);
            ok = data && ar->files.write(path, *data, mode);
        } else unsupported("libxar: extracting an item of type " + kind);
        s.x[0] = ok ? 0 : static_cast<std::uint32_t>(-1);
    });
    // xar_iter_t xar_iter_new(void) / void xar_iter_free(xar_iter_t)
    support.fill_function(xar_library, "_xar_iter_new", [ar](CpuState& s, GuestMemory&) {
        const auto handle = ar->token();
        std::lock_guard lock(ar->mutex);
        ar->iters.emplace(handle, std::make_unique<XarIter>());
        s.x[0] = handle;
    });
    support.fill_function(xar_library, "_xar_iter_free", [ar](CpuState& s, GuestMemory&) {
        bool known = false;
        { std::lock_guard lock(ar->mutex); known = ar->iters.erase(s.x[0]) != 0; }
        if (known) ar->s.process.release_guest(s.x[0]);
    });
    // xar_file_t xar_file_first(xar_t, xar_iter_t) / xar_file_next(xar_iter_t)
    support.fill_function(xar_library, "_xar_file_first", [ar, archive_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(ar->mutex);
        auto* x = archive_of(s.x[0]);
        const auto it = ar->iters.find(s.x[1]);
        if (!x || it == ar->iters.end()) { s.x[0] = 0; return; }
        auto& iter = *it->second;
        iter.archive = x;
        iter.nodes = x->archive->all();
        iter.next = 0;
        s.x[0] = iter.nodes.empty() ? 0 : ar->file_handle(*x, iter.nodes[iter.next++]);
    });
    support.fill_function(xar_library, "_xar_file_next", [ar](CpuState& s, GuestMemory&) {
        std::lock_guard lock(ar->mutex);
        const auto it = ar->iters.find(s.x[0]);
        if (it == ar->iters.end() || !it->second->archive) { s.x[0] = 0; return; }
        auto& iter = *it->second;
        s.x[0] = iter.next < iter.nodes.size() ? ar->file_handle(*iter.archive, iter.nodes[iter.next++]) : 0;
    });
    // char* xar_get_path(xar_file_t): malloc'd, freed by the caller.
    support.fill_function(xar_library, "_xar_get_path", [ar, file_of](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(ar->mutex);
        const auto [x, node] = file_of(s.x[0]);
        if (!node) { s.x[0] = 0; return; }
        const auto path = node->path();
        const auto at = ar->s.call(libsystem_library, "_malloc", {path.size() + 1});
        if (!at) { s.x[0] = 0; return; }
        m.write_bytes(at, std::span(reinterpret_cast<const std::uint8_t*>(path.c_str()), path.size() + 1));
        s.x[0] = at;
    });
    // char* xar_get_safe_path(xar_file_t): the path without empty, "." and ".."
    // components (no escape from the extraction directory); malloc'd.
    support.fill_function(xar_library, "_xar_get_safe_path", [ar, file_of](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(ar->mutex);
        const auto [x, node] = file_of(s.x[0]);
        if (!node) { s.x[0] = 0; return; }
        const auto raw = node->path();
        std::string safe;
        for (std::size_t at = 0; at <= raw.size();) {
            const auto slash = std::min(raw.find('/', at), raw.size());
            const auto part = raw.substr(at, slash - at);
            if (!part.empty() && part != "." && part != "..") { if (!safe.empty()) safe.push_back('/'); safe += part; }
            at = slash + 1;
        }
        const auto at = ar->s.call(libsystem_library, "_malloc", {safe.size() + 1});
        if (!at) { s.x[0] = 0; return; }
        m.write_bytes(at, std::span(reinterpret_cast<const std::uint8_t*>(safe.c_str()), safe.size() + 1));
        s.x[0] = at;
    });
    // int32_t xar_prop_get(xar_file_t, const char* key, const char** value) / xar_prop_set(xar_file_t, const char* key, const char* value)
    support.fill_function(xar_library, "_xar_prop_get", [ar, file_of](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(ar->mutex);
        const auto [x, node] = file_of(s.x[0]);
        const auto* value = node ? node->property(ar->c_string(s.x[1])) : nullptr;
        if (!value) { s.x[0] = static_cast<std::uint32_t>(-1); return; }
        if (s.x[2]) m.write64(s.x[2], value->empty() ? 0 : ar->guest_string(*x, *value));
        s.x[0] = 0;
    });
    support.fill_function(xar_library, "_xar_prop_set", [ar, file_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(ar->mutex);
        const auto [x, node] = file_of(s.x[0]);
        if (!node) { s.x[0] = static_cast<std::uint32_t>(-1); return; }
        node->set(ar->c_string(s.x[1]), ar->c_string(s.x[2]));
        s.x[0] = 0;
    });
    // Subdocs.
    support.fill_function(xar_library, "_xar_subdoc_new", [ar, archive_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(ar->mutex);
        auto* x = archive_of(s.x[0]);
        if (!x || !s.x[1]) { s.x[0] = 0; return; }
        auto sub = std::make_unique<xar::Subdoc>();
        sub->name = ar->c_string(s.x[1]);
        auto* raw = sub.get();
        x->archive->subdocs.push_back(std::move(sub));
        s.x[0] = ar->subdoc_handle(*x, raw);
    });
    support.fill_function(xar_library, "_xar_subdoc_first", [ar, archive_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(ar->mutex);
        auto* x = archive_of(s.x[0]);
        s.x[0] = x && !x->archive->subdocs.empty() ? ar->subdoc_handle(*x, x->archive->subdocs.front().get()) : 0;
    });
    support.fill_function(xar_library, "_xar_subdoc_next", [ar, subdoc_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(ar->mutex);
        const auto [x, sub] = subdoc_of(s.x[0]);
        if (!sub) { s.x[0] = 0; return; }
        auto& list = x->archive->subdocs;
        for (std::size_t i = 0; i + 1 < list.size(); ++i) if (list[i].get() == sub) { s.x[0] = ar->subdoc_handle(*x, list[i + 1].get()); return; }
        s.x[0] = 0;
    });
    support.fill_function(xar_library, "_xar_subdoc_name", [ar, subdoc_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(ar->mutex);
        const auto [x, sub] = subdoc_of(s.x[0]);
        s.x[0] = sub ? ar->guest_string(*x, sub->name) : 0;
    });
    support.fill_function(xar_library, "_xar_subdoc_prop_get", [ar, subdoc_of](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(ar->mutex);
        const auto [x, sub] = subdoc_of(s.x[0]);
        const auto* value = sub ? sub->property(ar->c_string(s.x[1])) : nullptr;
        if (!value) { s.x[0] = static_cast<std::uint32_t>(-1); return; }
        if (s.x[2]) m.write64(s.x[2], value->empty() ? 0 : ar->guest_string(*x, *value));
        s.x[0] = 0;
    });
    support.fill_function(xar_library, "_xar_subdoc_prop_set", [ar, subdoc_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(ar->mutex);
        const auto [x, sub] = subdoc_of(s.x[0]);
        if (!sub) { s.x[0] = static_cast<std::uint32_t>(-1); return; }
        sub->set(ar->c_string(s.x[1]), ar->c_string(s.x[2]));
        s.x[0] = 0;
    });
}

} // namespace alchemy::gui::completion_kit::data
