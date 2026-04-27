#include "Pose.h"

void Pose::SolveFK()
{
    const int n = static_cast<int>(channels.size());
    worldPos.resize(n);

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
        // Havok Z-up → viewport Y-up
        worldPos[i] = glm::vec3(wT[i].x, wT[i].z, wT[i].y);
    }
}
