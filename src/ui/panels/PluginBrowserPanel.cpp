#include "PluginBrowserPanel.h"
#include "AppState.h"
#include "DotNetHost.h"
#include <imgui.h>
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
