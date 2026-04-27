#pragma once
#include "ui/IPanel.h"
#include <imgui.h>
#include <vector>

// ── TimelinePanel ──────────────────────────────────────────────────────────────
// NLE-style sequence timeline.
//
// Layout:
//   Transport bar  — play/pause, loop, time counter
//   Ruler          — time axis with adaptive tick marks, scrubber head
//   Track area     — one collapsible group per actor, one lane per track type,
//                    scene tracks (Camera, Audio) below a separator
//   Scrubber       — vertical playhead across the full height
//
// Interaction:
//   Click ruler         → snap time to click position
//   Drag scrubber head  → scrub time
//   Drag clip body      → move seqStart
//   Drag clip left edge → adjust trimIn
//   Drag clip right edge→ adjust trimOut
//   Right-click clip    → context menu (remove, etc.)
class TimelinePanel : public IPanel {
public:
    void        Draw(AppState& state) override;
    const char* PanelID() const override { return "Timeline"; }

private:
    // ── View state ────────────────────────────────────────────────────────────
    float pxPerSec_  = 80.f;   // horizontal zoom: pixels per second of sequence time
    float scrollX_   = 0.f;    // horizontal scroll offset (pixels)
    float headerW_   = 150.f;  // width of the left track-label column

    // ── Drag state ────────────────────────────────────────────────────────────
    enum class DragTarget { None, Scrubber, ItemBody, ItemLeft, ItemRight };
    DragTarget dragTarget_   = DragTarget::None;
    int        dragLaneActor_ = -1;   // actorIndex (-1 = scene lane)
    int        dragLaneIdx_   = -1;   // index within the actor's lanes
    int        dragItemIdx_   = -1;   // item index within the lane
    float      dragOffsetSec_ = 0.f;  // offset from click point to item seqStart

    // ── Internal draw helpers ─────────────────────────────────────────────────
    struct LaneCtx {
        int   actorIdx;   // -1 for scene lanes
        int   laneIdx;
        float yTop;       // screen Y of lane top
        float yBot;       // screen Y of lane bottom
    };

    void DrawTransportBar(AppState& state);
    void DrawRuler(AppState& state, float trackX, float trackW,
                   float rulerY, float rulerH);
    // Returns list of drawn lanes for hit-testing.
    void DrawTrackArea(AppState& state, float trackX, float trackW,
                       float areaY, float areaH,
                       std::vector<LaneCtx>& lanesOut);
    void DrawActorGroup(AppState& state, int actorIdx,
                        float trackX, float trackW, float& curY,
                        std::vector<LaneCtx>& lanesOut);
    void DrawLane(AppState& state, int actorIdx, int laneIdx,
                  float trackX, float trackW, float laneY, float laneH,
                  std::vector<LaneCtx>& lanesOut);
    void DrawScrubber(AppState& state, float trackX, float trackW,
                      float areaY, float fullH, float rulerY);
    void HandleInput(AppState& state, float trackX, float trackW,
                     float areaY, float fullH, float rulerY,
                     const std::vector<LaneCtx>& lanes);
    // Called after DrawTrackArea builds lane geometry; places a dragged clip.
    void HandleDrop(AppState& state, int clipIdx, ImVec2 dropPos,
                    float trackX, float trackW,
                    const std::vector<LaneCtx>& lanes);

    // Convert between sequence seconds and screen X.
    float SeqToX(float t, float trackX) const { return trackX + t * pxPerSec_ - scrollX_; }
    float XToSeq(float x, float trackX) const { return (x - trackX + scrollX_) / pxPerSec_; }
};
