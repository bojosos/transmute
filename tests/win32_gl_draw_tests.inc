#ifdef _WIN32
void test_guest_opengl_draw_pipeline() {
    struct NativeWindow {
        HWND window{::CreateWindowExW(0, L"STATIC", L"OpenGL draw test", 0,
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
    expect(host.dc != nullptr, "draw test could not acquire a native window DC");
    PIXELFORMATDESCRIPTOR descriptor{};
    descriptor.nSize = sizeof(descriptor);
    descriptor.nVersion = 1;
    descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    descriptor.cColorBits = 24;
    const auto format = ::ChoosePixelFormat(host.dc, &descriptor);
    expect(format != 0 && ::SetPixelFormat(host.dc, format, &descriptor), "draw test pixel format failed");
    host.context = ::wglCreateContext(host.dc);
    expect(host.context != nullptr && ::wglMakeCurrent(host.dc, host.context), "draw test context failed");
    // Without a GPU driver Windows offers only its OpenGL 1.1 software renderer
    // (no WGL_ARB_create_context, no core profile), so there is nothing to draw with.
    const auto* renderer = reinterpret_cast<const char*>(::glGetString(GL_RENDERER));
    const std::string on_renderer = std::string(" (renderer: ") + (renderer != nullptr ? renderer : "unknown") + ")";
    const auto create_context = reinterpret_cast<HGLRC(WINAPI*)(HDC, HGLRC, const int*)>(::wglGetProcAddress("wglCreateContextAttribsARB"));
    if (create_context == nullptr)
        skip_test("no WGL_ARB_create_context, which Blender's modern WGL context path needs" + on_renderer);
    const int attributes[]{0x2091, 4, 0x2092, 3, 0x9126, 1, 0};
    const auto modern = create_context(host.dc, nullptr, attributes);
    if (modern == nullptr) skip_test("the driver offers no OpenGL 4.3 core context" + on_renderer);
    ::wglMakeCurrent(nullptr, nullptr);
    ::wglDeleteContext(host.context);
    host.context = modern;
    expect(::wglMakeCurrent(host.dc, host.context), "draw test could not activate a core context");

    auto image = transmute::PeImage::parse(executable({0xd65f03c0U}));
    transmute::ReferenceEngine engine(image);
    transmute::Win32Bridge bridge;
    auto& memory = static_cast<transmute::GuestMemory&>(engine);
    const auto top = engine.state().sp;
    const auto lookup = [&](const char* name) {
        memory.write_bytes(top - 512, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(name), std::strlen(name) + 1));
        transmute::CpuState cpu{};
        cpu.x[0] = top - 512;
        static_cast<void>(bridge.dispatch({"opengl32.dll", "wglGetProcAddress", 0}, cpu, memory));
        if (cpu.x[0] == 0) {
            cpu.x[0] = 0x0000600100020000ULL;
            cpu.x[1] = top - 512;
            static_cast<void>(bridge.dispatch({"opengl32.dll", "transmuteGetProcAddress", 0}, cpu, memory));
        }
        expect(cpu.x[0] != 0, std::string("missing draw adapter: ") + name);
        return cpu.x[0];
    };
    const auto invoke = [&](const char* name, std::initializer_list<std::uint64_t> args) {
        const auto result = engine.invoke_callback(lookup(name), std::span<const std::uint64_t>(args.begin(), args.size()));
        const auto error = ::glGetError();
        expect(error == GL_NO_ERROR, std::string(name) + " produced GL error=" + std::to_string(error));
        return result;
    };
    const auto shader = [&](GLenum type, const char* source) {
        const auto object = invoke("glCreateShader", {type});
        memory.write_bytes(top - 3000, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(source), std::strlen(source) + 1));
        memory.write64(top - 1800, top - 3000);
        static_cast<void>(invoke("glShaderSource", {object, 1, top - 1800, 0}));
        static_cast<void>(invoke("glCompileShader", {object}));
        static_cast<void>(invoke("glGetShaderiv", {object, 0x8b81, top - 1800}));
        expect(memory.read32(top - 1800) == 1, "draw test GLSL did not compile through guest source storage");
        return object;
    };
    const auto vertex = shader(0x8b31, "#version 330\nlayout(location=0) in vec2 pos;void main(){gl_Position=vec4(pos,0,1);}");
    const auto fragment = shader(0x8b30, "#version 330\nout vec4 color;void main(){color=vec4(1,0,0,1);}");
    const auto program = invoke("glCreateProgram", {});
    static_cast<void>(invoke("glAttachShader", {program, vertex}));
    static_cast<void>(invoke("glAttachShader", {program, fragment}));
    static_cast<void>(invoke("glLinkProgram", {program}));
    static_cast<void>(invoke("glGetProgramiv", {program, 0x8b82, top - 1800}));
    expect(memory.read32(top - 1800) == 1, "draw test program did not link");
    static_cast<void>(invoke("glUseProgram", {program}));

    // Hidden window drawables may have no allocated back buffer. Render to an
    // owned attachment so pixel ownership cannot mask a forwarding failure.
    static_cast<void>(invoke("glGenTextures", {1, top - 1800}));
    const auto texture = memory.read32(top - 1800);
    static_cast<void>(invoke("glBindTexture", {GL_TEXTURE_2D, texture}));
    static_cast<void>(invoke("glTexStorage2D", {GL_TEXTURE_2D, 1, 0x8058, 64, 64}));
    static_cast<void>(invoke("glGenFramebuffers", {1, top - 1800}));
    const auto framebuffer = memory.read32(top - 1800);
    static_cast<void>(invoke("glBindFramebuffer", {0x8d40, framebuffer}));
    static_cast<void>(invoke("glFramebufferTexture2D", {0x8d40, 0x8ce0, GL_TEXTURE_2D, texture, 0}));
    expect(invoke("glCheckFramebufferStatus", {0x8d40}) == 0x8cd5, "draw test framebuffer was incomplete");

    static_cast<void>(invoke("glGenVertexArrays", {1, top - 1800}));
    const auto vao = memory.read32(top - 1800);
    static_cast<void>(invoke("glBindVertexArray", {vao}));
    static_cast<void>(invoke("glGenBuffers", {3, top - 1800}));
    const auto vertex_buffer = memory.read32(top - 1800);
    const auto index_buffer = memory.read32(top - 1796);
    const auto command_buffer = memory.read32(top - 1792);
    constexpr std::array<GLfloat, 6> positions{-1, -1, 3, -1, -1, 3};
    memory.write_bytes(top - 3000, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(positions.data()), sizeof(positions)));
    static_cast<void>(invoke("glBindBuffer", {0x8892, vertex_buffer}));
    static_cast<void>(invoke("glBufferData", {0x8892, sizeof(positions), top - 3000, 0x88e4}));
    static_cast<void>(invoke("glVertexAttribFormat", {0, 2, GL_FLOAT, GL_FALSE, 0}));
    static_cast<void>(invoke("glVertexAttribBinding", {0, 2}));
    static_cast<void>(invoke("glBindVertexBuffer", {2, vertex_buffer, 0, 2 * sizeof(GLfloat)}));
    static_cast<void>(invoke("glEnableVertexAttribArray", {0}));
    constexpr std::array<GLfloat, 4> viewport{0, 0, 64, 64};
    memory.write_bytes(top - 3000, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(viewport.data()), sizeof(viewport)));
    static_cast<void>(invoke("glViewportArrayv", {0, 1, top - 3000}));
    const auto expect_red = [&] {
        static_cast<void>(invoke("glReadPixels", {32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, top - 2000}));
        const auto error = ::glGetError();
        expect(memory.read32(top - 2000) == 0xff0000ffU,
               "guest draw pixel=" + std::to_string(memory.read32(top - 2000)) + " GL error=" + std::to_string(error));
        expect(error == GL_NO_ERROR, "guest draw pipeline produced a native OpenGL error");
    };
    ::glClearColor(0, 1, 0, 1);
    ::glClear(GL_COLOR_BUFFER_BIT);
    static_cast<void>(invoke("glDrawArraysInstancedBaseInstance", {GL_TRIANGLES, 0, 3, 1, 7}));
    expect_red();

    constexpr std::array<GLuint, 3> indices{0, 1, 2};
    memory.write_bytes(top - 3000, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(indices.data()), sizeof(indices)));
    static_cast<void>(invoke("glBindBuffer", {0x8893, index_buffer}));
    static_cast<void>(invoke("glBufferData", {0x8893, sizeof(indices), top - 3000, 0x88e4}));
    ::glClear(GL_COLOR_BUFFER_BIT);
    static_cast<void>(invoke("glDrawElementsInstancedBaseVertexBaseInstance", {GL_TRIANGLES, 3, GL_UNSIGNED_INT, 0, 1, 0, 7}));
    expect_red();

    constexpr std::array<GLuint, 4> command{3, 1, 0, 7};
    memory.write_bytes(top - 3000, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(command.data()), sizeof(command)));
    static_cast<void>(invoke("glBindBuffer", {0x8f3f, command_buffer}));
    static_cast<void>(invoke("glBufferData", {0x8f3f, sizeof(command), top - 3000, 0x88e4}));
    GLint indirect_binding = 0, indirect_size = 0, indirect_mapped = 0;
    ::glGetIntegerv(0x8f43, &indirect_binding); // GL_DRAW_INDIRECT_BUFFER_BINDING
    const auto buffer_parameter = reinterpret_cast<void(APIENTRY*)(GLenum, GLenum, GLint*)>(::wglGetProcAddress("glGetBufferParameteriv"));
    expect(buffer_parameter != nullptr, "native indirect buffer query was unavailable");
    buffer_parameter(0x8f3f, 0x8764, &indirect_size); // GL_BUFFER_SIZE
    buffer_parameter(0x8f3f, 0x88bc, &indirect_mapped); // GL_BUFFER_MAPPED
    expect(::glGetError() == GL_NO_ERROR, "native indirect buffer inspection failed");
    expect(static_cast<GLuint>(indirect_binding) == command_buffer &&
           indirect_size == static_cast<GLint>(sizeof(command)) && indirect_mapped == GL_FALSE,
           "native indirect binding, command extent or mapping state was incorrect");
    std::array<GLuint, 4> native_command{};
    const auto read_buffer = reinterpret_cast<void(APIENTRY*)(GLenum, std::intptr_t, std::intptr_t, void*)>(::wglGetProcAddress("glGetBufferSubData"));
    expect(read_buffer != nullptr, "native indirect command readback was unavailable");
    read_buffer(0x8f3f, 0, sizeof(native_command), native_command.data());
    expect(::glGetError() == GL_NO_ERROR, "native indirect command readback failed");
    expect(native_command == command, "native indirect command bytes changed during guest upload");
    ::glClear(GL_COLOR_BUFFER_BIT);
    expect(::glGetError() == GL_NO_ERROR, "native indirect baseline clear failed");
    const auto native_indirect = reinterpret_cast<void(APIENTRY*)(GLenum, const void*)>(::wglGetProcAddress("glDrawArraysIndirect"));
    expect(native_indirect != nullptr, "native indirect draw target was unavailable");
    native_indirect(GL_TRIANGLES, nullptr);
    const auto native_indirect_error = ::glGetError();
    expect(native_indirect_error == GL_NO_ERROR,
           "native indirect draw baseline GL error=" + std::to_string(native_indirect_error));
    expect_red();
    ::glClear(GL_COLOR_BUFFER_BIT);
    static_cast<void>(invoke("glDrawArraysIndirect", {GL_TRIANGLES, 0}));
    expect_red();
    static_cast<void>(invoke("glBindBuffer", {0x8f3f, 0}));
    memory.write32(top - 1800, vertex_buffer);
    memory.write32(top - 1796, index_buffer);
    memory.write32(top - 1792, command_buffer);
    static_cast<void>(invoke("glDeleteBuffers", {3, top - 1800}));
    memory.write32(top - 1800, vao);
    static_cast<void>(invoke("glDeleteVertexArrays", {1, top - 1800}));
    static_cast<void>(invoke("glDeleteProgram", {program}));
    static_cast<void>(invoke("glDeleteShader", {vertex}));
    static_cast<void>(invoke("glDeleteShader", {fragment}));
    static_cast<void>(invoke("glBindFramebuffer", {0x8d40, 0}));
    memory.write32(top - 1800, framebuffer);
    static_cast<void>(invoke("glDeleteFramebuffers", {1, top - 1800}));
    memory.write32(top - 1800, texture);
    static_cast<void>(invoke("glDeleteTextures", {1, top - 1800}));
}
#endif
