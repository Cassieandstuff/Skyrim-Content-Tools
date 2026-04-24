#include "NifEditorPanel.h"
#include <imgui.h>

void NifEditorPanel::Draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove     |
        ImGuiWindowFlags_NoDocking  | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.11f, 0.12f, 1.0f));
    ImGui::Begin("##NifEditorFull", nullptr, kFlags);
    ImGui::PopStyleColor();

    ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPos({
        (size.x - 200.0f) * 0.5f,
        (size.y -  40.0f) * 0.5f
    });
    ImGui::TextDisabled("NIF Editor  —  coming soon");

    ImGui::End();
}
