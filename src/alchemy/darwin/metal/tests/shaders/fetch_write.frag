#version 460
// Fragment stage writing two color attachments (framebuffer fetch test).
layout(location = 0) in vec4 color;
layout(location = 0) out vec4 result;
layout(location = 1) out vec4 header;
void main() {
    result = vec4(0.0, 0.0, 0.0, 1.0) + 0.0 * color;
    header = vec4(0.25, 0.5, 0.75, 1.0);
}
