#pragma once
#include "ui/IPanel.h"
#include <string>

// ── InspectorPanel ────────────────────────────────────────────────────────────
// Context-sensitive right-column panel.
//   Nothing selected  →  Scene Properties
//   Actor selected    →  Actor identity, skeleton, NIF, linked NPC info
//   (Clip, Bone views added in later phases)
class InspectorPanel : public IPanel {
public:
    void        Draw(AppState& state) override;
    const char* PanelID() const override { return "Inspector"; }

private:
    void DrawSceneProperties(AppState& state);
    void DrawActorProperties(AppState& state, int castIdx);
    void DrawLinkedNpc(const AppState& state, int castIdx);
    void DrawSkeletonSection(AppState& state, int castIdx);
    void DrawMeshesSection(AppState& state, int castIdx);
    void DrawFaceMorphsSection(AppState& state, int castIdx);
    void DrawSkeletonPickerModal(AppState& state, int castIdx);
    void DrawCellRefProperties(const AppState& state, int refIndex);

    // Identity edit buffers — reloaded when selection changes
    int  lastSelected_     = -2;
    char nameEditBuf_[128] = {};
    char editorIdBuf_[128] = {};

    // Skeleton picker modal
    bool showSkelPicker_     = false;
    int  skelPickerIdx_      = -1;
    char skelFilterBuf_[128] = {};
    char skelErr_[256]       = {};

    // Face morph filter
    char morphFilterBuf_[128] = {};
};
