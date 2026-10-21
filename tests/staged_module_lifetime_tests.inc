// Included inside test_main.cpp's anonymous namespace on Windows x64.

void test_staged_module_cached_code_discard() {
    constexpr std::uint64_t module_base = 0x180000000ULL;
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    auto bytes = executable({0xd2800540U, 0xd65f03c0U}); // movz x0,#42; ret
    put16(bytes, 0x96, 0x2000);
    put64(bytes, 0x98 + 24, module_base);
    auto module = transmute::PeImage::parse(bytes);
    transmute::ReferenceEngine engine(image);
    engine.set_jit_enabled(true);
    engine.stage_module(module);
    expect(engine.invoke_callback(module.entry(), 0) == 42 &&
               engine.invoke_callback(module.entry(), 0) == 42,
           "staged code discard fixture did not warm native code");
    engine.discard_staged_modules();
    // Keep the object alive for a deterministic red failure. Executing an
    // image removed from the address space is already wrong independently of
    // whether its storage has been released by its loader yet.
    expect_failure([&] {
        static_cast<void>(engine.invoke_callback(module.entry(), 0));
    }, "guest attempted to execute unmapped memory");
}

void test_staged_module_hint_replacement() {
    constexpr std::uint64_t module_base = 0x180000000ULL;
    for (const bool destroy_owner : {false, true}) {
        auto image = transmute::PeImage::parse(executable({
            0xd61f0000U // br x0: enter staged module without changing LR
        }));
        auto bytes = executable({
            0xaa1e03f3U, // mov x19,x30
            0xd63f0200U, // blr x16: callback removes this module
            0xd2800540U, // movz x0,#42: stale module contents
            0xaa1303feU, // mov x30,x19
            0xd65f03c0U
        });
        put16(bytes, 0x96, 0x2000);
        put64(bytes, 0x98 + 24, module_base);
        auto module = std::make_unique<transmute::PeImage>(
            transmute::PeImage::parse(bytes));
        transmute::ReferenceEngine engine(image);
        engine.set_jit_enabled(true);
        engine.stage_module(*module);
        const auto callback = engine.register_guest_call(
            [&](transmute::CpuState&, transmute::GuestMemory& memory) {
                engine.discard_staged_modules();
                if (destroy_owner) module.reset();
                memory.map_memory(module_base, 0x2000);
                memory.set_memory_access(module_base, true, true, true);
                memory.write32(module_base + 0x1008, 0xd28000e0U); // movz x0,#7
                memory.write32(module_base + 0x100c, 0xaa1303feU);
                memory.write32(module_base + 0x1010, 0xd65f03c0U);
            });
        engine.set_initial_register(0, module->entry());
        engine.set_initial_register(16, callback);
        const auto result = engine.run();
        expect(result.exit_code == 7 && engine.state().x[0] == 7,
               "module hint fetched discarded image bytes after callback address reuse");
        engine.unregister_guest_call(callback);
    }
}
