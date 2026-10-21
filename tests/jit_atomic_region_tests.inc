// Runtime integration checks, separate from the direct host-fragment matrix.

void test_native_lse_regions() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    using transmute::jit_detail::NativeLseOperation;
    struct Registers { unsigned source, target, base; };
    constexpr std::array<Registers, 8> registers{
        Registers{0, 1, 2}, {0, 0, 2}, {0, 2, 2}, {2, 1, 2},
        {31, 1, 2}, {0, 31, 2}, {31, 31, 31}, {30, 1, 30}};
    constexpr std::uint64_t data = 0x504000000ULL;
    std::size_t cases = 0;
    for (unsigned operation = 0; operation != 10; ++operation) {
        const auto op = static_cast<NativeLseOperation>(operation);
        for (const auto width : {1U, 2U, 4U, 8U}) {
            const auto mask = width == 8 ? UINT64_MAX : (1ULL << (width * 8)) - 1;
            for (unsigned ordering = 0; ordering != 4; ++ordering) {
                for (const auto reg : registers) {
                    for (const bool match : {false, true}) {
                        const auto opcode = native_lse_test_opcode(
                            op, width, ordering, reg.source, reg.target, reg.base);
                        auto reference_image = transmute::PeImage::parse(executable({
                            0xd10083ffU, 0xd51b4214U, 0xd51b4415U, 0xd51b4436U,
                            opcode, 0x910083ffU, 0xd65f0300U})); // reserve SP, MSR state, atomic, restore SP
                        auto native_image = transmute::PeImage::parse(executable({
                            0xd10083ffU, 0xd51b4214U, 0xd51b4415U, 0xd51b4436U,
                            opcode, 0x910083ffU, 0xd65f0300U}));
                        transmute::ReferenceEngine reference(reference_image), native(native_image);
                        native.set_jit_enabled(true);
                        std::array<std::uint64_t, 2> addresses{};
                        std::array<std::optional<std::uint64_t>, 2> revisions{};
                        unsigned index = 0;
                        for (auto* engine : {&reference, &native}) {
                            const auto sentinel = engine->state().x[30];
                            engine->map_memory(data, 32);
                            const auto address = reg.base == 31 ? engine->state().sp - 32 : data + 8;
                            addresses[index] = address;
                            for (unsigned r = 0; r != 30; ++r)
                                engine->set_initial_register(r, 0x1234567800000000ULL + r);
                            for (unsigned r = 0; r != 32; ++r)
                                engine->set_initial_vector(r, {UINT64_MAX - r, 0xabcdef0000000000ULL + r});
                            if (reg.source != 31) engine->set_initial_register(reg.source, UINT64_MAX);
                            if (reg.target != 31) engine->set_initial_register(reg.target, 0x8000000080000080ULL);
                            if (reg.base != 31) engine->set_initial_register(reg.base, address);
                            engine->set_initial_register(20, 0xb0000000U);
                            engine->set_initial_register(21, 0x03c00000U);
                            engine->set_initial_register(22, 0xa800009fU);
                            engine->set_initial_register(24, sentinel);
                            const auto expected = reg.source == 31 ? 0 : engine->state().x[reg.source] & mask;
                            const auto old = op == NativeLseOperation::compare_swap
                                ? (match ? expected : (expected + 1) & mask)
                                : 0x807fffff80000001ULL & mask;
                            engine->write64(address, old);
                            engine->write64(address - 8, 0x123456789abcdef0ULL);
                            engine->write64(address + 8, 0xfedcba9876543210ULL);
                            revisions[index++] = engine->write_revision(address, width);
                        }
                        const auto expected = reference.run(), actual = native.run();
                        expect(actual.instructions == expected.instructions &&
                                   native_region_same_state(reference.state(), native.state()),
                               "native LSE region changed guest registers, status, control or instruction count");
                        for (unsigned byte = 0; byte != 24; ++byte)
                            expect(static_cast<const transmute::GuestMemory&>(reference).read8(addresses[0] - 8 + byte) ==
                                       static_cast<const transmute::GuestMemory&>(native).read8(addresses[1] - 8 + byte),
                                   "native LSE region changed replacement or adjacent bytes");
                        const bool wrote = op != NativeLseOperation::compare_swap || match;
                        expect(revisions[1] &&
                                   (wrote ? native.write_revision(addresses[1], width) > revisions[1]
                                          : native.write_revision(addresses[1], width) == revisions[1]),
                               "native LSE CAS mismatch dirtied history or a successful RMW missed its write");
                        expect(native.jit_stats().native_region_instructions != 0,
                               "eligible LSE runtime matrix never entered native atomic execution");
                        ++cases;
                    }
                }
            }
        }
    }
    expect(cases == 2560, "runtime LSE matrix omitted a width, ordering, alias or CAS outcome");
#endif
}

void test_native_lse_region_guards() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    using transmute::jit_detail::NativeLseOperation;
    constexpr std::uint64_t data = 0x504000000ULL;
    for (unsigned kind = 0; kind != 7; ++kind) {
        for (const bool match : {false, true}) {
            const auto opcode = native_lse_test_opcode(
                NativeLseOperation::compare_swap, 8, 3, 0, 1, 2);
            auto reference_image = transmute::PeImage::parse(executable({opcode, 0xd65f03c0U}));
            auto native_image = transmute::PeImage::parse(executable({opcode, 0xd65f03c0U}));
            transmute::ReferenceEngine reference(reference_image), native(native_image);
            native.set_jit_enabled(true);
            alignas(16) std::array<std::array<std::uint8_t, 32>, 2> external{};
            unsigned index = 0;
            for (auto* engine : {&reference, &native}) {
                // Odd mapping base makes an aligned guest address resolve to
                // an unaligned owned host pointer. Guest misalignment is separate.
                const auto base = kind == 4 ? data + 1 : data;
                if (kind == 3) engine->map_external_memory(base, external[index].data(), 32, true);
                else engine->map_memory(base, 32);
                const auto address = kind == 5 ? data + 1 : data + 8;
                engine->write64(address, match ? 13 : 14);
                engine->set_initial_register(0, 13);
                engine->set_initial_register(1, 42);
                engine->set_initial_register(2, address);
                if (kind == 1) engine->set_memory_access(base, false, true, false);
                if (kind == 2) engine->set_memory_access(base, true, false, false);
                if (kind == 6) engine->set_memory_access(base, true, true, true);
                ++index;
            }
            std::array<std::string, 2> failures{};
            index = 0;
            for (auto* engine : {&reference, &native}) {
                try { static_cast<void>(engine->run()); }
                catch (const std::runtime_error& error) { failures[index] = error.what(); }
                ++index;
            }
            expect(failures[0].empty() == failures[1].empty() &&
                       native_region_same_state(reference.state(), native.state()),
                   "LSE permission/alignment side exit changed fault or architectural state");
            if (kind == 1 || kind == 5 || (kind == 2 && match))
                expect(!failures[0].empty(), "LSE guard oracle unexpectedly accepted invalid access");
            else expect(failures[0].empty(), "LSE checked fallback rejected a valid guarded-out access");
            if (kind == 1) {
                reference.set_memory_access(data, true, true, false);
                native.set_memory_access(data, true, true, false);
            }
            for (unsigned byte = 0; byte != 32; ++byte) {
                const auto base = kind == 4 ? data + 1 : data;
                expect(static_cast<const transmute::GuestMemory&>(reference).read8(base + byte) ==
                           static_cast<const transmute::GuestMemory&>(native).read8(base + byte),
                       "LSE guard fallback changed a replacement or wrote outside its width");
            }
        }
    }
#endif
}

void test_native_lse_image_fallback() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    using transmute::jit_detail::NativeLseOperation;
    const auto opcode = native_lse_test_opcode(NativeLseOperation::compare_swap, 8, 3, 0, 1, 2);
    auto bytes = executable({opcode, 0xd65f03c0U});
    bytes.resize(0x600);
    put16(bytes, 0x86, 2);
    put32(bytes, 0x98 + 56, 0x3000);
    constexpr std::size_t section = 0x188 + 40;
    bytes[section] = '.';
    bytes[section + 1] = 'd';
    put32(bytes, section + 8, 0x200);
    put32(bytes, section + 12, 0x2000);
    put32(bytes, section + 16, 0x200);
    put32(bytes, section + 20, 0x400);
    put32(bytes, section + 36, 0xc0000040);
    auto reference_image = transmute::PeImage::parse(bytes);
    auto native_image = transmute::PeImage::parse(bytes);
    transmute::ReferenceEngine reference(reference_image), native(native_image);
    native.set_jit_enabled(true);
    const auto address = native_image.base() + 0x2000;
    for (auto* engine : {&reference, &native}) {
        engine->write64(address, 13);
        engine->set_initial_register(0, 13);
        engine->set_initial_register(1, 42);
        engine->set_initial_register(2, address);
    }
    const auto expected = reference.run(), actual = native.run();
    expect(actual.instructions == expected.instructions &&
               native_region_same_state(reference.state(), native.state()) &&
               native.read64(address) == 42 && reference.read64(address) == 42,
           "native LSE guard rejected or bypassed checked writable PE data");
#endif
}

void test_native_lse_shared_aliases() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    using transmute::jit_detail::NativeLseOperation;
    const auto opcode = native_lse_test_opcode(NativeLseOperation::add, 8, 3, 1, 2, 0);
    auto image = transmute::PeImage::parse(executable({
        0x91000463U, opcode, 0x91000463U, 0xd65f03c0U}));
    transmute::ReferenceEngine engine(image);
    engine.set_jit_enabled(true);
    constexpr std::uint64_t data = 0x504000000ULL;
    engine.map_memory(data, 16);
    const std::array<std::uint64_t, 4> args{data, 1, 0, 0};
    expect(engine.invoke_callback(image.entry(), args) == data &&
               engine.jit_stats().compiled_native_regions != 0,
           "atomic alias fixture did not warm its native owner");
    engine.set_jit_profiling(true);
    const auto before = engine.native_concurrency_stats().shared_read_leases;
    for (const unsigned offset : {0U, 4U, 8U, 12U})
        expect(engine.invoke_callback(image.entry() + offset, args) == data,
               "native atomic interior alias changed the callback return");
    expect(engine.native_concurrency_stats().shared_read_leases >= before + 4 &&
               engine.jit_stats().native_region_instructions != 0,
           "aligned native atomic owner or interior alias never entered shared execution");
#endif
}

void test_native_lse_region_journal() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    using transmute::jit_detail::NativeLseOperation;
    constexpr std::uint64_t data = 0x504000000ULL;
    const auto add = native_lse_test_opcode(NativeLseOperation::add, 8, 3, 1, 2, 0);
    const std::vector<std::uint32_t> words{add, 0xd1000463U, 0xb5ffffc3U, 0xd65f03c0U};
    for (const unsigned budget : {1U, 2U, 3U, 191U, 192U, 193U, 390U, 391U}) {
        auto reference_image = transmute::PeImage::parse(executable(words));
        auto native_image = transmute::PeImage::parse(executable(words));
        transmute::ReferenceEngine reference(reference_image), native(native_image);
        native.set_jit_enabled(true);
        for (auto* engine : {&reference, &native}) {
            engine->map_memory(data, 16);
            engine->write64(data, 0);
            engine->set_initial_register(0, data);
            engine->set_initial_register(1, 1);
            engine->set_initial_register(3, 130);
        }
        const auto before = native.write_revision(data, 8);
        for (auto* engine : {&reference, &native}) {
            if (budget < 391) expect_failure([&] { static_cast<void>(engine->run(budget)); },
                                            "guest exceeded instruction limit");
            else static_cast<void>(engine->run(budget));
        }
        const auto effects = (budget + 2) / 3;
        expect(native_region_same_state(reference.state(), native.state()) &&
                   reference.read64(data) == native.read64(data) &&
                   before && native.write_revision(data, 8) == *before + std::min(effects, 130U),
               "native LSE journal lost or replayed effects before an exact budget observer");
        if (budget == 391) {
            const std::array<std::uint64_t, 4> args{data, 1, 0, 130};
            expect(native.invoke_callback(native_image.entry(), args) == data && native.read64(data) == 260 &&
                       native.jit_stats().native_journal_exits != 0,
                   "warm LSE journal did not commit across capacity exits");
        }
    }
#endif
}

void test_native_lse_region_monitor() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    using transmute::jit_detail::NativeLseOperation;
    constexpr std::uint64_t data = 0x504000000ULL;
    for (const auto operation : {NativeLseOperation::add, NativeLseOperation::swap,
                                 NativeLseOperation::compare_swap}) {
        for (const std::uint64_t offset : {0ULL, 16ULL}) {
            for (const bool match : {false, true}) {
                const auto atomic = native_lse_test_opcode(operation, 8, 3, 0, 1, 2);
                auto image = transmute::PeImage::parse(executable({
                    0xc85ffc81U, 0xc803fc82U, 0xd65f03c0U, // LDAXR/STLXR
                    atomic, 0xd65f03c0U}));
                transmute::ReferenceEngine parent(image);
                parent.map_memory(data, 32);
                parent.write64(data, 13);
                parent.write64(data + 16, 13);
                auto worker = parent.fork_thread(image.entry() + 12, 0);
                worker->set_jit_enabled(true);
                parent.set_initial_register(4, data);
                parent.set_initial_register(2, 99);
                expect_failure([&] { static_cast<void>(parent.run(1)); },
                               "guest exceeded instruction limit");
                worker->set_initial_register(0,
                    operation == NativeLseOperation::add && match ? 0 : match ? 13 : 14);
                worker->set_initial_register(1,
                    operation == NativeLseOperation::compare_swap && match ? 13 : 42);
                worker->set_initial_register(2, data + offset);
                static_cast<void>(worker->run());
                static_cast<void>(parent.run());
                const bool dirty = operation != NativeLseOperation::compare_swap || match;
                expect(parent.state().x[3] == (offset == 0 && dirty ? 1U : 0U),
                       "native LSE history missed an equal-value write or dirtied a failed CAS monitor");
                expect(worker->jit_stats().native_region_instructions != 0,
                       "LSE monitor fixture silently used the checked atomic path");
            }
        }
    }
#endif
}

void test_native_lse_fp_fault_session() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    using transmute::jit_detail::NativeLseOperation;
    GuardedFpHostState restore;
    constexpr unsigned host_csr = 0x1f80U | 0x8040U | 0x2000U | 0x25U;
    constexpr std::uint64_t data = 0x504100000ULL;
    const auto cas = native_lse_test_opcode(NativeLseOperation::compare_swap, 8, 3, 0, 1, 2);
    const std::vector<std::uint32_t> words{
        0xd51b4414U, 0xd51b4435U, 0xd51b4216U, // initialize FPCR/FPSR/NZCV
        0x1e651886U, // fdiv d6,d4,d5: an IX-producing committed prefix
        cas, 0xd53b4437U, 0xd65f03c0U}; // atomic, observe FPSR, return
    for (unsigned kind = 0; kind != 8; ++kind) {
        for (const bool match : {false, true}) {
            for (const unsigned budget : {3U, 4U, 5U, 7U}) {
                auto reference_image = transmute::PeImage::parse(executable(words));
                auto native_image = transmute::PeImage::parse(executable(words));
                transmute::ReferenceEngine reference(reference_image), native(native_image);
                native.set_jit_enabled(true);
                alignas(16) std::array<std::array<std::uint8_t, 32>, 2> external{};
                const auto base = kind == 5 ? data + 1 : data;
                const auto address = kind == 4 ? data + 1 : data + 8;
                std::array<std::optional<std::uint64_t>, 2> revisions{};
                unsigned index = 0;
                for (auto* engine : {&reference, &native}) {
                    if (kind != 3) {
                        if (kind == 6) engine->map_external_memory(base, external[index].data(), 32, true);
                        else engine->map_memory(base, 32);
                        engine->write64(address, match ? 13 : 14);
                        revisions[index] = engine->write_revision(address, 8);
                        if (kind == 1) engine->set_memory_access(base, true, false, false);
                        if (kind == 2) engine->set_memory_access(base, false, true, false);
                        if (kind == 7) engine->set_memory_access(base, true, true, true);
                    }
                    engine->set_initial_register(0, 13);
                    engine->set_initial_register(1, 42);
                    engine->set_initial_register(2, address);
                    engine->set_initial_register(20, 0);
                    engine->set_initial_register(21, 0x08000080U);
                    engine->set_initial_register(22, 0xb0000000U);
                    engine->set_initial_register(23, UINT64_MAX);
                    engine->set_initial_vector(4, {std::bit_cast<std::uint64_t>(1.0), UINT64_MAX});
                    engine->set_initial_vector(5, {std::bit_cast<std::uint64_t>(3.0), UINT64_MAX});
                    engine->set_initial_vector(6, {UINT64_MAX, UINT64_MAX});
                    ++index;
                }
                const bool atomic_fault = budget >= 5 &&
                    ((kind == 1 && match) || kind == 2 || kind == 3 || kind == 4);
                std::array<std::string, 2> failures{};
                std::array<std::uint64_t, 2> counts{};
                index = 0;
                for (auto* engine : {&reference, &native}) {
                    _mm_setcsr(host_csr);
                    try { counts[index] = engine->run(budget).instructions; }
                    catch (const std::runtime_error& error) { failures[index] = error.what(); }
                    expect(_mm_getcsr() == host_csr,
                           "FP-prefix atomic side exit leaked guest MXCSR into the host");
                    ++index;
                }
                expect(native_region_same_state(reference.state(), native.state()) &&
                           failures[0].empty() == failures[1].empty() && counts[0] == counts[1],
                       "FP-prefix atomic side exit changed the committed state or dynamic count");
                for (const auto& failure : failures) {
                    if (atomic_fault) {
                        expect(!failure.empty() && failure.find("guest exceeded instruction limit") == std::string::npos,
                               "FP-prefix atomic fault became a budget exit or successful access");
                    } else if (budget < words.size()) {
                        expect(failure.find("guest exceeded instruction limit") != std::string::npos,
                               "FP-prefix atomic budget did not stop at its exact instruction");
                    } else expect(failure.empty(), "FP-prefix guarded atomic rejected a valid checked access");
                }
                expect(native.state().fpsr == (budget >= 4 ? 0x08000090U : 0x08000080U),
                       "FP-prefix atomic side exit lost committed IX or invented another exception");
                if (atomic_fault)
                    expect(native.state().pc == native_image.entry() + 16,
                           "FP-prefix atomic fault did not identify the uncompleted atomic instruction");
                if (budget >= 4)
                    expect(native.jit_stats().native_region_instructions != 0,
                           "FP-prefix atomic fixture never exercised a live native FP session");
                if (kind == 0 && budget == 5)
                    expect(native.jit_stats().native_region_instructions == 2,
                           "owned atomic following FP silently fell back instead of committing two native steps");
                if (kind != 3) {
                    if (kind == 2) {
                        reference.set_memory_access(base, true, true, false);
                        native.set_memory_access(base, true, true, false);
                    }
                    for (unsigned byte = 0; byte != 32; ++byte)
                        expect(static_cast<const transmute::GuestMemory&>(reference).read8(base + byte) ==
                                   static_cast<const transmute::GuestMemory&>(native).read8(base + byte),
                               "FP-prefix atomic side exit replayed an effect or damaged adjacent bytes");
                    if (revisions[1]) {
                        const bool wrote = budget >= 5 && match && !atomic_fault;
                        expect(wrote ? native.write_revision(address, 8) > revisions[1]
                                     : native.write_revision(address, 8) == revisions[1],
                               "FP-prefix atomic side exit published a rejected or missing journal record");
                    }
                }
            }
        }
    }
    // A rejected FZ arithmetic operation follows the successful IX prefix.
    // Its checked retry must see restored host state and precede the atomic;
    // an exact budget between them must leave the atomic completely untouched.
    const std::vector<std::uint32_t> guarded_words{
        0xd51b4414U, 0xd51b4435U, 0xd51b4216U,
        0x1e651886U, 0x1e651907U, // fdiv d6,d4,d5; fdiv d7,d8,d5
        cas, 0xd53b4437U, 0xd65f03c0U};
    for (const bool match : {false, true}) {
        for (const unsigned budget : {5U, 8U}) {
            auto reference_image = transmute::PeImage::parse(executable(guarded_words));
            auto native_image = transmute::PeImage::parse(executable(guarded_words));
            transmute::ReferenceEngine reference(reference_image), native(native_image);
            native.set_jit_enabled(true);
            for (auto* engine : {&reference, &native}) {
                engine->map_memory(data, 16);
                engine->write64(data, match ? 13 : 14);
                engine->set_initial_register(0, 13);
                engine->set_initial_register(1, 42);
                engine->set_initial_register(2, data);
                engine->set_initial_register(20, 1U << 24);
                engine->set_initial_register(21, 0x08000080U);
                engine->set_initial_register(22, 0xb0000000U);
                engine->set_initial_vector(4, {std::bit_cast<std::uint64_t>(1.0), 0});
                engine->set_initial_vector(5, {std::bit_cast<std::uint64_t>(3.0), 0});
                engine->set_initial_vector(8, {0x0010000000000000ULL, 0});
            }
            for (auto* engine : {&reference, &native}) {
                _mm_setcsr(host_csr);
                if (budget == 5) expect_failure([&] { static_cast<void>(engine->run(budget)); },
                                                "guest exceeded instruction limit");
                else expect(engine->run(budget).instructions == budget,
                            "guarded FP atomic suffix changed the completed instruction count");
                expect(_mm_getcsr() == host_csr, "rejected FP before atomic leaked host control state");
            }
            expect(native_region_same_state(reference.state(), native.state()) &&
                       reference.read64(data) == native.read64(data) &&
                       (native.state().fpsr & 0x18U) == 0x18U,
                   "rejected FP retry before atomic lost sticky flags, state or memory ordering");
            if (budget == 5)
                expect(native.read64(data) == (match ? 13U : 14U) &&
                           native.jit_stats().native_region_instructions == 1,
                       "rejected FP exact prefix completed or dirtied the following atomic");
        }
    }
#endif
}
