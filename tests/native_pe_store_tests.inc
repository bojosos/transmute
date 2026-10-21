// Included inside test_main.cpp's anonymous namespace on Windows x64.

std::vector<std::uint8_t> native_pe_data_fixture(
    std::vector<std::uint32_t> instructions,
    std::uint32_t data_flags = 0xc0000040U) {
    auto bytes = executable(std::move(instructions));
    bytes.resize(0x800);
    put16(bytes, 0x86, 3);
    put32(bytes, 0x98 + 56, 0x3000);
    for (const unsigned index : {1U, 2U}) {
        const auto section = 0x188 + index * 40;
        bytes[section] = '.';
        bytes[section + 1] = index == 1 ? 'd' : 'r';
        put32(bytes, section + 8, 0x200);
        put32(bytes, section + 12, 0x2000 + (index - 1) * 0x200);
        put32(bytes, section + 16, 0x200);
        put32(bytes, section + 20, 0x400 + (index - 1) * 0x200);
        put32(bytes, section + 36, index == 1 ? data_flags : 0x40000040U);
    }
    return bytes;
}

void test_native_pe_store_widths_budgets() {
    struct Form { unsigned width; std::uint32_t opcode; };
    constexpr std::uint64_t value = 0x8877665544332211ULL;
    for (const auto form : {Form{1, 0x39000001U}, Form{2, 0x79000001U},
                            Form{4, 0xb9000001U}, Form{8, 0xf9000001U}}) {
        auto image = transmute::PeImage::parse(native_pe_data_fixture({
            form.opcode, 0xd1000442U, 0xb5ffffc2U, 0xd65f03c0U}));
        transmute::ReferenceEngine engine(image);
        engine.set_jit_enabled(true);
        engine.set_jit_profiling(true);
        const auto data = image.base() + 0x2000;
        engine.set_initial_register(0, data);
        engine.set_initial_register(1, value);
        engine.set_initial_register(2, 130);
        expect_failure([&] { static_cast<void>(engine.run(1)); },
                       "guest exceeded instruction limit");
        const auto mask = form.width == 8 ? UINT64_MAX : (1ULL << (form.width * 8)) - 1;
        expect(engine.state().pc == image.entry() + 4 && engine.state().x[2] == 130 &&
                   engine.read64(data) == (value & mask) && !engine.write_revision(data, 8),
               "native PE store lost its exact budget commit or changed PE revision tracking");
        static_cast<void>(engine.run());
        const auto first = engine.jit_stats();
        expect(first.native_store_refills.successful == 1 &&
                   first.native_store_refills.rejected == 0 && first.native_journal_exits != 0,
               "published PE data stores did not reuse one successful span or fill the journal");
        const std::array<std::uint64_t, 3> args{data + 16, value, 130};
        expect(engine.invoke_callback(image.entry(), args) == data + 16 &&
                   engine.read64(data + 16) == (value & mask) &&
                   engine.jit_stats().compiled_blocks == first.compiled_blocks &&
                   engine.jit_stats().native_store_refills.successful == 1 &&
                   engine.native_concurrency_stats().shared_read_leases != 0,
               "warm PE data writes invalidated code, replayed a store or lost shared admission");
    }
}

void test_native_pe_store_permissions_fault_prefix() {
    constexpr std::uint64_t value = 0x8877665544332211ULL;
    for (const bool jit : {false, true}) {
        auto image = transmute::PeImage::parse(native_pe_data_fixture({
            0xf9000001U, 0xf9400003U, 0xf9000081U, 0xd65f03c0U}));
        transmute::ReferenceEngine engine(image);
        engine.set_jit_enabled(jit);
        engine.set_jit_profiling(jit);
        const auto data = image.base() + 0x2000;
        engine.set_initial_register(0, data);
        engine.set_initial_register(1, value);
        engine.set_initial_register(4, data + 0x1fc);
        expect_failure([&] { static_cast<void>(engine.run()); },
                       "guest write to read-only PE memory");
        expect(engine.state().pc == image.entry() + 8 && engine.state().x[3] == value &&
                   engine.read64(data) == value && engine.read32(data + 0x1fc) == 0x44332211U &&
                   engine.read32(data + 0x200) == 0,
               "native PE store boundary exit replayed its prefix or changed partial-fault bytes");
        if (jit) {
            expect(engine.jit_stats().native_store_refills.successful != 0 &&
                       engine.jit_stats().native_store_refills.rejections[static_cast<std::size_t>(
                           transmute::NativeSpanRefillRejection::boundary)] != 0,
                   "PE store boundary failure bypassed precise span admission");
        }
    }
    for (const auto flags : {0x40000040U, 0xe0000040U}) {
        auto image = transmute::PeImage::parse(native_pe_data_fixture({
            0xf9000001U, 0xd65f03c0U}, flags));
        transmute::ReferenceEngine engine(image);
        engine.set_jit_enabled(true);
        engine.set_jit_profiling(true);
        const auto data = image.base() + 0x2000;
        const std::array<std::uint64_t, 2> args{data, value};
        if (flags == 0x40000040U) {
            expect_failure([&] { static_cast<void>(engine.invoke_callback(image.entry(), args)); },
                           "guest write to read-only PE memory");
            expect(engine.read64(data) == 0,
                   "read-only PE span rejection committed a forbidden store");
        } else {
            expect(engine.invoke_callback(image.entry(), args) == data &&
                       engine.read64(data) == value,
                   "writable executable PE memory lost the checked store path");
        }
        const auto rejection = flags == 0x40000040U
            ? transmute::NativeSpanRefillRejection::permissions
            : transmute::NativeSpanRefillRejection::executable;
        expect(engine.jit_stats().native_store_refills.successful == 0 &&
                   engine.jit_stats().native_store_refills.rejections[
                       static_cast<std::size_t>(rejection)] != 0,
               "PE write admission failed to reject read-only or executable backing");
    }
}

void test_native_pe_store_staged_publication() {
    constexpr std::uint64_t module_base = 0x180000000ULL;
    auto image = transmute::PeImage::parse(executable({0xf9000001U, 0xd65f03c0U}));
    transmute::ReferenceEngine engine(image);
    engine.set_jit_enabled(true);
    engine.set_jit_profiling(true);
    auto bytes = native_pe_data_fixture({0xd65f03c0U});
    put16(bytes, 0x96, 0x2000);
    put64(bytes, 0x98 + 24, module_base);
    auto module = transmute::PeImage::parse(bytes);
    const auto data = module_base + 0x2000;
    engine.stage_module(module);
    const std::array<std::uint64_t, 2> initial{data, 42};
    expect(engine.invoke_callback(image.entry(), initial) == data &&
               engine.read64(data) == 42 && engine.jit_stats().native_store_refills.successful == 0 &&
               engine.jit_stats().native_store_refills.rejections[static_cast<std::size_t>(
                   transmute::NativeSpanRefillRejection::image)] != 0,
           "private staged PE data escaped checked store admission");
    engine.commit_staged_modules();
    const std::array<std::uint64_t, 2> published{data, 43};
    expect(engine.invoke_callback(image.entry(), published) == data &&
               engine.read64(data) == 43 && engine.jit_stats().native_store_refills.successful == 1,
           "published DLL data did not become a stable native write span");
    auto worker = engine.fork_thread(image.entry(), data);
    worker->set_jit_profiling(true);
    const std::array<std::uint64_t, 2> worker_args{data, 44};
    expect(worker->invoke_callback(image.entry(), worker_args) == data && engine.read64(data) == 44 &&
               worker->jit_stats().native_store_refills.successful == 1 &&
               worker->jit_stats().native_store_refills.rejected == 0,
           "worker PE data journal could not use the published module identity");
}

void test_native_pe_store_reservation_history() {
    for (const std::uint64_t reserved_offset : {0ULL, 16ULL, 32ULL}) {
        for (const bool net_zero : {false, true}) {
            auto image = transmute::PeImage::parse(native_pe_data_fixture({
                0xc85ffc81U, 0xc803fc82U, 0xd65f03c0U,
                0xf9000001U, 0xf9000002U, 0xd65f03c0U}));
            transmute::ReferenceEngine engine(image);
            engine.set_jit_enabled(true);
            const auto data = image.base() + 0x2000;
            engine.write64(data + reserved_offset, 42);
            engine.set_initial_register(4, data + reserved_offset);
            engine.set_initial_register(2, 99);
            expect_failure([&] { static_cast<void>(engine.run(1)); },
                           "guest exceeded instruction limit");
            auto worker = engine.fork_thread(image.entry() + 12, data + 15);
            worker->set_jit_profiling(true);
            // Both identical writes and a pair returning bytes to their original
            // value must invalidate reservations in each touched granule.
            const auto original = engine.read64(data + 15);
            const std::array<std::uint64_t, 3> args{
                data + 15, net_zero ? original ^ UINT64_MAX : original, original};
            expect(worker->invoke_callback(image.entry() + 12, args) == data + 15 &&
                       engine.read64(data + 15) == original &&
                       worker->jit_stats().native_store_refills.successful == 1 &&
                       worker->jit_stats().native_store_refills.rejected == 0,
                   "native PE net-zero store fixture did not execute admitted image stores");
            static_cast<void>(engine.run());
            expect(engine.state().x[3] == (reserved_offset < 32 ? 1U : 0U) &&
                       (reserved_offset != 32 || engine.read64(data + 32) == 99),
                   "native PE journal lost net-zero write history or touched an unrelated reservation");
        }
    }
}

void test_native_pe_store_shared_atomic_contention() {
    auto image = transmute::PeImage::parse(native_pe_data_fixture({
        0xb8210003U, // ldadd w1,w3,[x0]
        0xd1000442U, 0xb5ffffc2U, 0xd65f03c0U}));
    transmute::ReferenceEngine parent(image);
    parent.set_jit_enabled(true);
    const auto data = image.base() + 0x2000;
    constexpr unsigned workers = 4, iterations = 3001;
    std::array<std::unique_ptr<transmute::ReferenceEngine>, workers> engines;
    for (auto& engine : engines) {
        engine = parent.fork_thread(image.entry(), data);
        engine->set_jit_profiling(true);
        const std::array<std::uint64_t, 3> warm{data, 1, 2};
        static_cast<void>(engine->invoke_callback(image.entry(), warm));
    }
    parent.write32(data, 0);
    std::atomic<bool> start{};
    std::array<std::thread, workers> threads;
    std::array<std::exception_ptr, workers> failures;
    for (unsigned index = 0; index < workers; ++index) {
        threads[index] = std::thread([&, index] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            try {
                const std::array<std::uint64_t, 3> args{data, 1, iterations};
                static_cast<void>(engines[index]->invoke_callback(image.entry(), args));
            } catch (...) { failures[index] = std::current_exception(); }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    expect(parent.read32(data) == workers * iterations && parent.read32(data + 4) == 0,
           "shared native PE atomic stores lost updates or changed adjacent data");
    for (const auto& engine : engines) {
        expect(engine->jit_stats().native_store_refills.rejected == 0 &&
                   engine->jit_stats().native_serialized_access_exits == 0 &&
                   engine->native_concurrency_stats().shared_read_leases != 0,
               "published PE LSE stores did not retain the native shared path");
    }
}
