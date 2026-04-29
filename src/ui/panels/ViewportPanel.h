#pragma once
#include "ui/IPanel.h"
#include "AppState.h"
#include "Camera.h"
#include "NifAnim.h"
#include "Pose.h"
#include "renderer/ISceneRenderer.h"
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <string>
#include <cstdint>

// ── ViewportMode ──────────────────────────────────────────────────────────────
// Determines how the viewport renders and which camera/overlay is active.
// The mode set is injected at construction time so the same class can serve
// different app tabs with different tab bars.
enum class ViewportMode {
    Scene,    // 3D perspective — all actors, free orbit camera
    Face,     // Camera locked to head bone, close-up face rig  (scaffolded)
    Cameras,  // Preview through a scene Camera track           (scaffolded)
    Bones,    // Bone-highlight overlay for animation editing   (scaffolded)
};

struct ViewportTabDef {
    const char*  label;
    ViewportMode mode;
};

// ── ViewportPanel ─────────────────────────────────────────────────────────────
// Pure display component: renders the skeleton(s) from AppState and handles
// viewport camera input.  Owns only camera state and cached render data.
// The tab bar shown is configured at construction time.
class ViewportPanel : public IPanel {
public:
    ViewportPanel(std::vector<ViewportTabDef> tabs, ISceneRenderer& renderer,
                  const char* panelId = "Viewport");
    ~ViewportPanel();

    void        Draw(AppState& state) override;
    const char* PanelID() const override { return panelId_; }

private:
    const char* panelId_;
    // ── Renderer (non-owning, lifetime managed by MainLayout) ────────────────
    ISceneRenderer& renderer_;

    // ── Tab configuration (injected at construction) ─────────────────────────
    std::vector<ViewportTabDef> tabs_;
    int          activeTabIdx_ = 0;
    ViewportMode mode_         = ViewportMode::Scene;

    // ── Camera ────────────────────────────────────────────────────────────────
    Camera  camera_;

    // ── Cell background streaming ─────────────────────────────────────────────
    // Workers produce these on background threads; the main thread drains them
    // and does the GL uploads.  No GL handles — pure data.
    struct PendingShape {
        std::string            baseFormKey;
        MeshData               meshData;
        glm::mat4              toRoot         = glm::mat4(1.f);
        glm::vec3              localMin       = glm::vec3(0.f);
        glm::vec3              localMax       = glm::vec3(0.f);
        std::string            diffusePath;   // normalised lowercase key
        std::vector<uint8_t>   ddsBytes;      // pre-extracted; empty = miss
        DrawSurface::BlendMode blendMode      = DrawSurface::BlendMode::Opaque;
        float                  alphaThreshold = 0.5f;
        // Animation support
        glm::mat4                                     shapeLocal{ 1.f };
        std::vector<std::pair<std::string, glm::mat4>> parentChain;
        NifAnimClip                                   animClip;   // set on first shape per base
        NifShapeMorphAnim                             morphAnim;  // per-shape vertex morph
    };

    // ── Per-actor render cache ────────────────────────────────────────────────

    // Resolved skin binding for one mesh: maps skin-local bone indices to
    // Havok skeleton bone indices plus cached inverse-bind matrices.
    struct MeshSkinBinding {
        bool                   isSkinned = false;
        std::vector<int>       skelBoneIdx;     // skeleton bone index per skin bone (-1 = not found)
        std::vector<glm::mat4> inverseBindMats; // NIF skin space → bone local space
    };

    // Base NIF positions stored for CPU-side morph blending.
    struct MeshMorphBase {
        std::vector<glm::vec3> positions;  // original NIF vertex positions
        int                    vertexCount = 0;
    };

    // ── Cell render cache ─────────────────────────────────────────────────────
    // One GPU shape from a base-object NIF (STAT/MSTT/FURN/etc.).
    struct CellShapeEntry {
        MeshHandle              mesh           = MeshHandle::Invalid;
        TextureHandle           texture        = TextureHandle::Invalid;
        glm::mat4               toRoot         = glm::mat4(1.f);
        glm::vec3               localMin       = glm::vec3(0.f);
        glm::vec3               localMax       = glm::vec3(0.f);
        DrawSurface::BlendMode  blendMode      = DrawSurface::BlendMode::Opaque;
        float                   alphaThreshold = 0.5f;
        // Animation support: shape local transform + ancestor chain for toRoot
        // recomposition when the NIF has controller data.
        glm::mat4                                     shapeLocal{ 1.f };
        std::vector<std::pair<std::string, glm::mat4>> parentChain;
        NifShapeMorphAnim                             morphAnim;
    };

    // All shapes belonging to one unique base object, keyed by baseFormKey.
    struct CellCatalogEntry {
        std::vector<CellShapeEntry> shapes;
        NifAnimClip                 animClip;  // empty for unanimated NIFs
    };

    // One placed reference: catalog lookup key + pre-baked world transform.
    // placement = kNifToWorld * T * R * S  (kNifToWorld already applied).
    // Draw call: DrawMesh(shape.mesh, inst.placement * shape.toRoot, surf)
    struct CellInstance {
        std::string baseFormKey;
        glm::mat4   placement = glm::mat4(1.f);
        int         refIndex  = -1;  // index into loadedCell.refs
    };

    struct ActorRenderData {
        Pose                          refPose;
        Pose                          pose;
        std::vector<MeshHandle>       meshHandles;      // GPU meshes uploaded from nifPath
        std::vector<TextureHandle>    textureHandles;   // diffuse textures, parallel to meshHandles
        std::vector<glm::mat4>        meshTransforms;   // per-mesh toRoot from NifDocument
        std::vector<MeshSkinBinding>  meshSkinBindings; // per-mesh skin binding, parallel to meshHandles
        std::vector<MeshMorphBase>    meshMorphBases;   // base positions for morph blending
        std::map<std::string, float>  morphWeightsCached; // last-applied weights (dirty check)
        std::map<std::string, float>  morphWeightsEval;   // weights from FaceData timeline track
        std::string                   loadedNifPath;    // path these handles came from
    };
    std::vector<ActorRenderData> actorCache_;
    int cachedActorCount_   = -1;  // invalidation sentinel
    int cachedSkeletonCount_ = -1;

    // Cell environment render cache — rebuilt when state.loadedCell.formKey changes.
    std::string                             cellLoadedKey_;
    std::map<std::string, CellCatalogEntry> cellMeshCatalog_; // baseFormKey → shapes
    std::vector<CellInstance>               cellInstances_;
    MeshHandle                              terrainMesh_  = MeshHandle::Invalid;
    std::string                             terrainCellKey_;   // key for terrain mesh invalidation
    // Texture dedup cache: lowercase-normalised diffuse path → GPU handle.
    // Shapes reference handles here but don't own them; FreeCellCache frees this map.
    std::unordered_map<std::string, TextureHandle> cellTexCache_;

    // Streaming thread and its shared state.
    std::thread               cellStream_;
    std::atomic<bool>         cellStreamCancel_{false};
    std::atomic<bool>         cellStreaming_{false};   // true while worker is running
    std::atomic<int>          cellStreamTotal_{0};     // refs submitted to worker
    std::atomic<int>          cellStreamDone_{0};      // refs the worker has finished
    std::mutex                cellStreamMtx_;
    std::vector<PendingShape> cellStreamPending_;      // worker pushes, main drains

    // ── Ambient simulation clock ──────────────────────────────────────────────
    // Free-running; not tied to scene playback state.  Drives NIF controller
    // animations for cell environment objects.
    float simTime_ = 0.f;

    // ── Internals ─────────────────────────────────────────────────────────────
    void RebuildActorCache(AppState& state);
    void SyncNifHandles(AppState& state);   // re-uploads if nifPath changed
    void SyncCellMeshes(AppState& state);   // detect cell change, launch stream
    void SyncTerrainMesh(AppState& state);  // upload terrain mesh for exterior cells
    void FreeCellCache();                    // cancel stream + free all GPU resources
    void SyncMorphs(AppState& state);       // applies ARKit blend-shape weights to face meshes
    void EvaluatePoses(AppState& state);
    void FrameAll();
    // Ray-cast picking: returns index into loadedCell.refs, or -1.
    int  PickCellRef(float ndcX, float ndcY, float aspect) const;
    // Background worker: resolves + parses NIFs, pushes PendingShapes.
    void StreamWorker(std::string dataFolder,
                      std::vector<std::string> bsaSearchList,
                      std::vector<CellPlacedRef> refs);
};
