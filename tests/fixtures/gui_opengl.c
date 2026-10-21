// Standalone Windows ARM64 fixture. Link only the SDK import libraries, without
// a CRT, so the runtime must bridge every GUI and graphics call in this file.
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned long long u64;
typedef long long i64;
typedef void* handle;
#define API __declspec(dllimport)
typedef i64 (*window_proc)(handle, u32, u64, i64);
struct window_class {
    u32 style;
    window_proc procedure;
    int class_extra, window_extra;
    handle instance, icon, cursor, background;
    const u16 *menu, *name;
};
struct point { int x, y; };
struct message { handle window; u32 code; u64 wparam; i64 lparam; u32 time; struct point point; u32 reserved; };
struct pixel_format {
    u16 size, version;
    u32 flags;
    unsigned char type, color, red, red_shift, green, green_shift, blue, blue_shift;
    unsigned char alpha, alpha_shift, accum, accum_red, accum_green, accum_blue, accum_alpha;
    unsigned char depth, stencil, aux, layer, reserved;
    u32 layer_mask, visible_mask, damage_mask;
};
API handle GetModuleHandleW(const u16*);
API const u16* GetCommandLineW(void);
API handle GetStdHandle(u32);
API int WriteFile(handle, const void*, u32, u32*, void*);
API u16 RegisterClassW(const struct window_class*);
API int UnregisterClassW(const u16*, handle);
API handle CreateWindowExW(u32, const u16*, const u16*, u32, int, int, int, int, handle, handle, handle, void*);
API int DestroyWindow(handle);
API i64 DefWindowProcW(handle, u32, u64, i64);
API handle GetDC(handle);
API int ReleaseDC(handle, handle);
API int PostMessageW(handle, u32, u64, i64);
API int PeekMessageW(struct message*, handle, u32, u32, u32);
API i64 DispatchMessageW(const struct message*);
API int ChoosePixelFormat(handle, const struct pixel_format*);
API int SetPixelFormat(handle, int, const struct pixel_format*);
API int SwapBuffers(handle);
API handle wglCreateContext(handle);
API int wglMakeCurrent(handle, handle);
API int wglDeleteContext(handle);
API const unsigned char* glGetString(u32);
API void glClearColor(float, float, float, float);
API void glClear(u32);
API void glReadPixels(int, int, int, int, u32, u32, void*);
API u32 glGetError(void);

static int created, posted, destroyed, keyboard, character, mouse;
static i64 procedure(handle window, u32 code, u64 wparam, i64 lparam) {
    if (code == 0x81) return 1; // WM_NCCREATE
    if (code == 1) { created = 1; return 0; }
    if (code == 2) { destroyed = 1; return 0; }
    if (code == 0x100) { keyboard = wparam == 'A' && lparam == 0x1e0001; return 0; }
    if (code == 0x102) { character = wparam == 0x3a9 && lparam == 1; return 0; }
    if (code == 0x200) { mouse = wparam == 4 && lparam == ((23 << 16) | 17); return 0; }
    if (code == 0x8001) { posted = wparam == 0x1234 && lparam == 0x5678; return 0; }
    return DefWindowProcW(window, code, wparam, lparam);
}

int guest_entry(void) {
    static const u16 name[] = {'T','r','a','n','s','m','u','t','e','A','r','m','G','L',0};
    struct window_class klass = {0};
    klass.style = 0x20; // CS_OWNDC
    klass.procedure = procedure;
    klass.instance = GetModuleHandleW(0);
    klass.name = name;
    if (!RegisterClassW(&klass)) return 1;
    handle window = CreateWindowExW(0, name, name, 0xcf0000, 10, 10, 320, 240, 0, 0, klass.instance, 0);
    if (!window || !created) return 2;
    handle dc = GetDC(window);
    if (!dc) return 3;
    struct pixel_format format = {0};
    format.size = sizeof(format);
    format.version = 1;
    format.flags = 0x25; // PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER
    format.color = 24;
    int selected = ChoosePixelFormat(dc, &format);
    if (!selected || !SetPixelFormat(dc, selected, &format)) return 4;
    handle context = wglCreateContext(dc);
    if (!context || !wglMakeCurrent(dc, context)) return 5;
    const unsigned char* version = glGetString(0x1f02);
    if (!version || !*version) return 6;
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(0x4000);
    unsigned char pixel[4] = {0};
    glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, pixel);
    if (glGetError() || pixel[0] < 63 || pixel[0] > 64 || pixel[1] < 127 || pixel[1] > 128 || pixel[2] < 191 || pixel[2] > 192 || pixel[3] != 255) return 7;
    if (!SwapBuffers(dc)) return 18;
    if (!PostMessageW(window, 0x8001, 0x1234, 0x5678)) return 8;
    if (!PostMessageW(window, 0x100, 'A', 0x1e0001) ||
        !PostMessageW(window, 0x102, 0x3a9, 1) ||
        !PostMessageW(window, 0x200, 4, (23 << 16) | 17)) return 16;
    struct message message;
    while (PeekMessageW(&message, 0, 0, 0, 1)) DispatchMessageW(&message);
    if (!posted) return 9;
    if (!keyboard || !character || !mouse) return 17;
    static const u16 builtin_class[] = {'S','T','A','T','I','C',0};
    handle secondary_window = CreateWindowExW(0, builtin_class, name, 0xcf0000, 0, 0, 64, 64, 0, 0, klass.instance, 0);
    if (!secondary_window) return 12;
    handle secondary_dc = GetDC(secondary_window);
    int secondary_format = ChoosePixelFormat(secondary_dc, &format);
    if (!secondary_format || !SetPixelFormat(secondary_dc, secondary_format, &format)) return 13;
    handle secondary_context = wglCreateContext(secondary_dc);
    if (!secondary_context || !wglMakeCurrent(secondary_dc, secondary_context)) return 14;
    if (!wglMakeCurrent(dc, context) || !wglDeleteContext(secondary_context) ||
        !ReleaseDC(secondary_window, secondary_dc) || !DestroyWindow(secondary_window)) return 15;
    // Exercise runtime teardown after the engine has released guest memory.
    // A stale WndProc callback used to terminate the host during this cleanup.
    const u16* command = GetCommandLineW();
    for (const u16* p = command; *p; ++p)
        if (p[0] == '-' && p[1] == '-' && p[2] == 'a' && p[3] == 'b' && p[4] == 'a' && p[5] == 'n' && p[6] == 'd' && p[7] == 'o' && p[8] == 'n') return 0;
    if (!wglMakeCurrent(0, 0) || !wglDeleteContext(context) || !ReleaseDC(window, dc)) return 10;
    if (!DestroyWindow(window) || !destroyed || !UnregisterClassW(name, klass.instance)) return 11;
    static const char success[] = "ARM64 Win32 callbacks, message loop and native OpenGL readback passed\r\n";
    u32 written = 0;
    WriteFile(GetStdHandle((u32)-11), success, sizeof(success) - 1, &written, 0);
    return 0;
}
