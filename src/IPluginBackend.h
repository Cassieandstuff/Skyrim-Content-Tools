#pragma once
#include <array>
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

// A WRLD (worldspace) record found by sct_worldspace_search.
struct WorldspaceRecord {
    std::string formKey;      // "XXXXXXXX:Plugin.esm"
    std::string editorId;
    std::string name;         // FULL display name; may be empty
    std::string pluginSource; // e.g. "Skyrim.esm"
};

// One alpha-blended landscape texture layer (from ATXT + VTXT).
// blendMap is a pre-stitched 33×33 float grid (row-major, 0.0–1.0) covering
// the full cell by assembling the four 17×17 quadrant VTXT grids.
struct TerrainAlphaLayer {
    std::string              path;           // data-relative, Textures\ prefix guaranteed
    float                    tileRate = 6.f;
    std::array<float, 33*33> blendMap = {};  // opacity at each terrain vertex
};

// Decoded VHGT/VCLR/BTXT/ATXT terrain data for one exterior cell.
// Heights are world-space Z values in Skyrim units.
// Vertex colors are optional (hasColors == false if no VCLR sub-record).
struct LandRecord {
    float   heights[33][33]   = {};   // world-space Z, row-major
    bool    hasColors         = false;
    uint8_t colors[33][33][3] = {};   // RGB, 0-255
    // Base landscape texture (BTXT layer → LTEX → TXST diffuse path).
    // Empty string = no texture; data-relative, Textures\ prefix guaranteed.
    std::string baseTexPath;
    float       texTileRate   = 6.f;  // UV tiling multiplier (Skyrim default 6.0)
    // Alpha-blended overlay layers (ATXT + VTXT), up to 5 unique textures per cell.
    std::vector<TerrainAlphaLayer> alphaLayers;
};

// A CELL record found by sct_cell_search.
struct CellRecord {
    uint32_t    formId = 0;
    std::string formKey;      // "XXXXXXXX:Plugin.esm"
    std::string editorId;
    std::string name;         // in-game display name (FULL); may be empty
    std::string pluginSource; // e.g. "Skyrim.esm"
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
