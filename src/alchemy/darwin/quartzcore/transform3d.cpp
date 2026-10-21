#include "alchemy/darwin/kit/completion/graphics/providers.hpp"

#include "alchemy/darwin/quartzcore/graphics_math.hpp"
#include "alchemy/darwin/quartzcore/graphics_support.hpp"

// The CATransform3D functions of QuartzCore (graphics_math.cpp does the
// math). CATransform3D is sixteen doubles (128 bytes, not a homogeneous
// aggregate the registers can hold), so transforms are passed by reference
// in x registers and returned through x8; CGAffineTransform (48 bytes)
// likewise. Each function is added only when no provider exports it.
namespace alchemy::gui::completion_kit::graphics {
namespace {

Transform3D read(GuestMemory& m, Address at) {
    if (!at) fail("CATransform3D argument without storage");
    Transform3D t{};
    for (unsigned i = 0; i < 16; ++i) t[i] = std::bit_cast<double>(m.read64(at + i * 8));
    return t;
}
void write(GuestMemory& m, Address at, const Transform3D& t) {
    if (!at) fail("CATransform3D result without storage (x8)");
    for (unsigned i = 0; i < 16; ++i) m.write64(at + i * 8, std::bit_cast<std::uint64_t>(t[i]));
}

} // namespace

void install_transform3d(Support& support) {
    const auto fn = [&support](const char* symbol, Handler handler) { support.fill_function(quartzcore_library, symbol, std::move(handler)); };
    fn("_CATransform3DIsIdentity", [](CpuState& s, GuestMemory& m) { put_bool(s, transform_is_identity(read(m, s.x[0]))); });
    fn("_CATransform3DEqualToTransform", [](CpuState& s, GuestMemory& m) { put_bool(s, read(m, s.x[0]) == read(m, s.x[1])); });
    fn("_CATransform3DMakeTranslation", [](CpuState& s, GuestMemory& m) { write(m, s.x[8], transform_translation(d(s, 0), d(s, 1), d(s, 2))); });
    fn("_CATransform3DMakeScale", [](CpuState& s, GuestMemory& m) { write(m, s.x[8], transform_scale(d(s, 0), d(s, 1), d(s, 2))); });
    fn("_CATransform3DMakeRotation", [](CpuState& s, GuestMemory& m) {
        write(m, s.x[8], transform_rotation(d(s, 0), d(s, 1), d(s, 2), d(s, 3)));
    });
    fn("_CATransform3DTranslate", [](CpuState& s, GuestMemory& m) {
        write(m, s.x[8], transform_translate(read(m, s.x[0]), d(s, 0), d(s, 1), d(s, 2)));
    });
    fn("_CATransform3DScale", [](CpuState& s, GuestMemory& m) {
        write(m, s.x[8], transform_scale_by(read(m, s.x[0]), d(s, 0), d(s, 1), d(s, 2)));
    });
    fn("_CATransform3DRotate", [](CpuState& s, GuestMemory& m) {
        write(m, s.x[8], transform_rotate(read(m, s.x[0]), d(s, 0), d(s, 1), d(s, 2), d(s, 3)));
    });
    fn("_CATransform3DConcat", [](CpuState& s, GuestMemory& m) { write(m, s.x[8], transform_concat(read(m, s.x[0]), read(m, s.x[1]))); });
    fn("_CATransform3DInvert", [](CpuState& s, GuestMemory& m) { write(m, s.x[8], transform_invert(read(m, s.x[0]))); });
    fn("_CATransform3DIsAffine", [](CpuState& s, GuestMemory& m) { put_bool(s, transform_is_affine(read(m, s.x[0]))); });
    fn("_CATransform3DMakeAffineTransform", [](CpuState& s, GuestMemory& m) {
        std::array<double, 6> affine{};
        for (unsigned i = 0; i < 6; ++i) affine[i] = std::bit_cast<double>(m.read64(s.x[0] + i * 8));
        write(m, s.x[8], transform_from_affine(affine));
    });
    fn("_CATransform3DGetAffineTransform", [](CpuState& s, GuestMemory& m) {
        const auto affine = transform_to_affine(read(m, s.x[0]));
        for (unsigned i = 0; i < 6; ++i) m.write64(s.x[8] + i * 8, std::bit_cast<std::uint64_t>(affine[i]));
    });
}

} // namespace alchemy::gui::completion_kit::graphics
