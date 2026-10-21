#version 460
// Vertex stage (set 0): [[stage_in]] attribute 0 from vertex buffer 0,
// per-vertex colors through [[buffer(1)]] (setVertexBytes).
#extension GL_EXT_buffer_reference : require
layout(location = 0) in vec2 position;
layout(location = 0) out vec4 color;
layout(buffer_reference, std430, buffer_reference_align = 16) buffer Colors { vec4 value[]; };
layout(set = 0, binding = 0, std140) uniform ArgumentTable { Colors vertices; Colors colors; } table;
void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    color = table.colors.value[gl_VertexIndex];
}
