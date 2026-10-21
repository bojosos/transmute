#pragma once

#include "win32_gl_memory.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace transmute::gl_detail {

class BufferMappings {
public:
    std::uint64_t publish(void* native, std::size_t size, bool readable,
                          bool writable, GuestMemory& memory) {
        std::lock_guard lock(mutex_);
        if (mappings_.contains(native)) throw std::runtime_error("aliased OpenGL buffer mappings are unsupported");
        const auto guest = next_;
        next_ += (size + 4095) & ~std::uint64_t{4095};
        memory.map_external_memory(guest, static_cast<std::uint8_t*>(native), size, writable);
        try {
            memory.set_memory_access(guest, readable, writable, false);
            mappings_.emplace(native, Mapping{guest, ::wglGetCurrentContext()});
        } catch (...) { memory.unmap_memory(guest); throw; }
        return guest;
    }

    std::uint64_t guest_pointer(void* native) const {
        if (native == nullptr) return 0;
        std::lock_guard lock(mutex_);
        const auto found = mappings_.find(native);
        if (found == mappings_.end()) throw std::runtime_error("OpenGL buffer pointer has no guest mapping");
        return found->second.guest;
    }

    void revoke(void* native, GuestMemory& memory) {
        std::lock_guard lock(mutex_);
        const auto found = mappings_.find(native);
        if (found == mappings_.end()) return;
        memory.unmap_memory(found->second.guest);
        mappings_.erase(found);
    }

    bool has_context(HGLRC context) const {
        std::lock_guard lock(mutex_);
        return std::any_of(mappings_.begin(), mappings_.end(),
            [context](const auto& entry) { return entry.second.context == context; });
    }

    bool empty() const {
        std::lock_guard lock(mutex_);
        return mappings_.empty();
    }

private:
    struct Mapping { std::uint64_t guest; HGLRC context; };
    mutable std::mutex mutex_;
    std::unordered_map<void*, Mapping> mappings_;
    std::uint64_t next_{0x0000603300000000ULL};
};

template<class Function>
Function buffer_function(const char* name) {
    const auto address = reinterpret_cast<std::uintptr_t>(::wglGetProcAddress(name));
    if (address <= 3 || address == UINTPTR_MAX)
        throw std::runtime_error(std::string("OpenGL buffer helper is unavailable: ") + name);
    return reinterpret_cast<Function>(address);
}

inline GuestCallHandler resolve_buffer(std::string_view name, std::uintptr_t target,
                                      const std::shared_ptr<BufferMappings>& mappings) {
    using GetPointer = void(APIENTRY*)(GLenum, GLenum, void**);
    using Unmap = GLboolean(APIENTRY*)(GLenum);
    if (name == "glMapBuffer" || name == "glMapBufferRange" || name == "glMapNamedBuffer" || name == "glMapNamedBufferRange") {
        const bool named = name.find("Named") != std::string_view::npos;
        const auto unmap = buffer_function<Unmap>(named ? "glUnmapNamedBuffer" : "glUnmapBuffer");
        const bool range = name.ends_with("Range");
        using GetSize = void(APIENTRY*)(GLenum, GLenum, std::int64_t*);
        const auto get_size = range ? nullptr : buffer_function<GetSize>(named ? "glGetNamedBufferParameteri64v" : "glGetBufferParameteri64v");
        return [mappings, target, unmap, get_size, range](CpuState& cpu, GuestMemory& memory) {
            const auto buffer_target = static_cast<GLenum>(cpu.x[0]);
            std::int64_t size = range ? static_cast<std::int64_t>(cpu.x[2]) : 0;
            if (!range) get_size(buffer_target, 0x8764, &size); // GL_BUFFER_SIZE
            if (size < 0 || size > 256LL * 1024 * 1024)
                throw std::runtime_error("OpenGL buffer mapping exceeds transfer limit");
            bool readable = false, writable = false;
            void* native = nullptr;
            if (range) {
                const auto access = static_cast<GLbitfield>(cpu.x[3]);
                readable = (access & 1) != 0; // GL_MAP_READ_BIT
                writable = (access & 2) != 0; // GL_MAP_WRITE_BIT
                native = reinterpret_cast<void*(APIENTRY*)(GLenum, std::intptr_t, std::intptr_t, GLbitfield)>(target)(
                    buffer_target, static_cast<std::intptr_t>(cpu.x[1]), static_cast<std::intptr_t>(size), access);
            } else {
                const auto access = static_cast<GLenum>(cpu.x[1]);
                readable = access == 0x88b8 || access == 0x88ba; // GL_READ_ONLY, GL_READ_WRITE
                writable = access == 0x88b9 || access == 0x88ba; // GL_WRITE_ONLY, GL_READ_WRITE
                native = reinterpret_cast<void*(APIENTRY*)(GLenum, GLenum)>(target)(buffer_target, access);
            }
            cpu.x[0] = 0;
            if (native == nullptr) return;
            if (size == 0) { unmap(buffer_target); return; }
            try {
                // The driver owns the bytes. Guest accesses retain their bounds
                // and permissions; uploading and flushing need no staging copy.
                cpu.x[0] = mappings->publish(native, static_cast<std::size_t>(size), readable, writable, memory);
            } catch (...) { unmap(buffer_target); throw; }
        };
    }
    if (name == "glUnmapBuffer" || name == "glUnmapNamedBuffer") {
        const auto get_pointer = buffer_function<GetPointer>(name == "glUnmapNamedBuffer" ? "glGetNamedBufferPointerv" : "glGetBufferPointerv");
        const auto unmap = reinterpret_cast<Unmap>(target);
        return [mappings, get_pointer, unmap](CpuState& cpu, GuestMemory& memory) {
            const auto buffer_target = static_cast<GLenum>(cpu.x[0]);
            void* pointer = nullptr;
            get_pointer(buffer_target, 0x88bd, &pointer); // GL_BUFFER_MAP_POINTER
            // Revoke guest access before the driver invalidates its host span.
            mappings->revoke(pointer, memory);
            cpu.x[0] = unmap(buffer_target);
        };
    }
    if (name == "glGetBufferPointerv" || name == "glGetNamedBufferPointerv") {
        const auto get_pointer = reinterpret_cast<GetPointer>(target);
        return [mappings, get_pointer](CpuState& cpu, GuestMemory& memory) {
            require_range(memory, cpu.x[2], sizeof(std::uint64_t), true);
            void* pointer = nullptr;
            get_pointer(static_cast<GLenum>(cpu.x[0]), static_cast<GLenum>(cpu.x[1]), &pointer);
            memory.write64(cpu.x[2], mappings->guest_pointer(pointer));
        };
    }
    if (name == "glDeleteBuffers") {
        const auto remove = objects<false>(target);
        const auto get_pointer = buffer_function<GetPointer>("glGetBufferPointerv");
        const auto bind = buffer_function<void(APIENTRY*)(GLenum, GLuint)>("glBindBuffer");
        const auto is_buffer = buffer_function<GLboolean(APIENTRY*)(GLuint)>("glIsBuffer");
        return [mappings, remove, get_pointer, bind, is_buffer](CpuState& cpu, GuestMemory& memory) {
            if (!mappings->empty()) {
                const auto count = static_cast<GLsizei>(cpu.x[0]);
                if (count < 0 || count > 65536) throw std::runtime_error("invalid OpenGL buffer deletion count");
                require_range(memory, cpu.x[1], static_cast<std::uint64_t>(count) * 4, false);
                GLint previous = 0;
                ::glGetIntegerv(0x8f36, &previous); // GL_COPY_READ_BUFFER_BINDING
                for (GLsizei index = 0; index < count; ++index) {
                    const auto object = memory.read32(cpu.x[1] + 4ULL * index);
                    if (!is_buffer(object)) continue;
                    bind(0x8f36, object);
                    void* pointer = nullptr;
                    get_pointer(0x8f36, 0x88bd, &pointer);
                    try { mappings->revoke(pointer, memory); }
                    catch (...) { bind(0x8f36, static_cast<GLuint>(previous)); throw; }
                }
                bind(0x8f36, static_cast<GLuint>(previous));
            }
            remove(cpu, memory);
        };
    }
    return {};
}

} // namespace transmute::gl_detail
