#pragma once

// Ken Perlin's improved Simplex noise - standalone module.
// CPU implementation matches GPU GLSL in shaders/water.vert exactly
// (same permutation table, same gradient vectors, same algorithm).

namespace WaterNoise {

// Generate shared permutation/gradient tables (auto-called on first use).
void Initialize();

// Access raw permutation table for uploading to GPU (512 unsigned chars).
const unsigned char* GetPermutationTable();
int GetPermutationTableSize();

// Core noise (single octave, range approx [-1, 1]).
float Simplex3D(float x, float y, float z, int seed);
float Simplex2D(float x, float y, int seed);

// Fractal Brownian Motion (multiple octaves, normalized to [-1, 1]).
float FBM3D(float x, float y, float z, int octaves, float persistence, float lacunarity, int seed);
float FBM2D(float x, float y, int octaves, float persistence, float lacunarity, int seed);

} // namespace WaterNoise
