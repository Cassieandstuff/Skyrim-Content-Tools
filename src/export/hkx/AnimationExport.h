#pragma once
#include <string>
#include <vector>

// ── AnimationExport ───────────────────────────────────────────────────────────
// Maps a Sequence clip placement on the Beat timeline to a concrete HKX clip
// index within the exported per-actor animation file.

struct AnimClipExportEntry {
    int         sequenceItemIndex; // index into Sequence::actorTracks[ai].lanes[…].items
    int         hkxClipIndex;      // 0-based index in the animation HKX clip array
    std::string clipName;          // e.g. "SCT_Clip_0"
    float       trimIn  = 0.f;     // seconds — trim from clip start
    float       trimOut = 0.f;     // seconds — trim from clip end
};

// Per-actor export manifest: one entry per placed clip on the timeline.
struct ActorAnimExport {
    int                           actorIndex;   // index into AppState::actors
    std::string                   formKey;       // actor FormKey for YAML cross-reference
    std::vector<AnimClipExportEntry> clips;
};
