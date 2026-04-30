#pragma once
#include "export/annotation/FaceAnnotation.h"
#include "export/annotation/EventAnnotation.h"
#include <string>
#include <vector>

// ── HkxAnnotation ─────────────────────────────────────────────────────────────
// One Havok annotation track entry: a timestamp + text payload.
// Written into hkaAnnotationTrack inside the animation HKX.
struct HkxAnnotation {
    float       time;
    std::string text;   // e.g. "MorphFace.RokokofaceUBE|jawOpen|42"
};

// ── AnnotationEncoder ─────────────────────────────────────────────────────────
// Converts face morph keyframes and event markers into sorted Havok annotation
// entries suitable for writing into an animation HKX annotation track.

// Encode face morph keyframes → annotation entries.
// Multiple morphs at the same timestamp each produce one entry.
std::vector<HkxAnnotation> EncodeFaceAnnotations(
    const std::vector<FaceAnnotation>& morphs);

// Encode audio cue events → annotation entries.
std::vector<HkxAnnotation> EncodeAudioCues(
    const std::vector<AudioCueAnnotation>& cues);

// Encode camera switch events → annotation entries.
std::vector<HkxAnnotation> EncodeCameraEvents(
    const std::vector<CameraEventAnnotation>& events);

// Merge and sort all annotation sources by time.
std::vector<HkxAnnotation> MergeAnnotations(
    std::vector<HkxAnnotation> a,
    std::vector<HkxAnnotation> b);
