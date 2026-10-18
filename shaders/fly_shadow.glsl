// The sun's shadow cascades for the lit and water programs. Kept apart from fly_common.glsl because
// sokol-shdc registers a file's @image_sample_type lines even when their block is never included.
//
// The program declares the uniforms sunDir and shadowVP (see shaders/lit.glsl) and includes fly_rt_uv
// before this block.
@block fly_shadow
layout(binding=1) uniform texture2D shadowMap;
layout(binding=1) uniform sampler shadowMap_smp;
@image_sample_type shadowMap depth
@sampler_type shadowMap_smp comparison
// The further shadow cascades (see ShadowCalculation); they share shadowMap's sampler.
layout(binding=3) uniform texture2D shadowMap1;
layout(binding=4) uniform texture2D shadowMap2;
layout(binding=5) uniform texture2D shadowMap3;
@image_sample_type shadowMap1 depth
@image_sample_type shadowMap2 depth
@image_sample_type shadowMap3 depth

// 1 = lit, 0 = in shadow, filtered by the hardware.
float ShadowTap(int cascade, vec3 p) {
    if (cascade == 0) return texture(sampler2DShadow(shadowMap, shadowMap_smp), p);
    if (cascade == 1) return texture(sampler2DShadow(shadowMap1, shadowMap_smp), p);
    if (cascade == 2) return texture(sampler2DShadow(shadowMap2, shadowMap_smp), p);
    return texture(sampler2DShadow(shadowMap3, shadowMap_smp), p);
}
vec2 ShadowTexel(int cascade) {
    if (cascade == 0) return 1.0 / vec2(textureSize(sampler2DShadow(shadowMap, shadowMap_smp), 0));
    if (cascade == 1) return 1.0 / vec2(textureSize(sampler2DShadow(shadowMap1, shadowMap_smp), 0));
    if (cascade == 2) return 1.0 / vec2(textureSize(sampler2DShadow(shadowMap2, shadowMap_smp), 0));
    return 1.0 / vec2(textureSize(sampler2DShadow(shadowMap3, shadowMap_smp), 0));
}

// The sun's shadow: 0 = lit, 1 = in shadow.
//
// The shadow maps are cascades: the first is a box of ground round the camera drawn
// in fine detail, each further one a box about three times as wide at the same
// resolution (shadowVP[i] takes a world position into box i; sunDir.w says how many
// there are, 0 = shadows off). A point takes its shadow from the finest box it is in.
float ShadowCalculation(vec3 worldPos, vec3 normal) {
    int count = int(sunDir.w + 0.5);
    // Combined anti-acne scheme (shared by every lit mesh).
    //
    // (B) SLOPE-SCALE DEPTH BIAS - grows as the surface tilts away from the
    //     light, where acne is worst. In depth-buffer units: tuned for the first box's
    //     1..430 m range, and the further boxes' ranges are longer in step with their texels.
    float facing = 1.0 - dot(normal, -sunDir.xyz);
    float bias = max(0.0037 * facing * facing, 0.00093);
    float scale0 = length(vec3(shadowVP[0][0][0], shadowVP[0][1][0], shadowVP[0][2][0]));
    vec3 n = normalize(normal);
    for (int c = 0; c < 4; ++c) {
        if (c >= count) break;
        // (A) NORMAL-BASED BIAS - shift the shadow comparison point along the
        //     surface normal in WORLD space before projecting to light space: 5 cm in
        //     the first box (correct at every camera altitude), more where texels are larger.
        float coarser = scale0 / max(length(vec3(shadowVP[c][0][0], shadowVP[c][1][0], shadowVP[c][2][0])), 1e-9);
        vec4 biasedLightPos = shadowVP[c] * vec4(worldPos + n * (0.05 * coarser), 1.0);
        vec3 ndc = biasedLightPos.xyz / biasedLightPos.w;
        float currentDepth = ndc.z * 0.5 + 0.5;
        // The rim of a box is left to the next one (its taps would reach over the edge).
        float rim = c + 1 < count ? 0.97 : 1.0;
        if (abs(ndc.x) > rim || abs(ndc.y) > rim || currentDepth > 1.0) continue;

        vec2 uv = fly_rt_uv(ndc.xy);
        vec2 texelSize = ShadowTexel(c);
        // 3x3 PCF; each tap is itself a filtered hardware comparison.
        float shadow = 0.0;
        for (int x = -1; x <= 1; ++x) {
            for (int y = -1; y <= 1; ++y) {
                shadow += 1.0 - ShadowTap(c, vec3(uv + vec2(x, y) * texelSize, currentDepth - bias));
            }
        }
        return shadow / 9.0;
    }
    return 0.0;
}
@end
