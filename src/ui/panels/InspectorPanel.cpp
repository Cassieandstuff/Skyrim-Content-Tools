#include "InspectorPanel.h"
#include "AppState.h"
#include "DotNetHost.h"
#include "TriDocument.h"
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
#include <shlobj.h>
#endif

// ── Main draw ─────────────────────────────────────────────────────────────────

void InspectorPanel::Draw(AppState& state)
{
    ImGui::Begin(PanelID());

    if (state.selectedCast >= 0 && state.selectedCast < (int)state.cast.size())
        DrawActorProperties(state, state.selectedCast);
    else
        DrawSceneProperties(state);

    DrawSkeletonPickerModal(state, state.selectedCast);

    ImGui::End();
}

// ── Scene Properties ──────────────────────────────────────────────────────────

void InspectorPanel::DrawSceneProperties(AppState& state)
{
    ImGui::SeparatorText("Scene");

    // Scene name
    static char nameBuf[128] = {};
    static std::string lastName;
    if (state.projectName != lastName) {
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", state.projectName.c_str());
        lastName = state.projectName;
    }
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputTextWithHint("##scn_name", "Scene name\xe2\x80\xa6", nameBuf, sizeof(nameBuf))) {
        state.projectName = nameBuf;
        state.projectDirty = true;
    }

    ImGui::Spacing();

    // Data folder
    ImGui::TextDisabled("Data Folder:");
    if (state.dataFolder.empty()) {
        ImGui::TextColored({0.9f, 0.5f, 0.3f, 1.f}, "Not set");
    } else {
        ImGui::TextWrapped("%s", state.dataFolder.c_str());
    }
    if (ImGui::Button("Browse\xe2\x80\xa6##df", {-1.f, 0.f})) {
#if defined(_WIN32)
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        BROWSEINFOA bi = {};
        bi.lpszTitle  = "Select Skyrim Data folder";
        bi.ulFlags    = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (pidl) {
            char pathBuf[MAX_PATH] = {};
            if (SHGetPathFromIDListA(pidl, pathBuf)) {
                state.dataFolder = pathBuf;
                state.ScanDataFolder();
                state.SaveSettings();
            }
            CoTaskMemFree(pidl);
        }
        if (SUCCEEDED(hrCom)) CoUninitialize();
#endif
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Stats");

    const float seqDur = state.sequence.Duration();
    ImGui::TextDisabled("Actors:    %d", (int)state.actors.size());
    ImGui::TextDisabled("Clips:     %d", (int)state.clips.size());
    ImGui::TextDisabled("Duration:  %.2f s", seqDur);
    if (!state.dataFolder.empty())
        ImGui::TextDisabled("Skeletons: %d found", (int)state.discoveredSkeletons.size());
}

// ── Actor Properties ──────────────────────────────────────────────────────────

void InspectorPanel::DrawActorProperties(AppState& state, int castIdx)
{
    ActorDocument& doc = state.cast[castIdx];

    // Reload edit buffers when selection changes
    if (castIdx != lastSelected_) {
        std::snprintf(nameEditBuf_,  sizeof(nameEditBuf_),  "%s", doc.name.c_str());
        std::snprintf(editorIdBuf_,  sizeof(editorIdBuf_),  "%s", doc.editorId.c_str());
        morphFilterBuf_[0] = '\0';
        lastSelected_   = castIdx;
    }

    // Header
    char header[128];
    std::snprintf(header, sizeof(header), "Actor: %s",
                  doc.name.empty() ? "(unnamed)" : doc.name.c_str());
    ImGui::SeparatorText(header);

    // Identity
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.85f, 1.f));
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputTextWithHint("##isp_name", "Name\xe2\x80\xa6", nameEditBuf_, sizeof(nameEditBuf_))) {
        doc.name = nameEditBuf_;
        state.projectDirty = true;
    }
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputTextWithHint("##isp_eid", "EditorID\xe2\x80\xa6", editorIdBuf_, sizeof(editorIdBuf_))) {
        doc.editorId = editorIdBuf_;
        state.projectDirty = true;
    }
    ImGui::PopStyleColor();

    DrawLinkedNpc(state, castIdx);

    ImGui::Spacing();
    ImGui::SeparatorText("Skeleton");
    DrawSkeletonSection(state, castIdx);

    ImGui::Spacing();
    ImGui::SeparatorText("Meshes");
    DrawMeshesSection(state, castIdx);

    DrawFaceMorphsSection(state, castIdx);
}

// ── Linked NPC ────────────────────────────────────────────────────────────────

void InspectorPanel::DrawLinkedNpc(const AppState& state, int castIdx)
{
    const ActorDocument& doc = state.cast[castIdx];
    if (!doc.IsLinked()) return;

    ImGui::Spacing();
    ImGui::SeparatorText("Linked NPC");

    ImGui::TextDisabled("%-8s %s", "Race:",   doc.raceEditorId.empty()  ? "\xe2\x80\x94" : doc.raceEditorId.c_str());
    ImGui::TextDisabled("%-8s %s", "Sex:",    doc.isFemale ? "Female" : "Male");
    ImGui::TextDisabled("%-8s %s", "Source:", doc.pluginSource.empty() ? "\xe2\x80\x94" : doc.pluginSource.c_str());
    ImGui::TextDisabled("%-8s %08X", "FormID:", doc.formId);

    ImGui::Spacing();
    if (ImGui::SmallButton("Unlink##isp"))
        const_cast<ActorDocument&>(doc).Unlink();
}

// ── Skeleton section ──────────────────────────────────────────────────────────

void InspectorPanel::DrawSkeletonSection(AppState& state, int castIdx)
{
    ActorDocument& doc = state.cast[castIdx];

    if (doc.skeletonIndex >= 0 && doc.skeletonIndex < (int)state.skeletons.size()) {
        const auto& skel = state.skeletons[doc.skeletonIndex];
        ImGui::TextDisabled("%s  \xe2\x80\xa2  %d bones",
            doc.creatureType.empty() ? "unknown" : doc.creatureType.c_str(),
            (int)skel.bones.size());
        ImGui::Spacing();
        if (ImGui::Button("Change Skeleton\xe2\x80\xa6", {-1.f, 0.f})) {
            showSkelPicker_ = true;
            skelPickerIdx_  = -1;
            skelFilterBuf_[0] = '\0';
            skelErr_[0] = '\0';
        }
    } else {
        ImGui::TextColored({0.9f, 0.5f, 0.3f, 1.f}, "No skeleton assigned");
        ImGui::Spacing();
        if (!state.discoveredSkeletons.empty()) {
            if (ImGui::Button("Pick Skeleton\xe2\x80\xa6", {-1.f, 0.f})) {
                showSkelPicker_ = true;
                skelPickerIdx_  = -1;
                skelFilterBuf_[0] = '\0';
                skelErr_[0] = '\0';
            }
        } else {
            ImGui::TextDisabled("Set a Data Folder to browse skeletons.");
        }
    }

    if (skelErr_[0])
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", skelErr_);
}

// ── Skeleton picker modal ─────────────────────────────────────────────────────

void InspectorPanel::DrawSkeletonPickerModal(AppState& state, int castIdx)
{
    if (showSkelPicker_) {
        ImGui::OpenPopup("Pick Skeleton##isp");
        showSkelPicker_ = false;
    }

    ImGui::SetNextWindowSize({420.f, 420.f}, ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Pick Skeleton##isp", nullptr,
                                ImGuiWindowFlags_NoResize)) return;

    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##isp_sk_filter", "Filter by name or type\xe2\x80\xa6",
                             skelFilterBuf_, sizeof(skelFilterBuf_));
    std::string filterLow = skelFilterBuf_;
    std::transform(filterLow.begin(), filterLow.end(), filterLow.begin(), ::tolower);

    ImGui::BeginChild("##isp_sk_list", {-1.f, -52.f}, true);
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
        std::snprintf(rowId, sizeof(rowId), "%s##ispsk%d", ds.displayName.c_str(), i);
        if (ImGui::Selectable(rowId, sel, ImGuiSelectableFlags_AllowDoubleClick)) {
            skelPickerIdx_ = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                doConfirm = true;
        }
        if (ImGui::IsItemHovered()) {
            if (ds.bsaInternal.empty())
                ImGui::SetTooltip("%s", ds.path.c_str());
            else {
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
    if ((ImGui::Button("OK", {80.f, 0.f}) || doConfirm) && canOK && castIdx >= 0) {
        const DiscoveredSkeleton& ds = state.discoveredSkeletons[skelPickerIdx_];
        skelErr_[0] = '\0';
        state.AssignSkeletonToCast(castIdx, ds, skelErr_, sizeof(skelErr_));
        if (!skelErr_[0]) ImGui::CloseCurrentPopup();
    }
    if (!canOK) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", {80.f, 0.f}))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

// ── Meshes section ────────────────────────────────────────────────────────────
// Shows all body-slot NIFs (body, hands, feet, head) in a compact two-column
// table, then a collapsible head-parts list.  Browse/Clear is only offered for
// unlinked actors (where the body NIF must be chosen manually).

void InspectorPanel::DrawMeshesSection(AppState& state, int castIdx)
{
    ActorDocument& doc   = state.cast[castIdx];
    const bool     linked = doc.IsLinked();

    // Helper: one slot row — label | filename (tooltip = full path)
    auto SlotRow = [](const char* label, const std::string& path) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", label);
        ImGui::TableSetColumnIndex(1);
        if (path.empty()) {
            ImGui::TextDisabled("-");
        } else {
            std::string fn = std::filesystem::path(path).filename().string();
            ImGui::TextDisabled("%s", fn.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", path.c_str());
        }
    };

    constexpr ImGuiTableFlags kTblFlags =
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
    if (ImGui::BeginTable("##isp_meshes", 2, kTblFlags)) {
        ImGui::TableSetupColumn("##lbl", ImGuiTableColumnFlags_WidthFixed,   46.f);
        ImGui::TableSetupColumn("##val", ImGuiTableColumnFlags_WidthStretch);
        SlotRow("Body",  doc.bodyNifPath);
        SlotRow("Hands", doc.handsNifPath);
        SlotRow("Feet",  doc.feetNifPath);
        SlotRow("Head",  doc.headNifPath);
        ImGui::EndTable();
    }

    // Browse / Clear — only for manually-managed (unlinked) actors
    if (!linked) {
        ImGui::Spacing();
        const float browseW = doc.bodyNifPath.empty() ? -1.f : -60.f;
        if (ImGui::Button("Browse Body NIF\xe2\x80\xa6", {browseW, 0.f})) {
#if defined(_WIN32)
            char buf[512] = {};
            OPENFILENAMEA ofn   = {};
            ofn.lStructSize     = sizeof(ofn);
            ofn.lpstrFilter     = "NIF Files\0*.nif\0All Files\0*.*\0";
            ofn.lpstrFile       = buf;
            ofn.nMaxFile        = sizeof(buf);
            ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            ofn.lpstrTitle      = "Select Body NIF";
            if (GetOpenFileNameA(&ofn)) {
                doc.bodyNifPath    = buf;
                state.projectDirty = true;
            }
#endif
        }
        if (!doc.bodyNifPath.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Clear##isp_nif")) {
                doc.bodyNifPath.clear();
                state.projectDirty = true;
            }
        }
    }

    // Head parts — collapsible list
    if (!doc.headPartNifs.empty()) {
        ImGui::Spacing();
        char hdrBuf[64];
        std::snprintf(hdrBuf, sizeof(hdrBuf),
                      "Head Parts (%d)", (int)doc.headPartNifs.size());
        if (ImGui::TreeNodeEx(hdrBuf, ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (const auto& hp : doc.headPartNifs) {
                std::string fn = std::filesystem::path(hp).filename().string();
                ImGui::TextDisabled("  %s", fn.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", hp.c_str());
            }
            ImGui::TreePop();
        }
    }
}

// ── Face Morphs section ───────────────────────────────────────────────────────
// Lazily loads the actor's extended ARKit TRI files (from extendedTriPaths) on
// first draw, then shows a filterable slider list for all morph targets.
// Weights are stored in ActorDocument::morphWeights keyed by full morph name.

void InspectorPanel::DrawFaceMorphsSection(AppState& state, int castIdx)
{
    ActorDocument& doc = state.cast[castIdx];

    // Section only appears when the actor has any expression TRI paths
    // (either vanilla or MFEE-extended).
    if (doc.expressionTriPaths.empty()) return;

    ImGui::Spacing();
    ImGui::SeparatorText("Face Morphs");

    // ── MFEE status / Reload button ───────────────────────────────────────────
    if (doc.extendedTriPaths.empty()) {
        ImGui::TextDisabled("%d vanilla TRI path(s) — no MFEE mapping yet",
                            (int)doc.expressionTriPaths.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload##mfee")) {
            state.RemapMfeeForActor(castIdx);
        }
        if (doc.extendedTriPaths.empty()) {
            // Still nothing after remap attempt — nothing to show
            ImGui::TextDisabled("MFEE config not found in Data Folder.");
            return;
        }
    }

    // ── Lazy load extended TRIs ───────────────────────────────────────────────
    if (!doc.triDocsLoaded) {
        doc.triDocsLoaded = true;
        for (const auto& triPath : doc.extendedTriPaths) {
            std::vector<uint8_t> bytes;
            if (state.ResolveAsset(triPath, bytes)) {
                TriDocument td = LoadTriDocumentFromBytes(bytes, triPath);
                if (!td.empty())
                    doc.triDocs.push_back(std::move(td));
            } else {
                fprintf(stderr, "[Inspector] TRI not resolved: '%s'\n", triPath.c_str());
            }
        }
    }

    if (doc.triDocs.empty()) {
        ImGui::TextDisabled("TRI files not found — check Data Folder / BSA list.");
        return;
    }

    // ── Build deduplicated, sorted morph list ─────────────────────────────────
    // Pair: (displayName, fullName)
    struct Entry { std::string disp, full; };
    std::vector<Entry> morphList;
    morphList.reserve(64);
    for (const auto& td : doc.triDocs) {
        for (const auto& m : td.morphs) {
            bool dup = false;
            for (const auto& e : morphList)
                if (e.full == m.name) { dup = true; break; }
            if (dup) continue;
            std::string disp = m.name;
            const auto pipe = m.name.find('|');
            if (pipe != std::string::npos)
                disp = m.name.substr(pipe + 1);
            morphList.push_back({ std::move(disp), m.name });
        }
    }
    std::sort(morphList.begin(), morphList.end(),
        [](const Entry& a, const Entry& b){ return a.disp < b.disp; });

    // Summary + filter
    char sumBuf[80];
    std::snprintf(sumBuf, sizeof(sumBuf), "%d morphs, %d TRI file(s)",
                  (int)morphList.size(), (int)doc.triDocs.size());
    ImGui::TextDisabled("%s", sumBuf);

    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##morph_flt", "Filter...",
                             morphFilterBuf_, sizeof(morphFilterBuf_));
    std::string filterLow = morphFilterBuf_;
    std::transform(filterLow.begin(), filterLow.end(), filterLow.begin(), ::tolower);

    // ── Slider list ───────────────────────────────────────────────────────────
    // Label column is a fixed 120 px offset; slider fills the rest.
    // We use SameLine(kLabelX) instead of BeginTable to guarantee the stretch
    // column fills the child window regardless of ImGui table sizing rules.
    constexpr float kLabelX = 120.f;

    const float lineH  = ImGui::GetTextLineHeightWithSpacing();
    const float childH = std::min(static_cast<float>(morphList.size()), 14.f) * lineH + 4.f;
    ImGui::BeginChild("##morph_list", { -1.f, childH }, true);

    for (const auto& e : morphList) {
        if (!filterLow.empty()) {
            std::string hay = e.disp;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (hay.find(filterLow) == std::string::npos) continue;
        }

        float weight = 0.f;
        auto  it     = doc.morphWeights.find(e.full);
        if (it != doc.morphWeights.end()) weight = it->second;

        // Label — yellow tint when active (non-zero)
        const bool active = (weight != 0.f);
        ImGui::AlignTextToFramePadding();
        if (active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 0.6f, 1.f));
        ImGui::TextUnformatted(e.disp.c_str());
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", e.full.c_str()); // show full name on hover

        // Slider — positioned at fixed offset so it fills the remaining width
        ImGui::SameLine(kLabelX);
        ImGui::SetNextItemWidth(-1.f);
        char sliderID[160];
        std::snprintf(sliderID, sizeof(sliderID), "##ms_%s", e.full.c_str());
        if (ImGui::SliderFloat(sliderID, &weight, 0.f, 1.f, "%.2f")) {
            if (weight == 0.f) doc.morphWeights.erase(e.full);
            else               doc.morphWeights[e.full] = weight;
            state.projectDirty = true;
        }
    }

    ImGui::EndChild();

    // Reset all
    if (!doc.morphWeights.empty()) {
        if (ImGui::SmallButton("Reset All##isp_morph")) {
            doc.morphWeights.clear();
            state.projectDirty = true;
        }
    }
}
