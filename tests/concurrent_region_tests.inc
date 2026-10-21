// Included inside test_main.cpp's anonymous namespace on Windows x64.

void test_execution_gate_parallel_readers() {
    transmute::ExecutionGate gate;
    std::atomic<unsigned> admitted{};
    std::atomic<bool> release{};
    std::atomic<bool> writer_acquired{};
    std::array<std::thread, 2> readers;
    for (auto& thread : readers) {
        thread = std::thread([&] {
            transmute::ExecutionGate::Read lease(gate);
            if (lease.owns_lock()) admitted.fetch_add(1);
            while (!release.load()) std::this_thread::yield();
        });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (admitted.load() != 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    std::thread writer([&] {
        transmute::ExecutionGate::Exclusive lease(gate);
        writer_acquired.store(true);
        // Recursive checked calls borrow the existing exclusive gate, while
        // retaining the normal recursive address-space mutex contract.
        transmute::ExecutionGate::Exclusive nested(gate);
    });
    while (!gate.has_pending_writer() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    bool new_reader_yielded = false;
    bool writer_waited = false;
    {
        transmute::ExecutionGate::Read later(gate);
        new_reader_yielded = !later.owns_lock();
        writer_waited = !writer_acquired.load();
    }
    release.store(true);
    for (auto& thread : readers) thread.join();
    writer.join();
    expect(admitted.load() == 2 && writer_waited && new_reader_yielded &&
               writer_acquired.load(),
           "execution gate failed parallel read admission or writer quiescence");
}

void test_execution_gate_reader_wait_wakeup() {
    // A reader must wake even when multiple queued writers change the value
    // it waited on, or drain the queue before that reader starts waiting.
    for (unsigned round = 0; round < 32; ++round) {
        transmute::ExecutionGate gate;
        std::optional<transmute::ExecutionGate::Read> held(std::in_place, gate);
        expect(held->owns_lock(), "reader-wakeup fixture could not pin its backing");
        std::atomic<unsigned> writers_done{}, readers_ready{}, readers_done{};
        std::array<std::thread, 2> writers;
        for (auto& writer : writers) {
            writer = std::thread([&] {
                transmute::ExecutionGate::Exclusive owner(gate);
                transmute::ExecutionGate::Exclusive nested(gate);
                writers_done.fetch_add(1);
            });
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!gate.has_pending_writer() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        const bool pending = gate.has_pending_writer();
        std::array<std::thread, 4> readers;
        for (auto& reader : readers) {
            reader = std::thread([&] {
                readers_ready.fetch_add(1);
                for (;;) {
                    transmute::ExecutionGate::Read owner(gate);
                    if (owner.owns_lock()) {
                        readers_done.fetch_add(1);
                        break;
                    }
                    gate.wait_for_pending_writer();
                }
            });
        }
        while (readers_ready.load() != readers.size() &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        held.reset();
        for (auto& writer : writers) writer.join();
        for (auto& reader : readers) reader.join();
        expect(pending && writers_done.load() == writers.size() &&
                   readers_done.load() == readers.size() && !gate.has_pending_writer(),
               "parked native readers lost a writer-drain wakeup or ownership");
        gate.wait_for_pending_writer(); // An empty queue must return immediately.
    }
}

void check_concurrent_native_regions(bool register_only) {
    auto image = transmute::PeImage::parse(executable({
        register_only ? 0xd2800022U : 0xf9400002U, // mov x2,#1 / ldr x2,[x0]
        0x8b020063U, // add x3,x3,x2
        0xd1000421U, // sub x1,x1,#1
        0xb5ffffa1U, // cbnz x1,-12
        0xaa0303e0U, // mov x0,x3
        0xd65f03c0U
    }));
    transmute::ReferenceEngine parent(image);
    parent.set_jit_enabled(true);
    constexpr std::uint64_t data = 0x501200000ULL;
    parent.map_memory(data, 4096);
    parent.write64(data, 1);
    std::array<std::unique_ptr<transmute::ReferenceEngine>, 4> engines;
    for (auto& worker : engines) worker = parent.fork_thread(image.entry(), 0);
    for (auto& worker : engines) {
        const std::array<std::uint64_t, 4> args{data, 64, 0, 0};
        expect(worker->invoke_callback(image.entry(), args) == 64 &&
                   worker->jit_stats().native_blocks != 0 &&
                   worker->jit_stats().compiled_native_regions != 0,
               "parallel native fixture did not warm its counted region");
        worker->set_jit_profiling(true);
    }
    std::atomic<unsigned> ready{};
    std::atomic<bool> start{};
    std::array<std::exception_ptr, 4> failures;
    std::array<std::uint64_t, 4> values{};
    std::array<std::thread, 4> threads;
    constexpr std::uint64_t iterations = 50'000;
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i] = std::thread([&, i] {
            ready.fetch_add(1);
            while (!start.load()) std::this_thread::yield();
            try {
                const std::array<std::uint64_t, 4> args{data, iterations, 0, 0};
                values[i] = engines[i]->invoke_callback(image.entry(), args);
            } catch (...) { failures[i] = std::current_exception(); }
        });
    }
    while (ready.load() != threads.size()) std::this_thread::yield();
    start.store(true);
    // Checked writes must quiesce all currently leased native readers, then
    // publish the complete width before another read region is admitted.
    for (unsigned i = 0; i < 500; ++i) parent.write64(data, (i & 1U) ? 1 : 3);
    for (auto& thread : threads) thread.join();
    for (std::size_t i = 0; i < threads.size(); ++i) {
        if (failures[i]) std::rethrow_exception(failures[i]);
        expect(values[i] >= iterations && values[i] <= iterations * 3 &&
                   (values[i] - iterations) % 2 == 0,
               "parallel native region observed torn writes or lost guest steps");
    }
    const auto concurrency = parent.native_concurrency_stats();
    expect(concurrency.shared_read_leases != 0,
           "actual translated workers took no read-only native leases");
    OverlapCheck overlap("translated read-only workers");
    overlap.observe(concurrency.peak_shared_read_leases >= 2,
                    "leases=" + std::to_string(concurrency.shared_read_leases) + " peak=" +
                        std::to_string(concurrency.peak_shared_read_leases));
    overlap.finish();
}

void test_concurrent_native_read_regions() {
    check_concurrent_native_regions(false);
}

void test_concurrent_native_register_regions() {
    check_concurrent_native_regions(true);
}

void test_read_only_cached_block_predicate() {
    transmute::X64BlockCache cache;
    transmute::CpuState state{};
    constexpr std::uint64_t pc = 0x502000000ULL;
    constexpr std::uint64_t generation = 42;
    const std::array<std::uint32_t, 2> pure{0x91000400U, 0xd65f03c0U};
    state.pc = pc;
    state.x[30] = pc + 4096;
    expect(cache.execute(state, pc, pure, generation) &&
               cache.has_read_only_cached_block(pc, generation) &&
               !cache.has_read_only_native_region(pc, generation) &&
               !cache.has_read_only_cached_block(pc, generation + 1),
           "register-only cache admission lost generation or native-region classification");
    cache.invalidate_range(pc, 8);
    expect(!cache.has_read_only_cached_block(pc, generation),
           "invalidated native code remained available for shared execution");
    cache.remember_fallback(pc, generation);
    expect(!cache.has_read_only_cached_block(pc, generation),
           "negative cache entry admitted shared execution");

    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine memory(image);
    constexpr std::uint64_t data = 0x502100000ULL;
    memory.map_memory(data, 4096);
    memory.write64(data, 7);
    for (const auto access : {0xf9400001U, 0xf9000001U}) {
        const std::array<std::uint32_t, 2> words{access, 0xd65f03c0U};
        state.pc = pc;
        state.x[0] = data;
        state.x[1] = 7;
        expect(cache.execute(state, pc, words, generation, &memory) &&
                   !cache.has_read_only_cached_block(pc, generation),
               "legacy checked memory helper admitted shared execution");
        cache.invalidate_all();
    }
}

void test_exclusive_reservation_mapping_replacement() {
    struct Form {
        std::uint32_t load;
        std::uint32_t store;
        unsigned base_register;
        unsigned status_register;
    };
    for (const auto form : {
             Form{0x085ffc81U, 0x0803fc82U, 4, 3}, // LDAXRB / STLXRB
             Form{0x485ffc81U, 0x4803fc82U, 4, 3}, // LDAXRH / STLXRH
             Form{0x885ffc81U, 0x8803fc82U, 4, 3}, // LDAXR W / STLXR W
             Form{0xc85ffc81U, 0xc803fc82U, 4, 3}, // LDAXR X / STLXR X
             Form{0x887f9ca6U, 0x882aa4a8U, 5, 10}, // LDAXP W / STLXP W
             Form{0xc87f9ca6U, 0xc82aa4a8U, 5, 10}}) { // LDAXP X / STLXP X
        for (const bool jit : {false, true}) {
            for (const bool external : {false, true}) {
                auto image = transmute::PeImage::parse(executable({
                    form.load, form.store, 0xd65f03c0U}));
                transmute::ReferenceEngine engine(image);
                engine.set_jit_enabled(jit);
                constexpr std::uint64_t data = 0x503000000ULL;
                std::array<std::uint8_t, 16> first{};
                std::array<std::uint8_t, 16> replacement{};
                if (external) engine.map_external_memory(data, first.data(), first.size(), true);
                else engine.map_memory(data, 16);
                engine.write64(data, 13);
                engine.set_initial_register(form.base_register, data);
                engine.set_initial_register(2, 42);
                engine.set_initial_register(8, 42);
                engine.set_initial_register(9, 43);
                expect_failure([&] { static_cast<void>(engine.run(1)); },
                               "guest exceeded instruction limit");
                engine.unmap_memory(data);
                if (external) {
                    engine.map_external_memory(data, replacement.data(), replacement.size(), true);
                } else engine.map_memory(data, 16);
                // Deliberately do not write the replacement: a checked write
                // would advance the granule epoch and mask the stale monitor.
                static_cast<void>(engine.run());
                expect(engine.state().x[form.status_register] == 1 &&
                           engine.read64(data) == 0 && engine.read64(data + 8) == 0,
                       "exclusive store retained its reservation across same-address backing replacement");
            }
        }
    }
}

void test_exclusive_reservation_backing_boundaries() {
    auto image = transmute::PeImage::parse(executable({
        0xc85ffc81U, 0xc803fc82U, 0xd65f03c0U})); // ldaxr / stlxr
    transmute::ReferenceEngine engine(image);
    constexpr std::uint64_t data = 0x503010000ULL;
    engine.map_memory(data, 2);
    engine.map_memory(data + 2, 4);
    engine.map_memory(data + 6, 2);
    engine.write64(data, 0x1122334455667788ULL);
    engine.set_initial_register(4, data);
    engine.set_initial_register(2, 42);
    expect_failure([&] { static_cast<void>(engine.run(1)); },
                   "guest exceeded instruction limit");
    engine.unmap_memory(data + 2);
    engine.map_memory(data + 2, 4);
    static_cast<void>(engine.run());
    expect(engine.state().x[3] == 1 &&
               engine.read64(data) == 0x1122000000007788ULL,
           "exclusive monitor missed replacement of a middle backing extent");
}

void test_exclusive_reservation_module_identity() {
    for (const bool replace : {false, true}) {
        auto image = transmute::PeImage::parse(executable({
            0xc85ffc81U, 0xc803fc82U, 0xd65f03c0U}));
        auto bytes = executable({0xd65f03c0U});
        put16(bytes, 0x96, 0x2000);
        put64(bytes, 0x98 + 24, 0x180000000ULL);
        put32(bytes, 0x188 + 36, 0xe0000020U); // readable+writable+code
        auto module = transmute::PeImage::parse(bytes);
        transmute::ReferenceEngine engine(image);
        engine.stage_module(module);
        const auto data = module.base() + 0x1008;
        engine.write64(data, 13);
        engine.set_initial_register(4, data);
        engine.set_initial_register(2, 42);
        expect_failure([&] { static_cast<void>(engine.run(1)); },
                       "guest exceeded instruction limit");
        if (replace) {
            engine.discard_staged_modules();
            // Reusing even the identical PeImage object is a fresh guest
            // registration, while publication preserves the original owner.
            engine.stage_module(module);
        } else engine.commit_staged_modules();
        static_cast<void>(engine.run());
        expect(engine.state().x[3] == (replace ? 1U : 0U) &&
                   engine.read64(data) == (replace ? 13U : 42U),
               "exclusive monitor confused staged module publication with replacement");
    }
}

void test_concurrent_native_scalar_stores() {
    auto image = transmute::PeImage::parse(executable({
        0xf9400004U, // ldr x4,[x0]
        0xf9000401U, // str x1,[x0,#8]
        0xf9400405U, // ldr x5,[x0,#8]
        0xca0100a6U, // eor x6,x5,x1
        0xaa060063U, // orr x3,x3,x6
        0xd1000442U, // sub x2,x2,#1
        0xb5ffff42U, // cbnz x2,-24
        0xaa0303e0U, // mov x0,x3
        0xd65f03c0U
    }));
    transmute::ReferenceEngine parent(image);
    parent.set_jit_enabled(true);
    constexpr std::uint64_t data = 0x503020000ULL;
    constexpr std::uint64_t iterations = 50'000;
    parent.map_memory(data, 4096);
    std::array<std::unique_ptr<transmute::ReferenceEngine>, 4> engines;
    for (auto& worker : engines) worker = parent.fork_thread(image.entry(), 0);
    for (std::size_t i = 0; i < engines.size(); ++i) {
        const std::array<std::uint64_t, 4> args{data + i * 16, i + 1, 64, 0};
        expect(engines[i]->invoke_callback(image.entry(), args) == 0 &&
                   engines[i]->jit_stats().compiled_native_regions != 0,
               "parallel store fixture did not warm its native scalar region");
        engines[i]->set_jit_profiling(true);
    }
    const auto before = parent.write_revision(data, 64);
    std::atomic<unsigned> ready{};
    std::atomic<bool> start{};
    std::array<std::exception_ptr, 4> failures;
    std::array<std::uint64_t, 4> values{};
    std::array<std::thread, 4> threads;
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i] = std::thread([&, i] {
            ready.fetch_add(1);
            while (!start.load()) std::this_thread::yield();
            try {
                const std::array<std::uint64_t, 4> args{
                    data + i * 16, i + 1, iterations, 0};
                values[i] = engines[i]->invoke_callback(image.entry(), args);
            } catch (...) { failures[i] = std::current_exception(); }
        });
    }
    while (ready.load() != threads.size()) std::this_thread::yield();
    start.store(true);
    for (auto& thread : threads) thread.join();
    for (std::size_t i = 0; i < threads.size(); ++i) {
        if (failures[i]) std::rethrow_exception(failures[i]);
        expect(values[i] == 0 && parent.read64(data + i * 16 + 8) == i + 1,
               "parallel native scalar store/load lost its value");
    }
    const auto concurrency = parent.native_concurrency_stats();
    expect(before && parent.write_revision(data, 64) ==
               *before + iterations * threads.size(),
           "parallel native journals lost or duplicated store revisions");
    OverlapCheck overlap("native store workers");
    overlap.observe(concurrency.peak_shared_read_leases >= 2,
                    "leases=" + std::to_string(concurrency.shared_read_leases) + " peak=" +
                        std::to_string(concurrency.peak_shared_read_leases));
    overlap.finish();
}

void test_concurrent_native_scalar_widths() {
    struct Form { unsigned bytes; std::uint32_t load; std::uint32_t store; };
    OverlapCheck overlap("native scalar width workers");
    for (const auto form : {
             Form{1, 0x39400003U, 0x39000001U},
             Form{2, 0x79400003U, 0x79000001U},
             Form{4, 0xb9400003U, 0xb9000001U},
             Form{8, 0xf9400003U, 0xf9000001U}}) {
        auto image = transmute::PeImage::parse(executable({
            form.load, // reader: ldr[b/h] x3,[x0]
            0xca010064U, // eor x4,x3,x1
            0xb4000064U, // cbz x4,+12: first complete pattern
            0xeb05009fU, // cmp x4,x5: complement mask
            0x540000a1U, // b.ne +20: torn value
            0xd1000442U, // sub x2,x2,#1
            0xb5ffff42U, // cbnz x2,-24
            0xd2800000U, 0xd65f03c0U,
            0xd2800020U, 0xd65f03c0U,
            form.store, // writer: str[b/h] x1,[x0]
            form.store + 2U, // str[b/h] x3,[x0]
            0xd1000442U,
            0xb5ffffa2U, // cbnz x2,-12
            0xd65f03c0U
        }));
        transmute::ReferenceEngine parent(image);
        parent.set_jit_enabled(true);
        constexpr std::uint64_t data = 0x503030000ULL;
        constexpr std::uint64_t iterations = 50'000;
        const auto mask = form.bytes == 8 ? UINT64_MAX
            : (1ULL << (form.bytes * 8)) - 1;
        const auto first = 0xaaaaaaaaaaaaaaaaULL & mask;
        const auto second = 0x5555555555555555ULL & mask;
        parent.map_memory(data, 4096);
        parent.write64(data, first);
        std::array<std::unique_ptr<transmute::ReferenceEngine>, 4> engines;
        for (auto& worker : engines) worker = parent.fork_thread(image.entry(), 0);
        const auto writer_pc = image.entry() + 44;
        for (std::size_t i = 0; i < engines.size(); ++i) {
            const std::array<std::uint64_t, 6> args{
                data, first, 64, i == 0 ? second : 0, 0, mask};
            const auto pc = i == 0 ? writer_pc : image.entry();
            const auto value = engines[i]->invoke_callback(pc, args);
            expect((i == 0 || value == 0) &&
                       engines[i]->jit_stats().compiled_native_regions != 0,
                   "parallel width fixture did not warm native memory");
            engines[i]->set_jit_profiling(true);
        }
        const auto before = parent.write_revision(data, 8);
        std::atomic<unsigned> ready{};
        std::atomic<bool> start{};
        std::array<std::exception_ptr, 4> failures;
        std::array<std::uint64_t, 4> values{};
        std::array<std::thread, 4> threads;
        for (std::size_t i = 0; i < threads.size(); ++i) {
            threads[i] = std::thread([&, i] {
                ready.fetch_add(1);
                while (!start.load()) std::this_thread::yield();
                try {
                    const std::array<std::uint64_t, 6> args{
                        data, first, iterations, i == 0 ? second : 0, 0, mask};
                    values[i] = engines[i]->invoke_callback(
                        i == 0 ? writer_pc : image.entry(), args);
                } catch (...) { failures[i] = std::current_exception(); }
            });
        }
        while (ready.load() != threads.size()) std::this_thread::yield();
        start.store(true);
        for (auto& thread : threads) thread.join();
        for (std::size_t i = 0; i < threads.size(); ++i) {
            if (failures[i]) std::rethrow_exception(failures[i]);
            expect(i == 0 || values[i] == 0,
                   "parallel scalar native load observed a torn width" +
                       std::to_string(form.bytes));
        }
        expect(before && parent.write_revision(data, 8) == *before + iterations * 2 &&
                   parent.try_read_contiguous(data, form.bytes) == second,
               "parallel scalar width execution lost stores or revisions");
        overlap.observe(parent.native_concurrency_stats().peak_shared_read_leases >= 2,
                        "width " + std::to_string(form.bytes));
    }
    overlap.finish();
}

void test_shared_native_scalar_alignment_retries() {
    struct Form { unsigned bytes; std::uint32_t load; std::uint32_t store; };
    for (const auto form : {
             Form{1, 0x39400003U, 0x39000001U},
             Form{2, 0x79400003U, 0x79000001U},
             Form{4, 0xb9400003U, 0xb9000001U},
             Form{8, 0xf9400003U, 0xf9000001U}}) {
        auto image = transmute::PeImage::parse(executable({
            form.load, form.store, 0xd1000442U, 0xb5ffffa2U, 0xd65f03c0U}));
        transmute::ReferenceEngine engine(image);
        engine.set_jit_enabled(true);
        constexpr std::uint64_t aligned = 0x503040000ULL;
        engine.map_memory(aligned, 4096);
        const std::array<std::uint64_t, 3> warm{aligned, 42, 64};
        expect(engine.invoke_callback(image.entry(), warm) == aligned &&
                   engine.jit_stats().compiled_native_regions != 0,
               "alignment retry fixture did not warm its native scalar owner");
        engine.set_jit_profiling(true);
        for (const unsigned variant : {0U, 1U, 2U}) {
            // Variant1 misaligns the guest address. Variant2 keeps the guest
            // naturally aligned while skewing its offset into host storage.
            const auto base = aligned + (variant + 1) * 0x1000 +
                (variant == 2 && form.bytes != 1 ? 1U : 0U);
            engine.map_memory(base, 64);
            const auto address = base +
                (variant == 1 && form.bytes != 1 ? 1U :
                 variant == 2 && form.bytes != 1 ? form.bytes - 1U : 0U);
            const auto before = engine.write_revision(base, 64);
            const auto stats = engine.jit_stats();
            const auto steps = engine.jit_executed_instructions();
            constexpr std::uint64_t iterations = 32;
            const std::array<std::uint64_t, 3> args{address, 42, iterations};
            expect(engine.invoke_callback(image.entry(), args) == address &&
                       engine.try_read_contiguous(address, form.bytes) == 42 &&
                       before && engine.write_revision(base, 64) == *before + iterations &&
                       engine.jit_executed_instructions() - steps == iterations * 4 + 1,
                   "shared scalar retry duplicated a prefix, store revision or guest step");
            const auto serial_exits = engine.jit_stats().native_serialized_access_exits -
                stats.native_serialized_access_exits;
            expect((form.bytes == 1 || variant == 0) ? serial_exits == 0
                                                    : serial_exits != 0,
                   "shared native scalar guard failed guest or host alignment classification");
        }
    }
}

void test_shared_native_store_reservation_commit() {
    for (const std::uint64_t reserved_offset : {0ULL, 16ULL}) {
        auto image = transmute::PeImage::parse(executable({
            0xc85ffc81U, 0xc803fc82U, 0xd65f03c0U,
            0xf9000001U, // worker: str x1,[x0]
            0xd1000442U,
            0xb5ffffc2U,
            0xd65f03c0U
        }));
        transmute::ReferenceEngine parent(image);
        parent.set_jit_enabled(true);
        constexpr std::uint64_t data = 0x503050000ULL;
        parent.map_memory(data, 64);
        parent.write64(data + reserved_offset, 13);
        auto worker = parent.fork_thread(image.entry() + 12, data);
        const std::array<std::uint64_t, 3> warm{data, 42, 64};
        expect(worker->invoke_callback(image.entry() + 12, warm) == data &&
                   worker->jit_stats().compiled_native_regions != 0,
               "shared store reservation fixture did not warm native code");
        worker->set_jit_profiling(true);
        parent.set_initial_register(4, data + reserved_offset);
        parent.set_initial_register(2, 99);
        expect_failure([&] { static_cast<void>(parent.run(1)); },
                       "guest exceeded instruction limit");
        std::exception_ptr failure;
        std::thread thread([&] {
            try {
                const std::array<std::uint64_t, 3> args{data, 42, 130};
                static_cast<void>(worker->invoke_callback(image.entry() + 12, args));
            } catch (...) { failure = std::current_exception(); }
        });
        thread.join();
        if (failure) std::rethrow_exception(failure);
        static_cast<void>(parent.run());
        expect(parent.state().x[3] == (reserved_offset == 0 ? 1U : 0U) &&
                   parent.read64(data + reserved_offset) ==
                       (reserved_offset == 0 ? 42U : 99U) &&
                   parent.native_concurrency_stats().shared_read_leases != 0,
               "shared native journal commit lost a monitor invalidation or affected an unrelated granule");
    }
}

void test_shared_native_store_checked_fallbacks() {
    auto image = transmute::PeImage::parse(executable({
        0xf9000001U, // str x1,[x0]
        0xf9400003U, // ldr x3,[x0]
        0xf9000081U, // str x1,[x4]
        0xd65f03c0U
    }));
    transmute::ReferenceEngine engine(image);
    engine.set_jit_enabled(true);
    constexpr std::uint64_t data = 0x503060000ULL;
    constexpr std::uint64_t boundary = data + 0x2000;
    constexpr std::uint64_t value = 0x8877665544332211ULL;
    engine.map_memory(data, 64);
    const std::array<std::uint64_t, 5> warm{data, 42, 0, 0, data + 8};
    expect(engine.invoke_callback(image.entry(), warm) == data &&
               engine.jit_stats().compiled_native_regions != 0,
           "shared checked fallback fixture did not warm native code");
    engine.map_memory(boundary, 4);
    engine.write32(boundary, 0);
    engine.write64(data, 0);
    const auto before = engine.write_revision(data, 8);
    engine.set_initial_register(0, data);
    engine.set_initial_register(1, value);
    engine.set_initial_register(4, boundary);
    expect_failure([&] { static_cast<void>(engine.run()); },
                   "guest write outside mapped memory");
    expect(engine.state().pc == image.entry() + 8 &&
               engine.state().x[3] == value && engine.read64(data) == value &&
               engine.read32(boundary) == 0x44332211U && before &&
               engine.write_revision(data, 8) == *before + 1 &&
               engine.write_revision(boundary, 4) == *before + 5,
           "shared store exit replayed its prefix or changed the checked partial-fault contract");

    auto write_only_image = transmute::PeImage::parse(executable({
        0xf9000001U, 0xd1000442U, 0xb5ffffc2U, 0xd65f03c0U}));
    transmute::ReferenceEngine write_only(write_only_image);
    write_only.set_jit_enabled(true);
    write_only.map_memory(data, 64);
    const std::array<std::uint64_t, 3> warm_write{data, 42, 64};
    expect(write_only.invoke_callback(write_only_image.entry(), warm_write) == data,
           "write-only fallback fixture did not warm its store owner");
    const auto writable_before = write_only.write_revision(data, 8);
    write_only.set_memory_access(data, false, true, false);
    const std::array<std::uint64_t, 3> write_args{data, value, 32};
    expect(write_only.invoke_callback(write_only_image.entry(), write_args) == data,
           "write-only native fallback failed its checked store");
    write_only.set_memory_access(data, true, true, false);
    expect(writable_before && write_only.read64(data) == value &&
               write_only.write_revision(data, 8) == *writable_before + 32,
           "write-only fallback lost or repeated stores after shared lease rejection");
}

void test_shared_native_lse_contention() {
    using transmute::jit_detail::NativeLseOperation;
    OverlapCheck overlap("translated LSE workers");
    for (const auto operation : {NativeLseOperation::add, NativeLseOperation::eor,
                                 NativeLseOperation::compare_swap}) {
        for (const auto width : {1U, 2U, 4U, 8U}) {
            const auto opcode = native_lse_test_opcode(operation, width, 3, 1, 2, 0);
            const auto load = width == 1 ? 0x39400001U : width == 2 ? 0x79400001U
                : width == 4 ? 0xb9400001U : 0xf9400001U;
            const bool cas = operation == NativeLseOperation::compare_swap;
            const auto words = cas ? std::vector<std::uint32_t>{
                load, 0x91000422U, 0xaa0103e5U, opcode,
                0xeb05003fU, 0x54ffff61U, // retry failed CAS without charging a success
                0x8b050084U, 0xd1000463U, 0xb5ffff03U,
                0xaa0403e0U, 0xd65f03c0U}
                : std::vector<std::uint32_t>{
                    opcode, 0x8b020084U, 0xd1000463U, 0xb5ffffa3U,
                    0xaa0403e0U, 0xd65f03c0U};
            auto image = transmute::PeImage::parse(executable(words));
            transmute::ReferenceEngine parent(image);
            parent.set_jit_enabled(true);
            constexpr std::uint64_t data = 0x503070000ULL;
            constexpr std::uint64_t iterations = 4'003;
            parent.map_memory(data, 64);
            std::array<std::unique_ptr<transmute::ReferenceEngine>, 4> engines;
            for (auto& worker : engines) worker = parent.fork_thread(image.entry(), 0);
            for (auto& worker : engines) {
                const std::array<std::uint64_t, 5> args{data, 1, 0, 16, 0};
                static_cast<void>(worker->invoke_callback(image.entry(), args));
                expect(worker->jit_stats().compiled_native_regions != 0,
                       "shared LSE fixture did not warm actual translated atomics");
                worker->set_jit_profiling(true);
            }
            parent.write64(data, 0);
            const auto before = parent.write_revision(data, width);
            std::atomic<unsigned> ready{};
            std::atomic<bool> start{};
            std::array<std::thread, 4> threads;
            std::array<std::uint64_t, 4> sums{};
            std::array<std::exception_ptr, 4> failures;
            for (std::size_t i = 0; i < threads.size(); ++i) {
                threads[i] = std::thread([&, i] {
                    ready.fetch_add(1);
                    while (!start.load()) std::this_thread::yield();
                    try {
                        const std::array<std::uint64_t, 5> args{
                            data, 1, 0, iterations, 0};
                        sums[i] = engines[i]->invoke_callback(image.entry(), args);
                    } catch (...) { failures[i] = std::current_exception(); }
                });
            }
            while (ready.load() != threads.size()) std::this_thread::yield();
            start.store(true);
            for (auto& thread : threads) thread.join();
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < threads.size(); ++i) {
                if (failures[i]) std::rethrow_exception(failures[i]);
                sum += sums[i];
            }
            constexpr std::uint64_t total = iterations * 4;
            const auto modulus = width < 8 ? 1ULL << (width * 8) : 0;
            const auto remainder = modulus == 0 ? total : total % modulus;
            const auto expected_sum = operation == NativeLseOperation::eor ? total / 2
                : remainder * (remainder - 1) / 2 + (modulus == 0 ? 0
                    : total / modulus * (modulus * (modulus - 1) / 2));
            const auto expected_final = operation == NativeLseOperation::eor ? 0 : remainder;
            expect(parent.try_read_contiguous(data, width) == expected_final &&
                       sum == expected_sum && before &&
                       parent.write_revision(data, width) == *before + total,
                   "shared guest LSE lost an update, old value, CAS outcome or journal revision");
            overlap.observe(parent.native_concurrency_stats().peak_shared_read_leases >= 2,
                            "width " + std::to_string(width));
        }
    }
    overlap.finish();
}

void test_shared_native_lse_monitor_outcomes() {
    using transmute::jit_detail::NativeLseOperation;
    for (const bool match : {false, true}) {
        const auto cas = native_lse_test_opcode(
            NativeLseOperation::compare_swap, 8, 3, 0, 1, 2);
        auto image = transmute::PeImage::parse(executable({
            0xc85ffc81U, 0xc803fc82U, 0xd65f03c0U, cas, 0xd65f03c0U}));
        transmute::ReferenceEngine parent(image);
        parent.set_jit_enabled(true);
        constexpr std::uint64_t data = 0x503080000ULL;
        parent.map_memory(data, 32);
        auto worker = parent.fork_thread(image.entry() + 12, 0);
        const std::array<std::uint64_t, 3> warm{0, 13, data};
        static_cast<void>(worker->invoke_callback(image.entry() + 12, warm));
        expect(worker->jit_stats().compiled_native_regions != 0,
               "CAS monitor fixture did not warm native execution");
        worker->set_jit_profiling(true);
        parent.write64(data, 13);
        const auto before = parent.write_revision(data, 8);
        const auto leases = parent.native_concurrency_stats().shared_read_leases;
        parent.set_initial_register(4, data);
        parent.set_initial_register(2, 99);
        expect_failure([&] { static_cast<void>(parent.run(1)); },
                       "guest exceeded instruction limit");
        std::exception_ptr failure;
        std::thread thread([&] {
            try {
                const std::array<std::uint64_t, 3> args{match ? 13U : 14U, 13, data};
                expect(worker->invoke_callback(image.entry() + 12, args) == 13,
                       "shared CAS did not return the original value");
            } catch (...) { failure = std::current_exception(); }
        });
        thread.join();
        if (failure) std::rethrow_exception(failure);
        expect(before && parent.write_revision(data, 8) == *before + (match ? 1U : 0U) &&
                   parent.native_concurrency_stats().shared_read_leases > leases,
               "equal-value shared CAS missed history or failed shared CAS dirtied history");
        static_cast<void>(parent.run());
        expect(parent.state().x[3] == (match ? 1U : 0U) &&
                   parent.read64(data) == (match ? 13U : 99U),
               "shared CAS monitor outcome missed successful equal-value write or failed-CAS cleanliness");
    }
}

void test_shared_native_lse_plain_store_interference() {
    using transmute::jit_detail::NativeLseOperation;
    const auto cas = native_lse_test_opcode(
        NativeLseOperation::compare_swap, 8, 3, 1, 2, 0);
    auto image = transmute::PeImage::parse(executable({
        0xd2800041U, cas, 0xf100043fU, 0x540000a8U, // mismatch must return only0/1
        0xd1000463U, 0xb5ffff63U, 0xd2800000U, 0xd65f03c0U,
        0xd2800020U, 0xd65f03c0U,
        0xf9000001U, 0xf9000002U, 0xd1000463U, 0xb5ffffa3U, 0xd65f03c0U
    }));
    transmute::ReferenceEngine parent(image);
    parent.set_jit_enabled(true);
    constexpr std::uint64_t data = 0x503090000ULL;
    constexpr std::uint64_t iterations = 10'003;
    parent.map_memory(data, 64);
    std::array<std::unique_ptr<transmute::ReferenceEngine>, 4> engines;
    for (auto& worker : engines) worker = parent.fork_thread(image.entry(), 0);
    const auto writer_pc = image.entry() + 40;
    for (std::size_t i = 0; i < engines.size(); ++i) {
        const std::array<std::uint64_t, 4> args{data, 0, i < 2 ? 1U : 3U, 16};
        const auto value = engines[i]->invoke_callback(
            i < 2 ? writer_pc : image.entry(), args);
        expect((i < 2 || value == 0) &&
                   engines[i]->jit_stats().compiled_native_regions != 0,
               "plain store/LSE fixture did not warm native owners");
        engines[i]->set_jit_profiling(true);
    }
    parent.write64(data, 0);
    const auto before = parent.write_revision(data, 8);
    std::atomic<unsigned> ready{};
    std::atomic<bool> start{};
    std::array<std::thread, 4> threads;
    std::array<std::uint64_t, 4> values{};
    std::array<std::exception_ptr, 4> failures;
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i] = std::thread([&, i] {
            ready.fetch_add(1);
            while (!start.load()) std::this_thread::yield();
            try {
                const std::array<std::uint64_t, 4> args{
                    data, 0, i < 2 ? 1U : 3U, iterations};
                values[i] = engines[i]->invoke_callback(
                    i < 2 ? writer_pc : image.entry(), args);
            } catch (...) { failures[i] = std::current_exception(); }
        });
    }
    while (ready.load() != threads.size()) std::this_thread::yield();
    start.store(true);
    for (auto& thread : threads) thread.join();
    for (std::size_t i = 0; i < threads.size(); ++i) {
        if (failures[i]) std::rethrow_exception(failures[i]);
        expect(i < 2 || values[i] == 0,
               "failed CAS observed a torn ordinary store or wrote its replacement");
    }
    expect(before && parent.write_revision(data, 8) == *before + iterations * 4 &&
               parent.read64(data) <= 1,
           "ordinary-store/LSE interference dirtied failed-CAS history");
    OverlapCheck overlap("ordinary-store and LSE workers");
    overlap.observe(parent.native_concurrency_stats().peak_shared_read_leases >= 2);
    overlap.finish();
}
