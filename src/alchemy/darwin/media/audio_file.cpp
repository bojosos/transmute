#include "alchemy/darwin/media/audio_file.hpp"
#include "alchemy/darwin/media/aac_encoder.hpp"
#include "alchemy/darwin/media/media_codecs.hpp"
#include "alchemy/darwin/media/packet_file.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <optional>
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
// - Compressed files with a decoder (audio_stream.hpp: MP3, FLAC, Ogg Opus, Apple Lossless in CAF,
//   MP4 and Matroska audio in those codecs) open read-only: AudioFile describes and reads their packets (data format,
//   packet counts and sizes, magic cookie, packet table, ReadPacketData with descriptions;
//   ReadBytes answers kAudioFileOperationNotSupportedError), and ExtAudioFile reads them decoded.
//   Other compressed data (AAC, ...) answers kAudioFileUnsupportedDataFormatError.
// - Writing Apple Lossless and AAC: CAF and MPEG-4 (M4A) files of 'alac' or 'aac ' packets (packet_file.hpp;
//   AAC through the host's encoder, aac_encoder.hpp, its delay as the priming).
//   AudioFileWritePackets appends packets with their descriptions (from the next packet only), the
//   magic cookie and packet table info are set as properties, and AudioFileOptimize and
//   AudioFileClose write the packet table or the movie. ExtAudioFile writes them from any linear
//   PCM client format: frames are encoded (media_codecs.hpp) in whole packets as they come, the
//   last, shorter one and the cookie at ExtAudioFileDispose. Such a file is not read back through
//   the handle that writes it (kAudioFileOperationNotSupportedError).
// - Writing: AudioFileCreateWithURL and InitializeWithCallbacks make files in the format given,
//   which the container must hold (kAudioFileUnsupportedDataFormatError otherwise); bytes and
//   packets go into the data, and the header's sizes are brought up to date by AudioFileOptimize
//   and AudioFileClose. ExtAudioFileCreateWithURL / ExtAudioFileWrite convert from the client
//   format (same rate). An existing file opens for writing when its header is the one Alchemy
//   would write for it (a canonical WAVE, or an Alchemy-written file); others answer
//   kAudioFileOperationNotSupportedError. ExtAudioFileWriteAsync writes at once.
// - User data (AudioFileCountUserData and the rest): the chunks of a WAVE, AIFF/AIFC or CAF file
//   other than its audio data, by ID and index. In a file being written, the chunks of the header
//   Alchemy writes are fixed; others are set, replaced and removed in memory and follow the audio
//   data once the header is brought up to date (AudioFileOptimize, AudioFileClose). Compressed files
//   answer kAudioFileOperationNotSupportedError.
// - AudioFileGetGlobalInfo / GetGlobalInfoSize describe the file types above: the readable and
//   writable types, their names, extensions, MIME types, UTIs and HFS codes (and the reverse
//   lookups), their data formats and the stream descriptions each holds.
namespace alchemy::gui::completion_kit::media {
namespace {

using namespace audio_file;
constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

// <ExtendedAudioFile.h>
constexpr std::int32_t ext_invalid_property = -66561, ext_invalid_property_size = -66562, ext_non_pcm_client_format = -66563,
    ext_invalid_seek = -66568;
constexpr std::int32_t permissions_error = static_cast<std::int32_t>(four("prm?")), invalid_chunk = static_cast<std::int32_t>(four("chk?"));
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
    // User data: the chunks other than the audio data, found at first use. Fixed: part of the header
    // of a file being written. Held: its bytes in memory (the chunks after a written file's data).
    struct Chunk { std::uint32_t id{}; std::uint64_t offset{}, size{}; std::vector<std::uint8_t> bytes; bool fixed{}, held{}; };
    std::optional<std::vector<Chunk>> chunks;
    // A compressed file being written (packet_file.hpp). container().format is then the linear PCM
    // ExtAudioFile converts its client frames to before encoding them: 32-bit signed integers.
    std::optional<packet_file::Writer> packets;
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
    // Writing a compressed file: the encoder, and the frames short of a whole packet (left-justified).
    std::shared_ptr<codecs::AudioEncoder> encoder;
    std::vector<std::int32_t> unencoded;
    // AAC: the encoder and the frames it has taken.
    std::shared_ptr<aac::Encoder> aac;
    std::uint64_t aac_frames{};
};

// AVAudioFile's and AVAssetWriter's AVEncoderBitRateKey for the ExtAudioFile they write through
// (set_encode_bit_rate in audio_file.hpp).
std::mutex bit_rate_mutex;
std::map<Address, std::uint32_t> bit_rates;

void status(CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

// The file types AudioFileGetGlobalInfo describes: those Alchemy reads (audio_file_core.hpp and the
// compressed streams of audio_stream.hpp) and writes (linear PCM, and Apple Lossless in CAF and MPEG-4).
struct FileType {
    std::uint32_t type;
    const char* name;
    std::vector<const char*> extensions, mime, utis;
    std::vector<std::uint32_t> formats;
    bool writable;
};
const std::vector<FileType>& file_types() {
    static const std::vector<FileType> types{
        {four("WAVE"), "WAVE", {"wav", "wave"}, {"audio/wav", "audio/x-wav", "audio/vnd.wave"}, {"com.microsoft.waveform-audio"}, {four("lpcm")}, true},
        {four("AIFF"), "AIFF", {"aif", "aiff"}, {"audio/aiff", "audio/x-aiff"}, {"public.aiff-audio"}, {four("lpcm")}, true},
        {four("AIFC"), "AIFC", {"aifc", "aiff", "aif"}, {"audio/aiff", "audio/x-aiff"}, {"public.aifc-audio"}, {four("lpcm")}, true},
        {four("caff"), "CAF", {"caf"}, {"audio/x-caf"}, {"com.apple.coreaudio-format"}, {four("lpcm"), four("alac"), four("aac ")}, true},
        {four("MPG3"), "MPEG Layer 3", {"mp3"}, {"audio/mpeg", "audio/mpeg3", "audio/mp3", "audio/x-mpeg3"}, {"public.mp3"}, {four(".mp3")}, false},
        {four("MPG2"), "MPEG Layer 2", {"mp2"}, {"audio/mpeg"}, {}, {four(".mp2")}, false},
        {four("MPG1"), "MPEG Layer 1", {"mp1"}, {"audio/mpeg"}, {}, {four(".mp1")}, false},
        {four("flac"), "FLAC", {"flac"}, {"audio/flac"}, {"org.xiph.flac"}, {four("flac")}, false},
        {four("m4af"), "Apple MPEG-4 Audio", {"m4a", "m4r"}, {"audio/mp4", "audio/x-m4a"}, {"com.apple.m4a-audio"},
         {four(".mp3"), four("flac"), four("opus"), four("alac"), four("aac ")}, true},
        {four("mp4f"), "MPEG-4 Audio", {"mp4"}, {"audio/mp4"}, {"public.mpeg-4-audio"}, {four(".mp3"), four("flac"), four("opus"), four("alac"), four("aac ")}, true},
    };
    return types;
}
const FileType* file_type(std::uint32_t type) {
    for (const auto& t : file_types())
        if (t.type == type) return &t;
    return nullptr;
}
bool same_text(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

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
    // The header's sizes, brought up to date, and the held user data after the audio data.
    std::int32_t finish(File& f) {
        if (!f.writable || !f.dirty) return 0;
        if (f.packets) {
            for (const auto& [offset, bytes] : f.packets->finish()) {
                if (const auto result = write_at(f, offset, bytes)) return result;
                f.audio.size = std::max<std::uint64_t>(f.audio.size, offset + bytes.size());
            }
            f.dirty = false;
            return 0;
        }
        std::vector<std::uint8_t> bytes;
        if (const auto result = header(f, bytes)) return result;
        if (const auto result = write_at(f, 0, bytes)) return result;
        auto end = f.container().data_offset + f.container().data_bytes;
        if (f.chunks && std::any_of(f.chunks->begin(), f.chunks->end(), [](const File::Chunk& c) { return c.held; })) {
            const auto type = f.container().file_type;
            const bool caf = type == four("caff"), little = type == four("WAVE");
            std::vector<std::uint8_t> tail;
            if (!caf && (end & 1)) tail.push_back(0);
            const auto put = [&](std::uint64_t value, int count, bool le) {
                for (int i = 0; i < count; ++i) tail.push_back(static_cast<std::uint8_t>(value >> (8 * (le ? i : count - 1 - i))));
            };
            for (const auto& c : *f.chunks) {
                if (!c.held) continue;
                put(c.id, 4, false);
                put(c.bytes.size(), caf ? 8 : 4, little);
                tail.insert(tail.end(), c.bytes.begin(), c.bytes.end());
                if (!caf && (c.bytes.size() & 1)) tail.push_back(0);
            }
            if (const auto result = write_at(f, end, tail)) return result;
            end += tail.size();
            if (!caf) {   // the RIFF or FORM size covers them
                std::vector<std::uint8_t> outer;
                for (int i = 0; i < 4; ++i) outer.push_back(static_cast<std::uint8_t>((end - 8) >> (8 * (little ? i : 3 - i))));
                if (const auto result = write_at(f, 4, outer)) return result;
            }
            f.audio.size = end;
        }
        if (f.audio.fd >= 0) {
            if (size_of(f) > end) static_cast<void>(support.call(libsystem, "_ftruncate", {static_cast<Address>(f.audio.fd), end}));
        } else if (f.set_size_proc) {
            if (size_of(f) > end) static_cast<void>(call(f.set_size_proc, {f.client, end}));   // OSStatus (*)(void *client, SInt64 size)
        }
        f.dirty = false;
        return 0;
    }

    // ---- user data ---------------------------------------------------------------------------------
    // The chunks in [at, end): all but the audio data chunk (stopping at it when `stop_at_audio`);
    // with `hold` their bytes are read into memory.
    void scan_chunks(File& f, std::uint64_t at, std::uint64_t end, bool stop_at_audio, bool fixed, bool hold, std::vector<File::Chunk>& out) {
        const auto type = f.container().file_type;
        const bool caf = type == four("caff"), little = type == four("WAVE");
        const std::uint64_t head = caf ? 12 : 8;
        const auto audio = caf || little ? four("data") : four("SSND");
        while (at + head <= end) {
            std::array<std::uint8_t, 12> h{};
            if (read_at(f, at, std::span(h.data(), head)) != head) break;
            const auto id = (std::uint32_t(h[0]) << 24) | (h[1] << 16) | (h[2] << 8) | h[3];
            std::uint64_t length = 0;
            for (int i = 0; i < (caf ? 8 : 4); ++i) length |= std::uint64_t(h[4 + i]) << (8 * (little ? i : (caf ? 7 : 3) - i));
            const auto body = at + head;
            const bool open_ended = caf && length == ~0ULL;
            if (open_ended || body + length > end) length = end - std::min(end, body);
            if (id == audio) {
                if (stop_at_audio) return;
            } else {
                File::Chunk c{id, body, length, {}, fixed, hold};
                if (hold) {
                    c.bytes.resize(static_cast<std::size_t>(length));
                    c.bytes.resize(read_at(f, body, c.bytes));
                    c.size = c.bytes.size();
                }
                out.push_back(std::move(c));
            }
            if (open_ended) return;
            at = body + length + (!caf && (length & 1));
        }
    }
    std::int32_t user_chunks(File& f, std::vector<File::Chunk>*& out) {
        if (f.audio.stream || f.packets) return error::operation_not_supported;
        if (!f.chunks) {
            std::vector<File::Chunk> list;
            const std::uint64_t first = f.container().file_type == four("caff") ? 8 : 12;
            if (f.writable) {
                // The header's chunks, then whatever follows the audio data (held, to be written again after it).
                scan_chunks(f, first, f.container().data_offset, true, true, false, list);
                auto after = f.container().data_offset + f.container().data_bytes;
                if (f.container().file_type != four("caff") && (after & 1)) ++after;
                scan_chunks(f, after, size_of(f), false, false, true, list);
            } else {
                scan_chunks(f, first, f.audio.size, false, false, false, list);
            }
            f.chunks = std::move(list);
        }
        out = &*f.chunks;
        return 0;
    }
    static File::Chunk* nth_chunk(std::vector<File::Chunk>& list, std::uint32_t id, std::uint32_t index) {
        for (auto& c : list)
            if (c.id == id && index-- == 0) return &c;
        return nullptr;
    }
    std::size_t chunk_bytes(const File& f, const File::Chunk& c, std::uint64_t offset, std::span<std::uint8_t> out) {
        if (offset >= c.size) return 0;
        out = out.first(static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), c.size - offset)));
        if (c.held) {
            std::memcpy(out.data(), c.bytes.data() + offset, out.size());
            return out.size();
        }
        return read_at(f, c.offset + offset, out);
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
        if (format_id != four("lpcm") && packet_file::writable(type, format_id)) return start_packet_file(f, type, asbd);
        if (type == four("m4af") || type == four("mp4f")) return error::unsupported_data_format;
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

    // A new compressed file: the header written, no packets.
    std::int32_t start_packet_file(File& f, std::uint32_t type, std::span<const std::uint8_t> asbd) {
        packet_file::Format format;
        format.file_type = type;
        std::memcpy(&format.rate, asbd.data(), 8);
        std::memcpy(&format.format_id, asbd.data() + 8, 4);
        std::memcpy(&format.flags, asbd.data() + 12, 4);
        std::memcpy(&format.frames_per_packet, asbd.data() + 20, 4);
        std::memcpy(&format.channels, asbd.data() + 28, 4);
        std::string why;
        auto writer = packet_file::Writer::make(format, why);
        if (!writer) return error::unsupported_data_format;
        Pcm source;
        source.rate = format.rate;
        source.channels = format.channels;
        source.bits = 32;
        source.container = 4;
        f.audio.container.file_type = type;
        f.audio.container.format = source;
        f.audio.container.data_offset = writer->data_offset();
        f.audio.container.data_bytes = 0;
        f.writable = true;
        if (const auto result = write_at(f, 0, writer->header())) return result;
        f.audio.size = writer->data_offset();
        f.packets = std::move(*writer);
        f.dirty = true;
        return 0;
    }
    // A packet written at the data's end.
    std::int32_t append_packet(File& f, std::span<const std::uint8_t> bytes, std::uint32_t frames) {
        auto& w = *f.packets;
        if (const auto result = write_at(f, w.data_end(), bytes)) return result;
        w.add_packet(static_cast<std::uint32_t>(bytes.size()), frames);
        f.audio.container.data_bytes = w.data_bytes();
        f.audio.size = std::max<std::uint64_t>(f.audio.size, w.data_end());
        f.dirty = true;
        return 0;
    }
    // The compressed description of a file being written.
    static std::vector<std::uint8_t> packet_asbd(const packet_file::Writer& w) {
        std::vector<std::uint8_t> out(40, 0);
        const auto& format = w.format();
        std::memcpy(out.data(), &format.rate, 8);
        std::memcpy(out.data() + 8, &format.format_id, 4);
        std::memcpy(out.data() + 12, &format.flags, 4);
        std::memcpy(out.data() + 20, &format.frames_per_packet, 4);
        std::memcpy(out.data() + 28, &format.channels, 4);
        return out;
    }
    // The frames of a file being written: its packets' less the priming and remainder.
    static std::uint64_t packet_frames(const packet_file::Writer& w) {
        const std::uint64_t trimmed = std::uint64_t{w.priming()} + w.remainder();
        return w.frames() > trimmed ? w.frames() - trimmed : 0;
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
    // A compressed file being written.
    std::int32_t packet_property(const File& f, std::uint32_t id, Value& out) {
        const auto& w = *f.packets;
        switch (id) {
        case four("ffmt"): out.bytes = raw(f.container().file_type); return 0;
        case four("dfmt"): out.bytes = packet_asbd(w); return 0;
        case four("bcnt"): out.bytes = raw(w.data_bytes()); return 0;
        case four("pcnt"): out.bytes = raw(w.packet_count()); return 0;
        case four("psze"): out.bytes = raw(w.maximum_packet_bytes()); return 0;
        case four("pkub"):   // a packet stored verbatim
            out.bytes = raw(std::max(w.maximum_packet_bytes(), w.format().frames_per_packet * w.format().channels * 4 + 64));
            return 0;
        case four("doff"): out.bytes = raw(static_cast<std::int64_t>(w.data_offset())); return 0;
        case four("optm"): out.bytes = raw(std::uint32_t{f.dirty ? 0U : 1U}); return 0;
        case four("edur"): out.bytes = raw(static_cast<double>(packet_frames(w)) / w.format().rate); return 0;
        case four("mgic"): out.bytes = w.cookie(); out.writable = true; return 0;
        case four("pnfo"): {   // AudioFilePacketTableInfo {SInt64 valid; SInt32 priming; SInt32 remainder}
            out.bytes = raw(static_cast<std::int64_t>(packet_frames(w)));
            const auto priming = raw(w.priming()), remainder = raw(w.remainder());
            out.bytes.insert(out.bytes.end(), priming.begin(), priming.end());
            out.bytes.insert(out.bytes.end(), remainder.begin(), remainder.end());
            out.writable = true;
            return 0;
        }
        case four("flst"): {
            out.bytes = packet_asbd(w);
            const auto tag = raw(0xffff0000U | w.format().channels);
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
        if (f.packets) return packet_property(f, id, out);
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
        case four("ffmt"):   // FileDataFormat
            out.bytes = f->audio.stream ? f->audio.stream->asbd() : f->packets ? packet_asbd(*f->packets) : asbd_from_pcm(f->container().format);
            return 0;
        case four("cfmt"): out.bytes = asbd_from_pcm(e.client); out.writable = true; return 0;      // ClientDataFormat
        case four("fclo"):                                                                           // FileChannelLayout
            out.bytes = !f->layout.empty() ? f->layout : layout_for_mask(f->container().channel_mask, f->container().format.channels);
            return out.bytes.empty() ? ext_invalid_property : 0;
        case four("cclo"):                                                                           // ClientChannelLayout
            out.bytes = !e.client_layout.empty() ? e.client_layout : layout_for_mask(f->container().channel_mask, e.client.channels);
            out.writable = true;
            return out.bytes.empty() ? ext_invalid_property : 0;
        case four("#frm"):                                                                           // FileLengthFrames
            out.bytes = raw(static_cast<std::int64_t>(f->packets ? packet_frames(*f->packets) + e.unencoded.size() / f->packets->format().channels : f->frames()));
            return 0;
        case four("afil"): out.bytes = raw(e.file); return 0;                                       // AudioFile
        case four("fmps"):                                                                           // FileMaxPacketSize
            out.bytes = raw(f->audio.stream ? f->audio.stream->maximum_packet_bytes()
                            : f->packets ? (e.encoder ? e.encoder->max_packet_bytes : f->packets->maximum_packet_bytes())
                                         : f->container().format.frame_bytes());
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
        if (f->packets) return error::operation_not_supported;   // a compressed file being written
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
    std::int32_t write(Ext& e, GuestMemory& m, std::uint32_t requested, Address list, Address handle) {
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
        if (f->packets) return encode_frames(e, *f, m, data, frames, handle);
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
    // ExtAudioFileWrite into a compressed file: the client frames as 32-bit integers, rounded to the
    // source bit depth, encoded a whole packet at a time.
    std::int32_t encode_frames(Ext& e, File& f, GuestMemory& m, const std::vector<Address>& data, std::uint64_t frames, Address handle) {
        auto& w = *f.packets;
        const auto& format = w.format();
        const bool is_aac = format.format_id == four("aac ");
        const std::uint32_t bits = is_aac || format.flags == 1 ? 16 : format.flags == 2 ? 20 : format.flags == 3 ? 24 : 32;
        if (is_aac && !e.aac) {
            std::uint32_t bit_rate = 0;
            {
                std::lock_guard lock(bit_rate_mutex);
                if (const auto found = bit_rates.find(handle); found != bit_rates.end()) bit_rate = found->second;
            }
            std::string why;
            e.aac = aac::open(static_cast<std::uint32_t>(format.rate), format.channels, bit_rate, why);
            if (!e.aac) return error::unsupported_data_format;
            w.set_priming(e.aac->priming(), 0);
        } else if (!is_aac && !e.encoder) {
            std::string why;
            e.encoder = codecs::make_audio_encoder(format.format_id, format.rate, format.channels, bits, format.frames_per_packet, why);
            if (!e.encoder) return error::unsupported_data_format;
        }
        const auto& target = f.container().format;
        const std::size_t per_packet = is_aac ? 0 : std::size_t(e.encoder->frames_per_packet) * format.channels;
        const std::int64_t step = bits == 32 ? 1 : std::int64_t{1} << (32 - bits);
        std::vector<std::uint8_t> packet;
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
            for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4) {
                std::int32_t v;
                std::memcpy(&v, bytes.data() + i, 4);
                if (step > 1) {   // to the nearest value of the source depth
                    const auto rounded = static_cast<std::int64_t>(std::floor((static_cast<double>(v) + static_cast<double>(step / 2)) / static_cast<double>(step))) * step;
                    v = static_cast<std::int32_t>(std::clamp<std::int64_t>(rounded, -(std::int64_t{1} << 31), (std::int64_t{1} << 31) - step));
                }
                e.unencoded.push_back(v);
            }
            if (is_aac) {
                // 16-bit samples into the AAC encoder, which keeps what does not yet make a packet.
                std::vector<std::int16_t> pcm(e.unencoded.size());
                for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<std::int16_t>(e.unencoded[i] >> 16);
                e.unencoded.clear();
                std::vector<std::vector<std::uint8_t>> made;
                if (!e.aac->encode(pcm, made)) return error::unsupported_data_format;
                for (const auto& p : made)
                    if (const auto result = append_packet(f, p, 0)) return result;
                e.aac_frames += count;
                done += count;
                continue;
            }
            while (e.unencoded.size() >= per_packet) {
                std::string why;
                if (!e.encoder->encode(std::span<const std::int32_t>(e.unencoded.data(), per_packet), packet, why)) return error::unsupported_data_format;
                if (const auto result = append_packet(f, packet, 0)) return result;
                e.unencoded.erase(e.unencoded.begin(), e.unencoded.begin() + static_cast<std::ptrdiff_t>(per_packet));
            }
            done += count;
        }
        e.position += frames;
        return 0;
    }
    // ExtAudioFileDispose of a compressed file being written: the last, shorter packet, and the cookie.
    std::int32_t flush(Ext& e) {
        auto* f = file(e.file);
        if (!f || !f->packets) return 0;
        if (e.aac) {
            // The encoder's last packets; the cookie; the frames past the input are the remainder.
            std::vector<std::vector<std::uint8_t>> made;
            if (!e.aac->drain(made)) return error::unsupported_data_format;
            for (const auto& p : made)
                if (const auto result = append_packet(*f, p, 0)) return result;
            auto& w = *f->packets;
            const auto bits = e.aac->bit_rate();
            w.set_cookie(aac::es_descriptor(e.aac->audio_specific_config(), bits, bits, 768 * w.format().channels));
            const auto used = std::uint64_t{e.aac->priming()} + e.aac_frames;
            w.set_priming(e.aac->priming(), w.frames() > used ? static_cast<std::uint32_t>(w.frames() - used) : 0);
            f->dirty = true;
            e.aac.reset();
            return 0;
        }
        if (!e.encoder) return 0;
        const auto channels = f->packets->format().channels;
        if (!e.unencoded.empty()) {
            std::vector<std::uint8_t> packet;
            std::string why;
            if (!e.encoder->encode(e.unencoded, packet, why)) return error::unsupported_data_format;
            if (const auto result = append_packet(*f, packet, static_cast<std::uint32_t>(e.unencoded.size() / channels))) return result;
            e.unencoded.clear();
        }
        f->packets->set_cookie(e.encoder->cookie());
        f->dirty = true;
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

std::int64_t open_guest_read(Support& support, const std::string& path, std::uint64_t& size, std::int32_t& status) {
    const auto fd = open_guest(support, path, 0, status);
    size = 0;
    if (fd < 0) {
        if (status != permissions_error) status = error::file_not_found;
        return -1;
    }
    const auto end = static_cast<std::int64_t>(support.call(libsystem, "_lseek", {static_cast<Address>(fd), 0, 2}));
    size = end > 0 ? static_cast<std::uint64_t>(end) : 0;
    return fd;
}

void close_guest_fd(Support& support, std::int64_t fd) {
    if (fd >= 0) static_cast<void>(support.call(libsystem, "_close", {static_cast<Address>(fd)}));
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

void set_encode_bit_rate(Address ext, std::uint32_t bits_per_second) {
    std::lock_guard lock(bit_rate_mutex);
    bit_rates[ext] = bits_per_second;
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
    // ---- user data -------------------------------------------------------------------------------
    // AudioFileCountUserData(file, UInt32 userDataID, UInt32 *outNumberItems)
    support.function(lib, "_AudioFileCountUserData", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* f = state->file(s.x[0]);
        if (!f || !s.x[2]) return status(s, error::param);
        std::vector<File::Chunk>* list{};
        if (const auto result = state->user_chunks(*f, list)) return status(s, result);
        const auto id = static_cast<std::uint32_t>(s.x[1]);
        m.write32(s.x[2], static_cast<std::uint32_t>(std::count_if(list->begin(), list->end(), [id](const File::Chunk& c) { return c.id == id; })));
        status(s, 0);
    });
    // AudioFileGetUserDataSize(file, id, index, UInt32 *outSize) / AudioFileGetUserDataSize64(..., UInt64 *outSize)
    for (const bool wide : {false, true})
        support.function(lib, wide ? "_AudioFileGetUserDataSize64" : "_AudioFileGetUserDataSize", [state, wide](CpuState& s, GuestMemory& m) {
            std::lock_guard lock(state->mutex);
            auto* f = state->file(s.x[0]);
            if (!f || !s.x[3]) return status(s, error::param);
            std::vector<File::Chunk>* list{};
            if (const auto result = state->user_chunks(*f, list)) return status(s, result);
            const auto* c = AudioFiles::nth_chunk(*list, static_cast<std::uint32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]));
            if (!c) return status(s, invalid_chunk);
            if (wide) m.write64(s.x[3], c->size);
            else if (c->size > 0xffffffffULL) return status(s, error::bad_property_size);
            else m.write32(s.x[3], static_cast<std::uint32_t>(c->size));
            status(s, 0);
        });
    // AudioFileGetUserData(file, id, index, UInt32 *ioSize, void *out): the whole chunk, which must fit.
    // AudioFileGetUserDataAtOffset(file, id, index, SInt64 offset, UInt32 *ioSize, void *out): what fits from offset.
    for (const bool at_offset : {false, true})
        support.function(lib, at_offset ? "_AudioFileGetUserDataAtOffset" : "_AudioFileGetUserData", [state, at_offset](CpuState& s, GuestMemory& m) {
            const auto size_at = at_offset ? s.x[4] : s.x[3], out = at_offset ? s.x[5] : s.x[4];
            const auto offset = at_offset ? static_cast<std::int64_t>(s.x[3]) : 0;
            std::lock_guard lock(state->mutex);
            auto* f = state->file(s.x[0]);
            if (!f || !size_at || offset < 0) return status(s, error::param);
            std::vector<File::Chunk>* list{};
            if (const auto result = state->user_chunks(*f, list)) return status(s, result);
            const auto* c = AudioFiles::nth_chunk(*list, static_cast<std::uint32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]));
            if (!c) return status(s, invalid_chunk);
            const auto room = m.read32(size_at);
            if (!at_offset && room < c->size) return status(s, error::bad_property_size);
            std::vector<std::uint8_t> bytes(std::min<std::uint64_t>(room, c->size - std::min<std::uint64_t>(c->size, static_cast<std::uint64_t>(offset))));
            bytes.resize(state->chunk_bytes(*f, *c, static_cast<std::uint64_t>(offset), bytes));
            if (!bytes.empty() && !out) return status(s, error::param);
            if (!bytes.empty()) m.write_bytes(out, bytes);
            m.write32(size_at, static_cast<std::uint32_t>(bytes.size()));
            status(s, 0);
        });
    // AudioFileSetUserData(file, id, index, UInt32 size, const void *data): replaces the chunk at index, or
    // adds one past the last; AudioFileRemoveUserData(file, id, index). A file being written only; the
    // chunks of its header cannot change.
    const auto change_user_data = [state](CpuState& s, GuestMemory& m, bool remove) {
        std::lock_guard lock(state->mutex);
        auto* f = state->file(s.x[0]);
        if (!f) return status(s, error::param);
        std::vector<File::Chunk>* list{};
        if (const auto result = state->user_chunks(*f, list)) return status(s, result);
        if (!f->writable) return status(s, permissions_error);
        const auto id = static_cast<std::uint32_t>(s.x[1]);
        const auto index = static_cast<std::uint32_t>(s.x[2]);
        const auto audio = f->container().file_type == four("caff") || f->container().file_type == four("WAVE") ? four("data") : four("SSND");
        if (id == audio || std::any_of(list->begin(), list->end(), [id](const File::Chunk& c) { return c.id == id && c.fixed; }))
            return status(s, error::operation_not_supported);
        auto* c = AudioFiles::nth_chunk(*list, id, index);
        if (remove) {
            if (!c) return status(s, invalid_chunk);
            list->erase(list->begin() + (c - list->data()));
        } else {
            const auto size = static_cast<std::uint32_t>(s.x[3]);
            if (size && !s.x[4]) return status(s, error::param);
            std::vector<std::uint8_t> bytes(size);
            if (size) m.read_bytes(s.x[4], bytes);
            if (!c) {
                list->push_back(File::Chunk{id, 0, 0, {}, false, true});
                c = &list->back();
            }
            c->size = bytes.size();
            c->bytes = std::move(bytes);
        }
        f->dirty = true;
        status(s, 0);
    };
    support.function(lib, "_AudioFileSetUserData", [change_user_data](CpuState& s, GuestMemory& m) { change_user_data(s, m, false); });
    support.function(lib, "_AudioFileRemoveUserData", [change_user_data](CpuState& s, GuestMemory& m) { change_user_data(s, m, true); });

    // ---- global info ---------------------------------------------------------------------------
    // The value of kAudioFileGlobalInfo_* `id` for (specifierSize, specifier): bytes, or a +1 CF object made
    // when `make` is set (its 8 bytes then hold it).
    const auto global_info = [sp](GuestMemory& m, std::uint32_t id, std::uint32_t specifier_size, Address specifier, bool make,
                                  std::vector<std::uint8_t>& out) -> std::int32_t {
        const auto u32s = [&](const std::vector<std::uint32_t>& values) {
            out.resize(values.size() * 4);
            if (!values.empty()) std::memcpy(out.data(), values.data(), out.size());
        };
        const auto object = [&](Address value) {
            out.assign(8, 0);
            std::memcpy(out.data(), &value, 8);
        };
        const auto strings = [&](const std::vector<const char*>& values) {
            if (!make) return object(0);
            std::vector<Address> items;
            for (const auto* v : values) items.push_back(sp->cf.string(v));
            object(sp->cf.array(items));
            for (const auto item : items) sp->release(item);
        };
        const auto type_specifier = [&]() -> const FileType* {
            if (specifier_size < 4 || !specifier) return nullptr;
            return file_type(m.read32(specifier));
        };
        const auto string_specifier = [&]() -> std::optional<std::string> {
            if (specifier_size < 8 || !specifier) return std::nullopt;
            const auto value = m.read64(specifier);
            return value ? std::optional<std::string>(sp->cf.utf8(value)) : std::nullopt;
        };
        std::vector<std::uint32_t> ids;
        switch (id) {
        case four("afrf"): case four("afwf"):   // ReadableTypes, WritableTypes
            for (const auto& t : file_types())
                if (id == four("afrf") || t.writable) ids.push_back(t.type);
            u32s(ids);
            return 0;
        case four("ftnm"): case four("fmid"): case four("fext"): case four("fmim"): case four("futi"): case four("fhfs"): {
            if (specifier_size < 4 || !specifier) return error::bad_property_size;
            const auto* t = type_specifier();
            if (!t) return error::unsupported_file_type;
            switch (id) {
            case four("ftnm"): object(make ? sp->cf.string(t->name) : 0); return 0;   // FileTypeName
            case four("fmid"): u32s(t->formats); return 0;                             // AvailableFormatIDs
            case four("fext"): strings(t->extensions); return 0;                       // ExtensionsForType
            case four("fmim"): strings(t->mime); return 0;                             // MIMETypesForType
            case four("futi"): strings(t->utis); return 0;                             // UTIsForType
            default: u32s({t->type}); return 0;                                        // HFSTypeCodesForType
            }
        }
        case four("alxt"): case four("amim"): case four("auti"): {   // AllExtensions, AllMIMETypes, AllUTIs
            std::vector<const char*> all;
            for (const auto& t : file_types())
                for (const auto* v : id == four("alxt") ? t.extensions : id == four("amim") ? t.mime : t.utis)
                    if (std::none_of(all.begin(), all.end(), [v](const char* have) { return same_text(have, v); })) all.push_back(v);
            strings(all);
            return 0;
        }
        case four("ahfs"):   // AllHFSTypeCodes
            for (const auto& t : file_types()) ids.push_back(t.type);
            u32s(ids);
            return 0;
        case four("text"): case four("tmim"): case four("tuti"): {   // TypesForExtension, TypesForMIMEType, TypesForUTI
            const auto text = string_specifier();
            if (!text) return error::bad_property_size;
            for (const auto& t : file_types())
                for (const auto* v : id == four("text") ? t.extensions : id == four("tmim") ? t.mime : t.utis)
                    if (same_text(v, *text)) { ids.push_back(t.type); break; }
            u32s(ids);
            return 0;
        }
        case four("thfs"): {   // TypesForHFSTypeCode (the codes are the type IDs)
            if (specifier_size < 4 || !specifier) return error::bad_property_size;
            if (const auto* t = file_type(m.read32(specifier))) ids.push_back(t->type);
            u32s(ids);
            return 0;
        }
        case four("sdid"): {   // AvailableStreamDescriptionsForFormat: {AudioFileTypeID, AudioFormatID}
            if (specifier_size < 8 || !specifier) return error::bad_property_size;
            const auto* t = file_type(m.read32(specifier));
            if (!t) return error::unsupported_file_type;
            const auto format = m.read32(specifier + 4);
            if (std::find(t->formats.begin(), t->formats.end(), format) == t->formats.end()) return error::unsupported_data_format;
            out.clear();
            if (format == four("lpcm")) {
                // Each sample layout the container holds (rate and channels 0: any).
                for (const bool is_float : {false, true})
                    for (const std::uint32_t bytes : is_float ? std::vector<std::uint32_t>{4, 8} : std::vector<std::uint32_t>{1, 2, 3, 4})
                        for (const bool big : {false, true})
                            for (const bool is_signed : {true, false}) {
                                if (is_float && !is_signed) continue;
                                Pcm p;
                                p.rate = 44100;
                                p.channels = 1;
                                p.bits = bytes * 8;
                                p.container = bytes;
                                p.is_float = is_float;
                                p.big_endian = big;
                                p.is_signed = is_signed;
                                std::vector<std::uint8_t> header_bytes;
                                std::string why;
                                if (write_header(t->type, p, 0, {}, header_bytes, why)) continue;
                                auto asbd = asbd_from_pcm(p);
                                std::fill(asbd.begin(), asbd.begin() + 8, std::uint8_t{0});   // mSampleRate 0
                                for (const std::size_t at : {16, 24, 28}) std::fill_n(asbd.begin() + at, 4, std::uint8_t{0});   // bytes, channels 0
                                out.insert(out.end(), asbd.begin(), asbd.end());
                            }
            } else {
                const std::uint32_t frames = format == four(".mp1") ? 384 : format == four("opus") ? 960 : format == four("flac") ? 0
                                             : format == four("alac") ? 4096 : format == four("aac ") ? 1024 : 1152;
                std::vector<std::uint8_t> asbd(40, 0);
                std::memcpy(asbd.data() + 8, &format, 4);
                std::memcpy(asbd.data() + 20, &frames, 4);
                out = asbd;
            }
            return 0;
        }
        default:
            return error::unsupported_property;
        }
    };
    // AudioFileGetGlobalInfoSize(AudioFilePropertyID, UInt32 specifierSize, void *specifier, UInt32 *outDataSize)
    support.function(lib, "_AudioFileGetGlobalInfoSize", [global_info](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) return status(s, error::param);
        std::vector<std::uint8_t> value;
        if (const auto result = global_info(m, static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]), s.x[2], false, value))
            return status(s, result);
        m.write32(s.x[3], static_cast<std::uint32_t>(value.size()));
        status(s, 0);
    });
    // AudioFileGetGlobalInfo(AudioFilePropertyID, UInt32 specifierSize, void *specifier, UInt32 *ioDataSize, void *outData):
    // CF values are +1.
    support.function(lib, "_AudioFileGetGlobalInfo", [global_info, sp](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) return status(s, error::param);
        const auto id = static_cast<std::uint32_t>(s.x[0]);
        std::vector<std::uint8_t> value;
        if (const auto result = global_info(m, id, static_cast<std::uint32_t>(s.x[1]), s.x[2], false, value)) return status(s, result);
        if (m.read32(s.x[3]) < value.size()) return status(s, error::bad_property_size);
        if (!value.empty() && !s.x[4]) return status(s, error::param);
        static_cast<void>(global_info(m, id, static_cast<std::uint32_t>(s.x[1]), s.x[2], true, value));
        if (!value.empty()) m.write_bytes(s.x[4], value);
        m.write32(s.x[3], static_cast<std::uint32_t>(value.size()));
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
        if (f->packets) {
            // A compressed file: its magic cookie and packet table info.
            if (id == four("mgic")) {
                f->packets->set_cookie(std::move(data));
            } else if (id == four("pnfo")) {
                if (size < 16) return status(s, error::bad_property_size);
                std::int32_t priming{}, remainder{};
                std::memcpy(&priming, data.data() + 8, 4);
                std::memcpy(&remainder, data.data() + 12, 4);
                if (priming < 0 || remainder < 0) return status(s, error::param);
                f->packets->set_priming(static_cast<std::uint32_t>(priming), static_cast<std::uint32_t>(remainder));
            } else {
                return status(s, id == four("dfmt") ? error::operation_not_supported : error::unsupported_property);
            }
            f->dirty = true;
            return status(s, 0);
        }
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
        if (f->packets) return status(s, error::operation_not_supported);   // a compressed file being written
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
        if (f->packets) {
            // Compressed packets, each with its description, appended at the next packet.
            if (static_cast<std::uint64_t>(packet) != f->packets->packet_count()) return status(s, error::invalid_packet_offset);
            const auto count = m.read32(s.x[5]);
            if (count && !s.x[3]) return status(s, error::param);
            const auto total = static_cast<std::uint32_t>(s.x[2]);
            std::uint32_t written = 0;
            std::int32_t result = 0;
            for (; written < count; ++written) {
                const auto d = s.x[3] + 16ULL * written;
                const auto offset = m.read64(d);
                const auto frames = m.read32(d + 8), bytes = m.read32(d + 12);
                if (offset + bytes > total) { result = error::param; break; }
                std::vector<std::uint8_t> data(bytes);
                m.read_bytes(s.x[6] + offset, data);
                if ((result = state->append_packet(*f, data, frames))) break;
            }
            m.write32(s.x[5], written);
            return status(s, result);
        }
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
        const auto flushed = state->flush(*e);
        {
            std::lock_guard lock(bit_rate_mutex);
            bit_rates.erase(s.x[0]);
        }
        if (e->owns_file) state->close(e->file);
        else if (auto* f = state->file(e->file)) static_cast<void>(state->finish(*f));
        state->exts.erase(s.x[0]);
        state->support.process.release_guest(s.x[0]);
        status(s, flushed);
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
            status(s, state->write(*e, m, static_cast<std::uint32_t>(s.x[1]), s.x[2], s.x[0]));
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

    install_audio_file_stream(support);
    install_audio_services(support);
}

} // namespace alchemy::gui::completion_kit::media
