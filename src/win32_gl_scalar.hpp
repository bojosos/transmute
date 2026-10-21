#pragma once

#include "transmute/a64.hpp"

#include <bit>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace transmute::gl_detail {

// Windows ARM64 allocates integer and FP arguments independently. The host
// compiler then supplies the x64 ABI, including its mixed-register positions.
struct Arguments {
    CpuState& cpu;
    GuestMemory& memory;
    unsigned integer{};
    unsigned floating{};
    unsigned stack{};
    template<class T> T next() {
        static_assert(!std::is_pointer_v<T>);
        if constexpr (std::is_floating_point_v<T>) {
            const auto bits = floating < 8 ? cpu.v[floating++][0]
                : memory.read64(cpu.sp + 8ULL * stack++);
            if constexpr (sizeof(T) == 4) return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
            else return std::bit_cast<double>(bits);
        } else {
            return static_cast<T>(integer < 8 ? cpu.x[integer++]
                : memory.read64(cpu.sp + 8ULL * stack++));
        }
    }
};

template<class Result, class... Parameter>
GuestCallHandler scalar(std::uintptr_t target) {
    static_assert(sizeof...(Parameter) <= 16);
    const auto native = reinterpret_cast<Result(APIENTRY*)(Parameter...)>(target);
    return [native](CpuState& cpu, GuestMemory& memory) {
        Arguments reader{cpu, memory};
        const std::tuple<Parameter...> values{reader.next<Parameter>()...};
        if constexpr (std::is_void_v<Result>) std::apply(native, values);
        else cpu.x[0] = static_cast<std::make_unsigned_t<Result>>(std::apply(native, values));
    };
}

inline GuestCallHandler resolve_scalar(std::string_view name, std::uintptr_t target) {
#define GL_CALL(symbol, ...) \
    if (name == #symbol) return scalar<__VA_ARGS__>(target)
    GL_CALL(glGetError, GLenum);
    GL_CALL(glFlush, void);
    GL_CALL(glFinish, void);
    GL_CALL(glClear, void, GLbitfield);
    GL_CALL(glClearColor, void, GLfloat, GLfloat, GLfloat, GLfloat);
    GL_CALL(glClearDepth, void, GLdouble);
    GL_CALL(glClearStencil, void, GLint);
    GL_CALL(glViewport, void, GLint, GLint, GLsizei, GLsizei);
    GL_CALL(glScissor, void, GLint, GLint, GLsizei, GLsizei);
    GL_CALL(glEnable, void, GLenum);
    GL_CALL(glDisable, void, GLenum);
    GL_CALL(glIsEnabled, GLboolean, GLenum);
    GL_CALL(glBlendFunc, void, GLenum, GLenum);
    GL_CALL(glBlendFuncSeparate, void, GLenum, GLenum, GLenum, GLenum);
    GL_CALL(glBlendEquation, void, GLenum);
    GL_CALL(glBlendEquationSeparate, void, GLenum, GLenum);
    GL_CALL(glBlendColor, void, GLfloat, GLfloat, GLfloat, GLfloat);
    GL_CALL(glDepthFunc, void, GLenum);
    GL_CALL(glDepthMask, void, GLboolean);
    GL_CALL(glDepthRange, void, GLdouble, GLdouble);
    GL_CALL(glColorMask, void, GLboolean, GLboolean, GLboolean, GLboolean);
    GL_CALL(glCullFace, void, GLenum);
    GL_CALL(glFrontFace, void, GLenum);
    GL_CALL(glPolygonMode, void, GLenum, GLenum);
    GL_CALL(glPolygonOffset, void, GLfloat, GLfloat);
    GL_CALL(glProvokingVertex, void, GLenum);
    GL_CALL(glLogicOp, void, GLenum);
    GL_CALL(glClipControl, void, GLenum, GLenum);
    GL_CALL(glLineWidth, void, GLfloat);
    GL_CALL(glPointSize, void, GLfloat);
    GL_CALL(glPixelStorei, void, GLenum, GLint);
    GL_CALL(glReadBuffer, void, GLenum);
    GL_CALL(glDrawBuffer, void, GLenum);
    GL_CALL(glActiveTexture, void, GLenum);
    GL_CALL(glBindTexture, void, GLenum, GLuint);
    GL_CALL(glTexParameteri, void, GLenum, GLenum, GLint);
    GL_CALL(glTexParameterf, void, GLenum, GLenum, GLfloat);
    GL_CALL(glTextureParameteri, void, GLuint, GLenum, GLint);
    GL_CALL(glTextureParameterf, void, GLuint, GLenum, GLfloat);
    GL_CALL(glTexBuffer, void, GLenum, GLenum, GLuint);
    GL_CALL(glTextureBuffer, void, GLuint, GLenum, GLuint);
    GL_CALL(glTextureView, void, GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint);
    GL_CALL(glGenerateMipmap, void, GLenum);
    GL_CALL(glCreateShader, GLuint, GLenum);
    GL_CALL(glCompileShader, void, GLuint);
    GL_CALL(glDeleteShader, void, GLuint);
    GL_CALL(glCreateProgram, GLuint);
    GL_CALL(glAttachShader, void, GLuint, GLuint);
    GL_CALL(glDetachShader, void, GLuint, GLuint);
    GL_CALL(glLinkProgram, void, GLuint);
    GL_CALL(glValidateProgram, void, GLuint);
    GL_CALL(glUseProgram, void, GLuint);
    GL_CALL(glDeleteProgram, void, GLuint);
    GL_CALL(glProgramParameteri, void, GLuint, GLenum, GLint);
    GL_CALL(glUniformBlockBinding, void, GLuint, GLuint, GLuint);
    GL_CALL(glShaderStorageBlockBinding, void, GLuint, GLuint, GLuint);
    GL_CALL(glIsBuffer, GLboolean, GLuint);
    GL_CALL(glIsTexture, GLboolean, GLuint);
    GL_CALL(glIsFramebuffer, GLboolean, GLuint);
    GL_CALL(glIsVertexArray, GLboolean, GLuint);
    GL_CALL(glClearBufferfi, void, GLenum, GLint, GLfloat, GLint);
    GL_CALL(glCopyBufferSubData, void, GLenum, GLenum, std::intptr_t, std::intptr_t, std::intptr_t);
    GL_CALL(glCopyNamedBufferSubData, void, GLuint, GLuint, std::intptr_t, std::intptr_t, std::intptr_t);
    GL_CALL(glFlushMappedBufferRange, void, GLenum, std::intptr_t, std::intptr_t);
    GL_CALL(glFlushMappedNamedBufferRange, void, GLuint, std::intptr_t, std::intptr_t);
    GL_CALL(glUniform1i, void, GLint, GLint);
    GL_CALL(glUniform2i, void, GLint, GLint, GLint);
    GL_CALL(glUniform3i, void, GLint, GLint, GLint, GLint);
    GL_CALL(glUniform4i, void, GLint, GLint, GLint, GLint, GLint);
    GL_CALL(glUniform1f, void, GLint, GLfloat);
    GL_CALL(glUniform2f, void, GLint, GLfloat, GLfloat);
    GL_CALL(glUniform3f, void, GLint, GLfloat, GLfloat, GLfloat);
    GL_CALL(glUniform4f, void, GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    GL_CALL(glBindBuffer, void, GLenum, GLuint);
    GL_CALL(glBindBufferBase, void, GLenum, GLuint, GLuint);
    GL_CALL(glBindBufferRange, void, GLenum, GLuint, GLuint, std::intptr_t, std::intptr_t);
    GL_CALL(glBindVertexArray, void, GLuint);
    GL_CALL(glEnableVertexAttribArray, void, GLuint);
    GL_CALL(glDisableVertexAttribArray, void, GLuint);
    GL_CALL(glVertexAttribDivisor, void, GLuint, GLuint);
    GL_CALL(glBindVertexBuffer, void, GLuint, GLuint, std::intptr_t, GLsizei);
    GL_CALL(glVertexAttribBinding, void, GLuint, GLuint);
    GL_CALL(glVertexAttribFormat, void, GLuint, GLint, GLenum, GLboolean, GLuint);
    GL_CALL(glVertexAttribIFormat, void, GLuint, GLint, GLenum, GLuint);
    GL_CALL(glVertexAttribLFormat, void, GLuint, GLint, GLenum, GLuint);
    GL_CALL(glVertexAttrib4f, void, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
    GL_CALL(glDrawArrays, void, GLenum, GLint, GLsizei);
    GL_CALL(glDrawArraysInstanced, void, GLenum, GLint, GLsizei, GLsizei);
    GL_CALL(glDrawArraysInstancedBaseInstance, void, GLenum, GLint, GLsizei, GLsizei, GLuint);
    GL_CALL(glBindFramebuffer, void, GLenum, GLuint);
    GL_CALL(glCheckFramebufferStatus, GLenum, GLenum);
    GL_CALL(glFramebufferTexture, void, GLenum, GLenum, GLuint, GLint);
    GL_CALL(glFramebufferTexture2D, void, GLenum, GLenum, GLenum, GLuint, GLint);
    GL_CALL(glFramebufferTextureLayer, void, GLenum, GLenum, GLuint, GLint, GLint);
    GL_CALL(glFramebufferParameteri, void, GLenum, GLenum, GLint);
    GL_CALL(glFramebufferRenderbuffer, void, GLenum, GLenum, GLenum, GLuint);
    GL_CALL(glBindRenderbuffer, void, GLenum, GLuint);
    GL_CALL(glRenderbufferStorage, void, GLenum, GLenum, GLsizei, GLsizei);
    GL_CALL(glRenderbufferStorageMultisample, void, GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    GL_CALL(glBlitFramebuffer, void, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    GL_CALL(glBindSampler, void, GLuint, GLuint);
    GL_CALL(glSamplerParameteri, void, GLuint, GLenum, GLint);
    GL_CALL(glSamplerParameterf, void, GLuint, GLenum, GLfloat);
    GL_CALL(glStencilFunc, void, GLenum, GLint, GLuint);
    GL_CALL(glStencilMask, void, GLuint);
    GL_CALL(glStencilOp, void, GLenum, GLenum, GLenum);
    GL_CALL(glStencilFuncSeparate, void, GLenum, GLenum, GLint, GLuint);
    GL_CALL(glStencilMaskSeparate, void, GLenum, GLuint);
    GL_CALL(glStencilOpSeparate, void, GLenum, GLenum, GLenum, GLenum);
    GL_CALL(glBeginQuery, void, GLenum, GLuint);
    GL_CALL(glEndQuery, void, GLenum);
    GL_CALL(glQueryCounter, void, GLuint, GLenum);
    GL_CALL(glMemoryBarrier, void, GLbitfield);
    GL_CALL(glDispatchCompute, void, GLuint, GLuint, GLuint);
    GL_CALL(glDispatchComputeIndirect, void, std::intptr_t);
    GL_CALL(glCopyImageSubData, void, GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei);
    GL_CALL(glGenerateTextureMipmap, void, GLuint);
    GL_CALL(glTextureBarrier, void);
    GL_CALL(glFramebufferFetchBarrierEXT, void);
    GL_CALL(glPopDebugGroup, void);
    GL_CALL(glBindImageTexture, void, GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
    GL_CALL(glTexStorage2D, void, GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    GL_CALL(glTexStorage1D, void, GLenum, GLsizei, GLenum, GLsizei);
    GL_CALL(glTexStorage3D, void, GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);
    GL_CALL(glTexStorage2DMultisample, void, GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean);
    GL_CALL(glEnablei, void, GLenum, GLuint);
    GL_CALL(glDisablei, void, GLenum, GLuint);
    GL_CALL(glIsEnabledi, GLboolean, GLenum, GLuint);
    GL_CALL(glPrimitiveRestartIndex, void, GLuint);
    GL_CALL(glPatchParameteri, void, GLenum, GLint);
    GL_CALL(wglSwapIntervalEXT, BOOL, int);
    GL_CALL(wglGetSwapIntervalEXT, int);
#undef GL_CALL
    return {};
}

} // namespace transmute::gl_detail
