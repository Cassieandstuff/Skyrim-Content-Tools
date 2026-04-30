#include "ViewportPanel.h"
#include "AppState.h"
#include "Sequence.h"
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdio>

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
    // env_ destructor calls CancelAndJoin() (cancel + join only, no GPU free).
    // Explicit Free() releases GPU resources while renderer_ is still valid.
    env_.Free(renderer_);
    terrain_.Free(renderer_);
    actors_.Free(renderer_);
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

    // ── Draw-distance slider (exterior cells only) ────────────────────────────
    if (state.loadedCell.loaded && state.loadedCell.isExterior) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.f);
        ImGui::SetNextItemWidth(220.f);
        ImGui::SliderFloat("##cullDist", &cellCullDist_,
                           1024.f, 131072.f, "Draw dist: %.0f u",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine();
        ImGui::TextDisabled("%d / %d instances visible",
                            env_.VisibleCount(camera_.target, cellCullDist_),
                            env_.InstanceCount());
        ImGui::Separator();
    }

    // ── 3D viewport area ──────────────────────────────────────────────────────
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 1.f || size.y < 1.f) { ImGui::End(); return; }

    // ── Per-frame updates ─────────────────────────────────────────────────────

    // Actor cache: rebuild if actor/skeleton list changed.
    if (actors_.NeedsRebuild(state)) {
        actors_.Rebuild(state, renderer_);
        FrameAll();
    }
    actors_.SyncNifHandles(state, renderer_);

    // Terrain: Load() must come BEFORE env_.Load() so any running BulkWorker
    // is cancelled before env_.Free() clears wsAllCells_.
    const auto terrainResult = terrain_.Load(
        state.loadedCell, state.landRecord, state, renderer_, env_);
    if (terrainResult.changed) {
        camera_.target    = glm::vec3(
            (float)(terrainResult.cellX * 4096 + 2048),
            (float)(terrainResult.cellY * 4096 + 2048),
            terrainResult.avgHeight);
        camera_.radius    = 8192.f;
        camera_.azimuth   = 210.f;
        camera_.elevation = 15.f;
    }

    env_.Load(state.loadedCell, state.dataFolder, state.bsaSearchList, renderer_);
    terrain_.Sync(state, renderer_);

    actors_.EvaluatePoses(state);
    actors_.SyncMorphs(state, renderer_);

    // ── Render to FBO ─────────────────────────────────────────────────────────
    const int iw = (int)size.x;
    const int ih = (int)size.y;

    glm::mat4 proj, view;
    if (mode_ == ViewportMode::Cameras) {
        const int shotIdx = state.sequence.EvaluateCameraTrack(state.time);
        if (shotIdx >= 0 && shotIdx < (int)state.cameraShots.size()) {
            const CameraShot& shot = state.cameraShots[shotIdx];
            const float az = glm::radians(shot.yaw);
            const float el = glm::radians(shot.pitch);
            const glm::vec3 lookTarget = shot.eye - glm::vec3(
                std::cos(el) * std::sin(az),
                std::cos(el) * std::cos(az),
                std::sin(el));
            view = glm::lookAt(shot.eye, lookTarget, glm::vec3(0.f, 0.f, 1.f));
            proj = glm::perspective(glm::radians(shot.fov), size.x / size.y,
                                    0.1f, 5'000'000.f);
        } else {
            view = camera_.View();
            proj = camera_.Proj(size.x / size.y);
        }
    } else {
        proj = camera_.Proj(size.x / size.y);
        view = camera_.View();
    }

    env_.Sync(renderer_);

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

    if (mode_ == ViewportMode::Scene || mode_ == ViewportMode::Cameras) {
        if (mode_ == ViewportMode::Scene && !state.loadedCell.isExterior) {
            float unit = 1.f;
            if (camera_.radius > 100.f)      unit = 50.f;
            else if (camera_.radius > 20.f)  unit = 10.f;
            renderer_.DrawGrid(unit, 10);
        }

        terrain_.Draw(renderer_);

        env_.Draw(renderer_, simTime_, cellCullDist_,
                  camera_.target, state.selectedCellRefIndex);

        actors_.Draw(state, renderer_);
    }

    renderer_.EndFrame();

    // Display the FBO colour texture (UV-flipped: OpenGL Y=0 is at bottom).
    const ImVec2 imgMin = ImGui::GetCursorScreenPos();
    ImGui::Image(renderer_.GetOutputTexture(), size,
                 ImVec2(0.f, 1.f), ImVec2(1.f, 0.f));

    // Camera input + left-click picking — only active in Scene mode.
    if (mode_ == ViewportMode::Scene && (ImGui::IsItemHovered() || ImGui::IsItemActive())) {
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
        // Build ray analytically — avoids inverting the VP matrix, which is
        // numerically unstable at Skyrim scale with the 5,000,000/0.1 far/near ratio.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.f)) {
            const float     ndcX  = 2.f * (io.MousePos.x - imgMin.x) / size.x - 1.f;
            const float     ndcY  = 1.f - 2.f * (io.MousePos.y - imgMin.y) / size.y;
            const glm::mat4 vproj = camera_.Proj(size.x / size.y);
            const glm::vec3 eye   = camera_.Eye();
            const glm::vec3 fwd   = glm::normalize(camera_.target - eye);
            const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.f, 0.f, 1.f)));
            const glm::vec3 up    = glm::cross(right, fwd);
            const glm::vec3 rayO  = eye;
            const glm::vec3 rayD  = glm::normalize(right * (ndcX / vproj[0][0])
                                                  + up    * (ndcY / vproj[1][1])
                                                  + fwd);
            state.selectedCellRefIndex = env_.Pick(rayO, rayD);
        }
    }

    // Controller freecam.
    if (mode_ == ViewportMode::Scene)
        controller_.ApplyFreecam(camera_, ImGui::GetIO().DeltaTime);

    // Mirror orbit camera to AppState for the [⊕ Capture] workflow.
    state.viewportEye       = camera_.Eye();
    state.viewportAzimuth   = camera_.azimuth;
    state.viewportElevation = camera_.elevation;

    // ── Viewport overlays ─────────────────────────────────────────────────────
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (mode_ == ViewportMode::Scene) {
            // Axis gizmo — bottom-left corner
            const glm::mat4& v = camera_.View();
            const float gx  = imgMin.x + 45.f;
            const float gy  = imgMin.y + size.y - 45.f;
            const float len = 28.f;

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
            if (env_.IsStreaming() || env_.StreamDone() < env_.StreamTotal()) {
                const int   total = env_.StreamTotal();
                const int   done  = env_.StreamDone();
                const float frac  = (total > 0) ? (float)done / (float)total : 0.f;
                char label[48];
                std::snprintf(label, sizeof(label), "Loading cell...  %d / %d", done, total);
                const ImVec2 lsz = ImGui::CalcTextSize(label);
                const float  barW = size.x * 0.45f;
                const float  barH = 4.f;
                const float  cx   = imgMin.x + size.x * 0.5f;
                const float  py   = imgMin.y + size.y - 22.f;
                dl->AddText({ cx - lsz.x * 0.5f, py - lsz.y - 2.f },
                            IM_COL32(200, 200, 210, 200), label);
                dl->AddRectFilled({ cx - barW * 0.5f, py },
                                  { cx + barW * 0.5f, py + barH },
                                  IM_COL32(40, 42, 50, 200), 2.f);
                dl->AddRectFilled({ cx - barW * 0.5f, py },
                                  { cx - barW * 0.5f + barW * frac, py + barH },
                                  IM_COL32(80, 160, 240, 220), 2.f);
            }
        }
    }

    // Scaffolded mode overlays.
    if (mode_ == ViewportMode::Face || mode_ == ViewportMode::Bones) {
        const char* label = (mode_ == ViewportMode::Face)
            ? "Face View  —  coming soon"
            : "Bone Editor  —  coming soon";
        const ImVec2 p0 = ImGui::GetItemRectMin();
        ImVec2 ts = ImGui::CalcTextSize(label);
        ImGui::GetWindowDrawList()->AddText(
            { p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f },
            IM_COL32(180, 180, 200, 120), label);
    }

    // Cameras mode overlay: frame border + active shot badge.
    if (mode_ == ViewportMode::Cameras) {
        ImDrawList* dl  = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetItemRectMin();
        const ImVec2 p1 = { p0.x + size.x, p0.y + size.y };

        const int shotIdx = state.sequence.EvaluateCameraTrack(state.time);
        if (shotIdx >= 0 && shotIdx < (int)state.cameraShots.size()) {
            dl->AddRect(p0, p1, IM_COL32(51, 153, 115, 160), 0.f, 0, 2.f);
            const char* sname = state.cameraShots[shotIdx].name.c_str();
            const ImVec2 ts   = ImGui::CalcTextSize(sname);
            const float  bx   = p0.x + 10.f;
            const float  by   = p1.y - ts.y - 14.f;
            dl->AddRectFilled({bx - 5, by - 3}, {bx + ts.x + 5, by + ts.y + 3},
                              IM_COL32(8, 10, 16, 200), 3.f);
            dl->AddText({bx, by}, IM_COL32(51, 200, 140, 230), sname);
        } else {
            const char* hint = "No camera shot active";
            const ImVec2 ts  = ImGui::CalcTextSize(hint);
            dl->AddText({ p0.x + (size.x - ts.x) * 0.5f,
                          p0.y + (size.y - ts.y) * 0.5f },
                        IM_COL32(130, 140, 160, 140), hint);
        }
    }

    ImGui::End();
}

// ── FrameAll ──────────────────────────────────────────────────────────────────

void ViewportPanel::FrameAll()
{
    glm::vec3 mn, mx;
    if (!actors_.RefPoseBounds(mn, mx)) return;
    camera_.target    = (mn + mx) * 0.5f;
    camera_.radius    = glm::length(mx - mn) * 1.3f;
    camera_.azimuth   = 210.f;
    camera_.elevation = 10.f;
    fprintf(stderr, "[Viewport] FrameAll: mn=(%.2f,%.2f,%.2f) mx=(%.2f,%.2f,%.2f) radius=%.2f\n",
            mn.x, mn.y, mn.z, mx.x, mx.y, mx.z, camera_.radius);
}
