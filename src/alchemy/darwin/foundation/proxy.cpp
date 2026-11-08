#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <string>

// NSProxy (Foundation): the second root class, for objects that stand in for
// another object and forward the messages they receive.
//
// NSProxy has no superclass and its metaclass is a root metaclass, so classes
// a binary defines over it (_OBJC_CLASS_$_NSProxy, _OBJC_METACLASS_$_NSProxy)
// load like classes over NSObject. Allocation and memory management are
// NSObject's own implementations (+alloc, +allocWithZone:, -retain, -release,
// -autorelease, -retainCount, -dealloc, -zone), as are +class,
// +respondsToSelector:, -description and -debugDescription; -isProxy answers
// YES. NSProxy's -methodSignatureForSelector: and -forwardInvocation: raise
// NSInvalidArgumentException, as documented: a subclass overrides both, and
// every other message (-class, -isKindOfClass:, -respondsToSelector: and the
// rest) reaches them through Foundation's forward handler (invocation.cpp).
namespace alchemy::gui::completion_kit {

void install_proxy(Support& support) {
    using namespace rt;
    if (support.objc.find_class("NSProxy")) return;
    const auto root = class_named(support, "NSObject");
    const auto proxy = support.objc.register_native_class("NSProxy", 0, 8);   // published under libobjc and Foundation
    const auto object_class = [&](Address cls) { return support.call(libobjc, "_object_getClass", {cls}); };
    // NSObject's method `name` (a class method when `class_method`) added to NSProxy;
    // description and debugDescription only when NSObject has them by now.
    const auto share = [&](std::string_view name, bool class_method, bool required = true) {
        const auto from = class_method ? object_class(root) : root, to = class_method ? object_class(proxy) : proxy;
        const auto selector = support.objc.selector(name);
        const auto method = support.call(libobjc, "_class_getInstanceMethod", {from, selector});
        if (!method && !required) return;
        if (!method) fail("NSProxy: NSObject has no " + std::string(class_method ? "+" : "-") + std::string(name));
        static_cast<void>(support.call(libobjc, "_class_addMethod", {to, selector,
            support.call(libobjc, "_method_getImplementation", {method}), support.call(libobjc, "_method_getTypeEncoding", {method})}));
    };
    for (const auto* name : {"alloc", "allocWithZone:", "class", "respondsToSelector:", "initialize", "resolveInstanceMethod:", "resolveClassMethod:"})
        share(name, true);
    // (-forwardingTargetForSelector: answers nil, so forwarding goes on to -forwardInvocation:.)
    for (const auto* name : {"retain", "release", "autorelease", "retainCount", "dealloc", "zone", "forwardingTargetForSelector:"})
        share(name, false);
    for (const auto* name : {"description", "debugDescription"}) share(name, false, false);
    support.method(proxy, "isProxy", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    support.method(proxy, "methodSignatureForSelector:", [](CpuState&, GuestMemory&) {
        fail("NSInvalidArgumentException: -[NSProxy methodSignatureForSelector:] called; a subclass must override it");
    }, "@@::");
    support.method(proxy, "forwardInvocation:", [](CpuState&, GuestMemory&) {
        fail("NSInvalidArgumentException: -[NSProxy forwardInvocation:] called; a subclass must override it");
    }, "v@:@");
}

} // namespace alchemy::gui::completion_kit
