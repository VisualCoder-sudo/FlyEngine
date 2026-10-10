// Post-processing: the scene is rendered in linear light into a float target
// (gfx::BeginScene / EndScene, src/Engine/PostFX.cpp) and these passes turn it
// into the picture: exposure, bloom, tone mapping, colour grading, anti-aliasing.
@module post
@include fly_common.glsl

@vs vs_fullscreen
@include_block fly_fullscreen_vs
@end

// Tone curves. Each takes exposed scene-linear light and returns display-linear
// colour in 0..1. src/Engine/PostFX.cpp has the same curves on the CPU (it
// inverts them, so a flat sky colour comes out exactly as picked).
@block tonemap_curves
// 1: hue-preserving shoulder. Colours below the knee are untouched, so things
// look the colour they were given; brighter light rolls off towards white.
vec3 TonemapNeutral(vec3 c) {
    const float knee = 0.76;
    const float desaturation = 0.15;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < knee) return c;
    float d = 1.0 - knee;
    float newPeak = 1.0 - d * d / (peak + d - knee);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(c, vec3(newPeak), g);
}
// 2: ACES filmic (Stephen Hill's fit of the RRT + ODT), pre-scaled so mid grey stays mid grey.
vec3 TonemapACES(vec3 c) {
    const mat3 inMat = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    const mat3 outMat = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
    vec3 v = inMat * (c * 1.7);
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return clamp(outMat * (a / b), 0.0, 1.0);
}
// 3: AgX (Troy Sobotka; minimal fit by Benjamin Wrensch): soft, film-like, bright colours bleach instead of clipping.
vec3 TonemapAgX(vec3 c) {
    const mat3 inMat = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outMat = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    vec3 v = inMat * max(c, vec3(0.0));
    v = clamp(log2(max(v, vec3(1e-10))), minEv, maxEv);
    v = (v - minEv) / (maxEv - minEv);
    vec3 v2 = v * v;
    vec3 v4 = v2 * v2;
    v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
    v = outMat * v;
    return pow(clamp(v, 0.0, 1.0), vec3(2.2));
}
vec3 Tonemap(vec3 c, float op) {
    if (op < 0.5) return clamp(c, 0.0, 1.0);
    if (op < 1.5) return clamp(TonemapNeutral(c), 0.0, 1.0);
    if (op < 2.5) return TonemapACES(c);
    return TonemapAgX(c);
}
@end

// ---------------------------------------------------------------------------
// Exposure: a 1x1 target holding the multiplier applied before the tone curve.
// With eye adaptation it follows the picture's average brightness (from a small
// log-luminance image of the previous frame); otherwise it is the manual value.
// ---------------------------------------------------------------------------
@fs fs_exposure
layout(binding=0) uniform fs_exposure_params {
    vec4 expParams;     // x = manual multiplier, y = adaptation on (1) / off (0), z = blend towards the target this frame (0..1), w = 1: start over
    vec4 expRange;      // x = lowest, y = highest automatic multiplier, z = key (the brightness the average is brought to)
};
layout(binding=0) uniform texture2D lumaTex;        // log2 luminance of the previous frame
layout(binding=0) uniform sampler lumaTex_smp;
layout(binding=1) uniform texture2D prevExposureTex;
layout(binding=1) uniform sampler prevExposureTex_smp;
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;
void main() {
    float manual = expParams.x;
    if (expParams.y < 0.5) { fragColor = vec4(manual, manual, manual, 1.0); return; }
    // Centre-weighted average of the log luminance.
    float sum = 0.0;
    float wsum = 0.0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            vec2 p = (vec2(float(x), float(y)) + 0.5) / 16.0;
            vec2 q = p * 2.0 - 1.0;
            float w = 1.0 - 0.6 * clamp(dot(q, q), 0.0, 1.0);
            sum += textureLod(sampler2D(lumaTex, lumaTex_smp), p, 0.0).r * w;
            wsum += w;
        }
    }
    float avg = exp2(sum / wsum);
    float target = clamp(expRange.z / max(avg, 1e-4), expRange.x, expRange.y) * manual;
    float prev = textureLod(sampler2D(prevExposureTex, prevExposureTex_smp), vec2(0.5), 0.0).r;
    if (expParams.w > 0.5 || !(prev > 0.0) || prev > 1e4) prev = target;
    // Adapt in stops, so brightening and darkening feel alike.
    float e = exp2(mix(log2(prev), log2(target), expParams.z));
    fragColor = vec4(e, e, e, 1.0);
}
@end
@program post_exposure vs_fullscreen fs_exposure

// Log luminance of the scene, written into a small target for the pass above.
@fs fs_luma
@include_block fly_color
layout(binding=0) uniform texture2D srcTex;
layout(binding=0) uniform sampler srcTex_smp;
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;
void main() {
    vec3 c = textureLod(sampler2D(srcTex, srcTex_smp), uv, 0.0).rgb;
    float l = log2(clamp(fly_luma(c), 1e-4, 64.0));
    fragColor = vec4(l, l, l, 1.0);
}
@end
@program post_luma vs_fullscreen fs_luma

// ---------------------------------------------------------------------------
// Final pass: exposure, bloom, tone curve, grading, vignette, sRGB encoding.
// ---------------------------------------------------------------------------
@fs fs_tonemap
@include_block fly_color
@include_block tonemap_curves
layout(binding=0) uniform fs_tonemap_params {
    vec4 tmParams;      // x = tone curve (0 none, 1 neutral, 2 ACES, 3 AgX), y = bloom strength, z = vignette, w = film grain
    vec4 tmGrade;       // x = contrast, y = saturation, z = temperature (-1 cool .. 1 warm), w = time (s)
    vec4 tmTexel;       // xy = 1 / source size
};
layout(binding=0) uniform texture2D sceneTex;
layout(binding=0) uniform sampler sceneTex_smp;
layout(binding=1) uniform texture2D bloomTex;
layout(binding=1) uniform sampler bloomTex_smp;
layout(binding=2) uniform texture2D exposureTex;
layout(binding=2) uniform sampler exposureTex_smp;
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;

float Hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec3 c = textureLod(sampler2D(sceneTex, sceneTex_smp), uv, 0.0).rgb;
    if (tmParams.y > 0.0) {
        vec3 bloom = textureLod(sampler2D(bloomTex, bloomTex_smp), uv, 0.0).rgb;
        c = mix(c, bloom, tmParams.y);
    }
    c *= textureLod(sampler2D(exposureTex, exposureTex_smp), vec2(0.5), 0.0).r;
    // White balance: warm pushes red up and blue down.
    c *= vec3(1.0 + 0.12 * tmGrade.z, 1.0, 1.0 - 0.12 * tmGrade.z);
    c = Tonemap(max(c, vec3(0.0)), tmParams.x);
    // Grading on the display-linear picture.
    float l = fly_luma(c);
    c = mix(vec3(l), c, tmGrade.y);
    c = mix(vec3(0.18), c, tmGrade.x);
    float r2 = dot(ndc, ndc);
    c *= 1.0 - tmParams.z * smoothstep(0.35, 1.9, r2);
    c = fly_linear_to_srgb(clamp(c, 0.0, 1.0));
    // Half a step of noise hides banding in smooth gradients (sky, fog); grain adds more on purpose.
    vec2 px = uv / tmTexel.xy;
    float n = Hash12(px + fract(tmGrade.w) * 61.0) + Hash12(px * 1.37 + 17.0 + fract(tmGrade.w * 1.7) * 43.0) - 1.0;
    c += n * (0.5 / 255.0 + tmParams.w * 0.04);
    fragColor = vec4(c, 1.0);
}
@end
@program post_tonemap vs_fullscreen fs_tonemap
