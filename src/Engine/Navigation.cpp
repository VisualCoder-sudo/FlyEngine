#include "../../include/Engine/Navigation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace nav {
namespace {

constexpr int kDx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
constexpr int kDz[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };

int DirectionOf(int dx, int dz) {
    for (int d = 0; d < 8; ++d) if (kDx[d] == dx && kDz[d] == dz) return d;
    return -1;
}

bool Finite(const Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

} // namespace

void Mesh::Clear() {
    m_nx = m_nz = 0;
    m_columnStart.clear();
    m_cells.clear();
    m_g.clear(); m_parent.clear(); m_seen.clear(); m_closed.clear();
    m_stamp = 0;
}

size_t Mesh::CellCount() const {
    size_t n = 0;
    for (const Cell& c : m_cells) if (c.alive) ++n;
    return n;
}

bool Mesh::Build(const Params& p, const RayFn& cast) {
    Clear();
    if (!cast || !Finite(p.min) || !Finite(p.max)) return false;
    if (!(p.cellSize >= 0.05f) || !(p.agentHeight > 0.0f) || !(p.agentRadius >= 0.0f) || !(p.maxStep >= 0.0f)) return false;
    if (!(p.maxSlope >= 1.0f && p.maxSlope <= 89.0f)) return false;
    if (!(p.max.x > p.min.x) || !(p.max.y > p.min.y) || !(p.max.z > p.min.z)) return false;

    const double nxd = std::ceil((static_cast<double>(p.max.x) - p.min.x) / p.cellSize);
    const double nzd = std::ceil((static_cast<double>(p.max.z) - p.min.z) / p.cellSize);
    if (nxd * nzd > kMaxColumns) return false;
    m_params = p;
    m_nx = static_cast<int>(nxd);
    m_nz = static_cast<int>(nzd);

    const float cs = p.cellSize;
    const float cosSlope = std::cos(p.maxSlope * 3.14159265f / 180.0f);
    const float tanSlope = std::tan(p.maxSlope * 3.14159265f / 180.0f);
    const int columns = m_nx * m_nz;
    m_columnStart.assign(static_cast<size_t>(columns) + 1, 0);

    // 1. Surfaces: shoot down each column, keep the flat-enough hits that have room for the agent above.
    for (int c = 0; c < columns; ++c) {
        m_columnStart[c] = static_cast<uint32_t>(m_cells.size());
        const float x = p.min.x + (static_cast<float>(c % m_nx) + 0.5f) * cs;
        const float z = p.min.z + (static_cast<float>(c / m_nx) + 0.5f) * cs;
        float top = p.max.y;
        int layers = 0;
        bool hasPrev = false;
        float prevY = 0.0f;   // the surface the last ray hit, kept or not
        for (int guard = 0; guard < kMaxLayers * 4 && layers < kMaxLayers && top > p.min.y; ++guard) {
            Vector3 hit{}, normal{};
            if (!cast({ x, top, z }, { 0, -1, 0 }, top - p.min.y, hit, normal)) break;
            if (!std::isfinite(hit.y)) break;
            if (top - hit.y < 0.01f) { top = hit.y - 0.25f; continue; }   // began inside something solid
            if (normal.y >= cosSlope) {
                // Look up from the surface. Under another surface this must reach its underside: a ray that sails
                // through means the ray from above was inside a solid (a floor beneath a crate), so nobody can stand here.
                const float reach = hasPrev ? std::max(prevY - hit.y + 0.1f, p.agentHeight) : p.agentHeight;
                Vector3 ceilingHit{}, ceilingNormal{};
                const bool roof = cast({ x, hit.y + 0.02f, z }, { 0, 1, 0 }, reach, ceilingHit, ceilingNormal);
                bool open;
                if (!roof) open = !hasPrev;
                else open = ceilingHit.y - hit.y >= p.agentHeight - 0.02f && !(hasPrev && ceilingHit.y > prevY + 0.02f);
                if (open) {
                    Cell cell;
                    cell.y = hit.y;
                    cell.column = static_cast<uint32_t>(c);
                    std::fill(std::begin(cell.link), std::end(cell.link), -1);
                    m_cells.push_back(cell);
                    ++layers;
                }
            }
            hasPrev = true;
            prevY = hit.y;
            top = hit.y - 0.05f;
        }
    }
    m_columnStart[columns] = static_cast<uint32_t>(m_cells.size());
    if (m_cells.empty()) { Clear(); return false; }

    // 2. Links between sideways neighbours (E and N, mirrored to W and S). A step is fine when it is low or the ground
    // between the two really is a ramp, and two rays at foot and head height must not hit anything between the centres.
    auto wallBetween = [&](const Cell& a, const Cell& b, int ax, int az, int bx, int bz) {
        const Vector3 from = { p.min.x + (ax + 0.5f) * cs, 0, p.min.z + (az + 0.5f) * cs };
        const Vector3 to = { p.min.x + (bx + 0.5f) * cs, 0, p.min.z + (bz + 0.5f) * cs };
        const float len = std::sqrt((to.x - from.x) * (to.x - from.x) + (to.z - from.z) * (to.z - from.z));
        const Vector3 dir = { (to.x - from.x) / len, 0, (to.z - from.z) / len };
        const float base = std::max(a.y, b.y);
        const float heights[2] = { base + p.maxStep + 0.05f, base + std::max(p.maxStep + 0.1f, p.agentHeight - 0.1f) };
        for (const float h : heights) {
            Vector3 hit{}, normal{};
            if (cast({ from.x, h, from.z }, dir, len, hit, normal)) return true;
        }
        return false;
    };

    for (int c = 0; c < columns; ++c) {
        const int cx = c % m_nx, cz = c / m_nx;
        for (const int d : { 0, 2 }) {
            const int nx = cx + kDx[d], nz = cz + kDz[d];
            if (nx >= m_nx || nz >= m_nz) continue;
            const int other = nz * m_nx + nx;
            const float reach = std::max(p.maxStep, tanSlope * cs);
            for (uint32_t i = m_columnStart[c]; i < m_columnStart[c + 1]; ++i) {
                int best = -1;
                float bestDy = std::numeric_limits<float>::max();
                for (uint32_t j = m_columnStart[other]; j < m_columnStart[other + 1]; ++j) {
                    const float dy = std::fabs(m_cells[j].y - m_cells[i].y);
                    if (dy <= reach && dy < bestDy) { best = static_cast<int>(j); bestDy = dy; }
                }
                if (best < 0 || wallBetween(m_cells[i], m_cells[best], cx, cz, nx, nz)) continue;
                m_cells[i].link[d] = best;
                m_cells[best].link[d + 4] = static_cast<int32_t>(i);
            }
        }
    }
    // Diagonals only where both ways round the corner are open and lead to the same cell.
    for (int i = 0; i < static_cast<int>(m_cells.size()); ++i) {
        for (int d = 1; d < 8; d += 2) {
            const int a = (d + 7) & 7, b = (d + 1) & 7;
            const int o1 = m_cells[i].link[a], o2 = m_cells[i].link[b];
            if (o1 < 0 || o2 < 0) continue;
            const int t = m_cells[o1].link[b];
            if (t >= 0 && t == m_cells[o2].link[a]) m_cells[i].link[d] = t;
        }
    }

    // 3. Keep the agent's radius away from walls and ledges.
    const int erode = std::max(0, static_cast<int>(std::ceil(p.agentRadius / cs - 0.5f)));
    if (erode > 0) Erode(erode);
    if (m_cells.empty() || CellCount() == 0) { Clear(); return false; }
    m_g.assign(m_cells.size(), 0.0f);
    m_parent.assign(m_cells.size(), -1);
    m_seen.assign(m_cells.size(), 0);
    m_closed.assign(m_cells.size(), 0);
    return true;
}

void Mesh::Erode(int rings) {
    const int n = static_cast<int>(m_cells.size());
    std::vector<int> dist(n, -1);
    std::vector<int> frontier;
    for (int i = 0; i < n; ++i) {
        for (int d = 0; d < 8; ++d) {
            if (m_cells[i].link[d] < 0) { dist[i] = 0; frontier.push_back(i); break; }
        }
    }
    for (size_t head = 0; head < frontier.size(); ++head) {
        const int i = frontier[head];
        if (dist[i] + 1 >= rings) continue;
        for (int d = 0; d < 8; ++d) {
            const int j = m_cells[i].link[d];
            if (j >= 0 && dist[j] < 0) { dist[j] = dist[i] + 1; frontier.push_back(j); }
        }
    }
    for (int i = 0; i < n; ++i) {
        if (dist[i] < 0 || dist[i] >= rings) continue;
        m_cells[i].alive = false;
    }
    for (int i = 0; i < n; ++i) {
        for (int d = 0; d < 8; ++d) {
            const int j = m_cells[i].link[d];
            if (j >= 0 && (!m_cells[i].alive || !m_cells[j].alive)) m_cells[i].link[d] = -1;
        }
    }
    DropBrokenDiagonals();
}

void Mesh::DropBrokenDiagonals() {
    for (int i = 0; i < static_cast<int>(m_cells.size()); ++i) {
        for (int d = 1; d < 8; d += 2) {
            const int t = m_cells[i].link[d];
            if (t < 0) continue;
            const int a = (d + 7) & 7, b = (d + 1) & 7;
            const int o1 = m_cells[i].link[a], o2 = m_cells[i].link[b];
            const bool open = o1 >= 0 && o2 >= 0 && m_cells[o1].link[b] == t && m_cells[o2].link[a] == t;
            if (!open) {
                m_cells[i].link[d] = -1;
                if (m_cells[t].link[(d + 4) & 7] == i) m_cells[t].link[(d + 4) & 7] = -1;
            }
        }
    }
}

Vector3 Mesh::CellPosition(int cell) const {
    const Cell& c = m_cells[cell];
    return { m_params.min.x + (static_cast<float>(c.column % m_nx) + 0.5f) * m_params.cellSize, c.y,
             m_params.min.z + (static_cast<float>(c.column / m_nx) + 0.5f) * m_params.cellSize };
}

int Mesh::ColumnOf(float x, float z) const {
    if (m_nx <= 0) return -1;
    const float fx = std::floor((x - m_params.min.x) / m_params.cellSize);
    const float fz = std::floor((z - m_params.min.z) / m_params.cellSize);
    if (!(fx >= 0.0f && fz >= 0.0f && fx < static_cast<float>(m_nx) && fz < static_cast<float>(m_nz))) return -1;
    return static_cast<int>(fz) * m_nx + static_cast<int>(fx);
}

int Mesh::Snap(Vector3 p) const {
    if (m_cells.empty() || !Finite(p)) return -1;
    const float cs = m_params.cellSize;
    const int reach = 4;
    const float maxDy = m_params.agentHeight * 2.0f;
    const int cx = static_cast<int>(std::floor((p.x - m_params.min.x) / cs));
    const int cz = static_cast<int>(std::floor((p.z - m_params.min.z) / cs));
    int best = -1;
    float bestScore = std::numeric_limits<float>::max();
    for (int dz = -reach; dz <= reach; ++dz) {
        for (int dx = -reach; dx <= reach; ++dx) {
            const int x = cx + dx, z = cz + dz;
            if (x < 0 || z < 0 || x >= m_nx || z >= m_nz) continue;
            const int col = z * m_nx + x;
            for (uint32_t i = m_columnStart[col]; i < m_columnStart[col + 1]; ++i) {
                if (!m_cells[i].alive) continue;
                const Vector3 q = CellPosition(static_cast<int>(i));
                const float dy = q.y - p.y;
                if (std::fabs(dy) > maxDy) continue;
                // Standing a little above a floor counts as being on it; being under one does not.
                const float score = (q.x - p.x) * (q.x - p.x) + (q.z - p.z) * (q.z - p.z) + (dy * 2.0f) * (dy * 2.0f) + (dy > 0.0f ? 1.0f : 0.0f);
                if (score < bestScore) { bestScore = score; best = static_cast<int>(i); }
            }
        }
    }
    return best;
}

bool Mesh::Nearest(Vector3 p, Vector3& out) const {
    const int c = Snap(p);
    if (c < 0) return false;
    out = CellPosition(c);
    return true;
}

bool Mesh::IsWalkable(Vector3 p) const {
    const int col = ColumnOf(p.x, p.z);
    if (col < 0 || !Finite(p)) return false;
    const float reach = std::max(m_params.maxStep, 0.25f) + 0.05f;
    for (uint32_t i = m_columnStart[col]; i < m_columnStart[col + 1]; ++i)
        if (m_cells[i].alive && std::fabs(m_cells[i].y - p.y) <= reach) return true;
    return false;
}

bool Mesh::RandomPoint(Vector3 center, float radius, Vector3& out) const {
    if (m_cells.empty() || !(radius > 0.0f) || !Finite(center)) return false;
    const float cs = m_params.cellSize;
    const int span = std::min(static_cast<int>(std::ceil(radius / cs)), 400);
    const int cx = static_cast<int>(std::floor((center.x - m_params.min.x) / cs));
    const int cz = static_cast<int>(std::floor((center.z - m_params.min.z) / cs));
    int chosen = -1, seen = 0;
    for (int dz = -span; dz <= span; ++dz) {
        for (int dx = -span; dx <= span; ++dx) {
            const int x = cx + dx, z = cz + dz;
            if (x < 0 || z < 0 || x >= m_nx || z >= m_nz) continue;
            const int col = z * m_nx + x;
            for (uint32_t i = m_columnStart[col]; i < m_columnStart[col + 1]; ++i) {
                if (!m_cells[i].alive) continue;
                const Vector3 q = CellPosition(static_cast<int>(i));
                if ((q.x - center.x) * (q.x - center.x) + (q.z - center.z) * (q.z - center.z) > radius * radius) continue;
                if (std::fabs(q.y - center.y) > 40.0f) continue;
                ++seen;
                if (std::uniform_int_distribution<int>(1, seen)(m_rng) == 1) chosen = static_cast<int>(i);   // reservoir sample
            }
        }
    }
    if (chosen < 0) return false;
    out = CellPosition(chosen);
    return true;
}

// Walks the straight line between two cell centres and checks that every column it crosses is one step along the
// links, so the shortcut is exactly as safe as the path it replaces.
bool Mesh::LineClear(int from, int to) const {
    return Walk(from, CellPosition(from), CellPosition(to)) == to;
}

// Follows the line from `a` to `b` column by column, starting on cell `start`. Every column the line enters must be
// linked to the one before, so the whole line lies on cells an agent may stand on. A start point in a neighbouring
// column (not the snapped cell's own) must be linked to it too. The columns are stepped through exactly, edge by edge,
// so a line clipping the corner of a column is caught however little of it the line crosses; a line through a grid
// corner is a diagonal step, which only exists where both ways round the corner are open.
int Mesh::Walk(int start, Vector3 a, Vector3 b) const {
    if (start < 0 || !Finite(a) || !Finite(b)) return -1;
    int cur = start;
    int x = static_cast<int>(m_cells[cur].column % m_nx), z = static_cast<int>(m_cells[cur].column / m_nx);
    auto stepTo = [&](int nx, int nz) {
        const int d = DirectionOf(nx - x, nz - z);
        if (d < 0 || nx < 0 || nz < 0 || nx >= m_nx || nz >= m_nz) return false;
        const int next = m_cells[cur].link[d];
        if (next < 0) return false;
        cur = next; x = nx; z = nz;
        return true;
    };
    const int first = ColumnOf(a.x, a.z);
    if (first < 0) return -1;
    if ((first % m_nx != x || first / m_nx != z) && !stepTo(first % m_nx, first / m_nx)) return -1;

    const double cs = m_params.cellSize;
    const double dx = static_cast<double>(b.x) - a.x, dz = static_cast<double>(b.z) - a.z;
    const int sx = dx > 0.0 ? 1 : (dx < 0.0 ? -1 : 0), sz = dz > 0.0 ? 1 : (dz < 0.0 ? -1 : 0);
    const double inf = std::numeric_limits<double>::infinity();
    // The line parameter (0 at a, 1 at b) at which it crosses the next column edge along each axis.
    const double edgeX = m_params.min.x + (x + (sx > 0 ? 1 : 0)) * cs, edgeZ = m_params.min.z + (z + (sz > 0 ? 1 : 0)) * cs;
    double tx = sx != 0 ? (edgeX - a.x) / dx : inf, tz = sz != 0 ? (edgeZ - a.z) / dz : inf;
    const double stepX = sx != 0 ? cs / std::fabs(dx) : inf, stepZ = sz != 0 ? cs / std::fabs(dz) : inf;
    // Edges crossed strictly before `b` are stepped over here; `b`'s own column is settled after the loop, so an end
    // lying exactly on an edge lands in the column ColumnOf gives it, whichever way the line runs.
    for (int guard = 0; guard < 4 * (m_nx + m_nz) && std::min(tx, tz) < 1.0 - 1e-9; ++guard) {
        if (std::fabs(tx - tz) < 1e-9) {
            if (!stepTo(x + sx, z + sz)) return -1;
            tx += stepX; tz += stepZ;
        } else if (tx < tz) {
            if (!stepTo(x + sx, z)) return -1;
            tx += stepX;
        } else {
            if (!stepTo(x, z + sz)) return -1;
            tz += stepZ;
        }
    }
    const int last = ColumnOf(b.x, b.z);
    if (last < 0) return -1;
    if ((last % m_nx != x || last / m_nx != z) && !stepTo(last % m_nx, last / m_nx)) return -1;
    return cur;
}

// The living cell in `p`'s own column whose height is closest to `p`, -1 if none is within a step (with room for the
// blended height SurfaceY gives on a slope). Snap is not used for this: it prefers cells below the point, so on the
// lower half of an uphill cell, where the ground is below the cell's centre, it would pick the cell behind.
int Mesh::CellUnder(Vector3 p) const {
    const int col = ColumnOf(p.x, p.z);
    if (col < 0 || !Finite(p)) return -1;
    const float tanSlope = std::tan(m_params.maxSlope * 3.14159265f / 180.0f);
    const float reach = std::max(m_params.maxStep, tanSlope * m_params.cellSize) + 0.1f;
    int best = -1;
    float bestDy = reach;
    for (uint32_t i = m_columnStart[col]; i < m_columnStart[col + 1]; ++i) {
        const float dy = std::fabs(m_cells[i].y - p.y);
        if (m_cells[i].alive && dy <= bestDy) { bestDy = dy; best = static_cast<int>(i); }
    }
    return best;
}

int Mesh::StartCell(Vector3 p) const {
    const int c = CellUnder(p);
    return c >= 0 ? c : Snap(p);
}

bool Mesh::LineClear(Vector3 from, Vector3 to) const {
    const int end = Walk(StartCell(from), from, to);
    return end >= 0 && std::fabs(m_cells[end].y - to.y) <= m_params.agentHeight;
}

// The cell's own height at its centre, sloping half way towards each linked neighbour at its edges. Both sides of an
// edge agree on the height there, so something moved across a ramp rises smoothly instead of in cell-sized steps.
float Mesh::SurfaceY(int cell, float x, float z) const {
    const Cell& c = m_cells[cell];
    const Vector3 centre = CellPosition(cell);
    const float fx = (x - centre.x) / m_params.cellSize, fz = (z - centre.z) / m_params.cellSize;
    float y = c.y;
    const int ex = c.link[fx >= 0.0f ? 0 : 4], ez = c.link[fz >= 0.0f ? 2 : 6];
    if (ex >= 0) y += (m_cells[ex].y - c.y) * std::fabs(fx);
    if (ez >= 0) y += (m_cells[ez].y - c.y) * std::fabs(fz);
    return y;
}

// Moves in quarter-cell sub-steps, one axis at a time (the larger first). An axis that would cross into a column with
// no link stops just short of the edge and the other axis keeps going, which is the slide along a wall. The point only
// ever enters a column through a link, so it can never end up on a removed cell or the far side of a wall.
bool Mesh::MoveAlong(Vector3 from, Vector3 delta, Vector3& out) const {
    out = { from.x + delta.x, from.y + delta.y, from.z + delta.z };
    if (!Finite(delta)) return false;
    int cur = StartCell(from);
    if (cur < 0) return false;
    const float cs = m_params.cellSize;
    const float eps = cs * 1e-3f;
    int cx = static_cast<int>(m_cells[cur].column % m_nx), cz = static_cast<int>(m_cells[cur].column / m_nx);
    float x = from.x, z = from.z;
    if (ColumnOf(x, z) != static_cast<int>(m_cells[cur].column)) {
        // Off the mesh: pull back into the nearest cell's square first, but only from up to two cells away. Further
        // than that it is somewhere else (inside a wall, on a crate), not a little off the edge.
        const float lox = m_params.min.x + static_cast<float>(cx) * cs, loz = m_params.min.z + static_cast<float>(cz) * cs;
        x = std::clamp(x, lox + eps, lox + cs - eps);
        z = std::clamp(z, loz + eps, loz + cs - eps);
        if (std::hypot(x - from.x, z - from.z) > 2.0f * cs) return false;
    }

    // In double: a huge (but finite) delta would overflow when squared in float.
    double dx = delta.x, dz = delta.z;
    const double length = std::hypot(dx, dz);
    const double maxLength = cs * 256.0;   // a teleport, not a step: keep the loop bounded
    if (length > maxLength) { dx *= maxLength / length; dz *= maxLength / length; }
    const int steps = std::max(1, static_cast<int>(std::ceil(std::min(length, maxLength) / (cs * 0.25))));
    const float step[2] = { static_cast<float>(dx / steps), static_cast<float>(dz / steps) };
    const int first = std::fabs(dz) > std::fabs(dx) ? 1 : 0;
    for (int k = 0; k < steps; ++k) {
        for (int n = 0; n < 2; ++n) {
            const int axis = (first + n) & 1;
            const float d = step[axis];
            if (d == 0.0f) continue;
            float& v = axis == 0 ? x : z;
            int& index = axis == 0 ? cx : cz;
            const float lo = (axis == 0 ? m_params.min.x : m_params.min.z) + static_cast<float>(index) * cs;
            const float moved = v + d;
            if (moved >= lo && moved < lo + cs) { v = moved; continue; }
            const int next = m_cells[cur].link[axis == 0 ? (d > 0.0f ? 0 : 4) : (d > 0.0f ? 2 : 6)];
            if (next >= 0) { v = moved; cur = next; index += d > 0.0f ? 1 : -1; }
            else v = d > 0.0f ? lo + cs - eps : lo + eps;
        }
    }
    out = { x, SurfaceY(cur, x, z), z };
    return true;
}

PathResult Mesh::FindPath(Vector3 from, Vector3 to, std::vector<Vector3>& out, bool allowPartial) const {
    out.clear();
    const int start = Snap(from), goal = Snap(to);
    if (start < 0 || goal < 0) return PathResult::None;
    const Vector3 goalPos = CellPosition(goal);

    if (++m_stamp == 0) {   // wrapped: forget every old mark
        std::fill(m_seen.begin(), m_seen.end(), 0u);
        std::fill(m_closed.begin(), m_closed.end(), 0u);
        m_stamp = 1;
    }
    auto heuristic = [&](int cell) {
        const Vector3 p = CellPosition(cell);
        return std::sqrt((p.x - goalPos.x) * (p.x - goalPos.x) + (p.y - goalPos.y) * (p.y - goalPos.y) + (p.z - goalPos.z) * (p.z - goalPos.z));
    };

    using Entry = std::pair<float, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    m_g[start] = 0.0f; m_parent[start] = -1; m_seen[start] = m_stamp;
    open.push({ heuristic(start), start });
    int nearest = start;
    float nearestH = heuristic(start);
    bool found = false;
    while (!open.empty()) {
        const int cur = open.top().second;
        open.pop();
        if (m_closed[cur] == m_stamp) continue;
        m_closed[cur] = m_stamp;
        if (cur == goal) { found = true; break; }
        const float h = heuristic(cur);
        if (h < nearestH) { nearestH = h; nearest = cur; }
        const Vector3 here = CellPosition(cur);
        for (int d = 0; d < 8; ++d) {
            const int next = m_cells[cur].link[d];
            if (next < 0 || m_closed[next] == m_stamp) continue;
            const Vector3 there = CellPosition(next);
            const float cost = std::sqrt((there.x - here.x) * (there.x - here.x) + (there.y - here.y) * (there.y - here.y) + (there.z - here.z) * (there.z - here.z));
            const float g = m_g[cur] + cost;
            if (m_seen[next] != m_stamp || g < m_g[next]) {
                m_seen[next] = m_stamp; m_g[next] = g; m_parent[next] = cur;
                open.push({ g + heuristic(next), next });
            }
        }
    }

    int end = goal;
    PathResult result = PathResult::Complete;
    if (!found) {
        // Already as near as it gets: a one-point path where the caller stands, so an agent knows it has arrived
        // instead of searching again every frame.
        if (!allowPartial) return PathResult::None;
        end = nearest;
        result = PathResult::Partial;
    }

    std::vector<int> cells;
    for (int c = end; c >= 0; c = m_parent[c]) cells.push_back(c);
    std::reverse(cells.begin(), cells.end());

    // The ends are where the caller asked, when that lies within a cell of the snapped ends.
    const float cs = m_params.cellSize;
    const Vector3 startPos = CellPosition(cells[0]), endPos = CellPosition(cells.back());
    bool keepFrom = std::fabs(from.x - startPos.x) <= cs && std::fabs(from.z - startPos.z) <= cs;
    bool keepTo = result == PathResult::Complete && std::fabs(to.x - endPos.x) <= cs && std::fabs(to.z - endPos.z) <= cs;

    // Straighten: from each kept cell jump to the farthest one that can be walked to in a straight line. The first leg
    // is checked from where the caller stands, since that is where the agent walks it from.
    auto clear = [&](size_t a, size_t b) {
        if (a == 0 && keepFrom) return Walk(cells[0], from, CellPosition(cells[b])) == cells[b];
        return LineClear(cells[a], cells[b]);
    };
    std::vector<int> corners;
    size_t i = 0;
    corners.push_back(cells[0]);
    while (i + 1 < cells.size()) {
        size_t j = cells.size() - 1;
        while (j > i + 1 && !clear(i, j)) --j;
        // Not even the next cell is straight ahead of the caller: start from the cell centre instead.
        if (i == 0 && j == 1 && keepFrom && !clear(0, 1)) keepFrom = false;
        corners.push_back(cells[j]);
        i = j;
    }
    // The last leg ends at `to` only if it can be walked straight from the corner before it.
    if (keepTo && corners.size() > 1) {
        const bool fromStart = corners.size() == 2 && keepFrom;
        const int prev = corners[corners.size() - 2];
        keepTo = Walk(prev, fromStart ? from : CellPosition(prev), to) >= 0;
    }

    for (const int c : corners) out.push_back(CellPosition(c));
    if (keepFrom) out.front() = { from.x, out.front().y, from.z };
    if (keepTo) out.back() = { to.x, out.back().y, to.z };
    return result;
}

void Mesh::DrawDebug(Vector3 around, float radius, const std::vector<Vector3>* path) const {
    const float cs = m_params.cellSize;
    const float lift = 0.06f;
    int drawn = 0;
    for (int i = 0; i < static_cast<int>(m_cells.size()) && drawn < 8000; ++i) {
        if (!m_cells[i].alive) continue;
        const Vector3 p = CellPosition(i);
        if (std::fabs(p.x - around.x) > radius || std::fabs(p.z - around.z) > radius || std::fabs(p.y - around.y) > radius) continue;
        ++drawn;
        bool edge = false;
        for (int d = 0; d < 8; d += 2) if (m_cells[i].link[d] < 0) edge = true;
        const Color color = edge ? Color{ 255, 150, 40, 200 } : Color{ 60, 220, 120, 160 };
        const float h = cs * 0.5f * 0.9f;
        const Vector3 c0 = { p.x - h, p.y + lift, p.z - h }, c1 = { p.x + h, p.y + lift, p.z - h };
        const Vector3 c2 = { p.x + h, p.y + lift, p.z + h }, c3 = { p.x - h, p.y + lift, p.z + h };
        DrawLine3D(c0, c1, color); DrawLine3D(c1, c2, color); DrawLine3D(c2, c3, color); DrawLine3D(c3, c0, color);
    }
    if (path && path->size() > 1) {
        for (size_t k = 0; k + 1 < path->size(); ++k) {
            Vector3 a = (*path)[k], b = (*path)[k + 1];
            a.y += 0.15f; b.y += 0.15f;
            DrawLine3D(a, b, Color{ 80, 200, 255, 255 });
        }
    }
}

Active& World() {
    static Active active;
    return active;
}

void ResetWorld() {
    Active& a = World();
    a.mesh.Clear();
    a.lastPath.clear();
    a.debug = false;
}

void DrawWorldDebug(Vector3 around) {
    const Active& a = World();
    if (!a.debug || !a.mesh.Built()) return;
    a.mesh.DrawDebug(around, 40.0f, &a.lastPath);
}

} // namespace nav
