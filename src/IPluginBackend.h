#pragma once
#include <cstdint>
#include <string>
#include <vector>

// One body-part NIF resolved from NPC_ WornArmor → Armor → ArmorAddon.
struct NpcBodyPart {
    std::string slot;    // "body", "hands", "feet"
    std::string nifPath; // data-relative path, always includes "Meshes\" prefix
};

// Data fetched from or created in a plugin file.
// Mirrors PluginBridge.NpcRecord (camelCase JSON).
struct NpcRecord {
    uint32_t    formId            = 0;
    std::string formKey;           // Mutagen format: "RRRRR:PluginName.esm"
    std::string editorId;
    std::string name;
    std::string raceEditorId;
    bool        isFemale          = false;
    std::string skeletonModelPath;              // race skeleton NIF path (C++ derives creature type from this)
    std::vector<std::string> expressionTriPaths; // all HDPT Part TRI paths across all head parts
    std::string facegenNifPath;                 // Meshes/Actors/Character/FaceGenData/FaceGeom/{plugin}/{id}.nif
    std::string pluginSource;      // source plugin filename, e.g. "Plugin.esp"

    // Body geometry from WornArmor → Armor → ArmorAddon (body, hands, feet slots).
    std::vector<NpcBodyPart> bodyParts;

    // Head part NIFs: race-default parts (ears, brows, teeth…) + NPC-chosen
    // parts (hair, eyes). Excludes the facegeom NIF (that's facegenNifPath).
    std::vector<std::string> headPartNifs;
};

// Parameters for creating a new NPC_ record in the project mod.
struct NpcCreateParams {
    std::string editorId;
    std::string name;
    std::string raceFormKey;   // Mutagen format: "RRRRR:PluginName.esm"
    bool        isFemale = false;
};

// Abstract plugin I/O backend.
// Concrete implementation: MutagenBackend (Skyrim SE via SctBridge/.NET).
// Future forks: Fallout4Backend, StarfieldBackend, etc.
struct IPluginBackend {
    virtual ~IPluginBackend() = default;

    // Load a plugin for reading. dataFolder = game Data directory.
    virtual bool PluginLoad(const std::string& pluginPath,
                            const std::string& dataFolder,
                            char* errOut, int errLen) = 0;
    virtual void PluginUnload() = 0;

    // Search NPC_ records by name or EditorID (case-insensitive).
    // Empty query returns up to maxResults records from the start of the plugin.
    virtual bool NpcSearch(const std::string& query, int maxResults,
                           std::vector<NpcRecord>& outRecords,
                           char* errOut, int errLen) = 0;

    // Fetch a single NPC_ record by local FormID (24-bit, no mod-index byte).
    virtual bool NpcGet(uint32_t formId,
                        NpcRecord& outRecord,
                        char* errOut, int errLen) = 0;

    // Project mod lifecycle — writable mod that accumulates new records.
    virtual bool ProjectNew(const std::string& modName,
                            char* errOut, int errLen) = 0;
    virtual bool ProjectLoad(const std::string& path,
                             char* errOut, int errLen) = 0;
    virtual bool ProjectSave(const std::string& path,
                             char* errOut, int errLen) = 0;

    // Create a new NPC_ record in the project mod.
    virtual bool NpcCreate(const NpcCreateParams& params,
                           NpcRecord& outRecord,
                           char* errOut, int errLen) = 0;
};
