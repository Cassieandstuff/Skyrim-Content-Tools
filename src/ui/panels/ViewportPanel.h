#pragma once
#include "ui/IPanel.h"
#include "app/AppState.h"
#include "ui/Camera.h"
#include "ui/ControllerInput.h"
#include "env/CellEnvironmentManager.h"
#include "env/TerrainStreamManager.h"
#include "ui/panels/ActorRenderCache.h"
#include "renderer/ISceneRenderer.h"
#include <vector>
#include <string>

// ── ViewportMode ──────────────────────────────────────────────────────────────
// Determines how the viewport renders and which camera/overlay is active.
// The mode set is injected at construction time so the same class can serve
// different app tabs with different tab bars.
enum class ViewportMode {
    Scene,    // 3D perspective — all actors, free orbit camera
    Face,     // Camera locked to head bone, close-up face rig  (scaffolded)
    Cameras,  // Preview through a scene Camera track           (scaffolded)
    Bones,    // Bone-highlight overlay for animation editing   (scaffolded)
};

struct ViewportTabDef {
    const char*  label;
    ViewportMode mode;
};

// ── ViewportPanel ─────────────────────────────────────────────────────────────
// Pure display component: renders the skeleton(s) from AppState and handles
// viewport camera input.  Owns only camera state and the three sub-managers.
// The tab bar shown is configured at construction time.
class ViewportPanel : public IPanel {
public:
    ViewportPanel(std::vector<ViewportTabDef> tabs, ISceneRenderer& renderer,
                  const char* panelId = "Viewport");
    ~ViewportPanel();

    void        Draw(AppState& state) override;
    const char* PanelID() const override { return panelId_; }

private:
    const char* panelId_;
    // ── Renderer (non-owning, lifetime managed by MainLayout) ────────────────
    ISceneRenderer& renderer_;

    // ── Tab configuration (injected at construction) ─────────────────────────
    std::vector<ViewportTabDef> tabs_;
    int          activeTabIdx_ = 0;
    ViewportMode mode_         = ViewportMode::Scene;

    // ── Camera ────────────────────────────────────────────────────────────────
    Camera          camera_;
    ControllerInput controller_;

    // ── Sub-managers ──────────────────────────────────────────────────────────
    CellEnvironmentManager env_;      // static cell NIF streaming + draw dispatch
    TerrainStreamManager   terrain_;  // terrain tile streaming + draw dispatch
    ActorRenderCache       actors_;   // per-actor GPU cache + pose eval + draw

    // ── Draw distance cull ────────────────────────────────────────────────────
    float cellCullDist_ = 8192.f;

    // ── Ambient simulation clock ──────────────────────────────────────────────
    // Free-running; drives NIF controller animations for cell environment objects.
    float simTime_ = 0.f;

    // ── Internals ─────────────────────────────────────────────────────────────
    void FrameAll();
};
