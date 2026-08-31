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

    // Wave foam from vertex noise
    float foamPattern = noiseValue * 0.5 + 0.5;
    float waveFoam = smoothstep(waterBodyFoamParams.z, waterBodyFoamParams.z + 0.1, foamPattern);

    // Fine-grain foam texture, tiled by foam.scale (waterBodyFoamParams.y)
    float foamDetail = vnoise(worldPos.xz * waterBodyFoamParams.y) * 0.5 + 0.5;
    waveFoam *= (0.5 + 0.5 * foamDetail);

    totalFoam = waveFoam * waterBodyFoamParams.x;

    // Object proximity foam
    float speed = waterBodyNoiseParams1.z;
    float time = waterBodyNoiseParams2.w * speed;
    for (int i = 0; i < objectCount && i < 16; i++) {
        vec3 objPos = objectPositions[i].xyz;
        float objRadius = objectPositions[i].w;

        float dx = abs(worldPos.x - objPos.x) - objRadius;
        float dz = abs(worldPos.z - objPos.z) - objRadius;
        float dist = max(dx, dz);

        float ring = smoothstep(3.0, 0.0, dist);

        // Simple animated noise for organic foam look
        float n = vnoise(worldPos.xz * 1.5 + time * 0.4) * 0.5 + 0.5;
        ring *= (0.5 + 0.5 * n) * waterBodyFoamParams.x;

        totalFoam = max(totalFoam, ring);
    }

    totalFoam = clamp(totalFoam, 0.0, 1.0);

    // Lighting
    float diffuse = smoothstep(0.0, 0.01, NdotL) * 0.6 + 0.4;
    float spec = pow(NdotH, 32.0) * 0.7;
    float rim = pow(1.0 - NdotV, 2.5) * 0.4;

    vec3 color = baseColor * diffuse;
    color += waterBodyFoamColor * totalFoam;
    color += vec3(1.0) * spec;
    color += baseColor * rim;

    fragColor = vec4(color, alpha * chunkFade);
}
