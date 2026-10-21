// End-to-end compute fixture: run through Vulkan by the msl "vulkan" test
// group and compared with a host reference.
#include <metal_stdlib>
using namespace metal;

constant float offset_bias [[function_constant(0)]];
constant bool use_sin [[function_constant(1)]];

struct Params {
    float scale;
    uint count;
};

// out[i] = scale * x[i] + y[i] + offset_bias (+ sin(x[i]) when use_sin), with a
// short data-dependent loop; partial sums of out per threadgroup through
// threadgroup memory and a global atomic counter of odd results.
kernel void saxpy_reduce(device const float* x [[buffer(0)]],
                         device const float* y [[buffer(1)]],
                         device float* out [[buffer(2)]],
                         constant Params& params [[buffer(3)]],
                         device float* sums [[buffer(4)]],
                         device atomic_uint* odd [[buffer(5)]],
                         threadgroup float* scratch [[threadgroup(0)]],
                         uint gid [[thread_position_in_grid]],
                         uint lid [[thread_position_in_threadgroup]],
                         uint group [[threadgroup_position_in_grid]],
                         uint width [[threads_per_threadgroup]]) {
    float value = 0.0f;
    if (gid < params.count) {
        value = params.scale * x[gid] + y[gid] + offset_bias;
        if (use_sin) value += sin(x[gid]);
        for (uint k = 0; k < (gid & 3u); ++k) value += 0.5f;
        out[gid] = value;
        if ((int(value) & 1) != 0) atomic_fetch_add_explicit(odd, 1u, memory_order_relaxed);
    }
    scratch[lid] = value;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = width / 2; stride > 0; stride >>= 1) {
        if (lid < stride) scratch[lid] += scratch[lid + stride];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lid == 0) sums[group] = scratch[0];
}

// dispatchThreads: with a grid that is not a multiple of the threadgroup
// size: no bounds check here, so only the compiler's grid guard (kernel push
// constants) keeps the threads past the grid from writing.
kernel void grid_guard(device uint* out [[buffer(0)]],
                       uint gid [[thread_position_in_grid]],
                       uint3 grid [[threads_per_grid]]) {
    out[gid] = grid.x * 4096u + gid;
}

// Program-scope constants computed from function constants (ANGLE's
// ANGLESampleMaskWriteEnabled pattern): their values exist only once the
// pipeline is specialized, and one derives from another.
constant float derived_bias = is_function_constant_defined(offset_bias) ? offset_bias * 4.0f : 7.0f;
constant bool derived_flag = use_sin && derived_bias > 0.5f;

kernel void derived_constants(device float* out [[buffer(0)]],
                              uint gid [[thread_position_in_grid]]) {
    out[gid] = derived_flag ? derived_bias + float(gid) : -1.0f;
}
