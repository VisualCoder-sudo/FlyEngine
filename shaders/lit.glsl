// Lit shader family (gfx::GetLitShader / GetRoadShader / instanced city
// buildings): sunlight, sky and ground ambient, 3x3 PCF shadow map and a subtle
// underwater tint. Ported from the GLSL 330 sources that used to be embedded in
// src/Engine/Graphics.cpp.
//
// Lighting is done in linear light and written to the float scene target; the
// colours a surface is given (vertex colour, tint, texture) are sRGB and are
// linearized here. Fog, tone mapping and the sRGB encoding happen later, in the
// post passes (shaders/post.glsl).
@module lit
@include fly_common.glsl

@block lit_vs_outputs
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;
out vec4 fragShadowCoord;
out vec3 fragWorldPos;
@end

@block lit_vs_inputs
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
@end

@vs vs
@include_block fly_clip
layout(binding=0) uniform vs_params {
    mat4 mvp;
    mat4 matNormal;
    mat4 matModel;
    mat4 lightVP;
};
@include_block lit_vs_inputs
@include_block lit_vs_outputs
void main() {
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    fragShadowCoord = lightVP * matModel * vec4(vertexPosition, 1.0);
    fragWorldPos = vec3(matModel * vec4(vertexPosition, 1.0));
    gl_Position = fly_clip(mvp * vec4(vertexPosition, 1.0));
}
@end

// Road variant: identical outputs plus the height above road elevation, which
// orders the stacked road layers in the fragment depth bias.
@vs vs_road
@include_block fly_clip
layout(binding=0) uniform vs_params {
    mat4 mvp;
    mat4 matNormal;
    mat4 matModel;
    mat4 lightVP;
};
@include_block lit_vs_inputs
@include_block lit_vs_outputs
out float fragLayer;
void main() {
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    fragShadowCoord = lightVP * matModel * vec4(vertexPosition, 1.0);
    fragWorldPos = vec3(matModel * vec4(vertexPosition, 1.0));
    gl_Position = fly_clip(mvp * vec4(vertexPosition, 1.0));
    // Layer = height above the road surface at this spot (written by the city mesh builder in
    // the otherwise-unused texcoord.x), NOT absolute y: raised roads/pads must not be pushed
    // toward the camera by their altitude.
    fragLayer = max(vertexTexCoord.x, 0.0);
}
@end

// Instanced variant: the model matrix comes from the per-instance transform.
// The per-instance tint rides in the (otherwise unused) bottom row of the
// transform, so every colour of a shape shares one draw call; an all-zero tint
// (e.g. editor markers) means "no per-instance tint".
@vs vs_instanced
@include_block fly_clip
layout(binding=0) uniform vs_inst_params {
    mat4 mvp;
    mat4 lightVP;
};
@include_block lit_vs_inputs
in vec4 instanceTransform0;
in vec4 instanceTransform1;
in vec4 instanceTransform2;
in vec4 instanceTransform3;
@include_block lit_vs_outputs
out vec4 fragInst;      // xyz = instance scale (metres), w = 1 for tinted city buildings (0 = plain marker)
out float fragBaseY;    // world y of the building's bottom face
out float fragFound;    // height of the buried foundation / plinth above that face (m): no windows below it
out vec3 fragOrigin;    // the instance's world position (street lamps switch on one by one from a hash of it)
void main() {
    // Built from column vectors rather than by assigning model[i][3]: writing
    // individual elements of a mat4 was miscompiled by at least one OpenGL driver
    // (NVIDIA), which lost the instance's scale whenever a tint was present.
    vec3 instTint = vec3(instanceTransform0.w, instanceTransform1.w, instanceTransform2.w);
    // The matrix's w (m15) is 1 for everything; city buildings put 1 + foundation height there.
    fragFound = max(instanceTransform3.w - 1.0, 0.0);
    mat4 model = mat4(vec4(instanceTransform0.xyz, 0.0),
                      vec4(instanceTransform1.xyz, 0.0),
                      vec4(instanceTransform2.xyz, 0.0),
                      vec4(instanceTransform3.xyz, 1.0));
    // Tint sign encodes the mode: all-zero = plain marker, negative = prop (vertex colours x |tint|, no facade).
    float isBuilding = dot(instTint, instTint) < 1e-6 ? 0.0 : 1.0;
    if (instTint.x < 0.0) { isBuilding = 2.0; instTint = abs(instTint); }
    if (isBuilding < 0.5) instTint = vec3(1.0);
    vec3 instScale = vec3(length(instanceTransform0.xyz), length(instanceTransform1.xyz), length(instanceTransform2.xyz));
    fragInst = vec4(instScale, isBuilding);
    fragOrigin = instanceTransform3.xyz;
    fragBaseY = instanceTransform3.y - instScale.y * 0.5;

    vec4 worldPos = model * vec4(vertexPosition, 1.0);
    fragTexCoord = vertexTexCoord;
    fragColor = vec4(vertexColor.rgb * instTint, vertexColor.a);
    fragNormal = normalize(mat3(model) * vertexNormal);
    fragShadowCoord = lightVP * worldPos;
    fragWorldPos = worldPos.xyz;
    gl_Position = fly_clip(mvp * worldPos);
}
@end

@block lit_fs_common
@include_block fly_rt_uv
@include_block fly_color
layout(binding=0) uniform texture2D texture0;
layout(binding=0) uniform sampler texture0_smp;
layout(binding=1) uniform texture2D shadowMap;
layout(binding=1) uniform sampler shadowMap_smp;
@image_sample_type shadowMap depth
@sampler_type shadowMap_smp comparison
layout(binding=2) uniform texture2D cloudShadowTex;
layout(binding=2) uniform sampler cloudShadowTex_smp;
// The further shadow cascades (see ShadowCalculation); they share shadowMap's sampler.
layout(binding=3) uniform texture2D shadowMap1;
layout(binding=4) uniform texture2D shadowMap2;
layout(binding=5) uniform texture2D shadowMap3;
@image_sample_type shadowMap1 depth
@image_sample_type shadowMap2 depth
@image_sample_type shadowMap3 depth

in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;
in vec4 fragShadowCoord;
in vec3 fragWorldPos;
out vec4 finalColor;

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
float ShadowCalculation(vec3 normal) {
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
        vec4 biasedLightPos = shadowVP[c] * vec4(fragWorldPos + n * (0.05 * coarser), 1.0);
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

// How much of the sun the clouds let through to this point: the clouds' shadow is a texture over the
// ground round the camera (fs_cloud_shadow in clouds.glsl), found by following the sun's direction
// from here up to the height of the cloud base. cloudShadow: xy = the texture's corner (world xz),
// z = 1 / its size, w = the height of the cloud base (0 = no clouds).
float CloudLight() {
    if (cloudShadow.w <= 0.0) return 1.0;
    vec3 toSun = -sunDir.xyz;
    vec2 q = fragWorldPos.xz + toSun.xz * (max(cloudShadow.w - fragWorldPos.y, 0.0) / max(toSun.y, 0.12));
    return textureLod(sampler2D(cloudShadowTex, cloudShadowTex_smp), (q - cloudShadow.xy) * cloudShadow.z, 0.0).r;
}

// baseColor is the surface's sRGB colour; the result is linear light.
vec3 ShadeLitWith(vec3 baseColor, float baseAlpha, out float alpha) {
    vec3 normal = normalize(fragNormal);
    float diffuse = max(dot(normal, -sunDir.xyz), 0.0);
    float shadow = ShadowCalculation(normal);

    // Light from the sky on what faces up, light bounced off the ground on what faces down.
    vec3 amb = mix(ambientGround.rgb, ambientSky.rgb, normal.y * 0.5 + 0.5);
    vec3 lit = (amb + (1.0 - shadow) * diffuse * CloudLight() * sunColor.rgb) * fly_srgb_to_linear(baseColor * colDiffuse.rgb);

    float depthBelow = waterSurfaceY - fragWorldPos.y;
    if (depthBelow > 0.0) {
        float t = clamp(depthBelow * 0.3, 0.0, 1.0);
        vec3 waterTint = vec3(0.6, 0.75, 0.9);
        lit = mix(lit, lit * waterTint, t * 0.25);
        float luma = dot(lit, vec3(0.299, 0.587, 0.114));
        lit = mix(lit, vec3(luma), t * 0.1);
    }
    alpha = baseAlpha * colDiffuse.a;
    return lit;
}

vec3 ShadeLit(out float alpha) {
    vec4 texelColor = texture(sampler2D(texture0, texture0_smp), fragTexCoord);
    return ShadeLitWith(texelColor.rgb * fragColor.rgb, texelColor.a, alpha);
}
@end

// The scene's light, shared by the three programs (gfx::UpdateLighting fills it in):
//   sunDir         xyz = unit vector the sunlight (or moonlight) travels along, w = how many shadow cascades there are (0 = shadows off)
//   shadowVP       world to shadow-map space, one matrix per cascade
//   sunColor       rgb = linear light on a surface that faces the sun
//   ambientSky     rgb = linear light from the sky on a surface that faces up
//   ambientGround  rgb = linear light bounced off the ground on a surface that faces down
//   cloudShadow    where the clouds' shadow texture lies on the world (see CloudLight)
@fs fs
layout(binding=1) uniform fs_params {
    vec4 colDiffuse;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambientSky;
    vec4 ambientGround;
    vec4 cloudShadow;
    float waterSurfaceY;
    mat4 shadowVP[4];
};
@include_block lit_fs_common
void main() {
    float alpha;
    vec3 lit = ShadeLit(alpha);
    finalColor = vec4(lit, alpha);
}
@end

// Night light pools: warm light from the nearest street lamps and car headlights (positions are fed per frame
// by the city: xyz = world position, w = radius), added on top of the lit surface. Included after a fragment
// shader declares nightAmount, lightCount and nightLights. baseColor is sRGB, the result linear.
@block night_glow
vec3 NightGlow(vec3 baseColor, vec3 pos, vec3 n) {
    vec3 sum = vec3(0.0);
    int count = int(lightCount);
    for (int i = 0; i < 32; ++i) {
        if (i >= count) break;
        vec3 d = nightLights[i].xyz - pos;
        float dist = length(d);
        float att = clamp(1.0 - dist / nightLights[i].w, 0.0, 1.0);
        att *= att;
        float facing = max(dot(n, d / max(dist, 0.001)), 0.2);
        sum += vec3(1.0, 0.60, 0.21) * att * facing;
    }
    return fly_srgb_to_linear(baseColor) * sum * nightAmount * 1.9;
}
@end

// Road fragment shader: the shared lit shading plus an exact per-fragment
// world-space depth bias (metres toward the camera), so parallel road layers
// never z-fight and roads win against the near-coplanar ground plane at
// altitude. Window depth follows OpenGL's 0.5*ndc+0.5 on every backend (see
// fly_clip), so the formulas below are backend-independent.
@fs fs_road
layout(binding=1) uniform fs_road_params {
    vec4 colDiffuse;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambientSky;
    vec4 ambientGround;
    vec4 skyColor;      // rgb = the sky's linear light, mirrored by puddles
    vec4 cloudShadow;
    float waterSurfaceY;
    float nightAmount;
    float lightCount;
    vec4 nightLights[32];
    vec4 weather;       // x = wetness 0..1
    vec4 camPos;        // xyz = camera position
    mat4 shadowVP[4];
    mat4 matProjection;
};
@include_block lit_fs_common
@include_block night_glow
in float fragLayer;

float rnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = fract(sin(dot(i, vec2(127.1, 311.7))) * 43758.5453);
    float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(127.1, 311.7))) * 43758.5453);
    float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
    float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    float alpha;
    vec3 lit = ShadeLit(alpha);
    if (weather.x > 0.01) {
        // Wet ground: darker, with puddles that mirror the sky and the sun.
        vec3 n = normalize(fragNormal);
        float puddle = smoothstep(0.32, 0.62, rnoise(fragWorldPos.xz * 0.30) * 0.65 + rnoise(fragWorldPos.xz * 1.3) * 0.35);
        float wet = weather.x * mix(0.6, 1.0, puddle);
        lit *= 1.0 - 0.45 * wet;
        vec3 V = normalize(camPos.xyz - fragWorldPos);
        float fres = pow(1.0 - clamp(dot(n, V), 0.0, 1.0), 4.0);
        float sheen = wet * (0.015 + puddle * (0.025 + 0.5 * fres));
        lit += skyColor.rgb * sheen;
        vec3 R = reflect(sunDir.xyz, n);
        // The sun's glint in a puddle is far brighter than the road around it (the bloom picks it up).
        lit += sunColor.rgb * CloudLight() * pow(max(dot(R, V), 0.0), 90.0) * 5.0 * wet * puddle;
    }
    if (nightAmount > 0.02 && lightCount > 0.5)
        lit += NightGlow(texture(sampler2D(texture0, texture0_smp), fragTexCoord).rgb * fragColor.rgb, fragWorldPos, normalize(fragNormal));
    finalColor = vec4(lit, alpha);
    float roadDepth;
    if (matProjection[3][3] > 0.5) {                                   // orthographic
        float dz = 0.02 + fragLayer * 2.0;
        roadDepth = gl_FragCoord.z + 0.5 * matProjection[2][2] * dz;
    } else {                                                            // perspective
        float B = matProjection[3][2];
        float invW = gl_FragCoord.w;
        float w = 1.0 / max(invW, 1e-6);
        float depthStep = 1.2e-7 * w * w / max(-B, 1e-3);               // metres per depth step
        float dz = max(0.02, depthStep * 3.0) + fragLayer * max(2.0, depthStep * 150.0);
        dz = min(dz, 6.0);
        roadDepth = gl_FragCoord.z + 0.5 * B * dz * invW * invW;
    }
    gl_FragDepth = clamp(roadDepth, 0.0, 1.0);
}
@end

// City building fragment shader: procedural facade (floors, window bays, lit windows, roof and
// base band) instead of the texture, shape-agnostic because it works in world space. Anything
// that is not a tinted building (editor node markers) falls back to the plain lit look.
@fs fs_building
layout(binding=1) uniform fs_building_params {
    vec4 colDiffuse;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambientSky;
    vec4 ambientGround;
    vec4 cloudShadow;
    float waterSurfaceY;
    float nightAmount;      // 0 = day, 1 = night: scales the emissive window / lamp / car light glow
    float lightCount;
    vec4 nightLights[32];
    vec4 weather;           // x = wetness 0..1, y = time (s)
    mat4 shadowVP[4];
};
@include_block lit_fs_common
@include_block night_glow
in vec4 fragInst;
in float fragBaseY;
in float fragFound;
in vec3 fragOrigin;

// Per-lamp random number from the lamp's position (the city computes the same value for the light pool on the road).
float LampHash(vec2 xz) {
    vec2 q = floor(xz * 2.0 + 0.5);
    vec3 p3 = fract(vec3(q.x, q.y, q.x) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
// 0..1: street lamps come on one by one as it gets dark; a few faulty ones flicker.
float LampOn(vec2 xz) {
    float h = LampHash(xz);
    float thr = mix(0.20, 0.62, h);
    float on = smoothstep(thr, thr + 0.12, nightAmount);
    float h2 = fract(h * 17.31 + 0.37);
    if (h2 < 0.10) {
        float k = fract(floor(weather.y * 8.0 + h * 50.0) * 0.61803 + h * 3.7);
        on *= k < 0.3 ? 0.2 : 1.0;
    }
    return on;
}

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// Things that give off light are brighter than anything the sun lights, so they
// stay bright through the tone curve and the bloom spreads their glow.
void main() {
    float alpha;
    vec3 lit;
    vec3 emit = vec3(0.0);   // linear
    vec3 glowBase = fragColor.rgb;
    if (fragInst.w < 0.5) {
        lit = ShadeLit(alpha);
    } else if (fragInst.w > 1.5) {
        lit = ShadeLitWith(fragColor.rgb, 1.0, alpha);   // prop
        if (abs(fragColor.a - 0.9725) < 0.004) emit = fly_srgb_to_linear(fragColor.rgb) * LampOn(fragOrigin.xz) * 9.0;   // street lamp heads
        else if (fragColor.a < 0.99) emit = fly_srgb_to_linear(fragColor.rgb) * nightAmount * 6.0;   // car lights, signs: glow at night
    } else {
        vec3 n = normalize(fragNormal);
        vec3 wall = fragColor.rgb;
        vec3 base = wall;
        if (n.y > 0.7) {
            base = wall * 0.72;                                    // roof
        } else if (n.y > -0.5) {
            vec3 t = normalize(cross(vec3(0.0, 1.0, 0.0), n));
            float u = dot(fragWorldPos, t);
            float y = fragWorldPos.y - fragBaseY;
            float floorH = 3.4;
            const float bayW = 3.2;
            float topEdge = fragInst.y;                            // building height (storeys * floorH + any foundation)
            // A stepped tower marks each tier in its texcoords (-1 - bottom, top, fractions of the height): the floors of
            // a tier are laid out between its own bottom and top, a whole number of them, so windows and slab bands
            // line up with every ledge whatever the building's height or foundation.
            bool tiered = fragTexCoord.x < -0.5;
            float tierBot = 0.0;
            if (tiered) {
                topEdge = fragInst.y * fragTexCoord.y;
                float botF = -1.0 - fragTexCoord.x;
                tierBot = botF < 0.001 ? fragFound : fragInst.y * botF;
                float hT = max(topEdge - tierBot, 0.5);
                floorH = hT / max(floor(hT / floorH + 0.5), 1.0);
            }
            // Floors are counted down from the roof, so the top is never chopped; any foundation sits at the bottom.
            float fy = (topEdge - y) / floorH;
            float floorIdx = floor(fy);
            float fv = fract(fy);
            float bu = u / bayW;
            float bayIdx = floor(bu);
            float fu = fract(bu);
            // Below fragFound the building is a plain plinth (it is mostly buried by a sloped pad: windows there would be
            // cut diagonally by the ground); the first floor and its windows start above it.
            bool plinth = y < fragFound;
            bool groundFloor = y < fragFound + floorH && (!tiered || tierBot <= fragFound + 0.01);
            bool inGlass = !plinth && fu > 0.18 && fu < 0.82 && fv > 0.22 && fv < 0.78;
            if (groundFloor) inGlass = !plinth && fu > 0.1 && fu < 0.9 && fv > 0.12 && fv < 0.7;
            float r = hash21(vec2(bayIdx, floorIdx) + floor(fragBaseY));
            // Shop signs along the ground floor of the tall (downtown) buildings: painted by day, lit at night.
            float yy = y - fragFound;
            bool isSign = false;
            if (!plinth && fragInst.y > 22.0 && groundFloor && yy > 2.4 && yy < 3.3 && fu > 0.08 && fu < 0.92) {
                float sr = hash21(vec2(bayIdx * 1.7 + 3.0, 9.0) + floor(fragBaseY * 0.5));
                if (sr > 0.45) {
                    float pick = fract(sr * 7.31);
                    vec3 sc = pick < 0.2 ? vec3(1.0, 0.22, 0.25) : pick < 0.4 ? vec3(0.2, 0.85, 1.0) : pick < 0.6 ? vec3(1.0, 0.28, 0.8)
                            : pick < 0.8 ? vec3(1.0, 0.72, 0.22) : vec3(0.3, 1.0, 0.5);
                    base = sc * 0.62;
                    // Not much over 1: a brighter sign would lose its colour to the tone curve. The bloom gives it its glow.
                    emit = fly_srgb_to_linear(sc) * nightAmount * 1.25;
                    isSign = true;
                }
            }
            if (isSign) {
            } else if (inGlass) {
                vec3 glass = mix(vec3(0.16, 0.22, 0.30), vec3(0.34, 0.44, 0.56), fv);
                // Lit windows only appear as it gets dark (none in daylight: every window is blue glass), more of them the darker it is.
                if (r > mix(1.001, 0.42, clamp(nightAmount * 1.6, 0.0, 1.0))) {
                    glass = vec3(0.95, 0.82, 0.48) * 0.9;
                    // Rooms are lit to different levels.
                    emit = fly_srgb_to_linear(glass) * nightAmount * (0.9 + 1.7 * fract(r * 13.7));
                }
                base = glass;
            } else {
                float band = smoothstep(0.0, 0.06, fv) * (1.0 - smoothstep(0.94, 1.0, fv));
                base = wall * (0.82 + 0.18 * band);                   // slab edges shade the wall
                if (groundFloor) base *= 0.88;
                if (plinth) base = wall * 0.74;                       // concrete-ish plinth
            }
        }
        lit = ShadeLitWith(base, 1.0, alpha);
        glowBase = base;
    }
    if (nightAmount > 0.02 && lightCount > 0.5 && fragInst.w > 0.5)
        emit += NightGlow(glowBase, fragWorldPos, normalize(fragNormal));
    lit *= 1.0 - 0.16 * weather.x;        // rain-soaked walls and props are a little darker
    finalColor = vec4(lit + emit, alpha);
}
@end

@program lit vs fs
@program lit_road vs_road fs_road
@program lit_instanced vs_instanced fs_building
