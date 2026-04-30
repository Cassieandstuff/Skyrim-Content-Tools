#pragma once
#include "plugin/IPluginBackend.h"
#include <cstdint>
#include <map>
#include <string>
#include <utility>

// Bootstraps the .NET 10+ runtime and loads SctBridge.dll in-process once at
// startup. All functions are safe to call even if Init() failed — they return
// false / do nothing, so callers don't need to guard every use.
struct DotNetHost {
    // Call once before any bridge functions. Returns false if .NET or
    // SctBridge.dll are unavailable (errOut receives a human-readable reason).
    static bool Init(char* errOut, int errLen);

    static bool Ready();

    // Deserialize a binary .hkx file to Havok packfile XML in memory.
    // On success: *xmlOut is a heap buffer of *xmlLen bytes (null-terminated).
    //             Caller MUST call FreeBuffer(*xmlOut) after use.
    // On failure: errOut receives the error; returns false.
    static bool HkxToXml(const char* hkxPath,
                          char** xmlOut, int* xmlLen,
                          char* errOut, int errLen);

    static void FreeBuffer(void* ptr);

    // ── Plugin bridge (Mutagen backend) ──────────────────────────────────────
    // Loaded in Init() alongside the HKX bridge; non-fatal if unavailable.

    static bool PluginReady();

    // Load a plugin as a read-only overlay and build a link cache covering it
    // and all its declared direct masters. dataFolder = game Data directory.
    static bool PluginLoad(const char* pluginPath, const char* dataFolder,
                           char* errOut, int errLen);

    // Load the full active load order from plugins.txt (SE AppData) into the
    // bridge. Falls back to all ESM/ESL in dataFolder if plugins.txt not found.
    // Returns the number of mods loaded, or -1 on failure.
    static int  LoadOrderLoad(const char* dataFolder, char* errOut, int errLen);

    static void PluginUnload();

    // Search NPC_ records; outJson receives a UTF-8 JSON array string.
    static bool NpcSearch(const char* query, int maxResults,
                          std::string& outJson,
                          char* errOut, int errLen);

    // Fetch a single NPC_ by local FormID; outJson receives a JSON object.
    static bool NpcGet(uint32_t formId,
                       std::string& outJson,
                       char* errOut, int errLen);

    // Project mod lifecycle.
    static bool ProjectNew(const char* modName, char* errOut, int errLen);
    static bool ProjectLoad(const char* path,   char* errOut, int errLen);
    static bool ProjectSave(const char* path,   char* errOut, int errLen);

    // Create a new NPC_ record; inJson = NpcCreateRequest JSON,
    // outJson receives the created NpcRecord as JSON.
    static bool NpcCreate(const char* inJson,
                          std::string& outJson,
                          char* errOut, int errLen);

    // Search interior CELL records; outJson receives a UTF-8 JSON array.
    // query = substring matched against EditorID and full name (empty = all).
    static bool CellSearch(const char* query, int maxResults,
                           std::string& outJson,
                           char* errOut, int errLen);

    // Fetch all placed objects in a cell as a JSON array of CellRefRecord.
    // formKey = FormKey string as returned by CellSearch ("XXXXXXXX:Plugin.esm").
    // Each entry carries nifPath, position, rotation (radians, Havok Z-up), scale,
    // and a baseFormKey suitable for use as a mesh-catalog instancing key.
    static bool CellGetRefs(const char* formKey,
                            std::string& outJson,
                            char* errOut, int errLen);

    // Search worldspaces; outJson receives a UTF-8 JSON array of WorldspaceRecord.
    static bool WorldspaceSearch(const char* query, int maxResults,
                                 std::string& outJson,
                                 char* errOut, int errLen);

    // Fetch placed refs for an exterior cell at grid (cellX, cellY) in the given
    // worldspace.  Same JSON format as CellGetRefs.
    static bool ExteriorCellGetRefs(const char* worldspaceFormKey, int cellX, int cellY,
                                    std::string& outJson,
                                    char* errOut, int errLen);

    // Fetch decoded terrain height + colour data for the LAND record of an exterior
    // cell.  outJson = { heights:[float x 1089], colors:[byte x 3267 or empty] }.
    static bool LandGetData(const char* worldspaceFormKey, int cellX, int cellY,
                            std::string& outJson,
                            char* errOut, int errLen);

    // Fetch all LAND records for a worldspace in one binary round-trip (SLRT format).
    // Header: magic(4) + version(4) + cellCount(4).
    // Per cell: int16 cellX + int16 cellY + float[1089] heights + uint8 hasColors
    //           + [if hasColors] uint8[3267] colors.
    // outTiles is populated with one LandRecord per cell; existing entries are cleared.
    static bool WorldspaceGetTerrainBulk(
        const char* wsFormKey,
        std::map<std::pair<int,int>, LandRecord>& outTiles,
        char* errOut, int errLen);
};
