#include "Engine.hpp"
#include "City.hpp"
#include "CityEditor.hpp"
#include "Graphics.hpp"
#include "city_shims.h"
int main() {
    Mesh mesh = { 0 };
    mesh.vertexCount = 630;
    Model m = LoadModelFromMesh(mesh);
    m.materials[0].shader = gfx::GetLitShader();
    m.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = gfx::GetDefaultTexture();
    std::printf("repro ok materials=%p meshCount=%d\n", (void*)m.materials, m.meshCount);
    return 0;
}