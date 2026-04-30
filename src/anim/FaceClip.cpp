#include "anim/FaceClip.h"
#include <pugixml.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string_view>
#include <unordered_map>
#include <utility>

// Annotation integer weight scale: values are 0–100 representing 0.0–1.0.
static constexpr float kWeightScale = 100.f;

// ── ParseFaceAnnotationText ───────────────────────────────────────────────────
// Splits "MorphFace.<system>|<morphName>|<intWeight>" into morphName + weight.
// Returns false for any annotation that doesn't match this 3-field format.
static bool ParseFaceAnnotationText(std::string_view text,
                                    std::string& outMorphName, float& outWeight)
{
    // First field: must start with "MorphFace".
    auto p1 = text.find('|');
    if (p1 == std::string_view::npos) return false;
    if (text.substr(0, 9) != "MorphFace") return false;

    auto rest = text.substr(p1 + 1);

    // Second field: ARKit morph name.
    auto p2 = rest.find('|');
    if (p2 == std::string_view::npos) return false;
    outMorphName = std::string(rest.substr(0, p2));
    if (outMorphName.empty()) return false;

    // Third field: integer weight.
    auto wStr = rest.substr(p2 + 1);
    // Null-terminate for strtol.
    char buf[32];
    if (wStr.size() >= sizeof(buf)) return false;
    std::memcpy(buf, wStr.data(), wStr.size());
    buf[wStr.size()] = '\0';
    char* ep = nullptr;
    long intW = std::strtol(buf, &ep, 10);
    if (ep == buf) return false;

    outWeight = static_cast<float>(intW) / kWeightScale;
    return true;
}

// ── FaceClip::ParseFromXml ────────────────────────────────────────────────────

bool FaceClip::ParseFromXml(const char* xmlData, int xmlLen, const char* clipName,
                             char* errOut, int errLen)
{
    name     = clipName ? clipName : "";
    duration = 0.f;
    channels.clear();

    pugi::xml_document doc;
    pugi::xml_parse_result res =
        doc.load_buffer(xmlData, static_cast<size_t>(xmlLen));
    if (!res) {
        std::snprintf(errOut, errLen,
                      "FaceClip: XML parse error: %s", res.description());
        return false;
    }

    // Navigate hkpackfile → hksection (same as HavokAnimation.cpp).
    auto section = doc.child("hkpackfile").child("hksection");

    // Build morph → channel index map for accumulation.
    std::unordered_map<std::string, int> morphIndex;

    for (auto node : section.children("hkobject")) {
        std::string_view cls = node.attribute("class").as_string();

        // Accept both supported animation types; skip everything else.
        if (cls != "hkaSplineCompressedAnimation" &&
            cls != "hkaInterleavedUncompressedAnimation") continue;

        // Pull duration (keep the largest one if there are multiple animations).
        auto durParam = node.find_child_by_attribute("hkparam", "name", "duration");
        if (durParam) {
            float d = durParam.text().as_float(0.f);
            if (d > duration) duration = d;
        }

        // Find <hkparam name="annotationTracks">.
        auto tracksParam =
            node.find_child_by_attribute("hkparam", "name", "annotationTracks");
        if (!tracksParam) continue;

        // Each hkobject child = one hkaAnnotationTrack.
        for (auto trackObj : tracksParam.children("hkobject")) {
            // <hkparam name="annotations">
            auto annsParam =
                trackObj.find_child_by_attribute("hkparam", "name", "annotations");
            if (!annsParam) continue;

            // Each hkobject child = one hkaAnnotationTrackAnnotation.
            for (auto annObj : annsParam.children("hkobject")) {
                auto timeParam =
                    annObj.find_child_by_attribute("hkparam", "name", "time");
                auto textParam =
                    annObj.find_child_by_attribute("hkparam", "name", "text");
                if (!timeParam || !textParam) continue;

                float       t    = timeParam.text().as_float(0.f);
                const char* text = textParam.text().as_string();

                std::string morphName;
                float       weight = 0.f;
                if (!ParseFaceAnnotationText(text, morphName, weight)) continue;

                // Find or create the channel for this morph.
                auto it = morphIndex.find(morphName);
                int  ci;
                if (it == morphIndex.end()) {
                    ci = static_cast<int>(channels.size());
                    morphIndex[morphName] = ci;
                    FaceMorphChannel ch;
                    ch.morphName = morphName;
                    channels.push_back(std::move(ch));
                } else {
                    ci = it->second;
                }
                channels[ci].times.push_back(t);
                channels[ci].weights.push_back(weight);
            }
        }
    }

    // Sort each channel's keyframes by time.  Annotations are usually already
    // in order, but the spec doesn't guarantee it.
    for (auto& ch : channels) {
        if (ch.times.size() <= 1) continue;
        // Zip-sort times + weights together.
        std::vector<std::pair<float, float>> kf;
        kf.reserve(ch.times.size());
        for (size_t i = 0; i < ch.times.size(); i++)
            kf.push_back({ ch.times[i], ch.weights[i] });
        std::sort(kf.begin(), kf.end());
        for (size_t i = 0; i < kf.size(); i++) {
            ch.times[i]   = kf[i].first;
            ch.weights[i] = kf[i].second;
        }
    }

    return true;
}

// ── FaceClip::Evaluate ────────────────────────────────────────────────────────

float FaceClip::Evaluate(const std::string& morphName, float t) const
{
    for (const auto& ch : channels) {
        if (ch.morphName != morphName) continue;
        if (ch.times.empty()) return 0.f;

        // Clamp to the clip's time range.
        t = std::max(0.f, std::min(t, duration));

        // Binary search for the first keyframe > t.
        auto it = std::lower_bound(ch.times.begin(), ch.times.end(), t);

        if (it == ch.times.end())   return ch.weights.back();
        if (it == ch.times.begin()) return ch.weights.front();

        int   hi = static_cast<int>(it - ch.times.begin());
        int   lo = hi - 1;
        float t0 = ch.times[lo],   t1 = ch.times[hi];
        float w0 = ch.weights[lo], w1 = ch.weights[hi];
        if (t1 - t0 < 1e-6f) return w0;
        float alpha = (t - t0) / (t1 - t0);
        return w0 + alpha * (w1 - w0);
    }
    return 0.f;
}

// ── FaceClip::EvaluateAll ─────────────────────────────────────────────────────

void FaceClip::EvaluateAll(float t,
                            std::map<std::string, float>& outWeights) const
{
    for (const auto& ch : channels) {
        float w = Evaluate(ch.morphName, t);
        if (w != 0.f) outWeights[ch.morphName] = w;
    }
}
