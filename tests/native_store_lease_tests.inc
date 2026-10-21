// Included inside test_main.cpp's anonymous namespace on Windows x64.
// These fixtures remain valid for checked stores and then exercise journal
// commit/capacity ordering when native store regions are enabled.

void test_native_store_lease_revision_commits() {
    constexpr std::uint64_t data = 0x500800000ULL;
    constexpr std::uint64_t count = 130;
    for (const bool pair : {false, true}) {
        auto image = transmute::PeImage::parse(executable({
            0xf9400004U, // ldr x4,[x0]
            0xf9400005U, // ldr x5,[x0]
            pair ? 0xa9000801U : 0xf9000001U, // stp x1,x2 / str x1,[x0]
            0xd1000463U, // sub x3,x3,#1
            0xb5ffffc3U, // cbnz x3,-8
            0xd65f03c0U
        }));
        transmute::ReferenceEngine engine(image);
        engine.set_jit_enabled(true);
        engine.map_memory(data, 64);
        engine.write64(data, 0);
        engine.write64(data + 8, 0);
        const auto before = engine.write_revision(data, 16);
        expect(before.has_value(), "native store fixture has no owned revision");
        engine.set_initial_register(0, data);
        engine.set_initial_register(1, 0x8877665544332211ULL);
        engine.set_initial_register(2, 0x1020304050607080ULL);
        engine.set_initial_register(3, count);
        expect_failure([&] { static_cast<void>(engine.run(5)); },
                       "guest exceeded instruction limit");
        const auto records_per_store = pair ? 2ULL : 1ULL;
        expect(engine.state().pc == image.entry() + 8 &&
                   engine.state().x[3] == count - 1 &&
                   engine.write_revision(data, 16) == *before + records_per_store &&
                   engine.read64(data) == 0x8877665544332211ULL &&
                   engine.read64(data + 8) == (pair ? 0x1020304050607080ULL : 0),
               "native store metadata was not committed before the instruction-limit observer");
        const auto rest = engine.run();
        expect(rest.instructions == count * 3 - 2 &&
                   engine.write_revision(data, 16) ==
                       *before + count * records_per_store,
               "native store journal lost, duplicated or replayed a successful width write");
        const auto warm_before = engine.write_revision(data, 16);
        const std::array<std::uint64_t, 4> arguments{
            data, 0x0123456789abcdefULL, 0xfedcba9876543210ULL, count};
        expect(engine.invoke_callback(image.entry(), arguments) == data &&
                   engine.write_revision(data, 16) ==
                       *warm_before + count * records_per_store &&
                   engine.read64(data) == arguments[1] &&
                   engine.read64(data + 8) == (pair ? arguments[2] : 0),
               "warm native store journal capacity/region exits changed memory or revision counts");
        expect(engine.jit_stats().native_journal_exits != 0,
               "native store capacity fixture never exercised a full dirty journal");
    }
}

void test_native_store_lease_partial_fault_commit() {
    constexpr std::uint64_t data = 0x500800000ULL;
    constexpr std::uint64_t boundary = data + 0x1000;
    constexpr std::uint64_t value = 0x8877665544332211ULL;
    auto image = transmute::PeImage::parse(executable({
        0xf9000001U, // str x1,[x0]: successful prefix
        0xf9400003U, // ldr x3,[x0]
        0xf9400004U, // ldr x4,[x0]
        0xf90000a2U, // str x2,[x5]: only four bytes are mapped
        0xd65f03c0U
    }));
    transmute::ReferenceEngine engine(image);
    engine.set_jit_enabled(true);
    engine.map_memory(data, 64);
    engine.map_memory(boundary, 4);
    engine.write32(boundary, 0);
    engine.write64(data, 0);
    const auto before = engine.write_revision(data, 8);
    engine.set_initial_register(0, data);
    engine.set_initial_register(1, value);
    engine.set_initial_register(2, 0x0123456789abcdefULL);
    engine.set_initial_register(5, boundary);
    expect_failure([&] { static_cast<void>(engine.run()); },
                   "guest write outside mapped memory");
    expect(engine.state().pc == image.entry() + 12 &&
               engine.state().x[3] == value && engine.state().x[4] == value &&
               engine.read64(data) == value && engine.read32(boundary) == 0x89abcdefU &&
               engine.write_revision(data, 8) == *before + 1 &&
               engine.write_revision(boundary, 4) == *before + 5,
           "native store prefix metadata was not committed exactly once before a partial fault");
}

void test_native_store_lease_cross_granule_reservations() {
    constexpr std::uint64_t data = 0x500800000ULL;
    for (const std::uint64_t reserved_offset : {0ULL, 16ULL, 32ULL}) {
        auto image = transmute::PeImage::parse(executable({
            0xc85ffc81U, // ldaxr x1,[x4]
            0xc803fc82U, // stlxr w3,x2,[x4]
            0xd65f03c0U,
            0xf9400005U, // worker: ldr x5,[x0]
            0xf9400006U, // ldr x6,[x0]
            0xf9000001U, // str x1,[x0]: crosses a sixteen-byte granule
            0xd65f03c0U
        }));
        transmute::ReferenceEngine engine(image);
        engine.map_memory(data, 64);
        engine.write64(data + reserved_offset, 42);
        engine.set_initial_register(2, 99);
        engine.set_initial_register(4, data + reserved_offset);
        expect_failure([&] { static_cast<void>(engine.run(1)); },
                       "guest exceeded instruction limit");
        auto worker = engine.fork_thread(image.entry() + 12, data + 15);
        worker->set_jit_enabled(true);
        worker->set_initial_register(1, 0x1122334455667788ULL);
        std::exception_ptr worker_fault;
        std::thread native_worker([&] {
            try { static_cast<void>(worker->run()); }
            catch (...) { worker_fault = std::current_exception(); }
        });
        native_worker.join();
        if (worker_fault) std::rethrow_exception(worker_fault);
        static_cast<void>(engine.run());
        expect(engine.state().x[3] == (reserved_offset < 32 ? 1U : 0U) &&
                   worker->jit_executed_instructions() == 4 &&
                   (reserved_offset != 32 || engine.read64(data + 32) == 99),
               "native store commit missed a touched granule or invalidated an unrelated reservation");
    }
}
