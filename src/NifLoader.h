#pragma once
#include "renderer/ISceneRenderer.h"
#include <string>
#include <vector>

// One renderable shape extracted from a NIF file.
struct NifShape {
    std::string name;
    MeshData    meshData;
};

// All shapes loaded from a single NIF file.
struct NifAsset {
    std::vector<NifShape> shapes;
    bool empty() const { return shapes.empty(); }
};

// Load all renderable shapes from a NIF file into CPU-side MeshData.
// Returns an asset with empty shapes on failure (bad path, unsupported format, etc.).
NifAsset LoadNif(const std::string& path);
