#include "GlSceneRenderer.h"
#include "ShaderSources.h"
#include "anim/HavokSkeleton.h"
#include "anim/Pose.h"
#include "asset/DdsLoader.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

static void GlLog(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#if defined(_WIN32)
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
#endif
    fprintf(stderr, "%s\n", buf);
}

// ── Shader helper ─────────────────────────────────────────────────────────────

unsigned int GlSceneRenderer::CompileProgram(const char* vs, const char* fs,
                                              const char* label)
{
    auto compile = [](GLenum type, const char* src, const char* lbl) -> unsigned int {
        unsigned int sh = glCreateShader(type);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        int ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512]; glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
            GlLog("SCT [GL] %s shader compile FAILED: %s", lbl, log);
        }
        return sh;
    };

    unsigned int vsh = compile(GL_VERTEX_SHADER,   vs, label);
    unsigned int fsh = compile(GL_FRAGMENT_SHADER, fs, label);
    unsigned int prog = glCreateProgram();
    glAttachShader(prog, vsh);
    glAttachShader(prog, fsh);
    glLinkProgram(prog);
    int ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        GlLog("SCT [GL] %s link FAILED: %s", label, log);
        prog = 0;
    } else {
        GlLog("SCT [GL] %s program linked OK (id=%u)", label, prog);
    }
    glDeleteShader(vsh);
    glDeleteShader(fsh);
    return prog;
}

// ── Construction / destruction ────────────────────────────────────────────────

GlSceneRenderer::GlSceneRenderer()
{
    // Shaders
    m_primShader    = CompileProgram(kPrimVS,    kPrimFS,    "prim");
    m_meshShader    = CompileProgram(kMeshVS,    kMeshFS,    "mesh");
    m_skinnedShader = CompileProgram(kSkinnedVS, kMeshFS,    "skinned");
    m_terrainShader = CompileProgram(kTerrainVS, kTerrainFS, "terrain");

    // Batch VAO / VBO (dynamic, updated each frame)
    glGenVertexArrays(1, &m_batchVao);
    glGenBuffers(1, &m_batchVbo);
    glBindVertexArray(m_batchVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_batchVbo);
    // position: vec3 at location 0
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVert),
                          (void*)offsetof(BatchVert, pos));
    // colour: 4 normalized ubytes at location 1 → vec4
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(BatchVert),
                          (void*)offsetof(BatchVert, r));
    glBindVertexArray(0);

    // Bone SSBO (allocated on first skinned draw)
    glGenBuffers(1, &m_boneSsbo);

    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_DEPTH_TEST);
    // GL_LEQUAL so AlphaBlend shapes flush with pass-1 geometry (same depth)
    // pass the depth test rather than being silently culled.
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

GlSceneRenderer::~GlSceneRenderer()
{
    // Meshes
    for (auto& [id, gm] : m_meshes) {
        glDeleteVertexArrays(1, &gm.vao);
        unsigned int bufs[] = { gm.vboPos, gm.vboNorm, gm.vboUv,
                                gm.vboBoneIdx, gm.vboBoneWt, gm.vboVtxCol, gm.ebo };
        glDeleteBuffers(7, bufs);
    }
    // Textures
    for (auto& [id, tex] : m_textures)
        glDeleteTextures(1, &tex);

    if (m_batchVao) glDeleteVertexArrays(1, &m_batchVao);
    if (m_batchVbo) glDeleteBuffers(1, &m_batchVbo);
    if (m_boneSsbo) glDeleteBuffers(1, &m_boneSsbo);

    if (m_fbo)      glDeleteFramebuffers(1, &m_fbo);
    if (m_colorTex) glDeleteTextures(1, &m_colorTex);
    if (m_depthRbo) glDeleteRenderbuffers(1, &m_depthRbo);

    if (m_primShader)    glDeleteProgram(m_primShader);
    if (m_meshShader)    glDeleteProgram(m_meshShader);
    if (m_skinnedShader) glDeleteProgram(m_skinnedShader);
    if (m_terrainShader) glDeleteProgram(m_terrainShader);
}

// ── FBO management ────────────────────────────────────────────────────────────

void GlSceneRenderer::ResizeFbo(int w, int h)
{
    if (m_fbo) {
        glDeleteFramebuffers(1,  &m_fbo);
        glDeleteTextures(1,      &m_colorTex);
        glDeleteRenderbuffers(1, &m_depthRbo);
    }

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    glGenTextures(1, &m_colorTex);
    glBindTexture(GL_TEXTURE_2D, m_colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_colorTex, 0);

    glGenRenderbuffers(1, &m_depthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, m_depthRbo);

    GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fboStatus != GL_FRAMEBUFFER_COMPLETE)
        GlLog("SCT [GL] FBO incomplete at %dx%d  status=0x%X", w, h, fboStatus);
    else
        GlLog("SCT [GL] FBO OK %dx%d  colorTex=%u", w, h, m_colorTex);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_fboW = w;
    m_fboH = h;
}

// ── Frame lifecycle ───────────────────────────────────────────────────────────

void GlSceneRenderer::BeginFrame(int width, int height)
{
    if (width < 1)  width  = 1;
    if (height < 1) height = 1;

    if (width != m_fboW || height != m_fboH)
        ResizeFbo(width, height);

    // Drain any errors accumulated during init so per-draw checks are clean.
    { GLenum e; while ((e = glGetError()) != GL_NO_ERROR) (void)e; }

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, width, height);
    glClearColor(0.10f, 0.11f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_lineVerts.clear();
    m_pointVerts.clear();
}

void GlSceneRenderer::SetCamera(const glm::mat4& view, const glm::mat4& proj)
{
    m_view = view;
    m_proj = proj;
}

void GlSceneRenderer::SetLight(const glm::vec3& dir,
                                const glm::vec3& color,
                                const glm::vec3& ambient)
{
    m_lightDir     = dir;
    m_lightColor   = color;
    m_ambientColor = ambient;
}

void GlSceneRenderer::EndFrame()
{
    FlushBatch();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

ImTextureID GlSceneRenderer::GetOutputTexture() const
{
    return (ImTextureID)(uintptr_t)m_colorTex;
}

// ── Batch helpers ─────────────────────────────────────────────────────────────

void GlSceneRenderer::PushLine(glm::vec3 a, glm::vec3 b,
                                uint8_t r, uint8_t g, uint8_t b8, uint8_t alpha)
{
    m_lineVerts.push_back({ a, r, g, b8, alpha });
    m_lineVerts.push_back({ b, r, g, b8, alpha });
}

void GlSceneRenderer::PushPoint(glm::vec3 p,
                                 uint8_t r, uint8_t g, uint8_t b8, uint8_t alpha)
{
    m_pointVerts.push_back({ p, r, g, b8, alpha });
}

void GlSceneRenderer::FlushBatch()
{
    if (m_lineVerts.empty() && m_pointVerts.empty()) return;

    if (!m_primShader) {
        GlLog("SCT [GL] FlushBatch: prim shader is 0, skipping draw (%zu lines, %zu points)",
              m_lineVerts.size(), m_pointVerts.size());
        return;
    }

    glUseProgram(m_primShader);
    const glm::mat4 vp = m_proj * m_view;
    glUniformMatrix4fv(glGetUniformLocation(m_primShader, "u_vp"),
                       1, GL_FALSE, glm::value_ptr(vp));

    glBindVertexArray(m_batchVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_batchVbo);

    if (!m_lineVerts.empty()) {
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(m_lineVerts.size() * sizeof(BatchVert)),
                     m_lineVerts.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_LINES, 0, (GLsizei)m_lineVerts.size());
    }

    if (!m_pointVerts.empty()) {
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(m_pointVerts.size() * sizeof(BatchVert)),
                     m_pointVerts.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_POINTS, 0, (GLsizei)m_pointVerts.size());
    }

    glBindVertexArray(0);
}

// ── Draw: grid ────────────────────────────────────────────────────────────────

void GlSceneRenderer::DrawGrid(float cellSize, int halfExtent)
{
    const float ext   = cellSize * static_cast<float>(halfExtent);
    const uint8_t gc  = 80;  // grey

    // Grid in the XY plane (Z=0) — Skyrim's ground plane is Z-up.
    for (int i = -halfExtent; i <= halfExtent; ++i) {
        if (i == 0) continue;
        const float f = cellSize * static_cast<float>(i);
        PushLine({ f, -ext, 0.f }, { f,  ext, 0.f }, gc, gc, gc + 25, 255);
        PushLine({ -ext, f, 0.f }, { ext, f,  0.f }, gc, gc, gc + 25, 255);
    }
    // X axis (red = east)
    PushLine({ -ext, 0.f, 0.f }, { ext, 0.f, 0.f }, 210, 55,  55,  255);
    // Y axis (green = north)
    PushLine({ 0.f, -ext, 0.f }, { 0.f, ext, 0.f }, 55,  210, 55,  255);
}

// ── Draw: skeleton ────────────────────────────────────────────────────────────

void GlSceneRenderer::DrawSkeleton(const Skeleton& skel, const Pose& pose,
                                    const glm::mat4& world)
{
    const int n = (int)skel.bones.size();
    if (n == 0 || pose.empty()) return;

    // Bone sticks
    for (int i = 0; i < n; i++) {
        int p = skel.bones[i].parent;
        if (p < 0) continue;
        glm::vec3 a = glm::vec3(world * glm::vec4(pose.worldPos[i], 1.f));
        glm::vec3 b = glm::vec3(world * glm::vec4(pose.worldPos[p], 1.f));
        PushLine(a, b, 220, 200, 80, 230);
    }
    // Joint dots
    for (int i = 0; i < n; i++) {
        glm::vec3 p = glm::vec3(world * glm::vec4(pose.worldPos[i], 1.f));
        PushPoint(p, 255, 255, 255, 200);
    }
}

// ── GPU resource management ───────────────────────────────────────────────────

MeshHandle GlSceneRenderer::UploadMesh(const MeshData& data)
{
    if (data.positions.empty() || data.indices.empty()) return MeshHandle::Invalid;

    GpuMesh gm;
    gm.indexCount  = (int)data.indices.size();
    gm.vertexCount = (int)data.positions.size();
    gm.skinned     = !data.boneIndices.empty();

    glGenVertexArrays(1, &gm.vao);
    glBindVertexArray(gm.vao);

    auto uploadBuf = [](unsigned int& buf, GLenum target,
                        const void* data, GLsizeiptr sz) {
        glGenBuffers(1, &buf);
        glBindBuffer(target, buf);
        glBufferData(target, sz, data, GL_DYNAMIC_DRAW);
    };

    // Positions (location 0)
    uploadBuf(gm.vboPos, GL_ARRAY_BUFFER,
              data.positions.data(),
              (GLsizeiptr)(data.positions.size() * sizeof(glm::vec3)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    // Normals (location 1)
    if (!data.normals.empty()) {
        uploadBuf(gm.vboNorm, GL_ARRAY_BUFFER,
                  data.normals.data(),
                  (GLsizeiptr)(data.normals.size() * sizeof(glm::vec3)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    // UVs (location 2)
    if (!data.uvs.empty()) {
        uploadBuf(gm.vboUv, GL_ARRAY_BUFFER,
                  data.uvs.data(),
                  (GLsizeiptr)(data.uvs.size() * sizeof(glm::vec2)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    // Bone indices (location 3) — integer attribute
    if (!data.boneIndices.empty()) {
        uploadBuf(gm.vboBoneIdx, GL_ARRAY_BUFFER,
                  data.boneIndices.data(),
                  (GLsizeiptr)(data.boneIndices.size() * sizeof(glm::u8vec4)));
        glEnableVertexAttribArray(3);
        glVertexAttribIPointer(3, 4, GL_UNSIGNED_BYTE, 0, nullptr);
    }

    // Bone weights (location 4)
    if (!data.boneWeights.empty()) {
        uploadBuf(gm.vboBoneWt, GL_ARRAY_BUFFER,
                  data.boneWeights.data(),
                  (GLsizeiptr)(data.boneWeights.size() * sizeof(glm::vec4)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    // Vertex colours (location 5) — normalised to [0,1] in the shader
    if (!data.vertexColors.empty()) {
        uploadBuf(gm.vboVtxCol, GL_ARRAY_BUFFER,
                  data.vertexColors.data(),
                  (GLsizeiptr)(data.vertexColors.size() * sizeof(glm::u8vec3)));
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 3, GL_UNSIGNED_BYTE, GL_TRUE, 0, nullptr);
    }

    // Index buffer
    uploadBuf(gm.ebo, GL_ELEMENT_ARRAY_BUFFER,
              data.indices.data(),
              (GLsizeiptr)(data.indices.size() * sizeof(uint16_t)));

    glBindVertexArray(0);

    const uint32_t id = m_nextMeshId++;
    m_meshes[id] = gm;
    return static_cast<MeshHandle>(id);
}

void GlSceneRenderer::FreeMesh(MeshHandle handle)
{
    auto it = m_meshes.find(static_cast<uint32_t>(handle));
    if (it == m_meshes.end()) return;
    GpuMesh& gm = it->second;
    glDeleteVertexArrays(1, &gm.vao);
    unsigned int bufs[] = { gm.vboPos, gm.vboNorm, gm.vboUv,
                             gm.vboBoneIdx, gm.vboBoneWt, gm.vboVtxCol, gm.ebo };
    glDeleteBuffers(7, bufs);
    m_meshes.erase(it);
}

void GlSceneRenderer::UpdateMeshPositions(MeshHandle handle,
                                           std::span<const glm::vec3> positions)
{
    auto it = m_meshes.find(static_cast<uint32_t>(handle));
    if (it == m_meshes.end()) return;
    const GpuMesh& gm = it->second;
    if (positions.empty() || (int)positions.size() != gm.vertexCount) return;
    glBindBuffer(GL_ARRAY_BUFFER, gm.vboPos);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(positions.size() * sizeof(glm::vec3)),
                 positions.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

TextureHandle GlSceneRenderer::LoadTexture(const std::string& path)
{
    if (path.empty()) return TextureHandle::Invalid;

    unsigned int glTex = DdsLoadGLTexture(path);
    if (!glTex) {
        GlLog("SCT [GL] LoadTexture: failed to load '%s'", path.c_str());
        return TextureHandle::Invalid;
    }

    const uint32_t id = m_nextTexId++;
    m_textures[id] = glTex;
    return static_cast<TextureHandle>(id);
}

TextureHandle GlSceneRenderer::LoadTextureFromMemory(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty()) return TextureHandle::Invalid;

    unsigned int glTex = DdsLoadGLTextureFromBuffer(bytes.data(), bytes.size());
    if (!glTex) {
        GlLog("SCT [GL] LoadTextureFromMemory: failed to upload %zu-byte DDS buffer",
              bytes.size());
        return TextureHandle::Invalid;
    }

    const uint32_t id = m_nextTexId++;
    m_textures[id] = glTex;
    return static_cast<TextureHandle>(id);
}

void GlSceneRenderer::FreeTexture(TextureHandle handle)
{
    auto it = m_textures.find(static_cast<uint32_t>(handle));
    if (it == m_textures.end()) return;
    glDeleteTextures(1, &it->second);
    m_textures.erase(it);
}

TextureHandle GlSceneRenderer::UploadBlendMap(const float* data33x33)
{
    unsigned int glTex = 0;
    glGenTextures(1, &glTex);
    glBindTexture(GL_TEXTURE_2D, glTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 33, 33, 0, GL_RED, GL_FLOAT, data33x33);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    const uint32_t id = m_nextTexId++;
    m_textures[id] = glTex;
    return static_cast<TextureHandle>(id);
}

// ── Draw: static mesh ─────────────────────────────────────────────────────────

void GlSceneRenderer::DrawMesh(MeshHandle handle, const glm::mat4& world,
                                const DrawSurface& surface)
{
    auto it = m_meshes.find(static_cast<uint32_t>(handle));
    if (it == m_meshes.end()) return;
    const GpuMesh& gm = it->second;

    glUseProgram(m_meshShader);
    const glm::mat4 mvp = m_proj * m_view * world;
    glUniformMatrix4fv(glGetUniformLocation(m_meshShader, "u_mvp"),
                       1, GL_FALSE, glm::value_ptr(mvp));
    glUniformMatrix4fv(glGetUniformLocation(m_meshShader, "u_model"),
                       1, GL_FALSE, glm::value_ptr(world));
    glUniform4fv(glGetUniformLocation(m_meshShader, "u_tint"),
                 1, glm::value_ptr(surface.tint));

    // Light uniforms
    const glm::vec3 camPos = glm::inverse(m_view)[3];
    glUniform3fv(glGetUniformLocation(m_meshShader, "u_lightDir"),     1, glm::value_ptr(m_lightDir));
    glUniform3fv(glGetUniformLocation(m_meshShader, "u_lightColor"),   1, glm::value_ptr(m_lightColor));
    glUniform3fv(glGetUniformLocation(m_meshShader, "u_ambientColor"), 1, glm::value_ptr(m_ambientColor));
    glUniform3fv(glGetUniformLocation(m_meshShader, "u_camPos"),       1, glm::value_ptr(camPos));

    int alphaMode = 0;
    if      (surface.blendMode == DrawSurface::BlendMode::AlphaTest)        alphaMode = 1;
    else if (surface.blendMode == DrawSurface::BlendMode::AlphaBlend)       alphaMode = 2;
    else if (surface.blendMode == DrawSurface::BlendMode::Additive)         alphaMode = 3;
    else if (surface.blendMode == DrawSurface::BlendMode::AlphaTestAndBlend) alphaMode = 4;
    glUniform1i(glGetUniformLocation(m_meshShader, "u_alphaMode"),      alphaMode);
    glUniform1f(glGetUniformLocation(m_meshShader, "u_alphaThreshold"), surface.alphaThreshold);

    glUniform1i(glGetUniformLocation(m_meshShader, "u_useVtxColor"),
                surface.useVertexColor ? 1 : 0);
    glUniform1i(glGetUniformLocation(m_meshShader, "u_terrainTex"),  surface.terrainTex  ? 1 : 0);
    glUniform1f(glGetUniformLocation(m_meshShader, "u_terrainTile"), surface.texTileRate);

    const bool hasTex = (surface.diffuse != TextureHandle::Invalid);
    glUniform1i(glGetUniformLocation(m_meshShader, "u_hasTex"), hasTex ? 1 : 0);
    if (hasTex) {
        auto texIt = m_textures.find(static_cast<uint32_t>(surface.diffuse));
        if (texIt != m_textures.end()) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texIt->second);
            glUniform1i(glGetUniformLocation(m_meshShader, "u_diffuse"), 0);
        }
    }

    // Blend state — AlphaTest and AlphaTestAndBlend keep depth write on (surviving
    // pixels are opaque; correct occlusion requires depth writes).
    // AlphaBlend and Additive turn depth write off.
    if (surface.blendMode == DrawSurface::BlendMode::Additive)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    const bool depthWriteOff = (alphaMode == 2 || alphaMode == 3);
    if (depthWriteOff) glDepthMask(GL_FALSE);

    glBindVertexArray(gm.vao);
    if (surface.wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glDrawElements(GL_TRIANGLES, gm.indexCount, GL_UNSIGNED_SHORT, nullptr);
    if (surface.wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glBindVertexArray(0);

    // Restore blend state
    if (surface.blendMode == DrawSurface::BlendMode::Additive)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (depthWriteOff) glDepthMask(GL_TRUE);
}

// ── Draw: skinned mesh ────────────────────────────────────────────────────────

void GlSceneRenderer::DrawSkinnedMesh(MeshHandle handle, const glm::mat4& world,
                                       std::span<const glm::mat4> boneTransforms,
                                       const DrawSurface& surface)
{
    auto it = m_meshes.find(static_cast<uint32_t>(handle));
    if (it == m_meshes.end()) return;
    const GpuMesh& gm = it->second;

    // Upload bone transforms to SSBO
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_boneSsbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 (GLsizeiptr)(boneTransforms.size() * sizeof(glm::mat4)),
                 boneTransforms.data(), GL_STREAM_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_boneSsbo);

    glUseProgram(m_skinnedShader);
    const glm::mat4 vp = m_proj * m_view;
    glUniformMatrix4fv(glGetUniformLocation(m_skinnedShader, "u_vp"),
                       1, GL_FALSE, glm::value_ptr(vp));
    glUniformMatrix4fv(glGetUniformLocation(m_skinnedShader, "u_model"),
                       1, GL_FALSE, glm::value_ptr(world));
    glUniform4fv(glGetUniformLocation(m_skinnedShader, "u_tint"),
                 1, glm::value_ptr(surface.tint));

    // Light uniforms
    const glm::vec3 camPos = glm::inverse(m_view)[3];
    glUniform3fv(glGetUniformLocation(m_skinnedShader, "u_lightDir"),     1, glm::value_ptr(m_lightDir));
    glUniform3fv(glGetUniformLocation(m_skinnedShader, "u_lightColor"),   1, glm::value_ptr(m_lightColor));
    glUniform3fv(glGetUniformLocation(m_skinnedShader, "u_ambientColor"), 1, glm::value_ptr(m_ambientColor));
    glUniform3fv(glGetUniformLocation(m_skinnedShader, "u_camPos"),       1, glm::value_ptr(camPos));

    int alphaMode = 0;
    if      (surface.blendMode == DrawSurface::BlendMode::AlphaTest)        alphaMode = 1;
    else if (surface.blendMode == DrawSurface::BlendMode::AlphaBlend)       alphaMode = 2;
    else if (surface.blendMode == DrawSurface::BlendMode::Additive)         alphaMode = 3;
    else if (surface.blendMode == DrawSurface::BlendMode::AlphaTestAndBlend) alphaMode = 4;
    glUniform1i(glGetUniformLocation(m_skinnedShader, "u_alphaMode"),      alphaMode);
    glUniform1f(glGetUniformLocation(m_skinnedShader, "u_alphaThreshold"), surface.alphaThreshold);

    glUniform1i(glGetUniformLocation(m_skinnedShader, "u_useVtxColor"), 0);

    const bool hasTex = (surface.diffuse != TextureHandle::Invalid);
    glUniform1i(glGetUniformLocation(m_skinnedShader, "u_hasTex"), hasTex ? 1 : 0);
    if (hasTex) {
        auto texIt = m_textures.find(static_cast<uint32_t>(surface.diffuse));
        if (texIt != m_textures.end()) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texIt->second);
            glUniform1i(glGetUniformLocation(m_skinnedShader, "u_diffuse"), 0);
        }
    }

    // Blend state — AlphaTest and AlphaTestAndBlend keep depth write on.
    // AlphaBlend and Additive turn depth write off.
    if (surface.blendMode == DrawSurface::BlendMode::Additive)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    const bool depthWriteOff = (alphaMode == 2 || alphaMode == 3);
    if (depthWriteOff) glDepthMask(GL_FALSE);

    glBindVertexArray(gm.vao);
    glDrawElements(GL_TRIANGLES, gm.indexCount, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);

    // Restore blend state
    if (surface.blendMode == DrawSurface::BlendMode::Additive)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (depthWriteOff) glDepthMask(GL_TRUE);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// ── Draw: terrain tile ────────────────────────────────────────────────────────

void GlSceneRenderer::DrawTerrainTile(MeshHandle handle, const TerrainSurface& surf)
{
    auto it = m_meshes.find(static_cast<uint32_t>(handle));
    if (it == m_meshes.end()) return;
    const GpuMesh& gm = it->second;

    if (!m_terrainShader) return;
    glUseProgram(m_terrainShader);

    const glm::mat4 vp = m_proj * m_view;
    glUniformMatrix4fv(glGetUniformLocation(m_terrainShader, "u_vp"),
                       1, GL_FALSE, glm::value_ptr(vp));

    const glm::vec3 camPos = glm::inverse(m_view)[3];
    glUniform3fv(glGetUniformLocation(m_terrainShader, "u_lightDir"),     1, glm::value_ptr(m_lightDir));
    glUniform3fv(glGetUniformLocation(m_terrainShader, "u_lightColor"),   1, glm::value_ptr(m_lightColor));
    glUniform3fv(glGetUniformLocation(m_terrainShader, "u_ambientColor"), 1, glm::value_ptr(m_ambientColor));
    glUniform3fv(glGetUniformLocation(m_terrainShader, "u_camPos"),       1, glm::value_ptr(camPos));

    const int lc = std::max(1, std::min(surf.layerCount, TerrainSurface::kMaxLayers));
    glUniform1i(glGetUniformLocation(m_terrainShader, "u_layerCount"),   lc);
    glUniform1i(glGetUniformLocation(m_terrainShader, "u_useVtxColor"),  surf.useVertexColor ? 1 : 0);
    glUniform1fv(glGetUniformLocation(m_terrainShader, "u_tileRate"),    TerrainSurface::kMaxLayers,
                 surf.layerRates);

    // Diffuse layer textures → texture units 0-5
    static const int kLayerUnits[6] = { 0, 1, 2, 3, 4, 5 };
    for (int i = 0; i < lc; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        auto texIt = m_textures.find(static_cast<uint32_t>(surf.layers[i]));
        if (texIt != m_textures.end())
            glBindTexture(GL_TEXTURE_2D, texIt->second);
    }
    glUniform1iv(glGetUniformLocation(m_terrainShader, "u_layer"),
                 TerrainSurface::kMaxLayers, kLayerUnits);

    // Blend map textures → texture units 6-10
    static const int kBlendUnits[5] = { 6, 7, 8, 9, 10 };
    for (int i = 0; i < lc - 1; ++i) {
        glActiveTexture(GL_TEXTURE6 + i);
        auto texIt = m_textures.find(static_cast<uint32_t>(surf.blendMaps[i]));
        if (texIt != m_textures.end())
            glBindTexture(GL_TEXTURE_2D, texIt->second);
    }
    glUniform1iv(glGetUniformLocation(m_terrainShader, "u_blend"),
                 TerrainSurface::kMaxLayers - 1, kBlendUnits);

    glBindVertexArray(gm.vao);
    glDrawElements(GL_TRIANGLES, gm.indexCount, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);

    glActiveTexture(GL_TEXTURE0);
}
