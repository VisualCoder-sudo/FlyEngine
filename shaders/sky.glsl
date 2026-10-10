// The sky behind the scene (src/Engine/Atmosphere.cpp). Drawn as a full-screen
// pass into the float scene target before any geometry.
@module sky
@include fly_common.glsl
@include fly_atmosphere.glsl

@vs vs_fullscreen
@include_block fly_fullscreen_vs
@end

// One colour (the Sky item without an atmosphere, or the editor's own
// background). The colour arrives already taken back through the tone curve, and
// is divided by the exposure here, so it reaches the screen exactly as picked.
@fs fs_flat
layout(binding=0) uniform fs_flat_params {
    vec4 skyHdr;        // rgb = the colour before exposure and tone curve
};
layout(binding=0) uniform texture2D exposureTex;
layout(binding=0) uniform sampler exposureTex_smp;
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;
void main() {
    float e = textureLod(sampler2D(exposureTex, exposureTex_smp), vec2(0.5), 0.0).r;
    fragColor = vec4(skyHdr.rgb / max(e, 1e-4), 1.0);
}
@end
@program sky_flat vs_fullscreen fs_flat

// ---------------------------------------------------------------------------
// The sky-view table (see fly_atmosphere.glsl): the atmosphere's light in every
// direction from the camera's height, lit by the sun and by the moon. Small, and
// redrawn every frame.
// ---------------------------------------------------------------------------
@fs fs_skyview
layout(binding=0) uniform fs_skyview_params {
    vec4 atRayleigh;
    vec4 atMie;
    vec4 atMieAbs;
    vec4 atOzone;
    vec4 atPlanet;
    vec4 svSun;         // xyz = direction towards the sun
    vec4 svSunLight;    // rgb = the sun's light above the air
    vec4 svMoon;        // xyz = direction towards the moon
    vec4 svMoonLight;   // rgb = the moon's light above the air
};
layout(binding=0) uniform texture2D transLut;
layout(binding=0) uniform sampler transLut_smp;
layout(binding=1) uniform texture2D msLut;
layout(binding=1) uniform sampler msLut_smp;
@include_block atmo_trans
@include_block atmo_common
@include_block atmo_skyview
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;
void main() {
    float r = atPlanet.x + max(atPlanet.z, 0.001);
    vec3 ro = vec3(0.0, r, 0.0);
    vec3 rd = SvDirection(uv, r);
    float tGround = AtRaySphere(ro, rd, atPlanet.x);
    float tTop = AtRaySphere(ro, rd, atPlanet.y);
    float tMax = tGround > 0.0 ? tGround : tTop;
    if (tMax <= 0.0) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    vec3 trans;
    vec3 L = AtScatter(ro, rd, tMax, 32, svSun.xyz, svSunLight.rgb, svMoon.xyz, svMoonLight.rgb, trans);
    // a = how much of what lies beyond (the far ground) still shows through the air.
    fragColor = vec4(L, dot(trans, vec3(1.0 / 3.0)));
}
@end
@program sky_view vs_fullscreen fs_skyview

// ---------------------------------------------------------------------------
// The sky as the camera sees it: the atmosphere from the table above, the sun's
// and the moon's discs, stars, and below the horizon the ground of the planet
// (what a high camera sees beyond the scene's far plane).
// ---------------------------------------------------------------------------
@fs fs_atmo
layout(binding=0) uniform fs_atmo_params {
    vec4 camProj;       // see fly_camera
    vec4 camRight;      // xyz = the camera's right axis, w = 1 for an orthographic camera
    vec4 camUp;
    vec4 camFwd;
    vec4 atPlanet;
    vec4 skSun;         // xyz = direction towards the sun, w = its angular radius (radians)
    vec4 skSunDisc;     // rgb = the disc's light above the air
    vec4 skMoon;        // xyz = direction towards the moon, w = its angular radius (radians)
    vec4 skMoonDisc;    // rgb = the lit moon's light, a = phase (0 = new, 0.5 = full, 1 = new again)
    vec4 skGround;      // rgb = light coming off the far ground
    vec4 skParams;      // x = star brightness, y = time (s), z = overcast 0..1 (greys the sky when there are no clouds to do it), w = angle of one pixel (radians)
    vec4 skStars;       // x, y = cos and sin of the sky's turn with the time of day, z = night sky glow
};
layout(binding=0) uniform texture2D transLut;
layout(binding=0) uniform sampler transLut_smp;
layout(binding=1) uniform texture2D skyViewLut;
layout(binding=1) uniform sampler skyViewLut_smp;
@include_block fly_color
@include_block atmo_trans
@include_block atmo_skyview
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;

vec3 Hash33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}
float Hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float Noise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(Hash21(i), Hash21(i + vec2(1.0, 0.0)), f.x), mix(Hash21(i + vec2(0.0, 1.0)), Hash21(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Stars: points scattered over the faces of a cube round the viewer, in two sizes of grid.
vec3 Stars(vec3 d, float pixelAngle, float time) {
    vec3 a = abs(d);
    vec2 f2;
    float face;
    if (a.x >= a.y && a.x >= a.z) { f2 = d.yz / a.x; face = d.x > 0.0 ? 0.0 : 1.0; }
    else if (a.y >= a.z) { f2 = d.xz / a.y; face = d.y > 0.0 ? 2.0 : 3.0; }
    else { f2 = d.xy / a.z; face = d.z > 0.0 ? 4.0 : 5.0; }
    vec3 col = vec3(0.0);
    for (int layer = 0; layer < 2; ++layer) {
        float n = layer == 0 ? 70.0 : 150.0;                    // cells along a face
        vec2 g = (f2 * 0.5 + 0.5) * n;
        vec2 id = floor(g);
        vec3 h = Hash33(vec3(id, face * 7.0 + float(layer) * 31.0));
        // Few cells hold a star, and most stars are faint: a handful of bright ones among a dusting.
        float keep = layer == 0 ? 0.90 : 0.86;
        if (h.x < keep) continue;
        float rank = (h.x - keep) / (1.0 - keep);
        vec2 c = (h.yz - 0.5) * 0.6 + 0.5;
        // A star is about a pixel wide whatever the field of view; the bright ones a little wider.
        float cellAngle = 1.5708 / n;
        float bright = pow(rank, 4.0) * (layer == 0 ? 1.0 : 0.22) + 0.012;
        float radius = max((0.75 + 0.9 * bright) * pixelAngle / cellAngle, 0.012);
        float dist = length(fract(g) - c);
        float twinkle = 0.8 + 0.2 * sin(time * (2.0 + 5.0 * h.y) + h.z * 40.0);
        vec3 tint = mix(vec3(1.0, 0.80, 0.60), vec3(0.70, 0.82, 1.0), h.z);
        col += tint * bright * twinkle * (1.0 - smoothstep(radius * 0.3, radius, dist));
    }
    return col;
}

void main() {
    vec3 rd;
    if (camRight.w > 0.5) rd = camFwd.xyz;
    else rd = normalize(camRight.xyz * ((ndc.x + camProj.z) * camProj.x) + camUp.xyz * ((ndc.y + camProj.w) * camProj.y) + camFwd.xyz);

    float r = atPlanet.x + max(atPlanet.z, 0.001);
    vec4 sv = textureLod(sampler2D(skyViewLut, skyViewLut_smp), SvUv(rd, r), 0.0);
    vec3 L = sv.rgb;
    float px = skParams.w;

    if (AtHitsGround(r, rd.y)) {
        L += skGround.rgb * sv.a;
    } else {
        vec3 T = AtTransmittance(r, rd.y);
        float covered = 0.0;        // 1 where the sun's or the moon's disc hides the stars

        // The sun: a disc that is a little darker towards its edge.
        float sunAngle = asin(clamp(length(cross(rd, skSun.xyz)), 0.0, 1.0));
        if (dot(rd, skSun.xyz) > 0.0 && sunAngle < skSun.w + px) {
            float disc = 1.0 - smoothstep(skSun.w - px, skSun.w + px, sunAngle);
            float mu = sqrt(max(1.0 - (sunAngle * sunAngle) / (skSun.w * skSun.w), 0.0));
            L += skSunDisc.rgb * T * disc * (1.0 - 0.55 * (1.0 - mu));
            covered = disc;
        }

        // The moon: a sphere lit from the side its phase says, with darker seas.
        float moonAngle = asin(clamp(length(cross(rd, skMoon.xyz)), 0.0, 1.0));
        if (dot(rd, skMoon.xyz) > 0.0 && moonAngle < skMoon.w + px) {
            float disc = 1.0 - smoothstep(skMoon.w - px, skMoon.w + px, moonAngle);
            vec3 mx = normalize(cross(vec3(0.0, 1.0, 0.0), skMoon.xyz));
            vec3 my = cross(skMoon.xyz, mx);
            vec2 q = vec2(dot(rd, mx), dot(rd, my)) / sin(skMoon.w);
            vec3 n = vec3(q, sqrt(max(1.0 - dot(q, q), 0.0)));
            float phase = skMoonDisc.a * 2.0 * AT_PI;
            vec3 lightDir = vec3(sin(phase), 0.0, -cos(phase));
            float lit = smoothstep(-0.02, 0.12, dot(n, lightDir));
            float seas = 0.62 + 0.38 * smoothstep(0.35, 0.65, Noise2(q * 2.3 + 7.0) * 0.6 + Noise2(q * 5.1 + 3.0) * 0.4);
            L += skMoonDisc.rgb * T * disc * (lit * seas + 0.012);
            covered = max(covered, disc);
        }

        // Stars turn with the time of day, about an axis tilted towards the north.
        if (skParams.x > 0.0) {
            const vec3 axis = vec3(0.0, 0.5, 0.8660254);
            vec3 sd = rd * skStars.x + cross(axis, rd) * skStars.y + axis * dot(axis, rd) * (1.0 - skStars.x);
            L += Stars(sd, px, skParams.y) * skParams.x * T * (1.0 - covered);
        }
        L += vec3(0.35, 0.45, 0.75) * skStars.z * T;      // the night sky is never quite black
    }

    // Without cloud to cover it, an overcast sky is drawn as a flat grey.
    float grey = fly_luma(L);
    L = mix(L, vec3(grey) * 0.85, 0.7 * skParams.z);
    fragColor = vec4(L, 1.0);
}
@end
@program sky_atmo vs_fullscreen fs_atmo
