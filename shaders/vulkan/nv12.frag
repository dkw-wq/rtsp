#version 450

layout(binding = 0) uniform sampler2D yTexture;
layout(binding = 1) uniform sampler2D uvTexture;

layout(push_constant) uniform VideoPushConstants {
    int filterMode;
} pc;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

vec3 applyFilter(int mode, vec3 rgb) {
    if (mode == 1) {
        float gray = dot(rgb, vec3(0.299, 0.587, 0.114));
        return vec3(gray);
    }
    if (mode == 2) {
        return clamp(rgb * vec3(1.08, 1.02, 0.92) + vec3(0.025, 0.01, 0.0), 0.0, 1.0);
    }
    if (mode == 3) {
        return vec3(1.0) - rgb;
    }
    if (mode == 4) {
        return clamp((rgb - vec3(0.5)) * 1.25 + vec3(0.5), 0.0, 1.0);
    }
    if (mode == 5) {
        float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
        return clamp(mix(vec3(luma), rgb, 1.35), 0.0, 1.0);
    }
    return rgb;
}

void main() {
    float y = texture(yTexture, fragTexCoord).r;
    vec2 uv = texture(uvTexture, fragTexCoord).rg - vec2(0.5, 0.5);

    float r = y + 1.5748 * uv.y;
    float g = y - 0.1873 * uv.x - 0.4681 * uv.y;
    float b = y + 1.8556 * uv.x;

    vec3 rgb = clamp(vec3(r, g, b), 0.0, 1.0);
    outColor = vec4(applyFilter(pc.filterMode, rgb), 1.0);
}
