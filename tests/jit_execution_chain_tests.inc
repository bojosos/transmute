// Included after the shared native-state comparison helper.

std::uint32_t chain_test_branch(std::uint64_t from, std::uint64_t to,
                                bool link = false) {
    return (link ? 0x94000000U : 0x14000000U) |
        static_cast<std::uint32_t>((to - from) / 4 & 0x03ffffffU);
}

void initialize_execution_chain(transmute::ReferenceEngine& engine,
                                std::uint64_t code, std::uint64_t data,
                                unsigned iterations) {
    engine.map_memory(code, 4096);
    engine.set_memory_access(code, true, true, true);
    engine.map_memory(data, 16);
    // Indirect edges deliberately end each canonical region. No first
    // region can decode the loop as one internal control-flow region.
    engine.write32(code, 0x91000400U); // add x0,x0,#1
    engine.write32(code + 4, 0xd61f0140U); // br x10
    engine.write32(code + 64, 0xf9400043U); // ldr x3,[x2]
    engine.write32(code + 68, 0x8b030000U); // add x0,x0,x3
    engine.write32(code + 72, 0xd61f0160U); // br x11
    engine.write32(code + 128, 0xf9000040U); // str x0,[x2]
    engine.write32(code + 132, 0xf1000421U); // subs x1,x1,#1
    const auto displacement = static_cast<std::uint32_t>(
        (code - (code + 136)) / 4) & 0x7ffffU;
    engine.write32(code + 136, 0x54000001U | (displacement << 5));
    engine.write32(code + 140, 0xd65f03c0U);
    engine.write64(data, 2);
    engine.set_initial_register(0, 5);
    engine.set_initial_register(1, iterations);
    engine.set_initial_register(2, data);
    engine.set_initial_register(9, code);
    engine.set_initial_register(10, code + 64);
    engine.set_initial_register(11, code + 128);
}

void test_native_execution_chain_budgets_and_refills() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t code = 0x50b000000ULL;
    constexpr std::uint64_t data = code + 0x10000;
    constexpr unsigned iterations = 70;
    constexpr unsigned total = 1 + iterations * 8 + 1;
    for (const unsigned budget : {1U, 2U, 3U, 4U, 5U, 7U, 8U, 9U,
                                  63U, 64U, 255U, 256U, 257U, total}) {
        auto ri = transmute::PeImage::parse(executable({0xd61f0120U}));
        auto ni = transmute::PeImage::parse(executable({0xd61f0120U}));
        transmute::ReferenceEngine reference(ri), native(ni);
        initialize_execution_chain(reference, code, data, iterations);
        initialize_execution_chain(native, code, data, iterations);
        native.set_jit_enabled(true);
        const std::array<std::uint64_t, 3> warm{5, 2, data};
        static_cast<void>(native.invoke_callback(code, warm));
        // A topology change clears read/write span metadata without clearing
        // cached code, proving refill can resume a different chained owner.
        native.map_memory(data + 0x1000, 16);
        native.write64(data, 2); // Make this region the latest dirty identity.
        const auto before = native.jit_stats();
        const auto dirty = native.write_revision(data, 8);
        if (budget < total) {
            for (auto* engine : {&reference, &native})
                expect_failure([&] { static_cast<void>(engine->run(budget)); },
                               "guest exceeded instruction limit");
            expect(native_region_same_state(native.state(), reference.state()) &&
                       native.read64(data) == reference.read64(data),
                   "native cross-owner chain lost a precise budget state");
        }
        const auto expected = reference.run();
        const auto actual = native.run();
        const auto after = native.jit_stats();
        if (!(actual.instructions == expected.instructions &&
                   native_region_same_state(native.state(), reference.state()) &&
                   native.read64(data) == reference.read64(data) && dirty &&
                   native.write_revision(data, 8) == *dirty + iterations)) {
            std::ostringstream message;
            message << "native chain differs at budget " << budget
                    << ": resumed steps " << actual.instructions << '/' << expected.instructions
                    << ", data " << native.read64(data) << '/' << reference.read64(data)
                    << ", revision " << native.write_revision(data, 8).value_or(0)
                    << ", expected revision " << dirty.value_or(0) + iterations
                    << ", PC " << std::hex << native.state().pc << '/' << reference.state().pc;
            for (unsigned reg = 0; reg < native.state().x.size(); ++reg)
                if (native.state().x[reg] != reference.state().x[reg])
                    message << ", X" << std::dec << reg << '=' << std::hex
                            << native.state().x[reg] << '/' << reference.state().x[reg];
            throw std::runtime_error(message.str());
        }
        expect(after.native_chain_edges > before.native_chain_edges + 100 &&
                   after.native_blocks - before.native_blocks < 24,
               "cross-owner loop still returned to the C++ cache for each block");
    }
#endif
}

void test_native_execution_chain_fault_and_retry() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t code = 0x50b100000ULL;
    constexpr std::uint64_t data = code + 0x10000;
    auto ri = transmute::PeImage::parse(executable({0xd61f0120U}));
    auto ni = transmute::PeImage::parse(executable({0xd61f0120U}));
    transmute::ReferenceEngine reference(ri), native(ni);
    initialize_execution_chain(reference, code, data, 2);
    initialize_execution_chain(native, code, data, 2);
    native.set_jit_enabled(true);
    const std::array<std::uint64_t, 3> warm{5, 2, data};
    static_cast<void>(native.invoke_callback(code, warm));
    native.write64(data, 2);
    for (auto* engine : {&reference, &native}) {
        engine->set_memory_access(data, false, false, false);
        expect_failure([&] { static_cast<void>(engine->run()); },
                       "guest read");
    }
    expect(native.state().pc == code + 64 && native.state().x[0] == 6 &&
               native_region_same_state(native.state(), reference.state()),
           "chained load fault replayed or discarded its preceding owner");
    for (auto* engine : {&reference, &native})
        engine->set_memory_access(data, true, true, false);
    const auto expected = reference.run(), actual = native.run();
    expect(actual.instructions == expected.instructions &&
               native_region_same_state(native.state(), reference.state()) &&
               native.read64(data) == reference.read64(data),
           "chained fault retry restarted the initial region");
#endif
}

void test_native_execution_chain_collision_and_invalidation() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t first = 0x50b200000ULL;
    constexpr std::uint64_t second = first + 0x4000; // same chain-table slot
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine native(image);
    for (const auto code : {first, second}) {
        native.map_memory(code, 64);
        native.set_memory_access(code, true, true, true);
    }
    native.write32(first, 0x91000400U);
    native.write32(first + 4, chain_test_branch(first + 4, second));
    native.write32(second, 0x91000800U);
    native.write32(second + 4, 0xd65f03c0U);
    native.set_jit_enabled(true);
    for (unsigned iteration = 0; iteration < 4; ++iteration)
        expect(native.invoke_callback(first, 5) == 8,
               "native chain collision jumped into another PC's code");
    native.write32(second, 0x91002400U); // target changes after publication
    expect(native.invoke_callback(first, 5) == 15,
           "executable target write retained a stale direct chain address");
    native.unmap_memory(second);
    expect_failure([&] { static_cast<void>(native.invoke_callback(first, 5)); },
                   "guest attempted to execute");
    native.map_memory(second, 64);
    native.set_memory_access(second, true, true, true);
    native.write32(second, 0xd1000800U);
    native.write32(second + 4, 0xd65f03c0U);
    expect(native.invoke_callback(first, 5) == 4,
           "native chain reused a retired target after same-address remap");
#endif
}

void test_native_execution_chain_fp_state() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t code = 0x50b300000ULL;
    auto ri = transmute::PeImage::parse(executable({0xd61f0120U}));
    auto ni = transmute::PeImage::parse(executable({0xd61f0120U}));
    transmute::ReferenceEngine reference(ri), native(ni);
    for (auto* engine : {&reference, &native}) {
        engine->map_memory(code, 256);
        engine->set_memory_access(code, true, true, true);
        engine->write32(code, 0x1e621820U); // fdiv d0,d1,d2 (inexact)
        engine->write32(code + 4, 0xd61f0140U);
        engine->write32(code + 64, 0x91000400U); // pure owner in live FP session
        engine->write32(code + 68, 0xd61f0160U);
        engine->write32(code + 128, 0x1e612863U); // fadd d3,d3,d1
        engine->write32(code + 132, 0xd65f03c0U);
        engine->set_initial_register(9, code);
        engine->set_initial_register(10, code + 64);
        engine->set_initial_register(11, code + 128);
        engine->set_initial_vector(1, {0x3ff0000000000000ULL, 0});
        engine->set_initial_vector(2, {0x4008000000000000ULL, 0});
        engine->set_initial_vector(3, {0x4000000000000000ULL, 0});
    }
    native.set_jit_enabled(true);
    static_cast<void>(native.invoke_callback(code, 0));
    const auto before = native.jit_stats().native_chain_edges;
    const auto csr = _mm_getcsr();
    const auto expected = reference.run(), actual = native.run();
    expect(actual.instructions == expected.instructions &&
               native_region_same_state(native.state(), reference.state()) &&
               (native.state().fpsr & 16U) != 0 && _mm_getcsr() == csr &&
               native.jit_stats().native_chain_edges >= before + 2,
           "native chained FP session lost sticky flags or leaked host control state");
#endif
}

void test_jit_integer_region_continuity_families() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t pc = 0x5000;
    const auto check = [&](std::uint32_t opcode, unsigned destination,
                           std::uint64_t left, std::uint64_t right) {
        auto image = transmute::PeImage::parse(executable({
            0xd51b4214U, opcode, 0xd65f03c0U})); // msr nzcv,x20
        transmute::ReferenceEngine reference(image);
        reference.set_initial_register(0, left);
        reference.set_initial_register(1, right);
        reference.set_initial_register(20, 0xb0000000U);
        expect_failure([&] { static_cast<void>(reference.run(1)); },
                       "guest exceeded instruction limit");
        auto state = reference.state();
        state.pc = pc;
        const auto expected = reference.run();
        transmute::X64BlockCache cache;
        const std::array words{opcode, 0xd65f03c0U};
        expect(cache.execute(state, pc, words), "integer continuity family was not lowered");
        expect(state.x == reference.state().x && state.nzcv == reference.state().nzcv &&
                   state.sp == reference.state().sp &&
                   (destination == 31 || state.x[destination] == reference.state().x[destination]) &&
                   state.fpcr == reference.state().fpcr && state.fpsr == reference.state().fpsr &&
                   static_cast<std::uint32_t>(state.x[0]) == expected.exit_code,
               "integer continuity family changed aliases, flags or W upper bits");
    };
    for (const bool wide : {false, true}) {
        for (const bool sign : {false, true}) {
            for (const unsigned destination : {0U, 1U, 2U, 31U}) {
                const auto opcode = (wide ? 0x80000000U : 0U) |
                    (sign ? 0x1ac00c00U : 0x1ac00800U) | (1U << 16) | destination;
                for (const auto left : {0ULL, 1ULL, UINT64_MAX, 0x8000000000000000ULL,
                                       0x80000000ULL, 0xfffffffffffffff9ULL})
                    for (const auto right : {0ULL, 1ULL, 3ULL, UINT64_MAX, 0xffffffffULL})
                        check(opcode, destination, left, right);
            }
            for (const unsigned source : {0U, 31U})
                for (const unsigned second : {1U, 31U})
                    for (const unsigned destination : {0U, 1U, 31U})
                        check((wide ? 0x80000000U : 0U) |
                              (sign ? 0x1ac00c00U : 0x1ac00800U) |
                              (source << 5) | (second << 16) | destination,
                              destination, UINT64_MAX, 3);
        }
        for (const unsigned source : {0U, 31U})
            for (const auto value : {0ULL, 1ULL, UINT64_MAX, 0x8000000000000000ULL,
                                     0x100000000ULL, 0x80000000ULL})
                check((wide ? 0xdac01000U : 0x5ac01000U) | (source << 5) | 2U,
                      2, value, 0);
        for (unsigned operation = 0; operation < 4; ++operation)
            for (unsigned shift_type = 0; shift_type < 4; ++shift_type)
                for (const bool invert : {false, true})
                    for (const unsigned shift : {0U, 1U, wide ? 63U : 31U})
                        for (const unsigned destination : {0U, 1U, 31U})
                            check((wide ? 0x80000000U : 0U) | 0x0a000000U |
                                  (operation << 29) | (shift_type << 22) |
                                  (invert ? 0x200000U : 0U) | (1U << 16) |
                                  (shift << 10) | destination,
                                  destination, 0x8167432301234567ULL, 0xf123456789abcdefULL);
    }
    expect(!transmute::decode_integer_block(pc, std::array{0x0a018002U}) &&
               !transmute::decode_integer_block(pc, std::array{0x1ac01002U}) &&
               !transmute::decode_integer_block(pc, std::array{0x5ac11002U}),
           "integer continuity decoder admitted reserved encodings");
#endif
}

void test_native_retained_integer_span_aliases_and_budgets() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    const std::vector<std::uint32_t> words{
        0x8b010c00U, // add x0,x0,x1,lsl#3
        0xcac1fc01U, // eor x1,x0,x1,ror#63 (Rd=Rm)
        0x4b807c20U, // sub w0,w1,w0,asr#31 (Rd=Rm, upper clearing)
        0xaa0003e1U, // mov x1,x0
        0x72a24681U, // movk w1,#0x1234,lsl#16
        0x910043ffU, // add sp,sp,#16
        0x910017e0U, // add x0,sp,#5
        0x92403c1fU, // and sp,x0,#0xffff (logical Rd31 is SP, Rn31 would be ZR)
        0x9ac10c02U, // sdiv x2,x0,x1
        0xdac01043U, // clz x3,x2
        0xd65f03c0U,
    };
    for (unsigned budget = 1; budget <= words.size(); ++budget) {
        auto ri = transmute::PeImage::parse(executable(words));
        auto ni = transmute::PeImage::parse(executable(words));
        transmute::ReferenceEngine reference(ri), native(ni);
        for (auto* engine : {&reference, &native}) {
            engine->set_initial_register(0, 0xffff123456789abcULL);
            engine->set_initial_register(1, 0x8123456789abcdefULL);
        }
        native.set_jit_enabled(true);
        static_cast<void>(native.invoke_callback(ni.entry(),
            std::span<const std::uint64_t>{}));
        const auto compiled = native.jit_stats().compiled_native_regions;
        expect(native.jit_stats().compiled_register_spans != 0,
               "pure integer region did not retain host registers");
        if (budget < words.size()) {
            for (auto* engine : {&reference, &native})
                expect_failure([&] { static_cast<void>(engine->run(budget)); },
                               "guest exceeded instruction limit");
            expect(native_region_same_state(native.state(), reference.state()),
                   "retained span committed effects beyond an alias's budget");
        }
        const auto expected = reference.run(), actual = native.run();
        expect(actual.instructions == expected.instructions &&
                   native_region_same_state(native.state(), reference.state()) &&
                   native.jit_stats().compiled_native_regions == compiled,
               "retained integer suffix lost W/SP/alias state or recompiled on resume");
    }
#endif
}

void test_native_execution_chain_shared_exit_footprint() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t code = 0x50b400000ULL;
    constexpr unsigned owners = 16;
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine native(image);
    native.map_memory(code, 4096);
    native.set_memory_access(code, true, true, true);
    for (unsigned index = 0; index < owners; ++index)
        native.write32(code + index * 64, 0xd65f03c0U);
    native.set_jit_enabled(true);
    expect(native.invoke_callback(code, 7) == 7,
           "shared native exit fixture could not initialize its first owner");
    const auto first = native.jit_stats();
    for (unsigned index = 1; index < owners; ++index)
        expect(native.invoke_callback(code + index * 64, 7) == 7,
               "shared native exit returned through another owner's frame");
    const auto warmed = native.jit_stats();
    expect(warmed.compiled_native_regions == owners &&
               warmed.emitted_code_bytes - first.emitted_code_bytes <= (owners - 1) * 128 &&
               warmed.allocated_code_bytes == first.allocated_code_bytes,
           "tiny native owners duplicated the shared chain/finalizer code");
    // Executable writes reclaim ordinary owners. The sealed exit pair remains
    // available for the next generation without recompilation or stale edges.
    native.write32(code, 0xd65f03c0U);
    expect(native.invoke_callback(code, 9) == 9 &&
               native.jit_stats().emitted_code_bytes - warmed.emitted_code_bytes <= 128,
           "code invalidation dropped or regenerated the active native exit pair");
#endif
}

void test_jit_native_refill_diagnostics_opt_in() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t code = 0x50b500000ULL;
    constexpr std::uint64_t data = code + 0x10000;
    auto image = transmute::PeImage::parse(executable({0xd61f0120U}));
    transmute::ReferenceEngine native(image);
    initialize_execution_chain(native, code, data, 2);
    native.set_jit_enabled(true);
    const std::array<std::uint64_t, 3> arguments{5, 2, data};
    expect(native.invoke_callback(code, arguments) == 17,
           "refill diagnostic fixture changed its native chain result");
    const auto disabled = native.jit_stats();
    const auto sum = [](const auto& counts) {
        std::uint64_t total = 0;
        for (const auto count : counts) total += count;
        return total;
    };
    expect(disabled.native_read_misses != 0 && disabled.native_store_misses != 0 &&
               sum(disabled.native_read_refills.causes) == 0 &&
               sum(disabled.native_store_refills.causes) == 0,
           "native refill reason collection ran with profiling disabled");
    native.set_jit_profiling(true);
    // Force an affected-span refill while unrelated mappings now retain
    // warmed entries through selective topology replay.
    native.set_memory_access(data, false, false, false);
    native.set_memory_access(data, true, true, false);
    native.write64(data, 2);
    const auto dirty = native.write_revision(data, 8);
    const auto before = native.jit_stats();
    expect(native.invoke_callback(code, arguments) == 17 && native.read64(data) == 17 &&
               dirty && native.write_revision(data, 8) == *dirty + 2,
           "refill profiling changed the guest result or committed stores");
    const auto after = native.jit_stats();
    const auto check = [&sum](const transmute::JitSpanRefillStats& refills,
                              std::uint64_t misses) {
        return refills.successful != 0 &&
            refills.successful + refills.rejected == misses &&
            sum(refills.causes) == misses && sum(refills.rejections) == refills.rejected;
    };
    expect(check(after.native_read_refills,
                 after.native_read_misses - before.native_read_misses) &&
               check(after.native_store_refills,
                 after.native_store_misses - before.native_store_misses),
           "profiled native refill reasons did not account for every read/store miss");
#endif
}

void test_jit_compact_state_operands_and_terminal_exits() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t code = 0x50b600000ULL;
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine native(image);
    native.map_memory(code, 4096);
    native.set_memory_access(code, true, true, true);
    for (const auto offset : {0U, 64U}) native.write32(code + offset, 0xd65f03c0U);
    for (const auto offset : {128U, 192U, 256U, 320U}) {
        const auto source = offset == 128 || offset == 256 ? 15U : 16U;
        native.write32(code + offset, 0xaa0003e0U | (source << 16)); // mov x0,xN
        if (offset >= 256) {
            native.write32(code + offset + 4, 0x91000400U); // add x0,x0,#1
            native.write32(code + offset + 8, 0xd65f03c0U);
        } else {
            native.write32(code + offset + 4, 0xd65f03c0U);
        }
    }
    native.set_initial_register(15, 41);
    native.set_initial_register(16, 41);
    native.set_jit_enabled(true);
    expect(native.invoke_callback(code, 7) == 7,
           "compact code fixture failed to initialize its shared exits");
    auto previous = native.jit_stats();
    expect(native.invoke_callback(code + 64, 9) == 9,
           "terminal native owner did not return its guest result");
    auto current = native.jit_stats();
    expect(current.emitted_code_bytes - previous.emitted_code_bytes <= 81,
           "terminal native owner still emitted an unreachable fallthrough tail");
    previous = current;
    expect(native.invoke_callback(code + 128, 7) == 41,
           "compact disp8 source changed a guest register");
    current = native.jit_stats();
    const auto compact_leaf = current.emitted_code_bytes - previous.emitted_code_bytes;
    previous = current;
    expect(native.invoke_callback(code + 192, 7) == 41,
           "disp32 boundary source changed a guest register");
    current = native.jit_stats();
    expect(current.emitted_code_bytes - previous.emitted_code_bytes == compact_leaf + 3,
           "X15 state access did not use the smaller encoding than X16");
    previous = current;
    expect(native.invoke_callback(code + 256, 7) == 42,
           "compact retained span changed its guest result");
    current = native.jit_stats();
    const auto compact_span = current.emitted_code_bytes - previous.emitted_code_bytes;
    previous = current;
    expect(native.invoke_callback(code + 320, 7) == 42,
           "disp32 retained span changed its guest result");
    current = native.jit_stats();
    expect(current.emitted_code_bytes - previous.emitted_code_bytes == compact_span + 9,
           "retained suffix entries duplicated long X15 state accesses");

    const std::vector<std::uint32_t> words{
        0x91000400U, // add x0,x0,#1: zero displacement
        0x9100400fU, // add x15,x0,#16: maximum positive disp8 register
        0x110005f0U, // add w16,w15,#1: disp32 and W upper clearing
        0x91000600U, // add x0,x16,#1
        0xd65f03c0U,
    };
    for (unsigned budget = 1; budget <= words.size(); ++budget) {
        auto ri = transmute::PeImage::parse(executable(words));
        auto ni = transmute::PeImage::parse(executable(words));
        transmute::ReferenceEngine reference(ri), translated(ni);
        for (auto* engine : {&reference, &translated})
            engine->set_initial_register(0, 0xfffffffffffffffdULL);
        translated.set_jit_enabled(true);
        static_cast<void>(translated.invoke_callback(ni.entry(),
            std::span<const std::uint64_t>{}));
        const auto compiled = translated.jit_stats().compiled_native_regions;
        if (budget < words.size()) {
            for (auto* engine : {&reference, &translated})
                expect_failure([&] { static_cast<void>(engine->run(budget)); },
                               "guest exceeded instruction limit");
            expect(native_region_same_state(translated.state(), reference.state()),
                   "compact state operand lost an exact alias/budget prefix");
        }
        const auto expected = reference.run(), actual = translated.run();
        expect(actual.instructions == expected.instructions &&
                   native_region_same_state(translated.state(), reference.state()) &&
                   translated.jit_stats().compiled_native_regions == compiled,
               "compact zero/disp8/disp32 state operands changed resumed execution");
    }
#endif
}
