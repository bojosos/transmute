#include "alchemy/darwin/media/media_support.hpp"

// QLab gap group G5 (media): AVFoundation, AudioToolbox (MusicPlayer/ExtAudioFile/AU listeners), AudioUnit, CoreMIDI, CoreMedia, CoreVideo, Accelerate.
// Owner and scope: docs/ALCHEMY_QLAB_GAPS.md, group G5. Each provider file in
// this directory adds one install_* function called from here (see
// media_support.hpp); cmake/darwin/kit_completion_media.cmake lists the sources and the
// group's tests. The CoreMedia, CoreVideo, VideoToolbox and Accelerate
// additions live in src/alchemy/media and src/alchemy/darwin/kit/fwgaps (fill providers
// this group owns).
namespace alchemy::gui::completion_kit {

void install_group_media(Support& support) {
    // AudioVideoBridging (AVB network audio): QLab links the framework but
    // imports no symbol and references no AVB class. No AVB interface exists
    // under Alchemy; the identity loads so linking succeeds, and any symbol
    // bound to it stays unresolved.
    ProcessContext::add_provider_library("/System/Library/Frameworks/AudioVideoBridging.framework/Versions/A/AudioVideoBridging");
    media::install_core_midi(support);
    media::install_hal(support);
    media::install_music(support);
    media::install_audio_file(support);
    media::install_au_listeners(support);
    media::install_av_foundation(support);
}

} // namespace alchemy::gui::completion_kit
