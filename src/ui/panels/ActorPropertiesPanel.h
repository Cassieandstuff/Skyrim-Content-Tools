#pragma once
#include "ui/IPanel.h"
#include "IPluginBackend.h"
#include <string>
#include <vector>

// ── ActorPropertiesPanel ──────────────────────────────────────────────────────
// Docked below the Bin panel. Two modes:
//   idx < 0  — no actor selected; shows NPC search/create flow with "Add to Cast"
//   idx >= 0 — actor selected; shows identity, skeleton, linked NPC, and link flow
class ActorPropertiesPanel : public IPanel {
public:
    void        Draw(AppState& state) override;
    const char* PanelID() const override { return "Actor Properties"; }

private:
    void DrawIdentity(AppState& state, int idx);
    void DrawLinkedNpc(AppState& state, int idx);
    void DrawSkeletonSection(AppState& state, int idx);
    void DrawSkeletonPickerModal(AppState& state, int idx);
    void DrawNifSection(AppState& state, int idx);
    void DrawPluginSection(AppState& state, int idx);
    void DrawSearchTab(AppState& state, int idx);
    void DrawCreateTab(AppState& state, int idx);
    void TryAutoLoadPlugin(AppState& state);

    // Identity edit buffers — reloaded when selection changes
    int  lastSelected_     = -2;
    char nameEditBuf_[128] = {};
    char editorIdBuf_[128] = {};

    // Plugin combo — index into state.discoveredPlugins; auto-loads on change
    int  selectedPluginIdx_  = -1;
    int  pluginLoadedIdx_    = -1;
    bool loadOrderActive_    = false;  // true after "Load All" was used
    int  loadOrderModCount_  = 0;
    std::string lastDataFolder_;
    char pluginErr_[256]     = {};

    // Search tab
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

    // Skeleton picker modal
    bool showSkelPicker_     = false;
    int  skelPickerIdx_      = -1;
    char skelFilterBuf_[128] = {};
    char skelErr_[256]       = {};
};
