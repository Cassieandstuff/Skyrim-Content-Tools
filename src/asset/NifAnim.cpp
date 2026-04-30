#include "asset/NifAnim.h"

#include "NifFile.hpp"
#include "Animation.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

// ── Key interpolation helpers ─────────────────────────────────────────────────

static glm::vec3 LerpVec3(const std::vector<float>&     times,
                           const std::vector<glm::vec3>& values,
                           float t)
{
    if (values.empty()) return {};
    if (values.size() == 1 || t <= times.front()) return values.front();
    if (t >= times.back()) return values.back();

    int hi = (int)(std::lower_bound(times.begin(), times.end(), t) - times.begin());
    if (hi == 0) return values.front();
    const int lo = hi - 1;
    const float alpha = (t - times[lo]) / (times[hi] - times[lo]);
    return glm::mix(values[lo], values[hi], alpha);
}

static float LerpFloat(const std::vector<float>& times,
                        const std::vector<float>& values,
                        float t)
{
    if (values.empty()) return 1.f;
    if (values.size() == 1 || t <= times.front()) return values.front();
    if (t >= times.back()) return values.back();

    int hi = (int)(std::lower_bound(times.begin(), times.end(), t) - times.begin());
    if (hi == 0) return values.front();
    const int lo = hi - 1;
    const float alpha = (t - times[lo]) / (times[hi] - times[lo]);
    return glm::mix(values[lo], values[hi], alpha);
}

static glm::quat SlerpQuat(const std::vector<float>&     times,
                            const std::vector<glm::quat>& values,
                            float t)
{
    if (values.empty()) return glm::quat(1.f, 0.f, 0.f, 0.f);
    if (values.size() == 1 || t <= times.front()) return values.front();
    if (t >= times.back()) return values.back();

    int hi = (int)(std::lower_bound(times.begin(), times.end(), t) - times.begin());
    if (hi == 0) return values.front();
    const int lo = hi - 1;
    const float alpha = (t - times[lo]) / (times[hi] - times[lo]);
    return glm::slerp(values[lo], values[hi], alpha);
}

// ── NifShapeMorphAnim::Evaluate ───────────────────────────────────────────────

void NifShapeMorphAnim::Evaluate(float t, std::vector<glm::vec3>& out) const
{
    if (duration > 0.f && t > 0.f)
        t = std::fmod(t, duration);

    out.resize(base.size());
    std::copy(base.begin(), base.end(), out.begin());

    for (int m = 0; m < (int)deltas.size(); m++) {
        const float w = LerpFloat(channels[m].times, channels[m].weights, t);
        if (w == 0.f) continue;
        const int n = std::min((int)deltas[m].size(), (int)out.size());
        for (int v = 0; v < n; v++)
            out[v] += deltas[m][v] * w;
    }
}

// ── NifAnimClip::Evaluate ─────────────────────────────────────────────────────

void NifAnimClip::Evaluate(float t,
                            std::unordered_map<std::string, glm::mat4>& outLocals) const
{
    if (duration > 0.f && t > 0.f)
        t = std::fmod(t, duration);

    for (const NifNodeAnim& anim : nodes) {
        glm::vec3 pos(0.f);
        glm::quat rot(1.f, 0.f, 0.f, 0.f);
        float     scale = 1.f;

        if (anim.hasPos())   pos   = LerpVec3(anim.posTimes,   anim.posValues,   t);
        if (anim.hasRot())   rot   = SlerpQuat(anim.rotTimes,  anim.rotValues,   t);
        if (anim.hasScale()) scale = LerpFloat(anim.scaleTimes, anim.scaleValues, t);

        glm::mat4 m = glm::mat4_cast(rot);
        m[0] *= scale;
        m[1] *= scale;
        m[2] *= scale;
        m[3]  = glm::vec4(pos, 1.f);
        outLocals[anim.name] = m;
    }
}

// ── ParseNifAnim ──────────────────────────────────────────────────────────────

NifAnimClip ParseNifAnim(nifly::NifFile& nif)
{
    NifAnimClip clip;

    // Find NiControllerManager by scanning all blocks (it lives on the root node
    // but scanning is simpler than walking the controller linked-list).
    nifly::NiControllerManager* mgr = nullptr;
    const uint32_t numBlocks = nif.GetHeader().GetNumBlocks();
    for (uint32_t i = 0; i < numBlocks; i++) {
        mgr = nif.GetHeader().GetBlock<nifly::NiControllerManager>(i);
        if (mgr) break;
    }
    if (!mgr) return clip;

    // Find an ambient loop sequence.  Prefer one named "Idle"; fall back to the
    // first CYCLE_LOOP sequence found.  CYCLE_CLAMP / CYCLE_REVERSE sequences
    // are triggered one-shots (doors, drawers) — never loop them automatically.
    nifly::NiControllerSequence* seq = nullptr;
    for (auto it = mgr->controllerSequenceRefs.cbegin();
              it != mgr->controllerSequenceRefs.cend(); ++it)
    {
        nifly::NiControllerSequence* candidate =
            nif.GetHeader().GetBlock<nifly::NiControllerSequence>(it->index);
        if (!candidate || candidate->cycleType != nifly::CYCLE_LOOP) continue;
        if (!seq) seq = candidate;  // first loop sequence seen
        if (candidate->name.get() == "Idle") { seq = candidate; break; }
    }
    if (!seq) return clip;

    clip.duration = seq->stopTime - seq->startTime;
    if (clip.duration <= 0.f) {
        // Degenerate sequence — try to infer from key times below.
        clip.duration = 0.f;
    }

    // NiSyncVector only has non-const begin()/end(); iterate via non-const seq pointer.
    for (nifly::ControllerLink& link : seq->controlledBlocks) {
        // Node name: NiStringRef is the modern SSE form; NiString is the legacy form.
        std::string nodeName = link.nodeName.get();
        if (nodeName.empty()) nodeName = link.targetName.get();
        if (nodeName.empty()) continue;

        // Only handle transform interpolators.
        auto* interp =
            nif.GetHeader().GetBlock<nifly::NiTransformInterpolator>(link.interpolatorRef);
        if (!interp) continue;

        auto* data =
            nif.GetHeader().GetBlock<nifly::NiTransformData>(interp->dataRef);
        if (!data) continue;

        NifNodeAnim anim;
        anim.name = nodeName;

        // Position keys (NiAnimationKeyGroup<Vector3>).
        const uint32_t nPos = data->translations.GetNumKeys();
        for (uint32_t i = 0; i < nPos; i++) {
            const auto k = data->translations.GetKey((int)i);
            anim.posTimes.push_back(k.time);
            anim.posValues.push_back({ k.value.x, k.value.y, k.value.z });
            clip.duration = std::max(clip.duration, k.time);
        }

        // Rotation keys — quaternion or XYZ Euler channels.
        if (data->rotationType == nifly::LINEAR_KEY  ||
            data->rotationType == nifly::QUADRATIC_KEY ||
            data->rotationType == nifly::TBC_KEY      ||
            data->rotationType == nifly::CONST_KEY)
        {
            for (const auto& k : data->quaternionKeys) {
                anim.rotTimes.push_back(k.time);
                // nifly Quaternion: w first (wxyz); glm::quat ctor: (w, x, y, z)
                anim.rotValues.emplace_back(k.value.w, k.value.x,
                                            k.value.y, k.value.z);
                clip.duration = std::max(clip.duration, k.time);
            }
        }
        else if (data->rotationType == nifly::XYZ_ROTATION_KEY) {
            // Sample the three Euler channels at a shared time grid and convert
            // to quaternions.  Use whichever channel has the most keys as driver.
            const uint32_t nx = data->xRotations.GetNumKeys();
            const uint32_t ny = data->yRotations.GetNumKeys();
            const uint32_t nz = data->zRotations.GetNumKeys();

            // Collect all unique key times.
            std::vector<float> allTimes;
            allTimes.reserve(nx + ny + nz);
            for (uint32_t i = 0; i < nx; i++) allTimes.push_back(data->xRotations.GetKey((int)i).time);
            for (uint32_t i = 0; i < ny; i++) allTimes.push_back(data->yRotations.GetKey((int)i).time);
            for (uint32_t i = 0; i < nz; i++) allTimes.push_back(data->zRotations.GetKey((int)i).time);
            std::sort(allTimes.begin(), allTimes.end());
            allTimes.erase(std::unique(allTimes.begin(), allTimes.end()), allTimes.end());

            // Build float key tables for each axis for interpolation.
            auto buildAxis = [](const nifly::NiAnimationKeyGroup<float>& grp)
                -> std::pair<std::vector<float>, std::vector<float>>
            {
                std::pair<std::vector<float>, std::vector<float>> out;
                const uint32_t n = grp.GetNumKeys();
                for (uint32_t i = 0; i < n; i++) {
                    const auto k = grp.GetKey((int)i);
                    out.first.push_back(k.time);
                    out.second.push_back(k.value);
                }
                return out;
            };

            auto [xTimes, xVals] = buildAxis(data->xRotations);
            auto [yTimes, yVals] = buildAxis(data->yRotations);
            auto [zTimes, zVals] = buildAxis(data->zRotations);

            for (float kt : allTimes) {
                const float ax = LerpFloat(xTimes, xVals, kt);
                const float ay = LerpFloat(yTimes, yVals, kt);
                const float az = LerpFloat(zTimes, zVals, kt);
                // Skyrim uses extrinsic ZYX (yaw-pitch-roll) in radians.
                glm::quat q = glm::quat(glm::vec3(ax, ay, az));
                anim.rotTimes.push_back(kt);
                anim.rotValues.push_back(q);
                clip.duration = std::max(clip.duration, kt);
            }
        }

        // Scale keys.
        const uint32_t nScl = data->scales.GetNumKeys();
        for (uint32_t i = 0; i < nScl; i++) {
            const auto k = data->scales.GetKey((int)i);
            anim.scaleTimes.push_back(k.time);
            anim.scaleValues.push_back(k.value);
            clip.duration = std::max(clip.duration, k.time);
        }

        if (anim.hasPos() || anim.hasRot() || anim.hasScale())
            clip.nodes.push_back(std::move(anim));
    }

    if (clip.duration <= 0.f) clip.duration = 1.f;  // fallback for constant-key clips

    if (!clip.empty())
        fprintf(stderr, "[NifAnim] %zu animated nodes, duration=%.2fs\n",
                clip.nodes.size(), clip.duration);

    return clip;
}

// ── ParseNifShapeMorph ────────────────────────────────────────────────────────

NifShapeMorphAnim ParseNifShapeMorph(nifly::NifFile& nif,
                                     nifly::NiGeomMorpherController& gmc,
                                     uint32_t numVerts)
{
    NifShapeMorphAnim result;

    auto* md = nif.GetHeader().GetBlock<nifly::NiMorphData>(gmc.dataRef);
    if (!md) return result;
    if (md->numVertices != numVerts) return result;

    std::vector<nifly::Morph> morphs = md->GetMorphs();
    if (morphs.size() < 2) return result;  // need base + at least one delta

    // morphs[0] = base absolute positions
    result.base.reserve(morphs[0].vectors.size());
    for (const auto& v : morphs[0].vectors)
        result.base.push_back({ v.x, v.y, v.z });

    const int numDeltas = (int)morphs.size() - 1;
    result.deltas.resize(numDeltas);
    result.channels.resize(numDeltas);

    const uint32_t numInterps = gmc.interpolatorRefs.GetSize();

    for (int m = 0; m < numDeltas; m++) {
        const auto& morph = morphs[m + 1];
        result.deltas[m].reserve(morph.vectors.size());
        for (const auto& v : morph.vectors)
            result.deltas[m].push_back({ v.x, v.y, v.z });

        // interpolatorRefs[0] = base (ignored), [m+1] = delta m.
        // SSE (file >= V20_2_0_7) writes interpWeights instead of interpolatorRefs,
        // so interpolatorRefs will be empty for SSE NIFs — check both.
        const uint32_t interpIdx = (uint32_t)(m + 1);
        nifly::NiFloatInterpolator* interp = nullptr;

        if (interpIdx < numInterps)
            interp = nif.GetHeader().GetBlock<nifly::NiFloatInterpolator>(
                gmc.interpolatorRefs.GetBlockRef(interpIdx));

        if (!interp && interpIdx < (uint32_t)gmc.interpWeights.size())
            interp = nif.GetHeader().GetBlock<nifly::NiFloatInterpolator>(
                gmc.interpWeights[interpIdx].interpRef);

        if (!interp) continue;

        auto* fd = nif.GetHeader().GetBlock<nifly::NiFloatData>(interp->dataRef);
        if (!fd) {
            // Constant weight baked into floatValue
            if (interp->floatValue != 0.f) {
                result.channels[m].times   = { 0.f };
                result.channels[m].weights = { interp->floatValue };
            }
            continue;
        }

        const uint32_t nKeys = fd->data.GetNumKeys();
        for (uint32_t k = 0; k < nKeys; k++) {
            const auto key = fd->data.GetKey((int)k);
            result.channels[m].times.push_back(key.time);
            result.channels[m].weights.push_back(key.value);
            result.duration = std::max(result.duration, key.time);
        }
    }

    if (!result.deltas.empty() && result.duration <= 0.f)
        result.duration = 1.f;

    fprintf(stderr, "[NifAnim] shape morph: %d deltas, %u verts, duration=%.2fs\n",
            numDeltas, numVerts, result.duration);

    return result;
}
