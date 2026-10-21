// Included inside test_main.cpp's anonymous namespace after its test helpers.

bool native_region_same_state(const transmute::CpuState& left,
                              const transmute::CpuState& right) {
    return left.x == right.x && left.v == right.v && left.sp == right.sp &&
           left.pc == right.pc && left.nzcv == right.nzcv &&
           left.fpcr == right.fpcr && left.fpsr == right.fpsr;
}

void test_native_region_loop_budgets() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t object = 0x500400000ULL;
    constexpr std::uint64_t leaf = object + 0x1000;
    constexpr unsigned iterations = 67;
    constexpr unsigned complete_steps = 1 + iterations * 4 + 1;
    // The second load consumes the result of the first. Its back edge stays
    // inside the compiled region and crosses the 256-step lease yield.
    const auto bytes = executable({
        0xf9400081U, // ldr x1,[x4]
        0xf9400022U, // ldr x2,[x1]
        0x8b020000U, // add x0,x0,x2
        0xf1000463U, // subs x3,x3,#1
        0x54ffffa1U, // b.ne three instructions backward
        0xd65f03c0U,
    });
    for (const unsigned budget : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 63U,
                                   254U, 255U, 256U, 257U, 268U, 269U,
                                   complete_steps}) {
        auto reference_image = transmute::PeImage::parse(bytes);
        auto native_image = transmute::PeImage::parse(bytes);
        transmute::ReferenceEngine reference(reference_image);
        transmute::ReferenceEngine native(native_image);
        for (auto* engine : {&reference, &native}) {
            engine->map_memory(object, 16);
            engine->map_memory(leaf, 16);
            engine->write64(object, leaf);
            engine->write64(leaf, 7);
            engine->set_initial_register(0, 5);
            engine->set_initial_register(1, UINT64_MAX);
            engine->set_initial_register(2, UINT64_MAX);
            engine->set_initial_register(3, iterations);
            engine->set_initial_register(4, object);
            engine->set_initial_vector(31, {0x123456789abcdef0ULL, UINT64_MAX});
        }
        native.set_jit_enabled(true);
        native.set_jit_profiling(true);
        if (budget < complete_steps) {
            for (auto* engine : {&reference, &native}) {
                expect_failure([&] { static_cast<void>(engine->run(budget)); },
                               "guest exceeded instruction limit");
            }
            expect(native_region_same_state(native.state(), reference.state()),
                   "native loop crossed a dynamic budget or lost its side-exit state");
            const auto expected = reference.run();
            const auto actual = native.run();
            expect(actual.instructions == expected.instructions &&
                       actual.exit_code == expected.exit_code,
                   "native loop could not resume exactly after a budget exit");
        } else {
            const auto expected = reference.run(budget);
            const auto actual = native.run(budget);
            expect(actual.instructions == complete_steps &&
                       actual.instructions == expected.instructions &&
                       actual.exit_code == 5 + 7 * iterations,
                   "native internal loop did not count repeated instructions exactly");
            const auto stats = native.jit_stats(16);
            expect(std::any_of(stats.hottest_blocks.begin(), stats.hottest_blocks.end(),
                               [](const auto& block) { return block.instructions >= 5; }),
                   "dependent-load loop never entered a larger compiled region");
        }
        expect(native_region_same_state(native.state(), reference.state()),
               "native dependent-load loop changed final registers, vectors or flags");
    }
#endif
}

void test_native_region_precise_second_load_fault() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t object = 0x500500000ULL;
    constexpr std::uint64_t absent = object + 0x1000;
    const auto bytes = executable({
        0xf9400081U, // ldr x1,[x4]
        0xf9400022U, // ldr x2,[x1]
        0x8b020000U, // add x0,x0,x2
        0xd65f03c0U,
    });
    for (const bool unreadable : {false, true}) {
        auto reference_image = transmute::PeImage::parse(bytes);
        auto native_image = transmute::PeImage::parse(bytes);
        transmute::ReferenceEngine reference(reference_image);
        transmute::ReferenceEngine native(native_image);
        for (auto* engine : {&reference, &native}) {
            engine->map_memory(object, 16);
            engine->write64(object, absent);
            if (unreadable) {
                engine->map_memory(absent, 16);
                engine->set_memory_access(absent, false, false, false);
            }
            engine->set_initial_register(0, 9);
            engine->set_initial_register(1, UINT64_MAX);
            engine->set_initial_register(2, 123);
            engine->set_initial_register(4, object);
        }
        native.set_jit_enabled(true);
        for (auto* engine : {&reference, &native}) {
            expect_failure([&] { static_cast<void>(engine->run()); },
                           unreadable ? "guest read from inaccessible mapped memory"
                                      : "guest read outside mapped memory");
        }
        expect(native_region_same_state(native.state(), reference.state()) &&
                   native.state().x[1] == absent && native.state().x[2] == 123 &&
                   native.state().pc == native_image.entry() + 4,
               "native second-load fault replayed its prefix or changed the faulting destination");
        if (unreadable) native.set_memory_access(absent, true, true, false);
        else native.map_memory(absent, 16);
        native.write64(absent, 42);
        const auto resumed = native.run();
        expect(resumed.instructions == 3 && resumed.exit_code == 51,
               "native second-load fault could not resume after restoring its mapping");
    }
#endif
}

void test_native_region_pair_fault_commit() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t container = 0x500600000ULL;
    constexpr std::uint64_t object = container + 0x1000;
    // Post-index must update neither the base nor either destination when
    // the second element faults. The preceding dependent load must remain.
    const auto bytes = executable({
        0xf94000a4U, // ldr x4,[x5]
        0xa8c10881U, // ldp x1,x2,[x4],#16
        0xd65f03c0U,
    });
    auto reference_image = transmute::PeImage::parse(bytes);
    auto native_image = transmute::PeImage::parse(bytes);
    transmute::ReferenceEngine reference(reference_image);
    transmute::ReferenceEngine native(native_image);
    for (auto* engine : {&reference, &native}) {
        engine->map_memory(container, 16);
        engine->map_memory(object, 8);
        engine->write64(container, object);
        engine->write64(object, 42);
        engine->set_initial_register(1, 11);
        engine->set_initial_register(2, 22);
        engine->set_initial_register(4, UINT64_MAX);
        engine->set_initial_register(5, container);
    }
    native.set_jit_enabled(true);
    for (auto* engine : {&reference, &native}) {
        expect_failure([&] { static_cast<void>(engine->run()); },
                       "guest read outside mapped memory");
    }
    expect(native_region_same_state(native.state(), reference.state()) &&
               native.state().x[1] == 11 && native.state().x[2] == 22 &&
               native.state().x[4] == object &&
               native.state().pc == native_image.entry() + 4,
           "native pair fault committed a destination or post-index base prematurely");
    for (auto* engine : {&reference, &native}) {
        engine->map_memory(object + 8, 8);
        engine->write64(object + 8, 99);
    }
    const auto expected = reference.run();
    const auto actual = native.run();
    expect(actual.instructions == expected.instructions && actual.instructions == 2 &&
               native_region_same_state(native.state(), reference.state()) &&
               native.state().x[1] == 42 && native.state().x[2] == 99 &&
               native.state().x[4] == object + 16,
           "native pair did not use checked boundary fallback or resume its writeback exactly");
#endif
}

void test_native_region_ordered_store_load() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t data = 0x500700000ULL;
    const auto bytes = executable({
        0xf9000081U, // str x1,[x4]
        0xf9400082U, // ldr x2,[x4]
        0x91000442U, // add x2,x2,#1
        0xf9000082U, // str x2,[x4]
        0xf9400083U, // ldr x3,[x4]
        0xeb03005fU, // cmp x2,x3
        0x1a9f07e0U, // cset w0,ne
        0xd65f03c0U,
    });
    for (const std::uint64_t value : {0ULL, UINT64_MAX, 0x8877665544332211ULL}) {
        auto reference_image = transmute::PeImage::parse(bytes);
        auto native_image = transmute::PeImage::parse(bytes);
        transmute::ReferenceEngine reference(reference_image);
        transmute::ReferenceEngine native(native_image);
        for (auto* engine : {&reference, &native}) {
            engine->map_memory(data, 16);
            engine->write64(data, value ^ UINT64_MAX);
            engine->set_initial_register(1, value);
            engine->set_initial_register(4, data);
        }
        native.set_jit_enabled(true);
        native.set_jit_profiling(true);
        const auto before = native.write_revision(data, 8);
        const auto expected = reference.run();
        const auto actual = native.run();
        expect(actual.instructions == expected.instructions && actual.exit_code == 0 &&
                   native_region_same_state(native.state(), reference.state()) &&
                   native.read64(data) == value + 1 &&
                   native.write_revision(data, 8) == *before + 2,
               "native stores were reordered, hidden from subsequent loads or committed twice");
        expect(native.jit_stats().native_region_instructions == actual.instructions,
               "ordered stores/loads did not execute in a complete native region");
    }
#endif
}
