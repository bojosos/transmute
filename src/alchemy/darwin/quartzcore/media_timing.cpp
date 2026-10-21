#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <array>
#include <string>
#include <utility>

// CAMediaTimingFunction (QLab sets the timing of its animations), added when
// QuartzCore lacks it: a cubic Bezier from (0,0) to (1,1) with two control
// points. Named functions use Core Animation's control points:
//   linear (0,0)(1,1), easeIn (0.42,0)(1,1), easeOut (0,0)(0.58,1),
//   easeInEaseOut (0.42,0)(0.58,1), default (0.25,0.1)(0.25,1).
// The kCAMediaTimingFunction* name constants are exported when missing.
// Animations are timed but not interpolated (quartzcore.cpp), so the curve is
// data here; graphics/graphics_math.cpp evaluates it (timing_curve_value).
namespace alchemy::gui::completion_kit {
namespace {

constexpr std::string_view quartzcore_library = "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore";

struct TimingState final : ObjectPayload { std::array<float, 4> points{0, 0, 1, 1}; };

constexpr std::array<std::pair<const char*, std::array<float, 4>>, 5> named{{
    {"linear", {0.f, 0.f, 1.f, 1.f}}, {"easeIn", {0.42f, 0.f, 1.f, 1.f}}, {"easeOut", {0.f, 0.f, 0.58f, 1.f}},
    {"easeInEaseOut", {0.42f, 0.f, 0.58f, 1.f}}, {"default", {0.25f, 0.1f, 0.25f, 1.f}}}};

} // namespace

void install_media_timing(Support& support) {
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_kCAMediaTimingFunctionLinear", "linear"}, {"_kCAMediaTimingFunctionEaseIn", "easeIn"},
             {"_kCAMediaTimingFunctionEaseOut", "easeOut"}, {"_kCAMediaTimingFunctionEaseInEaseOut", "easeInEaseOut"},
             {"_kCAMediaTimingFunctionDefault", "default"}})
        if (!support.process.resolve(quartzcore_library, symbol)) support.string_constant(quartzcore_library, symbol, value);
    // The class may exist as a shell (a probe or a framework stub); its
    // methods are added either way.
    const auto cls = support.declare_class("CAMediaTimingFunction", "NSObject", quartzcore_library);
    support.objc.add_native_protocol(cls, "NSSecureCoding");
    auto* sp = &support;
    const auto state_of = [sp](Address self) { return sp->objc.host_state<TimingState>(self); };
    const auto make = [sp, cls, state_of](const std::array<float, 4>& points) {
        const auto made = sp->send_id(sp->send_id(cls, "alloc"), "init");
        state_of(made)->points = points;
        return made;
    };
    support.method(cls, "functionWithName:", [sp, make](CpuState& s, GuestMemory&) {
        const auto name = s.x[2] ? sp->cf.utf8(s.x[2]) : std::string();
        for (const auto& [key, points] : named)
            if (name == key) { s.x[0] = sp->send_id(make(points), "autorelease"); return; }
        fail("+[CAMediaTimingFunction functionWithName:] unknown name '" + name + "'");
    }, "@@:@", true);
    // Control points arrive as four floats in s0-s3.
    const auto points_of = [](const CpuState& s) {
        std::array<float, 4> points{};
        for (unsigned i = 0; i < 4; ++i) points[i] = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[i][0]));
        return points;
    };
    support.method(cls, "functionWithControlPoints::::", [sp, make, points_of](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(make(points_of(s)), "autorelease");
    }, "@@:ffff", true);
    support.method(cls, "initWithControlPoints::::", [state_of, points_of](CpuState& s, GuestMemory&) {
        state_of(s.x[0])->points = points_of(s);
    }, "@@:ffff");
    // -getControlPointAtIndex:values: (float[2]); index 0 is (0,0), 3 is (1,1).
    support.method(cls, "getControlPointAtIndex:values:", [state_of](CpuState& s, GuestMemory& m) {
        if (s.x[2] > 3) fail("-[CAMediaTimingFunction getControlPointAtIndex:values:] index " + std::to_string(s.x[2]));
        const auto& p = state_of(s.x[0])->points;
        const std::array<std::array<float, 2>, 4> all{{{0.f, 0.f}, {p[0], p[1]}, {p[2], p[3]}, {1.f, 1.f}}};
        if (!s.x[3]) return;
        m.write32(s.x[3], std::bit_cast<std::uint32_t>(all[s.x[2]][0]));
        m.write32(s.x[3] + 4, std::bit_cast<std::uint32_t>(all[s.x[2]][1]));
    }, "v@:Q^f");
    support.method(cls, "copyWithZone:", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->send_id(s.x[0], "retain"); }, "@@:^v");
    // Value equality over the control points.
    support.method(cls, "isEqual:", [sp, state_of](CpuState& s, GuestMemory&) {
        const auto other = s.x[2];
        if (other == s.x[0]) { s.x[0] = 1; return; }
        const auto theirs = other ? sp->objc.find_host_state<TimingState>(other) : nullptr;
        s.x[0] = theirs && theirs->points == state_of(s.x[0])->points ? 1 : 0;
    }, "B@:@");
    support.method(cls, "hash", [state_of](CpuState& s, GuestMemory&) {
        std::uint64_t h = 1469598103934665603ULL;
        for (const auto value : state_of(s.x[0])->points) h = (h ^ std::bit_cast<std::uint32_t>(value)) * 1099511628211ULL;
        s.x[0] = h;
    }, "Q@:");
    support.method(cls, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
}

} // namespace alchemy::gui::completion_kit
