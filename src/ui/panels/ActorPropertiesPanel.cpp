#include "ActorPropertiesPanel.h"
#include "AppState.h"
#include "CastEntry.h"
#include "DotNetHost.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#endif

// ── Main draw ─────────────────────────────────────────────────────────────────

void ActorPropertiesPanel::Draw(AppState& state)
{
    ImGui::Begin(PanelID());

    // Clamp stale index
    if (state.selectedCast >= (int)state.cast.size())
        state.selectedCast = -1;

    // Sync plugin combo when data folder changes
    TryAutoLoadPlugin(state);

    const int idx = state.selectedCast;

    if (idx >= 0) {
        DrawIdentity(state, idx);
        DrawLinkedNpc(state, idx);

        ImGui::Spacing();
        ImGui::SeparatorText("Skeleton");
        DrawSkeletonSection(state, idx);

        ImGui::Spacing();
        ImGui::SeparatorText("Body NIF");
        DrawNifSection(state, idx);

        ImGui::Spacing();
        ImGui::SeparatorText("Link NPC");
        DrawPluginSection(state, idx);
    } else {
        ImGui::TextDisabled("No actor selected.");
        ImGui::Spacing();
        ImGui::TextWrapped("Select a character from the Cast list, or use the search below to add a new one.");
        ImGui::Spacing();
        ImGui::SeparatorText("Add Actor");
        DrawPluginSection(state, -1);
    }

    DrawSkeletonPickerModal(state, idx);

    ImGui::End();
}

// ── Identity ──────────────────────────────────────────────────────────────────

void ActorPropertiesPanel::DrawIdentity(AppState& state, int idx)
{
    CastEntry& entry = state.cast[idx];

    if (idx != lastSelected_) {
        std::snprintf(nameEditBuf_,  sizeof(nameEditBuf_),  "%s", entry.name.c_str());
        std::snprintf(editorIdBuf_,  sizeof(editorIdBuf_),  "%s", entry.editorId.c_str());
        lastSelected_ = idx;
        selectedResult_ = -1;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.85f, 1.f));
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputTextWithHint("##apname", "Name\xe2\x80\xa6", nameEditBuf_, sizeof(nameEditBuf_)))
        entry.name = nameEditBuf_;
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputTextWithHint("##apeid", "EditorID\xe2\x80\xa6", editorIdBuf_, sizeof(editorIdBuf_)))
        entry.editorId = editorIdBuf_;
    ImGui::PopStyleColor();
}

// ── Linked NPC ────────────────────────────────────────────────────────────────

void ActorPropertiesPanel::DrawLinkedNpc(AppState& state, int idx)
{
    CastEntry& entry = state.cast[idx];
    if (!entry.npcRecord.has_value()) return;

    const NpcRecord& rec = *entry.npcRecord;
    ImGui::Spacing();
    ImGui::SeparatorText("Linked NPC");

    ImGui::TextDisabled("%-8s %s", "Race:",   rec.raceEditorId.empty()  ? "\xe2\x80\x94" : rec.raceEditorId.c_str());
    ImGui::TextDisabled("%-8s %s", "Sex:",    rec.isFemale ? "Female" : "Male");
    ImGui::TextDisabled("%-8s %s", "Source:", rec.pluginSource.empty() ? "\xe2\x80\x94" : rec.pluginSource.c_str());
    ImGui::TextDisabled("%-8s %08X", "FormID:", rec.formId);

    ImGui::Spacing();
    if (ImGui::SmallButton("Unlink##ap"))
        entry.npcRecord.reset();
}

// ── Skeleton section ──────────────────────────────────────────────────────────

void ActorPropertiesPanel::DrawSkeletonSection(AppState& state, int idx)
{
    CastEntry& entry = state.cast[idx];

    if (entry.skeletonIndex >= 0 && entry.skeletonIndex < (int)state.skeletons.size()) {
        const auto& skel = state.skeletons[entry.skeletonIndex];
        ImGui::TextDisabled("%s  \xe2\x80\xa2  %d bones",
            entry.skeletonType.empty() ? "unknown" : entry.skeletonType.c_str(),
            (int)skel.bones.size());
        ImGui::Spacing();
        if (ImGui::Button("Change Skeleton\xe2\x80\xa6", {-1.f, 0.f})) {
            showSkelPicker_ = true;
            skelPickerIdx_  = -1;
            skelFilterBuf_[0] = '\0';
            skelErr_[0]     = '\0';
        }
    } else {
        ImGui::TextColored({0.9f, 0.5f, 0.3f, 1.f}, "No skeleton assigned");
        ImGui::Spacing();
        if (!state.discoveredSkeletons.empty()) {
            if (ImGui::Button("Pick Skeleton\xe2\x80\xa6", {-1.f, 0.f})) {
                showSkelPicker_ = true;
                skelPickerIdx_  = -1;
                skelFilterBuf_[0] = '\0';
                skelErr_[0]     = '\0';
            }
        } else {
            ImGui::TextDisabled("Set a Data Folder to browse skeletons.");
        }
    }

    if (skelErr_[0]) {
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", skelErr_);
    }
}

// ── Skeleton picker modal ─────────────────────────────────────────────────────

void ActorPropertiesPanel::DrawSkeletonPickerModal(AppState& state, int idx)
{
    if (showSkelPicker_) {
        ImGui::OpenPopup("Pick Skeleton##ap");
        showSkelPicker_ = false;
    }

    ImGui::SetNextWindowSize({420.f, 420.f}, ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Pick Skeleton##ap", nullptr,
                                ImGuiWindowFlags_NoResize)) return;

    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##apsk_filter", "Filter by name or type\xe2\x80\xa6",
                             skelFilterBuf_, sizeof(skelFilterBuf_));
    std::string filterLow = skelFilterBuf_;
    std::transform(filterLow.begin(), filterLow.end(), filterLow.begin(), ::tolower);

    ImGui::BeginChild("##apsk_list", {-1.f, -52.f}, true);

    bool doConfirm = false;
    std::string lastType;
    for (int i = 0; i < (int)state.discoveredSkeletons.size(); i++) {
        const DiscoveredSkeleton& ds = state.discoveredSkeletons[i];

        if (!filterLow.empty()) {
            std::string hay = ds.creatureType + ds.displayName;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (hay.find(filterLow) == std::string::npos) continue;
        }

        if (ds.creatureType != lastType) {
            std::string hdr = ds.creatureType.empty() ? "Unknown" : ds.creatureType;
            hdr[0] = (char)std::toupper((unsigned char)hdr[0]);
            ImGui::SeparatorText(hdr.c_str());
            lastType = ds.creatureType;
        }

        bool sel = (skelPickerIdx_ == i);
        char rowId[256];
        std::snprintf(rowId, sizeof(rowId), "%s##apsk%d", ds.displayName.c_str(), i);
        if (ImGui::Selectable(rowId, sel, ImGuiSelectableFlags_AllowDoubleClick)) {
            skelPickerIdx_ = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                doConfirm = true;
        }
        if (ImGui::IsItemHovered()) {
            if (ds.bsaInternal.empty()) {
                ImGui::SetTooltip("%s", ds.path.c_str());
            } else {
                auto bsaName = std::filesystem::path(ds.path).filename().string();
                ImGui::SetTooltip("%s :: %s", bsaName.c_str(), ds.bsaInternal.c_str());
            }
        }
    }

    ImGui::EndChild();
    ImGui::Separator();

    const bool canOK = (skelPickerIdx_ >= 0 &&
                        skelPickerIdx_ < (int)state.discoveredSkeletons.size());
    if (!canOK) ImGui::BeginDisabled();
    if ((ImGui::Button("OK", {80.f, 0.f}) || doConfirm) && canOK) {
        const DiscoveredSkeleton& ds = state.discoveredSkeletons[skelPickerIdx_];
        skelErr_[0] = '\0';
        if (idx >= 0) {
            if (!state.AssignSkeletonToCast(idx, ds, skelErr_, sizeof(skelErr_)))
                ; // error already written to skelErr_
        }
        ImGui::CloseCurrentPopup();
    }
    if (!canOK) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", {80.f, 0.f}))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

// ── NIF section ───────────────────────────────────────────────────────────────

void ActorPropertiesPanel::DrawNifSection(AppState& state, int idx)
{
    CastEntry& entry = state.cast[idx];

    if (entry.nifPath.empty()) {
        ImGui::TextDisabled("None");
    } else {
        std::string fn = std::filesystem::path(entry.nifPath).filename().string();
        ImGui::TextDisabled("%s", fn.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", entry.nifPath.c_str());
    }

    ImGui::Spacing();
    if (ImGui::Button("Browse NIF\xe2\x80\xa6", { entry.nifPath.empty() ? -1.f : -60.f, 0.f })) {
#if defined(_WIN32)
        char buf[512] = {};
        OPENFILENAMEA ofn     = {};
        ofn.lStructSize       = sizeof(ofn);
        ofn.lpstrFilter       = "NIF Files\0*.nif\0All Files\0*.*\0";
        ofn.lpstrFile         = buf;
        ofn.nMaxFile          = sizeof(buf);
        ofn.Flags             = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        ofn.lpstrTitle        = "Select Body NIF";
        if (GetOpenFileNameA(&ofn))
            entry.nifPath = buf;
#endif
    }

    if (!entry.nifPath.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Clear##nif"))
            entry.nifPath.clear();
    }
}

// ── Plugin section ────────────────────────────────────────────────────────────

void ActorPropertiesPanel::DrawPluginSection(AppState& state, int idx)
{
    if (!state.pluginBackend) {
        ImGui::TextColored({0.9f, 0.5f, 0.3f, 1.f}, "Plugin bridge unavailable.");
        ImGui::TextWrapped(".NET 10 runtime or SctBridge.dll not found next to the exe.");
        return;
    }

    if (state.discoveredPlugins.empty()) {
        ImGui::TextDisabled("No plugins found. Set a Data Folder in the Cast tab.");
        return;
    }

    // ── Plugin combo + Load All button ───────────────────────────────────────
    ImGui::TextDisabled("Plugin:");
    ImGui::SameLine();

    if (loadOrderActive_) {
        ImGui::SetNextItemWidth(-90.f);
    } else {
        ImGui::SetNextItemWidth(-90.f);
    }

    const char* previewLabel = loadOrderActive_
        ? "(load order)"
        : (selectedPluginIdx_ >= 0 &&
           selectedPluginIdx_ < (int)state.discoveredPlugins.size())
            ? state.discoveredPlugins[selectedPluginIdx_].c_str()
            : "(select\xe2\x80\xa6)";

    if (loadOrderActive_) ImGui::BeginDisabled();
    if (ImGui::BeginCombo("##pluginCombo", previewLabel)) {
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
    if (ImGui::Button("Load All", {-1.f, 0.f}) && state.pluginBackend) {
        pluginErr_[0]    = '\0';
        results_.clear();
        selectedResult_  = -1;
        const int count  = DotNetHost::LoadOrderLoad(
            state.dataFolder.c_str(), pluginErr_, sizeof(pluginErr_));
        if (count >= 0) {
            loadOrderActive_   = true;
            loadOrderModCount_ = count;
            pluginLoadedIdx_   = selectedPluginIdx_; // suppress auto-load
        } else {
            loadOrderActive_ = false;
        }
    }
    if (loadOrderActive_) {
        ImGui::TextDisabled("%d mods loaded", loadOrderModCount_);
    }

    if (pluginErr_[0]) {
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", pluginErr_);
        ImGui::Spacing();
    }

    // Not loaded yet — TryAutoLoadPlugin handles this; just return if still unloaded
    const bool pluginReady = loadOrderActive_ ||
                             (pluginLoadedIdx_ == selectedPluginIdx_ && selectedPluginIdx_ >= 0);
    if (!pluginReady) return;

    ImGui::Spacing();
    if (ImGui::BeginTabBar("##linkTabs")) {
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
}

// ── TryAutoLoadPlugin ─────────────────────────────────────────────────────────

void ActorPropertiesPanel::TryAutoLoadPlugin(AppState& state)
{
    // Reset when data folder changes
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

    // Clamp index if discoveredPlugins shrank
    if (selectedPluginIdx_ >= (int)state.discoveredPlugins.size())
        selectedPluginIdx_ = -1;

    // Auto-load when index changes
    if (selectedPluginIdx_ < 0 || selectedPluginIdx_ == pluginLoadedIdx_) return;
    if (!state.pluginBackend) return;

    pluginErr_[0] = '\0';
    const std::string path = state.dataFolder + "\\" + state.discoveredPlugins[selectedPluginIdx_];
    if (state.pluginBackend->PluginLoad(path, state.dataFolder, pluginErr_, sizeof(pluginErr_)))
        pluginLoadedIdx_ = selectedPluginIdx_;
    else
        pluginLoadedIdx_ = -1;
}

// ── Search tab ────────────────────────────────────────────────────────────────

void ActorPropertiesPanel::DrawSearchTab(AppState& state, int idx)
{
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-55.f);
    const bool hitEnter = ImGui::InputTextWithHint("##srch", "Name or EditorID\xe2\x80\xa6",
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
    ImGui::BeginChild("##npcresults", {-1.f, listH > 40.f ? listH : 40.f}, true);
    for (int i = 0; i < (int)results_.size(); i++) {
        const NpcRecord& r = results_[i];
        bool sel = (selectedResult_ == i);
        char label[256];
        std::snprintf(label, sizeof(label), "%s##res%d",
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

    const char* actionLabel = (idx < 0) ? "Add to Cast" : "Assign to Actor";
    if (ImGui::Button(actionLabel, {-1.f, 0.f}) && canAssign) {
        const NpcRecord& picked = results_[selectedResult_];
        if (idx < 0) {
            // NPC-first: create actor from record
            const int actorIdx = state.AddActorFromRecord(picked);
            if (actorIdx >= 0)
                state.selectedCast = state.actors[actorIdx].castIndex;
        } else {
            // Assign record to existing cast entry
            CastEntry& entry = state.cast[idx];
            entry.npcRecord  = picked;
            if (entry.name.empty()) {
                entry.name = picked.name.empty() ? picked.editorId : picked.name;
                std::snprintf(nameEditBuf_, sizeof(nameEditBuf_), "%s", entry.name.c_str());
            }
            if (entry.editorId.empty()) {
                entry.editorId = picked.editorId;
                std::snprintf(editorIdBuf_, sizeof(editorIdBuf_), "%s", entry.editorId.c_str());
            }
        }
    }

    if (!canAssign) ImGui::EndDisabled();
}

// ── Create tab ────────────────────────────────────────────────────────────────

void ActorPropertiesPanel::DrawCreateTab(AppState& state, int idx)
{
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##cr_eid",  "EditorID (no spaces)\xe2\x80\xa6",
                             newEditorId_, sizeof(newEditorId_));
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##cr_name", "Display name\xe2\x80\xa6",
                             newName_,     sizeof(newName_));
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##cr_race", "Race FormKey  RRRRR:Plugin.esm",
                             raceFormKey_, sizeof(raceFormKey_));
    ImGui::Checkbox("Female", &newIsFemale_);

    // Project mod
    ImGui::Spacing();
    ImGui::TextDisabled("Project:");
    ImGui::SameLine();
    ImGui::TextDisabled(projectName_.empty() ? "(none)" : projectName_.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("New\xe2\x80\xa6##proj")) {
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
    if (ImGui::SmallButton("Load\xe2\x80\xa6##proj")) {
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
                CastEntry& entry = state.cast[idx];
                entry.npcRecord  = out;
                entry.name       = out.name.empty() ? p.name : out.name;
                entry.editorId   = out.editorId;
                std::snprintf(nameEditBuf_,  sizeof(nameEditBuf_),  "%s", entry.name.c_str());
                std::snprintf(editorIdBuf_,  sizeof(editorIdBuf_),  "%s", entry.editorId.c_str());
            }
        }
    }

    if (!canCreate) ImGui::EndDisabled();

    if (createErr_[0]) {
        ImGui::Spacing();
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", createErr_);
    }
}
