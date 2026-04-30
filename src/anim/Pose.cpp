#include "anim/Pose.h"
#include <glm/gtc/matrix_transform.hpp>

void Pose::SolveFK()
{
    const int n = static_cast<int>(channels.size());
    worldPos.resize(n);
    boneWorldMat.resize(n);

    std::vector<glm::vec3> wT(n);
    std::vector<glm::quat> wR(n);

    for (int i = 0; i < n; i++) {
        int p = parents[i];
        if (p < 0) {
            wT[i] = channels[i].localT;
            wR[i] = channels[i].localR;
        } else {
            wT[i] = wR[p] * channels[i].localT + wT[p];
            wR[i] = wR[p] * channels[i].localR;
        }
        // SCT world is Z-up (same as Havok/Skyrim) — no axis conversion needed.
        worldPos[i] = glm::vec3(wT[i].x, wT[i].y, wT[i].z);

        // Full 4x4 world matrix in NIF/Havok Z-up space, used for LBS skinning.
        // skinMat[j] = kNifToWorld * boneWorldMat[skelBoneIdx] * inverseBindMatrix
        boneWorldMat[i] = glm::mat4_cast(wR[i]);
        boneWorldMat[i][3] = glm::vec4(wT[i], 1.f);
    }
}
