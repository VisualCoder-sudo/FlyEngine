#pragma once
#include "raylib.h"

#include <cstdint>
#include <functional>
#include <random>
#include <vector>

// Walkable-surface navigation: bakes a layered grid from ray casts into the world, then finds paths over it with A*.
//
// The grid has one column per `cellSize` square of the floor plan. A column holds one cell for every surface an agent
// can stand on (a floor, a ramp, a platform above it). Two neighbouring cells are linked when the step between them is
// small enough, nothing thin stands between them and the agent fits. Cells too close to a wall or ledge for the agent's
// radius are removed.
//
// The mesh knows nothing about physics: `Build` is handed a ray function. It must report the first thing that is part
// of the level (leave out characters, projectiles and anything that moves).
namespace nav {

struct Params {
    Vector3 min = { -50.0f, -10.0f, -50.0f };   // the box that gets baked
    Vector3 max = { 50.0f, 40.0f, 50.0f };
    float cellSize = 0.5f;      // metres per column; smaller is finer and slower
    float agentRadius = 0.4f;   // cells closer than this to an obstacle or ledge are dropped
    float agentHeight = 1.8f;   // free space needed above a cell
    float maxStep = 0.5f;       // the highest step an agent walks up or down
    float maxSlope = 45.0f;     // degrees; steeper surfaces are not walkable
};

// The first solid hit along a ray, false for none. `direction` is a unit vector.
using RayFn = std::function<bool(Vector3 origin, Vector3 direction, float maxDistance, Vector3& point, Vector3& normal)>;

enum class PathResult { None = 0, Complete = 1, Partial = 2 };

class Mesh {
public:
    // False (and the mesh left empty) for unusable parameters, a grid over kMaxColumns, or a world with no floor.
    bool Build(const Params& params, const RayFn& cast);
    void Clear();

    bool Built() const { return !m_cells.empty(); }
    size_t CellCount() const;                   // cells an agent can stand on
    int ColumnsX() const { return m_nx; }
    int ColumnsZ() const { return m_nz; }
    const Params& GetParams() const { return m_params; }

    // The walkable point nearest `p` (looked for a few cells around and a couple of agent heights up and down).
    bool Nearest(Vector3 p, Vector3& out) const;
    // True when `p` is standing on the mesh: within one cell sideways and a step vertically.
    bool IsWalkable(Vector3 p) const;
    // A random walkable point within `radius` (flat distance) of `center`.
    bool RandomPoint(Vector3 center, float radius, Vector3& out) const;

    // From `from` to `to`, both snapped onto the mesh. The path starts at `from` and ends at `to` (or at the nearest
    // reachable point when `allowPartial` is set and `to` cannot be reached; just the start when that is already the
    // nearest). Straightened: only the corners remain.
    PathResult FindPath(Vector3 from, Vector3 to, std::vector<Vector3>& out, bool allowPartial = false) const;

    // True when an agent at `from` can walk straight to `to` without leaving the mesh. `from` is snapped onto the mesh
    // first, then the line is walked from where it really is (not from a cell centre), so it answers for an agent that
    // stands anywhere inside a cell. `to` must end up within an agent height of the surface the line reaches.
    bool LineClear(Vector3 from, Vector3 to) const;
    // Moves `from` by the flat part of `delta` over the walkable cells, sliding along blocked edges instead of crossing
    // them, so `out` is always on the mesh. A `from` up to two cells off the mesh is first pulled onto the nearest cell.
    // `out.y` is the ground height there. False (and `out` = `from` + `delta`) when there is no walkable ground near
    // `from`.
    bool MoveAlong(Vector3 from, Vector3 delta, Vector3& out) const;

    // Wire view of the cells within `radius` of `around`, plus `path` when given. Call between BeginMode3D / EndMode3D.
    void DrawDebug(Vector3 around, float radius, const std::vector<Vector3>* path) const;

    static constexpr int kMaxColumns = 4000000;
    static constexpr int kMaxLayers = 8;

private:
    struct Cell {
        float y = 0.0f;
        uint32_t column = 0;
        bool alive = true;
        int32_t link[8];   // E, NE, N, NW, W, SW, S, SE (x then z); -1 = no way through
    };

    Vector3 CellPosition(int cell) const;
    int ColumnOf(float x, float z) const;                  // -1 outside the grid
    int Snap(Vector3 p) const;                              // nearest living cell, -1 if none close
    bool LineClear(int from, int to) const;
    int CellUnder(Vector3 p) const;                         // the living cell in p's column nearest its height, -1 if none
    int StartCell(Vector3 p) const;                         // CellUnder, else Snap
    int Walk(int start, Vector3 a, Vector3 b) const;        // the cell the line from `a` ends on, -1 if it leaves the mesh
    float SurfaceY(int cell, float x, float z) const;       // ground height inside a cell, blended towards its neighbours
    void Erode(int cells);
    void DropBrokenDiagonals();

    Params m_params;
    int m_nx = 0, m_nz = 0;
    std::vector<uint32_t> m_columnStart;   // m_nx * m_nz + 1 entries into m_cells
    std::vector<Cell> m_cells;

    // Search scratch, reused between calls. A mesh is not safe to search from two threads.
    mutable std::vector<float> m_g;
    mutable std::vector<int32_t> m_parent;
    mutable std::vector<uint32_t> m_seen, m_closed;
    mutable uint32_t m_stamp = 0;
    mutable std::mt19937 m_rng{ 12345u };
};

// The mesh the scripting API bakes and searches, the debug-view switch and the last path a script asked for.
struct Active {
    Mesh mesh;
    bool debug = false;
    std::vector<Vector3> lastPath;
};
Active& World();
void ResetWorld();   // when play starts and stops
// Draws the debug view of the active mesh around `around` if a script switched it on. Call inside the 3D pass.
void DrawWorldDebug(Vector3 around);

} // namespace nav
