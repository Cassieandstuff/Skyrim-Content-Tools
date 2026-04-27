#include "NifDocument.h"
#include "NifFile.hpp"
#include <cstdio>
#include <unordered_map>

NifDocument LoadNifDocument(const std::string& path)
{
    NifDocument doc;
    doc.path = path;

    nifly::NifFile nif;
    try {
        if (nif.Load(path) != 0) {
            fprintf(stderr, "[NifDoc] Load('%s') failed\n", path.c_str());
            return doc;
        }
    } catch (...) {
        fprintf(stderr, "[NifDoc] Load('%s') threw\n", path.c_str());
        return doc;
    }

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
            result = cur->transform.ComposeTransforms(result);
            cur = nif.GetParentNode(cur);
        }
        return result; // local → NIF root
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
            if (auto* p = dynamic_cast<nifly::BSXFlags*>(ed)) {
                std::snprintf(vbuf, sizeof(vbuf), "0x%08X", p->integerData);
                b.extraValue = vbuf;
            } else if (auto* p = dynamic_cast<nifly::BSBehaviorGraphExtraData*>(ed)) {
                b.extraValue = p->behaviorGraphFile.get();
            } else if (auto* p = dynamic_cast<nifly::NiStringExtraData*>(ed)) {
                b.extraValue = p->stringData.get();
            } else if (auto* p = dynamic_cast<nifly::NiIntegerExtraData*>(ed)) {
                std::snprintf(vbuf, sizeof(vbuf), "%u", p->integerData);
                b.extraValue = vbuf;
            } else if (auto* p = dynamic_cast<nifly::NiFloatExtraData*>(ed)) {
                std::snprintf(vbuf, sizeof(vbuf), "%.4f", p->floatData);
                b.extraValue = vbuf;
            } else if (auto* p = dynamic_cast<nifly::BSInvMarker*>(ed)) {
                std::snprintf(vbuf, sizeof(vbuf), "rot(%u,%u,%u) zoom=%.3f",
                              p->rotationX, p->rotationY, p->rotationZ, p->zoom);
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
            path.c_str(), (int)doc.blocks.size(),
            (int)nodeObjs.size(), (int)doc.shapes.size(),
            (int)doc.blocks.size() - (int)nodeObjs.size() - (int)doc.shapes.size());
    return doc;
}
