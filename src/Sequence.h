#pragma once
#include "Pose.h"
#include <string>
#include <vector>
#include <functional>

// Forward declaration — full definition in AppState.h.
struct AppState;

// ── Track types ───────────────────────────────────────────────────────────────
// Per-actor tracks describe data that belongs to one character.
// Scene-level tracks (Camera, Audio) belong to the whole scene.
enum class TrackType {
    AnimClip,  // body animation (per-actor)
    FaceData,  // facial animation / morphs (per-actor, humanoid)
    LookAt,    // look-at target constraint (per-actor)
    Camera,    // scene camera (scene-level)
    Audio,     // sound / voice (scene-level)
};

// ── SequenceItem ──────────────────────────────────────────────────────────────
// One asset placed on the sequence timeline.
// assetIndex indexes into the relevant AppState pool (clips[], etc.).
// All times are in seconds.
struct SequenceItem {
    int   assetIndex = -1;  // index into pool (e.g. AppState::clips)
    float seqStart   = 0.f; // position on the sequence clock
    float trimIn     = 0.f; // start offset into source asset
    float trimOut    = 0.f; // end offset into source asset

    float blendIn    = 0.f; // crossfade ramp duration at head (seconds)
    float blendOut   = 0.f; // crossfade ramp duration at tail (seconds)

    // Derived helpers
    float SeqEnd()    const { return seqStart + (trimOut > trimIn ? trimOut - trimIn : 0.f); }
    float SourceDur() const { return trimOut > trimIn ? trimOut - trimIn : 0.f; }
    bool  ActiveAt(float t) const { return t >= seqStart && t <= SeqEnd(); }
};

// ── TrackLane ─────────────────────────────────────────────────────────────────
// A horizontal row in the timeline for one TrackType.
// Inside an ActorTrackGroup = per-actor lane.
// Inside Sequence::sceneTracks  = scene-level lane.
struct TrackLane {
    TrackType                 type;
    std::vector<SequenceItem> items;
};

// ── ActorTrackGroup ────────────────────────────────────────────────────────────
// All track lanes belonging to one actor, keyed by actorIndex.
// One group per actor; groups are created/destroyed as actors are added/removed.
struct ActorTrackGroup {
    int                    actorIndex = -1;
    bool                   collapsed  = false;
    std::vector<TrackLane> lanes;
};

// ── ActorEval ─────────────────────────────────────────────────────────────────
// Output of Sequence::Evaluate for one actor at a given time.
// Written by the TrackRegistry evaluate functions, read by the viewport renderer.
struct ActorEval {
    Pose pose;
    // Future fields: morphWeights, lookAtTarget, etc.
};

// ── Sequence ──────────────────────────────────────────────────────────────────
// The NLE sequence document: a collection of actor track groups and scene tracks.
// Evaluation produces one ActorEval per actor.
struct Sequence {
    std::string                  name;
    std::vector<ActorTrackGroup> actorTracks;
    std::vector<TrackLane>       sceneTracks;  // Camera, Audio, etc.

    // Total duration = end of the last item across all lanes.
    float Duration() const;

    // Evaluate all track lanes at time t → fill out[actorIndex].
    // Callers must resize out to at least state.actors.size() before calling.
    void Evaluate(float t, AppState& state, std::vector<ActorEval>& out) const;

    // Structure helpers
    void EnsureActorGroup(int actorIndex);
    void EnsureSceneTrack(TrackType type);

    bool Empty() const { return actorTracks.empty() && sceneTracks.empty(); }
};
