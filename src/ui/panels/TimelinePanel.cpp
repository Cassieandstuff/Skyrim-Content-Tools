#include "TimelinePanel.h"
#include "app/AppState.h"
#include "anim/FaceClip.h"
#include "anim/Sequence.h"
#include "ui/TrackRegistry.h"
#include "ui/StylePalette.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace SCT;

// ── Layout constants ──────────────────────────────────────────────────────────
static constexpr float kRulerH       = 20.f;
static constexpr float kActorHeaderH = 22.f;
static constexpr float kLaneH        = 26.f;
static constexpr float kEdgeHitW     = 6.f;
static constexpr float kMinItemW     = 4.f;

// ── IPanel::Draw ──────────────────────────────────────────────────────────────

void TimelinePanel::Draw(AppState& state)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(PanelID());
    ImGui::PopStyleVar();

    // ── Transport bar ─────────────────────────────────────────────────────────
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 3));
    DrawTransportBar(state);
    ImGui::PopStyleVar();
    ImGui::Separator();

    // ── Available area for ruler + tracks ────────────────────────────────────
    const ImVec2 p0   = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 4.f || avail.y < 4.f) { ImGui::End(); return; }

    const float trackX = p0.x + headerW_;
    const float trackW = avail.x - headerW_;
    const float rulerY = p0.y;
    const float areaY  = p0.y + kRulerH;
    const float areaH  = avail.y - kRulerH;

    // Full invisible hit region for drag/click handling.
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##tl_area", avail,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

    // ── Clip drag-drop target ─────────────────────────────────────────────────
    // Must be registered immediately after the InvisibleButton (before any other
    // items are drawn) so g.LastItemData still refers to ##tl_area.
    // We save the mouse position here and resolve the actor lane after
    // DrawTrackArea has built the lane geometry.
    int    pendingDropClip     = -1;
    ImVec2 pendingDropPos      = {};
    int    pendingDropFaceClip = -1;
    ImVec2 pendingDropFacePos  = {};
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("SCT_CLIP")) {
            IM_ASSERT(pay->DataSize == sizeof(int));
            pendingDropClip = *(const int*)pay->Data;
            pendingDropPos  = ImGui::GetIO().MousePos;
        }
        if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("SCT_FACE_CLIP")) {
            IM_ASSERT(pay->DataSize == sizeof(int));
            pendingDropFaceClip = *(const int*)pay->Data;
            pendingDropFacePos  = ImGui::GetIO().MousePos;
        }
        ImGui::EndDragDropTarget();
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, {p0.x + avail.x, p0.y + avail.y}, true);

    // Background
    dl->AddRectFilled(p0, {p0.x + avail.x, p0.y + avail.y}, kColBg);

    // Ruler
    DrawRuler(state, trackX, trackW, rulerY, kRulerH);

    // Track area
    std::vector<LaneCtx> lanes;
    DrawTrackArea(state, trackX, trackW, areaY, areaH, lanes);

    // Scrubber overlaid on everything
    DrawScrubber(state, trackX, trackW, areaY, areaH, rulerY);

    dl->PopClipRect();

    // Process any clip drop now that lane geometry is available.
    if (pendingDropClip >= 0)
        HandleDrop(state, pendingDropClip, pendingDropPos, trackX, trackW, lanes);
    if (pendingDropFaceClip >= 0)
        HandleFaceDrop(state, pendingDropFaceClip, pendingDropFacePos, trackX, trackW, lanes);

    // Input handling (after draw so we have lane geometry)
    HandleInput(state, trackX, trackW, areaY, areaH, rulerY, lanes);

    ImGui::End();
}

// ── Transport bar ─────────────────────────────────────────────────────────────

void TimelinePanel::DrawTransportBar(AppState& state)
{
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);

    // |<  step-back  play/pause  step-forward  >|
    if (ImGui::Button("|<")) { state.time = 0.f; state.playing = false; }
    ImGui::SameLine(0, 2);
    if (ImGui::Button("<|")) {
        state.time    = std::max(0.f, state.time - 1.f / 30.f);
        state.playing = false;
    }
    ImGui::SameLine(0, 2);
    if (ImGui::Button(state.playing ? " \xe2\x80\x96 " : "  \xe2\x96\xb6  "))  // ‖ / ▶
        state.playing ^= true;
    ImGui::SameLine(0, 2);
    if (ImGui::Button("|>")) {
        const float seqDur = state.sequence.Duration();
        const float clipDur = (state.selectedClip >= 0 &&
                               state.selectedClip < (int)state.clips.size())
            ? state.clips[state.selectedClip].duration : 0.f;
        const float maxT = seqDur > 0.f ? seqDur : clipDur;
        state.time    = state.time + 1.f / 30.f;
        if (maxT > 0.f) state.time = std::min(state.time, maxT);
        state.playing = false;
    }
    ImGui::SameLine(0, 2);
    if (ImGui::Button(">|")) {
        const float dur = state.sequence.Duration();
        state.time    = dur > 0.f ? dur : 0.f;
        state.playing = false;
    }
    ImGui::SameLine(0, 10);

    const float dur = state.sequence.Duration() > 0.f
        ? state.sequence.Duration()
        : (state.selectedClip >= 0 && state.selectedClip < (int)state.clips.size()
           ? state.clips[state.selectedClip].duration : 0.f);

    if (dur > 0.f)
        ImGui::Text("%.2f / %.2f s", state.time, dur);
    else
        ImGui::TextDisabled("0.00 s");

    ImGui::SameLine(0, 12);
    // Loop toggle button (highlighted when active).
    // Capture state BEFORE the click so push/pop are always balanced.
    const bool loopWas = state.loop;
    if (loopWas)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.38f, 0.65f, 1.f));
    if (ImGui::Button("LOOP")) state.loop ^= true;
    if (loopWas)
        ImGui::PopStyleColor();

    // Zoom controls on the right
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 110.f + ImGui::GetCursorPosX() - 4.f);
    ImGui::TextDisabled("Zoom:");
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(80.f);
    ImGui::SliderFloat("##zoom", &pxPerSec_, 10.f, 400.f, "%.0f px/s",
                       ImGuiSliderFlags_Logarithmic);
}

// ── Ruler ─────────────────────────────────────────────────────────────────────

void TimelinePanel::DrawRuler(AppState& state, float trackX, float trackW,
                               float rulerY, float rulerH)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Header background
    dl->AddRectFilled({trackX - headerW_, rulerY}, {trackX + trackW, rulerY + rulerH},
                      kColRuler);
    dl->AddLine({trackX - headerW_, rulerY + rulerH},
                {trackX + trackW,   rulerY + rulerH}, kColSep);

    if (trackW <= 0.f) return;

    // Adaptive tick spacing
    float tickStep = 0.1f;
    if (pxPerSec_ < 8.f)   tickStep = 10.f;
    else if (pxPerSec_ < 20.f)  tickStep = 5.f;
    else if (pxPerSec_ < 50.f)  tickStep = 1.f;
    else if (pxPerSec_ < 120.f) tickStep = 0.5f;

    const float seqStart = scrollX_ / pxPerSec_;
    const float seqEnd   = (scrollX_ + trackW) / pxPerSec_;

    const float firstTick = std::floor(seqStart / tickStep) * tickStep;
    for (float s = firstTick; s <= seqEnd + tickStep * 0.5f; s += tickStep) {
        const float x = SeqToX(s, trackX);
        if (x < trackX || x > trackX + trackW) continue;

        dl->AddLine({x, rulerY}, {x, rulerY + rulerH}, IM_COL32(50, 60, 80, 180));
        char buf[16]; std::snprintf(buf, sizeof(buf), "%.1fs", s);
        dl->AddText({x + 2, rulerY + 3}, kColTextDim, buf);
    }

    // "Add to Timeline" prompt in header area when there are clips but no sequence items
    if (!state.clips.empty() && state.sequence.Empty() && state.actors.empty()) {
        const char* hint = "Load a skeleton, then drag clips here";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText({trackX + (trackW - ts.x) * 0.5f, rulerY + (rulerH - ts.y) * 0.5f},
                    kColTextDim, hint);
    }
}

// ── Track area ────────────────────────────────────────────────────────────────

void TimelinePanel::DrawTrackArea(AppState& state, float trackX, float trackW,
                                   float areaY, float areaH,
                                   std::vector<LaneCtx>& lanesOut)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float curY = areaY;

    if (state.sequence.actorTracks.empty() && state.sequence.sceneTracks.empty()) {
        // Empty state hint
        const char* msg = state.actors.empty()
            ? "Add actors from the Workflow tab, then drag clips here"
            : "Actor added — drag a clip from the Bin to place it";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText({trackX + (trackW - ts.x) * 0.5f, areaY + (areaH - ts.y) * 0.5f},
                    kColTextDim, msg);
        return;
    }

    // Actor track groups
    for (const auto& group : state.sequence.actorTracks)
        DrawActorGroup(state, group.actorIndex, trackX, trackW, curY, lanesOut);

    // Scene tracks (Camera, Audio, …) — separator then scene lanes
    if (!state.sequence.sceneTracks.empty()) {
        dl->AddLine({trackX - headerW_, curY}, {trackX + trackW, curY},
                    IM_COL32(60, 65, 80, 200), 1.f);
        // "Scene" header
        dl->AddRectFilled({trackX - headerW_, curY},
                          {trackX + trackW, curY + kActorHeaderH}, kColHeader);
        dl->AddText({trackX - headerW_ + 8, curY + 4}, kColText, "Scene");
        curY += kActorHeaderH;

        for (int li = 0; li < (int)state.sequence.sceneTracks.size(); li++) {
            DrawLane(state, -1, li, trackX, trackW, curY, kLaneH, lanesOut);
            curY += kLaneH;
        }
    }
}

void TimelinePanel::DrawActorGroup(AppState& state, int actorIdx,
                                    float trackX, float trackW, float& curY,
                                    std::vector<LaneCtx>& lanesOut)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Find the group
    ActorTrackGroup* grp = nullptr;
    for (auto& g : state.sequence.actorTracks)
        if (g.actorIndex == actorIdx) { grp = &g; break; }
    if (!grp) return;

    // Actor name
    const char* actorName = "Actor";
    if (actorIdx >= 0 && actorIdx < (int)state.actors.size()) {
        const int ci = state.actors[actorIdx].castIndex;
        if (ci >= 0 && ci < (int)state.cast.size())
            actorName = state.cast[ci].name.c_str();
    }

    // Group header row (clickable to collapse/expand)
    const float hx0 = trackX - headerW_;
    dl->AddRectFilled({hx0, curY}, {trackX + trackW, curY + kActorHeaderH}, kColHeader);
    dl->AddLine({hx0, curY + kActorHeaderH}, {trackX + trackW, curY + kActorHeaderH}, kColSep);

    // Collapse triangle
    const char* tri = grp->collapsed ? "\xe2\x96\xb6" : "\xe2\x96\xbc";  // ▶ / ▼
    dl->AddText({hx0 + 4, curY + 4}, kColText, tri);
    dl->AddText({hx0 + 18, curY + 4}, kColText, actorName);

    // Click to toggle collapse
    ImGui::SetCursorScreenPos({hx0, curY});
    char collapseId[32]; std::snprintf(collapseId, sizeof(collapseId), "##ahdr_%d", actorIdx);
    if (ImGui::InvisibleButton(collapseId, {trackX + trackW - hx0, kActorHeaderH}))
        grp->collapsed = !grp->collapsed;

    curY += kActorHeaderH;
    if (grp->collapsed) return;

    // Lane rows
    for (int li = 0; li < (int)grp->lanes.size(); li++) {
        DrawLane(state, actorIdx, li, trackX, trackW, curY, kLaneH, lanesOut);
        curY += kLaneH;
    }
}

void TimelinePanel::DrawLane(AppState& state, int actorIdx, int laneIdx,
                              float trackX, float trackW, float laneY, float laneH,
                              std::vector<LaneCtx>& lanesOut)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Identify lane
    TrackLane* lane = nullptr;
    if (actorIdx >= 0) {
        for (auto& g : state.sequence.actorTracks)
            if (g.actorIndex == actorIdx && laneIdx < (int)g.lanes.size())
                { lane = &g.lanes[laneIdx]; break; }
    } else if (laneIdx < (int)state.sequence.sceneTracks.size()) {
        lane = &state.sequence.sceneTracks[laneIdx];
    }
    if (!lane) return;

    const TrackTypeDef* def = TrackRegistry::Get().Find(lane->type);
    const char* laneLabel = def ? def->label : "?";

    // Check for an active drag — drives visual feedback below.
    const ImGuiPayload* dragPay        = ImGui::GetDragDropPayload();
    const bool          isDragClip     = (dragPay && dragPay->IsDataType("SCT_CLIP"));
    const bool          isDragFaceClip = (dragPay && dragPay->IsDataType("SCT_FACE_CLIP"));

    // Record for hit testing
    lanesOut.push_back({ actorIdx, laneIdx, laneY, laneY + laneH });

    // Lane background (alternating)
    const ImU32 bgCol = ((lanesOut.size() % 2) == 0) ? kColLane : kColLaneAlt;
    dl->AddRectFilled({trackX - headerW_, laneY}, {trackX + trackW, laneY + laneH}, bgCol);
    dl->AddLine({trackX - headerW_, laneY + laneH}, {trackX + trackW, laneY + laneH}, kColSep);

    // Drag-over highlight: blue/green = compatible, red = wrong lane type.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (trackW > 0.f && mouse.y >= laneY && mouse.y < laneY + laneH) {
        if (isDragClip) {
            if (lane->type == TrackType::AnimClip) {
                // Resolve skeleton types (value copies — strings are short).
                const int dci = *(const int*)dragPay->Data;
                std::string clipType, actorType;
                if (dci >= 0 && dci < (int)state.clips.size())
                    clipType = state.clips[dci].skeletonType;
                if (actorIdx >= 0 && actorIdx < (int)state.actors.size()) {
                    const int ci = state.actors[actorIdx].castIndex;
                    if (ci >= 0 && ci < (int)state.cast.size())
                        actorType = state.cast[ci].creatureType;
                }
                const bool typeOk = clipType.empty() || actorType.empty() || clipType == actorType;

                if (typeOk) {
                    dl->AddRectFilled({trackX, laneY + 1}, {trackX + trackW, laneY + laneH - 1},
                                      IM_COL32(80, 150, 230, 40));
                    dl->AddRect      ({trackX, laneY + 1}, {trackX + trackW, laneY + laneH - 1},
                                      IM_COL32(100, 180, 255, 110), 0.f, 0, 1.5f);
                } else {
                    dl->AddRectFilled({trackX, laneY + 1}, {trackX + trackW, laneY + laneH - 1},
                                      IM_COL32(200, 130, 40, 35));
                    dl->AddRect      ({trackX, laneY + 1}, {trackX + trackW, laneY + laneH - 1},
                                      IM_COL32(230, 160, 60, 120), 0.f, 0, 1.5f);
                    const char* warn = "wrong type";
                    const ImVec2 ts  = ImGui::CalcTextSize(warn);
                    dl->AddText({trackX + (trackW - ts.x) * 0.5f,
                                 laneY  + (laneH  - ts.y) * 0.5f},
                                IM_COL32(230, 160, 60, 180), warn);
                }
            } else {
                dl->AddRectFilled({trackX, laneY + 1}, {trackX + trackW, laneY + laneH - 1},
                                  IM_COL32(180, 60, 60, 22));
            }
        } else if (isDragFaceClip) {
            if (lane->type == TrackType::FaceData) {
                // Green tint — face clips only land on FaceData lanes.
                dl->AddRectFilled({trackX, laneY + 1}, {trackX + trackW, laneY + laneH - 1},
                                  IM_COL32(80, 200, 130, 40));
                dl->AddRect      ({trackX, laneY + 1}, {trackX + trackW, laneY + laneH - 1},
                                  IM_COL32(120, 230, 160, 110), 0.f, 0, 1.5f);
            } else {
                dl->AddRectFilled({trackX, laneY + 1}, {trackX + trackW, laneY + laneH - 1},
                                  IM_COL32(180, 60, 60, 22));
            }
        }
    }

    // Lane label in header column
    dl->AddText({trackX - headerW_ + 22, laneY + (laneH - ImGui::GetTextLineHeight()) * 0.5f},
                kColTextDim, laneLabel);

    // Scaffolded indicator for unimplemented lane types (excludes lanes that
    // accept real items even if their evaluate callback isn't wired yet).
    const bool acceptsItems = (lane->type == TrackType::AnimClip ||
                               lane->type == TrackType::FaceData ||
                               lane->type == TrackType::Camera);
    if (def && !def->evaluate && !acceptsItems) {
        char badge[32]; std::snprintf(badge, sizeof(badge), "[%s]", laneLabel);
        const float bx = trackX + 4;
        dl->AddText({bx, laneY + (laneH - ImGui::GetTextLineHeight()) * 0.5f},
                    IM_COL32(100, 110, 130, 100), badge);
    }

    // [⊕ Shot] button in the Camera lane header — captures viewport camera as a
    // new shot and places it at state.time on the Camera lane.
    if (lane->type == TrackType::Camera) {
        const float btnH = ImGui::GetFrameHeight();
        const float btnX = trackX - 62.f;
        const float btnY = laneY + (laneH - btnH) * 0.5f;
        ImGui::SetCursorScreenPos({btnX, btnY});
        if (ImGui::SmallButton("\xe2\x8a\x95 Shot##cam_cap")) {  // ⊕
            CameraShot shot;
            shot.name  = "Shot " + std::to_string(state.cameraShots.size() + 1);
            shot.eye   = state.viewportEye;
            shot.yaw   = state.viewportAzimuth;
            shot.pitch = state.viewportElevation;
            state.cameraShots.push_back(shot);
            const int shotIdx = (int)state.cameraShots.size() - 1;

            SequenceItem item;
            item.assetIndex = shotIdx;
            item.seqStart   = state.time;
            item.trimIn     = 0.f;
            item.trimOut    = 5.f;
            lane->items.push_back(item);

            state.selectedShotIndex    = shotIdx;
            state.selectedCast         = -1;
            state.selectedCellRefIndex = -1;
            state.projectDirty         = true;
        }
    }

    if (trackW <= 0.f) return;

    // Draw items
    const ImVec4 itemColor = def ? def->color : ImVec4(0.4f, 0.4f, 0.5f, 1.f);
    const ImU32  itemCol   = ImGui::ColorConvertFloat4ToU32(itemColor);
    const ImU32  itemColDim = IM_COL32(
        (ImU8)(itemColor.x * 180), (ImU8)(itemColor.y * 180),
        (ImU8)(itemColor.z * 180), 200);

    for (const auto& item : lane->items) {
        const float x0 = SeqToX(item.seqStart, trackX);
        const float x1 = SeqToX(item.SeqEnd(),  trackX);
        if (x1 < trackX || x0 > trackX + trackW) continue;  // off-screen

        const float cx0 = std::max(x0, trackX);
        const float cx1 = std::min(x1, trackX + trackW);
        if (cx1 - cx0 < 0.5f) continue;

        // Item body
        dl->AddRectFilled({cx0, laneY + 2}, {cx1, laneY + laneH - 2}, itemColDim, 3.f);
        dl->AddRect       ({cx0, laneY + 2}, {cx1, laneY + laneH - 2}, itemCol,    3.f, 0, 1.f);

        // Blend ramps (visual only — darker shade over blendIn/Out regions)
        if (item.blendIn > 0.f) {
            const float bx1 = std::min(SeqToX(item.seqStart + item.blendIn, trackX), cx1);
            if (bx1 > cx0)
                dl->AddRectFilled({cx0, laneY + 2}, {bx1, laneY + laneH - 2},
                                  IM_COL32(0, 0, 0, 80), 3.f);
        }
        if (item.blendOut > 0.f) {
            const float bx0o = std::max(SeqToX(item.SeqEnd() - item.blendOut, trackX), cx0);
            if (bx0o < cx1)
                dl->AddRectFilled({bx0o, laneY + 2}, {cx1, laneY + laneH - 2},
                                  IM_COL32(0, 0, 0, 80), 3.f);
        }

        // Item label — resolve name from the appropriate asset pool.
        const char* cname = nullptr;
        if (lane->type == TrackType::AnimClip &&
            item.assetIndex >= 0 && item.assetIndex < (int)state.clips.size())
            cname = state.clips[item.assetIndex].name.c_str();
        else if (lane->type == TrackType::FaceData &&
                 item.assetIndex >= 0 && item.assetIndex < (int)state.faceClips.size())
            cname = state.faceClips[item.assetIndex].name.c_str();
        else if (lane->type == TrackType::Camera &&
                 item.assetIndex >= 0 && item.assetIndex < (int)state.cameraShots.size())
            cname = state.cameraShots[item.assetIndex].name.c_str();
        if (cname) {
            ImVec2 ts = ImGui::CalcTextSize(cname);
            if (ts.x + 8.f <= cx1 - cx0)
                dl->AddText({cx0 + 4, laneY + (laneH - ts.y) * 0.5f},
                            IM_COL32(210, 220, 240, 220), cname);
        }

        // Trim handles (visible on hover — draw as bright edge lines)
        // Full handles always visible at full width; at narrow widths, skip.
        if (x1 - x0 >= kEdgeHitW * 3.f) {
            dl->AddLine({x0, laneY + 3}, {x0, laneY + laneH - 3},
                        IM_COL32(255, 255, 255, 160), 2.f);
            dl->AddLine({x1, laneY + 3}, {x1, laneY + laneH - 3},
                        IM_COL32(255, 255, 255, 160), 2.f);
        }
    }

    // Ghost preview while dragging over a compatible lane.
    if (mouse.y >= laneY && mouse.y < laneY + laneH) {
        if (isDragClip && lane->type == TrackType::AnimClip) {
            const int dci = *(const int*)dragPay->Data;
            std::string clipType, actorType2;
            if (dci >= 0 && dci < (int)state.clips.size())
                clipType = state.clips[dci].skeletonType;
            if (actorIdx >= 0 && actorIdx < (int)state.actors.size()) {
                const int ci = state.actors[actorIdx].castIndex;
                if (ci >= 0 && ci < (int)state.cast.size())
                    actorType2 = state.cast[ci].creatureType;
            }
            const bool typeOk = clipType.empty() || actorType2.empty() || clipType == actorType2;
            if (dci >= 0 && dci < (int)state.clips.size() && typeOk) {
                const float gt  = std::max(0.f, XToSeq(mouse.x, trackX));
                const float gx0 = std::max(SeqToX(gt,                                    trackX), trackX);
                const float gx1 = std::min(SeqToX(gt + state.clips[dci].duration, trackX),
                                            trackX + trackW);
                if (gx1 > gx0) {
                    dl->AddRectFilled({gx0, laneY + 2}, {gx1, laneY + laneH - 2},
                                      IM_COL32(100, 170, 255,  80), 3.f);
                    dl->AddRect      ({gx0, laneY + 2}, {gx1, laneY + laneH - 2},
                                      IM_COL32(160, 210, 255, 220), 3.f, 0, 1.5f);
                    const char*  gname = state.clips[dci].name.c_str();
                    const ImVec2 gts   = ImGui::CalcTextSize(gname);
                    if (gts.x + 8.f <= gx1 - gx0)
                        dl->AddText({gx0 + 4.f, laneY + (laneH - gts.y) * 0.5f},
                                    IM_COL32(200, 230, 255, 200), gname);
                }
            }
        } else if (isDragFaceClip && lane->type == TrackType::FaceData) {
            const int dfi = *(const int*)dragPay->Data;
            if (dfi >= 0 && dfi < (int)state.faceClips.size()) {
                const float gt  = std::max(0.f, XToSeq(mouse.x, trackX));
                const float gx0 = std::max(SeqToX(gt,                                        trackX), trackX);
                const float gx1 = std::min(SeqToX(gt + state.faceClips[dfi].duration, trackX),
                                            trackX + trackW);
                if (gx1 > gx0) {
                    dl->AddRectFilled({gx0, laneY + 2}, {gx1, laneY + laneH - 2},
                                      IM_COL32( 80, 210, 140,  80), 3.f);
                    dl->AddRect      ({gx0, laneY + 2}, {gx1, laneY + laneH - 2},
                                      IM_COL32(140, 240, 180, 220), 3.f, 0, 1.5f);
                    const char*  gname = state.faceClips[dfi].name.c_str();
                    const ImVec2 gts   = ImGui::CalcTextSize(gname);
                    if (gts.x + 8.f <= gx1 - gx0)
                        dl->AddText({gx0 + 4.f, laneY + (laneH - gts.y) * 0.5f},
                                    IM_COL32(200, 245, 220, 200), gname);
                }
            }
        }
    }
}

// ── Scrubber ──────────────────────────────────────────────────────────────────

void TimelinePanel::DrawScrubber(AppState& state, float trackX, float trackW,
                                  float areaY, float areaH, float rulerY)
{
    const float dur = state.sequence.Duration() > 0.f
        ? state.sequence.Duration()
        : (state.selectedClip >= 0 && state.selectedClip < (int)state.clips.size()
           ? state.clips[state.selectedClip].duration : 0.f);

    if (dur <= 0.f) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float phx = SeqToX(state.time, trackX);
    if (phx < trackX || phx > trackX + trackW) return;

    // Vertical line through the full height
    dl->AddLine({phx, rulerY}, {phx, rulerY + kRulerH + areaH}, kColScrubber, 1.5f);

    // Triangle head in ruler (grabbable)
    dl->AddTriangleFilled({phx - 5, rulerY},
                          {phx + 5, rulerY},
                          {phx,     rulerY + 10}, kColScrubber);
}

// ── Input handling ────────────────────────────────────────────────────────────

void TimelinePanel::HandleInput(AppState& state,
                                 float trackX, float trackW,
                                 float areaY,  float areaH,
                                 float rulerY,
                                 const std::vector<LaneCtx>& lanes)
{
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const float  dur   = state.sequence.Duration() > 0.f
        ? state.sequence.Duration()
        : (state.selectedClip >= 0 && state.selectedClip < (int)state.clips.size()
           ? state.clips[state.selectedClip].duration : 0.f);

    // ── Mouse wheel zoom ──────────────────────────────────────────────────────
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.f && io.KeyCtrl) {
        const float tAtMouse = XToSeq(mouse.x, trackX);
        pxPerSec_ *= std::pow(1.15f, io.MouseWheel);
        pxPerSec_ = std::clamp(pxPerSec_, 5.f, 2000.f);
        // Keep the time-under-mouse stationary
        scrollX_ = (tAtMouse * pxPerSec_) - (mouse.x - trackX);
        scrollX_ = std::max(0.f, scrollX_);
    }

    // ── Horizontal scroll ─────────────────────────────────────────────────────
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.f && !io.KeyCtrl) {
        scrollX_ -= io.MouseWheel * 40.f;
        scrollX_ = std::max(0.f, scrollX_);
    }

    // ── Begin drag ───────────────────────────────────────────────────────────
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsItemActive()) {
        dragTarget_ = DragTarget::None;

        // Did we click in the ruler or on the scrubber head?
        const bool inRuler = (mouse.y >= rulerY && mouse.y <= rulerY + kRulerH);
        const float phx = SeqToX(state.time, trackX);
        const bool onScrubHead = inRuler && std::abs(mouse.x - phx) <= 8.f;

        if (onScrubHead || inRuler) {
            dragTarget_ = DragTarget::Scrubber;
            if (dur > 0.f)
                state.time = std::clamp(XToSeq(mouse.x, trackX), 0.f, dur);
        } else {
            // Check lanes for item hit
            for (const auto& lc : lanes) {
                if (mouse.y < lc.yTop || mouse.y > lc.yBot) continue;

                TrackLane* lane = nullptr;
                if (lc.actorIdx >= 0) {
                    for (auto& g : state.sequence.actorTracks)
                        if (g.actorIndex == lc.actorIdx && lc.laneIdx < (int)g.lanes.size())
                            { lane = &g.lanes[lc.laneIdx]; break; }
                } else if (lc.laneIdx < (int)state.sequence.sceneTracks.size()) {
                    lane = &state.sequence.sceneTracks[lc.laneIdx];
                }
                if (!lane) continue;

                for (int ii = 0; ii < (int)lane->items.size(); ii++) {
                    const auto& item = lane->items[ii];
                    const float x0 = SeqToX(item.seqStart, trackX);
                    const float x1 = SeqToX(item.SeqEnd(),  trackX);

                    if (mouse.x >= x0 && mouse.x <= x1) {
                        dragLaneActor_ = lc.actorIdx;
                        dragLaneIdx_   = lc.laneIdx;
                        dragItemIdx_   = ii;

                        if (mouse.x <= x0 + kEdgeHitW)
                            dragTarget_ = DragTarget::ItemLeft;
                        else if (mouse.x >= x1 - kEdgeHitW)
                            dragTarget_ = DragTarget::ItemRight;
                        else {
                            dragTarget_   = DragTarget::ItemBody;
                            dragOffsetSec_ = XToSeq(mouse.x, trackX) - item.seqStart;
                        }

                        // Select shot when clicking a Camera lane item.
                        if (lane->type == TrackType::Camera) {
                            state.selectedShotIndex    = item.assetIndex;
                            state.selectedCast         = -1;
                            state.selectedCellRefIndex = -1;
                        }
                        break;
                    }
                }
                if (dragTarget_ != DragTarget::None) break;
            }
        }
    }

    // ── Continue drag ─────────────────────────────────────────────────────────
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && dragTarget_ != DragTarget::None) {
        const float tMouse = XToSeq(mouse.x, trackX);

        if (dragTarget_ == DragTarget::Scrubber && dur > 0.f) {
            state.time = std::clamp(tMouse, 0.f, dur);
        } else {
            TrackLane* lane = nullptr;
            if (dragLaneActor_ >= 0) {
                for (auto& g : state.sequence.actorTracks)
                    if (g.actorIndex == dragLaneActor_ && dragLaneIdx_ < (int)g.lanes.size())
                        { lane = &g.lanes[dragLaneIdx_]; break; }
            } else if (dragLaneIdx_ < (int)state.sequence.sceneTracks.size()) {
                lane = &state.sequence.sceneTracks[dragLaneIdx_];
            }
            if (lane && dragItemIdx_ < (int)lane->items.size()) {
                auto& item = lane->items[dragItemIdx_];

                if (dragTarget_ == DragTarget::ItemBody) {
                    item.seqStart = std::max(0.f, tMouse - dragOffsetSec_);
                } else if (dragTarget_ == DragTarget::ItemLeft) {
                    const float newTrimIn = item.trimIn + (tMouse - item.seqStart);
                    if (newTrimIn < item.trimOut - 0.05f) {
                        item.seqStart = std::max(0.f, tMouse);
                        item.trimIn   = std::clamp(newTrimIn, 0.f, item.trimOut - 0.05f);
                    }
                } else if (dragTarget_ == DragTarget::ItemRight) {
                    const float sourceT = tMouse - item.seqStart + item.trimIn;
                    if (lane->type == TrackType::Camera) {
                        // Camera shots have no source-asset duration limit — resize freely.
                        item.trimOut = std::max(sourceT, item.trimIn + 0.05f);
                    } else {
                        float maxOut = 0.f;
                        if (lane->type == TrackType::AnimClip &&
                            item.assetIndex >= 0 && item.assetIndex < (int)state.clips.size())
                            maxOut = state.clips[item.assetIndex].duration;
                        else if (lane->type == TrackType::FaceData &&
                                 item.assetIndex >= 0 && item.assetIndex < (int)state.faceClips.size())
                            maxOut = state.faceClips[item.assetIndex].duration;
                        if (maxOut > 0.f)
                            item.trimOut = std::clamp(sourceT, item.trimIn + 0.05f, maxOut);
                    }
                }
            }
        }
    }

    // ── End drag ──────────────────────────────────────────────────────────────
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        dragTarget_ = DragTarget::None;

    // ── Right-click context menu ──────────────────────────────────────────────
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        for (const auto& lc : lanes) {
            if (mouse.y < lc.yTop || mouse.y > lc.yBot) continue;

            TrackLane* lane = nullptr;
            if (lc.actorIdx >= 0) {
                for (auto& g : state.sequence.actorTracks)
                    if (g.actorIndex == lc.actorIdx && lc.laneIdx < (int)g.lanes.size())
                        { lane = &g.lanes[lc.laneIdx]; break; }
            } else if (lc.laneIdx < (int)state.sequence.sceneTracks.size()) {
                lane = &state.sequence.sceneTracks[lc.laneIdx];
            }
            if (!lane) continue;

            for (int ii = 0; ii < (int)lane->items.size(); ii++) {
                const auto& item = lane->items[ii];
                const float x0 = SeqToX(item.seqStart, trackX);
                const float x1 = SeqToX(item.SeqEnd(),  trackX);
                if (mouse.x >= x0 && mouse.x <= x1) {
                    // Open context menu next frame
                    ImGui::OpenPopup("##item_ctx");
                    dragLaneActor_ = lc.actorIdx;
                    dragLaneIdx_   = lc.laneIdx;
                    dragItemIdx_   = ii;
                    break;
                }
            }
        }
    }

    if (ImGui::BeginPopup("##item_ctx")) {
        TrackLane* lane = nullptr;
        if (dragLaneActor_ >= 0) {
            for (auto& g : state.sequence.actorTracks)
                if (g.actorIndex == dragLaneActor_ && dragLaneIdx_ < (int)g.lanes.size())
                    { lane = &g.lanes[dragLaneIdx_]; break; }
        } else if (dragLaneIdx_ >= 0 && dragLaneIdx_ < (int)state.sequence.sceneTracks.size()) {
            lane = &state.sequence.sceneTracks[dragLaneIdx_];
        }

        if (lane && dragItemIdx_ >= 0 && dragItemIdx_ < (int)lane->items.size()) {
            const auto& item = lane->items[dragItemIdx_];
            const char* itemName = nullptr;
            if (lane->type == TrackType::AnimClip &&
                item.assetIndex >= 0 && item.assetIndex < (int)state.clips.size())
                itemName = state.clips[item.assetIndex].name.c_str();
            else if (lane->type == TrackType::FaceData &&
                     item.assetIndex >= 0 && item.assetIndex < (int)state.faceClips.size())
                itemName = state.faceClips[item.assetIndex].name.c_str();
            else if (lane->type == TrackType::Camera &&
                     item.assetIndex >= 0 && item.assetIndex < (int)state.cameraShots.size())
                itemName = state.cameraShots[item.assetIndex].name.c_str();
            if (itemName) ImGui::TextDisabled("%s", itemName);
            ImGui::Separator();

            // Camera-specific context actions.
            if (lane->type == TrackType::Camera &&
                item.assetIndex >= 0 && item.assetIndex < (int)state.cameraShots.size()) {
                if (ImGui::MenuItem("Set from Current Camera")) {
                    CameraShot& shot = state.cameraShots[item.assetIndex];
                    shot.eye   = state.viewportEye;
                    shot.yaw   = state.viewportAzimuth;
                    shot.pitch = state.viewportElevation;
                    state.projectDirty = true;
                }
            }

            if (ImGui::MenuItem("Remove")) {
                lane->items.erase(lane->items.begin() + dragItemIdx_);
                dragItemIdx_ = -1;
            }
        }
        ImGui::EndPopup();
    }
}

// ── Face clip drag-drop placement ────────────────────────────────────────────

void TimelinePanel::HandleFaceDrop(AppState& state, int faceClipIdx, ImVec2 dropPos,
                                    float trackX, float trackW,
                                    const std::vector<LaneCtx>& lanes)
{
    if (faceClipIdx < 0 || faceClipIdx >= (int)state.faceClips.size()) return;

    for (const auto& lc : lanes) {
        if (dropPos.y < lc.yTop || dropPos.y >= lc.yBot) continue;

        TrackLane* lane = nullptr;
        if (lc.actorIdx >= 0) {
            for (auto& g : state.sequence.actorTracks)
                if (g.actorIndex == lc.actorIdx && lc.laneIdx < (int)g.lanes.size()) {
                    lane = &g.lanes[lc.laneIdx];
                    break;
                }
        } else if (lc.laneIdx < (int)state.sequence.sceneTracks.size()) {
            lane = &state.sequence.sceneTracks[lc.laneIdx];
        }
        if (!lane) continue;

        if (lane->type != TrackType::FaceData) return;

        const FaceClip& fc   = state.faceClips[faceClipIdx];
        const float seqStart = std::max(0.f, XToSeq(dropPos.x, trackX));

        SequenceItem item;
        item.assetIndex = faceClipIdx;
        item.seqStart   = seqStart;
        item.trimIn     = 0.f;
        item.trimOut    = fc.duration;
        item.blendIn    = 0.f;
        item.blendOut   = 0.f;

        lane->items.push_back(item);
        state.time    = seqStart;
        state.playing = false;
        return;
    }
}

// ── Body clip drag-drop placement ─────────────────────────────────────────────

void TimelinePanel::HandleDrop(AppState& state, int clipIdx, ImVec2 dropPos,
                                float trackX, float trackW,
                                const std::vector<LaneCtx>& lanes)
{
    if (clipIdx < 0 || clipIdx >= (int)state.clips.size()) return;

    for (const auto& lc : lanes) {
        if (dropPos.y < lc.yTop || dropPos.y >= lc.yBot) continue;

        // Resolve the lane pointer.
        TrackLane* lane = nullptr;
        if (lc.actorIdx >= 0) {
            for (auto& g : state.sequence.actorTracks)
                if (g.actorIndex == lc.actorIdx && lc.laneIdx < (int)g.lanes.size()) {
                    lane = &g.lanes[lc.laneIdx];
                    break;
                }
        } else if (lc.laneIdx < (int)state.sequence.sceneTracks.size()) {
            lane = &state.sequence.sceneTracks[lc.laneIdx];
        }
        if (!lane) continue;

        // Only AnimClip lanes accept clip assets; drop on anything else is a no-op.
        if (lane->type != TrackType::AnimClip) return;

        // Reject if skeleton types are known and mismatched.
        const AnimClip& clip = state.clips[clipIdx];
        if (!clip.skeletonType.empty() && lc.actorIdx >= 0 &&
            lc.actorIdx < (int)state.actors.size()) {
            const int ci = state.actors[lc.actorIdx].castIndex;
            if (ci >= 0 && ci < (int)state.cast.size() &&
                !state.cast[ci].creatureType.empty() &&
                state.cast[ci].creatureType != clip.skeletonType) {
                return;  // type mismatch — orange feedback already warned the user
            }
        }
        const float     seqStart = std::max(0.f, XToSeq(dropPos.x, trackX));

        SequenceItem item;
        item.assetIndex = clipIdx;
        item.seqStart   = seqStart;
        item.trimIn     = 0.f;
        item.trimOut    = clip.duration;
        item.blendIn    = 0.f;
        item.blendOut   = 0.f;

        lane->items.push_back(item);
        state.time    = seqStart;
        state.playing = false;
        return;
    }
}
