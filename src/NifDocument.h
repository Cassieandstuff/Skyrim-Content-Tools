#pragma once
#include "renderer/ISceneRenderer.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>

// One block in the NIF file hierarchy (NiNode, BSTriShape, etc.).
struct NifBlock {
    int              index;
    std::string      kind;            // "NiNode", "BSFadeNode", "BSTriShape", …
    std::string      name;
    int              parent = -1;     // index in NifDocument::blocks; -1 = root
    std::vector<int> children;
    glm::mat4        toParent{ 1.f }; // local transform to parent space
    glm::mat4        toRoot{ 1.f };   // accumulated transform to NIF root space
    bool             isShape     = false; // true → has an entry in NifDocument::shapes
    bool             isExtraData = false; // true → NiExtraData subclass
    std::string      extraValue;          // human-readable value for extra data blocks
};

// Geometry for one renderable shape.
// Vertex positions are stored in LOCAL shape space.
// To render: apply block.toRoot (NIF root space), then kNifToWorld (world space).
struct NifDocShape {
    int         blockIndex;   // → NifDocument::blocks[blockIndex]
    MeshData    meshData;
    std::string diffusePath;  // relative texture path, e.g. "textures/actors/…_d.dds"
};

struct NifDocument {
    std::string              path;
    std::vector<NifBlock>    blocks;
    std::vector<NifDocShape> shapes;  // one per renderable block (isShape == true)
    std::vector<int>         roots;   // top-level block indices (parent == -1)

    bool empty() const { return blocks.empty(); }
};

NifDocument LoadNifDocument(const std::string& path);
