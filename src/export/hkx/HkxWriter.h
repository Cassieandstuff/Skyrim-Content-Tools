#pragma once
#include "export/hkx/BehaviorGraph.h"
#include "export/hkx/AnimationExport.h"
#include <string>

// ── HkxWriter ─────────────────────────────────────────────────────────────────
// Main HKX export pipeline: converts an ActorAnimExport manifest into two HKX
// files per actor — a behavior graph HKX and an animation HKX.
//
// Behavior HKX: hkRootLevelContainer → hkbBehaviorGraph → hkbStateMachine
//               → per-clip hkbClipGenerators
// Animation HKX: hkRootLevelContainer → hkaAnimationContainer → clips[]
//
// Both files are written as Havok packfile XML and must be compiled to binary
// by hkxcmd (or equivalent) before Engine Relay can load them.

struct HkxWriteResult {
    bool        ok = false;
    std::string errorMessage;
};

// Write behavior graph + animation HKX for one actor.
// behaviorOutPath and animOutPath receive XML packfile content (UTF-8, no BOM).
HkxWriteResult WriteActorHkx(
    const ActorAnimExport& actorExport,
    const std::string&     behaviorOutPath,
    const std::string&     animOutPath);
