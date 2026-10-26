#include "alchemy/darwin/headless_additions.hpp"

// alchemy-fx: CoreVideo pixel buffers are core_video.cpp's; the headless answer
// (pixel_buffers.cpp) is alchemy.exe's only.
namespace alchemy::darwin_headless {

void install_media_pixel_buffers(ProcessContext&, ObjcRuntime&, CoreFoundation&) {}

} // namespace alchemy::darwin_headless
