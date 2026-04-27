#include "AnimClip.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>

void AnimClip::Evaluate(float t, Pose& pose) const
{
    if (numFrames == 0 || numTracks == 0 || duration <= 0.f) return;

    t = std::clamp(t, 0.f, duration);

    if (numFrames == 1) {
        for (int tk = 0; tk < numTracks; ++tk) {
            int b = trackToBone[tk];
            if (b < 0 || b >= (int)pose.channels.size()) continue;
            pose.channels[b] = transforms[tk];
        }
        return;
    }

    const float fi    = t * static_cast<float>(numFrames - 1) / duration;
    int         f0    = static_cast<int>(fi);
    const float alpha = fi - static_cast<float>(f0);
    f0 = std::clamp(f0, 0, numFrames - 2);
    const int f1 = f0 + 1;

    for (int tk = 0; tk < numTracks; ++tk) {
        int b = trackToBone[tk];
        if (b < 0 || b >= (int)pose.channels.size()) continue;
        const PoseChannel& c0 = transforms[f0 * numTracks + tk];
        const PoseChannel& c1 = transforms[f1 * numTracks + tk];
        pose.channels[b].localT = glm::mix(c0.localT, c1.localT, alpha);
        pose.channels[b].localR = glm::slerp(c0.localR, c1.localR, alpha);
    }
}
