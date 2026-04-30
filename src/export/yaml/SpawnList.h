#pragma once
#include "export/yaml/ActorPlacement.h"
#include <string>
#include <vector>

// ── SpawnListEntry ────────────────────────────────────────────────────────────
// One actor to spawn: who (formKey) + where (placement).
struct SpawnListEntry {
    std::string     formKey;    // actor base FormKey — same as ActorPlacement::formKey
    ActorPlacement  placement;
};

// ── SpawnList ─────────────────────────────────────────────────────────────────
// Full spawn manifest for one exported scene.
struct SpawnList {
    std::vector<SpawnListEntry> entries;
};
