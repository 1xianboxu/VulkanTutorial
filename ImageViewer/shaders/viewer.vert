#version 450

// 单位四边形（[-1,1]）经 push constants 变换到目标矩形（NDC）
layout(push_constant) uniform PushConstants {
    vec2 scale;
    vec2 offset;
    vec4 tint;
    float checker;
    float blend;
    float rotDeg;
} pc;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec2 fragTexCoord;

void main() {
    gl_Position = vec4(inPosition * pc.scale + pc.offset, 0.0, 1.0);
    vec2 uv = inTexCoord;
    if (pc.rotDeg != 0.0) {
        float r = radians(pc.rotDeg);
        float cs = cos(r), sn = sin(r);
        vec2 c = vec2(0.5);
        uv = c + (uv - c) * mat2(cs, -sn, sn, cs);
    }
    fragTexCoord = uv;
}
