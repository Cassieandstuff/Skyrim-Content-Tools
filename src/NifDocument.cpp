#include "NifDocument.h"
#include "NifAnim.h"
#include "NifFile.hpp"
#include "Animation.hpp"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <unordered_map>

// ── Shared parse logic ────────────────────────────────────────────────────────
// Caller is responsible for loading `nif`; debugPath is used only for logging.

static NifDocument ParseNifFile(nifly::NifFile& nif, const std::string& debugPath)
{
    NifDocument doc;
    doc.path = debugPath;

    // Map nifly block ID → our block index.
    std::unordered_map<uint32_t, int> nifIdToIdx;

    // Walk the parent chain to accumulate the shape/node's transform to NIF root.
    // MatTransform::ComposeTransforms semantics:
    //   t1.ComposeTransforms(t2).Apply(v) == t1.Apply(t2.Apply(v))
    // So parent.ComposeTransforms(child) converts child-local → root space.
    auto computeToRoot = [&](nifly::NiAVObject* obj) -> nifly::MatTransform {
        nifly::MatTransform result = obj->transform; // local → parent
        nifly::NiNode* cur = nif.GetParentNode(obj);
        while (cur) {
            nifly::NiNode* grandparent = nif.GetParentNode(cur);
            // Skip the root node's own transform — the game engine's REFR placement
            // replaces it rather than stacking on top of it.
            if (grandparent)
                result = cur->transform.ComposeTransforms(result);
            cur = grandparent;
        }
        return result; // local → NIF root (root node's own transform excluded)
    };

    // ── Pass 1: NiNode-type blocks ────────────────────────────────────────────
    std::vector<nifly::NiNode*> nodeObjs;
    for (nifly::NiNode* node : nif.GetNodes()) {
        const int idx = (int)doc.blocks.size();
        nifIdToIdx[nif.GetBlockID(node)] = idx;
        nodeObjs.push_back(node);

        NifBlock b;
        b.index    = idx;
        b.kind     = node->GetBlockName();
        b.name     = node->name.get();
        b.isShape  = false;
        b.toParent = node->transform.ToGLMMatrix<glm::mat4>();
        b.toRoot   = computeToRoot(node).ToGLMMatrix<glm::mat4>();
        doc.blocks.push_back(std::move(b));
    }

    // ── Pass 2: shape blocks ──────────────────────────────────────────────────
    std::vector<nifly::NiShape*> shapeObjs;
    for (nifly::NiShape* shape : nif.GetShapes()) {
        const int idx = (int)doc.blocks.size();
        nifIdToIdx[nif.GetBlockID(shape)] = idx;
        shapeObjs.push_back(shape);

        NifBlock b;
        b.index    = idx;
        b.kind     = shape->GetBlockName();
        b.name     = shape->name.get();
        b.isShape  = true;
        b.toParent = shape->transform.ToGLMMatrix<glm::mat4>();
        b.toRoot   = computeToRoot(shape).ToGLMMatrix<glm::mat4>();
        doc.blocks.push_back(std::move(b));
    }

    // ── Pass 3: parent–child links ────────────────────────────────────────────
    auto wire = [&](nifly::NiAVObject* obj, int myIdx) {
        nifly::NiNode* par = nif.GetParentNode(obj);
        if (par) {
            auto it = nifIdToIdx.find(nif.GetBlockID(par));
            if (it != nifIdToIdx.end()) {
                const int pi = it->second;
                doc.blocks[myIdx].parent = pi;
                doc.blocks[pi].children.push_back(myIdx);
                return;
            }
        }
        doc.roots.push_back(myIdx);
    };

    for (int i = 0; i < (int)nodeObjs.size(); i++)
        wire(nodeObjs[i], i);
    for (int si = 0; si < (int)shapeObjs.size(); si++)
        wire(shapeObjs[si], (int)nodeObjs.size() + si);

    // ── Pass 4: geometry ──────────────────────────────────────────────────────
    for (int si = 0; si < (int)shapeObjs.size(); si++) {
        nifly::NiShape* shape = shapeObjs[si];
        const int blockIdx = (int)nodeObjs.size() + si;

        const auto* verts = nif.GetVertsForShape(shape);
        std::vector<nifly::Triangle> tris;
        shape->GetTriangles(tris);

        if (!verts || verts->empty() || tris.empty()) continue;

        NifDocShape ds;
        ds.blockIndex = blockIdx;

        // Diffuse texture path (slot 0).
        {
            std::string texPath;
            if (nif.GetTextureSlot(shape, texPath, 0) && !texPath.empty()) {
                // Normalize backslashes to forward slashes.
                for (char& c : texPath) if (c == '\\') c = '/';
                ds.diffusePath = std::move(texPath);
            }
        }

        // Positions in LOCAL shape space (caller transforms via block.toRoot).
        ds.meshData.positions.reserve(verts->size());
        for (const auto& v : *verts)
            ds.meshData.positions.push_back({ v.x, v.y, v.z });

        // Normals
        if (const auto* norms = nif.GetNormalsForShape(shape)) {
            ds.meshData.normals.reserve(norms->size());
            for (const auto& n : *norms)
                ds.meshData.normals.push_back({ n.x, n.y, n.z });
        } else {
            ds.meshData.normals.assign(verts->size(), { 0.f, 1.f, 0.f });
        }

        // UVs
        if (const auto* uvs = nif.GetUvsForShape(shape)) {
            ds.meshData.uvs.reserve(uvs->size());
            for (const auto& uv : *uvs)
                ds.meshData.uvs.push_back({ uv.u, uv.v });
        } else {
            ds.meshData.uvs.assign(verts->size(), { 0.f, 0.f });
        }

        // Indices
        ds.meshData.indices.reserve(tris.size() * 3);
        for (const auto& t : tris) {
            ds.meshData.indices.push_back(t.p1);
            ds.meshData.indices.push_back(t.p2);
            ds.meshData.indices.push_back(t.p3);
        }

        // ── Skin binding (BSTriShape + BSSkin::Instance) ─────────────────────
        if (shape->IsSkinned()) {
            std::vector<std::string> boneNames;
            nif.GetShapeBoneList(shape, boneNames);
            const int numSkinBones = (int)boneNames.size();

            if (numSkinBones > 0) {
                ds.isSkinned = true;
                ds.skinBindings.resize(numSkinBones);

                for (int j = 0; j < numSkinBones; j++) {
                    ds.skinBindings[j].boneName = boneNames[j];
                    nifly::MatTransform xform;
                    if (nif.GetShapeTransformSkinToBone(shape, (uint32_t)j, xform))
                        ds.skinBindings[j].inverseBindMatrix = xform.ToGLMMatrix<glm::mat4>();
                    // else keep identity
                }

                // Per-vertex bone indices and weights.
                // Accumulate from per-bone weight maps into per-vertex slots (max 4).
                const int numVerts = (int)ds.meshData.positions.size();
                ds.meshData.boneIndices.assign(numVerts, glm::u8vec4(0));
                ds.meshData.boneWeights.assign(numVerts, glm::vec4(0.f));

                // Track how many influences have been written per vertex.
                std::vector<int> slotUsed(numVerts, 0);

                for (int j = 0; j < numSkinBones; j++) {
                    std::unordered_map<uint16_t, float> weightsMap;
                    nif.GetShapeBoneWeights(shape, (uint32_t)j, weightsMap);
                    for (const auto& [vi, wt] : weightsMap) {
                        if (vi >= (uint16_t)numVerts) continue;
                        const int slot = slotUsed[vi];
                        if (slot >= 4) continue;
                        ds.meshData.boneIndices[vi][slot] = (uint8_t)std::min(j, 255);
                        ds.meshData.boneWeights[vi][slot] = wt;
                        slotUsed[vi]++;
                    }
                }

                fprintf(stderr, "[NifDoc] '%s' shape '%s': skinned, %d bones\n",
                        debugPath.c_str(), shape->name.get().c_str(), numSkinBones);
            }
        }

        // ── Alpha mode from NiAlphaProperty ──────────────────────────────────
        // NiAlphaProperty::flags bitfield (Gamebryo/Bethesda):
        //   bit 0      : alpha blending enable
        //   bits 1-4   : src blend factor  (6 = SRC_ALPHA)
        //   bits 5-8   : dst blend factor  (0 = ONE, 7 = ONE_MINUS_SRC_ALPHA)
        //   bit 9      : alpha test enable
        // Additive: blend enable + dst == 0 (ONE).
        // NifSkope treats blendOn and testOn as independent booleans — both can be
        // active simultaneously.  Use independent if-statements (not else-if).
        if (auto* ap = nif.GetAlphaProperty(shape)) {
            const uint16_t fl  = ap->flags;
            const bool blendOn = (fl & 0x0001) != 0;
            const bool testOn  = (fl & 0x0200) != 0;

            if (testOn) {
                ds.alphaThreshold = ap->threshold / 255.f;
            }

            if (blendOn && testOn) {
                const int dst = (fl >> 5) & 0xF;
                // Additive+test is unusual; treat as AlphaTestAndBlend regardless.
                ds.alphaMode = (dst == 0) ? NifAlphaMode::Additive
                                           : NifAlphaMode::AlphaTestAndBlend;
            } else if (blendOn) {
                const int dst = (fl >> 5) & 0xF;
                ds.alphaMode = (dst == 0) ? NifAlphaMode::Additive
                                           : NifAlphaMode::AlphaBlend;
            } else if (testOn) {
                ds.alphaMode = NifAlphaMode::AlphaTest;
            }
        }

        // ── Parent chain for animated transform recomposition ────────────────
        // Walk up from the shape's direct parent, collecting (name, restLocal)
        // pairs in inside-out order (direct parent first, root last-excluded).
        // Mirrors the computeToRoot exclusion: the NIF root node's own transform
        // is skipped because REFR placement replaces it.
        ds.shapeLocal = shape->transform.ToGLMMatrix<glm::mat4>();
        {
            nifly::NiNode* cur = nif.GetParentNode(shape);
            while (cur) {
                nifly::NiNode* gp = nif.GetParentNode(cur);
                if (!gp) break; // cur is the root — excluded
                ds.parentChain.emplace_back(cur->name.get(),
                                            cur->transform.ToGLMMatrix<glm::mat4>());
                cur = gp;
            }
        }

        // ── Vertex morph animation (NiGeomMorpherController on this shape) ──────
        {
            nifly::NiTimeController* ctrl =
                nif.GetHeader().GetBlock<nifly::NiTimeController>(shape->controllerRef);
            while (ctrl) {
                if (auto* gmc = dynamic_cast<nifly::NiGeomMorpherController*>(ctrl)) {
                    ds.morphAnim = ParseNifShapeMorph(nif, *gmc,
                                       (uint32_t)ds.meshData.positions.size());
                    break;
                }
                ctrl = nif.GetHeader().GetBlock<nifly::NiTimeController>(
                    ctrl->nextControllerRef);
            }
        }

        doc.shapes.push_back(std::move(ds));
    }

    // ── Pass 5: extra data blocks ─────────────────────────────────────────────
    // NiExtraData subclasses are attached via extraDataRefs on NiAVObject (nodes
    // and shapes).  They are not reachable from GetNodes() / GetShapes().
    auto addExtraData = [&](nifly::NiAVObject* avobj, int parentIdx) {
        for (auto& ref : avobj->extraDataRefs) {
            if (ref.IsEmpty()) continue;
            auto* ed = nif.GetHeader().GetBlock<nifly::NiExtraData>(ref);
            if (!ed) continue;

            const int idx = (int)doc.blocks.size();

            NifBlock b;
            b.index       = idx;
            b.kind        = ed->GetBlockName();
            b.name        = ed->name.get();
            b.parent      = parentIdx;
            b.isShape     = false;
            b.isExtraData = true;
            b.toParent    = glm::mat4(1.f);
            b.toRoot      = glm::mat4(1.f);

            // Human-readable value for the properties panel.
            char vbuf[128];
            if (auto* pBsx = dynamic_cast<nifly::BSXFlags*>(ed)) {
                std::snprintf(vbuf, sizeof(vbuf), "0x%08X", pBsx->integerData);
                b.extraValue = vbuf;
            } else if (auto* pBhk = dynamic_cast<nifly::BSBehaviorGraphExtraData*>(ed)) {
                b.extraValue = pBhk->behaviorGraphFile.get();
            } else if (auto* pStr = dynamic_cast<nifly::NiStringExtraData*>(ed)) {
                b.extraValue = pStr->stringData.get();
            } else if (auto* pInt = dynamic_cast<nifly::NiIntegerExtraData*>(ed)) {
                std::snprintf(vbuf, sizeof(vbuf), "%u", pInt->integerData);
                b.extraValue = vbuf;
            } else if (auto* pFlt = dynamic_cast<nifly::NiFloatExtraData*>(ed)) {
                std::snprintf(vbuf, sizeof(vbuf), "%.4f", pFlt->floatData);
                b.extraValue = vbuf;
            } else if (auto* pInv = dynamic_cast<nifly::BSInvMarker*>(ed)) {
                std::snprintf(vbuf, sizeof(vbuf), "rot(%u,%u,%u) zoom=%.3f",
                              pInv->rotationX, pInv->rotationY, pInv->rotationZ, pInv->zoom);
                b.extraValue = vbuf;
            }

            doc.blocks.push_back(std::move(b));
            doc.blocks[parentIdx].children.push_back(idx);
        }
    };

    for (int i = 0; i < (int)nodeObjs.size(); i++)
        addExtraData(nodeObjs[i], i);
    for (int si = 0; si < (int)shapeObjs.size(); si++)
        addExtraData(shapeObjs[si], (int)nodeObjs.size() + si);

    fprintf(stderr, "[NifDoc] '%s': %d blocks (%d nodes, %d shapes, %d extra data)\n",
            debugPath.c_str(), (int)doc.blocks.size(),
            (int)nodeObjs.size(), (int)doc.shapes.size(),
            (int)doc.blocks.size() - (int)nodeObjs.size() - (int)doc.shapes.size());

    // ── Pass 6: animation controllers ────────────────────────────────────────
    doc.animClip = ParseNifAnim(nif);

    return doc;
}

// ── Public API ────────────────────────────────────────────────────────────────

NifDocument LoadNifDocument(const std::string& path)
{
    nifly::NifFile nif;
    try {
        if (nif.Load(path) != 0) {
            fprintf(stderr, "[NifDoc] Load('%s') failed\n", path.c_str());
            return {};
        }
    } catch (...) {
        fprintf(stderr, "[NifDoc] Load('%s') threw\n", path.c_str());
        return {};
    }
    return ParseNifFile(nif, path);
}

NifDocument LoadNifDocumentFromBytes(const std::vector<uint8_t>& bytes,
                                     const std::string& debugPath)
{
    if (bytes.empty()) return {};
    nifly::NifFile nif;
    try {
        // std::istringstream doesn't do newline translation, so binary data is safe.
        std::string buf(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::istringstream iss(std::move(buf));
        if (nif.Load(iss) != 0) {
            fprintf(stderr, "[NifDoc] Load from bytes failed ('%s')\n", debugPath.c_str());
            return {};
        }
    } catch (...) {
        fprintf(stderr, "[NifDoc] Load from bytes threw ('%s')\n", debugPath.c_str());
        return {};
    }
    return ParseNifFile(nif, debugPath);
}
