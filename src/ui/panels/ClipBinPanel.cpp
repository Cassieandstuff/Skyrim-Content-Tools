#include "ClipBinPanel.h"
#include "AppState.h"
#include "FaceClip.h"
#include "Sequence.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

// ── IPanel::Draw ──────────────────────────────────────────────────────────────

void BinPanel::Draw(AppState& state)
{
    ImGui::Begin(PanelID());

    if (ImGui::BeginTabBar("##bin_tabs")) {
        if (ImGui::BeginTabItem("Clips")) {
            DrawClipsTab(state);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Face")) {
            DrawFaceClipsTab(state);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Cast")) {
            DrawCastTab(state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ── Clips tab ─────────────────────────────────────────────────────────────────

void BinPanel::DrawClipsTab(AppState& state)
{
    std::vector<std::string> types;
    for (const auto& c : state.clips) {
        if (std::find(types.begin(), types.end(), c.skeletonType) == types.end())
            types.push_back(c.skeletonType);
    }

    const bool showHeaders = (types.size() > 1 ||
                              (!types.empty() && !types[0].empty()));

    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.22f, 0.30f, 0.50f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.30f, 0.40f, 0.65f, 1.0f));

    if (state.clips.empty())
        ImGui::TextDisabled("No clips imported.");

    for (const auto& type : types) {
        if (showHeaders) {
            std::string label = type.empty() ? "Unknown" : type;
            label[0] = (char)std::toupper((unsigned char)label[0]);
            ImGui::SeparatorText(label.c_str());
        }

        for (int i = 0; i < (int)state.clips.size(); i++) {
            const AnimClip& c = state.clips[i];
            if (c.skeletonType != type) continue;

            bool selected = (state.selectedClip == i);
            if (ImGui::Selectable(c.name.c_str(), selected,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                state.selectedClip = i;
                state.time         = 0.f;
                state.playing      = false;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    AddSelectedClipToTimeline(state);
            }

            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("SCT_CLIP", &i, sizeof(int));
                ImGui::Text("%.2fs  \xe2\x80\x94  %s", c.duration, c.name.c_str());
                ImGui::EndDragDropSource();
            }

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 44.f);
            ImGui::TextDisabled("%.2fs", c.duration);
        }
    }

    ImGui::PopStyleColor(2);

    if (state.importErr[0]) {
        ImGui::Spacing();
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", state.importErr);
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("+ Import Clip", {-1.f, 0.f}))
        OpenImportDialog(state);
}

void BinPanel::OpenImportDialog(AppState& state)
{
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    OPENFILENAMEA ofn  = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Havok Files\0*.hkx;*.xml\0Havok Binary\0*.hkx\0Havok XML\0*.xml\0All Files\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = sizeof(buf);
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle  = "Import Animation";
    if (!GetOpenFileNameA(&ofn)) return;

    state.importErr[0] = '\0';
    const int idx = state.LoadClipFromPath(buf, state.importErr, sizeof(state.importErr));
    if (idx >= 0) {
        state.selectedClip = idx;
        state.importErr[0] = '\0';
        state.time         = 0.f;
        state.playing      = false;
        state.projectDirty = true;
    }
#endif
}

void BinPanel::AddSelectedClipToTimeline(AppState& state)
{
    if (state.selectedClip < 0 || state.selectedClip >= (int)state.clips.size()) return;
    if (state.actors.empty()) return;

    const int targetActor = 0;
    state.sequence.EnsureActorGroup(targetActor);

    ActorTrackGroup* grp = nullptr;
    for (auto& g : state.sequence.actorTracks)
        if (g.actorIndex == targetActor) { grp = &g; break; }
    if (!grp) return;

    TrackLane* animLane = nullptr;
    for (auto& lane : grp->lanes)
        if (lane.type == TrackType::AnimClip) { animLane = &lane; break; }
    if (!animLane) return;

    const AnimClip& clip = state.clips[state.selectedClip];

    float placeAt = 0.f;
    for (const auto& item : animLane->items)
        placeAt = std::max(placeAt, item.SeqEnd());

    SequenceItem item;
    item.assetIndex = state.selectedClip;
    item.seqStart   = placeAt;
    item.trimIn     = 0.f;
    item.trimOut    = clip.duration;
    item.blendIn    = 0.f;
    item.blendOut   = 0.f;

    animLane->items.push_back(item);

    state.time    = placeAt;
    state.playing = false;
}

// ── Face Clips tab ────────────────────────────────────────────────────────────

void BinPanel::DrawFaceClipsTab(AppState& state)
{
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.22f, 0.45f, 0.35f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.30f, 0.58f, 0.46f, 1.0f));

    if (state.faceClips.empty()) {
        ImGui::TextDisabled("No face clips imported.");
        ImGui::Spacing();
        ImGui::TextWrapped("Face clips are extracted automatically when you import a "
                           "body animation HKX that contains MorphFace annotations. "
                           "You can also import a face-only HKX below.");
    }

    for (int i = 0; i < (int)state.faceClips.size(); i++) {
        const FaceClip& fc = state.faceClips[i];
        bool selected = (selectedFaceClip_ == i);

        if (ImGui::Selectable(fc.name.c_str(), selected)) {
            selectedFaceClip_ = i;
        }

        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("SCT_FACE_CLIP", &i, sizeof(int));
            ImGui::Text("%.2fs  [%d ch]  \xe2\x80\x94  %s",
                        fc.duration, (int)fc.channels.size(), fc.name.c_str());
            ImGui::EndDragDropSource();
        }

        // Right-aligned metadata.
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.f);
        ImGui::TextDisabled("%.2fs", fc.duration);
        ImGui::SameLine();
        ImGui::TextDisabled("[%d ch]", (int)fc.channels.size());

        // Tooltip listing all morph channel names.
        if (ImGui::IsItemHovered() && !fc.channels.empty()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("ARKit morph channels:");
            for (const auto& ch : fc.channels) {
                int n = (int)ch.times.size();
                ImGui::Text("  %-28s  %d keys", ch.morphName.c_str(), n);
            }
            ImGui::EndTooltip();
        }
    }

    ImGui::PopStyleColor(2);

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("+ Import Face Clip", {-1.f, 0.f}))
        OpenImportFaceDialog(state);
}

void BinPanel::OpenImportFaceDialog(AppState& state)
{
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    OPENFILENAMEA ofn  = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Havok Files\0*.hkx;*.xml\0Havok Binary\0*.hkx\0Havok XML\0*.xml\0All Files\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = sizeof(buf);
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle  = "Import Face Animation";
    if (!GetOpenFileNameA(&ofn)) return;

    char errBuf[256] = {};
    const int idx = state.LoadFaceClipFromPath(buf, errBuf, sizeof(errBuf));
    if (idx >= 0) {
        selectedFaceClip_  = idx;
        state.projectDirty = true;
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Face clip imported — %d morph channels",
                      (int)state.faceClips[idx].channels.size());
        state.PushToast(msg, ToastLevel::Info);
    } else {
        state.PushToast(errBuf[0] ? errBuf : "Face clip import failed", ToastLevel::Error);
    }
#endif
}

// ── Cast tab ──────────────────────────────────────────────────────────────────

void BinPanel::DrawCastTab(AppState& state)
{
    // ── Data folder ───────────────────────────────────────────────────────────
    ImGui::SeparatorText("Data Folder");

    if (state.dataFolder.empty()) {
        ImGui::TextDisabled("(not set)");
    } else {
        const char* display = state.dataFolder.c_str();
        const float avail   = ImGui::GetContentRegionAvail().x - 60.f;
        while (*display && ImGui::CalcTextSize(display).x > avail)
            display++;
        ImGui::TextDisabled("%s%s", display == state.dataFolder.c_str() ? "" : "...", display);
        if (!state.discoveredSkeletons.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("[%d skel]", (int)state.discoveredSkeletons.size());
        }
        if (!state.discoveredPlugins.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("[%d plugins]", (int)state.discoveredPlugins.size());
        }
    }

    if (ImGui::Button("Browse...", {-1.f, 0.f})) {
#if defined(_WIN32)
        const HRESULT hrCom = CoInitializeEx(nullptr,
            COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        char pathBuf[MAX_PATH] = {};
        BROWSEINFOA bi  = {};
        bi.lpszTitle    = "Select your Skyrim Special Edition Data folder";
        bi.ulFlags      = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (pidl) {
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
    ImGui::SeparatorText("Characters");

    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.30f, 0.22f, 0.50f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.42f, 0.30f, 0.65f, 1.0f));

    for (int i = 0; i < (int)state.cast.size(); i++) {
        const ActorDocument& entry = state.cast[i];

        // ● = placed in scene, ○ = cast-only
        bool placed = false;
        for (const auto& a : state.actors)
            if (a.castIndex == i) { placed = true; break; }

        bool selected = (state.selectedCast == i);
        char label[256];
        std::snprintf(label, sizeof(label), "%s %s##cast%d",
                      placed ? "\xe2\x97\x8f" : "\xe2\x97\x8b",
                      entry.name.empty() ? "(unnamed)" : entry.name.c_str(), i);
        if (ImGui::Selectable(label, selected))
            state.selectedCast = i;

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Properties"))
                state.selectedCast = i;
            ImGui::Separator();
            if (ImGui::MenuItem("Remove")) {
                state.actors.erase(
                    std::remove_if(state.actors.begin(), state.actors.end(),
                        [i](const Actor& a){ return a.castIndex == i; }),
                    state.actors.end());
                state.cast.erase(state.cast.begin() + i);
                if (state.selectedCast == i)      state.selectedCast = -1;
                else if (state.selectedCast > i) --state.selectedCast;
            }
            ImGui::EndPopup();
        }

        if (i < (int)state.cast.size()) {
            if (entry.skeletonIndex >= 0 && entry.skeletonIndex < (int)state.skeletons.size()) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.f);
                ImGui::TextDisabled("%d bones",
                    (int)state.skeletons[entry.skeletonIndex].bones.size());
            } else {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.f);
                ImGui::TextColored({0.9f, 0.5f, 0.3f, 1.f}, "no skel");
            }
        }
    }

    ImGui::PopStyleColor(2);
}
