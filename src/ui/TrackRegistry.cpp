#include "TrackRegistry.h"
#include "app/AppState.h"
#include "anim/AnimClip.h"
#include "anim/FaceClip.h"
#include "core/math/Interpolation.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

// ── Singleton ─────────────────────────────────────────────────────────────────

TrackRegistry& TrackRegistry::Get()
{
    static TrackRegistry inst;
    return inst;
}

void TrackRegistry::Register(TrackTypeDef def)
{
    defs_.push_back(std::move(def));
}

const TrackTypeDef* TrackRegistry::Find(TrackType id) const
{
    for (auto& d : defs_)
        if (d.id == id) return &d;
    return nullptr;
}

// ── Built-in track registrations ─────────────────────────────────────────────

void RegisterAllTrackTypes()
{
    auto& reg = TrackRegistry::Get();

    // ── AnimClip — body animation ─────────────────────────────────────────────
    {
        TrackTypeDef def;
        def.id           = TrackType::AnimClip;
        def.label        = "Anim Clips";
        def.color        = ImVec4(0.22f, 0.40f, 0.65f, 1.0f);
        def.isSceneLevel = false;
        def.isCompatible = nullptr;   // valid for every actor

        def.evaluate = [](const TrackLane& lane, float t,
                          AppState& state, ActorEval& eval)
        {
            // Collect active items and their blend weights.
            struct Active { const SequenceItem* item; float weight; float localT; };
            std::vector<Active> active;
            active.reserve(4);

            for (const auto& item : lane.items) {
                if (!item.ActiveAt(t)) continue;
                if (item.assetIndex < 0 || item.assetIndex >= (int)state.clips.size())
                    continue;

                const float w = Interp::BlendWeight(t, item.seqStart, item.SeqEnd(),
                                                    item.blendIn, item.blendOut);

                active.push_back({ &item, w, t - item.seqStart + item.trimIn });
            }

            if (active.empty()) return;

            if (active.size() == 1) {
                // Hot path: single active clip, no blend needed.
                state.clips[active[0].item->assetIndex].Evaluate(active[0].localT, eval.pose);
                return;
            }

            // Multiple overlapping items: blend in order of appearance.
            // Primary = first (oldest) clip; blend subsequent items onto it by weight.
            Pose primary = eval.pose;
            state.clips[active[0].item->assetIndex].Evaluate(active[0].localT, primary);
            eval.pose = primary;

            for (int i = 1; i < (int)active.size(); i++) {
                const float w = active[i].weight;
                if (w < 1e-4f) continue;

                Pose other = eval.pose;  // start from reference
                state.clips[active[i].item->assetIndex].Evaluate(active[i].localT, other);

                // Per-channel lerp/slerp blend.
                const int n = std::min((int)eval.pose.channels.size(),
                                       (int)other.channels.size());
                for (int ci = 0; ci < n; ci++) {
                    eval.pose.channels[ci].localT =
                        glm::mix(eval.pose.channels[ci].localT, other.channels[ci].localT, w);
                    eval.pose.channels[ci].localR =
                        glm::slerp(eval.pose.channels[ci].localR, other.channels[ci].localR, w);
                }
            }
        };

        reg.Register(std::move(def));
    }

    // ── FaceData — facial animation / morphs ─────────────────────────────────
    {
        TrackTypeDef def;
        def.id           = TrackType::FaceData;
        def.label        = "Face Data";
        def.color        = ImVec4(0.55f, 0.30f, 0.65f, 1.0f);
        def.isSceneLevel = false;
        def.isCompatible = nullptr;

        def.evaluate = [](const TrackLane& lane, float t,
                          AppState& state, ActorEval& eval)
        {
            for (const auto& item : lane.items) {
                if (!item.ActiveAt(t)) continue;
                if (item.assetIndex < 0 || item.assetIndex >= (int)state.faceClips.size())
                    continue;
                const float localT = t - item.seqStart + item.trimIn;
                state.faceClips[item.assetIndex].EvaluateAll(localT, eval.morphWeights);
            }
        };

        reg.Register(std::move(def));
    }

    // ── LookAt — look-at target constraint (scaffolded) ────────────────────────
    {
        TrackTypeDef def;
        def.id           = TrackType::LookAt;
        def.label        = "Look At";
        def.color        = ImVec4(0.65f, 0.55f, 0.20f, 1.0f);
        def.isSceneLevel = false;
        def.evaluate     = nullptr;
        reg.Register(std::move(def));
    }

    // ── Camera — scene-level camera track (scaffolded) ──────────────────────────
    {
        TrackTypeDef def;
        def.id           = TrackType::Camera;
        def.label        = "Camera";
        def.color        = ImVec4(0.20f, 0.60f, 0.45f, 1.0f);
        def.isSceneLevel = true;
        def.evaluate     = nullptr;
        reg.Register(std::move(def));
    }

    // ── Audio — scene-level sound track (scaffolded) ────────────────────────────
    {
        TrackTypeDef def;
        def.id           = TrackType::Audio;
        def.label        = "Audio";
        def.color        = ImVec4(0.65f, 0.40f, 0.20f, 1.0f);
        def.isSceneLevel = true;
        def.evaluate     = nullptr;
        reg.Register(std::move(def));
    }
}
