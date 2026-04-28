#include "ProjectFile.h"
#include "AppState.h"
#include "Sequence.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <algorithm>

using json = nlohmann::json;

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

    // Cast entries (ActorDocument)
    json jCast = json::array();
    for (const auto& doc : state.cast) {
        json entry;
        // Identity
        entry["name"]        = doc.name;
        entry["editorId"]    = doc.editorId;
        entry["formId"]      = doc.formId;
        entry["formKey"]     = doc.formKey;
        entry["raceEditorId"]= doc.raceEditorId;
        entry["pluginSource"]= doc.pluginSource;
        entry["isFemale"]    = doc.isFemale;
        // Asset cache
        entry["creatureType"]= doc.creatureType;
        entry["skeleton"]    = {{"hkxPath",     doc.skeletonHkxPath},
                                 {"hkxInternal", doc.skeletonHkxInternal}};
        entry["bodyNifPath"]  = doc.bodyNifPath;
        entry["handsNifPath"] = doc.handsNifPath;
        entry["feetNifPath"]  = doc.feetNifPath;
        entry["headNifPath"]  = doc.headNifPath;
        entry["headTriPath"]  = doc.headTriPath;
        if (!doc.headPartNifs.empty()) {
            json jHPNifs = json::array();
            for (const auto& hp : doc.headPartNifs) jHPNifs.push_back(hp);
            entry["headPartNifs"] = std::move(jHPNifs);
        }
        if (!doc.expressionTriPaths.empty()) {
            json jTriPaths = json::array();
            for (const auto& tp : doc.expressionTriPaths) jTriPaths.push_back(tp);
            entry["expressionTriPaths"] = std::move(jTriPaths);
        }
        if (!doc.extendedTriPaths.empty()) {
            json jExt = json::array();
            for (const auto& ep : doc.extendedTriPaths) jExt.push_back(ep);
            entry["extendedTriPaths"] = std::move(jExt);
        }
        // Authored overrides
        if (!doc.morphWeights.empty()) {
            json jMorphs = json::object();
            for (const auto& [k, v] : doc.morphWeights)
                jMorphs[k] = v;
            entry["morphWeights"] = std::move(jMorphs);
        }
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

    // ── Cast entries (ActorDocument) ──────────────────────────────────────────
    if (root.contains("cast")) {
        for (const auto& jce : root["cast"]) {
            ActorDocument doc;
            // Identity
            doc.name         = jce.value("name",         std::string{});
            doc.editorId     = jce.value("editorId",     std::string{});
            doc.formId       = jce.value("formId",       0u);
            doc.formKey      = jce.value("formKey",      std::string{});
            doc.raceEditorId = jce.value("raceEditorId", std::string{});
            doc.pluginSource = jce.value("pluginSource", std::string{});
            doc.isFemale     = jce.value("isFemale",     false);
            // Asset cache
            doc.creatureType = jce.value("creatureType", std::string{});
            doc.bodyNifPath  = jce.value("bodyNifPath",  std::string{});
            doc.handsNifPath = jce.value("handsNifPath", std::string{});
            doc.feetNifPath  = jce.value("feetNifPath",  std::string{});
            doc.headNifPath  = jce.value("headNifPath",  std::string{});
            doc.headTriPath  = jce.value("headTriPath",  std::string{});
            if (jce.contains("headPartNifs") && jce["headPartNifs"].is_array())
                for (const auto& hp : jce["headPartNifs"])
                    if (hp.is_string()) doc.headPartNifs.push_back(hp.get<std::string>());

            // Expression TRI paths — array form (new); fall back to single headTriPath (legacy).
            if (jce.contains("expressionTriPaths") && jce["expressionTriPaths"].is_array()) {
                for (const auto& tp : jce["expressionTriPaths"])
                    if (tp.is_string()) doc.expressionTriPaths.push_back(tp.get<std::string>());
            } else if (!doc.headTriPath.empty()) {
                doc.expressionTriPaths.push_back(doc.headTriPath);
            }
            if (jce.contains("extendedTriPaths") && jce["extendedTriPaths"].is_array()) {
                for (const auto& ep : jce["extendedTriPaths"])
                    if (ep.is_string()) doc.extendedTriPaths.push_back(ep.get<std::string>());
            }
            // Legacy key fallback
            if (doc.creatureType.empty())
                doc.creatureType = jce.value("skeletonType", std::string{});

            // Skeleton ref
            std::string skelPath, skelInternal;
            if (jce.contains("skeleton")) {
                const auto& js = jce["skeleton"];
                skelPath     = js.value("hkxPath",     std::string{});
                skelInternal = js.value("hkxInternal", std::string{});
                // Legacy key fallback
                if (skelPath.empty())     skelPath     = js.value("path",     std::string{});
                if (skelInternal.empty()) skelInternal = js.value("internal", std::string{});
            }

            // Authored overrides
            if (jce.contains("morphWeights")) {
                for (const auto& [k, v] : jce["morphWeights"].items())
                    doc.morphWeights[k] = v.get<float>();
            }

            // Legacy: old format embedded full NpcRecord; promote fields if missing
            if (jce.contains("npcRecord")) {
                const auto& jn = jce["npcRecord"];
                if (doc.formId == 0)          doc.formId       = jn.value("formId",            0u);
                if (doc.formKey.empty())      doc.formKey      = jn.value("formKey",            std::string{});
                if (doc.raceEditorId.empty()) doc.raceEditorId = jn.value("raceEditorId",       std::string{});
                if (doc.pluginSource.empty()) doc.pluginSource = jn.value("pluginSource",       std::string{});
                if (!doc.isFemale)            doc.isFemale     = jn.value("isFemale",           false);
                if (doc.headNifPath.empty())  doc.headNifPath  = jn.value("facegenNifPath",     std::string{});
                if (doc.headTriPath.empty())  doc.headTriPath  = jn.value("expressionTriPath",  std::string{});
            }

            const int castIdx = static_cast<int>(state.cast.size());
            state.cast.push_back(std::move(doc));

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
