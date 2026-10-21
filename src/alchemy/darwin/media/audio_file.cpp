#include "alchemy/darwin/media/audio_file.hpp"

#include "alchemy/darwin/security/bridge.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

// AudioToolbox AudioFile and ExtAudioFile for reading linear PCM WAVE, AIFF,
// AIFC and CAF files (audio_file_core.hpp), through the guest's libSystem so
// paths resolve in the root. Files open read-only: writing (and opening for
// writing) answers kAudioFileOperationNotSupportedError. ExtAudioFile
// converts to any linear PCM client format with the file's sample rate
// (kAudioConverterErr_FormatNotSupported otherwise); it has no AudioConverter
// object to expose.
namespace alchemy::gui::completion_kit::media {
namespace {

using namespace audio_file;
constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

// <ExtendedAudioFile.h>
constexpr std::int32_t ext_invalid_property = -66561, ext_invalid_property_size = -66562, ext_non_pcm_client_format = -66563,
    ext_invalid_seek = -66568;
constexpr std::int32_t permissions_error = static_cast<std::int32_t>(four("prm?"));

using File = GuestAudio;

struct Ext {
    Address file{};
    bool owns_file{};
    Pcm client;
    std::vector<std::uint8_t> client_layout;
    std::uint64_t position{};
    std::uint32_t io_buffer_bytes{32768};
};

void status(CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

struct AudioFiles {
    Support& support;
    std::shared_ptr<security::Bridge> bridge = security::bridge();
    std::recursive_mutex mutex;
    std::map<Address, File> files;
    std::map<Address, Ext> exts;

    explicit AudioFiles(Support& owner) : support(owner) {}

    std::size_t pread(std::int64_t fd, std::uint64_t offset, std::span<std::uint8_t> out) { return read_guest(support, fd, offset, out); }
    // AudioFileOpenURL's work: an OSStatus, and the handle in `out`.
    std::int32_t open(const std::string& path, Address& out) {
        File file;
        if (const auto result = open_guest_audio(support, path, file)) return result;
        out = support.process.allocate_guest(16, 16);
        files[out] = std::move(file);
        return 0;
    }
    void close(Address handle) {
        const auto found = files.find(handle);
        if (found == files.end()) return;
        close_guest_audio(support, found->second);
        files.erase(found);
        support.process.release_guest(handle);
    }
    File* file(Address handle) {
        const auto found = files.find(handle);
        return found == files.end() ? nullptr : &found->second;
    }
    Ext* ext(Address handle) {
        const auto found = exts.find(handle);
        return found == exts.end() ? nullptr : &found->second;
    }

    // A property value: bytes, or a +1 CF object (8 bytes) made on demand.
    struct Value { std::vector<std::uint8_t> bytes; bool object{}; bool writable{}; };
    template <class T> static std::vector<std::uint8_t> raw(const T& value) {
        std::vector<std::uint8_t> out(sizeof(T));
        std::memcpy(out.data(), &value, sizeof(T));
        return out;
    }
    std::int32_t file_property(const File& f, std::uint32_t id, Value& out) {
        const auto& c = f.container;
        switch (id) {
        case four("ffmt"): out.bytes = raw(c.file_type); return 0;                                   // kAudioFilePropertyFileFormat
        case four("dfmt"): out.bytes = asbd_from_pcm(c.format); return 0;                            // DataFormat
        case four("bcnt"): out.bytes = raw(c.data_bytes); return 0;                                  // AudioDataByteCount
        case four("pcnt"): out.bytes = raw(f.frames()); return 0;                                    // AudioDataPacketCount
        case four("psze"): case four("pkub"): out.bytes = raw(c.format.frame_bytes()); return 0;     // Maximum packet size / upper bound
        case four("doff"): out.bytes = raw(static_cast<std::int64_t>(c.data_offset)); return 0;      // DataOffset
        case four("optm"): out.bytes = raw(std::uint32_t{1}); return 0;                              // IsOptimized
        case four("edur"): out.bytes = raw(static_cast<double>(f.frames()) / c.format.rate); return 0;   // EstimatedDuration
        case four("brat"): out.bytes = raw(static_cast<std::uint32_t>(std::llround(c.format.rate * c.format.frame_bytes() * 8))); return 0;
        case four("cmap"): {                                                                          // ChannelLayout
            out.bytes = layout_for_mask(c.channel_mask, c.format.channels);
            return out.bytes.empty() ? error::unsupported_property : 0;
        }
        case four("flst"): {   // FormatList: AudioFormatListItem {ASBD; AudioChannelLayoutTag} (48 bytes)
            out.bytes = asbd_from_pcm(c.format);
            const std::uint32_t unknown_layout = 0xffff0000U | c.format.channels;   // kAudioChannelLayoutTag_Unknown | channels
            const auto tag = raw(unknown_layout);
            out.bytes.insert(out.bytes.end(), tag.begin(), tag.end());
            out.bytes.resize(48);
            return 0;
        }
        case four("info"): out.bytes.assign(8, 0); out.object = true; return 0;                      // InfoDictionary (+1)
        default: return error::unsupported_property;   // magic cookie, packet table, markers, regions...: none for PCM
        }
    }
    Address info_dictionary(const File& f) {
        char text[64];
        std::snprintf(text, sizeof text, "%.3f", static_cast<double>(f.frames()) / f.container.format.rate);
        const auto key = support.cf.string("approximate duration in seconds"), value = support.cf.string(text);
        const auto dictionary = bridge->dictionary({{key, value}});
        support.release(key);
        support.release(value);
        return dictionary;
    }
    std::int32_t ext_property(const Ext& e, std::uint32_t id, Value& out) {
        const auto* f = file(e.file);
        if (!f) return error::param;
        switch (id) {
        case four("ffmt"): out.bytes = asbd_from_pcm(f->container.format); return 0;                // FileDataFormat
        case four("cfmt"): out.bytes = asbd_from_pcm(e.client); out.writable = true; return 0;      // ClientDataFormat
        case four("fclo"):                                                                           // FileChannelLayout
            out.bytes = layout_for_mask(f->container.channel_mask, f->container.format.channels);
            return out.bytes.empty() ? ext_invalid_property : 0;
        case four("cclo"):                                                                           // ClientChannelLayout
            out.bytes = !e.client_layout.empty() ? e.client_layout : layout_for_mask(f->container.channel_mask, e.client.channels);
            out.writable = true;
            return out.bytes.empty() ? ext_invalid_property : 0;
        case four("#frm"): out.bytes = raw(static_cast<std::int64_t>(f->frames())); return 0;       // FileLengthFrames
        case four("afil"): out.bytes = raw(e.file); return 0;                                       // AudioFile
        case four("fmps"): out.bytes = raw(f->container.format.frame_bytes()); return 0;            // FileMaxPacketSize
        case four("cmps"): out.bytes = raw(e.client.frame_bytes()); return 0;                       // ClientMaxPacketSize
        case four("iobs"): out.bytes = raw(e.io_buffer_bytes); out.writable = true; return 0;       // IOBufferSizeBytes
        default: return ext_invalid_property;   // AudioConverter, packet table, ...: there is no converter object
        }
    }
    // Writes `value` to (UInt32 *ioSize, void *out) per the AudioFile rules.
    std::int32_t answer(GuestMemory& m, Address size_at, Address out, const Value& value, std::int32_t size_error, const File* f) {
        const auto room = m.read32(size_at);
        if (room < value.bytes.size()) return size_error;
        if (!out) return error::param;
        if (value.object) m.write64(out, info_dictionary(*f));
        else m.write_bytes(out, value.bytes);
        m.write32(size_at, static_cast<std::uint32_t>(value.bytes.size()));
        return 0;
    }

    // ExtAudioFileRead's work.
    std::int32_t read(Ext& e, GuestMemory& m, Address frames_at, Address list) {
        auto* f = file(e.file);
        if (!f) return error::param;
        const auto requested = m.read32(frames_at);
        const auto buffer_count = m.read32(list);
        const std::uint32_t expected = e.client.non_interleaved ? e.client.channels : 1;
        if (buffer_count != expected) return error::param;
        std::uint64_t frames = requested;
        std::vector<Address> data(buffer_count);
        for (std::uint32_t i = 0; i < buffer_count; ++i) {
            const auto entry = list + 8 + 16ULL * i;
            data[i] = m.read64(entry + 8);
            if (!data[i]) return error::param;
            frames = std::min<std::uint64_t>(frames, m.read32(entry + 4) / e.client.frame_bytes());
        }
        const auto total = f->frames();
        frames = e.position >= total ? 0 : std::min<std::uint64_t>(frames, total - e.position);
        const auto& source = f->container.format;
        std::uint64_t done = 0;
        while (done < frames) {
            const auto count = std::min<std::uint64_t>(frames - done, 16384);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count * source.frame_bytes()));
            const auto got = pread(f->fd, f->container.data_offset + (e.position + done) * source.frame_bytes(), bytes);
            const auto whole = got / source.frame_bytes();
            if (!whole) break;   // the file ended early (truncated data chunk)
            const auto samples = decode(source, {std::span<const std::uint8_t>(bytes.data(), whole * source.frame_bytes())}, whole);
            std::vector<std::vector<std::uint8_t>> out(buffer_count, std::vector<std::uint8_t>(whole * e.client.frame_bytes()));
            std::vector<std::span<std::uint8_t>> spans;
            for (auto& buffer : out) spans.emplace_back(buffer);
            encode(e.client, samples, source.channels, whole, spans);
            for (std::uint32_t i = 0; i < buffer_count; ++i) m.write_bytes(data[i] + done * e.client.frame_bytes(), out[i]);
            done += whole;
            if (whole < count) break;
        }
        for (std::uint32_t i = 0; i < buffer_count; ++i) m.write32(list + 8 + 16ULL * i + 4, static_cast<std::uint32_t>(done * e.client.frame_bytes()));
        m.write32(frames_at, static_cast<std::uint32_t>(done));
        e.position += done;
        return 0;
    }
};

} // namespace

std::size_t read_guest(Support& support, std::int64_t fd, std::uint64_t offset, std::span<std::uint8_t> out) {
    std::size_t done = 0;
    const auto chunk_size = std::min<std::size_t>(out.size(), 1 << 20);
    const auto chunk = support.process.allocate_guest(std::max<std::size_t>(chunk_size, 1));
    while (done < out.size()) {
        const auto want = std::min<std::size_t>(out.size() - done, chunk_size);
        const auto got = static_cast<std::int64_t>(support.call(libsystem, "_pread", {static_cast<Address>(fd), chunk, want, offset + done}));
        if (got <= 0) break;
        support.memory().read_bytes(chunk, out.subspan(done, static_cast<std::size_t>(got)));
        done += static_cast<std::size_t>(got);
    }
    support.process.release_guest(chunk);
    return done;
}

std::int32_t open_guest_audio(Support& support, const std::string& path, GuestAudio& out) {
    const auto text = support.process.allocate_guest(path.size() + 1, 1);
    std::vector<std::uint8_t> name(path.begin(), path.end());
    name.push_back(0);
    support.memory().write_bytes(text, name);
    const auto fd = static_cast<std::int64_t>(support.call(libsystem, "_open", {text, 0}));
    support.process.release_guest(text);
    if (fd < 0) {
        const auto at = support.call(libsystem, "___error", {});
        const auto error_number = at ? static_cast<int>(support.memory().read32(at)) : 0;
        return error_number == 13 ? permissions_error : error::file_not_found;   // EACCES
    }
    GuestAudio audio;
    audio.fd = fd;
    const auto end = static_cast<std::int64_t>(support.call(libsystem, "_lseek", {static_cast<Address>(fd), 0, 2}));
    audio.size = end > 0 ? static_cast<std::uint64_t>(end) : 0;
    std::string why;
    const auto result = parse([&support, fd](std::uint64_t offset, std::span<std::uint8_t> bytes) { return read_guest(support, fd, offset, bytes); },
                              audio.size, audio.container, why);
    if (result) {
        static_cast<void>(support.call(libsystem, "_close", {static_cast<Address>(fd)}));
        return result;
    }
    out = std::move(audio);
    return 0;
}

void close_guest_audio(Support& support, GuestAudio& audio) {
    if (audio.fd >= 0) static_cast<void>(support.call(libsystem, "_close", {static_cast<Address>(std::exchange(audio.fd, -1))}));
}

void install_audio_file(Support& support) {
    auto state = std::make_shared<AudioFiles>(support);
    auto* sp = &support;
    const auto lib = audio_toolbox;

    // AudioFileOpenURL(CFURLRef, AudioFilePermissions (SInt8), AudioFileTypeID hint, AudioFileID *out)
    support.function(lib, "_AudioFileOpenURL", [state, sp](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[3]) return status(s, error::param);
        m.write64(s.x[3], 0);
        if (static_cast<std::int8_t>(s.x[1]) != 1) return status(s, error::operation_not_supported);   // read-only
        const auto path = sp->cf.url_path(s.x[0]);
        std::lock_guard lock(state->mutex);
        Address handle{};
        const auto result = state->open(path, handle);
        if (!result) m.write64(s.x[3], handle);
        status(s, result);
    });
    support.function(lib, "_AudioFileClose", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        if (!state->file(s.x[0])) return status(s, error::param);
        for (const auto& [handle, ext] : state->exts)
            if (ext.file == s.x[0] && ext.owns_file) return status(s, error::param);   // owned by an ExtAudioFile
        state->close(s.x[0]);
        status(s, 0);
    });
    // AudioFileGetPropertyInfo(file, AudioFilePropertyID, UInt32 *outDataSize, UInt32 *isWritable)
    support.function(lib, "_AudioFileGetPropertyInfo", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto* f = state->file(s.x[0]);
        if (!f) return status(s, error::param);
        AudioFiles::Value value;
        if (const auto result = state->file_property(*f, static_cast<std::uint32_t>(s.x[1]), value)) return status(s, result);
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(value.bytes.size()));
        if (s.x[3]) m.write32(s.x[3], 0);   // read-only files
        status(s, 0);
    });
    // AudioFileGetProperty(file, AudioFilePropertyID, UInt32 *ioDataSize, void *outData)
    support.function(lib, "_AudioFileGetProperty", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, error::param);
        std::lock_guard lock(state->mutex);
        const auto* f = state->file(s.x[0]);
        if (!f) return status(s, error::param);
        AudioFiles::Value value;
        if (const auto result = state->file_property(*f, static_cast<std::uint32_t>(s.x[1]), value)) return status(s, result);
        status(s, state->answer(m, s.x[2], s.x[3], value, error::bad_property_size, f));
    });
    support.function(lib, "_AudioFileSetProperty", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        status(s, state->file(s.x[0]) ? error::operation_not_supported : error::param);   // files are read-only
    });
    // AudioFileReadBytes(file, Boolean useCache, SInt64 startingByte, UInt32 *ioNumBytes, void *outBuffer)
    support.function(lib, "_AudioFileReadBytes", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* f = state->file(s.x[0]);
        if (!f || !s.x[3] || !s.x[4]) return status(s, error::param);
        const auto start = static_cast<std::int64_t>(s.x[2]);
        if (start < 0) return status(s, error::invalid_packet_offset);
        const auto wanted = m.read32(s.x[3]);
        const auto available = static_cast<std::uint64_t>(start) >= f->container.data_bytes ? 0 : f->container.data_bytes - static_cast<std::uint64_t>(start);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<std::uint64_t>(wanted, available)));
        const auto got = state->pread(f->fd, f->container.data_offset + static_cast<std::uint64_t>(start), bytes);
        if (got) m.write_bytes(s.x[4], std::span<const std::uint8_t>(bytes.data(), got));
        m.write32(s.x[3], static_cast<std::uint32_t>(got));
        status(s, got < wanted ? error::end_of_file : 0);
    });
    // AudioFileReadPacketData(file, Boolean useCache, UInt32 *ioNumBytes, AudioStreamPacketDescription *,
    //     SInt64 startingPacket, UInt32 *ioNumPackets, void *outBuffer): PCM packets are frames.
    support.function(lib, "_AudioFileReadPacketData", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* f = state->file(s.x[0]);
        if (!f || !s.x[2] || !s.x[5] || !s.x[6]) return status(s, error::param);
        const auto packet = static_cast<std::int64_t>(s.x[4]);
        if (packet < 0) return status(s, error::invalid_packet_offset);
        const auto frame_bytes = f->container.format.frame_bytes();
        auto count = std::min<std::uint64_t>(m.read32(s.x[5]), m.read32(s.x[2]) / frame_bytes);
        const auto total = f->frames();
        count = static_cast<std::uint64_t>(packet) >= total ? 0 : std::min<std::uint64_t>(count, total - static_cast<std::uint64_t>(packet));
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count * frame_bytes));
        const auto got = state->pread(f->fd, f->container.data_offset + static_cast<std::uint64_t>(packet) * frame_bytes, bytes) / frame_bytes * frame_bytes;
        if (got) m.write_bytes(s.x[6], std::span<const std::uint8_t>(bytes.data(), got));
        m.write32(s.x[2], static_cast<std::uint32_t>(got));
        m.write32(s.x[5], static_cast<std::uint32_t>(got / frame_bytes));
        status(s, got ? 0 : error::end_of_file);
    });

    // ---- ExtAudioFile --------------------------------------------------------------------------
    const auto make_ext = [state](Address file, bool owns) {
        Ext e;
        e.file = file;
        e.owns_file = owns;
        e.client = state->file(file)->container.format;   // the file's own format until a client format is set
        const auto handle = state->support.process.allocate_guest(16, 16);
        state->exts[handle] = std::move(e);
        return handle;
    };
    // ExtAudioFileWrapAudioFileID(AudioFileID, Boolean forWriting, ExtAudioFileRef *out)
    support.function(lib, "_ExtAudioFileWrapAudioFileID", [state, make_ext](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, error::param);
        m.write64(s.x[2], 0);
        std::lock_guard lock(state->mutex);
        if (!state->file(s.x[0])) return status(s, error::param);
        if (s.x[1] & 0xff) return status(s, error::operation_not_supported);   // writing is not provided
        m.write64(s.x[2], make_ext(s.x[0], false));
        status(s, 0);
    });
    // ExtAudioFileOpenURL(CFURLRef, ExtAudioFileRef *out)
    support.function(lib, "_ExtAudioFileOpenURL", [state, sp, make_ext](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) return status(s, error::param);
        m.write64(s.x[1], 0);
        std::lock_guard lock(state->mutex);
        Address file{};
        if (const auto result = state->open(sp->cf.url_path(s.x[0]), file)) return status(s, result);
        m.write64(s.x[1], make_ext(file, true));
        status(s, 0);
    });
    support.function(lib, "_ExtAudioFileDispose", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        auto* e = state->ext(s.x[0]);
        if (!e) return status(s, error::param);
        if (e->owns_file) state->close(e->file);
        state->exts.erase(s.x[0]);
        state->support.process.release_guest(s.x[0]);
        status(s, 0);
    });
    // ExtAudioFileGetPropertyInfo(ext, ExtAudioFilePropertyID, UInt32 *outSize, Boolean *outWritable)
    support.function(lib, "_ExtAudioFileGetPropertyInfo", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* e = state->ext(s.x[0]);
        if (!e) return status(s, error::param);
        AudioFiles::Value value;
        if (const auto result = state->ext_property(*e, static_cast<std::uint32_t>(s.x[1]), value)) return status(s, result);
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(value.bytes.size()));
        if (s.x[3]) m.write8(s.x[3], value.writable ? 1 : 0);
        status(s, 0);
    });
    // ExtAudioFileGetProperty(ext, ExtAudioFilePropertyID, UInt32 *ioDataSize, void *outData)
    support.function(lib, "_ExtAudioFileGetProperty", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, error::param);
        std::lock_guard lock(state->mutex);
        auto* e = state->ext(s.x[0]);
        if (!e) return status(s, error::param);
        AudioFiles::Value value;
        if (const auto result = state->ext_property(*e, static_cast<std::uint32_t>(s.x[1]), value)) return status(s, result);
        status(s, state->answer(m, s.x[2], s.x[3], value, ext_invalid_property_size, nullptr));
    });
    // ExtAudioFileSetProperty(ext, ExtAudioFilePropertyID, UInt32 size, const void *data)
    support.function(lib, "_ExtAudioFileSetProperty", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* e = state->ext(s.x[0]);
        if (!e || (s.x[2] && !s.x[3])) return status(s, error::param);
        const auto size = static_cast<std::uint32_t>(s.x[2]);
        if (size > (1 << 16)) return status(s, ext_invalid_property_size);
        std::vector<std::uint8_t> data(size);
        if (size) m.read_bytes(s.x[3], data);
        const auto* f = state->file(e->file);
        switch (static_cast<std::uint32_t>(s.x[1])) {
        case four("cfmt"): {
            if (size < 40) return status(s, ext_invalid_property_size);
            std::string why;
            std::uint32_t format_id{};
            std::memcpy(&format_id, data.data() + 8, 4);
            if (format_id != four("lpcm")) return status(s, ext_non_pcm_client_format);
            const auto client = pcm_from_asbd(data, why);
            if (!client || !convertible(f->container.format, *client)) return status(s, error::unsupported_data_format);   // kAudioConverterErr_FormatNotSupported
            e->client = *client;
            e->client_layout.clear();
            return status(s, 0);
        }
        case four("cclo"): {
            if (size < 12) return status(s, ext_invalid_property_size);
            e->client_layout = std::move(data);
            return status(s, 0);
        }
        case four("iobs"):
            if (size != 4) return status(s, ext_invalid_property_size);
            std::memcpy(&e->io_buffer_bytes, data.data(), 4);
            return status(s, 0);
        default: return status(s, ext_invalid_property);
        }
    });
    // ExtAudioFileRead(ext, UInt32 *ioNumberFrames, AudioBufferList *ioData): 0 frames at the end.
    support.function(lib, "_ExtAudioFileRead", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || !s.x[2]) return status(s, error::param);
        std::lock_guard lock(state->mutex);
        auto* e = state->ext(s.x[0]);
        if (!e) return status(s, error::param);
        status(s, state->read(*e, m, s.x[1], s.x[2]));
    });
    // ExtAudioFileSeek(ext, SInt64 frameOffset), ExtAudioFileTell(ext, SInt64 *out)
    support.function(lib, "_ExtAudioFileSeek", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        auto* e = state->ext(s.x[0]);
        if (!e) return status(s, error::param);
        if (static_cast<std::int64_t>(s.x[1]) < 0) return status(s, ext_invalid_seek);
        e->position = s.x[1];
        status(s, 0);
    });
    support.function(lib, "_ExtAudioFileTell", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* e = state->ext(s.x[0]);
        if (!e || !s.x[1]) return status(s, error::param);
        m.write64(s.x[1], e->position);
        status(s, 0);
    });
    for (const auto* name : {"_ExtAudioFileWrite", "_ExtAudioFileWriteAsync"})
        support.function(lib, name, [state](CpuState& s, GuestMemory&) {
            std::lock_guard lock(state->mutex);
            status(s, state->ext(s.x[0]) ? error::operation_not_supported : error::param);   // files open for reading only
        });
}

} // namespace alchemy::gui::completion_kit::media
