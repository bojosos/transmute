#include "alchemy/darwin/kit/completion/graphics/providers.hpp"
#include "alchemy/darwin/quartzcore/graphics_support.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

// Core Animation's animation objects: CAAnimation, CAPropertyAnimation,
// CABasicAnimation, CASpringAnimation, CAKeyframeAnimation, CAAnimationGroup
// and CATransition, CAValueFunction, and CAEmitterCell (the cells of
// CAEmitterLayer, quartzcore.cpp: kept, no particles are drawn),
// with the CAMediaTiming properties and Apple's defaults (duration 0 meaning
// the transaction's 0.25 s, speed 1, repeatCount 0, fillMode "removed",
// removedOnCompletion YES, a retained delegate). Unknown keys are stored
// (CAAnimation is key-value coding compliant for any key), and the objects
// are copied when added to a layer (-[CALayer addAnimation:forKey:] in
// src/alchemy/darwin/quartzcore/quartzcore.cpp, which runs their timing and sends
// the delegate's animationDidStart: / animationDidStop:finished:, and shows
// CABasicAnimations' presentation values; keyframe, group, spring and
// transition animations are timed but do not change what is shown).
//
// CASpringAnimation's settlingDuration is computed from its spring: the
// motion of a mass on a damped spring from the start value to the end value
// (x'' = -(stiffness / mass)(x - 1) - (damping / mass) x', with the initial
// velocity in end-to-start distances per second), until it stays within 0.1%
// of the end. The macOS 14 perceptual duration and bounce follow the usual
// conversions (stiffness = (2 pi / duration)^2 mass, a damping ratio of
// 1 - bounce, or 1 / (1 + bounce) for a negative bounce); unverified against
// Core Animation.
namespace alchemy::gui::completion_kit::graphics {
namespace {

constexpr double pi = 3.14159265358979323846;

// The time a spring takes to settle within 0.1% of its end value.
double settling_duration(double mass, double stiffness, double damping, double velocity) {
    if (!(mass > 0) || !(stiffness > 0) || damping < 0) return 0;
    const double dt = 1.0 / 1000, limit = 100;
    double x = 0, v = velocity, last_out = 0;
    for (double t = 0; t < limit; t += dt) {
        const double a = (-stiffness * (x - 1) - damping * v) / mass;
        v += a * dt;   // semi-implicit Euler: stable for the spring
        x += v * dt;
        if (std::abs(x - 1) >= 0.001) last_out = t + dt;
        else if (t - last_out > 2 * pi * std::sqrt(mass / stiffness)) break;   // settled for a whole period
    }
    return last_out;
}

} // namespace

void install_animation(Support& support) {
    auto* sp = &support;
    const auto ca = support.declare_class("CAAnimation", "NSObject", quartzcore_library);
    support.objc.add_native_protocol(ca, "CAMediaTiming");
    support.objc.add_native_protocol(ca, "NSCopying");
    support.objc.add_native_protocol(ca, "NSSecureCoding");
    Props(support, ca)
        .lifecycle()
        .real("beginTime", "setBeginTime:")
        .real("timeOffset", "setTimeOffset:")
        .single("repeatCount", "setRepeatCount:")
        .real("repeatDuration", "setRepeatDuration:")
        .real("duration", "setDuration:")
        .single("speed", "setSpeed:", 1.0f)
        .boolean("autoreverses", "setAutoreverses:")
        .object("fillMode", "setFillMode:", true, [sp] { return sp->cf.constant_string("removed"); })
        .object("timingFunction", "setTimingFunction:")
        .object("delegate", "setDelegate:")   // CAAnimation's delegate is strong
        .boolean("isRemovedOnCompletion", "setRemovedOnCompletion:", true);
    support.method(ca, "removedOnCompletion", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send(s.x[0], "isRemovedOnCompletion").x0 & 0xff;
    }, "B@:");
    support.method(ca, "animation", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(sp->send_id(sp->send_id(s.x[0], "alloc"), "init"), "autorelease");
    }, "@@:", true);
    support.method(ca, "defaultValueForKey:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:@", true);
    support.method(ca, "shouldArchiveValueForKey:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@");
    support.method(ca, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    // Key-value coding for keys without accessors: stored with the animation.
    support.method(ca, "setValue:forUndefinedKey:", [sp](CpuState& s, GuestMemory&) {
        bag_set_object(*sp, s.x[0], "kvc:" + sp->cf.utf8(s.x[3]), s.x[2]);
    }, "v@:@@");
    support.method(ca, "valueForUndefinedKey:", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = bag_object(*sp, s.x[0], "kvc:" + sp->cf.utf8(s.x[2]));
    }, "@@:@");

    const auto property = support.declare_class("CAPropertyAnimation", "CAAnimation", quartzcore_library);
    Props(support, property)
        .object("keyPath", "setKeyPath:", true)
        .boolean("isAdditive", "setAdditive:")
        .boolean("isCumulative", "setCumulative:")
        .object("valueFunction", "setValueFunction:");
    support.method(property, "animationWithKeyPath:", [sp](CpuState& s, GuestMemory&) {
        const auto made = sp->send_id(sp->send_id(s.x[0], "alloc"), "init");
        sp->send_id(made, "setKeyPath:", {s.x[2]});
        s.x[0] = sp->send_id(made, "autorelease");
    }, "@@:@", true);
    support.method(property, "additive", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->send(s.x[0], "isAdditive").x0 & 0xff; }, "B@:");
    support.method(property, "cumulative", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->send(s.x[0], "isCumulative").x0 & 0xff; }, "B@:");

    const auto basic = support.declare_class("CABasicAnimation", "CAPropertyAnimation", quartzcore_library);
    Props(support, basic)
        .object("fromValue", "setFromValue:")
        .object("toValue", "setToValue:")
        .object("byValue", "setByValue:");

    // ---- CASpringAnimation ----
    const auto spring = support.declare_class("CASpringAnimation", "CABasicAnimation", quartzcore_library);
    Props(support, spring)
        .real("mass", "setMass:", 1)
        .real("stiffness", "setStiffness:", 100)
        .real("damping", "setDamping:", 10)
        .real("initialVelocity", "setInitialVelocity:", 0)
        .boolean("allowsOverdamping", "setAllowsOverdamping:");
    const auto spring_value = [sp](Address self, const char* getter) { return sp->send(self, getter).d(0); };
    support.method(spring, "settlingDuration", [spring_value](CpuState& s, GuestMemory&) {
        support_kit::return_real(s, 0, settling_duration(spring_value(s.x[0], "mass"), spring_value(s.x[0], "stiffness"),
                                            spring_value(s.x[0], "damping"), spring_value(s.x[0], "initialVelocity")));
    }, "d@:");
    support.method(spring, "perceptualDuration", [spring_value](CpuState& s, GuestMemory&) {
        const double mass = spring_value(s.x[0], "mass"), stiffness = spring_value(s.x[0], "stiffness");
        support_kit::return_real(s, 0, stiffness > 0 ? 2 * pi * std::sqrt(mass / stiffness) : 0);
    }, "d@:");
    support.method(spring, "bounce", [spring_value](CpuState& s, GuestMemory&) {
        const double mass = spring_value(s.x[0], "mass"), stiffness = spring_value(s.x[0], "stiffness");
        const double critical = 2 * std::sqrt(std::max(0.0, stiffness * mass));
        const double ratio = critical > 0 ? spring_value(s.x[0], "damping") / critical : 1;
        support_kit::return_real(s, 0, ratio <= 1 ? 1 - ratio : 1 / ratio - 1);
    }, "d@:");
    support.method(spring, "initWithPerceptualDuration:bounce:", [sp](CpuState& s, GuestMemory&) {
        const double duration = std::bit_cast<double>(s.v[0][0]), bounce = std::bit_cast<double>(s.v[1][0]);
        if (!(duration > 0) || !(bounce > -1) || !(bounce < 1))
            throw std::invalid_argument("Alchemy: -[CASpringAnimation initWithPerceptualDuration:bounce:] needs a duration > 0 and a bounce in (-1, 1)");
        const auto self = sp->send_id(s.x[0], "init");
        const double stiffness = std::pow(2 * pi / duration, 2);
        const double ratio = bounce >= 0 ? 1 - bounce : 1 / (1 + bounce);
        const auto set = [&](const char* setter, double value) {
            CallRegs r;
            r.set_d(0, value);
            static_cast<void>(sp->send(self, setter, r));
        };
        set("setMass:", 1);
        set("setStiffness:", stiffness);
        set("setDamping:", ratio * 2 * std::sqrt(stiffness));
        s.x[0] = self;
    }, "@@:dd");

    // ---- CAValueFunction ----
    const auto value_function = support.declare_class("CAValueFunction", "NSObject", quartzcore_library);
    Props(support, value_function).lifecycle(false);
    support.method(value_function, "functionWithName:", [sp](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        const auto made = sp->send_id(sp->send_id(s.x[0], "alloc"), "init");
        bag_set_object(*sp, made, "name", s.x[2], true);
        s.x[0] = sp->send_id(made, "autorelease");
    }, "@@:@", true);
    support.method(value_function, "name", [sp](CpuState& s, GuestMemory&) { s.x[0] = bag_object(*sp, s.x[0], "name"); }, "@@:");

    // ---- CAEmitterCell ----
    const auto cell = support.declare_class("CAEmitterCell", "NSObject", quartzcore_library);
    support.objc.add_native_protocol(cell, "CAMediaTiming");
    Props(support, cell)
        .lifecycle()
        .object("name", "setName:", true)
        .boolean("isEnabled", "setEnabled:", true)
        .single("birthRate", "setBirthRate:")
        .single("lifetime", "setLifetime:")
        .single("lifetimeRange", "setLifetimeRange:")
        .real("emissionLatitude", "setEmissionLatitude:")
        .real("emissionLongitude", "setEmissionLongitude:")
        .real("emissionRange", "setEmissionRange:")
        .real("velocity", "setVelocity:")
        .real("velocityRange", "setVelocityRange:")
        .real("xAcceleration", "setXAcceleration:")
        .real("yAcceleration", "setYAcceleration:")
        .real("zAcceleration", "setZAcceleration:")
        .real("scale", "setScale:", 1)
        .real("scaleRange", "setScaleRange:")
        .real("scaleSpeed", "setScaleSpeed:")
        .real("spin", "setSpin:")
        .real("spinRange", "setSpinRange:")
        .object("color", "setColor:")
        .single("redRange", "setRedRange:")
        .single("greenRange", "setGreenRange:")
        .single("blueRange", "setBlueRange:")
        .single("alphaRange", "setAlphaRange:")
        .single("redSpeed", "setRedSpeed:")
        .single("greenSpeed", "setGreenSpeed:")
        .single("blueSpeed", "setBlueSpeed:")
        .single("alphaSpeed", "setAlphaSpeed:")
        .object("contents", "setContents:")
        .rect("contentsRect", "setContentsRect:", {0, 0, 1, 1})
        .real("contentsScale", "setContentsScale:", 1)
        .object("minificationFilter", "setMinificationFilter:", true, [sp] { return sp->cf.constant_string("linear"); })
        .object("magnificationFilter", "setMagnificationFilter:", true, [sp] { return sp->cf.constant_string("linear"); })
        .single("minificationFilterBias", "setMinificationFilterBias:")
        .object("emitterCells", "setEmitterCells:", true)
        .object("style", "setStyle:", true)
        .real("beginTime", "setBeginTime:")
        .real("timeOffset", "setTimeOffset:")
        .single("repeatCount", "setRepeatCount:")
        .real("repeatDuration", "setRepeatDuration:")
        .real("duration", "setDuration:")
        .single("speed", "setSpeed:", 1.0f)
        .boolean("autoreverses", "setAutoreverses:")
        .object("fillMode", "setFillMode:", true, [sp] { return sp->cf.constant_string("removed"); });
    support.method(cell, "emitterCell", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(sp->send_id(sp->send_id(s.x[0], "alloc"), "init"), "autorelease");
    }, "@@:", true);
    support.method(cell, "defaultValueForKey:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:@", true);
    support.method(cell, "shouldArchiveValueForKey:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@");

    const auto keyframe = support.declare_class("CAKeyframeAnimation", "CAPropertyAnimation", quartzcore_library);
    Props(support, keyframe)
        .object("values", "setValues:", true)
        .object("keyTimes", "setKeyTimes:", true)
        .object("timingFunctions", "setTimingFunctions:", true)
        .object("calculationMode", "setCalculationMode:", true, [sp] { return sp->cf.constant_string("linear"); })
        .object("rotationMode", "setRotationMode:", true)
        .object("tensionValues", "setTensionValues:", true)
        .object("continuityValues", "setContinuityValues:", true)
        .object("biasValues", "setBiasValues:", true);
    // CGPathRef: retained with CFRetain semantics through the object store.
    support.method(keyframe, "path", [sp](CpuState& s, GuestMemory&) { s.x[0] = bag_object(*sp, s.x[0], "path"); }, "^v@:");
    support.method(keyframe, "setPath:", [sp](CpuState& s, GuestMemory&) { bag_set_object(*sp, s.x[0], "path", s.x[2]); }, "v@:^v");

    const auto group = support.declare_class("CAAnimationGroup", "CAAnimation", quartzcore_library);
    Props(support, group).object("animations", "setAnimations:", true);

    const auto transition = support.declare_class("CATransition", "CAAnimation", quartzcore_library);
    Props(support, transition)
        .object("type", "setType:", true, [sp] { return sp->cf.constant_string("fade"); })
        .object("subtype", "setSubtype:", true)
        .single("startProgress", "setStartProgress:", 0.0f)
        .single("endProgress", "setEndProgress:", 1.0f)
        .object("filter", "setFilter:");

    // The kCA* names of the animation and layer API (their values are the
    // strings Core Animation uses).
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_kCAFillModeForwards", "forwards"}, {"_kCAFillModeBackwards", "backwards"}, {"_kCAFillModeBoth", "both"},
             {"_kCAFillModeRemoved", "removed"}, {"_kCAAnimationLinear", "linear"}, {"_kCAAnimationDiscrete", "discrete"},
             {"_kCAAnimationPaced", "paced"}, {"_kCAAnimationCubic", "cubic"}, {"_kCAAnimationCubicPaced", "cubicPaced"},
             {"_kCAAnimationRotateAuto", "auto"}, {"_kCAAnimationRotateAutoReverse", "autoReverse"},
             {"_kCATransitionFade", "fade"}, {"_kCATransitionMoveIn", "moveIn"}, {"_kCATransitionPush", "push"},
             {"_kCATransitionReveal", "reveal"}, {"_kCATransitionFromRight", "fromRight"}, {"_kCATransitionFromLeft", "fromLeft"},
             {"_kCATransitionFromTop", "fromTop"}, {"_kCATransitionFromBottom", "fromBottom"},
             {"_kCAGravityCenter", "center"}, {"_kCAGravityTop", "top"}, {"_kCAGravityBottom", "bottom"},
             {"_kCAGravityLeft", "left"}, {"_kCAGravityRight", "right"}, {"_kCAGravityTopLeft", "topLeft"},
             {"_kCAGravityTopRight", "topRight"}, {"_kCAGravityBottomLeft", "bottomLeft"},
             {"_kCAGravityBottomRight", "bottomRight"}, {"_kCAGravityResize", "resize"},
             {"_kCAGravityResizeAspect", "resizeAspect"}, {"_kCAGravityResizeAspectFill", "resizeAspectFill"},
             {"_kCACornerCurveCircular", "circular"}, {"_kCACornerCurveContinuous", "continuous"},
             {"_kCAGradientLayerAxial", "axial"}, {"_kCAGradientLayerRadial", "radial"}, {"_kCAGradientLayerConic", "conic"},
             {"_kCAFilterLinear", "linear"}, {"_kCAFilterNearest", "nearest"}, {"_kCAFilterTrilinear", "trilinear"},
             {"_kCAOnOrderIn", "onOrderIn"}, {"_kCAOnOrderOut", "onOrderOut"}, {"_kCATransition", "transition"},
             {"_kCAAlignmentNatural", "natural"}, {"_kCAAlignmentLeft", "left"}, {"_kCAAlignmentRight", "right"},
             {"_kCAAlignmentCenter", "center"}, {"_kCAAlignmentJustified", "justified"},
             {"_kCATruncationNone", "none"}, {"_kCATruncationStart", "start"}, {"_kCATruncationEnd", "end"},
             {"_kCATruncationMiddle", "middle"}, {"_kCAContentsFormatRGBA8Uint", "RGBA8"},
             {"_kCAContentsFormatRGBA16Float", "RGBAh"}, {"_kCAContentsFormatGray8Uint", "Gray8"},
             {"_kCAValueFunctionRotateX", "rotateX"}, {"_kCAValueFunctionRotateY", "rotateY"},
             {"_kCAValueFunctionRotateZ", "rotateZ"}, {"_kCAValueFunctionScale", "scale"},
             {"_kCAValueFunctionScaleX", "scaleX"}, {"_kCAValueFunctionScaleY", "scaleY"},
             {"_kCAValueFunctionScaleZ", "scaleZ"}, {"_kCAValueFunctionTranslate", "translate"},
             {"_kCAValueFunctionTranslateX", "translateX"}, {"_kCAValueFunctionTranslateY", "translateY"},
             {"_kCAValueFunctionTranslateZ", "translateZ"},
             // CAEmitterLayer's shapes, modes and render modes (the same naming; unverified).
             {"_kCAEmitterLayerPoint", "point"}, {"_kCAEmitterLayerLine", "line"}, {"_kCAEmitterLayerRectangle", "rectangle"},
             {"_kCAEmitterLayerCuboid", "cuboid"}, {"_kCAEmitterLayerCircle", "circle"}, {"_kCAEmitterLayerSphere", "sphere"},
             {"_kCAEmitterLayerPoints", "points"}, {"_kCAEmitterLayerOutline", "outline"}, {"_kCAEmitterLayerSurface", "surface"},
             {"_kCAEmitterLayerVolume", "volume"}, {"_kCAEmitterLayerUnordered", "unordered"},
             {"_kCAEmitterLayerOldestFirst", "oldestFirst"}, {"_kCAEmitterLayerOldestLast", "oldestLast"},
             {"_kCAEmitterLayerBackToFront", "backToFront"}, {"_kCAEmitterLayerAdditive", "additive"},
             // A CAContext option key (Godot binds it; CAContext has no public
             // documentation, so the value follows the naming above: unverified).
             {"_kCAContextCIFilterBehavior", "ciFilterBehavior"}})
        if (!support.process.resolve(quartzcore_library, symbol)) support.string_constant(quartzcore_library, symbol, value);
}

} // namespace alchemy::gui::completion_kit::graphics
