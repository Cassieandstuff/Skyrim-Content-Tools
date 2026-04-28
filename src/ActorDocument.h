#pragma once
#include "TriDocument.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// ── ActorDocument ─────────────────────────────────────────────────────────────
// Self-contained authoring record for one scene participant.
//
// Three logical layers:
//
//   Identity     — formId, formKey, editorId, name, pluginSource, raceEditorId
//                  Stable references for export and plugin re-resolution.
//
//   Asset cache  — creatureType, skeletonHkxPath/Internal, bodyNifPath,
//                  headNifPath, headTriPath, isFemale
//                  Resolved once from the plugin and re-serialised into the
//                  project file so Mutagen is not required for subsequent loads.
//
//   Overrides    — morphWeights
//                  Author's non-destructive edits applied on top of the base.
//
// Runtime field: skeletonIndex (index into AppState::skeletons) is not
// serialised — re-derived from the HKX paths on project load.
struct ActorDocument {

    // ── Identity ───────────────────────────────────────────────────────────────
    std::string name;
    std::string editorId;
    uint32_t    formId      = 0;
    std::string formKey;        // Mutagen format: "RRRRR:PluginName.esm"
    std::string raceEditorId;
    std::string pluginSource;   // source plugin filename, e.g. "Skyrim.esm"

    // ── Resolved asset cache ───────────────────────────────────────────────────
    std::string creatureType;           // "character", "horse", etc.
    std::string skeletonHkxPath;        // BSA file path or loose HKX full path
    std::string skeletonHkxInternal;    // BSA-internal lowercase path; empty = loose
    std::string bodyNifPath;            // torso/body NIF (slot 32); empty until assigned
    std::string handsNifPath;           // hands NIF (slot 33)
    std::string feetNifPath;            // feet NIF (slot 37)
    std::string headNifPath;            // facegeom NIF; empty if not an NPC
    std::string headTriPath;            // primary expression TRI (first entry in expressionTriPaths)
    std::vector<std::string> headPartNifs;       // race+NPC head part NIFs (hair, eyes, mouth, ears…)
    std::vector<std::string> expressionTriPaths; // all vanilla expression TRI paths from head parts
    std::vector<std::string> extendedTriPaths;   // MFEE-mapped extended ARKit TRI paths (meshes\-prefixed)
    bool        isFemale    = false;

    // ── Authored overrides (non-destructive) ───────────────────────────────────
    std::map<std::string, float> morphWeights;

    // ── Runtime — not serialised ───────────────────────────────────────────────
    int skeletonIndex  = -1;    // index into AppState::skeletons; -1 = not loaded

    // Lazily loaded from extendedTriPaths by InspectorPanel on first access.
    // Cleared when extendedTriPaths changes (relink/unlink).
    std::vector<TriDocument> triDocs;
    bool                     triDocsLoaded = false;

    // Returns true if this document has a plugin identity link.
    bool IsLinked() const { return formId != 0 || !formKey.empty(); }

    // Clear all plugin-derived identity and asset-cache fields, keeping
    // name/editorId/bodyNifPath/morphWeights which the author may have set.
    void Unlink()
    {
        formId       = 0;
        formKey      = {};
        raceEditorId = {};
        pluginSource = {};
        handsNifPath = {};
        feetNifPath  = {};
        headNifPath          = {};
        headTriPath          = {};
        headPartNifs.clear();
        expressionTriPaths.clear();
        extendedTriPaths.clear();
        triDocs.clear();
        triDocsLoaded = false;
        isFemale     = false;
    }
};
