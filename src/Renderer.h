#pragma once
#include "Camera.h"
#include <cstdint>

// Phase 2: render-to-texture framebuffer + grid floor.
// Call Init() once after the OpenGL context is ready.
// Call Resize() whenever the panel size changes, then Render() each frame.
// ColorTexture() returns the GLuint to pass to ImGui::Image.
class Renderer {
public:
    void Init();
    void Shutdown();

    void Resize(int w, int h);
    void Render(const Camera& cam);

    unsigned int ColorTexture() const { return m_colorTex; }

private:
    void BuildGrid();

    // FBO resources
    unsigned int m_fbo      = 0;
    unsigned int m_colorTex = 0;
    unsigned int m_depthRbo = 0;
    int          m_width    = 0;
    int          m_height   = 0;

    // Grid shader + geometry
    unsigned int m_shader    = 0;
    unsigned int m_gridVao   = 0;
    unsigned int m_gridVbo   = 0;
    int          m_gridVerts = 0;
    unsigned int m_axisVao   = 0;
    unsigned int m_axisVbo   = 0;

    bool m_ready = false;
};
