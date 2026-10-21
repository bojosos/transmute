// Included after forwarder_import_executable inside test_main.cpp.
#ifdef _WIN32

std::vector<std::uint8_t> native_import_fixture(std::string_view function) {
    auto bytes = forwarder_import_executable(false);
    std::fill(bytes.begin() + 0x470, bytes.end(), 0);
    constexpr std::string_view dll = "kernel32.dll";
    std::copy(dll.begin(), dll.end(), bytes.begin() + 0x470);
    std::copy(function.begin(), function.end(), bytes.begin() + 0x482);
    put32(bytes, 0x200, 0xd61f0200U); // br x16, whose target is the actual IAT import
    return bytes;
}

void test_native_import_scalar_wrappers() {
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine memory(image);
    transmute::Win32Bridge bridge;
    struct RestoreErrorMode {
        UINT mode = GetErrorMode();
        DWORD error = GetLastError();
        ~RestoreErrorMode() { SetErrorMode(mode); SetLastError(error); }
    } restore;
    constexpr std::array<std::string_view, 9> stable{
        "GetCurrentThreadId", "GetCurrentProcessId", "IsDebuggerPresent",
        "GetErrorMode", "SetErrorMode", "GetACP", "GetOEMCP",
        "EncodePointer", "DecodePointer"};
    for (const auto dll : {"kernel32.dll", "KERNELBASE.DLL",
                           "API-MS-WIN-CORE-SYSINFO-L1-1-0.DLL"}) {
        for (const auto name : stable) {
            const transmute::Import imported{dll, std::string(name), 0};
            const auto target = transmute::resolve_direct_win32_import(imported);
            expect(target != nullptr, "eligible scalar Win32 import was not resolved");
            for (const auto bits : {0ULL, 0xffffffffffffffffULL,
                                     0x12345678abcdef00ULL}) {
                transmute::CpuState reference{}, actual{};
                for (unsigned i = 0; i != reference.x.size(); ++i)
                    reference.x[i] = 0x1234567800000000ULL + i;
                for (unsigned i = 0; i != reference.v.size(); ++i)
                    reference.v[i] = {0x0123456789abcdefULL + i, UINT64_MAX};
                reference.x[0] = name == "SetErrorMode" ?
                    static_cast<std::uint64_t>(restore.mode) | 0xffffffff00000000ULL : bits;
                reference.x[18] = 0; // the direct API does not need a guest TEB
                reference.pc = image.entry();
                reference.sp = 0x1234567887654321ULL;
                reference.nzcv = 0xa0000000U;
                reference.fpcr = 0x07c00000U;
                reference.fpsr = 0x0800009fU;
                actual = reference;
                // Compare API-set bindings with their canonical kernel32 API;
                // the old dispatcher does not resolve raw core contract names.
                const transmute::Import canonical{"kernel32.dll", std::string(name), 0};
                static_cast<void>(bridge.dispatch(canonical, reference, memory));
                target(actual);
                expect(native_region_same_state(reference, actual),
                       "native scalar wrapper changed argument width, result or unrelated CPU state");
            }
        }
    }
    for (const auto name : {"GetTickCount", "GetTickCount64"}) {
        const auto target = transmute::resolve_direct_win32_import({"kernel32.dll", name, 0});
        expect(target != nullptr, "native tick API was not bound");
        transmute::CpuState state{};
        const auto before = name == std::string_view("GetTickCount") ?
            static_cast<std::uint64_t>(GetTickCount()) : GetTickCount64();
        target(state);
        const auto after = name == std::string_view("GetTickCount") ?
            static_cast<std::uint64_t>(GetTickCount()) : GetTickCount64();
        if (name == std::string_view("GetTickCount")) {
            expect(state.x[0] <= UINT32_MAX &&
                       static_cast<DWORD>(state.x[0] - before) <=
                           static_cast<DWORD>(after - before),
                   "native tick32 wrapper changed width or wraparound ordering");
        } else {
            expect(state.x[0] >= before && state.x[0] <= after,
                   "native tick64 wrapper changed the native clock value");
        }
    }
    for (const auto name : {"GetLastError", "SetLastError", "CreateThread",
                            "QueryPerformanceCounter", "ReadFile", "#1", "getcurrentthreadid"})
        expect(!transmute::resolve_direct_win32_import({"kernel32.dll", name, 0}),
               "native scalar import bypassed guest memory, callback or named-export rules");
    expect(!transmute::resolve_direct_win32_import({"user32.dll", "GetCurrentThreadId", 0}) &&
               !transmute::resolve_direct_win32_import({"kernel32.dll.extra", "GetACP", 0}),
           "native import resolver ignored DLL identity");
    for (const auto name : {"GetKeyState", "GetAsyncKeyState", "MapVirtualKeyA", "MapVirtualKeyW", "GetSystemMetrics"}) {
        const transmute::Import imported{"user32.dll", name, 0};
        const auto target = transmute::resolve_direct_win32_import(imported);
        expect(target != nullptr, "scalar User32 import was not bound");
        transmute::CpuState expected{}, actual{};
        expected.x[0] = 0xffffffff7fffffffULL;
        expected.x[1] = 0xffffffff00000000ULL;
        expected.x[18] = 0;
        expected.nzcv = 0xa0000000U;
        actual = expected;
        static_cast<void>(bridge.dispatch(imported, expected, memory));
        target(actual);
        expect(native_region_same_state(expected, actual),
               "scalar User32 wrapper changed argument width, result or unrelated state");
    }
    for (const auto name : {"GetDC", "CreateWindowExW", "DispatchMessageW", "GetKeyboardState"})
        expect(!transmute::resolve_direct_win32_import({"user32.dll", name, 0}),
               "User32 native path bypassed handles, guest memory or callbacks");
    for (const auto name : {"wglMakeCurrent", "wglGetProcAddress", "glGetString", "glReadPixels"})
        expect(!transmute::resolve_direct_win32_import({"opengl32.dll", name, 0}),
               "OpenGL native path bypassed handles or guest memory");
}

struct NativeImportResolveProbe {
    inline static unsigned resolutions{};
    static transmute::NativeImportCall resolve(const transmute::Import& imported) noexcept {
        ++resolutions;
        return transmute::resolve_direct_win32_import(imported);
    }
};

void test_native_import_binding_lifetime() {
    auto image = transmute::PeImage::parse(native_import_fixture("GetCurrentProcessId"));
    unsigned fallbacks = 0;
    transmute::ReferenceEngine engine(image,
        [&](const transmute::Import& imported, transmute::CpuState& state,
            transmute::GuestMemory& memory) {
            ++fallbacks;
            return transmute::dispatch_win32_import(imported, state, memory);
        });
    engine.set_initial_register(16, image.imports().front().entry);
    NativeImportResolveProbe::resolutions = 0;
    engine.set_native_import_resolver(NativeImportResolveProbe::resolve);
    for (unsigned i = 0; i != 5; ++i)
        expect(engine.invoke_callback(image.entry(), std::span<const std::uint64_t>{}) ==
                   GetCurrentProcessId(), "resolved guest import returned the wrong process ID");
    expect(fallbacks == 0 && NativeImportResolveProbe::resolutions == 1,
           "guest scalar import repeated resolution or used the general dispatcher");
    engine.set_native_import_resolver(nullptr);
    expect(engine.invoke_callback(image.entry(), std::span<const std::uint64_t>{}) ==
               GetCurrentProcessId() && fallbacks == 1,
           "disabling a resolved import did not restore the checked dispatcher");
    engine.set_native_import_resolver(NativeImportResolveProbe::resolve);
    auto dll_bytes = native_import_fixture("GetCurrentThreadId");
    put16(dll_bytes, 0x96, 0x2002);
    put64(dll_bytes, 0x98 + 24, 0x191000000ULL);
    auto dll = transmute::PeImage::parse(dll_bytes);
    dll.set_import_bridge_base(0x0000600000100000ULL);
    engine.stage_module(dll);
    const auto entry = dll.imports().front().entry;
    expect(engine.invoke_callback(entry, std::span<const std::uint64_t>{}) == GetCurrentThreadId(),
           "staged native import was not bound before publication");
    engine.commit_staged_modules();
    auto worker = engine.fork_thread(image.entry(), 0);
    std::exception_ptr failure;
    std::thread thread([&] {
        try {
            expect(worker->invoke_callback(entry, std::span<const std::uint64_t>{}) ==
                       GetCurrentThreadId(),
                   "native import binding captured the parent thread or was lost on fork");
        } catch (...) { failure = std::current_exception(); }
    });
    thread.join();
    if (failure) std::rethrow_exception(failure);
    expect(fallbacks == 1, "published or worker native import used the general dispatcher");
}

void benchmark_native_import_bindings() {
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine memory(image);
    transmute::Win32Bridge bridge;
    constexpr unsigned calls = 200'000;
    for (const auto& imported : {transmute::Import{"kernel32.dll", "GetCurrentThreadId", 0},
                                 transmute::Import{"user32.dll", "GetSystemMetrics", 0}}) {
        const auto target = transmute::resolve_direct_win32_import(imported);
        const auto expected = imported.function == "GetCurrentThreadId"
            ? GetCurrentThreadId() : static_cast<DWORD>(GetSystemMetrics(SM_CXSCREEN));
        const auto host = imported.function == "GetCurrentThreadId"
            ? +[](transmute::CpuState& state) { state.x[0] = GetCurrentThreadId(); }
            : +[](transmute::CpuState& state) { state.x[0] = static_cast<std::uint32_t>(GetSystemMetrics(SM_CXSCREEN)); };
        for (unsigned round = 0; round != 5; ++round) {
            for (unsigned order = 0; order != 3; ++order) {
                const unsigned mode = (round + order) % 3;
                transmute::CpuState state{};
                const auto start = std::chrono::steady_clock::now();
                for (unsigned i = 0; i != calls; ++i) {
                    state.x[0] = SM_CXSCREEN;
                    if (mode == 0) host(state);
                    else if (mode == 1) target(state);
                    else static_cast<void>(bridge.dispatch(imported, state, memory));
                }
                const auto ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                expect(state.x[0] == expected, "native import benchmark returned a wrong result");
                std::cout << imported.function << ',' << (mode == 0 ? "host" : mode == 1 ? "native" : "dispatcher") << ',' << calls << ',' << ms << '\n';
            }
        }
    }
}

#endif
