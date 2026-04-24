#include "TimelinePanel.h"
#include <imgui.h>
#include <cmath>

void TimelinePanel::Draw() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Timeline");
    ImGui::PopStyleVar();

    ImVec2 size = ImGui::GetContentRegionAvail();
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      pos = ImGui::GetCursorScreenPos();

    // Background
    dl->AddRectFilled(pos, { pos.x + size.x, pos.y + size.y },
                      IM_COL32(13, 14, 17, 255));

    // Draw representative track rows to show the future layout
    const float labelW  = 100.0f;
    const float rowH    = 22.0f;
    const float headerH = 20.0f;

    if (size.x > 120 && size.y > 80) {
        // Header bar (time ruler stub)
        dl->AddRectFilled(pos, { pos.x + size.x, pos.y + headerH },
                          IM_COL32(20, 22, 28, 255));
        dl->AddLine({ pos.x, pos.y + headerH },
                    { pos.x + size.x, pos.y + headerH },
                    IM_COL32(50, 55, 70, 255));

        // Tick marks every 60px
        for (float x = pos.x + labelW; x < pos.x + size.x; x += 60.0f) {
            dl->AddLine({ x, pos.y }, { x, pos.y + headerH },
                        IM_COL32(50, 55, 70, 200));
        }

        struct TrackRow { const char* label; ImU32 blockCol; float blockStart; float blockEnd; };
        TrackRow rows[] = {
            { "Camera",   IM_COL32(160, 120, 40,  180), 0.0f,  0.85f },
            { "Dialogue", IM_COL32(80,  100, 160, 180), 0.02f, 0.90f },
            { "--- AUXESIA ---", 0, 0, 0 },
            { "  Face",   IM_COL32(180, 80,  140, 180), 0.0f,  0.55f },
            { "  LookAt", IM_COL32(80,  160, 120, 180), 0.0f,  0.35f },
            { "  Anim",   IM_COL32(60,  100, 200, 180), 0.05f, 0.70f },
            { "  Voice",  IM_COL32(60,  160, 160, 180), 0.0f,  0.88f },
        };

        float y = pos.y + headerH;
        const float trackW = size.x - labelW;

        for (auto& row : rows) {
            // Row background (alternating)
            const ImU32 bg = (&row - rows) % 2 == 0
                ? IM_COL32(16, 18, 22, 255)
                : IM_COL32(19, 21, 26, 255);
            dl->AddRectFilled({ pos.x, y }, { pos.x + size.x, y + rowH }, bg);

            // Label
            dl->AddText({ pos.x + 6, y + 5 }, IM_COL32(160, 170, 185, 200), row.label);

            // Clip block (skip separator rows)
            if (row.blockCol != 0 && trackW > 0) {
                float bx0 = pos.x + labelW + row.blockStart * trackW;
                float bx1 = pos.x + labelW + row.blockEnd   * trackW;
                dl->AddRectFilled({ bx0, y + 3 }, { bx1, y + rowH - 3 },
                                  row.blockCol, 3.0f);
            }

            // Row separator
            dl->AddLine({ pos.x, y + rowH }, { pos.x + size.x, y + rowH },
                        IM_COL32(30, 33, 40, 255));

            y += rowH;
            if (y > pos.y + size.y) break;
        }

        // Playhead at 25%
        const float phx = pos.x + labelW + trackW * 0.25f;
        dl->AddLine({ phx, pos.y }, { phx, pos.y + size.y },
                    IM_COL32(240, 200, 60, 220), 1.5f);
        dl->AddTriangleFilled(
            { phx - 5, pos.y }, { phx + 5, pos.y }, { phx, pos.y + 10 },
            IM_COL32(240, 200, 60, 220));
    }

    ImGui::Dummy(size);
    ImGui::End();
}
