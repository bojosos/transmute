#include "alchemy/darwin/openal/openal.hpp"

#include <cstdint>
#include <string_view>
#include <utility>

// The AL and ALC enum names of al.h / alc.h (alGetEnumValue, alcGetEnumValue), and the
// AL_EXT_float32 / AL_EXT_MCFORMATS formats.
namespace alchemy::openal {
namespace {

constexpr std::pair<std::string_view, std::uint32_t> al_enums[] = {
    {"AL_NONE", 0}, {"AL_FALSE", 0}, {"AL_TRUE", 1}, {"AL_SOURCE_RELATIVE", 0x202},
    {"AL_CONE_INNER_ANGLE", 0x1001}, {"AL_CONE_OUTER_ANGLE", 0x1002}, {"AL_PITCH", 0x1003},
    {"AL_POSITION", 0x1004}, {"AL_DIRECTION", 0x1005}, {"AL_VELOCITY", 0x1006}, {"AL_LOOPING", 0x1007},
    {"AL_BUFFER", 0x1009}, {"AL_GAIN", 0x100a}, {"AL_MIN_GAIN", 0x100d}, {"AL_MAX_GAIN", 0x100e},
    {"AL_ORIENTATION", 0x100f}, {"AL_SOURCE_STATE", 0x1010}, {"AL_INITIAL", 0x1011}, {"AL_PLAYING", 0x1012},
    {"AL_PAUSED", 0x1013}, {"AL_STOPPED", 0x1014}, {"AL_BUFFERS_QUEUED", 0x1015},
    {"AL_BUFFERS_PROCESSED", 0x1016}, {"AL_REFERENCE_DISTANCE", 0x1020}, {"AL_ROLLOFF_FACTOR", 0x1021},
    {"AL_CONE_OUTER_GAIN", 0x1022}, {"AL_MAX_DISTANCE", 0x1023}, {"AL_SEC_OFFSET", 0x1024},
    {"AL_SAMPLE_OFFSET", 0x1025}, {"AL_BYTE_OFFSET", 0x1026}, {"AL_SOURCE_TYPE", 0x1027},
    {"AL_STATIC", 0x1028}, {"AL_STREAMING", 0x1029}, {"AL_UNDETERMINED", 0x1030},
    {"AL_FORMAT_MONO8", 0x1100}, {"AL_FORMAT_MONO16", 0x1101}, {"AL_FORMAT_STEREO8", 0x1102},
    {"AL_FORMAT_STEREO16", 0x1103}, {"AL_FREQUENCY", 0x2001}, {"AL_BITS", 0x2002}, {"AL_CHANNELS", 0x2003},
    {"AL_SIZE", 0x2004}, {"AL_UNUSED", 0x2010}, {"AL_PENDING", 0x2011}, {"AL_PROCESSED", 0x2012},
    {"AL_NO_ERROR", 0}, {"AL_INVALID_NAME", 0xa001}, {"AL_INVALID_ENUM", 0xa002},
    {"AL_INVALID_VALUE", 0xa003}, {"AL_INVALID_OPERATION", 0xa004}, {"AL_OUT_OF_MEMORY", 0xa005},
    {"AL_VENDOR", 0xb001}, {"AL_VERSION", 0xb002}, {"AL_RENDERER", 0xb003}, {"AL_EXTENSIONS", 0xb004},
    {"AL_DOPPLER_FACTOR", 0xc000}, {"AL_DOPPLER_VELOCITY", 0xc001}, {"AL_SPEED_OF_SOUND", 0xc003},
    {"AL_DISTANCE_MODEL", 0xd000}, {"AL_INVERSE_DISTANCE", 0xd001}, {"AL_INVERSE_DISTANCE_CLAMPED", 0xd002},
    {"AL_LINEAR_DISTANCE", 0xd003}, {"AL_LINEAR_DISTANCE_CLAMPED", 0xd004},
    {"AL_EXPONENT_DISTANCE", 0xd005}, {"AL_EXPONENT_DISTANCE_CLAMPED", 0xd006}};
constexpr std::pair<std::string_view, std::uint32_t> alc_enums[] = {
    {"ALC_FALSE", 0}, {"ALC_TRUE", 1}, {"ALC_FREQUENCY", 0x1007}, {"ALC_REFRESH", 0x1008},
    {"ALC_SYNC", 0x1009}, {"ALC_MONO_SOURCES", 0x1010}, {"ALC_STEREO_SOURCES", 0x1011},
    {"ALC_NO_ERROR", 0}, {"ALC_INVALID_DEVICE", 0xa001}, {"ALC_INVALID_CONTEXT", 0xa002},
    {"ALC_INVALID_ENUM", 0xa003}, {"ALC_INVALID_VALUE", 0xa004}, {"ALC_OUT_OF_MEMORY", 0xa005},
    {"ALC_MAJOR_VERSION", 0x1000}, {"ALC_MINOR_VERSION", 0x1001}, {"ALC_ATTRIBUTES_SIZE", 0x1002},
    {"ALC_ALL_ATTRIBUTES", 0x1003}, {"ALC_DEFAULT_DEVICE_SPECIFIER", 0x1004},
    {"ALC_DEVICE_SPECIFIER", 0x1005}, {"ALC_EXTENSIONS", 0x1006}, {"ALC_CAPTURE_DEVICE_SPECIFIER", 0x310},
    {"ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER", 0x311}, {"ALC_CAPTURE_SAMPLES", 0x312}};

constexpr std::pair<std::string_view, std::uint32_t> extension_enums[] = {
    {"AL_FORMAT_MONO_FLOAT32", 0x10010}, {"AL_FORMAT_STEREO_FLOAT32", 0x10011}, {"AL_FORMAT_QUAD8", 0x1204},
    {"AL_FORMAT_QUAD16", 0x1205}, {"AL_FORMAT_QUAD32", 0x1206}, {"AL_FORMAT_REAR8", 0x1207}, {"AL_FORMAT_REAR16", 0x1208},
    {"AL_FORMAT_REAR32", 0x1209}, {"AL_FORMAT_51CHN8", 0x120A}, {"AL_FORMAT_51CHN16", 0x120B}, {"AL_FORMAT_51CHN32", 0x120C},
    {"AL_FORMAT_61CHN8", 0x120D}, {"AL_FORMAT_61CHN16", 0x120E}, {"AL_FORMAT_61CHN32", 0x120F}, {"AL_FORMAT_71CHN8", 0x1210},
    {"AL_FORMAT_71CHN16", 0x1211}, {"AL_FORMAT_71CHN32", 0x1212}, {"ALC_DEFAULT_ALL_DEVICES_SPECIFIER", 0x1012},
    {"ALC_ALL_DEVICES_SPECIFIER", 0x1013}};

} // namespace

std::uint32_t enum_value(std::string_view name, bool extensions) {
    for (const auto& [key, value] : al_enums) if (key == name) return value;
    for (const auto& [key, value] : alc_enums) if (key == name) return value;
    if (extensions)
        for (const auto& [key, value] : extension_enums) if (key == name) return value;
    return 0;
}

} // namespace alchemy::openal
