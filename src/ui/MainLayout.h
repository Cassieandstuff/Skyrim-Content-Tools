#pragma once
#include <imgui.h>
#include "AppState.h"
#include "TabRegistry.h"
#include "TrackRegistry.h"
#include "renderer/GlSceneRenderer.h"
#include "panels/ViewportPanel.h"
#include "panels/ClipBinPanel.h"
#include "panels/InspectorPanel.h"
#include "panels/PluginBrowserPanel.h"
#include "panels/TimelinePanel.h"
#include "panels/SceneGraphPanel.h"
#include "panels/AnimatorPanel.h"
#include "panels/NifEditorPanel.h"
#include "panels/NifEditorState.h"
#include "panels/NifGraphPanel.h"
#include "panels/NifBrowserPanel.h"

// ── MainLayout ────────────────────────────────────────────────────────────────
// Owns all panel instances (by value), registers them into the TabRegistry and
// TrackRegistry on construction, then dispatches each frame through the registry.
class MainLayout {
public:
    MainLayout();

    void Draw();
    bool WantsQuit()       const { return m_wantQuit; }
    const char* GetWindowTitle() const;

private:
    // ── State ─────────────────────────────────────────────────────────────────
    AppState    m_state;
    const char* m_status           = "Ready";
    bool        m_layoutInitialized[4] = {};  // one per AppTab
    bool        m_wantQuit         = false;

    // ── Renderer (must be declared before any panel that holds a reference to it) ──
    GlSceneRenderer m_renderer;

    // ── Panel instances (owned here, registered as non-owning ptrs) ───────────
    // Scene Editor tab
    BinPanel             m_bin;
    PluginBrowserPanel   m_pluginBrowser;
    SceneGraphPanel      m_sceneGraph;
    ViewportPanel        m_sceneViewport;
    TimelinePanel        m_timeline;
    InspectorPanel       m_inspector;
    // Anim Editor tab
    ViewportPanel        m_animViewport;

    // Full-page stubs / standalone panels
    AnimatorPanel  m_animator;

    // NIF Editor tab — shared state + four dockable sub-panels
    NifEditorState     m_nifState;
    NifBrowserPanel    m_nifBrowser;
    NifGraphPanel      m_nifGraph;
    NifPropertiesPanel m_nifProps;
    NifViewportPanel   m_nifViewport;

    // ── Layout helpers ────────────────────────────────────────────────────────
    void DrawMenuBar();
    void DrawStatusBar();
    void DrawToasts();
    void SetupDefaultLayout(ImGuiID dockspaceId, AppTab tab);

    // ── File operations ───────────────────────────────────────────────────────
    void DoNewProject();
    void DoOpenProject();
    void DoSaveProject();
    void DoSaveProjectAs();

    char m_fileErr[256] = {};
};
