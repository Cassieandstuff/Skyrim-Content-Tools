#include "NifLoader.h"
#include "NifFile.hpp"
#include <glm/glm.hpp>
#include <cstdio>
#include <cfloat>

NifAsset LoadNif(const std::string& path)
{
    NifAsset result;

    nifly::NifFile nif;
    int rc = -1;
    try {
        rc = nif.Load(path);
    } catch (...) {}

    fprintf(stderr, "[NifLoader] Load('%s') rc=%d  shapes=%d\n",
            path.c_str(), rc, rc == 0 ? (int)nif.GetShapes().size() : 0);

    if (rc != 0) return result;

    for (auto* shape : nif.GetShapes()) {
        const auto* verts = nif.GetVertsForShape(shape);

        std::vector<nifly::Triangle> tris;
        shape->GetTriangles(tris);

        // bounding box for diagnosis
        glm::vec3 bmin(1e9f), bmax(-1e9f);
        if (verts) for (const auto& v : *verts) {
            bmin = glm::min(bmin, glm::vec3(v.x, v.y, v.z));
            bmax = glm::max(bmax, glm::vec3(v.x, v.y, v.z));
        }
        fprintf(stderr, "[NifLoader]   shape '%s'  verts=%d  tris=%d  bbox=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)\n",
                shape->name.get().c_str(),
                verts ? (int)verts->size() : -1,
                (int)tris.size(),
                bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z);

        if (!verts || verts->empty()) continue;
        if (tris.empty()) continue;

        NifShape ns;
        ns.name = shape->name.get();

        // Positions
        ns.meshData.positions.reserve(verts->size());
        for (const auto& v : *verts)
            ns.meshData.positions.push_back({ v.x, v.y, v.z });

        // Normals
        if (const auto* norms = nif.GetNormalsForShape(shape)) {
            ns.meshData.normals.reserve(norms->size());
            for (const auto& n : *norms)
                ns.meshData.normals.push_back({ n.x, n.y, n.z });
        } else {
            ns.meshData.normals.assign(verts->size(), { 0.f, 1.f, 0.f });
        }

        // UVs
        if (const auto* uvs = nif.GetUvsForShape(shape)) {
            ns.meshData.uvs.reserve(uvs->size());
            for (const auto& uv : *uvs)
                ns.meshData.uvs.push_back({ uv.u, uv.v });
        } else {
            ns.meshData.uvs.assign(verts->size(), { 0.f, 0.f });
        }

        // Indices
        ns.meshData.indices.reserve(tris.size() * 3);
        for (const auto& t : tris) {
            ns.meshData.indices.push_back(t.p1);
            ns.meshData.indices.push_back(t.p2);
            ns.meshData.indices.push_back(t.p3);
        }

        if (!ns.meshData.positions.empty()) {
            const auto& p = ns.meshData.positions[0];
            fprintf(stderr, "[NifLoader]   pos[0]=(%.4f,%.4f,%.4f)  indices[0..2]=%u %u %u\n",
                    p.x, p.y, p.z,
                    (unsigned)ns.meshData.indices[0],
                    (unsigned)ns.meshData.indices[1],
                    (unsigned)ns.meshData.indices[2]);
        }
        result.shapes.push_back(std::move(ns));
    }

    return result;
}
