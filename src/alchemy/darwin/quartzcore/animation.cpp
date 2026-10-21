#include "alchemy/darwin/kit/completion/graphics/providers.hpp"
#include "alchemy/darwin/quartzcore/graphics_support.hpp"

#include <cmath>
#include <limits>

// Core Animation's animation objects: CAAnimation, CAPropertyAnimation,
// CABasicAnimation, CAKeyframeAnimation, CAAnimationGroup and CATransition,
// with the CAMediaTiming properties and Apple's defaults (duration 0 meaning
// the transaction's 0.25 s, speed 1, repeatCount 0, fillMode "removed",
// removedOnCompletion YES, a retained delegate). Unknown keys are stored
// (CAAnimation is key-value coding compliant for any key), and the objects
// are copied when added to a layer (-[CALayer addAnimation:forKey:] in
// src/alchemy/darwin/quartzcore/quartzcore.cpp, which runs their timing and sends
// the delegate's animationDidStart: / animationDidStop:finished:). There is
// no render tree, so the animated values themselves are never interpolated:
// the layer shows its model values.
namespace alchemy::gui::completion_kit::graphics {

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
             {"_kCAValueFunctionTranslateZ", "translateZ"}})
        if (!support.process.resolve(quartzcore_library, symbol)) support.string_constant(quartzcore_library, symbol, value);
}

} // namespace alchemy::gui::completion_kit::graphics
