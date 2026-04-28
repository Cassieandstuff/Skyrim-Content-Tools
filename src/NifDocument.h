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

// One entry in a shape's skin binding: name of the skeleton bone and its
// inverse-bind matrix (skin-to-bone transform in NIF/Havok Z-up space).
struct SkinBoneBinding {
    std::string boneName;
    glm::mat4   inverseBindMatrix{ 1.f }; // NIF skin space → bone local space
};

// Geometry for one renderable shape.
// Vertex positions are stored in LOCAL shape space.
// To render static:  apply block.toRoot then kNifToWorld.
// To render skinned: model = identity; pass skinMats to DrawSkinnedMesh.
struct NifDocShape {
    int         blockIndex;   // → NifDocument::blocks[blockIndex]
    MeshData    meshData;     // boneIndices/boneWeights filled iff isSkinned
    std::string diffusePath;  // relative texture path, e.g. "textures/actors/…_d.dds"

    bool                         isSkinned = false;
    std::vector<SkinBoneBinding> skinBindings; // indexed by skin-local bone index
};

struct NifDocument {
    std::string              path;
    std::vector<NifBlock>    blocks;
    std::vector<NifDocShape> shapes;  // one per renderable block (isShape == true)
    std::vector<int>         roots;   // top-level block indices (parent == -1)

    bool empty() const { return blocks.empty(); }
};

NifDocument LoadNifDocument(const std::string& path);
NifDocument LoadNifDocumentFromBytes(const std::vector<uint8_t>& bytes,
                                     const std::string& debugPath = {});
