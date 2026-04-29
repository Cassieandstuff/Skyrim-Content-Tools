#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <imgui.h>

struct Skeleton;
struct Pose;

// ── Typed GPU resource handles ────────────────────────────────────────────────
enum class MeshHandle    : uint32_t { Invalid = 0 };
enum class TextureHandle : uint32_t { Invalid = 0 };

// ── Mesh upload descriptor ─────────────────────────────────────────────────────
// boneIndices / boneWeights are optional (leave empty for static meshes).
// indices are required; positions/normals/uvs must be the same length.
struct MeshData {
    std::vector<glm::vec3>   positions;
    std::vector<glm::vec3>   normals;
    std::vector<glm::vec2>   uvs;
    std::vector<glm::u8vec4> boneIndices;  // 4 indices per vertex (uint8)
    std::vector<glm::vec4>   boneWeights;  // 4 weights per vertex, sum == 1
    std::vector<uint16_t>    indices;
};

// ── Per-draw surface parameters ───────────────────────────────────────────────
struct DrawSurface {
    enum class BlendMode { Opaque, AlphaTest, AlphaBlend, Additive };

    TextureHandle diffuse         = TextureHandle::Invalid;
    glm::vec4     tint            = { 1.f, 1.f, 1.f, 1.f };
    bool          wireframe       = false;
    bool          xray            = false;
    BlendMode     blendMode       = BlendMode::Opaque;
    float         alphaThreshold  = 0.5f;   // normalised; only used for AlphaTest mode
};

// ── ISceneRenderer ────────────────────────────────────────────────────────────
// Scene-level rendering abstraction.  Implementations (GL, D3D11, …) are
// drop-in: they differ only in backend machinery; the viewport only calls this.
//
// Frame sequence:
//   BeginFrame(w, h)          — bind / resize FBO, clear
//   SetCamera(view, proj)     — store matrices for this frame
//   DrawGrid / DrawSkeleton / DrawMesh / DrawSkinnedMesh  (any order)
//   EndFrame()                — flush batched primitives, unbind FBO
//   GetOutputTexture()        — ImTextureID of the color attachment
struct ISceneRenderer {
    virtual ~ISceneRenderer() = default;

    // ── Frame lifecycle ───────────────────────────────────────────────────────
    virtual void BeginFrame(int width, int height)                            = 0;
    virtual void SetCamera(const glm::mat4& view, const glm::mat4& proj)     = 0;
    virtual void EndFrame()                                                   = 0;
    virtual ImTextureID GetOutputTexture() const                              = 0;

    // ── GPU resource management ───────────────────────────────────────────────
    virtual MeshHandle    UploadMesh  (const MeshData& data)                  = 0;
    virtual void          FreeMesh   (MeshHandle handle)                      = 0;
    // Overwrite the position buffer of an already-uploaded mesh in-place.
    // `positions` must be the same length as the original upload.  Used for
    // CPU-side morph blending (face ARKit blend shapes).
    virtual void          UpdateMeshPositions(MeshHandle handle,
                                              std::span<const glm::vec3> positions) = 0;
    virtual TextureHandle LoadTexture          (const std::string& path)               = 0;
    virtual TextureHandle LoadTextureFromMemory(const std::vector<uint8_t>& bytes)     = 0;
    virtual void          FreeTexture          (TextureHandle handle)                  = 0;

    // ── Draw calls ────────────────────────────────────────────────────────────
    // Set the scene directional light.  dir is expected to be normalised.
    // Call once per frame before draw calls.
    virtual void SetLight(const glm::vec3& dir,
                          const glm::vec3& color,
                          const glm::vec3& ambient)                          = 0;

    virtual void DrawGrid(float cellSize = 1.f, int halfExtent = 10)          = 0;

    virtual void DrawSkeleton(const Skeleton& skel, const Pose& pose,
                              const glm::mat4& world = glm::mat4(1.f))        = 0;

    virtual void DrawMesh(MeshHandle mesh, const glm::mat4& world,
                          const DrawSurface& surface = {})                    = 0;

    virtual void DrawSkinnedMesh(MeshHandle mesh, const glm::mat4& world,
                                 std::span<const glm::mat4> boneTransforms,
                                 const DrawSurface& surface = {})             = 0;
};
