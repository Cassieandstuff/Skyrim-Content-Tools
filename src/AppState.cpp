#include "AppState.h"
#include "BsaReader.h"
#include "FaceClip.h"
#include "HavokSkeleton.h"
#include "HavokAnimation.h"
#include "DotNetHost.h"
#include "MuFeeConfig.h"
#include <nlohmann/json.hpp>
#include "ui/TrackRegistry.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// ── ExtractCreatureType ────────────────────────────────────────────────────────

std::string ExtractCreatureType(const char* path)
{
    if (!path || !*path) return {};
    // Normalise to forward slashes and lowercase for searching.
    std::string p = path;
    for (char& c : p) if (c == '\\') c = '/';
    std::string pl = p;
    std::transform(pl.begin(), pl.end(), pl.begin(), ::tolower);
    // Find "actors/" segment.
    auto pos = pl.find("actors/");
    if (pos == std::string::npos) return {};
    pos += 7; // skip "actors/"
    auto end = p.find('/', pos);
    if (end == std::string::npos) return {};
    std::string t = p.substr(pos, end - pos);
    std::transform(t.begin(), t.end(), t.begin(), ::tolower);
    return t;
}

void AppState::PushToast(std::string msg, ToastLevel level, float duration)
{
    toasts.push_back({ std::move(msg), duration, level });
}

void AppState::Tick(float dt)
{
    // Tick toasts
    for (auto& t : toasts) t.ttl -= dt;
    toasts.erase(std::remove_if(toasts.begin(), toasts.end(),
                                [](const Toast& t){ return t.ttl <= 0.f; }),
                 toasts.end());

    if (!playing) return;

    // Prefer sequence duration; fall back to selected clip for bin-preview mode.
    float dur = sequence.Duration();
    if (dur <= 0.f && selectedClip >= 0 && selectedClip < (int)clips.size())
        dur = clips[selectedClip].duration;
    if (dur <= 0.f) return;

    time += dt;
    if (time >= dur) {
        if (loop) time = std::fmod(time, dur);
        else      { time = dur; playing = false; }
    }
}

// ── NewProject ────────────────────────────────────────────────────────────────

void AppState::NewProject()
{
    clips.clear();
    faceClips.clear();
    cast.clear();
    actors.clear();
    skeletons.clear();
    sequence     = Sequence{};
    loadedCell   = CellContext{};
    time         = 0.f;
    playing      = false;
    selectedClip = -1;
    selectedCast = -1;
    importErr[0] = '\0';
    toasts.clear();
    projectPath  = "";
    projectName  = "Untitled";
    projectDirty = false;
    // dataFolder, discoveredSkeletons, discoveredPlugins, pluginBackend — preserved
}

// ── LoadClipFromPath ──────────────────────────────────────────────────────────

int AppState::LoadClipFromPath(const char* path, char* errOut, int errLen)
{
    AnimClip clip;
    bool ok = false;
    std::string stem = std::filesystem::path(path).stem().string();

    const char* ext   = std::strrchr(path, '.');
    const bool  isHkx = ext && (_stricmp(ext, ".hkx") == 0);

    if (isHkx) {
        if (!DotNetHost::Ready()) {
            std::snprintf(errOut, errLen,
                "HKX import unavailable — .NET 10 runtime or SctBridge.dll not found");
            return -1;
        }
        char* xmlBuf = nullptr; int xmlLen = 0;
        if (DotNetHost::HkxToXml(path, &xmlBuf, &xmlLen, errOut, errLen)) {
            ok = LoadHavokAnimationXmlFromBuffer(xmlBuf, xmlLen, stem.c_str(),
                                                 clip, errOut, errLen);
            if (ok) {
                // Also extract face annotations from the same XML buffer — free
                // bonus since we already paid for the HkxToXml round-trip.
                FaceClip fc;
                char faceErr[256] = {};
                if (fc.ParseFromXml(xmlBuf, xmlLen, stem.c_str(), faceErr, sizeof(faceErr))) {
                    if (!fc.channels.empty()) {
                        fc.sourcePath = path;
                        faceClips.push_back(std::move(fc));
                        char msg[128];
                        std::snprintf(msg, sizeof(msg),
                            "Face clip extracted — %d morph channels",
                            (int)faceClips.back().channels.size());
                        PushToast(msg, ToastLevel::Info);
                    }
                    // If channels is empty it just means no MorphFace annotations
                    // in this HKX — silently fine.
                } else if (faceErr[0]) {
                    PushToast(std::string("Face parse: ") + faceErr, ToastLevel::Warning);
                }
            }
            DotNetHost::FreeBuffer(xmlBuf);
        }
    } else {
        ok = LoadHavokAnimationXml(path, clip, errOut, errLen);
    }

    if (!ok) return -1;

    clip.sourcePath   = path;
    clip.skeletonType = ExtractCreatureType(path);
    const int idx     = static_cast<int>(clips.size());
    clips.push_back(std::move(clip));
    return idx;
}

// ── LoadFaceClipFromPath ──────────────────────────────────────────────────────

int AppState::LoadFaceClipFromPath(const char* path, char* errOut, int errLen)
{
    FaceClip clip;
    std::string stem = std::filesystem::path(path).stem().string();

    const char* ext   = std::strrchr(path, '.');
    const bool  isHkx = ext && (_stricmp(ext, ".hkx") == 0);

    bool ok = false;
    if (isHkx) {
        if (!DotNetHost::Ready()) {
            std::snprintf(errOut, errLen,
                "HKX import unavailable — .NET 10 runtime or SctBridge.dll not found");
            return -1;
        }
        char* xmlBuf = nullptr; int xmlLen = 0;
        if (DotNetHost::HkxToXml(path, &xmlBuf, &xmlLen, errOut, errLen)) {
            ok = clip.ParseFromXml(xmlBuf, xmlLen, stem.c_str(), errOut, errLen);
            DotNetHost::FreeBuffer(xmlBuf);
        }
    } else {
        // Direct XML path.
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            std::snprintf(errOut, errLen, "Cannot open: %s", path);
            return -1;
        }
        std::string xml((std::istreambuf_iterator<char>(f)), {});
        ok = clip.ParseFromXml(xml.c_str(), (int)xml.size(), stem.c_str(),
                               errOut, errLen);
    }

    if (!ok) return -1;

    clip.sourcePath     = path;
    const int idx       = static_cast<int>(faceClips.size());
    faceClips.push_back(std::move(clip));
    return idx;
}

// ── LoadCell / UnloadCell ─────────────────────────────────────────────────────

bool AppState::LoadCell(const char* formKey, const char* cellName,
                        char* errOut, int errLen)
{
    if (!DotNetHost::PluginReady()) {
        std::snprintf(errOut, errLen, "plugin bridge not available — load a plugin first");
        return false;
    }

    std::string jsonStr;
    if (!DotNetHost::CellGetRefs(formKey, jsonStr, errOut, errLen))
        return false;

    try {
        using json = nlohmann::json;
        auto arr = json::parse(jsonStr);

        CellContext ctx;
        ctx.formKey = formKey;
        ctx.name    = (cellName && *cellName) ? cellName : formKey;
        ctx.loaded  = true;

        for (const auto& j : arr) {
            CellPlacedRef ref;
            ref.refFormKey  = j.value("refFormKey",  std::string{});
            ref.baseFormKey = j.value("baseFormKey", std::string{});
            ref.nifPath     = j.value("nifPath",     std::string{});
            ref.posX        = j.value("posX",  0.f);
            ref.posY        = j.value("posY",  0.f);
            ref.posZ        = j.value("posZ",  0.f);
            ref.rotX        = j.value("rotX",  0.f);
            ref.rotY        = j.value("rotY",  0.f);
            ref.rotZ        = j.value("rotZ",  0.f);
            ref.scale       = j.value("scale", 1.f);
            if (ref.nifPath.empty()) continue;
            ctx.refs.push_back(std::move(ref));
        }

        loadedCell = std::move(ctx);
        return true;
    }
    catch (const std::exception& e) {
        std::snprintf(errOut, errLen, "cell JSON parse error: %s", e.what());
        return false;
    }
}

void AppState::UnloadCell()
{
    loadedCell = CellContext{};
}

const Skeleton* AppState::SkeletonForCast(int castIndex) const
{
    if (castIndex < 0 || castIndex >= (int)cast.size()) return nullptr;
    int si = cast[castIndex].skeletonIndex;
    if (si < 0 || si >= (int)skeletons.size()) return nullptr;
    return &skeletons[si];
}

const Skeleton* AppState::SkeletonForActor(int actorIndex) const
{
    if (actorIndex < 0 || actorIndex >= (int)actors.size()) return nullptr;
    return SkeletonForCast(actors[actorIndex].castIndex);
}

int AppState::LoadSkeletonAndAddActor(const char* path, char* errOut, int errLen)
{
    Skeleton sk;
    bool ok = false;

    const char* ext = std::strrchr(path, '.');
    const bool  isHkx = ext && (_stricmp(ext, ".hkx") == 0);

    if (isHkx) {
        if (!DotNetHost::Ready()) {
            std::snprintf(errOut, errLen,
                "HKX import unavailable — .NET 10 runtime or SctBridge.dll not found");
            return -1;
        }
        char* xmlBuf = nullptr; int xmlLen = 0;
        if (DotNetHost::HkxToXml(path, &xmlBuf, &xmlLen, errOut, errLen)) {
            ok = LoadHavokSkeletonXmlFromBuffer(xmlBuf, xmlLen, sk, errOut, errLen);
            DotNetHost::FreeBuffer(xmlBuf);
        }
    } else {
        ok = LoadHavokSkeletonXml(path, sk, errOut, errLen);
    }

    if (!ok) return -1;

    // Add skeleton (no deduplication yet — extend later if needed).
    const int skelIdx = static_cast<int>(skeletons.size());
    skeletons.push_back(std::move(sk));

    // Create a matching actor document.
    ActorDocument entry;
    entry.name          = std::filesystem::path(path).stem().string();
    entry.creatureType  = ExtractCreatureType(path);
    entry.skeletonIndex = skelIdx;
    const int castIdx   = static_cast<int>(cast.size());
    cast.push_back(std::move(entry));

    // Create a scene actor instance.
    Actor actor;
    actor.castIndex   = castIdx;
    const int actorIdx = static_cast<int>(actors.size());
    actors.push_back(actor);

    // Ensure the sequence has a track group for this actor (populated with
    // one lane per registered per-actor track type).
    sequence.EnsureActorGroup(actorIdx);

    return actorIdx;
}

// ── ScanDataFolder ────────────────────────────────────────────────────────────

void AppState::ScanDataFolder()
{
    discoveredSkeletons.clear();
    discoveredPlugins.clear();
    if (dataFolder.empty()) return;

    namespace fs = std::filesystem;
    std::error_code ec;

    // ── Plugin files at data folder root ──────────────────────────────────────
    for (const auto& entry : fs::directory_iterator(fs::path(dataFolder), ec)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".esp" && ext != ".esm" && ext != ".esl") continue;
        discoveredPlugins.push_back(entry.path().filename().string());
    }
    std::sort(discoveredPlugins.begin(), discoveredPlugins.end(),
              [](const std::string& a, const std::string& b) {
                  std::string al = a, bl = b;
                  std::transform(al.begin(), al.end(), al.begin(), ::tolower);
                  std::transform(bl.begin(), bl.end(), bl.begin(), ::tolower);
                  return al < bl;
              });

    // ── Loose skeleton HKX files under meshes/actors/ ────────────────────────
    fs::path actorsDir = fs::path(dataFolder) / "meshes" / "actors";
    if (fs::exists(actorsDir, ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(actorsDir, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".hkx") continue;
            std::string stem = entry.path().stem().string();
            std::string stemLow = stem;
            std::transform(stemLow.begin(), stemLow.end(), stemLow.begin(), ::tolower);
            if (stemLow.find("skeleton") == std::string::npos) continue;

            std::string ct = ExtractCreatureType(entry.path().string().c_str());
            if (ct.empty()) continue;

            discoveredSkeletons.push_back({ entry.path().string(), {}, ct, stem });
        }
    }

    // ── Skeletons packed inside BSA archives ──────────────────────────────────
    for (const auto& entry : fs::directory_iterator(fs::path(dataFolder), ec)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".bsa") continue;

        BsaReader bsa;
        char bsaErr[256] = {};
        if (!bsa.Open(entry.path().string(), bsaErr, sizeof(bsaErr))) {
            fprintf(stderr, "[SCT] BSA skip: %s — %s\n",
                    entry.path().filename().string().c_str(), bsaErr);
            continue;
        }

        int skelCount = 0;
        for (const auto* fi : bsa.Find("skeleton")) {
            // Must be under meshes\actors\ with .hkx extension
            const std::string& ip = fi->internalPath;
            if (ip.find("meshes\\actors\\") == std::string::npos) continue;
            if (ip.size() < 4 || ip.substr(ip.size() - 4) != ".hkx") continue;

            std::string ct = ExtractCreatureType(ip.c_str());
            if (ct.empty()) continue;

            // Stem = filename without extension
            const size_t slash = ip.rfind('\\');
            const size_t dot   = ip.rfind('.');
            std::string stem = ip.substr(
                slash != std::string::npos ? slash + 1 : 0,
                dot   != std::string::npos ? dot - (slash != std::string::npos ? slash + 1 : 0)
                                           : std::string::npos);

            discoveredSkeletons.push_back({ entry.path().string(), ip, ct, stem });
            ++skelCount;
        }
        if (skelCount > 0)
            fprintf(stderr, "[SCT] BSA %s: %d skeleton(s) indexed\n",
                    entry.path().filename().string().c_str(), skelCount);
    }

    std::sort(discoveredSkeletons.begin(), discoveredSkeletons.end(),
              [](const DiscoveredSkeleton& a, const DiscoveredSkeleton& b) {
                  return a.creatureType < b.creatureType ||
                         (a.creatureType == b.creatureType && a.displayName < b.displayName);
              });

    // ── BSA search list ───────────────────────────────────────────────────────
    // Priority: plugin-associated BSAs (highest) → ini-listed base-game BSAs.
    bsaSearchList.clear();
    std::vector<std::string> seenLow;   // lowercase deduplification set

    auto addBsa = [&](const std::string& bsaPath) {
        std::string lp = bsaPath;
        std::transform(lp.begin(), lp.end(), lp.begin(), ::tolower);
        if (std::find(seenLow.begin(), seenLow.end(), lp) != seenLow.end()) return;
        seenLow.push_back(lp);
        bsaSearchList.push_back(bsaPath);
    };

    // Step 1: Plugin-associated BSAs — e.g. "Dawnguard.esm" → "Dawnguard.bsa",
    // "Dawnguard - Textures.bsa", …
    for (const auto& plugin : discoveredPlugins) {
        std::string stemLow = fs::path(plugin).stem().string();
        std::transform(stemLow.begin(), stemLow.end(), stemLow.begin(), ::tolower);

        for (const auto& entry : fs::directory_iterator(fs::path(dataFolder), ec)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".bsa") continue;

            std::string bsaStemLow = entry.path().stem().string();
            std::transform(bsaStemLow.begin(), bsaStemLow.end(), bsaStemLow.begin(), ::tolower);

            // Match: exact stem, or stem followed by ' ', '-', or '_' separator.
            bool match = (bsaStemLow == stemLow);
            if (!match && bsaStemLow.size() > stemLow.size() &&
                bsaStemLow.substr(0, stemLow.size()) == stemLow)
            {
                const char sep = bsaStemLow[stemLow.size()];
                match = (sep == ' ' || sep == '-' || sep == '_');
            }
            if (match) addBsa(entry.path().string());
        }
    }

    // Step 2: Ini-listed BSAs from Skyrim.ini [Archive] section.
    auto loadIniListed = [&](const std::string& iniPath) {
        std::ifstream ini(iniPath);
        if (!ini) return;
        bool inArchive = false;
        std::string line;
        while (std::getline(ini, line)) {
            // Trim CR/whitespace.
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                     line.back() == '\t')) line.pop_back();
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                line.erase(0, 1);
            if (line.empty()) continue;
            if (line[0] == '[') {
                inArchive = (line == "[Archive]");
                continue;
            }
            if (!inArchive) continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
                key.pop_back();
            std::string keyLow = key;
            std::transform(keyLow.begin(), keyLow.end(), keyLow.begin(), ::tolower);
            if (keyLow != "sresourcearchivelist" && keyLow != "sresourcearchivelist2")
                continue;
            std::string val = line.substr(eq + 1);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
                val.erase(0, 1);
            std::istringstream ss(val);
            std::string token;
            while (std::getline(ss, token, ',')) {
                while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
                    token.erase(0, 1);
                while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
                    token.pop_back();
                if (token.empty()) continue;
                std::string full = (fs::path(dataFolder) / token).string();
                if (fs::exists(full, ec)) addBsa(full);
            }
        }
    };

    // Look in standard SSE My Games directory and game install parent.
    {
        char* up = nullptr; _dupenv_s(&up, nullptr, "USERPROFILE");
        if (up) {
            loadIniListed((fs::path(up) / "Documents" / "My Games" /
                           "Skyrim Special Edition" / "Skyrim.ini").string());
            free(up);
        }
        loadIniListed((fs::path(dataFolder).parent_path() / "Skyrim.ini").string());
    }

    fprintf(stderr, "[SCT] ScanDataFolder: %d skeleton(s), %d BSA(s) in search list\n",
            (int)discoveredSkeletons.size(), (int)bsaSearchList.size());
    for (int i = 0; i < (int)bsaSearchList.size(); i++)
        fprintf(stderr, "[SCT]   [%d] %s\n", i,
                std::filesystem::path(bsaSearchList[i]).filename().string().c_str());

    // ── MFEE (MuFacialExpressionExtended) TRI map ────────────────────────────
    mfeeExtendedTris.clear();
    const std::string mfeeIni = (fs::path(dataFolder) /
        "SKSE" / "plugins" / "MuFacialExpressionExtended" /
        "performance capture.ini").string();
    if (fs::exists(mfeeIni, ec))
        mfeeExtendedTris = LoadMuFeeConfig(mfeeIni);
    else
        fprintf(stderr, "[SCT] MFEE ini not found — extended TRI discovery disabled\n");
}

// ── ResolveAsset ──────────────────────────────────────────────────────────────

bool AppState::ResolveAsset(const std::string& relPath, std::vector<uint8_t>& outBytes) const
{
    if (relPath.empty()) return false;

    // Normalise to forward-slash form for loose-file lookup.
    std::string rel = relPath;
    for (char& c : rel) if (c == '\\') c = '/';

    // 1. Loose file wins.
    if (!dataFolder.empty()) {
        std::string full = dataFolder + "/" + rel;
        std::ifstream f(full, std::ios::binary);
        if (f) {
            outBytes.assign(std::istreambuf_iterator<char>(f),
                            std::istreambuf_iterator<char>());
            return true;
        }
    }

    // 2. Walk BSA search list (highest priority first).
    // BSA internal paths: lowercase backslash-separated.
    std::string bsaKey = rel;
    for (char& c : bsaKey) if (c == '/') c = '\\';
    std::transform(bsaKey.begin(), bsaKey.end(), bsaKey.begin(), ::tolower);

    fprintf(stderr, "[ResolveAsset] key='%s'  bsaList=%d\n",
            bsaKey.c_str(), (int)bsaSearchList.size());

    int bsaChecked = 0;
    for (const auto& bsaPath : bsaSearchList) {
        BsaReader bsa;
        char err[256] = {};
        if (!bsa.Open(bsaPath, err, sizeof(err))) {
            fprintf(stderr, "[ResolveAsset]   OPEN FAIL '%s': %s\n",
                    std::filesystem::path(bsaPath).filename().string().c_str(), err);
            continue;
        }
        ++bsaChecked;
        const BsaFileInfo* fi = bsa.FindExact(bsaKey);
        if (!fi) continue;
        char extractErr[256] = {};
        if (bsa.Extract(*fi, outBytes, extractErr, sizeof(extractErr))) {
            fprintf(stderr, "[ResolveAsset]   found in '%s'\n",
                    std::filesystem::path(bsaPath).filename().string().c_str());
            return true;
        }
        fprintf(stderr, "[ResolveAsset]   extract FAIL in '%s': %s\n",
                std::filesystem::path(bsaPath).filename().string().c_str(), extractErr);
    }

    fprintf(stderr, "[ResolveAsset]   MISS after %d BSA(s)\n", bsaChecked);
    return false;
}

// ── AddActorFromRecord ────────────────────────────────────────────────────────

int AppState::AddActorFromRecord(const NpcRecord& rec)
{
    ActorDocument entry;
    entry.name          = rec.name.empty() ? rec.editorId : rec.name;
    entry.editorId      = rec.editorId;
    entry.formId        = rec.formId;
    entry.formKey       = rec.formKey;
    entry.raceEditorId  = rec.raceEditorId;
    entry.pluginSource  = rec.pluginSource;
    entry.isFemale      = rec.isFemale;
    entry.headNifPath          = rec.facegenNifPath;
    entry.expressionTriPaths   = rec.expressionTriPaths;
    entry.headTriPath          = rec.expressionTriPaths.empty() ? std::string{} : rec.expressionTriPaths[0];
    entry.creatureType         = ExtractCreatureType(rec.skeletonModelPath.c_str());

    // Body parts (body/hands/feet) from WornArmor ArmorAddon chain.
    for (const auto& bp : rec.bodyParts) {
        if      (bp.slot == "body")  entry.bodyNifPath  = bp.nifPath;
        else if (bp.slot == "hands") entry.handsNifPath = bp.nifPath;
        else if (bp.slot == "feet")  entry.feetNifPath  = bp.nifPath;
    }

    // Race/NPC head parts (hair, eyes, mouth, ears, …)
    entry.headPartNifs = rec.headPartNifs;

    // ── MFEE: map vanilla TRI paths → extended ARKit TRI paths ───────────────
    if (!mfeeExtendedTris.empty()) {
        for (const auto& triPath : entry.expressionTriPaths) {
            const std::string key = NormalizeMuFeePath(triPath);
            const auto it = mfeeExtendedTris.find(key);
            if (it != mfeeExtendedTris.end()) {
                // Store with "meshes\" prefix for ResolveAsset compatibility.
                entry.extendedTriPaths.push_back("meshes\\" + it->second);
            } else {
                fprintf(stderr, "[SCT] MFEE: no mapping for TRI '%s'\n", triPath.c_str());
            }
        }
        fprintf(stderr, "[SCT] MFEE: %d/%d TRI path(s) mapped for '%s'\n",
                (int)entry.extendedTriPaths.size(),
                (int)entry.expressionTriPaths.size(),
                rec.editorId.c_str());
    }

    const int castIdx   = static_cast<int>(cast.size());
    cast.push_back(std::move(entry));

    Actor actor;
    actor.castIndex    = castIdx;
    const int actorIdx = static_cast<int>(actors.size());
    actors.push_back(actor);

    sequence.EnsureActorGroup(actorIdx);

    // Auto-assign skeleton from the NPC's race skeleton model path.
    // skeletonModelPath is a NIF path relative to Data\Meshes\, e.g.
    // "Actors\Character\Character Assets\Skeleton.nif".
    // Convert to lowercase BSA internal HKX path and match against
    // discoveredSkeletons (populated by ScanDataFolder).
    const ActorDocument& stored = cast[castIdx];
    fprintf(stderr, "[SCT] AddActorFromRecord '%s': body='%s' hands='%s' feet='%s' head='%s' "
            "headParts=%d triPaths=%d extendedTris=%d\n",
            rec.editorId.c_str(),
            stored.bodyNifPath.c_str(), stored.handsNifPath.c_str(),
            stored.feetNifPath.c_str(), stored.headNifPath.c_str(),
            (int)stored.headPartNifs.size(),
            (int)stored.expressionTriPaths.size(),
            (int)stored.extendedTriPaths.size());
    fprintf(stderr, "[SCT] AddActorFromRecord '%s': skeletonModelPath='%s'  discoveredSkeletons=%d\n",
            rec.editorId.c_str(), rec.skeletonModelPath.c_str(), (int)discoveredSkeletons.size());

    if (!rec.skeletonModelPath.empty() && !discoveredSkeletons.empty()) {
        std::string hkxPath = rec.skeletonModelPath;
        for (char& c : hkxPath) {
            if (c == '/') c = '\\';
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const auto dot = hkxPath.rfind('.');
        if (dot != std::string::npos)
            hkxPath = hkxPath.substr(0, dot) + ".hkx";
        if (hkxPath.rfind("meshes\\", 0) != 0)
            hkxPath = "meshes\\" + hkxPath;

        fprintf(stderr, "[SCT] AddActorFromRecord: looking for hkxPath='%s'\n", hkxPath.c_str());

        bool found = false;
        for (const auto& ds : discoveredSkeletons) {
            bool match = false;
            if (ds.fromBsa()) {
                match = (ds.bsaInternal == hkxPath);
            } else {
                std::string pathLow = ds.path;
                for (char& c : pathLow) {
                    if (c == '/') c = '\\';
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                match = pathLow.size() >= hkxPath.size() &&
                        pathLow.compare(pathLow.size() - hkxPath.size(),
                                        hkxPath.size(), hkxPath) == 0;
            }
            if (match) {
                fprintf(stderr, "[SCT] AddActorFromRecord: matched '%s' (bsa=%d)\n",
                        ds.fromBsa() ? ds.bsaInternal.c_str() : ds.path.c_str(),
                        (int)ds.fromBsa());
                char skelErr[256] = {};
                if (!AssignSkeletonToCast(castIdx, ds, skelErr, sizeof(skelErr)))
                    fprintf(stderr, "[SCT] AssignSkeletonToCast failed: %s\n", skelErr);
                found = true;
                break;
            }
        }
        if (!found)
            fprintf(stderr, "[SCT] AddActorFromRecord: no skeleton match found for '%s'\n", hkxPath.c_str());
    } else if (rec.skeletonModelPath.empty()) {
        fprintf(stderr, "[SCT] AddActorFromRecord: skeletonModelPath is empty — no auto-assign\n");
    } else {
        fprintf(stderr, "[SCT] AddActorFromRecord: discoveredSkeletons is empty — run ScanDataFolder first\n");
    }

    projectDirty = true;
    return actorIdx;
}

// ── RelinkActorFromRecord ──────────────────────────────────────────────────────

void AppState::RelinkActorFromRecord(int castIdx, const NpcRecord& rec)
{
    if (castIdx < 0 || castIdx >= (int)cast.size()) return;
    ActorDocument& doc = cast[castIdx];

    // Identity
    doc.formId       = rec.formId;
    doc.formKey      = rec.formKey;
    doc.raceEditorId = rec.raceEditorId;
    doc.pluginSource = rec.pluginSource;
    doc.isFemale     = rec.isFemale;

    // Asset cache
    doc.headNifPath        = rec.facegenNifPath;
    doc.expressionTriPaths = rec.expressionTriPaths;
    doc.headTriPath        = rec.expressionTriPaths.empty()
                             ? std::string{} : rec.expressionTriPaths[0];

    // MFEE mapping
    doc.extendedTriPaths.clear();
    if (!mfeeExtendedTris.empty()) {
        for (const auto& tp : doc.expressionTriPaths) {
            const auto it = mfeeExtendedTris.find(NormalizeMuFeePath(tp));
            if (it != mfeeExtendedTris.end())
                doc.extendedTriPaths.push_back("meshes\\" + it->second);
        }
    }

    // Reset lazy TRI cache so it reloads on next Inspector draw
    doc.triDocs.clear();
    doc.triDocsLoaded = false;

    projectDirty = true;
}

// ── RemapMfeeForActor ──────────────────────────────────────────────────────────

void AppState::RemapMfeeForActor(int castIdx)
{
    if (castIdx < 0 || castIdx >= (int)cast.size()) return;
    ActorDocument& doc = cast[castIdx];
    if (doc.expressionTriPaths.empty()) return;

    doc.extendedTriPaths.clear();
    doc.triDocs.clear();
    doc.triDocsLoaded = false;

    if (!mfeeExtendedTris.empty()) {
        for (const auto& tp : doc.expressionTriPaths) {
            const auto it = mfeeExtendedTris.find(NormalizeMuFeePath(tp));
            if (it != mfeeExtendedTris.end())
                doc.extendedTriPaths.push_back("meshes\\" + it->second);
            else
                fprintf(stderr, "[SCT] RemapMfee: no mapping for '%s'\n", tp.c_str());
        }
        fprintf(stderr, "[SCT] RemapMfee castIdx=%d: %d/%d mapped\n",
                castIdx, (int)doc.extendedTriPaths.size(),
                (int)doc.expressionTriPaths.size());
    } else {
        fprintf(stderr, "[SCT] RemapMfee: mfeeExtendedTris is empty — run ScanDataFolder first\n");
    }
    projectDirty = true;
}

// ── AssignSkeletonToCast ──────────────────────────────────────────────────────

bool AppState::AssignSkeletonToCast(int castIdx, const DiscoveredSkeleton& ds,
                                    char* errOut, int errLen)
{
    if (castIdx < 0 || castIdx >= (int)cast.size()) return false;

    Skeleton sk;
    bool ok = false;

    if (ds.fromBsa()) {
        // ── Extract HKX bytes from BSA → temp file → HkxToXml ────────────────
        if (!DotNetHost::Ready()) {
            std::snprintf(errOut, errLen,
                "HKX import unavailable — .NET 10 runtime or SctBridge.dll not found");
            return false;
        }

        BsaReader bsa;
        if (!bsa.Open(ds.path, errOut, errLen)) return false;

        const BsaFileInfo* fi = bsa.FindExact(ds.bsaInternal);
        if (!fi) {
            std::snprintf(errOut, errLen, "File not found in BSA: %s", ds.bsaInternal.c_str());
            return false;
        }

        std::vector<uint8_t> hkxBuf;
        if (!bsa.Extract(*fi, hkxBuf, errOut, errLen)) return false;

        // Write to a temp file so HkxToXml (which takes a path) can read it.
        char tempDir[MAX_PATH] = {};
        GetTempPathA(sizeof(tempDir), tempDir);
        const std::string tmpPath = std::string(tempDir) + "sct_skel_tmp.hkx";

        FILE* tf = nullptr; fopen_s(&tf, tmpPath.c_str(), "wb");
        if (!tf) {
            std::snprintf(errOut, errLen, "Cannot write temp file: %s", tmpPath.c_str());
            return false;
        }
        fwrite(hkxBuf.data(), 1, hkxBuf.size(), tf);
        fclose(tf);

        char* xmlBuf = nullptr; int xmlLen = 0;
        if (DotNetHost::HkxToXml(tmpPath.c_str(), &xmlBuf, &xmlLen, errOut, errLen)) {
            ok = LoadHavokSkeletonXmlFromBuffer(xmlBuf, xmlLen, sk, errOut, errLen);
            DotNetHost::FreeBuffer(xmlBuf);
        }
        DeleteFileA(tmpPath.c_str());
    } else {
        // ── Loose file ────────────────────────────────────────────────────────
        const char* ext   = std::strrchr(ds.path.c_str(), '.');
        const bool  isHkx = ext && (_stricmp(ext, ".hkx") == 0);
        if (isHkx) {
            if (!DotNetHost::Ready()) {
                std::snprintf(errOut, errLen,
                    "HKX import unavailable — .NET 10 runtime or SctBridge.dll not found");
                return false;
            }
            char* xmlBuf = nullptr; int xmlLen = 0;
            if (DotNetHost::HkxToXml(ds.path.c_str(), &xmlBuf, &xmlLen, errOut, errLen)) {
                ok = LoadHavokSkeletonXmlFromBuffer(xmlBuf, xmlLen, sk, errOut, errLen);
                DotNetHost::FreeBuffer(xmlBuf);
            }
        } else {
            ok = LoadHavokSkeletonXml(ds.path.c_str(), sk, errOut, errLen);
        }
    }

    if (!ok) return false;

    const int skelIdx            = static_cast<int>(skeletons.size());
    skeletons.push_back(std::move(sk));
    cast[castIdx].skeletonIndex       = skelIdx;
    cast[castIdx].creatureType        = ds.creatureType;
    cast[castIdx].skeletonHkxPath     = ds.path;
    cast[castIdx].skeletonHkxInternal = ds.bsaInternal;
    return true;
}

// ── Settings persistence ──────────────────────────────────────────────────────

void AppState::LoadSettings()
{
    std::ifstream f("sct_settings.ini");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "dataFolder" && !val.empty()) {
            dataFolder = val;
            ScanDataFolder();
        }
        else if (key == "lightAzimuth")   { try { lightAzimuth   = std::stof(val); } catch(...){} }
        else if (key == "lightElevation") { try { lightElevation = std::stof(val); } catch(...){} }
        else if (key == "lightR")  { try { lightColor[0]   = std::stof(val); } catch(...){} }
        else if (key == "lightG")  { try { lightColor[1]   = std::stof(val); } catch(...){} }
        else if (key == "lightB")  { try { lightColor[2]   = std::stof(val); } catch(...){} }
        else if (key == "ambientR") { try { ambientColor[0] = std::stof(val); } catch(...){} }
        else if (key == "ambientG") { try { ambientColor[1] = std::stof(val); } catch(...){} }
        else if (key == "ambientB") { try { ambientColor[2] = std::stof(val); } catch(...){} }
    }
}

void AppState::SaveSettings() const
{
    std::ofstream f("sct_settings.ini");
    if (!f) return;
    f << "dataFolder="    << dataFolder    << "\n"
      << "lightAzimuth="  << lightAzimuth  << "\n"
      << "lightElevation="<< lightElevation<< "\n"
      << "lightR="  << lightColor[0]   << "\n"
      << "lightG="  << lightColor[1]   << "\n"
      << "lightB="  << lightColor[2]   << "\n"
      << "ambientR=" << ambientColor[0] << "\n"
      << "ambientG=" << ambientColor[1] << "\n"
      << "ambientB=" << ambientColor[2] << "\n";
}
