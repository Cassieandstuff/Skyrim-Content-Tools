#pragma once
#include <string>

// ── FaceAnnotation ────────────────────────────────────────────────────────────
// One face morph keyframe encoded as a Havok annotation track entry.
// Serialised as: "MorphFace.<system>|<morphName>|<intWeight>"
// where intWeight is floor(weight * 100).
struct FaceAnnotation {
    float       time;       // seconds on the animation timeline
    std::string system;     // capture system tag, e.g. "RokokofaceUBE"
    std::string morphName;  // ARKit blend shape name, e.g. "jawOpen"
    float       weight;     // normalized 0–1
};
