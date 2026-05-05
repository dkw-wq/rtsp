#version 450

layout(binding = 0) uniform sampler2D yTexture;
layout(binding = 1) uniform sampler2D uvTexture;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    float y = texture(yTexture, fragTexCoord).r;
    vec2 uv = texture(uvTexture, fragTexCoord).rg - vec2(0.5, 0.5);

    float r = y + 1.5748 * uv.y;
    float g = y - 0.1873 * uv.x - 0.4681 * uv.y;
    float b = y + 1.8556 * uv.x;

    outColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
}
