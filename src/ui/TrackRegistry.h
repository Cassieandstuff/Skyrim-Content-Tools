#pragma once
#include "Sequence.h"
#include "ActorDocument.h"
#include <functional>
#include <vector>
#include <imgui.h>

struct AppState;

// ── TrackTypeDef ───────────────────────────────────────────────────────────────
// Describes one track type: its identity, visual appearance, and behaviour.
// Register once at startup via TrackRegistry::Get().Register(...).
// Adding a new track type = fill one of these and call Register.
struct TrackTypeDef {
    TrackType   id;
    const char* label;       // displayed in the lane header, e.g. "Anim Clips"
    ImVec4      color;       // lane item tint / background

    // true  = scene-level lane (Camera, Audio); stored in Sequence::sceneTracks.
    // false = per-actor lane  (AnimClip, FaceData, LookAt); one per actor group.
    bool isSceneLevel = false;

    // Optional: return false to hide this lane for a particular cast entry.
    // nullptr = always show.
    std::function<bool(const ActorDocument&)> isCompatible;

    // Evaluate all active items in this lane at sequence time t and write the
    // result into eval.  nullptr = scaffolded / not yet implemented (no-op).
    std::function<void(const TrackLane&, float t, AppState&, ActorEval&)> evaluate;
};

// ── TrackRegistry ──────────────────────────────────────────────────────────────
// Singleton registry of all known track types.
// Iterated by: timeline panel (render lanes), Sequence::EnsureActorGroup (build lanes).
class TrackRegistry {
public:
    static TrackRegistry& Get();

    void Register(TrackTypeDef def);
    const TrackTypeDef* Find(TrackType id) const;
    const std::vector<TrackTypeDef>& All() const { return defs_; }

private:
    TrackRegistry() = default;
    std::vector<TrackTypeDef> defs_;
};

// Call once from App::Init() before any panels or sequences are created.
void RegisterAllTrackTypes();
