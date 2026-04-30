#include "PluginBrowserPanel.h"
#include "app/AppState.h"
#include "plugin/DotNetHost.h"
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#endif

// ── Main draw ─────────────────────────────────────────────────────────────────

void PluginBrowserPanel::Draw(AppState& state)
{
    ImGui::Begin(PanelID());

    TryAutoLoadPlugin(state);

    if (!state.pluginBackend) {
        ImGui::TextColored({0.9f, 0.5f, 0.3f, 1.f}, "Plugin bridge unavailable.");
        ImGui::TextWrapped(".NET 10 runtime or SctBridge.dll not found next to the exe.");
        ImGui::End();
        return;
    }

    if (state.discoveredPlugins.empty()) {
        ImGui::TextDisabled("No plugins found.");
        ImGui::TextDisabled("Set a Data Folder in the Inspector.");
        ImGui::End();
        return;
    }

    // ── Plugin combo + Load All ───────────────────────────────────────────────
    ImGui::TextDisabled("Plugin:");
    ImGui::SameLine();

    const char* previewLabel = loadOrderActive_
        ? "(load order)"
        : (selectedPluginIdx_ >= 0 &&
           selectedPluginIdx_ < (int)state.discoveredPlugins.size())
            ? state.discoveredPlugins[selectedPluginIdx_].c_str()
            : "(select\xe2\x80\xa6)";

    if (loadOrderActive_) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-90.f);
    if (ImGui::BeginCombo("##pb_plugin", previewLabel)) {
        for (int i = 0; i < (int)state.discoveredPlugins.size(); i++) {
            const bool sel = (selectedPluginIdx_ == i);
            if (ImGui::Selectable(state.discoveredPlugins[i].c_str(), sel)) {
                if (selectedPluginIdx_ != i || loadOrderActive_) {
                    selectedPluginIdx_ = i;
                    pluginLoadedIdx_   = -1;
                    loadOrderActive_   = false;
                    results_.clear();
                    selectedResult_    = -1;
                    pluginErr_[0]      = '\0';
                }
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (loadOrderActive_) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Load All", {-1.f, 0.f})) {
        pluginErr_[0]   = '\0';
        results_.clear();
        selectedResult_ = -1;
        const int count = DotNetHost::LoadOrderLoad(
            state.dataFolder.c_str(), pluginErr_, sizeof(pluginErr_));
        if (count >= 0) {
            loadOrderActive_   = true;
            loadOrderModCount_ = count;
            pluginLoadedIdx_   = selectedPluginIdx_;
        } else {
            loadOrderActive_ = false;
        }
    }
    if (loadOrderActive_)
        ImGui::TextDisabled("%d mods loaded", loadOrderModCount_);

    if (pluginErr_[0]) {
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", pluginErr_);
        ImGui::Spacing();
    }

    const bool pluginReady = loadOrderActive_ ||
                             (pluginLoadedIdx_ == selectedPluginIdx_ &&
                              selectedPluginIdx_ >= 0);
    if (!pluginReady) { ImGui::End(); return; }

    const int idx = state.selectedCast;

    ImGui::Spacing();
    if (ImGui::BeginTabBar("##pb_tabs")) {
        if (ImGui::BeginTabItem("Search")) {
            DrawSearchTab(state, idx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Create New")) {
            DrawCreateTab(state, idx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Cells")) {
            DrawCellsTab(state);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Exterior")) {
            DrawExteriorTab(state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ── TryAutoLoadPlugin ─────────────────────────────────────────────────────────

void PluginBrowserPanel::TryAutoLoadPlugin(AppState& state)
{
    if (state.dataFolder != lastDataFolder_) {
        lastDataFolder_    = state.dataFolder;
        selectedPluginIdx_ = -1;
        pluginLoadedIdx_   = -1;
        loadOrderActive_   = false;
        loadOrderModCount_ = 0;
        pluginErr_[0]      = '\0';
        results_.clear();
        selectedResult_    = -1;
    }

    if (selectedPluginIdx_ >= (int)state.discoveredPlugins.size())
        selectedPluginIdx_ = -1;

    if (selectedPluginIdx_ < 0 || selectedPluginIdx_ == pluginLoadedIdx_) return;
    if (!state.pluginBackend) return;

    pluginErr_[0] = '\0';
    const std::string path = state.dataFolder + "\\" +
                             state.discoveredPlugins[selectedPluginIdx_];
    if (state.pluginBackend->PluginLoad(path, state.dataFolder, pluginErr_, sizeof(pluginErr_)))
        pluginLoadedIdx_ = selectedPluginIdx_;
    else
        pluginLoadedIdx_ = -1;
}

// ── Search tab ────────────────────────────────────────────────────────────────

void PluginBrowserPanel::DrawSearchTab(AppState& state, int idx)
{
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-55.f);
    const bool hitEnter = ImGui::InputTextWithHint(
        "##pb_srch", "Name or EditorID\xe2\x80\xa6",
        searchBuf_, sizeof(searchBuf_),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool doSearch = hitEnter || ImGui::Button("Go", {-1.f, 0.f});

    if (doSearch) {
        selectedResult_ = -1;
        searchErr_[0]   = '\0';
        if (!state.pluginBackend->NpcSearch(searchBuf_, 50, results_,
                                            searchErr_, sizeof(searchErr_)))
            results_.clear();
    }

    if (searchErr_[0])
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", searchErr_);

    const float listH = ImGui::GetContentRegionAvail().y - 28.f;
    ImGui::BeginChild("##pb_results", {-1.f, listH > 40.f ? listH : 40.f}, true);
    for (int i = 0; i < (int)results_.size(); i++) {
        const NpcRecord& r = results_[i];
        bool sel = (selectedResult_ == i);
        char label[256];
        std::snprintf(label, sizeof(label), "%s##pbr%d",
                      r.name.empty() ? r.editorId.c_str() : r.name.c_str(), i);
        if (ImGui::Selectable(label, sel))
            selectedResult_ = i;
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (!r.editorId.empty())     ImGui::TextDisabled("EditorID: %s", r.editorId.c_str());
            if (!r.raceEditorId.empty()) ImGui::TextDisabled("Race:     %s", r.raceEditorId.c_str());
            if (!r.pluginSource.empty()) ImGui::TextDisabled("Source:   %s", r.pluginSource.c_str());
            ImGui::TextDisabled("FormID:   %08X", r.formId);
            ImGui::EndTooltip();
        }
    }
    ImGui::EndChild();

    const bool canAssign = (selectedResult_ >= 0 &&
                            selectedResult_ < (int)results_.size());
    if (!canAssign) ImGui::BeginDisabled();

    const char* actionLabel = (idx < 0) ? "Add to Cast" : "Assign to Selected";
    if (ImGui::Button(actionLabel, {-1.f, 0.f}) && canAssign) {
        const NpcRecord& picked = results_[selectedResult_];
        if (idx < 0) {
            const int actorIdx = state.AddActorFromRecord(picked);
            if (actorIdx >= 0)
                state.selectedCast = state.actors[actorIdx].castIndex;
        } else {
            state.RelinkActorFromRecord(idx, picked);
            ActorDocument& doc = state.cast[idx];
            if (doc.name.empty())
                doc.name = picked.name.empty() ? picked.editorId : picked.name;
            if (doc.editorId.empty())
                doc.editorId = picked.editorId;
        }
    }

    if (!canAssign) ImGui::EndDisabled();
}

// ── Create tab ────────────────────────────────────────────────────────────────

void PluginBrowserPanel::DrawCreateTab(AppState& state, int idx)
{
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##pb_eid",  "EditorID (no spaces)\xe2\x80\xa6",
                             newEditorId_, sizeof(newEditorId_));
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##pb_name", "Display name\xe2\x80\xa6",
                             newName_,     sizeof(newName_));
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##pb_race", "Race FormKey  RRRRR:Plugin.esm",
                             raceFormKey_, sizeof(raceFormKey_));
    ImGui::Checkbox("Female##pb", &newIsFemale_);

    ImGui::Spacing();
    ImGui::TextDisabled("Project:");
    ImGui::SameLine();
    ImGui::TextDisabled(projectName_.empty() ? "(none)" : projectName_.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("New\xe2\x80\xa6##pb_proj")) {
        if (newEditorId_[0]) {
            std::string modName = std::string(newEditorId_) + "_SCT.esp";
            char err[256] = {};
            if (state.pluginBackend->ProjectNew(modName, err, sizeof(err)))
                projectName_ = modName;
            else
                std::snprintf(createErr_, sizeof(createErr_), "%s", err);
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Load\xe2\x80\xa6##pb_proj")) {
#if defined(_WIN32)
        char buf[512] = {};
        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = "Plugin Files\0*.esp;*.esm;*.esl\0All Files\0*.*\0";
        ofn.lpstrFile   = buf;
        ofn.nMaxFile    = sizeof(buf);
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        ofn.lpstrTitle  = "Load Project Mod";
        if (GetOpenFileNameA(&ofn)) {
            char err[256] = {};
            if (state.pluginBackend->ProjectLoad(buf, err, sizeof(err))) {
                const char* fn = std::strrchr(buf, '\\');
                projectName_   = fn ? fn + 1 : buf;
                createErr_[0]  = '\0';
            } else {
                std::snprintf(createErr_, sizeof(createErr_), "%s", err);
            }
        }
#endif
    }

    ImGui::Spacing();
    const bool canCreate = (newEditorId_[0] && !projectName_.empty());
    if (!canCreate) ImGui::BeginDisabled();

    const char* createLabel = (idx < 0) ? "Create && Add to Cast" : "Create && Assign";
    if (ImGui::Button(createLabel, {-1.f, 0.f}) && canCreate) {
        NpcCreateParams p;
        p.editorId    = newEditorId_;
        p.name        = newName_;
        p.raceFormKey = raceFormKey_;
        p.isFemale    = newIsFemale_;
        NpcRecord out;
        createErr_[0] = '\0';
        if (state.pluginBackend->NpcCreate(p, out, createErr_, sizeof(createErr_))) {
            if (idx < 0) {
                const int actorIdx = state.AddActorFromRecord(out);
                if (actorIdx >= 0)
                    state.selectedCast = state.actors[actorIdx].castIndex;
            } else {
                state.RelinkActorFromRecord(idx, out);
                ActorDocument& doc = state.cast[idx];
                doc.name    = out.name.empty() ? p.name : out.name;
                doc.editorId = out.editorId;
            }
        }
    }

    if (!canCreate) ImGui::EndDisabled();

    if (createErr_[0]) {
        ImGui::Spacing();
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", createErr_);
    }
}

// ── Cells tab ─────────────────────────────────────────────────────────────────

void PluginBrowserPanel::DrawCellsTab(AppState& state)
{
    // ── Current cell status ───────────────────────────────────────────────────
    ImGui::Spacing();
    if (state.loadedCell.loaded) {
        ImGui::TextDisabled("Loaded:");
        ImGui::SameLine();
        ImGui::TextUnformatted(state.loadedCell.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%d refs)", (int)state.loadedCell.refs.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Unload")) {
            state.UnloadCell();
            state.PushToast("Cell unloaded", ToastLevel::Info);
        }
    } else {
        ImGui::TextDisabled("No cell loaded");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Cell search ───────────────────────────────────────────────────────────
    ImGui::SetNextItemWidth(-55.f);
    const bool hitEnter = ImGui::InputTextWithHint(
        "##cell_srch", "Name or EditorID\xe2\x80\xa6",
        cellSearchBuf_, sizeof(cellSearchBuf_),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool doSearch = hitEnter || ImGui::Button("Go##cell", {-1.f, 0.f});

    if (doSearch) {
        selectedCell_ = -1;
        cellErr_[0]   = '\0';
        cellResults_.clear();

        std::string jsonStr;
        if (DotNetHost::CellSearch(cellSearchBuf_, 200, jsonStr,
                                   cellErr_, sizeof(cellErr_))) {
            try {
                using json = nlohmann::json;
                for (const auto& j : json::parse(jsonStr)) {
                    CellRecord r;
                    r.formId      = j.value("formId",      0u);
                    r.formKey     = j.value("formKey",     std::string{});
                    r.editorId    = j.value("editorId",    std::string{});
                    r.name        = j.value("name",        std::string{});
                    r.pluginSource = j.value("pluginSource", std::string{});
                    if (!r.formKey.empty())
                        cellResults_.push_back(std::move(r));
                }
            } catch (const std::exception& e) {
                std::snprintf(cellErr_, sizeof(cellErr_),
                              "JSON parse error: %s", e.what());
                cellResults_.clear();
            }
        }
    }

    if (cellErr_[0]) {
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", cellErr_);
        ImGui::Spacing();
    }

    // ── Results list ──────────────────────────────────────────────────────────
    const float listH = ImGui::GetContentRegionAvail().y - 28.f;
    ImGui::BeginChild("##cell_results", {-1.f, listH > 40.f ? listH : 40.f}, true);

    for (int i = 0; i < (int)cellResults_.size(); i++) {
        const CellRecord& r = cellResults_[i];

        // Show name if available, fall back to EditorID
        const char* label = r.name.empty() ? r.editorId.c_str() : r.name.c_str();
        char fullLabel[320];
        std::snprintf(fullLabel, sizeof(fullLabel), "%s##cell%d", label, i);

        bool sel = (selectedCell_ == i);
        if (ImGui::Selectable(fullLabel, sel,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            selectedCell_ = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                // Double-click → load immediately
                cellErr_[0] = '\0';
                const char* displayName = r.name.empty()
                    ? r.editorId.c_str() : r.name.c_str();
                if (!state.LoadCell(r.formKey.c_str(), displayName,
                                    cellErr_, sizeof(cellErr_))) {
                    // error shown below
                } else {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg),
                        "Cell loaded — %d refs", (int)state.loadedCell.refs.size());
                    state.PushToast(msg, ToastLevel::Info);
                }
            }
        }

        // Tooltip with metadata
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (!r.editorId.empty())    ImGui::TextDisabled("EditorID: %s", r.editorId.c_str());
            if (!r.pluginSource.empty())ImGui::TextDisabled("Source:   %s", r.pluginSource.c_str());
            ImGui::TextDisabled("FormKey:  %s", r.formKey.c_str());
            ImGui::TextDisabled("Double-click to load");
            ImGui::EndTooltip();
        }

        // Right-aligned plugin source label
        if (!r.pluginSource.empty()) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x
                            - ImGui::CalcTextSize(r.pluginSource.c_str()).x);
            ImGui::TextDisabled("%s", r.pluginSource.c_str());
        }
    }

    ImGui::EndChild();

    // ── Load button ───────────────────────────────────────────────────────────
    const bool canLoad = (selectedCell_ >= 0 &&
                          selectedCell_ < (int)cellResults_.size());
    if (!canLoad) ImGui::BeginDisabled();

    if (ImGui::Button("Load Cell", {-1.f, 0.f}) && canLoad) {
        cellErr_[0] = '\0';
        const CellRecord& r = cellResults_[selectedCell_];
        const char* displayName = r.name.empty()
            ? r.editorId.c_str() : r.name.c_str();
        if (state.LoadCell(r.formKey.c_str(), displayName,
                           cellErr_, sizeof(cellErr_))) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                "Cell loaded — %d refs", (int)state.loadedCell.refs.size());
            state.PushToast(msg, ToastLevel::Info);
        }
    }

    if (!canLoad) ImGui::EndDisabled();

    if (cellErr_[0]) {
        ImGui::Spacing();
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", cellErr_);
    }
}

// ── Exterior tab ──────────────────────────────────────────────────────────────

void PluginBrowserPanel::DrawExteriorTab(AppState& state)
{
    ImGui::Spacing();

    // Current exterior cell status
    if (state.loadedCell.loaded && state.loadedCell.isExterior) {
        ImGui::TextDisabled("Loaded:");
        ImGui::SameLine();
        ImGui::TextUnformatted(state.loadedCell.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%d refs)", (int)state.loadedCell.refs.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Unload##ext")) {
            state.UnloadCell();
            state.PushToast("Cell unloaded", ToastLevel::Info);
        }
    } else {
        ImGui::TextDisabled("No exterior cell loaded");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Worldspace search ─────────────────────────────────────────────────────
    ImGui::TextDisabled("Worldspace:");
    ImGui::SetNextItemWidth(-55.f);
    const bool hitEnter = ImGui::InputTextWithHint(
        "##ws_srch", "Name or EditorID\xe2\x80\xa6",
        wsSearchBuf_, sizeof(wsSearchBuf_),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool doSearch = hitEnter || ImGui::Button("Go##ws", {-1.f, 0.f});

    if (doSearch) {
        selectedWs_ = -1;
        wsErr_[0]   = '\0';
        wsResults_.clear();

        std::string jsonStr;
        if (DotNetHost::WorldspaceSearch(wsSearchBuf_, 50, jsonStr,
                                         wsErr_, sizeof(wsErr_))) {
            try {
                using json = nlohmann::json;
                for (const auto& j : json::parse(jsonStr)) {
                    WorldspaceRecord r;
                    r.formKey      = j.value("formKey",      std::string{});
                    r.editorId     = j.value("editorId",     std::string{});
                    r.name         = j.value("name",         std::string{});
                    r.pluginSource = j.value("pluginSource", std::string{});
                    if (!r.formKey.empty())
                        wsResults_.push_back(std::move(r));
                }
            } catch (const std::exception& e) {
                std::snprintf(wsErr_, sizeof(wsErr_), "JSON parse error: %s", e.what());
                wsResults_.clear();
            }
        }
    }

    if (wsErr_[0]) {
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", wsErr_);
        ImGui::Spacing();
    }

    // Worldspace results list
    ImGui::BeginChild("##ws_results", {-1.f, 80.f}, true);
    for (int i = 0; i < (int)wsResults_.size(); i++) {
        const WorldspaceRecord& r = wsResults_[i];
        const char* label = r.name.empty() ? r.editorId.c_str() : r.name.c_str();
        char fullLabel[320];
        std::snprintf(fullLabel, sizeof(fullLabel), "%s##ws%d", label, i);
        bool sel = (selectedWs_ == i);
        if (ImGui::Selectable(fullLabel, sel))
            selectedWs_ = i;
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (!r.editorId.empty())     ImGui::TextDisabled("EditorID: %s", r.editorId.c_str());
            if (!r.pluginSource.empty()) ImGui::TextDisabled("Source:   %s", r.pluginSource.c_str());
            ImGui::TextDisabled("FormKey:  %s", r.formKey.c_str());
            ImGui::EndTooltip();
        }
        if (!r.pluginSource.empty()) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x
                            - ImGui::CalcTextSize(r.pluginSource.c_str()).x);
            ImGui::TextDisabled("%s", r.pluginSource.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Cell coordinates ──────────────────────────────────────────────────────
    ImGui::TextDisabled("Cell X:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.f);
    ImGui::InputInt("##ext_cx", &exteriorCellX_, 0, 0);
    ImGui::SameLine();
    ImGui::TextDisabled("Y:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.f);
    ImGui::InputInt("##ext_cy", &exteriorCellY_, 0, 0);

    ImGui::Spacing();

    const bool canLoad = (selectedWs_ >= 0 && selectedWs_ < (int)wsResults_.size());
    if (!canLoad) ImGui::BeginDisabled();

    if (ImGui::Button("Load Exterior Cell", {-1.f, 0.f}) && canLoad) {
        exteriorErr_[0] = '\0';
        const WorldspaceRecord& ws = wsResults_[selectedWs_];
        const char* wsName = ws.name.empty() ? ws.editorId.c_str() : ws.name.c_str();
        char cellName[256];
        std::snprintf(cellName, sizeof(cellName), "%s [%d, %d]",
                      wsName, exteriorCellX_, exteriorCellY_);
        if (state.LoadExteriorCell(ws.formKey.c_str(), exteriorCellX_, exteriorCellY_,
                                   cellName, exteriorErr_, sizeof(exteriorErr_))) {
            char msg[256];
            std::snprintf(msg, sizeof(msg), "Exterior cell loaded — %d refs",
                          (int)state.loadedCell.refs.size());
            state.PushToast(msg, ToastLevel::Info);
        }
    }

    if (!canLoad) ImGui::EndDisabled();

    if (exteriorErr_[0]) {
        ImGui::Spacing();
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", exteriorErr_);
    }
}
