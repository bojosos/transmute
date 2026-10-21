// OpenAL (src/alchemy/darwin/openal/): no device or context can be created.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view openal = "/System/Library/Frameworks/OpenAL.framework/Versions/A/OpenAL";

const Case openal_case("openal", [](Fixture& f) {
    require(f.call(openal, "_alcOpenDevice", {0}) == 0, "no output device");
    require(f.call(openal, "_alcGetError", {0}) == 0xa004, "ALC_INVALID_VALUE after alcOpenDevice");
    require(f.call(openal, "_alcGetError", {0}) == 0, "alcGetError clears the error");
    require(f.call(openal, "_alGetError") == 0xa004, "AL_INVALID_OPERATION without a context");
    require(f.call(openal, "_alGetEnumValue", {f.text("AL_PLAYING")}) == 0x1012, "AL_PLAYING");
});

} // namespace
