// NSProxy (proxy.cpp): a root class with a root metaclass, and a subclass that
// forwards to an NSString through -methodSignatureForSelector: and -forwardInvocation:.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view foundation = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
constexpr std::string_view libobjc = "/usr/lib/libobjc.A.dylib";

Address target{};   // what the test proxy stands in for
alchemy::ObjcRuntime* runtime{};

const Case foundation_proxy_case("foundation_proxy", [](Fixture& f) {
    const auto proxy = f.objc.find_class("NSProxy");
    require(proxy != 0 && f.symbol(foundation, "_OBJC_CLASS_$_NSProxy") == proxy && f.symbol(libobjc, "_OBJC_CLASS_$_NSProxy") == proxy,
            "_OBJC_CLASS_$_NSProxy");
    const auto meta = f.call(libobjc, "_object_getClass", {proxy});
    require(f.symbol(foundation, "_OBJC_METACLASS_$_NSProxy") == meta, "_OBJC_METACLASS_$_NSProxy");
    require(f.call(libobjc, "_class_getSuperclass", {proxy}) == 0 && f.call(libobjc, "_object_getClass", {meta}) == meta &&
                f.call(libobjc, "_class_getSuperclass", {meta}) == proxy, "a root class with a root metaclass");

    // A subclass standing in for a string.
    runtime = &f.objc;
    target = f.cf.string("hello");
    const auto forwarding = f.objc.register_native_class("AlchemyTestProxy", proxy, 8);
    static_cast<void>(f.objc.add_native_method(forwarding, "methodSignatureForSelector:", [](transmute::CpuState& s, transmute::GuestMemory&) {
        alchemy::CallRegs query;
        query.x[2] = s.x[2];
        s.x[0] = runtime->send(target, runtime->selector("methodSignatureForSelector:"), query).x0;
    }, false, "@@::"));
    static_cast<void>(f.objc.add_native_method(forwarding, "forwardInvocation:", [](transmute::CpuState& s, transmute::GuestMemory&) {
        alchemy::CallRegs invoke;
        invoke.x[2] = target;
        static_cast<void>(runtime->send(s.x[2], runtime->selector("invokeWithTarget:"), invoke));
    }, false, "v@:@"));

    const auto object = f.send(forwarding, "alloc");
    require(object != 0 && f.call(libobjc, "_object_getClass", {object}) == forwarding, "+alloc");
    require((f.send(object, "isProxy") & 0xff) == 1, "-isProxy");
    require(f.send(object, "length") == 5, "a forwarded -length");
    require((f.send(object, "isKindOfClass:", {f.objc.find_class("NSString")}) & 0xff) == 1, "a forwarded -isKindOfClass:");
    require(f.send(object, "retain") == object && f.send(object, "retainCount") == 2, "-retain");
    f.send(object, "release");
    require(f.send(object, "retainCount") == 1, "-release");
    f.send(object, "release");
});

} // namespace
