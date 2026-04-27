#include "ViewportPanel.h"
#include "AppState.h"
#include "NifDocument.h"
#include "Sequence.h"
#include "HavokSkeleton.h"
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
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
    for (auto& ar : actorCache_)
        for (MeshHandle h : ar.meshHandles)
            renderer_.FreeMesh(h);
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
        ImGui::TextDisabled("No actors — add one from Actor Properties");
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

        // NIF space (Z-up, Y-forward) → world Y-up.
        // -90° around X: NIF-Z(up) → world-Y, NIF-Y(forward) → world-(-Z).
        static const glm::mat4 kNifToWorld =
            glm::rotate(glm::mat4(1.f), glm::radians(-90.f), glm::vec3(1.f, 0.f, 0.f));

        for (int ai = 0; ai < (int)actorCache_.size(); ai++) {
            // Static NIF meshes — apply per-mesh toRoot then coordinate conversion.
            const auto& cache = actorCache_[ai];
            for (int mi = 0; mi < (int)cache.meshHandles.size(); mi++) {
                const glm::mat4 model = kNifToWorld * cache.meshTransforms[mi];
                renderer_.DrawMesh(cache.meshHandles[mi], model, { .wireframe = true });
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
    // Free all GPU meshes before resizing — SyncNifHandles will re-upload.
    for (auto& ar : actorCache_) {
        for (MeshHandle h : ar.meshHandles)
            renderer_.FreeMesh(h);
        ar.meshHandles.clear();
        ar.meshTransforms.clear();
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
        const CastEntry* ce = (actor.castIndex >= 0 &&
                               actor.castIndex < (int)state.cast.size())
                            ? &state.cast[actor.castIndex] : nullptr;
        const std::string& nifPath = ce ? ce->nifPath : "";

        if (nifPath == actorCache_[ai].loadedNifPath) continue;

        for (MeshHandle h : actorCache_[ai].meshHandles)
            renderer_.FreeMesh(h);
        actorCache_[ai].meshHandles.clear();
        actorCache_[ai].meshTransforms.clear();
        actorCache_[ai].loadedNifPath = nifPath;

        if (!nifPath.empty()) {
            NifDocument doc = LoadNifDocument(nifPath);
            for (int si = 0; si < (int)doc.shapes.size(); si++) {
                const NifDocShape& ds    = doc.shapes[si];
                const NifBlock&    block = doc.blocks[ds.blockIndex];
                MeshHandle h = renderer_.UploadMesh(ds.meshData);
                actorCache_[ai].meshHandles.push_back(h);
                actorCache_[ai].meshTransforms.push_back(block.toRoot);
            }
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
