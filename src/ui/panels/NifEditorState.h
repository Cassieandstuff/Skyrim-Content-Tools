#pragma once
#include "NifDocument.h"
#include "Camera.h"
#include "renderer/ISceneRenderer.h"
#include <vector>
#include <string>

struct AppState;  // forward declaration — full definition in AppState.h

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

    explicit NifEditorState(ISceneRenderer& r);
    ~NifEditorState();

    // Loads and displays a NIF file; resolves textures via state.ResolveAsset().
    void LoadFile(const std::string& path, const AppState& state);
    void FreeHandles();
};
