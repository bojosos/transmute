// Emits "<intel syntax>|<hex bytes>" lines for tools/fx_asm_check.py, which
// disassembles the bytes with llvm-objdump and compares the text.
#include "fx/common/x64_assembler.hpp"

#include <cstdio>
#include <functional>
#include <string>

using namespace transmute::fx::x64;

namespace {
void emit(const char* text, const std::function<void(Assembler&)>& body) {
    Assembler a;
    body(a);
    a.finalize();
    std::printf("%s|", text);
    for (auto b : a.bytes()) std::printf("%02x", b);
    std::printf("\n");
}
} // namespace

int main() {
    using G = Gp;
    emit("mov rax, rbx", [](Assembler& a) { a.mov(64, G::rax, G::rbx); });
    emit("mov r12d, r13d", [](Assembler& a) { a.mov(32, G::r12, G::r13); });
    emit("mov qword ptr [r15 - 0x80], rax", [](Assembler& a) { a.mov(64, ptr(G::r15, -128), G::rax); });
    emit("mov rcx, qword ptr [r15 + 0x78]", [](Assembler& a) { a.mov(64, G::rcx, ptr(G::r15, 120)); });
    emit("mov rax, qword ptr [r12]", [](Assembler& a) { a.mov(64, G::rax, ptr(G::r12)); });
    emit("mov rax, qword ptr [r13]", [](Assembler& a) { a.mov(64, G::rax, ptr(G::r13)); });
    emit("mov rax, qword ptr [rsp + 0x10]", [](Assembler& a) { a.mov(64, G::rax, ptr(G::rsp, 16)); });
    emit("mov rax, qword ptr [rbp + 0x1000]", [](Assembler& a) { a.mov(64, G::rax, ptr(G::rbp, 0x1000)); });
    emit("mov eax, dword ptr [rbx + 4*r9 + 0x8]", [](Assembler& a) { a.mov(32, G::rax, ptr(G::rbx, G::r9, 4, 8)); });
    emit("mov word ptr [rdi], si", [](Assembler& a) { a.mov(16, ptr(G::rdi), G::rsi); });
    emit("mov byte ptr [rax], sil", [](Assembler& a) { a.mov(8, ptr(G::rax), G::rsi); });
    emit("mov byte ptr [r8], r9b", [](Assembler& a) { a.mov(8, ptr(G::r8), G::r9); });
    emit("mov qword ptr [rax], -0x1", [](Assembler& a) { a.mov(64, ptr(G::rax), -1); });
    emit("mov dword ptr [r15 + 0x100], 0x7", [](Assembler& a) { a.mov(32, ptr(G::r15, 0x100), 7); });
    emit("mov eax, 0x12345678", [](Assembler& a) { a.mov_imm(G::rax, 0x12345678); });
    emit("mov r9d, 0xffffffff", [](Assembler& a) { a.mov_imm(G::r9, 0xffffffffULL); });
    emit("mov rax, -0x2", [](Assembler& a) { a.mov_imm(G::rax, 0xfffffffffffffffeULL); });
    emit("movabs r10, 0x123456789abcdef0", [](Assembler& a) { a.mov_imm(G::r10, 0x123456789abcdef0ULL); });
    emit("movzx eax, byte ptr [rcx]", [](Assembler& a) { a.movzx(32, G::rax, ptr(G::rcx), 8); });
    emit("movzx eax, sil", [](Assembler& a) { a.movzx(32, G::rax, G::rsi, 8); });
    emit("movzx r8d, word ptr [r9 + 0x2]", [](Assembler& a) { a.movzx(32, G::r8, ptr(G::r9, 2), 16); });
    emit("movsx rax, byte ptr [rdx]", [](Assembler& a) { a.movsx(64, G::rax, ptr(G::rdx), 8); });
    emit("movsx eax, cx", [](Assembler& a) { a.movsx(32, G::rax, G::rcx, 16); });
    emit("movsxd r11, dword ptr [rax]", [](Assembler& a) { a.movsx(64, G::r11, ptr(G::rax), 32); });
    emit("movsxd rax, ecx", [](Assembler& a) { a.movsx(64, G::rax, G::rcx, 32); });
    emit("lea rax, [rbx + rcx]", [](Assembler& a) { a.lea(64, G::rax, ptr(G::rbx, G::rcx, 1)); });
    emit("lea r14d, [r13 + 8*rax - 0x4]", [](Assembler& a) { a.lea(32, G::r14, ptr(G::r13, G::rax, 8, -4)); });
    emit("add rax, rbx", [](Assembler& a) { a.add(64, G::rax, G::rbx); });
    emit("sub r8d, r9d", [](Assembler& a) { a.sub(32, G::r8, G::r9); });
    emit("and rcx, 0x7f", [](Assembler& a) { a.and_(64, G::rcx, 0x7f); });
    emit("or edx, 0x1000", [](Assembler& a) { a.or_(32, G::rdx, 0x1000); });
    emit("xor r15, r15", [](Assembler& a) { a.xor_(64, G::r15, G::r15); });
    emit("cmp rax, qword ptr [r15 - 0x78]", [](Assembler& a) { a.cmp(64, G::rax, ptr(G::r15, -120)); });
    emit("cmp qword ptr [rax + 0x8], 0x0", [](Assembler& a) { a.cmp(64, ptr(G::rax, 8), 0); });
    emit("add qword ptr [rcx], rdx", [](Assembler& a) { a.alu(Alu::add, 64, ptr(G::rcx), G::rdx); });
    emit("adc eax, ecx", [](Assembler& a) { a.adc(32, G::rax, G::rcx); });
    emit("sbb rax, rcx", [](Assembler& a) { a.sbb(64, G::rax, G::rcx); });
    emit("cmp sil, 0x5", [](Assembler& a) { a.cmp(8, G::rsi, 5); });
    emit("test rax, rcx", [](Assembler& a) { a.test(64, G::rax, G::rcx); });
    emit("test r9d, 0x100", [](Assembler& a) { a.test(32, G::r9, 0x100); });
    emit("test byte ptr [rax + 0x3], 0x1", [](Assembler& a) { a.test(8, ptr(G::rax, 3), 1); });
    emit("not rax", [](Assembler& a) { a.not_(64, G::rax); });
    emit("neg r10d", [](Assembler& a) { a.neg(32, G::r10); });
    emit("mul rcx", [](Assembler& a) { a.mul(64, G::rcx); });
    emit("imul r11", [](Assembler& a) { a.imul1(64, G::r11); });
    emit("div r8", [](Assembler& a) { a.div(64, G::r8); });
    emit("idiv ecx", [](Assembler& a) { a.idiv(32, G::rcx); });
    emit("imul rax, rbx", [](Assembler& a) { a.imul(64, G::rax, G::rbx); });
    emit("imul r9d, r10d, 0x64", [](Assembler& a) { a.imul(32, G::r9, G::r10, 100); });
    emit("imul rax, rcx, 0x186a0", [](Assembler& a) { a.imul(64, G::rax, G::rcx, 100000); });
    emit("cqo", [](Assembler& a) { a.cqo(); });
    emit("shl rax, 0x3", [](Assembler& a) { a.shl(64, G::rax, 3); });
    emit("shr r9d, 0x1f", [](Assembler& a) { a.shr(32, G::r9, 31); });
    emit("sar rcx, cl", [](Assembler& a) { a.shift_cl(Shift::sar, 64, G::rcx); });
    emit("ror eax, 0x8", [](Assembler& a) { a.ror(32, G::rax, 8); });
    emit("shl rdx", [](Assembler& a) { a.shl(64, G::rdx, 1); });
    emit("shld rax, rcx, 0x10", [](Assembler& a) { a.shld(64, G::rax, G::rcx, 16); });
    emit("shrd r8, r9, 0x4", [](Assembler& a) { a.shrd(64, G::r8, G::r9, 4); });
    emit("bswap rax", [](Assembler& a) { a.bswap(64, G::rax); });
    emit("bswap r12d", [](Assembler& a) { a.bswap(32, G::r12); });
    emit("bt rax, 0x3f", [](Assembler& a) { a.bt(64, G::rax, 63); });
    emit("bts ecx, 0x2", [](Assembler& a) { a.bts(32, G::rcx, 2); });
    emit("sete al", [](Assembler& a) { a.setcc(Cond::e, G::rax); });
    emit("setb dil", [](Assembler& a) { a.setcc(Cond::b, G::rdi); });
    emit("setg r10b", [](Assembler& a) { a.setcc(Cond::g, G::r10); });
    emit("cmovne rax, rcx", [](Assembler& a) { a.cmov(Cond::ne, 64, G::rax, G::rcx); });
    emit("cmovl r8d, dword ptr [rax]", [](Assembler& a) { a.cmov(Cond::l, 32, G::r8, ptr(G::rax)); });
    emit("xchg qword ptr [rcx], rax", [](Assembler& a) { a.xchg(64, ptr(G::rcx), G::rax); });
    emit("lock cmpxchg qword ptr [rdx], rcx", [](Assembler& a) { a.lock(); a.cmpxchg(64, ptr(G::rdx), G::rcx); });
    emit("lock xadd dword ptr [r8], r9d", [](Assembler& a) { a.lock(); a.xadd(32, ptr(G::r8), G::r9); });
    emit("lock cmpxchg16b xmmword ptr [r10]", [](Assembler& a) { a.lock(); a.cmpxchg16b(ptr(G::r10)); });
    emit("lock cmpxchg byte ptr [rax], sil", [](Assembler& a) { a.lock(); a.cmpxchg(8, ptr(G::rax), G::rsi); });
    emit("jmp rax", [](Assembler& a) { a.jmp(G::rax); });
    emit("jmp qword ptr [r15 + 0x200]", [](Assembler& a) { a.jmp(ptr(G::r15, 0x200)); });
    emit("call qword ptr [r15 + 0x400]", [](Assembler& a) { a.call(ptr(G::r15, 0x400)); });
    emit("call r11", [](Assembler& a) { a.call(G::r11); });
    emit("push r12", [](Assembler& a) { a.push(G::r12); });
    emit("pop rbx", [](Assembler& a) { a.pop(G::rbx); });
    emit("mfence", [](Assembler& a) { a.mfence(); });
    emit("lahf", [](Assembler& a) { a.lahf(); });
    emit("lzcnt rax, rcx", [](Assembler& a) { a.lzcnt(64, G::rax, G::rcx); });
    emit("tzcnt r8d, r9d", [](Assembler& a) { a.tzcnt(32, G::r8, G::r9); });
    emit("popcnt rax, r11", [](Assembler& a) { a.popcnt(64, G::rax, G::r11); });
    emit("crc32 eax, cl", [](Assembler& a) { a.crc32(G::rax, G::rcx, 8); });
    emit("crc32 rax, r9", [](Assembler& a) { a.crc32(G::rax, G::r9, 64); });
    emit("andn rax, rbx, rcx", [](Assembler& a) { a.andn(64, G::rax, G::rbx, G::rcx); });
    emit("bextr eax, ecx, edx", [](Assembler& a) { a.bextr(32, G::rax, G::rcx, G::rdx); });
    emit("bzhi r8, r9, r10", [](Assembler& a) { a.bzhi(64, G::r8, G::r9, G::r10); });
    emit("blsr rax, rcx", [](Assembler& a) { a.blsr(64, G::rax, G::rcx); });
    emit("shlx rax, rbx, rcx", [](Assembler& a) { a.shlx(64, G::rax, G::rbx, G::rcx); });
    emit("sarx r9d, r10d, r11d", [](Assembler& a) { a.sarx(32, G::r9, G::r10, G::r11); });
    emit("shrx r12, r13, r14", [](Assembler& a) { a.shrx(64, G::r12, G::r13, G::r14); });
    emit("rorx rax, rcx, 0xd", [](Assembler& a) { a.rorx(64, G::rax, G::rcx, 13); });
    emit("mulx r8, r9, rcx", [](Assembler& a) { a.mulx(64, G::r8, G::r9, G::rcx); });
    emit("pdep rax, rbx, rcx", [](Assembler& a) { a.pdep(64, G::rax, G::rbx, G::rcx); });
    emit("pext eax, ebx, ecx", [](Assembler& a) { a.pext(32, G::rax, G::rbx, G::rcx); });
    emit("movbe rax, qword ptr [rcx]", [](Assembler& a) { a.movbe(64, G::rax, ptr(G::rcx)); });
    emit("movbe dword ptr [r8], r9d", [](Assembler& a) { a.movbe(32, ptr(G::r8), G::r9); });
    emit("addps xmm0, xmm1", [](Assembler& a) { a.sse(sse::addps, Xmm::xmm0, Xmm::xmm1); });
    emit("addsd xmm8, xmm15", [](Assembler& a) { a.sse(sse::addsd, Xmm::xmm8, Xmm::xmm15); });
    emit("mulss xmm2, dword ptr [rax + 0x4]", [](Assembler& a) { a.sse(sse::mulss, Xmm::xmm2, ptr(G::rax, 4)); });
    emit("paddd xmm3, xmmword ptr [r15 + 0x200]", [](Assembler& a) { a.sse(sse::paddd, Xmm::xmm3, ptr(G::r15, 0x200)); });
    emit("pshufb xmm9, xmm10", [](Assembler& a) { a.sse(sse::pshufb, Xmm::xmm9, Xmm::xmm10); });
    emit("pmulld xmm1, xmm2", [](Assembler& a) { a.sse(sse::pmulld, Xmm::xmm1, Xmm::xmm2); });
    emit("shufps xmm0, xmm1, 0x1b", [](Assembler& a) { a.sse_imm(sse::shufps, Xmm::xmm0, Xmm::xmm1, 0x1b); });
    emit("pshufd xmm12, xmm13, 0x4e", [](Assembler& a) { a.sse_imm(sse::pshufd, Xmm::xmm12, Xmm::xmm13, 0x4e); });
    emit("palignr xmm1, xmm2, 0x8", [](Assembler& a) { a.sse_imm(sse::palignr, Xmm::xmm1, Xmm::xmm2, 8); });
    emit("roundsd xmm0, xmm1, 0xb", [](Assembler& a) { a.sse_imm(sse::roundsd, Xmm::xmm0, Xmm::xmm1, 0xb); });
    emit("psrld xmm5, 0x3", [](Assembler& a) { a.sse_shift_imm(0x72, 2, Xmm::xmm5, 3); });
    emit("pslldq xmm9, 0x4", [](Assembler& a) { a.sse_shift_imm(0x73, 7, Xmm::xmm9, 4); });
    emit("movd xmm0, eax", [](Assembler& a) { a.movd(Xmm::xmm0, G::rax); });
    emit("movq xmm10, r11", [](Assembler& a) { a.movq(Xmm::xmm10, G::r11); });
    emit("movq rax, xmm9", [](Assembler& a) { a.movq(G::rax, Xmm::xmm9); });
    emit("movd r8d, xmm1", [](Assembler& a) { a.movd(G::r8, Xmm::xmm1); });
    emit("movq xmm1, xmm2", [](Assembler& a) { a.movq(Xmm::xmm1, Xmm::xmm2); });
    emit("movq xmm3, qword ptr [rax]", [](Assembler& a) { a.movq(Xmm::xmm3, ptr(G::rax)); });
    emit("movq qword ptr [r15 + 0x200], xmm4", [](Assembler& a) { a.movq(ptr(G::r15, 0x200), Xmm::xmm4); });
    emit("movdqu xmm0, xmmword ptr [rcx]", [](Assembler& a) { a.movdqu(Xmm::xmm0, ptr(G::rcx)); });
    emit("movdqu xmmword ptr [r9 + 0x10], xmm11", [](Assembler& a) { a.movdqu(ptr(G::r9, 16), Xmm::xmm11); });
    emit("movdqa xmm7, xmmword ptr [r15 + 0xc0]", [](Assembler& a) { a.movdqa(Xmm::xmm7, ptr(G::r15, 0xc0)); });
    emit("movaps xmm1, xmm14", [](Assembler& a) { a.movaps(Xmm::xmm1, Xmm::xmm14); });
    emit("movss xmm0, dword ptr [rdx]", [](Assembler& a) { a.movss(Xmm::xmm0, ptr(G::rdx)); });
    emit("movsd qword ptr [rax], xmm8", [](Assembler& a) { a.movsd(ptr(G::rax), Xmm::xmm8); });
    emit("pinsrd xmm1, eax, 0x2", [](Assembler& a) { a.pinsr(32, Xmm::xmm1, G::rax, 2); });
    emit("pinsrq xmm9, r10, 0x1", [](Assembler& a) { a.pinsr(64, Xmm::xmm9, G::r10, 1); });
    emit("pinsrw xmm2, ecx, 0x7", [](Assembler& a) { a.pinsr(16, Xmm::xmm2, G::rcx, 7); });
    emit("pextrb eax, xmm3, 0xf", [](Assembler& a) { a.pextr(8, G::rax, Xmm::xmm3, 15); });
    emit("pextrq r11, xmm12, 0x1", [](Assembler& a) { a.pextr(64, G::r11, Xmm::xmm12, 1); });
    emit("pextrw edx, xmm1, 0x3", [](Assembler& a) { a.pextr(16, G::rdx, Xmm::xmm1, 3); });
    emit("cvtsi2sd xmm0, rax", [](Assembler& a) { a.cvtsi2sd(Xmm::xmm0, G::rax, 64); });
    emit("cvttss2si r9d, xmm2", [](Assembler& a) { a.cvttss2si(G::r9, Xmm::xmm2, 32); });
    emit("cvttsd2si rax, xmm15", [](Assembler& a) { a.cvttsd2si(G::rax, Xmm::xmm15, 64); });
    emit("movmskps eax, xmm1", [](Assembler& a) { a.movmskps(G::rax, Xmm::xmm1); });
    emit("pmovmskb r8d, xmm9", [](Assembler& a) { a.pmovmskb(G::r8, Xmm::xmm9); });
    emit("ucomisd xmm0, xmm1", [](Assembler& a) { a.sse(sse::ucomisd, Xmm::xmm0, Xmm::xmm1); });
    emit("vfmadd231ps xmm0, xmm1, xmm2", [](Assembler& a) { a.vex(vex::vfmadd231ps, Xmm::xmm0, Xmm::xmm1, Xmm::xmm2); });
    emit("vfmadd231sd xmm8, xmm9, xmm10", [](Assembler& a) { a.vex(vex::vfmadd231sd, Xmm::xmm8, Xmm::xmm9, Xmm::xmm10); });
    emit("vfnmadd231ss xmm1, xmm2, dword ptr [rax]", [](Assembler& a) { a.vex(vex::vfnmadd231ss, Xmm::xmm1, Xmm::xmm2, ptr(G::rax)); });
    emit("vaddps ymm0, ymm1, ymm2", [](Assembler& a) { a.vex(vex::vaddps, Xmm::xmm0, Xmm::xmm1, Xmm::xmm2, true); });
    emit("vpaddd xmm11, xmm12, xmm13", [](Assembler& a) { a.vex(vex::vpaddd, Xmm::xmm11, Xmm::xmm12, Xmm::xmm13); });
    emit("vbroadcastss xmm0, xmm1", [](Assembler& a) { a.vex(vex::vbroadcastss, Xmm::xmm0, Xmm::xmm0, Xmm::xmm1); });
    emit("vpblendvb xmm0, xmm1, xmm2, xmm3", [](Assembler& a) { a.vex_is4(vex::vpblendvb, Xmm::xmm0, Xmm::xmm1, Xmm::xmm2, Xmm::xmm3); });
    emit("ldmxcsr dword ptr [r15 + 0x10]", [](Assembler& a) { a.ldmxcsr(ptr(G::r15, 16)); });
    emit("stmxcsr dword ptr [rsp + 0x8]", [](Assembler& a) { a.stmxcsr(ptr(G::rsp, 8)); });
    emit("mov rax, qword ptr [8*rcx + 0x1000]", [](Assembler& a) { a.mov(64, G::rax, ptr_index(G::rcx, 8, 0x1000)); });
    // Labels: forward jump over 2 bytes, rip-relative load of the label.
    emit("HEX:e9020000009090", [](Assembler& a) { auto l = a.new_label(); a.jmp(l); a.nop(); a.nop(); a.bind(l); });
    emit("HEX:0f84faffffff", [](Assembler& a) { auto l = a.bound_label(); a.jcc(Cond::e, l); });
    emit("HEX:ebfe", [](Assembler& a) { auto l = a.bound_label(); a.jmp_short(l); });
    emit("lea rax, [rip]", [](Assembler& a) { auto l = a.new_label(); a.lea(64, G::rax, rip(l)); a.bind(l); });
    emit("cmp dword ptr [rip], 0x1", [](Assembler& a) { auto l = a.new_label(); a.cmp(32, rip(l), 1); a.bind(l); });
    emit("HEX:909090", [](Assembler& a) { a.nop(); a.align_rel32_after(1); });
    return 0;
}
