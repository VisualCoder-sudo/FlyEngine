// Unit tests for TechTools::MemoryTracker.
//
// This links the real FlyEngineCore -- TechnicalTools.cpp is part of that
// library -- so the assertions run against shipping code rather than a copy.
// Nothing here needs a window, a GL context or a running game loop: the tracker
// only touches raylib's pure size helpers (GetPixelDataSize) and /proc, and
// never issues a draw call. Draw() itself is not exercised because it needs an
// active ImGui context.
//
// Usage:  memory_tracker_test [output-dir]
//         output-dir defaults to "."; exports are written there and removed.
//
// Exits 0 when every check passes, 1 otherwise, so ctest reports it directly.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Engine/TechnicalTools.hpp"

using namespace TechTools;

namespace {

int g_failures = 0;
int g_checks = 0;
const char* g_test = "";

// Where the export test writes. ctest runs from the build directory, so this
// defaults to "." and a caller can point it elsewhere.
std::string g_outputDir = ".";

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("  FAIL [%s] line %d: %s\n", g_test, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_checks;                                                          \
        const auto va = (a);                                                 \
        const auto vb = (b);                                                 \
        if (!(va == vb)) {                                                   \
            std::printf("  FAIL [%s] line %d: %s (%lld) != %s (%lld)\n",      \
                        g_test, __LINE__, #a, (long long)va, #b,             \
                        (long long)vb);                                      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// String comparison, kept separate because the numeric formatter in CHECK_EQ
// cannot print one.
#define CHECK_STR(a, b)                                                      \
    do {                                                                     \
        ++g_checks;                                                          \
        const std::string sa = (a);                                          \
        const std::string sb = (b);                                          \
        if (sa != sb) {                                                      \
            std::printf("  FAIL [%s] line %d: %s (\"%s\") != %s (\"%s\")\n",  \
                        g_test, __LINE__, #a, sa.c_str(), #b, sb.c_str());  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// Index into a container that a check has just proven non-empty. Failing the
// bounds here means a size check above already failed, so this only has to
// avoid reading out of range.
template <typename T>
const T& At(const std::vector<T>& v, size_t i) {
    static const T empty{};
    if (i >= v.size()) {
        std::printf("  FAIL [%s] index %zu out of range (size %zu)\n", g_test,
                    i, v.size());
        ++g_failures;
        return empty;
    }
    return v[i];
}

// Per-category high-water mark. Absent means "never allocated in this category",
// which is reported as 0 rather than as a missing key.
size_t CategoryPeak(const MemoryTracker& m, const std::string& category) {
    const auto& peaks = m.GetCategoryPeak();
    auto it = peaks.find(category);
    return it == peaks.end() ? 0u : it->second;
}

// Every test starts from a known-empty tracker so the cases are independent of
// each other's ordering.
void Fresh(MemoryTracker& m) {
    m.ClearTracked();
    m.ResetCounters();
    m.ClearLeakBaseline();
    m.SetTrackingEnabled(true);
    m.SetCaptureCallstacks(false);
}

// The invariants the whole accounting rests on. Checked after every test, so a
// bookkeeping path that forgets a counter is caught by the test that broke it
// rather than by whichever test runs last.
//
// `cumulative` is false for the two operations that are documented to break the
// lifetime identity: ClearTracked() forgets without freeing, and ResetCounters()
// zeroes the totals.
void CheckInvariants(MemoryTracker& m, bool cumulative = true) {
    const size_t live = m.GetCurrentAllocated();
    if (cumulative) {
        const size_t alloc = m.GetTotalAllocated();
        const size_t freed = m.GetTotalFreed();
        if (alloc != freed + live) {
            std::printf("  FAIL [%s]: totalAllocated(%zu) != totalFreed(%zu) + current(%zu)\n",
                        g_test, alloc, freed, live);
            ++g_failures;
        }
    }

    // Three independent views of the same number: the counter, the sum over
    // per-category usage, and the sum over the allocation list. They are
    // maintained separately, so agreement between all three is what catches a
    // path that updates only some of them.
    size_t categorySum = 0;
    for (const auto& entry : m.GetCategoryUsage()) categorySum += entry.second;
    if (categorySum != live) {
        std::printf("  FAIL [%s]: per-category sum(%zu) != currentAllocated(%zu)\n",
                    g_test, categorySum, live);
        ++g_failures;
    }

    const auto& list = m.GetAllocs();
    size_t listSum = 0;
    for (const auto& a : list) listSum += a.size;
    if (listSum != live) {
        std::printf("  FAIL [%s]: allocation-list sum(%zu) != currentAllocated(%zu)\n",
                    g_test, listSum, live);
        ++g_failures;
    }

    if (m.GetAllocationCount() != list.size()) {
        std::printf("  FAIL [%s]: count(%zu) != list size(%zu)\n", g_test,
                    m.GetAllocationCount(), list.size());
        ++g_failures;
    }

    if (live > m.GetPeakAllocated()) {
        std::printf("  FAIL [%s]: peak(%zu) < live(%zu)\n", g_test,
                    m.GetPeakAllocated(), live);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// Accounting
// ---------------------------------------------------------------------------

void Test_BasicAccounting(MemoryTracker& m) {
    CHECK_EQ(m.GetCurrentAllocated(), 0u);
    CHECK_EQ(m.GetAllocationCount(), 0u);
    CHECK_EQ(m.GetTotalAllocated(), 0u);

    m.TrackAlloc((void*)0x1000, 1000, "texture", "wood.png");
    m.TrackAlloc((void*)0x2000, 2000, "mesh", "cube.obj");

    CHECK_EQ(m.GetCurrentAllocated(), 3000u);
    CHECK_EQ(m.GetAllocationCount(), 2u);
    CHECK_EQ(m.GetTotalAllocated(), 3000u);
    CHECK_EQ(m.GetCurrentUsage("texture"), 1000u);
    CHECK_EQ(m.GetCurrentUsage("mesh"), 2000u);
    CHECK_EQ(m.GetPeakAllocated(), 3000u);

    // The category peak is tracked separately from the live usage and must
    // survive the free below.
    CHECK_EQ(CategoryPeak(m, "texture"), 1000u);
    CHECK_EQ(CategoryPeak(m, "never-used"), 0u);
}

void Test_Free(MemoryTracker& m) {
    m.TrackAlloc((void*)0x1000, 1000, "texture", "a");
    m.TrackAlloc((void*)0x2000, 2000, "mesh", "b");

    m.TrackFree((void*)0x1000);
    CHECK_EQ(m.GetCurrentAllocated(), 2000u);
    CHECK_EQ(m.GetAllocationCount(), 1u);
    CHECK_EQ(m.GetTotalFreed(), 1000u);
    CHECK_EQ(m.GetCurrentUsage("texture"), 0u);
    CHECK_EQ(m.GetCurrentUsage("mesh"), 2000u);
    // Releasing memory must not lower the peak.
    CHECK_EQ(m.GetPeakAllocated(), 3000u);
    // ...nor the per-category peak.
    CHECK_EQ(CategoryPeak(m, "texture"), 1000u);

    // Freeing a pointer that was never tracked is a no-op, not a corruption.
    // Teardown paths call it unconditionally, so it has to tolerate junk.
    m.TrackFree((void*)0xdead);
    m.TrackFree(nullptr);
    CHECK_EQ(m.GetCurrentAllocated(), 2000u);
    CHECK_EQ(m.GetTotalFreed(), 1000u);
    CHECK_EQ(m.GetAllocationCount(), 1u);
}

void Test_Realloc(MemoryTracker& m) {
    void* b = (void*)0x2000;
    m.TrackAlloc(b, 2000, "mesh", "b");

    // Same pointer: the entry keeps its identity, category and tag.
    // The lifetime counters move by the *delta*, not by the new size, because
    // that is what the allocator actually did: a growing realloc obtains only
    // the difference. This is also what keeps
    // totalAllocated == totalFreed + currentAllocated true across a resize.
    m.TrackRealloc(b, b, 5000);
    CHECK_EQ(m.GetCurrentAllocated(), 5000u);
    CHECK_EQ(m.GetCurrentUsage("mesh"), 5000u);
    CHECK_EQ(m.GetAllocationCount(), 1u);
    CHECK_EQ(m.GetTotalAllocated(), 2000u + 3000u);
    CHECK_EQ(m.GetTotalFreed(), 0u);

    m.TrackRealloc(b, b, 500);
    CHECK_EQ(m.GetCurrentUsage("mesh"), 500u);
    CHECK_EQ(m.GetCurrentAllocated(), 500u);
    // Shrinking returns only the difference to the allocator.
    CHECK_EQ(m.GetTotalFreed(), 4500u);
    CHECK_EQ(m.GetTotalAllocated(), 5000u);

    // Moving realloc: the old key disappears, the new one inherits the metadata.
    void* a = (void*)0x1000;
    m.TrackAlloc(a, 100, "script", "keep");
    void* c = (void*)0x3000;
    m.TrackRealloc(a, c, 250);
    CHECK_EQ(m.GetCurrentUsage("script"), 250u);
    CHECK_EQ(m.GetAllocationCount(), 2u);
    CHECK_STR(At(m.GetAllocs(), 0).category, "script");
    CHECK_STR(At(m.GetAllocs(), 0).tag, "keep");
    CHECK(At(m.GetAllocs(), 0).ptr == c);

    m.TrackFree(c);
    CHECK_EQ(m.GetCurrentUsage("script"), 0u);

    // realloc to null is a free.
    void* d = (void*)0x4000;
    m.TrackAlloc(d, 77, "audio", "clunk.wav");
    m.TrackRealloc(d, nullptr, 0);
    CHECK_EQ(m.GetCurrentUsage("audio"), 0u);
    CHECK_EQ(m.GetAllocationCount(), 1u);
}

// ---------------------------------------------------------------------------
// raylib resource keys
// ---------------------------------------------------------------------------

void Test_ResourceKeys(MemoryTracker& m) {
    Texture2D tex{};
    tex.id = 7;
    tex.width = 64;
    tex.height = 64;
    tex.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

    const size_t texBytes = TextureBytes(tex);
    CHECK_EQ(texBytes, 64u * 64u * 4u);

    m.TrackTexture(tex, "brick.png");
    CHECK_EQ(m.GetCurrentUsage("texture"), texBytes);

    // Re-tracking the same handle must update in place, not double count. This
    // is the case that a struct-address key would get wrong, because raylib
    // passes textures by value.
    m.TrackTexture(tex, "brick.png");
    CHECK_EQ(m.GetCurrentUsage("texture"), texBytes);
    CHECK_EQ(m.GetAllocationCount(), 1u);

    m.UntrackTexture(tex);
    CHECK_EQ(m.GetCurrentUsage("texture"), 0u);
    CHECK_EQ(m.GetAllocationCount(), 0u);

    // A zeroed handle has no identity. Keying it would merge every unloaded
    // resource in the process into one bogus entry.
    Texture2D empty{};
    m.TrackTexture(empty, "nothing");
    CHECK_EQ(m.GetAllocationCount(), 0u);

    // Distinct handles are distinct entries.
    Texture2D other = tex;
    other.id = 8;
    m.TrackTexture(tex, "a");
    m.TrackTexture(other, "b");
    CHECK_EQ(m.GetAllocationCount(), 2u);
    CHECK_EQ(m.GetCurrentUsage("texture"), texBytes * 2);
}

void Test_SizeHelpers(MemoryTracker& m) {
    // Nothing measurable in a zeroed resource.
    CHECK_EQ(ImageBytes(Image{0, 0, 0, 0, 0}), 0u);
    CHECK_EQ(TextureBytes(Texture2D{}), 0u);
    CHECK_EQ(MeshBytes(Mesh{}), 0u);
    CHECK_EQ(ShaderBytes(Shader{}), 0u);
    CHECK_EQ(ModelBytes(Model{}), 0u);
    CHECK_EQ(RenderTextureBytes(RenderTexture{}), 0u);
    CHECK_EQ(WaveBytes(Wave{}), 0u);

    Image img{};
    img.data = (void*)0xF00D;
    img.width = 32;
    img.height = 32;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    img.mipmaps = 1;
    CHECK_EQ(ImageBytes(img), 32u * 32u * 4u);

    // A mip chain costs base + every halved level, not just the base.
    Image mipped = img;
    mipped.mipmaps = 3;
    CHECK_EQ(ImageBytes(mipped), 32u * 32u * 4u + 16u * 16u * 4u + 8u * 8u * 4u);

    Wave w{};
    w.frameCount = 48000;
    w.sampleRate = 48000;
    w.sampleSize = 16;
    w.channels = 2;
    w.data = (void*)0xABCD;
    CHECK_EQ(WaveBytes(w), 48000u * 2u * 2u);

    // A mesh counts only the arrays that are actually present: raylib leaves
    // skinning and anim pointers NULL for static meshes, and a NULL array is
    // not a vertexCount-sized allocation.
    Mesh mesh{};
    mesh.vertexCount = 10;
    mesh.vertices = (float*)0x10;
    mesh.indices = (unsigned short*)0x20;
    mesh.triangleCount = 3;
    CHECK_EQ(MeshBytes(mesh), 10u * 3u * sizeof(float) + 10u * sizeof(unsigned short));

    const size_t meshBefore = m.GetCurrentUsage("mesh");
    m.TrackMesh(mesh, "tri.mesh");
    CHECK_EQ(m.GetCurrentUsage("mesh"), meshBefore + MeshBytes(mesh));
    m.UntrackMesh(mesh);
    CHECK_EQ(m.GetCurrentUsage("mesh"), meshBefore);

    // A shader is its own state plus a fixed uniform-location table.
    Shader sh{};
    sh.id = 3;
    CHECK_EQ(ShaderBytes(sh), sizeof(Shader) + 32u * sizeof(int));
    m.TrackShader(sh, "lit");
    CHECK_EQ(m.GetCurrentUsage("shader"), ShaderBytes(sh));
    m.UntrackShader(sh);
    CHECK_EQ(m.GetCurrentUsage("shader"), 0u);

    // This raylib's RenderTexture has no fboDepth flag, so depth is counted
    // from its own descriptor when one is attached.
    RenderTexture2D rt{};
    rt.id = 5;
    rt.texture.id = 11;
    rt.texture.width = 128;
    rt.texture.height = 128;
    rt.texture.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    rt.depth.id = 12;
    rt.depth.width = 128;
    rt.depth.height = 128;
    rt.depth.format = PIXELFORMAT_UNCOMPRESSED_R32;
    CHECK_EQ(RenderTextureBytes(rt), 128u * 128u * 4u * 2u);
    m.TrackRenderTexture(rt, "shadow");
    CHECK_EQ(m.GetCurrentUsage("rendertexture"), RenderTextureBytes(rt));
    m.UntrackRenderTexture(rt);
    CHECK_EQ(m.GetCurrentUsage("rendertexture"), 0u);
}

// ---------------------------------------------------------------------------
// Leak detection
// ---------------------------------------------------------------------------

void Test_LeakBaseline(MemoryTracker& m) {
    void* before = (void*)0xAAA0;
    void* after = (void*)0xBBB0;
    m.TrackAlloc(before, 111, "general", "pre-baseline");

    CHECK(!m.HasLeakBaseline());
    m.MarkLeakBaseline();
    CHECK(m.HasLeakBaseline());

    m.TrackAlloc(after, 222, "general", "post-baseline");

    // With a baseline set, only what predates it and is still live is a leak.
    // Anything allocated afterwards is new work, not a leak.
    MemoryTracker::LeakReport r1 = m.DetectLeaks();
    CHECK_EQ(r1.leaks.size(), 1u);
    CHECK_EQ(r1.totalLeakedBytes, 111u);
    CHECK(At(r1.leaks, 0).ptr == before);
    CHECK_EQ(r1.leaksByCategory.at("general"), 111u);
    CHECK_EQ(r1.leaksByTag.at("pre-baseline"), 111u);

    // Freeing the pre-baseline allocation clears the report.
    m.TrackFree(before);
    MemoryTracker::LeakReport r2 = m.DetectLeaks();
    CHECK_EQ(r2.leaks.size(), 0u);
    CHECK_EQ(r2.totalLeakedBytes, 0u);

    // With no baseline, everything live is reported -- the report is never
    // empty just because nobody remembered to set a baseline.
    m.ClearLeakBaseline();
    CHECK(!m.HasLeakBaseline());
    MemoryTracker::LeakReport r3 = m.DetectLeaks();
    CHECK_EQ(r3.leaks.size(), 1u);
    CHECK_EQ(r3.totalLeakedBytes, 222u);
    CHECK(At(r3.leaks, 0).ptr == after);
}

void Test_SnapshotDiff(MemoryTracker& m) {
    void* p1 = (void*)0x10000;
    void* p2 = (void*)0x20000;
    m.TrackAlloc(p1, 1000, "texture", "t1");
    m.TrackAlloc(p2, 2000, "mesh", "m1");
    MemoryTracker::Snapshot s1 = m.TakeSnapshot();
    CHECK_EQ(s1.allocations.size(), 2u);

    m.TrackFree(p2);                 // freed
    m.TrackRealloc(p1, p1, 1500);    // grown
    void* p3 = (void*)0x30000;
    m.TrackAlloc(p3, 500, "image", "i1");  // new

    MemoryTracker::DiffResult d = m.DiffSnapshots(s1, m.TakeSnapshot());
    CHECK_EQ(d.newAllocs.size(), 1u);
    CHECK_EQ(d.freedAllocs.size(), 1u);
    CHECK_EQ(d.grownAllocs.size(), 1u);
    CHECK_EQ(d.shrunkAllocs.size(), 0u);
    // netBytes is the signed change in live total: +500 new, +500 growth, -2000 freed.
    CHECK_EQ(d.netBytes, (int64_t)(500 + 500 - 2000));
    CHECK(At(d.newAllocs, 0).ptr == p3);
    CHECK(At(d.freedAllocs, 0).ptr == p2);
    CHECK(At(d.grownAllocs, 0).ptr == p1);

    // Shrinking is detected too, and reported with a negative delta.
    MemoryTracker::Snapshot s3 = m.TakeSnapshot();
    m.TrackRealloc(p1, p1, 200);
    MemoryTracker::DiffResult d2 = m.DiffSnapshots(s3, m.TakeSnapshot());
    CHECK_EQ(d2.shrunkAllocs.size(), 1u);
    CHECK_EQ(d2.grownAllocs.size(), 0u);
    CHECK_EQ(d2.netBytes, (int64_t)(200 - 1500));

    // A snapshot taken against itself is empty rather than a full diff.
    MemoryTracker::DiffResult d3 = m.DiffSnapshots(s3, s3);
    CHECK_EQ(d3.newAllocs.size(), 0u);
    CHECK_EQ(d3.freedAllocs.size(), 0u);
    CHECK_EQ(d3.grownAllocs.size(), 0u);
    CHECK_EQ(d3.netBytes, (int64_t)0);
}

void Test_Budgets(MemoryTracker& m) {
    m.SetBudget("texture", 4096);
    CHECK_EQ(m.GetBudget("texture"), 4096u);
    CHECK(m.GetBudgetUsagePercent("texture") >= 0.0f);

    m.TrackAlloc((void*)0x9000, 8192, "texture", "huge");
    CHECK(m.GetBudgetUsagePercent("texture") > 100.0f);
    CHECK_EQ(m.GetOverBudgetCategories().size(), 1u);
    CHECK_STR(At(m.GetOverBudgetCategories(), 0), "texture");

    // A category with no budget set is never "over budget".
    m.TrackAlloc((void*)0xA000, 100, "mesh", "small");
    CHECK_EQ(m.GetOverBudgetCategories().size(), 1u);

    m.SetBudget("mesh", 50);
    CHECK_EQ(m.GetOverBudgetCategories().size(), 2u);
    m.SetBudget("mesh", 1000);
    CHECK_EQ(m.GetOverBudgetCategories().size(), 1u);

    // A zero budget means "unlimited", never "zero bytes allowed".
    m.SetBudget("shader", 0);
    CHECK_EQ(m.GetBudget("shader"), 0u);
    CHECK_EQ(m.GetBudgetUsagePercent("shader"), 0.0f);
    CHECK_EQ(m.GetBudgetUsagePercent("no-such-category"), 0.0f);
    CHECK_EQ(m.GetCurrentUsage("no-such-category"), 0u);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void Test_ClearVsReset(MemoryTracker& m) {
    m.TrackAlloc((void*)0x9000, 8192, "texture", "huge");
    const size_t totalBefore = m.GetTotalAllocated();
    CHECK_EQ(totalBefore, 8192u);

    // Clear forgets the live entries but keeps the lifetime counters, so a
    // leak report taken afterwards is measured from this point on. It does not
    // free anything.
    m.ClearTracked();
    CHECK_EQ(m.GetCurrentAllocated(), 0u);
    CHECK_EQ(m.GetAllocationCount(), 0u);
    CHECK_EQ(m.GetTotalAllocated(), totalBefore);
    CheckInvariants(m, /*cumulative=*/false);

    // Reset zeroes the cumulative totals and the peaks, but leaves live bytes
    // alone -- what is allocated now is still allocated now.
    m.TrackAlloc((void*)0x9001, 4096, "texture", "still-here");
    m.ResetCounters();
    CHECK_EQ(m.GetTotalAllocated(), 0u);
    CHECK_EQ(m.GetTotalFreed(), 0u);
    CHECK_EQ(m.GetCurrentAllocated(), 4096u);
    // The overall peak restarts from the current live total, so it never
    // reports less than is actually held.
    CHECK_EQ(m.GetPeakAllocated(), 4096u);
    CheckInvariants(m, /*cumulative=*/false);
}

void Test_TrackingDisabled(MemoryTracker& m) {
    m.SetTrackingEnabled(false);
    CHECK(!m.IsTrackingEnabled());
    m.TrackAlloc((void*)0xB000, 500, "general", "");
    CHECK_EQ(m.GetAllocationCount(), 0u);
    CHECK_EQ(m.GetCurrentAllocated(), 0u);

    // Untracking something that was never tracked is still harmless, so
    // teardown does not have to know whether tracking was on.
    m.TrackFree((void*)0xB000);

    m.SetTrackingEnabled(true);
    m.TrackAlloc((void*)0xB000, 500, "general", "");
    CHECK_EQ(m.GetAllocationCount(), 1u);
    CHECK_EQ(m.GetCurrentAllocated(), 500u);
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Minimal RFC 4180 reader: splits text into records of fields, honouring
// doubled quotes and newlines that fall inside a quoted field. Counting
// newlines would be wrong -- a raw newline inside a quoted field is legal CSV
// and does not start a new record.
std::vector<std::vector<std::string>> ParseCsv(const std::string& text) {
    std::vector<std::vector<std::string>> records;
    std::vector<std::string> fields;
    std::string field;
    bool inQuotes = false;
    bool haveField = false;

    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += c;
            }
        } else if (c == '"') {
            inQuotes = true;
            haveField = true;
        } else if (c == ',') {
            fields.push_back(field);
            field.clear();
            haveField = false;
        } else if (c == '\n') {
            fields.push_back(field);
            field.clear();
            haveField = false;
            records.push_back(fields);
            fields.clear();
        } else if (c != '\r') {
            field += c;
            haveField = true;
        }
    }
    if (haveField || !field.empty() || !fields.empty()) {
        fields.push_back(field);
        records.push_back(fields);
    }
    return records;
}

// Tags are supplied by callers (asset paths, script names) and routinely contain
// characters that need escaping. Both writers have to survive that: a report
// that cannot be parsed back is worse than no report.
void Test_ExportEscaping(MemoryTracker& m) {
    const std::string tag = "quote\"and\\slash\nand\ttab,and-comma";
    m.TrackAlloc((void*)0xC000, 4096, "texture", tag);

    const std::string csvPath = g_outputDir + "/mem_export_test.csv";
    const std::string jsonPath = g_outputDir + "/mem_export_test.json";
    m.ExportCSV(csvPath);
    m.ExportJSON(jsonPath);

    const std::string csv = ReadFile(csvPath);
    const std::string json = ReadFile(jsonPath);
    CHECK(!csv.empty());
    CHECK(!json.empty());

    // The tag must round-trip through CSV byte for byte, which is the only
    // thing that matters: an escaped field that does not read back the same is
    // worse than an unescaped one.
    const auto records = ParseCsv(csv);
    // header + 1 data row + blank + "# summary" + 8 summary lines.
    CHECK_EQ(records.size(), 12u);
    if (records.size() >= 2) {
        CHECK_EQ(records[0].size(), 7u);
        CHECK_STR(At(records[0], 0), "pointer");
        CHECK_STR(At(records[0], 3), "tag");
        // One data row: the embedded newline did not split it.
        CHECK_EQ(records[1].size(), 7u);
        CHECK_STR(At(records[1], 2), "texture");
        CHECK_STR(At(records[1], 3), tag);
        CHECK_STR(At(records[1], 4), "0");
        CHECK_STR(At(records[1], 6), "1");
    }

    // JSON: the dangerous characters become two-character escapes, so no raw
    // control character can survive inside a string.
    CHECK(json.find("\\\"") != std::string::npos);
    CHECK(json.find("\\\\") != std::string::npos);
    CHECK(json.find("\\n") != std::string::npos);
    CHECK(json.find("\\t") != std::string::npos);
    CHECK(json.find("quote\\\"and") != std::string::npos);
    // The tag is a single JSON string, so the escaped newline is the two
    // characters \ and n, never a literal line break inside the value.
    CHECK(json.find("\"tag\": \"quote\\\"and\\\\slash\\nand\\ttab,and-comma\"") !=
          std::string::npos);

    // The summary block agrees with the live counters.
    CHECK(csv.find("current_allocated,4096") != std::string::npos);
    CHECK(csv.find("live_count,1") != std::string::npos);
    CHECK(json.find("\"current\": 4096") != std::string::npos);

    std::remove(csvPath.c_str());
    std::remove(jsonPath.c_str());
}

// ---------------------------------------------------------------------------
// Platform probes
// ---------------------------------------------------------------------------

void Test_ProcessMemory(MemoryTracker&) {
    const size_t rss = MemoryTracker::GetProcessMemoryBytes();
    const size_t peak = MemoryTracker::GetProcessPeakMemoryBytes();
    // Not asserting a value: /proc may be unavailable in a sandbox or a
    // container. What must hold whenever it does report is that the
    // high-water mark is at least the current reading, and that asking twice
    // is safe.
    if (rss > 0) {
        std::printf("  RSS %zu bytes, peak %zu bytes\n", rss, peak);
        CHECK(peak >= rss);
    } else {
        std::printf("  process RSS not reported on this platform; skipped\n");
    }
    (void)MemoryTracker::GetProcessPeakMemoryBytes();
    (void)MemoryTracker::GetProcessMemoryBytes();
}

// Look an allocation up by pointer. GetAllocs() is newest-first, so positional
// indexing would make these assertions depend on insertion order.
//
// MemAlloc is fully qualified because raylib declares a MemAlloc(unsigned)
// function, and `using namespace TechTools` makes the bare name ambiguous.
const TechTools::MemAlloc* FindByPtr(const MemoryTracker& m, void* ptr) {
    for (const auto& a : m.GetAllocs()) {
        if (a.ptr == ptr) return &a;
    }
    return nullptr;
}

void Test_Callstacks(MemoryTracker& m) {
    // The callstack capture is the expensive part of tracking, so the switch
    // that turns it off has to actually skip the work.
    void* quiet = (void*)0xD000;
    m.TrackAlloc(quiet, 64, "general", "no-stack");
    const TechTools::MemAlloc* quietEntry = FindByPtr(m, quiet);
    CHECK(quietEntry != nullptr);
    if (quietEntry) CHECK(quietEntry->callstack.empty());

    void* loud = (void*)0xD008;
    m.SetCaptureCallstacks(true);
    CHECK(m.GetCaptureCallstacks());
    m.TrackAlloc(loud, 64, "general", "with-stack");
    CHECK_EQ(m.GetAllocationCount(), 2u);
    CHECK_EQ(m.GetCurrentAllocated(), 128u);

    const TechTools::MemAlloc* loudEntry = FindByPtr(m, loud);
    CHECK(loudEntry != nullptr);
    if (loudEntry) {
        const std::string& stack = loudEntry->callstack;
        // Whether symbols resolve depends on the platform and on whether the
        // binary was linked with -rdynamic, so only the shape is asserted.
        if (!stack.empty()) {
            std::printf("  captured stack:\n%s\n", stack.c_str());
            // Lines joined by \n, with no trailing newline: the captured block
            // can be printed or concatenated without a stray blank line.
            CHECK(stack.back() != '\n');
            size_t pos = 0;
            while (pos < stack.size()) {
                size_t eol = stack.find('\n', pos);
                if (eol == std::string::npos) eol = stack.size();
                CHECK(eol > pos);  // no blank frames
                pos = eol + 1;
            }
        } else {
            std::printf("  no symbols resolved on this platform; entry still tracked\n");
        }
    }
    m.SetCaptureCallstacks(false);

    m.SetMaxCallstackFrames(4);
    CHECK_EQ(m.GetMaxCallstackFrames(), 4);
    m.SetMaxCallstackFrames(16);
}

// Several systems (TextureManager, the loader threads) track concurrently. The
// mutex is what makes the sums above hold, so exercise it with enough volume
// that a missing or wrong lock shows up as a torn total.
void Test_ConcurrentTracking(MemoryTracker& m) {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&m, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                // Distinct fake addresses per thread so the keys never collide
                // and the expected total is exact.
                void* p = (void*)(uintptr_t)(0x100000 + t * 0x10000 + i * 8);
                m.TrackAlloc(p, 16, "thread", "t" + std::to_string(t));
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK_EQ(m.GetAllocationCount(), (size_t)(kThreads * kPerThread));
    CHECK_EQ(m.GetCurrentAllocated(), (size_t)(kThreads * kPerThread * 16));
    std::printf("  tracked %zu allocations across %d threads\n",
                m.GetAllocationCount(), kThreads);
    CheckInvariants(m);
}

// ---------------------------------------------------------------------------

struct TestCase {
    const char* name;
    void (*fn)(MemoryTracker&);
    // True for the tests that exercise ClearTracked/ResetCounters, which are
    // documented to break the lifetime identity. They still get the structural
    // invariants checked, just not the cumulative one.
    bool breaksCumulativeIdentity = false;
};

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1) g_outputDir = argv[1];

    MemoryTracker& m = MemoryTracker::Instance();
    m.Initialize();

    const TestCase tests[] = {
        {"basic accounting", &Test_BasicAccounting},
        {"free", &Test_Free},
        {"realloc", &Test_Realloc},
        {"raylib resource keys", &Test_ResourceKeys},
        {"size helpers", &Test_SizeHelpers},
        {"leak baseline", &Test_LeakBaseline},
        {"snapshot diff", &Test_SnapshotDiff},
        {"budgets", &Test_Budgets},
        {"clear vs reset", &Test_ClearVsReset, /*breaksCumulativeIdentity=*/true},
        {"tracking disabled", &Test_TrackingDisabled},
        {"export escaping", &Test_ExportEscaping},
        {"process memory", &Test_ProcessMemory},
        {"callstacks", &Test_Callstacks},
        {"concurrent tracking", &Test_ConcurrentTracking},
    };

    for (const TestCase& tc : tests) {
        g_test = tc.name;
        std::printf("== %s ==\n", tc.name);
        const int before = g_failures;

        Fresh(m);
        tc.fn(m);
        CheckInvariants(m, !tc.breaksCumulativeIdentity);

        if (g_failures == before) std::printf("  ok\n");
    }

    m.Shutdown();

    std::printf("\n%d checks, %d failure%s\n", g_checks, g_failures,
                g_failures == 1 ? "" : "s");
    if (g_failures != 0) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
