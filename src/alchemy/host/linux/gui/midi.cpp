// Linux host backend, area "midi" (include/alchemy/host/midi.hpp), over ALSA's sequencer (libasound). The
// inputs are the ports other clients let us read and subscribe to, the outputs those we may write and subscribe
// to, both in client and port order (not this process's own, nor the system client's timer and announce ports),
// listed afresh on every call. Without a sequencer (no /dev/snd/seq: WSL, a container) there are no ports.
// - An input is subscribed to this process's own input port; one thread polls the sequencer and turns each
//   event back into its bytes (a SysEx is collected until its F7), handing them with the host time to the
//   port's Receiver.
// - Outputs open on the first send: this process's own output port, connected straight to the port's client,
//   with the message encoded into sequencer events and delivered at once (blocking until the sequencer took
//   them).
#include "alchemy/host/midi.hpp"
#include "alchemy/host/time.hpp"

#include <alsa/asoundlib.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

namespace alchemy::host::midi {

namespace {

struct Address {
    int client{}, port{};
    bool operator<(const Address& o) const { return std::tie(client, port) < std::tie(o.client, o.port); }
    bool operator==(const Address& o) const { return client == o.client && port == o.port; }
};

struct Listed {
    Address address;
    Port port;
};

// The ports of the other clients with `caps` (both bits), in order.
std::vector<Listed> list(snd_seq_t* seq, unsigned caps) {
    std::vector<Listed> out;
    if (!seq) return out;
    const int self = snd_seq_client_id(seq);
    snd_seq_client_info_t* client;
    snd_seq_port_info_t* port;
    snd_seq_client_info_alloca(&client);
    snd_seq_port_info_alloca(&port);
    snd_seq_client_info_set_client(client, -1);
    while (snd_seq_query_next_client(seq, client) >= 0) {
        const int id = snd_seq_client_info_get_client(client);
        if (id == self || id == SND_SEQ_CLIENT_SYSTEM) continue;
        snd_seq_port_info_set_client(port, id);
        snd_seq_port_info_set_port(port, -1);
        while (snd_seq_query_next_port(seq, port) >= 0) {
            const unsigned have = snd_seq_port_info_get_capability(port);
            if ((have & caps) != caps || (have & SND_SEQ_PORT_CAP_NO_EXPORT)) continue;
            const unsigned type = snd_seq_port_info_get_type(port);
            Listed l;
            l.address = {id, snd_seq_port_info_get_port(port)};
            l.port.name = snd_seq_port_info_get_name(port);
            l.port.manufacturer = snd_seq_client_info_get_name(client);
            l.port.driver = "org.alsa.seq";
            l.port.synthesizer = (type & (SND_SEQ_PORT_TYPE_SYNTH | SND_SEQ_PORT_TYPE_SYNTHESIZER)) != 0;
            out.push_back(std::move(l));
        }
    }
    return out;
}

constexpr unsigned readable = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
constexpr unsigned writable = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;

} // namespace

struct Devices::State {
    std::mutex mutex;
    snd_seq_t* input{};    // nonblocking: the reading thread polls it
    snd_seq_t* output{};   // blocking: a send returns once the sequencer took the events
    int input_port{-1}, output_port{-1};
    int wake{-1};          // eventfd that ends the reading thread
    std::thread reader;
    struct Source {
        Receiver receive;
        snd_midi_event_t* decoder{};
        std::vector<std::uint8_t> sysex;   // a SysEx being collected
    };
    std::map<Address, Source> sources;

    ~State() {
        if (reader.joinable()) {
            const std::uint64_t one = 1;
            static_cast<void>(::write(wake, &one, sizeof one));
            reader.join();
        }
        for (auto& [address, source] : sources)
            if (source.decoder) snd_midi_event_free(source.decoder);
        if (wake >= 0) ::close(wake);
        if (input) snd_seq_close(input);
        if (output) snd_seq_close(output);
    }

    void read_events() {
        const int count = snd_seq_poll_descriptors_count(input, POLLIN);
        std::vector<pollfd> fds(static_cast<std::size_t>(count) + 1);
        fds[0] = {wake, POLLIN, 0};
        snd_seq_poll_descriptors(input, fds.data() + 1, static_cast<unsigned>(count), POLLIN);
        for (;;) {
            if (::poll(fds.data(), fds.size(), -1) < 0) continue;
            if (fds[0].revents & POLLIN) return;
            snd_seq_event_t* event = nullptr;
            while (snd_seq_event_input(input, &event) >= 0 && event) {
                const auto now = time::monotonic_ns();
                std::lock_guard lock(mutex);
                const auto found = sources.find({event->source.client, event->source.port});
                if (found == sources.end()) continue;
                auto& source = found->second;
                if (event->type == SND_SEQ_EVENT_SYSEX) {
                    const auto* bytes = static_cast<const std::uint8_t*>(event->data.ext.ptr);
                    source.sysex.insert(source.sysex.end(), bytes, bytes + event->data.ext.len);
                    if (!source.sysex.empty() && source.sysex.back() == 0xF7) {
                        if (source.receive) source.receive(std::exchange(source.sysex, {}), now);
                        source.sysex.clear();
                    }
                    continue;
                }
                std::uint8_t buffer[16];
                const long length = snd_midi_event_decode(source.decoder, buffer, sizeof buffer, event);
                if (length > 0 && source.receive) source.receive(std::vector<std::uint8_t>(buffer, buffer + length), now);
            }
        }
    }
};

Devices::Devices() : state_(std::make_unique<State>()) {
    auto& s = *state_;
    // No sequencer: no ports (the handles stay null). Without its device node ALSA is not asked (it would print why).
    if (::access("/dev/snd/seq", F_OK) != 0 || snd_seq_open(&s.input, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
        s.input = nullptr;
        return;
    }
    if (snd_seq_open(&s.output, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) s.output = nullptr;
    snd_seq_set_client_name(s.input, "Alchemy");
    if (s.output) snd_seq_set_client_name(s.output, "Alchemy");
}

Devices::~Devices() = default;

Result<std::vector<Port>> Devices::inputs() {
    std::lock_guard lock(state_->mutex);
    std::vector<Port> out;
    for (auto& l : list(state_->input, readable)) out.push_back(std::move(l.port));
    return out;
}

Result<std::vector<Port>> Devices::outputs() {
    std::lock_guard lock(state_->mutex);
    std::vector<Port> out;
    for (auto& l : list(state_->input ? state_->input : state_->output, writable)) out.push_back(std::move(l.port));
    return out;
}

Status Devices::open_input(std::size_t index, Receiver receive) {
    auto& s = *state_;
    std::lock_guard lock(s.mutex);
    const auto ports = list(s.input, readable);
    if (index >= ports.size()) return Error::not_found;
    const auto address = ports[index].address;
    if (s.sources.contains(address)) return Error::none;   // keeps its first Receiver
    if (s.input_port < 0) {
        s.input_port = snd_seq_create_simple_port(s.input, "Alchemy input", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                                  SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (s.input_port < 0) return Error::io;
    }
    State::Source source;
    if (snd_midi_event_new(16, &source.decoder) < 0) return Error::no_memory;
    snd_midi_event_no_status(source.decoder, 1);   // every message whole, with its status byte
    if (snd_seq_connect_from(s.input, s.input_port, address.client, address.port) < 0) {
        snd_midi_event_free(source.decoder);
        return Error::io;
    }
    source.receive = std::move(receive);
    s.sources.emplace(address, std::move(source));
    if (!s.reader.joinable()) {
        s.wake = ::eventfd(0, EFD_CLOEXEC);
        if (s.wake < 0) return Error::io;
        s.reader = std::thread([&s] { s.read_events(); });
    }
    return Error::none;
}

void Devices::close_input(std::size_t index) noexcept {
    auto& s = *state_;
    std::lock_guard lock(s.mutex);
    const auto ports = list(s.input, readable);
    if (index >= ports.size()) return;
    const auto found = s.sources.find(ports[index].address);
    if (found == s.sources.end()) return;
    snd_seq_disconnect_from(s.input, s.input_port, found->first.client, found->first.port);
    if (found->second.decoder) snd_midi_event_free(found->second.decoder);
    s.sources.erase(found);   // under the lock: its Receiver is not running and is not called again
}

Status Devices::send(std::size_t index, std::span<const std::uint8_t> message) {
    auto& s = *state_;
    std::lock_guard lock(s.mutex);
    if (!s.output) return index < list(s.input, writable).size() ? Error::io : Error::not_found;
    const auto ports = list(s.output, writable);
    if (index >= ports.size()) return Error::not_found;
    if (message.empty()) return Error::invalid_argument;
    if (s.output_port < 0) {
        s.output_port = snd_seq_create_simple_port(s.output, "Alchemy output", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                                   SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (s.output_port < 0) return Error::io;
    }
    snd_midi_event_t* encoder = nullptr;
    if (snd_midi_event_new(std::max<std::size_t>(message.size(), 16), &encoder) < 0) return Error::no_memory;
    const auto address = ports[index].address;
    Status result = Error::none;
    std::size_t at = 0;
    while (at < message.size()) {
        snd_seq_event_t event;
        snd_seq_ev_clear(&event);
        const long used = snd_midi_event_encode(encoder, message.data() + at, static_cast<long>(message.size() - at), &event);
        if (used <= 0) { result = Error::invalid_argument; break; }
        at += static_cast<std::size_t>(used);
        if (event.type == SND_SEQ_EVENT_NONE) continue;   // an incomplete message so far
        snd_seq_ev_set_source(&event, s.output_port);
        snd_seq_ev_set_dest(&event, address.client, address.port);
        snd_seq_ev_set_direct(&event);
        if (snd_seq_event_output_direct(s.output, &event) < 0) { result = Error::io; break; }
    }
    snd_midi_event_free(encoder);
    return result;
}

} // namespace alchemy::host::midi
