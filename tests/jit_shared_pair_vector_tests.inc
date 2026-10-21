// Included inside test_main.cpp's anonymous namespace on Windows x64.

struct SharedPairVectorForm {
    unsigned bytes;
    bool pair;
    bool vector;
    std::uint32_t load;
    std::uint32_t store;
};

constexpr std::array shared_pair_vector_forms{
    SharedPairVectorForm{4, true, false, 0x29401404U, 0x29001404U}, // W4,W5
    SharedPairVectorForm{8, true, false, 0xa9401404U, 0xa9001404U}, // X4,X5
    SharedPairVectorForm{1, false, true, 0x3d400004U, 0x3d000004U}, // B4
    SharedPairVectorForm{2, false, true, 0x7d400004U, 0x7d000004U}, // H4
    SharedPairVectorForm{4, false, true, 0xbd400004U, 0xbd000004U}, // S4
    SharedPairVectorForm{8, false, true, 0xfd400004U, 0xfd000004U}, // D4
    SharedPairVectorForm{4, true, true, 0x2d401404U, 0x2d001404U}, // S4,S5
    SharedPairVectorForm{8, true, true, 0x6d401404U, 0x6d001404U}, // D4,D5
    SharedPairVectorForm{16, false, true, 0x3dc00004U, 0x3d800004U}, // Q4
    SharedPairVectorForm{16, true, true, 0xad401404U, 0xad001404U}, // Q4,Q5
};

void test_shared_pair_vector_admission_alignment() {
    constexpr std::uint64_t data = 0x50d000000ULL;
    constexpr std::uint64_t iterations = 130;
    for (const auto form : shared_pair_vector_forms) {
        for (const unsigned variant : {0U, 1U, 2U}) {
            const auto words = executable({form.load, form.store,
                0xd1000463U, 0xb5ffffa3U, 0xd65f03c0U});
            auto ri = transmute::PeImage::parse(words);
            auto ni = transmute::PeImage::parse(words);
            transmute::ReferenceEngine reference(ri), native(ni);
            native.set_jit_enabled(true);
            for (auto* engine : {&reference, &native}) engine->map_memory(data, 64);
            const std::array<std::uint64_t, 4> warm{data, 0, 0, 64};
            static_cast<void>(native.invoke_callback(ni.entry(), warm));
            native.set_jit_profiling(true);
            // Pair elements need their own alignment. Exercise Q at the
            // baseline 8-byte alignment as well as unaligned guest/host
            // addresses, while its whole owner remains exclusive.
            const auto alignment = std::min(form.bytes, 8U);
            const auto base = data + 0x1000 +
                (variant == 2 && alignment > 1 ? 1U : 0U);
            const auto address = base + (variant == 0 ? alignment :
                variant == 1 && alignment > 1 ? 1U :
                variant == 2 && alignment > 1 ? alignment - 1U : 0U);
            for (auto* engine : {&reference, &native}) {
                engine->map_memory(base, 64);
                for (unsigned offset = 0; offset != 64; ++offset)
                    static_cast<transmute::GuestMemory&>(*engine).write8(base + offset,
                        static_cast<std::uint8_t>(offset * 37U + 11U));
                engine->set_initial_register(0, address);
                engine->set_initial_register(3, iterations);
                engine->set_initial_vector(4, {UINT64_MAX, UINT64_MAX});
                engine->set_initial_vector(5, {UINT64_MAX, UINT64_MAX});
            }
            const auto before = native.write_revision(base, 64);
            const auto leases = native.native_concurrency_stats().shared_read_leases;
            const auto serial = native.jit_stats().native_serialized_access_exits;
            const auto expected = reference.run();
            const auto actual = native.run();
            expect(actual.instructions == expected.instructions &&
                       native_region_same_state(native.state(), reference.state()) &&
                       before && native.write_revision(base, 64) ==
                           *before + iterations * (form.pair ? 2U : 1U),
                   "shared pair/SIMD alignment retry lost state, steps or store history");
            for (unsigned offset = 0; offset != 64; offset += 8)
                expect(native.read64(base + offset) == reference.read64(base + offset),
                       "shared pair/SIMD memory changed adjacent bytes");
            const auto exits = native.jit_stats().native_serialized_access_exits - serial;
            if (form.bytes == 16) {
                expect(native.native_concurrency_stats().shared_read_leases == leases &&
                           exits == 0,
                       "Q owner entered shared admission or retried alignment after exclusion");
            } else {
                expect(native.native_concurrency_stats().shared_read_leases > leases &&
                           ((variant == 0 || form.bytes == 1) ? exits == 0 : exits != 0),
                       "pair/SIMD owner failed shared admission or per-element alignment fallback");
            }
        }
    }
    for (const auto form : shared_pair_vector_forms) {
        if (form.bytes != 16) continue;
        constexpr unsigned complete_steps = iterations * 4 + 1;
        for (const unsigned budget : {1U, 2U, 3U, 4U, 5U, 6U, 7U,
                                     255U, 256U, 257U, 519U, 520U}) {
            const auto bytes = executable({form.load, form.store,
                0xd1000463U, 0xb5ffffa3U, 0xd65f03c0U});
            auto ri = transmute::PeImage::parse(bytes);
            auto ni = transmute::PeImage::parse(bytes);
            transmute::ReferenceEngine reference(ri), native(ni);
            native.set_jit_enabled(true);
            for (auto* engine : {&reference, &native}) {
                engine->map_memory(data, 64);
                for (unsigned offset = 0; offset != 64; offset += 8)
                    engine->write64(data + offset, 0x0123456789abcdefULL + offset);
            }
            const std::array<std::uint64_t, 4> warm{data + 8, 0, 0, 64};
            static_cast<void>(native.invoke_callback(ni.entry(), warm));
            native.set_jit_profiling(true);
            for (auto* engine : {&reference, &native}) {
                engine->set_initial_register(0, data + 8);
                engine->set_initial_register(3, iterations);
                engine->set_initial_vector(4, {UINT64_MAX, UINT64_MAX});
                engine->set_initial_vector(5, {UINT64_MAX, UINT64_MAX});
            }
            const auto before = native.write_revision(data, 64);
            const auto leases = native.native_concurrency_stats().shared_read_leases;
            const auto serial = native.jit_stats().native_serialized_access_exits;
            for (auto* engine : {&reference, &native})
                expect_failure([&] { static_cast<void>(engine->run(budget)); },
                               "guest exceeded instruction limit");
            expect(native_region_same_state(native.state(), reference.state()) && before &&
                       native.write_revision(data, 64) ==
                           *before + ((budget + 2) / 4) * (form.pair ? 2U : 1U) &&
                       native.native_concurrency_stats().shared_read_leases == leases &&
                       native.jit_stats().native_serialized_access_exits == serial,
                   "exclusive Q budget exit lost admission, exact state, aliases or one-record-per-Q history");
            const auto expected = reference.run();
            const auto actual = native.run();
            expect(actual.instructions == complete_steps - budget &&
                       actual.instructions == expected.instructions &&
                       native_region_same_state(native.state(), reference.state()) &&
                       native.write_revision(data, 64) ==
                           *before + iterations * (form.pair ? 2U : 1U),
                   "exclusive Q budget exit could not resume its exact prefix");
            for (unsigned offset = 0; offset != 64; offset += 8)
                expect(native.read64(data + offset) == reference.read64(data + offset),
                       "exclusive Q budget exit changed a vector lane or adjacent bytes");
        }
    }
    for (const auto form : shared_pair_vector_forms) {
        if (form.bytes != 16) continue;
        const auto bytes = executable({
            0x91000421U, 0xd61f0140U, // add x1,x1,#1; br x10
            form.load, form.store, 0xd65f03c0U});
        auto ri = transmute::PeImage::parse(bytes);
        auto ni = transmute::PeImage::parse(bytes);
        transmute::ReferenceEngine reference(ri), native(ni);
        native.set_jit_enabled(true);
        for (auto* engine : {&reference, &native}) {
            engine->map_memory(data, 64);
            for (unsigned offset = 0; offset != 64; offset += 8)
                engine->write64(data + offset, 0x0123456789abcdefULL + offset);
            engine->set_initial_register(0, data + 8);
            engine->set_initial_register(1, 42);
            engine->set_initial_register(10, ni.entry() + 8);
        }
        // Publish the Q target first, then the register-only source. Its
        // shared native chain must stop at the target's exclusive owner.
        const std::array<std::uint64_t, 2> warm{data + 8, 42};
        static_cast<void>(native.invoke_callback(ni.entry() + 8, warm));
        static_cast<void>(native.invoke_callback(ni.entry(), warm));
        native.set_jit_profiling(true);
        const auto before = native.write_revision(data, 64);
        const auto leases = native.native_concurrency_stats().shared_read_leases;
        const auto serial = native.jit_stats().native_serialized_access_exits;
        const auto expected = reference.run();
        const auto actual = native.run();
        expect(actual.instructions == 5 && actual.instructions == expected.instructions &&
                   native_region_same_state(native.state(), reference.state()) && before &&
                   native.write_revision(data, 64) == *before + (form.pair ? 2U : 1U) &&
                   native.native_concurrency_stats().shared_read_leases == leases + 1 &&
                   native.jit_stats().native_serialized_access_exits == serial,
               "shared register chain failed Q owner promotion or changed state, steps or history");
        for (unsigned offset = 0; offset != 64; offset += 8)
            expect(native.read64(data + offset) == reference.read64(data + offset),
                   "Q owner promotion changed a vector lane or adjacent bytes");
    }
}

void test_shared_pair_vector_element_contention() {
    constexpr std::uint64_t data = 0x50d010000ULL;
    constexpr std::uint64_t iterations = 25'000;
    for (const auto form : shared_pair_vector_forms) {
        std::vector<std::uint32_t> words{form.load};
        if (form.vector) {
            words.push_back(0x9e660084U); // fmov x4,d4; narrow loads clear upper bits.
            if (form.pair) words.push_back(0x9e6600a5U); // fmov x5,d5
            if (form.bytes == 16) {
                words.push_back(0x5e180486U); // mov d6,v4.d[1]
                words.push_back(0x9e6600c7U); // fmov x7,d6
                if (form.pair) {
                    words.push_back(0x5e1804a7U); // mov d7,v5.d[1]
                    words.push_back(0x9e6600e8U); // fmov x8,d7
                }
            }
        }
        std::vector<std::size_t> failures;
        std::vector<unsigned> checked_registers{4};
        if (form.pair) checked_registers.push_back(5);
        if (form.bytes == 16) {
            checked_registers.push_back(7);
            if (form.pair) checked_registers.push_back(8);
        }
        for (const unsigned reg : checked_registers) {
            words.push_back(0xca010000U | (reg << 5) | reg); // eor with first pattern
            words.push_back(0xb4000060U | reg); // cbz +12: first full pattern
            words.push_back(0xeb06001fU | (reg << 5)); // cmp against complement mask
            failures.push_back(words.size());
            words.push_back(0x54000001U); // b.ne failure
        }
        words.push_back(0xd1000463U); // sub x3,x3,#1
        const auto back = words.size();
        words.push_back(0xb5000003U |
            ((static_cast<std::uint32_t>(-static_cast<std::int64_t>(back)) &
              0x7ffffU) << 5));
        words.push_back(0xd2800000U);
        words.push_back(0xd65f03c0U);
        const auto fail = words.size();
        words.push_back(0xd2800020U);
        words.push_back(0xd65f03c0U);
        for (const auto offset : failures)
            words[offset] |= static_cast<std::uint32_t>(fail - offset) << 5;
        const auto writer_offset = words.size() * 4;
        if (form.vector) {
            words.push_back(form.bytes == 16 ? 0x4e080c24U : 0x9e670024U);
            if (form.pair) words.push_back(form.bytes == 16 ? 0x4e080c25U : 0x9e670025U);
        } else {
            words.push_back(0xaa0103e4U);
            words.push_back(0xaa0103e5U);
        }
        const auto writer_loop = words.size();
        words.push_back(form.store);
        if (form.vector) {
            // Q writers broadcast each pattern to both 64-bit lanes.
            words.push_back(form.bytes == 16 ? 0x4e080c44U : 0x9e670044U);
            if (form.pair) words.push_back(form.bytes == 16 ? 0x4e080c45U : 0x9e670045U);
        } else {
            words.push_back(0xaa0203e4U);
            words.push_back(0xaa0203e5U);
        }
        words.push_back(form.store);
        if (form.vector) {
            words.push_back(form.bytes == 16 ? 0x4e080c24U : 0x9e670024U);
            if (form.pair) words.push_back(form.bytes == 16 ? 0x4e080c25U : 0x9e670025U);
        } else {
            words.push_back(0xaa0103e4U);
            words.push_back(0xaa0103e5U);
        }
        words.push_back(0xd1000463U);
        const auto writer_back = words.size();
        words.push_back(0xb5000003U |
            ((static_cast<std::uint32_t>(writer_loop - writer_back) & 0x7ffffU) << 5));
        words.push_back(0xd65f03c0U);
        auto image = transmute::PeImage::parse(executable(words));
        transmute::ReferenceEngine parent(image);
        parent.set_jit_enabled(true);
        parent.map_memory(data, 64);
        const auto mask = form.bytes >= 8 ? UINT64_MAX : (1ULL << (form.bytes * 8)) - 1;
        const auto first = 0xaaaaaaaaaaaaaaaaULL & mask;
        const auto second = 0x5555555555555555ULL & mask;
        parent.write64(data, first);
        parent.write64(data + form.bytes, first);
        if (form.bytes == 16)
            for (unsigned offset = 0; offset != form.bytes * (form.pair ? 2U : 1U);
                 offset += 8) parent.write64(data + offset, first);
        std::array<std::unique_ptr<transmute::ReferenceEngine>, 4> engines;
        // Allocate all guest stacks before warming the writer. Stack creation
        // advances the shared write clock even though data is unchanged.
        for (auto& worker : engines) worker = parent.fork_thread(image.entry(), 0);
        for (std::size_t i = 0; i != engines.size(); ++i) {
            const std::array<std::uint64_t, 7> args{data, first, second, 64, 0, 0, mask};
            const auto value = engines[i]->invoke_callback(
                image.entry() + (i == 0 ? writer_offset : 0), args);
            expect(i == 0 || value == 0, "pair/SIMD contention fixture failed its warm read");
            engines[i]->set_jit_profiling(true);
        }
        const auto before = parent.write_revision(data, 32);
        const auto leases = parent.native_concurrency_stats().shared_read_leases;
        std::atomic<unsigned> ready{};
        std::atomic<bool> start{};
        std::array<std::thread, 4> threads;
        std::array<std::exception_ptr, 4> exceptions;
        std::array<std::uint64_t, 4> values{};
        for (std::size_t i = 0; i != threads.size(); ++i) {
            threads[i] = std::thread([&, i] {
                ready.fetch_add(1);
                while (!start.load()) std::this_thread::yield();
                try {
                    const std::array<std::uint64_t, 7> args{
                        data, first, second, iterations, 0, 0, mask};
                    values[i] = engines[i]->invoke_callback(
                        image.entry() + (i == 0 ? writer_offset : 0), args);
                } catch (...) { exceptions[i] = std::current_exception(); }
            });
        }
        while (ready.load() != threads.size()) std::this_thread::yield();
        start.store(true);
        for (auto& thread : threads) thread.join();
        for (std::size_t i = 0; i != threads.size(); ++i) {
            if (exceptions[i]) std::rethrow_exception(exceptions[i]);
            expect(i == 0 || values[i] == 0,
                   "shared pair/SIMD reader observed a torn scalar element");
        }
        const auto scalar_bytes = std::min(form.bytes, 8U);
        expect(before && parent.write_revision(data, 32) ==
                   *before + iterations * 2 * (form.pair ? 2U : 1U) &&
                   parent.try_read_contiguous(data, scalar_bytes) == second &&
                   (form.bytes == 16
                       ? parent.native_concurrency_stats().shared_read_leases == leases &&
                           parent.native_concurrency_stats().peak_shared_read_leases == 0
                       : parent.native_concurrency_stats().peak_shared_read_leases >= 2),
               "pair/SIMD contention after stack allocation lost history or admission: width=" +
                   std::to_string(form.bytes) + " pair=" + std::to_string(form.pair) +
                   " vector=" + std::to_string(form.vector) +
                   " before=" + std::to_string(before.value_or(0)) +
                   " after=" + std::to_string(parent.write_revision(data, 32).value_or(0)) +
                   " expected_delta=" + std::to_string(iterations * 2 * (form.pair ? 2U : 1U)) +
                   " value=" + std::to_string(parent.try_read_contiguous(data, scalar_bytes).value_or(0)) +
                   " expected_value=" + std::to_string(second) +
                   " peak=" + std::to_string(parent.native_concurrency_stats().peak_shared_read_leases));
        if (form.bytes == 16)
            for (unsigned offset = 0; offset != form.bytes * (form.pair ? 2U : 1U);
                 offset += 8)
                expect(parent.read64(data + offset) == second,
                       "exclusive Q contention lost the final pattern in a 64-bit lane");
    }
}

void test_shared_pair_vector_precise_faults() {
    constexpr std::uint64_t data = 0x50d020000ULL;
    constexpr std::uint64_t boundary = data + 0x1000;
    for (const auto form : shared_pair_vector_forms) {
        for (const bool load : {false, true}) {
            const auto words = executable({
                0x91000421U, 0xf90000e1U, // committed add/store prefix via X7
                load ? form.load : form.store, 0xd65f03c0U});
            auto ri = transmute::PeImage::parse(words);
            auto ni = transmute::PeImage::parse(words);
            transmute::ReferenceEngine reference(ri), native(ni);
            native.set_jit_enabled(true);
            for (auto* engine : {&reference, &native}) {
                engine->map_memory(data, 64);
                engine->set_initial_register(4, 0x0123456789abcdefULL);
                engine->set_initial_register(5, 0xfedcba9876543210ULL);
                engine->set_initial_vector(4, {0x0123456789abcdefULL, UINT64_MAX});
                engine->set_initial_vector(5, {0xfedcba9876543210ULL, UINT64_MAX});
            }
            const std::array<std::uint64_t, 8> warm{data, 42, 0, 0,
                0x0123456789abcdefULL, 0xfedcba9876543210ULL, 0, data + 32};
            static_cast<void>(native.invoke_callback(ni.entry(), warm));
            native.set_jit_profiling(true);
            const auto available = form.bytes * (form.pair ? 1U : 0U) + form.bytes / 2;
            for (auto* engine : {&reference, &native}) {
                if (available != 0) {
                    engine->map_memory(boundary, available);
                    for (unsigned offset = 0; offset != available; ++offset)
                        static_cast<transmute::GuestMemory&>(*engine).write8(boundary + offset, 0xcc);
                }
                engine->write64(data + 32, 0); // Latest revision belongs to prefix.
                engine->set_initial_register(0, boundary);
                engine->set_initial_register(1, 42);
                engine->set_initial_register(7, data + 32);
            }
            const auto before = native.write_revision(data + 32, 8);
            const auto leases = native.native_concurrency_stats().shared_read_leases;
            for (auto* engine : {&reference, &native})
                expect_failure([&] { static_cast<void>(engine->run()); },
                    load ? "guest read outside mapped memory" : "guest write outside mapped memory");
            expect(native_region_same_state(native.state(), reference.state()) &&
                       native.state().pc == ni.entry() + 8 &&
                       native.read64(data + 32) == 43 && before &&
                       native.write_revision(data + 32, 8) == *before + 1 &&
                       (form.bytes == 16
                           ? native.native_concurrency_stats().shared_read_leases == leases
                           : native.native_concurrency_stats().shared_read_leases > leases),
                   "pair/SIMD fault changed admission, replayed its prefix or changed exact fault state");
            for (unsigned offset = 0; offset != available; ++offset)
                expect(static_cast<transmute::GuestMemory&>(native).read8(boundary + offset) ==
                           static_cast<transmute::GuestMemory&>(reference).read8(boundary + offset),
                       "shared pair/SIMD fault lost checked partial memory effects");
        }
    }
}

void test_shared_pair_vector_monitor_commit() {
    constexpr std::uint64_t data = 0x50d030000ULL;
    for (const auto form : shared_pair_vector_forms) {
        for (const std::uint64_t reserved_offset : {0ULL, 16ULL, 32ULL}) {
            auto image = transmute::PeImage::parse(executable({
                0xc85ffc81U, 0xc803fc82U, 0xd65f03c0U,
                form.store, 0xd65f03c0U}));
            transmute::ReferenceEngine parent(image);
            parent.set_jit_enabled(true);
            parent.map_memory(data, 64);
            auto worker = parent.fork_thread(image.entry() + 12, 0);
            worker->set_initial_vector(4, {13, form.bytes == 16 ? 13U : 0U});
            worker->set_initial_vector(5, {13, form.bytes == 16 ? 13U : 0U});
            const std::array<std::uint64_t, 6> warm{data + 8, 0, 0, 0, 13, 13};
            static_cast<void>(worker->invoke_callback(image.entry() + 12, warm));
            worker->set_jit_profiling(true);
            parent.write64(data + reserved_offset, 13);
            parent.set_initial_register(4, data + reserved_offset);
            parent.set_initial_register(2, 99);
            expect_failure([&] { static_cast<void>(parent.run(1)); },
                           "guest exceeded instruction limit");
            const auto before = parent.write_revision(data, 64);
            const auto leases = parent.native_concurrency_stats().shared_read_leases;
            std::exception_ptr failure;
            std::thread thread([&] {
                try {
                    // Q and pair stores can cross several monitor granules.
                    const std::array<std::uint64_t, 6> args{data + 8, 0, 0, 0, 13, 13};
                    static_cast<void>(worker->invoke_callback(image.entry() + 12, args));
                } catch (...) { failure = std::current_exception(); }
            });
            thread.join();
            if (failure) std::rethrow_exception(failure);
            expect(before && parent.write_revision(data, 64) ==
                       *before + (form.pair ? 2U : 1U) &&
                       (form.bytes == 16
                           ? parent.native_concurrency_stats().shared_read_leases == leases
                           : parent.native_concurrency_stats().shared_read_leases > leases),
                   "pair/SIMD equal-value store changed admission or lost its journal publication");
            static_cast<void>(parent.run());
            const bool touched = reserved_offset <
                8 + form.bytes * (form.pair ? 2U : 1U);
            expect(parent.state().x[3] == (touched ? 1U : 0U) &&
                       (touched || parent.read64(data + reserved_offset) == 99),
                   "shared pair/SIMD store missed a touched monitor granule or invalidated another");
        }
    }
}
