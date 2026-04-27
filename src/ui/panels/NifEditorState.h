#pragma once
#include "NifDocument.h"
#include "Camera.h"
#include "renderer/ISceneRenderer.h"
#include <vector>
#include <string>

// Shared state for all three NIF editor sub-panels.
// Owned by MainLayout; injected by reference at panel construction time.
struct NifEditorState {
    NifDocument                doc;
    int                        selectedBlock = -1;
    int                        layoutVersion = 0;  // incremented on LoadFile
    Camera                     camera;
    std::vector<MeshHandle>    handles;   // parallel to doc.shapes
    std::vector<TextureHandle> textures;  // parallel to doc.shapes
    ISceneRenderer&            renderer;
    const std::string*         dataFolder = nullptr; // points into AppState

    explicit NifEditorState(ISceneRenderer& r);
    ~NifEditorState();

    void LoadFile(const std::string& path);
    void FreeHandles();
};
