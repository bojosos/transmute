// dispatch_data_t (dispatch_data.cpp), as a renderer plug-in's images call it (split from the export-fill tests).
#include "alchemy/darwin/kit/export_fill/tests/export_fill_test_kit.hpp"

namespace {

void test_guest_dispatch_data(Fixture& f) {
    const auto system = "/usr/lib/libSystem.B.dylib";
    // dispatch_data: copies, maps, subranges, concatenation, apply and release.
    {
        const auto source = f.text("hello dispatch");
        const auto data = f.sys("_dispatch_data_create", {source, 14, 0, 0});
        require(data && f.sys("_dispatch_data_get_size", {data}) == 14, "dispatch_data_create size");
        static_cast<transmute::GuestMemory&>(f.engine).write8(source, 'J'); // the object owns a copy
        const auto buffer = f.process.allocate_guest(8), size = f.process.allocate_guest(8);
        const auto map = f.sys("_dispatch_data_create_map", {data, buffer, size});
        require(map && f.engine.read64(size) == 14 && f.read(f.engine.read64(buffer)).substr(0, 14) == "hello dispatch", "dispatch_data_create_map");
        const auto sub = f.sys("_dispatch_data_create_subrange", {data, 6, 100});
        require(f.sys("_dispatch_data_get_size", {sub}) == 8, "dispatch_data_create_subrange clamps");
        const auto both = f.sys("_dispatch_data_create_concat", {data, sub});
        require(f.sys("_dispatch_data_get_size", {both}) == 22, "dispatch_data_create_concat");
        const auto empty = f.symbol(system, "__dispatch_data_empty");
        require(f.sys("_dispatch_data_get_size", {empty}) == 0 && f.sys("_dispatch_data_create", {source, 0, 0, 0}) == empty, "DISPATCH_DATA_EMPTY");
        static std::uint64_t applied = 0;
        applied = 0;
        const auto invoke = f.process.install_function("_export_fill_test_applier", [](transmute::CpuState& s, transmute::GuestMemory&) {
            applied += s.x[4];
            s.x[0] = 1;
        }, "/alchemy/test/export_fill");
        const auto block = f.process.allocate_guest(32);
        f.engine.fill_bytes(block, 0, 32);
        f.engine.write64(block + 16, invoke);
        require(f.sys("_dispatch_data_apply", {both, block}) == 1 && applied == 22, "dispatch_data_apply");
        // DISPATCH_DATA_DESTRUCTOR_FREE releases the caller's malloc'd buffer.
        const auto owned = f.sys("_malloc", {4});
        f.engine.write32(owned, 0x61626364);
        const auto free_destructor = f.engine.read64(f.symbol(system, "__dispatch_data_destructor_free"));
        const auto adopted = f.sys("_dispatch_data_create", {owned, 4, 0, free_destructor});
        require(f.sys("_dispatch_data_get_size", {adopted}) == 4, "dispatch_data_create with DISPATCH_DATA_DESTRUCTOR_FREE");
        // data and its map are one object (two references).
        const auto before = f.process.objects().live_count();
        for (const auto object : {map, sub, both, adopted}) f.sys("_dispatch_release", {object});
        require(f.process.objects().live_count() == before - 3, "dispatch_release frees data objects");
        f.sys("_dispatch_retain", {data});
        f.sys("_dispatch_release", {data});
        require(f.process.objects().live_count() == before - 3, "retained data object survives");
        f.sys("_dispatch_release", {data});
        require(f.process.objects().live_count() == before - 4, "last dispatch_release");
        // Other dispatch objects still reach the base provider.
        const auto semaphore = f.sys("_dispatch_semaphore_create", {0});
        f.sys("_dispatch_retain", {semaphore});
        f.sys("_dispatch_release", {semaphore});
        f.sys("_dispatch_release", {semaphore});
    }
}

} // namespace

int main() {
    try {
        const auto stage = [](std::string_view name) { std::cout << "[stage] " << name << std::endl; };
        stage("fixture"); Fixture f;
        stage("guest dispatch data"); test_guest_dispatch_data(f);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "dispatch data tests passed\n";
    return 0;
}
