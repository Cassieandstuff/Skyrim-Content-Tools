#pragma once
#include "ActorDocument.h"
#include "AnimClip.h"
#include "HavokSkeleton.h"
#include "IPluginBackend.h"
#include "Sequence.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ── Toast notifications ───────────────────────────────────────────────────────
enum class ToastLevel { Info, Warning, Error };
struct Toast {
    std::string message;
    float       ttl;
    ToastLevel  level = ToastLevel::Info;
};

// ── DiscoveredSkeleton ─────────────────────────────────────────────────────────
// One skeleton HKX found by scanning the data folder — either loose or inside a BSA.
struct DiscoveredSkeleton {
    std::string path;          // loose: full file path; BSA: full path to the .bsa file
    std::string bsaInternal;   // BSA-internal lowercase path; empty for loose files
    std::string creatureType;  // folder name under actors/, e.g. "horse"
    std::string displayName;   // filename stem, e.g. "skeleton"

    bool fromBsa() const { return !bsaInternal.empty(); }
};

// Extract creature type from a skeleton/animation file path.
// "Data\meshes\actors\horse\character assets\skeleton.hkx"  →  "horse"
// Returns empty string if the actors/ segment is not found.
std::string ExtractCreatureType(const char* path);

// ── AppTab ─────────────────────────────────────────────────────────────────────
enum class AppTab {
    SceneEditor = 0,
    AnimEditor,
    NifEditor,
    Workflow,       // Plugin browser, asset promotion, previs generation
};

// ── Actor ──────────────────────────────────────────────────────────────────────
// An actor is an instance of a CastEntry placed in the active scene.
// It holds the "which character" reference; the skeleton and animation data
// are reached through castIndex → CastEntry::skeletonIndex → AppState::skeletons.
struct Actor {
    int castIndex = -1;  // index into AppState::cast
    // World placement — single actor at origin for now; extend for multi-actor.
    // glm::vec3 worldPos = {0,0,0};
    // glm::quat worldRot = {1,0,0,0};
};

// ── AppState ────────────────────────────────────────────────────────────────────
// Single source of truth shared across every panel and system.
// Lives in MainLayout, passed by reference to every IPanel::Draw call.
struct AppState {

    // ── Asset pools (the "bins") ────────────────────────────────────────────────
    std::vector<AnimClip>  clips;     // animation clip library (immutable once loaded)
    std::vector<Skeleton>  skeletons; // loaded skeleton definitions (deduplicated)

    // ── Cast ─────────────────────────────────────────────────────────────────────
    std::vector<ActorDocument> cast;  // actor authoring documents

    // ── Scene ─────────────────────────────────────────────────────────────────────
    std::vector<Actor>     actors;    // actor instances active in the scene
    Sequence               sequence;  // the NLE timeline document

    // ── Playback ───────────────────────────────────────────────────────────────────
    float  time    = 0.f;
    bool   playing = false;
    bool   loop    = true;

    // ── UI state ────────────────────────────────────────────────────────────────────
    AppTab activeTab    = AppTab::SceneEditor;
    int    selectedClip = -1;           // highlighted clip in the bin
    int    selectedCast = -1;           // selected cast entry (-1 = none)

    // ── Data folder ──────────────────────────────────────────────────────────────────
    std::string                   dataFolder;           // path to the game's Data directory
    std::vector<DiscoveredSkeleton> discoveredSkeletons; // populated by ScanDataFolder()
    std::vector<std::string>      discoveredPlugins;    // .esp/.esm/.esl filenames at dataFolder root
    // Ordered BSA search list (highest priority first): plugin-associated BSAs then
    // ini-listed base-game BSAs.  Rebuilt by ScanDataFolder().
    std::vector<std::string>      bsaSearchList;

    // MFEE (MuFacialExpressionExtended) vanilla→extended TRI map.
    // Key:   normalized vanilla TRI path (lowercase, backslash, no "meshes\" prefix)
    // Value: normalized extended ARKit TRI path (same form, prepend "meshes\" for ResolveAsset)
    // Populated by ScanDataFolder() when the MFEE ini is present; empty otherwise.
    std::unordered_map<std::string, std::string> mfeeExtendedTris;

    // ── Plugin backend ────────────────────────────────────────────────────────────────
    // Set to std::make_unique<MutagenBackend>() after DotNetHost::Init() succeeds.
    // Null-checked before use; swap for a different backend to support other games.
    std::unique_ptr<IPluginBackend> pluginBackend;

    // ── Project file ─────────────────────────────────────────────────────────────────
    std::string projectPath;            // absolute path to current .sct file; empty if unsaved
    std::string projectName = "Untitled";
    bool        projectDirty = false;

    // ── Toast notifications ───────────────────────────────────────────────────────────
    std::vector<Toast> toasts;
    void PushToast(std::string msg, ToastLevel level = ToastLevel::Info, float duration = 4.f);

    // ── Error / status ──────────────────────────────────────────────────────────────
    char   importErr[256] = {};

    // ── Lifecycle ────────────────────────────────────────────────────────────────────

    // Advance playback clock; respects loop and sequence/clip duration.
    void Tick(float dt);

    // Clear all scene data (clips, cast, actors, skeletons, sequence) and reset
    // project metadata. Preserves dataFolder, discoveredSkeletons, pluginBackend.
    void NewProject();

    // Load a single animation clip from a file path (HKX or XML).
    // Sets sourcePath and skeletonType. Returns new clip index or -1 on failure.
    int LoadClipFromPath(const char* path, char* errOut, int errLen);

    // Scan dataFolder/meshes/actors/ for skeleton HKX files; fills discoveredSkeletons.
    // Also builds bsaSearchList from Skyrim.ini + plugin-associated BSAs.
    void ScanDataFolder();

    // Resolve a data-folder-relative asset path to raw bytes.
    // Checks loose file first, then walks bsaSearchList in priority order.
    // relPath uses either slash direction, any case — normalised internally.
    bool ResolveAsset(const std::string& relPath, std::vector<uint8_t>& outBytes) const;

    // Persist/restore dataFolder to sct_settings.ini (working directory).
    void LoadSettings();
    void SaveSettings() const;

    // ── Convenience accessors ────────────────────────────────────────────────────────

    // Skeleton for a given cast entry index, or nullptr if unassigned.
    const Skeleton* SkeletonForCast(int castIndex) const;

    // Skeleton for a given actor index, or nullptr.
    const Skeleton* SkeletonForActor(int actorIndex) const;

    // Load a skeleton HKX or XML file, add to skeletons[] (deduplication by path),
    // create a matching CastEntry in cast[], and add an Actor + sequence group.
    // Returns the new actorIndex, or -1 on failure.
    int LoadSkeletonAndAddActor(const char* path, char* errOut, int errLen);

    // Create a CastEntry + Actor from an NpcRecord (NPC-first flow).
    // The cast entry is pre-filled with name/editorId/npcRecord; skeletonIndex = -1.
    // Returns the new actorIndex, or -1 on failure.
    int AddActorFromRecord(const NpcRecord& rec);

    // Re-link an existing cast entry's identity + asset-cache fields to a new
    // NpcRecord.  Updates formId/formKey/raceEditorId/pluginSource/isFemale,
    // headNifPath, expressionTriPaths, headTriPath, and runs MFEE mapping to
    // rebuild extendedTriPaths.  Does NOT overwrite name/editorId (caller keeps
    // the author-set values).  Sets projectDirty = true.
    void RelinkActorFromRecord(int castIdx, const NpcRecord& rec);

    // Re-run the MFEE vanilla→extended mapping on an existing cast entry using
    // the current mfeeExtendedTris map (populated by ScanDataFolder).
    // No-op if expressionTriPaths is empty.  Clears the TRI doc cache.
    void RemapMfeeForActor(int castIdx);

    // Load a skeleton (loose file or extracted from BSA) and assign it to an
    // existing cast entry.  Returns true on success.
    bool AssignSkeletonToCast(int castIdx, const DiscoveredSkeleton& ds,
                              char* errOut, int errLen);
};
