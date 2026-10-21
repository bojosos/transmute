// Included after the native-region helpers on Windows x64.
void test_native_span_guard_unsigned_bounds() {
    struct Form { unsigned bytes; std::uint32_t load; std::uint32_t store; };
    constexpr std::array forms{
        Form{1, 0x39400001U, 0x39000001U},
        Form{2, 0x79400001U, 0x79000001U},
        Form{4, 0xb9400001U, 0xb9000001U},
        Form{8, 0xf9400001U, 0xf9000001U},
        Form{16, 0x3dc00001U, 0x3d800001U},
        Form{32, 0xad400801U, 0xad000801U}, // Q1,Q2
    };
    for (const auto base : {0x50e000040ULL, 0xfffffffffffff040ULL}) {
        for (const auto form : forms) {
            for (const bool store : {false, true}) {
                for (const unsigned variant : {0U, 1U, 2U, 3U}) {
                    if (variant == 3 && form.bytes == 1) continue;
                    const auto code = executable({store ? form.store : form.load,
                        0xd65f03c0U, store ? 0x39000001U : 0x39400001U,
                        0xd65f03c0U});
                    auto ri = transmute::PeImage::parse(code);
                    auto ni = transmute::PeImage::parse(code);
                    transmute::ReferenceEngine reference(ri), native(ni);
                    native.set_jit_enabled(true);
                    native.set_jit_profiling(true);
                    const auto size = variant == 3 ? form.bytes - 1U : 64U;
                    for (auto* engine : {&reference, &native}) {
                        engine->map_memory(base, size);
                        engine->set_initial_vector(1, {0x0123456789abcdefULL,
                                                      0xfedcba9876543210ULL});
                        engine->set_initial_vector(2, {UINT64_MAX, 0});
                    }
                    // Fill the selected entry before testing its lower/upper
                    // bound, or before subtracting a width larger than size.
                    const std::array<std::uint64_t, 2> warm{base, 0};
                    static_cast<void>(native.invoke_callback(
                        ni.entry() + (variant == 3 ? 8U : 0U), warm));
                    const auto address = variant == 0 ? base - 1 :
                        variant == 1 ? base + size - form.bytes :
                        variant == 2 ? base + size - form.bytes + 1 : base;
                    for (auto* engine : {&reference, &native}) {
                        for (unsigned i = 0; i != size; ++i)
                            static_cast<transmute::GuestMemory&>(*engine).write8(
                                base + i, static_cast<std::uint8_t>(i * 37U + 11U));
                        engine->set_initial_register(0, address);
                        engine->set_initial_register(1, 0x8877665544332211ULL);
                    }
                    if (variant == 1) {
                        const auto expected = reference.run();
                        const auto actual = native.run();
                        expect(actual.instructions == expected.instructions,
                               "exact-end native span changed its instruction count");
                    } else {
                        for (auto* engine : {&reference, &native})
                            expect_failure([&] { static_cast<void>(engine->run()); },
                                store ? "guest write outside mapped memory"
                                      : "guest read outside mapped memory");
                    }
                    expect(native_region_same_state(native.state(), reference.state()),
                           "unsigned native span bounds changed fault or result state");
                    for (unsigned i = 0; i != size; ++i)
                        expect(static_cast<transmute::GuestMemory&>(native).read8(base + i) ==
                                   static_cast<transmute::GuestMemory&>(reference).read8(base + i),
                               "native span bounds changed successful prefix bytes");
                }
            }
        }
    }
}
