#pragma once
#include "AppState.h"
#include "Pose.h"
#include "renderer/ISceneRenderer.h"
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

// ── ActorRenderCache ──────────────────────────────────────────────────────────
// Owns the per-actor GPU resource lifecycle: skin binding resolution, morph
// blending, pose evaluation, mesh/texture upload, and draw dispatch.
//
// Call sequence each frame (main/GL thread, inside BeginFrame/EndFrame):
//   NeedsRebuild() / Rebuild()   — if actor/skeleton list changed
//   SyncNifHandles()             — re-upload if any NIF path changed
//   EvaluatePoses()              — compute world-space bone transforms
//   SyncMorphs()                 — CPU blend shapes → UpdateMeshPositions
//   Draw()                       — issue DrawMesh / DrawSkinnedMesh / DrawSkeleton
class ActorRenderCache {
public:
    ActorRenderCache()  = default;
    ~ActorRenderCache() = default;

    ActorRenderCache(const ActorRenderCache&)            = delete;
    ActorRenderCache& operator=(const ActorRenderCache&) = delete;

    // True when the actor or skeleton list changed since the last Rebuild.
    bool NeedsRebuild(const AppState& state) const;

    // Free all GPU resources, resize cache_ to match state.actors, and compute
    // reference poses. Caller is responsible for calling FrameAll() afterward.
    void Rebuild(AppState& state, ISceneRenderer& renderer);

    // Re-upload NIF geometry for any actor whose NIF path changed.
    void SyncNifHandles(AppState& state, ISceneRenderer& renderer);

    // Apply ARKit blend-shape weights from the active FaceData track or
    // ActorDocument.morphWeights to the head mesh, when weights changed.
    void SyncMorphs(AppState& state, ISceneRenderer& renderer);

    // Evaluate all actor poses from the sequence, selected clip, or reference pose.
    void EvaluatePoses(AppState& state);

    // Issue draw calls for all actors.
    void Draw(const AppState& state, ISceneRenderer& renderer) const;

    // Free all GPU resources. Safe to call multiple times.
    void Free(ISceneRenderer& renderer);

    // Compute the axis-aligned bounding box of all reference-pose bone positions.
    // Returns false if there are no actors or all reference poses are empty.
    bool RefPoseBounds(glm::vec3& outMin, glm::vec3& outMax) const;

    int Count() const { return (int)cache_.size(); }

private:
    // Resolved skin binding for one mesh: maps skin-local bone indices to
    // Havok skeleton bone indices plus cached inverse-bind matrices.
    struct MeshSkinBinding {
        bool                   isSkinned = false;
        std::vector<int>       skelBoneIdx;      // skeleton bone index per skin bone (-1 = not found)
        std::vector<glm::mat4> inverseBindMats;  // NIF skin space → bone local space
    };

    // Base NIF positions stored for CPU-side morph blending.
    struct MeshMorphBase {
        std::vector<glm::vec3> positions;
        int                    vertexCount = 0;
    };

    struct ActorRenderData {
        Pose                          refPose;
        Pose                          pose;
        std::vector<MeshHandle>       meshHandles;
        std::vector<TextureHandle>    textureHandles;
        std::vector<glm::mat4>        meshTransforms;
        std::vector<MeshSkinBinding>  meshSkinBindings;
        std::vector<MeshMorphBase>    meshMorphBases;
        std::map<std::string, float>  morphWeightsCached;
        std::map<std::string, float>  morphWeightsEval;
        std::string                   loadedNifPath;
    };

    std::vector<ActorRenderData> cache_;
    int cachedActorCount_    = -1;
    int cachedSkeletonCount_ = -1;
};
