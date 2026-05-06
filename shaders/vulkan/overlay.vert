#version 450

layout(location = 0) in vec2 inPosition;

layout(push_constant) uniform OverlayPushConstants {
    layout(offset = 0) vec2 screenSize;
    layout(offset = 16) vec4 color;
} pc;

void main() {
    vec2 zeroToOne = inPosition / pc.screenSize;
    vec2 clip = zeroToOne * 2.0 - 1.0;
    gl_Position = vec4(clip.x, clip.y, 0.0, 1.0);
}
