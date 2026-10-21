// Included inside test_main.cpp's anonymous namespace on Windows x64.

void test_native_topology_history_replay_wrap() {
    using History = transmute::NativeTopologyHistory;
    History history;
    const auto initial = history.stamp();
    history.append(0x1000, 16);
    history.append();
    auto replay = history.since(initial);
    expect(replay.complete && replay.count == 2 && replay[0].base == 0x1000 &&
               replay[0].size == 16 && replay[1].size == 0,
           "native topology history lost a range or metadata-only record");
    expect(History::overlaps(0x1000, 16, replay[0]) &&
               History::overlaps(0x100f, 16, replay[0]) &&
               !History::overlaps(0x1010, 16, replay[0]) &&
               !History::overlaps(0xff0, 16, replay[0]) &&
               !History::overlaps(0x1000, 16, replay[1]),
           "native topology range overlap changed half-open boundary semantics");
    const auto recent = history.stamp();
    for (std::size_t index = 0; index < History::capacity; ++index)
        history.append(0x2000 + index * 16, 16);
    replay = history.since(recent);
    expect(replay.complete && replay.count == History::capacity &&
               replay[0].base == 0x2000 && replay[History::capacity - 1].base ==
                   0x2000 + (History::capacity - 1) * 16,
           "native topology replay lost exactly one ring of retained changes");
    history.append(0x4000, 16);
    expect(!history.since(recent).complete && !history.since(initial).complete,
           "overwritten native topology records were incorrectly treated as complete history");
    History missing(7, 3);
    expect(!missing.since({6, 3}).complete,
           "native topology replay trusted a sequence without a matching retained slot");
    History wrapping(UINT64_MAX - 1, 9);
    const auto before_wrap = wrapping.stamp();
    wrapping.append(0x1000, 16);
    expect(wrapping.since(before_wrap).complete &&
               wrapping.since(before_wrap)[0].sequence == UINT64_MAX,
           "last native topology sequence before wrap could not be replayed");
    wrapping.append(0x2000, 16);
    expect(wrapping.stamp().sequence == 0 && wrapping.stamp().era == 10 &&
               !wrapping.since(before_wrap).complete,
           "native topology sequence wrap reused an old replay identity");
    const auto after_wrap = wrapping.stamp();
    wrapping.append(0x3000, 16);
    expect(wrapping.since(after_wrap).complete && wrapping.since(after_wrap)[0].base == 0x3000,
           "native topology history failed to resume safe replay in its new era");
    History exhausted(UINT64_MAX, UINT64_MAX);
    exhausted.append(0x1000, 16);
    expect(!exhausted.usable() && !exhausted.since(exhausted.stamp()).complete,
           "native topology era exhaustion did not permanently require full invalidation");
    exhausted.append(0x2000, 16);
    expect(!exhausted.since(exhausted.stamp()).complete,
           "native topology history resumed replay after identity exhaustion");
    const History::Change high{1, UINT64_MAX - 31, 16};
    expect(History::overlaps(UINT64_MAX - 32, 16, high) &&
               !History::overlaps(0, 16, high),
           "native topology overlap overflowed a high guest address");
}

void test_native_topology_retains_unrelated_spans() {
    constexpr std::uint64_t data = 0x50d000000ULL;
    auto image = transmute::PeImage::parse(executable({
        0xf9400003U, 0xf9000001U, 0xd65f03c0U}));
    transmute::ReferenceEngine engine(image);
    engine.set_jit_enabled(true);
    engine.set_jit_profiling(true);
    engine.map_memory(data, 16);
    const std::array<std::uint64_t, 2> args{data, 42};
    const auto invoke = [&] {
        expect(engine.invoke_callback(image.entry(), args) == data && engine.read64(data) == 42,
               "selective topology refresh changed the warmed read/store result");
    };
    invoke();
    const auto warm = engine.jit_stats();
    const auto same_index = data + transmute::NativeReadTable::entry_count * 4096;
    engine.map_memory(same_index, 16);
    invoke();
    std::array<std::uint8_t, 16> external{};
    engine.map_external_memory(same_index + 4096, external.data(), external.size(), true);
    invoke();
    engine.reserve_memory(same_index + 8192, 4096);
    engine.set_native_import_resolver(nullptr);
    invoke();
    engine.unmap_memory(same_index + 8192);
    {
        auto worker = engine.fork_thread(image.entry(), data);
        invoke(); // A worker stack addition must retain these heap spans.
    }
    invoke(); // Its stack removal must also retain them.
    auto dll_bytes = executable({0xd65f03c0U});
    put16(dll_bytes, 0x96, 0x2000);
    put64(dll_bytes, 0x98 + 24, 0x180000000ULL);
    auto module = transmute::PeImage::parse(dll_bytes);
    engine.add_module(module);
    invoke();
    const auto after = engine.jit_stats();
    const auto topology = engine.native_topology_stats();
    expect(warm.native_read_refills.successful == 1 && warm.native_store_refills.successful == 1 &&
               after.native_read_refills.successful == 1 && after.native_store_refills.successful == 1 &&
               topology.selective_refreshes >= 6 && topology.replayed_changes >= 8 &&
               topology.retained_read_entries != 0 && topology.retained_write_entries != 0 &&
               topology.evicted_read_entries == 0 && topology.evicted_write_entries == 0 &&
               topology.full_flushes == 0,
           "unrelated mappings, metadata, stacks or published DLLs flushed valid native spans");
}

void test_native_topology_affected_backing_permissions() {
    constexpr std::uint64_t data = 0x50d100000ULL;
    for (const bool store : {false, true}) {
        auto image = transmute::PeImage::parse(executable({
            store ? 0xf9000001U : 0xf9400002U,
            store ? 0xf9000001U : 0xf9400000U, 0xd65f03c0U}));
        transmute::ReferenceEngine engine(image);
        engine.set_jit_enabled(true);
        engine.set_jit_profiling(true);
        engine.map_memory(data, 16);
        engine.write64(data, 42);
        const std::array<std::uint64_t, 2> args{data, 42};
        const auto invoke = [&] {
            return engine.invoke_callback(image.entry(), args);
        };
        expect(invoke() == (store ? data : 42), "native topology permission fixture did not warm");
        const auto warm = native_refills(engine, store);
        engine.set_memory_access(data, store, !store, false);
        expect_failure([&] { static_cast<void>(invoke()); },
                       store ? "guest write to read-only mapped memory"
                             : "guest read from inaccessible mapped memory");
        engine.set_memory_access(data, true, true, false);
        expect(invoke() == (store ? data : 42) &&
                   native_refills(engine, store).successful == warm.successful + 1,
               "restored native permissions reused an invalidated span");
        engine.unmap_memory(data);
        engine.map_memory(data, 16);
        engine.write64(data, 43);
        expect(invoke() == (store ? data : 43) && engine.read64(data) == (store ? 42U : 43U) &&
                   native_refills(engine, store).successful == warm.successful + 2,
               "native topology replay dereferenced a freed backing after remapping");
        const auto topology = engine.native_topology_stats();
        expect((store ? topology.evicted_write_entries : topology.evicted_read_entries) >= 2 &&
                   topology.full_flushes == 0,
               "affected backing and permission changes bypassed selective eviction");
    }
}

void test_native_topology_overflow_drops_dead_spans() {
    constexpr std::uint64_t data = 0x50d200000ULL;
    auto image = transmute::PeImage::parse(executable({
        0xf9400002U, 0xf9400000U, 0xd65f03c0U}));
    transmute::ReferenceEngine engine(image);
    engine.set_jit_enabled(true);
    engine.set_jit_profiling(true);
    engine.map_memory(data, 16);
    engine.write64(data, 42);
    expect(engine.invoke_callback(image.entry(), data) == 42,
           "native topology overflow fixture did not warm its old backing");
    engine.unmap_memory(data);
    std::array<std::uint8_t, 16> external{};
    external[0] = 77;
    engine.map_external_memory(data, external.data(), external.size(), true);
    for (std::size_t index = 0; index <= transmute::NativeTopologyHistory::capacity; ++index)
        engine.set_native_import_resolver(nullptr);
    expect(engine.invoke_callback(image.entry(), data) == 77 &&
               engine.native_topology_stats().full_flushes == 1 &&
               engine.jit_stats().native_read_refills.rejections[static_cast<std::size_t>(
                   transmute::NativeSpanRefillRejection::external)] != 0,
           "a history gap resurrected dead backing or bypassed the external checked path");
}

void test_native_topology_private_staged_overlap() {
    constexpr std::uint64_t module_base = 0x180000000ULL;
    auto image = transmute::PeImage::parse(executable({
        0xf9400002U, 0xf9400000U, 0xd65f03c0U}));
    transmute::ReferenceEngine engine(image);
    engine.set_jit_enabled(true);
    engine.set_jit_profiling(true);
    auto other = engine.fork_thread(image.entry(), 0);
    {
        auto dll_bytes = executable({0xd65f03c0U});
        put16(dll_bytes, 0x96, 0x2000);
        put64(dll_bytes, 0x98 + 24, module_base);
        put64(dll_bytes, 0x300, 42);
        auto staged = transmute::PeImage::parse(dll_bytes);
        engine.stage_module(staged);
        expect(engine.invoke_callback(image.entry(), module_base + 0x1100) == 42,
               "native topology staged fixture did not cache private PE backing");
        // The other engine cannot see this private module, so its valid map
        // can replace the backing selected by the first engine's checked read.
        other->map_memory(module_base, 0x2000);
        other->write64(module_base + 0x1100, 77);
        expect(engine.invoke_callback(image.entry(), module_base + 0x1100) == 77,
               "unrelated-engine mapping retained a shadowed private PE read span");
        engine.discard_staged_modules();
    }
    other->unmap_memory(module_base);
    other->map_memory(module_base, 0x2000);
    other->write64(module_base + 0x1100, 88);
    expect(engine.invoke_callback(image.entry(), module_base + 0x1100) == 88 &&
               engine.native_topology_stats().evicted_read_entries >= 2 &&
               engine.native_topology_stats().full_flushes == 0,
           "discarded private image or remapped shared backing escaped selective invalidation");
}

void test_native_topology_private_partial_shadow() {
    constexpr std::uint64_t module_base = 0x180000000ULL;
    constexpr auto low = module_base + 0x1080;
    constexpr auto shadow = module_base + 0x1100;
    constexpr auto high = module_base + 0x1180;
    const auto code = executable({
        0xf9400002U, // ldr x2,[x0]
        0xf9400023U, // ldr x3,[x1]
        0xf9400080U, // ldr x0,[x4]
        0x8b020000U, // add x0,x0,x2
        0x8b030000U, // add x0,x0,x3
        0xd65f03c0U});
    auto ri = transmute::PeImage::parse(code);
    auto ni = transmute::PeImage::parse(code);
    auto dll_bytes = executable({0xd65f03c0U});
    put16(dll_bytes, 0x96, 0x2002U);
    put64(dll_bytes, 0x98 + 24, module_base);
    put64(dll_bytes, 0x280, 11);
    put64(dll_bytes, 0x300, 13);
    put64(dll_bytes, 0x380, 17);
    auto rd = transmute::PeImage::parse(dll_bytes);
    auto nd = transmute::PeImage::parse(dll_bytes);
    transmute::ReferenceEngine reference(ri), native(ni);
    native.set_jit_enabled(true);
    native.set_jit_profiling(true);
    auto reference_other = reference.fork_thread(ri.entry(), 0);
    auto native_other = native.fork_thread(ni.entry(), 0);
    reference.stage_module(rd);
    native.stage_module(nd);
    const auto verify = [&](std::uint64_t value) {
        for (const bool reverse : {false, true}) {
            // All three complete 8-byte loads share the same table index.
            // Refill from either private PE gap must exclude the middle map.
            const std::array<std::uint64_t, 5> args{
                reverse ? high : low, shadow, 0, 0, reverse ? low : high};
            const auto expected = reference.invoke_callback(ri.entry(), args);
            const auto actual = native.invoke_callback(ni.entry(), args);
            expect(actual == expected && actual == 11 + value + 17,
                   "private native PE read span covered a partial shared shadow mapping");
        }
    };
    verify(13); // Warm whole private PE backing before a partial map appears.
    for (auto* other : {reference_other.get(), native_other.get()}) {
        other->map_memory(shadow, 8);
        other->write64(shadow, 31);
    }
    verify(31);
    for (auto* other : {reference_other.get(), native_other.get()})
        other->unmap_memory(shadow);
    verify(13);
    for (auto* other : {reference_other.get(), native_other.get()}) {
        other->map_memory(shadow, 8);
        other->write64(shadow, 73);
    }
    verify(73);
    for (auto* other : {reference_other.get(), native_other.get()}) {
        other->unmap_memory(shadow);
        other->map_memory(shadow, 8);
        other->write64(shadow, 97);
    }
    verify(97);
    expect(native.jit_stats().native_region_instructions >= 60 &&
               native.jit_stats().fallback_instructions == 0,
           "partial private PE shadow fixture did not exercise complete native loads");
    reference.discard_staged_modules();
    native.discard_staged_modules();
}
