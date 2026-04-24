#include "ClipBinPanel.h"
#include <imgui.h>

void ClipBinPanel::Draw() {
    ImGui::Begin("Clip Bin");

    ImGui::SeparatorText("Animations");

    // Stub entries — will be replaced by live project data in Phase 7
    struct Entry { const char* icon; const char* name; const char* duration; };
    static const Entry kAnimEntries[] = {
        { "\xf0\x9f\x8e\xac", "mt_idle",            "9.13s" },  // emoji fallback → text below
        { "",                  "mt_walk_f",           "1.03s" },
        { "",                  "mt_run",              "0.76s" },
        { "",                  "taunt_01",            "3.20s" },
        { "",                  "draw_weapon",         "0.90s" },
    };
    static const Entry kFaceEntries[] = {
        { "", "capture_take_01",   "4.50s" },
        { "", "capture_take_02",   "2.10s" },
    };

    ImGui::PushStyleColor(ImGuiCol_Header,
                          ImVec4(0.22f, 0.30f, 0.50f, 1.0f));

    for (const auto& e : kAnimEntries) {
        ImGui::Selectable(e.name);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 36.0f);
        ImGui::TextDisabled("%s", e.duration);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Face Mocap");

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.60f, 0.85f, 1.0f));
    for (const auto& e : kFaceEntries) {
        ImGui::Selectable(e.name);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 36.0f);
        ImGui::TextDisabled("%s", e.duration);
    }
    ImGui::PopStyleColor();

    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("+ Import Clip", { -1.0f, 0.0f })) {}

    ImGui::End();
}
