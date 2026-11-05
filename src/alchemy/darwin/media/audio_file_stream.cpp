#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/audio_file_stream_core.hpp"

#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>

// AudioToolbox AudioFileStream over the streaming parser (audio_file_stream_core.hpp): the client
// hands in the stream's bytes as they arrive; its property listener hears of each property as the
// header gives it (every value is kept, so kAudioFileStreamPropertyFlag_PropertyIsCached is set)
// and then kAudioFileStreamProperty_ReadyToProducePackets, and its packets procedure receives the
// whole packets of each AudioFileStreamParseBytes call, with descriptions for MPEG audio and none
// for linear PCM. The buffers it is handed live until it returns. No property can be set.
namespace alchemy::gui::completion_kit::media {
namespace {

using namespace audio_file_stream;
constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::int32_t param_error = -50;

struct Stream {
    Address client{}, listener{}, packets{};
    Parser parser;
    int delivering{};                  // ParseBytes calls handing out events
    bool closed{};                     // closed from a callback: freed when the last delivery ends
    Stream(Address c, Address l, Address p, std::uint32_t hint) : client(c), listener(l), packets(p), parser(hint) {}
};

void status(CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

bool translation(std::uint32_t id) {
    return id == 0x706b6672 || id == 0x6672706b || id == 0x706b6279 || id == 0x6279706b;   // 'pkfr' 'frpk' 'pkby' 'bypk'
}

struct Streams {
    Support& support;
    std::recursive_mutex mutex;
    std::map<Address, std::shared_ptr<Stream>> streams;

    explicit Streams(Support& owner) : support(owner) {}

    std::uint64_t call(Address function, std::initializer_list<std::uint64_t> arguments) {
        return EngineAdapter(support.process.calling_engine()).call(function, std::span(arguments.begin(), arguments.size()));
    }
    std::shared_ptr<Stream> find(Address id) {
        const auto it = streams.find(id);
        return it == streams.end() || it->second->closed ? nullptr : it->second;
    }
    void forget(Address id) {
        streams.erase(id);
        support.process.release_guest(id);
    }

    // Hands `events` to the client's procedures, without the lock (they call back in).
    void deliver(Address id, Stream& st, const std::vector<Event>& events) {
        auto& m = support.memory();
        for (const auto& e : events) {
            if (st.closed) return;
            if (e.property) {
                // AudioFileStream_PropertyListenerProc(void *client, AudioFileStreamID, AudioFileStreamPropertyID, AudioFileStreamPropertyFlags *ioFlags)
                const auto flags = support.process.allocate_guest(8, 8);
                m.write32(flags, 1);   // kAudioFileStreamPropertyFlag_PropertyIsCached
                call(st.listener, {st.client, id, e.property, flags});
                support.process.release_guest(flags);
                continue;
            }
            // AudioFileStream_PacketsProc(void *client, UInt32 numberBytes, UInt32 numberPackets, const void *data,
            //                             AudioStreamPacketDescription *descriptions)
            const auto data = support.process.allocate_guest(std::max<std::size_t>(e.bytes.size(), 16), 16);
            m.write_bytes(data, e.bytes);
            Address descriptions = 0;
            if (!e.descriptions.empty()) {
                descriptions = support.process.allocate_guest(16 * e.descriptions.size(), 8);
                for (std::size_t i = 0; i < e.descriptions.size(); ++i) {
                    const auto at = descriptions + 16 * i;
                    m.write64(at, static_cast<std::uint64_t>(e.descriptions[i].start));
                    m.write32(at + 8, e.descriptions[i].variable_frames);
                    m.write32(at + 12, e.descriptions[i].bytes);
                }
            }
            call(st.packets, {st.client, e.bytes.size(), e.packets, data, descriptions});
            if (descriptions) support.process.release_guest(descriptions);
            support.process.release_guest(data);
        }
    }
};

} // namespace

void install_audio_file_stream(Support& support) {
    auto state = std::make_shared<Streams>(support);
    const auto lib = audio_toolbox;

    // AudioFileStreamOpen(void *client, PropertyListenerProc, PacketsProc, AudioFileTypeID hint, AudioFileStreamID *out)
    support.function(lib, "_AudioFileStreamOpen", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || !s.x[2] || !s.x[4]) return status(s, param_error);
        std::lock_guard lock(state->mutex);
        const auto id = state->support.process.allocate_guest(16, 16);
        state->streams[id] = std::make_shared<Stream>(s.x[0], s.x[1], s.x[2], static_cast<std::uint32_t>(s.x[3]));
        m.write64(s.x[4], id);
        status(s, 0);
    });
    // AudioFileStreamParseBytes(AudioFileStreamID, UInt32 size, const void *data, AudioFileStreamParseFlags (1: discontinuity))
    support.function(lib, "_AudioFileStreamParseBytes", [state](CpuState& s, GuestMemory& m) {
        const auto id = s.x[0];
        const auto size = static_cast<std::uint32_t>(s.x[1]);
        std::shared_ptr<Stream> st;
        std::vector<Event> events;
        std::int32_t result = 0;
        {
            std::lock_guard lock(state->mutex);
            st = state->find(id);
            if (!st || (size && !s.x[2])) return status(s, param_error);
            std::vector<std::uint8_t> bytes(size);
            if (size) m.read_bytes(s.x[2], bytes);
            result = st->parser.parse(bytes, (s.x[3] & 1) != 0, events);
            ++st->delivering;
        }
        state->deliver(id, *st, events);
        {
            std::lock_guard lock(state->mutex);
            if (--st->delivering == 0 && st->closed) state->forget(id);
        }
        status(s, result);
    });
    // AudioFileStreamSeek(AudioFileStreamID, SInt64 packetOffset, SInt64 *outDataByteOffset, AudioFileStreamSeekFlags *ioFlags)
    support.function(lib, "_AudioFileStreamSeek", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto st = state->find(s.x[0]);
        if (!st || !s.x[2]) return status(s, param_error);
        std::int64_t offset = 0;
        bool estimated = false;
        if (const auto result = st->parser.seek(static_cast<std::int64_t>(s.x[1]), offset, estimated)) return status(s, result);
        m.write64(s.x[2], static_cast<std::uint64_t>(offset));
        if (s.x[3]) m.write32(s.x[3], (m.read32(s.x[3]) & ~1U) | (estimated ? 1U : 0U));   // kAudioFileStreamSeekFlag_OffsetIsEstimated
        status(s, 0);
    });
    // AudioFileStreamGetPropertyInfo(AudioFileStreamID, AudioFileStreamPropertyID, UInt32 *outSize, Boolean *outWritable)
    support.function(lib, "_AudioFileStreamGetPropertyInfo", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto st = state->find(s.x[0]);
        if (!st) return status(s, param_error);
        const std::vector<std::uint8_t> question(24);
        std::vector<std::uint8_t> value;
        if (const auto result = st->parser.property(static_cast<std::uint32_t>(s.x[1]), question, value)) return status(s, result);
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(value.size()));
        if (s.x[3]) m.write8(s.x[3], 0);
        status(s, 0);
    });
    // AudioFileStreamGetProperty(AudioFileStreamID, AudioFileStreamPropertyID, UInt32 *ioSize, void *outData)
    support.function(lib, "_AudioFileStreamGetProperty", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, param_error);
        std::lock_guard lock(state->mutex);
        const auto st = state->find(s.x[0]);
        if (!st) return status(s, param_error);
        const auto id = static_cast<std::uint32_t>(s.x[1]);
        const auto room = m.read32(s.x[2]);
        std::vector<std::uint8_t> question, value;
        if (translation(id) && s.x[3]) {
            question.resize(std::min<std::uint32_t>(room, 24));
            m.read_bytes(s.x[3], question);
        }
        if (const auto result = st->parser.property(id, question, value)) return status(s, result);
        if (room < value.size()) return status(s, error::bad_property_size);
        if (!s.x[3]) return status(s, param_error);
        m.write_bytes(s.x[3], value);
        m.write32(s.x[2], static_cast<std::uint32_t>(value.size()));
        status(s, 0);
    });
    // AudioFileStreamSetProperty(AudioFileStreamID, AudioFileStreamPropertyID, UInt32 size, const void *data): the
    // parser's properties come from the stream.
    support.function(lib, "_AudioFileStreamSetProperty", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        const auto st = state->find(s.x[0]);
        if (!st) return status(s, param_error);
        status(s, st->parser.known_property(static_cast<std::uint32_t>(s.x[1])) ? error::illegal_operation : error::unsupported_property);
    });
    // AudioFileStreamClose(AudioFileStreamID)
    support.function(lib, "_AudioFileStreamClose", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        const auto st = state->find(s.x[0]);
        if (!st) return status(s, param_error);
        if (st->delivering) st->closed = true;
        else state->forget(s.x[0]);
        status(s, 0);
    });
}

} // namespace alchemy::gui::completion_kit::media
