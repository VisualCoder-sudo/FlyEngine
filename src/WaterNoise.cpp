#include "WaterNoise.hpp"
#include <cmath>
#include <algorithm>

namespace WaterNoise {

static unsigned char s_perm[512];
static bool s_initialized = false;

static const int s_grad3[12][3] = {
    {1,1,0}, {-1,1,0}, {1,-1,0}, {-1,-1,0},
    {1,0,1}, {-1,0,1}, {1,0,-1}, {-1,0,-1},
    {0,1,1}, {0,-1,1}, {0,1,-1}, {0,-1,-1}
};

static const int s_grad2[8][2] = {
    {1,0}, {-1,0}, {0,1}, {0,-1},
    {1,1}, {-1,1}, {1,-1}, {-1,-1}
};

void Initialize() {
    if (s_initialized) return;

    for (int i = 0; i < 256; i++) s_perm[i] = (unsigned char)i;
    for (int i = 255; i > 0; i--) {
        int j = rand() % (i + 1);
        unsigned char tmp = s_perm[i];
        s_perm[i] = s_perm[j];
        s_perm[j] = tmp;
    }
    for (int i = 0; i < 256; i++) s_perm[i + 256] = s_perm[i];

    s_initialized = true;
}

const unsigned char* GetPermutationTable() {
    Initialize();
    return s_perm;
}

int GetPermutationTableSize() {
    return 512;
}

static inline float dot3(int g[3], float x, float y, float z) {
    return g[0]*x + g[1]*y + g[2]*z;
}

static inline float dot2(int g[2], float x, float y) {
    return g[0]*x + g[1]*y;
}

float Simplex3D(float x, float y, float z, int seed) {
    Initialize();

    const float F3 = 1.0f / 3.0f;
    const float G3 = 1.0f / 6.0f;

    float s = (x + y + z) * F3;
    int i = (int)floorf(x + s);
    int j = (int)floorf(y + s);
    int k = (int)floorf(z + s);

    float t = (float)(i + j + k) * G3;
    float x0 = x - ((float)i - t);
    float y0 = y - ((float)j - t);
    float z0 = z - ((float)k - t);

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

    float x1 = x0 - (float)i1 + G3;
    float y1 = y0 - (float)j1 + G3;
    float z1 = z0 - (float)k1 + G3;
    float x2 = x0 - (float)i2 + 2.0f*G3;
    float y2 = y0 - (float)j2 + 2.0f*G3;
    float z2 = z0 - (float)k2 + 2.0f*G3;
    float x3 = x0 - 1.0f + 3.0f*G3;
    float y3 = y0 - 1.0f + 3.0f*G3;
    float z3 = z0 - 1.0f + 3.0f*G3;

    int ii = (i + seed) & 255;
    int jj = (j + seed) & 255;
    int kk = (k + seed) & 255;

    int gi0 = s_perm[ii + s_perm[jj + s_perm[kk]]] % 12;
    int gi1 = s_perm[ii + i1 + s_perm[jj + j1 + s_perm[kk + k1]]] % 12;
    int gi2 = s_perm[ii + i2 + s_perm[jj + j2 + s_perm[kk + k2]]] % 12;
    int gi3 = s_perm[ii + 1 + s_perm[jj + 1 + s_perm[kk + 1]]] % 12;

    float t0 = 0.6f - x0*x0 - y0*y0 - z0*z0;
    float n0 = 0.0f;
    if (t0 > 0.0f) { t0 *= t0; n0 = t0 * t0 * dot3((int*)s_grad3[gi0], x0, y0, z0); }

    float t1 = 0.6f - x1*x1 - y1*y1 - z1*z1;
    float n1 = 0.0f;
    if (t1 > 0.0f) { t1 *= t1; n1 = t1 * t1 * dot3((int*)s_grad3[gi1], x1, y1, z1); }

    float t2 = 0.6f - x2*x2 - y2*y2 - z2*z2;
    float n2 = 0.0f;
    if (t2 > 0.0f) { t2 *= t2; n2 = t2 * t2 * dot3((int*)s_grad3[gi2], x2, y2, z2); }

    float t3 = 0.6f - x3*x3 - y3*y3 - z3*z3;
    float n3 = 0.0f;
    if (t3 > 0.0f) { t3 *= t3; n3 = t3 * t3 * dot3((int*)s_grad3[gi3], x3, y3, z3); }

    return 32.0f * (n0 + n1 + n2 + n3);
}

float Simplex2D(float x, float y, int seed) {
    Initialize();

    const float F2 = 0.5f * (sqrtf(3.0f) - 1.0f);
    const float G2 = (3.0f - sqrtf(3.0f)) / 6.0f;

    float s = (x + y) * F2;
    int i = (int)floorf(x + s);
    int j = (int)floorf(y + s);

    float t = (float)(i + j) * G2;
    float x0 = x - ((float)i - t);
    float y0 = y - ((float)j - t);

    int i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; }
    else          { i1 = 0; j1 = 1; }

    float x1 = x0 - (float)i1 + G2;
    float y1 = y0 - (float)j1 + G2;
    float x2 = x0 - 1.0f + 2.0f*G2;
    float y2 = y0 - 1.0f + 2.0f*G2;

    int ii = (i + seed) & 255;
    int jj = (j + seed) & 255;

    int gi0 = s_perm[ii + s_perm[jj]] % 8;
    int gi1 = s_perm[ii + i1 + s_perm[jj + j1]] % 8;
    int gi2 = s_perm[ii + 1 + s_perm[jj + 1]] % 8;

    float t0 = 0.5f - x0*x0 - y0*y0;
    float n0 = 0.0f;
    if (t0 > 0.0f) { t0 *= t0; n0 = t0 * t0 * dot2((int*)s_grad2[gi0], x0, y0); }

    float t1 = 0.5f - x1*x1 - y1*y1;
    float n1 = 0.0f;
    if (t1 > 0.0f) { t1 *= t1; n1 = t1 * t1 * dot2((int*)s_grad2[gi1], x1, y1); }

    float t2 = 0.5f - x2*x2 - y2*y2;
    float n2 = 0.0f;
    if (t2 > 0.0f) { t2 *= t2; n2 = t2 * t2 * dot2((int*)s_grad2[gi2], x2, y2); }

    return 70.0f * (n0 + n1 + n2);
}

float FBM3D(float x, float y, float z, int octaves, float persistence, float lacunarity, int seed) {
    float value = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float maxValue = 0.0f;

    for (int i = 0; i < octaves; i++) {
        value += amplitude * Simplex3D(x * frequency, y * frequency, z * frequency, seed);
        maxValue += amplitude;
        amplitude *= persistence;
        frequency *= lacunarity;
    }

    return value / maxValue;
}

float FBM2D(float x, float y, int octaves, float persistence, float lacunarity, int seed) {
    float value = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float maxValue = 0.0f;

    for (int i = 0; i < octaves; i++) {
        value += amplitude * Simplex2D(x * frequency, y * frequency, seed);
        maxValue += amplitude;
        amplitude *= persistence;
        frequency *= lacunarity;
    }

    return value / maxValue;
}

} // namespace WaterNoise
