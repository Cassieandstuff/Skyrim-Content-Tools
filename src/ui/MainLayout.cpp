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
      }, m_sceneRenderer)
    , m_animViewport({                          // Anim Editor viewport tabs
          { "Scene",   ViewportMode::Scene   },
          { "Bones",   ViewportMode::Bones   },
      }, m_animRenderer, "Anim Viewport")       // distinct window name
    , m_nifState(m_nifRenderer)
    , m_nifBrowser(m_nifState)
    , m_nifGraph(m_nifState)
    , m_nifProps(m_nifState)
    , m_nifViewport(m_nifState)
{
    // ── Persistent settings (data folder, etc.) ───────────────────────────────
    m_state.LoadSettings();
    m_nifState.dataFolder = &m_state.dataFolder;

    // ── Plugin backend ────────────────────────────────────────────────────────
    if (DotNetHost::PluginReady())
        m_state.pluginBackend = std::make_unique<MutagenBackend>();

    // ── Track types ───────────────────────────────────────────────────────────
    RegisterAllTrackTypes();

    // ── App tabs ──────────────────────────────────────────────────────────────
    TabRegistry::Get().Register({
        AppTab::SceneEditor, "  Scene Editor  ",
        { &m_bin, &m_actorProps, &m_sceneGraph, &m_sceneViewport, &m_timeline }
    });
    TabRegistry::Get().Register({
        AppTab::AnimEditor, "  Anim Editor  ",
        { &m_animViewport }
    });
    TabRegistry::Get().Register({
        AppTab::NifEditor, "  NIF Editor  ",
        { &m_nifBrowser, &m_nifGraph, &m_nifProps, &m_nifViewport }
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
        ImGui::SeparatorText("Scene Editor Panels");
        if (ImGui::MenuItem("Bin"))         {}
        if (ImGui::MenuItem("Scene Graph")) {}
        if (ImGui::MenuItem("Viewport"))    {}
        if (ImGui::MenuItem("Timeline"))    {}
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

    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.f), "\xe2\x97\x8f");  // ●
    ImGui::SameLine(0.f, 6.f);
    ImGui::TextDisabled("%s", m_status);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    const float seqDur = m_state.sequence.Duration();
    if (!m_state.actors.empty() || seqDur > 0.f) {
        ImGui::TextDisabled("%d actor(s)  |  seq %.2fs",
                            (int)m_state.actors.size(), seqDur);
    } else {
        ImGui::TextDisabled("No project loaded");
    }

    ImGui::End();
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

    // Scene Editor: left column | scene graph | viewport + timeline
    ImGuiID left, rest;
    ImGui::DockBuilderSplitNode(id, ImGuiDir_Left, 0.17f, &left, &rest);
    ImGuiID binId, propsId;
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.60f, &binId, &propsId);

    ImGuiID graphId, mainArea;
    ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, 0.22f, &graphId, &mainArea);

    ImGuiID viewportId, timelineId;
    ImGui::DockBuilderSplitNode(mainArea, ImGuiDir_Up, 0.60f, &viewportId, &timelineId);

    ImGui::DockBuilderDockWindow("Bin",              binId);
    ImGui::DockBuilderDockWindow("Actor Properties", propsId);
    ImGui::DockBuilderDockWindow("Scene Graph",      graphId);
    ImGui::DockBuilderDockWindow("Viewport",         viewportId);
    ImGui::DockBuilderDockWindow("Timeline",         timelineId);

    ImGui::DockBuilderFinish(id);
}
