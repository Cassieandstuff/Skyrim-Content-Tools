#include "ViewportPanel.h"
#include "AppState.h"
#include "NifDocument.h"
#include "Sequence.h"
#include "HavokSkeleton.h"
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cfloat>
#include <cstdio>

// ── Coordinate conversion ─────────────────────────────────────────────────────
// NIF / Havok space is Z-up, Y-forward.  SCT world space is Y-up, Z-forward.
// Column-major: col1 = NIF-Y→world-Z, col2 = NIF-Z→world-Y.
// Used by both actor NIF rendering and cell placed-object rendering.
static const glm::mat4 kNifToWorld = glm::mat4(
    glm::vec4(1, 0, 0, 0),   // NIF X  → world X
    glm::vec4(0, 0, 1, 0),   // NIF Y (forward) → world Z
    glm::vec4(0, 1, 0, 0),   // NIF Z (up)      → world Y
    glm::vec4(0, 0, 0, 1)
);

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

    const glm::mat4 VP    = camera_.Proj(aspect) * camera_.View();
    const glm::mat4 invVP = glm::inverse(VP);
    const glm::vec4 far4  = invVP * glm::vec4(ndcX, ndcY, 1.f, 1.f);
    const glm::vec3 rayO  = camera_.Eye();
    const glm::vec3 rayD  = glm::normalize(glm::vec3(far4) / far4.w - rayO);

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

    // Evaluate all actor poses for this frame (also fills morphWeightsEval from FaceData track).
    EvaluatePoses(state);

    // Apply ARKit blend-shape weights to face mesh positions when weights change.
    SyncMorphs(state);

    // Render scene to FBO.
    const int iw = (int)size.x;
    const int ih = (int)size.y;
    const glm::mat4 proj = camera_.Proj(size.x / size.y);
    const glm::mat4 view = camera_.View();

    renderer_.BeginFrame(iw, ih);
    renderer_.SetCamera(view, proj);

    if (mode_ == ViewportMode::Scene) {
        float unit = 1.f;
        if (camera_.radius > 100.f)      unit = 50.f;
        else if (camera_.radius > 20.f)  unit = 10.f;
        renderer_.DrawGrid(unit, 10);

        // kNifToWorld defined at file scope above — converts NIF/Havok Z-up to Y-up world.

        // ── Cell environment ──────────────────────────────────────────────────
        for (const auto& inst : cellInstances_) {
            auto it = cellMeshCatalog_.find(inst.baseFormKey);
            if (it == cellMeshCatalog_.end()) continue;
            const bool selected = (inst.refIndex == state.selectedCellRefIndex);
            for (const auto& shape : it->second.shapes) {
                if (shape.mesh == MeshHandle::Invalid) continue;
                DrawSurface surf;
                surf.diffuse = shape.texture;
                if (selected) {
                    surf.tint = glm::vec4(1.f, 0.65f, 0.1f, 1.f);  // orange highlight
                } else {
                    surf.tint = (shape.texture != TextureHandle::Invalid)
                        ? glm::vec4(1.f, 1.f, 1.f, 1.f)
                        : glm::vec4(0.70f, 0.70f, 0.75f, 1.f);
                }
                renderer_.DrawMesh(shape.mesh, inst.placement * shape.toRoot, surf);
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
                            skinMats[j] = kNifToWorld
                                        * cache.pose.boneWorldMat[si]
                                        * msb->inverseBindMats[j];
                    }
                    renderer_.DrawSkinnedMesh(cache.meshHandles[mi],
                                             glm::mat4(1.f), skinMats, surf);
                } else {
                    // Static path: apply per-mesh toRoot then coordinate conversion.
                    const glm::mat4 model = kNifToWorld * cache.meshTransforms[mi];
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
            state.selectedCellRefIndex = PickCellRef(ndcX, ndcY, size.x / size.y);
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
    for (auto& [key, entry] : cellMeshCatalog_) {
        for (auto& shape : entry.shapes) {
            if (shape.mesh    != MeshHandle::Invalid)    renderer_.FreeMesh(shape.mesh);
            if (shape.texture != TextureHandle::Invalid) renderer_.FreeTexture(shape.texture);
        }
    }
    cellMeshCatalog_.clear();
    cellInstances_.clear();
    cellLoadedKey_.clear();
}

void ViewportPanel::SyncCellMeshes(AppState& state)
{
    // Compute the key representing the currently desired cell state.
    const std::string newKey = state.loadedCell.loaded
                             ? state.loadedCell.formKey
                             : std::string{};

    if (newKey == cellLoadedKey_) return;   // no change

    FreeCellCache();                         // releases old GPU resources
    cellLoadedKey_ = newKey;

    if (state.loadedCell.Empty()) return;

    fprintf(stderr, "[Cell] Building render cache for '%s' (%d refs)...\n",
            state.loadedCell.name.c_str(), (int)state.loadedCell.refs.size());

    // NOTE: This is synchronous and will block for several seconds on large
    // cells (e.g. Whiterun).  Async streaming is a future improvement.

    for (int ri = 0; ri < (int)state.loadedCell.refs.size(); ++ri) {
        const auto& ref = state.loadedCell.refs[ri];

        // ── Mesh catalog: one NIF upload per unique base object ───────────────
        if (cellMeshCatalog_.find(ref.baseFormKey) == cellMeshCatalog_.end()) {
            CellCatalogEntry entry;

            NifDocument doc;
            const bool isAbsolute =
                (ref.nifPath.size() >= 2 && ref.nifPath[1] == ':') ||
                (!ref.nifPath.empty() &&
                 (ref.nifPath[0] == '/' || ref.nifPath[0] == '\\'));

            if (isAbsolute) {
                doc = LoadNifDocument(ref.nifPath);
            } else {
                std::vector<uint8_t> nifBytes;
                if (state.ResolveAsset(ref.nifPath, nifBytes))
                    doc = LoadNifDocumentFromBytes(nifBytes, ref.nifPath);
            }

            for (int si = 0; si < (int)doc.shapes.size(); si++) {
                const NifDocShape& ds    = doc.shapes[si];
                const NifBlock&    block = doc.blocks[ds.blockIndex];
                if (ds.meshData.positions.empty()) continue;

                CellShapeEntry se;
                se.toRoot = block.toRoot;

                // Local-space AABB for ray picking.
                if (!ds.meshData.positions.empty()) {
                    glm::vec3 lo( FLT_MAX), hi(-FLT_MAX);
                    for (const auto& p : ds.meshData.positions) {
                        lo = glm::min(lo, p);
                        hi = glm::max(hi, p);
                    }
                    se.localMin = lo;
                    se.localMax = hi;
                }

                se.mesh = renderer_.UploadMesh(ds.meshData);

                // DEBUG: print full toRoot matrix for floor-cap + furniture
                // pieces to see if toRoot has any rotation component.
                if (ref.nifPath.find("FloorCap") != std::string::npos ||
                    ref.nifPath.find("Furniture") != std::string::npos ||
                    ref.nifPath.find("Alchemy")   != std::string::npos) {
                    // GLM column-major: [col][row]. Print rotation columns 0,1,2.
                    fprintf(stderr,
                        "[ToRootFull] %-55s  shape=%-25s\n"
                        "             col0=(%+.3f,%+.3f,%+.3f)  col1=(%+.3f,%+.3f,%+.3f)"
                        "  col2=(%+.3f,%+.3f,%+.3f)  tr=(%+.1f,%+.1f,%+.1f)\n",
                        ref.nifPath.c_str(), block.name.c_str(),
                        block.toRoot[0][0], block.toRoot[0][1], block.toRoot[0][2],
                        block.toRoot[1][0], block.toRoot[1][1], block.toRoot[1][2],
                        block.toRoot[2][0], block.toRoot[2][1], block.toRoot[2][2],
                        block.toRoot[3][0], block.toRoot[3][1], block.toRoot[3][2]);
                }

                if (!ds.diffusePath.empty()) {
                    std::vector<uint8_t> texBytes;
                    if (state.ResolveAsset(ds.diffusePath, texBytes))
                        se.texture = renderer_.LoadTextureFromMemory(texBytes);
                }
                entry.shapes.push_back(std::move(se));
            }

            // Always insert (even if empty) to avoid retrying a failed NIF.
            cellMeshCatalog_[ref.baseFormKey] = std::move(entry);
        }

        // ── Instance: placement transform (Skyrim Z-up → Y-up world) ─────────
        // Skyrim REFR uses extrinsic ZYX: rotZ (yaw around world Z-up) is applied
        // first to the vertex, then rotY, then rotX last.  Column-vector matrix
        // form: R = Rx * Ry * Rz  (rightmost applied first).
        //
        // Sign correction: kNifToWorld is a YZ-swap with det = -1.  Direct
        // calculation shows kNifToWorld * Rz(θ) acts as Ry(-θ) in world space,
        // kNifToWorld * Rx(θ) acts as Rx(-θ), and kNifToWorld * Ry(θ) acts as
        // Rz(-θ).  We pre-negate rotZ and rotY to compensate; rotX is left
        // positive because the double-negation via det=-1 cancels out.
        //
        // Order matters for compound rotations: Rx * Rz keeps the up-vector
        // (Z-axis) independent of the yaw (Rz doesn't rotate Z), so tilt and
        // yaw decouple correctly.  Rz * Rx couples them and "swings" tilted
        // pieces sideways.
        const glm::mat4 S  = glm::scale(glm::mat4(1.f), glm::vec3(ref.scale));
        const glm::mat4 Rx = glm::rotate(glm::mat4(1.f),  ref.rotX, glm::vec3(1.f, 0.f, 0.f));
        const glm::mat4 Ry = glm::rotate(glm::mat4(1.f), -ref.rotY, glm::vec3(0.f, 1.f, 0.f));
        const glm::mat4 Rz = glm::rotate(glm::mat4(1.f), -ref.rotZ, glm::vec3(0.f, 0.f, 1.f));
        const glm::mat4 T  = glm::translate(glm::mat4(1.f),
                                            glm::vec3(ref.posX, ref.posY, ref.posZ));
        const glm::mat4 TRS = T * (Rx * Ry * Rz) * S;

        // DEBUG: log refs with non-zero rotX/rotY to catch compound rotations.
        if (fabsf(ref.rotX) > 0.01f || fabsf(ref.rotY) > 0.01f) {
            fprintf(stderr, "[CellRot] nif=%-60s  pos=(%+8.1f,%+8.1f,%+8.1f)  rot=(%+.3f,%+.3f,%+.3f)\n",
                    ref.nifPath.c_str(),
                    ref.posX, ref.posY, ref.posZ,
                    ref.rotX, ref.rotY, ref.rotZ);
        }

        cellInstances_.push_back({ ref.baseFormKey, kNifToWorld * TRS, ri });
    }

    const int uniqueBases = (int)cellMeshCatalog_.size();
    const int totalShapes = [&]{
        int n = 0;
        for (auto& [k, e] : cellMeshCatalog_) n += (int)e.shapes.size();
        return n;
    }();
    fprintf(stderr, "[Cell] Done: %d unique bases, %d total shapes, %d instances\n",
            uniqueBases, totalShapes, (int)cellInstances_.size());
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
