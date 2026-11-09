#include <metal_stdlib>
using namespace metal;

// Compiler breadth (M3), in the shapes a large compute renderer's kernel
// library uses: enumerators as function constant indices, MSL 3.1
// program-scope builtins (also hidden by a parameter of the same name), entry
// parameters without index attributes, `__restrict` references, members of
// class template specializations qualified with an address space, `constant`
// member functions called through constant references, an OpenCL builtin name
// used as a function name, simd_vote::vote_t, and float compare-exchange.

enum {
    Constant_Dummy,
    Constant_Scale,
    Constant_Offset = 7,
    Constant_Flags,
};

constant float scale_value [[function_constant(Constant_Scale)]];
constant int offset_value [[function_constant(Constant_Offset)]];
constant uint flag_value [[function_constant(Constant_Flags)]];

const uint global_id [[thread_position_in_grid]];
const uint lane [[thread_index_in_simdgroup]];
const ushort local_size [[threads_per_threadgroup]];

struct Params {
    uint count;
    float bias;
    float table[4];
};

struct Packed {
    device float* values;
    device float* values_fn() constant { return values; }
};

struct Context {
    constant Params& params;
    constant Packed* packed;
    Context(constant Params& p, constant Packed* k) : params(p), packed(k) {}
    float bias() const { return params.bias; }
};

template<typename T, uint N> struct Leaf;
class Half {};
class Full {};
template<uint N> struct Leaf<Half, N> {
    float data[N];
    float value(uint i) const device { return data[i] * 0.5f; }
};
template<uint N> struct Leaf<Full, N> {
    float data[N];
    float value(uint i) const device { return data[i]; }
};

inline float to_global(float x) { return x + 1.0f; }

// A parameter named like a program-scope builtin hides it.
inline uint scaled(const uint global_id, const uint factor) { return global_id * factor; }

inline float table_value(constant float* table, uint i) { return table[i & 3u]; }

kernel void breadth(device float* out,
                    constant Params& __restrict params,
                    constant Packed* packed,
                    device Leaf<Half, 4>* half_leaf,
                    device Leaf<Full, 4>* full_leaf,
                    device atomic_float* total [[buffer(5)]]) {
    Context context(params, packed);
    if (global_id >= context.params.count) return;
    const float leaf = half_leaf->value(global_id & 3u) + full_leaf->value(global_id & 3u);
    const uint votes = uint((simd_vote::vote_t)simd_ballot(lane < 2u));
    float sum = to_global(float(scaled(global_id, 2u))) + context.bias() + leaf;
    sum += table_value(context.params.table, global_id) + float(local_size & 1u) + float(votes & 1u);
    if (is_function_constant_defined(scale_value)) sum *= scale_value;
    if (is_function_constant_defined(offset_value)) sum += float(offset_value);
    if (is_function_constant_defined(flag_value) && (flag_value & 1u)) sum = -sum;
    out[global_id] = sum + packed->values_fn()[0];
    float expected = atomic_load_explicit(total, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(total, &expected, expected + sum, memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}
