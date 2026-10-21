// Included inside test_main.cpp's anonymous namespace on Windows x64.

void test_native_read_span_page_reuse() {
    constexpr std::uint64_t mapped = 0x500900000ULL;
    constexpr auto pages = transmute::NativeReadTable::entry_count + 64;
    constexpr auto image_size = static_cast<std::uint32_t>((pages + 4) * 4096);
    for (const bool image_memory : {false, true}) {
        auto bytes = executable({
            0xf9400001U, // ldr x1,[x0]
            0x91400400U, // add x0,x0,#0x1000
            0xd1000442U, // sub x2,x2,#1
            0xb5ffffa2U, // cbnz x2,-12
            0xd65f03c0U
        });
        if (image_memory) {
            put32(bytes, 0x98 + 56, image_size);
            put32(bytes, 0x188 + 8, image_size - 0x1000);
        }
        auto image = transmute::PeImage::parse(bytes);
        transmute::ReferenceEngine engine(image);
        engine.set_jit_enabled(true);
        if (!image_memory) engine.map_memory(mapped, (pages + 2) * 4096);
        const auto data = image_memory ? image.base() + 0x2000 : mapped;
        const std::array<std::uint64_t, 3> arguments{data, 0, pages};
        const auto before = engine.jit_stats();
        expect(engine.invoke_callback(image.entry(), arguments) == data + pages * 4096,
               "native span page-reuse fixture changed its counted load loop");
        const auto warmed = engine.jit_stats();
        expect(warmed.compiled_native_regions != 0 &&
                   warmed.native_read_misses - before.native_read_misses <=
                       transmute::NativeReadTable::entry_count,
               "native span reuse refilled every page despite a valid complete extent");
        expect(engine.invoke_callback(image.entry(), arguments) == data + pages * 4096 &&
                   engine.jit_stats().native_read_misses == warmed.native_read_misses,
               "warm native span did not retain coverage across more pages than cache slots");
    }
}
