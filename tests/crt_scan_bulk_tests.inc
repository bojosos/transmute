// Included after BridgeCopyMemory and the existing Windows bridge fixtures.

std::uint64_t invoke_bulk_scan(transmute::Win32Bridge& bridge,
                               transmute::GuestMemory& memory,
                               std::uint64_t input, std::uint64_t format,
                               std::uint64_t count, bool wide = false,
                               std::uint64_t arguments = 0) {
    transmute::CpuState state{};
    state.x[1] = input;
    state.x[2] = count;
    state.x[3] = format;
    state.x[5] = arguments;
    static_cast<void>(bridge.dispatch(
        {"api-ms-win-crt-stdio-l1-1-0.dll",
         wide ? "__stdio_common_vswscanf" : "__stdio_common_vsscanf", 0},
        state, memory));
    return state.x[0];
}

void test_guest_crt_scan_bulk_boundaries() {
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine engine(image);
    transmute::Win32Bridge bridge;
    constexpr std::uint64_t input = 0x505010000ULL;
    const auto format = engine.state().sp - 256;
    const auto arguments = format + 64;
    const auto output = arguments + 64;
    const std::array<std::uint8_t, 3> narrow{'1', '2', '3'};
    const std::array<std::uint8_t, 5> narrow_format{'%', 'd', '%', 'n', 0};
    engine.map_memory(input, narrow.size());
    engine.write_bytes(input, narrow);
    engine.write_bytes(format, narrow_format);
    engine.write64(arguments, output);
    engine.write64(arguments + 8, output + 8);
    expect(invoke_bulk_scan(bridge, engine, input, format, 3, false, arguments) == 1 &&
               engine.read32(output) == 123 && engine.read32(output + 8) == 3,
           "bulk scanf snapshot read beyond a bounded unterminated input");
    expect_failure([&] { invoke_bulk_scan(bridge, engine, input, format, 4); },
                   "guest read outside mapped memory");
    engine.unmap_memory(input);
    const std::array<std::uint8_t, 4> terminated{'1', '2', '3', 0};
    engine.map_memory(input, terminated.size());
    engine.write_bytes(input, terminated);
    expect(invoke_bulk_scan(bridge, engine, input, format, UINT64_MAX, false,
                           arguments) == 1 && engine.read32(output) == 123,
           "bulk scanf snapshot read past a narrow boundary terminator");
    engine.unmap_memory(input);

    const std::array<std::uint8_t, 8> wide{'1', 0, '2', 0, '3', 0, 0, 0};
    const std::array<std::uint8_t, 10> wide_format{'%', 0, 'd', 0, '%', 0, 'n', 0, 0, 0};
    engine.map_memory(input, 3);
    engine.map_memory(input + 3, 5);
    engine.write_bytes(input, wide);
    engine.write_bytes(format, wide_format);
    expect(invoke_bulk_scan(bridge, engine, input, format, UINT64_MAX, true,
                           arguments) == 1 && engine.read32(output) == 123 &&
               engine.read32(output + 8) == 3,
           "bulk scanf snapshot lost UTF-16 units split across mappings");
    engine.unmap_memory(input);
    engine.unmap_memory(input + 3);
    engine.map_memory(input, 1);
    engine.map_memory(input + 1, 1);
    engine.fill_bytes(input, 0, 2);
    engine.set_memory_access(input + 1, false, true, false);
    expect_failure([&] { invoke_bulk_scan(bridge, engine, input, format, 1, true); },
                   "guest read from inaccessible mapped memory");
    engine.write_bytes(format, std::array<std::uint8_t, 1>{0});
    engine.write_bytes(format + 1, std::array<std::uint8_t, 1>{0});
    expect(invoke_bulk_scan(bridge, engine, UINT64_MAX, format, 0, true) == 0,
           "zero-count bulk scanf snapshot touched an invalid input address");
    engine.unmap_memory(input);
    engine.unmap_memory(input + 1);

    constexpr std::size_t maximum = 1024U * 1024U;
    for (const bool is_wide : {false, true}) {
        const auto unit = is_wide ? 2U : 1U;
        engine.map_memory(input, maximum * unit);
        engine.fill_bytes(input, 'A', maximum * unit);
        expect(invoke_bulk_scan(bridge, engine, input, format, maximum, is_wide) == 0,
               "bulk scanf rejected the exact supported bounded input size");
        expect_failure([&] {
            invoke_bulk_scan(bridge, engine, input, format, maximum + 1, is_wide);
        }, "guest scanf string exceeds supported size");
        engine.fill_bytes(input + (maximum - 1) * unit, 0, unit);
        expect(invoke_bulk_scan(bridge, engine, input, format, UINT64_MAX, is_wide) == 0,
               "bulk scanf rejected a terminator within the supported size cap");
        engine.unmap_memory(input);
    }
}

void test_guest_crt_scan_bulk_overflow() {
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine engine(image);
    transmute::Win32Bridge bridge;
    const auto empty_format = engine.state().sp - 128;
    engine.fill_bytes(empty_format, 0, 2);
    constexpr auto narrow = UINT64_MAX - 1;
    engine.map_memory(narrow, 1);
    engine.write_bytes(narrow, std::array<std::uint8_t, 1>{'A'});
    expect(invoke_bulk_scan(bridge, engine, narrow, empty_format, 1) == 0,
           "bulk scanf overflow guard rejected a safe final input unit");
    expect_failure([&] { invoke_bulk_scan(bridge, engine, narrow, 0x1234, 2); },
                   "guest scanf string address overflow");
    engine.write_bytes(narrow, std::array<std::uint8_t, 1>{0});
    expect(invoke_bulk_scan(bridge, engine, narrow, empty_format, UINT64_MAX) == 0,
           "bulk scanf overflow guard rejected an earlier narrow terminator");
    engine.unmap_memory(narrow);
    constexpr auto wide = UINT64_MAX - 3;
    engine.map_memory(wide, 3);
    const std::array<std::uint8_t, 3> unit{'A', 0, 'B'};
    engine.write_bytes(wide, unit);
    expect(invoke_bulk_scan(bridge, engine, wide, empty_format, 1, true) == 0,
           "bulk scanf overflow guard rejected a safe final UTF-16 unit");
    expect_failure([&] { invoke_bulk_scan(bridge, engine, wide, 0x1234, 2, true); },
                   "guest scanf string address overflow");
    engine.fill_bytes(wide, 0, 2);
    expect(invoke_bulk_scan(bridge, engine, wide, empty_format, UINT64_MAX, true) == 0,
           "bulk scanf overflow guard rejected an earlier UTF-16 terminator");
    expect_failure([&] {
        invoke_bulk_scan(bridge, engine, UINT64_MAX, empty_format, 1);
    }, "guest scanf string address overflow");
}

void test_guest_crt_scan_bulk_virtual_callbacks() {
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    BridgeCopyMemory memory(image);
    memory.chunks_enabled = false;
    transmute::Win32Bridge bridge;
    const auto input = memory.state().sp - 256;
    const auto format = input + 64;
    memory.fill_bytes(format, 0, 2);
    const std::array<std::uint8_t, 3> narrow{0xe9, 'Q', 0};
    memory.write_bytes(input, narrow);
    memory.reset_counts();
    expect(invoke_bulk_scan(bridge, memory, input, format, UINT64_MAX) == 0 &&
               memory.byte_reads == 4 && memory.read_chunks == 0,
           "bulk scanf bypassed overridden narrow byte callbacks");
    const std::array<std::uint8_t, 4> wide{0, 0x20, 0, 0};
    memory.write_bytes(input, wide);
    memory.reset_counts();
    expect(invoke_bulk_scan(bridge, memory, input, format, UINT64_MAX, true) == 0 &&
               memory.byte_reads == 6 && memory.read_chunks == 0,
           "bulk scanf skipped a wide high byte or custom callback");
    memory.reset_counts();
    expect(invoke_bulk_scan(bridge, memory, UINT64_MAX, format, 0, true) == 0 &&
               memory.byte_reads == 2,
           "zero-count bulk scanf invoked custom input callbacks");
    constexpr auto high = UINT64_MAX - 1;
    memory.map_memory(high, 1);
    memory.write8(high, 'A');
    memory.reset_counts();
    expect_failure([&] { invoke_bulk_scan(bridge, memory, high, format, 2); },
                   "guest scanf string address overflow");
    expect(memory.byte_reads == 1,
           "bulk scanf overflow changed the successful custom read prefix");
}
