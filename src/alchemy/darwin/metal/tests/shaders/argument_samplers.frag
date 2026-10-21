#version 460
// Fragment stage (set 1): [[texture(0)]] at binding 16 sampled through
// element 2 of an argument buffer of samplers (binding 192 + 0, 3 samplers).
layout(location = 0) in vec4 color;
layout(location = 0) out vec4 result;
layout(set = 1, binding = 16) uniform texture2D image;
layout(set = 1, binding = 192) uniform sampler samplers[3];
void main() { result = texture(sampler2D(image, samplers[2]), vec2(1.75, 0.25)) + 0.0 * color; }
