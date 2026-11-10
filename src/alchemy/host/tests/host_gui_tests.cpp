// Host layer tests for the GUI-only areas (ui, audio, midi, media_codec; compiled into
// alchemy-fx, so alchemy_host_tests, which links alchemy_core alone, cannot
// reach them). A real backend answers its queries and refuses what does not
// exist without opening anything; on the null backend every call fails with
// Error::unsupported.
#include "alchemy/host/audio.hpp"
#include "alchemy/host/camera.hpp"
#include "alchemy/host/gpu.hpp"
#include "alchemy/host/host.hpp"
#include "alchemy/host/media_codec.hpp"
#include "alchemy/host/midi.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/speech.hpp"
#include "alchemy/host/sync.hpp"
#include "alchemy/host/time.hpp"
#include "alchemy/host/ui.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace host = alchemy::host;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void null_backend() {
    host::midi::Devices devices;
    require(devices.inputs().error() == host::Error::unsupported && devices.outputs().error() == host::Error::unsupported,
        "null MIDI lists fail");
    require(devices.open_input(0, {}).error() == host::Error::unsupported, "null MIDI input fails");
    require(host::audio::endpoints().error() == host::Error::unsupported, "null audio endpoints fail");
    require(host::ui::monitors().error() == host::Error::unsupported && host::ui::idle_ms().error() == host::Error::unsupported &&
                host::ui::keyboard_kind().error() == host::Error::unsupported &&
                host::ui::reduce_motion().error() == host::Error::unsupported &&
                host::ui::high_contrast().error() == host::Error::unsupported,
        "null ui queries fail");
    require(host::ui::FrameTimer::create().error() == host::Error::unsupported, "null frame timer fails");
    require(host::ui::sample_thread_stack(host::process::current_thread_id(), 4).error() == host::Error::unsupported,
        "null stack sampling fails");
    host::ui::AlertDialog alert;
    alert.buttons = {"OK"};
    require(host::ui::show_alert(alert).error() == host::Error::unsupported &&
                host::ui::run_file_dialog({}).error() == host::Error::unsupported,
        "null dialogs fail");
    require(!host::ui::create_window_server(), "the null backend has no window server");
    require(host::ui::screen_reader().error() == host::Error::unsupported && host::ui::dark_appearance().error() == host::Error::unsupported &&
                host::ui::shell_open("x").error() == host::Error::unsupported && host::ui::desktop_picture().error() == host::Error::unsupported,
        "null shell calls fail");
    require(host::ui::spelling_languages().error() == host::Error::unsupported &&
                host::ui::check_spelling("en-US", u"teh").error() == host::Error::unsupported,
        "null spell checking fails");
    require(host::ui::wait_for_input({}, 0, false).error() == host::Error::unsupported && !host::ui::is_gui_thread(),
        "null run-loop waits fail");
    require(host::gpu::decode_image(std::vector<std::uint8_t>(8, 1)).error() == host::Error::unsupported &&
                host::gpu::vulkan_get_instance_proc_addr().error() == host::Error::not_found,
        "null gpu calls fail");
    require(host::gpu::system_fonts().error() == host::Error::unsupported && !host::gpu::registered_fonts() &&
                host::gpu::layout_text({}).error() == host::Error::unsupported,
        "null fonts fail");
    require(host::gpu::raster_metrics({"Arial", 64}).error() == host::Error::unsupported, "null raster text fails");
    namespace mc = host::media_codec;
    require(mc::encoders(mc::Codec::h264).error() == host::Error::unsupported && mc::open_video({}).error() == host::Error::unsupported &&
                mc::open_audio({mc::Codec::aac, 48000, 2}).error() == host::Error::unsupported,
        "null media encoders fail");
}

// The host's media encoders, where it has them: a machine without one of them reports the skip.
void media_codecs() {
    namespace mc = host::media_codec;
    const auto names = [](mc::Codec c) { return c == mc::Codec::h264 ? "H.264" : c == mc::Codec::hevc ? "HEVC" : "AAC"; };
    // A backend without host codecs (Linux, until its codecs are decided) says so for every codec.
    if (mc::encoders(mc::Codec::h264).error() == host::Error::unsupported) {
        for (const auto codec : {mc::Codec::h264, mc::Codec::hevc, mc::Codec::aac})
            require(mc::encoders(codec).error() == host::Error::unsupported && mc::decoders(codec).error() == host::Error::unsupported,
                std::string("no host ") + names(codec) + " codecs");
        std::cout << "no host media codecs on this backend: skipped" << std::endl;
        return;
    }
    // Video: 60 pictures of a moving gradient, a keyframe forced at 45.
    for (const auto codec : {mc::Codec::h264, mc::Codec::hevc}) {
        const auto listed = mc::encoders(codec);
        require(static_cast<bool>(listed), std::string(names(codec)) + " encoders are listed");
        if (listed->empty()) { std::cout << "no " << names(codec) << " encoder on this host: skipped\n"; continue; }
        for (const auto& c : *listed) require(!c.id.empty() && c.codec == codec, "an encoder has an id");
        mc::VideoSettings settings;
        settings.codec = codec;
        settings.width = 320;
        settings.height = 240;
        settings.frame_rate = 30;
        settings.keyframe_interval = 30;
        const auto opened = mc::open_video(settings);
        if (!opened) { std::cout << "no " << names(codec) << " encoder took 320x240 here (" << host::name(opened.error()) << "): skipped\n"; continue; }
        auto* encoder = *opened;
        std::vector<std::uint8_t> luma(320 * 240), chroma(320 * 120);
        std::vector<mc::Sample> samples;
        for (int i = 0; i < 60; ++i) {
            for (std::uint32_t y = 0; y < 240; ++y)
                for (std::uint32_t x = 0; x < 320; ++x) luma[y * 320 + x] = static_cast<std::uint8_t>((x + y + i * 4) & 0xff);
            std::fill(chroma.begin(), chroma.end(), static_cast<std::uint8_t>(128 + (i & 15)));
            mc::Picture p{320, 240, luma.data(), 320, chroma.data(), 320, i * 333333LL, 333333};
            require(static_cast<bool>(mc::encode(encoder, p, i == 45, samples)), std::string(names(codec)) + " encodes a picture");
        }
        require(static_cast<bool>(mc::drain(encoder, samples)), std::string(names(codec)) + " drains");
        require(samples.size() == 60, std::string(names(codec)) + ": a sample per picture, " + std::to_string(samples.size()));
        require(samples.front().keyframe && samples.front().pts == 0, std::string(names(codec)) + ": the first sample is a keyframe");
        bool forced = false;
        for (const auto& s : samples) {
            std::size_t at = 0;
            while (at + 4 <= s.data.size()) at += 4 + ((std::size_t(s.data[at]) << 24) | (s.data[at + 1] << 16) | (s.data[at + 2] << 8) | s.data[at + 3]);
            require(at == s.data.size() && !s.data.empty(), std::string(names(codec)) + ": samples are length-prefixed NAL units");
            forced = forced || (s.pts == 45 * 333333LL && s.keyframe);
        }
        require(forced, std::string(names(codec)) + ": the forced keyframe");
        const auto sets = mc::parameter_sets(encoder);
        const auto type = [codec](const std::vector<std::uint8_t>& u) { return codec == mc::Codec::hevc ? (u[0] >> 1) & 0x3f : u[0] & 0x1f; };
        require(sets.size() == (codec == mc::Codec::hevc ? 3u : 2u) && type(sets[0]) == (codec == mc::Codec::hevc ? 32 : 7),
            std::string(names(codec)) + ": the parameter sets");
        std::cout << names(codec) << " encoder: " << mc::component(encoder).name << (mc::component(encoder).hardware ? " (hardware)" : "") << '\n';
        const auto first_id = mc::component(encoder).id;
        // Decoded back by the host's decoder: every picture, its time, and the last picture close to what went in.
        const auto video_decoders = mc::decoders(codec);
        require(static_cast<bool>(video_decoders), std::string(names(codec)) + " decoders are listed");
        if (video_decoders->empty()) {
            std::cout << "no " << names(codec) << " decoder on this host: skipped\n";
        } else {
            const auto opened_decoder = mc::open_video_decoder({codec, sets, 320, 240, {}});
            require(static_cast<bool>(opened_decoder), std::string(names(codec)) + " decoder opens");
            std::vector<mc::DecodedPicture> pictures;
            for (const auto& s : samples) require(static_cast<bool>(mc::decode(*opened_decoder, s, pictures)), std::string(names(codec)) + " decodes");
            require(static_cast<bool>(mc::drain(*opened_decoder, pictures)), std::string(names(codec)) + " decoder drains");
            require(pictures.size() == 60, std::string(names(codec)) + ": a picture per sample, " + std::to_string(pictures.size()));
            for (std::size_t i = 0; i < pictures.size(); ++i)
                require(pictures[i].width == 320 && pictures[i].height == 240 && pictures[i].bit_depth == 8 && pictures[i].pts == static_cast<std::int64_t>(i) * 333333,
                        std::string(names(codec)) + ": picture " + std::to_string(i) + " in presentation order, displayed size");
            // The last picture against the last input (luma, PSNR).
            double error = 0;
            for (std::uint32_t y = 0; y < 240; ++y)
                for (std::uint32_t x = 0; x < 320; ++x) {
                    const double d = double(pictures.back().luma[y * pictures.back().luma_stride + x]) - luma[y * 320 + x];
                    error += d * d;
                }
            const double psnr = 10 * std::log10(255.0 * 255.0 / std::max(error / (320.0 * 240.0), 1e-9));
            require(psnr > 30, std::string(names(codec)) + ": the decoded picture matches, PSNR " + std::to_string(psnr));
            std::cout << names(codec) << " decoder: " << mc::component(*opened_decoder).name << ", PSNR " << psnr << " dB\n";
            mc::flush(*opened_decoder);
            mc::close(*opened_decoder);
        }
        mc::close(encoder);
        // Every other component by its id: ten pictures, a sample each (software encoders are synchronous
        // transforms, hardware ones asynchronous).
        for (const auto& other : *listed) {
            if (other.id == first_id) continue;
            auto by_id = settings;
            by_id.component = other.id;
            const auto again = mc::open_video(by_id);
            if (!again) { std::cout << names(codec) << " encoder " << other.name << " refused 320x240 (" << host::name(again.error()) << ")\n"; continue; }
            std::vector<mc::Sample> few;
            for (int i = 0; i < 10; ++i) {
                mc::Picture p{320, 240, luma.data(), 320, chroma.data(), 320, i * 333333LL, 333333};
                require(static_cast<bool>(mc::encode(*again, p, false, few)), other.name + " encodes");
            }
            require(static_cast<bool>(mc::drain(*again, few)) && few.size() == 10 && few.front().keyframe && mc::parameter_sets(*again).size() >= 2,
                other.name + ": ten samples, the first a keyframe: " + std::to_string(few.size()));
            std::cout << names(codec) << " encoder: " << other.name << (other.hardware ? " (hardware)" : "") << '\n';
            mc::close(*again);
        }
        // A profile and level asked for are the stream's (the SPS's profile_idc and level_idc), and an encoder takes
        // pictures again after a drain (VTCompressionSessionCompleteFrames, then more frames).
        {
            auto levelled = settings;
            levelled.profile = codec == mc::Codec::h264 ? mc::Profile::h264_main : mc::Profile::hevc_main;
            levelled.level = codec == mc::Codec::h264 ? 31 : 93;   // 3.1
            const auto again = mc::open_video(levelled);
            require(static_cast<bool>(again), std::string(names(codec)) + ": main profile, level 3.1 opens");
            std::vector<mc::Sample> few;
            for (int i = 0; i < 5; ++i) {
                mc::Picture p{320, 240, luma.data(), 320, chroma.data(), 320, i * 333333LL, 333333};
                require(static_cast<bool>(mc::encode(*again, p, false, few)), std::string(names(codec)) + ": level 3.1 encodes");
            }
            require(static_cast<bool>(mc::drain(*again, few)) && few.size() == 5, std::string(names(codec)) + ": five samples before the drain");
            for (int i = 5; i < 10; ++i) {
                mc::Picture p{320, 240, luma.data(), 320, chroma.data(), 320, i * 333333LL, 333333};
                require(static_cast<bool>(mc::encode(*again, p, false, few)), std::string(names(codec)) + ": encodes after a drain");
            }
            require(static_cast<bool>(mc::drain(*again, few)) && few.size() == 10, std::string(names(codec)) + ": ten samples after a second drain, " +
                std::to_string(few.size()));
            // The SPS without its emulation prevention bytes: H.264 profile_idc and level_idc at 1 and 3; HEVC's
            // general_profile_idc (low 5 bits) and general_level_idc at 3 and 14 (after the 2-byte header).
            const auto sets_now = mc::parameter_sets(*again);
            const auto& sps = sets_now.at(codec == mc::Codec::hevc ? 1 : 0);
            std::vector<std::uint8_t> raw;
            for (std::size_t i = 0, zeros = 0; i < sps.size(); ++i) {
                if (zeros >= 2 && sps[i] == 3) { zeros = 0; continue; }
                raw.push_back(sps[i]);
                zeros = sps[i] == 0 ? zeros + 1 : 0;
            }
            const unsigned profile = codec == mc::Codec::h264 ? raw.at(1) : raw.at(3) & 0x1f, level = codec == mc::Codec::h264 ? raw.at(3) : raw.at(14);
            require(profile == (codec == mc::Codec::h264 ? 77 : 1) && level == levelled.level,
                std::string(names(codec)) + ": profile " + std::to_string(profile) + ", level_idc " + std::to_string(level));
            mc::close(*again);
        }
        settings.width = 321;
        require(mc::open_video(settings).error() == host::Error::invalid_argument, "odd sizes are refused");
        settings.width = 320;
        settings.component = "no such encoder";
        require(mc::open_video(settings).error() == host::Error::not_found, "an unknown component is not found");
    }
    // AAC: one second of a sine at the encoder's own rate and channels.
    const auto listed = mc::encoders(mc::Codec::aac);
    require(static_cast<bool>(listed), "AAC encoders are listed");
    if (listed->empty()) { std::cout << "no AAC encoder on this host: skipped\n"; return; }
    const auto& c = listed->front();
    require(!c.sample_rates.empty() && !c.channel_counts.empty() && !c.bit_rates.empty(), "the AAC encoder says what it takes");
    const auto rate = std::find(c.sample_rates.begin(), c.sample_rates.end(), 48000u) != c.sample_rates.end() ? 48000u : c.sample_rates.front();
    const auto channels = std::find(c.channel_counts.begin(), c.channel_counts.end(), 2u) != c.channel_counts.end() ? 2u : c.channel_counts.front();
    const auto opened = mc::open_audio({mc::Codec::aac, rate, channels, 0, c.id});
    require(static_cast<bool>(opened), "an AAC encoder opens: " + std::string(host::name(opened.error())) + " (" + c.name + ", " + std::to_string(rate) + " Hz, " +
                                           std::to_string(channels) + " ch, " + std::to_string(c.bit_rates.size()) + " bit rates)");
    auto* encoder = *opened;
    require(mc::frames_per_packet(encoder) == 1024 && std::find(c.bit_rates.begin(), c.bit_rates.end(), mc::bit_rate(encoder)) != c.bit_rates.end(),
        "1024 frames a packet, at a bit rate it lists");
    const auto config = mc::configuration(encoder);
    require(config.size() >= 2 && (config[0] >> 3) == 2 && ((config[1] >> 3) & 15) == channels, "an AAC-LC AudioSpecificConfig of the channels");
    std::vector<std::int16_t> pcm(std::size_t(rate) * channels);
    for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<std::int16_t>(10000 * std::sin(0.05 * double(i / channels)));
    std::vector<mc::Sample> packets;
    for (std::size_t at = 0; at < pcm.size(); at += 1000 * channels)
        require(static_cast<bool>(mc::encode(encoder, std::span(pcm).subspan(at, std::min<std::size_t>(1000 * channels, pcm.size() - at)), packets)), "AAC encodes");
    require(static_cast<bool>(mc::drain(encoder, packets)), "AAC drains");
    const auto expected = (rate + 1023) / 1024;
    require(packets.size() >= expected && packets.size() <= expected + 3, "AAC packets for a second: " + std::to_string(packets.size()));
    for (const auto& p : packets) require(!p.data.empty() && p.data.size() < 6144 * channels, "an AAC frame");
    std::cout << "AAC encoder: " << c.name << ", " << rate << " Hz, " << channels << " ch, " << mc::bit_rate(encoder) << " b/s, "
              << packets.size() << " packets\n";
    // Decoded: a 1024-frame block per packet, those the decoder holds back (its recorded delay) at the drain.
    const auto audio_decoders = mc::decoders(mc::Codec::aac);
    require(static_cast<bool>(audio_decoders), "AAC decoders are listed");
    if (audio_decoders->empty()) {
        std::cout << "no AAC decoder on this host: skipped\n";
    } else {
        const auto opened_decoder = mc::open_audio_decoder({mc::Codec::aac, rate, channels, config, {}});
        require(static_cast<bool>(opened_decoder), "an AAC decoder opens");
        std::vector<std::int16_t> decoded;
        for (const auto& p : packets) require(static_cast<bool>(mc::decode(*opened_decoder, p.data, decoded)), "AAC decodes");
        const auto before = decoded.size() / channels;
        require(static_cast<bool>(mc::drain(*opened_decoder, decoded)), "the AAC decoder drains");
        const auto held = decoded.size() / channels - before;
        std::cout << "AAC decoder " << mc::component(*opened_decoder).name << " " << mc::component(*opened_decoder).id << ": holds back " << held
                  << ", recorded " << mc::component(*opened_decoder).delay << '\n';
        require(decoded.size() / channels == packets.size() * 1024 && held == mc::component(*opened_decoder).delay,
                "the AAC decoder gives a block a packet and holds back its recorded delay");
        mc::close(*opened_decoder);
    }
    mc::close(encoder);
    require(mc::open_audio({mc::Codec::aac, 7, channels}).error() == host::Error::invalid_argument, "a rate no encoder takes is refused");
    // Each AAC encoder's recorded delay is what decoding its output measures (the host's decoder; skipped without one).
    for (const auto& component : *listed) {
        const auto r = std::find(component.sample_rates.begin(), component.sample_rates.end(), 48000u) != component.sample_rates.end() ? 48000u : component.sample_rates.front();
        const auto n = std::find(component.channel_counts.begin(), component.channel_counts.end(), 2u) != component.channel_counts.end() ? 2u : component.channel_counts.front();
        const auto measured = mc::measure_delay({mc::Codec::aac, r, n, 0, component.id});
        if (!measured && measured.error() == host::Error::not_found) { std::cout << "no AAC decoder to measure " << component.name << " with: skipped\n"; continue; }
        require(static_cast<bool>(measured), component.name + ": the delay is measured");
        std::cout << "AAC encoder " << component.name << " " << component.id << ": measured delay " << *measured << ", recorded " << component.delay << '\n';
        require(*measured == component.delay, component.name + ": the measured delay is the recorded one");
    }
}

// The sans-serif family the font tests use: Arial on win32, DejaVu Sans on
// Linux (fontconfig's default sans-serif family; Arial is not installed there).
struct SansFamily {
    std::string family, postscript;
};
SansFamily sans_family() {
    if (host::backend() == "win32") return {"Arial", "ArialMT"};
    return {"DejaVu Sans", "DejaVuSans"};
}

void images_area() {
    // Image files: a PNG round trip; bytes that are no image are refused.
    const host::gpu::Pixels pixels{2, 1, {255, 0, 0, 255, 0, 0, 255, 128}};
    const auto png = host::gpu::encode_image(pixels, host::gpu::ImageFormat::png);
    require(png && png->size() > 8, "encode a PNG");
    const auto decoded = host::gpu::decode_image(*png);
    require(decoded && decoded->width == 2 && decoded->height == 1 && decoded->rgba == pixels.rgba, "the PNG decodes to its pixels");
    require(!host::gpu::decode_image(std::vector<std::uint8_t>(16, 7)), "bytes that are no image");
    // ImageIO's frames: a two-frame GIF; a header read has no pixels.
    const host::gpu::Pixels frames[] = {pixels, pixels};
    const auto gif = host::gpu::encode_image_frames(host::gpu::ImageFormat::gif, frames, 0.85);
    require(gif && host::gpu::image_frame_count(*gif).value_or(0) == 2, "a two-frame GIF");
    const auto header = host::gpu::image_frame(*png, 0, false);
    require(header && header->width == 2 && header->height == 1 && header->rgba.empty() && header->has_alpha, "a PNG frame header");
    const auto frame = host::gpu::image_frame(*png, 0, true);
    require(frame && frame->rgba == pixels.rgba && frame->orientation == 1, "a PNG frame's pixels");
    require(host::gpu::image_frame(*png, 1, false).error() == host::Error::out_of_range, "no second PNG frame");
}

void fonts_area() {
    // Fonts: a system family, its glyphs, metrics, outlines and tables; a layout and a fallback.
    namespace gpu = host::gpu;
    const auto sans = sans_family();
    const auto system = gpu::system_fonts();
    require(system && *system && *system == *gpu::system_fonts(), "the system fonts (one collection)");
    const auto sans_face = gpu::family_font(*system, sans.family, gpu::font_weight::normal, gpu::font_style::normal, gpu::font_stretch::normal);
    require(static_cast<bool>(sans_face), "the sans family");
    const auto info = sans_face->info();
    require(info && info->family == sans.family && info->metrics.units_per_em > 0 && info->metrics.ascent > 0 && !info->monospace,
        "the sans family's names and metrics");
    require(sans_face->same_face(*sans_face) && sans_face->glyph_count() > 100, "a face is itself; the sans family has glyphs");
    const std::uint32_t letters[] = {'A', 0x10ffff};
    const auto glyphs = sans_face->glyphs(letters);
    require(glyphs && (*glyphs)[0] != 0 && (*glyphs)[1] == 0, "a glyph for A, none for U+10FFFF");
    const std::uint16_t a[] = {(*glyphs)[0]};
    const auto metrics = sans_face->glyph_metrics(a);
    require(metrics && (*metrics)[0].advance_width > 0, "A's advance");
    const gpu::Point origin[] = {{0, 0}};
    const auto outline = sans_face->outlines(12, a, origin, {});
    require(outline && !outline->empty() && outline->front().op == gpu::PathOp::move, "A's outline");
    const auto head = sans_face->table(0x68656164);   // 'head'
    require(head && head->size() >= 54 && sans_face->table(0x7a7a7a7a).error() == host::Error::not_found, "font tables");
    const auto named = gpu::named_font(*system, gpu::FontName::postscript, sans.postscript);
    require(named && named->same_face(*sans_face), "the sans family by PostScript name");
    const auto names = gpu::family_names(*system);
    require(names && std::find(names->begin(), names->end(), sans.family) != names->end(), "the sans family is a system family");
    const auto faces = gpu::family_faces(*system, sans.family);
    require(faces && faces->size() >= 2, "the sans family's faces");
    require(!gpu::family_font(*system, "No Such Family 4711", 400, 0, 5), "an unknown family");
    require(gpu::register_font_file({}).error() == host::Error::invalid_argument &&
                gpu::register_font_file(std::vector<std::uint8_t>(64, 1)).error() == host::Error::unsupported,
        "font files that are not fonts");

    gpu::TextLayoutRequest request;
    request.text = u"Hello";
    request.base.collection = *system;
    request.base.family = sans.family;
    request.width = request.height = 10000;
    const auto layout = gpu::layout_text(request);
    require(layout && layout->lines.size() == 1 && layout->lines[0].length == 5 && layout->runs.size() == 1 &&
                layout->runs[0].glyphs.size() == 5 && layout->runs[0].font.same_face(*sans_face),
        "a one-line layout in the sans family");
    const auto fallback = gpu::fallback_font(u"\u4e2d", sans.family, 400, 0, 5);
    require(fallback && fallback->length == 1 && !fallback->font.same_face(*sans_face), "a fallback face for a CJK character");

    // Lines end after their newline (and the spaces a wrap leaves); spans' tags reach their runs.
    request.text = u"ab\ncd";
    const auto two_lines = gpu::layout_text(request);
    require(two_lines && two_lines->lines.size() == 2 && two_lines->lines[0].length == 3 && two_lines->lines[0].newline_length == 1 &&
                two_lines->lines[1].length == 2 && two_lines->lines[1].newline_length == 0,
        "a newline ends a line");
    request.text = u"Hello world";
    request.base.size = 12;
    request.width = 40;
    const auto wrapped = gpu::layout_text(request);
    require(wrapped && wrapped->lines.size() == 2 && wrapped->lines[0].length == 6 && wrapped->lines[1].length == 5, "a word wraps");
    request.width = 10000;
    gpu::TextSpan spans[2];
    spans[0] = request.base;
    spans[0].start = 0;
    spans[0].length = 5;
    spans[0].tag = 7;
    spans[1] = request.base;
    spans[1].start = 5;
    spans[1].length = 6;
    spans[1].tag = 9;
    request.spans = spans;
    const auto tagged = gpu::layout_text(request);
    require(tagged && tagged->runs.size() == 2 && tagged->runs[0].tag == 7 && tagged->runs[0].text_position == 0 &&
                tagged->runs[0].text_length == 5 && tagged->runs[1].tag == 9 && tagged->runs[1].text_position == 5 &&
                tagged->runs[1].clusters.size() == 6 && tagged->runs[1].x > tagged->runs[0].x,
        "spans' tags reach their runs");
}

void canvas_area() {
    // 2D: a square filled on a surface, read back.
    namespace gpu = host::gpu;
    auto surface = gpu::Surface::create(8, 8);
    require(surface && *surface, "a surface");
    const gpu::PathElement square[] = {{gpu::PathOp::move, {gpu::Point{2, 2}}}, {gpu::PathOp::line, {gpu::Point{6, 2}}},
                                       {gpu::PathOp::line, {gpu::Point{6, 6}}}, {gpu::PathOp::line, {gpu::Point{2, 6}}},
                                       {gpu::PathOp::close, {}}};
    const auto geometry = gpu::Geometry::path(square, false);
    require(geometry && *geometry, "a square geometry");
    require(static_cast<bool>((*surface)->begin()), "begin drawing");
    (*surface)->set_transform({});
    gpu::Brush red;
    red.color = {1, 0, 0, 1};
    require(static_cast<bool>((*surface)->fill(*geometry, red)), "fill the square");
    require(static_cast<bool>((*surface)->end()), "end drawing");
    const auto locked = (*surface)->lock({0, 0, 8, 8}, false);
    require(locked && locked->at(4, 4, {0, 0, 8, 8})[2] == 255 && locked->at(4, 4, {0, 0, 8, 8})[3] == 255 &&
                locked->at(0, 0, {0, 0, 8, 8})[3] == 0,
        "the square is red, the corner clear");
}

void raster_text_area() {
    // System text rasterizing: the sans family at 64 pixels.
    namespace gpu = host::gpu;
    const auto sans = sans_family();
    const gpu::RasterFont raster{sans.family, 256};
    const auto metrics = gpu::raster_metrics(raster);
    require(metrics && metrics->ascent > 40 && metrics->descent > 5 && metrics->cap_height, "the sans family's raster metrics");
    const auto width = gpu::raster_width(raster, u"Hello");
    const auto prefix = gpu::raster_prefix_widths(raster, u"Hello");
    require(width && *width > 50 && prefix && prefix->size() == 5 && prefix->back() == *width && (*prefix)[0] < (*prefix)[1],
        "advances of a line");
    const auto coverage = gpu::raster_coverage(raster, u"H", 64, 80, 2, 60);
    require(coverage && coverage->size() == 64u * 80u && *std::max_element(coverage->begin(), coverage->end()) > 0.9F,
        "an H is drawn");
}

void midi_area() {
    host::midi::Devices devices;
    const auto inputs = devices.inputs();
    const auto outputs = devices.outputs();
    require(inputs && outputs, "MIDI ports list");
    const std::uint8_t note[] = {0x90, 60, 0};
    require(!devices.open_input(inputs->size() + 1000, {}), "a missing MIDI input is refused");
    require(!devices.send(outputs->size() + 1000, note), "a missing MIDI output is refused");
}

void audio_area() {
    const auto endpoints = host::audio::endpoints();
    require(static_cast<bool>(endpoints) || endpoints.error() == host::Error::unsupported, "audio endpoints list (or no audio on this host)");
    if (!endpoints) return;
    for (const auto& endpoint : endpoints->endpoints) require(!endpoint.uid.empty(), "audio endpoints have uids");
    require(host::audio::open_output("no such endpoint 4711", 0, 0, [](float*, std::uint32_t, std::uint32_t) {}).error() == host::Error::not_found,
        "an unknown output endpoint is not found");
    // The default output asks for frames once started; they are silence, so nothing is heard.
    if (endpoints->default_output.empty()) return;
    std::atomic<std::uint32_t> rendered{0}, channels{0};
    const auto output = host::audio::open_output("", 0, 0, [&](float* samples, std::uint32_t frames, std::uint32_t) {
        std::fill(samples, samples + std::size_t(frames) * channels, 0.0F);
        rendered += frames;
    });
    require(static_cast<bool>(output), "the default output opens");
    const auto format = host::audio::output_format(*output);
    require(format.sample_rate > 0 && format.channels > 0 && format.period_frames > 0, "its format");
    channels = format.channels;   // before the start: render is not called until then
    require(static_cast<bool>(host::audio::start_output(*output)), "the default output starts");
    const auto deadline = host::time::monotonic_ns() + 2'000'000'000ULL;
    while (rendered < format.sample_rate / 20 && host::time::monotonic_ns() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    host::audio::stop_output(*output);
    const std::uint32_t after_stop = rendered;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(after_stop >= format.sample_rate / 20 && rendered == after_stop, "50 ms of silence rendered, none after the stop");
    host::audio::close_output(*output);
}

void camera_area() {
    // Listed (none is opened); an unknown one is refused.
    const auto cameras = host::camera::cameras();
    require(static_cast<bool>(cameras), "cameras list");
    for (const auto& camera : *cameras) require(!camera.id.empty() && !camera.formats.empty(), "cameras have ids and formats");
    const auto unknown = host::camera::open("no such camera 4711", 0, 0, 0, [](const host::camera::Picture&) {});
    require(!unknown && (unknown.error() == host::Error::not_found || unknown.error() == host::Error::unsupported), "an unknown camera is refused");
}

void speech_area() {
    const auto voices = host::speech::voices();
    require(static_cast<bool>(voices) || voices.error() == host::Error::unsupported, "speech voices list (or no speech on this host)");
}

void real_backend() {
    const auto monitors = host::ui::monitors();
    require(static_cast<bool>(monitors), "monitors list");
    for (const auto& monitor : *monitors) require(!monitor.primary || (monitor.left == 0 && monitor.top == 0), "the primary monitor is at the origin");
    require(static_cast<bool>(host::ui::idle_ms()) && static_cast<bool>(host::ui::keyboard_kind()), "input queries answer");
    require(static_cast<bool>(host::ui::reduce_motion()) && static_cast<bool>(host::ui::high_contrast()), "accessibility settings answer");

    auto timer = host::ui::FrameTimer::create();
    require(timer && static_cast<bool>(*timer), "a frame timer");
    const auto start = host::time::monotonic_ns();
    require(static_cast<bool>(timer->wait(2'000'000, 1000)), "the frame timer waits");
    const auto waited = host::time::monotonic_ns() - start;
    require(waited >= 1'000'000 && waited < 900'000'000, "the frame timer waits about its delay");
    require(static_cast<bool>(timer->wait(5'000'000'000, 20)), "a wait ends at its limit");
    require(!host::ui::FrameTimer().wait(1, 1), "no timer, no wait");

    // Dialogs (nothing is shown: an alert needs a button).
    require(host::ui::show_alert({}).error() == host::Error::invalid_argument, "an alert without buttons is refused");
    require(host::ui::disable_windows({}, 0).empty(), "no windows, none disabled");
    host::ui::enable_windows({});
    require(static_cast<bool>(host::ui::screen_reader()) && static_cast<bool>(host::ui::desktop_picture()), "shell settings answer");
    require(static_cast<bool>(host::ui::reveal_files({})), "revealing nothing opens nothing");
    // Vulkan: the loader is the system's (or there is none).
    {
        const auto loader = host::gpu::vulkan_get_instance_proc_addr();
        require(loader ? *loader != nullptr : loader.error() == host::Error::not_found, "the Vulkan loader");
        require(host::gpu::vulkan_surface_extension() == "VK_KHR_win32_surface", "the window surface extension");
        require(host::ui::window_client_size(0).error() == host::Error::not_found, "no window, no client area");
    }

    // The run loop's waits: a signaled event, a timeout, the handle limit.
    {
        const auto event = host::sync::create_event(false, false);
        require(static_cast<bool>(event), "an event");
        const host::OwnedHandle owned(*event);
        const host::Handle handles[] = {*event};
        require(host::ui::wait_for_input(handles, 0, false).error() == host::Error::timed_out, "an unsignaled event times out");
        require(static_cast<bool>(host::sync::set_event(*event)), "set the event");
        const auto woken = host::ui::wait_for_input(handles, 1000, true);
        require(woken && *woken == 0, "a signaled event wakes the wait");
        require(host::ui::wait_limit() >= 2 && host::ui::wait_for_input({}, 0, false).error() == host::Error::invalid_argument,
            "the wait needs a handle");
        require(!host::ui::in_modal_loop(), "not in a modal loop");
    }

    // Spell checking: whatever languages the host has; an unknown one finds nothing.
    const auto languages = host::ui::spelling_languages();
    require(static_cast<bool>(languages), "spelling languages list");
    const auto unknown = host::ui::check_spelling("xx-XX", u"teh qiuck");
    require(unknown && unknown->empty(), "an unknown language finds nothing");
    for (const auto& language : *languages)
        if (language == "en-US") {
            const auto found = host::ui::check_spelling(language, u"the qiuck fox");
            require(found && found->size() == 1 && (*found)[0].start == 4 && (*found)[0].length == 5, "a misspelled word is found");
            const auto guesses = host::ui::spelling_suggestions(language, u"qiuck");
            require(guesses && !guesses->empty(), "suggestions for a misspelled word");
        }
    const auto server = host::ui::create_window_server();
    require(server != nullptr, "a window server");

    // Another thread's host stack, sampled while it sleeps.
    std::atomic<std::uint64_t> sleeper_id{0};
    std::atomic<bool> stop{false};
    std::thread sleeper([&] {
        sleeper_id = host::process::current_thread_id();
        while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });
    while (!sleeper_id) std::this_thread::yield();
    const auto stack = host::ui::sample_thread_stack(sleeper_id, 24);
    stop = true;
    sleeper.join();
    require(stack && !stack->empty() && stack->size() <= 24 && !stack->front().module.empty(), "a sleeping thread's host stack");
    require(host::ui::sample_thread_stack(sleeper_id, 24).error() != host::Error::none, "an exited thread cannot be sampled");
}

} // namespace

// ALCHEMY_HOST_TEST_FONT_REPORT=<font file>: the host's reading of a font
// file (names, metrics, glyphs, outlines, a layout), printed for comparing
// two hosts' output (the same file, for example Windows's arial.ttf, read
// on win32 and on Linux).
void font_report(const std::string& path) {
    namespace gpu = host::gpu;
    std::ifstream file(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(!bytes.empty(), "the font file reads");
    const auto font = gpu::font_from_data(bytes);
    require(static_cast<bool>(font), "the font file loads");
    const auto info = font->info();
    require(static_cast<bool>(info), "the font's info");
    std::cout << std::fixed << std::setprecision(3);
    const auto& m = info->metrics;
    std::cout << "family=" << info->family << " style=" << info->style_name << " full=" << info->full_name << " postscript=" << info->postscript
              << "\nweight=" << info->weight << " style=" << info->style << " stretch=" << info->stretch << " monospace=" << info->monospace
              << " symbol=" << info->symbol << " color=" << info->color << "\nupem=" << m.units_per_em << " ascent=" << m.ascent
              << " descent=" << m.descent << " leading=" << m.leading << " cap=" << m.cap_height << " x=" << m.x_height
              << " underline=" << m.underline_position << "/" << m.underline_thickness << " box=" << m.box_left << "," << m.box_bottom << ","
              << m.box_width << "," << m.box_height << " slant=" << m.slant << "\nglyph_count=" << font->glyph_count() << '\n';
    const std::uint32_t text[] = {'A', 'g', 'x', ' ', 0xe9, 0x4e2d, 0x1f600};
    const auto glyphs = font->glyphs(text);
    require(static_cast<bool>(glyphs), "glyphs");
    const auto metrics = font->glyph_metrics(*glyphs);
    require(static_cast<bool>(metrics), "glyph metrics");
    for (std::size_t index = 0; index < glyphs->size(); ++index) {
        const auto& g = (*metrics)[index];
        std::cout << "U+" << std::hex << text[index] << std::dec << " glyph=" << (*glyphs)[index] << " advance=" << g.advance_width << "/"
                  << g.advance_height << " bearings=" << g.left_bearing << "," << g.right_bearing << "," << g.top_bearing << ","
                  << g.bottom_bearing << " vorigin=" << g.vertical_origin_y << '\n';
    }
    const std::uint16_t two[] = {(*glyphs)[0], (*glyphs)[1]};
    const gpu::Point places[] = {{0, 0}, {10, 2}};
    const auto outline = font->outlines(12, two, places, {1, 0, 0, 1, 5, 20});
    require(static_cast<bool>(outline), "outlines");
    std::cout << "outline elements=" << outline->size() << '\n';
    for (const auto& element : *outline) {
        std::cout << "  " << static_cast<int>(element.op);
        const int points = element.op == gpu::PathOp::close ? 0 : element.op == gpu::PathOp::cubic ? 3 : element.op == gpu::PathOp::quad ? 2 : 1;
        for (int index = 0; index < points; ++index) std::cout << " " << element.points[index].x << "," << element.points[index].y;
        std::cout << '\n';
    }
    for (const std::uint32_t tag : {0x68656164u, 0x68686561u, 0x4f532f32u, 0x706f7374u, 0x47535542u}) {
        const auto table = font->table(tag);
        std::cout << "table " << std::hex << tag << std::dec << " " << (table ? static_cast<long long>(table->size()) : -1LL) << '\n';
    }
    require(static_cast<bool>(gpu::register_font_file(bytes)), "the font file registers");
    gpu::TextLayoutRequest request;
    const std::u16string sample = u"Hello, world! Fig. AVA office\twrap me";
    request.text = sample;
    request.base.collection = gpu::registered_fonts();
    request.base.family = info->family;
    request.base.size = 12;
    request.width = 150;
    request.height = 1000;
    const auto layout = gpu::layout_text(request);
    require(static_cast<bool>(layout), "a layout in the registered font");
    for (const auto& line : layout->lines) std::cout << "line " << line.length << " newline " << line.newline_length << '\n';
    for (const auto& run : layout->runs) {
        std::cout << "run at " << run.text_position << "+" << run.text_length << " size " << run.size << " rtl " << run.rtl << " x " << run.x
                  << " y " << run.y << " same " << run.font.same_face(*font) << "\n  glyphs";
        for (const auto glyph : run.glyphs) std::cout << " " << glyph;
        std::cout << "\n  advances";
        for (const auto advance : run.advances) std::cout << " " << advance;
        std::cout << "\n  clusters";
        for (const auto cluster : run.clusters) std::cout << " " << cluster;
        std::cout << '\n';
    }
}

int main() {
    try {
        if (const auto report = host::process::environment("ALCHEMY_HOST_TEST_FONT_REPORT")) {
            font_report(*report);
            return 0;
        }
        // ALCHEMY_HOST_TEST_SKIP_AREAS=fonts,canvas,... leaves those areas out,
        // so a backend in progress can test the areas it has written. "platform"
        // is the rest of the real backend (ui, Vulkan, the run loop, spelling,
        // the window server, thread samples).
        const auto skipped = host::process::environment("ALCHEMY_HOST_TEST_SKIP_AREAS").value_or(std::string());
        const auto run_area = [&](std::string_view name, void (*area)()) {
            std::size_t start = 0;
            while (start <= skipped.size()) {
                const auto end = std::min(skipped.find(',', start), skipped.size());
                if (std::string_view(skipped).substr(start, end - start) == name) {
                    std::cout << "skipped area " << name << '\n';
                    return;
                }
                start = end + 1;
            }
            area();
        };
        if (host::backend() == "null") null_backend();
        else {
            run_area("images", images_area);
            run_area("fonts", fonts_area);
            run_area("canvas", canvas_area);
            run_area("raster_text", raster_text_area);
            run_area("midi", midi_area);
            run_area("audio", audio_area);
            run_area("camera", camera_area);
            run_area("speech", speech_area);
            run_area("platform", real_backend);
            run_area("media_codecs", media_codecs);
        }
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "host GUI tests passed (" << host::backend() << ")\n";
    return 0;
}
