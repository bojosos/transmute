#version 460
// Fragment stage (set 1): a [[color(1)]] input (framebuffer fetch) is input
// attachment 1 at binding 224 + 1 (msl::color_input_binding_base).
layout(location = 0) in vec4 color;
layout(location = 0) out vec4 result;
layout(input_attachment_index = 1, set = 1, binding = 225) uniform subpassInput header;
void main() { result = subpassLoad(header).bgra + 0.0 * color; }
