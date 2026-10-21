// Signature database lookup (owner: abi). The generated table comes from
// tools/sigdb/sigdb.py; the built-in table holds C++-only declarations the C
// header pass cannot see and corrections. Grammar: src/fx/abi/NOTES.md.

#include "fx/abi/abi_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string_view>

namespace transmute::fx::abi {
namespace {

struct Entry {
    const char* name;
    const char* sig;
};

constexpr Entry kGenerated[] = {
#include "fx/abi/generated/sigdb_table.inc"
};

// Sorted by name (strcmp order). Consulted before the generated table.
constexpr Entry kBuiltin[] = {
    {"_CxxThrowException", "v(pp)"},
    {"__CxxFrameHandler3", "i(pppp)"},
    {"__CxxFrameHandler4", "i(pppp)"},
    {"__current_exception", "p()"},
    {"__current_exception_context", "p()"},
    {"__std_exception_copy", "v(pp)"},
    {"__std_exception_destroy", "v(p)"},
    {"__std_terminate", "v()"},
    {"__std_type_info_compare", "i(pp)"},
    {"__std_type_info_name", "p(pp)"},
    {"__uncaught_exception", "C()"},
    {"__uncaught_exceptions", "i()"},
    {"_get_terminate", "@v()()"},
    {"_get_unexpected", "@v()()"},
    {"_purecall", "i()"},
    {"_query_new_handler", "@i(l)()"},
    {"_set_new_handler", "@i(l)(@i(l))"},
    {"_set_se_translator", "@v(Ip)(@v(Ip))"},
    {"set_terminate", "@v()(@v())"},
    {"set_unexpected", "@v()(@v())"},
};

// OpenGL/WGL entry points past GL 1.1. The SDK's gl.h stops there; everything
// else reaches the guest through wglGetProcAddress as an unnamed driver
// address, which would otherwise get the positional '?' signature and lose
// mixed integer/FP arguments (glSamplerParameterf, glClearBufferfi, ...).
// Sorted by name. GLenum/GLuint/GLbitfield = I, GLint/GLsizei = i,
// GLboolean = C, GLintptr/GLsizeiptr/GLuint64 = l, GLsync = p.
constexpr Entry kOpenGl[] = {
    {"glActiveTexture", "v(I)"},
    {"glAttachShader", "v(II)"},
    {"glBeginQuery", "v(II)"},
    {"glBindBuffer", "v(II)"},
    {"glBindBufferBase", "v(III)"},
    {"glBindBufferRange", "v(IIIll)"},
    {"glBindFramebuffer", "v(II)"},
    {"glBindImageTexture", "v(IIiCiII)"},
    {"glBindImageTextures", "v(Iip)"},
    {"glBindRenderbuffer", "v(II)"},
    {"glBindSampler", "v(II)"},
    {"glBindSamplers", "v(Iip)"},
    {"glBindTextures", "v(Iip)"},
    {"glBindVertexArray", "v(I)"},
    {"glBindVertexBuffer", "v(IIli)"},
    {"glBlendColor", "v(ffff)"},
    {"glBlendEquation", "v(I)"},
    {"glBlendEquationSeparate", "v(II)"},
    {"glBlendFuncSeparate", "v(IIII)"},
    {"glBlitFramebuffer", "v(iiiiiiiiII)"},
    {"glBufferData", "v(IlpI)"},
    {"glBufferStorage", "v(IlpI)"},
    {"glBufferSubData", "v(Illp)"},
    {"glCheckFramebufferStatus", "I(I)"},
    {"glClearBufferData", "v(IIIIp)"},
    {"glClearBufferfi", "v(Iifi)"},
    {"glClearBufferfv", "v(Iip)"},
    {"glClearBufferiv", "v(Iip)"},
    {"glClearBufferuiv", "v(Iip)"},
    {"glClearDepthf", "v(f)"},
    {"glClearNamedBufferData", "v(IIIIp)"},
    {"glClearNamedFramebufferfi", "v(IIifi)"},
    {"glClientWaitSync", "I(pIl)"},
    {"glClipControl", "v(II)"},
    {"glCompileShader", "v(I)"},
    {"glCompressedTexImage1D", "v(IiIiiip)"},
    {"glCompressedTexImage2D", "v(IiIiiiip)"},
    {"glCompressedTexImage3D", "v(IiIiiiiip)"},
    {"glCompressedTexSubImage1D", "v(IiiiIip)"},
    {"glCompressedTexSubImage2D", "v(IiiiiiIip)"},
    {"glCompressedTexSubImage3D", "v(IiiiiiiiIip)"},
    {"glCompressedTextureSubImage1D", "v(IiiiIip)"},
    {"glCompressedTextureSubImage2D", "v(IiiiiiIip)"},
    {"glCompressedTextureSubImage3D", "v(IiiiiiiiIip)"},
    {"glCopyBufferSubData", "v(IIlll)"},
    {"glCopyImageSubData", "v(IIiiiiIIiiiiiii)"},
    {"glCopyNamedBufferSubData", "v(IIlll)"},
    {"glCreateProgram", "I()"},
    {"glCreateShader", "I(I)"},
    {"glDebugMessageCallback", "v(@v(IIIIipp)p)"},
    {"glDebugMessageControl", "v(IIIipC)"},
    {"glDebugMessageInsert", "v(IIIIip)"},
    {"glDeleteBuffers", "v(ip)"},
    {"glDeleteFramebuffers", "v(ip)"},
    {"glDeleteProgram", "v(I)"},
    {"glDeleteQueries", "v(ip)"},
    {"glDeleteRenderbuffers", "v(ip)"},
    {"glDeleteSamplers", "v(ip)"},
    {"glDeleteShader", "v(I)"},
    {"glDeleteSync", "v(p)"},
    {"glDeleteVertexArrays", "v(ip)"},
    {"glDepthRangeIndexed", "v(Idd)"},
    {"glDepthRangef", "v(ff)"},
    {"glDisableVertexAttribArray", "v(I)"},
    {"glDispatchCompute", "v(III)"},
    {"glDispatchComputeIndirect", "v(l)"},
    {"glDrawArraysIndirect", "v(Ip)"},
    {"glDrawArraysInstanced", "v(Iiii)"},
    {"glDrawArraysInstancedBaseInstance", "v(IiiiI)"},
    {"glDrawBuffers", "v(ip)"},
    {"glDrawElementsBaseVertex", "v(IiIpi)"},
    {"glDrawElementsIndirect", "v(IIp)"},
    {"glDrawElementsInstanced", "v(IiIpi)"},
    {"glDrawElementsInstancedBaseVertex", "v(IiIpii)"},
    {"glDrawElementsInstancedBaseVertexBaseInstance", "v(IiIpiiI)"},
    {"glEnableVertexAttribArray", "v(I)"},
    {"glEndQuery", "v(I)"},
    {"glFenceSync", "p(II)"},
    {"glFlushMappedBufferRange", "v(Ill)"},
    {"glFramebufferFetchBarrierEXT", "v()"},
    {"glFramebufferParameteri", "v(IIi)"},
    {"glFramebufferRenderbuffer", "v(IIII)"},
    {"glFramebufferTexture", "v(IIIi)"},
    {"glFramebufferTexture2D", "v(IIIIi)"},
    {"glFramebufferTextureLayer", "v(IIIii)"},
    {"glGenBuffers", "v(ip)"},
    {"glGenFramebuffers", "v(ip)"},
    {"glGenQueries", "v(ip)"},
    {"glGenRenderbuffers", "v(ip)"},
    {"glGenSamplers", "v(ip)"},
    {"glGenVertexArrays", "v(ip)"},
    {"glGenerateMipmap", "v(I)"},
    {"glGenerateTextureMipmap", "v(I)"},
    {"glGetActiveAttrib", "v(IIipppp)"},
    {"glGetActiveUniformBlockName", "v(IIipp)"},
    {"glGetActiveUniformBlockiv", "v(IIIp)"},
    {"glGetActiveUniformName", "v(IIipp)"},
    {"glGetActiveUniformsiv", "v(IipIp)"},
    {"glGetAttribLocation", "i(Ip)"},
    {"glGetBufferParameteriv", "v(IIp)"},
    {"glGetInteger64v", "v(Ip)"},
    {"glGetIntegeri_v", "v(IIp)"},
    {"glGetProgramBinary", "v(Iippp)"},
    {"glGetProgramInfoLog", "v(Iipp)"},
    {"glGetProgramInterfaceiv", "v(IIIp)"},
    {"glGetProgramResourceName", "v(IIIipp)"},
    {"glGetProgramResourceiv", "v(IIIipipp)"},
    {"glGetProgramiv", "v(IIp)"},
    {"glGetQueryObjecti64v", "v(IIp)"},
    {"glGetQueryObjectiv", "v(IIp)"},
    {"glGetQueryObjectui64v", "v(IIp)"},
    {"glGetQueryObjectuiv", "v(IIp)"},
    {"glGetShaderInfoLog", "v(Iipp)"},
    {"glGetShaderiv", "v(IIp)"},
    {"glGetStringi", "p(II)"},
    {"glGetTextureImage", "v(IiIIip)"},
    {"glGetUniformBlockIndex", "I(Ip)"},
    {"glGetUniformLocation", "i(Ip)"},
    {"glGetUnsignedBytevEXT", "v(Ip)"},
    {"glIsBuffer", "C(I)"},
    {"glIsSync", "C(p)"},
    {"glLinkProgram", "v(I)"},
    {"glMapBuffer", "p(II)"},
    {"glMapBufferRange", "p(IllI)"},
    {"glMemoryBarrier", "v(I)"},
    {"glMinSampleShading", "v(f)"},
    {"glMultiDrawArraysIndirect", "v(Ipii)"},
    {"glMultiDrawElementsIndirect", "v(IIpii)"},
    {"glObjectLabel", "v(IIip)"},
    {"glPatchParameteri", "v(Ii)"},
    {"glPointParameterf", "v(If)"},
    {"glPointParameteri", "v(Ii)"},
    {"glPolygonOffsetClamp", "v(fff)"},
    {"glPopDebugGroup", "v()"},
    {"glPrimitiveRestartIndex", "v(I)"},
    {"glProgramBinary", "v(IIpi)"},
    {"glProgramParameteri", "v(IIi)"},
    {"glProgramUniform1f", "v(Iif)"},
    {"glProgramUniform2f", "v(Iiff)"},
    {"glProgramUniform3f", "v(Iifff)"},
    {"glProgramUniform4f", "v(Iiffff)"},
    {"glProvokingVertex", "v(I)"},
    {"glPushDebugGroup", "v(IIip)"},
    {"glQueryCounter", "v(II)"},
    {"glSamplerParameterf", "v(IIf)"},
    {"glSamplerParameterfv", "v(IIp)"},
    {"glSamplerParameteri", "v(IIi)"},
    {"glShaderSource", "v(Iipp)"},
    {"glStencilFuncSeparate", "v(IIiI)"},
    {"glStencilMaskSeparate", "v(II)"},
    {"glStencilOpSeparate", "v(IIII)"},
    {"glTexBuffer", "v(III)"},
    {"glTexImage3D", "v(IiiiiiiIIp)"},
    {"glTexStorage1D", "v(IiIi)"},
    {"glTexStorage2D", "v(IiIii)"},
    {"glTexStorage3D", "v(IiIiii)"},
    {"glTexSubImage3D", "v(IiiiiiiiIIp)"},
    {"glTextureBarrier", "v()"},
    {"glTextureBuffer", "v(III)"},
    {"glTextureParameterf", "v(IIf)"},
    {"glTextureParameteri", "v(IIi)"},
    {"glTextureParameteriv", "v(IIp)"},
    {"glTextureSubImage1D", "v(IiiiIIp)"},
    {"glTextureSubImage2D", "v(IiiiiiIIp)"},
    {"glTextureSubImage3D", "v(IiiiiiiiIIp)"},
    {"glTextureView", "v(IIIIIIII)"},
    {"glUniform1d", "v(id)"},
    {"glUniform1f", "v(if)"},
    {"glUniform1fv", "v(iip)"},
    {"glUniform1i", "v(ii)"},
    {"glUniform1iv", "v(iip)"},
    {"glUniform1ui", "v(iI)"},
    {"glUniform2d", "v(idd)"},
    {"glUniform2f", "v(iff)"},
    {"glUniform2fv", "v(iip)"},
    {"glUniform2i", "v(iii)"},
    {"glUniform2iv", "v(iip)"},
    {"glUniform3d", "v(iddd)"},
    {"glUniform3f", "v(ifff)"},
    {"glUniform3fv", "v(iip)"},
    {"glUniform3i", "v(iiii)"},
    {"glUniform3iv", "v(iip)"},
    {"glUniform4d", "v(idddd)"},
    {"glUniform4f", "v(iffff)"},
    {"glUniform4fv", "v(iip)"},
    {"glUniform4i", "v(iiiii)"},
    {"glUniform4iv", "v(iip)"},
    {"glUniformBlockBinding", "v(III)"},
    {"glUniformMatrix3fv", "v(iiCp)"},
    {"glUniformMatrix4fv", "v(iiCp)"},
    {"glUnmapBuffer", "C(I)"},
    {"glUnmapNamedBuffer", "C(I)"},
    {"glUseProgram", "v(I)"},
    {"glValidateProgram", "v(I)"},
    {"glVertexAttrib1f", "v(If)"},
    {"glVertexAttrib2f", "v(Iff)"},
    {"glVertexAttrib3f", "v(Ifff)"},
    {"glVertexAttrib4f", "v(Iffff)"},
    {"glVertexAttrib4fv", "v(Ip)"},
    {"glVertexAttribBinding", "v(II)"},
    {"glVertexAttribDivisor", "v(II)"},
    {"glVertexAttribFormat", "v(IiICI)"},
    {"glVertexAttribIFormat", "v(IiII)"},
    {"glVertexAttribIPointer", "v(IiIip)"},
    {"glVertexAttribL1d", "v(Id)"},
    {"glVertexAttribLFormat", "v(IiII)"},
    {"glVertexAttribPointer", "v(IiICip)"},
    {"glVertexBindingDivisor", "v(II)"},
    {"glViewportArrayv", "v(Iip)"},
    {"glViewportIndexedf", "v(Iffff)"},
    {"glWaitSync", "v(pIl)"},
    {"wglChoosePixelFormatARB", "i(pppIpp)"},
    {"wglCreateContextAttribsARB", "p(ppp)"},
    {"wglGetExtensionsStringARB", "p(p)"},
    {"wglGetExtensionsStringEXT", "p()"},
    {"wglGetPixelFormatAttribivARB", "i(piiIpp)"},
    {"wglGetSwapIntervalEXT", "i()"},
    {"wglSwapIntervalEXT", "i(i)"},
};

constexpr bool entry_less(const Entry& a, const Entry& b) {
    return std::string_view(a.name) < std::string_view(b.name);
}
static_assert(std::is_sorted(std::begin(kBuiltin), std::end(kBuiltin), entry_less), "kBuiltin must be sorted");
static_assert(std::is_sorted(std::begin(kOpenGl), std::end(kOpenGl), entry_less), "kOpenGl must be sorted");

const char* find(const Entry* begin, const Entry* end, std::string_view name, bool is_sorted) noexcept {
    if (!is_sorted) {
        for (const auto* it = begin; it != end; ++it)
            if (std::string_view(it->name) == name) return it->sig;
        return nullptr;
    }
    const auto* it = std::lower_bound(begin, end, name, [](const Entry& e, std::string_view key) {
        return std::string_view(e.name) < key;
    });
    if (it != end && std::string_view(it->name) == name) return it->sig;
    return nullptr;
}

} // namespace

const char* lookup_signature_text(std::string_view name) noexcept {
    // The generator sorts the table (too large for a constexpr check); if a
    // hand edit broke the order, say so once and fall back to a linear scan.
    static const bool generated_sorted = [] {
        const bool ok = std::is_sorted(std::begin(kGenerated), std::end(kGenerated), entry_less);
        if (!ok) std::fprintf(stderr, "fx abi: generated/sigdb_table.inc is not sorted; regenerate it\n");
        return ok;
    }();
    if (const char* sig = find(std::begin(kBuiltin), std::end(kBuiltin), name, true)) return sig;
    if (name.size() > 2 && (name[0] == 'g' || name[0] == 'w'))
        if (const char* sig = find(std::begin(kOpenGl), std::end(kOpenGl), name, true)) return sig;
    return find(std::begin(kGenerated), std::end(kGenerated), name, generated_sorted);
}

const Signature* lookup_signature(std::string_view name) {
    const char* text = lookup_signature_text(name);
    if (text == nullptr) return unknown_signature();
    const Signature* sig = intern_signature(text);
    return sig ? sig : unknown_signature();
}

std::size_t generated_signature_count() noexcept { return std::size(kGenerated); }
std::size_t opengl_signature_count() noexcept { return std::size(kOpenGl); }
const char* opengl_signature_name(std::size_t i) noexcept { return i < std::size(kOpenGl) ? kOpenGl[i].name : nullptr; }

} // namespace transmute::fx::abi
