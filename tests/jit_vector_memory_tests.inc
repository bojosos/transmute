// Included inside test_main.cpp's anonymous namespace.

void test_jit_native_vector_memory_forms() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t data = 0x500900000ULL;
    // Each addressing mode is checked against the reference engine for all
    // scalar vector widths and Q, in both directions and with aliased Vd/Vn.
    for (const bool load : {false, true}) {
        for (unsigned width_index = 0; width_index != 5; ++width_index) {
            const auto size = width_index == 4 ? 0U : width_index;
            const auto quad = width_index == 4 ? 0x00800000U : 0U;
            for (unsigned form = 0; form != 5; ++form) {
                const auto base = form == 0 ? 0x3d000000U :
                                  form == 4 ? 0x3c200800U : 0x3c000000U;
                const auto addressing = form == 0 ? 2U << 10 :
                    form == 4 ? (9U << 16) | (3U << 13) | 0x1000U :
                    (3U << 12) | (form == 2 ? 0xc00U : form == 3 ? 0x400U : 0);
                const auto word = base | (size << 30) | quad |
                    (load ? 0x00400000U : 0U) | addressing | (8U << 5) | 8U;
                const auto bytes = executable({word, 0xd65f03c0U});
                auto reference_image = transmute::PeImage::parse(bytes);
                auto native_image = transmute::PeImage::parse(bytes);
                transmute::ReferenceEngine reference(reference_image);
                transmute::ReferenceEngine native(native_image);
                for (auto* engine : {&reference, &native}) {
                    engine->map_memory(data, 256);
                    auto& memory = static_cast<transmute::GuestMemory&>(*engine);
                    for (unsigned index = 0; index != 256; ++index) {
                        memory.write8(data + index,
                            static_cast<std::uint8_t>(index * 37U + 11U));
                    }
                    engine->set_initial_register(8, data + 64);
                    engine->set_initial_register(9, 2);
                    for (unsigned index = 0; index != 32; ++index) {
                        engine->set_initial_vector(index,
                            {0x0123456789abcdefULL + index,
                             0xfedcba9876543210ULL - index});
                    }
                }
                native.set_jit_enabled(true);
                native.set_jit_profiling(true);
                const auto expected = reference.run();
                const auto actual = native.run();
                expect(actual.instructions == expected.instructions &&
                           native_region_same_state(native.state(), reference.state()),
                       "native vector addressing changed lanes, upper bits or base writeback");
                for (unsigned offset = 0; offset != 256; offset += 8) {
                    expect(native.read64(data + offset) == reference.read64(data + offset),
                           "native vector store changed bytes outside its exact width");
                }
                expect(native.jit_stats().native_region_instructions == 2,
                       "supported vector memory form remained in the interpreter");
            }
        }
    }
    for (const bool load : {false, true}) {
        for (unsigned size = 0; size != 3; ++size) {
            for (const unsigned mode : {1U, 2U, 3U}) {
                const auto word = 0x2c000000U | (size << 30) | (mode << 23) |
                    (load ? 0x00400000U : 0U) | (1U << 15) |
                    (1U << 10) | (8U << 5);
                auto reference_image = transmute::PeImage::parse(
                    executable({word, 0xd65f03c0U}));
                auto native_image = transmute::PeImage::parse(
                    executable({word, 0xd65f03c0U}));
                transmute::ReferenceEngine reference(reference_image);
                transmute::ReferenceEngine native(native_image);
                for (auto* engine : {&reference, &native}) {
                    engine->map_memory(data, 128);
                    for (unsigned offset = 0; offset != 128; offset += 8) {
                        engine->write64(data + offset, 0x1111111111111111ULL * (offset + 1));
                    }
                    engine->set_initial_register(8, data + 32);
                    engine->set_initial_vector(0, {0x123456789abcdef0ULL, UINT64_MAX});
                    engine->set_initial_vector(1, {0xfedcba9876543210ULL, 0});
                }
                native.set_jit_enabled(true);
                native.set_jit_profiling(true);
                const auto expected = reference.run();
                const auto actual = native.run();
                expect(actual.instructions == expected.instructions &&
                           native_region_same_state(native.state(), reference.state()),
                       "native vector pair changed registers or pair writeback");
                for (unsigned offset = 0; offset != 128; offset += 8) {
                    expect(native.read64(data + offset) == reference.read64(data + offset),
                           "native vector pair changed the wrong memory extent");
                }
                expect(native.jit_stats().native_region_instructions == 2,
                       "supported vector pair remained in checked fallback");
            }
        }
    }
#endif
}

void test_jit_native_vector_pair_faults() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t data = 0x500a00000ULL;
    for (const bool load : {false, true}) {
        const auto word = (load ? 0xad400000U : 0xad000000U) |
            (1U << 10) | (8U << 5);
        auto reference_image = transmute::PeImage::parse(
            executable({word, 0xd65f03c0U}));
        auto native_image = transmute::PeImage::parse(
            executable({word, 0xd65f03c0U}));
        transmute::ReferenceEngine reference(reference_image);
        transmute::ReferenceEngine native(native_image);
        for (auto* engine : {&reference, &native}) {
            engine->map_memory(data, 20);
            engine->write64(data, 42);
            engine->write64(data + 8, 43);
            engine->write32(data + 16, 44);
            engine->set_initial_register(8, data);
            engine->set_initial_vector(0, {0x0123456789abcdefULL, UINT64_MAX});
            engine->set_initial_vector(1, {0xfedcba9876543210ULL, 0x1122334455667788ULL});
        }
        native.set_jit_enabled(true);
        for (auto* engine : {&reference, &native}) {
            expect_failure([&] { static_cast<void>(engine->run()); },
                           load ? "guest read outside mapped memory"
                                : "guest write outside mapped memory");
        }
        expect(native_region_same_state(native.state(), reference.state()) &&
                   native.state().pc == native_image.entry() &&
                   native.read64(data) == reference.read64(data) &&
                   native.read64(data + 8) == reference.read64(data + 8) &&
                   native.read32(data + 16) == reference.read32(data + 16),
               "native Q pair fault lost checked partial register or memory effects");
    }
#endif
}
