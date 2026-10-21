// Objective-C runtime on the fx engine (owner: A1): the guest method cache
// gives the same results as the uncached dispatcher (ALCHEMY_OBJC_CACHE=0)
// and is flushed by every method change; host->guest sends carry FP, stack
// and HFA results; a micro-benchmark reports messages/sec and allocations/sec.
//
// Guest code is hand-assembled ARM64 (encodings from clang
// --target=arm64-apple-macos11, listed next to each routine).
#include "alchemy/objc_runtime.hpp"
#include "alchemy/engine_adapter.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <thread>
#include <vector>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using Address = std::uint64_t;
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
void set_cache(bool enabled) {
#ifdef _WIN32
    _putenv_s("ALCHEMY_OBJC_CACHE", enabled ? "1" : "0");
#else
    setenv("ALCHEMY_OBJC_CACHE", enabled ? "1" : "0", 1);
#endif
}

// mov x0, #27 ; ret
constexpr std::array<std::uint32_t, 2> return_27{0xd2800360, 0xd65f03c0};
// mov x0, #42 ; ret
constexpr std::array<std::uint32_t, 2> return_42{0xd2800540, 0xd65f03c0};
// -(NSRect)offset:(NSRect)r by:(double)d -> {x + d, y + d, w * 2, h * 2}
constexpr std::array<std::uint32_t, 5> offset_rect{0x1e642800, 0x1e642821, 0x1e622842, 0x1e632863, 0xd65f03c0};
// -(long)sum:a:b:c:d:e:f:g -> x2 + ... + x7 + [sp] + [sp, #8]
constexpr std::array<std::uint32_t, 10> sum_eight{0x8b030040, 0x8b040000, 0x8b050000, 0x8b060000, 0x8b070000,
    0xf94003e9, 0x8b090000, 0xf94007e9, 0x8b090000, 0xd65f03c0};
// -(struct {long a, b, c;})large -> {25, 26, 27} through x8
constexpr std::array<std::uint32_t, 7> store_large{0xd2800329, 0xf9000109, 0xd2800349, 0xf9000509, 0xd2800369,
    0xf9000909, 0xd65f03c0};
// send_loop(receiver, sel, count, objc_msgSend): count x [receiver sel]
constexpr std::array<std::uint32_t, 16> send_loop{0xa9bd7bfd, 0xa90153f3, 0xa9025bf5, 0xaa0003f3, 0xaa0103f4,
    0xaa0203f5, 0xaa0303f6, 0xaa1303e0, 0xaa1403e1, 0xd63f02c0, 0xf10006b5, 0x54ffff81, 0xa9425bf5, 0xa94153f3,
    0xa8c37bfd, 0xd65f03c0};
// alloc_loop(cls, objc_alloc_init, count, objc_release): count x objc_release(objc_alloc_init(cls))
constexpr std::array<std::uint32_t, 16> alloc_loop{0xa9bd7bfd, 0xa90153f3, 0xa9025bf5, 0xaa0003f3, 0xaa0103f4,
    0xaa0203f5, 0xaa0303f6, 0xaa1303e0, 0xd63f0280, 0xd63f02c0, 0xf10006b5, 0x54ffff81, 0xa9425bf5, 0xa94153f3,
    0xa8c37bfd, 0xd65f03c0};

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](auto, auto bytes) -> alchemy::OutputResult { return {bytes.size(), 0}; }};
    alchemy::ObjcRuntime runtime{process};
    Address export_address(std::string_view name) {
        const auto symbol = process.resolve(alchemy::objc_library, name);
        require(symbol.has_value(), "missing Objective-C export");
        return symbol->address;
    }
    Address call(std::string_view name, std::initializer_list<Address> args) {
        return engine.invoke_callback(export_address(name), std::span(args.begin(), args.size()));
    }
    Address call_at(Address entry, std::initializer_list<Address> args) {
        return engine.invoke_callback(entry, std::span(args.begin(), args.size()));
    }
    template <std::size_t N>
    Address code(const std::array<std::uint32_t, N>& words) {
        const auto result = process.allocate_guest(N * 4);
        for (std::size_t i = 0; i < N; ++i) engine.write32(result + 4 * i, words[i]);
        engine.set_memory_access(result, true, false, true);
        return result;
    }
    Address string(std::string_view value) {
        const auto result = process.allocate_guest(value.size() + 1);
        engine.write_bytes(result, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
        return result;
    }
    // class_addMethod with a guest IMP; returns whether it was added.
    bool add_guest_method(Address cls, std::string_view name, Address imp, std::string_view types = "q@:") {
        return call("_class_addMethod", {cls, runtime.selector(name), imp, string(types)}) != 0;
    }
};

// One dispatch scenario; every result must match between cached and uncached runs.
void test_dispatch_and_flush(bool cached) {
    set_cache(cached);
    Fixture f;
    const auto root = f.runtime.find_class("NSObject");
    const auto base = f.runtime.register_native_class("CacheBase", root, 8);
    const auto derived = f.runtime.register_native_class("CacheDerived", base, 8);
    const auto r27 = f.code(return_27), r42 = f.code(return_42);
    require(f.add_guest_method(base, "answer", r27), "class_addMethod failed");
    const auto object = f.runtime.allocate(derived), sel = f.runtime.selector("answer");
    for (int i = 0; i < 4; ++i) require(f.call("_objc_msgSend", {object, sel}) == 27, "inherited guest method");
    const bool filled = f.engine.read64(derived + 16) != 0 && f.engine.read64(derived + 24) == derived;
    require(filled == cached, cached ? "method cache was not filled" : "ALCHEMY_OBJC_CACHE=0 filled a cache");
    // A subclass method added after the inherited IMP was cached wins.
    require(f.add_guest_method(derived, "answer", r42), "subclass class_addMethod failed");
    for (int i = 0; i < 3; ++i) require(f.call("_objc_msgSend", {object, sel}) == 42, "class_addMethod did not flush");
    // [super answer] from CacheDerived reaches CacheBase through its cache.
    const auto super = f.process.allocate_guest(16);
    f.engine.write64(super, object);
    f.engine.write64(super + 8, derived);
    for (int i = 0; i < 3; ++i) require(f.call("_objc_msgSendSuper2", {super, sel}) == 27, "super2 dispatch");
    f.engine.write64(super + 8, base);
    for (int i = 0; i < 3; ++i) require(f.call("_objc_msgSendSuper", {super, sel}) == 27, "super dispatch");
    // class_replaceMethod and method_setImplementation flush cached IMPs.
    require(f.call("_class_replaceMethod", {base, sel, r42, 0}) == r27, "class_replaceMethod result");
    f.engine.write64(super + 8, derived);
    for (int i = 0; i < 3; ++i) require(f.call("_objc_msgSendSuper2", {super, sel}) == 42, "class_replaceMethod did not flush");
    const auto method = f.call("_class_getInstanceMethod", {derived, sel});
    require(f.call("_method_setImplementation", {method, r27}) == r42, "method_setImplementation result");
    for (int i = 0; i < 3; ++i) require(f.call("_objc_msgSend", {object, sel}) == 27, "method_setImplementation did not flush");
    // A native method replacing a cached guest one (add_native_method) flushes too.
    static_cast<void>(f.runtime.add_native_method(derived, "answer", [](auto& s, auto&) { s.x[0] = 99; }, false, "q@:"));
    for (int i = 0; i < 3; ++i) require(f.call("_objc_msgSend", {object, sel}) == 99, "add_native_method did not flush");
    // Class messages use the metaclass cache.
    static_cast<void>(f.runtime.add_native_method(base, "classAnswer", [](auto& s, auto&) { s.x[0] = s.x[2] + 1; }, true, "q@:q"));
    const auto class_sel = f.runtime.selector("classAnswer");
    for (Address i = 0; i < 4; ++i) require(f.call("_objc_msgSend", {derived, class_sel, i}) == i + 1, "class message");
    // nil receivers return zero, with and without a cache.
    require(f.call("_objc_msgSend", {0, sel}) == 0, "nil message");
    // Many selectors grow the cache past its first bucket array.
    for (int i = 0; i < 40; ++i) {
        const auto name = "grow" + std::to_string(i);
        static_cast<void>(f.runtime.add_native_method(base, name, [i](auto& s, auto&) { s.x[0] = static_cast<Address>(i); }, false, "q@:"));
    }
    for (int round = 0; round < 2; ++round) for (int i = 0; i < 40; ++i)
        require(f.call("_objc_msgSend", {object, f.runtime.selector("grow" + std::to_string(i))}) == static_cast<Address>(i),
            "grown cache returned a wrong IMP");
    require(f.call("_objc_msgSend", {object, sel}) == 99, "grown cache lost an entry");
    // respondsToSelector: and instancesRespondToSelector: see host and guest methods.
    const auto responds = f.runtime.selector("respondsToSelector:"), instances = f.runtime.selector("instancesRespondToSelector:");
    require(f.call("_objc_msgSend", {object, responds, sel}) == 1, "respondsToSelector: missed a method");
    require(f.call("_objc_msgSend", {object, responds, f.runtime.selector("missing")}) == 0, "respondsToSelector: invented a method");
    require(f.call("_objc_msgSend", {derived, instances, sel}) == 1, "instancesRespondToSelector: missed a method");
    require(f.call("_objc_msgSend", {derived, instances, class_sel}) == 0, "instancesRespondToSelector: saw a class method");
    require(f.call("_objc_msgSend", {derived, responds, class_sel}) == 1, "+respondsToSelector: missed a class method");
    require(f.runtime.responds(object, sel) && !f.runtime.responds(object, f.runtime.selector("missing")), "ObjcRuntime::responds");
    // libobjc's NSObject helpers.
    const auto perform = f.runtime.selector("performSelector:"), with = f.runtime.selector("performSelector:withObject:");
    require(f.call("_objc_msgSend", {object, perform, sel}) == 99, "performSelector:");
    require(f.call("_objc_msgSend", {derived, with, class_sel, 41}) == 42, "+performSelector:withObject:");
    require(f.call("_objc_msgSend", {object, f.runtime.selector("isMemberOfClass:"), derived}) == 1 &&
        f.call("_objc_msgSend", {object, f.runtime.selector("isMemberOfClass:"), base}) == 0, "isMemberOfClass:");
    require(f.call("_objc_msgSend", {derived, f.runtime.selector("isSubclassOfClass:"), root}) == 1 &&
        f.call("_objc_msgSend", {base, f.runtime.selector("isSubclassOfClass:"), derived}) == 0, "isSubclassOfClass:");
    const auto native_imp = f.call("_objc_msgSend", {object, f.runtime.selector("methodForSelector:"), sel});
    require(native_imp && native_imp == f.call("_objc_msgSend", {derived, f.runtime.selector("instanceMethodForSelector:"), sel}) &&
        f.call_at(native_imp, {object, sel}) == 99, "methodForSelector:/instanceMethodForSelector:");
    require(f.call("_objc_msgSend", {object, f.runtime.selector("superclass")}) == base, "-superclass");
    require(f.call("_objc_msgSend", {derived, f.runtime.selector("copy")}) == derived, "+copy");
    // An unknown selector fails explicitly, cached or not.
    bool rejected{};
    try { static_cast<void>(f.call("_objc_msgSend", {object, f.runtime.selector("missing")})); }
    catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("-[CacheDerived missing]") != std::string::npos; }
    require(rejected, "missing method did not fail explicitly");
    f.call("_objc_release", {object});
}

// Host -> guest sends: FP arguments and an NSRect (HFA) result in d0-d3,
// stack arguments, guest overrides of host methods, and send_super.
void test_send(bool cached) {
    set_cache(cached);
    Fixture f;
    const auto root = f.runtime.find_class("NSObject");
    const auto host = f.runtime.register_native_class("SendHost", root, 8);
    const auto guest = f.runtime.register_native_class("SendGuest", host, 8);
    static_cast<void>(f.runtime.add_native_method(host, "scale:", [](auto& s, auto&) {
        s.v[0] = {std::bit_cast<std::uint64_t>(std::bit_cast<double>(s.v[0][0]) * 3.0), 0}; }, false, "d@:d"));
    require(f.add_guest_method(guest, "offset:by:", f.code(offset_rect), "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}d"),
        "NSRect method registration");
    require(f.add_guest_method(guest, "sum:b:c:d:e:f:g:h:", f.code(sum_eight), "q@:qqqqqqqq"), "stack method registration");
    const auto object = f.runtime.allocate(guest);
    for (int round = 0; round < 3; ++round) {
        alchemy::CallRegs rect;
        rect.set_d(0, 1.0); rect.set_d(1, 2.0); rect.set_d(2, 3.0); rect.set_d(3, 4.0); rect.set_d(4, 0.5);
        const auto result = f.runtime.send(object, "offset:by:", rect);
        require(result.complete && result.d(0) == 1.5 && result.d(1) == 2.5 && result.d(2) == 6.0 && result.d(3) == 8.0,
            "NSRect result did not come back in d0-d3");
        alchemy::CallRegs sum;
        for (unsigned i = 2; i < 8; ++i) sum.x[i] = i;
        sum.push_stack(std::uint64_t{100});
        sum.push_stack(std::uint64_t{1000});
        require(f.runtime.send(object, "sum:b:c:d:e:f:g:h:", sum).x0 == 2 + 3 + 4 + 5 + 6 + 7 + 1100, "stack arguments");
        alchemy::CallRegs scale;
        scale.set_d(0, 2.0);
        require(f.runtime.send(object, "scale:", scale).d(0) == 6.0, "host method through send");
    }
    // A guest override of the host method wins; send_super reaches the host IMP.
    const auto doubled = f.code(std::array<std::uint32_t, 2>{0x1e602800, 0xd65f03c0}); // fadd d0, d0, d0 ; ret
    require(f.add_guest_method(guest, "scale:", doubled, "d@:d"), "override registration");
    alchemy::CallRegs scale;
    scale.set_d(0, 2.0);
    require(f.runtime.send(object, "scale:", scale).d(0) == 4.0, "guest override was not used");
    require(f.runtime.send_super(object, guest, f.runtime.selector("scale:"), scale).d(0) == 6.0, "send_super");
    require(f.runtime.lookup_imp(object, f.runtime.selector("scale:")) == doubled, "lookup_imp");
    require(!f.runtime.send_if_responds(object, f.runtime.selector("absent"), {}).has_value(), "send_if_responds");
    // A 24-byte result goes through the indirect-result register x8.
    require(f.add_guest_method(guest, "large", f.code(store_large), "{Large=qqq}@:"), "indirect result method registration");
    const auto large = f.process.allocate_guest(24);
    alchemy::CallRegs indirect;
    indirect.indirect_result = large;
    static_cast<void>(f.runtime.send(object, "large", indirect));
    require(f.engine.read64(large) == 25 && f.engine.read64(large + 8) == 26 && f.engine.read64(large + 16) == 27,
        "indirect result was not written through x8");
    require(f.runtime.send(0, "scale:", scale).d(0) == 0.0, "nil send");
    f.call("_objc_release", {object});
}

// Guest threads dispatch through caches while the main thread keeps adding
// methods (each a flush) and growing caches with new selectors.
void test_concurrent_flush() {
    set_cache(true);
    Fixture f;
    const auto root = f.runtime.find_class("NSObject");
    const auto cls = f.runtime.register_native_class("ConcurrentCache", root, 8);
    require(f.add_guest_method(cls, "answer", f.code(return_27)), "concurrent method");
    const auto object = f.runtime.allocate(cls), sel = f.runtime.selector("answer");
    const auto loop = f.code(send_loop), send = f.export_address("_objc_msgSend");
    std::vector<std::unique_ptr<transmute::ReferenceEngine>> engines;
    for (int i = 0; i < 4; ++i) engines.push_back(f.engine.fork_thread(loop, 0));
    std::atomic<bool> failed{};
    std::vector<std::thread> threads;
    for (auto& engine : engines) threads.emplace_back([&, worker = engine.get()] {
        try {
            const std::array<Address, 4> args{object, sel, 200'000, send};
            static_cast<void>(worker->invoke_callback(loop, args));
            const std::array<Address, 2> once{object, sel};
            if (worker->invoke_callback(send, once) != 27) failed = true;
        } catch (...) { failed = true; }
    });
    for (int i = 0; i < 200; ++i) {
        const auto name = "flush" + std::to_string(i);
        static_cast<void>(f.runtime.add_native_method(cls, name, [i](auto& s, auto&) { s.x[0] = static_cast<Address>(i); }, false, "q@:"));
        require(f.call("_objc_msgSend", {object, f.runtime.selector(name)}) == static_cast<Address>(i), "message during flushes");
    }
    for (auto& thread : threads) thread.join();
    require(!failed, "a guest thread failed while caches were flushed");
    for (auto& engine : engines) f.process.release_thread(*engine);
    f.call("_objc_release", {object});
}

double rate(std::uint64_t count, std::chrono::steady_clock::duration elapsed) {
    return static_cast<double>(count) / std::chrono::duration<double>(elapsed).count();
}

void benchmark(bool cached) {
    set_cache(cached);
    Fixture f;
    const auto root = f.runtime.find_class("NSObject");
    const auto cls = f.runtime.register_native_class("BenchObject", root, 16);
    require(f.add_guest_method(cls, "answer", f.code(return_27)), "benchmark method");
    static_cast<void>(f.runtime.add_native_method(cls, "nativeAnswer", [](auto& s, auto&) { s.x[0] = 42; }, false, "q@:"));
    const auto object = f.runtime.allocate(cls);
    const auto loop = f.code(send_loop), allocs = f.code(alloc_loop);
    const auto send = f.export_address("_objc_msgSend");
    constexpr std::uint64_t messages = 400'000, allocations = 100'000;
    static_cast<void>(f.call_at(loop, {object, f.runtime.selector("answer"), 1000, send}));
    auto start = std::chrono::steady_clock::now();
    static_cast<void>(f.call_at(loop, {object, f.runtime.selector("answer"), messages, send}));
    const auto guest_rate = rate(messages, std::chrono::steady_clock::now() - start);
    start = std::chrono::steady_clock::now();
    static_cast<void>(f.call_at(loop, {object, f.runtime.selector("nativeAnswer"), messages, send}));
    const auto native_rate = rate(messages, std::chrono::steady_clock::now() - start);
    const auto alloc_init = f.export_address("_objc_alloc_init"), release = f.export_address("_objc_release");
    static_cast<void>(f.call_at(allocs, {cls, alloc_init, 1000, release}));
    start = std::chrono::steady_clock::now();
    static_cast<void>(f.call_at(allocs, {cls, alloc_init, allocations, release}));
    const auto alloc_rate = rate(allocations, std::chrono::steady_clock::now() - start);
    require(f.process.objects().live_count() == 1, "benchmark leaked objects");
    std::cout << "objc benchmark (cache " << (cached ? "on" : "off") << "): "
              << static_cast<std::uint64_t>(guest_rate) << " guest-IMP msgs/s, "
              << static_cast<std::uint64_t>(native_rate) << " host-IMP msgs/s, "
              << static_cast<std::uint64_t>(alloc_rate) << " alloc+release/s\n";
    f.call("_objc_release", {object});
}
// The dispatcher the runtime installs is exactly runtime/objc/dispatch.S as
// assembled by clang (an arm64 MH_OBJECT): its __text up to the lookup literal.
void test_dispatch_encoding(const char* object_path) {
    std::ifstream file(object_path, std::ios::binary);
    require(file.good(), "cannot open the assembled dispatcher object");
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto read32 = [&](std::size_t offset) {
        require(offset + 4 <= bytes.size(), "truncated dispatcher object");
        std::uint32_t value{}; std::memcpy(&value, bytes.data() + offset, 4); return value;
    };
    require(read32(0) == 0xfeedfacf, "dispatcher object is not a 64-bit Mach-O");
    std::size_t text_offset{}, text_size{};
    for (std::size_t command = 32, index = 0; index < read32(16); ++index, command += read32(command + 4)) {
        if (read32(command) != 0x19) continue; // LC_SEGMENT_64; sections follow its 72-byte header
        for (std::uint32_t section = 0; section < read32(command + 64); ++section) {
            const auto header = command + 72 + section * 80ULL;
            if (std::string_view(reinterpret_cast<const char*>(bytes.data() + header), 6) != "__text") continue;
            std::uint64_t size{}; std::memcpy(&size, bytes.data() + header + 40, 8);
            text_offset = read32(header + 48); text_size = static_cast<std::size_t>(size);
        }
    }
    constexpr std::size_t literal = 0x120;
    require(text_size >= literal + 8, "dispatcher object has no __text");
    Fixture f;
    const auto installed = f.export_address("_objc_msgSend");
    for (std::size_t offset = 0; offset < literal; offset += 4)
        require(f.engine.read32(installed + offset) == read32(text_offset + offset),
            "objc_runtime.cpp's dispatcher encoding differs from runtime/objc/dispatch.S at +" + std::to_string(offset));
    require(f.export_address("_objc_msgSendSuper2") == installed + 0x80 && f.export_address("_objc_msgSendSuper") == installed + 0x98,
        "dispatcher entry offsets differ from dispatch.S");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 2 && std::string_view(argv[1]) == "--encoding") {
            test_dispatch_encoding(argv[2]);
            std::cout << "Alchemy objc dispatcher encoding matches dispatch.S\n";
            return 0;
        }
        const bool bench_only = argc > 1 && std::string_view(argv[1]) == "--bench-only";
        if (!bench_only) for (const bool cached : {true, false}) {
            test_dispatch_and_flush(cached);
            test_send(cached);
        }
        if (!bench_only) test_concurrent_flush();
        for (const bool cached : {true, false}) benchmark(cached);
        std::cout << "Alchemy GUI objc runtime tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
