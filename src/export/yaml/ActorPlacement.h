#pragma once
#include <string>

// ── ActorPlacement ────────────────────────────────────────────────────────────
// Per-actor world placement record written to the YAML sidecar.
// Position and rotation use Skyrim/Havok Z-up world space (same as REFR records).
struct ActorPlacement {
    std::string formKey;        // "XXXXXXXX:Plugin.esm"
    float posX = 0.f, posY = 0.f, posZ = 0.f;  // world position in Skyrim units
    float rotX = 0.f, rotY = 0.f, rotZ = 0.f;  // Euler angles in radians (extrinsic ZYX)
    float scale = 1.f;
};
