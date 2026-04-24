#include "HavokSkeleton.h"
#include <pugixml.hpp>
#include <glm/gtc/quaternion.hpp>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string_view>

// ── tuple parsers ────────────────────────────────────────────────────────────

static const char* ParseTuple3(const char* p, float& x, float& y, float& z)
{
    while (*p && *p != '(') ++p;
    if (!*p) return p;
    ++p;
    char* ep;
    x = std::strtof(p, &ep); p = ep;
    y = std::strtof(p, &ep); p = ep;
    z = std::strtof(p, &ep); p = ep;
    while (*p && *p != ')') ++p;
    if (*p) ++p;
    return p;
}

static const char* ParseTuple4(const char* p,
                                float& x, float& y, float& z, float& w)
{
    while (*p && *p != '(') ++p;
    if (!*p) return p;
    ++p;
    char* ep;
    x = std::strtof(p, &ep); p = ep;
    y = std::strtof(p, &ep); p = ep;
    z = std::strtof(p, &ep); p = ep;
    w = std::strtof(p, &ep); p = ep;
    while (*p && *p != ')') ++p;
    if (*p) ++p;
    return p;
}

// ── main loader ──────────────────────────────────────────────────────────────

bool LoadHavokSkeletonXml(const char* path, Skeleton& out,
                           char* errOut, int errLen)
{
    pugi::xml_document doc;
    auto res = doc.load_file(path);
    if (!res) {
        std::snprintf(errOut, errLen, "XML: %s", res.description());
        return false;
    }

    // Walk __data__ section for the first hkaSkeleton object
    pugi::xml_node skelNode;
    auto section = doc.child("hkpackfile").child("hksection");
    for (auto node : section.children("hkobject")) {
        if (std::string_view(node.attribute("class").as_string()) == "hkaSkeleton") {
            skelNode = node;
            break;
        }
    }
    if (!skelNode) {
        std::snprintf(errOut, errLen, "No hkaSkeleton in file");
        return false;
    }

    // parentIndices
    auto piNode = skelNode.find_child_by_attribute("hkparam", "name", "parentIndices");
    if (!piNode) {
        std::snprintf(errOut, errLen, "Missing parentIndices");
        return false;
    }
    std::vector<int> parents;
    {
        std::istringstream ss(piNode.text().as_string());
        int v;
        while (ss >> v) parents.push_back(v);
    }
    int n = static_cast<int>(parents.size());
    if (n == 0) {
        std::snprintf(errOut, errLen, "Empty parentIndices");
        return false;
    }

    // bone names
    auto bonesNode = skelNode.find_child_by_attribute("hkparam", "name", "bones");
    if (!bonesNode) {
        std::snprintf(errOut, errLen, "Missing bones param");
        return false;
    }
    std::vector<std::string> names;
    for (auto boneObj : bonesNode.children("hkobject")) {
        auto np = boneObj.find_child_by_attribute("hkparam", "name", "name");
        names.emplace_back(np ? np.text().as_string() : "");
    }

    // referencePose
    auto rpNode = skelNode.find_child_by_attribute("hkparam", "name", "referencePose");
    if (!rpNode) {
        std::snprintf(errOut, errLen, "Missing referencePose");
        return false;
    }
    std::vector<glm::vec3> localT(n);
    std::vector<glm::quat> localR(n);
    {
        const char* p = rpNode.text().as_string();
        for (int i = 0; i < n; i++) {
            float x,y,z, qx,qy,qz,qw, sx,sy,sz;
            p = ParseTuple3(p, x, y, z);
            p = ParseTuple4(p, qx, qy, qz, qw);
            p = ParseTuple3(p, sx, sy, sz);
            localT[i] = {x, y, z};
            localR[i] = glm::quat(qw, qx, qy, qz); // glm: (w, x, y, z)
        }
    }

    // Forward kinematics — Havok guarantees parents come before children
    std::vector<glm::vec3> worldT(n);
    std::vector<glm::quat> worldR(n);
    for (int i = 0; i < n; i++) {
        int p = parents[i];
        if (p < 0) {
            worldT[i] = localT[i];
            worldR[i] = localR[i];
        } else {
            worldT[i] = worldR[p] * localT[i] + worldT[p];
            worldR[i] = worldR[p] * localR[i];
        }
    }

    // Build output — swap Y↔Z to convert Havok Z-up → viewport Y-up
    out.bones.resize(n);
    out.worldPos.resize(n);
    for (int i = 0; i < n; i++) {
        out.bones[i].name   = i < static_cast<int>(names.size()) ? names[i] : "";
        out.bones[i].parent = parents[i];
        const auto& t = worldT[i];
        out.worldPos[i] = glm::vec3(t.x, t.z, t.y); // Z-up → Y-up
    }

    return true;
}
