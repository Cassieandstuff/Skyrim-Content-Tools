#include "AppState.h"
#include "BsaReader.h"
#include "HavokSkeleton.h"
#include "HavokAnimation.h"
#include "DotNetHost.h"
#include "ui/TrackRegistry.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
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

void AppState::Tick(float dt)
{
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
    cast.clear();
    actors.clear();
    skeletons.clear();
    sequence     = Sequence{};
    time         = 0.f;
    playing      = false;
    selectedClip = -1;
    selectedCast = -1;
    importErr[0] = '\0';
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

    // Create a matching cast entry.
    CastEntry entry;
    entry.name          = std::filesystem::path(path).stem().string();
    entry.editorId      = "";
    entry.skeletonType  = ExtractCreatureType(path);
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
}

// ── AddActorFromRecord ────────────────────────────────────────────────────────

int AppState::AddActorFromRecord(const NpcRecord& rec)
{
    CastEntry entry;
    entry.name          = rec.name.empty() ? rec.editorId : rec.name;
    entry.editorId      = rec.editorId;
    entry.skeletonType  = ExtractCreatureType(rec.skeletonModelPath.c_str());
    entry.skeletonIndex = -1;
    entry.npcRecord     = rec;
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

        FILE* tf = fopen(tmpPath.c_str(), "wb");
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
    cast[castIdx].skeletonIndex    = skelIdx;
    cast[castIdx].skeletonType     = ds.creatureType;
    cast[castIdx].skeletonPath     = ds.path;
    cast[castIdx].skeletonInternal = ds.bsaInternal;
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
    }
}

void AppState::SaveSettings() const
{
    std::ofstream f("sct_settings.ini");
    if (!f) return;
    f << "dataFolder=" << dataFolder << "\n";
}
