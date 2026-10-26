#pragma once

// Media frameworks for strict mode (src/alchemy/media): CoreMedia,
// VideoToolbox, AVFoundation, CoreVideo pixel-buffer gaps, AudioToolbox and
// AudioUnit gaps, MediaAccessibility and ImageCaptureCore. Qt WebEngine
// (a renderer's frame buffer browser panes) and QLab link them;
// installed by install_media (fill-only). The CoreAudio HAL (WASAPI devices),
// CoreMIDI (Windows MIDI), MusicPlayer, AudioFile/ExtAudioFile, AU listeners
// and the AVFoundation asset/format/audio-unit classes are the QLab media
// group's (src/alchemy/darwin/kit/completion/media), installed earlier.
//
// Alchemy has no camera, hardware or software video codec and no audio
// component (audio unit): capture device lists are empty, codec sessions
// cannot be created and the documented error codes say so. The data types
// that need no device are real (CMTime arithmetic, CMBlockBuffer,
// CMFormatDescription including H.264 parameter sets and audio descriptions,
// CMSampleBuffer with audio buffer lists, caption appearance defaults).

#include "alchemy/gui/fill_provider.hpp"

#include <optional>
#include <span>
#include <utility>

namespace alchemy::gui::fill {

void install_core_media(InstallContext& context);
void install_video_toolbox(InstallContext& context);
void install_av_foundation(InstallContext& context);
void install_core_video_gaps(InstallContext& context);
// core_video.cpp: CVPixelBuffer, attachments and pools (before install_core_video_gaps).
void install_core_video_pixel_buffers(InstallContext& context);
// pixel_transfer.cpp: VTPixelTransferSession and the VTSession property calls (before install_video_toolbox).
void install_pixel_transfer(InstallContext& context);
// vt_decompression.cpp: VTDecompressionSession for AV1, VP9 and VP8 (before install_video_toolbox).
void install_vt_decompression(InstallContext& context);
void install_audio(InstallContext& context);
void install_media_accessibility(InstallContext& context);
void install_image_capture(InstallContext& context);

// H.264 sequence parameter set (RBSP with emulation-prevention bytes): the
// coded picture size after cropping, or nullopt when it cannot be parsed.
std::optional<std::pair<std::int32_t, std::int32_t>> h264_sps_dimensions(std::span<const std::uint8_t> nal);

} // namespace alchemy::gui::fill
