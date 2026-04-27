#include "AnimatorPanel.h"
#include "AppState.h"
#include <imgui.h>

void AnimatorPanel::Draw(AppState& /*state*/)
{
    ImGui::Begin(PanelID());
    ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPos({ (size.x - 200.f) * 0.5f, (size.y - 40.f) * 0.5f });
    ImGui::TextDisabled("Animator  —  coming soon");
    ImGui::End();
}
