// Runtime fixtures for profiling-only native memory refill diagnostics.

transmute::JitSpanRefillStats native_refills(const transmute::ReferenceEngine& engine,
                                            bool store) {
    const auto stats = engine.jit_stats();
    return store ? stats.native_store_refills : stats.native_read_refills;
}

void expect_native_refill_accounting(const transmute::JitSpanRefillStats& stats) {
    std::uint64_t causes = 0, rejections = 0;
    for (const auto count : stats.causes) causes += count;
    for (const auto count : stats.rejections) rejections += count;
    expect(causes == stats.successful + stats.rejected && rejections == stats.rejected &&
               stats.rejections[static_cast<std::size_t>(
                   transmute::NativeSpanRefillRejection::none)] == 0,
           "native refill diagnostic totals do not conserve attempts and failures");
}

void test_native_span_refill_miss_causes() {
    constexpr std::uint64_t data = 0x50b600000ULL;
    for (const bool store : {false, true}) {
        const auto entries = store ? transmute::NativeWriteTable::entry_count
                                   : transmute::NativeReadTable::entry_count;
        const auto collided = data + entries * 4096;
        auto image = transmute::PeImage::parse(executable({
            store ? 0xf9000001U : 0xf9400001U,
            store ? 0xf9000001U : 0xf9400001U,
            0xd65f03c0U
        }));
        transmute::ReferenceEngine engine(image);
        engine.set_jit_enabled(true);
        engine.set_jit_profiling(true);
        engine.map_memory(data, 16);
        engine.map_memory(data + 16, 16);
        engine.map_memory(collided, 16);
        const auto invoke = [&](std::uint64_t address) {
            const std::array<std::uint64_t, 2> arguments{address, 0x1122334455667788ULL};
            expect(engine.invoke_callback(image.entry(), arguments) == address,
                   "native refill diagnostics changed a guest load/store result");
        };
        const auto cause = [](const auto& stats, transmute::NativeSpanMissKind kind) {
            return stats.causes[static_cast<std::size_t>(kind)];
        };
        invoke(data);
        const auto cold = native_refills(engine, store);
        expect(cold.successful == 1 && cold.rejected == 0 &&
                   cause(cold, transmute::NativeSpanMissKind::empty) == 1,
               "first native span refill was not diagnosed as a cold empty slot");
        invoke(data);
        expect(native_refills(engine, store).successful == cold.successful,
               "a warmed native span spuriously counted a refill");
        invoke(collided);
        const auto collision = native_refills(engine, store);
        expect(collision.successful == cold.successful + 1 &&
                   cause(collision, transmute::NativeSpanMissKind::collision) == 1,
               "same-index disjoint span replacement was not diagnosed as a collision");
        invoke(data);
        engine.set_memory_access(data, false, false, false);
        engine.set_memory_access(data, true, true, false);
        const auto before_stale = native_refills(engine, store);
        invoke(data);
        const auto stale = native_refills(engine, store);
        expect(stale.successful == before_stale.successful + 1 &&
                   cause(stale, transmute::NativeSpanMissKind::stale) ==
                       cause(before_stale, transmute::NativeSpanMissKind::stale) + 1,
               "topology-cleared native span was incorrectly diagnosed as cold empty");
        invoke(data + 12);
        const auto boundary = native_refills(engine, store);
        expect(cause(boundary, transmute::NativeSpanMissKind::range) >
                   cause(stale, transmute::NativeSpanMissKind::range) &&
                   boundary.rejections[static_cast<std::size_t>(
                       transmute::NativeSpanRefillRejection::boundary)] != 0,
               "a checked cross-mapping width fallback lost its range diagnosis");
        expect_native_refill_accounting(boundary);
    }
}

void test_native_span_refill_rejections() {
    constexpr std::uint64_t data = 0x50b700000ULL;
    using Rejection = transmute::NativeSpanRefillRejection;
    for (const bool store : {false, true}) {
        for (const auto rejection : {Rejection::unmapped, Rejection::permissions,
                                    Rejection::external, Rejection::boundary,
                                    Rejection::image, Rejection::executable}) {
            if (!store && (rejection == Rejection::image ||
                           rejection == Rejection::executable)) continue;
            auto bytes = executable({
                store ? 0xf9000001U : 0xf9400001U,
                store ? 0xf9000001U : 0xf9400001U,
                0xd65f03c0U
            });
            if (rejection == Rejection::image)
                put32(bytes, 0x188 + 36, 0xe0000020U);
            auto image = transmute::PeImage::parse(bytes);
            transmute::ReferenceEngine engine(image);
            engine.set_jit_enabled(true);
            engine.set_jit_profiling(true);
            std::array<std::uint8_t, 16> external{};
            auto target = data;
            std::optional<transmute::PeImage> staged;
            if (rejection == Rejection::image) {
                auto staged_bytes = executable({0xd65f03c0U});
                put16(staged_bytes, 0x96, 0x2000);
                put64(staged_bytes, 0x98 + 24, 0x180000000ULL);
                put32(staged_bytes, 0x188 + 36, 0xe0000020U);
                staged.emplace(transmute::PeImage::parse(staged_bytes));
                engine.stage_module(*staged);
                target = staged->entry() + 0x100;
            } else if (rejection == Rejection::external) {
                engine.map_external_memory(data, external.data(), external.size(), true);
            } else if (rejection == Rejection::boundary) {
                engine.map_memory(data, 4);
                engine.map_memory(data + 4, 12);
            } else if (rejection != Rejection::unmapped && rejection != Rejection::image) {
                engine.map_memory(data, 16);
                if (rejection == Rejection::permissions)
                    engine.set_memory_access(data, store, !store, false);
                if (rejection == Rejection::executable)
                    engine.set_memory_access(data, true, true, true);
            }
            const std::array<std::uint64_t, 2> arguments{target, 0x0102030405060708ULL};
            const auto invoke = [&] {
                static_cast<void>(engine.invoke_callback(image.entry(), arguments));
            };
            if (rejection == Rejection::unmapped) {
                expect_failure(invoke, store ? "guest write outside mapped memory"
                                             : "guest read outside mapped memory");
            } else if (rejection == Rejection::permissions) {
                expect_failure(invoke, store ? "guest write to read-only mapped memory"
                                             : "guest read from inaccessible mapped memory");
            } else {
                invoke();
                if (store)
                    expect(engine.read64(target) == arguments[1],
                           "rejected native refill changed the checked store fallback");
            }
            const auto stats = native_refills(engine, store);
            expect(stats.rejected != 0 &&
                       stats.rejections[static_cast<std::size_t>(rejection)] != 0,
                   "native refill rejection did not identify its checked fallback reason");
            expect_native_refill_accounting(stats);
        }
    }
}
