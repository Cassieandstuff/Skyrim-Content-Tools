#pragma once
#include <imgui.h>

enum class EditorMode : int {
    SceneEditor = 0,
    NifEditor,
    Animator,
    Count
};

class MainLayout {
public:
    void Draw();

private:
    void DrawMenuBar();
    void DrawModeTabBar();
    void DrawStatusBar();
    void DrawSceneEditorPanels();
    void DrawNifEditorPanel();
    void DrawAnimatorPanel();

    // Sets up the default dock layout for the Scene Editor on first launch.
    // Skipped once sct_layout.ini exists.
    void SetupDefaultLayout(ImGuiID dockspace_id);

    EditorMode  m_mode              = EditorMode::SceneEditor;
    bool        m_layoutInitialized = false;
    const char* m_status            = "Ready";
};
