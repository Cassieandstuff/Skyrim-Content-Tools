#pragma once

// ── WaitNode ──────────────────────────────────────────────────────────────────
// Parameters for the hkbWaitUntilConditionGenerator nodes that bracket each
// scene clip in Engine Relay's custom behavior graph template.
struct WaitNode {
    float blendInTime  = 0.3f;  // seconds — crossfade in from idle
    float holdDuration = 0.f;   // seconds — 0 = hold until externally exited
    float blendOutTime = 0.3f;  // seconds — crossfade out to idle
};
