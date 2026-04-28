#pragma once
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

struct PoseChannel {
    glm::vec3 localT{ 0.f, 0.f, 0.f };
    glm::quat localR{ 1.f, 0.f, 0.f, 0.f };
};

// Live bone pose. Sources (reference pose, clip evaluator, user drag) write
// channels; SolveFK() accumulates and writes worldPos (Y-up) and
// boneWorldMat (NIF/Havok Z-up space, for skinning math).
struct Pose {
    std::vector<PoseChannel> channels;
    std::vector<int>         parents;
    std::vector<glm::vec3>   worldPos;     // Y-up, written by SolveFK()
    std::vector<glm::mat4>   boneWorldMat; // NIF/Havok Z-up space (BEFORE YZ-swap), for LBS

    void SolveFK();
    bool empty() const { return channels.empty(); }
};
