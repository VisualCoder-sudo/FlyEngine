#pragma once
#include "raylib.h"

// Per-object skeletal animation for an imported model.
//
// Imported models are shared between objects (one mesh in memory and on the
// GPU), so a pose can't be written into the shared meshes. Each animated
// object instead owns private copies of the skinned meshes' vertex/normal
// arrays and GPU buffers, and skins them on the CPU from the shared bind-pose
// data. Meshes without bone data stay aliased to the shared ones.
//
// The shared meshes are normalized to [-0.5, 0.5] at load, while skeleton and
// clips stay in the file's original space; `norm` is that normalization (as a
// matrix) so bone matrices can be conjugated into the mesh's space.
class ModelAnimator {
public:
    ModelAnimator() = default;
    ~ModelAnimator() { Release(); }
    ModelAnimator(const ModelAnimator&) = delete;
    ModelAnimator& operator=(const ModelAnimator&) = delete;

    // `clips` is owned by the caller (the shared model entry) and must outlive
    // the animator. Returns false if the model has nothing to skin.
    bool Init(const Model& shared, const ModelAnimation* clips, int clipCount, const Matrix& norm);
    void Release();

    bool Active() const { return meshes_ != nullptr; }
    // Meshes to draw: private skinned copies where skinned, shared otherwise.
    Mesh* Meshes() { return meshes_; }

    int ClipCount() const { return clipCount_; }
    const char* ClipName(int index) const;
    int FindClip(const char* name) const;   // -1 if missing

    // Starts `clip` from its first frame. Out-of-range clip stops playback.
    void Play(int clip, bool loop = true, float speed = 1.0f);
    void Stop();                            // freezes on the current pose
    bool IsPlaying() const { return playing_; }
    int CurrentClip() const { return clip_; }
    float Speed() const { return speed_; }
    bool Looping() const { return loop_; }
    // Seconds into the current clip / its length.
    float Time() const { return time_; }
    float Duration() const;

    // Advances the clock (cheap). The pose is only skinned and uploaded by
    // Skin(), so objects that are culled this frame cost nothing.
    void Advance(float dt);
    // Skins and uploads if the pose changed since the last call.
    void Skin();

private:
    Model shared_{};                        // shared skeleton; `meshes` is replaced by meshes_
    const Mesh* bind_ = nullptr;            // the shared meshes: bind-pose vertices, normals and bone weights
    const ModelAnimation* clips_ = nullptr;
    int clipCount_ = 0;
    Matrix norm_{};
    Matrix normInv_{};

    Mesh* meshes_ = nullptr;
    bool* owned_ = nullptr;                 // per mesh: private copy (true) or aliased (false)
    Matrix* boneMatrices_ = nullptr;        // conjugated into mesh space, for positions
    Matrix* normalMatrices_ = nullptr;      // inverse transpose of the original-space bone matrix, for normals

    int clip_ = -1;
    bool playing_ = false;
    bool loop_ = true;
    float speed_ = 1.0f;
    float time_ = 0.0f;
    bool dirty_ = false;
};
