#pragma once

#include "win32_gl_scalar.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace transmute::gl_detail {

inline void require_range(GuestMemory& memory, std::uint64_t pointer,
                           std::uint64_t bytes, bool output) {
    if (bytes == 0) return;
    constexpr std::uint64_t limit = 256ULL * 1024 * 1024;
    if (bytes > limit || pointer == 0 || bytes > UINT64_MAX - pointer)
        throw std::runtime_error("invalid OpenGL guest transfer extent");
    const auto region = memory.query_region(pointer);
    if (!region || !(output ? region->writable : region->readable) ||
        pointer + bytes > region->base + region->size)
        throw std::runtime_error("OpenGL transfer exceeds guest memory permissions or extent");
}

inline std::string string(GuestMemory& memory, std::uint64_t pointer) {
    bool terminated = false;
    const auto bytes = memory.read_string(pointer, 1, 1024 * 1024, &terminated);
    if (!terminated) throw std::runtime_error("unterminated OpenGL guest string");
    return std::string(bytes.begin(), bytes.end());
}

template<bool Output>
GuestCallHandler objects(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(GLsizei, GLuint*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        const auto count = static_cast<GLsizei>(cpu.x[0]);
        if (count < 0 || count > 65536) throw std::runtime_error("invalid OpenGL object count");
        const auto bytes = static_cast<std::size_t>(count) * sizeof(GLuint);
        require_range(memory, cpu.x[1], bytes, Output);
        std::vector<GLuint> values(static_cast<std::size_t>(count));
        if constexpr (!Output) memory.read_bytes(cpu.x[1], std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(values.data()), bytes));
        native(count, values.data());
        if constexpr (Output) memory.write_bytes(cpu.x[1], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(values.data()), bytes));
    };
}

template<class Result>
GuestCallHandler named_location(std::uintptr_t target) {
    const auto native = reinterpret_cast<Result(APIENTRY*)(GLuint, const char*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        const auto text = string(memory, cpu.x[1]);
        cpu.x[0] = static_cast<std::make_unsigned_t<Result>>(native(static_cast<GLuint>(cpu.x[0]), text.c_str()));
    };
}

inline GuestCallHandler named_binding(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(GLuint, GLuint, const char*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        const auto text = string(memory, cpu.x[2]);
        native(static_cast<GLuint>(cpu.x[0]), static_cast<GLuint>(cpu.x[1]), text.c_str());
    };
}

inline GuestCallHandler single_query(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(GLuint, GLenum, GLint*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        const unsigned count = cpu.x[1] == 0x8267 ? 3 : 1; // GL_COMPUTE_WORK_GROUP_SIZE
        require_range(memory, cpu.x[2], count * 4ULL, true);
        std::array<GLint, 3> values{};
        for (unsigned i = 0; i < count; ++i) values[i] = static_cast<GLint>(memory.read32(cpu.x[2] + i * 4ULL));
        native(static_cast<GLuint>(cpu.x[0]), static_cast<GLenum>(cpu.x[1]), values.data());
        memory.write_bytes(cpu.x[2], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(values.data()), count * 4ULL));
    };
}

inline GuestCallHandler info_log(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(GLuint, GLsizei, GLsizei*, char*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        const auto capacity = static_cast<GLsizei>(cpu.x[1]);
        if (capacity < 0) throw std::runtime_error("invalid OpenGL log capacity");
        require_range(memory, cpu.x[3], static_cast<std::uint32_t>(capacity), true);
        if (cpu.x[2] != 0) require_range(memory, cpu.x[2], 4, true);
        std::vector<char> output(static_cast<std::size_t>(std::max(capacity, 1)));
        GLsizei length = 0;
        native(static_cast<GLuint>(cpu.x[0]), capacity, &length, output.data());
        if (length < 0 || (capacity != 0 && length >= capacity)) throw std::runtime_error("invalid native OpenGL log length");
        if (cpu.x[2] != 0) memory.write32(cpu.x[2], static_cast<std::uint32_t>(length));
        if (capacity != 0) memory.write_bytes(cpu.x[3], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(output.data()), static_cast<std::size_t>(length) + 1));
    };
}

inline GuestCallHandler shader_source(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        const auto count = static_cast<GLsizei>(cpu.x[1]);
        if (count < 0 || count > 4096) throw std::runtime_error("invalid OpenGL shader string count");
        require_range(memory, cpu.x[2], static_cast<std::uint64_t>(count) * 8, false);
        if (cpu.x[3] != 0) require_range(memory, cpu.x[3], static_cast<std::uint64_t>(count) * 4, false);
        std::vector<std::string> strings(static_cast<std::size_t>(count));
        std::vector<const char*> pointers(static_cast<std::size_t>(count));
        std::vector<GLint> lengths(static_cast<std::size_t>(count));
        std::uint64_t total = 0;
        for (GLsizei i = 0; i < count; ++i) {
            const auto index = static_cast<std::size_t>(i);
            const auto pointer = memory.read64(cpu.x[2] + index * 8);
            const auto length = cpu.x[3] == 0 ? -1 : static_cast<GLint>(memory.read32(cpu.x[3] + index * 4));
            if (length < 0) strings[index] = string(memory, pointer);
            else {
                require_range(memory, pointer, static_cast<std::uint32_t>(length), false);
                strings[index].resize(static_cast<std::size_t>(length));
                memory.read_bytes(pointer, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(strings[index].data()), strings[index].size()));
            }
            total += strings[index].size();
            if (total > 256ULL * 1024 * 1024) throw std::runtime_error("OpenGL shader source exceeds transfer limit");
            pointers[index] = strings[index].c_str();
            lengths[index] = static_cast<GLint>(strings[index].size());
        }
        native(static_cast<GLuint>(cpu.x[0]), count, pointers.data(), lengths.data());
    };
}

template<bool Subrange>
GuestCallHandler buffer_data(std::uintptr_t target) {
    return [target](CpuState& cpu, GuestMemory& memory) {
        const auto size = static_cast<std::intptr_t>(cpu.x[Subrange ? 2 : 1]);
        const auto pointer = cpu.x[Subrange ? 3 : 2];
        if (size < 0 || size > 256LL * 1024 * 1024) throw std::runtime_error("invalid OpenGL buffer transfer size");
        std::vector<std::uint8_t> bytes;
        if (pointer != 0) {
            require_range(memory, pointer, static_cast<std::uint64_t>(size), false);
            bytes.resize(static_cast<std::size_t>(size));
            memory.read_bytes(pointer, bytes);
        } else if constexpr (Subrange) {
            if (size != 0) throw std::runtime_error("null OpenGL buffer subrange source");
        }
        if constexpr (Subrange) reinterpret_cast<void(APIENTRY*)(GLenum, std::intptr_t, std::intptr_t, const void*)>(target)(
            static_cast<GLenum>(cpu.x[0]), static_cast<std::intptr_t>(cpu.x[1]), size, pointer == 0 ? nullptr : bytes.data());
        else reinterpret_cast<void(APIENTRY*)(GLenum, std::intptr_t, const void*, GLenum)>(target)(
            static_cast<GLenum>(cpu.x[0]), size, pointer == 0 ? nullptr : bytes.data(), static_cast<GLenum>(cpu.x[3]));
    };
}

template<class Value, unsigned Components, class First = GLint>
GuestCallHandler uniform_vector(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(First, GLsizei, const Value*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        const auto count = static_cast<GLsizei>(cpu.x[1]);
        if (count < 0 || count > 1048576) throw std::runtime_error("invalid OpenGL uniform count");
        const auto elements = static_cast<std::size_t>(count) * Components;
        require_range(memory, cpu.x[2], elements * sizeof(Value), false);
        std::vector<Value> values(elements);
        memory.read_bytes(cpu.x[2], std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(values.data()), elements * sizeof(Value)));
        native(static_cast<First>(cpu.x[0]), count, values.data());
    };
}

template<unsigned Elements>
GuestCallHandler uniform_matrix(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        const auto count = static_cast<GLsizei>(cpu.x[1]);
        if (count < 0 || count > 1048576) throw std::runtime_error("invalid OpenGL matrix count");
        const auto elements = static_cast<std::size_t>(count) * Elements;
        require_range(memory, cpu.x[3], elements * sizeof(GLfloat), false);
        std::vector<GLfloat> values(elements);
        memory.read_bytes(cpu.x[3], std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(values.data()), elements * sizeof(GLfloat)));
        native(static_cast<GLint>(cpu.x[0]), count, static_cast<GLboolean>(cpu.x[2]), values.data());
    };
}

template<class Value, bool Indexed>
GuestCallHandler state_query(std::uintptr_t target) {
    return [target](CpuState& cpu, GuestMemory& memory) {
        unsigned count = 1;
        if (cpu.x[0] == GL_VIEWPORT || cpu.x[0] == GL_SCISSOR_BOX || cpu.x[0] == GL_COLOR_WRITEMASK || cpu.x[0] == GL_COLOR_CLEAR_VALUE) count = 4;
        else if (cpu.x[0] == GL_DEPTH_RANGE || cpu.x[0] == GL_MAX_VIEWPORT_DIMS || cpu.x[0] == 0x846d || cpu.x[0] == 0x846e) count = 2;
        else if (cpu.x[0] == GL_MODELVIEW_MATRIX || cpu.x[0] == GL_PROJECTION_MATRIX || cpu.x[0] == GL_TEXTURE_MATRIX) count = 16;
        else if (cpu.x[0] == 0x86a3 || cpu.x[0] == 0x8df8 || cpu.x[0] == 0x87ff)
            throw std::runtime_error("variable-length OpenGL state query is not yet supported");
        const auto pointer = cpu.x[Indexed ? 2 : 1];
        require_range(memory, pointer, count * sizeof(Value), true);
        std::array<Value, 16> values{};
        if constexpr (Indexed) reinterpret_cast<void(APIENTRY*)(GLenum, GLuint, Value*)>(target)(static_cast<GLenum>(cpu.x[0]), static_cast<GLuint>(cpu.x[1]), values.data());
        else reinterpret_cast<void(APIENTRY*)(GLenum, Value*)>(target)(static_cast<GLenum>(cpu.x[0]), values.data());
        memory.write_bytes(pointer, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(values.data()), count * sizeof(Value)));
    };
}

template<bool Indexed>
GuestCallHandler uuid_query(std::uintptr_t target) {
    return [target](CpuState& cpu, GuestMemory& memory) {
        const auto luid = !Indexed && cpu.x[0] == 0x9599; // GL_DEVICE_LUID_EXT
        if (cpu.x[0] != 0x9597 && cpu.x[0] != 0x9598 && !luid)
            throw std::runtime_error("unsupported OpenGL identifier query parameter " + std::to_string(cpu.x[0]));
        const auto bytes = luid ? 8U : 16U;
        const auto pointer = cpu.x[Indexed ? 2 : 1];
        require_range(memory, pointer, bytes, true);
        std::array<GLubyte, 16> value{};
        if constexpr (Indexed) reinterpret_cast<void(APIENTRY*)(GLenum, GLuint, GLubyte*)>(target)(static_cast<GLenum>(cpu.x[0]), static_cast<GLuint>(cpu.x[1]), value.data());
        else reinterpret_cast<void(APIENTRY*)(GLenum, GLubyte*)>(target)(static_cast<GLenum>(cpu.x[0]), value.data());
        memory.write_bytes(pointer, std::span<const std::uint8_t>(value.data(), bytes));
    };
}

template<class Value>
GuestCallHandler internal_format(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(GLenum, GLenum, GLenum, GLsizei, Value*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        const auto count = static_cast<GLsizei>(cpu.x[3]);
        if (count < 0 || count > 65536) throw std::runtime_error("invalid OpenGL internal format output count");
        const auto bytes = static_cast<std::size_t>(count) * sizeof(Value);
        require_range(memory, cpu.x[4], bytes, true);
        std::vector<Value> values(static_cast<std::size_t>(count));
        native(static_cast<GLenum>(cpu.x[0]), static_cast<GLenum>(cpu.x[1]), static_cast<GLenum>(cpu.x[2]), count, values.data());
        memory.write_bytes(cpu.x[4], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(values.data()), bytes));
    };
}

inline unsigned pixel_bytes(GLenum format, GLenum type) {
    switch (type) {
    case 0x8032: case 0x8362: return 1;
    case 0x8363: case 0x8364: case 0x8033: case 0x8365: case 0x8034: case 0x8366: return 2;
    case 0x8035: case 0x8367: case 0x8036: case 0x8368: case 0x84fa: case 0x8c3b: case 0x8c3e: return 4;
    case 0x8dad: return 8;
    default: break;
    }
    unsigned components = 0;
    switch (format) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE:
    case GL_DEPTH_COMPONENT: case 0x8d94: components = 1; break;
    case GL_LUMINANCE_ALPHA: case 0x8227: case 0x8228: components = 2; break;
    case GL_RGB: case 0x80e0: case 0x8d98: case 0x8d9a: components = 3; break;
    case GL_RGBA: case 0x80e1: case 0x8d99: case 0x8d9b: components = 4; break;
    default: throw std::runtime_error("unsupported OpenGL pixel transfer format");
    }
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return components;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case 0x140b: return components * 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return components * 4;
    case GL_DOUBLE: return components * 8;
    default: throw std::runtime_error("unsupported OpenGL pixel transfer type");
    }
}

using ReadPixels = void(APIENTRY*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);

inline void read_pixels(CpuState& cpu, GuestMemory& memory, ReadPixels native) {
    const auto width = static_cast<GLsizei>(cpu.x[2]);
    const auto height = static_cast<GLsizei>(cpu.x[3]);
    const auto format = static_cast<GLenum>(cpu.x[4]);
    const auto type = static_cast<GLenum>(cpu.x[5]);
    GLint buffer = 0;
    if (::wglGetProcAddress("glBindBuffer") != nullptr) ::glGetIntegerv(0x88ed, &buffer); // GL_PIXEL_PACK_BUFFER_BINDING
    if (buffer != 0) {
        native(static_cast<GLint>(cpu.x[0]), static_cast<GLint>(cpu.x[1]), width, height,
               format, type, reinterpret_cast<void*>(cpu.x[6]));
        return;
    }
    GLint alignment = 0, row_length = 0, skip_rows = 0, skip_pixels = 0;
    ::glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    ::glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
    ::glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
    ::glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
    if (width < 0 || height < 0 || row_length < 0 || skip_rows < 0 || skip_pixels < 0 ||
        (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8))
        throw std::runtime_error("invalid OpenGL framebuffer readback extent");
    if (width == 0 || height == 0) return;
    constexpr std::uint64_t limit = 256ULL * 1024 * 1024;
    const auto element = pixel_bytes(format, type);
    const auto row_bytes = static_cast<std::uint64_t>(width) * element;
    const auto packed_row = static_cast<std::uint64_t>(row_length == 0 ? width : row_length) * element;
    const auto stride = (packed_row + alignment - 1) & ~static_cast<std::uint64_t>(alignment - 1);
    const auto rows = static_cast<std::uint64_t>(skip_rows) + height - 1;
    const auto pixel_offset = static_cast<std::uint64_t>(skip_pixels) * element;
    if (row_bytes > limit || stride > limit || pixel_offset > limit - row_bytes ||
        (stride != 0 && rows > (limit - pixel_offset - row_bytes) / stride))
        throw std::runtime_error("OpenGL framebuffer readback exceeds transfer limit");
    const auto size = rows * stride + pixel_offset + row_bytes;
    require_range(memory, cpu.x[6], size, true);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size));
    native(static_cast<GLint>(cpu.x[0]), static_cast<GLint>(cpu.x[1]), width, height,
           format, type, pixels.data());
    for (std::uint64_t row = 0; row < static_cast<std::uint64_t>(height); ++row) {
        const auto offset = (static_cast<std::uint64_t>(skip_rows) + row) * stride + pixel_offset;
        memory.write_bytes(cpu.x[6] + offset,
            std::span<const std::uint8_t>(pixels.data() + offset, static_cast<std::size_t>(row_bytes)));
    }
}

inline std::uint64_t unpack_extent(unsigned dimensions, GLsizei width, GLsizei height,
                                    GLsizei depth, GLenum format, GLenum type) {
    if (width < 0 || height < 0 || depth < 0) throw std::runtime_error("negative OpenGL texture extent");
    if (width == 0 || height == 0 || depth == 0) return 0;
    GLint alignment = 0, row_length = 0, image_height = 0, skip_pixels = 0, skip_rows = 0, skip_images = 0;
    ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    ::glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    ::glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
    ::glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
    if (dimensions == 3) {
        ::glGetIntegerv(0x806e, &image_height); // GL_UNPACK_IMAGE_HEIGHT
        ::glGetIntegerv(0x806d, &skip_images); // GL_UNPACK_SKIP_IMAGES
    }
    if ((alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8) ||
        row_length < 0 || image_height < 0 || skip_pixels < 0 || skip_rows < 0 || skip_images < 0)
        throw std::runtime_error("invalid OpenGL unpack state");
    constexpr std::uint64_t limit = 256ULL * 1024 * 1024;
    const auto multiply = [](std::uint64_t a, std::uint64_t b) {
        if (a != 0 && b > limit / a) throw std::runtime_error("OpenGL texture transfer exceeds limit");
        return a * b;
    };
    const auto add = [](std::uint64_t a, std::uint64_t b) {
        if (a > limit || b > limit - a) throw std::runtime_error("OpenGL texture transfer exceeds limit");
        return a + b;
    };
    const auto element = pixel_bytes(format, type);
    const auto row_bytes = multiply(static_cast<std::uint64_t>(row_length == 0 ? width : row_length), element);
    const auto stride = (row_bytes + alignment - 1) & ~static_cast<std::uint64_t>(alignment - 1);
    const auto image_stride = multiply(stride, static_cast<std::uint64_t>(image_height == 0 ? height : image_height));
    auto size = multiply(static_cast<std::uint64_t>(skip_pixels), element);
    if (dimensions > 1) size = add(size, multiply(static_cast<std::uint64_t>(skip_rows + static_cast<std::int64_t>(height) - 1), stride));
    if (dimensions > 2) size = add(size, multiply(static_cast<std::uint64_t>(skip_images + static_cast<std::int64_t>(depth) - 1), image_stride));
    return add(size, multiply(static_cast<std::uint64_t>(width), element));
}

template<unsigned Dimensions, bool Subrange>
GuestCallHandler texture_data(std::uintptr_t target) {
    return [target](CpuState& cpu, GuestMemory& memory) {
        const auto argument = [&cpu, &memory](unsigned index) {
            return index < 8 ? cpu.x[index] : memory.read64(cpu.sp + (index - 8) * 8ULL);
        };
        constexpr unsigned width_slot = Subrange ? 2 + Dimensions : 3;
        constexpr unsigned format_slot = Subrange ? 2 + 2 * Dimensions : 4 + Dimensions;
        const auto width = static_cast<GLsizei>(argument(width_slot));
        const auto height = Dimensions >= 2 ? static_cast<GLsizei>(argument(width_slot + 1)) : 1;
        const auto depth = Dimensions >= 3 ? static_cast<GLsizei>(argument(width_slot + 2)) : 1;
        const auto format = static_cast<GLenum>(argument(format_slot));
        const auto type = static_cast<GLenum>(argument(format_slot + 1));
        const auto pointer = argument(format_slot + 2);
        GLint buffer = 0;
        if (::wglGetProcAddress("glBindBuffer") != nullptr) ::glGetIntegerv(0x88ef, &buffer); // GL_PIXEL_UNPACK_BUFFER_BINDING
        std::vector<std::uint8_t> bytes;
        const void* data = nullptr;
        if (buffer != 0) data = reinterpret_cast<const void*>(pointer);
        else if (pointer != 0) {
            const auto size = unpack_extent(Dimensions, width, height, depth, format, type);
            require_range(memory, pointer, size, false);
            bytes.resize(static_cast<std::size_t>(size));
            memory.read_bytes(pointer, bytes);
            data = bytes.data();
        } else if constexpr (Subrange) {
            if (width != 0 && height != 0 && depth != 0) throw std::runtime_error("null OpenGL texture subrange source");
        }
        if constexpr (Dimensions == 1 && !Subrange) reinterpret_cast<void(APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLint, GLenum, GLenum, const void*)>(target)(
            static_cast<GLenum>(cpu.x[0]), static_cast<GLint>(cpu.x[1]), static_cast<GLint>(cpu.x[2]), width, static_cast<GLint>(argument(4)), format, type, data);
        else if constexpr (Dimensions == 2 && !Subrange) reinterpret_cast<void(APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)>(target)(
            static_cast<GLenum>(cpu.x[0]), static_cast<GLint>(cpu.x[1]), static_cast<GLint>(cpu.x[2]), width, height, static_cast<GLint>(argument(5)), format, type, data);
        else if constexpr (Dimensions == 3 && !Subrange) reinterpret_cast<void(APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)>(target)(
            static_cast<GLenum>(cpu.x[0]), static_cast<GLint>(cpu.x[1]), static_cast<GLint>(cpu.x[2]), width, height, depth, static_cast<GLint>(argument(6)), format, type, data);
        else if constexpr (Dimensions == 1) reinterpret_cast<void(APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLenum, GLenum, const void*)>(target)(
            static_cast<GLenum>(cpu.x[0]), static_cast<GLint>(cpu.x[1]), static_cast<GLint>(cpu.x[2]), width, format, type, data);
        else if constexpr (Dimensions == 2) reinterpret_cast<void(APIENTRY*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)>(target)(
            static_cast<GLenum>(cpu.x[0]), static_cast<GLint>(cpu.x[1]), static_cast<GLint>(cpu.x[2]), static_cast<GLint>(cpu.x[3]), width, height, format, type, data);
        else reinterpret_cast<void(APIENTRY*)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*)>(target)(
            static_cast<GLenum>(cpu.x[0]), static_cast<GLint>(cpu.x[1]), static_cast<GLint>(cpu.x[2]), static_cast<GLint>(cpu.x[3]), static_cast<GLint>(cpu.x[4]), width, height, depth, format, type, data);
    };
}

template<bool Integer>
GuestCallHandler vertex_pointer(std::uintptr_t target) {
    return [target](CpuState& cpu, GuestMemory&) {
        GLint buffer = 0;
        ::glGetIntegerv(0x8894, &buffer); // GL_ARRAY_BUFFER_BINDING
        if (buffer == 0) throw std::runtime_error("retained OpenGL client vertex arrays are unsupported");
        if constexpr (Integer) reinterpret_cast<void(APIENTRY*)(GLuint, GLint, GLenum, GLsizei, const void*)>(target)(
            static_cast<GLuint>(cpu.x[0]), static_cast<GLint>(cpu.x[1]), static_cast<GLenum>(cpu.x[2]),
            static_cast<GLsizei>(cpu.x[3]), reinterpret_cast<const void*>(cpu.x[4]));
        else reinterpret_cast<void(APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)>(target)(
            static_cast<GLuint>(cpu.x[0]), static_cast<GLint>(cpu.x[1]), static_cast<GLenum>(cpu.x[2]), static_cast<GLboolean>(cpu.x[3]),
            static_cast<GLsizei>(cpu.x[4]), reinterpret_cast<const void*>(cpu.x[5]));
    };
}

inline GuestCallHandler texture_read(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(GLenum, GLint, GLenum, GLenum, void*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        const auto texture_target = static_cast<GLenum>(cpu.x[0]);
        const auto level = static_cast<GLint>(cpu.x[1]);
        const auto format = static_cast<GLenum>(cpu.x[2]);
        const auto type = static_cast<GLenum>(cpu.x[3]);
        GLint buffer = 0;
        if (::wglGetProcAddress("glBindBuffer") != nullptr) ::glGetIntegerv(0x88ed, &buffer); // GL_PIXEL_PACK_BUFFER_BINDING
        if (buffer != 0) {
            native(texture_target, level, format, type, reinterpret_cast<void*>(cpu.x[4]));
            return;
        }
        GLint width = 0, height = 1, depth = 1, alignment = 0;
        ::glGetTexLevelParameteriv(texture_target, level, GL_TEXTURE_WIDTH, &width);
        if (texture_target != GL_TEXTURE_1D) ::glGetTexLevelParameteriv(texture_target, level, GL_TEXTURE_HEIGHT, &height);
        const auto volume = texture_target == 0x806f || texture_target == 0x8c1a || texture_target == 0x9009;
        if (volume) ::glGetTexLevelParameteriv(texture_target, level, 0x8071, &depth); // GL_TEXTURE_DEPTH
        for (const auto parameter : {GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS}) {
            GLint value = 0;
            ::glGetIntegerv(parameter, &value);
            if (value != 0) throw std::runtime_error("unsupported OpenGL texture readback packing");
        }
        if (volume) for (const auto parameter : {0x806c, 0x806b}) { // GL_PACK_IMAGE_HEIGHT, GL_PACK_SKIP_IMAGES
            GLint value = 0;
            ::glGetIntegerv(parameter, &value);
            if (value != 0) throw std::runtime_error("unsupported OpenGL volume readback packing");
        }
        ::glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        if (width < 0 || height < 0 || depth < 0 ||
            (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8))
            throw std::runtime_error("invalid OpenGL texture readback extent");
        const auto row_bytes = static_cast<std::uint64_t>(width) * pixel_bytes(format, type);
        const auto stride = (row_bytes + alignment - 1) & ~static_cast<std::uint64_t>(alignment - 1);
        constexpr std::uint64_t limit = 256ULL * 1024 * 1024;
        const auto rows = static_cast<std::uint64_t>(height) * static_cast<std::uint64_t>(depth);
        if (stride > limit || (stride != 0 && rows > limit / stride))
            throw std::runtime_error("OpenGL texture readback exceeds transfer limit");
        const auto size = rows == 0 ? 0 : stride * (rows - 1) + row_bytes;
        require_range(memory, cpu.x[4], size, true);
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size));
        native(texture_target, level, format, type, pixels.data());
        if (row_bytes == 0) return;
        for (std::uint64_t row = 0; row < rows; ++row)
            memory.write_bytes(cpu.x[4] + row * stride,
                std::span<const std::uint8_t>(pixels.data() + row * stride, static_cast<std::size_t>(row_bytes)));
    };
}

template<class Value, bool Clear>
GuestCallHandler parameter_vector(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(GLenum, GLint, const Value*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        const auto parameter = static_cast<GLenum>(cpu.x[Clear ? 0 : 1]);
        const auto count = Clear ? (parameter == GL_COLOR ? 4U : 1U)
            : (parameter == GL_TEXTURE_BORDER_COLOR || parameter == 0x8e46 ? 4U : 1U);
        require_range(memory, cpu.x[2], count * sizeof(Value), false);
        std::array<Value, 4> values{};
        memory.read_bytes(cpu.x[2], std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(values.data()), count * sizeof(Value)));
        native(static_cast<GLenum>(cpu.x[0]), static_cast<GLint>(cpu.x[1]), values.data());
    };
}

template<bool Bind>
GuestCallHandler object_array(std::uintptr_t target) {
    return [target](CpuState& cpu, GuestMemory& memory) {
        const auto count = static_cast<GLsizei>(cpu.x[Bind ? 1 : 0]);
        const auto pointer = cpu.x[Bind ? 2 : 1];
        if (count < 0 || count > 65536) throw std::runtime_error("invalid OpenGL object array count");
        const auto bytes = static_cast<std::size_t>(count) * sizeof(GLuint);
        std::vector<GLuint> values;
        const auto read_values = [&] {
            require_range(memory, pointer, bytes, false);
            values.resize(static_cast<std::size_t>(count));
            memory.read_bytes(pointer, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(values.data()), bytes));
        };
        if constexpr (Bind) { if (pointer != 0) read_values(); }
        else read_values();
        if constexpr (Bind) reinterpret_cast<void(APIENTRY*)(GLuint, GLsizei, const GLuint*)>(target)(static_cast<GLuint>(cpu.x[0]), count, values.empty() ? nullptr : values.data());
        else reinterpret_cast<void(APIENTRY*)(GLsizei, const GLenum*)>(target)(count, values.data());
    };
}

template<class Value, class... Parameter>
GuestCallHandler fixed_query(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(Parameter..., Value*)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        const auto parameter = cpu.x[sizeof...(Parameter) - 1];
        const auto count = parameter == GL_TEXTURE_BORDER_COLOR || parameter == 0x8e46 ? 4U : 1U;
        const auto pointer = cpu.x[sizeof...(Parameter)];
        require_range(memory, pointer, count * sizeof(Value), true);
        Arguments reader{cpu, memory};
        const std::tuple<Parameter...> arguments{reader.next<Parameter>()...};
        std::array<Value, 4> values{};
        std::apply([&](Parameter... args) { native(args..., values.data()); }, arguments);
        memory.write_bytes(pointer, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(values.data()), count * sizeof(Value)));
    };
}

template<bool Output>
GuestCallHandler program_binary(std::uintptr_t target) {
    return [target](CpuState& cpu, GuestMemory& memory) {
        const auto count = static_cast<GLsizei>(cpu.x[Output ? 1 : 3]);
        if (count < 0 || count > 256 * 1024 * 1024) throw std::runtime_error("invalid OpenGL program binary extent");
        const auto pointer = cpu.x[Output ? 4 : 2];
        require_range(memory, pointer, static_cast<std::uint64_t>(count), Output);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
        if constexpr (Output) {
            if (cpu.x[2] != 0) require_range(memory, cpu.x[2], 4, true);
            require_range(memory, cpu.x[3], 4, true);
            GLsizei written = 0;
            GLenum format = 0;
            reinterpret_cast<void(APIENTRY*)(GLuint, GLsizei, GLsizei*, GLenum*, void*)>(target)(
                static_cast<GLuint>(cpu.x[0]), count, &written, &format, bytes.data());
            if (written < 0 || written > count) throw std::runtime_error("invalid native OpenGL binary output length");
            if (cpu.x[2] != 0) memory.write32(cpu.x[2], static_cast<std::uint32_t>(written));
            memory.write32(cpu.x[3], format);
            memory.write_bytes(pointer, std::span<const std::uint8_t>(bytes.data(), static_cast<std::size_t>(written)));
        } else {
            memory.read_bytes(pointer, bytes);
            reinterpret_cast<void(APIENTRY*)(GLuint, GLenum, const void*, GLsizei)>(target)(
                static_cast<GLuint>(cpu.x[0]), static_cast<GLenum>(cpu.x[1]), bytes.data(), count);
        }
    };
}

template<unsigned Variant>
GuestCallHandler draw_elements(std::uintptr_t target) {
    return [target](CpuState& cpu, GuestMemory&) {
        GLint buffer = 0;
        ::glGetIntegerv(0x8895, &buffer); // GL_ELEMENT_ARRAY_BUFFER_BINDING
        if (buffer == 0) throw std::runtime_error("OpenGL client index arrays are unsupported");
        const auto mode = static_cast<GLenum>(cpu.x[0]);
        const auto count = static_cast<GLsizei>(cpu.x[1]);
        const auto type = static_cast<GLenum>(cpu.x[2]);
        const auto offset = reinterpret_cast<const void*>(cpu.x[3]);
        if constexpr (Variant == 0) reinterpret_cast<void(APIENTRY*)(GLenum, GLsizei, GLenum, const void*)>(target)(mode, count, type, offset);
        else if constexpr (Variant == 1) reinterpret_cast<void(APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLint)>(target)(mode, count, type, offset, static_cast<GLint>(cpu.x[4]));
        else if constexpr (Variant == 2) reinterpret_cast<void(APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLsizei)>(target)(mode, count, type, offset, static_cast<GLsizei>(cpu.x[4]));
        else if constexpr (Variant == 3) reinterpret_cast<void(APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint)>(target)(mode, count, type, offset, static_cast<GLsizei>(cpu.x[4]), static_cast<GLint>(cpu.x[5]));
        else if constexpr (Variant == 4) reinterpret_cast<void(APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLuint)>(target)(mode, count, type, offset, static_cast<GLsizei>(cpu.x[4]), static_cast<GLuint>(cpu.x[5]));
        else reinterpret_cast<void(APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint, GLuint)>(target)(mode, count, type, offset, static_cast<GLsizei>(cpu.x[4]), static_cast<GLint>(cpu.x[5]), static_cast<GLuint>(cpu.x[6]));
    };
}

template<bool Subrange>
GuestCallHandler buffer_clear(std::uintptr_t target) {
    return [target](CpuState& cpu, GuestMemory& memory) {
        constexpr auto format_slot = Subrange ? 4U : 2U;
        const auto format = static_cast<GLenum>(cpu.x[format_slot]);
        const auto type = static_cast<GLenum>(cpu.x[format_slot + 1]);
        const auto pointer = cpu.x[format_slot + 2];
        std::array<std::uint8_t, 32> value{};
        if (pointer != 0) {
            const auto bytes = pixel_bytes(format, type);
            require_range(memory, pointer, bytes, false);
            memory.read_bytes(pointer, std::span<std::uint8_t>(value.data(), bytes));
        }
        if constexpr (Subrange) reinterpret_cast<void(APIENTRY*)(GLuint, GLenum, std::intptr_t, std::intptr_t, GLenum, GLenum, const void*)>(target)(
            static_cast<GLuint>(cpu.x[0]), static_cast<GLenum>(cpu.x[1]), static_cast<std::intptr_t>(cpu.x[2]),
            static_cast<std::intptr_t>(cpu.x[3]), format, type, pointer == 0 ? nullptr : value.data());
        else reinterpret_cast<void(APIENTRY*)(GLuint, GLenum, GLenum, GLenum, const void*)>(target)(
            static_cast<GLuint>(cpu.x[0]), static_cast<GLenum>(cpu.x[1]), format, type, pointer == 0 ? nullptr : value.data());
    };
}

template<bool Indexed, bool Multiple>
GuestCallHandler draw_indirect(std::uintptr_t target) {
    return [target](CpuState& cpu, GuestMemory&) {
        GLint buffer = 0;
        ::glGetIntegerv(0x8f43, &buffer); // GL_DRAW_INDIRECT_BUFFER_BINDING
        if (buffer == 0) throw std::runtime_error("OpenGL client indirect draw commands are unsupported");
        if constexpr (Indexed) {
            ::glGetIntegerv(0x8895, &buffer);
            if (buffer == 0) throw std::runtime_error("OpenGL client index arrays are unsupported");
        }
        const auto mode = static_cast<GLenum>(cpu.x[0]);
        if constexpr (Indexed && Multiple) reinterpret_cast<void(APIENTRY*)(GLenum, GLenum, const void*, GLsizei, GLsizei)>(target)(
            mode, static_cast<GLenum>(cpu.x[1]), reinterpret_cast<const void*>(cpu.x[2]), static_cast<GLsizei>(cpu.x[3]), static_cast<GLsizei>(cpu.x[4]));
        else if constexpr (Indexed) reinterpret_cast<void(APIENTRY*)(GLenum, GLenum, const void*)>(target)(
            mode, static_cast<GLenum>(cpu.x[1]), reinterpret_cast<const void*>(cpu.x[2]));
        else if constexpr (Multiple) reinterpret_cast<void(APIENTRY*)(GLenum, const void*, GLsizei, GLsizei)>(target)(
            mode, reinterpret_cast<const void*>(cpu.x[1]), static_cast<GLsizei>(cpu.x[2]), static_cast<GLsizei>(cpu.x[3]));
        else reinterpret_cast<void(APIENTRY*)(GLenum, const void*)>(target)(mode, reinterpret_cast<const void*>(cpu.x[1]));
    };
}

template<class Value>
GuestCallHandler query_object(std::uintptr_t target) {
    const auto native = reinterpret_cast<void(APIENTRY*)(GLuint, GLenum, Value*)>(target);
    const auto buffer_query = reinterpret_cast<std::uintptr_t>(::wglGetProcAddress("glGetQueryBufferObjectuiv"));
    const bool query_buffers = buffer_query > 3 && buffer_query != UINTPTR_MAX;
    return [native, query_buffers](CpuState& cpu, GuestMemory& memory) {
        GLint buffer = 0;
        if (query_buffers) ::glGetIntegerv(0x9193, &buffer); // GL_QUERY_BUFFER_BINDING
        if (buffer != 0) {
            native(static_cast<GLuint>(cpu.x[0]), static_cast<GLenum>(cpu.x[1]), reinterpret_cast<Value*>(cpu.x[2]));
            return;
        }
        require_range(memory, cpu.x[2], sizeof(Value), true);
        Value value{};
        native(static_cast<GLuint>(cpu.x[0]), static_cast<GLenum>(cpu.x[1]), &value);
        memory.write_bytes(cpu.x[2], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)));
    };
}

inline GuestCallHandler resolve_memory(std::string_view name, std::uintptr_t target) {
    if (name == "glGetQueryObjectiv") return query_object<GLint>(target);
    if (name == "glGetQueryObjectuiv") return query_object<GLuint>(target);
    if (name == "glGetQueryObjecti64v") return query_object<std::int64_t>(target);
    if (name == "glGetQueryObjectui64v") return query_object<std::uint64_t>(target);
    if (name == "glViewportArrayv") return uniform_vector<GLfloat, 4, GLuint>(target);
    if (name == "glClearNamedBufferData" || name == "glClearBufferData") return buffer_clear<false>(target);
    if (name == "glClearNamedBufferSubData" || name == "glClearBufferSubData") return buffer_clear<true>(target);
    if (name == "glDrawArraysIndirect") return draw_indirect<false, false>(target);
    if (name == "glDrawElementsIndirect") return draw_indirect<true, false>(target);
    if (name == "glMultiDrawArraysIndirect") return draw_indirect<false, true>(target);
    if (name == "glMultiDrawElementsIndirect") return draw_indirect<true, true>(target);
    if (name == "glReadPixels") return [native = reinterpret_cast<ReadPixels>(target)](CpuState& cpu, GuestMemory& memory) {
        read_pixels(cpu, memory, native);
    };
    if (name == "glProgramBinary") return program_binary<false>(target);
    if (name == "glGetProgramBinary") return program_binary<true>(target);
    if (name == "glGetTexImage") return texture_read(target);
    if (name == "glClearBufferfv") return parameter_vector<GLfloat, true>(target);
    if (name == "glClearBufferiv") return parameter_vector<GLint, true>(target);
    if (name == "glClearBufferuiv") return parameter_vector<GLuint, true>(target);
    if (name == "glTexParameterfv" || name == "glSamplerParameterfv" || name == "glTextureParameterfv") return parameter_vector<GLfloat, false>(target);
    if (name == "glTexParameteriv" || name == "glSamplerParameteriv" || name == "glTextureParameteriv") return parameter_vector<GLint, false>(target);
    if (name == "glBindTextures" || name == "glBindSamplers" || name == "glBindImageTextures") return object_array<true>(target);
    if (name == "glDrawBuffers") return object_array<false>(target);
    if (name == "glGetTexLevelParameteriv") return fixed_query<GLint, GLenum, GLint, GLenum>(target);
    if (name == "glGetTexLevelParameterfv") return fixed_query<GLfloat, GLenum, GLint, GLenum>(target);
    if (name == "glGetTexParameteriv" || name == "glGetBufferParameteriv" || name == "glGetRenderbufferParameteriv") return fixed_query<GLint, GLenum, GLenum>(target);
    if (name == "glGetNamedBufferParameteriv") return fixed_query<GLint, GLuint, GLenum>(target);
    if (name == "glGetTexParameterfv") return fixed_query<GLfloat, GLenum, GLenum>(target);
    if (name == "glGetBufferParameteri64v" || name == "glGetNamedBufferParameteri64v") return fixed_query<std::int64_t, GLenum, GLenum>(target);
    if (name == "glGetSamplerParameteriv") return fixed_query<GLint, GLuint, GLenum>(target);
    if (name == "glGetSamplerParameterfv") return fixed_query<GLfloat, GLuint, GLenum>(target);
    if (name == "glGetProgramInterfaceiv") return fixed_query<GLint, GLuint, GLenum, GLenum>(target);
    if (name == "glGetFramebufferAttachmentParameteriv") return fixed_query<GLint, GLenum, GLenum, GLenum>(target);
    if (name == "glVertexAttribPointer") return vertex_pointer<false>(target);
    if (name == "glVertexAttribIPointer" || name == "glVertexAttribLPointer") return vertex_pointer<true>(target);
    if (name == "glDrawElements") return draw_elements<0>(target);
    if (name == "glDrawElementsBaseVertex") return draw_elements<1>(target);
    if (name == "glDrawElementsInstanced") return draw_elements<2>(target);
    if (name == "glDrawElementsInstancedBaseVertex") return draw_elements<3>(target);
    if (name == "glDrawElementsInstancedBaseInstance") return draw_elements<4>(target);
    if (name == "glDrawElementsInstancedBaseVertexBaseInstance") return draw_elements<5>(target);
    if (name == "glTexImage1D") return texture_data<1, false>(target);
    if (name == "glTexImage2D") return texture_data<2, false>(target);
    if (name == "glTexImage3D") return texture_data<3, false>(target);
    if (name == "glTexSubImage1D" || name == "glTextureSubImage1D") return texture_data<1, true>(target);
    if (name == "glTexSubImage2D" || name == "glTextureSubImage2D") return texture_data<2, true>(target);
    if (name == "glTexSubImage3D" || name == "glTextureSubImage3D") return texture_data<3, true>(target);
    if (name == "glGetUnsignedBytei_vEXT") return uuid_query<true>(target);
    if (name == "glGetUnsignedBytevEXT") return uuid_query<false>(target);
    if (name == "glGetIntegeri_v") return state_query<GLint, true>(target);
    if (name == "glGetFloati_v") return state_query<GLfloat, true>(target);
    if (name == "glGetBooleani_v") return state_query<GLboolean, true>(target);
    if (name == "glGetInteger64i_v") return state_query<std::int64_t, true>(target);
    if (name == "glGetInteger64v") return state_query<std::int64_t, false>(target);
    if (name == "glGetDoublev") return state_query<GLdouble, false>(target);
    if (name == "glGetInternalformativ") return internal_format<GLint>(target);
    if (name == "glGetInternalformati64v") return internal_format<std::int64_t>(target);
    for (const auto prefix : {"Buffers", "Textures", "VertexArrays", "Framebuffers", "Renderbuffers", "Samplers", "Queries"}) {
        if (name == std::string("glGen") + prefix) return objects<true>(target);
        if (name == std::string("glDelete") + prefix) return objects<false>(target);
    }
    if (name == "glGetUniformLocation" || name == "glGetAttribLocation" || name == "glGetFragDataLocation") return named_location<GLint>(target);
    if (name == "glGetUniformBlockIndex") return named_location<GLuint>(target);
    if (name == "glBindAttribLocation" || name == "glBindFragDataLocation") return named_binding(target);
    if (name == "glGetShaderiv" || name == "glGetProgramiv") return single_query(target);
    if (name == "glGetShaderInfoLog" || name == "glGetProgramInfoLog") return info_log(target);
    if (name == "glShaderSource") return shader_source(target);
    if (name == "glBufferData" || name == "glBufferStorage" || name == "glNamedBufferData" || name == "glNamedBufferStorage") return buffer_data<false>(target);
    if (name == "glBufferSubData" || name == "glNamedBufferSubData") return buffer_data<true>(target);
#define UNIFORM(symbol, type, count) if (name == #symbol) return uniform_vector<type, count>(target)
    UNIFORM(glUniform1fv, GLfloat, 1); UNIFORM(glUniform2fv, GLfloat, 2);
    UNIFORM(glUniform3fv, GLfloat, 3); UNIFORM(glUniform4fv, GLfloat, 4);
    UNIFORM(glUniform1iv, GLint, 1); UNIFORM(glUniform2iv, GLint, 2);
    UNIFORM(glUniform3iv, GLint, 3); UNIFORM(glUniform4iv, GLint, 4);
    UNIFORM(glUniform1uiv, GLuint, 1); UNIFORM(glUniform2uiv, GLuint, 2);
    UNIFORM(glUniform3uiv, GLuint, 3); UNIFORM(glUniform4uiv, GLuint, 4);
#undef UNIFORM
    if (name == "glUniformMatrix2fv") return uniform_matrix<4>(target);
    if (name == "glUniformMatrix3fv") return uniform_matrix<9>(target);
    if (name == "glUniformMatrix4fv") return uniform_matrix<16>(target);
    if (name == "glUniformMatrix2x3fv" || name == "glUniformMatrix3x2fv") return uniform_matrix<6>(target);
    if (name == "glUniformMatrix2x4fv" || name == "glUniformMatrix4x2fv") return uniform_matrix<8>(target);
    if (name == "glUniformMatrix3x4fv" || name == "glUniformMatrix4x3fv") return uniform_matrix<12>(target);
    return {};
}

} // namespace transmute::gl_detail
