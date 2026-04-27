#pragma once
#include "IPluginBackend.h"
#include <optional>
#include <string>

// A CastEntry is the authoring record for one participant in the scene.
// It captures identity data, skeleton reference, and optionally a linked NPC_
// record fetched from a plugin via the Mutagen backend.
//
// Cast entries live in AppState::cast[].
// Actor instances in the scene reference them by index.
struct CastEntry {
    std::string name;               // display / authoring name  ("Bandit_01")
    std::string editorId;           // plugin EditorID for export ("MyMod_NPC_Bandit01")
    std::string skeletonType;       // creature type from path, e.g. "character", "horse"
    int         skeletonIndex = -1; // index into AppState::skeletons (-1 = unassigned)

    // Skeleton source reference — stored so the project file can re-find it on load.
    std::string skeletonPath;       // BSA file path (fromBsa) or loose HKX full path
    std::string skeletonInternal;   // BSA internal lowercase path; empty for loose files

    // Body NIF — optional; empty until the user assigns one via Actor Properties.
    std::string nifPath;

    // Plugin NPC_ data — populated when the user assigns an existing NPC or creates
    // a new one via the Actor Properties panel.  Empty until then.
    std::optional<NpcRecord> npcRecord;
};
