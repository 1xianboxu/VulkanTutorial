#version 450

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    vec2 scale;
    vec2 offset;
    vec4 tint;
    float checker;
    float blend;
    float rotDeg;
} pc;

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(texSampler, fragTexCoord) * pc.tint;

    if (pc.blend > 0.5) {
        outColor = tex;
    } else if (pc.checker > 0.5) {
        vec2 cell = floor(gl_FragCoord.xy / 8.0);
        float c = mod(cell.x + cell.y, 2.0);
        vec3 bg = mix(vec3(0.22), vec3(0.32), c);
        outColor = vec4(mix(bg, tex.rgb, tex.a), 1.0);
    } else {
        outColor = vec4(tex.rgb, 1.0);
    }
}
