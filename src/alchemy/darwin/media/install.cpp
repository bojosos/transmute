#include "alchemy/darwin/media/media.hpp"

namespace alchemy::gui::fill {

void install_media(InstallContext& context) {
    install_core_media(context);
    install_pixel_transfer(context);
    install_vt_decompression(context);
    install_video_toolbox(context);
    install_av_foundation(context);
    install_core_video_pixel_buffers(context);
    install_core_video_gaps(context);
    install_audio(context);
    install_media_accessibility(context);
    install_image_capture(context);
}

} // namespace alchemy::gui::fill
