#include "MainLayout.h"
#include "DotNetHost.h"
#include "MutagenBackend.h"
#include "ProjectFile.h"
#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder API
#include <algorithm>
#include <cstdio>
#include <filesystem>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#endif

// ── Constructor: register tracks and tabs ────────────────────────────────────

MainLayout::MainLayout()
    : m_sceneViewport({                         // Scene Editor viewport tabs
          { "Scene",   ViewportMode::Scene   },
          { "Face",    ViewportMode::Face    },
          { "Cameras", ViewportMode::Cameras },
      }, m_renderer)
    , m_animViewport({                          // Anim Editor viewport tabs
          { "Scene",   ViewportMode::Scene   },
          { "Bones",   ViewportMode::Bones   },
      }, m_renderer, "Anim Viewport")           // distinct window name
    , m_nifState(m_renderer)
    , m_nifBrowser(m_nifState)
    , m_nifGraph(m_nifState)
    , m_nifProps(m_nifState)
    , m_nifViewport(m_nifState)
{
    // ── Persistent settings (data folder, etc.) ───────────────────────────────
    m_state.LoadSettings();

    // ── Plugin backend ────────────────────────────────────────────────────────
    if (DotNetHost::PluginReady())
        m_state.pluginBackend = std::make_unique<MutagenBackend>();

    // ── Track types ───────────────────────────────────────────────────────────
    RegisterAllTrackTypes();

    // ── App tabs ──────────────────────────────────────────────────────────────
    TabRegistry::Get().Register({
        AppTab::SceneEditor, "  Scene Editor  ",
        { &m_bin, &m_sceneGraph, &m_sceneViewport, &m_timeline, &m_inspector }
    });
    TabRegistry::Get().Register({
        AppTab::AnimEditor, "  Anim Editor  ",
        { &m_animViewport }
    });
    TabRegistry::Get().Register({
        AppTab::NifEditor, "  NIF Editor  ",
        { &m_nifBrowser, &m_nifGraph, &m_nifProps, &m_nifViewport }
    });
    TabRegistry::Get().Register({
        AppTab::Workflow, "  Workflow  ",
        { &m_pluginBrowser }
    });
}

// ── Per-frame draw ────────────────────────────────────────────────────────────

void MainLayout::Draw()
{
    // ── Full-screen host window (owns the dockspace) ──────────────────────────
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    constexpr ImGuiWindowFlags kHostFlags =
        ImGuiWindowFlags_NoTitleBar        | ImGuiWindowFlags_NoCollapse   |
        ImGuiWindowFlags_NoResize          | ImGuiWindowFlags_NoMove       |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_MenuBar           | ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0, 0));
    ImGui::Begin("##SCTHost", nullptr, kHostFlags);
    ImGui::PopStyleVar(3);

    m_state.Tick(ImGui::GetIO().DeltaTime);

    // ── Global keyboard shortcuts ─────────────────────────────────────────────
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && io.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_N, false)) DoNewProject();
            if (ImGui::IsKeyPressed(ImGuiKey_O, false)) DoOpenProject();
            if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                if (io.KeyShift) DoSaveProjectAs();
                else             DoSaveProject();
            }
        }
        if (!io.WantTextInput && !io.KeyCtrl) {
            // Playback
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
                m_state.playing ^= true;
            if (ImGui::IsKeyPressed(ImGuiKey_K, false)) {
                m_state.playing = false;
                m_state.time    = 0.f;
            }
            // Frame step  (comma = back, period = forward)
            constexpr float kFrame = 1.f / 30.f;
            if (ImGui::IsKeyPressed(ImGuiKey_Comma, false))
                m_state.time = std::max(0.f, m_state.time - kFrame);
            if (ImGui::IsKeyPressed(ImGuiKey_Period, false)) {
                const float dur = m_state.sequence.Duration();
                m_state.time = m_state.time + kFrame;
                if (dur > 0.f) m_state.time = std::min(m_state.time, dur);
            }
            // Jump to start / end
            if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
                m_state.time    = 0.f;
                m_state.playing = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
                const float dur = m_state.sequence.Duration();
                m_state.time    = dur > 0.f ? dur : 0.f;
                m_state.playing = false;
            }
            // Deselect
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                m_state.selectedCast = -1;
            // Tab switch
            if (ImGui::IsKeyPressed(ImGuiKey_1, false)) m_state.activeTab = AppTab::SceneEditor;
            if (ImGui::IsKeyPressed(ImGuiKey_2, false)) m_state.activeTab = AppTab::AnimEditor;
            if (ImGui::IsKeyPressed(ImGuiKey_3, false)) m_state.activeTab = AppTab::NifEditor;
            if (ImGui::IsKeyPressed(ImGuiKey_4, false)) m_state.activeTab = AppTab::Workflow;
        }
    }

    DrawMenuBar();

    // ── App tab bar ────────────────────────────────────────────────────────────
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,      ImVec2(14.f, 6.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,  ImVec2(6.f,  4.f));
    if (ImGui::BeginTabBar("##AppTabs")) {
        for (auto& tabDef : TabRegistry::Get().All()) {
            if (ImGui::BeginTabItem(tabDef.label)) {
                m_state.activeTab = tabDef.id;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar(2);

    // Reserve bottom strip for status bar before creating the dockspace.
    const float statusH = ImGui::GetFrameHeight() + 6.f;
    ImVec2 dockSize = ImGui::GetContentRegionAvail();
    dockSize.y -= statusH;

    // Per-tab dockspace — each tab has its own layout that persists independently.
    const int tabIdx = (int)m_state.activeTab;
    char dockName[32];
    std::snprintf(dockName, sizeof(dockName), "DockSpace_%d", tabIdx);
    const ImGuiID dockspaceId = ImGui::GetID(dockName);
    ImGui::DockSpace(dockspaceId, dockSize, ImGuiDockNodeFlags_PassthruCentralNode);

    if (!m_layoutInitialized[tabIdx]) {
        SetupDefaultLayout(dockspaceId, m_state.activeTab);
        m_layoutInitialized[tabIdx] = true;
    }

    ImGui::End();  // ##SCTHost

    // ── Draw active tab's panels ───────────────────────────────────────────────
    if (AppTabDef* activeTab = TabRegistry::Get().Find(m_state.activeTab)) {
        for (IPanel* panel : activeTab->panels)
            panel->Draw(m_state);
    }

    DrawStatusBar();
    DrawToasts();
}

// ── Menu bar ──────────────────────────────────────────────────────────────────

void MainLayout::DrawMenuBar()
{
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene",      "Ctrl+N"))         DoNewProject();
        if (ImGui::MenuItem("Open...",        "Ctrl+O"))         DoOpenProject();
        if (ImGui::MenuItem("Save",           "Ctrl+S"))         DoSaveProject();
        if (ImGui::MenuItem("Save As...",     "Ctrl+Shift+S"))   DoSaveProjectAs();
        ImGui::Separator();
        if (ImGui::MenuItem("Import Animation..."))
            m_bin.OpenImportDialog(m_state);
        ImGui::Separator();
        if (m_fileErr[0]) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
            ImGui::TextUnformatted(m_fileErr);
            ImGui::PopStyleColor();
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Exit", "Alt+F4"))
            m_wantQuit = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::SeparatorText("Scene Editor");
        if (ImGui::MenuItem("Bin"))         {}
        if (ImGui::MenuItem("Scene Graph")) {}
        if (ImGui::MenuItem("Viewport"))    {}
        if (ImGui::MenuItem("Timeline"))    {}
        if (ImGui::MenuItem("Inspector"))   {}
        ImGui::SeparatorText("Workflow");
        if (ImGui::MenuItem("Plugin Browser")) {}
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout"))
            m_layoutInitialized[(int)m_state.activeTab] = false;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Export")) {
        ImGui::SeparatorText("Scene");
        if (ImGui::MenuItem("Export Plugin (xeditlib)"))
            m_status = "Plugin export: not yet implemented";
        if (ImGui::MenuItem("Export Animations"))
            m_status = "Animation export: not yet implemented";
        ImGui::Separator();
        if (ImGui::MenuItem("Export All", "Ctrl+E"))
            m_status = "Export All: not yet implemented";
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About SCT")) {}
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

// ── Status bar ────────────────────────────────────────────────────────────────

void MainLayout::DrawStatusBar()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float h = ImGui::GetFrameHeight() + 6.f;

    ImGui::SetNextWindowPos({ vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h });
    ImGui::SetNextWindowSize({ vp->WorkSize.x, h });
    ImGui::SetNextWindowViewport(vp->ID);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize     | ImGuiWindowFlags_NoMove     |
        ImGuiWindowFlags_NoScrollbar  | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(8.f, 3.f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.12f, 0.18f, 1.f));
    ImGui::Begin("##StatusBar", nullptr, kFlags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    // Zone 1: status dot + message
    const ImVec4 dotCol = m_fileErr[0]
        ? ImVec4(1.f, 0.4f, 0.4f, 1.f)
        : ImVec4(0.3f, 0.9f, 0.4f, 1.f);
    ImGui::TextColored(dotCol, "\xe2\x97\x8f");  // ●
    ImGui::SameLine(0.f, 6.f);
    if (m_fileErr[0])
        ImGui::TextColored(ImVec4(1.f, 0.5f, 0.5f, 1.f), "%s", m_fileErr);
    else
        ImGui::TextDisabled("%s", m_status);

    // Zone 2: scene stats
    ImGui::SameLine(0.f, 16.f);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0.f, 8.f);
    const float seqDur = m_state.sequence.Duration();
    if (!m_state.actors.empty() || seqDur > 0.f)
        ImGui::TextDisabled("%d actor(s)  %.2fs", (int)m_state.actors.size(), seqDur);
    else
        ImGui::TextDisabled("No actors");

    // Zone 3: playback state
    ImGui::SameLine(0.f, 16.f);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0.f, 8.f);
    if (m_state.playing)
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.f),
                           "\xe2\x96\xb6 PLAYING  %.2fs", m_state.time);
    else
        ImGui::TextDisabled("\xe2\x96\xa0 %.2fs", m_state.time);  // ■

    // Zone 4: right-aligned project name
    char projBuf[256];
    std::snprintf(projBuf, sizeof(projBuf), "%s%s",
                  m_state.projectName.c_str(),
                  m_state.projectDirty ? " *" : "");
    const float projW  = ImGui::CalcTextSize(projBuf).x;
    const float rightX = ImGui::GetContentRegionMax().x - projW - 8.f;
    if (rightX > ImGui::GetCursorPosX())
        ImGui::SameLine(rightX);
    ImGui::TextDisabled("%s", projBuf);

    ImGui::End();
}

// ── Toast notifications ───────────────────────────────────────────────────────

void MainLayout::DrawToasts()
{
    if (m_state.toasts.empty()) return;

    ImDrawList*          fgDl = ImGui::GetForegroundDrawList();
    const ImGuiViewport* vp   = ImGui::GetMainViewport();
    const float statusH  = ImGui::GetFrameHeight() + 8.f;
    const float toastW   = 320.f;
    const float toastH   = 36.f;
    const float pad      = 6.f;
    const float textLineH = ImGui::GetTextLineHeight();

    float y = vp->WorkPos.y + vp->WorkSize.y - statusH - pad;

    int shown = 0;
    for (int i = (int)m_state.toasts.size() - 1; i >= 0 && shown < 5; --i) {
        const Toast& t = m_state.toasts[i];
        y -= toastH + pad;
        const float x = vp->WorkPos.x + vp->WorkSize.x - toastW - pad;

        ImU32 bg     = IM_COL32(28,  33,  48, 235);
        ImU32 border = IM_COL32(60,  70, 105, 210);
        ImU32 text   = IM_COL32(200, 210, 230, 255);
        if (t.level == ToastLevel::Warning) {
            bg     = IM_COL32(46, 38, 18, 235);
            border = IM_COL32(175, 125, 38, 210);
        } else if (t.level == ToastLevel::Error) {
            bg     = IM_COL32(46, 18, 18, 235);
            border = IM_COL32(185,  50, 50, 210);
        }

        fgDl->AddRectFilled({x, y}, {x + toastW, y + toastH}, bg, 5.f);
        fgDl->AddRect      ({x, y}, {x + toastW, y + toastH}, border, 5.f, 0, 1.f);

        // TTL progress bar at bottom edge
        const float barW = toastW * std::clamp(t.ttl / 4.f, 0.f, 1.f);
        if (barW > 0.5f)
            fgDl->AddRectFilled({x + 1, y + toastH - 3.f},
                                {x + barW - 1, y + toastH - 1.f},
                                border, 0.f, ImDrawFlags_RoundCornersBottom);

        fgDl->AddText({x + 10.f, y + (toastH - textLineH) * 0.5f},
                      text, t.message.c_str());
        ++shown;
    }
}

// ── Window title ──────────────────────────────────────────────────────────────

const char* MainLayout::GetWindowTitle() const
{
    static char buf[256];
    std::snprintf(buf, sizeof(buf), "Skyrim Content Tools - %s%s",
                  m_state.projectName.c_str(),
                  m_state.projectDirty ? " *" : "");
    return buf;
}

// ── File operations ───────────────────────────────────────────────────────────

void MainLayout::DoNewProject()
{
    m_state.NewProject();
    m_fileErr[0] = '\0';
    m_status     = "New scene";
}

void MainLayout::DoOpenProject()
{
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    OPENFILENAMEA ofn  = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.lpstrFilter  = "SCT Project\0*.sct\0All Files\0*.*\0";
    ofn.lpstrFile    = buf;
    ofn.nMaxFile     = sizeof(buf);
    ofn.lpstrDefExt  = "sct";
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle   = "Open SCT Project";
    if (!GetOpenFileNameA(&ofn)) return;

    m_fileErr[0] = '\0';
    if (!ProjectFile::Load(buf, m_state, m_fileErr, sizeof(m_fileErr))) {
        m_status = "Open failed";
    } else {
        std::fill(std::begin(m_layoutInitialized), std::end(m_layoutInitialized), false);
        m_status = "Project opened";
    }
#endif
}

void MainLayout::DoSaveProject()
{
    if (m_state.projectPath.empty()) { DoSaveProjectAs(); return; }

    m_fileErr[0] = '\0';
    if (!ProjectFile::Save(m_state.projectPath, m_state, m_fileErr, sizeof(m_fileErr))) {
        m_status = "Save failed";
    } else {
        m_state.projectDirty = false;
        m_status = "Project saved";
    }
}

void MainLayout::DoSaveProjectAs()
{
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    if (!m_state.projectPath.empty())
        std::snprintf(buf, sizeof(buf), "%s", m_state.projectPath.c_str());

    OPENFILENAMEA ofn  = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.lpstrFilter  = "SCT Project\0*.sct\0All Files\0*.*\0";
    ofn.lpstrFile    = buf;
    ofn.nMaxFile     = sizeof(buf);
    ofn.lpstrDefExt  = "sct";
    ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle   = "Save SCT Project As";
    if (!GetSaveFileNameA(&ofn)) return;

    m_state.projectPath = buf;
    m_state.projectName = std::filesystem::path(buf).stem().string();

    m_fileErr[0] = '\0';
    if (!ProjectFile::Save(buf, m_state, m_fileErr, sizeof(m_fileErr))) {
        m_status = "Save failed";
    } else {
        m_state.projectDirty = false;
        m_status = "Project saved";
    }
#endif
}

// ── Default dock layout (per tab) ─────────────────────────────────────────────

void MainLayout::SetupDefaultLayout(ImGuiID id, AppTab tab)
{
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, ImGui::GetMainViewport()->WorkSize);

    if (tab == AppTab::NifEditor) {
        // browser (far left) | graph+props (mid left) | viewport (right)
        ImGuiID nifBrowser, nifRest;
        ImGui::DockBuilderSplitNode(id, ImGuiDir_Left, 0.18f, &nifBrowser, &nifRest);
        ImGuiID nifLeft, nifRight;
        ImGui::DockBuilderSplitNode(nifRest, ImGuiDir_Left, 0.27f, &nifLeft, &nifRight);
        ImGuiID nifGraph, nifProps;
        ImGui::DockBuilderSplitNode(nifLeft, ImGuiDir_Up, 0.65f, &nifGraph, &nifProps);
        ImGui::DockBuilderDockWindow("NIF Browser",    nifBrowser);
        ImGui::DockBuilderDockWindow("NIF Graph",      nifGraph);
        ImGui::DockBuilderDockWindow("NIF Properties", nifProps);
        ImGui::DockBuilderDockWindow("NIF Viewport",   nifRight);
        ImGui::DockBuilderFinish(id);
        return;
    }

    if (tab == AppTab::AnimEditor) {
        // Single full-area viewport
        ImGui::DockBuilderDockWindow("Anim Viewport", id);
        ImGui::DockBuilderFinish(id);
        return;
    }

    if (tab == AppTab::Workflow) {
        // Workflow tab: Plugin Browser fills the space; future panels dock alongside it.
        ImGui::DockBuilderDockWindow("Plugin Browser", id);
        ImGui::DockBuilderFinish(id);
        return;
    }

    // Scene Editor:
    //   Top (65%): [Bin  |  Scene Graph  |  Viewport]
    //   Bottom (35%): [Timeline  |  Inspector]
    ImGuiID topArea, bottomArea;
    ImGui::DockBuilderSplitNode(id, ImGuiDir_Down, 0.35f, &bottomArea, &topArea);

    ImGuiID binId, topRest;
    ImGui::DockBuilderSplitNode(topArea, ImGuiDir_Left, 0.17f, &binId, &topRest);

    ImGuiID graphId, viewportId;
    ImGui::DockBuilderSplitNode(topRest, ImGuiDir_Left, 0.265f, &graphId, &viewportId);

    ImGuiID timelineId, inspectorId;
    ImGui::DockBuilderSplitNode(bottomArea, ImGuiDir_Right, 0.22f, &inspectorId, &timelineId);

    ImGui::DockBuilderDockWindow("Bin",         binId);
    ImGui::DockBuilderDockWindow("Scene Graph", graphId);
    ImGui::DockBuilderDockWindow("Viewport",    viewportId);
    ImGui::DockBuilderDockWindow("Timeline",    timelineId);
    ImGui::DockBuilderDockWindow("Inspector",   inspectorId);

    ImGui::DockBuilderFinish(id);
}
