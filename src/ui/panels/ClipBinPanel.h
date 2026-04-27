#pragma once
#include "ui/IPanel.h"

// ── BinPanel ──────────────────────────────────────────────────────────────────
// Two-tab panel:
//   Clips — animation clip library + import dialog + "Add to Timeline" action
//   Cast  — character roster; click an entry to populate the Actor Properties panel
class BinPanel : public IPanel {
public:
    void        Draw(AppState& state) override;
    const char* PanelID() const override { return "Bin"; }
    void        OpenImportDialog(AppState& state);

private:
    void DrawClipsTab(AppState& state);
    void AddSelectedClipToTimeline(AppState& state);

    void DrawCastTab(AppState& state);
};
