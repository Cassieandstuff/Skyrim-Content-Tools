#include "ViewportPanel.h"
#include <imgui.h>

void ViewportPanel::Draw() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");
    ImGui::PopStyleVar();

    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 1) size.x = 1;
    if (size.y < 1) size.y = 1;

    // Draw a dark background with a subtle grid hint and placeholder text.
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      pos = ImGui::GetCursorScreenPos();
    ImVec2      end = { pos.x + size.x, pos.y + size.y };

    // Background
    dl->AddRectFilled(pos, end, IM_COL32(12, 13, 16, 255));

    // Faint grid lines (every 40px) to suggest the floor plane
    const float grid = 40.0f;
    const ImU32 gridCol = IM_COL32(30, 35, 45, 255);
    for (float x = pos.x; x < end.x; x += grid)
        dl->AddLine({ x, pos.y }, { x, end.y }, gridCol);
    for (float y = pos.y; y < end.y; y += grid)
        dl->AddLine({ pos.x, y }, { end.x, y }, gridCol);

    // Border
    dl->AddRect(pos, end, IM_COL32(40, 55, 90, 200));

    // Centred placeholder label
    const char* label = "3D Viewport  —  Phase 2";
    ImVec2      ts    = ImGui::CalcTextSize(label);
    dl->AddText(
        { pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f },
        IM_COL32(60, 90, 140, 200), label);

    ImGui::Dummy(size); // consume layout space

    ImGui::End();
}
