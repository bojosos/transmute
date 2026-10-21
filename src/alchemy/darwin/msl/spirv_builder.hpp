#pragma once

// Minimal SPIR-V module writer for the MSL compiler (owner: A6): sections in
// the order the specification requires, deduplicated types and constants.

#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::msl::spv {

// The opcodes, enumerants and GLSL.std.450 numbers the lowering uses.
namespace op {
enum : std::uint32_t {
    Nop = 0, Undef = 1, Name = 5, MemberName = 6, String = 7, Extension = 10, ExtInstImport = 11, ExtInst = 12,
    MemoryModel = 14, EntryPoint = 15, ExecutionMode = 16, Capability = 17, TypeVoid = 19, TypeBool = 20, TypeInt = 21,
    TypeFloat = 22, TypeVector = 23, TypeMatrix = 24, TypeImage = 25, TypeSampler = 26, TypeSampledImage = 27,
    TypeArray = 28, TypeRuntimeArray = 29, TypeStruct = 30, TypePointer = 32, TypeFunction = 33,
    ConstantTrue = 41, ConstantFalse = 42, Constant = 43, ConstantComposite = 44, ConstantNull = 46,
    SpecConstantTrue = 48, SpecConstantFalse = 49, SpecConstant = 50, SpecConstantComposite = 51, SpecConstantOp = 52,
    Function = 54, FunctionParameter = 55, FunctionEnd = 56, FunctionCall = 57, Variable = 59, ImageTexelPointer = 60,
    Load = 61, Store = 62, CopyMemory = 63, AccessChain = 65, Decorate = 71, MemberDecorate = 72,
    VectorExtractDynamic = 77, VectorInsertDynamic = 78, VectorShuffle = 79, CompositeConstruct = 80,
    CompositeExtract = 81, CompositeInsert = 82, CopyObject = 83, SampledImage = 86,
    ImageSampleImplicitLod = 87, ImageSampleExplicitLod = 88, ImageSampleDrefImplicitLod = 89,
    ImageSampleDrefExplicitLod = 90, ImageFetch = 95, ImageGather = 96, ImageDrefGather = 97, ImageRead = 98,
    ImageWrite = 99, Image = 100, ImageQuerySizeLod = 103, ImageQuerySize = 104, ImageQueryLevels = 106,
    ImageQuerySamples = 107, ConvertFToU = 109, ConvertFToS = 110, ConvertSToF = 111, ConvertUToF = 112,
    UConvert = 113, SConvert = 114, FConvert = 115, QuantizeToF16 = 116, ConvertPtrToU = 117, ConvertUToPtr = 120,
    Bitcast = 124, SNegate = 126, FNegate = 127, IAdd = 128, FAdd = 129, ISub = 130, FSub = 131, IMul = 132,
    FMul = 133, UDiv = 134, SDiv = 135, FDiv = 136, UMod = 137, SRem = 138, SMod = 139, FRem = 140, FMod = 141,
    VectorTimesScalar = 142, Dot = 148, IAddCarry = 149, ISubBorrow = 150, UMulExtended = 151, SMulExtended = 152,
    Any = 154, All = 155, IsNan = 156, IsInf = 157, LogicalEqual = 164, LogicalNotEqual = 165, LogicalOr = 166,
    LogicalAnd = 167, LogicalNot = 168, Select = 169, IEqual = 170, INotEqual = 171, UGreaterThan = 172,
    SGreaterThan = 173, UGreaterThanEqual = 174, SGreaterThanEqual = 175, ULessThan = 176, SLessThan = 177,
    ULessThanEqual = 178, SLessThanEqual = 179, FOrdEqual = 180, FUnordEqual = 181, FOrdNotEqual = 182,
    FUnordNotEqual = 183, FOrdLessThan = 184, FUnordLessThan = 185, FOrdGreaterThan = 186, FUnordGreaterThan = 187,
    FOrdLessThanEqual = 188, FUnordLessThanEqual = 189, FOrdGreaterThanEqual = 190, FUnordGreaterThanEqual = 191,
    ShiftRightLogical = 194, ShiftRightArithmetic = 195, ShiftLeftLogical = 196, BitwiseOr = 197, BitwiseXor = 198,
    BitwiseAnd = 199, Not = 200, BitFieldInsert = 201, BitFieldSExtract = 202, BitFieldUExtract = 203,
    BitReverse = 204, BitCount = 205, DPdx = 207, DPdy = 208, Fwidth = 209, DPdxFine = 210, DPdyFine = 211,
    FwidthFine = 212, DPdxCoarse = 213, DPdyCoarse = 214, FwidthCoarse = 215, ControlBarrier = 224,
    MemoryBarrier = 225, AtomicLoad = 227, AtomicStore = 228, AtomicExchange = 229, AtomicCompareExchange = 230,
    AtomicIIncrement = 232, AtomicIDecrement = 233, AtomicIAdd = 234, AtomicISub = 235, AtomicSMin = 236,
    AtomicUMin = 237, AtomicSMax = 238, AtomicUMax = 239, AtomicAnd = 240, AtomicOr = 241, AtomicXor = 242,
    Phi = 245, LoopMerge = 246, SelectionMerge = 247, Label = 248, Branch = 249, BranchConditional = 250,
    Switch = 251, Kill = 252, Return = 253, ReturnValue = 254, Unreachable = 255,
    GroupNonUniformElect = 333, GroupNonUniformAll = 334, GroupNonUniformAny = 335, GroupNonUniformAllEqual = 336,
    GroupNonUniformBroadcast = 337, GroupNonUniformBroadcastFirst = 338, GroupNonUniformBallot = 339,
    GroupNonUniformShuffle = 345, GroupNonUniformShuffleXor = 346, GroupNonUniformShuffleUp = 347,
    GroupNonUniformShuffleDown = 348, GroupNonUniformIAdd = 349, GroupNonUniformFAdd = 350,
    GroupNonUniformIMul = 351, GroupNonUniformFMul = 352, GroupNonUniformSMin = 353, GroupNonUniformUMin = 354,
    GroupNonUniformFMin = 355, GroupNonUniformSMax = 356, GroupNonUniformUMax = 357, GroupNonUniformFMax = 358,
    GroupNonUniformBitwiseAnd = 359, GroupNonUniformBitwiseOr = 360, GroupNonUniformBitwiseXor = 361,
    GroupNonUniformQuadBroadcast = 365, GroupNonUniformQuadSwap = 366, TerminateInvocation = 4416,
    DemoteToHelperInvocation = 5380, ExecutionModeId = 331, DecorateId = 332, AtomicFAddEXT = 6035,
};
} // namespace op

namespace glsl {
enum : std::uint32_t {
    Round = 1, RoundEven = 2, Trunc = 3, FAbs = 4, SAbs = 5, FSign = 6, SSign = 7, Floor = 8, Ceil = 9, Fract = 10,
    Sin = 13, Cos = 14, Tan = 15, Asin = 16, Acos = 17, Atan = 18, Sinh = 19, Cosh = 20, Tanh = 21, Asinh = 22,
    Acosh = 23, Atanh = 24, Atan2 = 25, Pow = 26, Exp = 27, Log = 28, Exp2 = 29, Log2 = 30, Sqrt = 31,
    InverseSqrt = 32, FMin = 37, UMin = 38, SMin = 39, FMax = 40, UMax = 41, SMax = 42, FClamp = 43, UClamp = 44,
    SClamp = 45, FMix = 46, Step = 48, SmoothStep = 49, Fma = 50, Ldexp = 53, PackSnorm4x8 = 54, PackUnorm4x8 = 55,
    PackSnorm2x16 = 56, PackUnorm2x16 = 57, PackHalf2x16 = 58, UnpackSnorm2x16 = 60, UnpackUnorm2x16 = 61,
    UnpackHalf2x16 = 62, UnpackSnorm4x8 = 63, UnpackUnorm4x8 = 64, Length = 66, Distance = 67, Cross = 68,
    Normalize = 69, FaceForward = 70, Reflect = 71, Refract = 72, FindILsb = 73, FindSMsb = 74, FindUMsb = 75,
    NMin = 79, NMax = 80, NClamp = 81,
};
} // namespace glsl

namespace storage {
enum : std::uint32_t { UniformConstant = 0, Input = 1, Uniform = 2, Output = 3, Workgroup = 4, CrossWorkgroup = 5,
    Private = 6, Function = 7, PushConstant = 9, Image = 11, StorageBuffer = 12, PhysicalStorageBuffer = 5349 };
}
namespace decoration {
enum : std::uint32_t { SpecId = 1, Block = 2, ArrayStride = 6, BuiltIn = 11, NoPerspective = 13, Flat = 14,
    Centroid = 16, Sample = 17, Invariant = 18, NonWritable = 24, NonReadable = 25, Location = 30, Component = 31,
    Index = 32, Binding = 33, DescriptorSet = 34, Offset = 35, Aliased = 20, Restrict = 19, InputAttachmentIndex = 43,
    AliasedPointer = 5356, RestrictPointer = 5355 };
}
namespace builtin {
enum : std::uint32_t { Position = 0, PointSize = 1, ClipDistance = 3, PrimitiveId = 7, InvocationId = 8, Layer = 9,
    ViewportIndex = 10, FragCoord = 15, PointCoord = 16, FrontFacing = 17, SampleId = 18, SampleMask = 20,
    FragDepth = 22, NumWorkgroups = 24, WorkgroupSize = 25, WorkgroupId = 26, LocalInvocationId = 27,
    GlobalInvocationId = 28, LocalInvocationIndex = 29, SubgroupSize = 36, NumSubgroups = 38, SubgroupId = 40,
    SubgroupLocalInvocationId = 41, VertexIndex = 42, InstanceIndex = 43, BaseVertex = 4424, BaseInstance = 4425,
    FragStencilRefEXT = 5014 };
}
namespace capability {
enum : std::uint32_t { Matrix = 0, Shader = 1, Geometry = 2, Float16 = 9, Float64 = 10, Int64 = 11,
    Int16 = 22, ImageGatherExtended = 25, StorageImageMultisample = 27, ClipDistance = 32, Int8 = 39,
    Sampled1D = 43, Image1D = 44, SampledCubeArray = 45, SampledBuffer = 46, ImageBuffer = 47, ImageMSArray = 48,
    StorageImageExtendedFormats = 49, ImageQuery = 50, DerivativeControl = 51, InterpolationFunction = 52,
    StorageImageReadWithoutFormat = 55, StorageImageWriteWithoutFormat = 56, GroupNonUniform = 61,
    GroupNonUniformVote = 62, GroupNonUniformArithmetic = 63, GroupNonUniformBallot = 64,
    GroupNonUniformShuffle = 65, GroupNonUniformShuffleRelative = 66, GroupNonUniformQuad = 68,
    SampleRateShading = 35, ImageCubeArray = 34, DrawParameters = 4427, StorageBuffer16BitAccess = 4433,
    StorageBuffer8BitAccess = 4448, StencilExportEXT = 5013, ShaderLayer = 69, ShaderViewportIndex = 70,
    PhysicalStorageBufferAddresses = 5347, AtomicFloat32AddEXT = 6033, Int64Atomics = 12,
    MinLod = 42, DemoteToHelperInvocation = 5379, InputAttachment = 40 };
}

using Words = std::vector<std::uint32_t>;

class Module {
public:
    std::uint32_t id() { return next_id_++; }
    [[nodiscard]] std::uint32_t bound() const { return next_id_; }

    void capability(std::uint32_t value) { capabilities_.insert(value); }
    void extension(std::string_view name) { extensions_.insert(std::string(name)); }
    std::uint32_t glsl();

    // ---- types ------------------------------------------------------------------------
    std::uint32_t type_void() { return type({op::TypeVoid}); }
    std::uint32_t type_bool() { return type({op::TypeBool}); }
    std::uint32_t type_int(std::uint32_t bits, bool is_signed) { return type({op::TypeInt, bits, is_signed ? 1U : 0U}); }
    std::uint32_t type_float(std::uint32_t bits) { return type({op::TypeFloat, bits}); }
    std::uint32_t type_vector(std::uint32_t element, std::uint32_t count) {
        return count == 1 ? element : type({op::TypeVector, element, count});
    }
    std::uint32_t type_array(std::uint32_t element, std::uint32_t length_id, std::uint32_t stride = 0);
    std::uint32_t type_struct(const std::vector<std::uint32_t>& members, bool block = false,
        const std::vector<std::uint32_t>& offsets = {});
    std::uint32_t type_pointer(std::uint32_t storage, std::uint32_t pointee) { return type({op::TypePointer, storage, pointee}); }
    std::uint32_t type_function(std::uint32_t result, const std::vector<std::uint32_t>& params = {});
    std::uint32_t type_image(std::uint32_t sampled, std::uint32_t dim, std::uint32_t depth, std::uint32_t arrayed,
        std::uint32_t ms, std::uint32_t sampled_kind, std::uint32_t format) {
        return type({op::TypeImage, sampled, dim, depth, arrayed, ms, sampled_kind, format});
    }
    std::uint32_t type_sampler() { return type({op::TypeSampler}); }
    std::uint32_t type_sampled_image(std::uint32_t image) { return type({op::TypeSampledImage, image}); }

    // ---- constants ----------------------------------------------------------------------
    std::uint32_t constant_u32(std::uint32_t value) { return constant(type_int(32, false), {value}); }
    std::uint32_t constant_i32(std::int32_t value) { return constant(type_int(32, true), {static_cast<std::uint32_t>(value)}); }
    std::uint32_t constant_u64(std::uint64_t value) {
        return constant(type_int(64, false), {static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32)});
    }
    std::uint32_t constant_f32(float value);
    std::uint32_t constant_bool(bool value) { return type({value ? op::ConstantTrue : op::ConstantFalse, type_bool()}, true); }
    std::uint32_t constant(std::uint32_t type, const std::vector<std::uint32_t>& words);
    std::uint32_t constant_composite(std::uint32_t type, const std::vector<std::uint32_t>& parts);
    std::uint32_t constant_null(std::uint32_t type);
    std::uint32_t undef(std::uint32_t type);
    std::uint32_t spec_constant(std::uint32_t type, std::uint32_t spec_id, const std::vector<std::uint32_t>& default_words);
    std::uint32_t spec_constant_bool(std::uint32_t spec_id, bool default_value);
    std::uint32_t spec_constant_op(std::uint32_t type, std::uint32_t opcode, const std::vector<std::uint32_t>& operands);
    std::uint32_t spec_constant_composite(std::uint32_t type, const std::vector<std::uint32_t>& parts);

    // ---- globals, annotations, debug ------------------------------------------------------
    std::uint32_t variable(std::uint32_t pointer_type, std::uint32_t storage, std::uint32_t initializer = 0);
    void decorate(std::uint32_t target, std::uint32_t decoration, std::initializer_list<std::uint32_t> args = {});
    void member_decorate(std::uint32_t type, std::uint32_t member, std::uint32_t decoration, std::initializer_list<std::uint32_t> args = {});
    void name(std::uint32_t target, std::string_view text);
    void entry_point(std::uint32_t model, std::uint32_t function, std::string_view name, const std::vector<std::uint32_t>& interface);
    void execution_mode(std::uint32_t function, std::uint32_t mode, std::initializer_list<std::uint32_t> args = {});
    void execution_mode_id(std::uint32_t function, std::uint32_t mode, std::initializer_list<std::uint32_t> args);

    // ---- function body --------------------------------------------------------------------
    Words& code() { return code_; }
    void emit(std::uint32_t opcode, std::initializer_list<std::uint32_t> operands) { emit(opcode, std::vector<std::uint32_t>(operands)); }
    void emit(std::uint32_t opcode, const std::vector<std::uint32_t>& operands);
    // Instruction with a result: (result type, new id, operands...). Returns the id.
    std::uint32_t emit_value(std::uint32_t opcode, std::uint32_t result_type, const std::vector<std::uint32_t>& operands);
    // Function-scope variables go to the entry block's head.
    Words& function_variables() { return function_variables_; }

    // Final binary (SPIR-V version as 0x00MMmm00).
    [[nodiscard]] Words finish(std::uint32_t version, std::uint32_t addressing, std::uint32_t memory_model);

    static void append_string(Words& words, std::string_view text);

private:
    std::uint32_t type(Words key, bool constant = false);

    std::uint32_t next_id_{1};
    std::set<std::uint32_t> capabilities_;
    std::set<std::string> extensions_;
    std::uint32_t glsl_id_{};
    Words entry_points_, execution_modes_, debug_, annotations_, globals_, code_, function_variables_;
    std::map<Words, std::uint32_t> types_;
    std::set<std::pair<std::uint32_t, std::uint32_t>> decorated_;
};

} // namespace alchemy::msl::spv
