// Image-based lighting: the sky and the ground baked into cube maps that the lit
// shaders read for their ambient light and reflections (fly_ibl in fly_common.glsl).
// Driven by src/Engine/RL/rl_ibl.cpp, which redraws them when the light changes.
//
//   ibl_norm        1x1: how much to scale the raw sky by so a surface facing up gets
//                   exactly the scene's ambientSky (the tuned brightness of the sky
//                   light, clouds and the Ambient setting included). The cube keeps
//                   the sky's shape, the scene keeps its overall level.
//   ibl_capture     the surroundings' light in every direction: the sky (the
//                   atmosphere's sky-view table, or a sky of one colour) above,
//                   the ground's light below. No sun disc: the sun is a light of its own.
//   ibl_irradiance  the light on a surface facing each direction (diffuse).
//   ibl_prefilter   the capture blurred by a GGX lobe, one roughness per mip (specular).
//   ibl_brdf        the split-sum table: (scale, bias) on f0 by (n.v, roughness).
//
// The cube passes draw all six faces at once (one colour attachment per face, the
// "6" programs); the "1" programs draw one face, for GPUs with fewer than six.
//
// Face texels are addressed with gl_FragCoord: row 0 of an attachment is row 0 of
// the image (and t = 0 of a cube face) on every backend, so no flips are needed.
@module ibl
@include fly_common.glsl
@include fly_atmosphere.glsl

@vs vs_fullscreen
@include_block fly_fullscreen_vs
@end

@block ibl_common
const float IBL_PI = 3.14159265358979;
in vec2 uv;     // from fly_fullscreen_vs; unused (texels are addressed by gl_FragCoord)
in vec2 ndc;

// The direction through texel position st (0..1) of cube face `face` (+X, -X, +Y, -Y, +Z, -Z),
// in the cube-map convention every API shares.
vec3 CubeDir(int face, vec2 st) {
    vec2 uv = st * 2.0 - 1.0;
    vec3 d;
    if (face == 0) d = vec3(1.0, -uv.y, -uv.x);
    else if (face == 1) d = vec3(-1.0, -uv.y, uv.x);
    else if (face == 2) d = vec3(uv.x, 1.0, uv.y);
    else if (face == 3) d = vec3(uv.x, -1.0, -uv.y);
    else if (face == 4) d = vec3(uv.x, -uv.y, 1.0);
    else d = vec3(-uv.x, -uv.y, -1.0);
    return normalize(d);
}

// A unit basis round n.
void Basis(vec3 n, out vec3 t, out vec3 b) {
    vec3 up = abs(n.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

float RadicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 Hammersley(int i, int n) { return vec2(float(i) / float(n), RadicalInverse(uint(i))); }

// A half vector drawn from the GGX distribution of width a round n.
vec3 SampleGGX(vec2 xi, vec3 n, float a) {
    float phi = 2.0 * IBL_PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    vec3 t, b;
    Basis(n, t, b);
    return normalize(t * (cos(phi) * sinTheta) + b * (sin(phi) * sinTheta) + n * cosTheta);
}
@end

// The sky's own light in direction d, before the scaling ibl_norm works out, and how much of d is sky
// (1) rather than ground (0). The includer's uniform block holds:
//   vec4 envMode;     x = 1 for the atmosphere (skyViewLut), 0 for a sky of one colour; y = overcast 0..1
//   vec4 atPlanet;    the atmosphere's planet (see fly_atmosphere.glsl)
//   vec4 envZenith;   rgb = a sky of one colour: its light straight up
//   vec4 envHorizon;  rgb = ... and at the horizon
@block ibl_sky
const float AT_PI = 3.14159265358979;
@include_block atmo_skyview
vec3 SkyRaw(vec3 d, out float sky) {
    vec3 L;
    if (envMode.x > 0.5) {
        float r = atPlanet.x + max(atPlanet.z, 0.001);
        float rg = atPlanet.x;
        sky = (d.y < 0.0 && r * r * (1.0 - d.y * d.y) < rg * rg) ? 0.0 : 1.0;
        L = textureLod(sampler2D(skyViewLut, skyViewLut_smp), SvUv(d, r), 0.0).rgb;
    } else {
        sky = smoothstep(-0.03, 0.03, d.y);
        L = mix(envHorizon.rgb, envZenith.rgb, sqrt(max(d.y, 0.0)));
    }
    // Without cloud to cover it, an overcast sky is a flat grey (as in sky.glsl).
    float grey = dot(L, vec3(0.2126, 0.7152, 0.0722));
    return mix(L, vec3(grey) * 0.85, 0.7 * envMode.y);
}
@end

@fs fs_norm
layout(binding=0) uniform fs_norm_params {
    vec4 envMode;
    vec4 atPlanet;
    vec4 envZenith;
    vec4 envHorizon;
    vec4 envAmbientSky;     // rgb = the scene's light from the sky on a surface facing up
};
layout(binding=0) uniform texture2D skyViewLut;
layout(binding=0) uniform sampler skyViewLut_smp;
@include_block ibl_common
@include_block ibl_sky
out vec4 fragColor;
void main() {
    // The raw sky's light on a surface facing up: its radiance averaged over the upper half, weighted by cos.
    const int NT = 16, NP = 32;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < NT; ++i) {
        float theta = (float(i) + 0.5) / float(NT) * 0.5 * IBL_PI;
        float ct = cos(theta), st = sin(theta);
        for (int j = 0; j < NP; ++j) {
            float phi = (float(j) + 0.5) / float(NP) * 2.0 * IBL_PI;
            float sky;
            vec3 L = SkyRaw(vec3(st * cos(phi), ct, st * sin(phi)), sky);
            sum += L * sky * ct * st;
        }
    }
    vec3 raw = sum * (IBL_PI / float(NT * NP));
    fragColor = vec4(clamp(envAmbientSky.rgb / max(raw, vec3(1e-6)), vec3(0.0), vec3(1e4)), 1.0);
}
@end
@program ibl_norm vs_fullscreen fs_norm

@block ibl_capture_body
layout(binding=0) uniform fs_capture_params {
    vec4 iblFace;           // x = this mip's face size in pixels, y = the face to draw (single-face program)
    vec4 envMode;
    vec4 atPlanet;
    vec4 envZenith;
    vec4 envHorizon;
    vec4 envGround;         // rgb = the light coming off the ground (the scene's ambientGround)
};
layout(binding=0) uniform texture2D skyViewLut;
layout(binding=0) uniform sampler skyViewLut_smp;
layout(binding=1) uniform texture2D skyScale;
layout(binding=1) uniform sampler skyScale_smp;
@include_block ibl_common
@include_block ibl_sky
vec4 Capture(int face) {
    vec3 d = CubeDir(face, gl_FragCoord.xy / iblFace.x);
    float sky;
    vec3 L = SkyRaw(d, sky);
    vec3 scale = textureLod(sampler2D(skyScale, skyScale_smp), vec2(0.5), 0.0).rgb;
    return vec4(L * scale * sky + envGround.rgb * (1.0 - sky), 1.0);
}
@end

@fs fs_capture6
@include_block ibl_capture_body
layout(location=0) out vec4 face0;
layout(location=1) out vec4 face1;
layout(location=2) out vec4 face2;
layout(location=3) out vec4 face3;
layout(location=4) out vec4 face4;
layout(location=5) out vec4 face5;
void main() {
    face0 = Capture(0); face1 = Capture(1); face2 = Capture(2);
    face3 = Capture(3); face4 = Capture(4); face5 = Capture(5);
}
@end
@program ibl_capture6 vs_fullscreen fs_capture6

@fs fs_capture1
@include_block ibl_capture_body
out vec4 fragColor;
void main() { fragColor = Capture(int(iblFace.y + 0.5)); }
@end
@program ibl_capture1 vs_fullscreen fs_capture1

@block ibl_irradiance_body
layout(binding=0) uniform fs_irradiance_params {
    vec4 iblFace;           // x = face size in pixels, y = the face to draw (single-face program), z = mip of envCube to read
};
layout(binding=0) uniform textureCube envCube;
layout(binding=0) uniform sampler envCube_smp;
@include_block ibl_common
// Light on a surface facing n, in the engine's units (a white Lambert surface reflects exactly this):
// the radiance round n averaged with a cos weight.
vec4 Irradiance(int face) {
    vec3 n = CubeDir(face, gl_FragCoord.xy / iblFace.x);
    vec3 t, b;
    Basis(n, t, b);
    const int NT = 12, NP = 32;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < NT; ++i) {
        float theta = (float(i) + 0.5) / float(NT) * 0.5 * IBL_PI;
        float ct = cos(theta), st = sin(theta);
        for (int j = 0; j < NP; ++j) {
            float phi = (float(j) + 0.5) / float(NP) * 2.0 * IBL_PI;
            vec3 d = t * (st * cos(phi)) + b * (st * sin(phi)) + n * ct;
            sum += textureLod(samplerCube(envCube, envCube_smp), d, iblFace.z).rgb * ct * st;
        }
    }
    return vec4(sum * (IBL_PI / float(NT * NP)), 1.0);
}
@end

@fs fs_irradiance6
@include_block ibl_irradiance_body
layout(location=0) out vec4 face0;
layout(location=1) out vec4 face1;
layout(location=2) out vec4 face2;
layout(location=3) out vec4 face3;
layout(location=4) out vec4 face4;
layout(location=5) out vec4 face5;
void main() {
    face0 = Irradiance(0); face1 = Irradiance(1); face2 = Irradiance(2);
    face3 = Irradiance(3); face4 = Irradiance(4); face5 = Irradiance(5);
}
@end
@program ibl_irradiance6 vs_fullscreen fs_irradiance6

@fs fs_irradiance1
@include_block ibl_irradiance_body
out vec4 fragColor;
void main() { fragColor = Irradiance(int(iblFace.y + 0.5)); }
@end
@program ibl_irradiance1 vs_fullscreen fs_irradiance1

@block ibl_prefilter_body
layout(binding=0) uniform fs_prefilter_params {
    vec4 iblFace;           // x = this mip's face size in pixels, y = the face to draw (single-face program)
    vec4 iblFilter;         // x = roughness of this mip, y = envCube's face size, z = envCube's last mip, w = sample count
};
layout(binding=0) uniform textureCube envCube;
layout(binding=0) uniform sampler envCube_smp;
@include_block ibl_common
// The surroundings seen in a mirror of roughness iblFilter.x, looking straight at it (n = v = r).
// Each sample reads the mip whose texels match the solid angle it stands for, so few samples stay smooth.
vec4 Prefilter(int face) {
    vec3 n = CubeDir(face, gl_FragCoord.xy / iblFace.x);
    float rough = iblFilter.x;
    if (rough <= 0.0) return vec4(textureLod(samplerCube(envCube, envCube_smp), n, 0.0).rgb, 1.0);
    float a = rough * rough;
    int count = int(iblFilter.w);
    float texelSolid = 4.0 * IBL_PI / (6.0 * iblFilter.y * iblFilter.y);
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (int i = 0; i < 128; ++i) {
        if (i >= count) break;
        vec3 h = SampleGGX(Hammersley(i, count), n, a);
        vec3 L = normalize(2.0 * dot(n, h) * h - n);
        float NoL = dot(n, L);
        if (NoL <= 0.0) continue;
        float NoH = max(dot(n, h), 0.0);
        float a2 = a * a;
        float dd = (NoH * a2 - NoH) * NoH + 1.0;
        float D = a2 / (IBL_PI * dd * dd);
        float pdf = D * 0.25;                    // D * NoH / (4 VoH) with v = n
        float sampleSolid = 1.0 / (float(count) * pdf + 1e-4);
        float lod = clamp(0.5 * log2(sampleSolid / texelSolid) + 1.0, 0.0, iblFilter.z);
        sum += textureLod(samplerCube(envCube, envCube_smp), L, lod).rgb * NoL;
        weight += NoL;
    }
    return vec4(sum / max(weight, 1e-4), 1.0);
}
@end

@fs fs_prefilter6
@include_block ibl_prefilter_body
layout(location=0) out vec4 face0;
layout(location=1) out vec4 face1;
layout(location=2) out vec4 face2;
layout(location=3) out vec4 face3;
layout(location=4) out vec4 face4;
layout(location=5) out vec4 face5;
void main() {
    face0 = Prefilter(0); face1 = Prefilter(1); face2 = Prefilter(2);
    face3 = Prefilter(3); face4 = Prefilter(4); face5 = Prefilter(5);
}
@end
@program ibl_prefilter6 vs_fullscreen fs_prefilter6

@fs fs_prefilter1
@include_block ibl_prefilter_body
out vec4 fragColor;
void main() { fragColor = Prefilter(int(iblFace.y + 0.5)); }
@end
@program ibl_prefilter1 vs_fullscreen fs_prefilter1

// The split-sum table (Karis): for a view at n.v = x and roughness y, the scale and bias the
// prefiltered reflection is multiplied by: f0 * r + g.
@fs fs_brdf
layout(binding=0) uniform fs_brdf_params {
    vec4 iblFace;           // x = the table's size in pixels
};
@include_block ibl_common
out vec4 fragColor;
void main() {
    vec2 st = gl_FragCoord.xy / iblFace.x;
    float NoV = max(st.x, 1e-3);
    float rough = st.y;
    float a = max(rough * rough, 1e-3);
    vec3 v = vec3(sqrt(1.0 - NoV * NoV), 0.0, NoV);
    vec3 n = vec3(0.0, 0.0, 1.0);
    float k = a * 0.5;
    float A = 0.0, B = 0.0;
    const int COUNT = 256;
    for (int i = 0; i < COUNT; ++i) {
        vec3 h = SampleGGX(Hammersley(i, COUNT), n, a);
        vec3 L = normalize(2.0 * dot(v, h) * h - v);
        float NoL = max(L.z, 0.0);
        float NoH = max(h.z, 0.0);
        float VoH = max(dot(v, h), 0.0);
        if (NoL > 0.0) {
            float G = (NoV / (NoV * (1.0 - k) + k)) * (NoL / (NoL * (1.0 - k) + k));
            float Gv = G * VoH / max(NoH * NoV, 1e-4);
            float Fc = pow(1.0 - VoH, 5.0);
            A += (1.0 - Fc) * Gv;
            B += Fc * Gv;
        }
    }
    fragColor = vec4(A / float(COUNT), B / float(COUNT), 0.0, 1.0);
}
@end
@program ibl_brdf vs_fullscreen fs_brdf
