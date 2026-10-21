// Included inside test_main.cpp's anonymous namespace.

void test_guest_scalar_write_batch_history_and_faults() {
    constexpr std::uint64_t data = 0x50d040000ULL;
    const std::array fields{
        transmute::GuestScalarWrite{data, 0, 8},
        transmute::GuestScalarWrite{data + 8, 0x11223344, 4},
        transmute::GuestScalarWrite{data + 12, 0x55667788, 4},
        transmute::GuestScalarWrite{data + 16, 0x0123456789abcdefULL, 8},
        transmute::GuestScalarWrite{data + 24, 0, 8},
        transmute::GuestScalarWrite{data + 32, 400, 8},
    };
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine engine(image);
    engine.map_memory(data - 8, 56);
    engine.fill_bytes(data - 8, 0xcc, 56);
    const auto before = engine.write_revision(data - 8, 56);
    engine.write_scalar_fields(fields);
    expect(before && engine.write_revision(data - 8, 56) == *before + 6 &&
               engine.read64(data - 8) == 0xccccccccccccccccULL &&
               engine.read64(data + 40) == 0xccccccccccccccccULL &&
               engine.read64(data) == 0 && engine.read32(data + 8) == 0x11223344 &&
               engine.read32(data + 12) == 0x55667788 &&
               engine.read64(data + 16) == 0x0123456789abcdefULL &&
               engine.read64(data + 24) == 0 && engine.read64(data + 32) == 400,
           "typed scalar batch changed field widths, six-write history or adjacent bytes");

    for (const bool protected_field : {false, true}) {
        auto li = transmute::PeImage::parse(executable({0xd65f03c0U}));
        auto bi = transmute::PeImage::parse(executable({0xd65f03c0U}));
        transmute::ReferenceEngine legacy(li), batched(bi);
        for (auto* memory : {&legacy, &batched}) {
            // The second four-byte field either has only its first two bytes
            // mapped, or lies in a readable but protected adjacent extent.
            memory->map_memory(data, protected_field ? 12 : 14);
            memory->fill_bytes(data, 0xcc, protected_field ? 12 : 14);
            if (protected_field) {
                memory->map_memory(data + 12, 28);
                memory->fill_bytes(data + 12, 0xcc, 28);
                memory->set_memory_access(data + 12, true, false, false);
                memory->write64(data, UINT64_MAX); // Make prefix the latest revision.
            }
        }
        const auto dirty = batched.write_revision(data, protected_field ? 12 : 14);
        const auto fault = protected_field ? "guest write to read-only mapped memory"
                                           : "guest write outside mapped memory";
        expect_failure([&] {
            legacy.write64(data, 0);
            legacy.write32(data + 8, 0x11223344);
            legacy.write32(data + 12, 0x55667788);
            legacy.write64(data + 16, 0x0123456789abcdefULL);
            legacy.write64(data + 24, 0);
            legacy.write64(data + 32, 400);
        }, fault);
        expect_failure([&] { batched.write_scalar_fields(fields); }, fault);
        const auto available = protected_field ? 40U : 14U;
        for (unsigned offset = 0; offset != available; ++offset)
            expect(static_cast<transmute::GuestMemory&>(batched).read8(data + offset) ==
                       static_cast<transmute::GuestMemory&>(legacy).read8(data + offset),
                   "typed batch changed a checked field fault's exact successful prefix");
        expect(dirty && batched.write_revision(data, protected_field ? 12 : 14) ==
                   *dirty + (protected_field ? 2U : 4U),
               "typed batch changed the width/byte write history before its field fault");
    }

    class ObservedMemory final : public transmute::ReferenceEngine {
    public:
        using ReferenceEngine::ReferenceEngine;
        std::vector<unsigned> widths;
        void write8(std::uint64_t address, std::uint8_t value) override {
            widths.push_back(1);
            ReferenceEngine::write8(address, value);
        }
        void write32(std::uint64_t address, std::uint32_t value) override {
            widths.push_back(4);
            ReferenceEngine::write32(address, value);
        }
        void write64(std::uint64_t address, std::uint64_t value) override {
            widths.push_back(8);
            ReferenceEngine::write64(address, value);
        }
    } observed(image);
    observed.map_memory(data, 64);
    observed.write_scalar_fields(fields);
    expect(observed.widths == std::vector<unsigned>{8, 4, 4, 8, 8, 8},
           "typed batch bypassed a derived checked scalar memory contract");
    observed.widths.clear();
    const std::array narrow{
        transmute::GuestScalarWrite{data + 40, 0x1234, 1},
        transmute::GuestScalarWrite{data + 41, 0x5678, 2},
    };
    observed.write_scalar_fields(narrow);
    expect(observed.widths == std::vector<unsigned>{1, 1, 1} &&
               static_cast<transmute::GuestMemory&>(observed).read8(data + 40) == 0x34 &&
               static_cast<transmute::GuestMemory&>(observed).read8(data + 41) == 0x78 &&
               static_cast<transmute::GuestMemory&>(observed).read8(data + 42) == 0x56,
           "typed narrow batch lost checked byte dispatch or little-endian truncation");
}

void test_critical_section_typed_mirror_batch() {
#ifdef _WIN32
    constexpr std::uint64_t data = 0x50d050000ULL;
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine engine(image);
    engine.map_memory(data - 8, 56);
    engine.fill_bytes(data - 8, 0xcc, 56);
    transmute::Win32Bridge bridge;
    transmute::CpuState state{};
    state.x[0] = data;
    state.x[1] = 400;
    const auto before = engine.write_revision(data - 8, 56);
    expect(before.has_value(), "critical-section fixture has no owned write revision");
    static_cast<void>(bridge.dispatch(
        {"kernel32.dll", "InitializeCriticalSectionEx", 0}, state, engine));
    const bool initialized = state.x[0] == 1 && before &&
        engine.write_revision(data - 8, 56) == *before + 6 &&
        engine.read64(data) == 0 && engine.read32(data + 8) == UINT32_MAX &&
        engine.read32(data + 12) == 0 && engine.read64(data + 16) == 0 &&
        engine.read64(data + 24) == 0 && engine.read64(data + 32) == 400;
    state.x[0] = data;
    static_cast<void>(bridge.dispatch(
        {"kernel32.dll", "EnterCriticalSection", 0}, state, engine));
    const bool entered = engine.write_revision(data - 8, 56) == *before + 12 &&
        engine.read32(data + 12) == 1 && engine.read64(data + 16) == GetCurrentThreadId();
    state.x[0] = data;
    static_cast<void>(bridge.dispatch(
        {"kernel32.dll", "LeaveCriticalSection", 0}, state, engine));
    const bool released = engine.write_revision(data - 8, 56) == *before + 18 &&
        engine.read32(data + 12) == 0 && engine.read64(data + 16) == 0 &&
        engine.read64(data - 8) == 0xccccccccccccccccULL &&
        engine.read64(data + 40) == 0xccccccccccccccccULL;
    state.x[0] = data;
    static_cast<void>(bridge.dispatch(
        {"kernel32.dll", "DeleteCriticalSection", 0}, state, engine));
    expect(initialized && entered && released,
           "critical-section batch changed its 40-byte layout, six-write history or canaries");

    class CountedMemory final : public transmute::ReferenceEngine {
    public:
        using ReferenceEngine::ReferenceEngine;
        unsigned batches{};
        std::vector<unsigned> widths;
        void write_scalar_fields(std::span<const transmute::GuestScalarWrite> fields) override {
            ++batches;
            for (const auto& field : fields) widths.push_back(field.width);
            ReferenceEngine::write_scalar_fields(fields);
        }
    } counted(image);
    counted.map_memory(data, 40);
    transmute::Win32Bridge observed;
    state = {};
    state.x[0] = data;
    static_cast<void>(observed.dispatch(
        {"kernel32.dll", "InitializeCriticalSection", 0}, state, counted));
    const bool one_batch = counted.batches == 1 &&
        counted.widths == std::vector<unsigned>{8, 4, 4, 8, 8, 8};
    state.x[0] = data;
    static_cast<void>(observed.dispatch(
        {"kernel32.dll", "DeleteCriticalSection", 0}, state, counted));
    expect(one_batch, "critical-section mirror did not submit one six-field checked batch");
#endif
}
