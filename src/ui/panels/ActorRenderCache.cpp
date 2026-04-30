#include "ui/panels/ActorRenderCache.h"
#include "app/AppState.h"
#include "asset/NifDocument.h"
#include "anim/Sequence.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cfloat>
#include <cstdio>

// ── NeedsRebuild ─────────────────────────────────────────────────────────────

bool ActorRenderCache::NeedsRebuild(const AppState& state) const
{
    return (int)state.actors.size()    != cachedActorCount_
        || (int)state.skeletons.size() != cachedSkeletonCount_;
}

// ── Free ─────────────────────────────────────────────────────────────────────

void ActorRenderCache::Free(ISceneRenderer& renderer)
{
    for (auto& ar : cache_) {
        for (MeshHandle    h : ar.meshHandles)    renderer.FreeMesh(h);
        for (TextureHandle t : ar.textureHandles) renderer.FreeTexture(t);
    }
    cache_.clear();
    cachedActorCount_    = -1;
    cachedSkeletonCount_ = -1;
}

// ── Rebuild ───────────────────────────────────────────────────────────────────

void ActorRenderCache::Rebuild(AppState& state, ISceneRenderer& renderer)
{
    for (auto& ar : cache_) {
        for (MeshHandle    h : ar.meshHandles)    renderer.FreeMesh(h);
        for (TextureHandle t : ar.textureHandles) renderer.FreeTexture(t);
        ar.meshHandles.clear();
        ar.textureHandles.clear();
        ar.meshTransforms.clear();
        ar.meshSkinBindings.clear();
        ar.meshMorphBases.clear();
        ar.morphWeightsCached.clear();
        ar.morphWeightsEval.clear();
        ar.loadedNifPath.clear();
    }

    const int n = (int)state.actors.size();
    cache_.resize(n);
    for (int ai = 0; ai < n; ai++) {
        cache_[ai].refPose = {};
        cache_[ai].pose    = {};
        const Skeleton* skel = state.SkeletonForActor(ai);
        if (!skel) continue;
        cache_[ai].refPose = skel->MakeReferencePose();
        cache_[ai].pose    = cache_[ai].refPose;
        cache_[ai].pose.SolveFK();
    }
    cachedActorCount_    = n;
    cachedSkeletonCount_ = (int)state.skeletons.size();
}

// ── SyncNifHandles ────────────────────────────────────────────────────────────

void ActorRenderCache::SyncNifHandles(AppState& state, ISceneRenderer& renderer)
{
    for (int ai = 0; ai < (int)cache_.size(); ai++) {
        const Actor& actor = state.actors[ai];
        const ActorDocument* ce = (actor.castIndex >= 0 &&
                                   actor.castIndex < (int)state.cast.size())
                                ? &state.cast[actor.castIndex] : nullptr;
        const std::string& bodyPath  = ce ? ce->bodyNifPath  : "";
        const std::string& handsPath = ce ? ce->handsNifPath : "";
        const std::string& feetPath  = ce ? ce->feetNifPath  : "";
        const std::string& headPath  = ce ? ce->headNifPath  : "";

        std::string cacheKey = bodyPath + "|" + handsPath + "|" + feetPath + "|" + headPath;
        if (ce) for (const auto& hp : ce->headPartNifs) cacheKey += "|" + hp;
        if (cacheKey == cache_[ai].loadedNifPath) continue;

        for (MeshHandle    h : cache_[ai].meshHandles)    renderer.FreeMesh(h);
        for (TextureHandle t : cache_[ai].textureHandles) renderer.FreeTexture(t);
        cache_[ai].meshHandles.clear();
        cache_[ai].textureHandles.clear();
        cache_[ai].meshTransforms.clear();
        cache_[ai].meshSkinBindings.clear();
        cache_[ai].meshMorphBases.clear();
        cache_[ai].morphWeightsCached.clear();
        cache_[ai].morphWeightsEval.clear();
        cache_[ai].loadedNifPath = cacheKey;

        auto loadNif = [&](const std::string& path) {
            if (path.empty()) return;
            fprintf(stderr, "[Viewport] Loading NIF: %s\n", path.c_str());

            NifDocument doc;
            const bool isAbsolute = (path.size() >= 2 && path[1] == ':')
                                  || (!path.empty() && (path[0] == '/' || path[0] == '\\'));
            if (isAbsolute) {
                doc = LoadNifDocument(path);
            } else {
                std::vector<uint8_t> nifBytes;
                if (state.ResolveAsset(path, nifBytes))
                    doc = LoadNifDocumentFromBytes(nifBytes, path);
                else
                    fprintf(stderr, "[Viewport]   NIF not resolved via ResolveAsset\n");
            }

            fprintf(stderr, "[Viewport]   -> %d shape(s)\n", (int)doc.shapes.size());

            const Skeleton* skel = state.SkeletonForActor(ai);

            for (int si = 0; si < (int)doc.shapes.size(); si++) {
                const NifDocShape& ds    = doc.shapes[si];
                const NifBlock&    block = doc.blocks[ds.blockIndex];

                cache_[ai].meshHandles.push_back(renderer.UploadMesh(ds.meshData));
                cache_[ai].meshTransforms.push_back(block.toRoot);

                MeshMorphBase mmb;
                mmb.positions   = ds.meshData.positions;
                mmb.vertexCount = (int)ds.meshData.positions.size();
                cache_[ai].meshMorphBases.push_back(std::move(mmb));

                TextureHandle th = TextureHandle::Invalid;
                if (!ds.diffusePath.empty()) {
                    std::vector<uint8_t> texBytes;
                    if (state.ResolveAsset(ds.diffusePath, texBytes)) {
                        th = renderer.LoadTextureFromMemory(texBytes);
                        fprintf(stderr, "[Viewport]   tex OK  '%s' -> handle %u\n",
                                ds.diffusePath.c_str(), (unsigned)th);
                    } else {
                        fprintf(stderr, "[Viewport]   tex MISS '%s'\n",
                                ds.diffusePath.c_str());
                    }
                }
                cache_[ai].textureHandles.push_back(th);

                MeshSkinBinding msb;
                msb.isSkinned = ds.isSkinned;
                if (ds.isSkinned) {
                    const int numSkinBones = (int)ds.skinBindings.size();
                    msb.skelBoneIdx.resize(numSkinBones, -1);
                    msb.inverseBindMats.resize(numSkinBones, glm::mat4(1.f));
                    for (int j = 0; j < numSkinBones; j++) {
                        msb.inverseBindMats[j] = ds.skinBindings[j].inverseBindMatrix;
                        if (skel) {
                            const std::string& boneName = ds.skinBindings[j].boneName;
                            for (int bi = 0; bi < (int)skel->bones.size(); bi++) {
                                if (skel->bones[bi].name == boneName) {
                                    msb.skelBoneIdx[j] = bi;
                                    break;
                                }
                            }
                        }
                    }
                    int resolved = 0;
                    for (int idx : msb.skelBoneIdx) if (idx >= 0) resolved++;
                    fprintf(stderr, "[Viewport]   skin '%s': %d/%d bones resolved\n",
                            ds.skinBindings.empty() ? "" : ds.skinBindings[0].boneName.c_str(),
                            resolved, numSkinBones);
                }
                cache_[ai].meshSkinBindings.push_back(std::move(msb));
            }
        };

        loadNif(bodyPath);
        loadNif(handsPath);
        loadNif(feetPath);
        loadNif(headPath);
        if (ce) for (const auto& hp : ce->headPartNifs) loadNif(hp);
    }
}

// ── ApplyMorphs ───────────────────────────────────────────────────────────────
// Blend-shape evaluation: accumulate weighted deltas onto base positions.
// Returns the blended vertex buffer (always a full copy of base).
static std::vector<glm::vec3> ApplyMorphs(
    const std::vector<glm::vec3>&      base,
    const TriDocument&                 tri,
    const std::map<std::string, float>& weights)
{
    std::vector<glm::vec3> result = base;
    for (const TriMorph& m : tri.morphs) {
        auto it = weights.find(m.name);
        if (it == weights.end() || it->second == 0.f) continue;
        const float w = it->second;
        const int   n = std::min((int)m.deltas.size(), (int)result.size());
        for (int v = 0; v < n; v++)
            result[v] += m.deltas[v] * w;
    }
    return result;
}

// ── SyncMorphs ────────────────────────────────────────────────────────────────

void ActorRenderCache::SyncMorphs(AppState& state, ISceneRenderer& renderer)
{
    for (int ai = 0; ai < (int)cache_.size(); ai++) {
        const Actor& actor = state.actors[ai];
        if (actor.castIndex < 0 || actor.castIndex >= (int)state.cast.size()) continue;
        ActorDocument& doc = state.cast[actor.castIndex];

        const bool hasEval = !cache_[ai].morphWeightsEval.empty();
        const std::map<std::string, float>& effective =
            hasEval ? cache_[ai].morphWeightsEval : doc.morphWeights;

        if (effective == cache_[ai].morphWeightsCached) continue;
        cache_[ai].morphWeightsCached = effective;

        if (doc.triDocs.empty()) continue;

        for (int mi = 0; mi < (int)cache_[ai].meshHandles.size(); mi++) {
            if (mi >= (int)cache_[ai].meshMorphBases.size()) continue;
            const MeshMorphBase& base = cache_[ai].meshMorphBases[mi];
            if (base.positions.empty()) continue;

            const TriDocument* tri = nullptr;
            for (const TriDocument& td : doc.triDocs) {
                if (td.vertexNum == base.vertexCount) { tri = &td; break; }
            }
            if (!tri || tri->morphs.empty()) continue;

            renderer.UpdateMeshPositions(cache_[ai].meshHandles[mi],
                ApplyMorphs(base.positions, *tri, effective));
        }
    }
}

// ── EvaluatePoses ─────────────────────────────────────────────────────────────

void ActorRenderCache::EvaluatePoses(AppState& state)
{
    if (cache_.empty()) return;

    if (!state.sequence.Empty()) {
        std::vector<ActorEval> evals;
        state.sequence.Evaluate(state.time, state, evals);
        for (int ai = 0; ai < (int)cache_.size(); ai++) {
            if (ai < (int)evals.size()) {
                cache_[ai].pose = evals[ai].pose;
                cache_[ai].pose.SolveFK();
                cache_[ai].morphWeightsEval = std::move(evals[ai].morphWeights);
            } else {
                cache_[ai].pose = cache_[ai].refPose;
                cache_[ai].pose.SolveFK();
                cache_[ai].morphWeightsEval.clear();
            }
        }
    } else if (state.selectedClip >= 0 && state.selectedClip < (int)state.clips.size()) {
        for (int ai = 0; ai < (int)cache_.size(); ai++) {
            cache_[ai].pose = cache_[ai].refPose;
            state.clips[state.selectedClip].Evaluate(state.time, cache_[ai].pose);
            cache_[ai].pose.SolveFK();
            cache_[ai].morphWeightsEval.clear();
        }
    } else {
        for (auto& ar : cache_) {
            ar.pose = ar.refPose;
            ar.pose.SolveFK();
            ar.morphWeightsEval.clear();
        }
    }
}

// ── Draw ──────────────────────────────────────────────────────────────────────

void ActorRenderCache::Draw(const AppState& state, ISceneRenderer& renderer) const
{
    for (int ai = 0; ai < (int)cache_.size(); ai++) {
        const ActorRenderData& ar = cache_[ai];

        for (int mi = 0; mi < (int)ar.meshHandles.size(); mi++) {
            const TextureHandle th = (mi < (int)ar.textureHandles.size())
                ? ar.textureHandles[mi] : TextureHandle::Invalid;

            DrawSurface surf;
            surf.diffuse = th;
            surf.tint    = (th != TextureHandle::Invalid)
                ? glm::vec4(1.f, 1.f, 1.f, 1.f)
                : glm::vec4(0.70f, 0.70f, 0.75f, 1.f);

            const MeshSkinBinding* msb =
                (mi < (int)ar.meshSkinBindings.size())
                ? &ar.meshSkinBindings[mi] : nullptr;

            if (msb && msb->isSkinned && !msb->skelBoneIdx.empty()
                && !ar.pose.boneWorldMat.empty()) {
                const int numSkinBones = (int)msb->skelBoneIdx.size();
                std::vector<glm::mat4> skinMats(numSkinBones, glm::mat4(1.f));
                for (int j = 0; j < numSkinBones; j++) {
                    const int si = msb->skelBoneIdx[j];
                    if (si >= 0 && si < (int)ar.pose.boneWorldMat.size())
                        skinMats[j] = ar.pose.boneWorldMat[si] * msb->inverseBindMats[j];
                }
                renderer.DrawSkinnedMesh(ar.meshHandles[mi], glm::mat4(1.f), skinMats, surf);
            } else {
                renderer.DrawMesh(ar.meshHandles[mi], ar.meshTransforms[mi], surf);
            }
        }

        // Skeleton overlay
        if (ar.pose.empty()) continue;
        const Skeleton* skel = state.SkeletonForActor(ai);
        if (!skel) continue;
        renderer.DrawSkeleton(*skel, ar.pose);
    }
}

// ── RefPoseBounds ─────────────────────────────────────────────────────────────

bool ActorRenderCache::RefPoseBounds(glm::vec3& outMin, glm::vec3& outMax) const
{
    outMin = glm::vec3( FLT_MAX);
    outMax = glm::vec3(-FLT_MAX);
    bool any = false;
    for (const auto& ar : cache_) {
        if (ar.refPose.empty()) continue;
        for (const auto& wp : ar.refPose.worldPos) {
            outMin = glm::min(outMin, wp);
            outMax = glm::max(outMax, wp);
            any = true;
        }
    }
    return any;
}
