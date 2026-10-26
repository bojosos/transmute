#include "vpx/vpx_codec.h"
static const char *const cfg = "--target=generic-gnu --disable-encoders --enable-vp9-highbitdepth --disable-multithread";
const char *vpx_codec_build_config(void) { return cfg; }
