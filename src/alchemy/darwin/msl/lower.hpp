#pragma once

// LLVM IR -> SPIR-V lowering for one MSL entry point (owner: A6). Takes the
// entry's extern "C" wrapper from the library module, structurises it and
// emits a complete Vulkan SPIR-V module plus the entry's Reflection, under
// the resource model fixed in include/alchemy/msl/compiler.hpp:
//   - device/constant pointers are 64-bit physical addresses
//     (PhysicalStorageBuffer64), loaded from the argument table;
//   - thread (alloca) and threadgroup memory are word arrays in Function and
//     Workgroup storage, addressed by byte offsets, so any pointer arithmetic
//     or type punning the IR does keeps working; constant data globals are a
//     Private word array;
//   - i8/i16 values live widened in 32-bit registers and half values in
//     32-bit floats (8/16-bit types appear only in buffer memory).

#include "alchemy/msl/compiler.hpp"
#include "llvm_ir.hpp"
#include "rewriter.hpp"

#include <string>
#include <vector>

namespace alchemy::msl::detail {

struct LowerResult {
    std::vector<std::uint32_t> spirv;
    Reflection reflection;
    std::vector<Diagnostic> diagnostics;   // errors name the unsupported construct
};

[[nodiscard]] LowerResult lower(ir::Module& module, const EntryPoint& entry,
    const std::vector<FunctionConstantDecl>& constants, const CompileOptions& options);

// Builds a Reflection::types entry from an MSL type spelling.
std::uint32_t add_reflection_type(Reflection& reflection, const TypeSpelling& spelling);

} // namespace alchemy::msl::detail
