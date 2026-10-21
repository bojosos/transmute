#pragma once

#include "win32_gl_buffers.hpp"

namespace transmute::gl_detail {

class SyncObjects {
public:
    std::uint64_t publish(void* native) {
        std::lock_guard lock(mutex_);
        const auto guest = next_;
        objects_.emplace(guest, Object{native, ::wglGetCurrentContext()});
        next_ += 16;
        return guest;
    }

    template<class Call>
    auto with_native(std::uint64_t guest, Call&& call, bool allow_missing = false) {
        std::lock_guard lock(mutex_);
        const auto found = objects_.find(guest);
        if (found == objects_.end() && guest != 0 && !allow_missing)
            throw std::runtime_error("invalid guest OpenGL sync handle");
        return call(found == objects_.end() ? nullptr : found->second.native);
    }

    void remove(std::uint64_t guest, void(APIENTRY* native_delete)(void*)) {
        std::lock_guard lock(mutex_);
        if (guest == 0) return;
        const auto found = objects_.find(guest);
        if (found == objects_.end()) throw std::runtime_error("invalid guest OpenGL sync handle");
        native_delete(found->second.native);
        objects_.erase(found);
    }

    bool has_context(HGLRC context) const {
        std::lock_guard lock(mutex_);
        return std::any_of(objects_.begin(), objects_.end(), [context](const auto& entry) {
            return entry.second.context == context;
        });
    }

private:
    struct Object { void* native; HGLRC context; };
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Object> objects_;
    std::uint64_t next_{0x0000603400000000ULL};
};

inline GuestCallHandler resolve_sync(std::string_view name, std::uintptr_t target,
                                    const std::shared_ptr<SyncObjects>& objects) {
    if (name == "glFenceSync") {
        const auto native = reinterpret_cast<void*(APIENTRY*)(GLenum, GLbitfield)>(target);
        const auto remove = buffer_function<void(APIENTRY*)(void*)>("glDeleteSync");
        return [objects, native, remove](CpuState& cpu, GuestMemory&) {
            auto* sync = native(static_cast<GLenum>(cpu.x[0]), static_cast<GLbitfield>(cpu.x[1]));
            cpu.x[0] = 0;
            if (sync == nullptr) return;
            try { cpu.x[0] = objects->publish(sync); }
            catch (...) { remove(sync); throw; }
        };
    }
    if (name == "glDeleteSync") {
        const auto native = reinterpret_cast<void(APIENTRY*)(void*)>(target);
        return [objects, native](CpuState& cpu, GuestMemory&) { objects->remove(cpu.x[0], native); };
    }
    if (name == "glIsSync") {
        const auto native = reinterpret_cast<GLboolean(APIENTRY*)(void*)>(target);
        return [objects, native](CpuState& cpu, GuestMemory&) {
            cpu.x[0] = objects->with_native(cpu.x[0], [native](void* sync) { return sync == nullptr ? GL_FALSE : native(sync); }, true);
        };
    }
    if (name == "glClientWaitSync") {
        const auto native = reinterpret_cast<GLenum(APIENTRY*)(void*, GLbitfield, std::uint64_t)>(target);
        return [objects, native](CpuState& cpu, GuestMemory&) {
            cpu.x[0] = objects->with_native(cpu.x[0], [&](void* sync) {
                return native(sync, static_cast<GLbitfield>(cpu.x[1]), cpu.x[2]);
            });
        };
    }
    if (name == "glWaitSync") {
        const auto native = reinterpret_cast<void(APIENTRY*)(void*, GLbitfield, std::uint64_t)>(target);
        return [objects, native](CpuState& cpu, GuestMemory&) {
            objects->with_native(cpu.x[0], [&](void* sync) { native(sync, static_cast<GLbitfield>(cpu.x[1]), cpu.x[2]); });
        };
    }
    if (name == "glGetSynciv") {
        const auto native = reinterpret_cast<void(APIENTRY*)(void*, GLenum, GLsizei, GLsizei*, GLint*)>(target);
        return [objects, native](CpuState& cpu, GuestMemory& memory) {
            const auto parameter = static_cast<GLenum>(cpu.x[1]);
            const auto capacity = static_cast<GLsizei>(cpu.x[2]);
            if (parameter < 0x9112 || parameter > 0x9115 || capacity < 0)
                throw std::runtime_error("unsupported OpenGL sync query extent");
            if (cpu.x[3] != 0) require_range(memory, cpu.x[3], 4, true);
            if (capacity > 0) require_range(memory, cpu.x[4], 4, true);
            GLsizei written = 0;
            GLint value = 0;
            objects->with_native(cpu.x[0], [&](void* sync) { native(sync, parameter, capacity, &written, &value); });
            if (written < 0 || written > 1) throw std::runtime_error("invalid native OpenGL sync query length");
            if (cpu.x[3] != 0) memory.write32(cpu.x[3], static_cast<std::uint32_t>(written));
            if (written != 0) memory.write32(cpu.x[4], static_cast<std::uint32_t>(value));
        };
    }
    return {};
}

} // namespace transmute::gl_detail
