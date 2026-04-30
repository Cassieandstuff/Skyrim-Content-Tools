#pragma once
#include "plugin/IPluginBackend.h"

// Mutagen-backed plugin I/O for Skyrim SE.
// Delegates to DotNetHost which calls SctBridge.PluginBridge via hostfxr.
struct MutagenBackend : IPluginBackend {
    bool PluginLoad(const std::string& pluginPath,
                    const std::string& dataFolder,
                    char* errOut, int errLen) override;
    void PluginUnload() override;

    bool NpcSearch(const std::string& query, int maxResults,
                   std::vector<NpcRecord>& outRecords,
                   char* errOut, int errLen) override;

    bool NpcGet(uint32_t formId,
                NpcRecord& outRecord,
                char* errOut, int errLen) override;

    bool ProjectNew(const std::string& modName,
                    char* errOut, int errLen) override;
    bool ProjectLoad(const std::string& path,
                     char* errOut, int errLen) override;
    bool ProjectSave(const std::string& path,
                     char* errOut, int errLen) override;

    bool NpcCreate(const NpcCreateParams& params,
                   NpcRecord& outRecord,
                   char* errOut, int errLen) override;
};
