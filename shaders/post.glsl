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
// Air and fog between the camera and what it sees, at half resolution: a march
// along each pixel's view ray that adds the light the air scatters towards the
// eye (distant things turn pale and blue) and the fog's (the Fog item, and the
// haze of bad weather). Where the sun's shadow map reaches, each step asks it
// whether the sun gets there, which is what draws shafts of light in fog.
// rgb = light added, a = how much of the scene behind still shows.
// ---------------------------------------------------------------------------
@fs fs_fog
@include_block fly_camera
@include_block fly_rt_uv
layout(binding=0) uniform fs_fog_params {
    vec4 camProj;
    vec4 camDepth;
    mat4 camInvView;    // camera to world
    vec4 fgAirR;        // rgb = Rayleigh scattering per metre at the surface, a = 1 / scale height (per metre)
    vec4 fgAirM;        // rgb = Mie scattering per metre at the surface, a = 1 / scale height
    vec4 fgAirExt;      // rgb = Mie absorption per metre, a = g
    vec4 fgLight;       // rgb = the sun's (or moon's) light after the air above, a = 1 when its shadow map can be asked
    vec4 fgLightDir;    // xyz = direction towards it, w = how far (m) from the camera the shadow map is asked
    vec4 fgMulti;       // rgb = light scattered more than once, per unit of scattering
    vec4 fgFog;         // the Fog item: x = density per metre at height z, y = 1 / the height it thins over (0 = the same at every height), z = base height, w = g
    vec4 fgHaze;        // the haze of bad weather: x = density per metre at the ground, y = 1 / the height it thins over
    vec4 fgFogSun;      // rgb = sunlight the fog scatters
    vec4 fgFogAmb;      // rgb = the fog's own light: the sky's, or (a = 1) the flat sky colour before exposure
    vec4 fgParams;      // x = steps, y = noise offset, z = 1: the sky's pixels are fogged too, w = how far (m) for those
    vec4 fgCloudShadow; // where the clouds' shadow texture lies on the world (see CloudLight in lit.glsl); w = 0: no clouds
    mat4 lightVP;
};
layout(binding=0) uniform texture2D depthTex;
layout(binding=0) uniform sampler depthTex_smp;
@image_sample_type depthTex unfilterable_float
@sampler_type depthTex_smp nonfiltering
layout(binding=1) uniform texture2D shadowMap;
layout(binding=1) uniform sampler shadowMap_smp;
@image_sample_type shadowMap depth
@sampler_type shadowMap_smp comparison
layout(binding=2) uniform texture2D exposureTex;
layout(binding=2) uniform sampler exposureTex_smp;
layout(binding=4) uniform texture2D cloudShadowTex;
layout(binding=4) uniform sampler cloudShadowTex_smp;
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;

// How much of the sun the clouds let through to p: this is what puts the gaps between clouds into the haze as rays.
float CloudLight(vec3 p) {
    if (fgCloudShadow.w <= 0.0) return 1.0;
    vec2 q = p.xz + fgLightDir.xz * (max(fgCloudShadow.w - p.y, 0.0) / max(fgLightDir.y, 0.12));
    return textureLod(sampler2D(cloudShadowTex, cloudShadowTex_smp), (q - fgCloudShadow.xy) * fgCloudShadow.z, 0.0).r;
}

float PhaseHG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// 1 where the sun reaches p, 0 in shadow.
float SunVisible(vec3 p) {
    vec4 lp = lightVP * vec4(p, 1.0);
    vec3 l = lp.xyz / lp.w;
    if (abs(l.x) >= 1.0 || abs(l.y) >= 1.0 || l.z >= 1.0) return 1.0;
    return texture(sampler2DShadow(shadowMap, shadowMap_smp), vec3(fly_rt_uv(l.xy), l.z * 0.5 + 0.5 - 0.0015));
}

void main() {
    float depth = textureLod(sampler2D(depthTex, depthTex_smp), uv, 0.0).r;
    bool sky = depth >= 0.999999;
    // An orthographic view has no distance to fade over.
    if (camDepth.z > 0.5 || (sky && fgParams.z < 0.5)) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }

    vec3 vp = fly_view_pos(ndc, 1.0, camProj, camDepth);
    float tMax = sky ? fgParams.w : fly_view_depth(depth, camDepth) * length(vp);
    vec3 rd = mat3(camInvView) * normalize(vp);
    vec3 ro = camInvView[3].xyz;

    float cosL = dot(rd, fgLightDir.xyz);
    float phR = 3.0 / (16.0 * 3.14159265) * (1.0 + cosL * cosL);
    float phM = PhaseHG(cosL, fgAirExt.a);
    float phF = PhaseHG(cosL, fgFog.w);
    // The sky behind already holds the air's light all the way out; only fog is added in front of it.
    float air = sky ? 0.0 : 1.0;
    vec3 fogAmb = fgFogAmb.rgb;
    if (fgFogAmb.a > 0.5) fogAmb /= max(textureLod(sampler2D(exposureTex, exposureTex_smp), vec2(0.5), 0.0).r, 1e-4);

    // Each pixel starts its steps at a different point, so the steps do not show as bands.
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + fgParams.y, vec2(0.06711056, 0.00583715))));
    int steps = int(fgParams.x);
    float inv = 1.0 / float(steps);
    vec3 L = vec3(0.0);
    vec3 trans = vec3(1.0);
    for (int i = 0; i < 48; ++i) {
        if (i >= steps) break;
        float a0 = float(i) * inv, a1 = float(i + 1) * inv;
        float t0 = a0 * a0 * tMax, t1 = a1 * a1 * tMax;
        float dt = t1 - t0;
        float t = mix(t0, t1, noise);
        vec3 p = ro + rd * t;
        float h = max(p.y, 0.0);
        vec3 sR = fgAirR.rgb * (exp(-h * fgAirR.a) * air);
        float dM = exp(-h * fgAirM.a) * air;
        vec3 sM = fgAirM.rgb * dM;
        float fd = fgFog.x * exp(-max(h - fgFog.z, 0.0) * fgFog.y) + fgHaze.x * exp(-h * fgHaze.y);
        vec3 ext = sR + sM + fgAirExt.rgb * dM + vec3(fd);
        float vis = CloudLight(p);
        if (fgLight.a > 0.5 && t < fgLightDir.w) vis *= SunVisible(p);
        vec3 S = fgLight.rgb * vis * (sR * phR + sM * phM) + fgMulti.rgb * (sR + sM)
               + fd * (fgFogSun.rgb * (vis * phF) + fogAmb);
        vec3 stepT = exp(-ext * dt);
        L += trans * (S - S * stepT) / max(ext, vec3(1e-9));
        trans *= stepT;
    }
    fragColor = vec4(L, dot(trans, vec3(1.0 / 3.0)));
}
@end
@program post_fog vs_fullscreen fs_fog

// ---------------------------------------------------------------------------
// Ambient occlusion, at half resolution: how much of the sky's light a point is
// cut off from by what is near it (the corner where a wall meets the ground, the
// gap between two buildings). Found from the depth buffer alone: each pixel's
// position and surface direction are rebuilt from depth, and a ring of nearby
// pixels is asked how far each one rises above that surface (McGuire's Scalable
// Ambient Obscurance). r = 1 in the open, less where it is hemmed in.
// ---------------------------------------------------------------------------
@fs fs_ao
@include_block fly_camera
@include_block fly_uv_ndc
layout(binding=0) uniform fs_ao_params {
    vec4 camProj;
    vec4 camDepth;
    vec4 aoParams;      // xy = 1 / target size, z = radius (m), w = noise offset
    vec4 aoParams2;     // x = strength
};
layout(binding=0) uniform texture2D aoDepthTex;
layout(binding=0) uniform sampler aoDepthTex_smp;
@image_sample_type aoDepthTex unfilterable_float
@sampler_type aoDepthTex_smp nonfiltering
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;

vec3 ViewPos(vec2 p) {
    float d = textureLod(sampler2D(aoDepthTex, aoDepthTex_smp), p, 0.0).r;
    return fly_view_pos(fly_uv_ndc(p), fly_view_depth(d, camDepth), camProj, camDepth);
}

void main() {
    float depth = textureLod(sampler2D(aoDepthTex, aoDepthTex_smp), uv, 0.0).r;
    if (depth >= 0.999999) { fragColor = vec4(1.0); return; }
    vec3 P = fly_view_pos(ndc, fly_view_depth(depth, camDepth), camProj, camDepth);
    float z = -P.z;

    // The surface's direction, from whichever neighbour on each side lies nearer in depth
    // (the other may be across an edge, on a different surface).
    vec2 t = aoParams.xy;
    vec3 l = ViewPos(uv - vec2(t.x, 0.0)), r = ViewPos(uv + vec2(t.x, 0.0));
    vec3 dn = ViewPos(uv - vec2(0.0, t.y)), up = ViewPos(uv + vec2(0.0, t.y));
    vec3 dx = abs(r.z - P.z) < abs(l.z - P.z) ? r - P : P - l;
    vec3 dy = abs(up.z - P.z) < abs(dn.z - P.z) ? up - P : P - dn;
    // One pixel is too short a baseline to trust: depth is stored in steps, so on a gently tilted
    // surface (a flat floor seen from low down) the one-sided difference is zero on most rows and a
    // whole step on the odd one, and the odd row's tilted normal darkens it into a thin line. Where
    // the pixels three away on both sides lie on the same plane as this one, use those instead.
    {
        vec3 l3 = ViewPos(uv - vec2(3.0 * t.x, 0.0)), r3 = ViewPos(uv + vec2(3.0 * t.x, 0.0));
        vec3 d3 = ViewPos(uv - vec2(0.0, 3.0 * t.y)), u3 = ViewPos(uv + vec2(0.0, 3.0 * t.y));
        float tol = 0.004 * z;
        if (abs(r3.z + l3.z - 2.0 * P.z) < 0.1 * abs(r3.z - l3.z) + tol) dx = (r3 - l3) / 6.0;
        if (abs(u3.z + d3.z - 2.0 * P.z) < 0.1 * abs(u3.z - d3.z) + tol) dy = (u3 - d3) / 6.0;
    }
    vec3 N = normalize(cross(dx, dy));
    if (dot(N, P) > 0.0) N = -N;        // towards the camera

    float radius = aoParams.z;
    // The radius on screen (in UV), kept from growing without limit close to the camera.
    vec2 uvRadius = min(vec2(radius) / (vec2(camProj.x, camProj.y) * 2.0 * z), vec2(0.12));
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + aoParams.w, vec2(0.06711056, 0.00583715))));
    const int K = 10;
    float sum = 0.0;
    float r2 = radius * radius;
    // A surface seen edge-on changes depth a lot from one pixel to the next, and what is rebuilt from
    // depth there is too coarse to trust: the steeper it is, the more a sample must rise above it to count.
    float bias = 0.01 + 0.012 * z + 1.5 * (abs(dx.z) + abs(dy.z));
    for (int i = 0; i < K; ++i) {
        float a = (float(i) + noise) * 2.399963 + noise * 6.2831853;
        float rr = (float(i) + 0.5) / float(K);
        vec3 Q = ViewPos(uv + vec2(cos(a), sin(a)) * rr * uvRadius);
        vec3 v = Q - P;
        float vv = dot(v, v);
        float vn = dot(v, N);
        float f = max(r2 - vv, 0.0);
        sum += f * f * f * max((vn - bias) / (0.01 + vv), 0.0);
    }
    float ao = max(0.0, 1.0 - sum * aoParams2.x * 5.0 / (r2 * r2 * r2 * float(K)));
    fragColor = vec4(ao, ao, ao, 1.0);
}
@end
@program post_ao vs_fullscreen fs_ao

// Smooths the pass above (its ring of samples is turned by noise from pixel to
// pixel), without smearing it across edges in depth.
@fs fs_ao_blur
@include_block fly_camera
layout(binding=0) uniform fs_ao_blur_params {
    vec4 camDepth;
    vec4 abParams;      // xy = 1 / target size
};
layout(binding=0) uniform texture2D aoDepthTex;
layout(binding=0) uniform sampler aoDepthTex_smp;
layout(binding=1) uniform texture2D aoTex;
layout(binding=1) uniform sampler aoTex_smp;
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;
void main() {
    float d = fly_view_depth(textureLod(sampler2D(aoDepthTex, aoDepthTex_smp), uv, 0.0).r, camDepth);
    float sum = 0.0, wsum = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            vec2 p = uv + vec2(float(i), float(j)) * abParams.xy;
            float dS = fly_view_depth(textureLod(sampler2D(aoDepthTex, aoDepthTex_smp), p, 0.0).r, camDepth);
            float w = 1.0 / (0.02 + abs(dS - d) / max(d, 0.01) * 30.0);
            sum += textureLod(sampler2D(aoTex, aoTex_smp), p, 0.0).r * w;
            wsum += w;
        }
    }
    float ao = sum / max(wsum, 1e-5);
    fragColor = vec4(ao, ao, ao, 1.0);
}
@end
@program post_ao_blur vs_fullscreen fs_ao_blur

// ---------------------------------------------------------------------------
// Composite: the lit scene with the air and fog laid over it. The fog was made
// at half resolution; each pixel takes it from the half-size pixels at its own
// depth, so fog does not smear across the edges of things.
// ---------------------------------------------------------------------------
@fs fs_composite
@include_block fly_camera
layout(binding=0) uniform fs_composite_params {
    vec4 camDepth;
    vec4 cpTexel;       // xy = 1 / scene size, zw = 1 / fog size
    vec4 cpCloudTexel;  // xy = 1 / cloud target size
    vec4 cpParams;      // x = 1: fog, y = 1: clouds, z = how strongly ambient occlusion darkens (0 = off)
};
layout(binding=0) uniform texture2D sceneTex;
layout(binding=0) uniform sampler sceneTex_smp;
// (A texture name means one binding slot in the whole file, hence not "depthTex" again.)
layout(binding=1) uniform texture2D cpDepthTex;
layout(binding=1) uniform sampler cpDepthTex_smp;
@image_sample_type cpDepthTex unfilterable_float
@sampler_type cpDepthTex_smp nonfiltering
layout(binding=2) uniform texture2D fogTex;
layout(binding=2) uniform sampler fogTex_smp;
layout(binding=3) uniform texture2D cloudTex;
layout(binding=3) uniform sampler cloudTex_smp;
layout(binding=4) uniform texture2D occlusionTex;
layout(binding=4) uniform sampler occlusionTex_smp;
@include_block fly_color
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;

// A smaller target's value for this pixel, taken from those of its four nearest pixels that
// are at this pixel's depth (texel = 1 / the smaller target's size).
vec4 AtDepth(texture2D tex, vec2 texel, float d) {
    vec2 fpos = uv / texel - 0.5;
    vec2 base = floor(fpos);
    vec2 f = fpos - base;
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            vec2 tuv = (base + vec2(float(i), float(j)) + 0.5) * texel;
            float dS = fly_view_depth(textureLod(sampler2D(cpDepthTex, cpDepthTex_smp), tuv, 0.0).r, camDepth);
            float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            w = (w + 0.02) / (0.02 + abs(dS - d) / max(d, 0.01) * 24.0);
            sum += textureLod(sampler2D(tex, fogTex_smp), tuv, 0.0) * w;
            wsum += w;
        }
    }
    return sum / max(wsum, 1e-5);
}

void main() {
    vec3 c = textureLod(sampler2D(sceneTex, sceneTex_smp), uv, 0.0).rgb;
    float rawDepth = textureLod(sampler2D(cpDepthTex, cpDepthTex_smp), uv, 0.0).r;
    float d = fly_view_depth(rawDepth, camDepth);
    if (cpParams.z > 0.0 && rawDepth < 0.999999) {
        // Occlusion cuts off the sky's light, not the sun's: what the sun lights brightly is darkened less.
        float ao = AtDepth(occlusionTex, cpTexel.zw, d).r;
        float lit = clamp(fly_luma(c) * 0.7, 0.0, 0.75);
        c *= mix(1.0, ao, cpParams.z * (1.0 - lit));
    }
    if (cpParams.y > 0.5) {
        vec4 cloud = AtDepth(cloudTex, cpCloudTexel.xy, d);
        c = c * cloud.a + cloud.rgb;
    }
    if (cpParams.x > 0.5) {
        vec4 fog = AtDepth(fogTex, cpTexel.zw, d);
        c = c * fog.a + fog.rgb;
    }
    fragColor = vec4(c, 1.0);
}
@end
@program post_composite vs_fullscreen fs_composite

// ---------------------------------------------------------------------------
// Temporal anti-aliasing. Each frame is drawn shifted by a different fraction of
// a pixel, and this pass blends it into the frames before it, which over a few
// frames is as good as having drawn every pixel many times over: edges turn
// smooth, and the grain of the half-size passes (clouds, fog) averages away.
// Where the camera has moved, the earlier picture is looked up where each point
// was then (from its depth); and it is held to the range of colours round the
// pixel now, so what has since been uncovered or has moved does not leave a ghost.
// ---------------------------------------------------------------------------
@fs fs_taa
@include_block fly_camera
@include_block fly_rt_uv
@include_block fly_uv_ndc
@include_block fly_color
layout(binding=0) uniform fs_taa_params {
    vec4 camProj;
    vec4 camDepth;
    mat4 camInvView;
    mat4 taPrevViewProj;    // the previous frame's world-to-clip matrix, without its shift
    vec4 taParams;          // xy = 1 / size, z = the new frame's share, w = 1: there is an earlier picture
    vec4 taJitter;          // xy = this frame's shift (clip-space units)
};
layout(binding=0) uniform texture2D taCurTex;
layout(binding=0) uniform sampler taCurTex_smp;
layout(binding=1) uniform texture2D taHistoryTex;
layout(binding=1) uniform sampler taHistoryTex_smp;
layout(binding=2) uniform texture2D taDepthTex;
layout(binding=2) uniform sampler taDepthTex_smp;
@image_sample_type taDepthTex unfilterable_float
@sampler_type taDepthTex_smp nonfiltering
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;

// Bright values are squeezed before blending, or one sparkling pixel would outweigh all its neighbours.
vec3 Squeeze(vec3 c) { return c / (1.0 + fly_luma(c)); }
vec3 Unsqueeze(vec3 c) { return c / max(1.0 - fly_luma(c), 1e-4); }

void main() {
    vec2 t = taParams.xy;
    vec3 cur = max(textureLod(sampler2D(taCurTex, taCurTex_smp), uv, 0.0).rgb, vec3(0.0));
    if (taParams.w < 0.5) { fragColor = vec4(cur, 1.0); return; }

    vec3 c = Squeeze(cur);
    // The colours round this pixel now: their range, and their mean and spread (for variance clipping).
    vec3 lo = c, hi = c, m1 = c, m2 = c * c;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            if (i == 0 && j == 0) continue;
            vec3 s = Squeeze(max(textureLod(sampler2D(taCurTex, taCurTex_smp), uv + vec2(float(i), float(j)) * t, 0.0).rgb, vec3(0.0)));
            lo = min(lo, s);
            hi = max(hi, s);
            m1 += s;
            m2 += s * s;
        }
    }
    m1 /= 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - m1 * m1, vec3(0.0)));

    // The nearest of this pixel and its neighbours decides the motion, so the edge of a near thing moves with it.
    float dBest = textureLod(sampler2D(taDepthTex, taDepthTex_smp), uv, 0.0).r;
    vec2 uvBest = uv;
    for (int k = 0; k < 4; ++k) {
        vec2 p = uv + vec2(k < 2 ? -1.0 : 1.0, (k & 1) == 0 ? -1.0 : 1.0) * t;
        float d = textureLod(sampler2D(taDepthTex, taDepthTex_smp), p, 0.0).r;
        if (d < dBest) { dBest = d; uvBest = p; }
    }
    vec2 ndcB = fly_uv_ndc(uvBest);
    float viewDepth = dBest >= 0.999999 ? camDepth.w : fly_view_depth(dBest, camDepth);
    vec3 world = (camInvView * vec4(fly_view_pos(ndcB, viewDepth, camProj, camDepth), 1.0)).xyz;
    vec4 prevClip = taPrevViewProj * vec4(world, 1.0);
    vec2 histNdc = ndc - ((ndcB - taJitter.xy) - prevClip.xy / max(prevClip.w, 1e-5));
    if (prevClip.w <= 0.0 || abs(histNdc.x) >= 1.0 || abs(histNdc.y) >= 1.0) { fragColor = vec4(cur, 1.0); return; }

    // The earlier picture, read with a bicubic (Catmull-Rom) filter: a plain bilinear read blurs it a little
    // more every frame, which is what makes temporal anti-aliasing look soft.
    vec2 huv = fly_rt_uv(histNdc);
    vec2 pos = huv / t;
    vec2 cc = floor(pos - 0.5) + 0.5;
    vec2 f = pos - cc;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t12 = (cc + w2 / w12) * t, t0 = (cc - 1.0) * t, t3 = (cc + 2.0) * t;
    vec3 h = textureLod(sampler2D(taHistoryTex, taHistoryTex_smp), vec2(t12.x, t0.y), 0.0).rgb * (w12.x * w0.y)
           + textureLod(sampler2D(taHistoryTex, taHistoryTex_smp), vec2(t0.x, t12.y), 0.0).rgb * (w0.x * w12.y)
           + textureLod(sampler2D(taHistoryTex, taHistoryTex_smp), vec2(t12.x, t12.y), 0.0).rgb * (w12.x * w12.y)
           + textureLod(sampler2D(taHistoryTex, taHistoryTex_smp), vec2(t3.x, t12.y), 0.0).rgb * (w3.x * w12.y)
           + textureLod(sampler2D(taHistoryTex, taHistoryTex_smp), vec2(t12.x, t3.y), 0.0).rgb * (w12.x * w3.y);
    h /= (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
    vec3 hist = Squeeze(max(h, vec3(0.0)));

    // Variance clipping: the earlier colour is pulled along the line to the mean of the neighbourhood until it is
    // inside the box that neighbourhood spans (mean +- 1.25 spreads, and never beyond its own range). This lets
    // through a change that is plausible and rejects one that is not (something uncovered, something that moved),
    // with far less ghosting than clamping each channel.
    vec3 bmin = max(m1 - 1.25 * sigma, lo), bmax = min(m1 + 1.25 * sigma, hi);
    vec3 centre = 0.5 * (bmin + bmax), ext = max(0.5 * (bmax - bmin), vec3(1e-4));
    vec3 off = hist - centre;
    float clip = max(max(abs(off.x) / ext.x, abs(off.y) / ext.y), abs(off.z) / ext.z);
    float clipped = clip > 1.0 ? 1.0 : 0.0;
    if (clip > 1.0) hist = centre + off / clip;

    // How much the new frame counts: a tenth when nothing moved, more where the picture is moving (the earlier
    // one is then a worse match) or had to be clipped hard (it was wrong here).
    float motionPx = length((histNdc - ndc) / (2.0 * t));
    float alpha = clamp(taParams.z + 0.25 * clamp(motionPx * 0.5, 0.0, 1.0) + 0.2 * clipped, 0.0, 0.6);
    fragColor = vec4(Unsqueeze(mix(hist, c, alpha)), 1.0);
}
@end
@program post_taa vs_fullscreen fs_taa

// ---------------------------------------------------------------------------
// Bloom: the picture is halved again and again, each step a wide soft blur, and
// then added back up from the smallest; what is very bright spreads its light
// over its surroundings, as it does in an eye or a lens.
// ---------------------------------------------------------------------------
@fs fs_bloom_down
@include_block fly_color
layout(binding=0) uniform fs_bloom_down_params {
    vec4 bdParams;      // xy = 1 / source size, z = 1 on the first step (tames single very bright pixels), w = brightest value let in
};
layout(binding=0) uniform texture2D srcTex;
layout(binding=0) uniform sampler srcTex_smp;
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;

vec3 Tap(vec2 o) {
    return min(textureLod(sampler2D(srcTex, srcTex_smp), uv + o * bdParams.xy, 0.0).rgb, vec3(bdParams.w));
}
// A group's weight falls with its brightness, so one sparkling pixel does not flicker across the whole blur.
vec4 Group(vec3 a, vec3 b, vec3 c, vec3 d) {
    vec3 s = (a + b + c + d) * 0.25;
    float w = 1.0 / (1.0 + fly_luma(s));
    return vec4(s * w, w);
}

void main() {
    vec3 a = Tap(vec2(-2.0, 2.0)), b = Tap(vec2(0.0, 2.0)), c = Tap(vec2(2.0, 2.0));
    vec3 d = Tap(vec2(-2.0, 0.0)), e = Tap(vec2(0.0, 0.0)), f = Tap(vec2(2.0, 0.0));
    vec3 g = Tap(vec2(-2.0, -2.0)), h = Tap(vec2(0.0, -2.0)), i = Tap(vec2(2.0, -2.0));
    vec3 j = Tap(vec2(-1.0, 1.0)), k = Tap(vec2(1.0, 1.0)), l = Tap(vec2(-1.0, -1.0)), m = Tap(vec2(1.0, -1.0));
    vec3 r;
    if (bdParams.z > 0.5) {
        vec4 s = Group(j, k, l, m) * 0.5 + (Group(a, b, d, e) + Group(b, c, e, f) + Group(d, e, g, h) + Group(e, f, h, i)) * 0.125;
        r = s.rgb / max(s.a, 1e-5);
    } else {
        r = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    fragColor = vec4(max(r, vec3(0.0)), 1.0);
}
@end
@program post_bloom_down vs_fullscreen fs_bloom_down

// Blended (by its alpha) over the next larger step.
@fs fs_bloom_up
layout(binding=0) uniform fs_bloom_up_params {
    vec4 buParams;      // xy = 1 / source size, z = how much of the smaller step goes into the larger
};
layout(binding=0) uniform texture2D srcTex;
layout(binding=0) uniform sampler srcTex_smp;
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;
void main() {
    vec2 t = buParams.xy;
    vec3 r = textureLod(sampler2D(srcTex, srcTex_smp), uv, 0.0).rgb * 4.0;
    r += (textureLod(sampler2D(srcTex, srcTex_smp), uv + vec2(-t.x, 0.0), 0.0).rgb + textureLod(sampler2D(srcTex, srcTex_smp), uv + vec2(t.x, 0.0), 0.0).rgb
        + textureLod(sampler2D(srcTex, srcTex_smp), uv + vec2(0.0, -t.y), 0.0).rgb + textureLod(sampler2D(srcTex, srcTex_smp), uv + vec2(0.0, t.y), 0.0).rgb) * 2.0;
    r += textureLod(sampler2D(srcTex, srcTex_smp), uv + vec2(-t.x, -t.y), 0.0).rgb + textureLod(sampler2D(srcTex, srcTex_smp), uv + vec2(t.x, -t.y), 0.0).rgb
       + textureLod(sampler2D(srcTex, srcTex_smp), uv + vec2(-t.x, t.y), 0.0).rgb + textureLod(sampler2D(srcTex, srcTex_smp), uv + vec2(t.x, t.y), 0.0).rgb;
    fragColor = vec4(r / 16.0, buParams.z);
}
@end
@program post_bloom_up vs_fullscreen fs_bloom_up

// ---------------------------------------------------------------------------
// Exposure: a 1x1 target holding the multiplier applied before the tone curve.
// With eye adaptation it follows the picture's average brightness (from a small
// log-luminance image of the previous frame); otherwise it is the manual value.
// ---------------------------------------------------------------------------
@fs fs_exposure
layout(binding=0) uniform fs_exposure_params {
    vec4 expParams;     // x = manual multiplier, y = adaptation on (1) / off (0), z = blend towards the target this frame (0..1), w = 1: start over
    vec4 expRange;      // x = lowest, y = highest automatic multiplier, z = key (the brightness the average is brought towards), w = how far it is brought there (0 = not at all, 1 = fully)
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
    // The eye only partly makes up for a dark or a bright view: night stays darker than day.
    float target = clamp(pow(expRange.z / max(avg, 1e-4), expRange.w), expRange.x, expRange.y) * manual;
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
    // Only the grain moves: without it a still view is the same picture every frame.
    vec2 px = uv / tmTexel.xy;
    float t = tmParams.w > 0.0 ? tmGrade.w : 0.0;
    float n = Hash12(px + fract(t) * 61.0) + Hash12(px * 1.37 + 17.0 + fract(t * 1.7) * 43.0) - 1.0;
    c += n * (0.5 / 255.0 + tmParams.w * 0.04);
    fragColor = vec4(c, 1.0);
}
@end
@program post_tonemap vs_fullscreen fs_tonemap

// ---------------------------------------------------------------------------
// FXAA (after Timothy Lottes): smooths stair-stepped edges in the finished
// picture by blending along the edge it finds from the brightness of each
// pixel's neighbours.
// ---------------------------------------------------------------------------
@fs fs_fxaa
layout(binding=0) uniform fs_fxaa_params {
    vec4 fxTexel;       // xy = 1 / picture size
};
layout(binding=0) uniform texture2D srcTex;
layout(binding=0) uniform sampler srcTex_smp;
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;
void main() {
    const vec3 lumaW = vec3(0.299, 0.587, 0.114);
    vec2 t = fxTexel.xy;
    vec3 rgbM = textureLod(sampler2D(srcTex, srcTex_smp), uv, 0.0).rgb;
    float lumaNW = dot(textureLod(sampler2D(srcTex, srcTex_smp), uv + vec2(-1.0, -1.0) * t, 0.0).rgb, lumaW);
    float lumaNE = dot(textureLod(sampler2D(srcTex, srcTex_smp), uv + vec2(1.0, -1.0) * t, 0.0).rgb, lumaW);
    float lumaSW = dot(textureLod(sampler2D(srcTex, srcTex_smp), uv + vec2(-1.0, 1.0) * t, 0.0).rgb, lumaW);
    float lumaSE = dot(textureLod(sampler2D(srcTex, srcTex_smp), uv + vec2(1.0, 1.0) * t, 0.0).rgb, lumaW);
    float lumaM = dot(rgbM, lumaW);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    // Flat areas are left alone.
    if (lumaMax - lumaMin < max(0.03, lumaMax * 0.1)) { fragColor = vec4(rgbM, 1.0); return; }

    vec2 dir = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), (lumaNW + lumaSW) - (lumaNE + lumaSE));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 / 8.0), 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * t;
    vec3 rgbA = 0.5 * (textureLod(sampler2D(srcTex, srcTex_smp), uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb
                     + textureLod(sampler2D(srcTex, srcTex_smp), uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (textureLod(sampler2D(srcTex, srcTex_smp), uv - dir * 0.5, 0.0).rgb
                                   + textureLod(sampler2D(srcTex, srcTex_smp), uv + dir * 0.5, 0.0).rgb);
    float lumaB = dot(rgbB, lumaW);
    fragColor = vec4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, 1.0);
}
@end
@program post_fxaa vs_fullscreen fs_fxaa
