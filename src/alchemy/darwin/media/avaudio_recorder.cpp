#include "alchemy/darwin/media/avaudio.hpp"
#include "alchemy/darwin/media/hal_io.hpp"
#include "alchemy/darwin/media/resampler.hpp"

#include "alchemy/darwin/security/bridge.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// AVAudioRecorder and AVAudioApplication's record permission:
// - AVAudioRecorder records the default input device (hal_io.hpp InputIo: the host's microphone, silence in
//   cloaked runs) into an AVAudioFile opened for writing with its settings (or format) at -prepareToRecord
//   or the first -record. The device's channels are mapped (a mono device fills every channel) and resampled to
//   the file's rate on the device's input thread, and written there. -record answers NO without an input device
//   or when the host refuses the microphone; -recordForDuration: stops by itself after that much audio;
//   -recordAtTime: drops what arrives before the given device time. -pause keeps the file open; -stop closes it
//   and the delegate hears -audioRecorderDidFinishRecording:successfully: on the main queue (also after a
//   duration ends). Metering: -updateMeters takes the peak and RMS power (dBFS, -160 for silence) of what was
//   recorded since the previous update.
// - AVAudioApplication: +sharedInstance; -recordPermission is the microphone's AVCaptureDevice authorization
//   ('grnt' authorized, 'deny' denied or restricted, 'undt' not determined: what the host's privacy settings say,
//   no prompt); +requestRecordPermissionWithCompletionHandler: lets the host decide as AVCaptureDevice's request
//   does. -setInputMuted:error: mutes what recorders capture (silence is recorded).
namespace alchemy::gui::completion_kit::media::avaudio {
namespace {

struct Recorder final : ObjectPayload {
    std::mutex mutex;
    Address url{}, settings{}, format{};   // retained
    Address file{};                        // retained AVAudioFile while prepared
    Address processing{};                  // the file's processing format (owned by the file)
    std::uint32_t channels{};
    double rate{};
    Address delegate{};
    bool recording{}, metering{}, deallocating{};
    std::shared_ptr<hal_io::InputIo> input;
    std::unique_ptr<Resampler> resampler;
    std::deque<double> pending;             // device frames in the file's channels, waiting for the resampler
    std::uint64_t recorded{};               // frames written (file rate)
    std::optional<std::uint64_t> limit;     // -recordForDuration:
    std::uint64_t start_host{};             // -recordAtTime:: drop frames taken before this
    std::vector<double> peak, squares;      // since the last -updateMeters
    std::uint64_t meter_frames{};
    std::vector<float> peak_db, average_db;
};

std::atomic<bool>& input_muted() {
    static std::atomic<bool> value{false};
    return value;
}

class Recorders : public std::enable_shared_from_this<Recorders> {
public:
    explicit Recorders(Support& support) : support_(support), bridge_(security::bridge()) {}
    void install();

private:
    std::shared_ptr<Recorder> state(Address self) { return support_.objc.host_state<Recorder>(self); }
    bool prepare(Address self, Recorder& r);
    bool record(Address self, const std::shared_ptr<Recorder>& r);
    void stop_input(Address self, Recorder& r);
    void finish(Address self, const std::shared_ptr<Recorder>& r, bool notify);
    void captured(Address self, const std::shared_ptr<Recorder>& r, const float* samples, std::uint32_t frames, std::uint32_t device_channels,
                  double device_rate, const hal_io::Timing& timing);

    Support& support_;
    std::shared_ptr<security::Bridge> bridge_;
};

bool Recorders::prepare(Address self, Recorder& r) {
    static_cast<void>(self);
    if (r.file) return true;
    if (!r.url || !r.settings) return false;
    const auto error = support_.process.allocate_guest(8, 8);
    support_.memory().write64(error, 0);
    const auto file = support_.send_id(support_.send_id(support_.objc.find_class("AVAudioFile"), "alloc"), "initForWriting:settings:error:",
                                       {r.url, r.settings, error});
    support_.process.release_guest(error);
    if (!file) return false;
    const auto processing = support_.send_id(file, "processingFormat");
    const auto pcm = format_pcm(support_, processing);
    if (!pcm) {
        support_.release(file);
        return false;
    }
    r.file = file;
    r.processing = processing;
    r.channels = pcm->channels;
    r.rate = pcm->rate;
    if (!r.format) r.format = support_.retain(support_.send_id(file, "fileFormat"));
    return true;
}

// On the device's input thread: the frames in the file's channels and rate, written to the file.
void Recorders::captured(Address self, const std::shared_ptr<Recorder>& r, const float* samples, std::uint32_t frames, std::uint32_t device_channels,
                         double device_rate, const hal_io::Timing& timing) {
    std::vector<double> out;
    Address file{}, processing{};
    bool reached = false;
    {
        std::lock_guard lock(r->mutex);
        if (!r->recording || !r->file) return;
        std::uint32_t skip = 0;
        if (r->start_host && timing.output_host < r->start_host) {
            const auto early = static_cast<double>(r->start_host - timing.output_host) * device_rate / 1e9;
            if (early >= frames) return;
            skip = static_cast<std::uint32_t>(early);
        }
        const bool muted = input_muted();
        const auto c = r->channels;
        for (std::uint32_t f = skip; f < frames; ++f)
            for (std::uint32_t ch = 0; ch < c; ++ch) {
                const auto source = device_channels == 1 ? 0U : ch;
                r->pending.push_back(muted || source >= device_channels ? 0.0 : samples[static_cast<std::size_t>(f) * device_channels + source]);
            }
        const auto available = r->pending.size() / c;
        if (r->rate == device_rate) {
            out.assign(r->pending.begin(), r->pending.end());
            r->pending.clear();
        } else {
            if (!r->resampler) r->resampler = std::make_unique<Resampler>(device_rate, r->rate, c);
            auto count = static_cast<std::size_t>(std::floor(static_cast<double>(available) * r->rate / device_rate));
            while (count && r->resampler->input_needed(count) > available) --count;
            out.resize(count * c);
            r->resampler->render(out.data(), count, [&](double* to, std::size_t n) {
                const auto take = std::min(n * c, r->pending.size());
                std::copy_n(r->pending.begin(), take, to);
                std::fill(to + take, to + n * c, 0.0);
                r->pending.erase(r->pending.begin(), r->pending.begin() + static_cast<std::ptrdiff_t>(take));
            });
        }
        auto count = out.size() / c;
        if (r->limit && r->recorded + count >= *r->limit) {
            count = static_cast<std::size_t>(*r->limit - r->recorded);
            out.resize(count * c);
            reached = true;
        }
        r->recorded += count;
        if (r->metering)
            for (std::size_t f = 0; f < count; ++f)
                for (std::uint32_t ch = 0; ch < c; ++ch) {
                    const auto v = std::fabs(out[f * c + ch]);
                    r->peak[ch] = std::max(r->peak[ch], v);
                    r->squares[ch] += v * v;
                }
        r->meter_frames += count;
        file = support_.retain(r->file);
        processing = r->processing;
    }
    const auto frames_out = static_cast<std::uint32_t>(out.size() / std::max(1U, r->channels));
    if (frames_out) {
        const auto buffer = make_buffer(support_, processing, frames_out);
        if (const auto b = buffer ? support_.objc.find_host_state<PcmBuffer>(buffer) : nullptr) {
            fill_buffer(support_, *b, out, r->channels, frames_out);
            static_cast<void>(support_.send_id(file, "writeFromBuffer:error:", {buffer, 0}));
        }
        support_.release(buffer);
    }
    support_.release(file);
    if (reached) {
        // The duration is recorded: stop on the main queue (not from the input thread) and tell the delegate.
        const auto owner = shared_from_this();
        bridge_->dispatch_async(bridge_->main_queue(), [owner, self, r] { owner->finish(self, r, true); });
    }
}

bool Recorders::record(Address self, const std::shared_ptr<Recorder>& r) {
    std::shared_ptr<hal_io::InputIo> input;
    {
        std::lock_guard lock(r->mutex);
        if (r->recording) return true;
        if (!prepare(self, *r)) return false;
    }
    if (capture_authorization(false, false) == 2) return false;   // the host's privacy settings deny the microphone
    const auto device = hal_default_input();
    input = device ? hal_input_io(device) : nullptr;
    if (!input) return false;
    {
        std::lock_guard lock(r->mutex);
        r->input = input;
        r->recording = true;
        r->peak.assign(r->channels, 0.0);
        r->squares.assign(r->channels, 0.0);
        r->meter_frames = 0;
    }
    const auto owner = shared_from_this();
    const auto channels = input->channels();
    const auto rate = input->rate();
    const auto status = input->start(self, [owner, self, r, channels, rate](const float* samples, std::uint32_t frames, const hal_io::Timing& timing) {
        owner->captured(self, r, samples, frames, channels, rate, timing);
    }, 0);
    if (status) {
        std::lock_guard lock(r->mutex);
        r->recording = false;
        r->input.reset();
        return false;
    }
    return true;
}

void Recorders::stop_input(Address self, Recorder& r) {
    std::shared_ptr<hal_io::InputIo> input;
    {
        std::lock_guard lock(r.mutex);
        r.recording = false;
        input = std::move(r.input);
    }
    if (input) input->stop(self);
}

// Stops recording and closes the file; the delegate hears (on the calling thread: the main queue).
void Recorders::finish(Address self, const std::shared_ptr<Recorder>& r, bool notify) {
    stop_input(self, *r);
    Address file{}, delegate{};
    {
        std::lock_guard lock(r->mutex);
        file = std::exchange(r->file, 0);
        r->processing = 0;
        r->resampler.reset();
        r->pending.clear();
        r->recorded = 0;
        r->limit.reset();
        r->start_host = 0;
        delegate = r->deallocating ? 0 : r->delegate;
    }
    if (!file) return;
    static_cast<void>(support_.send(file, "close"));
    support_.release(file);
    if (notify && delegate) {
        CallRegs regs{};
        regs.x[2] = self;
        regs.x[3] = 1;
        static_cast<void>(support_.objc.send_if_responds(delegate, support_.objc.selector("audioRecorderDidFinishRecording:successfully:"), regs));
    }
}

void Recorders::install() {
    auto& support = support_;
    auto* sp = &support_;
    const auto self = shared_from_this();
    const auto recorder = declare(support, "AVAudioRecorder", "NSObject");
    const auto return_real32 = [](CpuState& s, float value) { s.v[0] = {std::bit_cast<std::uint32_t>(value), 0}; };
    const auto store_error = [self](Address out, std::int32_t status, std::string_view text) {
        if (out) self->bridge_->store_error(out, self->bridge_->os_status_error(status, text), true);
    };

    // -initWithURL:settings:error: / -initWithURL:format:error: (the file is created at -prepareToRecord or -record)
    support.method(recorder, "initWithURL:settings:error:", [self, sp, store_error](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3]) {
            store_error(s.x[4], -50, "AVAudioRecorder needs a URL and settings");
            sp->release(s.x[0]);
            s.x[0] = 0;
            return;
        }
        const auto r = self->state(s.x[0]);
        r->url = sp->retain(s.x[2]);
        r->settings = sp->cf.copy(s.x[3]);
    }, "@@:@@^@");
    support.method(recorder, "initWithURL:format:error:", [self, sp, store_error](CpuState& s, GuestMemory&) {
        const auto settings = s.x[3] ? sp->send_id(s.x[3], "settings") : 0;
        if (!s.x[2] || !settings) {
            store_error(s.x[4], -50, "AVAudioRecorder needs a URL and a format");
            sp->release(s.x[0]);
            s.x[0] = 0;
            return;
        }
        const auto r = self->state(s.x[0]);
        r->url = sp->retain(s.x[2]);
        r->settings = sp->cf.copy(settings);
        r->format = sp->retain(s.x[3]);
    }, "@@:@@^@");
    support.method(recorder, "prepareToRecord", [self](CpuState& s, GuestMemory&) {
        const auto r = self->state(s.x[0]);
        std::lock_guard lock(r->mutex);
        s.x[0] = self->prepare(s.x[0], *r) ? 1 : 0;
    }, "B@:");
    support.method(recorder, "record", [self](CpuState& s, GuestMemory&) { s.x[0] = self->record(s.x[0], self->state(s.x[0])) ? 1 : 0; }, "B@:");
    // -recordAtTime:(NSTimeInterval) in deviceCurrentTime seconds; -recordForDuration:(NSTimeInterval).
    const auto record_with = [self](Address object, std::optional<double> at, std::optional<double> duration) {
        const auto r = self->state(object);
        {
            std::lock_guard lock(r->mutex);
            if (!self->prepare(object, *r)) return false;
            if (at) r->start_host = static_cast<std::uint64_t>(std::max(0.0, *at) * 1e9);
            if (duration) r->limit = r->recorded + static_cast<std::uint64_t>(std::max(0.0, *duration) * r->rate);
        }
        return self->record(object, r);
    };
    support.method(recorder, "recordAtTime:", [record_with](CpuState& s, GuestMemory&) {
        s.x[0] = record_with(s.x[0], std::bit_cast<double>(s.v[0][0]), std::nullopt) ? 1 : 0;
    }, "B@:d");
    support.method(recorder, "recordForDuration:", [record_with](CpuState& s, GuestMemory&) {
        s.x[0] = record_with(s.x[0], std::nullopt, std::bit_cast<double>(s.v[0][0])) ? 1 : 0;
    }, "B@:d");
    support.method(recorder, "recordAtTime:forDuration:", [record_with](CpuState& s, GuestMemory&) {
        s.x[0] = record_with(s.x[0], std::bit_cast<double>(s.v[0][0]), std::bit_cast<double>(s.v[1][0])) ? 1 : 0;
    }, "B@:dd");
    support.method(recorder, "pause", [self](CpuState& s, GuestMemory&) { self->stop_input(s.x[0], *self->state(s.x[0])); }, "v@:");
    support.method(recorder, "stop", [self](CpuState& s, GuestMemory&) { self->finish(s.x[0], self->state(s.x[0]), true); }, "v@:");
    support.method(recorder, "deleteRecording", [self, sp](CpuState& s, GuestMemory&) {
        const auto r = self->state(s.x[0]);
        Address url{};
        {
            std::lock_guard lock(r->mutex);
            if (r->recording) { s.x[0] = 0; return; }
            url = r->url;
        }
        const auto manager = sp->send_id(sp->objc.find_class("NSFileManager"), "defaultManager");
        s.x[0] = url && manager && (sp->send_id(manager, "removeItemAtURL:error:", {url, 0}) & 0xff) ? 1 : 0;
    }, "B@:");
    support.method(recorder, "isRecording", [self](CpuState& s, GuestMemory&) {
        const auto r = self->state(s.x[0]);
        std::lock_guard lock(r->mutex);
        s.x[0] = r->recording ? 1 : 0;
    }, "B@:");
    support.method(recorder, "url", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state(s.x[0])->url; }, "@@:");
    support.method(recorder, "settings", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state(s.x[0])->settings; }, "@@:");
    support.method(recorder, "format", [self](CpuState& s, GuestMemory&) {
        const auto r = self->state(s.x[0]);
        std::lock_guard lock(r->mutex);
        s.x[0] = r->format;
    }, "@@:");
    // Seconds recorded so far (0 once stopped).
    support.method(recorder, "currentTime", [self](CpuState& s, GuestMemory&) {
        const auto r = self->state(s.x[0]);
        std::lock_guard lock(r->mutex);
        s.v[0] = {std::bit_cast<std::uint64_t>(r->rate > 0 ? static_cast<double>(r->recorded) / r->rate : 0.0), 0};
    }, "d@:");
    support.method(recorder, "deviceCurrentTime", [](CpuState& s, GuestMemory&) {
        s.v[0] = {std::bit_cast<std::uint64_t>(static_cast<double>(host_time_now()) / 1e9), 0};
    }, "d@:");
    support.method(recorder, "delegate", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state(s.x[0])->delegate; }, "@@:");
    support.method(recorder, "setDelegate:", [self](CpuState& s, GuestMemory&) { self->state(s.x[0])->delegate = s.x[2]; }, "v@:@");
    support.method(recorder, "isMeteringEnabled", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state(s.x[0])->metering ? 1 : 0; }, "B@:");
    support.method(recorder, "setMeteringEnabled:", [self](CpuState& s, GuestMemory&) {
        const auto r = self->state(s.x[0]);
        std::lock_guard lock(r->mutex);
        r->metering = (s.x[2] & 0xff) != 0;
        r->peak.assign(std::max(r->channels, 1U), 0.0);
        r->squares.assign(std::max(r->channels, 1U), 0.0);
        r->meter_frames = 0;
    }, "v@:B");
    support.method(recorder, "updateMeters", [self](CpuState& s, GuestMemory&) {
        const auto r = self->state(s.x[0]);
        std::lock_guard lock(r->mutex);
        const auto db = [](double v) { return v > 1e-8 ? static_cast<float>(std::max(-160.0, 20.0 * std::log10(v))) : -160.0f; };
        r->peak_db.clear();
        r->average_db.clear();
        for (std::size_t ch = 0; ch < r->peak.size(); ++ch) {
            r->peak_db.push_back(db(r->peak[ch]));
            r->average_db.push_back(db(r->meter_frames ? std::sqrt(r->squares[ch] / static_cast<double>(r->meter_frames)) : 0.0));
            r->peak[ch] = r->squares[ch] = 0;
        }
        r->meter_frames = 0;
    }, "v@:");
    support.method(recorder, "peakPowerForChannel:", [self, return_real32](CpuState& s, GuestMemory&) {
        const auto r = self->state(s.x[0]);
        std::lock_guard lock(r->mutex);
        return_real32(s, s.x[2] < r->peak_db.size() ? r->peak_db[s.x[2]] : -160.0f);
    }, "f@:Q");
    support.method(recorder, "averagePowerForChannel:", [self, return_real32](CpuState& s, GuestMemory&) {
        const auto r = self->state(s.x[0]);
        std::lock_guard lock(r->mutex);
        return_real32(s, s.x[2] < r->average_db.size() ? r->average_db[s.x[2]] : -160.0f);
    }, "f@:Q");
    support.method(recorder, "dealloc", [self, sp, recorder](CpuState& s, GuestMemory&) {
        if (const auto r = sp->objc.find_host_state<Recorder>(s.x[0])) {
            {
                std::lock_guard lock(r->mutex);
                r->deallocating = true;
            }
            self->finish(s.x[0], r, false);
            for (auto* object : {&r->url, &r->settings, &r->format}) sp->release(std::exchange(*object, 0));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], recorder, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- AVAudioApplication ----
    const auto application = declare(support, "AVAudioApplication", "NSObject");
    // AVAudioApplicationRecordPermission: 'undt', 'deny', 'grnt'.
    const auto permission = [](std::int64_t status) -> std::uint64_t {
        return status == 3 ? 0x67726E74 : status == 0 ? 0x756E6474 : 0x64656E79;
    };
    support.method(application, "sharedInstance", [sp, application](CpuState& s, GuestMemory&) {
        static Address shared{};
        if (!shared) shared = sp->send_id(sp->send_id(application, "alloc"), "init");   // lives for the process
        s.x[0] = shared;
    }, "@@:", true);
    support.method(application, "recordPermission", [permission](CpuState& s, GuestMemory&) { s.x[0] = permission(capture_authorization(false, false)); }, "q@:");
    support.method(application, "requestRecordPermissionWithCompletionHandler:", [self, permission](CpuState& s, GuestMemory&) {
        const auto granted = capture_authorization(false, true) == 3;
        static_cast<void>(permission);
        if (!s.x[2]) return;
        const auto block = self->bridge_->block_copy(s.x[2]);
        const auto b = self->bridge_;
        b->dispatch_async(b->main_queue(), [b, block, granted] {
            static_cast<void>(b->invoke_block(block, {granted ? 1U : 0U}));
            b->block_release(block);
        });
    }, "v@:@?", true);
    support.method(application, "isInputMuted", [](CpuState& s, GuestMemory&) { s.x[0] = input_muted() ? 1 : 0; }, "B@:");
    support.method(application, "setInputMuted:error:", [](CpuState& s, GuestMemory&) {
        input_muted() = (s.x[2] & 0xff) != 0;
        s.x[0] = 1;
    }, "B@:B^@");
}

} // namespace

void install_avaudio_recorder(Support& support) { std::make_shared<Recorders>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media::avaudio
