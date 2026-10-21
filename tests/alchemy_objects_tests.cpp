#include "alchemy/object_store.hpp"
#include "alchemy/process_context.hpp"

#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void test_lifetime() {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext context(engine, [](auto, auto bytes) -> alchemy::OutputResult { return {bytes.size(), 0}; });
    auto& objects = context.objects();
    unsigned destroyed = 0;
    const auto child = objects.create(16, 123);
    const auto parent = objects.create(16, 456, alchemy::ObjectKind::instance, {}, [&](auto address) {
        ++destroyed;
        check(objects.require(address)->destroying.load(), "destruction state was not published");
        check(engine.read64(address) == 456, "object memory vanished during destructor");
        objects.dispose(address); // NSObject's base deallocator must be harmless here.
        objects.release(child); // Reentrant destruction must not hold the registry lock.
    });
    check(objects.retain(parent) == parent, "retain changed object identity");
    objects.release(parent);
    check(destroyed == 0, "object destroyed while retained");
    objects.release(parent);
    check(destroyed == 1 && objects.live_count() == 0, "object destruction did not release children");
    check(!engine.query_region(parent) && !engine.query_region(child), "destroyed object storage remained mapped");
    bool rejected = false;
    try { objects.release(parent); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "destroyed pointer was accepted");
    const auto failing = objects.create(16, 0, alchemy::ObjectKind::instance, {}, [](auto) { throw std::runtime_error("fixture destructor"); });
    rejected = false;
    try { objects.release(failing); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected && !objects.find(failing) && !engine.query_region(failing), "failed destructor leaked object storage");
}
void test_constants_and_concurrency() {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext context(engine, [](auto, auto bytes) -> alchemy::OutputResult { return {bytes.size(), 0}; });
    auto& objects = context.objects();
    constexpr auto constant = 0x200000000ULL;
    engine.map_memory(constant, 32);
    engine.write64(constant, 11);
    engine.set_memory_access(constant, true, false, false);
    objects.adopt(constant, 32, 11, alchemy::ObjectKind::string, {});
    objects.set_class(alchemy::ObjectKind::string, 22);
    check(engine.read64(constant) == 11 && objects.require(constant)->class_address.load() == 22,
          "adopted constant storage was mutated");
    objects.release(constant);
    check(objects.live_count() == 0 && objects.live_count(true) == 1, "immortal object lifetime changed");
    std::atomic<unsigned> destroyed{0};
    const auto object = objects.create(16, 0, alchemy::ObjectKind::instance, {}, [&](auto) { ++destroyed; });
    std::array<std::thread, 4> workers;
    for (auto& worker : workers) worker = std::thread([&] {
        for (unsigned iteration = 0; iteration < 1000; ++iteration) {
            static_cast<void>(objects.retain(object));
            objects.release(object);
        }
    });
    for (auto& worker : workers) worker.join();
    check(destroyed == 0 && objects.require(object)->references.load() == 1, "concurrent ownership corrupted retain count");
    objects.release(object);
    check(destroyed == 1, "last ownership release did not destroy exactly once");
}
}
int main() {
    try {
        test_lifetime();
        test_constants_and_concurrency();
        std::cout << "Alchemy object ownership tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
