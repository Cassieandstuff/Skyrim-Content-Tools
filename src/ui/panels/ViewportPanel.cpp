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

    // Apply ARKit blend-shape weights to face mesh positions when weights change.
    SyncMorphs(state);

    // Evaluate all actor poses for this frame.
    EvaluatePoses(state);

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

        // NIF (Z-up, Y-forward) → world (Y-up, Z-forward).
        // Must match Pose::SolveFK's axis swap: worldPos = (havok_x, havok_z, havok_y).
        // Both NIF and Havok use Z-up / Y-forward; SolveFK maps forward(+Y)→worldZ(+Z).
        // Column-major: col0=nifX→worldX, col1=nifY→worldZ, col2=nifZ→worldY.
        static const glm::mat4 kNifToWorld = glm::mat4(
            glm::vec4(1, 0, 0, 0),   // NIF X  → world X
            glm::vec4(0, 0, 1, 0),   // NIF Y (forward) → world Z
            glm::vec4(0, 1, 0, 0),   // NIF Z (up)      → world Y
            glm::vec4(0, 0, 0, 1)
        );

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

    // Camera input on the image widget.
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

        // Dirty check — skip if weights unchanged since last apply.
        if (doc.morphWeights == actorCache_[ai].morphWeightsCached) continue;
        actorCache_[ai].morphWeightsCached = doc.morphWeights;

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
                auto it = doc.morphWeights.find(m.name);
                if (it == doc.morphWeights.end() || it->second == 0.f) continue;
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

void ViewportPanel::EvaluatePoses(AppState& state)
{
    if (actorCache_.empty()) return;

    if (!state.sequence.Empty()) {
        std::vector<ActorEval> evals;
        state.sequence.Evaluate(state.time, state, evals);
        for (int ai = 0; ai < (int)actorCache_.size() && ai < (int)evals.size(); ai++) {
            actorCache_[ai].pose = evals[ai].pose;
            actorCache_[ai].pose.SolveFK();
        }
    } else if (state.selectedClip >= 0 && state.selectedClip < (int)state.clips.size()) {
        for (int ai = 0; ai < (int)actorCache_.size(); ai++) {
            actorCache_[ai].pose = actorCache_[ai].refPose;
            state.clips[state.selectedClip].Evaluate(state.time, actorCache_[ai].pose);
            actorCache_[ai].pose.SolveFK();
        }
    } else {
        for (auto& ar : actorCache_) {
            ar.pose = ar.refPose;
            ar.pose.SolveFK();
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
