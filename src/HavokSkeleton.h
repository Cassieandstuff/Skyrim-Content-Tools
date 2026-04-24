#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>

struct SkeletonBone {
    std::string name;
    int         parent = -1;
};

struct Skeleton {
    std::vector<SkeletonBone> bones;
    std::vector<glm::vec3>    worldPos;  // Y-up, Havok units

    bool empty() const { return bones.empty(); }
};

// Parse the first hkaSkeleton from a Havok packfile XML.
// Returns true on success; writes a message into errOut on failure.
bool LoadHavokSkeletonXml(const char* path, Skeleton& out, char* errOut, int errLen);
