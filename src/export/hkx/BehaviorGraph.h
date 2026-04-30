#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ── hkbStateMachine ───────────────────────────────────────────────────────────
// Top-level node type for a per-actor behavior graph.
struct HkbState {
    std::string name;       // e.g. "SCT_Clip_0"
    int         stateId;    // unique int within this state machine
    std::string generatorRef; // reference to an hkbClipGenerator or hkbBlendTree node
};

struct HkbStateMachine {
    std::string          name;       // e.g. "SCT_BehaviorGraph"
    int                  startStateId = 0;
    std::vector<HkbState> states;
};

// ── hkbClipGenerator ─────────────────────────────────────────────────────────
// Wraps one animation HKX clip inside the behavior graph.
struct HkbClipGenerator {
    std::string name;           // e.g. "SCT_Clip_0"
    std::string animationName;  // path relative to meshes\actors\…\animations\
    float       cropStartAmountLocalTime = 0.f;
    float       cropEndAmountLocalTime   = 0.f;
    int         mode = 0;       // 0 = LOOPING, 1 = SINGLE_PLAY, 4 = USER_CONTROLLED
};

// ── hkbBlendTree ─────────────────────────────────────────────────────────────
// Simple two-child blend node (reserved for future crossfade support).
struct HkbBlendTreeChild {
    std::string generatorRef;
    float       weight = 0.5f;
};

struct HkbBlendTree {
    std::string                    name;
    std::vector<HkbBlendTreeChild> children;
};
