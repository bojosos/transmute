// Included after BridgeCopyMemory: it retains the prior checked chunk path as
// an independent oracle while exact ReferenceEngine uses pinned bulk extents.

void test_guest_bulk_memory_extents() {
    auto actual_image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    auto oracle_image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine actual(actual_image);
    BridgeCopyMemory oracle(oracle_image);
    constexpr std::uint64_t base = 0x504010000ULL;
    for (transmute::GuestMemory* memory : {static_cast<transmute::GuestMemory*>(&actual),
                                          static_cast<transmute::GuestMemory*>(&oracle)}) {
        memory->map_memory(base, 13);
        memory->map_memory(base + 13, 11);
        memory->map_memory(base + 24, 104);
        memory->map_memory(base + 256, 128);
    }
    std::array<std::uint8_t, 128> initial{};
    for (std::size_t i = 0; i < initial.size(); ++i)
        initial[i] = static_cast<std::uint8_t>(i * 37 + 11);
    for (const auto count : {0U, 1U, 7U, 8U, 9U, 15U, 16U, 17U, 31U, 64U, 103U}) {
        for (const auto alignment : {0U, 1U, 7U}) {
            actual.write_bytes(base, initial);
            oracle.write_bytes(base, initial);
            actual.fill_bytes(base + alignment, 0xb3, count);
            oracle.fill_bytes(base + alignment, 0xb3, count);
            actual.copy_bytes(base + 256 + alignment, base + alignment, count);
            oracle.copy_bytes(base + 256 + alignment, base + alignment, count);
            std::array<std::uint8_t, 128> expected{}, observed{};
            actual.read_bytes(base, observed);
            oracle.read_bytes(base, expected);
            expect(observed == expected, "bulk mapped extent fill differs from checked chunks");
            actual.read_bytes(base + 256, observed);
            oracle.read_bytes(base + 256, expected);
            expect(observed == expected, "bulk mapped extent copy differs from checked chunks");
            for (const auto offset : {0ULL, 13ULL, 24ULL, 256ULL})
                expect(actual.write_revision(base + offset, 1) ==
                           oracle.write_revision(base + offset, 1),
                       "bulk extent operation changed the existing dirty revision counts");
        }
    }
    for (const bool move : {false, true}) {
        for (const int shift : {-17, -8, -7, -1, 0, 1, 7, 8, 17}) {
            actual.write_bytes(base, initial);
            oracle.write_bytes(base, initial);
            actual.copy_bytes(base + 32 + shift, base + 32, 63, move);
            oracle.copy_bytes(base + 32 + shift, base + 32, 63, move);
            std::array<std::uint8_t, 128> expected{}, observed{};
            actual.read_bytes(base, observed);
            oracle.read_bytes(base, expected);
            expect(observed == expected, "bulk overlap changed forward propagation or backward move");
        }
    }
    std::array<std::uint8_t, 4> code{};
    actual.read_bytes(actual_image.entry(), code);
    expect(get32(std::vector<std::uint8_t>(code.begin(), code.end()), 0) == 0xd65f03c0U,
           "bulk snapshot could not read a PE image extent");
    actual.read_bytes(UINT64_MAX, {});
    actual.write_bytes(UINT64_MAX, {});
    actual.fill_bytes(UINT64_MAX, 0, 0);
    actual.copy_bytes(UINT64_MAX, UINT64_MAX, 0, true);
}

void test_guest_bulk_memory_faults_and_aliases() {
    constexpr std::uint64_t base = 0x504020000ULL;
    struct Case { unsigned source, destination, count; bool move, readable, writable, mapped; };
    for (const auto test : {
        Case{0, 32, 24, false, false, true, true},
        Case{32, 0, 24, false, true, false, true},
        Case{0, 32, 24, false, true, true, false},
        Case{0, 8, 24, true, false, true, true},
        Case{0, 8, 24, true, true, false, true},
        Case{0, 8, 32, true, true, true, false},
        Case{8, 0, 24, true, false, true, true}}) {
        auto actual_image = transmute::PeImage::parse(executable({0xd65f03c0U}));
        auto oracle_image = transmute::PeImage::parse(executable({0xd65f03c0U}));
        transmute::ReferenceEngine actual(actual_image);
        BridgeCopyMemory oracle(oracle_image);
        std::array<std::uint8_t, 64> observed{}, expected{};
        for (std::size_t i = 0; i < expected.size(); ++i)
            observed[i] = expected[i] = static_cast<std::uint8_t>(i * 29 + 7);
        const auto map = [&](transmute::GuestMemory& memory, auto& bytes) {
            memory.map_external_memory(base, bytes.data(), 13, true);
            if (test.mapped) {
                memory.map_external_memory(base + 13, bytes.data() + 13, 11, true);
                memory.set_memory_access(base + 13, test.readable, test.writable, false);
            }
            memory.map_external_memory(base + 24, bytes.data() + 24, 40, true);
        };
        map(actual, observed);
        map(oracle, expected);
        std::string actual_fault, oracle_fault;
        try { actual.copy_bytes(base + test.destination, base + test.source, test.count, test.move); }
        catch (const std::runtime_error& error) { actual_fault = error.what(); }
        try { oracle.copy_bytes(base + test.destination, base + test.source, test.count, test.move); }
        catch (const std::runtime_error& error) { oracle_fault = error.what(); }
        expect(actual_fault == oracle_fault && observed == expected,
               "pinned bulk copy changed first fault or completed prefix/suffix");
    }
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine actual(image);
    std::array<std::uint8_t, 13> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i + 1);
    actual.map_external_memory(base, bytes.data(), bytes.size(), true);
    std::array<std::uint8_t, 24> output{};
    output.fill(0xcc);
    expect_failure([&] { actual.read_bytes(base, output); }, "guest read outside mapped memory");
    expect(std::equal(bytes.begin(), bytes.end(), output.begin()) && output[13] == 0xcc,
           "faulting bulk snapshot lost its successful prefix");
    output.fill(0xa5);
    expect_failure([&] { actual.write_bytes(base, output); }, "guest write outside mapped memory");
    expect(std::all_of(bytes.begin(), bytes.end(), [](auto byte) { return byte == 0xa5; }),
           "faulting bulk host write lost its successful prefix");
    // Distinct guest mappings may alias the same host allocation at arbitrary
    // offsets. Their architectural direction is based on guest addresses.
    for (const bool move : {false, true}) {
        auto a_image = transmute::PeImage::parse(executable({0xd65f03c0U}));
        auto o_image = transmute::PeImage::parse(executable({0xd65f03c0U}));
        transmute::ReferenceEngine a(a_image);
        BridgeCopyMemory o(o_image);
        std::array<std::uint8_t, 65> observed{}, expected{};
        for (std::size_t i = 0; i < expected.size(); ++i)
            observed[i] = expected[i] = static_cast<std::uint8_t>(i);
        a.map_external_memory(base, observed.data(), 64, true);
        a.map_external_memory(base + 4096, observed.data() + 1, 64, true);
        o.map_external_memory(base, expected.data(), 64, true);
        o.map_external_memory(base + 4096, expected.data() + 1, 64, true);
        a.copy_bytes(base + 4096, base, 63, move);
        o.copy_bytes(base + 4096, base, 63, move);
        expect(observed == expected, "bulk copy changed arbitrary external alias chunk snapshots");
    }
}

void test_guest_bulk_memory_strings() {
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine engine(image);
    constexpr std::uint64_t base = 0x504030000ULL;
    engine.map_memory(base, 3);
    engine.map_memory(base + 3, 5);
    const std::array<std::uint8_t, 8> narrow{'a', 'b', 'c', 'd', 'e', 'f', 'g', 0};
    engine.write_bytes(base, narrow);
    bool terminated = false;
    expect(engine.read_string(base, 1, 32768, &terminated) ==
               std::vector<std::uint8_t>(narrow.begin(), narrow.end() - 1) && terminated,
           "bulk string scanner read beyond a boundary terminator");
    expect(engine.read_string(base, 1, 3, &terminated) ==
               std::vector<std::uint8_t>({'a', 'b', 'c'}) && !terminated,
           "bulk bounded string scanner changed its limit");
    expect(engine.string_length(base, 1, 32768, &terminated) == 7 && terminated,
           "allocation-free bulk string length changed boundary termination");
    engine.map_memory(base + 4096, 3);
    engine.map_memory(base + 4099, 3);
    const std::array<std::uint8_t, 6> wide{'a', 0, 'b', 0, 0, 0};
    engine.write_bytes(base + 4096, wide);
    expect(engine.read_string(base + 4096, 2, 32768, &terminated) ==
               std::vector<std::uint8_t>({'a', 0, 'b', 0}) && terminated,
           "bulk UTF16 scanner changed a character split across mappings");
    expect(engine.string_length(base + 4096, 2, 32768, &terminated) == 2 && terminated,
           "allocation-free UTF16 length changed a split character");
    engine.set_memory_access(base + 4099, false, false, false);
    expect_failure([&] { static_cast<void>(engine.read_string(base + 4096, 2, 32768)); },
                   "guest read from inaccessible mapped memory");
    engine.map_memory(base + 8192, 1);
    engine.map_memory(base + 8193, 1);
    engine.set_memory_access(base + 8193, false, false, false);
    expect_failure([&] { static_cast<void>(engine.read_string(base + 8192, 2, 1)); },
                   "guest read from inaccessible mapped memory");
    expect(engine.read_string(UINT64_MAX, 2, 0, &terminated).empty() && !terminated,
           "zero-length string span accessed an invalid address");
    transmute::Win32Bridge bridge;
    transmute::CpuState state{};
    state.x[0] = base;
    static_cast<void>(bridge.dispatch({"api-ms-win-crt-string-l1-1-0.dll", "strlen", 0}, state, engine));
    expect(state.x[0] == 7, "CRT strlen did not use the bounded bulk string semantics");
    state.x[0] = base;
    state.x[1] = 3;
    static_cast<void>(bridge.dispatch({"api-ms-win-crt-string-l1-1-0.dll", "strnlen", 0}, state, engine));
    expect(state.x[0] == 3, "CRT strnlen read beyond its bound");
}

void test_guest_bulk_memory_history_and_callbacks() {
    constexpr std::uint64_t base = 0x504040000ULL;
    for (const auto count : {0U, 1U, 7U, 8U, 9U, 15U, 16U, 17U, 257U}) {
        auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
        transmute::ReferenceEngine engine(image);
        engine.map_memory(base, 512);
        const auto before = engine.write_revision(base, 1);
        engine.fill_bytes(base, 0x55, count);
        expect(before && engine.write_revision(base, 1) == *before + count / 8 + count % 8,
               "bulk fill changed logical successful chunk revisions");
        const auto identical = engine.write_revision(base, 1);
        engine.fill_bytes(base, 0x55, count);
        expect(engine.write_revision(base, 1) == *identical + count / 8 + count % 8,
               "identical bulk fill failed to advance dirty identity");
    }
    for (const std::uint64_t offset : {0ULL, 16ULL}) {
        auto image = transmute::PeImage::parse(executable({
            0xc85ffc81U, 0xc803fc82U, 0xd65f03c0U}));
        transmute::ReferenceEngine engine(image);
        engine.set_jit_enabled(true);
        engine.map_memory(base, 64);
        engine.write64(base + offset, 13);
        engine.set_initial_register(4, base + offset);
        engine.set_initial_register(2, 99);
        expect_failure([&] { static_cast<void>(engine.run(1)); }, "guest exceeded instruction limit");
        engine.fill_bytes(base, 0x2a, 8);
        static_cast<void>(engine.run());
        expect(engine.state().x[3] == (offset == 0 ? 1U : 0U),
               "bulk write lost an exclusive monitor invalidation or changed another granule");
    }
    auto code = executable({0xd2800020U, 0xd65f03c0U});
    put32(code, 0x188 + 36, 0xe0000020U);
    auto image = transmute::PeImage::parse(code);
    transmute::ReferenceEngine engine(image);
    engine.set_jit_enabled(true);
    expect(engine.run().exit_code == 1, "bulk PE invalidation fixture did not warm code");
    const std::array<std::uint8_t, 4> replacement{0x40, 0x05, 0x80, 0xd2};
    engine.write_bytes(image.entry(), replacement);
    expect(engine.invoke_callback(image.entry(), 0) == 42,
           "bulk PE write retained a stale native region");
    BridgeCopyMemory custom(image);
    custom.map_memory(base, 512);
    custom.chunks_enabled = false;
    custom.fill_bytes(base, 0x41, 31);
    custom.reset_counts();
    std::array<std::uint8_t, 31> bytes{};
    custom.read_bytes(base, bytes);
    custom.write_bytes(base + 128, bytes);
    expect(custom.byte_reads == 31 && custom.byte_writes == 31 &&
               custom.read_chunks == 0 && custom.write_chunks == 0,
           "bulk memory bypassed overridden custom byte callbacks");
}

void test_guest_bulk_memory_snapshot_concurrency() {
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine parent(image);
    constexpr std::uint64_t base = 0x504050000ULL;
    constexpr std::size_t extent = 256 * 1024;
    parent.map_memory(base, extent);
    auto worker = parent.fork_thread(image.entry(), 0);
    std::atomic<bool> start{};
    std::exception_ptr failure;
    std::thread writer([&] {
        while (!start.load()) std::this_thread::yield();
        try {
            for (unsigned iteration = 0; iteration < 64; ++iteration)
                worker->fill_bytes(base, iteration % 2 ? 0x55 : 0xaa, extent);
        } catch (...) { failure = std::current_exception(); }
    });
    std::vector<std::uint8_t> snapshot(extent);
    bool coherent = true;
    start.store(true);
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
        parent.read_bytes(base, snapshot);
        const auto value = snapshot.front();
        coherent = coherent && (value == 0 || value == 0x55 || value == 0xaa) &&
            std::all_of(snapshot.begin(), snapshot.end(), [value](auto byte) { return byte == value; });
    }
    writer.join();
    if (failure) std::rethrow_exception(failure);
    expect(coherent, "bulk snapshot released its backing lease in the middle of a host transfer");
}

void benchmark_guest_bulk_memory() {
    constexpr std::uint64_t base = 0x504060000ULL;
    constexpr std::size_t extent = 1024 * 1024;
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    auto checked_image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine bulk(image);
    BridgeCopyMemory checked(checked_image);
    for (transmute::GuestMemory* memory : {static_cast<transmute::GuestMemory*>(&bulk),
                                          static_cast<transmute::GuestMemory*>(&checked)}) {
        memory->map_memory(base, extent * 2);
        memory->fill_bytes(base, 0x55, extent * 2);
    }
    std::vector<std::uint8_t> snapshot(extent);
    for (const char* operation : {"read", "write", "copy", "fill", "string"}) {
        for (transmute::GuestMemory* memory : {static_cast<transmute::GuestMemory*>(&checked),
                                              static_cast<transmute::GuestMemory*>(&bulk)}) {
            const auto started = std::chrono::steady_clock::now();
            for (unsigned iteration = 0; iteration < 4; ++iteration) {
                const std::string_view name(operation);
                if (name == "read") memory->read_bytes(base, snapshot);
                else if (name == "write") memory->write_bytes(base + extent, snapshot);
                else if (name == "copy") memory->copy_bytes(base + extent, base, extent);
                else if (name == "fill") memory->fill_bytes(base + extent, 0xaa, extent);
                else static_cast<void>(memory->string_length(base, 1, extent));
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            std::cout << "bulk-memory," << operation << ','
                      << (memory == &bulk ? "pinned" : "checked8") << ',' << extent * 4
                      << ',' << elapsed << '\n';
        }
    }
}
