#pragma once

#include "Terrain.hpp"
#include "TerrainTypes.hpp"

namespace terrain {

// ============================================================================
// Heightmap Import/Export
// ============================================================================

// Load heightmap from file (PNG, RAW16, EXR, TIFF) and apply to terrain
bool LoadHeightmap(Terrain& terrain, const std::string& path, const HeightmapImportSettings& settings = {});

// Save terrain heightmap to file (PNG 16-bit grayscale)
bool SaveHeightmap(const Terrain& terrain, const std::string& path);

// Generate procedural terrain from noise parameters
void GenerateFromNoise(Terrain& terrain, const NoiseParams& params);

// RAW16 format (16-bit raw grayscale, no header)
bool ExportRaw16(const Terrain& terrain, const std::string& path);
bool ImportRaw16(Terrain& terrain, const std::string& path, const HeightmapImportSettings& settings);

} // namespace terrain