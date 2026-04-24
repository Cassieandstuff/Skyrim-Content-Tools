#pragma once
#include "Camera.h"

// Phase 2: 3D viewport drawn via ImGui DrawList (manual perspective projection).
// Left-drag to orbit, right-drag to pan, scroll to zoom.
// Phase 3+ will layer FBO-rendered meshes on top of this.
class ViewportPanel {
public:
    void Draw();
private:
    Camera m_camera;
};
