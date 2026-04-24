#pragma once
#include "Renderer.h"
#include "Camera.h"

// Phase 2: render-to-texture viewport with orbit camera and grid floor.
// Left-drag to orbit, right-drag to pan, scroll to zoom.
class ViewportPanel {
public:
    ~ViewportPanel() { m_renderer.Shutdown(); }
    void Draw();

private:
    Renderer m_renderer;
    Camera   m_camera;
    bool     m_initialized = false;
};
