#version 330

in vec3 vertexPosition;
in vec3 vertexNormal;
in vec2 vertexTexCoord;

uniform mat4 wModel;
uniform mat4 wView;
uniform mat4 wProj;
uniform vec3 cameraPos;
uniform float globalTime;

uniform vec3 waterBodyPosition;
uniform float waterBodyHeight;
uniform vec3 waterBodySize;
uniform vec4 waterBodyBaseColor;
uniform vec4 waterBodyNoiseParams1;
uniform vec4 waterBodyNoiseParams2;
uniform vec2 waterBodyNoiseDirection;
uniform vec4 waterBodyFoamParams;
uniform vec3 waterBodyFoamColor;

// Ken Perlin's improved Simplex permutation table (512 entries, uploaded from CPU).
uniform int perm[512];

out vec3 worldPos;
out vec3 worldNormal;
out vec3 viewDir;
out vec2 texCoord;
out float heightOffset;
out float noiseValue;
flat out vec2 vsCamXZ;
out float fragDist;

// 12 gradient vectors — must match CPU WaterNoise.cpp exactly.
const vec3 grad3[12] = vec3[12](
    vec3(1,1,0), vec3(-1,1,0), vec3(1,-1,0), vec3(-1,-1,0),
    vec3(1,0,1), vec3(-1,0,1), vec3(1,0,-1), vec3(-1,0,-1),
    vec3(0,1,1), vec3(0,-1,1), vec3(0,1,-1), vec3(0,-1,-1)
);

// Improved Simplex 3D — same algorithm as CPU WaterNoise::Simplex3D().
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

    int gi0 = perm[ii + perm[jj + perm[kk]]] % 12;
    int gi1 = perm[ii + i1 + perm[jj + j1 + perm[kk + k1]]] % 12;
    int gi2 = perm[ii + i2 + perm[jj + j2 + perm[kk + k2]]] % 12;
    int gi3 = perm[ii + 1 + perm[jj + 1 + perm[kk + 1]]] % 12;

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

    // Fragment distance from the camera (symmetric in azimuth) — used for a
    // reflection distance fade that does not depend on screen position.
    fragDist = distance(cameraPos, worldPos4.xyz);

    vec3 noisePos = vec3((worldXZ.x - flowOffset.x) * frequency,
                         timeY,
                         (worldXZ.y - flowOffset.y) * frequency);

    float noise = fbm(noisePos, octaves, persistence, lacunarity, seed);

    float heightOffsetLocal = noise * amplitude;
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

    vec3 localNormal = normalize(vec3(-(hR - hL) / (2.0 * eps), 1.0, -(hU - hD) / (2.0 * eps)));

    worldPos = worldPos4.xyz;
    vsCamXZ = cameraPos.xz;

    mat3 normalMatrix = mat3(transpose(inverse(wModel)));
    worldNormal = normalize(normalMatrix * localNormal);
    viewDir = normalize(cameraPos - worldPos);
    texCoord = vertexTexCoord;
    heightOffset = heightOffsetLocal;
    noiseValue = noise;

    gl_Position = wProj * wView * worldPos4;
}
