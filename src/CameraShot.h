#pragma once
#include <glm/glm.hpp>
#include <string>

// ── CameraShot ─────────────────────────────────────────────────────────────────
// One named camera shot: world-space eye position + look direction + FOV.
// Stored in AppState::cameraShots[]; SequenceItem::assetIndex on a Camera
// scene track lane points into this pool.
//
// Angle convention: yaw/pitch match Camera::azimuth/elevation (degrees).
// The eye looks in the direction Camera::Eye() - Camera::target, i.e. the
// "outward" direction from the orbit pivot.
struct CameraShot {
    std::string name  = "Shot";
    glm::vec3   eye   = {0.f, 0.f, 128.f};  // world-space eye position
    float       yaw   = 0.f;   // azimuth  (degrees), same as Camera::azimuth
    float       pitch = 0.f;   // elevation (degrees), same as Camera::elevation
    float       fov   = 70.f;  // vertical field of view (degrees)
};
