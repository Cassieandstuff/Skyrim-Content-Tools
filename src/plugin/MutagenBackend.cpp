#include "plugin/MutagenBackend.h"
#include "plugin/DotNetHost.h"
#include <nlohmann/json.hpp>
#include <cstdio>

using json = nlohmann::json;

// Deserialize one NpcRecord from a parsed JSON object.
// Uses .value() with defaults so missing/null fields don't throw.
static NpcRecord ParseNpcRecord(const json& j)
{
    NpcRecord r;
    r.formId            = j.value("formId",            0u);
    r.formKey           = j.value("formKey",           std::string{});
    r.editorId          = j.value("editorId",          std::string{});
    r.name              = j.value("name",              std::string{});
    r.raceEditorId      = j.value("raceEditorId",      std::string{});
    r.isFemale          = j.value("isFemale",          false);
    r.skeletonModelPath = j.value("skeletonModelPath", std::string{});
    r.facegenNifPath    = j.value("facegenNifPath",    std::string{});

    // expressionTriPaths: new array form.  Fall back to legacy single-string
    // field if the bridge DLL has not yet been rebuilt.
    if (j.contains("expressionTriPaths") && j["expressionTriPaths"].is_array()) {
        for (const auto& tp : j["expressionTriPaths"])
            if (tp.is_string()) r.expressionTriPaths.push_back(tp.get<std::string>());
    } else {
        // Legacy: single expressionTriPath string from older bridge DLL.
        std::string single = j.value("expressionTriPath", std::string{});
        if (!single.empty()) r.expressionTriPaths.push_back(std::move(single));
    }
    r.pluginSource      = j.value("pluginSource",      std::string{});

    // Body parts: [{slot, nifPath}, ...]
    if (j.contains("bodyParts") && j["bodyParts"].is_array()) {
        for (const auto& bp : j["bodyParts"]) {
            NpcBodyPart part;
            part.slot    = bp.value("slot",    std::string{});
            part.nifPath = bp.value("nifPath", std::string{});
            if (!part.slot.empty() && !part.nifPath.empty())
                r.bodyParts.push_back(std::move(part));
        }
    }

    // Head part NIFs: ["Meshes\...", ...]
    if (j.contains("headPartNifs") && j["headPartNifs"].is_array()) {
        for (const auto& hp : j["headPartNifs"]) {
            if (hp.is_string()) {
                std::string p = hp.get<std::string>();
                if (!p.empty()) r.headPartNifs.push_back(std::move(p));
            }
        }
    }

    return r;
}

bool MutagenBackend::PluginLoad(const std::string& pluginPath,
                                 const std::string& dataFolder,
                                 char* errOut, int errLen)
{
    return DotNetHost::PluginLoad(pluginPath.c_str(), dataFolder.c_str(), errOut, errLen);
}

void MutagenBackend::PluginUnload()
{
    DotNetHost::PluginUnload();
}

bool MutagenBackend::NpcSearch(const std::string& query, int maxResults,
                                std::vector<NpcRecord>& outRecords,
                                char* errOut, int errLen)
{
    std::string jsonStr;
    if (!DotNetHost::NpcSearch(query.c_str(), maxResults, jsonStr, errOut, errLen))
        return false;
    try {
        auto arr = json::parse(jsonStr);
        outRecords.clear();
        for (const auto& item : arr)
            outRecords.push_back(ParseNpcRecord(item));
        return true;
    } catch (const std::exception& e) {
        std::snprintf(errOut, errLen, "JSON parse error: %s", e.what());
        return false;
    }
}

bool MutagenBackend::NpcGet(uint32_t formId,
                             NpcRecord& outRecord,
                             char* errOut, int errLen)
{
    std::string jsonStr;
    if (!DotNetHost::NpcGet(formId, jsonStr, errOut, errLen))
        return false;
    try {
        outRecord = ParseNpcRecord(json::parse(jsonStr));
        return true;
    } catch (const std::exception& e) {
        std::snprintf(errOut, errLen, "JSON parse error: %s", e.what());
        return false;
    }
}

bool MutagenBackend::ProjectNew(const std::string& modName,
                                 char* errOut, int errLen)
{
    return DotNetHost::ProjectNew(modName.c_str(), errOut, errLen);
}

bool MutagenBackend::ProjectLoad(const std::string& path, char* errOut, int errLen)
{
    return DotNetHost::ProjectLoad(path.c_str(), errOut, errLen);
}

bool MutagenBackend::ProjectSave(const std::string& path, char* errOut, int errLen)
{
    return DotNetHost::ProjectSave(path.c_str(), errOut, errLen);
}

bool MutagenBackend::NpcCreate(const NpcCreateParams& params,
                                NpcRecord& outRecord,
                                char* errOut, int errLen)
{
    // Serialize request — matches NpcCreateRequest on the C# side (camelCase)
    json req;
    req["editorId"]    = params.editorId;
    req["name"]        = params.name;
    req["raceFormKey"] = params.raceFormKey;
    req["isFemale"]    = params.isFemale;

    std::string outJson;
    if (!DotNetHost::NpcCreate(req.dump().c_str(), outJson, errOut, errLen))
        return false;
    try {
        outRecord = ParseNpcRecord(json::parse(outJson));
        return true;
    } catch (const std::exception& e) {
        std::snprintf(errOut, errLen, "JSON parse error: %s", e.what());
        return false;
    }
}
