#include "alchemy/darwin/kit/completion/graphics/providers.hpp"

#include "alchemy/darwin/quartzcore/graphics_math.hpp"

#include <array>
#include <cstring>
#include <vector>

// GLKit: the framework identity (QLab links it) and the GLKMath values and
// functions it binds. GLKMatrix4Identity and GLKQuaternionIdentity are data
// (16 and 4 floats, 16-byte aligned); GLKQuaternionSlerp interpolates along
// the shorter arc (graphics_math.cpp). GLKit's OpenGL classes (GLKView,
// GLKTextureLoader, GLKBaseEffect) are not provided: no OpenGL context can
// exist under Alchemy.
namespace alchemy::gui::completion_kit::graphics {
namespace {
constexpr std::string_view glkit_library = "/System/Library/Frameworks/GLKit.framework/Versions/A/GLKit";

void float_data(Support& support, const std::string& symbol, const std::vector<float>& values) {
    if (support.process.resolve(glkit_library, symbol)) return;
    const auto size = values.size() * sizeof(float);
    const auto address = support.process.allocate_guest(size, 16);
    std::vector<std::uint8_t> bytes(size);
    std::memcpy(bytes.data(), values.data(), size);
    support.memory().write_bytes(address, bytes);
    support.process.install_export(std::string(glkit_library), symbol, {address, ExportKind::data});
}
} // namespace

void install_glkit(Support& support) {
    ProcessContext::add_provider_library(std::string(glkit_library));
    float_data(support, "_GLKMatrix4Identity", {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    float_data(support, "_GLKMatrix3Identity", {1, 0, 0, 0, 1, 0, 0, 0, 1});
    float_data(support, "_GLKMatrix2Identity", {1, 0, 0, 1});
    float_data(support, "_GLKQuaternionIdentity", {0, 0, 0, 1});
    // GLKQuaternion GLKQuaternionSlerp(GLKQuaternion q0, GLKQuaternion q1, float t):
    // the quaternions are four-float HFAs (s0-s3, s4-s7); t no longer fits in
    // a vector register and is the first stack argument; the result is in s0-s3.
    support.fill_function(glkit_library, "_GLKQuaternionSlerp", [](CpuState& s, GuestMemory& m) {
        Quaternion from{}, to{};
        for (unsigned i = 0; i < 4; ++i) {
            from[i] = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[i][0]));
            to[i] = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[4 + i][0]));
        }
        const auto t = std::bit_cast<float>(m.read32(s.sp));
        const auto result = quaternion_slerp(from, to, t);
        for (unsigned i = 0; i < 4; ++i) s.v[i] = {std::bit_cast<std::uint32_t>(result[i]), 0};
    });
}

} // namespace alchemy::gui::completion_kit::graphics
