// The atmosphere, shared by the sky, cloud and fog shaders.
//
// A planet of radius atPlanet.x wrapped in air up to atPlanet.y (kilometres; the
// world's y = 0 is the planet's surface). The air scatters light three ways:
// Rayleigh (the gas itself: blue sky, red sunsets), Mie (haze: the white glow
// round the sun) and it absorbs some (ozone: the deep blue of twilight).
//
// Two tables are computed on the CPU when the Sky item changes
// (src/Engine/Atmosphere.cpp, which has the same maths):
//   transLut  how much light gets from a height, in a direction, out to space
//   msLut     the light that has bounced around the air more than once
// and one by the GPU each frame (the sky-view table below): the sky's light in
// every direction from where the camera is.
//
// A shader that includes atmo_trans declares, in its uniform block:
//   vec4 atPlanet;     x = planet radius, y = top of the air, z = camera height above the surface (all km)
// and the texture transLut. One that also includes atmo_common (after it) adds:
//   vec4 atRayleigh;   rgb = scattering per km at the surface, a = scale height (km)
//   vec4 atMie;        rgb = scattering per km at the surface, a = scale height (km)
//   vec4 atMieAbs;     rgb = absorption per km at the surface, a = g (how forward the haze scatters)
//   vec4 atOzone;      rgb = absorption per km at its densest
// and the texture msLut.

@block atmo_trans
const float AT_PI = 3.14159265358979;

// Distance along a ray to a sphere round the origin: the first hit in front of
// the ray's start, or -1 if it misses.
float AtRaySphere(vec3 ro, vec3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    if (c > 0.0 && b > 0.0) return -1.0;
    float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    if (c <= 0.0) return -b + sqrt(disc);      // started inside
    return -b - sqrt(disc);
}

// r = distance from the planet's centre, mu = cosine of the angle from straight up.
bool AtHitsGround(float r, float mu) {
    float rg = atPlanet.x;
    return mu < 0.0 && r * r * (1.0 - mu * mu) < rg * rg;
}

// Light that survives from (r, mu) out to space; nothing gets through the planet.
vec3 AtTransmittance(float r, float mu) {
    float rg = atPlanet.x, rt = atPlanet.y;
    if (AtHitsGround(r, mu)) return vec3(0.0);
    float H = sqrt(rt * rt - rg * rg);
    float rho = sqrt(max(r * r - rg * rg, 0.0));
    float d = max(-r * mu + sqrt(max(r * r * (mu * mu - 1.0) + rt * rt, 0.0)), 0.0);
    float dMin = rt - r, dMax = rho + H;
    vec2 x = clamp(vec2((d - dMin) / max(dMax - dMin, 1e-6), rho / H), 0.0, 1.0);
    // 0..1 runs from the first texel's centre to the last one's.
    vec2 lutUv = vec2(0.5 / 256.0, 0.5 / 64.0) + x * vec2(1.0 - 1.0 / 256.0, 1.0 - 1.0 / 64.0);
    return textureLod(sampler2D(transLut, transLut_smp), lutUv, 0.0).rgb;
}
@end

@block atmo_common
// Light scattered more than once, per unit of light arriving from a sun at cosSun, at height h (km).
vec3 AtMultiScatter(float h, float cosSun) {
    vec2 x = clamp(vec2(cosSun * 0.5 + 0.5, h / (atPlanet.y - atPlanet.x)), 0.0, 1.0);
    vec2 lutUv = vec2(0.5 / 32.0) + x * (1.0 - 1.0 / 32.0);
    return textureLod(sampler2D(msLut, msLut_smp), lutUv, 0.0).rgb;
}

// What the air does at height h (km): Rayleigh and Mie scattering, and everything it takes out of a ray.
void AtMedium(float h, out vec3 scatR, out vec3 scatM, out vec3 extinction) {
    float dR = exp(-h / atRayleigh.a);
    float dM = exp(-h / atMie.a);
    float dO = max(0.0, 1.0 - abs(h - 25.0) / 15.0);
    scatR = atRayleigh.rgb * dR;
    scatM = atMie.rgb * dM;
    extinction = scatR + scatM + atMieAbs.rgb * dM + atOzone.rgb * dO;
}

float AtPhaseRayleigh(float c) { return 3.0 / (16.0 * AT_PI) * (1.0 + c * c); }
float AtPhaseMie(float c, float g) {
    float g2 = g * g;
    return 3.0 / (8.0 * AT_PI) * ((1.0 - g2) * (1.0 + c * c)) / ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// Light scattered towards the start of a ray (ro from the planet's centre, km)
// over tMax km, by a sun and a moon (direction towards each, light arriving
// from each). `trans` is what is left of the light from beyond the ray. The
// steps grow with distance: the air near the eye matters most.
vec3 AtScatter(vec3 ro, vec3 rd, float tMax, int steps, vec3 toSun, vec3 sunLight, vec3 toMoon, vec3 moonLight, out vec3 trans) {
    float g = atMieAbs.a;
    float cs = dot(rd, toSun), cm = dot(rd, toMoon);
    float phRs = AtPhaseRayleigh(cs), phMs = AtPhaseMie(cs, g);
    float phRm = AtPhaseRayleigh(cm), phMm = AtPhaseMie(cm, g);
    vec3 L = vec3(0.0);
    trans = vec3(1.0);
    float inv = 1.0 / float(steps);
    for (int i = 0; i < 48; ++i) {
        if (i >= steps) break;
        float a0 = float(i) * inv, a1 = float(i + 1) * inv;
        float t0 = a0 * a0 * tMax, t1 = a1 * a1 * tMax;
        float dt = t1 - t0;
        vec3 p = ro + rd * mix(t0, t1, 0.4);
        float r = length(p);
        float h = max(r - atPlanet.x, 0.0);
        vec3 up = p / r;
        vec3 scatR, scatM, ext;
        AtMedium(h, scatR, scatM, ext);
        vec3 scat = scatR + scatM;
        float muS = dot(up, toSun), muM = dot(up, toMoon);
        vec3 S = sunLight * (AtTransmittance(r, muS) * (scatR * phRs + scatM * phMs) + AtMultiScatter(h, muS) * scat)
               + moonLight * (AtTransmittance(r, muM) * (scatR * phRm + scatM * phMm) + AtMultiScatter(h, muM) * scat);
        vec3 stepT = exp(-ext * dt);
        L += trans * (S - S * stepT) / max(ext, vec3(1e-7));
        trans *= stepT;
    }
    return L;
}
@end

// The sky-view table: the sky's light in every direction from the camera's
// height. u = compass direction; v = height above (first half) or below (second
// half) the horizon, with most texels near the horizon where the sky changes fastest.
@block atmo_skyview
const float SV_W = 256.0;
const float SV_H = 144.0;

// Angle from straight up to the horizon, as seen from distance r from the planet's centre.
float SvHorizonAngle(float r) {
    float rg = atPlanet.x;
    return AT_PI - acos(clamp(sqrt(max(r * r - rg * rg, 0.0)) / r, 0.0, 1.0));
}

vec3 SvDirection(vec2 lutUv, float r) {
    float horizon = SvHorizonAngle(r);
    float zenith;
    if (lutUv.y < 0.5) {
        float c = 1.0 - 2.0 * lutUv.y;
        zenith = horizon * (1.0 - c * c);
    } else {
        float c = 2.0 * lutUv.y - 1.0;
        zenith = horizon + (AT_PI - horizon) * c * c;
    }
    float az = lutUv.x * 2.0 * AT_PI;
    float s = sin(zenith);
    return vec3(s * cos(az), cos(zenith), s * sin(az));
}

vec2 SvUv(vec3 d, float r) {
    float horizon = SvHorizonAngle(r);
    float zenith = acos(clamp(d.y, -1.0, 1.0));
    float v;
    if (zenith < horizon) {
        v = 0.5 * (1.0 - sqrt(max(1.0 - zenith / horizon, 0.0)));
        v = clamp(v, 0.5 / SV_H, 0.5 - 0.5 / SV_H);
    } else {
        v = 0.5 + 0.5 * sqrt(clamp((zenith - horizon) / max(AT_PI - horizon, 1e-4), 0.0, 1.0));
        v = clamp(v, 0.5 + 0.5 / SV_H, 1.0 - 0.5 / SV_H);
    }
    float u = atan(d.z, d.x) / (2.0 * AT_PI);
    return vec2(u < 0.0 ? u + 1.0 : u, v);
}
@end
