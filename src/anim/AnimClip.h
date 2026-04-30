#pragma once
#include "anim/Pose.h"
#include <string>
#include <vector>

struct AnimClip {
    std::string name;
    std::string sourcePath;    // absolute path the clip was loaded from (for project save/load)
    std::string skeletonType;  // creature type derived from path, e.g. "character", "horse"
    float duration  = 0.f;
    int   numTracks = 0;
    int   numFrames = 0;
    // transforms[frame * numTracks + track], Havok Z-up local space
    std::vector<PoseChannel> transforms;
    // trackToBone[track] = bone index in skeleton
    std::vector<int> trackToBone;

    // Write interpolated local transforms into pose for the bones this clip covers.
    // Caller should pre-fill pose with reference channels before calling.
    void Evaluate(float t, Pose& pose) const;
};
