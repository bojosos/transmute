// Loops in the style Dawn's WGSL compiler (Tint) emits for WGSL's loop
// forms (design: Tint's public MSL writer): a `loop` whose `continuing`
// block (with `break if`) is copied into each `continue` path, a uint2
// iteration guard counting down from 0xffffffff, a `for` with continue and
// break, and a `while`. clang -O2 folds the `for` over odd numbers into a
// chain of comparisons that all reach one block, whose phi the
// structurizer's tail duplication copies; a copy that no longer dominates
// its use must be demoted with its original.
#include <metal_stdlib>
using namespace metal;

template<typename T, size_t N>
struct tint_array {
  const constant T& operator[](size_t i) const constant { return elements[i]; }
  device T& operator[](size_t i) device { return elements[i]; }
  const device T& operator[](size_t i) const device { return elements[i]; }
  thread T& operator[](size_t i) thread { return elements[i]; }
  const thread T& operator[](size_t i) const thread { return elements[i]; }
  threadgroup T& operator[](size_t i) threadgroup { return elements[i]; }
  const threadgroup T& operator[](size_t i) const threadgroup { return elements[i]; }
  T elements[N];
};

uint tint_mod_u32(uint lhs, uint rhs) {
  uint const divisor = select(rhs, 1u, (rhs == 0u));
  return (lhs - ((lhs / divisor) * divisor));
}

float4 loops(uint index) {
  float selected = 0.0f;
  switch(tint_mod_u32(index, 5u)) {
    case 0u:
    case 1u: {
      selected = 1.0f;
      break;
    }
    case 4u:
    default: {
      selected = 7.0f;
      break;
    }
    case 2u: {
      selected = 3.0f;
      break;
    }
  }
  uint step = 0u;
  uint sum = 0u;
  {
    uint2 guard = uint2(4294967295u);
    while(true) {
      if (all((guard == uint2(0u)))) {
        break;
      }
      if ((tint_mod_u32(step, 3u) == 0u)) {
        {
          uint const low = (guard.x - 1u);
          guard.x = low;
          guard.y = (guard.y - uint((low == 4294967295u)));
          step = (step + 1u);
          if ((step > index)) { break; }
        }
        continue;
      }
      sum = (sum + step);
      {
        uint const low = (guard.x - 1u);
        guard.x = low;
        guard.y = (guard.y - uint((low == 4294967295u)));
        step = (step + 1u);
        if ((step > index)) { break; }
      }
    }
  }
  uint odd = 0u;
  {
    uint k = 0u;
    while(true) {
      if ((k < 16u)) {
      } else {
        break;
      }
      if ((tint_mod_u32(k, 2u) == 0u)) {
        {
          k = (k + 1u);
        }
        continue;
      }
      if ((k > index)) {
        break;
      }
      odd = (odd + k);
      {
        k = (k + 1u);
      }
    }
  }
  uint root = 0u;
  {
    uint2 guard = uint2(4294967295u);
    while(true) {
      if (all((guard == uint2(0u)))) {
        break;
      }
      if (((root * root) < index)) {
      } else {
        break;
      }
      root = (root + 1u);
      {
        uint const low = (guard.x - 1u);
        guard.x = low;
        guard.y = (guard.y - uint((low == 4294967295u)));
      }
    }
  }
  return float4(selected, float(sum), float(odd), float(root));
}

[[max_total_threads_per_threadgroup(16)]]
kernel void loops_main(uint index [[thread_index_in_threadgroup]], device tint_array<float4, 16>* out [[buffer(0)]]) {
  (*out)[index] = loops(index);
}
