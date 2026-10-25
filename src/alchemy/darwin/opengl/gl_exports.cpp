#include "alchemy/darwin/opengl/opengl.hpp"

#include "alchemy/host/gl.hpp"
#include "alchemy/registration_audit.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

// The gl* entry points of OpenGL.framework (gl_functions.inc: OpenGL 1.0-4.1, compatibility and
// core, and the EXT/ARB/APPLE names macOS keeps), each a call into the host driver's function of
// the same name through the current context (host::gl).
//
// - Arguments: each entry's signature gives its parameter types. They are read with the Apple
//   arm64 rules (integer and floating-point registers counted apart; stack arguments packed at
//   their natural size and alignment) and passed with the host's: on x64 every argument has a
//   positional slot, an integer or an XMM register for the first four and 8 bytes of stack
//   after, so one host call shape per (kinds of the first four slots, kind of the result)
//   covers every entry point.
// - Pointers: guest addresses are host addresses in alchemy-fx (FxEngine), so client arrays,
//   pixel data, strings, mapped buffers (glMapBuffer's result) and sync objects pass unchanged.
//   glGetString/glGetStringi copy the driver's strings into guest memory.
// - The driver's entry points are looked up once, through the first context current when an
//   entry point is first called; an alias (EXT_framebuffer_object and the other promoted
//   extensions, APPLE_vertex_array_object, APPLE_flush_render) calls the core entry point when
//   the driver lacks the extension's name.
// - With no current context a call does nothing and answers 0, and glGetError answers
//   GL_INVALID_OPERATION (macOS has no defined answer; programs that probe before making a
//   context see an error instead of a crash). An entry point the host driver lacks stops with
//   a message naming it.
// - glTextureRangeAPPLE is a storage hint and is answered here.
// - Framebuffer 0 of a context drawing to a view is a framebuffer object (set_default_framebuffer):
//   glBindFramebuffer(EXT) binds it for 0, glGetIntegerv reports it as 0 and its color
//   attachment as GL_BACK, and glDrawBuffer(s)/glReadBuffer map the window buffers (GL_BACK,
//   GL_FRONT, ...) to its color attachment while it is bound.
namespace alchemy::opengl {
namespace {

using transmute::CpuState;
using transmute::GuestMemory;
using Slot = std::uint64_t;

constexpr std::uint32_t gl_invalid_operation = 0x0502;
constexpr std::uint32_t gl_framebuffer_binding = 0x8CA6, gl_read_framebuffer_binding = 0x8CAA, gl_draw_buffer = 0x0C01,
    gl_read_buffer = 0x0C02, gl_draw_buffer0 = 0x8825, gl_color_attachment0 = 0x8CE0, gl_back = 0x0405;
constexpr std::size_t maximum_arguments = 16;

struct Entry {
    const char* name;
    const char* signature;
    const char* alias;
};
constexpr Entry entries[] = {
#include "alchemy/darwin/opengl/gl_functions.inc"
};

// ---- host calls ----
// Slot kinds: 0 integer (any width, and pointers), 1 float, 2 double.
template <int K> struct SlotType { using type = Slot; };
template <> struct SlotType<1> { using type = float; };
template <> struct SlotType<2> { using type = double; };
template <int K>
typename SlotType<K>::type slot_value(Slot bits) {
    if constexpr (K == 1) return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
    else if constexpr (K == 2) return std::bit_cast<double>(bits);
    else return bits;
}

using Invoker = Slot (*)(void*, const Slot*);

template <int R, int K0, int K1, int K2, int K3>
Slot invoke(void* function, const Slot* a) {
    using Result = typename SlotType<R>::type;
    using F = Result (*)(typename SlotType<K0>::type, typename SlotType<K1>::type, typename SlotType<K2>::type, typename SlotType<K3>::type,
                         Slot, Slot, Slot, Slot, Slot, Slot, Slot, Slot, Slot, Slot, Slot, Slot);
    const auto result = reinterpret_cast<F>(function)(slot_value<K0>(a[0]), slot_value<K1>(a[1]), slot_value<K2>(a[2]), slot_value<K3>(a[3]),
                                                      a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
    if constexpr (R == 1) return std::bit_cast<std::uint32_t>(result);
    else if constexpr (R == 2) return std::bit_cast<std::uint64_t>(result);
    else return result;
}

template <std::size_t... I>
constexpr std::array<Invoker, sizeof...(I)> make_invokers(std::index_sequence<I...>) {
    return {&invoke<static_cast<int>(I / 81), static_cast<int>(I / 27 % 3), static_cast<int>(I / 9 % 3), static_cast<int>(I / 3 % 3),
                    static_cast<int>(I % 3)>...};
}
constexpr auto invokers = make_invokers(std::make_index_sequence<243>{});

int kind(char code) { return code == 'f' ? 1 : code == 'd' ? 2 : 0; }

// ---- signatures ----
struct Signature {
    char result{'v'};
    std::uint8_t count{};
    std::array<char, maximum_arguments> arguments{};
    Invoker invoker{};
};

Signature parse(const char* text) {
    Signature out;
    out.result = text[0];
    std::array<int, 4> kinds{};
    for (const char* p = text + 2; *p && *p != ')'; ++p) {
        if (out.count == maximum_arguments) throw std::logic_error(std::string("Alchemy OpenGL: too many arguments in ") + text);
        if (out.count < 4) kinds[out.count] = kind(*p);
        out.arguments[out.count++] = *p;
    }
    out.invoker = invokers[static_cast<std::size_t>(kind(out.result) * 81 + kinds[0] * 27 + kinds[1] * 9 + kinds[2] * 3 + kinds[3])];
    return out;
}

unsigned size_of(char code) {
    switch (code) {
    case 'C': case 'c': return 1;
    case 'S': case 's': return 2;
    case 'I': case 'i': case 'f': return 4;
    default: return 8;
    }
}

// An integer argument as the host slot: zero- or sign-extended from its width.
Slot integer(char code, std::uint64_t raw) {
    switch (code) {
    case 'I': return static_cast<std::uint32_t>(raw);
    case 'i': return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(raw)));
    case 'C': return static_cast<std::uint8_t>(raw);
    case 'c': return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(raw)));
    case 'S': return static_cast<std::uint16_t>(raw);
    case 's': return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(raw)));
    default: return raw;
    }
}

// The arguments as host slots (Apple arm64: x0-x7 and v0-v7 counted apart, then the stack with
// each argument at its natural alignment).
void read_arguments(const Signature& signature, CpuState& s, GuestMemory& m, Slot* slots) {
    unsigned general = 0, vector = 0;
    std::uint64_t stack = 0;
    for (unsigned i = 0; i < signature.count; ++i) {
        const char code = signature.arguments[i];
        const bool floating = code == 'f' || code == 'd';
        std::uint64_t raw{};
        if (floating ? vector < 8 : general < 8) {
            raw = floating ? s.v[vector++][0] : s.x[general++];
        } else {
            const auto size = size_of(code);
            stack = (stack + size - 1) / size * size;
            const auto at = s.sp + stack;
            raw = size == 8 ? m.read64(at) : size == 4 ? m.read32(at) : size == 2 ? (m.read8(at) | (m.read8(at + 1) << 8)) : m.read8(at);
            stack += size;
        }
        slots[i] = code == 'f' ? static_cast<std::uint32_t>(raw) : floating ? raw : integer(code, raw);
    }
}

void write_result(const Signature& signature, CpuState& s, Slot result) {
    switch (signature.result) {
    case 'v': return;
    case 'f': s.v[0] = {static_cast<std::uint32_t>(result), 0}; return;
    case 'd': s.v[0] = {result, 0}; return;
    default: s.x[0] = integer(signature.result, result); return;
    }
}

// ---- driver entry points ----
// One per entry of gl_functions.inc, for the whole process: the driver's functions do not
// depend on the guest process.
struct Binding {
    const Entry* entry{};
    Signature signature;
    std::atomic<void*> function{};
};

Binding* table() {
    static Binding* bindings = [] {
        auto* out = new Binding[std::size(entries)];
        for (std::size_t i = 0; i < std::size(entries); ++i) {
            out[i].entry = &entries[i];
            out[i].signature = parse(entries[i].signature);
        }
        return out;
    }();
    return bindings;
}

Binding* find(std::string_view name) {
    const auto* begin = std::begin(entries);
    const auto* end = std::end(entries);
    const auto* found = std::lower_bound(begin, end, name, [](const Entry& entry, std::string_view key) { return std::string_view(entry.name) < key; });
    return found != end && std::string_view(found->name) == name ? table() + (found - begin) : nullptr;
}

void* resolve(Binding& binding) {
    if (auto* found = binding.function.load(std::memory_order_acquire)) return found;
    if (!host::gl::current()) return nullptr;
    void* found = host::gl::proc(binding.entry->name);
    if (!found && *binding.entry->alias) found = host::gl::proc(binding.entry->alias);
    if (!found)
        throw std::runtime_error(std::string("unsupported: Alchemy: ") + binding.entry->name +
                                 " (the host's OpenGL driver does not provide it)");
    binding.function.store(found, std::memory_order_release);
    return found;
}

transmute::GuestCallHandler call_handler(Binding& binding) {
    return [&binding](CpuState& s, GuestMemory& m) {
        auto* function = resolve(binding);
        if (!function) {
            if (binding.signature.result != 'v') s.x[0] = 0;
            return;
        }
        std::array<Slot, maximum_arguments> slots{};
        read_arguments(binding.signature, s, m, slots.data());
        write_result(binding.signature, s, binding.signature.invoker(function, slots.data()));
    };
}

transmute::GuestCallHandler error_handler(Binding& binding) {
    return [&binding](CpuState& s, GuestMemory&) {
        auto* function = resolve(binding);
        s.x[0] = function ? reinterpret_cast<std::uint32_t (*)()>(function)() : gl_invalid_operation;
    };
}

// The driver's strings copied into guest memory, once per distinct text (immortal, as the
// driver's are); one per process.
struct Strings {
    explicit Strings(ProcessContext& owner) : process(owner) {}
    ProcessContext& process;
    std::mutex mutex;
    std::map<std::string, std::uint64_t, std::less<>> copies;

    std::uint64_t copy(GuestMemory& m, const char* text) {
        std::lock_guard lock(mutex);
        auto& slot = copies[text];
        if (!slot) {
            const auto length = std::strlen(text);
            slot = process.allocate_guest(length + 1, 1);
            m.write_bytes(slot, std::span(reinterpret_cast<const std::uint8_t*>(text), length + 1));
        }
        return slot;
    }
};

transmute::GuestCallHandler string_handler(Binding& binding, std::shared_ptr<Strings> strings, bool indexed) {
    return [&binding, strings = std::move(strings), indexed](CpuState& s, GuestMemory& m) {
        auto* function = resolve(binding);
        const char* text = !function ? nullptr
            : indexed ? reinterpret_cast<const char* (*)(std::uint32_t, std::uint32_t)>(function)(
                            static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]))
                      : reinterpret_cast<const char* (*)(std::uint32_t)>(function)(static_cast<std::uint32_t>(s.x[0]));
        s.x[0] = text ? strings->copy(m, text) : 0;
    };
}

// ---- framebuffer 0 of view drawables ----
std::mutex& defaults_mutex() {
    static std::mutex mutex;
    return mutex;
}
std::map<host::gl::Context*, std::uint32_t>& defaults() {
    static std::map<host::gl::Context*, std::uint32_t> map;
    return map;
}

bool window_buffer(std::uint32_t mode) {
    // GL_FRONT_LEFT .. GL_FRONT_AND_BACK (0x0400-0x0408)
    return mode >= 0x0400 && mode <= 0x0408;
}

std::int32_t integer(std::uint32_t pname) {
    std::int32_t value{};
    if (auto* get = reinterpret_cast<void (*)(std::uint32_t, std::int32_t*)>(gl_proc("glGetIntegerv"))) get(pname, &value);
    return value;
}

// The handler of `name` with framebuffer 0 standing for the current context's drawable.
transmute::GuestCallHandler framebuffer_zero(std::string_view name, transmute::GuestCallHandler call) {
    if (name == "glBindFramebuffer" || name == "glBindFramebufferEXT")
        return [call = std::move(call)](CpuState& s, GuestMemory& m) {
            if (static_cast<std::uint32_t>(s.x[1]) == 0)
                if (const auto stand_in = default_framebuffer(host::gl::current())) s.x[1] = stand_in;
            call(s, m);
        };
    if (name == "glDrawBuffer" || name == "glReadBuffer") {
        const bool draw = name == "glDrawBuffer";
        return [call = std::move(call), draw](CpuState& s, GuestMemory& m) {
            const auto stand_in = default_framebuffer(host::gl::current());
            if (stand_in && window_buffer(static_cast<std::uint32_t>(s.x[0])) &&
                static_cast<std::uint32_t>(integer(draw ? gl_framebuffer_binding : gl_read_framebuffer_binding)) == stand_in)
                s.x[0] = gl_color_attachment0;
            call(s, m);
        };
    }
    if (name == "glDrawBuffers")
        return [call = std::move(call)](CpuState& s, GuestMemory& m) {
            const auto stand_in = default_framebuffer(host::gl::current());
            const auto count = static_cast<std::int32_t>(s.x[0]);
            if (!stand_in || count <= 0 || count > 16 || !s.x[1] || static_cast<std::uint32_t>(integer(gl_framebuffer_binding)) != stand_in)
                return call(s, m);
            std::array<std::uint32_t, 16> buffers{};
            for (std::int32_t i = 0; i < count; ++i) {
                buffers[i] = m.read32(s.x[1] + 4ULL * i);
                if (window_buffer(buffers[i])) buffers[i] = gl_color_attachment0;
            }
            if (auto* draw = reinterpret_cast<void (*)(std::int32_t, const std::uint32_t*)>(gl_proc("glDrawBuffers"))) draw(count, buffers.data());
        };
    if (name == "glGetIntegerv")
        return [call = std::move(call)](CpuState& s, GuestMemory& m) {
            call(s, m);
            const auto stand_in = default_framebuffer(host::gl::current());
            if (!stand_in || !s.x[1]) return;
            const auto pname = static_cast<std::uint32_t>(s.x[0]);
            if (pname == gl_framebuffer_binding || pname == gl_read_framebuffer_binding) {
                if (m.read32(s.x[1]) == stand_in) m.write32(s.x[1], 0);
            } else if (pname == gl_draw_buffer || pname == gl_draw_buffer0 || pname == gl_read_buffer) {
                const auto binding = pname == gl_read_buffer ? gl_read_framebuffer_binding : gl_framebuffer_binding;
                if (m.read32(s.x[1]) == gl_color_attachment0 && static_cast<std::uint32_t>(integer(binding)) == stand_in)
                    m.write32(s.x[1], gl_back);
            }
        };
    return call;
}

} // namespace

void* gl_proc(std::string_view name) {
    if (auto* binding = find(name)) {
        try {
            return resolve(*binding);
        } catch (const std::runtime_error&) {
            return nullptr;
        }
    }
    return host::gl::current() ? host::gl::proc(std::string(name).c_str()) : nullptr;
}

void set_default_framebuffer(host::gl::Context* context, std::uint32_t framebuffer) {
    std::lock_guard lock(defaults_mutex());
    if (framebuffer) defaults()[context] = framebuffer;
    else defaults().erase(context);
}

std::uint32_t default_framebuffer(host::gl::Context* context) {
    if (!context) return 0;
    std::lock_guard lock(defaults_mutex());
    const auto found = defaults().find(context);
    return found == defaults().end() ? 0 : found->second;
}

void install_gl_exports(ProcessContext& process) {
    registration_audit::Site site;
    auto strings = std::make_shared<Strings>(process);
    auto* bindings = table();
    for (std::size_t i = 0; i < std::size(entries); ++i) {
        auto& binding = bindings[i];
        const std::string_view name = entries[i].name;
        transmute::GuestCallHandler handler;
        if (name == "glGetString") handler = string_handler(binding, strings, false);
        else if (name == "glGetStringi") handler = string_handler(binding, strings, true);
        else if (name == "glGetError") handler = error_handler(binding);
        else if (name == "glTextureRangeAPPLE") handler = [](CpuState&, GuestMemory&) {};
        else handler = framebuffer_zero(name, call_handler(binding));
        static_cast<void>(process.install_function("_" + std::string(name), std::move(handler), std::string(opengl_library)));
    }
}

} // namespace alchemy::opengl
