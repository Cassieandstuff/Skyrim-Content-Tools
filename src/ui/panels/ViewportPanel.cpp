#include "ViewportPanel.h"
#include "AppState.h"
#include "BsaReader.h"
#include "NifDocument.h"
#include "Sequence.h"
#include "HavokSkeleton.h"
#include "TerrainMesh.h"
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <unordered_set>

// SCT world space is Skyrim/Havok Z-up (X=east, Y=north, Z=up).
// No coordinate conversion is needed between NIF/Havok data and world space —
// they are the same coordinate system.  The camera is configured with Z as up.

// ── Construction ──────────────────────────────────────────────────────────────

ViewportPanel::ViewportPanel(std::vector<ViewportTabDef> tabs, ISceneRenderer& renderer,
                             const char* panelId)
    : renderer_(renderer)
    , tabs_(std::move(tabs))
    , panelId_(panelId)
{
    if (!tabs_.empty()) mode_ = tabs_[0].mode;
}

ViewportPanel::~ViewportPanel()
{
    // Stop any background stream before releasing GPU resources.
    cellStreamCancel_ = true;
    if (cellStream_.joinable()) cellStream_.join();

    for (auto& ar : actorCache_) {
        for (MeshHandle    h : ar.meshHandles)    renderer_.FreeMesh(h);
        for (TextureHandle t : ar.textureHandles) renderer_.FreeTexture(t);
    }
    FreeCellCache();
}

// ── Ray-AABB picking ──────────────────────────────────────────────────────────

int ViewportPanel::PickCellRef(float ndcX, float ndcY, float aspect) const
{
    if (cellInstances_.empty()) return -1;

    // Build ray analytically from camera parameters — avoids inverting VP,
    // which is numerically unstable with large Skyrim-scale coordinates and
    // the extreme far/near ratio (5,000,000 / 0.1) used by the camera.
    //
    // View-space ray direction for NDC pixel (ndcX, ndcY):
    //   x_v = ndcX / proj[0][0]   (proj[0][0] = f/aspect)
    //   y_v = ndcY / proj[1][1]   (proj[1][1] = f)
    //   z_v = -1                  (OpenGL: -Z into screen)
    // Transform to world: right * x_v + up * y_v + forward.
    const glm::mat4 proj  = camera_.Proj(aspect);
    const glm::vec3 eye   = camera_.Eye();
    const glm::vec3 fwd   = glm::normalize(camera_.target - eye);
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.f, 0.f, 1.f)));
    const glm::vec3 up    = glm::cross(right, fwd);
    const float     xv    = ndcX / proj[0][0];
    const float     yv    = ndcY / proj[1][1];
    const glm::vec3 rayO  = eye;
    const glm::vec3 rayD  = glm::normalize(right * xv + up * yv + fwd);

    float bestT  = FLT_MAX;
    int   bestRef = -1;

    for (const auto& inst : cellInstances_) {
        auto it = cellMeshCatalog_.find(inst.baseFormKey);
        if (it == cellMeshCatalog_.end()) continue;

        for (const auto& shape : it->second.shapes) {
            if (shape.mesh == MeshHandle::Invalid) continue;
            if (shape.localMin == shape.localMax) continue;  // degenerate

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
                const glm::vec4 w = M * glm::vec4(c, 1.f);
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
            // depth so large surrounding objects (FX glows, fill lights) lose
            // to foreground objects the ray hits from outside.
            const float hitT = (tMin >= 0.f) ? tMin : tMax;
            if (hit && hitT >= 0.f && hitT < bestT) {
                bestT   = hitT;
                bestRef = inst.refIndex;
            }
        }
    }
    return bestRef;
}

// ── IPanel::Draw ──────────────────────────────────────────────────────────────

void ViewportPanel::Draw(AppState& state)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin(PanelID());
    ImGui::PopStyleVar();
    if (!open) { ImGui::End(); return; }

    // ── Toolbar ───────────────────────────────────────────────────────────────
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);

    if (!state.actors.empty()) {
        ImGui::TextDisabled("%d actor(s)  |  %d bones",
                            (int)state.actors.size(),
                            state.skeletons.empty() ? 0 : (int)state.skeletons[0].bones.size());
    } else {
        ImGui::TextDisabled("No actors — add one from the Workflow tab");
    }

    // ── Viewport tab bar ──────────────────────────────────────────────────────
    if (tabs_.size() > 1) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 3.f));
        if (ImGui::BeginTabBar("##vp_tabs", ImGuiTabBarFlags_None)) {
            for (int i = 0; i < (int)tabs_.size(); i++) {
                const bool active = (i == activeTabIdx_);
                ImGuiTabItemFlags flags = active ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(tabs_[i].label, nullptr, flags)) {
                    activeTabIdx_ = i;
                    mode_         = tabs_[i].mode;
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::PopStyleVar();
    }

    ImGui::Separator();

    // ── 3D viewport area ──────────────────────────────────────────────────────
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 1.f || size.y < 1.f) { ImGui::End(); return; }

    // Update actor cache if actors/skeletons changed.
    const int ac = (int)state.actors.size();
    const int sc = (int)state.skeletons.size();
    if (ac != cachedActorCount_ || sc != cachedSkeletonCount_)
        RebuildActorCache(state);

    // Re-upload NIF geometry if any actor's nifPath changed.
    SyncNifHandles(state);

    // Rebuild cell GPU cache if the loaded cell changed.
    SyncCellMeshes(state);

    // Upload terrain mesh for exterior cells (keyed separately from cell stream).
    SyncTerrainMesh(state);

    // Evaluate all actor poses for this frame (also fills morphWeightsEval from FaceData track).
    EvaluatePoses(state);

    // Apply ARKit blend-shape weights to face mesh positions when weights change.
    SyncMorphs(state);

    // Render scene to FBO.
    const int iw = (int)size.x;
    const int ih = (int)size.y;
    const glm::mat4 proj = camera_.Proj(size.x / size.y);
    const glm::mat4 view = camera_.View();

    // ── Drain streamed assets from worker thread — GL uploads on main thread ──
    // Check the atomic flag first to skip the lock when there's nothing to do.
    if (cellStreaming_ || cellStreamDone_ > 0) {
        std::vector<PendingShape> batch;
        { std::lock_guard<std::mutex> lk(cellStreamMtx_);
          batch.swap(cellStreamPending_); }

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
            se.mesh           = renderer_.UploadMesh(ps.meshData);

            if (!ps.diffusePath.empty()) {
                auto it = cellTexCache_.find(ps.diffusePath);
                if (it != cellTexCache_.end()) {
                    se.texture = it->second;  // reuse already-uploaded texture
                } else {
                    TextureHandle th = TextureHandle::Invalid;
                    if (!ps.ddsBytes.empty())
                        th = renderer_.LoadTextureFromMemory(ps.ddsBytes);
                    cellTexCache_[ps.diffusePath] = th;
                    se.texture = th;
                }
            }

            CellCatalogEntry& entry = cellMeshCatalog_[ps.baseFormKey];
            // animClip is only set on the first PendingShape per base.
            if (!ps.animClip.empty())
                entry.animClip = std::move(ps.animClip);
            entry.shapes.push_back(std::move(se));
        }
    }

    // Advance free-running ambient simulation clock (independent of scene time).
    simTime_ += ImGui::GetIO().DeltaTime;

    renderer_.BeginFrame(iw, ih);
    renderer_.SetCamera(view, proj);

    {
        const float az = glm::radians(state.lightAzimuth);
        const float el = glm::radians(state.lightElevation);
        const glm::vec3 dir = glm::normalize(glm::vec3(
            std::cos(el) * std::sin(az),
            std::cos(el) * std::cos(az),
            std::sin(el)));
        renderer_.SetLight(dir,
            glm::vec3(state.lightColor[0],   state.lightColor[1],   state.lightColor[2]),
            glm::vec3(state.ambientColor[0], state.ambientColor[1], state.ambientColor[2]));
    }

    if (mode_ == ViewportMode::Scene) {
        float unit = 1.f;
        if (camera_.radius > 100.f)      unit = 50.f;
        else if (camera_.radius > 20.f)  unit = 10.f;
        renderer_.DrawGrid(unit, 10);

        // ── Terrain mesh — drawn before cell objects so they render on top ────────
        if (terrainMesh_ != MeshHandle::Invalid) {
            DrawSurface terrSurf;
            terrSurf.useVertexColor = state.landRecord.hasColors;
            terrSurf.tint = state.landRecord.hasColors
                ? glm::vec4(1.f, 1.f, 1.f, 1.f)
                : glm::vec4(0.35f, 0.45f, 0.30f, 1.f);
            renderer_.DrawMesh(terrainMesh_, glm::mat4(1.f), terrSurf);
        }

        // ── Cell environment — two-pass to avoid blended surfaces writing depth ──
        // Pass 1: opaque + alpha-test (depth write on, full depth rejection)
        // Pass 2: alpha-blend + additive (depth write off, blended on top)

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

            // Use animated toRoot if the catalog entry has controller data,
            // otherwise fall back to the baked static toRoot.
            const glm::mat4 toRoot = animLocals.empty()
                ? shape.toRoot
                : animatedToRoot(shape, animLocals);

            const bool selected = (inst.refIndex == state.selectedCellRefIndex);
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
            renderer_.DrawMesh(shape.mesh, inst.placement * toRoot, surf);
        };

        // Pre-pass: update GPU position buffers for morph-animated shapes.
        // Done once per catalog entry (not per instance) so shared mesh handles
        // are only written once per frame regardless of how many instances share them.
        {
            std::vector<glm::vec3> morphScratch;
            for (const auto& [key, entry] : cellMeshCatalog_) {
                for (const auto& shape : entry.shapes) {
                    if (shape.morphAnim.empty()) continue;
                    shape.morphAnim.Evaluate(simTime_, morphScratch);
                    renderer_.UpdateMeshPositions(shape.mesh, morphScratch);
                }
            }
        }

        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& inst : cellInstances_) {
                auto it = cellMeshCatalog_.find(inst.baseFormKey);
                if (it == cellMeshCatalog_.end()) continue;
                const CellCatalogEntry& entry = it->second;

                // Evaluate animated node transforms once per catalog entry per frame.
                std::unordered_map<std::string, glm::mat4> animLocals;
                if (!entry.animClip.empty())
                    entry.animClip.Evaluate(simTime_, animLocals);

                for (const auto& shape : entry.shapes)
                    drawCellShape(inst, shape, animLocals, pass == 1);
            }
        }

        for (int ai = 0; ai < (int)actorCache_.size(); ai++) {
            const auto& cache = actorCache_[ai];
            for (int mi = 0; mi < (int)cache.meshHandles.size(); mi++) {
                const TextureHandle th = (mi < (int)cache.textureHandles.size())
                    ? cache.textureHandles[mi] : TextureHandle::Invalid;
                const bool hasTex = (th != TextureHandle::Invalid);

                DrawSurface surf;
                surf.diffuse = th;
                surf.tint    = hasTex ? glm::vec4(1.f, 1.f, 1.f, 1.f)
                                      : glm::vec4(0.70f, 0.70f, 0.75f, 1.f);

                const MeshSkinBinding* msb =
                    (mi < (int)cache.meshSkinBindings.size())
                    ? &cache.meshSkinBindings[mi] : nullptr;

                if (msb && msb->isSkinned && !msb->skelBoneIdx.empty()
                    && !cache.pose.boneWorldMat.empty()) {
                    // Skinned path: build per-skin-bone transforms then GPU-skin on shader.
                    const int numSkinBones = (int)msb->skelBoneIdx.size();
                    std::vector<glm::mat4> skinMats(numSkinBones, glm::mat4(1.f));
                    for (int j = 0; j < numSkinBones; j++) {
                        const int si = msb->skelBoneIdx[j];
                        if (si >= 0 && si < (int)cache.pose.boneWorldMat.size())
                            skinMats[j] = cache.pose.boneWorldMat[si]
                                        * msb->inverseBindMats[j];
                    }
                    renderer_.DrawSkinnedMesh(cache.meshHandles[mi],
                                             glm::mat4(1.f), skinMats, surf);
                } else {
                    // Static path: apply per-mesh toRoot (NIF and world share Z-up space).
                    const glm::mat4 model = cache.meshTransforms[mi];
                    renderer_.DrawMesh(cache.meshHandles[mi], model, surf);
                }
            }

            // Skeleton overlay
            const Pose& pose = actorCache_[ai].pose;
            if (pose.empty()) continue;
            const Skeleton* skel = state.SkeletonForActor(ai);
            if (!skel) continue;
            renderer_.DrawSkeleton(*skel, pose);
        }
    }

    renderer_.EndFrame();

    // Display the FBO colour texture (UV-flipped: OpenGL Y=0 is at bottom).
    const ImVec2 imgMin = ImGui::GetCursorScreenPos();
    ImGui::Image(renderer_.GetOutputTexture(), size,
                 ImVec2(0.f, 1.f), ImVec2(1.f, 0.f));

    // Camera input + left-click picking on the image widget.
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.f)) {
            if (io.KeyShift)
                camera_.Pan(io.MouseDelta.x, -io.MouseDelta.y);
            else
                camera_.Orbit(-io.MouseDelta.x * 0.5f, io.MouseDelta.y * 0.5f);
        }
        if (io.MouseWheel != 0.f)
            camera_.Zoom(io.MouseWheel);

        // Left-click with no drag → pick cell reference.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.f)) {
            const float ndcX = 2.f * (io.MousePos.x - imgMin.x) / size.x - 1.f;
            const float ndcY = 1.f - 2.f * (io.MousePos.y - imgMin.y) / size.y;
            const int picked = PickCellRef(ndcX, ndcY, size.x / size.y);
            state.selectedCellRefIndex = picked;
        }
    }

    // ── Viewport overlays (drawn via window DrawList over the image) ─────────
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (mode_ == ViewportMode::Scene) {
            // Axis gizmo — bottom-left corner
            const glm::mat4& v = camera_.View();
            const float gx  = imgMin.x + 45.f;
            const float gy  = imgMin.y + size.y - 45.f;
            const float len = 28.f;

            // X (red), Y (green), Z (blue): project world unit vectors into screen
            // v[col][row]: col 0/1/2 gives how world X/Y/Z map to view-space x/y
            dl->AddLine({gx, gy}, {gx + v[0][0]*len, gy - v[0][1]*len},
                        IM_COL32(220, 65, 65, 230), 2.f);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                        {gx + v[0][0]*len + 2, gy - v[0][1]*len - 6},
                        IM_COL32(220, 65, 65, 200), "X");
            dl->AddLine({gx, gy}, {gx + v[1][0]*len, gy - v[1][1]*len},
                        IM_COL32(65, 210, 65, 230), 2.f);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                        {gx + v[1][0]*len + 2, gy - v[1][1]*len - 6},
                        IM_COL32(65, 210, 65, 200), "Y");
            dl->AddLine({gx, gy}, {gx + v[2][0]*len, gy - v[2][1]*len},
                        IM_COL32(65, 100, 220, 230), 2.f);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                        {gx + v[2][0]*len + 2, gy - v[2][1]*len - 6},
                        IM_COL32(65, 100, 220, 200), "Z");

            // Playback badge — top-right corner
            const char* badge  = state.playing ? "\xe2\x96\xb6 PLAYING" : "\xe2\x80\x96 PAUSED";
            const ImU32 badgeC = state.playing
                ? IM_COL32(80, 220, 100, 230)
                : IM_COL32(140, 140, 155, 180);
            const ImVec2 ts = ImGui::CalcTextSize(badge);
            const float  bx = imgMin.x + size.x - ts.x - 14.f;
            const float  by = imgMin.y + 10.f;
            dl->AddRectFilled({bx - 6, by - 4}, {bx + ts.x + 6, by + ts.y + 4},
                              IM_COL32(8, 10, 16, 180), 4.f);
            dl->AddText({bx, by}, badgeC, badge);

            // Exterior cell coordinate badge — top centre
            if (state.loadedCell.loaded && state.loadedCell.isExterior) {
                char coordText[128];
                std::snprintf(coordText, sizeof(coordText), "Cell [%d, %d]  (%.0f, %.0f)",
                              state.loadedCell.cellX, state.loadedCell.cellY,
                              (float)(state.loadedCell.cellX * 4096),
                              (float)(state.loadedCell.cellY * 4096));
                const ImVec2 cts = ImGui::CalcTextSize(coordText);
                const float  ctx = imgMin.x + size.x * 0.5f - cts.x * 0.5f;
                const float  cty = imgMin.y + 10.f;
                dl->AddRectFilled({ctx - 6.f, cty - 4.f},
                                  {ctx + cts.x + 6.f, cty + cts.y + 4.f},
                                  IM_COL32(8, 10, 16, 180), 4.f);
                dl->AddText({ctx, cty}, IM_COL32(180, 200, 240, 220), coordText);
            }

            // Cell stream progress bar — bottom centre, visible while loading.
            if (cellStreaming_ || cellStreamDone_ < cellStreamTotal_) {
                const int   total = cellStreamTotal_.load();
                const int   done  = cellStreamDone_.load();
                const float frac  = (total > 0) ? (float)done / (float)total : 0.f;
                char label[48];
                std::snprintf(label, sizeof(label), "Loading cell...  %d / %d", done, total);
                const ImVec2 lsz = ImGui::CalcTextSize(label);
                const float  barW = size.x * 0.45f;
                const float  barH = 4.f;
                const float  cx   = imgMin.x + size.x * 0.5f;
                const float  py   = imgMin.y + size.y - 22.f;
                // Text
                dl->AddText({ cx - lsz.x * 0.5f, py - lsz.y - 2.f },
                            IM_COL32(200, 200, 210, 200), label);
                // Track
                dl->AddRectFilled({ cx - barW * 0.5f, py },
                                  { cx + barW * 0.5f, py + barH },
                                  IM_COL32(40, 42, 50, 200), 2.f);
                // Fill
                dl->AddRectFilled({ cx - barW * 0.5f, py },
                                  { cx - barW * 0.5f + barW * frac, py + barH },
                                  IM_COL32(80, 160, 240, 220), 2.f);
            }
        }
    }

    // Scaffolded mode overlays (drawn on top via window DrawList).
    if (mode_ != ViewportMode::Scene) {
        const char* label = "Coming Soon";
        if      (mode_ == ViewportMode::Face)    label = "Face View  —  coming soon";
        else if (mode_ == ViewportMode::Cameras)  label = "Camera Preview  —  coming soon";
        else if (mode_ == ViewportMode::Bones)    label = "Bone Editor  —  coming soon";
        const ImVec2 p0 = ImGui::GetItemRectMin();
        ImVec2 ts = ImGui::CalcTextSize(label);
        ImGui::GetWindowDrawList()->AddText(
            { p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f },
            IM_COL32(180, 180, 200, 120), label);
    }

    ImGui::End();
}

// ── Internals ─────────────────────────────────────────────────────────────────

void ViewportPanel::RebuildActorCache(AppState& state)
{
    // Free all GPU resources before resizing — SyncNifHandles will re-upload.
    for (auto& ar : actorCache_) {
        for (MeshHandle    h : ar.meshHandles)    renderer_.FreeMesh(h);
        for (TextureHandle t : ar.textureHandles) renderer_.FreeTexture(t);
        ar.meshHandles.clear();
        ar.textureHandles.clear();
        ar.meshTransforms.clear();
        ar.meshSkinBindings.clear();
        ar.meshMorphBases.clear();
        ar.morphWeightsCached.clear();
        ar.morphWeightsEval.clear();
        ar.loadedNifPath.clear();
    }

    const int n = (int)state.actors.size();
    actorCache_.resize(n);
    for (int ai = 0; ai < n; ai++) {
        actorCache_[ai].refPose = {};
        actorCache_[ai].pose    = {};
        const Skeleton* skel = state.SkeletonForActor(ai);
        if (!skel) continue;
        actorCache_[ai].refPose = skel->MakeReferencePose();
        actorCache_[ai].pose    = actorCache_[ai].refPose;
        actorCache_[ai].pose.SolveFK();
    }
    cachedActorCount_    = n;
    cachedSkeletonCount_ = (int)state.skeletons.size();

    FrameAll();
}

void ViewportPanel::SyncNifHandles(AppState& state)
{
    for (int ai = 0; ai < (int)actorCache_.size(); ai++) {
        const Actor& actor = state.actors[ai];
        const ActorDocument* ce = (actor.castIndex >= 0 &&
                                   actor.castIndex < (int)state.cast.size())
                                ? &state.cast[actor.castIndex] : nullptr;
        const std::string& bodyPath  = ce ? ce->bodyNifPath  : "";
        const std::string& handsPath = ce ? ce->handsNifPath : "";
        const std::string& feetPath  = ce ? ce->feetNifPath  : "";
        const std::string& headPath  = ce ? ce->headNifPath  : "";

        // Cache key covers all loaded NIFs so any change triggers a full reload.
        std::string cacheKey = bodyPath + "|" + handsPath + "|" + feetPath + "|" + headPath;
        if (ce) for (const auto& hp : ce->headPartNifs) cacheKey += "|" + hp;
        if (cacheKey == actorCache_[ai].loadedNifPath) continue;

        for (MeshHandle    h : actorCache_[ai].meshHandles)    renderer_.FreeMesh(h);
        for (TextureHandle t : actorCache_[ai].textureHandles) renderer_.FreeTexture(t);
        actorCache_[ai].meshHandles.clear();
        actorCache_[ai].textureHandles.clear();
        actorCache_[ai].meshTransforms.clear();
        actorCache_[ai].meshSkinBindings.clear();
        actorCache_[ai].meshMorphBases.clear();
        actorCache_[ai].morphWeightsCached.clear(); // force SyncMorphs re-apply on reload
        actorCache_[ai].morphWeightsEval.clear();
        actorCache_[ai].loadedNifPath = cacheKey;

        // Load one NIF and append its shapes into the actor's render cache.
        // Data-relative paths (Meshes\...) go through ResolveAsset so BSA-packed
        // NIFs (e.g. vanilla facegeom) are found without a loose-file fallback.
        // Absolute paths (manually browsed) are loaded directly.
        auto loadNif = [&](const std::string& path) {
            if (path.empty()) return;
            fprintf(stderr, "[Viewport] Loading NIF: %s\n", path.c_str());

            NifDocument doc;
            const bool isAbsolute = (path.size() >= 2 && path[1] == ':')
                                  || (!path.empty() && (path[0] == '/' || path[0] == '\\'));
            if (isAbsolute) {
                doc = LoadNifDocument(path);
            } else {
                std::vector<uint8_t> nifBytes;
                if (state.ResolveAsset(path, nifBytes))
                    doc = LoadNifDocumentFromBytes(nifBytes, path);
                else
                    fprintf(stderr, "[Viewport]   NIF not resolved via ResolveAsset\n");
            }

            fprintf(stderr, "[Viewport]   -> %d shape(s)\n", (int)doc.shapes.size());

            const Skeleton* skel = state.SkeletonForActor(ai);

            for (int si = 0; si < (int)doc.shapes.size(); si++) {
                const NifDocShape& ds    = doc.shapes[si];
                const NifBlock&    block = doc.blocks[ds.blockIndex];

                actorCache_[ai].meshHandles.push_back(renderer_.UploadMesh(ds.meshData));
                actorCache_[ai].meshTransforms.push_back(block.toRoot);

                // Store base positions for CPU-side morph blending.
                MeshMorphBase mmb;
                mmb.positions   = ds.meshData.positions;
                mmb.vertexCount = (int)ds.meshData.positions.size();
                actorCache_[ai].meshMorphBases.push_back(std::move(mmb));

                // Texture
                TextureHandle th = TextureHandle::Invalid;
                if (!ds.diffusePath.empty()) {
                    std::vector<uint8_t> texBytes;
                    if (state.ResolveAsset(ds.diffusePath, texBytes)) {
                        th = renderer_.LoadTextureFromMemory(texBytes);
                        fprintf(stderr, "[Viewport]   tex OK  '%s' -> handle %u\n",
                                ds.diffusePath.c_str(), (unsigned)th);
                    } else {
                        fprintf(stderr, "[Viewport]   tex MISS '%s'\n",
                                ds.diffusePath.c_str());
                    }
                }
                actorCache_[ai].textureHandles.push_back(th);

                // Skin binding — resolve NIF bone names to Havok skeleton indices
                MeshSkinBinding msb;
                msb.isSkinned = ds.isSkinned;
                if (ds.isSkinned) {
                    const int numSkinBones = (int)ds.skinBindings.size();
                    msb.skelBoneIdx.resize(numSkinBones, -1);
                    msb.inverseBindMats.resize(numSkinBones, glm::mat4(1.f));
                    for (int j = 0; j < numSkinBones; j++) {
                        msb.inverseBindMats[j] = ds.skinBindings[j].inverseBindMatrix;
                        if (skel) {
                            const std::string& boneName = ds.skinBindings[j].boneName;
                            for (int bi = 0; bi < (int)skel->bones.size(); bi++) {
                                if (skel->bones[bi].name == boneName) {
                                    msb.skelBoneIdx[j] = bi;
                                    break;
                                }
                            }
                        }
                    }
                    // Log how many bones resolved
                    int resolved_count = 0;
                    for (int idx : msb.skelBoneIdx) if (idx >= 0) resolved_count++;
                    fprintf(stderr, "[Viewport]   skin '%s': %d/%d bones resolved to skeleton\n",
                            ds.skinBindings.empty() ? "" : ds.skinBindings[0].boneName.c_str(),
                            resolved_count, numSkinBones);
                }
                actorCache_[ai].meshSkinBindings.push_back(std::move(msb));
            }
        };

        loadNif(bodyPath);
        loadNif(handsPath);
        loadNif(feetPath);
        loadNif(headPath);
        if (ce) for (const auto& hp : ce->headPartNifs) loadNif(hp);
    }
}

void ViewportPanel::SyncMorphs(AppState& state)
{
    for (int ai = 0; ai < (int)actorCache_.size(); ai++) {
        const Actor& actor = state.actors[ai];
        if (actor.castIndex < 0 || actor.castIndex >= (int)state.cast.size()) continue;
        ActorDocument& doc = state.cast[actor.castIndex];

        // Timeline FaceData evaluation takes priority over authored inspector sliders.
        const bool hasEval = !actorCache_[ai].morphWeightsEval.empty();
        const std::map<std::string, float>& effective =
            hasEval ? actorCache_[ai].morphWeightsEval : doc.morphWeights;

        // Dirty check — skip if weights unchanged since last apply.
        if (effective == actorCache_[ai].morphWeightsCached) continue;
        actorCache_[ai].morphWeightsCached = effective;

        // triDocs are loaded lazily by InspectorPanel; skip if not yet loaded.
        if (doc.triDocs.empty()) continue;

        // For each mesh shape, find a TRI doc whose vertex count matches, then
        // blend: finalPos[v] = basePos[v] + Σ(weight[m] × delta[m][v])
        for (int mi = 0; mi < (int)actorCache_[ai].meshHandles.size(); mi++) {
            if (mi >= (int)actorCache_[ai].meshMorphBases.size()) continue;
            const MeshMorphBase& base = actorCache_[ai].meshMorphBases[mi];
            if (base.positions.empty()) continue;

            // Match TRI by vertex count.
            const TriDocument* tri = nullptr;
            for (const TriDocument& td : doc.triDocs) {
                if (td.vertexNum == base.vertexCount) { tri = &td; break; }
            }
            if (!tri || tri->morphs.empty()) continue;

            // Start from base and accumulate active morphs.
            std::vector<glm::vec3> morphed = base.positions;
            for (const TriMorph& m : tri->morphs) {
                auto it = effective.find(m.name);
                if (it == effective.end() || it->second == 0.f) continue;
                const float w = it->second;
                const int   n = std::min((int)m.deltas.size(), base.vertexCount);
                for (int v = 0; v < n; v++)
                    morphed[v] += m.deltas[v] * w;
            }

            // Upload morphed positions into the existing GPU buffer.
            renderer_.UpdateMeshPositions(actorCache_[ai].meshHandles[mi], morphed);
        }
    }
}

void ViewportPanel::FreeCellCache()
{
    // Signal and wait for the background stream to stop before touching GPU resources.
    cellStreamCancel_ = true;
    if (cellStream_.joinable()) cellStream_.join();
    cellStreamCancel_ = false;

    // Discard any shapes the worker queued but we haven't uploaded yet.
    { std::lock_guard<std::mutex> lk(cellStreamMtx_);
      cellStreamPending_.clear(); }

    for (auto& [key, entry] : cellMeshCatalog_)
        for (auto& shape : entry.shapes)
            if (shape.mesh != MeshHandle::Invalid) renderer_.FreeMesh(shape.mesh);

    // Textures are owned by cellTexCache_, not by individual shapes.
    for (auto& [path, tex] : cellTexCache_)
        if (tex != TextureHandle::Invalid) renderer_.FreeTexture(tex);

    cellTexCache_.clear();
    cellMeshCatalog_.clear();
    cellInstances_.clear();
    cellLoadedKey_.clear();
    cellStreamTotal_ = 0;
    cellStreamDone_  = 0;
    cellStreaming_    = false;

    if (terrainMesh_ != MeshHandle::Invalid) {
        renderer_.FreeMesh(terrainMesh_);
        terrainMesh_ = MeshHandle::Invalid;
    }
    terrainCellKey_.clear();
}

void ViewportPanel::SyncCellMeshes(AppState& state)
{
    const std::string newKey = state.loadedCell.loaded
                             ? state.loadedCell.formKey : std::string{};
    if (newKey == cellLoadedKey_) return;

    FreeCellCache();   // cancels + joins any old stream, frees GPU resources
    cellLoadedKey_ = newKey;

    if (state.loadedCell.Empty()) return;

    const int numRefs = (int)state.loadedCell.refs.size();
    fprintf(stderr, "[Cell] Streaming '%s' (%d refs)...\n",
            state.loadedCell.name.c_str(), numRefs);

    // ── Build instance list synchronously — pure TRS math, no I/O ────────────
    // NIF, Havok, and REFR data are all in the same Z-up coordinate system.
    // Negate angles: Bethesda CW-positive vs GLM CCW-positive.
    cellInstances_.reserve(numRefs);
    for (int ri = 0; ri < numRefs; ++ri) {
        const auto& ref = state.loadedCell.refs[ri];
        const glm::mat4 S  = glm::scale    (glm::mat4(1.f), glm::vec3(ref.scale));
        const glm::mat4 Rx = glm::rotate   (glm::mat4(1.f), -ref.rotX, glm::vec3(1,0,0));
        const glm::mat4 Ry = glm::rotate   (glm::mat4(1.f), -ref.rotY, glm::vec3(0,1,0));
        const glm::mat4 Rz = glm::rotate   (glm::mat4(1.f), -ref.rotZ, glm::vec3(0,0,1));
        const glm::mat4 T  = glm::translate(glm::mat4(1.f), glm::vec3(ref.posX, ref.posY, ref.posZ));
        cellInstances_.push_back({ ref.baseFormKey, T * (Rx * Ry * Rz) * S, ri });
    }

    // ── Snapshot state the worker needs — no AppState access after this ───────
    cellStreamTotal_ = numRefs;
    cellStreamDone_  = 0;
    cellStreaming_    = true;

    cellStream_ = std::thread(&ViewportPanel::StreamWorker, this,
                              state.dataFolder,
                              state.bsaSearchList,
                              state.loadedCell.refs);
}

void ViewportPanel::SyncTerrainMesh(AppState& state)
{
    const std::string key = (state.loadedCell.loaded && state.loadedCell.isExterior)
                          ? state.loadedCell.formKey : std::string{};
    if (key == terrainCellKey_) return;
    terrainCellKey_ = key;

    if (terrainMesh_ != MeshHandle::Invalid) {
        renderer_.FreeMesh(terrainMesh_);
        terrainMesh_ = MeshHandle::Invalid;
    }

    if (key.empty()) return;

    MeshData md = GenerateTerrainMesh(state.landRecord,
                                      state.loadedCell.cellX, state.loadedCell.cellY);
    terrainMesh_ = renderer_.UploadMesh(md);
    fprintf(stderr, "[Terrain] Generated mesh for cell (%d,%d)\n",
            state.loadedCell.cellX, state.loadedCell.cellY);

    // Aim the camera at the terrain centre at average height.
    float avgH = 0.f;
    for (int r = 0; r < 33; r++)
        for (int c = 0; c < 33; c++)
            avgH += state.landRecord.heights[r][c];
    avgH /= (33.f * 33.f);

    camera_.target    = glm::vec3(
        (float)(state.loadedCell.cellX * 4096 + 2048),
        (float)(state.loadedCell.cellY * 4096 + 2048),
        avgH);
    camera_.radius    = 8192.f;
    camera_.azimuth   = 210.f;
    camera_.elevation = 30.f;
}

void ViewportPanel::StreamWorker(std::string dataFolder,
                                 std::vector<std::string> bsaSearchList,
                                 std::vector<CellPlacedRef> refs)
{
    // Pre-open all BSAs once so every resolve call reuses the cached index
    // rather than re-reading the full index from disk on each lookup.
    std::vector<std::unique_ptr<BsaReader>> openBsas;
    openBsas.reserve(bsaSearchList.size());
    for (const auto& bsaPath : bsaSearchList) {
        auto bsa = std::make_unique<BsaReader>();
        char err[256] = {};
        if (bsa->Open(bsaPath, err, sizeof(err)))
            openBsas.push_back(std::move(bsa));
    }

    // Thread-local resolve: mirrors AppState::ResolveAsset but uses captured
    // snapshots so this thread never touches AppState after launch.
    auto resolve = [&](const std::string& relPath,
                       std::vector<uint8_t>& outBytes) -> bool
    {
        if (relPath.empty() || cellStreamCancel_) return false;
        std::string rel = relPath;
        for (char& c : rel) if (c == '\\') c = '/';

        // Loose file first.
        if (!dataFolder.empty()) {
            std::ifstream f(dataFolder + "/" + rel, std::ios::binary);
            if (f) {
                outBytes.assign(std::istreambuf_iterator<char>(f), {});
                return true;
            }
        }
        // Walk pre-opened BSAs — index already in memory, O(1) hash lookup per BSA.
        std::string bsaKey = rel;
        for (char& c : bsaKey) if (c == '/') c = '\\';
        for (char& c : bsaKey) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);

        for (const auto& bsa : openBsas) {
            if (cellStreamCancel_) return false;
            const BsaFileInfo* fi = bsa->FindExact(bsaKey);
            if (!fi) continue;
            char xErr[256] = {};
            if (bsa->Extract(*fi, outBytes, xErr, sizeof(xErr))) return true;
        }
        return false;
    };

    std::unordered_set<std::string> seenBases;

    for (int ri = 0; ri < (int)refs.size(); ++ri) {
        if (cellStreamCancel_) break;

        const CellPlacedRef& ref = refs[ri];
        ++cellStreamDone_;

        if (seenBases.count(ref.baseFormKey)) continue; // already loaded successfully

        // Resolve NIF bytes.
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
        // Don't mark seen on NIF miss — a later ref with the same baseFormKey
        // might carry a different nifPath that resolves.  Mark seen only after
        // a successful load so the catalog-presence check mirrors the old
        // synchronous behaviour (which retried until one ref succeeded).
        if (nifBytes.empty() || cellStreamCancel_) continue;
        seenBases.insert(ref.baseFormKey);

        // Parse NIF — nifly is stateless, safe to call from any thread.
        NifDocument doc = LoadNifDocumentFromBytes(nifBytes, ref.nifPath);
        if (cellStreamCancel_) break;

        // Build PendingShape list for this base object.
        std::vector<PendingShape> batch;
        for (int si = 0; si < (int)doc.shapes.size(); ++si) {
            if (cellStreamCancel_) break;
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
                case NifAlphaMode::AlphaTest:        ps.blendMode = DrawSurface::BlendMode::AlphaTest;        break;
                case NifAlphaMode::AlphaBlend:       ps.blendMode = DrawSurface::BlendMode::AlphaBlend;       break;
                case NifAlphaMode::Additive:         ps.blendMode = DrawSurface::BlendMode::Additive;         break;
                case NifAlphaMode::AlphaTestAndBlend:ps.blendMode = DrawSurface::BlendMode::AlphaTestAndBlend;break;
                default:                             ps.blendMode = DrawSurface::BlendMode::Opaque;           break;
            }
            // Local-space AABB for ray picking.
            glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
            for (const auto& p : ds.meshData.positions) {
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
            ps.localMin = lo;
            ps.localMax = hi;

            // Resolve texture bytes (pre-extract here so main thread only uploads).
            if (!ds.diffusePath.empty()) {
                ps.diffusePath = ds.diffusePath;
                for (char& c : ps.diffusePath)
                    c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
                resolve(ds.diffusePath, ps.ddsBytes);
            }
            batch.push_back(std::move(ps));
        }

        if (!batch.empty() && !cellStreamCancel_) {
            // Store animClip in the first shape only — main thread drains it
            // into the CellCatalogEntry on first encounter for this base.
            batch[0].animClip = std::move(doc.animClip);
            std::lock_guard<std::mutex> lk(cellStreamMtx_);
            for (auto& ps : batch)
                cellStreamPending_.push_back(std::move(ps));
        }
    }

    cellStreaming_ = false;
    fprintf(stderr, "[Cell] Stream finished: %d unique bases\n", (int)seenBases.size());
}

void ViewportPanel::EvaluatePoses(AppState& state)
{
    if (actorCache_.empty()) return;

    if (!state.sequence.Empty()) {
        std::vector<ActorEval> evals;
        state.sequence.Evaluate(state.time, state, evals);
        for (int ai = 0; ai < (int)actorCache_.size(); ai++) {
            if (ai < (int)evals.size()) {
                actorCache_[ai].pose = evals[ai].pose;
                actorCache_[ai].pose.SolveFK();
                // Capture face morph weights from FaceData track evaluation.
                actorCache_[ai].morphWeightsEval = std::move(evals[ai].morphWeights);
            } else {
                actorCache_[ai].pose = actorCache_[ai].refPose;
                actorCache_[ai].pose.SolveFK();
                actorCache_[ai].morphWeightsEval.clear();
            }
        }
    } else if (state.selectedClip >= 0 && state.selectedClip < (int)state.clips.size()) {
        for (int ai = 0; ai < (int)actorCache_.size(); ai++) {
            actorCache_[ai].pose = actorCache_[ai].refPose;
            state.clips[state.selectedClip].Evaluate(state.time, actorCache_[ai].pose);
            actorCache_[ai].pose.SolveFK();
            actorCache_[ai].morphWeightsEval.clear();  // no face track in clip-preview mode
        }
    } else {
        for (auto& ar : actorCache_) {
            ar.pose = ar.refPose;
            ar.pose.SolveFK();
            ar.morphWeightsEval.clear();
        }
    }
}

void ViewportPanel::FrameAll()
{
    glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
    bool any = false;
    for (const auto& ar : actorCache_) {
        if (ar.refPose.empty()) continue;
        for (const auto& wp : ar.refPose.worldPos) {
            mn = glm::min(mn, wp);
            mx = glm::max(mx, wp);
            any = true;
        }
    }
    if (!any) return;
    camera_.target    = (mn + mx) * 0.5f;
    camera_.radius    = glm::length(mx - mn) * 1.3f;
    camera_.azimuth   = 210.f;
    camera_.elevation = 10.f;
    fprintf(stderr, "[Viewport] FrameAll: mn=(%.2f,%.2f,%.2f) mx=(%.2f,%.2f,%.2f) radius=%.2f\n",
            mn.x, mn.y, mn.z, mx.x, mx.y, mx.z, camera_.radius);
}
