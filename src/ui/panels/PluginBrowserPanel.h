#pragma once
#include "ui/IPanel.h"
#include "IPluginBackend.h"
#include <string>
#include <vector>

// ── PluginBrowserPanel ────────────────────────────────────────────────────────
// Dockable panel for loading plugins, finding NPCs, and loading cell environments.
// NPC primary action adapts to selection:
//   selectedCast < 0  →  "Add to Cast"
//   selectedCast >= 0 →  "Assign to Selected"
class PluginBrowserPanel : public IPanel {
public:
    void        Draw(AppState& state) override;
    const char* PanelID() const override { return "Plugin Browser"; }

private:
    void DrawSearchTab(AppState& state, int selectedCast);
    void DrawCreateTab(AppState& state, int selectedCast);
    void DrawCellsTab(AppState& state);
    void DrawExteriorTab(AppState& state);
    void TryAutoLoadPlugin(AppState& state);

    // Plugin combo
    int  selectedPluginIdx_  = -1;
    int  pluginLoadedIdx_    = -1;
    bool loadOrderActive_    = false;
    int  loadOrderModCount_  = 0;
    std::string lastDataFolder_;
    char pluginErr_[256]     = {};

    // Search tab (NPCs)
    char searchBuf_[128]       = {};
    std::vector<NpcRecord> results_;
    int  selectedResult_       = -1;
    char searchErr_[256]       = {};

    // Create tab
    char newEditorId_[128] = {};
    char newName_[128]     = {};
    char raceFormKey_[128] = {};
    bool newIsFemale_      = false;
    std::string projectName_;
    char createErr_[256]   = {};

    // Cells tab
    char cellSearchBuf_[128]         = {};
    std::vector<CellRecord> cellResults_;
    int  selectedCell_               = -1;
    char cellErr_[256]               = {};

    // Exterior tab
    char wsSearchBuf_[128]                  = {};
    std::vector<WorldspaceRecord> wsResults_;
    int  selectedWs_                        = -1;
    char wsErr_[256]                        = {};
    int  exteriorCellX_                     = 0;
    int  exteriorCellY_                     = 0;
    char exteriorErr_[256]                  = {};
};
