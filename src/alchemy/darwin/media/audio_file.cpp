#include "alchemy/darwin/media/audio_file.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

// AudioToolbox AudioFile and ExtAudioFile for linear PCM WAVE, AIFF, AIFC and CAF files
// (audio_file_core.hpp), through the guest's libSystem so paths resolve in the root, or through
// the client's callbacks (AudioFileOpenWithCallbacks, AudioFileInitializeWithCallbacks).
// - Reading: any of those files. ExtAudioFile converts to any linear PCM client format with the
//   file's sample rate (kAudioConverterErr_FormatNotSupported otherwise); it has no AudioConverter
//   object to expose.
// - Compressed files with a decoder (audio_stream.hpp: MP3, FLAC, Ogg Opus, MP4 and Matroska audio
//   in those codecs) open read-only: AudioFile describes and reads their packets (data format,
//   packet counts and sizes, magic cookie, packet table, ReadPacketData with descriptions;
//   ReadBytes answers kAudioFileOperationNotSupportedError), and ExtAudioFile reads them decoded.
//   Other compressed data (AAC, ALAC, ...) answers kAudioFileUnsupportedDataFormatError.
// - Writing: AudioFileCreateWithURL and InitializeWithCallbacks make files in the format given,
//   which the container must hold (kAudioFileUnsupportedDataFormatError otherwise); bytes and
//   packets go into the data, and the header's sizes are brought up to date by AudioFileOptimize
//   and AudioFileClose. ExtAudioFileCreateWithURL / ExtAudioFileWrite convert from the client
//   format (same rate). An existing file opens for writing when its header is the one Alchemy
//   would write for it (a canonical WAVE, or an Alchemy-written file); others answer
//   kAudioFileOperationNotSupportedError. ExtAudioFileWriteAsync writes at once.
namespace alchemy::gui::completion_kit::media {
namespace {

using namespace audio_file;
constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

// <ExtendedAudioFile.h>
constexpr std::int32_t ext_invalid_property = -66561, ext_invalid_property_size = -66562, ext_non_pcm_client_format = -66563,
    ext_invalid_seek = -66568;
constexpr std::int32_t permissions_error = static_cast<std::int32_t>(four("prm?"));
// <fcntl.h> (Darwin)
constexpr std::uint64_t o_rdwr = 2, o_creat = 0x200, o_trunc = 0x400, o_excl = 0x800;
// AudioFileFlags
constexpr std::uint32_t flag_erase_file = 1;

struct File {
    GuestAudio audio;                  // fd -1 for a callback file
    // AudioFileOpenWithCallbacks / InitializeWithCallbacks: the client's procedures.
    Address client{}, read_proc{}, write_proc{}, size_proc{}, set_size_proc{};
    bool writable{};
    bool dirty{};                      // the header's sizes are behind the data
    std::vector<std::uint8_t> layout;  // a CAF file's channel layout (written in its header)
    [[nodiscard]] const Container& container() const { return audio.container; }
    [[nodiscard]] std::uint64_t frames() const { return audio.frames(); }
};

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

    std::uint64_t call(Address function, std::initializer_list<std::uint64_t> arguments) {
        return EngineAdapter(support.process.calling_engine()).call(function, std::span(arguments.begin(), arguments.size()));
    }

    // ---- I/O: the guest's descriptor, or the client's callbacks -------------------------------
    std::size_t read_at(const File& f, std::uint64_t offset, std::span<std::uint8_t> out) {
        if (f.audio.fd >= 0) return read_guest(support, f.audio.fd, offset, out);
        if (!f.read_proc || out.empty()) return 0;
        // OSStatus (*AudioFile_ReadProc)(void *client, SInt64 position, UInt32 count, void *buffer, UInt32 *actual)
        const auto buffer = support.process.allocate_guest(out.size() + 4, 16);
        const auto actual = buffer + out.size();
        support.memory().write32(actual, 0);
        const auto result = static_cast<std::int32_t>(call(f.read_proc, {f.client, offset, out.size(), buffer, actual}));
        const auto got = result ? 0U : std::min<std::uint32_t>(support.memory().read32(actual), static_cast<std::uint32_t>(out.size()));
        if (got) support.memory().read_bytes(buffer, out.first(got));
        support.process.release_guest(buffer);
        return got;
    }
    std::int32_t write_at(const File& f, std::uint64_t offset, std::span<const std::uint8_t> bytes) {
        if (bytes.empty()) return 0;
        const auto chunk_size = std::min<std::size_t>(bytes.size(), 1 << 20);
        const auto buffer = support.process.allocate_guest(chunk_size + 4, 16);
        std::int32_t result = 0;
        for (std::size_t done = 0; done < bytes.size() && !result;) {
            const auto n = std::min(chunk_size, bytes.size() - done);
            support.memory().write_bytes(buffer, bytes.subspan(done, n));
            if (f.audio.fd >= 0) {
                const auto wrote = static_cast<std::int64_t>(support.call(libsystem, "_pwrite", {static_cast<Address>(f.audio.fd), buffer, n, offset + done}));
                if (wrote <= 0) result = permissions_error;
                else done += static_cast<std::size_t>(wrote);
            } else {
                if (!f.write_proc) { result = permissions_error; break; }
                // OSStatus (*AudioFile_WriteProc)(void *client, SInt64 position, UInt32 count, const void *buffer, UInt32 *actual)
                support.memory().write32(buffer + chunk_size, 0);
                result = static_cast<std::int32_t>(call(f.write_proc, {f.client, offset + done, n, buffer, buffer + chunk_size}));
                const auto wrote = support.memory().read32(buffer + chunk_size);
                if (!result && !wrote) result = permissions_error;
                done += wrote;
            }
        }
        support.process.release_guest(buffer);
        return result;
    }
    std::uint64_t size_of(const File& f) {
        if (f.audio.fd >= 0) {
            const auto end = static_cast<std::int64_t>(support.call(libsystem, "_lseek", {static_cast<Address>(f.audio.fd), 0, 2}));
            return end > 0 ? static_cast<std::uint64_t>(end) : 0;
        }
        if (!f.size_proc) return 0;
        const auto size = static_cast<std::int64_t>(call(f.size_proc, {f.client}));   // SInt64 (*AudioFile_GetSizeProc)(void *client)
        return size > 0 ? static_cast<std::uint64_t>(size) : 0;
    }

    // ---- handles --------------------------------------------------------------------------------
    Address adopt(File file) {
        const auto handle = support.process.allocate_guest(16, 16);
        files[handle] = std::move(file);
        return handle;
    }
    // AudioFileOpenURL's work: an OSStatus, and the handle in `out`.
    std::int32_t open(const std::string& path, Address& out) {
        File file;
        if (const auto result = open_guest_audio(support, path, file.audio)) return result;
        out = adopt(std::move(file));
        return 0;
    }
    // The file's header as Alchemy would write it now.
    std::int32_t header(const File& f, std::vector<std::uint8_t>& out) {
        std::string why;
        return write_header(f.container().file_type, f.container().format, f.container().data_bytes, f.layout, out, why);
    }
    // Writing into an existing file: its header must be the one Alchemy would write (so its sizes
    // can be rewritten in place).
    std::int32_t make_writable(File& f) {
        std::vector<std::uint8_t> expected;
        if (const auto result = header(f, expected)) return result == error::unsupported_file_type ? result : error::operation_not_supported;
        if (expected.size() != f.container().data_offset) return error::operation_not_supported;
        std::vector<std::uint8_t> actual(expected.size());
        if (read_at(f, 0, actual) != actual.size() || actual != expected) return error::operation_not_supported;
        f.writable = true;
        return 0;
    }
    // The header's sizes, brought up to date.
    std::int32_t finish(File& f) {
        if (!f.writable || !f.dirty) return 0;
        std::vector<std::uint8_t> bytes;
        if (const auto result = header(f, bytes)) return result;
        if (const auto result = write_at(f, 0, bytes)) return result;
        if (f.audio.fd < 0 && f.set_size_proc) {
            const auto end = f.container().data_offset + f.container().data_bytes;
            if (size_of(f) > end) static_cast<void>(call(f.set_size_proc, {f.client, end}));   // OSStatus (*)(void *client, SInt64 size)
        }
        f.dirty = false;
        return 0;
    }
    // Writes `bytes` at `offset` into the audio data (extending it).
    std::int32_t write_data(File& f, std::uint64_t offset, std::span<const std::uint8_t> bytes) {
        if (!f.writable) return permissions_error;
        if (const auto result = write_at(f, f.container().data_offset + offset, bytes)) return result;
        auto& data_bytes = f.audio.container.data_bytes;
        data_bytes = std::max<std::uint64_t>(data_bytes, offset + bytes.size());
        f.audio.size = std::max<std::uint64_t>(f.audio.size, f.container().data_offset + data_bytes);
        f.dirty = true;
        return 0;
    }
    void close(Address handle) {
        const auto found = files.find(handle);
        if (found == files.end()) return;
        static_cast<void>(finish(found->second));
        close_guest_audio(support, found->second.audio);
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

    // A new file of `type` in `format` (the header written, no data).
    std::int32_t start_file(File& f, std::uint32_t type, std::span<const std::uint8_t> asbd) {
        std::string why;
        std::uint32_t format_id{};
        std::memcpy(&format_id, asbd.data() + 8, 4);
        if (type != four("WAVE") && type != four("AIFF") && type != four("AIFC") && type != four("caff")) return error::unsupported_file_type;
        const auto format = pcm_from_asbd(asbd, why);
        if (format_id != four("lpcm") || !format) return error::unsupported_data_format;   // no encoder exists
        f.audio.container.file_type = type;
        f.audio.container.format = *format;
        f.audio.container.data_bytes = 0;
        std::vector<std::uint8_t> bytes;
        if (const auto result = write_header(type, *format, 0, f.layout, bytes, why)) return result;
        f.audio.container.data_offset = bytes.size();
        f.writable = true;
        if (const auto result = write_at(f, 0, bytes)) return result;
        f.audio.size = bytes.size();
        return 0;
    }

    // A property value: bytes, or a +1 CF object (8 bytes) made on demand.
    struct Value { std::vector<std::uint8_t> bytes; bool object{}; bool writable{}; };
    template <class T> static std::vector<std::uint8_t> raw(const T& value) {
        std::vector<std::uint8_t> out(sizeof(T));
        std::memcpy(out.data(), &value, sizeof(T));
        return out;
    }
    // A compressed file's properties: its packets, as the container holds them.
    std::int32_t stream_property(const File& f, std::uint32_t id, Value& out) {
        const auto& st = *f.audio.stream;
        const auto seconds = static_cast<double>(st.frames()) / st.rate();
        std::uint64_t bytes = 0;
        for (const auto& sample : st.track().samples) bytes += sample.size;
        switch (id) {
        case four("ffmt"): out.bytes = raw(st.file_type()); return 0;
        case four("dfmt"): out.bytes = st.asbd(); return 0;
        case four("bcnt"): out.bytes = raw(bytes); return 0;
        case four("pcnt"): out.bytes = raw(static_cast<std::uint64_t>(st.packet_count())); return 0;
        case four("psze"): case four("pkub"): out.bytes = raw(st.maximum_packet_bytes()); return 0;
        case four("doff"): out.bytes = raw(static_cast<std::int64_t>(st.track().samples.front().offset)); return 0;
        case four("optm"): out.bytes = raw(std::uint32_t{1}); return 0;
        case four("edur"): out.bytes = raw(seconds); return 0;
        case four("brat"): out.bytes = raw(static_cast<std::uint32_t>(seconds > 0 ? std::llround(static_cast<double>(bytes) * 8 / seconds) : 0)); return 0;
        case four("pnfo"): {   // kAudioFilePropertyPacketTableInfo {SInt64 valid; SInt32 priming; SInt32 remainder}
            out.bytes = raw(static_cast<std::int64_t>(st.frames()));
            const auto tail = raw(static_cast<std::int32_t>(st.priming()));
            const auto end = raw(static_cast<std::int32_t>(st.remainder()));
            out.bytes.insert(out.bytes.end(), tail.begin(), tail.end());
            out.bytes.insert(out.bytes.end(), end.begin(), end.end());
            return 0;
        }
        case four("mgic"):     // kAudioFilePropertyMagicCookieData
            if (st.magic_cookie().empty()) return error::unsupported_property;
            out.bytes = st.magic_cookie();
            return 0;
        case four("cmap"):
            out.bytes = layout_for_mask(0, st.channels());
            return out.bytes.empty() ? error::unsupported_property : 0;
        case four("flst"): {
            out.bytes = st.asbd();
            const auto tag = raw(0xffff0000U | st.channels());
            out.bytes.insert(out.bytes.end(), tag.begin(), tag.end());
            out.bytes.resize(48);
            return 0;
        }
        case four("info"): out.bytes.assign(8, 0); out.object = true; return 0;
        default: return error::unsupported_property;
        }
    }
    std::int32_t file_property(const File& f, std::uint32_t id, Value& out) {
        if (f.audio.stream) return stream_property(f, id, out);
        const auto& c = f.container();
        switch (id) {
        case four("ffmt"): out.bytes = raw(c.file_type); return 0;                                   // kAudioFilePropertyFileFormat
        case four("dfmt"): out.bytes = asbd_from_pcm(c.format); out.writable = f.writable && !c.data_bytes; return 0;   // DataFormat
        case four("bcnt"): out.bytes = raw(c.data_bytes); return 0;                                  // AudioDataByteCount
        case four("pcnt"): out.bytes = raw(f.frames()); return 0;                                    // AudioDataPacketCount
        case four("psze"): case four("pkub"): out.bytes = raw(c.format.frame_bytes()); return 0;     // Maximum packet size / upper bound
        case four("doff"): out.bytes = raw(static_cast<std::int64_t>(c.data_offset)); return 0;      // DataOffset
        case four("optm"): out.bytes = raw(std::uint32_t{f.dirty ? 0U : 1U}); return 0;              // IsOptimized
        case four("edur"): out.bytes = raw(static_cast<double>(f.frames()) / c.format.rate); return 0;   // EstimatedDuration
        case four("brat"): out.bytes = raw(static_cast<std::uint32_t>(std::llround(c.format.rate * c.format.frame_bytes() * 8))); return 0;
        case four("cmap"): {                                                                          // ChannelLayout
            out.bytes = !f.layout.empty() ? f.layout : layout_for_mask(c.channel_mask, c.format.channels);
            out.writable = f.writable && c.file_type == four("caff") && !c.data_bytes;
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
        std::snprintf(text, sizeof text, "%.3f", static_cast<double>(f.frames()) / f.container().format.rate);
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
        case four("ffmt"): out.bytes = f->audio.stream ? f->audio.stream->asbd() : asbd_from_pcm(f->container().format); return 0;   // FileDataFormat
        case four("cfmt"): out.bytes = asbd_from_pcm(e.client); out.writable = true; return 0;      // ClientDataFormat
        case four("fclo"):                                                                           // FileChannelLayout
            out.bytes = !f->layout.empty() ? f->layout : layout_for_mask(f->container().channel_mask, f->container().format.channels);
            return out.bytes.empty() ? ext_invalid_property : 0;
        case four("cclo"):                                                                           // ClientChannelLayout
            out.bytes = !e.client_layout.empty() ? e.client_layout : layout_for_mask(f->container().channel_mask, e.client.channels);
            out.writable = true;
            return out.bytes.empty() ? ext_invalid_property : 0;
        case four("#frm"): out.bytes = raw(static_cast<std::int64_t>(f->frames())); return 0;       // FileLengthFrames
        case four("afil"): out.bytes = raw(e.file); return 0;                                       // AudioFile
        case four("fmps"):                                                                           // FileMaxPacketSize
            out.bytes = raw(f->audio.stream ? f->audio.stream->maximum_packet_bytes() : f->container().format.frame_bytes());
            return 0;
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

    // The client buffers of an AudioBufferList for `e`'s client format: their addresses, and the
    // frames they hold (by their byte sizes).
    std::int32_t client_buffers(const Ext& e, GuestMemory& m, Address list, std::uint64_t& frames, std::vector<Address>& data) {
        const auto buffer_count = m.read32(list);
        const std::uint32_t expected = e.client.non_interleaved ? e.client.channels : 1;
        if (buffer_count != expected) return error::param;
        data.resize(buffer_count);
        for (std::uint32_t i = 0; i < buffer_count; ++i) {
            const auto entry = list + 8 + 16ULL * i;
            data[i] = m.read64(entry + 8);
            if (!data[i]) return error::param;
            frames = std::min<std::uint64_t>(frames, m.read32(entry + 4) / e.client.frame_bytes());
        }
        return 0;
    }

    // ExtAudioFileRead's work.
    std::int32_t read(Ext& e, GuestMemory& m, Address frames_at, Address list) {
        auto* f = file(e.file);
        if (!f) return error::param;
        std::uint64_t frames = m.read32(frames_at);
        std::vector<Address> data;
        if (const auto result = client_buffers(e, m, list, frames, data)) return result;
        const auto buffer_count = static_cast<std::uint32_t>(data.size());
        const auto total = f->frames();
        frames = e.position >= total ? 0 : std::min<std::uint64_t>(frames, total - e.position);
        const auto& source = f->container().format;
        const ReadAt bytes_at = [this, f](std::uint64_t offset, std::span<std::uint8_t> out) { return read_at(*f, offset, out); };
        std::uint64_t done = 0;
        while (done < frames) {
            const auto count = std::min<std::uint64_t>(frames - done, 16384);
            std::vector<double> samples;
            const auto whole = read_audio_frames(f->audio, bytes_at, e.position + done, static_cast<std::size_t>(count), samples);
            if (!whole) break;   // the file ended early (truncated data chunk)
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
    // ExtAudioFileWrite's work: `frames` client frames at the position, in the file's format.
    std::int32_t write(Ext& e, GuestMemory& m, std::uint32_t requested, Address list) {
        auto* f = file(e.file);
        if (!f) return error::param;
        if (!f->writable) return permissions_error;
        if (!requested) return 0;
        if (!list) return error::param;
        std::uint64_t frames = requested;
        std::vector<Address> data;
        if (const auto result = client_buffers(e, m, list, frames, data)) return result;
        if (frames < requested) return error::param;   // the buffers hold fewer frames than asked
        const auto& target = f->container().format;
        for (std::uint64_t done = 0; done < frames;) {
            const auto count = std::min<std::uint64_t>(frames - done, 16384);
            std::vector<std::vector<std::uint8_t>> in(data.size(), std::vector<std::uint8_t>(static_cast<std::size_t>(count * e.client.frame_bytes())));
            std::vector<std::span<const std::uint8_t>> spans;
            for (std::size_t i = 0; i < data.size(); ++i) {
                m.read_bytes(data[i] + done * e.client.frame_bytes(), in[i]);
                spans.emplace_back(in[i]);
            }
            const auto samples = decode(e.client, spans, static_cast<std::size_t>(count));
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count * target.frame_bytes()));
            encode(target, samples, e.client.channels, static_cast<std::size_t>(count), {std::span<std::uint8_t>(bytes)});
            if (const auto result = write_data(*f, (e.position + done) * target.frame_bytes(), bytes)) return result;
            done += count;
        }
        e.position += frames;
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

namespace {

// open(2) through the guest: the descriptor, or -1 with the OSStatus in `result`.
std::int64_t open_guest(Support& support, const std::string& path, std::uint64_t flags, std::int32_t& result) {
    const auto text = support.process.allocate_guest(path.size() + 1, 1);
    std::vector<std::uint8_t> name(path.begin(), path.end());
    name.push_back(0);
    support.memory().write_bytes(text, name);
    CallRegs registers{};
    registers.x[0] = text;
    registers.x[1] = flags;
    registers.push_variadic(std::uint64_t{0644});   // open's mode is variadic
    const auto fd = static_cast<std::int64_t>(support.call(libsystem, "_open", registers).x0);
    support.process.release_guest(text);
    result = 0;
    if (fd < 0) {
        const auto at = support.call(libsystem, "___error", {});
        const auto error_number = at ? static_cast<int>(support.memory().read32(at)) : 0;
        result = error_number == 2 ? error::file_not_found : permissions_error;   // ENOENT; EACCES, EEXIST, ...
    }
    return fd;
}

} // namespace

std::int32_t open_guest_audio(Support& support, const std::string& path, GuestAudio& out, bool read_write) {
    std::int32_t result{};
    const auto fd = open_guest(support, path, read_write ? o_rdwr : 0, result);
    if (fd < 0) return result == permissions_error ? permissions_error : error::file_not_found;
    GuestAudio audio;
    audio.fd = fd;
    const auto end = static_cast<std::int64_t>(support.call(libsystem, "_lseek", {static_cast<Address>(fd), 0, 2}));
    audio.size = end > 0 ? static_cast<std::uint64_t>(end) : 0;
    std::string why;
    const ReadAt read = [&support, fd](std::uint64_t offset, std::span<std::uint8_t> bytes) { return read_guest(support, fd, offset, bytes); };
    result = parse(read, audio.size, audio.container, why);
    if (result == error::unsupported_file_type || result == error::unsupported_data_format) {
        if (read_write) result = error::operation_not_supported;   // compressed files are read-only
        else result = open_audio_stream(audio, read, result, why);
    }
    if (result) {
        static_cast<void>(support.call(libsystem, "_close", {static_cast<Address>(fd)}));
        return result;
    }
    out = std::move(audio);
    return 0;
}

std::int32_t open_audio_stream(GuestAudio& audio, ReadAt read, std::int32_t pcm_status, std::string& why) {
    std::int32_t result{};
    auto stream = audio_stream::Stream::open(std::move(read), audio.size, result, why);
    if (!stream) return pcm_status == error::unsupported_data_format ? pcm_status : result;
    Container c;
    c.file_type = stream->file_type();
    c.format.rate = stream->rate();
    c.format.channels = stream->channels();
    c.format.is_float = true;
    c.format.bits = 32;
    c.format.container = 4;
    c.data_offset = 0;
    c.data_bytes = stream->frames() * c.format.frame_bytes();
    audio.container = c;
    audio.stream = std::move(stream);
    return 0;
}

void close_guest_audio(Support& support, GuestAudio& audio) {
    if (audio.fd >= 0) static_cast<void>(support.call(libsystem, "_close", {static_cast<Address>(std::exchange(audio.fd, -1))}));
}

std::size_t read_audio_frames(const GuestAudio& audio, const ReadAt& read, std::uint64_t start, std::size_t count, std::vector<double>& out) {
    const auto total = audio.frames();
    if (start >= total) return 0;
    count = static_cast<std::size_t>(std::min<std::uint64_t>(count, total - start));
    if (audio.stream) {
        std::vector<float> decoded;
        const auto got = audio.stream->read(start, count, decoded);
        out.insert(out.end(), decoded.begin(), decoded.end());
        return got;
    }
    const auto& format = audio.container.format;
    std::vector<std::uint8_t> bytes(count * format.frame_bytes());
    const auto got = read(audio.container.data_offset + start * format.frame_bytes(), bytes) / format.frame_bytes();
    if (!got) return 0;
    const auto samples = decode(format, {std::span<const std::uint8_t>(bytes.data(), got * format.frame_bytes())}, got);
    out.insert(out.end(), samples.begin(), samples.end());
    return got;
}

std::size_t read_audio_frames(Support& support, const GuestAudio& audio, std::uint64_t start, std::size_t count, std::vector<double>& out) {
    const auto fd = audio.fd;
    return read_audio_frames(audio, [&support, fd](std::uint64_t offset, std::span<std::uint8_t> bytes) { return read_guest(support, fd, offset, bytes); },
                             start, count, out);
}

void install_audio_file(Support& support) {
    auto state = std::make_shared<AudioFiles>(support);
    auto* sp = &support;
    const auto lib = audio_toolbox;

    // AudioFileOpenURL(CFURLRef, AudioFilePermissions (SInt8: 1 read, 2 write, 3 both), AudioFileTypeID hint, AudioFileID *out)
    support.function(lib, "_AudioFileOpenURL", [state, sp](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[3]) return status(s, error::param);
        m.write64(s.x[3], 0);
        const auto permissions = static_cast<std::int8_t>(s.x[1]);
        if (permissions < 1 || permissions > 3) return status(s, error::param);
        const auto path = sp->cf.url_path(s.x[0]);
        std::lock_guard lock(state->mutex);
        File file;
        if (const auto result = open_guest_audio(*sp, path, file.audio, permissions != 1)) return status(s, result);
        if (permissions != 1) {
            if (const auto result = state->make_writable(file)) {
                close_guest_audio(*sp, file.audio);
                return status(s, result);
            }
        }
        m.write64(s.x[3], state->adopt(std::move(file)));
        status(s, 0);
    });
    // AudioFileCreateWithURL(CFURLRef, AudioFileTypeID, const AudioStreamBasicDescription *, AudioFileFlags, AudioFileID *out)
    support.function(lib, "_AudioFileCreateWithURL", [state, sp](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[2] || !s.x[4]) return status(s, error::param);
        m.write64(s.x[4], 0);
        std::vector<std::uint8_t> asbd(40);
        m.read_bytes(s.x[2], asbd);
        std::lock_guard lock(state->mutex);
        File file;
        std::int32_t result{};
        // Without kAudioFileFlags_EraseFile an existing file is not replaced.
        const auto flags = o_rdwr | o_creat | ((s.x[3] & flag_erase_file) ? o_trunc : o_excl);
        file.audio.fd = open_guest(*sp, sp->cf.url_path(s.x[0]), flags, result);
        if (file.audio.fd < 0) return status(s, result);
        if ((result = state->start_file(file, static_cast<std::uint32_t>(s.x[1]), asbd))) {
            close_guest_audio(*sp, file.audio);
            return status(s, result);
        }
        m.write64(s.x[4], state->adopt(std::move(file)));
        status(s, 0);
    });
    // AudioFileOpenWithCallbacks(client, read, write, getSize, setSize, AudioFileTypeID hint, AudioFileID *out)
    support.function(lib, "_AudioFileOpenWithCallbacks", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || !s.x[3] || !s.x[6]) return status(s, error::param);
        m.write64(s.x[6], 0);
        std::lock_guard lock(state->mutex);
        File file;
        file.client = s.x[0];
        file.read_proc = s.x[1];
        file.write_proc = s.x[2];
        file.size_proc = s.x[3];
        file.set_size_proc = s.x[4];
        file.audio.size = state->size_of(file);
        std::string why;
        auto result = parse([&state, &file](std::uint64_t offset, std::span<std::uint8_t> bytes) { return state->read_at(file, offset, bytes); },
                            file.audio.size, file.audio.container, why);
        if (result == error::unsupported_file_type || result == error::unsupported_data_format) {
            // A compressed file: its stream reads through a copy of the callbacks.
            auto* files = state.get();
            result = open_audio_stream(file.audio, [files, callbacks = file](std::uint64_t offset, std::span<std::uint8_t> bytes) {
                return files->read_at(callbacks, offset, bytes);
            }, result, why);
        }
        if (result) return status(s, result);
        if (file.write_proc && !file.audio.stream) static_cast<void>(state->make_writable(file));   // read-only unless its header can be rewritten
        m.write64(s.x[6], state->adopt(std::move(file)));
        status(s, 0);
    });
    // AudioFileInitializeWithCallbacks(client, read, write, getSize, setSize, AudioFileTypeID,
    //     const AudioStreamBasicDescription *, AudioFileFlags, AudioFileID *out (on the stack))
    support.function(lib, "_AudioFileInitializeWithCallbacks", [state](CpuState& s, GuestMemory& m) {
        const auto out = m.read64(s.sp);
        if (!s.x[2] || !s.x[3] || !s.x[6] || !out) return status(s, error::param);
        m.write64(out, 0);
        std::vector<std::uint8_t> asbd(40);
        m.read_bytes(s.x[6], asbd);
        std::lock_guard lock(state->mutex);
        File file;
        file.client = s.x[0];
        file.read_proc = s.x[1];
        file.write_proc = s.x[2];
        file.size_proc = s.x[3];
        file.set_size_proc = s.x[4];
        if (const auto result = state->start_file(file, static_cast<std::uint32_t>(s.x[5]), asbd)) return status(s, result);
        if (file.set_size_proc) static_cast<void>(state->call(file.set_size_proc, {file.client, file.container().data_offset}));
        m.write64(out, state->adopt(std::move(file)));
        status(s, 0);
    });
    support.function(lib, "_AudioFileClose", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        if (!state->file(s.x[0])) return status(s, error::param);
        for (const auto& [handle, ext] : state->exts)
            if (ext.file == s.x[0] && ext.owns_file) return status(s, error::param);   // owned by an ExtAudioFile
        state->close(s.x[0]);
        status(s, 0);
    });
    // AudioFileOptimize(file): the header's sizes brought up to date.
    support.function(lib, "_AudioFileOptimize", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        auto* f = state->file(s.x[0]);
        if (!f) return status(s, error::param);
        status(s, state->finish(*f));
    });
    // AudioFileGetPropertyInfo(file, AudioFilePropertyID, UInt32 *outDataSize, UInt32 *isWritable)
    support.function(lib, "_AudioFileGetPropertyInfo", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto* f = state->file(s.x[0]);
        if (!f) return status(s, error::param);
        AudioFiles::Value value;
        if (const auto result = state->file_property(*f, static_cast<std::uint32_t>(s.x[1]), value)) return status(s, result);
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(value.bytes.size()));
        if (s.x[3]) m.write32(s.x[3], value.writable ? 1 : 0);
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
    // AudioFileSetProperty(file, AudioFilePropertyID, UInt32 size, const void *data): the data format
    // and (CAF) the channel layout of a writable file before any data is written.
    support.function(lib, "_AudioFileSetProperty", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* f = state->file(s.x[0]);
        if (!f) return status(s, error::param);
        if (!f->writable) return status(s, error::operation_not_supported);
        const auto size = static_cast<std::uint32_t>(s.x[2]);
        if (size > (1 << 16) || (size && !s.x[3])) return status(s, error::param);
        std::vector<std::uint8_t> data(size);
        if (size) m.read_bytes(s.x[3], data);
        const auto id = static_cast<std::uint32_t>(s.x[1]);
        if (id != four("dfmt") && id != four("cmap")) return status(s, error::unsupported_property);
        if (f->container().data_bytes) return status(s, error::operation_not_supported);   // the data is written in the old layout
        if (id == four("cmap") && f->container().file_type != four("caff")) return status(s, error::unsupported_property);
        if (id == four("dfmt") && size < 40) return status(s, error::bad_property_size);
        if (id == four("cmap") && size < 12) return status(s, error::bad_property_size);
        auto changed = *f;
        if (id == four("cmap")) changed.layout = std::move(data);
        std::vector<std::uint8_t> header;
        std::string why;
        if (id == four("dfmt")) {
            const auto format = pcm_from_asbd(data, why);
            if (!format) return status(s, error::unsupported_data_format);
            changed.audio.container.format = *format;
        }
        if (const auto result = write_header(changed.container().file_type, changed.container().format, 0, changed.layout, header, why))
            return status(s, result);
        changed.audio.container.data_offset = header.size();
        if (const auto result = state->write_at(changed, 0, header)) return status(s, result);
        *f = std::move(changed);
        status(s, 0);
    });
    // AudioFileReadBytes(file, Boolean useCache, SInt64 startingByte, UInt32 *ioNumBytes, void *outBuffer)
    support.function(lib, "_AudioFileReadBytes", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* f = state->file(s.x[0]);
        if (!f || !s.x[3] || !s.x[4]) return status(s, error::param);
        const auto start = static_cast<std::int64_t>(s.x[2]);
        if (start < 0) return status(s, error::invalid_packet_offset);
        const auto wanted = m.read32(s.x[3]);
        if (f->audio.stream) return status(s, error::operation_not_supported);   // compressed packets are read with ReadPacketData
        const auto& c = f->container();
        const auto available = static_cast<std::uint64_t>(start) >= c.data_bytes ? 0 : c.data_bytes - static_cast<std::uint64_t>(start);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<std::uint64_t>(wanted, available)));
        const auto got = state->read_at(*f, c.data_offset + static_cast<std::uint64_t>(start), bytes);
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
        if (f->audio.stream) {
            // Compressed packets: as many whole ones as fit, with their descriptions
            // (AudioStreamPacketDescription {SInt64 start; UInt32 variableFrames; UInt32 bytes}).
            const auto& st = *f->audio.stream;
            const auto room = m.read32(s.x[2]);
            const auto wanted = m.read32(s.x[5]);
            std::vector<std::uint8_t> bytes;
            std::uint32_t count = 0;
            for (auto p = static_cast<std::uint64_t>(packet); count < wanted && p < st.packet_count(); ++p, ++count) {
                const auto data = st.packet(static_cast<std::size_t>(p));
                if (data.empty() || bytes.size() + data.size() > room) break;
                if (s.x[3]) {
                    const auto entry = s.x[3] + 16ULL * count;
                    const auto frames = static_cast<std::uint32_t>(st.packet_start(static_cast<std::size_t>(p) + 1) - st.packet_start(static_cast<std::size_t>(p)));
                    m.write64(entry, bytes.size());
                    m.write32(entry + 8, frames == st.frames_per_packet() ? 0 : frames);
                    m.write32(entry + 12, static_cast<std::uint32_t>(data.size()));
                }
                bytes.insert(bytes.end(), data.begin(), data.end());
            }
            if (!bytes.empty()) m.write_bytes(s.x[6], bytes);
            m.write32(s.x[2], static_cast<std::uint32_t>(bytes.size()));
            m.write32(s.x[5], count);
            return status(s, count ? 0 : error::end_of_file);
        }
        const auto frame_bytes = f->container().format.frame_bytes();
        auto count = std::min<std::uint64_t>(m.read32(s.x[5]), m.read32(s.x[2]) / frame_bytes);
        const auto total = f->frames();
        count = static_cast<std::uint64_t>(packet) >= total ? 0 : std::min<std::uint64_t>(count, total - static_cast<std::uint64_t>(packet));
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count * frame_bytes));
        const auto got = state->read_at(*f, f->container().data_offset + static_cast<std::uint64_t>(packet) * frame_bytes, bytes) / frame_bytes * frame_bytes;
        if (got) m.write_bytes(s.x[6], std::span<const std::uint8_t>(bytes.data(), got));
        m.write32(s.x[2], static_cast<std::uint32_t>(got));
        m.write32(s.x[5], static_cast<std::uint32_t>(got / frame_bytes));
        status(s, got ? 0 : error::end_of_file);
    });
    // AudioFileWriteBytes(file, Boolean useCache, SInt64 startingByte, UInt32 *ioNumBytes, const void *buffer)
    support.function(lib, "_AudioFileWriteBytes", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* f = state->file(s.x[0]);
        if (!f || !s.x[3] || !s.x[4]) return status(s, error::param);
        const auto start = static_cast<std::int64_t>(s.x[2]);
        if (start < 0) return status(s, error::invalid_packet_offset);
        std::vector<std::uint8_t> bytes(m.read32(s.x[3]));
        m.read_bytes(s.x[4], bytes);
        const auto result = state->write_data(*f, static_cast<std::uint64_t>(start), bytes);
        if (result) m.write32(s.x[3], 0);
        status(s, result);
    });
    // AudioFileWritePackets(file, Boolean useCache, UInt32 numBytes, const AudioStreamPacketDescription *,
    //     SInt64 startingPacket, UInt32 *ioNumPackets, const void *buffer): PCM packets are frames.
    support.function(lib, "_AudioFileWritePackets", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* f = state->file(s.x[0]);
        if (!f || !s.x[5] || !s.x[6]) return status(s, error::param);
        const auto packet = static_cast<std::int64_t>(s.x[4]);
        if (packet < 0) return status(s, error::invalid_packet_offset);
        const auto frame_bytes = f->container().format.frame_bytes();
        const auto count = std::min<std::uint64_t>(m.read32(s.x[5]), static_cast<std::uint32_t>(s.x[2]) / frame_bytes);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count * frame_bytes));
        m.read_bytes(s.x[6], bytes);
        const auto result = state->write_data(*f, static_cast<std::uint64_t>(packet) * frame_bytes, bytes);
        m.write32(s.x[5], result ? 0 : static_cast<std::uint32_t>(count));
        status(s, result);
    });

    // ---- ExtAudioFile --------------------------------------------------------------------------
    const auto make_ext = [state](Address file, bool owns) {
        Ext e;
        e.file = file;
        e.owns_file = owns;
        e.client = state->file(file)->container().format;   // the file's own format until a client format is set
        const auto handle = state->support.process.allocate_guest(16, 16);
        state->exts[handle] = std::move(e);
        return handle;
    };
    // ExtAudioFileWrapAudioFileID(AudioFileID, Boolean forWriting, ExtAudioFileRef *out)
    support.function(lib, "_ExtAudioFileWrapAudioFileID", [state, make_ext](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, error::param);
        m.write64(s.x[2], 0);
        std::lock_guard lock(state->mutex);
        const auto* f = state->file(s.x[0]);
        if (!f) return status(s, error::param);
        if ((s.x[1] & 0xff) && !f->writable) return status(s, permissions_error);
        auto handle = make_ext(s.x[0], false);
        if (s.x[1] & 0xff) state->ext(handle)->position = f->frames();   // writing appends
        m.write64(s.x[2], handle);
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
    // ExtAudioFileCreateWithURL(CFURLRef, AudioFileTypeID, const AudioStreamBasicDescription *fileFormat,
    //     const AudioChannelLayout *, UInt32 flags, ExtAudioFileRef *out): the client format starts as the file's.
    support.function(lib, "_ExtAudioFileCreateWithURL", [state, sp, make_ext](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[2] || !s.x[5]) return status(s, error::param);
        m.write64(s.x[5], 0);
        std::vector<std::uint8_t> asbd(40);
        m.read_bytes(s.x[2], asbd);
        std::lock_guard lock(state->mutex);
        File file;
        if (s.x[3] && static_cast<std::uint32_t>(s.x[1]) == four("caff")) {
            const auto count = m.read32(s.x[3] + 8);
            if (count <= 64) {
                file.layout.resize(12 + 20ULL * count);
                m.read_bytes(s.x[3], file.layout);
            }
        }
        std::int32_t result{};
        const auto flags = o_rdwr | o_creat | ((s.x[4] & flag_erase_file) ? o_trunc : o_excl);
        file.audio.fd = open_guest(*sp, sp->cf.url_path(s.x[0]), flags, result);
        if (file.audio.fd < 0) return status(s, result);
        if ((result = state->start_file(file, static_cast<std::uint32_t>(s.x[1]), asbd))) {
            close_guest_audio(*sp, file.audio);
            return status(s, result);
        }
        m.write64(s.x[5], make_ext(state->adopt(std::move(file)), true));
        status(s, 0);
    });
    support.function(lib, "_ExtAudioFileDispose", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        auto* e = state->ext(s.x[0]);
        if (!e) return status(s, error::param);
        if (e->owns_file) state->close(e->file);
        else if (auto* f = state->file(e->file)) static_cast<void>(state->finish(*f));
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
            // Reading converts file -> client, writing client -> file (same rate either way).
            if (!client || !convertible(f->container().format, *client) || !convertible(*client, f->container().format))
                return status(s, error::unsupported_data_format);   // kAudioConverterErr_FormatNotSupported
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
    // ExtAudioFileWrite / ExtAudioFileWriteAsync(ext, UInt32 numberFrames, const AudioBufferList *): the
    // client frames, converted, at the position. The asynchronous form writes at once (a call with no
    // frames, which primes it, writes nothing).
    for (const auto* name : {"_ExtAudioFileWrite", "_ExtAudioFileWriteAsync"})
        support.function(lib, name, [state](CpuState& s, GuestMemory& m) {
            std::lock_guard lock(state->mutex);
            auto* e = state->ext(s.x[0]);
            if (!e) return status(s, error::param);
            status(s, state->write(*e, m, static_cast<std::uint32_t>(s.x[1]), s.x[2]));
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
}

} // namespace alchemy::gui::completion_kit::media
