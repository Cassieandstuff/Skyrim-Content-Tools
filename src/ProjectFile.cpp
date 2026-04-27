#include "ProjectFile.h"
#include "AppState.h"
#include "Sequence.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <algorithm>

using json = nlohmann::json;

// ── NpcRecord helpers ─────────────────────────────────────────────────────────

static json SerializeNpcRecord(const NpcRecord& r)
{
    return {
        {"formId",            r.formId},
        {"formKey",           r.formKey},
        {"editorId",          r.editorId},
        {"name",              r.name},
        {"raceEditorId",      r.raceEditorId},
        {"isFemale",          r.isFemale},
        {"skeletonModelPath", r.skeletonModelPath},
        {"expressionTriPath", r.expressionTriPath},
        {"facegenNifPath",    r.facegenNifPath},
        {"pluginSource",      r.pluginSource},
    };
}

static NpcRecord DeserializeNpcRecord(const json& j)
{
    NpcRecord r;
    r.formId            = j.value("formId",            0u);
    r.formKey           = j.value("formKey",           std::string{});
    r.editorId          = j.value("editorId",          std::string{});
    r.name              = j.value("name",              std::string{});
    r.raceEditorId      = j.value("raceEditorId",      std::string{});
    r.isFemale          = j.value("isFemale",          false);
    r.skeletonModelPath = j.value("skeletonModelPath", std::string{});
    r.expressionTriPath = j.value("expressionTriPath", std::string{});
    r.facegenNifPath    = j.value("facegenNifPath",    std::string{});
    r.pluginSource      = j.value("pluginSource",      std::string{});
    return r;
}

// ── TrackType helpers ─────────────────────────────────────────────────────────

static const char* TrackTypeName(TrackType t)
{
    switch (t) {
    case TrackType::AnimClip: return "AnimClip";
    case TrackType::FaceData: return "FaceData";
    case TrackType::LookAt:   return "LookAt";
    case TrackType::Camera:   return "Camera";
    case TrackType::Audio:    return "Audio";
    default:                  return "AnimClip";
    }
}

static TrackType TrackTypeFromName(const std::string& s)
{
    if (s == "FaceData") return TrackType::FaceData;
    if (s == "LookAt")   return TrackType::LookAt;
    if (s == "Camera")   return TrackType::Camera;
    if (s == "Audio")    return TrackType::Audio;
    return TrackType::AnimClip;
}

// ── SequenceItem helpers ──────────────────────────────────────────────────────

static json SerializeItem(const SequenceItem& item)
{
    return {
        {"assetIndex", item.assetIndex},
        {"seqStart",   item.seqStart},
        {"trimIn",     item.trimIn},
        {"trimOut",    item.trimOut},
        {"blendIn",    item.blendIn},
        {"blendOut",   item.blendOut},
    };
}

static SequenceItem DeserializeItem(const json& j, const std::vector<int>& clipRemap)
{
    SequenceItem item;
    const int raw   = j.value("assetIndex", -1);
    item.assetIndex = (raw >= 0 && raw < (int)clipRemap.size()) ? clipRemap[raw] : -1;
    item.seqStart   = j.value("seqStart",  0.f);
    item.trimIn     = j.value("trimIn",    0.f);
    item.trimOut    = j.value("trimOut",   0.f);
    item.blendIn    = j.value("blendIn",   0.f);
    item.blendOut   = j.value("blendOut",  0.f);
    return item;
}

// ── Save ──────────────────────────────────────────────────────────────────────

bool ProjectFile::Save(const std::string& path, const AppState& state,
                       char* errOut, int errLen)
{
    json root;
    root["sctVersion"] = 1;
    root["name"]       = state.projectName;
    root["dataFolder"] = state.dataFolder;

    // Clips — store source path only
    json jClips = json::array();
    for (const auto& c : state.clips)
        jClips.push_back({{"path", c.sourcePath}});
    root["clips"] = std::move(jClips);

    // Cast entries
    json jCast = json::array();
    for (const auto& ce : state.cast) {
        json entry;
        entry["name"]         = ce.name;
        entry["editorId"]     = ce.editorId;
        entry["skeletonType"] = ce.skeletonType;
        entry["skeleton"]     = {{"path",     ce.skeletonPath},
                                  {"internal", ce.skeletonInternal}};
        if (ce.npcRecord)
            entry["npcRecord"] = SerializeNpcRecord(*ce.npcRecord);
        jCast.push_back(std::move(entry));
    }
    root["cast"] = std::move(jCast);

    // Actors
    json jActors = json::array();
    for (const auto& a : state.actors)
        jActors.push_back({{"castIndex", a.castIndex}});
    root["actors"] = std::move(jActors);

    // Sequence
    json jSeq;
    jSeq["name"] = state.sequence.name;

    json jActorTracks = json::array();
    for (const auto& grp : state.sequence.actorTracks) {
        json jGrp;
        jGrp["actorIndex"] = grp.actorIndex;
        jGrp["collapsed"]  = grp.collapsed;
        json jLanes = json::array();
        for (const auto& lane : grp.lanes) {
            json jLane;
            jLane["type"] = TrackTypeName(lane.type);
            json jItems = json::array();
            for (const auto& item : lane.items)
                jItems.push_back(SerializeItem(item));
            jLane["items"] = std::move(jItems);
            jLanes.push_back(std::move(jLane));
        }
        jGrp["lanes"] = std::move(jLanes);
        jActorTracks.push_back(std::move(jGrp));
    }
    jSeq["actorTracks"] = std::move(jActorTracks);

    json jSceneTracks = json::array();
    for (const auto& lane : state.sequence.sceneTracks) {
        json jLane;
        jLane["type"] = TrackTypeName(lane.type);
        json jItems = json::array();
        for (const auto& item : lane.items)
            jItems.push_back(SerializeItem(item));
        jLane["items"] = std::move(jItems);
        jSceneTracks.push_back(std::move(jLane));
    }
    jSeq["sceneTracks"] = std::move(jSceneTracks);
    root["sequence"] = std::move(jSeq);

    std::ofstream f(path);
    if (!f) {
        std::snprintf(errOut, errLen, "Cannot write: %s", path.c_str());
        return false;
    }
    f << root.dump(2);
    return f.good();
}

// ── Load ──────────────────────────────────────────────────────────────────────

bool ProjectFile::Load(const std::string& path, AppState& state,
                       char* errOut, int errLen)
{
    std::ifstream f(path);
    if (!f) {
        std::snprintf(errOut, errLen, "Cannot open: %s", path.c_str());
        return false;
    }

    json root;
    try { root = json::parse(f); }
    catch (const std::exception& e) {
        std::snprintf(errOut, errLen, "JSON parse error: %s", e.what());
        return false;
    }

    if (root.value("sctVersion", 0) != 1) {
        std::snprintf(errOut, errLen, "Unsupported sctVersion (expected 1)");
        return false;
    }

    // Clear scene; preserve dataFolder, plugins, backend
    state.NewProject();

    // Data folder — re-scan if changed
    const std::string df = root.value("dataFolder", std::string{});
    if (!df.empty() && df != state.dataFolder) {
        state.dataFolder = df;
        state.ScanDataFolder();
    }

    state.projectName  = root.value("name", std::string("Untitled"));
    state.projectPath  = path;
    state.projectDirty = false;

    // ── Clips — reload from source paths; build old-index → new-index remap ───
    std::vector<int> clipRemap;
    if (root.contains("clips")) {
        for (const auto& jc : root["clips"]) {
            const std::string cpath = jc.value("path", std::string{});
            char clipErr[256] = {};
            const int idx = cpath.empty() ? -1
                          : state.LoadClipFromPath(cpath.c_str(), clipErr, sizeof(clipErr));
            if (idx < 0 && !cpath.empty())
                fprintf(stderr, "[SCT] Project load: clip missing '%s': %s\n",
                        cpath.c_str(), clipErr);
            clipRemap.push_back(idx);
        }
    }

    // ── Cast entries ──────────────────────────────────────────────────────────
    if (root.contains("cast")) {
        for (const auto& jce : root["cast"]) {
            CastEntry ce;
            ce.name         = jce.value("name",         std::string{});
            ce.editorId     = jce.value("editorId",     std::string{});
            ce.skeletonType = jce.value("skeletonType", std::string{});
            if (jce.contains("npcRecord"))
                ce.npcRecord = DeserializeNpcRecord(jce["npcRecord"]);

            // Skeleton ref
            std::string skelPath, skelInternal;
            if (jce.contains("skeleton")) {
                skelPath     = jce["skeleton"].value("path",     std::string{});
                skelInternal = jce["skeleton"].value("internal", std::string{});
            }

            const int castIdx = static_cast<int>(state.cast.size());
            state.cast.push_back(std::move(ce));

            if (!skelPath.empty()) {
                for (const auto& ds : state.discoveredSkeletons) {
                    if (ds.path == skelPath && ds.bsaInternal == skelInternal) {
                        char skelErr[256] = {};
                        if (!state.AssignSkeletonToCast(castIdx, ds, skelErr, sizeof(skelErr)))
                            fprintf(stderr, "[SCT] Project load: skeleton assign failed: %s\n",
                                    skelErr);
                        break;
                    }
                }
            }
        }
    }

    // ── Actors ────────────────────────────────────────────────────────────────
    if (root.contains("actors")) {
        for (const auto& ja : root["actors"]) {
            Actor actor;
            actor.castIndex    = ja.value("castIndex", -1);
            const int actorIdx = static_cast<int>(state.actors.size());
            state.actors.push_back(actor);
            state.sequence.EnsureActorGroup(actorIdx);
        }
    }

    // ── Sequence ──────────────────────────────────────────────────────────────
    if (root.contains("sequence")) {
        const auto& jseq   = root["sequence"];
        state.sequence.name = jseq.value("name", std::string{});

        if (jseq.contains("actorTracks")) {
            for (const auto& jg : jseq["actorTracks"]) {
                const int actorIdx = jg.value("actorIndex", -1);
                auto it = std::find_if(state.sequence.actorTracks.begin(),
                                       state.sequence.actorTracks.end(),
                    [actorIdx](const ActorTrackGroup& g){ return g.actorIndex == actorIdx; });
                if (it == state.sequence.actorTracks.end()) continue;

                it->collapsed = jg.value("collapsed", false);
                if (!jg.contains("lanes")) continue;

                for (const auto& jl : jg["lanes"]) {
                    const TrackType type = TrackTypeFromName(jl.value("type", std::string{"AnimClip"}));
                    auto lit = std::find_if(it->lanes.begin(), it->lanes.end(),
                        [type](const TrackLane& l){ return l.type == type; });
                    if (lit == it->lanes.end()) continue;
                    if (!jl.contains("items")) continue;
                    for (const auto& ji : jl["items"])
                        lit->items.push_back(DeserializeItem(ji, clipRemap));
                }
            }
        }

        if (jseq.contains("sceneTracks")) {
            for (const auto& jl : jseq["sceneTracks"]) {
                const TrackType type = TrackTypeFromName(jl.value("type", std::string{"AnimClip"}));
                state.sequence.EnsureSceneTrack(type);
                auto lit = std::find_if(state.sequence.sceneTracks.begin(),
                                        state.sequence.sceneTracks.end(),
                    [type](const TrackLane& l){ return l.type == type; });
                if (lit == state.sequence.sceneTracks.end()) continue;
                if (!jl.contains("items")) continue;
                for (const auto& ji : jl["items"])
                    lit->items.push_back(DeserializeItem(ji, clipRemap));
            }
        }
    }

    return true;
}
