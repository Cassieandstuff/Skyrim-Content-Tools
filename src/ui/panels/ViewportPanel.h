#pragma once
#include "ui/IPanel.h"
#include "Camera.h"
#include "Pose.h"
#include "renderer/ISceneRenderer.h"
#include <vector>
#include <string>
#include <cstdint>

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
// viewport camera input.  Owns only camera state and cached render data.
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
    Camera  camera_;

    // ── Per-actor render cache ────────────────────────────────────────────────
    struct ActorRenderData {
        Pose                    refPose;
        Pose                    pose;
        std::vector<MeshHandle> meshHandles;    // GPU meshes uploaded from nifPath
        std::vector<glm::mat4>  meshTransforms; // per-mesh toRoot from NifDocument
        std::string             loadedNifPath;  // path these handles came from
    };
    std::vector<ActorRenderData> actorCache_;
    int cachedActorCount_   = -1;  // invalidation sentinel
    int cachedSkeletonCount_ = -1;

    // ── Internals ─────────────────────────────────────────────────────────────
    void RebuildActorCache(AppState& state);
    void SyncNifHandles(AppState& state);  // re-uploads if nifPath changed
    void EvaluatePoses(AppState& state);
    void FrameAll();
};
