#version 450

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    vec2 scale;
    vec2 offset;
    vec4 tint;
    float checker;
    float blend;
    float rotDeg;
    float sharpUpscale;
} pc;

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

// 放大时采用边缘保持的锐利双线性采样。普通双线性在一个完整源像素宽度
// 内混色，3x 以上会让卡通线条明显发糊；这里用 smoothstep 收窄过渡区，
// 不产生双三次卷积的振铃/柔化。所有混合都在预乘 Alpha 空间进行，透明
// 像素中隐藏的 RGB 不会污染轮廓。
vec4 sampleSharpUpscalePreserveAlpha(sampler2D tex, vec2 uv)
{
    ivec2 texSize = textureSize(tex, 0);
    vec2 tc = uv * vec2(texSize) - 0.5;
    vec2 f = fract(tc);
    ivec2 base = ivec2(floor(tc));

    ivec2 p00 = clamp(base,                         ivec2(0), texSize - 1);
    ivec2 p10 = clamp(base + ivec2(1, 0),          ivec2(0), texSize - 1);
    ivec2 p01 = clamp(base + ivec2(0, 1),          ivec2(0), texSize - 1);
    ivec2 p11 = clamp(base + ivec2(1, 1),          ivec2(0), texSize - 1);

    vec4 c00 = texelFetch(tex, p00, 0);
    vec4 c10 = texelFetch(tex, p10, 0);
    vec4 c01 = texelFetch(tex, p01, 0);
    vec4 c11 = texelFetch(tex, p11, 0);
    c00.rgb *= c00.a;
    c10.rgb *= c10.a;
    c01.rgb *= c01.a;
    c11.rgb *= c11.a;

    // 让重采样过渡始终约占 1 个屏幕像素：普通双线性的过渡宽度
    // 会随 3x/8x 放大成 3/8 个屏幕像素，看起来像持续加重的模糊。
    // smoothstep 保持抗锯齿和连续性，且结果不会越出源颜色范围。
    float zoom = max(pc.sharpUpscale, 1.0);
    float halfWidth = 0.5 / zoom;
    vec2 sharpF = smoothstep(vec2(0.5 - halfWidth),
                             vec2(0.5 + halfWidth), f);
    vec4 top = mix(c00, c10, sharpF.x);
    vec4 bottom = mix(c01, c11, sharpF.x);
    vec4 result = mix(top, bottom, sharpF.y);

    result.a = clamp(result.a, 0.0, 1.0);
    result.rgb = clamp(result.rgb, vec3(0.0), vec3(result.a));
    if (result.a > 1e-5)
        result.rgb /= result.a;
    else
        result.rgb = vec3(0.0);
    return result;
}

void main() {
    vec4 tex = (pc.sharpUpscale > 0.5
                    ? sampleSharpUpscalePreserveAlpha(texSampler, fragTexCoord)
                    : texture(texSampler, fragTexCoord)) * pc.tint;

    if (pc.blend > 0.5) {
        outColor = tex;
    } else if (pc.checker > 0.5) {
        vec2 cell = floor(gl_FragCoord.xy / 8.0);
        float c = mod(cell.x + cell.y, 2.0);
        vec3 bg = mix(vec3(0.78), vec3(0.90), c);
        outColor = vec4(mix(bg, tex.rgb, tex.a), 1.0);
    } else {
        // 即使调用方没有请求棋盘格，也绝不能把透明像素中隐藏的 RGB
        // 当作不透明颜色显示出来。白底是图片浏览器更安全的回退行为。
        outColor = vec4(mix(vec3(1.0), tex.rgb, tex.a), 1.0);
    }
}
