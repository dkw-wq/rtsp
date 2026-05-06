#version 450

layout(push_constant) uniform OverlayPushConstants {
    layout(offset = 0) vec2 screenSize;
    layout(offset = 16) vec4 color;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = pc.color;
}
