#pragma once
#include <string>

// ── AudioCueAnnotation ────────────────────────────────────────────────────────
// Triggers an audio event (voice line / SFX) at a specific time.
// Serialised as: "AudioCue|<eventName>" in the Havok annotation track.
struct AudioCueAnnotation {
    float       time;       // seconds on the animation timeline
    std::string eventName;  // Papyrus sound event name or FMOD event path
};

// ── CameraEventAnnotation ─────────────────────────────────────────────────────
// Switches the Engine Relay camera to a named CameraShot at a specific time.
// Serialised as: "CameraShot|<shotName>" in the Havok annotation track.
struct CameraEventAnnotation {
    float       time;       // seconds on the animation timeline
    std::string shotName;   // matches CameraShot::name in AppState::cameraShots
};
