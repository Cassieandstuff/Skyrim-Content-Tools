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

static bool ParseSkeletonDoc(const pugi::xml_document& doc,
                              Skeleton& out, char* errOut, int errLen)
{
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

    // referencePose — stored as local transforms per bone
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
            float x, y, z, qx, qy, qz, qw, sx, sy, sz;
            p = ParseTuple3(p, x, y, z);
            p = ParseTuple4(p, qx, qy, qz, qw);
            p = ParseTuple3(p, sx, sy, sz);
            localT[i] = { x, y, z };
            localR[i] = glm::quat(qw, qx, qy, qz); // glm: (w, x, y, z)
        }
    }

    // Build output — local ref pose stored per bone; FK lives in Pose::SolveFK()
    out.bones.resize(n);
    for (int i = 0; i < n; i++) {
        out.bones[i].name   = i < static_cast<int>(names.size()) ? names[i] : "";
        out.bones[i].parent = parents[i];
        out.bones[i].refT   = localT[i];
        out.bones[i].refR   = localR[i];
    }

    return true;
}

bool LoadHavokSkeletonXml(const char* path, Skeleton& out, char* errOut, int errLen)
{
    pugi::xml_document doc;
    auto res = doc.load_file(path);
    if (!res) { std::snprintf(errOut, errLen, "XML: %s", res.description()); return false; }
    return ParseSkeletonDoc(doc, out, errOut, errLen);
}

bool LoadHavokSkeletonXmlFromBuffer(const char* xmlData, int xmlLen,
                                    Skeleton& out, char* errOut, int errLen)
{
    pugi::xml_document doc;
    auto res = doc.load_buffer(xmlData, static_cast<size_t>(xmlLen));
    if (!res) { std::snprintf(errOut, errLen, "XML: %s", res.description()); return false; }
    return ParseSkeletonDoc(doc, out, errOut, errLen);
}

// ── reference pose factory ───────────────────────────────────────────────────

Pose Skeleton::MakeReferencePose() const
{
    Pose p;
    const int n = static_cast<int>(bones.size());
    p.channels.resize(n);
    p.parents.resize(n);
    for (int i = 0; i < n; i++) {
        p.channels[i] = { bones[i].refT, bones[i].refR };
        p.parents[i]  = bones[i].parent;
    }
    p.SolveFK();
    return p;
}
