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
uniform float globalTime;

uniform int objectCount;
uniform vec4 objectPositions[16];
uniform float chunkFade;

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

void main() {
    vec3 N = normalize(worldNormal);
    vec3 V = normalize(viewDir);
    vec3 lightDir = normalize(vec3(0.3, 1.0, 0.4));
    vec3 L = normalize(lightDir);
    vec3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);

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
    vec3 skyColor = vec3(0.65, 0.78, 0.88);

    vec3 color = mix(baseColor * diffuse, skyColor, fresnel * 0.6);
    color += waterBodyFoamColor * totalFoam;
    color += vec3(1.0) * spec;

    float edgeAlpha = mix(alpha, 1.0, fresnel * 0.5);

    fragColor = vec4(color, edgeAlpha * chunkFade);
}
