#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "anim/Pose.h"

struct SkeletonBone {
    std::string name;
    int         parent = -1;
    glm::vec3   refT{ 0.f, 0.f, 0.f };
    glm::quat   refR{ 1.f, 0.f, 0.f, 0.f };
};

struct Skeleton {
    std::vector<SkeletonBone> bones;
    bool empty() const { return bones.empty(); }
    Pose MakeReferencePose() const;
};

bool LoadHavokSkeletonXml(const char* path, Skeleton& out, char* errOut, int errLen);

// Same as above but reads from an in-memory XML buffer.
bool LoadHavokSkeletonXmlFromBuffer(const char* xmlData, int xmlLen,
                                    Skeleton& out, char* errOut, int errLen);
