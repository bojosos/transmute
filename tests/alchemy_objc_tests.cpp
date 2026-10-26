#include "alchemy/objc_runtime.hpp"
#include "transmute/jit.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
using Address = std::uint64_t;
void require(bool condition, std::string_view message) { if (!condition) throw std::runtime_error(std::string(message)); }
struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](auto, auto bytes) -> alchemy::OutputResult { return {bytes.size(), 0}; }};
    alchemy::ObjcRuntime runtime{process};
    Address call(std::string_view name, std::initializer_list<Address> args) {
        const auto symbol = process.resolve(alchemy::objc_library, name);
        require(symbol.has_value(), "missing Objective-C export");
        return engine.invoke_callback(symbol->address, std::span(args.begin(), args.size()));
    }
    Address string(std::string_view value) { const auto result = process.allocate_guest(value.size() + 1);
        engine.write_bytes(result, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}); return result; }
};
void test_dispatch(bool jit) {
    Fixture f; f.engine.set_jit_enabled(jit); f.engine.set_jit_profiling(jit);
    const auto base = f.runtime.register_native_class("TestBase", f.runtime.find_class("NSObject"));
    const auto derived = f.runtime.register_native_class("TestDerived", base);
    static_cast<void>(f.runtime.add_native_method(base, "answer:", [](auto& s, auto&) { s.x[0] = s.x[2] + 7; }));
    static_cast<void>(f.runtime.add_native_method(derived, "answer:", [](auto& s, auto&) { s.x[0] = s.x[2] + 20; }));
    const auto object = f.runtime.allocate(derived), sel = f.runtime.selector("answer:");
    require(f.call("_objc_msgSend", {object, sel, 7}) == 27, "overridden dispatch failed");
    const auto super = f.process.allocate_guest(16);
    f.engine.write64(super, object); f.engine.write64(super + 8, derived);
    require(f.call("_objc_msgSendSuper2", {super, sel, 7}) == 14, "super dispatch failed");
    require(f.call("_objc_msgSend", {0, sel, 7}) == 0, "nil integer dispatch failed");
    const auto vector_sel = f.runtime.selector("preserve:");
    const auto output = f.process.allocate_guest(32);
    const auto sp = (f.engine.state().sp - 256) & ~15ULL;
    f.engine.write64(sp, 89);
    f.engine.set_initial_register(8, output);
    for (unsigned i = 0; i < 8; ++i) f.engine.set_initial_vector(i, {0x100 + i, 0x200 + i});
    static_cast<void>(f.runtime.add_native_method(derived, "preserve:", [output](auto& s, auto& m) {
        require(s.x[2] == 27 && s.x[7] == 42 && s.x[8] == output, "dispatch corrupted integer/indirect registers");
        for (unsigned i = 0; i < 8; ++i) require(s.v[i][0] == 0x100 + i && s.v[i][1] == 0x200 + i, "dispatch corrupted SIMD registers");
        require(m.read64(s.sp) == 89, "dispatch moved stack arguments");
        m.write64(s.x[8], 1234); s.x[0] = 42;
    }));
    require(f.call("_objc_msgSend", {object, vector_sel, 27, 0, 0, 0, 0, 42}) == 42 && f.engine.read64(output) == 1234,
        "dispatch did not preserve return ABI");
    f.call("_objc_release", {object});
    if (jit) require(f.engine.jit_stats().native_guest_instructions != 0, "Objective-C dispatcher did not execute translated code");
}
void test_ownership_and_pools() {
    Fixture f; unsigned destroyed{};
    const auto cls = f.runtime.register_native_class("Ownership", f.runtime.find_class("NSObject"));
    static_cast<void>(f.runtime.add_native_method(cls, "dealloc", [&destroyed](auto&, auto&) { ++destroyed; }));
    const auto first = f.runtime.allocate(cls), second = f.runtime.allocate(cls);
    const auto slot = f.process.allocate_guest(8);
    f.call("_objc_storeStrong", {slot, first}); f.call("_objc_release", {first});
    f.call("_objc_storeStrong", {slot, second}); require(destroyed == 1, "strong replacement did not release old object");
    f.call("_objc_release", {second}); f.call("_objc_storeStrong", {slot, 0}); require(destroyed == 2, "strong clear leaked object");
    const auto outer = f.call("_objc_autoreleasePoolPush", {});
    f.call("_objc_autorelease", {f.runtime.allocate(cls)});
    const auto inner = f.call("_objc_autoreleasePoolPush", {});
    f.call("_objc_autorelease", {f.runtime.allocate(cls)});
    f.call("_objc_autoreleasePoolPop", {inner}); require(destroyed == 3, "nested pool did not drain independently");
    f.call("_objc_autoreleasePoolPop", {outer}); require(destroyed == 4, "outer pool did not drain");
    bool rejected{}; try { f.call("_objc_autoreleasePoolPop", {outer}); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "stale autorelease pool token accepted");
    f.call("_objc_autorelease", {f.runtime.allocate(cls)});
    f.runtime.cleanup_thread(*f.process.thread(f.engine)); require(destroyed == 5, "thread cleanup leaked unpooled object");
    require(f.process.objects().live_count() == 0, "owned objects survived cleanup");
    const auto property_class = f.runtime.register_native_class("PropertyOwner", f.runtime.find_class("NSObject"), 24);
    const auto owner = f.runtime.allocate(property_class), value = f.runtime.allocate(cls);
    f.call("_objc_setProperty_atomic", {owner, 0, value, 8}); f.call("_objc_release", {value});
    require(f.call("_objc_getProperty", {owner, 0, 8, 0}) == value, "property helper argument convention failed");
    f.call("_objc_setProperty_nonatomic", {owner, 0, 0, 8}); require(destroyed == 6, "property replacement leaked retained object");
    f.call("_objc_setProperty_atomic", {owner, 0, cls, 8});
    require(f.call("_objc_getProperty", {owner, 0, 8, 1}) == cls, "atomic getter rejected immortal class property");
    f.call("_objc_setProperty_nonatomic", {owner, 0, 0, 8});
    f.call("_objc_release", {owner});
    unsigned reentrant_destroyed{};
    const auto cleanup_class = f.runtime.register_native_class("CleanupReentrant", f.runtime.find_class("NSObject"));
    static_cast<void>(f.runtime.add_native_method(cleanup_class, "dealloc", [&](auto&, auto&) {
        ++reentrant_destroyed;
        if (reentrant_destroyed == 1) f.call("_objc_autorelease", {f.runtime.allocate(cleanup_class)});
    }, false, "v@:"));
    f.call("_objc_autorelease", {f.runtime.allocate(cleanup_class)});
    f.runtime.cleanup_thread(*f.process.thread(f.engine));
    require(reentrant_destroyed == 2 && f.process.objects().live_count() == 0, "cleanup leaked destructor-created autoreleased object");
}
void test_concurrent_initialization() {
    Fixture f; std::atomic<unsigned> count{};
    const auto cls = f.runtime.register_native_class("Initialized", f.runtime.find_class("NSObject"));
    static_cast<void>(f.runtime.add_native_method(cls, "initialize", [&count](auto&, auto&) {
        ++count; std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }, true));
    static_cast<void>(f.runtime.add_native_method(cls, "answer", [](auto& s, auto&) { s.x[0] = 27; }, true));
    const auto send = f.process.resolve(alchemy::objc_library, "_objc_msgSend")->address;
    const auto sel = f.runtime.selector("answer");
    auto first = f.engine.fork_thread(send, 0), second = f.engine.fork_thread(send, 0);
    std::array<Address, 2> results{};
    std::thread a([&] { const std::array<Address, 2> args{cls, sel}; results[0] = first->invoke_callback(send, args); });
    std::thread b([&] { const std::array<Address, 2> args{cls, sel}; results[1] = second->invoke_callback(send, args); });
    a.join(); b.join();
    require(results[0] == 27 && results[1] == 27 && count == 1, "concurrent class initialization ran twice");
    f.process.release_thread(*first); f.process.release_thread(*second);
}
void test_dynamic_registration() {
    Fixture f; const auto name = f.string("Dynamic"), root = f.runtime.find_class("NSObject");
    require(f.call("_class_getInstanceSize", {f.call("_object_getClass", {root})}) == 40,
        "native metaclass size differs from guest class record");
    const auto cls = f.call("_objc_allocateClassPair", {root, name, 0});
    require(cls && f.call("_objc_getClass", {name}) == 0, "unregistered class was visible");
    const auto ivar_name = f.string("value"), types = f.string("q");
    require(f.call("_class_addIvar", {cls, ivar_name, 8, 3, types}) == 1, "dynamic ivar registration failed");
    const auto sel = f.runtime.selector("answer"), encoding = f.string("q@:");
    const auto first = f.engine.register_guest_call([](auto& s, auto&) { s.x[0] = 27; });
    const auto second = f.engine.register_guest_call([](auto& s, auto&) { s.x[0] = 42; });
    require(f.call("_class_addMethod", {cls, sel, first, encoding}) == 1 &&
        f.call("_class_addMethod", {cls, sel, second, encoding}) == 0, "duplicate dynamic method accepted");
    f.call("_objc_registerClassPair", {cls}); require(f.call("_objc_getClass", {name}) == cls, "registered class remained hidden");
    require(f.call("_objc_opt_class", {cls}) == cls, "optimized class query returned a metaclass for a class receiver");
    const auto root_class_method = f.call("_class_getClassMethod", {root, f.runtime.selector("class")});
    require(static_cast<const transmute::GuestMemory&>(f.engine).read8(f.call("_method_getTypeEncoding", {root_class_method})) == '#', "native class method has incorrect type encoding");
    const auto ivar = f.call("_class_getInstanceVariable", {cls, ivar_name});
    require(ivar && f.call("_ivar_getOffset", {ivar}) == 8 && f.call("_class_getInstanceSize", {cls}) == 16, "dynamic ivar layout failed");
    const auto object = f.call("_class_createInstance", {cls, 0});
    require(f.call("_objc_msgSend", {object, sel}) == 27, "dynamic method did not execute");
    const auto method = f.call("_class_getInstanceMethod", {cls, sel});
    require(f.call("_class_replaceMethod", {cls, sel, second, 0}) == first &&
        f.call("_objc_msgSend", {object, sel}) == 42, "method replacement failed");
    require(f.call("_class_getInstanceMethod", {cls, sel}) == method && f.call("_method_getTypeEncoding", {method}) != 0,
        "replacement changed existing Method identity or lost its type encoding");
    require(f.call("_method_getImplementation", {method}) == second &&
        f.call("_method_setImplementation", {method, first}) == second && f.call("_objc_msgSend", {object, sel}) == 27,
        "method inspection/replacement failed");
    const auto inherited = f.runtime.selector("hash");
    const auto inherited_method = f.call("_class_getInstanceMethod", {cls, inherited});
    require(f.call("_class_replaceMethod", {cls, inherited, second, encoding}) == 0 &&
        f.call("_class_getInstanceMethod", {cls, inherited}) != inherited_method &&
        f.call("_objc_msgSend", {object, inherited}) == 42, "inherited method replacement did not add an independent own method");
    // class_copyMethodList: the class's own methods only (answer and the hash override), not NSObject's.
    static_cast<void>(f.process.install_function("_malloc", [&f](auto& s, auto&) { s.x[0] = f.process.allocate_guest(s.x[0]); }));
    const auto method_count = f.process.allocate_guest(4);
    const auto methods = f.call("_class_copyMethodList", {cls, method_count});
    require(f.engine.read32(method_count) == 2 && f.engine.read64(methods + 16) == 0, "class_copyMethodList count or terminator");
    std::set<Address> listed{f.engine.read64(methods), f.engine.read64(methods + 8)};
    require(listed.contains(method) && listed.contains(f.call("_class_getInstanceMethod", {cls, inherited})), "class_copyMethodList entries");
    // _objc_msgForward is an IMP whose call stops with the selector.
    bool forwarded{}; try { f.call("__objc_msgForward", {object, sel}); } catch (const std::runtime_error& error) {
        forwarded = std::string_view(error.what()).find("_objc_msgForward) of answer") != std::string_view::npos;
    }
    require(forwarded, "_objc_msgForward did not stop with the selector");
    f.call("_objc_release", {object});
    // Weak references: loads retain, deallocation clears every referrer.
    const auto weak = f.process.allocate_guest(16), target = f.call("_class_createInstance", {cls, 0});
    require(f.call("_objc_initWeak", {weak, target}) == target && f.engine.read64(weak) == target, "objc_initWeak failed");
    f.call("_objc_copyWeak", {weak + 8, weak});
    const auto loaded = f.call("_objc_loadWeakRetained", {weak + 8});
    require(loaded == target && f.process.objects().require(target)->references == 2, "objc_loadWeakRetained did not retain");
    f.call("_objc_release", {loaded}); f.call("_objc_release", {target});
    require(f.engine.read64(weak) == 0 && f.engine.read64(weak + 8) == 0 && f.call("_objc_loadWeakRetained", {weak}) == 0,
        "deallocation did not clear weak references");
    f.call("_objc_destroyWeak", {weak}); f.call("_objc_destroyWeak", {weak + 8});
    bool unsupported{}; try { f.call("_objc_exception_throw", {0}); } catch (const std::runtime_error& error) {
        unsupported = std::string_view(error.what()).find("Objective-C exceptions need libc++abi") != std::string_view::npos;
    }
    require(unsupported, "an Objective-C throw without the LLVM runtime did not produce a specific diagnostic");
}
void test_protected_transaction_rollback() {
    Fixture f; constexpr Address data = 0x300000000ULL;
    f.engine.map_memory(data, 4096);
    const auto cls = f.runtime.register_native_class("CategoryTarget", f.runtime.find_class("NSObject"));
    static_cast<void>(f.runtime.add_native_method(cls, "answer", [](auto& s, auto&) { s.x[0] = 27; }));
    const auto bytes = [&](Address offset, std::string_view value) {
        f.engine.write_bytes(data + offset, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    };
    bytes(256, "Category"); bytes(288, "answer"); bytes(320, "q@:");
    f.engine.write64(data, data + 64); f.engine.write64(data + 8, data + 288);
    f.engine.write64(data + 64, data + 256); f.engine.write64(data + 72, cls); f.engine.write64(data + 80, data + 128);
    f.engine.write32(data + 128, 24); f.engine.write32(data + 132, 1);
    f.engine.write64(data + 136, data + 288); f.engine.write64(data + 144, data + 320); f.engine.write64(data + 152, data + 2048);
    f.engine.write32(data + 2048, 0xd2800540); f.engine.write32(data + 2052, 0xd65f03c0);
    alchemy::ImageRuntimeInfo image; image.module_id = 17; image.begin = data; image.end = data + 4096;
    image.sections = {{"__DATA", "__objc_catlist", data, 8, 0}, {"__DATA", "__objc_selrefs", data + 8, 8, 0}};
    image.code_ranges = {{data + 2048, data + 2056}};
    f.runtime.prepare_images(std::span(&image, 1)); const auto sel = f.runtime.selector("answer");
    require(f.engine.read64(data + 8) == sel, "selector reference was not interned");
    f.engine.set_memory_access(data, true, false, true);
    const auto object = f.runtime.allocate(cls), category_method = f.call("_class_getInstanceMethod", {cls, sel});
    require(f.call("_objc_msgSend", {object, sel}) == 42, "category method was not registered");
    const std::array<Address, 1> ids{17}; f.runtime.rollback_images(ids);
    require(f.engine.read64(data + 8) == data + 288 && !f.engine.query_region(data)->writable, "rollback failed to restore protected selector and permissions");
    require(f.call("_objc_msgSend", {object, sel}) == 27, "rollback failed to restore category target method");
    bool invalid{}; try { f.call("_method_getImplementation", {category_method}); } catch (const std::runtime_error&) { invalid = true; }
    require(invalid, "rolled-back category method remained registered");
    f.call("_objc_release", {object});
}
void test_protocols() {
    Fixture f; constexpr Address first = 0x300000000ULL, second = first + 4096;
    f.engine.map_memory(first, 8192);
    const auto root = f.runtime.find_class("NSObject");
    const auto adopter = f.runtime.register_native_class("Adopter", root), child = f.runtime.register_native_class("AdopterChild", adopter);
    const auto bytes = [&](Address at, std::string_view value) {
        f.engine.write_bytes(at, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    };
    const auto protocol = [&](Address at, Address name, Address parents) {
        f.engine.write64(at + 8, name); f.engine.write64(at + 16, parents); f.engine.write32(at + 64, 96);
    };
    bytes(first + 512, "Shared"); bytes(first + 528, "answer"); bytes(first + 544, "q16@0:8");
    f.engine.write64(first, first + 64); protocol(first + 64, first + 512, 0);
    f.engine.write64(first + 64 + 24, first + 256); f.engine.write32(first + 256, 24); f.engine.write32(first + 260, 1);
    f.engine.write64(first + 264, first + 528); f.engine.write64(first + 272, first + 544);
    bytes(second + 512, "Shared"); bytes(second + 528, "Child"); bytes(second + 544, "Adopt");
    f.engine.write64(second, second + 64); f.engine.write64(second + 8, second + 192);
    f.engine.write64(second + 16, second + 64); f.engine.write64(second + 24, second + 320);
    protocol(second + 64, second + 512, 0);
    protocol(second + 192, second + 528, second + 400); f.engine.write64(second + 400, 1); f.engine.write64(second + 408, second + 64);
    f.engine.write64(second + 320, second + 544); f.engine.write64(second + 328, adopter); f.engine.write64(second + 352, second + 432);
    f.engine.write64(second + 432, 1); f.engine.write64(second + 440, second + 192);
    std::array<alchemy::ImageRuntimeInfo, 2> images;
    images[0].module_id = 31; images[0].begin = first; images[0].end = first + 4096;
    images[0].sections = {{"__DATA", "__objc_protolist", first, 8, 0}};
    images[1].module_id = 32; images[1].begin = second; images[1].end = second + 4096;
    images[1].sections = {{"__DATA", "__objc_protolist", second, 16, 0}, {"__DATA", "__objc_protorefs", second + 16, 8, 0},
        {"__DATA", "__objc_catlist", second + 24, 8, 0}};
    f.runtime.prepare_images(std::span(images).first(1));
    // Later images see earlier ones through the published image list.
    f.process.publish_image(images[0]);
    f.runtime.prepare_images(std::span(images).subspan(1));
    const auto shared = first + 64, derived = second + 192, protocol_class = f.call("_objc_getClass", {f.string("Protocol")});
    require(f.call("_objc_getProtocol", {f.string("Shared")}) == shared && f.call("_objc_getProtocol", {f.string("Child")}) == derived &&
        f.call("_objc_getProtocol", {f.string("Missing")}) == 0, "protocol lookup is not canonical by name");
    require(f.engine.read64(second + 16) == shared && f.engine.read64(second + 408) == shared && f.engine.read64(second + 440) == derived,
        "protocol references were not rewritten to the canonical object");
    require(protocol_class && f.engine.read64(shared) == protocol_class && f.engine.read64(derived) == protocol_class &&
        f.call("_object_getClass", {shared}) == protocol_class, "protocol objects lack the Protocol class");
    require(f.call("_protocol_conformsToProtocol", {derived, shared}) == 1 && f.call("_protocol_conformsToProtocol", {shared, derived}) == 0 &&
        f.call("_protocol_isEqual", {second + 64, shared}) == 1, "protocol inheritance or duplicate identity failed");
    require(f.call("_class_conformsToProtocol", {adopter, second + 64}) == 1 && f.call("_class_conformsToProtocol", {child, shared}) == 0,
        "category protocol adoption or own-list semantics failed");
    const auto conforms = f.runtime.selector("conformsToProtocol:");
    const auto object = f.runtime.allocate(child);
    require(f.call("_objc_msgSend", {child, conforms, derived}) == 1 && f.call("_objc_msgSend", {object, conforms, shared}) == 1 &&
        f.call("_objc_msgSend", {root, conforms, shared}) == 0, "conformsToProtocol: did not walk superclasses");
    const auto name = f.call("_protocol_getName", {derived});
    const auto name_bytes = static_cast<const transmute::GuestMemory&>(f.engine).read_string(name, 1, 16);
    require(std::string(name_bytes.begin(), name_bytes.end()) == "Child" &&
        f.call("_objc_msgSend", {derived, f.runtime.selector("name")}) == name, "protocol name mismatch");
    require(f.call("_objc_retain", {derived}) == derived && f.call("_objc_msgSend", {derived, f.runtime.selector("conformsTo:"), shared}) == 1,
        "protocol objects are not immortal message receivers");
    f.call("_objc_release", {derived});
    require(f.call("_protocol_getMethodDescription", {derived, f.runtime.selector("answer"), 1, 1}) == f.runtime.selector("answer") &&
        f.call("_protocol_getMethodDescription", {derived, f.runtime.selector("answer"), 0, 1}) == 0,
        "inherited protocol method description lookup failed");
    const auto count = f.process.allocate_guest(4);
    bool unavailable{}; try { f.call("_class_copyProtocolList", {adopter, count}); } catch (const std::runtime_error& error) {
        unavailable = std::string_view(error.what()).find("libSystem allocator") != std::string_view::npos; }
    require(unavailable, "protocol list copy without an allocator was not explicit");
    static_cast<void>(f.process.install_function("_malloc", [&f](auto& s, auto&) { s.x[0] = f.process.allocate_guest(s.x[0]); }));
    const auto list = f.call("_class_copyProtocolList", {adopter, count});
    require(f.engine.read32(count) == 1 && f.engine.read64(list) == derived && f.engine.read64(list + 8) == 0 &&
        f.call("_class_copyProtocolList", {child, count}) == 0 && f.engine.read32(count) == 0, "class protocol list copy failed");
    require(f.call("_class_addProtocol", {child, shared}) == 1 && f.call("_class_addProtocol", {child, shared}) == 0 &&
        f.call("_class_conformsToProtocol", {child, shared}) == 1, "dynamic protocol adoption failed");
    const std::array<Address, 1> ids{32}; f.runtime.rollback_images(ids);
    require(f.call("_objc_getProtocol", {f.string("Child")}) == 0 && f.call("_class_conformsToProtocol", {adopter, shared}) == 0 &&
        f.engine.read64(second + 16) == second + 64 && f.engine.read64(second + 440) == second + 192 && f.engine.read64(derived) == 0,
        "protocol rollback did not restore metadata");
    require(f.call("_objc_getProtocol", {f.string("Shared")}) == shared, "rollback removed an earlier image's protocol");
    bool mutated{}; try { f.call("_objc_enumerationMutation", {object}); } catch (const std::runtime_error& error) {
        mutated = std::string_view(error.what()).find("AdopterChild was mutated while being enumerated") != std::string_view::npos; }
    require(mutated, "enumeration mutation was not reported");
    Address seen{}; const auto handler = f.engine.register_guest_call([&seen](auto& s, auto&) { seen = s.x[0]; });
    f.call("_objc_setEnumerationMutationHandler", {handler}); f.call("_objc_enumerationMutation", {object});
    require(seen == object, "enumeration mutation handler was not called");
    f.call("_objc_release", {object});
}
// Non-fragile ivars: a guest class compiled against an 8-byte superclass whose
// real instance size is 64 has its ivar slid past the superclass.
void test_ivar_sliding() {
    Fixture f; constexpr Address data = 0x300000000ULL;
    f.engine.map_memory(data, 4096);
    const auto root = f.runtime.find_class("NSObject");
    const auto base = f.runtime.register_native_class("SlideBase", root, 64);
    const auto base_meta = f.engine.read64(base), root_meta = f.engine.read64(base_meta);
    const auto bytes = [&](Address at, std::string_view value) {
        f.engine.write_bytes(at, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size() + 1});
    };
    bytes(data + 1024, "SlideChild"); bytes(data + 1056, "value"); bytes(data + 1072, "q");
    const Address cls = data + 64, meta = data + 128, ro = data + 256, meta_ro = data + 384, ivars = data + 512, offset = data + 600;
    f.engine.write64(data, cls);                                                   // __objc_classlist
    f.engine.write64(cls, meta); f.engine.write64(cls + 8, base); f.engine.write64(cls + 32, ro);
    f.engine.write64(meta, root_meta); f.engine.write64(meta + 8, base_meta); f.engine.write64(meta + 32, meta_ro);
    f.engine.write32(ro + 4, 8); f.engine.write32(ro + 8, 16); f.engine.write64(ro + 24, data + 1024); f.engine.write64(ro + 48, ivars);
    f.engine.write32(meta_ro, 1); f.engine.write32(meta_ro + 4, 40); f.engine.write32(meta_ro + 8, 40); f.engine.write64(meta_ro + 24, data + 1024);
    f.engine.write32(ivars, 32); f.engine.write32(ivars + 4, 1);
    f.engine.write64(ivars + 8, offset); f.engine.write64(ivars + 16, data + 1056); f.engine.write64(ivars + 24, data + 1072);
    f.engine.write32(ivars + 32, 3); f.engine.write32(ivars + 36, 8);
    f.engine.write32(offset, 8);
    alchemy::ImageRuntimeInfo image; image.module_id = 41; image.begin = data; image.end = data + 4096;
    image.sections = {{"__DATA", "__objc_classlist", data, 8, 0}};
    f.runtime.prepare_images(std::span(&image, 1));
    require(f.engine.read32(offset) == 64, "ivar offset was not slid past the superclass");
    require(f.call("_class_getInstanceSize", {cls}) == 72, "slid instance size mismatch");
    const auto ivar = f.call("_class_getInstanceVariable", {cls, f.string("value")});
    require(ivar && f.call("_ivar_getOffset", {ivar}) == 64, "ivar_getOffset after sliding");
    const std::array<Address, 1> ids{41}; f.runtime.rollback_images(ids);
    require(f.engine.read32(offset) == 8, "rollback did not restore the ivar offset");
}
// Diagnostics: ALCHEMY_PROBE_MISSING=zero's message probe and the class dump.
void test_probe_messages() {
    Fixture f;
    const auto cls = f.runtime.register_native_class("ProbeTarget", f.runtime.find_class("NSObject"));
    static_cast<void>(f.runtime.add_native_method(cls, "answer", [](auto& s, auto&) { s.x[0] = 27; }));
    const auto object = f.runtime.allocate(cls), missing = f.runtime.selector("probeMissing:");
    const auto unsupported = [&](Address receiver) {
        try { f.call("_objc_msgSend", {receiver, missing, 5}); } catch (const std::runtime_error& error) {
            return std::string_view(error.what()).find("unsupported Objective-C message") != std::string_view::npos;
        }
        return false;
    };
    require(unsupported(object), "strict mode did not reject a missing message");
    f.runtime.set_probe_messages(true);
    require(f.call("_objc_msgSend", {object, missing, 5}) == 0 && f.call("_objc_msgSend", {object, missing, 6}) == 0 &&
        f.call("_objc_msgSend", {cls, missing, 7}) == 0, "probed message did not return zero");
    require(f.call("_objc_msgSend", {object, f.runtime.selector("answer")}) == 27, "probe mode changed implemented dispatch");
    const auto probed = f.runtime.probed_messages();
    require(probed.size() == 2 && probed[0] == "-[ProbeTarget probeMissing:]" && probed[1] == "+[ProbeTarget probeMissing:]",
        "probe did not log each missing message once in first-seen order");
    const auto dump = f.runtime.dump_json();
    require(dump.find("{\"name\":\"ProbeTarget\",\"super\":\"NSObject\",\"native\":true,\"instance\":[\"answer\"]") !=
        std::string::npos, "class dump lacks the native class and its selector");
    f.runtime.set_probe_messages(false);
    require(unsupported(object), "strict mode did not return after the probe");
    f.call("_objc_release", {object});
}
}
int main() {
    try { test_dispatch(false); test_dispatch(true); test_ownership_and_pools(); test_concurrent_initialization(); test_dynamic_registration(); test_protected_transaction_rollback(); test_protocols(); test_ivar_sliding(); test_probe_messages(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Alchemy Objective-C tests passed\n";
}
