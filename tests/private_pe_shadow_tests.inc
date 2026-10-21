// Included inside test_main.cpp's anonymous namespace on Windows x64.

void test_private_pe_shadow_checked_bulk_seams() {
    constexpr std::uint64_t module_base = 0x180000000ULL;
    constexpr auto shadow = module_base + 0x1100;
    constexpr auto start = shadow - 4;
    constexpr std::uint64_t buffer = 0x50e100000ULL;
    enum class Operation { wide_read, wide_write, read, write, fill,
                           copy_in, copy_out, move_forward, move_backward,
                           string_byte, string_wide };
    auto dll_bytes = executable({0xd65f03c0U});
    put16(dll_bytes, 0x96, 0x2002U);
    put64(dll_bytes, 0x98 + 24, module_base);
    put32(dll_bytes, 0x188 + 36, 0xe0000020U);
    std::fill(dll_bytes.begin() + 0x2f0, dll_bytes.begin() + 0x330, 0x51);
    for (const auto operation : {Operation::wide_read, Operation::wide_write,
            Operation::read, Operation::write, Operation::fill, Operation::copy_in,
            Operation::copy_out, Operation::move_forward, Operation::move_backward,
            Operation::string_byte, Operation::string_wide}) {
        for (const unsigned permissions : {3U, 2U, 1U}) {
            // Exercise a PE start crossing into the map, a mapped start crossing
            // back into PE, and a whole mapped width with denied permissions.
            const bool wide = operation == Operation::wide_read ||
                              operation == Operation::wide_write;
            for (const unsigned seam : {0U, 1U, 2U}) {
                if (!wide && seam != 0) continue;
                auto ri = transmute::PeImage::parse(executable({0xd65f03c0U}));
                auto ni = transmute::PeImage::parse(executable({0xd65f03c0U}));
                auto rd = transmute::PeImage::parse(dll_bytes);
                auto nd = transmute::PeImage::parse(dll_bytes);
                transmute::ReferenceEngine reference(ri), optimized(ni);
                auto reference_other = reference.fork_thread(ri.entry(), 0);
                auto optimized_other = optimized.fork_thread(ni.entry(), 0);
                reference.stage_module(rd);
                optimized.stage_module(nd);
                for (auto* other : {reference_other.get(), optimized_other.get()}) {
                    other->map_memory(shadow, 8);
                    other->write64(shadow, 0x00005a5958575655ULL);
                    other->map_memory(buffer, 32);
                    other->fill_bytes(buffer, 0xa3, 32);
                    other->set_memory_access(shadow, (permissions & 1) != 0,
                                             (permissions & 2) != 0, false);
                }
                const auto address = seam == 0 ? start : seam == 1 ? shadow + 4 : shadow;
                const auto revision = optimized.write_revision(shadow, 8);
                std::array<std::uint8_t, 24> expected{}, actual{}, payload{};
                expected.fill(0xee);
                actual = expected;
                payload.fill(0xa3);
                constexpr std::uint64_t value = 0x8877665544332211ULL;
                if (wide && seam != 2) {
                    expect(!optimized.try_read_contiguous(address, 8) &&
                               !optimized.try_write_contiguous(address, 8, value),
                           "private PE shadow whole-width probe crossed a backing seam");
                } else if (wide) {
                    expect(optimized.try_read_contiguous(address, 8).has_value() ==
                               ((permissions & 1) != 0),
                           "private PE shadow whole-width probe bypassed mapped read permissions");
                    if ((permissions & 2) == 0)
                        expect(!optimized.try_write_contiguous(address, 8, value),
                               "private PE shadow whole-width probe bypassed mapped write permissions");
                }
                std::uint64_t expected_value{}, actual_value{};
                bool expected_terminated{}, actual_terminated{};
                auto& reference_memory = static_cast<transmute::GuestMemory&>(reference);
                auto& reference_shared = static_cast<transmute::GuestMemory&>(*reference_other);
                auto& optimized_shared = static_cast<transmute::GuestMemory&>(*optimized_other);
                const auto oracle = [&] {
                    if (operation == Operation::wide_read) {
                        for (unsigned index = 0; index < 8; ++index)
                            expected_value |= std::uint64_t{reference_memory.read8(address + index)} << (index * 8);
                    } else if (operation == Operation::wide_write) {
                        for (unsigned index = 0; index < 8; ++index)
                            reference_memory.write8(address + index, static_cast<std::uint8_t>(value >> (index * 8)));
                    } else if (operation == Operation::string_byte || operation == Operation::string_wide) {
                        const auto width = operation == Operation::string_byte ? 1U : 2U;
                        for (unsigned index = 0; index < 12; ++index) {
                            const auto low = reference_memory.read8(start + index * width);
                            const auto high = width == 2 ? reference_memory.read8(start + index * width + 1) : 0;
                            if (low == 0 && high == 0) { expected_terminated = true; break; }
                            ++expected_value;
                        }
                    } else {
                        const bool backward = operation == Operation::move_backward;
                        const unsigned count = backward || operation == Operation::move_forward ? 23 : 24;
                        for (unsigned completed = 0; completed < count; ++completed) {
                            const auto index = backward ? count - completed - 1 : completed;
                            if (operation == Operation::read) expected[index] = reference_memory.read8(start + index);
                            else if (operation == Operation::write || operation == Operation::fill)
                                reference_memory.write8(start + index, payload[index]);
                            else if (operation == Operation::copy_in)
                                reference_memory.write8(start + index, reference_memory.read8(buffer + index));
                            else if (operation == Operation::copy_out)
                                reference_memory.write8(buffer + index, reference_memory.read8(start + index));
                            else if (backward)
                                reference_memory.write8(start + index + 1, reference_memory.read8(start + index));
                            else reference_memory.write8(start + index, reference_memory.read8(start + index + 1));
                        }
                    }
                };
                const auto fast = [&] {
                    switch (operation) {
                    case Operation::wide_read: actual_value = optimized.read64(address); break;
                    case Operation::wide_write: optimized.write64(address, value); break;
                    case Operation::read: optimized.read_bytes(start, actual); break;
                    case Operation::write: optimized.write_bytes(start, payload); break;
                    case Operation::fill: optimized.fill_bytes(start, 0xa3, 24); break;
                    case Operation::copy_in: optimized.copy_bytes(start, buffer, 24); break;
                    case Operation::copy_out: optimized.copy_bytes(buffer, start, 24); break;
                    case Operation::move_forward: optimized.copy_bytes(start, start + 1, 23, true); break;
                    case Operation::move_backward: optimized.copy_bytes(start + 1, start, 23, true); break;
                    case Operation::string_byte:
                    case Operation::string_wide: {
                        const auto width = operation == Operation::string_byte ? 1U : 2U;
                        actual_value = optimized.string_length(start, width, 12, &actual_terminated);
                        const auto snapshot = optimized.read_string(start, width, 12);
                        expect(snapshot.size() == actual_value * width,
                               "private PE shadow string snapshot crossed the mapped terminator");
                        break;
                    }
                    }
                };
                std::string expected_fault, actual_fault;
                try { oracle(); } catch (const std::runtime_error& error) { expected_fault = error.what(); }
                try { fast(); } catch (const std::runtime_error& error) { actual_fault = error.what(); }
                expect(expected_fault == actual_fault && expected == actual &&
                           (!expected_fault.empty() || (expected_value == actual_value &&
                                                       expected_terminated == actual_terminated)),
                       "private PE shadow changed scalar/bulk/string fault order or completed read prefix");
                const auto after_revision = optimized.write_revision(shadow, 8);
                // Inspect canonical backing after restoring access, including
                // hidden PE bytes that must never receive a mapped store.
                for (auto* other : {reference_other.get(), optimized_other.get()})
                    other->set_memory_access(shadow, true, true, false);
                bool mapped_changed = false;
                for (unsigned index = 0; index < 64; ++index)
                    expect(rd.read8(shadow - 16 + index) == nd.read8(shadow - 16 + index),
                           "private PE shadow store changed hidden PE backing or its successful prefix/suffix");
                for (unsigned index = 0; index < 32; ++index)
                    expect(reference_shared.read8(buffer + index) == optimized_shared.read8(buffer + index),
                           "private PE shadow copy changed its completed destination prefix");
                for (unsigned index = 0; index < 8; ++index) {
                    const auto byte = optimized_shared.read8(shadow + index);
                    expect(reference_shared.read8(shadow + index) == byte,
                           "private PE shadow store skipped or overwrote mapped backing");
                    mapped_changed |= byte != static_cast<std::uint8_t>(0x00005a5958575655ULL >> (index * 8));
                }
                if (mapped_changed && revision)
                    expect(after_revision && *after_revision > *revision,
                           "private PE shadow successful mapped writes lost revision tracking");
                reference.discard_staged_modules();
                optimized.discard_staged_modules();
            }
        }
    }
    // A bulk store crossing the private/shared seam still invalidates another
    // engine's LL/SC reservation, including writes that return identical bytes.
    auto image = transmute::PeImage::parse(executable({
        0xc85ffc81U, 0xc803fc82U, 0xd65f03c0U}));
    auto dll = transmute::PeImage::parse(dll_bytes);
    transmute::ReferenceEngine engine(image);
    auto reader = engine.fork_thread(image.entry(), 0);
    engine.stage_module(dll);
    reader->map_memory(shadow, 8);
    reader->fill_bytes(shadow, 0x51, 8);
    reader->set_initial_register(4, shadow);
    reader->set_initial_register(2, 99);
    expect_failure([&] { static_cast<void>(reader->run(1)); }, "guest exceeded instruction limit");
    const auto before = engine.write_revision(shadow, 8);
    engine.fill_bytes(start, 0x51, 24);
    static_cast<void>(reader->run());
    expect(reader->state().x[3] == 1 && reader->read64(shadow) == 0x5151515151515151ULL &&
               before && engine.write_revision(shadow, 8) > before,
           "private PE shadow bulk seam lost identical mapped write revision or LL/SC history");
    engine.discard_staged_modules();
}
