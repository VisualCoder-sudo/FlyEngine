#version 330

in vec3 worldPos;
in vec3 worldNormal;
in vec3 viewDir;
in vec2 texCoord;
in float heightOffset;
in float noiseValue;

uniform vec4 waterBodyBaseColor;
uniform vec4 waterBodyNoiseParams1;
uniform vec4 waterBodyNoiseParams2;
uniform vec4 waterBodyFoamParams;
uniform vec3 waterBodyFoamColor;
uniform vec4 waterBodyDetailParams; // intensity, scale, speed, _pad
uniform float globalTime;

uniform sampler2D reflectionTex;    // planar mirror of the world captured from the reflected camera
uniform mat4 reflViewProj;          // view-projection of the reflected camera
uniform vec4 reflParams;            // x=strength, y=distortion, z=distance fade, w=enabled

uniform int objectCount;
uniform vec4 objectPositions[16];
uniform float chunkFade;
flat in vec2 vsCamXZ;
uniform vec2 farRimParams; // x=clip radius (0 disables), y=feather width

out vec4 fragColor;

in float fragDist;

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
    // one flat sweep. Pure shading — no geometry or physics impact. The field
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
    // the captured world image there (standard mirror trick — no manual ray math).
    vec4 reflClip = reflViewProj * vec4(worldPos, 1.0);
    vec2 reflUV = reflClip.xy / max(reflClip.w, 0.0001) * 0.5 + 0.5;

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
    vec2 reflSampleUV = clamp(reflUVClamped + distortUV, vec2(0.0), vec2(1.0));

    vec3 reflectionColor = texture(reflectionTex, reflSampleUV).rgb;
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
