// OpenAL (src/alchemy/darwin/openal/): the enum names, errors without a context, and a device
// when the host has an audio output (mixing is checked over a fake device in the media tests,
// media_guest_tests.cpp "openal").
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view openal = "/System/Library/Frameworks/OpenAL.framework/Versions/A/OpenAL";

const Case openal_case("openal", [](Fixture& f) {
    require(f.call(openal, "_alGetError") == 0xa004, "AL_INVALID_OPERATION without a context");
    require(f.call(openal, "_alGetEnumValue", {f.text("AL_PLAYING")}) == 0x1012, "AL_PLAYING");
    require(f.call(openal, "_alGetEnumValue", {f.text("AL_FORMAT_STEREO_FLOAT32")}) == 0x10011, "AL_EXT_float32 format");
    require(f.call(openal, "_alcOpenDevice", {f.text("No Such Device")}) == 0 && f.call(openal, "_alcGetError", {0}) == 0xa004,
            "an unknown device name: ALC_INVALID_VALUE");
    require(f.call(openal, "_alcGetError", {0}) == 0, "alcGetError clears the error");
    require(f.call(openal, "_alcCaptureOpenDevice", {0, 44100, 0x1101, 1024}) == 0, "no capture device");
    if (const auto device = f.call(openal, "_alcOpenDevice", {0})) {
        require(f.read_text(f.call(openal, "_alcGetString", {device, 0x1005})) == "Alchemy Default Output", "device name");
        require(f.call(openal, "_alcCloseDevice", {device}) == 1, "alcCloseDevice");
    }
});

} // namespace
