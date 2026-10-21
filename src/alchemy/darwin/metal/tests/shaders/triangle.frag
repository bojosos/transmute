#version 460
// Fragment stage (set 1): [[buffer(0)]] tint through setFragmentBytes.
#extension GL_EXT_buffer_reference : require
layout(location = 0) in vec4 color;
layout(location = 0) out vec4 result;
layout(buffer_reference, std430, buffer_reference_align = 16) buffer Tint { vec4 value; };
layout(set = 1, binding = 0, std140) uniform ArgumentTable { Tint tint; } table;
void main() { result = color * table.tint.value; }
