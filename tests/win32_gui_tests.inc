#ifdef _WIN32
void test_guest_opengl_builtin_window() {
    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    image.set_source("C:\\transmute-tests\\opengl-builtin-window.exe");
    transmute::ReferenceEngine engine(image);
    transmute::Win32Bridge bridge;
    bridge.register_guest_module(image);
    const auto top = engine.state().sp;
    constexpr char class_name[] = "STATIC";
    constexpr char title[] = "BlenderGLEW";
    engine.write_bytes(top - 512, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(class_name), sizeof(class_name)));
    engine.write_bytes(top - 480, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(title), sizeof(title)));
    transmute::CpuState cpu{};
    cpu.sp = top - 128;
    cpu.x[1] = top - 512;
    cpu.x[2] = top - 480;
    cpu.x[3] = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    cpu.x[6] = cpu.x[7] = 64;
    engine.write64(cpu.sp, 0);
    engine.write64(cpu.sp + 8, 0);
    engine.write64(cpu.sp + 16, image.base());
    engine.write64(cpu.sp + 24, 0);
    const auto call = [&](const char* dll, const char* name) {
        static_cast<void>(bridge.dispatch({dll, name, 0}, cpu, engine));
    };
    call("user32.dll", "CreateWindowExA");
    const auto window = cpu.x[0];
    expect(window != 0, "Blender's built-in STATIC window required guest class registration");
    cpu.x[0] = window;
    call("user32.dll", "GetDC");
    const auto dc = cpu.x[0];
    expect(dc != 0, "built-in window did not retain a guest device-context identity");
    PIXELFORMATDESCRIPTOR descriptor{};
    descriptor.nSize = sizeof(descriptor);
    descriptor.nVersion = 1;
    descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    descriptor.cColorBits = 24;
    engine.write_bytes(top - 768, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&descriptor), sizeof(descriptor)));
    cpu.x[0] = dc;
    cpu.x[1] = top - 768;
    call("gdi32.dll", "ChoosePixelFormat");
    const auto format = cpu.x[0];
    expect(format != 0, "built-in window selected a screen DC instead of an OpenGL window DC");
    cpu.x[0] = dc;
    cpu.x[1] = format;
    cpu.x[2] = top - 768;
    call("gdi32.dll", "SetPixelFormat");
    expect(cpu.x[0] != 0, "built-in window pixel format failed");
    cpu.x[0] = dc;
    call("opengl32.dll", "wglCreateContext");
    const auto context = cpu.x[0];
    expect(context != 0, "secondary graphics context could not use the built-in window");
    cpu.x[0] = dc;
    cpu.x[1] = context;
    call("opengl32.dll", "wglMakeCurrent");
    expect(cpu.x[0] != 0, "secondary graphics context could not be activated");
    // Creating a graphics window may queue DWM notifications before our posts.
    for (;;) {
        cpu.x[0] = top - 1000;
        cpu.x[1] = cpu.x[2] = cpu.x[3] = 0;
        cpu.x[4] = PM_REMOVE;
        call("user32.dll", "PeekMessageW");
        if (cpu.x[0] == 0) break;
        cpu.x[0] = top - 1000;
        call("user32.dll", "DispatchMessageW");
    }
    {
        struct NativeHelper {
            HWND window{};
            WNDPROC previous{};
            int messages{}, timers{};
            NativeHelper() {
                window = ::CreateWindowExW(0, L"STATIC", L"Native helper", 0, 0, 0, 16, 16, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
                expect(window != nullptr, "native message helper creation failed");
                ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
                previous = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&receive)));
            }
            ~NativeHelper() { ::DestroyWindow(window); }
            static LRESULT CALLBACK receive(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
                auto* self = reinterpret_cast<NativeHelper*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
                if (message == WM_APP + 9) { ++self->messages; return 0; }
                return ::CallWindowProcW(self->previous, window, message, wparam, lparam);
            }
            static NativeHelper*& active_timer() {
                static thread_local NativeHelper* current = nullptr;
                return current;
            }
            static void CALLBACK timer(HWND, UINT, UINT_PTR, DWORD) {
                ++active_timer()->timers;
            }
        } helper;
        for (const auto flags : {PM_NOREMOVE, PM_REMOVE}) {
            NativeHelper::active_timer() = &helper;
            const auto timer = ::SetTimer(nullptr, 0, 60000, &NativeHelper::timer);
            expect(timer != 0, "native thread timer creation failed");
            ::PostThreadMessageW(::GetCurrentThreadId(), WM_TIMER, timer, reinterpret_cast<LPARAM>(&NativeHelper::timer));
            ::PostMessageW(helper.window, WM_APP + 9, 0, reinterpret_cast<LPARAM>(&helper));
            cpu.x[0] = window;
            cpu.x[1] = WM_APP + 9;
            cpu.x[2] = 42;
            cpu.x[3] = 43;
            call("user32.dll", "PostMessageW");
            cpu.x[0] = top - 1000;
            cpu.x[1] = cpu.x[2] = cpu.x[3] = 0;
            cpu.x[4] = flags;
            call("user32.dll", "PeekMessageW");
            ::KillTimer(nullptr, timer);
            expect(cpu.x[0] == 1 && engine.read32(top - 992) == WM_APP + 9 && engine.read64(top - 976) == 43,
                   "native helper queue returned result=" + std::to_string(cpu.x[0]) +
                   " message=" + std::to_string(engine.read32(top - 992)) +
                   " lparam=" + std::to_string(engine.read64(top - 976)));
            if (flags == PM_NOREMOVE) {
                cpu.x[0] = top - 1000;
                cpu.x[4] = PM_REMOVE;
                call("user32.dll", "PeekMessageW");
            }
        }
        expect(helper.messages == 2 && helper.timers == 2,
               "native helper dispatch counts: messages=" + std::to_string(helper.messages) +
               " timers=" + std::to_string(helper.timers));
    }
    cpu.x[0] = cpu.x[1] = 0;
    call("opengl32.dll", "wglMakeCurrent");
    cpu.x[0] = context;
    call("opengl32.dll", "wglDeleteContext");
    cpu.x[0] = window;
    cpu.x[1] = dc;
    call("user32.dll", "ReleaseDC");
    cpu.x[0] = window;
    call("user32.dll", "DestroyWindow");
    expect(cpu.x[0] != 0, "built-in window cleanup failed");
    cpu.x[0] = window;
    call("user32.dll", "IsWindow");
    expect(cpu.x[0] == 0, "destroyed built-in window retained its guest handle");
}

void test_guest_opengl_string_simd_tail() {
    struct NativeWindow {
        HWND window{::CreateWindowExW(0, L"STATIC", L"OpenGL string test", WS_OVERLAPPED,
                                      0, 0, 64, 64, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr)};
        HDC dc{window == nullptr ? nullptr : ::GetDC(window)};
        HGLRC context{};
        ~NativeWindow() {
            ::wglMakeCurrent(nullptr, nullptr);
            if (context != nullptr) ::wglDeleteContext(context);
            if (dc != nullptr) ::ReleaseDC(window, dc);
            if (window != nullptr) ::DestroyWindow(window);
        }
    } host;
    expect(host.dc != nullptr, "native test window could not acquire its DC");
    PIXELFORMATDESCRIPTOR descriptor{};
    descriptor.nSize = sizeof(descriptor);
    descriptor.nVersion = 1;
    descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    descriptor.cColorBits = 24;
    const auto format = ::ChoosePixelFormat(host.dc, &descriptor);
    expect(format != 0 && ::SetPixelFormat(host.dc, format, &descriptor), "native test pixel format failed");
    host.context = ::wglCreateContext(host.dc);
    expect(host.context != nullptr && ::wglMakeCurrent(host.dc, host.context), "native test context failed");
    auto image = transmute::PeImage::parse(executable({0x3dc00000U, 0xd65f03c0U})); // ldr q0,[x0]; ret
    transmute::ReferenceEngine engine(image);
    transmute::Win32Bridge bridge;
    transmute::CpuState state{};
    state.x[0] = GL_VENDOR;
    static_cast<void>(bridge.dispatch({"opengl32.dll", "glGetString", 0}, state, engine));
    const auto pointer = state.x[0];
    expect(pointer != 0, "native driver did not supply a vendor string");
    const auto length = engine.string_length(pointer, 1, 4096);
    expect(length > 0 && length < 4096, "invalid guest OpenGL vendor string");
    const std::array<std::uint64_t, 1> arguments{pointer + length - 1};
    // VCRUNTIME's ARM64 strlen reads vectors through the terminator. It must
    // see the same readable page padding as a native Windows string.
    static_cast<void>(engine.invoke_callback(image.entry(), arguments));
    engine.set_jit_enabled(true);
    static_cast<void>(engine.invoke_callback(image.entry(), arguments));

    if (const auto native = ::wglGetProcAddress("glGetUnsignedBytevEXT")) {
        std::array<GLubyte, 8> expected{};
        reinterpret_cast<void(APIENTRY*)(GLenum, GLubyte*)>(native)(0x9599, expected.data()); // GL_DEVICE_LUID_EXT
        constexpr char name[] = "glGetUnsignedBytevEXT";
        const auto symbol = engine.state().sp - 256;
        engine.write_bytes(symbol, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(name), sizeof(name)));
        state.x[0] = symbol;
        static_cast<void>(bridge.dispatch({"opengl32.dll", "wglGetProcAddress", 0}, state, engine));
        constexpr std::uint64_t output = 0x60000000;
        engine.map_memory(output, expected.size());
        const std::array<std::uint64_t, 2> query{0x9599, output};
        static_cast<void>(engine.invoke_callback(state.x[0], query));
        std::array<std::uint8_t, 8> actual{};
        engine.read_bytes(output, actual);
        expect(actual == expected, "Windows GPU LUID did not fit its eight-byte guest output");
    }

    // GL loaders enumerate exports they never call. An unimplemented adapter
    // must report its error when invoked, rather than aborting enumeration.
    constexpr char unsupported[] = "glGetPolygonStipple";
    const auto symbol = engine.state().sp - 256;
    engine.write_bytes(symbol, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(unsupported), sizeof(unsupported)));
    state.x[1] = symbol;
    static_cast<void>(bridge.dispatch({"opengl32.dll", "transmuteGetProcAddress", 0}, state, engine));
    expect(state.x[0] != 0, "native OpenGL export enumeration lost an existing function");
    bool rejected = false;
    try { static_cast<void>(engine.invoke_callback(state.x[0], std::uint64_t{0})); }
    catch (const std::runtime_error& error) { rejected = std::string(error.what()).find(unsupported) != std::string::npos; }
    expect(rejected, "unimplemented OpenGL invocation did not identify the missing adapter");
}
#endif
