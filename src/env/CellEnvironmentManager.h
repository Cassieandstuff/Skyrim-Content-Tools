#pragma once
#include "app/AppState.h"
#include "asset/NifAnim.h"
#include "core/render/GpuAssetCache.h"
#include "renderer/ISceneRenderer.h"
#include <atomic>
#include <glm/glm.hpp>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

// ── CellEnvironmentManager ────────────────────────────────────────────────────
// Owns all background streaming, GPU resource management, and draw dispatch for
// the background cell environment (static object NIFs placed via CellContext).
//
// Call sequence each frame (all on the main/GL thread):
//   1. Load()  — if loadedCell changed, cancel old stream and start new one
//   2. Sync()  — drain worker results and upload to GPU
//   3. Draw()  — issue draw calls
//
// StreamWorker and ExteriorNifStreamWorker run on background threads.  They
// never touch GL; all GPU uploads happen in Sync() on the main thread.
//
// BackgroundWorker is not used here because both workers loop over hundreds of
// refs with cooperative cancellation checks between items — a pattern that does
// not fit the one-request → one-result model.  The thread lifetime is instead
// managed explicitly via CancelAndJoin().

class CellEnvironmentManager {
public:
    CellEnvironmentManager()  = default;
    ~CellEnvironmentManager() { CancelAndJoin(); }

    CellEnvironmentManager(const CellEnvironmentManager&)            = delete;
    CellEnvironmentManager& operator=(const CellEnvironmentManager&) = delete;

    // ── Main-thread API ───────────────────────────────────────────────────────

    // Detect whether the cell changed; if so cancel the old stream, free GPU
    // resources, and launch new streaming threads.
    // dataFolder and bsaSearchList are snapshot-copied for the worker threads.
    void Load(const CellContext& cell,
              const std::string& dataFolder,
              const std::vector<std::string>& bsaSearchList,
              ISceneRenderer& renderer);

    // Cancel streams and free all GPU resources.
    void Free(ISceneRenderer& renderer);

    // Drain completed shapes and instances from the worker threads and upload
    // to the GPU. Call once per frame before Draw().
    void Sync(ISceneRenderer& renderer);

    // Issue draw calls for all cell objects.
    // Two-pass: opaque/alphatest first, then blended/additive.
    // simTime drives per-NIF ambient controller animations.
    void Draw(ISceneRenderer& renderer,
              float simTime, float cullDist,
              glm::vec3 cameraTarget, int selectedRefIndex) const;

    // Ray-AABB pick. Returns index into CellContext::refs, or -1.
    // Caller builds the ray from the camera (ViewportPanel) and passes it in.
    int Pick(glm::vec3 rayO, glm::vec3 rayD) const;

    // ── Progress / status accessors ───────────────────────────────────────────
    bool IsStreaming()   const { return cellStreaming_ || exteriorNifStreaming_; }
    int  StreamTotal()   const { return streamTotal_.load(); }
    int  StreamDone()    const { return streamDone_.load(); }
    int  InstanceCount() const { return (int)instances_.size(); }

    // Number of instances within cullDist of cameraTarget (for the viewport label).
    int VisibleCount(glm::vec3 cameraTarget, float cullDist) const;

    // True if Load() would trigger a reload (for coordinating terrain teardown).
    bool NeedsReload(const CellContext& cell) const
    {
        const std::string newKey = cell.loaded ? cell.formKey : std::string{};
        return newKey != loadedKey_;
    }

    // Called by TerrainBulkWorker once the worldspace cell list is sorted and
    // ready.  Thread-safe; wakes ExteriorNifStreamWorker's spin-wait.
    void SetWorldspaceCells(std::vector<std::pair<int,int>> cells);

private:
    // ── Internal types ────────────────────────────────────────────────────────

    // Worker output: geometry + texture bytes for one NIF shape.
    // No GL handles — all GPU uploads happen on the main thread in Sync().
    struct PendingShape {
        std::string                                    baseFormKey;
        MeshData                                       meshData;
        glm::mat4                                      toRoot         = glm::mat4(1.f);
        glm::vec3                                      localMin       = {};
        glm::vec3                                      localMax       = {};
        std::string                                    diffusePath;
        std::vector<uint8_t>                           ddsBytes;
        DrawSurface::BlendMode                         blendMode      = DrawSurface::BlendMode::Opaque;
        float                                          alphaThreshold = 0.5f;
        glm::mat4                                      shapeLocal{ 1.f };
        std::vector<std::pair<std::string, glm::mat4>> parentChain;
        NifAnimClip                                    animClip;   // set only on first shape per base
        NifShapeMorphAnim                              morphAnim;
    };

    // One uploaded GPU shape from a base-object NIF.
    struct CellShapeEntry {
        MeshHandle                                     mesh           = MeshHandle::Invalid;
        TextureHandle                                  texture        = TextureHandle::Invalid;
        glm::mat4                                      toRoot         = glm::mat4(1.f);
        glm::vec3                                      localMin       = {};
        glm::vec3                                      localMax       = {};
        DrawSurface::BlendMode                         blendMode      = DrawSurface::BlendMode::Opaque;
        float                                          alphaThreshold = 0.5f;
        glm::mat4                                      shapeLocal{ 1.f };
        std::vector<std::pair<std::string, glm::mat4>> parentChain;
        NifShapeMorphAnim                              morphAnim;
    };

    // All shapes for one unique base object, keyed by baseFormKey.
    struct CellCatalogEntry {
        std::vector<CellShapeEntry> shapes;
        NifAnimClip                 animClip;
    };

    // One placed reference: catalog lookup key + pre-baked world transform.
    struct CellInstance {
        std::string baseFormKey;
        glm::mat4   placement = glm::mat4(1.f);
        int         refIndex  = -1;
    };

    // Queued instance from ExteriorNifStreamWorker.
    struct PendingInstance {
        std::string baseFormKey;
        glm::mat4   placement;
    };

    // ── GPU-side environment state ─────────────────────────────────────────────
    std::string                                     loadedKey_;
    std::map<std::string, CellCatalogEntry>         meshCatalog_;
    std::vector<CellInstance>                       instances_;
    GpuAssetCache<>                                  texCache_;

    // ── Streaming threads ──────────────────────────────────────────────────────
    std::thread        cellStream_;
    std::thread        exteriorNifStream_;
    std::atomic<bool>  cancel_{false};
    std::atomic<bool>  cellStreaming_{false};
    std::atomic<bool>  exteriorNifStreaming_{false};
    std::atomic<int>   streamTotal_{0};
    std::atomic<int>   streamDone_{0};

    // shape results (worker → main)
    mutable std::mutex       shapeMtx_;
    std::vector<PendingShape> shapePending_;

    // instance placements from ExteriorNifStreamWorker (worker → main)
    mutable std::mutex            instanceMtx_;
    std::vector<PendingInstance>  instancePending_;

    // Cross-worker dedup: both workers claim bases here before loading a NIF.
    mutable std::mutex                  claimedBasesMtx_;
    std::unordered_set<std::string>     claimedBases_;

    // Whole-worldspace sorted cell list written by TerrainBulkWorker
    // and read by ExteriorNifStreamWorker.
    mutable std::mutex             wsAllCellsMtx_;
    std::vector<std::pair<int,int>> wsAllCells_;
    std::atomic<bool>              wsAllCellsReady_{false};

    // ── Private helpers ────────────────────────────────────────────────────────
    void CancelAndJoin();

    void StreamWorker(std::string dataFolder,
                      std::vector<std::string> bsaSearchList,
                      std::vector<CellPlacedRef> refs);

    void ExteriorNifStreamWorker(std::string wsFormKey,
                                  int centerX, int centerY,
                                  std::string dataFolder,
                                  std::vector<std::string> bsaSearchList);
};
