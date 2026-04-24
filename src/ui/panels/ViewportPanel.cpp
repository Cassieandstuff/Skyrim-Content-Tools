#include "ViewportPanel.h"
#include <imgui.h>
#include <glm/glm.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

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

void ViewportPanel::OpenSkeletonDialog()
{
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    OPENFILENAMEA ofn  = {};
    ofn.lStructSize    = sizeof(ofn);
    ofn.lpstrFilter    = "Havok XML\0*.xml\0All Files\0*.*\0";
    ofn.lpstrFile      = buf;
    ofn.nMaxFile       = sizeof(buf);
    ofn.Flags          = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle     = "Open skeleton.xml";
    if (!GetOpenFileNameA(&ofn)) return;

    Skeleton sk;
    if (LoadHavokSkeletonXml(buf, sk, m_loadErr, sizeof(m_loadErr))) {
        m_skeleton = std::move(sk);
        m_loadErr[0] = '\0';
        FrameSkeleton();
    }
#endif
}

void ViewportPanel::FrameSkeleton()
{
    // Find AABB of world positions and frame the camera around it
    if (m_skeleton.empty()) return;
    glm::vec3 mn = m_skeleton.worldPos[0];
    glm::vec3 mx = m_skeleton.worldPos[0];
    for (auto& p : m_skeleton.worldPos) {
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }
    glm::vec3 center = (mn + mx) * 0.5f;
    float     extent = glm::length(mx - mn) * 0.5f;

    m_camera.target    = center;
    m_camera.radius    = extent * 2.5f;
    m_camera.azimuth   = 30.0f;
    m_camera.elevation = 10.0f;
}

void ViewportPanel::Draw()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin("Viewport");
    ImGui::PopStyleVar();
    if (!open) { ImGui::End(); return; }

    // ── Toolbar ───────────────────────────────────────────────────────────────
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
    if (ImGui::Button("Load Skeleton XML..."))
        OpenSkeletonDialog();
    if (m_loadErr[0]) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s", m_loadErr);
    } else if (!m_skeleton.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d bones", static_cast<int>(m_skeleton.bones.size()));
    }
    ImGui::Separator();

    // ── 3D viewport ───────────────────────────────────────────────────────────
    const ImVec2 p0   = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();

    ImGui::InvisibleButton("##vp", size,
        ImGuiButtonFlags_MouseButtonLeft  |
        ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);

    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGuiIO& io = ImGui::GetIO();
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

    dl->AddRectFilled(p0, {p0.x + size.x, p0.y + size.y}, IM_COL32(28, 33, 51, 255));

    const glm::mat4 vp = m_camera.Proj(size.x / size.y) * m_camera.View();

    auto proj = [&](glm::vec3 w, ImVec2& out) {
        return Project(vp, w, p0, size, out);
    };

    // ── Grid (scales with camera radius so it stays readable) ─────────────────
    {
        float unit  = 1.0f;
        if (m_camera.radius > 100.f) unit = 50.f;
        else if (m_camera.radius > 20.f) unit = 10.f;
        constexpr int kHalf = 10;
        const ImU32 kGrid = IM_COL32(90, 90, 115, 255);
        for (int i = -kHalf; i <= kHalf; ++i) {
            if (i == 0) continue;
            const float f = static_cast<float>(i) * unit;
            const float e = static_cast<float>(kHalf) * unit;
            ImVec2 a, b;
            if (proj({f, 0.f, -e}, a) && proj({f, 0.f, e}, b)) dl->AddLine(a, b, kGrid);
            if (proj({-e, 0.f, f}, a) && proj({e, 0.f, f}, b)) dl->AddLine(a, b, kGrid);
        }
        ImVec2 a, b;
        if (proj({-10.f * unit, 0.f, 0.f}, a) && proj({10.f * unit, 0.f, 0.f}, b))
            dl->AddLine(a, b, IM_COL32(210, 55, 55, 255), 2.f);
        if (proj({0.f, 0.f, -10.f * unit}, a) && proj({0.f, 0.f, 10.f * unit}, b))
            dl->AddLine(a, b, IM_COL32(55, 110, 210, 255), 2.f);
    }

    // ── Skeleton ──────────────────────────────────────────────────────────────
    if (!m_skeleton.empty()) {
        constexpr ImU32 kBone  = IM_COL32(220, 200,  80, 230);
        constexpr ImU32 kJoint = IM_COL32(255, 255, 255, 200);

        const int n = static_cast<int>(m_skeleton.bones.size());

        // Bone sticks
        for (int i = 0; i < n; i++) {
            int p = m_skeleton.bones[i].parent;
            if (p < 0) continue;
            ImVec2 a, b;
            if (proj(m_skeleton.worldPos[i], a) &&
                proj(m_skeleton.worldPos[p], b))
                dl->AddLine(a, b, kBone, 1.5f);
        }

        // Joint dots
        for (int i = 0; i < n; i++) {
            ImVec2 s;
            if (proj(m_skeleton.worldPos[i], s))
                dl->AddCircleFilled(s, 2.5f, kJoint);
        }
    }

    dl->PopClipRect();
    ImGui::End();
}
