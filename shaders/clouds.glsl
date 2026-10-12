// Volumetric clouds (src/Engine/Clouds.cpp): a layer of cloud between two
// heights, marched along every view ray.
//
// The shape of the cloud comes from three tiling noise textures made on the CPU:
//   weatherTex  2D, tens of kilometres across: r = where cloud is, g = how tall it grows there
//   shapeTex    3D, a few kilometres across: the billows (Perlin-Worley noise)
//   detailTex   3D, a few hundred metres across: the wisps that eat into the billows' edges
// At each step the light reaching that point from the sun is found by a short
// second march towards the sun, and the light a thick cloud passes around inside
// itself is approximated by adding the same thing again, weaker and less
// directional (Wrenninge's octaves).
//
// The layer follows the planet's curve (it meets the horizon instead of going on
// above it for ever): height above the ground is taken as y plus the drop of the
// ground with distance, d^2 / 2R.
@module clouds
@include fly_common.glsl

@vs vs_fullscreen
@include_block fly_fullscreen_vs
@end

@block cloud_shape
float Remap(float v, float lo, float hi, float newLo, float newHi) {
    return newLo + (v - lo) / (hi - lo) * (newHi - newLo);
}

// Height above the ground of a world position, on a round planet.
float CloudAltitude(vec3 p) {
    vec2 d = p.xz - clOrigin.xz;
    return p.y + dot(d, d) * clOrigin.w;
}

// How much cloud there is at p (0 = none). `cheap` leaves the wisps out (light marches, shadows).
float CloudDensity(vec3 p, bool cheap) {
    float h = (CloudAltitude(p) - clLayer.x) / clLayer.y;
    if (h <= 0.0 || h >= 1.0) return 0.0;
    vec4 weather = textureLod(sampler2D(weatherTex, weatherTex_smp), (p.xz + clWind.zw) * clShape.z, 0.0);
    // Coverage: with the setting at 0 nothing passes, at 1 everything does.
    float cover = clamp(Remap(weather.r, 1.0 - clLayer.z * 1.25, 1.0 - clLayer.z * 1.25 + 0.5, 0.0, 1.0), 0.0, 1.0);
    if (cover <= 0.0) return 0.0;
    // A cloud's outline in height: a flat-ish base, a rounded top; `tall` decides how much of the layer it fills.
    float tall = mix(0.35, 1.0, weather.g);
    float profile = smoothstep(0.0, 0.07, h) * (1.0 - smoothstep(tall * 0.55, tall, h));
    if (profile <= 0.0) return 0.0;
    float shape = textureLod(sampler3D(shapeTex, shapeTex_smp), vec3(p.xz + clWind.xy, p.y).xzy * clShape.x, 0.0).r;
    float d = Remap(shape * profile, 1.0 - cover, 1.0, 0.0, 1.0) * cover;
    if (d <= 0.0) return 0.0;
    if (!cheap) {
        float detail = textureLod(sampler3D(detailTex, detailTex_smp), vec3(p.xz + clWind.xy * 1.6, p.y - clWind.x * 0.2).xzy * clShape.y, 0.0).r;
        // Wispy near the base, billowy higher up.
        detail = mix(1.0 - detail, detail, clamp(h * 4.0, 0.0, 1.0));
        d = Remap(d, detail * clShape.w, 1.0, 0.0, 1.0);
    }
    return max(d, 0.0) * clLayer.w;
}

// Where a ray is inside the layer: up to two stretches, of which the first that lies ahead is returned.
// Height along the ray is y0 + b t + a t^2 (the t^2 is the planet curving away).
bool CloudRange(vec3 ro, vec3 rd, out float tEnter, out float tExit) {
    float a = max(dot(rd.xz, rd.xz) * clOrigin.w, 1e-12);
    float b = rd.y;
    float y0 = ro.y;
    float base = clLayer.x, top = clLayer.x + clLayer.y;
    // Below the top between these two (always, if the ray never rises to it... which a curve like this always does).
    float discT = b * b - 4.0 * a * (y0 - top);
    if (discT < 0.0) return false;                 // the whole ray is above the layer
    float sT = sqrt(discT);
    float tT1 = (-b - sT) / (2.0 * a), tT2 = (-b + sT) / (2.0 * a);
    float discB = b * b - 4.0 * a * (y0 - base);
    if (discB <= 0.0) {                            // never under the base
        tEnter = max(tT1, 0.0);
        tExit = tT2;
        return tExit > tEnter;
    }
    float sB = sqrt(discB);
    float tB1 = (-b - sB) / (2.0 * a), tB2 = (-b + sB) / (2.0 * a);
    tEnter = max(tT1, 0.0);
    tExit = tB1;
    if (tExit > tEnter) return true;
    tEnter = max(tB2, 0.0);
    tExit = tT2;
    return tExit > tEnter;
}
@end

// ---------------------------------------------------------------------------
// The clouds as the camera sees them. rgb = their light, a = how much of what is
// behind them still shows.
// ---------------------------------------------------------------------------
@fs fs_clouds
@include_block fly_camera
layout(binding=0) uniform fs_clouds_params {
    vec4 camProj;
    vec4 camDepth;
    mat4 camInvView;
    vec4 clOrigin;      // xyz = camera position, w = 1 / (2 x planet radius in metres)
    vec4 clLayer;       // x = base height (m), y = thickness (m), z = coverage 0..1, w = density (extinction per metre of full cloud)
    vec4 clShape;       // x = 1 / size of the shape noise (m), y = 1 / size of the detail noise, z = 1 / size of the weather map, w = how deep the wisps eat in
    vec4 clWind;        // xy = how far the wind has carried the shapes, zw = the weather map
    vec4 clSun;         // xyz = direction towards the sun (or moon), w = how much the far clouds fade into the sky per metre
    vec4 clSunLight;    // rgb = its light at the height of the clouds
    vec4 clAmbTop;      // rgb = the sky's light on the top of the layer
    vec4 clAmbBottom;   // rgb = the light from below on its underside
    vec4 clParams;      // x = steps, y = steps towards the sun, z = noise offset, w = how far (m) clouds are drawn
    vec4 atPlanet;      // see fly_atmosphere.glsl
};
layout(binding=0) uniform texture2D clDepthTex;
layout(binding=0) uniform sampler clDepthTex_smp;
@image_sample_type clDepthTex unfilterable_float
@sampler_type clDepthTex_smp nonfiltering
layout(binding=1) uniform texture2D weatherTex;
layout(binding=1) uniform sampler weatherTex_smp;
layout(binding=2) uniform texture3D shapeTex;
layout(binding=2) uniform sampler shapeTex_smp;
layout(binding=3) uniform texture3D detailTex;
layout(binding=3) uniform sampler detailTex_smp;
layout(binding=4) uniform texture2D skyViewLut;
layout(binding=4) uniform sampler skyViewLut_smp;
@include_block cloud_shape
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;

const float CL_PI = 3.14159265358979;

float PhaseHG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * CL_PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// The sky-view table's lookup (fly_atmosphere.glsl), for fading far clouds into the sky behind them.
vec3 SkyAt(vec3 d) {
    float r = atPlanet.x + max(atPlanet.z, 0.001);
    float horizon = CL_PI - acos(clamp(sqrt(max(r * r - atPlanet.x * atPlanet.x, 0.0)) / r, 0.0, 1.0));
    float zenith = acos(clamp(d.y, -1.0, 1.0));
    float v;
    if (zenith < horizon) v = clamp(0.5 * (1.0 - sqrt(max(1.0 - zenith / horizon, 0.0))), 0.5 / 144.0, 0.5 - 0.5 / 144.0);
    else v = clamp(0.5 + 0.5 * sqrt(clamp((zenith - horizon) / max(CL_PI - horizon, 1e-4), 0.0, 1.0)), 0.5 + 0.5 / 144.0, 1.0 - 0.5 / 144.0);
    float u = atan(d.z, d.x) / (2.0 * CL_PI);
    return textureLod(sampler2D(skyViewLut, skyViewLut_smp), vec2(u < 0.0 ? u + 1.0 : u, v), 0.0).rgb;
}

// How much cloud lies between p and the sun (optical depth).
float SunDepth(vec3 p) {
    int n = int(clParams.y);
    float stepLen = clLayer.y * 0.045;
    float tau = 0.0;
    float t = stepLen * 0.5;
    for (int i = 0; i < 8; ++i) {
        if (i >= n) break;
        tau += CloudDensity(p + clSun.xyz * t, true) * stepLen;
        stepLen *= 1.7;
        t += stepLen;
    }
    return tau;
}

void main() {
    float depth = textureLod(sampler2D(clDepthTex, clDepthTex_smp), uv, 0.0).r;
    if (camDepth.z > 0.5) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    vec3 vp = fly_view_pos(ndc, 1.0, camProj, camDepth);
    vec3 rd = mat3(camInvView) * normalize(vp);
    vec3 ro = camInvView[3].xyz;
    float tScene = depth >= 0.999999 ? 1e9 : fly_view_depth(depth, camDepth) * length(vp);

    float t0, t1;
    if (!CloudRange(ro, rd, t0, t1)) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    t1 = min(min(t1, tScene), clParams.w);
    // A long, shallow path through the layer is only marched over its near part.
    t1 = min(t1, t0 + clLayer.y * 12.0);
    if (t1 <= t0) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }

    int steps = int(clParams.x);
    float dt = (t1 - t0) / float(steps);
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + clParams.z, vec2(0.06711056, 0.00583715))));
    float cosS = dot(rd, clSun.xyz);
    // Forward scattering makes the bright rim towards the sun, the weaker lobe lights the side away from it.
    float phase = mix(PhaseHG(cosS, 0.72), PhaseHG(cosS, -0.25), 0.3);
    float phase2 = mix(PhaseHG(cosS, 0.36), PhaseHG(cosS, -0.12), 0.3);
    float phase3 = 1.0 / (4.0 * CL_PI);

    vec3 L = vec3(0.0);
    float trans = 1.0;
    float tMean = 0.0, wMean = 0.0;       // where along the ray the cloud is, for the fade into the sky
    float t = t0 + dt * noise;
    for (int i = 0; i < 128; ++i) {
        if (i >= steps || trans < 0.008) break;
        vec3 p = ro + rd * t;
        float d = CloudDensity(p, false);
        if (d > 0.0) {
            float tau = SunDepth(p);
            // Light from the sun: straight through, then the same having bounced once, twice (each weaker, deeper, less directional).
            float sun = exp(-tau) * phase + 0.5 * exp(-tau * 0.5) * phase2 + 0.25 * exp(-tau * 0.25) * phase3;
            // Deep inside, light still seeps down from above: it thins with depth, but far slower than a beam does.
            // This is what shows the lighter and darker patches on the underside of a sheet of cloud.
            float seep = 1.0 / (1.0 + 0.3 * tau);
            sun += 0.07 * seep;
            float h = clamp((CloudAltitude(p) - clLayer.x) / clLayer.y, 0.0, 1.0);
            vec3 amb = mix(clAmbBottom.rgb, clAmbTop.rgb * (0.25 + 0.75 * seep), h) * (0.35 + 0.65 * h);
            vec3 S = (clSunLight.rgb * sun * 4.2 + amb) * d;
            float stepT = exp(-d * dt);
            L += trans * (S - S * stepT) / d;
            wMean += trans * (1.0 - stepT);
            tMean += trans * (1.0 - stepT) * t;
            trans *= stepT;
        }
        t += dt;
    }
    // Far clouds take on the colour of the air in front of them.
    if (wMean > 1e-4) {
        float dist = tMean / wMean;
        float fade = 1.0 - exp(-dist * clSun.w);
        L = mix(L, SkyAt(rd) * (1.0 - trans), fade);
    }
    fragColor = vec4(L, trans);
}
@end
@program clouds_march vs_fullscreen fs_clouds

// ---------------------------------------------------------------------------
// The clouds' shadow on the world: for a square of ground round the camera, how
// much sunlight gets down through the layer (r). A lit surface finds its place
// in it by following the sun's direction up to the height of the cloud base.
// ---------------------------------------------------------------------------
@fs fs_cloud_shadow
layout(binding=0) uniform fs_cloud_shadow_params {
    vec4 clOrigin;
    vec4 clLayer;
    vec4 clShape;
    vec4 clWind;
    vec4 clSun;
    vec4 csArea;        // xy = centre of the square (world xz), z = its size (m)
};
layout(binding=1) uniform texture2D weatherTex;
layout(binding=1) uniform sampler weatherTex_smp;
layout(binding=2) uniform texture3D shapeTex;
layout(binding=2) uniform sampler shapeTex_smp;
layout(binding=3) uniform texture3D detailTex;
layout(binding=3) uniform sampler detailTex_smp;
@include_block cloud_shape
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;
void main() {
    // Start on the cloud base above this texel and go up through the layer towards the sun.
    vec3 p = vec3(csArea.x + (uv.x - 0.5) * csArea.z, clLayer.x + 1.0, csArea.y + (uv.y - 0.5) * csArea.z);
    vec3 dir = clSun.xyz;
    float len = clLayer.y / max(dir.y, 0.12);
    const int n = 12;
    float dt = len / float(n);
    float tau = 0.0;
    for (int i = 0; i < n; ++i) tau += CloudDensity(p + dir * ((float(i) + 0.5) * dt), true) * dt;
    float t = exp(-tau);
    fragColor = vec4(t, t, t, 1.0);
}
@end
@program clouds_shadow vs_fullscreen fs_cloud_shadow
