#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

// Foundation NSGeometry (headless-correct): NSIsEmptyRect, NSEqualSizes,
// NSIntersectionRect, NSMouseInRect. CGRect / CGPoint / CGAffineTransform
// values are CoreGraphics' headless shell (darwin/shells.hpp).
// Moved from darwin/identities/installer_frameworks.cpp (an installer
// binds them first); registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class Geometry final : public Provider {
public:
    using Provider::Provider;
    struct Rect { double x, y, width, height; };
    static Rect rect_argument(const CpuState& s, unsigned first) {
        return {real(s, first), real(s, first + 1), real(s, first + 2), real(s, first + 3)};
    }
    static void return_rect(CpuState& s, const Rect& value) {
        return_real(s, 0, value.x); return_real(s, 1, value.y); return_real(s, 2, value.width); return_real(s, 3, value.height);
    }

    void install() {
        // CGRect / CGPoint / CGAffineTransform values are CoreGraphics'
        // headless shell (darwin/shells.hpp).
        // NSGeometry (Foundation).
        function(foundation, "_NSIsEmptyRect", [](CpuState& s, GuestMemory&) {
            const auto value = rect_argument(s, 0);
            s.x[0] = !(value.width > 0) || !(value.height > 0);
        });
        function(foundation, "_NSEqualSizes", [](CpuState& s, GuestMemory&) {
            s.x[0] = real(s, 0) == real(s, 2) && real(s, 1) == real(s, 3);
        });
        function(foundation, "_NSIntersectionRect", [](CpuState& s, GuestMemory&) {
            const auto a = rect_argument(s, 0), b = rect_argument(s, 4);
            const auto left = std::max(a.x, b.x), right = std::min(a.x + a.width, b.x + b.width);
            const auto bottom = std::max(a.y, b.y), top = std::min(a.y + a.height, b.y + b.height);
            return_rect(s, left < right && bottom < top ? Rect{left, bottom, right - left, top - bottom} : Rect{0, 0, 0, 0});
        });
        function(foundation, "_NSMouseInRect", [](CpuState& s, GuestMemory&) {
            const auto px = real(s, 0), py = real(s, 1);
            const auto r = rect_argument(s, 2);
            const bool flipped = (s.x[0] & 0xff) != 0;
            const bool inside_x = px >= r.x && px < r.x + r.width;
            const bool inside_y = flipped ? (py >= r.y && py < r.y + r.height) : (py > r.y && py <= r.y + r.height);
            s.x[0] = r.width > 0 && r.height > 0 && inside_x && inside_y;
        });
    }
};

} // namespace

void install_foundation_geometry(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<Geometry>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
