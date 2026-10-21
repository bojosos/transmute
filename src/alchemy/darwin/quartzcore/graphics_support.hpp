#pragma once

// Shared helpers of the QLab graphics group (G6) providers: register access
// for the Apple ARM64 calling convention, and PropertyBag, the host state of
// the group's value-like classes (animations, shadows, CoreImage objects,
// WebKit configurations), with typed accessors registered per class.

#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <any>
#include <array>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::graphics {

inline constexpr std::string_view quartzcore_library = "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore";
inline constexpr std::string_view coreimage_library = "/System/Library/Frameworks/CoreImage.framework/Versions/A/CoreImage";
inline constexpr std::string_view coregraphics_library = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
inline constexpr std::string_view metal_library = "/System/Library/Frameworks/Metal.framework/Versions/A/Metal";
inline constexpr std::string_view metalkit_library = "/System/Library/Frameworks/MetalKit.framework/Versions/A/MetalKit";
inline constexpr std::string_view webkit_library = "/System/Library/Frameworks/WebKit.framework/Versions/A/WebKit";
inline constexpr std::string_view glkit_library = "/System/Library/Frameworks/GLKit.framework/Versions/A/GLKit";
inline constexpr std::string_view opengl_library = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL";
inline constexpr std::string_view libsystem_library = "/usr/lib/libSystem.B.dylib";

inline constexpr auto rect_type = "{CGRect={CGPoint=dd}{CGSize=dd}}";
inline constexpr auto point_type = "{CGPoint=dd}";
inline constexpr auto size_type = "{CGSize=dd}";

// ---- registers --------------------------------------------------------------------
inline double d(const CpuState& s, unsigned index) { return std::bit_cast<double>(s.v[index][0]); }
inline float f(const CpuState& s, unsigned index) { return std::bit_cast<float>(static_cast<std::uint32_t>(s.v[index][0])); }
inline void put_d(CpuState& s, unsigned index, double value) { s.v[index] = {std::bit_cast<std::uint64_t>(value), 0}; }
inline void put_f(CpuState& s, unsigned index, float value) { s.v[index] = {std::bit_cast<std::uint32_t>(value), 0}; }
inline bool flag(const CpuState& s, unsigned index) { return (s.x[index] & 0xff) != 0; }
inline void put_bool(CpuState& s, bool value) { s.x[0] = value ? 1 : 0; }
inline std::array<double, 4> rect_arg(const CpuState& s, unsigned first = 0) {
    return {d(s, first), d(s, first + 1), d(s, first + 2), d(s, first + 3)};
}
inline void put_rect(CpuState& s, const std::array<double, 4>& r) { for (unsigned i = 0; i < 4; ++i) put_d(s, i, r[i]); }
inline std::array<double, 4> result_rect(const CallResult& r) { return {r.d(0), r.d(1), r.d(2), r.d(3)}; }

// ---- property bags ------------------------------------------------------------------
// Scalar values are kept as raw 64-bit register images (double / float bits
// or integers) and four of them per entry (points, sizes, rectangles).
struct PropertyBag final : ObjectPayload {
    std::mutex mutex;
    std::map<std::string, Address> objects;                     // +1 each (nil values are absent)
    std::map<std::string, std::array<std::uint64_t, 4>> scalars;
    std::any extra;                                             // class-specific host state
};

class Props {
public:
    Props(Support& support, Address cls) : support_(support), cls_(cls) {}
    [[nodiscard]] Address cls() const { return cls_; }

    // An object property: setters retain (or copy, `copy`), getters return
    // the stored value (no autorelease, like a nonatomic synthesized getter).
    // `fallback` answers when the value was never set (nil when 0).
    Props& object(const char* getter, const char* setter, bool copy = false, std::function<Address()> fallback = {});
    // Only the setter of an object property stored under `key`, for a class
    // that answers the getter itself (a lazily made default, ...).
    Props& object_setter(const char* key, const char* setter, bool copy = false);
    Props& real(const char* getter, const char* setter, double fallback = 0);          // CGFloat / double / NSTimeInterval
    Props& single(const char* getter, const char* setter, float fallback = 0);         // float
    Props& boolean(const char* getter, const char* setter, bool fallback = false);
    Props& integer(const char* getter, const char* setter, std::int64_t fallback = 0); // NSInteger / NSUInteger / enums
    Props& point(const char* getter, const char* setter, std::array<double, 2> fallback = {});   // CGPoint / CGSize
    Props& rect(const char* getter, const char* setter, std::array<double, 4> fallback = {});

    // -dealloc releasing the bag (once per class hierarchy root) and, with
    // `copying`, -copyWithZone: copying it into a new instance of the
    // receiver's class (object values retained again; `copy` properties were
    // copies already). An immutable value class answers -copyWithZone: itself.
    Props& lifecycle(bool copying = true);

private:
    Support& support_;
    Address cls_;
};

std::shared_ptr<PropertyBag> bag(Support& support, Address object);
// Typed reads for the providers' own logic.
Address bag_object(Support& support, Address object, const std::string& key);
void bag_set_object(Support& support, Address object, const std::string& key, Address value, bool copy = false);
std::optional<double> bag_real(Support& support, Address object, const std::string& key);
std::optional<std::int64_t> bag_integer(Support& support, Address object, const std::string& key);
std::optional<std::array<std::uint64_t, 4>> bag_raw(Support& support, Address object, const std::string& key);
void bag_set_raw(Support& support, Address object, const std::string& key, std::array<std::uint64_t, 4> value);

// A +0 autoreleased NSString.
Address autoreleased_string(Support& support, std::string_view text);
// An NSArray's elements (CF arrays directly, otherwise -count / -objectAtIndex:).
std::vector<Address> array_items(Support& support, Address array);
// The NSNumber's double value (0 for nil).
double number_value(Support& support, Address number);

} // namespace alchemy::gui::completion_kit::graphics
