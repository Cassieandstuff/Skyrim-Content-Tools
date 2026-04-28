#include "NifEditorState.h"
#include "AppState.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cfloat>

static const glm::mat4 kNifToWorld =
    glm::rotate(glm::mat4(1.f), glm::radians(-90.f), glm::vec3(1.f, 0.f, 0.f));

NifEditorState::NifEditorState(ISceneRenderer& r)
    : renderer(r)
{
    camera.azimuth   = 210.f;
    camera.elevation = 15.f;
    camera.radius    = 150.f;
}

NifEditorState::~NifEditorState()
{
    FreeHandles();
}

void NifEditorState::FreeHandles()
{
    for (MeshHandle h : handles)
        renderer.FreeMesh(h);
    handles.clear();

    for (TextureHandle t : textures)
        renderer.FreeTexture(t);
    textures.clear();
}

void NifEditorState::LoadFile(const std::string& path, const AppState& state)
{
    FreeHandles();
    doc           = LoadNifDocument(path);
    selectedBlock = -1;
    ++layoutVersion;

    for (const NifDocShape& ds : doc.shapes) {
        handles.push_back(renderer.UploadMesh(ds.meshData));

        TextureHandle th = TextureHandle::Invalid;
        if (!ds.diffusePath.empty()) {
            std::vector<uint8_t> texBytes;
            if (state.ResolveAsset(ds.diffusePath, texBytes))
                th = renderer.LoadTextureFromMemory(texBytes);
        }
        textures.push_back(th);
    }

    // Frame camera to AABB of all shapes transformed into world space.
    if (!doc.shapes.empty()) {
        glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
        for (const NifDocShape& ds : doc.shapes) {
            const glm::mat4& toRoot = doc.blocks[ds.blockIndex].toRoot;
            for (const glm::vec3& lp : ds.meshData.positions) {
                glm::vec3 wp = glm::vec3(kNifToWorld * toRoot * glm::vec4(lp, 1.f));
                mn = glm::min(mn, wp);
                mx = glm::max(mx, wp);
            }
        }
        camera.target    = (mn + mx) * 0.5f;
        camera.radius    = glm::length(mx - mn) * 0.8f;
        camera.azimuth   = 210.f;
        camera.elevation = 15.f;
    }
}
