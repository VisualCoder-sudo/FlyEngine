#include "../../../include/Engine/Backend/ModelAnimator.hpp"

#include "raymath.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

// Keyframes are sampled at this rate. raylib's glTF loader resamples every
// clip at 60 fps and its other loaders don't keep the source rate.
constexpr float kClipFps = 60.0f;

bool HasBoneData(const Mesh& m) {
    return m.vertices && m.boneIndices && m.boneWeights && m.vertexCount > 0;
}

Matrix PoseMatrix(const Transform& t) {
    return MatrixMultiply(MatrixMultiply(MatrixScale(t.scale.x, t.scale.y, t.scale.z), QuaternionToMatrix(t.rotation)),
                          MatrixTranslate(t.translation.x, t.translation.y, t.translation.z));
}

// Upper 3x3 only, in the same layout Vector3Transform reads.
Vector3 TransformDir(const Matrix& m, float x, float y, float z) {
    return { m.m0 * x + m.m4 * y + m.m8 * z, m.m1 * x + m.m5 * y + m.m9 * z, m.m2 * x + m.m6 * y + m.m10 * z };
}

} // namespace

bool ModelAnimator::Init(const Model& shared, const ModelAnimation* clips, int clipCount, const Matrix& norm) {
    Release();
    if (!shared.meshes || shared.meshCount <= 0 || shared.skeleton.boneCount <= 0 || !shared.skeleton.bones ||
        !shared.skeleton.bindPose || !clips || clipCount <= 0) return false;
    bool anySkinned = false;
    for (int i = 0; i < shared.meshCount; ++i) anySkinned = anySkinned || HasBoneData(shared.meshes[i]);
    if (!anySkinned) return false;

    shared_ = shared;
    bind_ = shared.meshes;
    clips_ = clips;
    clipCount_ = clipCount;
    norm_ = norm;
    normInv_ = MatrixInvert(norm);

    const int boneCount = shared.skeleton.boneCount;
    boneMatrices_ = (Matrix*)calloc((size_t)boneCount, sizeof(Matrix));
    normalMatrices_ = (Matrix*)calloc((size_t)boneCount, sizeof(Matrix));
    meshes_ = (Mesh*)calloc((size_t)shared.meshCount, sizeof(Mesh));
    owned_ = (bool*)calloc((size_t)shared.meshCount, sizeof(bool));
    for (int b = 0; b < boneCount; ++b) boneMatrices_[b] = normalMatrices_[b] = MatrixIdentity();

    for (int i = 0; i < shared.meshCount; ++i) {
        Mesh m = shared.meshes[i];
        if (HasBoneData(m)) {
            const size_t vBytes = (size_t)m.vertexCount * 3 * sizeof(float);
            float* verts = (float*)malloc(vBytes);
            std::memcpy(verts, m.vertices, vBytes);
            float* normals = nullptr;
            if (m.normals) {
                normals = (float*)malloc(vBytes);
                std::memcpy(normals, m.normals, vBytes);
            }
            m.vertices = verts;
            m.normals = normals;
            m.animVertices = nullptr;
            m.animNormals = nullptr;
            m.vaoId = 0;
            m.vboId = nullptr;
            UploadMesh(&m, true);
            owned_[i] = true;
        }
        meshes_[i] = m;
    }
    shared_.meshes = meshes_;
    return true;
}

void ModelAnimator::Release() {
    if (meshes_) {
        for (int i = 0; i < shared_.meshCount; ++i) {
            if (!owned_[i]) continue;
            // Everything but the private vertex/normal arrays still belongs to
            // the shared mesh; UnloadMesh() frees whatever it is handed.
            Mesh m = meshes_[i];
            m.texcoords = m.texcoords2 = m.tangents = nullptr;
            m.colors = nullptr;
            m.indices = nullptr;
            m.boneIndices = nullptr;
            m.boneWeights = nullptr;
            m.animVertices = m.animNormals = nullptr;
            UnloadMesh(m);
        }
    }
    free(meshes_);
    free(owned_);
    free(boneMatrices_);
    free(normalMatrices_);
    meshes_ = nullptr;
    owned_ = nullptr;
    boneMatrices_ = nullptr;
    normalMatrices_ = nullptr;
    shared_ = Model{};
    bind_ = nullptr;
    clips_ = nullptr;
    clipCount_ = 0;
    clip_ = -1;
    playing_ = false;
    time_ = 0.0f;
    dirty_ = false;
}

const char* ModelAnimator::ClipName(int index) const {
    return (index >= 0 && index < clipCount_) ? clips_[index].name : "";
}

int ModelAnimator::FindClip(const char* name) const {
    if (!name) return -1;
    for (int i = 0; i < clipCount_; ++i) {
        if (std::strncmp(clips_[i].name, name, sizeof(clips_[i].name)) == 0) return i;
    }
    return -1;
}

float ModelAnimator::Duration() const {
    if (clip_ < 0 || clip_ >= clipCount_) return 0.0f;
    return (float)clips_[clip_].keyframeCount / kClipFps;
}

void ModelAnimator::Play(int clip, bool loop, float speed) {
    if (!Active()) return;
    if (clip < 0 || clip >= clipCount_) { Stop(); return; }
    clip_ = clip;
    loop_ = loop;
    speed_ = speed;
    time_ = 0.0f;
    playing_ = true;
    dirty_ = true;
}

void ModelAnimator::Stop() {
    playing_ = false;
}

void ModelAnimator::Advance(float dt) {
    if (!Active() || !playing_ || clip_ < 0) return;
    const float length = Duration();
    time_ += dt * speed_;
    if (length <= 0.0f) { dirty_ = true; return; }
    if (loop_) {
        time_ = std::fmod(time_, length);
        if (time_ < 0.0f) time_ += length;
    } else if (time_ >= length) {
        time_ = length;
        playing_ = false;
    } else if (time_ < 0.0f) {
        time_ = 0.0f;
        playing_ = false;
    }
    dirty_ = true;
}

void ModelAnimator::Skin() {
    if (!Active() || !dirty_ || clip_ < 0 || clip_ >= clipCount_) return;
    dirty_ = false;

    const ModelAnimation& anim = clips_[clip_];
    const ModelSkeleton& skel = shared_.skeleton;
    if (anim.keyframeCount <= 0 || !anim.keyframePoses || anim.boneCount < skel.boneCount) return;

    // Two neighbouring keyframes, blended. A looping clip wraps its last frame
    // to its first; a one-shot clip holds the last frame.
    const float frame = time_ * kClipFps;
    int f0 = (int)frame;
    float blend = std::clamp(frame - (float)f0, 0.0f, 1.0f);
    f0 = std::clamp(f0, 0, anim.keyframeCount - 1);
    int f1 = f0 + 1;
    if (f1 >= anim.keyframeCount) f1 = loop_ ? 0 : anim.keyframeCount - 1;

    for (int b = 0; b < skel.boneCount; ++b) {
        const Transform& a = anim.keyframePoses[f0][b];
        const Transform& c = anim.keyframePoses[f1][b];
        Transform cur;
        cur.translation = Vector3Lerp(a.translation, c.translation, blend);
        cur.rotation = QuaternionSlerp(a.rotation, c.rotation, blend);
        cur.scale = Vector3Lerp(a.scale, c.scale, blend);

        // Bind-relative bone matrix in the file's space...
        const Matrix m = MatrixMultiply(MatrixInvert(PoseMatrix(skel.bindPose[b])), PoseMatrix(cur));
        // ...moved into the normalized mesh space for positions...
        boneMatrices_[b] = MatrixMultiply(MatrixMultiply(normInv_, m), norm_);
        // ...while normals are untouched by normalization, so they use the
        // original matrix (inverse transpose; identity if it is degenerate).
        normalMatrices_[b] = std::fabs(MatrixDeterminant(m)) > 1e-12f ? MatrixTranspose(MatrixInvert(m)) : MatrixIdentity();
    }

    for (int i = 0; i < shared_.meshCount; ++i) {
        if (!owned_[i]) continue;
        const Mesh& src = bind_[i];
        Mesh& dst = meshes_[i];
        const int n = src.vertexCount;
        for (int v = 0; v < n; ++v) {
            const Vector3 p = { src.vertices[v * 3], src.vertices[v * 3 + 1], src.vertices[v * 3 + 2] };
            Vector3 n3 = { 0, 0, 0 };
            if (src.normals) n3 = { src.normals[v * 3], src.normals[v * 3 + 1], src.normals[v * 3 + 2] };
            Vector3 outP = { 0, 0, 0 };
            Vector3 outN = { 0, 0, 0 };
            float used = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const float w = src.boneWeights[v * 4 + k];
                if (w <= 0.0f) continue;
                const int bone = src.boneIndices[v * 4 + k];
                if (bone >= skel.boneCount) continue;
                const Vector3 tp = Vector3Transform(p, boneMatrices_[bone]);
                outP = { outP.x + tp.x * w, outP.y + tp.y * w, outP.z + tp.z * w };
                if (src.normals) {
                    const Vector3 tn = TransformDir(normalMatrices_[bone], n3.x, n3.y, n3.z);
                    outN = { outN.x + tn.x * w, outN.y + tn.y * w, outN.z + tn.z * w };
                }
                used += w;
            }
            if (used <= 0.0f) { outP = p; outN = n3; }   // unweighted vertex stays in bind pose
            else if (src.normals) outN = Vector3Normalize(outN);
            dst.vertices[v * 3] = outP.x;
            dst.vertices[v * 3 + 1] = outP.y;
            dst.vertices[v * 3 + 2] = outP.z;
            if (dst.normals) {
                dst.normals[v * 3] = outN.x;
                dst.normals[v * 3 + 1] = outN.y;
                dst.normals[v * 3 + 2] = outN.z;
            }
        }
        UpdateMeshBuffer(dst, 0, dst.vertices, n * 3 * (int)sizeof(float), 0);
        if (dst.normals) UpdateMeshBuffer(dst, 2, dst.normals, n * 3 * (int)sizeof(float), 0);
    }
}
