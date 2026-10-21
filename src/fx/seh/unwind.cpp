// ARM64 unwind information: .pdata/.xdata decoding and virtual unwinding.
//
// Packed (.pdata Flag 1/2) entries are expanded into the unwind codes of
// their canonical prolog/epilog, so one interpreter handles both forms.

#include "fx/seh/seh.hpp"

#include <cstring>

namespace transmute::fx::seh {
namespace {

constexpr std::uint8_t op_end = 0xe4;
constexpr std::uint8_t op_end_c = 0xe5;
constexpr std::uint8_t op_save_next = 0xe6;

// Number of prolog/epilog instructions an opcode stands for.
unsigned instruction_weight(std::uint8_t op) noexcept {
    if (op == op_end || op == op_end_c) return 0;
    if (op >= 0xe8 && op <= 0xec) return 0;  // custom stack frames (asm routines)
    return 1;
}

bool is_reserved(const std::uint8_t* code, std::size_t available) noexcept {
    const std::uint8_t op = code[0];
    if (op == 0xe7) return available < 2 || (code[1] & 0x80) != 0;
    if (op >= 0xed && op <= 0xfb) return true;
    if (op >= 0xfd) return true;
    return false;
}

struct CodeRunner {
    Arm64Context& c;
    Arm64ContextPointers* ptrs;
    bool pc_set = false;
    UnwindStatus status = UnwindStatus::ok;

    bool load(std::uint64_t address, std::uint64_t& value) {
        if (!read_guest_value(address, value)) {
            status = UnwindStatus::unreadable;
            return false;
        }
        return true;
    }

    bool restore_x(unsigned reg, std::uint64_t address) {
        if (reg > 30) { status = UnwindStatus::bad_opcode; return false; }
        std::uint64_t value;
        if (!load(address, value)) return false;
        c.X[reg] = value;
        if (ptrs && reg >= 19) {
            auto** slots = &ptrs->X19;  // X19..X28, Fp, Lr are consecutive
            slots[reg - 19] = reinterpret_cast<PDWORD64>(static_cast<std::uintptr_t>(address));
        }
        return true;
    }

    bool restore_d(unsigned reg, std::uint64_t address) {
        if (reg > 31) { status = UnwindStatus::bad_opcode; return false; }
        std::uint64_t value;
        if (!load(address, value)) return false;
        c.V[reg].Low = value;
        if (ptrs && reg >= 8 && reg <= 15) {
            auto** slots = &ptrs->D8;
            slots[reg - 8] = reinterpret_cast<PDWORD64>(static_cast<std::uintptr_t>(address));
        }
        return true;
    }

    bool restore_q(unsigned reg, std::uint64_t address) {
        if (reg > 31) { status = UnwindStatus::bad_opcode; return false; }
        std::uint64_t low, high;
        if (!load(address, low) || !load(address + 8, high)) return false;
        c.V[reg].Low = low;
        c.V[reg].High = static_cast<LONGLONG>(high);
        if (ptrs && reg >= 8 && reg <= 15) {
            auto** slots = &ptrs->D8;
            slots[reg - 8] = reinterpret_cast<PDWORD64>(static_cast<std::uintptr_t>(address));
        }
        return true;
    }

    bool restore_x_run(unsigned first, unsigned count, std::uint64_t address) {
        for (unsigned i = 0; i < count; ++i)
            if (!restore_x(first + i, address + 8ull * i)) return false;
        return true;
    }
    bool restore_d_run(unsigned first, unsigned count, std::uint64_t address) {
        for (unsigned i = 0; i < count; ++i)
            if (!restore_d(first + i, address + 8ull * i)) return false;
        return true;
    }

    // Executes codes starting at byte `index`, first skipping `skip`
    // instructions (partially executed prolog/epilog).
    bool run(const std::uint8_t* codes, std::uint32_t size, std::uint32_t index, std::uint32_t skip) {
        std::uint32_t pos = index;
        while (skip > 0 && pos < size) {
            const std::uint8_t op = codes[pos];
            if (op == op_end || op == op_end_c) break;
            skip -= instruction_weight(op) ? 1 : 0;
            pos += unwind_code_size(op);
        }
        // save_next codes precede the pair code they extend (unwind order is
        // the reverse of the prolog), so they accumulate into its count.
        unsigned pair_count = 2;
        while (pos < size) {
            const std::uint8_t* p = codes + pos;
            const std::uint8_t op = p[0];
            const unsigned len = unwind_code_size(op);
            if (pos + len > size) { status = UnwindStatus::truncated; return false; }
            if (is_reserved(p, size - pos)) { status = UnwindStatus::bad_opcode; return false; }
            const std::uint32_t b1 = len > 1 ? p[1] : 0;
            const std::uint64_t sp = c.Sp;
            if (op < 0x20) {                                   // alloc_s
                c.Sp = sp + 16ull * (op & 0x1f);
            } else if (op < 0x40) {                            // save_r19r20_x
                if (!restore_x_run(19, pair_count, sp)) return false;
                c.Sp = sp + 8ull * (op & 0x1f);
            } else if (op < 0x80) {                            // save_fplr
                const std::uint64_t at = sp + 8ull * (op & 0x3f);
                if (!restore_x(29, at) || !restore_x(30, at + 8)) return false;
            } else if (op < 0xc0) {                            // save_fplr_x
                if (!restore_x(29, sp) || !restore_x(30, sp + 8)) return false;
                c.Sp = sp + 8ull * ((op & 0x3f) + 1);
            } else if (op < 0xc8) {                            // alloc_m
                c.Sp = sp + 16ull * (((op & 0x7u) << 8) | b1);
            } else if (op < 0xcc) {                            // save_regp
                const unsigned reg = 19 + (((op & 0x3u) << 2) | (b1 >> 6));
                const std::uint64_t base = sp + 8ull * (b1 & 0x3f);
                if (!restore_x_run(reg, pair_count, base)) return false;
            } else if (op < 0xd0) {                            // save_regp_x
                const unsigned reg = 19 + (((op & 0x3u) << 2) | (b1 >> 6));
                if (!restore_x_run(reg, pair_count, sp)) return false;
                c.Sp = sp + 8ull * ((b1 & 0x3f) + 1);
            } else if (op < 0xd4) {                            // save_reg
                const unsigned reg = 19 + (((op & 0x3u) << 2) | (b1 >> 6));
                if (!restore_x(reg, sp + 8ull * (b1 & 0x3f))) return false;
            } else if (op < 0xd6) {                            // save_reg_x
                const unsigned reg = 19 + (((op & 0x1u) << 3) | (b1 >> 5));
                if (!restore_x(reg, sp)) return false;
                c.Sp = sp + 8ull * ((b1 & 0x1f) + 1);
            } else if (op < 0xd8) {                            // save_lrpair
                const unsigned reg = 19 + 2 * (((op & 0x1u) << 2) | (b1 >> 6));
                const std::uint64_t at = sp + 8ull * (b1 & 0x3f);
                if (reg > 29) { status = UnwindStatus::bad_opcode; return false; }
                if (!restore_x(reg, at) || !restore_x(30, at + 8)) return false;
            } else if (op < 0xda) {                            // save_fregp
                const unsigned reg = 8 + (((op & 0x1u) << 2) | (b1 >> 6));
                const std::uint64_t base = sp + 8ull * (b1 & 0x3f);
                if (!restore_d_run(reg, pair_count, base)) return false;
            } else if (op < 0xdc) {                            // save_fregp_x
                const unsigned reg = 8 + (((op & 0x1u) << 2) | (b1 >> 6));
                if (!restore_d_run(reg, pair_count, sp)) return false;
                c.Sp = sp + 8ull * ((b1 & 0x3f) + 1);
            } else if (op < 0xde) {                            // save_freg
                const unsigned reg = 8 + (((op & 0x1u) << 2) | (b1 >> 6));
                if (!restore_d(reg, sp + 8ull * (b1 & 0x3f))) return false;
            } else if (op == 0xde) {                           // save_freg_x
                const unsigned reg = 8 + (b1 >> 5);
                if (!restore_d(reg, sp)) return false;
                c.Sp = sp + 8ull * ((b1 & 0x1f) + 1);
            } else if (op == 0xdf) {                           // alloc_z (SVE)
                status = UnwindStatus::unsupported_opcode;
                return false;
            } else if (op == 0xe0) {                           // alloc_l
                const std::uint64_t units = (std::uint64_t{p[1]} << 16) | (std::uint64_t{p[2]} << 8) | p[3];
                c.Sp = sp + 16ull * units;
            } else if (op == 0xe1) {                           // set_fp
                c.Sp = c.Fp;
            } else if (op == 0xe2) {                           // add_fp
                c.Sp = c.Fp - 8ull * b1;
            } else if (op == 0xe3) {                           // nop
            } else if (op == op_end) {
                return true;
            } else if (op == op_end_c) {
            } else if (op == op_save_next) {
                pair_count += 2;
                pos += len;
                continue;
            } else if (op == 0xe7) {                           // save_any_reg
                const std::uint32_t b2 = p[2];
                const bool pair = (b1 & 0x40) != 0;
                const bool writeback = (b1 & 0x20) != 0;
                const unsigned reg = b1 & 0x1f;
                const unsigned kind = b2 >> 6;
                const unsigned o = b2 & 0x3f;
                if (kind == 3) { status = UnwindStatus::unsupported_opcode; return false; }
                const unsigned count = pair ? pair_count : 1;
                const unsigned element = kind == 2 ? 16 : 8;
                const std::uint64_t offset = (kind == 2 || pair || writeback) ? 16ull * o : 8ull * o;
                const std::uint64_t base = writeback ? sp : sp + offset;
                for (unsigned i = 0; i < count; ++i) {
                    const std::uint64_t at = base + std::uint64_t{element} * i;
                    const bool ok = kind == 0 ? restore_x(reg + i, at)
                                  : kind == 1 ? restore_d(reg + i, at)
                                              : restore_q(reg + i, at);
                    if (!ok) return false;
                }
                if (writeback) c.Sp = sp + offset;
            } else if (op == 0xe8) {                           // MSFT_OP_TRAP_FRAME (kernel)
                status = UnwindStatus::unsupported_opcode;
                return false;
            } else if (op == 0xe9) {                           // MSFT_OP_MACHINE_FRAME
                std::uint64_t new_sp, new_pc;
                if (!load(sp, new_sp) || !load(sp + 8, new_pc)) return false;
                c.Pc = new_pc;
                c.Sp = new_sp;
                c.ContextFlags &= ~kCtxUnwoundToCall;
                pc_set = true;
            } else if (op == 0xea) {                           // MSFT_OP_CONTEXT
                Arm64Context saved;
                if (!read_guest(sp, &saved, sizeof(saved))) { status = UnwindStatus::unreadable; return false; }
                const DWORD flags = (c.ContextFlags & ~kCtxUnwoundToCall) | (saved.ContextFlags & kCtxUnwoundToCall);
                c = saved;
                c.ContextFlags = flags;
                if (ptrs) {
                    auto** xs = &ptrs->X19;
                    for (unsigned r = 19; r <= 30; ++r)
                        xs[r - 19] = reinterpret_cast<PDWORD64>(static_cast<std::uintptr_t>(
                            sp + 8ull + 8ull * r));
                    auto** ds = &ptrs->D8;
                    for (unsigned r = 8; r <= 15; ++r)
                        ds[r - 8] = reinterpret_cast<PDWORD64>(static_cast<std::uintptr_t>(
                            sp + offsetof(Arm64Context, V) + 16ull * r));
                }
                pc_set = true;
            } else if (op == 0xeb) {                           // MSFT_OP_EC_CONTEXT (ARM64EC only)
                status = UnwindStatus::unsupported_opcode;
                return false;
            } else if (op == 0xec) {                           // MSFT_OP_CLEAR_UNWOUND_TO_CALL
                c.Pc = c.Lr;
                c.ContextFlags &= ~kCtxUnwoundToCall;
                pc_set = true;
            } else if (op == 0xfc) {                           // pac_sign_lr: return address is never signed here
            } else {
                status = UnwindStatus::bad_opcode;
                return false;
            }
            pair_count = 2;
            pos += len;
        }
        // The end of the code array terminates a sequence that has no `end`
        // (compilers emit such epilog sequences, padded with nops).
        return true;
    }
};

// ---- packed unwind data ----------------------------------------------------

struct CodeBuilder {
    std::uint8_t bytes[64];
    unsigned size = 0;
    struct Op { std::uint8_t b[4]; std::uint8_t len; bool set_fp; bool home; };
    Op ops[40];
    unsigned count = 0;

    void push(std::initializer_list<std::uint8_t> code, bool set_fp = false, bool home = false) {
        Op op{};
        op.len = static_cast<std::uint8_t>(code.size());
        unsigned i = 0;
        for (auto b : code) op.b[i++] = b;
        op.set_fp = set_fp;
        op.home = home;
        ops[count++] = op;
    }
    void alloc(std::uint32_t bytes_to_allocate) {
        const std::uint32_t units = bytes_to_allocate / 16;
        if (units < 32) push({static_cast<std::uint8_t>(units)});
        else if (units < 2048) push({static_cast<std::uint8_t>(0xc0 | (units >> 8)), static_cast<std::uint8_t>(units & 0xff)});
        else push({0xe0, static_cast<std::uint8_t>(units >> 16), static_cast<std::uint8_t>(units >> 8),
                   static_cast<std::uint8_t>(units)});
    }
    void emit(const Op& op) {
        for (unsigned i = 0; i < op.len; ++i) bytes[size++] = op.b[i];
    }
    void emit_byte(std::uint8_t b) { bytes[size++] = b; }
};

// Expands a packed .pdata word into unwind codes (prolog codes, then the
// epilog codes) following the canonical sequence of the ARM64 ABI.
UnwindStatus expand_packed(const Arm64RuntimeFunction& entry, FunctionUnwindInfo& out) {
    const unsigned flag = entry.Flag;
    const unsigned reg_f = entry.RegF;
    const unsigned reg_i = entry.RegI;
    const unsigned h = entry.H;
    const unsigned cr = entry.CR;
    const unsigned frame_size = entry.FrameSize * 16u;
    if (flag == 3 || reg_i > 10) return UnwindStatus::bad_packed;

    unsigned int_size = reg_i * 8 + (cr == 1 ? 8 : 0);
    const unsigned fp_count = reg_f ? reg_f + 1 : 0;
    const unsigned fp_size = fp_count * 8;
    const unsigned save_size = (int_size + fp_size + 64 * h + 15) & ~15u;
    if (frame_size < save_size) return UnwindStatus::bad_packed;
    const unsigned local_size = frame_size - save_size;
    if ((cr == 2 || cr == 3) && local_size < 16) return UnwindStatus::bad_packed;

    CodeBuilder b;   // ops in prolog execution order
    if (cr == 2) b.push({0xfc});
    // Integer callee-saved registers.
    for (unsigned i = 0; i < reg_i / 2; ++i) {
        if (i == 0) {
            const unsigned z = save_size / 8 - 1;   // stp x19,x20,[sp,#-savsz]!
            b.push({0xcc, static_cast<std::uint8_t>(z)});
        } else {
            const unsigned x = 2 * i, z = 2 * i;    // stp x(19+2i),x(20+2i),[sp,#16i]
            b.push({static_cast<std::uint8_t>(0xc8 | (x >> 2)), static_cast<std::uint8_t>(((x & 3) << 6) | z)});
        }
    }
    if (reg_i % 2) {
        const unsigned j = reg_i - 1;
        if (cr == 1) {
            if (j == 0) {
                // sub sp,sp,#savsz ; stp x19,lr,[sp]
                b.alloc(save_size);
                b.push({0xd6, 0x00});
            } else {
                const unsigned x = j / 2, z = j;    // stp x(19+j),lr,[sp,#8j]
                b.push({static_cast<std::uint8_t>(0xd6 | (x >> 2)), static_cast<std::uint8_t>(((x & 3) << 6) | z)});
            }
        } else if (j == 0) {
            const unsigned z = save_size / 8 - 1;   // str x19,[sp,#-savsz]!
            b.push({0xd4, static_cast<std::uint8_t>(z & 0x1f)});
        } else {
            const unsigned x = j, z = j;            // str x(19+j),[sp,#8j]
            b.push({static_cast<std::uint8_t>(0xd0 | (x >> 2)), static_cast<std::uint8_t>(((x & 3) << 6) | z)});
        }
    } else if (cr == 1) {
        if (reg_i == 0) {
            const unsigned z = save_size / 8 - 1;   // str lr,[sp,#-savsz]!
            b.push({static_cast<std::uint8_t>(0xd4 | (11 >> 3)), static_cast<std::uint8_t>(((11 & 7) << 5) | (z & 0x1f))});
        } else {
            const unsigned z = reg_i;               // str lr,[sp,#(intsz-8)]
            b.push({static_cast<std::uint8_t>(0xd0 | (11 >> 2)), static_cast<std::uint8_t>(((11 & 3) << 6) | z)});
        }
    }
    // Floating-point callee-saved registers.
    for (unsigned i = 0; i < fp_count / 2; ++i) {
        const unsigned x = 2 * i;
        if (i == 0 && int_size == 0) {
            const unsigned z = save_size / 8 - 1;   // stp d8,d9,[sp,#-savsz]!
            b.push({static_cast<std::uint8_t>(0xda | (x >> 2)), static_cast<std::uint8_t>(((x & 3) << 6) | z)});
        } else {
            const unsigned z = (int_size + 16 * i) / 8;
            b.push({static_cast<std::uint8_t>(0xd8 | (x >> 2)), static_cast<std::uint8_t>(((x & 3) << 6) | z)});
        }
    }
    if (fp_count % 2) {
        const unsigned x = fp_count - 1, z = (int_size + 8 * (fp_count - 1)) / 8;
        b.push({static_cast<std::uint8_t>(0xdc | (x >> 2)), static_cast<std::uint8_t>(((x & 3) << 6) | z)});
    }
    // Homed integer arguments: four stp x0..x7, no unwind effect.
    if (h) {
        if (int_size == 0 && fp_size == 0) {
            b.alloc(save_size);   // stp x0,x1,[sp,#-savsz]!
            b.ops[b.count - 1].home = true;
        } else {
            b.push({0xe3}, false, true);
        }
        for (int i = 0; i < 3; ++i) b.push({0xe3}, false, true);
    }
    // Local area, <x29,lr> and outgoing arguments.
    if (cr == 2 || cr == 3) {
        if (local_size <= 512) {
            b.push({static_cast<std::uint8_t>(0x80 | (local_size / 8 - 1))});   // stp x29,lr,[sp,#-locsz]!
        } else {
            if (local_size > 4080) {
                b.alloc(4080);
                b.alloc(local_size - 4080);
            } else {
                b.alloc(local_size);
            }
            b.push({0x40});                                                     // stp x29,lr,[sp]
        }
        b.push({0xe1}, true);                                                   // mov x29,sp
    } else if (local_size > 0) {
        if (local_size > 4080) {
            b.alloc(4080);
            b.alloc(local_size - 4080);
        } else {
            b.alloc(local_size);
        }
    }

    // Prolog codes in unwind order (reverse of execution), then `end`.
    if (flag == 2) b.emit_byte(op_end_c);
    for (unsigned i = b.count; i-- > 0;) b.emit(b.ops[i]);
    b.emit_byte(op_end);
    std::uint32_t epilog_instructions = 0;
    const std::uint32_t epilog_index = b.size;
    if (flag == 1) {
        // Epilog: the prolog in reverse without `mov x29,sp` and without
        // the homing stores (a homing store that allocated the save area
        // becomes `add sp`).
        for (unsigned i = b.count; i-- > 0;) {
            const auto& op = b.ops[i];
            if (op.set_fp) continue;
            if (op.home && op.b[0] == 0xe3) continue;
            b.emit(op);
            ++epilog_instructions;
        }
        b.emit_byte(op_end);
    }

    out.packed = true;
    out.packed_flag = static_cast<std::uint8_t>(flag);
    out.length = entry.FunctionLength;
    out.code_size = b.size;
    std::memcpy(out.codes, b.bytes, b.size);
    out.has_handler = false;
    out.handler_rva = 0;
    out.handler_data = 0;
    out.xdata_size = 0;
    out.epilog_scopes_address = 0;
    if (flag == 1) {
        const std::uint32_t epilog_length = epilog_instructions + 1;   // + ret
        out.epilog_count = 1;
        out.synthesized_epilog.index = epilog_index;
        out.synthesized_epilog.start = out.length >= epilog_length ? out.length - epilog_length : 0;
    } else {
        out.epilog_count = 0;
    }
    return UnwindStatus::ok;
}

UnwindStatus describe_xdata(std::uint64_t image_base, const Arm64RuntimeFunction& entry, FunctionUnwindInfo& out) {
    const std::uint64_t xdata = image_base + entry.UnwindData;
    std::uint32_t header;
    if (!read_guest_value(xdata, header)) return UnwindStatus::unreadable;
    const std::uint32_t version = (header >> 18) & 3;
    if (version != 0) return UnwindStatus::bad_version;
    out.packed = false;
    out.packed_flag = 0;
    out.length = header & 0x3ffff;
    out.has_handler = ((header >> 20) & 1) != 0;
    const bool epilog_in_header = ((header >> 21) & 1) != 0;
    std::uint32_t epilog_field = (header >> 22) & 0x1f;
    std::uint32_t code_words = (header >> 27) & 0x1f;
    std::uint32_t offset = 4;
    if (epilog_field == 0 && code_words == 0) {
        std::uint32_t extended;
        if (!read_guest_value(xdata + 4, extended)) return UnwindStatus::unreadable;
        epilog_field = extended & 0xffff;
        code_words = (extended >> 16) & 0xff;
        offset = 8;
    }
    if (!epilog_in_header) {
        out.epilog_count = epilog_field;
        out.epilog_scopes_address = epilog_field ? xdata + offset : 0;
        offset += 4 * epilog_field;
    } else {
        out.epilog_count = 1;
        out.epilog_scopes_address = 0;
    }
    out.code_size = code_words * 4;
    if (out.code_size > kMaxUnwindCodeBytes) return UnwindStatus::truncated;
    if (!read_guest(xdata + offset, out.codes, out.code_size)) return UnwindStatus::unreadable;
    offset += out.code_size;
    if (out.has_handler) {
        if (!read_guest_value(xdata + offset, out.handler_rva)) return UnwindStatus::unreadable;
        out.handler_data = xdata + offset + 4;
    } else {
        out.handler_rva = 0;
        out.handler_data = 0;
    }
    out.xdata_size = offset + (out.has_handler ? 4 : 0);
    if (epilog_in_header) {
        if (epilog_field >= out.code_size) return UnwindStatus::bad_epilog;
        const std::uint32_t epilog_length = sequence_instruction_count(out, epilog_field) + 1;
        out.synthesized_epilog.index = epilog_field;
        out.synthesized_epilog.start = out.length >= epilog_length ? out.length - epilog_length : 0;
    }
    return UnwindStatus::ok;
}

} // namespace

const char* unwind_status_name(UnwindStatus status) noexcept {
    switch (status) {
    case UnwindStatus::ok: return "ok";
    case UnwindStatus::unreadable: return "unreadable";
    case UnwindStatus::bad_version: return "bad_version";
    case UnwindStatus::truncated: return "truncated";
    case UnwindStatus::bad_opcode: return "bad_opcode";
    case UnwindStatus::unsupported_opcode: return "unsupported_opcode";
    case UnwindStatus::bad_packed: return "bad_packed";
    case UnwindStatus::bad_epilog: return "bad_epilog";
    }
    return "unknown";
}

unsigned unwind_code_size(std::uint8_t op) noexcept {
    if (op < 0xc0) return 1;
    if (op < 0xe0) return 2;
    switch (op) {
    case 0xe0: return 4;
    case 0xe2: return 2;
    case 0xe7: return 3;
    case 0xf8: return 2;
    case 0xf9: return 3;
    case 0xfa: return 4;
    case 0xfb: return 5;
    default: return 1;
    }
}

std::uint32_t sequence_instruction_count(const FunctionUnwindInfo& info, std::uint32_t index) noexcept {
    std::uint32_t count = 0;
    std::uint32_t pos = index;
    while (pos < info.code_size) {
        const std::uint8_t op = info.codes[pos];
        if (op == op_end || op == op_end_c) break;
        count += instruction_weight(op);
        pos += unwind_code_size(op);
    }
    return count;
}

std::uint32_t prolog_instruction_count(const FunctionUnwindInfo& info) noexcept {
    return sequence_instruction_count(info, 0);
}

std::uint32_t epilog_scopes(const FunctionUnwindInfo& info, EpilogScope* out, std::uint32_t capacity) noexcept {
    if (info.epilog_count == 0) return 0;
    if (info.epilog_scopes_address == 0) {
        if (capacity == 0) return 0;
        out[0] = info.synthesized_epilog;
        return 1;
    }
    std::uint32_t written = 0;
    for (std::uint32_t i = 0; i < info.epilog_count && written < capacity; ++i) {
        std::uint32_t word;
        if (!read_guest_value(info.epilog_scopes_address + 4ull * i, word)) break;
        out[written++] = EpilogScope{word & 0x3ffff, word >> 22};
    }
    return written;
}

UnwindStatus describe_function(std::uint64_t image_base, const Arm64RuntimeFunction& entry,
                               FunctionUnwindInfo& out) noexcept {
    out.begin_rva = entry.BeginAddress;
    if (entry.Flag != 0) return expand_packed(entry, out);
    return describe_xdata(image_base, entry, out);
}

std::uint32_t function_length_bytes(std::uint64_t image_base, const Arm64RuntimeFunction& entry) noexcept {
    if (entry.Flag != 0) return entry.FunctionLength * 4u;
    std::uint32_t header;
    if (!read_guest_value(image_base + entry.UnwindData, header)) return 0;
    return (header & 0x3ffff) * 4u;
}

UnwindStatus validate_function(const FunctionUnwindInfo& info) noexcept {
    auto check_sequence = [&](std::uint32_t index) -> UnwindStatus {
        std::uint32_t pos = index;
        bool saw_end_c = false;
        while (pos < info.code_size) {
            const std::uint8_t* p = info.codes + pos;
            const unsigned len = unwind_code_size(p[0]);
            if (pos + len > info.code_size) return UnwindStatus::truncated;
            if (is_reserved(p, info.code_size - pos)) return UnwindStatus::bad_opcode;
            const std::uint8_t op = p[0];
            if (op == op_end) return UnwindStatus::ok;
            if (op == op_end_c) {
                if (saw_end_c) return UnwindStatus::bad_opcode;
                saw_end_c = true;
            }
            const unsigned b1 = len > 1 ? p[1] : 0;
            if (op >= 0xc8 && op < 0xd4) {
                if (19 + (((op & 3u) << 2) | (b1 >> 6)) > 30) return UnwindStatus::bad_opcode;
            } else if (op >= 0xd4 && op < 0xd6) {
                if (19 + (((op & 1u) << 3) | (b1 >> 5)) > 30) return UnwindStatus::bad_opcode;
            } else if (op >= 0xd6 && op < 0xd8) {
                if (19 + 2 * (((op & 1u) << 2) | (b1 >> 6)) > 29) return UnwindStatus::bad_opcode;
            } else if (op == 0xe7) {
                if ((p[2] >> 6) == 3) return UnwindStatus::unsupported_opcode;
            } else if (op == 0xdf || op == 0xe8 || op == 0xeb) {
                return UnwindStatus::unsupported_opcode;
            }
            pos += len;
        }
        return UnwindStatus::ok;   // end of the code array ends the sequence
    };
    if (info.code_size == 0) return UnwindStatus::truncated;
    UnwindStatus status = check_sequence(0);
    if (status != UnwindStatus::ok) return status;
    EpilogScope scopes[64];
    std::uint32_t done = 0;
    while (done < info.epilog_count) {
        // Read scopes in chunks to bound stack use.
        FunctionUnwindInfo view = info;
        const std::uint32_t chunk = info.epilog_count - done < 64 ? info.epilog_count - done : 64;
        if (info.epilog_scopes_address) {
            view.epilog_scopes_address = info.epilog_scopes_address + 4ull * done;
            view.epilog_count = chunk;
        }
        const std::uint32_t n = epilog_scopes(view, scopes, chunk);
        if (n != chunk) return UnwindStatus::unreadable;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (scopes[i].index >= info.code_size) return UnwindStatus::bad_epilog;
            if (scopes[i].start > info.length) return UnwindStatus::bad_epilog;
            status = check_sequence(scopes[i].index);
            if (status != UnwindStatus::ok) return status;
        }
        done += n;
    }
    return UnwindStatus::ok;
}

VirtualUnwindResult virtual_unwind_info(std::uint32_t handler_type, std::uint64_t image_base,
                                        std::uint64_t control_pc, const FunctionUnwindInfo& info,
                                        Arm64Context& context, Arm64ContextPointers* pointers) noexcept {
    (void)handler_type;
    VirtualUnwindResult result;
    const std::uint64_t begin = image_base + info.begin_rva;
    const std::uint64_t offset = control_pc >= begin ? (control_pc - begin) / 4 : ~0ull;

    CodeRunner runner{context, pointers};
    bool done = false;
    const std::uint32_t prolog = prolog_instruction_count(info);
    if (offset < prolog) {
        result.in_prolog = true;
        runner.run(info.codes, info.code_size, 0, static_cast<std::uint32_t>(prolog - offset));
        done = true;
    }
    if (!done && info.epilog_count && offset != ~0ull) {
        // Find the last epilog scope starting at or before the PC.
        EpilogScope best{};
        bool found = false;
        if (info.epilog_scopes_address == 0) {
            if (info.synthesized_epilog.start <= offset) { best = info.synthesized_epilog; found = true; }
        } else {
            for (std::uint32_t i = 0; i < info.epilog_count; ++i) {
                std::uint32_t word;
                if (!read_guest_value(info.epilog_scopes_address + 4ull * i, word)) break;
                const EpilogScope scope{word & 0x3ffff, word >> 22};
                if (scope.start > offset) break;
                best = scope;
                found = true;
            }
        }
        if (found && best.index < info.code_size) {
            const std::uint32_t length = sequence_instruction_count(info, best.index);
            if (offset <= std::uint64_t{best.start} + length) {
                result.in_epilog = true;
                runner.run(info.codes, info.code_size, best.index,
                           static_cast<std::uint32_t>(offset - best.start));
                done = true;
            }
        }
    }
    if (!done) {
        runner.run(info.codes, info.code_size, 0, 0);
        if (info.has_handler) {
            result.handler = image_base + info.handler_rva;
            result.handler_data = info.handler_data;
        }
    }
    result.status = runner.status;
    if (runner.status != UnwindStatus::ok) {
        result.handler = 0;
        result.handler_data = 0;
        return result;
    }
    if (!runner.pc_set) {
        context.Pc = context.Lr;
        context.ContextFlags |= kCtxUnwoundToCall;
    }
    result.establisher_frame = context.Sp;
    return result;
}

VirtualUnwindResult virtual_unwind(std::uint32_t handler_type, std::uint64_t image_base,
                                   std::uint64_t control_pc, const Arm64RuntimeFunction& entry,
                                   Arm64Context& context, Arm64ContextPointers* pointers) noexcept {
    static thread_local FunctionUnwindInfo info;
    const UnwindStatus status = describe_function(image_base, entry, info);
    if (status != UnwindStatus::ok) {
        VirtualUnwindResult result;
        result.status = status;
        return result;
    }
    return virtual_unwind_info(handler_type, image_base, control_pc, info, context, pointers);
}

} // namespace transmute::fx::seh
