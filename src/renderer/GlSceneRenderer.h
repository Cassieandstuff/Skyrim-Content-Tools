#pragma once
#include "ISceneRenderer.h"
#include <unordered_map>
#include <vector>

// ── GlSceneRenderer ───────────────────────────────────────────────────────────
// OpenGL 4.5 implementation of ISceneRenderer.
//
// Primitives (grid, skeleton) are batched into CPU vectors each frame and
// flushed as a single GL_LINES + GL_POINTS draw at EndFrame().
//
// Meshes are uploaded once via UploadMesh() and drawn immediately on each
// DrawMesh() / DrawSkinnedMesh() call.  Skinned draws upload bone transforms
// into a shared SSBO bound at binding point 0.
//
// The FBO is resized lazily whenever BeginFrame() receives a new (w, h).
class GlSceneRenderer : public ISceneRenderer {
public:
    GlSceneRenderer();
    ~GlSceneRenderer() override;

    GlSceneRenderer(const GlSceneRenderer&)            = delete;
    GlSceneRenderer& operator=(const GlSceneRenderer&) = delete;

    void        BeginFrame(int width, int height) override;
    void        SetCamera(const glm::mat4& view, const glm::mat4& proj) override;
    void        EndFrame() override;
    ImTextureID GetOutputTexture() const override;

    MeshHandle    UploadMesh           (const MeshData& data) override;
    void          FreeMesh            (MeshHandle handle) override;
    void          UpdateMeshPositions (MeshHandle handle,
                                       std::span<const glm::vec3> positions) override;
    TextureHandle LoadTexture         (const std::string& path) override;
    TextureHandle LoadTextureFromMemory(const std::vector<uint8_t>& bytes) override;
    void          FreeTexture         (TextureHandle handle) override;

    void SetLight(const glm::vec3& dir,
                  const glm::vec3& color,
                  const glm::vec3& ambient) override;

    void DrawGrid(float cellSize, int halfExtent) override;
    void DrawSkeleton(const Skeleton& skel, const Pose& pose,
                      const glm::mat4& world) override;
    void DrawMesh(MeshHandle mesh, const glm::mat4& world,
                  const DrawSurface& surface) override;
    void DrawSkinnedMesh(MeshHandle mesh, const glm::mat4& world,
                         std::span<const glm::mat4> boneTransforms,
                         const DrawSurface& surface) override;

private:
    // ── Batch rendering (lines + points) ──────────────────────────────────────
    struct BatchVert {
        glm::vec3 pos;
        uint8_t   r, g, b, a;
    };
    std::vector<BatchVert> m_lineVerts;
    std::vector<BatchVert> m_pointVerts;
    unsigned int m_batchVao = 0;
    unsigned int m_batchVbo = 0;

    void PushLine (glm::vec3 a, glm::vec3 b,
                   uint8_t r, uint8_t g, uint8_t b8, uint8_t alpha = 255);
    void PushPoint(glm::vec3 p,
                   uint8_t r, uint8_t g, uint8_t b8, uint8_t alpha = 255);
    void FlushBatch();

    // ── FBO ───────────────────────────────────────────────────────────────────
    unsigned int m_fbo      = 0;
    unsigned int m_colorTex = 0;
    unsigned int m_depthRbo = 0;
    int          m_fboW     = 0;
    int          m_fboH     = 0;
    void ResizeFbo(int w, int h);

    // ── Shaders ───────────────────────────────────────────────────────────────
    unsigned int m_primShader    = 0;   // lines / points with per-vertex colour
    unsigned int m_meshShader    = 0;   // static mesh, Blinn-Phong
    unsigned int m_skinnedShader = 0;   // skinned mesh via SSBO
    unsigned int CompileProgram(const char* vs, const char* fs, const char* label);

    // ── GPU mesh store ────────────────────────────────────────────────────────
    struct GpuMesh {
        unsigned int vao        = 0;
        unsigned int vboPos     = 0;
        unsigned int vboNorm    = 0;
        unsigned int vboUv      = 0;
        unsigned int vboBoneIdx = 0;
        unsigned int vboBoneWt  = 0;
        unsigned int vboVtxCol  = 0;   // optional per-vertex RGB, location 5
        unsigned int ebo        = 0;
        int          indexCount  = 0;
        int          vertexCount = 0;
        bool         skinned     = false;
    };
    uint32_t m_nextMeshId = 1;
    std::unordered_map<uint32_t, GpuMesh> m_meshes;

    // ── Texture store ─────────────────────────────────────────────────────────
    uint32_t m_nextTexId = 1;
    std::unordered_map<uint32_t, unsigned int> m_textures;

    // ── Bone SSBO (shared, re-uploaded per skinned draw) ─────────────────────
    unsigned int m_boneSsbo = 0;

    // ── Camera ────────────────────────────────────────────────────────────────
    glm::mat4 m_view { 1.f };
    glm::mat4 m_proj { 1.f };

    // ── Scene light ───────────────────────────────────────────────────────────
    glm::vec3 m_lightDir     { 0.371f, 0.928f, 0.0f };  // defaults set by first SetLight()
    glm::vec3 m_lightColor   { 1.0f,  0.98f, 0.92f };
    glm::vec3 m_ambientColor { 0.12f, 0.13f, 0.18f };
};
