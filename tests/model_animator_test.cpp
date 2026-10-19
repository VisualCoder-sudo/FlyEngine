// CPU skinning of ModelAnimator against hand-built data: a two-bone strip
// whose second bone slides along +x. Headless; the GPU upload is a no-op
// without a graphics context, so only the CPU arrays are checked.
#include "Engine/Backend/ModelAnimator.hpp"
#include "raymath.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int g_failures = 0;

static void Expect(float got, float want, const char* label) {
    if (std::fabs(got - want) > 1e-4f) {
        ++g_failures;
        std::printf("FAIL %s: got %g, expected %g\n", label, got, want);
    }
}

static Transform Pose(float x, float y, float z) {
    return { { x, y, z }, QuaternionIdentity(), { 1, 1, 1 } };
}

int main() {
    // Bones in file space: root at the origin, tip one unit up.
    BoneInfo bones[2] = {};
    std::strcpy(bones[0].name, "root"); bones[0].parent = -1;
    std::strcpy(bones[1].name, "tip");  bones[1].parent = 0;
    Transform bind[2] = { Pose(0, 0, 0), Pose(0, 1, 0) };

    // Vertices already in normalized space: file-space (x, y) scaled by 0.5.
    float verts[9] = { 0, 0, 0,   0, 0.5f, 0,   0, 0.25f, 0 };
    float normals[9] = { 0, 0, 1,  0, 0, 1,  0, 0, 1 };
    unsigned char idx[12] = { 0, 0, 0, 0,   1, 0, 0, 0,   0, 1, 0, 0 };
    float weights[12] = { 1, 0, 0, 0,   1, 0, 0, 0,   0.5f, 0.5f, 0, 0 };

    Mesh mesh = {};
    mesh.vertexCount = 3;
    mesh.vertices = verts;
    mesh.normals = normals;
    mesh.boneIndices = idx;
    mesh.boneWeights = weights;

    Model model = {};
    model.meshCount = 1;
    model.meshes = &mesh;
    model.skeleton.boneCount = 2;
    model.skeleton.bones = bones;
    model.skeleton.bindPose = bind;

    // Two keyframes: the tip slides +2 along x by the second one.
    Transform f0[2] = { Pose(0, 0, 0), Pose(0, 1, 0) };
    Transform f1[2] = { Pose(0, 0, 0), Pose(2, 1, 0) };
    Transform* frames[2] = { f0, f1 };
    ModelAnimation clip = {};
    std::strcpy(clip.name, "slide");
    clip.boneCount = 2;
    clip.keyframeCount = 2;
    clip.keyframePoses = frames;

    // File space -> normalized space is a uniform 0.5 scale.
    const Matrix norm = MatrixScale(0.5f, 0.5f, 0.5f);

    ModelAnimator anim;
    if (!anim.Init(model, &clip, 1, norm)) { std::printf("FAIL Init\n"); return 1; }
    if (anim.FindClip("slide") != 0 || anim.FindClip("nope") != -1) { ++g_failures; std::printf("FAIL FindClip\n"); }

    {   // names cut by the 32-byte buffer: a longer request still finds them; a shorter or different one does not
        ModelAnimation clips[3] = {};
        std::memset(clips[0].name, 'a', 31);   // 31 chars + NUL
        std::memset(clips[1].name, 'b', 32);   // 32 chars, no NUL
        std::strcpy(clips[2].name, "abc");
        for (auto& c : clips) { c.boneCount = 2; c.keyframeCount = 2; c.keyframePoses = frames; }
        ModelAnimator names;
        if (!names.Init(model, clips, 3, norm)) { std::printf("FAIL Init names\n"); return 1; }
        const std::string a40(40, 'a'), a31(31, 'a'), a30(30, 'a'), b40(40, 'b'), b32(32, 'b'), b31(31, 'b');
        if (names.FindClip(a40.c_str()) != 0 || names.FindClip(a31.c_str()) != 0 || names.FindClip(a30.c_str()) != -1 ||
            names.FindClip(b40.c_str()) != 1 || names.FindClip(b32.c_str()) != 1 || names.FindClip(b31.c_str()) != -1 ||
            names.FindClip("abc") != 2 || names.FindClip("ab") != -1 || names.FindClip("abcd") != -1)
        { ++g_failures; std::printf("FAIL FindClip long names\n"); }
    }

    // Half a keyframe (1/120 s) in: the tip is 1 unit along x in file space.
    anim.Play(0, true);
    anim.Advance(0.5f / 60.0f);
    anim.Skin();
    const Mesh& out = anim.Meshes()[0];
    Expect(out.vertices[0], 0.0f, "root vertex x");
    Expect(out.vertices[3], 0.5f, "tip vertex x (1 file unit * 0.5)");
    Expect(out.vertices[4], 0.5f, "tip vertex y unchanged");
    Expect(out.vertices[6], 0.25f, "blended vertex x (half the tip move)");
    Expect(out.normals[5], 1.0f, "normal still +z");
    // The shared bind data is untouched.
    Expect(verts[3], 0.0f, "bind vertex x untouched");

    // A one-shot clip holds its last frame and stops.
    anim.Play(0, false);
    anim.Advance(5.0f);
    anim.Skin();
    Expect(out.vertices[3], 1.0f, "tip at the last frame (2 file units * 0.5)");
    if (anim.IsPlaying()) { ++g_failures; std::printf("FAIL one-shot should have stopped\n"); }

    anim.Release();
    std::printf(g_failures ? "model_animator_test: %d FAILED\n" : "model_animator_test: all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
