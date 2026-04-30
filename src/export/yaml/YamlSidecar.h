#pragma once
#include "export/yaml/SpawnList.h"
#include "export/yaml/WaitNode.h"
#include <string>

// ── YamlSidecar ───────────────────────────────────────────────────────────────
// Serializes a complete scene export sidecar to a YAML file.
// The sidecar is consumed by Engine Relay at scene load time to:
//   1. Spawn actors at their authored placements
//   2. Drive wait-node blend parameters for per-actor behavior graphs
//
// Output format (yaml-cpp):
//   spawn:
//     - formKey: "XXXXXXXX:Plugin.esm"
//       pos: [x, y, z]
//       rot: [x, y, z]
//       scale: 1.0
//   waitNodes:
//     - actorFormKey: "…"
//       blendIn:  0.3
//       hold:     0.0
//       blendOut: 0.3

struct YamlSidecarResult {
    bool        ok = false;
    std::string errorMessage;
};

YamlSidecarResult WriteYamlSidecar(
    const std::string& outPath,
    const SpawnList&   spawnList,
    const WaitNode&    defaultWaitParams);
