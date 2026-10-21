// Included inside test_main.cpp's anonymous namespace on Windows x64.
void test_jit_worker_profile_inheritance() {
    auto image = transmute::PeImage::parse(executable({
        0x91000400U, // add x0,x0,#1
        0xd65f03c0U
    }));
    transmute::ReferenceEngine parent(image);
    parent.set_jit_enabled(true);
    parent.set_jit_profiling(true);
    const std::array<std::uint64_t, 1> args{7};
    expect(parent.invoke_callback(image.entry(), args) == 8,
           "worker-profile fixture could not warm immutable parent code");
    auto profiled = parent.fork_thread(image.entry(), 0);
    expect(profiled->invoke_callback(image.entry(), args) == 8 &&
               profiled->invoke_callback(image.entry(), args) == 8 &&
               !profiled->jit_stats().hottest_blocks.empty() &&
               parent.native_concurrency_stats().shared_read_leases != 0 &&
               parent.native_concurrency_stats().profiled_worker_caches == 1 &&
               parent.native_concurrency_stats().inherited_worker_native_regions != 0,
           "explicit JIT profiling did not include native worker execution");
    parent.set_jit_profiling(false);
    auto normal = parent.fork_thread(image.entry(), 0);
    expect(normal->invoke_callback(image.entry(), args) == 8 &&
               normal->invoke_callback(image.entry(), args) == 8 &&
               normal->jit_stats().hottest_blocks.empty(),
           "normal guest workers unexpectedly enabled JIT profiling");
}
