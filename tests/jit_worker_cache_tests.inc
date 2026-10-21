// Included after the shared native-state comparison helper.

void test_jit_worker_cache_budget_fp_and_journal() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t data = 0x50bc00000ULL;
    const std::vector<std::uint32_t> words{
        0xf9400001U, // ldr x1,[x0]
        0x91000421U, // add x1,x1,#1
        0xf9000001U, // str x1,[x0]
        0x1e621820U, // fdiv d0,d1,d2
        0x9e660000U, // fmov x0,d0
        0xd65f03c0U,
    };
    for (unsigned budget = 1; budget <= words.size(); ++budget) {
        auto ri = transmute::PeImage::parse(executable(words));
        auto ni = transmute::PeImage::parse(executable(words));
        transmute::ReferenceEngine reference_parent(ri), native_parent(ni);
        for (auto* engine : {&reference_parent, &native_parent}) {
            engine->map_memory(data, 8);
            engine->write64(data, 10);
            engine->set_initial_vector(1, {0x3ff0000000000000ULL, 0});
            engine->set_initial_vector(2, {0x4008000000000000ULL, 0});
        }
        native_parent.set_jit_enabled(true);
        static_cast<void>(native_parent.invoke_callback(ni.entry(), data));
        auto reference = reference_parent.fork_thread(ri.entry(), data);
        auto native = native_parent.fork_thread(ni.entry(), data);
        for (auto* engine : {reference.get(), native.get()}) {
            engine->set_initial_vector(1, {0x3ff0000000000000ULL, 0});
            engine->set_initial_vector(2, {0x4008000000000000ULL, 0});
        }
        reference_parent.write64(data, 10);
        native_parent.write64(data, 10);
        const auto dirty = native->write_revision(data, 8);
        const auto inherited = native->jit_stats();
        expect(inherited.inherited_native_regions == 1 &&
                   inherited.inherited_code_bytes != 0 &&
                   inherited.inherited_chunk_bytes == 64 * 1024 &&
                   inherited.compiled_native_regions == 0,
               "worker did not inherit the warmed immutable PE owner");
        const auto csr = _mm_getcsr();
        if (budget < words.size()) {
            for (auto* engine : {reference.get(), native.get()})
                expect_failure([&] { static_cast<void>(engine->run(budget)); },
                               "guest exceeded instruction limit");
            expect(native_region_same_state(native->state(), reference->state()) &&
                       native->read64(data) == reference->read64(data) && _mm_getcsr() == csr,
                   "inherited owner lost a precise budget, store prefix or host FP state");
        }
        const auto expected = reference->run(), actual = native->run();
        const auto after = native->jit_stats();
        expect(actual.instructions == expected.instructions &&
                   native_region_same_state(native->state(), reference->state()) &&
                   native->read64(data) == 11 && dirty &&
                   native->write_revision(data, 8) == *dirty + 1 &&
                   (native->state().fpsr & 16U) != 0 && _mm_getcsr() == csr &&
                   after.compiled_native_regions == 0 && after.native_region_instructions != 0,
               "inherited native body changed resumed FP/journal state or recompiled");
    }
#endif
}

void test_jit_worker_cache_sealed_lifetime() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    std::vector<std::uint32_t> words(18, 0xd503201fU);
    words[0] = 0x91001c00U; // add x0,x0,#7
    words[1] = 0xd65f03c0U;
    words[16] = 0x91002400U; // add x0,x0,#9
    words[17] = 0xd65f03c0U;
    auto image = transmute::PeImage::parse(executable(words));
    auto parent = std::make_unique<transmute::ReferenceEngine>(image);
    parent->set_jit_enabled(true);
    expect(parent->invoke_callback(image.entry(), 5) == 12,
           "worker lifetime fixture could not warm its parent");
    auto worker = parent->fork_thread(image.entry(), 5);
    expect(worker->jit_stats().inherited_native_regions == 1,
           "worker lifetime fixture did not receive immutable native code");
    const auto allocation = parent->jit_stats().allocated_code_bytes;
    std::atomic<bool> started{false}, failed{false};
    std::atomic<std::uint64_t> calls{0};
    std::exception_ptr failure;
    std::jthread execution([&](std::stop_token stop) {
        try {
            started.store(true, std::memory_order_release);
            while (!stop.stop_requested()) {
                expect(worker->invoke_callback(image.entry(), 5) == 12,
                       "shared RX body changed during donor compilation or destruction");
                calls.fetch_add(1, std::memory_order_release);
            }
        } catch (...) {
            failure = std::current_exception();
            failed.store(true, std::memory_order_release);
        }
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    expect(parent->invoke_callback(image.entry() + 64, 5) == 14 &&
               parent->jit_stats().allocated_code_bytes >= allocation + 64 * 1024,
           "donor appended into a sealed chunk instead of allocating private RX code");
    parent->set_jit_enabled(false); // Retire every donor owner and exit blob.
    parent.reset();
    const auto before = calls.load(std::memory_order_acquire);
    while (calls.load(std::memory_order_acquire) < before + 64 &&
           !failed.load(std::memory_order_acquire)) std::this_thread::yield();
    execution.request_stop();
    execution.join();
    if (failure) std::rethrow_exception(failure);
    expect(worker->jit_stats().compiled_native_regions == 0 &&
               worker->jit_stats().native_region_instructions >= 128,
           "worker lost shared code ownership after donor cache destruction");
#endif
}

void test_jit_worker_cache_exclusions() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    // Writable PE text must compile privately even if its current words match.
    auto writable_bytes = executable({0x91001c00U, 0xd65f03c0U});
    put32(writable_bytes, 0x188 + 36, 0xe0000020U);
    auto writable = transmute::PeImage::parse(writable_bytes);
    transmute::ReferenceEngine writable_parent(writable);
    writable_parent.set_jit_enabled(true);
    static_cast<void>(writable_parent.invoke_callback(writable.entry(), 5));
    auto writable_worker = writable_parent.fork_thread(writable.entry(), 5);
    expect(writable_worker->jit_stats().inherited_native_regions == 0 &&
               writable_worker->run().exit_code == 12 &&
               writable_worker->jit_stats().compiled_native_regions != 0,
           "worker inherited writable PE instructions");

    constexpr std::uint64_t mapped = 0x50bd00000ULL;
    auto image = transmute::PeImage::parse(executable({0x91001c00U, 0xd65f03c0U}));
    transmute::ReferenceEngine parent(image);
    parent.set_jit_enabled(true);
    static_cast<void>(parent.invoke_callback(image.entry(), 5));
    parent.map_memory(mapped, 16);
    parent.set_memory_access(mapped, true, true, true); // Invalidate the old generation.
    parent.write32(mapped, 0x91002400U);
    parent.write32(mapped + 4, 0xd65f03c0U);
    auto stale_worker = parent.fork_thread(image.entry(), 5);
    expect(stale_worker->jit_stats().inherited_native_regions == 0 &&
               stale_worker->run().exit_code == 12,
           "worker inherited an owner from a stale code generation");
    static_cast<void>(parent.invoke_callback(mapped, 5));
    auto mapped_worker = parent.fork_thread(mapped, 5);
    expect(mapped_worker->jit_stats().inherited_native_regions == 0 &&
               mapped_worker->run().exit_code == 14,
           "worker inherited mapped executable instructions");

    auto dll_bytes = executable({0x91001c00U, 0xd65f03c0U});
    put16(dll_bytes, 0x96, 0x2002U);
    put64(dll_bytes, 0x98 + 24, 0x180000000ULL);
    auto dll = transmute::PeImage::parse(dll_bytes, 0x180000000ULL);
    auto host = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine staged_parent(host);
    staged_parent.set_jit_enabled(true);
    staged_parent.stage_module(dll);
    expect(staged_parent.invoke_callback(dll.entry(), 5) == 12,
           "worker staged-image fixture could not warm its private owner");
    auto staged_worker = staged_parent.fork_thread(dll.entry(), 5);
    expect(staged_worker->jit_stats().inherited_native_regions == 0,
           "private staged PE code escaped into a worker snapshot");
    expect_failure([&] { static_cast<void>(staged_worker->run()); },
                   "guest attempted to execute");
    staged_parent.commit_staged_modules();
    expect(staged_worker->run().exit_code == 12,
           "worker could not execute newly published PE code after a private miss");
#endif
}

void test_jit_worker_cache_changed_words_and_negative_state() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    // Place a loader-managed IAT in immutable text. bind_import can change its
    // words without the engine generation, so inheritance must compare bytes.
    auto bytes = executable({0xd65f03c0U});
    put32(bytes, 0x98 + 120, 0x1080U);
    put32(bytes, 0x98 + 124, 40U);
    put32(bytes, 0x280, 0x10a8U);
    put32(bytes, 0x28c, 0x10c0U);
    put32(bytes, 0x290, 0x1000U);
    put64(bytes, 0x2a8, 0x10d0U);
    put64(bytes, 0x200, 0x10d0U);
    constexpr char dll_name[] = "kernel32.dll", function[] = "Unused";
    for (std::size_t index = 0; index < sizeof(dll_name); ++index)
        bytes[0x2c0 + index] = static_cast<std::uint8_t>(dll_name[index]);
    for (std::size_t index = 0; index < sizeof(function); ++index)
        bytes[0x2d2 + index] = static_cast<std::uint8_t>(function[index]);
    auto image = transmute::PeImage::parse(bytes);
    image.bind_import(0, (0xd65f03c0ULL << 32) | 0x91001c00U);
    transmute::ReferenceEngine parent(image,
        [](const transmute::Import&, transmute::CpuState&, transmute::GuestMemory&)
            -> std::optional<std::uint32_t> { throw std::runtime_error("unexpected fixture import"); });
    parent.set_jit_enabled(true);
    expect(parent.invoke_callback(image.entry(), 5) == 12,
           "changed-word fixture could not warm its immutable owner");
    image.bind_import(0, (0xd65f03c0ULL << 32) | 0x91002400U);
    auto changed_worker = parent.fork_thread(image.entry(), 5);
    expect(changed_worker->jit_stats().inherited_native_regions == 0 &&
               changed_worker->run().exit_code == 14,
           "worker inherited stale words after a loader-managed PE update");

    auto fp_image = transmute::PeImage::parse(executable({
        0xd51b4414U, // msr fpcr,x20: checked, negative-cache entry
        0x1e622820U, // fadd d0,d1,d2: data-policy-sensitive native owner
        0x9e660000U,
        0xd65f03c0U}));
    transmute::ReferenceEngine fp_parent(fp_image);
    fp_parent.set_initial_register(20, 1U << 8);
    fp_parent.set_initial_vector(1, {0x3ff0000000000000ULL, 0});
    fp_parent.set_initial_vector(2, {0x4008000000000000ULL, 0});
    fp_parent.set_jit_enabled(true);
    static_cast<void>(fp_parent.invoke_callback(fp_image.entry(), 0));
    const auto parent_before = fp_parent.jit_stats();
    expect(parent_before.negative_cache_entries != 0,
           "worker negative-cache fixture did not create its checked policy entry");
    auto fp_worker = fp_parent.fork_thread(fp_image.entry(), 0);
    expect(fp_worker->jit_stats().inherited_native_regions == 1 &&
               fp_worker->jit_stats().negative_cache_entries == 0,
           "worker inherited parent negative entries or omitted positive FP aliases");
    fp_worker->set_initial_vector(1, {0x3ff0000000000000ULL, 0});
    fp_worker->set_initial_vector(2, {0x4008000000000000ULL, 0});
    const auto csr = _mm_getcsr();
    static_cast<void>(fp_worker->run());
    expect(fp_worker->state().fpcr == 0 &&
               fp_worker->state().v[0][0] == 0x4010000000000000ULL &&
               fp_worker->jit_stats().compiled_native_regions == 0 &&
               fp_worker->jit_stats().native_region_instructions >= 3 &&
               fp_parent.jit_stats().negative_cache_entries == parent_before.negative_cache_entries &&
               _mm_getcsr() == csr,
           "inherited FP aliases reused parent policy/negative state or leaked host MXCSR");
#endif
}
