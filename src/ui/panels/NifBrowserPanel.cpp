#include "NifBrowserPanel.h"
#include "AppState.h"
#include <imgui.h>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace fs = std::filesystem;

// ── Scan ──────────────────────────────────────────────────────────────────────

void NifBrowserPanel::Scan(const std::string& dataFolder)
{
    m_entries.clear();
    m_scannedDataFolder = dataFolder;

    const fs::path meshRoot = fs::path(dataFolder) / "meshes";
    if (!fs::exists(meshRoot)) return;

    for (const auto& de : fs::recursive_directory_iterator(
             meshRoot, fs::directory_options::skip_permission_denied)) {
        if (!de.is_regular_file()) continue;
        if (de.path().extension() != ".nif" &&
            de.path().extension() != ".NIF") continue;

        Entry e;
        e.fullPath = de.path().string();

        // Label relative to meshes/ folder, forward-slashes.
        std::string rel = fs::relative(de.path(), meshRoot).string();
        for (char& c : rel) if (c == '\\') c = '/';
        e.label = std::move(rel);

        e.lowerLabel = e.label;
        for (char& c : e.lowerLabel)
            c = (char)std::tolower((unsigned char)c);

        m_entries.push_back(std::move(e));
    }

    std::sort(m_entries.begin(), m_entries.end(),
              [](const Entry& a, const Entry& b){ return a.label < b.label; });
}

// ── Tree draw ─────────────────────────────────────────────────────────────────
// Reconstruct a folder hierarchy from flat entry list by tracking the current
// path prefix and opening/closing TreeNodes as we descend/ascend.

void NifBrowserPanel::DrawTree(AppState& state)
{
    char lowerFilter[128];
    size_t flen = strlen(m_filter);
    for (size_t i = 0; i <= flen; i++)
        lowerFilter[i] = (char)std::tolower((unsigned char)m_filter[i]);

    // Folder stack: pair of (folderName, wasOpen).
    // wasOpen == false means TreeNodeEx returned false; children of that node
    // must be skipped without calling TreePop when we leave the level.
    struct FolderLevel { std::string name; bool open; };
    std::vector<FolderLevel> stack;

    auto closeTo = [&](int targetDepth) {
        while ((int)stack.size() > targetDepth) {
            if (stack.back().open) ImGui::TreePop();
            stack.pop_back();
        }
    };

    for (const Entry& e : m_entries) {
        if (lowerFilter[0] && e.lowerLabel.find(lowerFilter) == std::string::npos)
            continue;

        // Split into parts (folders + filename).
        std::vector<std::string> parts;
        {
            std::string tok;
            for (char c : e.label) {
                if (c == '/') { if (!tok.empty()) { parts.push_back(tok); tok.clear(); } }
                else tok += c;
            }
            if (!tok.empty()) parts.push_back(tok);
        }
        if (parts.empty()) continue;

        // Find common prefix with current stack.
        int common = 0;
        int folders = (int)parts.size() - 1;
        for (int d = 0; d < folders && d < (int)stack.size(); d++) {
            if (stack[d].name == parts[d]) common++;
            else break;
        }

        closeTo(common);

        // If any ancestor in the current stack is closed, skip this entry.
        bool ancestorClosed = false;
        for (int d = 0; d < common; d++) {
            if (!stack[d].open) { ancestorClosed = true; break; }
        }
        if (ancestorClosed) continue;

        // Open new folder levels.
        bool allOpen = true;
        for (int d = common; d < folders; d++) {
            if (!allOpen) {
                // A parent was closed; push a closed placeholder so closeTo works.
                stack.push_back({ parts[d], false });
                continue;
            }
            bool nodeOpen = ImGui::TreeNodeEx(parts[d].c_str(),
                ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow);
            stack.push_back({ parts[d], nodeOpen });
            if (!nodeOpen) allOpen = false;
        }

        if (!allOpen) continue;

        // Draw file leaf.
        const char* fname = parts.back().c_str();
        const bool  sel   = (!m_s.doc.path.empty() && m_s.doc.path == e.fullPath);
        if (ImGui::Selectable(fname, sel,
                ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                m_s.LoadFile(e.fullPath, state);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", e.label.c_str());
    }

    closeTo(0);
}

// ── IPanel::Draw ──────────────────────────────────────────────────────────────

void NifBrowserPanel::Draw(AppState& state)
{
    if (!ImGui::Begin(PanelID())) { ImGui::End(); return; }

    const std::string& df = state.dataFolder;

    if (df.empty()) {
        ImGui::TextDisabled("Set a Data folder in the Scene Editor tab");
        ImGui::End();
        return;
    }

    // Re-scan when data folder changes.
    if (m_scannedDataFolder != df)
        Scan(df);

    // Refresh button + filter.
    if (ImGui::Button("Refresh"))
        Scan(df);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputText("##filter", m_filter, sizeof(m_filter));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Filter NIF paths (case-insensitive)");

    ImGui::Separator();

    if (m_entries.empty()) {
        ImGui::TextDisabled("No .nif files found under meshes/");
    } else {
        ImGui::BeginChild("##niflist", ImVec2(0, 0), false);
        DrawTree(state);
        ImGui::EndChild();
    }

    ImGui::End();
}
