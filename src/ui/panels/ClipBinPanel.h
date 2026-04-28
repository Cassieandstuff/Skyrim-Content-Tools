#pragma once
#include "ui/IPanel.h"

// ── BinPanel ──────────────────────────────────────────────────────────────────
// Three-tab panel:
//   Clips — body animation clip library + import dialog + "Add to Timeline"
//   Face  — face animation clip library (ARKit morph channels from HKX annotations)
//   Cast  — character roster; click to populate Actor Properties panel
class BinPanel : public IPanel {
public:
    void        Draw(AppState& state) override;
    const char* PanelID() const override { return "Bin"; }
    void        OpenImportDialog(AppState& state);

private:
    void DrawClipsTab(AppState& state);
    void AddSelectedClipToTimeline(AppState& state);

    void DrawFaceClipsTab(AppState& state);
    void OpenImportFaceDialog(AppState& state);
    int  selectedFaceClip_ = -1;

    void DrawCastTab(AppState& state);
};
