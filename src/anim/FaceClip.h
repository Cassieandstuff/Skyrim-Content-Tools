#pragma once
#include <map>
#include <string>
#include <vector>

// ── FaceMorphChannel ──────────────────────────────────────────────────────────
// Time-sorted keyframes for a single ARKit blend shape.
struct FaceMorphChannel {
    std::string        morphName;
    std::vector<float> times;    // seconds, sorted ascending
    std::vector<float> weights;  // normalized 0–1, parallel to times
};

// ── FaceClip ──────────────────────────────────────────────────────────────────
// Face animation clip built from Havok annotation track entries of the form:
//   "MorphFace.<system>|<morphName>|<intWeight>"
// where intWeight is an integer on the 0–100 scale.
//
// One annotation entry per morph per keyframe; multiple morphs at the same
// timestamp live in separate annotation entries.  The parser accumulates all
// entries into per-morph channels for efficient evaluation.
struct FaceClip {
    std::string name;
    std::string sourcePath;   // absolute path of the source HKX
    float       duration = 0.f;

    // Per-morph channels, one per unique ARKit blend shape found in annotations.
    std::vector<FaceMorphChannel> channels;

    // ── Parsing ───────────────────────────────────────────────────────────────
    // Parse face annotations from a Havok packfile XML buffer (output of HkxToXml).
    // Populates *this with all annotation entries matching the MorphFace format.
    // Returns true on success (even if zero face annotations are found — that just
    // leaves channels empty, which is valid for body-only HKX files).
    bool ParseFromXml(const char* xmlData, int xmlLen, const char* clipName,
                      char* errOut, int errLen);

    // ── Evaluation ────────────────────────────────────────────────────────────
    // Linear-interpolated weight for a single morph at time t.
    // Returns 0.f if morphName is not in this clip.
    float Evaluate(const std::string& morphName, float t) const;

    // Fill outWeights with interpolated weights for every channel at time t.
    // Only channels with non-zero weight at t are written.
    void EvaluateAll(float t, std::map<std::string, float>& outWeights) const;
};
