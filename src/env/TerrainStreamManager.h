#pragma once
#include "app/AppState.h"
#include "core/render/GpuAssetCache.h"
#include "renderer/ISceneRenderer.h"
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class CellEnvironmentManager;

// ── TerrainStreamManager ──────────────────────────────────────────────────────
// Owns all terrain tile state: ring-0 synchronous upload, background BulkWorker,
// pending tile drain, GPU catalog, and draw dispatch.
//
// Call sequence each frame (main/GL thread):
//   1. Load()  — BEFORE CellEnvironmentManager::Load(); cancels old worker if cell changed
//   2. Sync()  — drain BulkWorker results and upload to GPU
//   3. Draw()  — issue DrawTerrainTile / DrawMesh calls
class TerrainStreamManager {
public:
    TerrainStreamManager()  = default;
    ~TerrainStreamManager() { CancelAndJoin(); }

    TerrainStreamManager(const TerrainStreamManager&)            = delete;
    TerrainStreamManager& operator=(const TerrainStreamManager&) = delete;

    struct LoadResult {
        bool  changed   = false;  // true when worldspace/cell changed this frame
        float avgHeight = 0.f;    // average terrain Z of the new ring-0 cell
        int   cellX     = 0;
        int   cellY     = 0;
    };

    // Must be called BEFORE CellEnvironmentManager::Load().
    // If the worldspace or cell center changed, cancels any running BulkWorker
    // (preventing stale SetWorldspaceCells() writes after env_.Free()), tears down
    // old tiles, uploads ring 0 from landRecord synchronously, and launches a new
    // BulkWorker that calls env.SetWorldspaceCells() once the sorted list is ready.
    // Returns LoadResult with camera-repositioning data when the cell changed.
    LoadResult Load(const CellContext& cell, const LandRecord& landRecord,
                    AppState& state, ISceneRenderer& renderer,
                    CellEnvironmentManager& env);

    // Free all terrain GPU resources and cancel the worker.
    void Free(ISceneRenderer& renderer);

    // Drain completed PendingTiles from the BulkWorker and upload to GPU.
    void Sync(AppState& state, ISceneRenderer& renderer);

    // Issue draw calls for all terrain tiles.
    void Draw(ISceneRenderer& renderer) const;

private:
    using CellCoord = std::pair<int, int>;

    struct TerrainTileEntry {
        MeshHandle    mesh              = MeshHandle::Invalid;
        bool          hasColors         = false;
        int           layerCount        = 0;
        TextureHandle layers[6]         = {};
        float         layerRates[6]     = { 6,6,6,6,6,6 };
        TextureHandle blendMaps[5]      = {};
    };

    struct PendingTile {
        int         cellX, cellY;
        MeshData    meshData;
        bool        hasColors   = false;
        std::string baseTexPath;
        float       baseTexRate = 6.f;
        std::vector<TerrainAlphaLayer> alphaLayers;
    };

    std::map<CellCoord, TerrainTileEntry> tiles_;
    GpuAssetCache<>                       texCache_;
    std::string wsKey_;
    CellCoord   center_ = { -0x7FFFFFFF, -0x7FFFFFFF };

    std::thread       worker_;
    std::atomic<bool> cancel_{ false };
    std::mutex        pendingMtx_;
    std::vector<PendingTile> pending_;

    void          CancelAndJoin();
    TextureHandle ResolveLayerTex(const std::string& path,
                                  AppState& state, ISceneRenderer& renderer);
    void          BulkWorker(std::string wsFormKey, int centerX, int centerY,
                             CellEnvironmentManager* env);
};
