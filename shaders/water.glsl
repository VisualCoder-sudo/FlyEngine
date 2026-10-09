// Water surface: CPU-matched simplex FBM waves in the vertex stage, planar
// reflection, foam and capillary glints in the fragment stage. Ported from
// water.vert/water.frag (GLSL 330); the 512-entry permutation table is an
// ivec4[128] uniform array, read through P().
@module water
@include fly_common.glsl

@vs vs
@include_block fly_clip
layout(binding=0) uniform vs_params {
    mat4 wModel;
    mat4 wView;
    mat4 wProj;
    vec4 waterBodyNoiseParams1;
    vec4 waterBodyNoiseParams2;
    vec4 rippleParams;              // xy=window origin (world XZ), z=1/window size, w=enabled
    vec3 cameraPos;
    float _pad0;
    vec2 waterBodyNoiseDirection;
    ivec4 perm[128];
};
// Dynamic ripple layer (boat wakes / splashes): r = height (m), g = foam 0..1.
// The fragment stage binds the same texture under its own name (rippleTexFS).
layout(binding=1) uniform texture2D rippleTexVS;
layout(binding=1) uniform sampler rippleTexVS_smp;
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;

out vec3 worldPos;
out vec3 worldNormal;
out vec3 viewDir;
out vec2 texCoord;
out float heightOffset;
out float noiseValue;
flat out vec2 vsCamXZ;
out float fragDist;

int P(int i) {
    return perm[i >> 2][i & 3];
}

// 12 gradient vectors - must match CPU WaterNoise.cpp exactly.
const vec3 grad3[12] = vec3[12](
    vec3(1,1,0), vec3(-1,1,0), vec3(1,-1,0), vec3(-1,-1,0),
    vec3(1,0,1), vec3(-1,0,1), vec3(1,0,-1), vec3(-1,0,-1),
    vec3(0,1,1), vec3(0,-1,1), vec3(0,1,-1), vec3(0,-1,-1)
);

// Improved Simplex 3D - same algorithm as CPU WaterNoise::Simplex3D().
// Returns value in approximately [-1, 1].
float simplex3D(vec3 p, int seed) {
    const float F3 = 1.0 / 3.0;
    const float G3 = 1.0 / 6.0;

    float s = (p.x + p.y + p.z) * F3;
    int i = int(floor(p.x + s));
    int j = int(floor(p.y + s));
    int k = int(floor(p.z + s));

    float t = float(i + j + k) * G3;
    float x0 = p.x - (float(i) - t);
    float y0 = p.y - (float(j) - t);
    float z0 = p.z - (float(k) - t);

    int i1, j1, k1;
    int i2, j2, k2;

    if (x0 >= y0) {
        if (y0 >= z0)      { i1=1; j1=0; k1=0; i2=1; j2=1; k2=0; }
        else if (x0 >= z0) { i1=1; j1=0; k1=0; i2=1; j2=0; k2=1; }
        else               { i1=0; j1=0; k1=1; i2=1; j2=0; k2=1; }
    } else {
        if (y0 < z0)       { i1=0; j1=0; k1=1; i2=0; j2=1; k2=1; }
        else if (x0 < z0)  { i1=0; j1=1; k1=0; i2=0; j2=1; k2=1; }
        else               { i1=0; j1=1; k1=0; i2=1; j2=1; k2=0; }
    }

    float x1 = x0 - float(i1) + G3;
    float y1 = y0 - float(j1) + G3;
    float z1 = z0 - float(k1) + G3;
    float x2 = x0 - float(i2) + 2.0 * G3;
    float y2 = y0 - float(j2) + 2.0 * G3;
    float z2 = z0 - float(k2) + 2.0 * G3;
    float x3 = x0 - 1.0 + 3.0 * G3;
    float y3 = y0 - 1.0 + 3.0 * G3;
    float z3 = z0 - 1.0 + 3.0 * G3;

    int ii = (i + seed) & 255;
    int jj = (j + seed) & 255;
    int kk = (k + seed) & 255;

    int gi0 = P(ii + P(jj + P(kk))) % 12;
    int gi1 = P(ii + i1 + P(jj + j1 + P(kk + k1))) % 12;
    int gi2 = P(ii + i2 + P(jj + j2 + P(kk + k2))) % 12;
    int gi3 = P(ii + 1 + P(jj + 1 + P(kk + 1))) % 12;

    float t0 = 0.6 - x0*x0 - y0*y0 - z0*z0;
    float n0 = 0.0;
    if (t0 > 0.0) { t0 *= t0; n0 = t0 * t0 * dot(grad3[gi0], vec3(x0,y0,z0)); }

    float t1 = 0.6 - x1*x1 - y1*y1 - z1*z1;
    float n1 = 0.0;
    if (t1 > 0.0) { t1 *= t1; n1 = t1 * t1 * dot(grad3[gi1], vec3(x1,y1,z1)); }

    float t2 = 0.6 - x2*x2 - y2*y2 - z2*z2;
    float n2 = 0.0;
    if (t2 > 0.0) { t2 *= t2; n2 = t2 * t2 * dot(grad3[gi2], vec3(x2,y2,z2)); }

    float t3 = 0.6 - x3*x3 - y3*y3 - z3*z3;
    float n3 = 0.0;
    if (t3 > 0.0) { t3 *= t3; n3 = t3 * t3 * dot(grad3[gi3], vec3(x3,y3,z3)); }

    return 32.0 * (n0 + n1 + n2 + n3);
}

float fbm(vec3 p, int octaves, float persistence, float lacunarity, int seed) {
    float value = 0.0;
    float amplitude = 1.0;
    float frequency = 1.0;
    float maxValue = 0.0;

    for (int i = 0; i < octaves; i++) {
        value += amplitude * simplex3D(p * frequency, seed);
        maxValue += amplitude;
        amplitude *= persistence;
        frequency *= lacunarity;
    }

    return value / maxValue;
}

// Displacement of the water surface at a world XZ position: procedural waves plus
// the dynamic ripple layer. Used to stitch chunk borders (see main).
float surfaceHeightAt(vec2 xz) {
    float amplitude = waterBodyNoiseParams1.x;
    float frequency = waterBodyNoiseParams1.y;
    float time = waterBodyNoiseParams2.w * waterBodyNoiseParams1.z;
    vec2 flowOffset = waterBodyNoiseDirection * time;
    vec3 np = vec3((xz.x - flowOffset.x) * frequency, time * 0.5, (xz.y - flowOffset.y) * frequency);
    float n = fbm(np, int(waterBodyNoiseParams1.w), waterBodyNoiseParams2.x, waterBodyNoiseParams2.y, int(waterBodyNoiseParams2.z));
    vec2 uv = (xz - rippleParams.xy) * rippleParams.z;
    float rh = textureLod(sampler2D(rippleTexVS, rippleTexVS_smp), uv, 0.0).r * rippleParams.w;
    return n * amplitude + rh;
}

void main() {
    vec3 localPos = vertexPosition;

    float amplitude = waterBodyNoiseParams1.x;
    float frequency = waterBodyNoiseParams1.y;
    float speed = waterBodyNoiseParams1.z;
    int octaves = int(waterBodyNoiseParams1.w);
    float persistence = waterBodyNoiseParams2.x;
    float lacunarity = waterBodyNoiseParams2.y;
    int seed = int(waterBodyNoiseParams2.z);
    float time = waterBodyNoiseParams2.w * speed;
    float timeY = time * 0.5;

    // Waves drift along waterBodyNoiseDirection over time (wind-driven flow),
    // layered on top of the organic per-frame evolution from timeY.
    vec2 flowOffset = waterBodyNoiseDirection * time;

    // Compute world XZ BEFORE noise so adjacent chunks sample seamlessly.
    vec4 worldPos4 = wModel * vec4(localPos, 1.0);
    vec2 worldXZ = worldPos4.xz;

    // Fragment distance from the camera (symmetric in azimuth) - used for a
    // reflection distance fade that does not depend on screen position.
    fragDist = distance(cameraPos, worldPos4.xyz);

    vec3 noisePos = vec3((worldXZ.x - flowOffset.x) * frequency,
                         timeY,
                         (worldXZ.y - flowOffset.y) * frequency);

    float noise = fbm(noisePos, octaves, persistence, lacunarity, seed);

    // Dynamic ripples (wakes, splashes) added on top of the procedural waves.
    // Sampled unconditionally (the texture is always bound); masked by w.
    const float RIPPLE_N = 256.0;
    float rippleTexel = 1.0 / RIPPLE_N;
    float rippleCell = 1.0 / (max(rippleParams.z, 1e-6) * RIPPLE_N); // metres per texel
    vec2 rippleUV = (worldXZ - rippleParams.xy) * rippleParams.z;
    float rippleOn = rippleParams.w;
    float rh = textureLod(sampler2D(rippleTexVS, rippleTexVS_smp), rippleUV, 0.0).r * rippleOn;
    float rhL = textureLod(sampler2D(rippleTexVS, rippleTexVS_smp), rippleUV - vec2(rippleTexel, 0.0), 0.0).r * rippleOn;
    float rhR = textureLod(sampler2D(rippleTexVS, rippleTexVS_smp), rippleUV + vec2(rippleTexel, 0.0), 0.0).r * rippleOn;
    float rhD = textureLod(sampler2D(rippleTexVS, rippleTexVS_smp), rippleUV - vec2(0.0, rippleTexel), 0.0).r * rippleOn;
    float rhU = textureLod(sampler2D(rippleTexVS, rippleTexVS_smp), rippleUV + vec2(0.0, rippleTexel), 0.0).r * rippleOn;
    vec2 rippleSlope = vec2(rhR - rhL, rhU - rhD) / (2.0 * rippleCell);

    float heightOffsetLocal = noise * amplitude + rh;

    // Crack-free LOD borders: chunks of different resolution share an edge but
    // tessellate it differently, so their displaced edges drift apart and leave
    // slivers (wide ribbons where a wake displaces the water) that show the
    // background. Vertices on a chunk border (local |x| or |z| = CHUNK_SIZE/2 = 10)
    // are therefore displaced along the straight line between the two lattice
    // points of the coarsest LOD (5 m), which every LOD includes - so both sides of
    // any border compute the identical height.
    {
        const float HALF = 10.0;
        const float LATTICE = 5.0;
        bool onZ = abs(abs(localPos.z) - HALF) < 0.002; // edge running along x
        bool onX = abs(abs(localPos.x) - HALF) < 0.002; // edge running along z
        if (onZ || onX) {
            float along = onZ ? localPos.x : localPos.z;
            float a0 = floor((along + HALF) / LATTICE) * LATTICE - HALF;
            float t = (along - a0) / LATTICE;
            vec2 p0 = worldXZ, p1 = worldXZ;
            if (onZ) { p0.x += a0 - along; p1.x = p0.x + LATTICE; }
            else     { p0.y += a0 - along; p1.y = p0.y + LATTICE; }
            heightOffsetLocal = mix(surfaceHeightAt(p0), surfaceHeightAt(p1), t);
        }
    }
    worldPos4.y += heightOffsetLocal;

    // Analytic normal via central-difference of the height field, so lighting
    // reacts to actual wave slope instead of a flat up-vector. Uses a reduced
    // octave count since fine detail barely affects large-scale slope, and
    // this already costs 4 extra fbm evaluations per vertex.
    int normalOctaves = min(octaves, 2);
    float eps = max(0.15 / max(frequency, 0.001), 0.15);

    vec3 npL = vec3((worldXZ.x - eps - flowOffset.x) * frequency, timeY, (worldXZ.y - flowOffset.y) * frequency);
    vec3 npR = vec3((worldXZ.x + eps - flowOffset.x) * frequency, timeY, (worldXZ.y - flowOffset.y) * frequency);
    vec3 npD = vec3((worldXZ.x - flowOffset.x) * frequency, timeY, (worldXZ.y - eps - flowOffset.y) * frequency);
    vec3 npU = vec3((worldXZ.x - flowOffset.x) * frequency, timeY, (worldXZ.y + eps - flowOffset.y) * frequency);

    float hL = fbm(npL, normalOctaves, persistence, lacunarity, seed) * amplitude;
    float hR = fbm(npR, normalOctaves, persistence, lacunarity, seed) * amplitude;
    float hD = fbm(npD, normalOctaves, persistence, lacunarity, seed) * amplitude;
    float hU = fbm(npU, normalOctaves, persistence, lacunarity, seed) * amplitude;

    vec3 localNormal = normalize(vec3(-(hR - hL) / (2.0 * eps) - rippleSlope.x, 1.0,
                                      -(hU - hD) / (2.0 * eps) - rippleSlope.y));

    worldPos = worldPos4.xyz;
    vsCamXZ = cameraPos.xz;

    mat3 normalMatrix = mat3(transpose(inverse(wModel)));
    worldNormal = normalize(normalMatrix * localNormal);
    viewDir = normalize(cameraPos - worldPos);
    texCoord = vertexTexCoord;
    heightOffset = heightOffsetLocal;
    noiseValue = noise;

    gl_Position = fly_clip(wProj * wView * worldPos4);
}
@end

@fs fs
@include_block fly_rt_uv
layout(binding=1) uniform fs_params {
    vec4 waterBodyBaseColor;
    vec4 waterBodyNoiseParams1;
    vec4 waterBodyNoiseParams2;
    vec4 waterBodyFoamParams;
    vec4 waterBodyDetailParams;     // intensity, scale, speed, _pad
    vec4 reflParams;                // x=strength, y=distortion, z=distance fade, w=enabled
    vec4 rippleParams;              // xy=window origin (world XZ), z=1/window size, w=enabled
    mat4 reflViewProj;              // view-projection of the reflected camera
    vec4 objectPositions[16];
    vec3 waterBodyFoamColor;
    float globalTime;
    vec2 farRimParams;              // x=clip radius (0 disables), y=feather width
    float chunkFade;
    int objectCount;
};
layout(binding=0) uniform texture2D reflectionTex;   // planar mirror of the world
layout(binding=0) uniform sampler reflectionTex_smp;
layout(binding=2) uniform texture2D rippleTexFS;     // r = ripple height, g = foam trail
layout(binding=2) uniform sampler rippleTexFS_smp;

in vec3 worldPos;
in vec3 worldNormal;
in vec3 viewDir;
in vec2 texCoord;
in float heightOffset;
in float noiseValue;
flat in vec2 vsCamXZ;
in float fragDist;
out vec4 fragColor;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Small-scale capillary ripple height field (2 octaves), in world-scaled units.
// The second octave drifts with time so the pattern evolves, not just slides.
float microHeight(vec2 p, float t) {
    float n1 = vnoise(p);
    float n2 = vnoise(p * 2.13 + vec2(17.0 + t * 0.5, 29.0 - t * 0.35));
    return n1 * 0.55 + n2 * 0.45;
}

void main() {
    // Far-shell rim clip: the detailed chunk layer fully covers the disk around
    // the camera, so the far shell discards fragments inside it. Doing this here
    // means the shell never double-blends over the chunk water.
    if (farRimParams.x > 0.5) {
        float rimDist = distance(worldPos.xz, vsCamXZ);
        float rimAlpha = smoothstep(farRimParams.x - farRimParams.y, farRimParams.x + farRimParams.y, rimDist);
        if (rimAlpha < 0.001) discard;
    }

    vec3 N = normalize(worldNormal);
    vec3 V = normalize(viewDir);
    vec3 lightDir = normalize(vec3(0.3, 1.0, 0.4));
    vec3 L = normalize(lightDir);
    vec3 H = normalize(V + L);
    vec3 skyColor = vec3(0.65, 0.78, 0.88);

    // Capillary micro-detail: perturb the smooth wave normal with high-frequency
    // ripples so the specular highlight breaks into scattered glints instead of
    // one flat sweep. Pure shading - no geometry or physics impact. The field
    // scrolls with time (and its detail octave reshapes) so the glints shimmer
    // instead of sitting fixed on the wave.
    float detailIntensity = waterBodyDetailParams.x;
    float detailScale = waterBodyDetailParams.y;
    float detailSpeed = waterBodyDetailParams.z;
    float t = globalTime * detailSpeed;

    vec2 p0 = worldPos.xz * detailScale + vec2(t, t * 0.6);
    float e = 0.08;
    float h0 = microHeight(p0, t);
    float hx = microHeight(p0 + vec2(e, 0.0), t);
    float hz = microHeight(p0 + vec2(0.0, e), t);
    vec2 grad = vec2(hx - h0, hz - h0) / e;
    grad = clamp(grad, vec2(-1.0), vec2(1.0));

    vec3 worldUp = vec3(0.0, 1.0, 0.0);
    vec3 T = cross(worldUp, N);
    float tangLen = length(T);
    T = tangLen > 0.0001 ? normalize(T) : vec3(1.0, 0.0, 0.0);
    vec3 B = cross(N, T);

    vec3 microN = normalize(N + (T * grad.x + B * grad.y) * detailIntensity);

    // Planar reflection: project this fragment into the mirrored camera and sample
    // the captured world image there (standard mirror trick - no manual ray math).
    vec4 reflClip = reflViewProj * vec4(worldPos, 1.0);
    vec2 reflUV = reflClip.xy / max(reflClip.w, 0.0001) * 0.5 + 0.5;
    vec2 reflTexUV = fly_rt_uv(reflClip.xy / max(reflClip.w, 0.0001));

    // Fade the reflection toward the borders of the mirrored image. Fragments that
    // fall outside the reflected camera's frustum clamp onto the edge texels, which
    // smears the object's image across neighbouring water (the ghost "behind" it)
    // and pops in a hard line when it slides out of view while turning. Fading the
    // border keeps that transition smooth instead of showing a reversing streak.
    vec2 edgeDist = abs(reflUV - vec2(0.5)) * 2.0; // 0 = centre, 1 = frustum edge
    float edgeFade = clamp(1.0 - smoothstep(0.55, 0.95, edgeDist.x)
                                  - smoothstep(0.55, 0.95, edgeDist.y), 0.0, 1.0);
    vec2 reflUVClamped = clamp(reflUV, vec2(0.0), vec2(1.0));

    // Distortion grows with wave steepness: calm water stays crisp, rough water
    // smears the mirror image. High-frequency capillary ripples (grad) add the
    // "broken by droplets" breakup on top of the larger wave tilt.
    float waveSlope = clamp(length(N.xz) / max(N.y, 0.08), 0.0, 2.0);
    float rough = clamp(waveSlope, 0.0, 1.0) * reflParams.y;
    vec2 distortUV = grad * 0.05 + N.xz * 0.08;
    distortUV *= rough;
    vec2 reflSampleUV = clamp(clamp(reflTexUV, vec2(0.0), vec2(1.0)) + distortUV, vec2(0.0), vec2(1.0));

    vec3 reflectionColor = texture(sampler2D(reflectionTex, reflectionTex_smp), reflSampleUV).rgb;
    // Distance fade is based on the fragment's own 3D distance from the camera
    // (azimuth-symmetric), NOT on the mirrored-frustum depth: the old version
    // varied with reflUV/edgeFade, which is screen-position-dependent and made
    // the middle of the far ocean diverge in shade from the left/right sides in
    // a pattern that followed the camera.
    float distFade = clamp(1.0 - fragDist * reflParams.z, 0.0, 1.0);
    reflectionColor = mix(skyColor, reflectionColor, distFade * edgeFade);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(microN, H), 0.0);

    vec3 baseColor = waterBodyBaseColor.rgb;
    float alpha = waterBodyBaseColor.a;

    float totalFoam = 0.0;
    float speed = waterBodyNoiseParams1.z;
    float time = waterBodyNoiseParams2.w * speed;

    // Wave foam: only appears near actual wave crests, broken into patches
    // by a noise pattern tiled by foam.scale (waterBodyFoamParams.y).
    float waveHeight01 = noiseValue * 0.5 + 0.5; // 0..1
    float crestMask = smoothstep(0.62, 0.85, waveHeight01); // top of the wave only

    float foamNoise = vnoise(worldPos.xz * waterBodyFoamParams.y + time * 0.15);
    float patchMask = smoothstep(waterBodyFoamParams.z, waterBodyFoamParams.z + 0.2, foamNoise);

    float waveFoam = crestMask * patchMask;

    totalFoam = waveFoam * waterBodyFoamParams.x;

    // Object proximity foam
    for (int i = 0; i < objectCount && i < 16; i++) {
        vec3 objPos = objectPositions[i].xyz; // objPos.y is the object's BOTTOM, not its center
        float objRadius = objectPositions[i].w;

        // Compare against the undisplaced base water level, not the
        // instantaneous wave-bobbed surface (worldPos.y), which oscillates
        // and made contact flicker in and out. Tolerance scales with wave
        // amplitude so the whole range of wave motion still counts.
        float baseWaterLevel = worldPos.y - heightOffset;
        float amplitude = waterBodyNoiseParams1.x;
        float tolIn = 0.15 + amplitude * 0.3;
        float tolOut = 0.4 + amplitude * 0.9;
        float heightDelta = abs(objPos.y - baseWaterLevel);
        float heightFactor = 1.0 - smoothstep(tolIn, tolOut, heightDelta);
        if (heightFactor <= 0.001) continue;

        // Circular distance from the object's footprint edge (not a box
        // metric), for a natural wake instead of a diamond-shaped blob.
        float distFromCenter = length(worldPos.xz - objPos.xz);
        float distFromEdge = distFromCenter - objRadius;

        // Falloff scales with the object's own size instead of a fixed
        // magic number, so small props don't get an oversized halo.
        float wakeWidth = clamp(objRadius * 0.6, 0.5, 2.5);

        // Perturb the wake's radius with low-frequency noise so the edge
        // bulges out in some places and pulls in in others, instead of
        // reading as a perfect blurry circle.
        float radiusNoise = vnoise((worldPos.xz - objPos.xz) * 0.6 + time * 0.08) * 2.0 - 1.0;
        distFromEdge -= radiusNoise * wakeWidth * 0.7;

        // A thin foam band around the object's edge rather than a filled
        // disk: fades out both toward the object's center and away from it.
        float outerFade = smoothstep(wakeWidth, 0.0, distFromEdge);
        float innerFade = smoothstep(-wakeWidth * 1.5, -wakeWidth * 0.3, distFromEdge);
        float ring = outerFade * innerFade;

        // Softer, less contrasty noise breakup for a more subtle wake.
        float n = vnoise(worldPos.xz * 2.5 + time * 0.4) * 0.5 + 0.5;
        n = smoothstep(0.2, 0.9, n);
        ring *= n * waterBodyFoamParams.x * heightFactor * 0.5;

        totalFoam = max(totalFoam, ring);
    }

    // Wake / splash foam trail from the dynamic ripple layer. It persists and
    // fades on the CPU side, so a passing boat leaves a lingering white trail.
    vec2 rippleUV = (worldPos.xz - rippleParams.xy) * rippleParams.z;
    float trail = texture(sampler2D(rippleTexFS, rippleTexFS_smp), rippleUV).g * rippleParams.w;
    float trailBreakup = vnoise(worldPos.xz * 3.2 + time * 0.35) * 0.55 + vnoise(worldPos.xz * 9.0 - time * 0.2) * 0.45;
    float trailFoam = smoothstep(0.04, 0.55, trail * (0.55 + 0.9 * trailBreakup));
    totalFoam = max(totalFoam, trailFoam);

    totalFoam = clamp(totalFoam, 0.0, 1.0);

    // Lighting
    float diffuse = smoothstep(0.0, 0.01, NdotL) * 0.6 + 0.4;
    float spec = pow(NdotH, 48.0) * 0.8;

    // Fresnel-Schlick: water gets more reflective (and visually more opaque)
    // at grazing angles instead of always showing the flat base color.
    float F0 = 0.02;
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);

    // Reflections blend toward the base color under foam (foam covers the
    // surface). A floor keeps a readable mirror even looking straight down,
    // ramping to full mirror strength at grazing angles.
    float reflEnabled = reflParams.w;
    float reflAmt = mix(0.12, 1.0, fresnel);
    float reflMix = mix(fresnel * 0.6, reflParams.x * reflAmt * (1.0 - totalFoam), reflEnabled);
    vec3 reflTerm = mix(skyColor, reflectionColor, reflEnabled);

    vec3 color = mix(baseColor * diffuse, reflTerm, reflMix);
    color += waterBodyFoamColor * totalFoam;
    color += vec3(1.0) * spec;

    float edgeAlpha = mix(alpha, 1.0, fresnel * 0.5);

    fragColor = vec4(color, edgeAlpha * chunkFade);
}
@end

@program water vs fs
