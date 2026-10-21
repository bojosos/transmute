#version 460
// Fragment stage (set 1): [[texture(0)]] at binding 16, [[sampler(0)]] at
// binding 160; no buffers, so no argument table.
layout(location = 0) in vec4 color;
layout(location = 0) out vec4 result;
layout(set = 1, binding = 16) uniform texture2D image;
layout(set = 1, binding = 160) uniform sampler point_sampler;
void main() { result = texture(sampler2D(image, point_sampler), vec2(0.75, 0.25)) + 0.0 * color; }
