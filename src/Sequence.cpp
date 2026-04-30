#include "Sequence.h"
#include "AppState.h"
#include "ui/TrackRegistry.h"
#include <algorithm>
#include <cmath>

// ── Sequence ──────────────────────────────────────────────────────────────────

float Sequence::Duration() const
{
    float d = 0.f;
    for (const auto& group : actorTracks)
        for (const auto& lane : group.lanes)
            for (const auto& item : lane.items)
                d = std::max(d, item.SeqEnd());
    for (const auto& lane : sceneTracks)
        for (const auto& item : lane.items)
            d = std::max(d, item.SeqEnd());
    return d;
}

void Sequence::EnsureActorGroup(int actorIndex)
{
    for (const auto& g : actorTracks)
        if (g.actorIndex == actorIndex) return;

    ActorTrackGroup group;
    group.actorIndex = actorIndex;

    // Pre-populate one lane per registered per-actor track type, in registry order.
    for (const auto& def : TrackRegistry::Get().All()) {
        if (def.isSceneLevel) continue;
        TrackLane lane;
        lane.type = def.id;
        group.lanes.push_back(std::move(lane));
    }

    actorTracks.push_back(std::move(group));
}

void Sequence::EnsureSceneTrack(TrackType type)
{
    for (const auto& lane : sceneTracks)
        if (lane.type == type) return;
    TrackLane lane;
    lane.type = type;
    sceneTracks.push_back(std::move(lane));
}

int Sequence::EvaluateCameraTrack(float t) const
{
    for (const auto& lane : sceneTracks) {
        if (lane.type != TrackType::Camera) continue;
        int   result    = -1;
        float bestStart = -1.f;
        for (const auto& item : lane.items) {
            if (item.seqStart <= t && item.SeqEnd() > t && item.seqStart > bestStart) {
                bestStart = item.seqStart;
                result    = item.assetIndex;
            }
        }
        return result;
    }
    return -1;
}

void Sequence::Evaluate(float t, AppState& state, std::vector<ActorEval>& out) const
{
    out.resize(state.actors.size());

    for (const auto& group : actorTracks) {
        const int ai = group.actorIndex;
        if (ai < 0 || ai >= (int)state.actors.size()) continue;

        const Skeleton* skel = state.SkeletonForActor(ai);
        if (!skel) continue;

        // Start each actor from their reference pose.
        out[ai].pose = skel->MakeReferencePose();

        for (const auto& lane : group.lanes) {
            if (group.collapsed) continue;
            const TrackTypeDef* def = TrackRegistry::Get().Find(lane.type);
            if (def && def->evaluate)
                def->evaluate(lane, t, state, out[ai]);
        }
    }
    // Scene-level tracks (Camera, Audio) are processed separately when those
    // systems ship — for now they have no evaluate functions.
}
