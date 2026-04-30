#include "env/CellEnvironmentManager.h"
#include "app/AppState.h"
#include "asset/BsaReader.h"
#include "plugin/DotNetHost.h"
#include "asset/NifDocument.h"
#include <nlohmann/json.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <unordered_set>

// ── CancelAndJoin ─────────────────────────────────────────────────────────────

void CellEnvironmentManager::CancelAndJoin()
{
    cancel_ = true;
    if (cellStream_.joinable())        cellStream_.join();
    if (exteriorNifStream_.joinable()) exteriorNifStream_.join();
}

// ── SetWorldspaceCells ────────────────────────────────────────────────────────
// Called by TerrainBulkWorker (ViewportPanel) once the worldspace cell list is
// sorted.  Thread-safe; wakes ExteriorNifStreamWorker's spin-wait.

void CellEnvironmentManager::SetWorldspaceCells(std::vector<std::pair<int,int>> cells)
{
    { std::lock_guard<std::mutex> lk(wsAllCellsMtx_);
      wsAllCells_ = std::move(cells); }
    wsAllCellsReady_ = true;
}

// ── Free ──────────────────────────────────────────────────────────────────────

void CellEnvironmentManager::Free(ISceneRenderer& renderer)
{
    CancelAndJoin();
    cancel_               = false;
    exteriorNifStreaming_ = false;

    { std::lock_guard<std::mutex> lk(shapeMtx_);       shapePending_.clear(); }
    { std::lock_guard<std::mutex> lk(instanceMtx_);    instancePending_.clear(); }
    { std::lock_guard<std::mutex> lk(claimedBasesMtx_); claimedBases_.clear(); }
    { std::lock_guard<std::mutex> lk(wsAllCellsMtx_);  wsAllCells_.clear(); }
    wsAllCellsReady_ = false;

    for (auto& [key, entry] : meshCatalog_)
        for (auto& shape : entry.shapes)
            if (shape.mesh != MeshHandle::Invalid) renderer.FreeMesh(shape.mesh);

    texCache_.Free(renderer);
    meshCatalog_.clear();
    instances_.clear();
    loadedKey_.clear();
    streamTotal_ = 0;
    streamDone_  = 0;
    cellStreaming_ = false;
}

// ── Load ──────────────────────────────────────────────────────────────────────

void CellEnvironmentManager::Load(const CellContext& cell,
                                   const std::string& dataFolder,
                                   const std::vector<std::string>& bsaSearchList,
                                   ISceneRenderer& renderer)
{
    const std::string newKey = cell.loaded ? cell.formKey : std::string{};
    if (newKey == loadedKey_) return;

    Free(renderer);
    loadedKey_ = newKey;

    if (cell.Empty()) return;

    const int numRefs = (int)cell.refs.size();
    fprintf(stderr, "[Cell] Streaming '%s' (%d refs)...\n", cell.name.c_str(), numRefs);

    // Build instance list synchronously — pure TRS math, no I/O.
    // Negate angles: Bethesda CW-positive vs GLM CCW-positive.
    instances_.reserve(numRefs);
    for (int ri = 0; ri < numRefs; ++ri) {
        const auto& ref = cell.refs[ri];
        const glm::mat4 S  = glm::scale    (glm::mat4(1.f), glm::vec3(ref.scale));
        const glm::mat4 Rx = glm::rotate   (glm::mat4(1.f), -ref.rotX, glm::vec3(1,0,0));
        const glm::mat4 Ry = glm::rotate   (glm::mat4(1.f), -ref.rotY, glm::vec3(0,1,0));
        const glm::mat4 Rz = glm::rotate   (glm::mat4(1.f), -ref.rotZ, glm::vec3(0,0,1));
        const glm::mat4 T  = glm::translate(glm::mat4(1.f), glm::vec3(ref.posX, ref.posY, ref.posZ));
        instances_.push_back({ ref.baseFormKey, T * (Rx * Ry * Rz) * S, ri });
    }

    streamTotal_   = numRefs;
    streamDone_    = 0;
    cellStreaming_  = true;

    cellStream_ = std::thread(&CellEnvironmentManager::StreamWorker, this,
                              dataFolder, bsaSearchList, cell.refs);

    if (cell.isExterior) {
        exteriorNifStreaming_ = true;
        exteriorNifStream_ = std::thread(&CellEnvironmentManager::ExteriorNifStreamWorker, this,
                                          cell.worldspaceFormKey,
                                          cell.cellX, cell.cellY,
                                          dataFolder, bsaSearchList);
    }
}

// ── Sync ──────────────────────────────────────────────────────────────────────

void CellEnvironmentManager::Sync(ISceneRenderer& renderer)
{
    // Drain pending instance placements from ExteriorNifStreamWorker.
    {
        std::vector<PendingInstance> instBatch;
        { std::lock_guard<std::mutex> lk(instanceMtx_);
          instBatch.swap(instancePending_); }
        for (auto& pi : instBatch)
            instances_.push_back({ pi.baseFormKey, pi.placement, -1 });
    }

    // Drain pending NIF shapes and upload to GPU.
    std::vector<PendingShape> batch;
    { std::lock_guard<std::mutex> lk(shapeMtx_);
      batch.swap(shapePending_); }

    for (auto& ps : batch) {
        CellShapeEntry se;
        se.toRoot         = ps.toRoot;
        se.localMin       = ps.localMin;
        se.localMax       = ps.localMax;
        se.blendMode      = ps.blendMode;
        se.alphaThreshold = ps.alphaThreshold;
        se.shapeLocal     = ps.shapeLocal;
        se.parentChain    = std::move(ps.parentChain);
        se.morphAnim      = std::move(ps.morphAnim);
        se.mesh           = renderer.UploadMesh(ps.meshData);

        if (!ps.diffusePath.empty())
            // ps.diffusePath is already lowercased by StreamWorker.
            se.texture = texCache_.GetOrLoad(ps.diffusePath, [&] {
                return ps.ddsBytes.empty()
                    ? TextureHandle::Invalid
                    : renderer.LoadTextureFromMemory(ps.ddsBytes);
            });

        CellCatalogEntry& entry = meshCatalog_[ps.baseFormKey];
        // animClip is only set on the first PendingShape per base.
        if (!ps.animClip.empty())
            entry.animClip = std::move(ps.animClip);
        entry.shapes.push_back(std::move(se));
    }
}

// ── Draw ──────────────────────────────────────────────────────────────────────

void CellEnvironmentManager::Draw(ISceneRenderer& renderer,
                                   float simTime, float cullDist,
                                   glm::vec3 cameraTarget, int selectedRefIndex) const
{
    // Recompute a shape's toRoot from its parentChain using animated local
    // transforms where available, falling back to rest transforms.
    auto animatedToRoot = [](const CellShapeEntry& shape,
                             const std::unordered_map<std::string, glm::mat4>& animLocals)
        -> glm::mat4
    {
        glm::mat4 result = shape.shapeLocal;
        for (const auto& [name, restLocal] : shape.parentChain) {
            auto it = animLocals.find(name);
            result = (it != animLocals.end() ? it->second : restLocal) * result;
        }
        return result;
    };

    auto drawCellShape = [&](const CellInstance& inst,
                             const CellShapeEntry& shape,
                             const std::unordered_map<std::string, glm::mat4>& animLocals,
                             bool pass2) {
        if (shape.mesh == MeshHandle::Invalid) return;
        // AlphaTestAndBlend routes to pass 1 (depth write on) like AlphaTest:
        // surviving pixels are fully opaque in practice (DXT1 punch-through),
        // so depth must be written for correct occlusion at distance.
        const bool isBlended = (shape.blendMode == DrawSurface::BlendMode::AlphaBlend ||
                                shape.blendMode == DrawSurface::BlendMode::Additive);
        if (isBlended != pass2) return;

        const glm::mat4 toRoot = animLocals.empty()
            ? shape.toRoot
            : animatedToRoot(shape, animLocals);

        const bool selected = (inst.refIndex == selectedRefIndex);
        DrawSurface surf;
        surf.diffuse        = shape.texture;
        surf.blendMode      = shape.blendMode;
        surf.alphaThreshold = shape.alphaThreshold;
        if (selected) {
            surf.tint = glm::vec4(1.f, 0.65f, 0.1f, 1.f);
        } else {
            surf.tint = (shape.texture != TextureHandle::Invalid)
                ? glm::vec4(1.f, 1.f, 1.f, 1.f)
                : glm::vec4(0.70f, 0.70f, 0.75f, 1.f);
        }
        renderer.DrawMesh(shape.mesh, inst.placement * toRoot, surf);
    };

    // Pre-pass: update GPU position buffers for morph-animated shapes.
    // Done once per catalog entry (not per instance) so shared mesh handles
    // are only written once per frame regardless of how many instances share them.
    {
        std::vector<glm::vec3> morphScratch;
        for (const auto& [key, entry] : meshCatalog_) {
            for (const auto& shape : entry.shapes) {
                if (shape.morphAnim.empty()) continue;
                shape.morphAnim.Evaluate(simTime, morphScratch);
                renderer.UpdateMeshPositions(shape.mesh, morphScratch);
            }
        }
    }

    const float cullDistSq = cullDist * cullDist;

    // Two-pass: opaque/alphatest first, then blended/additive.
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& inst : instances_) {
            const glm::vec3 instPos(inst.placement[3]);
            const glm::vec3 diff = instPos - cameraTarget;
            if (glm::dot(diff, diff) > cullDistSq) continue;

            auto it = meshCatalog_.find(inst.baseFormKey);
            if (it == meshCatalog_.end()) continue;
            const CellCatalogEntry& entry = it->second;

            std::unordered_map<std::string, glm::mat4> animLocals;
            if (!entry.animClip.empty())
                entry.animClip.Evaluate(simTime, animLocals);

            for (const auto& shape : entry.shapes)
                drawCellShape(inst, shape, animLocals, pass == 1);
        }
    }
}

// ── Pick ──────────────────────────────────────────────────────────────────────

int CellEnvironmentManager::Pick(glm::vec3 rayO, glm::vec3 rayD) const
{
    if (instances_.empty()) return -1;

    float bestT   = FLT_MAX;
    int   bestRef = -1;

    for (const auto& inst : instances_) {
        auto it = meshCatalog_.find(inst.baseFormKey);
        if (it == meshCatalog_.end()) continue;

        for (const auto& shape : it->second.shapes) {
            if (shape.mesh == MeshHandle::Invalid) continue;
            if (shape.localMin == shape.localMax) continue;

            // Transform local AABB corners to world space → world AABB.
            const glm::mat4 M = inst.placement * shape.toRoot;
            glm::vec3 wMin( FLT_MAX), wMax(-FLT_MAX);
            for (int cx = 0; cx < 2; ++cx)
            for (int cy = 0; cy < 2; ++cy)
            for (int cz = 0; cz < 2; ++cz) {
                const glm::vec3 c = {
                    cx ? shape.localMax.x : shape.localMin.x,
                    cy ? shape.localMax.y : shape.localMin.y,
                    cz ? shape.localMax.z : shape.localMin.z
                };
                const glm::vec4 w  = M * glm::vec4(c, 1.f);
                const glm::vec3 wv = glm::vec3(w) / w.w;
                wMin = glm::min(wMin, wv);
                wMax = glm::max(wMax, wv);
            }

            // Slab ray-AABB test.
            // tMin starts at -FLT_MAX (unclamped) so camera-inside AABBs
            // produce a negative tMin rather than clamping to 0 and winning.
            float tMin = -FLT_MAX, tMax = FLT_MAX;
            bool  hit  = true;
            for (int a = 0; a < 3; ++a) {
                const float d  = (&rayD.x)[a];
                const float o  = (&rayO.x)[a];
                const float lo = (&wMin.x)[a];
                const float hi = (&wMax.x)[a];
                if (fabsf(d) < 1e-8f) {
                    if (o < lo || o > hi) { hit = false; break; }
                } else {
                    float t1 = (lo - o) / d, t2 = (hi - o) / d;
                    if (t1 > t2) std::swap(t1, t2);
                    tMin = std::max(tMin, t1);
                    tMax = std::min(tMax, t2);
                    if (tMin > tMax) { hit = false; break; }
                }
            }
            // Camera inside AABB: tMin < 0, tMax > 0.  Use tMax as the sort
            // depth so large surrounding objects lose to foreground hits.
            const float hitT = (tMin >= 0.f) ? tMin : tMax;
            if (hit && hitT >= 0.f && hitT < bestT) {
                bestT   = hitT;
                bestRef = inst.refIndex;
            }
        }
    }
    return bestRef;
}

// ── VisibleCount ──────────────────────────────────────────────────────────────

int CellEnvironmentManager::VisibleCount(glm::vec3 cameraTarget, float cullDist) const
{
    const float cullDistSq = cullDist * cullDist;
    int count = 0;
    for (const auto& inst : instances_) {
        const glm::vec3 diff = glm::vec3(inst.placement[3]) - cameraTarget;
        if (glm::dot(diff, diff) <= cullDistSq) ++count;
    }
    return count;
}

// ── StreamWorker ──────────────────────────────────────────────────────────────

void CellEnvironmentManager::StreamWorker(std::string dataFolder,
                                           std::vector<std::string> bsaSearchList,
                                           std::vector<CellPlacedRef> refs)
{
    // Pre-open all BSAs once so every resolve call reuses the cached index.
    std::vector<std::unique_ptr<BsaReader>> openBsas;
    openBsas.reserve(bsaSearchList.size());
    for (const auto& bsaPath : bsaSearchList) {
        auto bsa = std::make_unique<BsaReader>();
        char err[256] = {};
        if (bsa->Open(bsaPath, err, sizeof(err)))
            openBsas.push_back(std::move(bsa));
    }

    auto resolve = [&](const std::string& relPath,
                       std::vector<uint8_t>& outBytes) -> bool
    {
        if (relPath.empty() || cancel_) return false;
        std::string rel = relPath;
        for (char& c : rel) if (c == '\\') c = '/';

        if (!dataFolder.empty()) {
            std::ifstream f(dataFolder + "/" + rel, std::ios::binary);
            if (f) { outBytes.assign(std::istreambuf_iterator<char>(f), {}); return true; }
        }
        std::string bsaKey = rel;
        for (char& c : bsaKey) if (c == '/') c = '\\';
        for (char& c : bsaKey) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);

        for (const auto& bsa : openBsas) {
            if (cancel_) return false;
            const BsaFileInfo* fi = bsa->FindExact(bsaKey);
            if (!fi) continue;
            char xErr[256] = {};
            if (bsa->Extract(*fi, outBytes, xErr, sizeof(xErr))) return true;
        }
        return false;
    };

    std::unordered_set<std::string> seenBases;

    for (int ri = 0; ri < (int)refs.size(); ++ri) {
        if (cancel_) break;

        const CellPlacedRef& ref = refs[ri];
        ++streamDone_;

        if (seenBases.count(ref.baseFormKey)) continue;

        // Cross-worker claim: skip if ExteriorNifStreamWorker already took this base.
        {
            std::lock_guard<std::mutex> lk(claimedBasesMtx_);
            if (!claimedBases_.insert(ref.baseFormKey).second) {
                seenBases.insert(ref.baseFormKey);
                continue;
            }
        }

        std::vector<uint8_t> nifBytes;
        const bool isAbsolute = (ref.nifPath.size() >= 2 && ref.nifPath[1] == ':') ||
                                (!ref.nifPath.empty() &&
                                 (ref.nifPath[0] == '/' || ref.nifPath[0] == '\\'));
        if (isAbsolute) {
            std::ifstream f(ref.nifPath, std::ios::binary);
            if (f) nifBytes.assign(std::istreambuf_iterator<char>(f), {});
        } else {
            resolve(ref.nifPath, nifBytes);
        }
        // Don't mark seen on NIF miss — a later ref with the same baseFormKey might
        // carry a different nifPath that resolves.  Mark seen only after success.
        if (nifBytes.empty() || cancel_) continue;
        seenBases.insert(ref.baseFormKey);

        NifDocument doc = LoadNifDocumentFromBytes(nifBytes, ref.nifPath);
        if (cancel_) break;

        std::vector<PendingShape> batch;
        for (int si = 0; si < (int)doc.shapes.size(); ++si) {
            if (cancel_) break;
            const NifDocShape& ds    = doc.shapes[si];
            const NifBlock&    block = doc.blocks[ds.blockIndex];
            if (ds.meshData.positions.empty()) continue;

            PendingShape ps;
            ps.baseFormKey    = ref.baseFormKey;
            ps.meshData       = ds.meshData;
            ps.toRoot         = block.toRoot;
            ps.alphaThreshold = ds.alphaThreshold;
            ps.shapeLocal     = ds.shapeLocal;
            ps.parentChain    = ds.parentChain;
            ps.morphAnim      = std::move(ds.morphAnim);
            switch (ds.alphaMode) {
                case NifAlphaMode::AlphaTest:         ps.blendMode = DrawSurface::BlendMode::AlphaTest;         break;
                case NifAlphaMode::AlphaBlend:        ps.blendMode = DrawSurface::BlendMode::AlphaBlend;        break;
                case NifAlphaMode::Additive:          ps.blendMode = DrawSurface::BlendMode::Additive;          break;
                case NifAlphaMode::AlphaTestAndBlend: ps.blendMode = DrawSurface::BlendMode::AlphaTestAndBlend; break;
                default:                              ps.blendMode = DrawSurface::BlendMode::Opaque;            break;
            }
            glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
            for (const auto& p : ds.meshData.positions) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
            ps.localMin = lo;
            ps.localMax = hi;

            if (!ds.diffusePath.empty()) {
                ps.diffusePath = ds.diffusePath;
                for (char& c : ps.diffusePath)
                    c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
                resolve(ds.diffusePath, ps.ddsBytes);
            }
            batch.push_back(std::move(ps));
        }

        if (!batch.empty() && !cancel_) {
            // Store animClip in the first shape only — Sync() drains it into the catalog.
            batch[0].animClip = std::move(doc.animClip);
            std::lock_guard<std::mutex> lk(shapeMtx_);
            for (auto& ps : batch)
                shapePending_.push_back(std::move(ps));
        }
    }

    cellStreaming_ = false;
    fprintf(stderr, "[Cell] Stream finished: %d unique bases\n", (int)seenBases.size());
}

// ── ExteriorNifStreamWorker ───────────────────────────────────────────────────

void CellEnvironmentManager::ExteriorNifStreamWorker(
    std::string wsFormKey, int centerX, int centerY,
    std::string dataFolder, std::vector<std::string> bsaSearchList)
{
    using json = nlohmann::json;

    std::vector<std::unique_ptr<BsaReader>> openBsas;
    openBsas.reserve(bsaSearchList.size());
    for (const auto& bsaPath : bsaSearchList) {
        auto bsa = std::make_unique<BsaReader>();
        char bsaErr[256] = {};
        if (bsa->Open(bsaPath, bsaErr, sizeof(bsaErr)))
            openBsas.push_back(std::move(bsa));
    }

    auto resolve = [&](const std::string& relPath, std::vector<uint8_t>& outBytes) -> bool {
        if (relPath.empty() || cancel_) return false;
        std::string rel = relPath;
        for (char& c : rel) if (c == '\\') c = '/';
        {
            std::ifstream f(dataFolder + "/" + rel, std::ios::binary);
            if (f) { outBytes.assign(std::istreambuf_iterator<char>(f), {}); return true; }
        }
        std::string bsaKey = rel;
        for (char& c : bsaKey) if (c == '/') c = '\\';
        for (char& c : bsaKey) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        for (const auto& bsa : openBsas) {
            if (cancel_) return false;
            const BsaFileInfo* fi = bsa->FindExact(bsaKey);
            if (!fi) continue;
            char xErr[256] = {};
            if (bsa->Extract(*fi, outBytes, xErr, sizeof(xErr))) return true;
        }
        return false;
    };

    // Hoist declarations before the first goto so we never jump over ctor calls.
    std::vector<std::pair<int,int>> cells;
    std::unordered_set<std::string> seenBases;
    int totalBases = 0;

    // Wait for TerrainBulkWorker to populate the full worldspace cell list.
    while (!wsAllCellsReady_ && !cancel_)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (cancel_) goto done;

    // Copy the shared list and remove the center cell (handled by StreamWorker).
    {
        std::lock_guard<std::mutex> lk(wsAllCellsMtx_);
        cells = wsAllCells_;
    }
    cells.erase(std::remove_if(cells.begin(), cells.end(),
        [&](const std::pair<int,int>& c) {
            return c.first == centerX && c.second == centerY;
        }), cells.end());

    for (const auto& [cellX, cellY] : cells) {
        if (cancel_) break;

        std::string refsJson;
        char err[256] = {};
        if (!DotNetHost::ExteriorCellGetRefs(wsFormKey.c_str(), cellX, cellY,
                                              refsJson, err, sizeof(err)))
            continue;

        std::vector<CellPlacedRef> refs;
        try {
            for (const auto& j : json::parse(refsJson)) {
                CellPlacedRef ref;
                ref.refFormKey  = j.value("refFormKey",  std::string{});
                ref.baseFormKey = j.value("baseFormKey", std::string{});
                ref.nifPath     = j.value("nifPath",     std::string{});
                ref.posX  = j.value("posX",  0.f); ref.posY  = j.value("posY",  0.f);
                ref.posZ  = j.value("posZ",  0.f); ref.rotX  = j.value("rotX",  0.f);
                ref.rotY  = j.value("rotY",  0.f); ref.rotZ  = j.value("rotZ",  0.f);
                ref.scale = j.value("scale", 1.f);
                if (!ref.nifPath.empty()) refs.push_back(std::move(ref));
            }
        } catch (...) { continue; }

        for (const auto& ref : refs) {
            if (cancel_) goto done;

            // Queue this placement — instance is visible once catalog entry has shapes.
            {
                const glm::mat4 S  = glm::scale    (glm::mat4(1.f), glm::vec3(ref.scale));
                const glm::mat4 Rx = glm::rotate   (glm::mat4(1.f), -ref.rotX, glm::vec3(1,0,0));
                const glm::mat4 Ry = glm::rotate   (glm::mat4(1.f), -ref.rotY, glm::vec3(0,1,0));
                const glm::mat4 Rz = glm::rotate   (glm::mat4(1.f), -ref.rotZ, glm::vec3(0,0,1));
                const glm::mat4 T  = glm::translate(glm::mat4(1.f), glm::vec3(ref.posX, ref.posY, ref.posZ));
                PendingInstance pi{ ref.baseFormKey, T * (Rx * Ry * Rz) * S };
                std::lock_guard<std::mutex> lk(instanceMtx_);
                instancePending_.push_back(std::move(pi));
            }

            // Only load the NIF once per unique base.
            if (seenBases.count(ref.baseFormKey)) continue;
            {
                std::lock_guard<std::mutex> lk(claimedBasesMtx_);
                if (!claimedBases_.insert(ref.baseFormKey).second) {
                    seenBases.insert(ref.baseFormKey);
                    continue;
                }
            }
            seenBases.insert(ref.baseFormKey);

            std::vector<uint8_t> nifBytes;
            if (!resolve(ref.nifPath, nifBytes) || cancel_) continue;

            NifDocument doc = LoadNifDocumentFromBytes(nifBytes, ref.nifPath);
            if (cancel_) goto done;

            std::vector<PendingShape> batch;
            for (int si = 0; si < (int)doc.shapes.size(); ++si) {
                if (cancel_) break;
                const NifDocShape& ds = doc.shapes[si];
                if (ds.meshData.positions.empty()) continue;

                PendingShape ps;
                ps.baseFormKey    = ref.baseFormKey;
                ps.meshData       = ds.meshData;
                ps.toRoot         = doc.blocks[ds.blockIndex].toRoot;
                ps.alphaThreshold = ds.alphaThreshold;
                ps.shapeLocal     = ds.shapeLocal;
                ps.parentChain    = ds.parentChain;
                ps.morphAnim      = std::move(ds.morphAnim);
                switch (ds.alphaMode) {
                    case NifAlphaMode::AlphaTest:         ps.blendMode = DrawSurface::BlendMode::AlphaTest;         break;
                    case NifAlphaMode::AlphaBlend:        ps.blendMode = DrawSurface::BlendMode::AlphaBlend;        break;
                    case NifAlphaMode::Additive:          ps.blendMode = DrawSurface::BlendMode::Additive;          break;
                    case NifAlphaMode::AlphaTestAndBlend: ps.blendMode = DrawSurface::BlendMode::AlphaTestAndBlend; break;
                    default:                              ps.blendMode = DrawSurface::BlendMode::Opaque;            break;
                }
                glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
                for (const auto& p : ds.meshData.positions) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
                ps.localMin = lo; ps.localMax = hi;

                if (!ds.diffusePath.empty()) {
                    ps.diffusePath = ds.diffusePath;
                    for (char& c : ps.diffusePath)
                        c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
                    resolve(ds.diffusePath, ps.ddsBytes);
                }
                batch.push_back(std::move(ps));
            }
            if (!batch.empty() && !cancel_) {
                batch[0].animClip = std::move(doc.animClip);
                std::lock_guard<std::mutex> lk(shapeMtx_);
                for (auto& ps : batch)
                    shapePending_.push_back(std::move(ps));
                ++totalBases;
            }
        }
    }
done:
    exteriorNifStreaming_ = false;
    fprintf(stderr, "[ExteriorNif] Stream finished: %d unique bases across %d cells\n",
            totalBases, (int)cells.size());
}
