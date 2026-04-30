#pragma once
#include "asset/NifAnim.h"
#include "renderer/ISceneRenderer.h"
#include <glm/glm.hpp>
#include <string>
#include <utility>
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

// Alpha rendering mode decoded from NiAlphaProperty flags.
// Opaque:          no alpha property, or blending/testing both disabled.
// AlphaTest:       bit 9 only — discard fragments below threshold (depth write on).
// AlphaBlend:      bit 0 only — standard SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend.
// Additive:        bit 0 + dst==ONE — fire, glows, particles.
// AlphaTestAndBlend: bits 0 and 9 both set — discard AND blend simultaneously
//                  (NifSkope applies both GL states independently; depth write off).
enum class NifAlphaMode { Opaque, AlphaTest, AlphaBlend, Additive, AlphaTestAndBlend };

// Geometry for one renderable shape.
// Vertex positions are stored in LOCAL shape space.
// To render static:  apply block.toRoot then kNifToWorld.
// To render skinned: model = identity; pass skinMats to DrawSkinnedMesh.
struct NifDocShape {
    int         blockIndex;   // → NifDocument::blocks[blockIndex]
    MeshData    meshData;     // boneIndices/boneWeights filled iff isSkinned
    std::string diffusePath;  // relative texture path, e.g. "textures/actors/…_d.dds"

    NifAlphaMode alphaMode      = NifAlphaMode::Opaque;
    float        alphaThreshold = 0.5f;  // normalised (0-1); from NiAlphaProperty::threshold/255

    bool                         isSkinned = false;
    std::vector<SkinBoneBinding> skinBindings; // indexed by skin-local bone index

    // For animated NIFs: the shape's own local-to-parent transform and the chain
    // of ancestor nodes from direct parent up to (but not including) the NIF root.
    // Each entry is (nodeName, restLocalTransform).  Used by the viewport to
    // recompose toRoot at runtime when controller data is present.
    glm::mat4 shapeLocal{ 1.f };
    std::vector<std::pair<std::string, glm::mat4>> parentChain;

    // Vertex morph animation from NiGeomMorpherController (banners, flags, etc.).
    // Empty for non-morphing shapes.
    NifShapeMorphAnim morphAnim;
};

struct NifDocument {
    std::string              path;
    std::vector<NifBlock>    blocks;
    std::vector<NifDocShape> shapes;  // one per renderable block (isShape == true)
    std::vector<int>         roots;   // top-level block indices (parent == -1)
    NifAnimClip              animClip; // empty if no NiControllerManager found

    bool empty() const { return blocks.empty(); }
};

NifDocument LoadNifDocument(const std::string& path);
NifDocument LoadNifDocumentFromBytes(const std::vector<uint8_t>& bytes,
                                     const std::string& debugPath = {});
