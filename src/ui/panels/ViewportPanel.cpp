#include "ViewportPanel.h"
#include <imgui.h>

void ViewportPanel::Draw() {
    if (!m_initialized) {
        m_renderer.Init();
        m_initialized = true;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin("Viewport");
    ImGui::PopStyleVar();

    if (!open) {
        ImGui::End();
        return;
    }

    ImVec2 size = ImGui::GetContentRegionAvail();

    if (size.x > 1.0f && size.y > 1.0f) {
        // ── Mouse input ───────────────────────────────────────────────────────
        // AllowWhenBlockedByActiveItem lets drags continue when ImGui has an
        // active item (e.g. a docking drag on another panel).
        constexpr ImGuiHoveredFlags kHoverFlags =
            ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
            ImGuiHoveredFlags_AllowWhenBlockedByPopup;

        if (ImGui::IsWindowHovered(kHoverFlags)) {
            ImGuiIO& io = ImGui::GetIO();

            // Left drag: orbit
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))
                m_camera.Orbit(io.MouseDelta.x * 0.5f, -io.MouseDelta.y * 0.5f);

            // Right drag: pan
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 1.0f))
                m_camera.Pan(io.MouseDelta.x, -io.MouseDelta.y);

            // Scroll: zoom
            if (io.MouseWheel != 0.0f)
                m_camera.Zoom(io.MouseWheel);
        }

        // ── Render scene to FBO then display as image ─────────────────────────
        m_renderer.Resize(static_cast<int>(size.x), static_cast<int>(size.y));
        m_renderer.Render(m_camera);

        // Flip V: OpenGL origin is bottom-left, ImGui is top-left
        ImGui::Image(
            (ImTextureID)(intptr_t)m_renderer.ColorTexture(),
            size,
            ImVec2(0, 1), ImVec2(1, 0)
        );
    }

    ImGui::End();
}
