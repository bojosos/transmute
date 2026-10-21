// Dispatch objects as Objective-C objects and queue-specific data
// (dispatch_objects.cpp; split from the QLab runtime tests).
#include "alchemy/darwin/kit/completion/runtime/tests/runtime_test_kit.hpp"

namespace {

void test_dispatch_objects(Fixture& f) {
    const auto queue = f.libsystem("_dispatch_queue_create", {f.c_string("g2.test.queue"), 0});
    require(f.objc.object_class(queue) == f.cls("OS_dispatch_queue_serial"), "a serial queue's isa is OS_dispatch_queue_serial");
    require(f.send(queue, "isKindOfClass:", {f.cls("OS_dispatch_queue")}) & 1, "a queue is an OS_dispatch_queue");
    require(f.send(queue, "isKindOfClass:", {f.cls("OS_object")}) & 1, "a queue is an OS_object");
    // Retain and release through objc_msgSend / objc_retain reach dispatch_retain / dispatch_release.
    require(f.send(queue, "retain") == queue, "-retain returns the queue");
    require(f.send(queue, "retainCount") == 2, "-retain took a dispatch reference");
    f.call("/usr/lib/libobjc.A.dylib", "_objc_retain", {queue});
    require(f.send(queue, "retainCount") == 3, "objc_retain took a dispatch reference");
    f.call("/usr/lib/libobjc.A.dylib", "_objc_release", {queue});
    f.send(queue, "release");
    require(f.send(queue, "retainCount") == 1, "releases returned the references");
    const auto description = f.text(f.send(queue, "description"));
    require(description.find("OS_dispatch_queue_serial") != std::string::npos && description.find("g2.test.queue") != std::string::npos,
            "description names the class and label: " + description);
    const auto main = f.symbol("/usr/lib/libSystem.B.dylib", "__dispatch_main_q");
    require(f.objc.object_class(main) == f.cls("OS_dispatch_queue_main"), "the main queue's isa");
    const auto global = f.libsystem("_dispatch_get_global_queue", {0, 0});
    require(f.objc.object_class(global) == f.cls("OS_dispatch_queue_global"), "a global queue's isa");
    const auto group = f.libsystem("_dispatch_group_create");
    require(f.objc.object_class(group) == f.cls("OS_dispatch_group"), "a group's isa");
    f.send(group, "release");

    // Queue-specific data: on the queue while it runs work, on the main queue on the main thread.
    const Address key = f.c_string("key"), context = 0x1234, main_context = 0x5678;
    f.libsystem("_dispatch_queue_set_specific", {queue, key, context, 0});
    require(f.libsystem("_dispatch_queue_get_specific", {queue, key}) == context, "dispatch_queue_get_specific");
    require(f.libsystem("_dispatch_get_specific", {key}) == 0, "the main thread is not on the queue");
    f.libsystem("_dispatch_queue_set_specific", {main, key, main_context, 0});
    require(f.libsystem("_dispatch_get_specific", {key}) == main_context, "the main thread runs the main queue");
    Address seen{};
    const auto probe = f.process.install_function("_g2_test_specific", [&f, &seen, key](CpuState& s, GuestMemory&) {
        seen = f.libsystem("_dispatch_get_specific", {key});
        s.x[0] = 0;
    });
    f.libsystem("_dispatch_sync_f", {queue, 0, probe});
    require(seen == context, "dispatch_get_specific inside dispatch_sync sees the queue's value");
    // A destructor runs when the value is replaced.
    Address destroyed{};
    const auto destructor = f.process.install_function("_g2_test_destructor", [&destroyed](CpuState& s, GuestMemory&) { destroyed = s.x[0]; s.x[0] = 0; });
    f.libsystem("_dispatch_queue_set_specific", {queue, key, 0x99, destructor});
    f.libsystem("_dispatch_queue_set_specific", {queue, key, 0, 0});
    require(destroyed == 0x99, "the destructor ran with the replaced context");
    f.call("/usr/lib/libobjc.A.dylib", "_objc_release", {queue});
}

} // namespace

int main() {
    try {
        Fixture fixture;
        test_dispatch_objects(fixture);
        std::cout << "Alchemy dispatch objects tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
