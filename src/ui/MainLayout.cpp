#include "MainLayout.h"
#include "panels/SceneGraphPanel.h"
#include "panels/TimelinePanel.h"
#include "panels/ClipBinPanel.h"
#include "panels/NifEditorPanel.h"
#include "panels/AnimatorPanel.h"

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder API

// ── Public ─────────────────────────────────────────────────────────────────

void MainLayout::Draw() {
    // ── Full-screen invisible host window (owns the dockspace) ───────────────
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    constexpr ImGuiWindowFlags kHostFlags =
        ImGuiWindowFlags_NoTitleBar        | ImGuiWindowFlags_NoCollapse   |
        ImGuiWindowFlags_NoResize          | ImGuiWindowFlags_NoMove       |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_MenuBar           | ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,  0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2(0, 0));
    ImGui::Begin("##SCTHost", nullptr, kHostFlags);
    ImGui::PopStyleVar(3);

    DrawMenuBar();
    DrawModeTabBar();

    // Reserve bottom strip for status bar before creating the dockspace.
    const float statusH = ImGui::GetFrameHeight() + 6.0f;
    ImVec2 dockSize = ImGui::GetContentRegionAvail();
    dockSize.y -= statusH;

    const ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspaceId, dockSize, ImGuiDockNodeFlags_PassthruCentralNode);

    if (!m_layoutInitialized) {
        SetupDefaultLayout(dockspaceId);
        m_layoutInitialized = true;
    }

    ImGui::End(); // ##SCTHost

    // ── Mode-specific panels (drawn as independent ImGui windows) ────────────
    switch (m_mode) {
        case EditorMode::SceneEditor:
            DrawSceneEditorPanels();
            break;
        case EditorMode::NifEditor:
            DrawNifEditorPanel();
            break;
        case EditorMode::Animator:
            DrawAnimatorPanel();
            break;
        default: break;
    }

    DrawStatusBar();
}

// ── Private ────────────────────────────────────────────────────────────────

void MainLayout::DrawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene",     "Ctrl+N")) {}
        if (ImGui::MenuItem("Open...",       "Ctrl+O")) {}
        if (ImGui::MenuItem("Save",          "Ctrl+S")) {}
        if (ImGui::MenuItem("Save As...",    "Ctrl+Shift+S")) {}
        ImGui::Separator();
        if (ImGui::MenuItem("Import Actor..."))      {}
        if (ImGui::MenuItem("Import Animation..."))  {}
        if (ImGui::MenuItem("Import Face Capture...")) {}
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) {}
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::SeparatorText("Scene Editor Panels");
        if (ImGui::MenuItem("Clip Bin"))    {}
        if (ImGui::MenuItem("Scene Graph")) {}
        if (ImGui::MenuItem("Viewport"))    {}
        if (ImGui::MenuItem("Timeline"))    {}
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            m_layoutInitialized = false; // triggers SetupDefaultLayout next frame
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Export")) {
        ImGui::SeparatorText("Scene");
        if (ImGui::MenuItem("Export Scene YAML"))    { m_status = "Scene YAML export: not yet implemented"; }
        if (ImGui::MenuItem("Export Animations"))    { m_status = "Animation export: not yet implemented"; }
        ImGui::Separator();
        if (ImGui::MenuItem("Export All",  "Ctrl+E")) { m_status = "Export All: not yet implemented"; }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About SCT")) {}
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void MainLayout::DrawModeTabBar() {
    // A thin tab bar between the menu and the dockspace selects the editor mode.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(6.0f, 4.0f));

    if (ImGui::BeginTabBar("##ModeTabs", ImGuiTabBarFlags_None)) {
        struct { const char* label; EditorMode mode; } kTabs[] = {
            { "  Scene Editor  ", EditorMode::SceneEditor },
            { "  NIF Editor  ",   EditorMode::NifEditor   },
            { "  Animator  ",     EditorMode::Animator    },
        };
        for (auto& t : kTabs) {
            bool selected = (m_mode == t.mode);
            if (ImGui::BeginTabItem(t.label, nullptr,
                    selected ? ImGuiTabItemFlags_SetSelected : 0))
            {
                m_mode = t.mode;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::PopStyleVar(2);
}

void MainLayout::DrawStatusBar() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float h = ImGui::GetFrameHeight() + 6.0f;

    ImGui::SetNextWindowPos({ vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h });
    ImGui::SetNextWindowSize({ vp->WorkSize.x, h });
    ImGui::SetNextWindowViewport(vp->ID);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize     | ImGuiWindowFlags_NoMove     |
        ImGuiWindowFlags_NoScrollbar  | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,  0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2(8.0f, 3.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.12f, 0.18f, 1.0f));
    ImGui::Begin("##StatusBar", nullptr, kFlags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    // Coloured dot: green = ready, amber = working, red = error
    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "\xe2\x97\x8f"); // UTF-8 "●"
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextDisabled("%s", m_status);

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("No project loaded");

    ImGui::End();
}

void MainLayout::DrawSceneEditorPanels() {
    m_viewport.Draw();
    SceneGraphPanel::Draw();
    TimelinePanel::Draw();
    ClipBinPanel::Draw();
}

void MainLayout::DrawNifEditorPanel()  { NifEditorPanel::Draw(); }
void MainLayout::DrawAnimatorPanel()   { AnimatorPanel::Draw();  }

void MainLayout::SetupDefaultLayout(ImGuiID id) {
    // Only run if no saved layout exists (first launch or after "Reset Layout").
    // ImGui will skip this if sct_layout.ini already has positions.
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, ImGui::GetMainViewport()->WorkSize);

    // Horizontal split: clip bin (left) | rest
    ImGuiID left, rest;
    ImGui::DockBuilderSplitNode(id, ImGuiDir_Left, 0.17f, &left, &rest);

    // rest → scene graph (left) | main area (right)
    ImGuiID graphId, mainArea;
    ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, 0.28f, &graphId, &mainArea);

    // main area → viewport (top 60%) | timeline (bottom 40%)
    ImGuiID viewportId, timelineId;
    ImGui::DockBuilderSplitNode(mainArea, ImGuiDir_Up, 0.60f, &viewportId, &timelineId);

    ImGui::DockBuilderDockWindow("Clip Bin",    left);
    ImGui::DockBuilderDockWindow("Scene Graph", graphId);
    ImGui::DockBuilderDockWindow("Viewport",    viewportId);
    ImGui::DockBuilderDockWindow("Timeline",    timelineId);

    ImGui::DockBuilderFinish(id);
}
