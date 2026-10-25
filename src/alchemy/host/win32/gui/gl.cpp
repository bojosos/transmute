// Win32 host backend, area "gl" (include/alchemy/host/gl.hpp): WGL contexts.
// Each context draws to a hidden window of its own (never shown, so cloaking
// has nothing to hide). The windows belong to one thread of this backend that
// only creates and destroys them, so a context outlives the guest thread that
// made it; the contexts themselves are made current on any thread. Pixel
// formats and contexts come from WGL_ARB_pixel_format and
// WGL_ARB_create_context(_profile), loaded once through a probe context.
#include "alchemy/host/gl.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <GL/gl.h>

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace alchemy::host::gl {

struct Context {
    HWND window{};
    HDC dc{};
    HGLRC rc{};
    Format format;
};

namespace {

// WGL_ARB_pixel_format, WGL_ARB_multisample, WGL_ARB_create_context(_profile).
constexpr int draw_to_window = 0x2001, acceleration = 0x2003, support_opengl = 0x2010, double_buffer = 0x2011,
              pixel_type = 0x2013, color_bits = 0x2014, alpha_bits = 0x201B, depth_bits = 0x2022, stencil_bits = 0x2023,
              full_acceleration = 0x2027, type_rgba = 0x202B, sample_buffers = 0x2041, samples = 0x2042;
constexpr int context_major = 0x2091, context_minor = 0x2092, context_flags = 0x2094, context_profile = 0x9126;
constexpr int profile_core = 0x1, profile_compatibility = 0x2, forward_compatible = 0x2;
// GL_VENDOR, GL_RENDERER, GL_VERSION, GL_MAJOR_VERSION, GL_MINOR_VERSION.
constexpr unsigned gl_vendor = 0x1F00, gl_renderer = 0x1F01, gl_version = 0x1F02, gl_major = 0x821B, gl_minor = 0x821C;

using ChooseFormatArb = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using FormatAttribsArb = BOOL(WINAPI*)(HDC, int, int, UINT, const int*, int*);
using CreateContextArb = HGLRC(WINAPI*)(HDC, HGLRC, const int*);

thread_local Context* current_context{};

// Core profile versions, newest first.
constexpr std::pair<int, int> core_versions[] = {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}, {3, 2}};

// A task posted to the window thread, and its completion.
struct Request {
    const std::function<void()>* task{};
    std::mutex mutex;
    std::condition_variable done;
    bool finished{};
};

// The thread that owns the hidden windows: it runs each request posted to it
// and pumps whatever messages the windows get.
class WindowThread {
public:
    WindowThread() {
        std::unique_lock lock(mutex_);
        thread_ = std::thread([this] { run(); });
        ready_.wait(lock, [this] { return id_ != 0; });
    }
    ~WindowThread() {
        PostThreadMessageW(id_, WM_QUIT, 0, 0);
        if (thread_.joinable()) thread_.join();
    }
    // Runs `task` on the window thread and waits for it.
    void run_on(const std::function<void()>& task) {
        Request request;
        request.task = &task;
        if (!PostThreadMessageW(id_, WM_APP, 0, reinterpret_cast<LPARAM>(&request))) { task(); return; }
        std::unique_lock lock(request.mutex);
        request.done.wait(lock, [&] { return request.finished; });
    }

private:
    void run() {
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);   // makes this thread's message queue
        {
            std::lock_guard lock(mutex_);
            id_ = GetCurrentThreadId();
        }
        ready_.notify_all();
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!message.hwnd && message.message == WM_APP) {
                auto* request = reinterpret_cast<Request*>(message.lParam);
                (*request->task)();
                { std::lock_guard lock(request->mutex); request->finished = true; }
                request->done.notify_all();
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    DWORD id_{};
    std::thread thread_;
};

WindowThread& window_thread() {
    static WindowThread thread;
    return thread;
}

const wchar_t* window_class() {
    static const wchar_t* name = [] {
        WNDCLASSEXW cls{sizeof(cls)};
        cls.style = CS_OWNDC;
        cls.lpfnWndProc = DefWindowProcW;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"AlchemyOpenGLDrawable";
        RegisterClassExW(&cls);
        return cls.lpszClassName;
    }();
    return name;
}

HWND make_window() {
    HWND window{};
    window_thread().run_on([&] {
        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, window_class(), L"", WS_POPUP, 0, 0, 16, 16, nullptr,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
    });
    return window;
}

void destroy_window(HWND window) {
    if (window) window_thread().run_on([window] { DestroyWindow(window); });
}

// The WGL extension entry points and the driver's strings, from a probe
// context on a basic pixel format (made once).
struct Probe {
    ChooseFormatArb choose{};
    FormatAttribsArb attributes{};
    CreateContextArb create{};
    Result<Renderer> renderer{Error::unsupported};
};

const Probe& probe() {
    static const Probe result = [] {
        Probe out;
        const auto window = make_window();
        if (!window) return out;
        const auto dc = GetDC(window);
        PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1};
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        const int chosen = ChoosePixelFormat(dc, &pfd);
        HGLRC rc{};
        if (chosen && SetPixelFormat(dc, chosen, &pfd) && (rc = wglCreateContext(dc)) != nullptr) {
            const auto previous_dc = wglGetCurrentDC();
            const auto previous_rc = wglGetCurrentContext();
            if (wglMakeCurrent(dc, rc)) {
                out.choose = reinterpret_cast<ChooseFormatArb>(wglGetProcAddress("wglChoosePixelFormatARB"));
                out.attributes = reinterpret_cast<FormatAttribsArb>(wglGetProcAddress("wglGetPixelFormatAttribivARB"));
                out.create = reinterpret_cast<CreateContextArb>(wglGetProcAddress("wglCreateContextAttribsARB"));
                const auto text = [](unsigned name) {
                    const auto* value = reinterpret_cast<const char*>(glGetString(name));
                    return std::string(value ? value : "");
                };
                Renderer renderer{text(gl_vendor), text(gl_renderer), text(gl_version)};
                // The highest core profile version the driver makes (drivers make
                // exactly the version asked for, so ask from the newest down).
                for (const auto [major, minor] : core_versions) {
                    if (!out.create) break;
                    const int attributes[] = {context_major, major, context_minor, minor, context_profile, profile_core, context_flags, forward_compatible, 0};
                    if (const auto core = out.create(dc, nullptr, attributes)) {
                        if (wglMakeCurrent(dc, core)) {
                            GLint major{}, minor{};
                            glGetIntegerv(gl_major, &major);
                            glGetIntegerv(gl_minor, &minor);
                            renderer.major = static_cast<std::uint32_t>(major);
                            renderer.minor = static_cast<std::uint32_t>(minor);
                        }
                        wglMakeCurrent(nullptr, nullptr);
                        wglDeleteContext(core);
                        break;
                    }
                }
                out.renderer = renderer;
            }
            wglMakeCurrent(previous_dc, previous_rc);
            wglDeleteContext(rc);
        }
        ReleaseDC(window, dc);
        destroy_window(window);
        return out;
    }();
    return result;
}

// The pixel format closest to `format` on `dc` (0: none).
int pick_format(HDC dc, const Format& format, const Probe& wgl) {
    if (wgl.choose) {
        std::vector<int> attributes{draw_to_window, TRUE, support_opengl, TRUE, acceleration, full_acceleration, pixel_type, type_rgba,
                                    double_buffer, format.double_buffer ? TRUE : FALSE, color_bits, static_cast<int>(std::max(format.color_bits, 24U)),
                                    alpha_bits, static_cast<int>(format.alpha_bits), depth_bits, static_cast<int>(format.depth_bits),
                                    stencil_bits, static_cast<int>(format.stencil_bits)};
        if (format.samples > 1) attributes.insert(attributes.end(), {sample_buffers, 1, samples, static_cast<int>(format.samples)});
        attributes.push_back(0);
        int chosen{};
        UINT count{};
        if (wgl.choose(dc, attributes.data(), nullptr, 1, &chosen, &count) && count) return chosen;
        if (format.samples > 1) {   // no multisampled format: drop the samples
            Format single = format;
            single.samples = 0;
            return pick_format(dc, single, wgl);
        }
    }
    PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1};
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | (format.double_buffer ? PFD_DOUBLEBUFFER : 0);
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = static_cast<BYTE>(std::max(format.color_bits, 24U));
    pfd.cAlphaBits = static_cast<BYTE>(format.alpha_bits);
    pfd.cDepthBits = static_cast<BYTE>(format.depth_bits);
    pfd.cStencilBits = static_cast<BYTE>(format.stencil_bits);
    return ChoosePixelFormat(dc, &pfd);
}

// The format `chosen` actually has.
Format describe(HDC dc, int chosen, bool core, const Probe& wgl) {
    Format out;
    out.core = core;
    PIXELFORMATDESCRIPTOR pfd{};
    if (DescribePixelFormat(dc, chosen, sizeof(pfd), &pfd)) {
        out.color_bits = pfd.cColorBits;
        out.alpha_bits = pfd.cAlphaBits;
        out.depth_bits = pfd.cDepthBits;
        out.stencil_bits = pfd.cStencilBits;
        out.double_buffer = (pfd.dwFlags & PFD_DOUBLEBUFFER) != 0;
    }
    if (wgl.attributes) {
        const int names[] = {sample_buffers, samples};
        int values[2]{};
        if (wgl.attributes(dc, chosen, 0, 2, names, values) && values[0]) out.samples = static_cast<std::uint32_t>(values[1]);
    }
    return out;
}

} // namespace

Result<Context*> create_context(const Format& format, Context* share) {
    const auto& wgl = probe();
    if (!wgl.renderer) return Error::unsupported;
    if (format.core && (!wgl.create || wgl.renderer->major < 3)) return Error::unsupported;
    if (share && share->format.core != format.core) return Error::invalid_argument;
    auto context = std::make_unique<Context>();
    context->window = make_window();
    if (!context->window) return win32::last_error();
    context->dc = GetDC(context->window);
    const int chosen = pick_format(context->dc, format, wgl);
    PIXELFORMATDESCRIPTOR pfd{};
    if (!chosen || !DescribePixelFormat(context->dc, chosen, sizeof(pfd), &pfd) || !SetPixelFormat(context->dc, chosen, &pfd)) {
        ReleaseDC(context->window, context->dc);
        destroy_window(context->window);
        return Error::unsupported;
    }
    context->format = describe(context->dc, chosen, format.core, wgl);
    if (wgl.create) {
        // A core profile context of the highest version the driver has (as macOS gives 4.1 for any
        // core profile request).
        const int core[] = {context_major, static_cast<int>(wgl.renderer->major), context_minor, static_cast<int>(wgl.renderer->minor),
                            context_profile, profile_core, context_flags, forward_compatible, 0};
        const int compatibility[] = {context_major, 2, context_minor, 1, context_profile, profile_compatibility, 0};
        context->rc = wgl.create(context->dc, share ? share->rc : nullptr, format.core ? core : compatibility);
    } else {
        context->rc = wglCreateContext(context->dc);
        if (context->rc && share && !wglShareLists(share->rc, context->rc)) {
            wglDeleteContext(context->rc);
            context->rc = nullptr;
        }
    }
    if (!context->rc) {
        const auto error = win32::last_error();
        ReleaseDC(context->window, context->dc);
        destroy_window(context->window);
        return error == Error::none ? Error::unsupported : error;
    }
    return context.release();
}

void destroy_context(Context* context) noexcept {
    if (!context) return;
    if (current_context == context) static_cast<void>(make_current(nullptr));
    wglDeleteContext(context->rc);
    ReleaseDC(context->window, context->dc);
    destroy_window(context->window);
    delete context;
}

Status make_current(Context* context) {
    if (!(context ? wglMakeCurrent(context->dc, context->rc) : wglMakeCurrent(nullptr, nullptr))) return win32::last_error();
    current_context = context;
    return {};
}

Context* current() noexcept { return current_context; }

Format format(const Context* context) noexcept { return context ? context->format : Format{}; }

void swap_buffers(Context* context) noexcept {
    if (context && context->format.double_buffer) SwapBuffers(context->dc);
}

void* proc(const char* name) noexcept {
    if (current_context) {
        const auto found = reinterpret_cast<std::intptr_t>(wglGetProcAddress(name));
        // Some drivers answer small values or -1 instead of NULL for unknown names.
        if (found > 3 || found < -1) return reinterpret_cast<void*>(found);
    }
    static const HMODULE opengl32 = GetModuleHandleW(L"opengl32.dll");
    return opengl32 ? reinterpret_cast<void*>(GetProcAddress(opengl32, name)) : nullptr;
}

Result<Renderer> renderer() { return probe().renderer; }

} // namespace alchemy::host::gl
