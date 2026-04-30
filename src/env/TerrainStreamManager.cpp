#include "env/TerrainStreamManager.h"
#include "env/CellEnvironmentManager.h"
#include "app/AppState.h"
#include "plugin/DotNetHost.h"
#include "asset/TerrainMesh.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cfloat>
#include <cstdio>

// ── CancelAndJoin ─────────────────────────────────────────────────────────────

void TerrainStreamManager::CancelAndJoin()
{
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

// ── Free ──────────────────────────────────────────────────────────────────────

void TerrainStreamManager::Free(ISceneRenderer& renderer)
{
    CancelAndJoin();
    cancel_ = false;
    { std::lock_guard<std::mutex> lk(pendingMtx_); pending_.clear(); }

    for (auto& [coord, tile] : tiles_) {
        if (tile.mesh != MeshHandle::Invalid) renderer.FreeMesh(tile.mesh);
        for (int i = 0; i < tile.layerCount - 1; ++i)
            if (tile.blendMaps[i] != TextureHandle::Invalid)
                renderer.FreeTexture(tile.blendMaps[i]);
    }
    tiles_.clear();
    texCache_.Free(renderer);

    wsKey_  = {};
    center_ = { -0x7FFFFFFF, -0x7FFFFFFF };
}

// ── ResolveLayerTex ───────────────────────────────────────────────────────────

TextureHandle TerrainStreamManager::ResolveLayerTex(const std::string& path,
                                                     AppState& state,
                                                     ISceneRenderer& renderer)
{
    if (path.empty()) return TextureHandle::Invalid;
    return texCache_.GetOrLoad(LowerPath(path), [&] {
        std::vector<uint8_t> bytes;
        return state.ResolveAsset(path, bytes)
            ? renderer.LoadTextureFromMemory(bytes)
            : TextureHandle::Invalid;
    });
}

// ── Load ──────────────────────────────────────────────────────────────────────

TerrainStreamManager::LoadResult TerrainStreamManager::Load(
    const CellContext& cell, const LandRecord& landRecord,
    AppState& state, ISceneRenderer& renderer,
    CellEnvironmentManager& env)
{
    const bool        isExt    = cell.loaded && cell.isExterior;
    const std::string newWsKey = isExt ? cell.worldspaceFormKey : std::string{};
    const CellCoord   newCenter{ cell.cellX, cell.cellY };

    const bool wsChanged     = (newWsKey != wsKey_);
    const bool centerChanged = !newWsKey.empty() && (newCenter != center_);

    if (!wsChanged && !centerChanged) return {};

    // Cancel old BulkWorker BEFORE CellEnvironmentManager::Load() is called,
    // so it cannot write stale data to env after env_.Free() clears wsAllCells_.
    CancelAndJoin();
    cancel_ = false;
    { std::lock_guard<std::mutex> lk(pendingMtx_); pending_.clear(); }

    for (auto& [coord, tile] : tiles_) {
        if (tile.mesh != MeshHandle::Invalid) renderer.FreeMesh(tile.mesh);
        for (int i = 0; i < tile.layerCount - 1; ++i)
            if (tile.blendMaps[i] != TextureHandle::Invalid)
                renderer.FreeTexture(tile.blendMaps[i]);
    }
    tiles_.clear();
    texCache_.Free(renderer);

    wsKey_  = newWsKey;
    center_ = newCenter;

    if (newWsKey.empty()) return { true, 0.f, cell.cellX, cell.cellY };

    // ── Ring 0: synchronous upload from state.landRecord ──────────────────────
    {
        TerrainTileEntry r0;
        r0.mesh      = renderer.UploadMesh(GenerateTerrainMesh(landRecord, cell.cellX, cell.cellY));
        r0.hasColors = landRecord.hasColors;

        int lc = 0;
        TextureHandle base = ResolveLayerTex(landRecord.baseTexPath, state, renderer);
        if (base != TextureHandle::Invalid) {
            r0.layers[lc]     = base;
            r0.layerRates[lc] = landRecord.texTileRate;
            ++lc;
        }
        for (const auto& al : landRecord.alphaLayers) {
            if (lc >= TerrainSurface::kMaxLayers) break;
            r0.layers[lc]        = ResolveLayerTex(al.path, state, renderer);
            r0.layerRates[lc]    = al.tileRate;
            r0.blendMaps[lc - 1] = renderer.UploadBlendMap(al.blendMap.data());
            ++lc;
        }
        r0.layerCount    = lc;
        tiles_[newCenter] = r0;
    }

    // Compute average height for camera repositioning (returned to caller).
    float minH =  FLT_MAX, maxH = -FLT_MAX, avgH = 0.f;
    for (int r = 0; r < 33; r++)
        for (int c = 0; c < 33; c++) {
            const float h = landRecord.heights[r][c];
            if (h < minH) minH = h;
            if (h > maxH) maxH = h;
            avgH += h;
        }
    avgH /= (33.f * 33.f);
    fprintf(stderr, "[Terrain] cell (%d,%d): h min=%.0f max=%.0f avg=%.0f  colors=%s\n",
            cell.cellX, cell.cellY, minH, maxH, avgH,
            landRecord.hasColors ? "yes" : "no");

    // ── Background BulkWorker for all remaining cells ─────────────────────────
    worker_ = std::thread(&TerrainStreamManager::BulkWorker, this,
                          newWsKey, cell.cellX, cell.cellY, &env);

    return { true, avgH, cell.cellX, cell.cellY };
}

// ── Sync ──────────────────────────────────────────────────────────────────────

void TerrainStreamManager::Sync(AppState& state, ISceneRenderer& renderer)
{
    std::vector<PendingTile> batch;
    { std::lock_guard<std::mutex> lk(pendingMtx_); batch.swap(pending_); }

    for (auto& pt : batch) {
        const CellCoord cc{ pt.cellX, pt.cellY };

        auto it = tiles_.find(cc);
        if (it != tiles_.end()) {
            // Ring 0 already uploaded without textures — upgrade in place.
            auto& ex = it->second;
            if (ex.layerCount > 0 || pt.baseTexPath.empty()) continue;
            int lc = 0;
            TextureHandle base = ResolveLayerTex(pt.baseTexPath, state, renderer);
            if (base != TextureHandle::Invalid) {
                ex.layers[lc]     = base;
                ex.layerRates[lc] = pt.baseTexRate;
                ++lc;
            }
            for (auto& al : pt.alphaLayers) {
                if (lc >= TerrainSurface::kMaxLayers) break;
                ex.layers[lc]        = ResolveLayerTex(al.path, state, renderer);
                ex.layerRates[lc]    = al.tileRate;
                ex.blendMaps[lc - 1] = renderer.UploadBlendMap(al.blendMap.data());
                ++lc;
            }
            ex.layerCount = lc;
            continue;
        }

        TerrainTileEntry entry;
        entry.mesh      = renderer.UploadMesh(pt.meshData);
        entry.hasColors = pt.hasColors;

        int lc = 0;
        TextureHandle base = ResolveLayerTex(pt.baseTexPath, state, renderer);
        if (base != TextureHandle::Invalid) {
            entry.layers[lc]     = base;
            entry.layerRates[lc] = pt.baseTexRate;
            ++lc;
        }
        for (auto& al : pt.alphaLayers) {
            if (lc >= TerrainSurface::kMaxLayers) break;
            entry.layers[lc]        = ResolveLayerTex(al.path, state, renderer);
            entry.layerRates[lc]    = al.tileRate;
            entry.blendMaps[lc - 1] = renderer.UploadBlendMap(al.blendMap.data());
            ++lc;
        }
        entry.layerCount = lc;
        tiles_[cc]       = entry;
    }
}

// ── Draw ──────────────────────────────────────────────────────────────────────

void TerrainStreamManager::Draw(ISceneRenderer& renderer) const
{
    for (const auto& [coord, tile] : tiles_) {
        if (tile.mesh == MeshHandle::Invalid) continue;
        if (tile.layerCount > 0) {
            TerrainSurface surf;
            surf.layerCount     = tile.layerCount;
            surf.useVertexColor = tile.hasColors;
            for (int i = 0; i < tile.layerCount; ++i) {
                surf.layers[i]     = tile.layers[i];
                surf.layerRates[i] = tile.layerRates[i];
            }
            for (int i = 0; i < tile.layerCount - 1; ++i)
                surf.blendMaps[i] = tile.blendMaps[i];
            renderer.DrawTerrainTile(tile.mesh, surf);
        } else {
            DrawSurface terrSurf;
            terrSurf.useVertexColor = tile.hasColors;
            terrSurf.tint = tile.hasColors
                ? glm::vec4(2.f, 2.f, 2.f, 1.f)
                : glm::vec4(0.35f, 0.45f, 0.30f, 1.f);
            renderer.DrawMesh(tile.mesh, glm::mat4(1.f), terrSurf);
        }
    }
}

// ── BulkWorker ────────────────────────────────────────────────────────────────

void TerrainStreamManager::BulkWorker(std::string wsFormKey, int centerX, int centerY,
                                       CellEnvironmentManager* env)
{
    std::map<std::pair<int,int>, LandRecord> allTiles;
    char err[512] = {};
    if (!DotNetHost::WorldspaceGetTerrainBulk(wsFormKey.c_str(), allTiles, err, sizeof(err))) {
        fprintf(stderr, "[Terrain] bulk fetch failed: %s\n", err);
        fflush(stderr);
        if (env) env->SetWorldspaceCells({});  // unblock ExteriorNifStreamWorker
        return;
    }

    // Sort by Chebyshev distance from center so nearby tiles upload first.
    std::vector<std::pair<int,int>> coords;
    coords.reserve(allTiles.size());
    for (const auto& [coord, rec] : allTiles)
        coords.push_back(coord);
    std::sort(coords.begin(), coords.end(),
        [&](const std::pair<int,int>& a, const std::pair<int,int>& b) {
            const int da = std::max(std::abs(a.first  - centerX),
                                    std::abs(a.second - centerY));
            const int db = std::max(std::abs(b.first  - centerX),
                                    std::abs(b.second - centerY));
            return da < db;
        });

    // Share the full sorted list with ExteriorNifStreamWorker before capping.
    if (env) env->SetWorldspaceCells(coords);

    if (coords.size() > 500)
        coords.resize(500);

    for (const auto& coord : coords) {
        if (cancel_) break;

        const LandRecord& land = allTiles[coord];
        PendingTile pt;
        pt.cellX       = coord.first;
        pt.cellY       = coord.second;
        pt.meshData    = GenerateTerrainMesh(land, coord.first, coord.second);
        pt.hasColors   = land.hasColors;
        pt.baseTexPath = land.baseTexPath;
        pt.baseTexRate = land.texTileRate;
        pt.alphaLayers = land.alphaLayers;

        { std::lock_guard<std::mutex> lk(pendingMtx_);
          pending_.push_back(std::move(pt)); }
    }
}
