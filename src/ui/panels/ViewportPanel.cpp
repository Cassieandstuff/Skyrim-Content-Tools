#include "ViewportPanel.h"
#include <imgui.h>
#include <glm/glm.hpp>

// Project a world-space point to ImGui screen coordinates.
// Returns false (and leaves `out` unchanged) when the point is behind the camera.
static bool Project(const glm::mat4& vp, glm::vec3 w,
                    ImVec2 origin, ImVec2 size, ImVec2& out)
{
    glm::vec4 clip = vp * glm::vec4(w, 1.0f);
    if (clip.w <= 0.01f) return false;
    float nx = clip.x / clip.w;
    float ny = clip.y / clip.w;
    out = {
        origin.x + (nx *  0.5f + 0.5f) * size.x,
        origin.y + (0.5f - ny * 0.5f) * size.y,
    };
    return true;
}

void ViewportPanel::Draw() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin("Viewport");
    ImGui::PopStyleVar();
    if (!open) { ImGui::End(); return; }

    const ImVec2 p0   = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();

    // InvisibleButton owns the content rect so hover/active work reliably
    // even inside a docked window.
    ImGui::InvisibleButton("##vp", size,
        ImGuiButtonFlags_MouseButtonLeft  |
        ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);

    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGuiIO& io = ImGui::GetIO();
        // Blender-style: MMB = orbit, Shift+MMB = pan, scroll = zoom
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f)) {
            if (io.KeyShift)
                m_camera.Pan(io.MouseDelta.x, -io.MouseDelta.y);
            else
                m_camera.Orbit(-io.MouseDelta.x * 0.5f, io.MouseDelta.y * 0.5f);
        }
        if (io.MouseWheel != 0.0f)
            m_camera.Zoom(io.MouseWheel);
    }

    if (size.x < 1.0f || size.y < 1.0f) { ImGui::End(); return; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, {p0.x + size.x, p0.y + size.y}, true);

    // Background
    dl->AddRectFilled(p0, {p0.x + size.x, p0.y + size.y}, IM_COL32(28, 33, 51, 255));

    const glm::mat4 vp = m_camera.Proj(size.x / size.y) * m_camera.View();

    auto proj = [&](glm::vec3 w, ImVec2& out) {
        return Project(vp, w, p0, size, out);
    };

    // ── Minor grid ────────────────────────────────────────────────────────────
    constexpr int  kHalf    = 10;
    constexpr ImU32 kGrid   = IM_COL32(90, 90, 115, 255);

    for (int i = -kHalf; i <= kHalf; ++i) {
        if (i == 0) continue;
        const float f = static_cast<float>(i);
        ImVec2 a, b;
        if (proj({f, 0.f, -(float)kHalf}, a) && proj({f, 0.f, (float)kHalf}, b))
            dl->AddLine(a, b, kGrid);
        if (proj({-(float)kHalf, 0.f, f}, a) && proj({(float)kHalf, 0.f, f}, b))
            dl->AddLine(a, b, kGrid);
    }

    // ── Axis lines ────────────────────────────────────────────────────────────
    {
        ImVec2 a, b;
        if (proj({-(float)kHalf, 0.f, 0.f}, a) && proj({(float)kHalf, 0.f, 0.f}, b))
            dl->AddLine(a, b, IM_COL32(210, 55, 55, 255), 2.f);   // X — red
        if (proj({0.f, 0.f, -(float)kHalf}, a) && proj({0.f, 0.f, (float)kHalf}, b))
            dl->AddLine(a, b, IM_COL32(55, 110, 210, 255), 2.f);  // Z — blue
    }

    dl->PopClipRect();
    ImGui::End();
}
