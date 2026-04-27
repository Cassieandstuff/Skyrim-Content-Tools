#include "SceneGraphPanel.h"
#include "AppState.h"
#include <imgui.h>

void SceneGraphPanel::Draw(AppState& /*state*/)
{
    ImGui::Begin(PanelID());

    ImVec2 size = ImGui::GetContentRegionAvail();
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      pos = ImGui::GetCursorScreenPos();

    dl->AddRectFilled(pos, { pos.x + size.x, pos.y + size.y },
                      IM_COL32(14, 16, 20, 255));

    auto DrawNode = [&](float x, float y, const char* label, ImU32 col) {
        ImVec2 np = { pos.x + x, pos.y + y };
        ImVec2 ne = { np.x + 110.f, np.y + 36.f };
        dl->AddRectFilled(np, ne, IM_COL32(28, 34, 46, 255), 4.f);
        dl->AddRect(np, ne, col, 4.f);
        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText({ np.x + (110.f - ts.x) * 0.5f, np.y + 10.f },
                    IM_COL32(200, 210, 230, 200), label);
    };

    if (size.x > 140 && size.y > 160) {
        const float cx = size.x * 0.5f - 55.f;
        DrawNode(cx - 40,  20, "Entry",  IM_COL32(60, 120, 80,  255));
        DrawNode(cx - 40, 100, "Beat 1", IM_COL32(60, 90,  160, 255));
        DrawNode(cx - 40, 180, "End",    IM_COL32(160, 60, 60,  255));
        const ImU32 kWire = IM_COL32(50, 90, 160, 180);
        dl->AddLine({ pos.x + cx + 15, pos.y +  56 },
                    { pos.x + cx + 15, pos.y + 100 }, kWire, 1.5f);
        dl->AddLine({ pos.x + cx + 15, pos.y + 136 },
                    { pos.x + cx + 15, pos.y + 180 }, kWire, 1.5f);
    }

    ImGui::Dummy(size);
    ImGui::TextDisabled("Scene Graph  —  Phase 7");

    ImGui::End();
}
