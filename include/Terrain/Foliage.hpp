#pragma once
#include "raylib.h"
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

class BasicTerrain;

// Foliage: grass, flowers, bushes, trees, pines and rocks scattered over a BasicTerrain by rules.
//
// Nothing is stored per plant. A rule says "grow this kind of plant on this painted texture layer, this dense, up to
// this steep, between these heights", and the plants are placed from that when they come into view (in square cells
// round the camera), the same way every time. Painting a layer with the terrain brush grows plants there; sculpting
// moves them with the ground. Plants are drawn instanced from persistent GPU buffers, sway in the wind, and the tall
// kinds cast shadows.
namespace foliage {

enum class Kind { Grass, Flowers, Bush, Tree, Pine, Rock, Count };

const char* KindName(Kind k);
bool KindFromName(const char* name, Kind& out);

struct Rule {
    Kind kind = Kind::Grass;
    bool enabled = true;
    int layer = 0;              // terrain texture layer (0..3) the plants grow on; -1 = everywhere
    float minWeight = 0.35f;    // how much of that layer must be painted at a spot (0..1)
    float density = 60.0f;      // plants per 100 square metres of fully painted ground
    float minScale = 0.8f;      // size range; 1 = the kind's natural size (grass 0.6 m, a tree 7 m)
    float maxScale = 1.3f;
    float maxSlope = 35.0f;     // steepest ground it grows on, in degrees
    float minAltitude = -10000.0f;   // lowest and highest ground height (metres, world)
    float maxAltitude = 10000.0f;
    float viewDistance = 70.0f; // metres from the camera
    int seed = 1;
    // Distance fade. Plants thin out at random from `fadeStart` (a fraction of viewDistance) and are all gone at
    // viewDistance; `farKeep` is the share that lasts until the very end (0 = a steady thinning, 1 = none leave early).
    float fadeStart = 0.45f;
    float farKeep = 0.0f;
    // Level of detail: from `lod1` x viewDistance a plant uses a mesh with about half the triangles, from `lod2` x the barest
    // silhouette (0 = always the cheapest, 1 = always full detail).
    float lod1 = 0.25f;
    float lod2 = 0.55f;
    // Painted rules ignore the texture layer: plants grow where the foliage brush has painted (a mask of kMaskRes x
    // kMaskRes texels over the terrain; shared between copies of the rule).
    bool painted = false;
    std::shared_ptr<std::vector<uint8_t>> mask;
    Color tintA = { 255, 255, 255, 255 };   // each plant gets a colour between these two
    Color tintB = { 255, 255, 255, 255 };
};

// True when two rules place the same plants.
bool SameRule(const Rule& a, const Rule& b);

struct Settings {
    bool enabled = true;
    float wind = 0.6f;          // how far plants bend (0 = still); multiplies the Lighting wind speed
    float density = 1.0f;       // multiplies every rule's density (the quality knob; places the plants again)
    float distance = 1.0f;      // multiplies every rule's view distance
    std::vector<Rule> rules;
};

// A sensible rule for a kind (density, size, distance, tints).
Rule DefaultRule(Kind kind);

// The settings of one terrain, created empty on first use. Has() says whether it has any.
Settings& For(BasicTerrain* terrain);
bool Has(const BasicTerrain* terrain);
void Forget(const BasicTerrain* terrain);   // the terrain is going away
void Invalidate(const BasicTerrain* terrain);   // rules changed: place the plants again

// Draws every terrain's plants. `shadowPass` draws only what casts shadows, nearer in. Call inside the 3D pass
// (after the opaque scene) and inside each shadow cascade.
void Draw(const Camera3D& camera, bool shadowPass);
void Shutdown();                            // frees the GPU buffers and meshes

int MeshTriangles(Kind kind, int lod);       // triangles of one kind at one level of detail (0 = full, 2 = barest)
int InstanceCount();                        // plants placed so far (cells in range of the camera)
int DrawnCount();                           // plants drawn last frame

// Saved with the project as foliage.cfg beside basicterrain.bt, one block per terrain (by position in the terrain
// list). Read() applies to the terrains that exist at that point.
bool Write(std::ostream& out);
int Read(std::istream& in);
bool SaveFile(const std::string& path);     // false when there is nothing to save or it cannot be written
int LoadFile(const std::string& path);      // terrains configured

constexpr int kMaskRes = 512;

// The foliage brush. Paints rule `rule` of `terrain` (it becomes a painted rule); `terrain` null or `rule` < 0 turns
// painting off. BasicTerrain::Update() calls Paint() while the mouse is down.
void SetPaintTarget(const BasicTerrain* terrain, int rule);
bool PaintTarget(const BasicTerrain* terrain, int* rule = nullptr);
// One stamp of the brush: adds (or, erasing, takes away) `amount` (0..1) of full coverage at its middle, less towards
// the rim (`hardness` is how far out it stays full).
void Paint(BasicTerrain* terrain, float worldX, float worldZ, float radius, float amount, float hardness, bool erase);
void FillMask(BasicTerrain* terrain, int rule, bool filled);    // whole terrain painted / cleared

// Where plants stand, for a rule: tests and tools place them without drawing. Appends world-space positions.
void Scatter(const BasicTerrain& terrain, const Rule& rule, float minX, float minZ, float maxX, float maxZ,
             std::vector<Vector3>& positions);

} // namespace foliage
