#include "Renderer.h"
#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <vector>

// ---------------------------------------------------------------------------
// Shader source
// ---------------------------------------------------------------------------
static const char* kVert = R"(
#version 450 core
layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
void main() { gl_Position = uViewProj * vec4(aPos, 1.0); }
)";

static const char* kFrag = R"(
#version 450 core
uniform vec4 uColor;
out vec4 FragColor;
void main() { FragColor = uColor; }
)";

// ---------------------------------------------------------------------------
static GLuint CompileProgram(const char* vert, const char* frag) {
    auto compile = [](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512]; glGetShaderInfoLog(s, 512, nullptr, log);
            fprintf(stderr, "SCT: shader compile error:\n%s\n", log);
        }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER,   vert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, frag);
    GLuint p  = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);    glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(p, 512, nullptr, log);
        fprintf(stderr, "SCT: program link error:\n%s\n", log);
    }
    return p;
}

// ---------------------------------------------------------------------------
void Renderer::Init() {
    if (m_ready) return;
    m_shader = CompileProgram(kVert, kFrag);
    BuildGrid();
    m_ready = true;
}

void Renderer::Shutdown() {
    if (!m_ready) return;
    if (m_fbo) {
        glDeleteFramebuffers(1, &m_fbo);
        glDeleteTextures(1, &m_colorTex);
        glDeleteRenderbuffers(1, &m_depthRbo);
        m_fbo = m_colorTex = m_depthRbo = 0;
    }
    glDeleteVertexArrays(1, &m_gridVao); glDeleteBuffers(1, &m_gridVbo);
    glDeleteVertexArrays(1, &m_axisVao); glDeleteBuffers(1, &m_axisVbo);
    glDeleteProgram(m_shader);
    m_ready = false;
}

// ---------------------------------------------------------------------------
void Renderer::BuildGrid() {
    constexpr int kHalf  = 10;
    constexpr int kSteps = kHalf * 2; // 20 intervals → 21 lines each direction

    std::vector<float> grid; // minor grid lines (excluding centre axes)
    grid.reserve(kSteps * 2 * 3 * 2);

    for (int i = -kHalf; i <= kHalf; ++i) {
        if (i == 0) continue;
        float f = static_cast<float>(i);
        // Line along Z at x = f
        grid.insert(grid.end(), { f, 0.f, -(float)kHalf,  f, 0.f, (float)kHalf });
        // Line along X at z = f
        grid.insert(grid.end(), { -(float)kHalf, 0.f, f,  (float)kHalf, 0.f, f });
    }
    m_gridVerts = static_cast<int>(grid.size()) / 3;

    glGenVertexArrays(1, &m_gridVao); glGenBuffers(1, &m_gridVbo);
    glBindVertexArray(m_gridVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_gridVbo);
    glBufferData(GL_ARRAY_BUFFER, grid.size() * sizeof(float), grid.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Axis lines: X (red) then Z (blue) — 4 vertices total
    float axes[] = {
        -(float)kHalf, 0.f, 0.f,   (float)kHalf, 0.f, 0.f,   // X axis
         0.f, 0.f, -(float)kHalf,   0.f, 0.f, (float)kHalf,   // Z axis
    };
    glGenVertexArrays(1, &m_axisVao); glGenBuffers(1, &m_axisVbo);
    glBindVertexArray(m_axisVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_axisVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(axes), axes, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
void Renderer::Resize(int w, int h) {
    if (w == m_width && h == m_height) return;
    m_width = w; m_height = h;

    if (m_fbo) {
        glDeleteFramebuffers(1, &m_fbo);
        glDeleteTextures(1, &m_colorTex);
        glDeleteRenderbuffers(1, &m_depthRbo);
    }

    glGenTextures(1, &m_colorTex);
    glBindTexture(GL_TEXTURE_2D, m_colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenRenderbuffers(1, &m_depthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthRbo);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ---------------------------------------------------------------------------
void Renderer::Render(const Camera& cam) {
    if (!m_ready || !m_fbo) return;

    // Save state we're about to clobber
    GLint  prevFbo = 0;      glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFbo);
    GLint  prevVp[4] = {};   glGetIntegerv(GL_VIEWPORT, prevVp);
    GLboolean depthWasOn = glIsEnabled(GL_DEPTH_TEST);

    // Render to FBO
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, m_width, m_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearColor(0.11f, 0.13f, 0.20f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glm::mat4 vp = cam.Proj((float)m_width / (float)m_height) * cam.View();

    glUseProgram(m_shader);
    GLint vpLoc    = glGetUniformLocation(m_shader, "uViewProj");
    GLint colorLoc = glGetUniformLocation(m_shader, "uColor");
    glUniformMatrix4fv(vpLoc, 1, GL_FALSE, glm::value_ptr(vp));

    glLineWidth(1.0f);

    // Minor grid
    glUniform4f(colorLoc, 0.38f, 0.38f, 0.50f, 1.0f);
    glBindVertexArray(m_gridVao);
    glDrawArrays(GL_LINES, 0, m_gridVerts);

    glLineWidth(2.0f);

    // X axis (red)
    glUniform4f(colorLoc, 0.85f, 0.22f, 0.22f, 1.0f);
    glBindVertexArray(m_axisVao);
    glDrawArrays(GL_LINES, 0, 2);

    // Z axis (blue)
    glUniform4f(colorLoc, 0.22f, 0.45f, 0.90f, 1.0f);
    glDrawArrays(GL_LINES, 2, 2);

    glLineWidth(1.0f);

    glBindVertexArray(0);
    glUseProgram(0);

    // Restore state
    if (!depthWasOn) glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
}
