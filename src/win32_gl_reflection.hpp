#pragma once

#include "win32_gl_memory.hpp"

namespace transmute::gl_detail {

template<class... Prefix>
GuestCallHandler resource_name(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(Prefix..., GLsizei, GLsizei*, char*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        constexpr auto slot = sizeof...(Prefix);
        const auto capacity = static_cast<GLsizei>(cpu.x[slot]);
        if (capacity < 0 || capacity > 1048576) throw std::runtime_error("invalid OpenGL resource name capacity");
        if (cpu.x[slot + 1] != 0) require_range(memory, cpu.x[slot + 1], 4, true);
        require_range(memory, cpu.x[slot + 2], static_cast<std::uint64_t>(capacity), true);
        Arguments reader{cpu, memory};
        const std::tuple<Prefix...> prefix{reader.next<Prefix>()...};
        std::vector<char> name(static_cast<std::size_t>(capacity));
        GLsizei written = 0;
        std::apply([&](Prefix... args) { native(args..., capacity, &written, name.data()); }, prefix);
        if (written < 0 || (capacity > 0 && written >= capacity)) throw std::runtime_error("invalid native OpenGL resource name length");
        if (cpu.x[slot + 1] != 0) memory.write32(cpu.x[slot + 1], static_cast<std::uint32_t>(written));
        if (capacity > 0) memory.write_bytes(cpu.x[slot + 2],
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(name.data()), static_cast<std::size_t>(written) + 1));
    };
}

inline GuestCallHandler active_variable(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, char*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        const auto capacity = static_cast<GLsizei>(cpu.x[2]);
        if (capacity < 0 || capacity > 1048576) throw std::runtime_error("invalid OpenGL variable name capacity");
        for (unsigned slot = 3; slot <= 5; ++slot) if (cpu.x[slot] != 0) require_range(memory, cpu.x[slot], 4, true);
        require_range(memory, cpu.x[6], static_cast<std::uint64_t>(capacity), true);
        std::vector<char> name(static_cast<std::size_t>(capacity));
        GLsizei written = 0;
        GLint size = 0;
        GLenum type = 0;
        native(static_cast<GLuint>(cpu.x[0]), static_cast<GLuint>(cpu.x[1]), capacity, &written, &size, &type, name.data());
        if (written < 0 || (capacity > 0 && written >= capacity)) throw std::runtime_error("invalid native OpenGL variable name length");
        if (cpu.x[3] != 0) memory.write32(cpu.x[3], static_cast<std::uint32_t>(written));
        if (cpu.x[4] != 0) memory.write32(cpu.x[4], static_cast<std::uint32_t>(size));
        if (cpu.x[5] != 0) memory.write32(cpu.x[5], type);
        if (capacity > 0) memory.write_bytes(cpu.x[6],
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(name.data()), static_cast<std::size_t>(written) + 1));
    };
}

inline GuestCallHandler resolve_reflection(std::string_view name, std::uintptr_t target) {
    if (name == "glGetActiveAttrib" || name == "glGetActiveUniform") return active_variable(target);
    if (name == "glGetActiveUniformName" || name == "glGetActiveUniformBlockName") return resource_name<GLuint, GLuint>(target);
    if (name == "glGetProgramResourceName") return resource_name<GLuint, GLenum, GLuint>(target);
    if (name == "glGetActiveUniformsiv") {
        const auto native = reinterpret_cast<void(APIENTRY*)(GLuint, GLsizei, const GLuint*, GLenum, GLint*)>(target);
        return [native](CpuState& cpu, GuestMemory& memory) {
            const auto count = static_cast<GLsizei>(cpu.x[1]);
            if (count < 0 || count > 65536) throw std::runtime_error("invalid OpenGL uniform reflection count");
            const auto bytes = static_cast<std::size_t>(count) * 4;
            require_range(memory, cpu.x[2], bytes, false);
            require_range(memory, cpu.x[4], bytes, true);
            std::vector<GLuint> indices(static_cast<std::size_t>(count));
            std::vector<GLint> values(static_cast<std::size_t>(count));
            memory.read_bytes(cpu.x[2], std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(indices.data()), bytes));
            native(static_cast<GLuint>(cpu.x[0]), count, indices.data(), static_cast<GLenum>(cpu.x[3]), values.data());
            memory.write_bytes(cpu.x[4], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(values.data()), bytes));
        };
    }
    if (name == "glGetActiveUniformBlockiv") {
        const auto native = reinterpret_cast<void(APIENTRY*)(GLuint, GLuint, GLenum, GLint*)>(target);
        return [native](CpuState& cpu, GuestMemory& memory) {
            const auto program = static_cast<GLuint>(cpu.x[0]);
            const auto index = static_cast<GLuint>(cpu.x[1]);
            const auto parameter = static_cast<GLenum>(cpu.x[2]);
            GLint count = 1;
            if (parameter == 0x8a43) native(program, index, 0x8a42, &count); // ACTIVE_UNIFORM_INDICES, ACTIVE_UNIFORMS
            if (count < 0 || count > 65536) throw std::runtime_error("invalid OpenGL block reflection count");
            const auto bytes = static_cast<std::size_t>(count) * 4;
            require_range(memory, cpu.x[3], bytes, true);
            std::vector<GLint> values(static_cast<std::size_t>(count));
            if (count != 0) native(program, index, parameter, values.data());
            memory.write_bytes(cpu.x[3], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(values.data()), bytes));
        };
    }
    if (name == "glGetProgramResourceiv") {
        const auto native = reinterpret_cast<void(APIENTRY*)(GLuint, GLenum, GLuint, GLsizei, const GLenum*, GLsizei, GLsizei*, GLint*)>(target);
        return [native](CpuState& cpu, GuestMemory& memory) {
            const auto count = static_cast<GLsizei>(cpu.x[3]);
            const auto capacity = static_cast<GLsizei>(cpu.x[5]);
            if (count < 0 || count > 65536 || capacity < 0 || capacity > 65536)
                throw std::runtime_error("invalid OpenGL resource reflection extent");
            require_range(memory, cpu.x[4], static_cast<std::uint64_t>(count) * 4, false);
            require_range(memory, cpu.x[7], static_cast<std::uint64_t>(capacity) * 4, true);
            if (cpu.x[6] != 0) require_range(memory, cpu.x[6], 4, true);
            std::vector<GLenum> properties(static_cast<std::size_t>(count));
            std::vector<GLint> values(static_cast<std::size_t>(capacity));
            memory.read_bytes(cpu.x[4], std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(properties.data()), static_cast<std::size_t>(count) * 4));
            GLsizei written = 0;
            native(static_cast<GLuint>(cpu.x[0]), static_cast<GLenum>(cpu.x[1]), static_cast<GLuint>(cpu.x[2]),
                   count, properties.data(), capacity, &written, values.data());
            if (written < 0 || written > capacity) throw std::runtime_error("invalid native OpenGL reflection output length");
            if (cpu.x[6] != 0) memory.write32(cpu.x[6], static_cast<std::uint32_t>(written));
            memory.write_bytes(cpu.x[7], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(values.data()), static_cast<std::size_t>(written) * 4));
        };
    }
    return {};
}

} // namespace transmute::gl_detail
