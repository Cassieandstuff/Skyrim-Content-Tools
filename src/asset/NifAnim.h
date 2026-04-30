#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace nifly { class NifFile; class NiGeomMorpherController; }

// Per-node keyframe data extracted from a NiControllerSequence.
struct NifNodeAnim {
    std::string            name;

    std::vector<float>     posTimes;
    std::vector<glm::vec3> posValues;

    std::vector<float>     rotTimes;
    std::vector<glm::quat> rotValues;

    std::vector<float>     scaleTimes;
    std::vector<float>     scaleValues;

    bool hasPos()   const { return !posTimes.empty(); }
    bool hasRot()   const { return !rotTimes.empty(); }
    bool hasScale() const { return !scaleTimes.empty(); }
};

// A looping ambient animation clip parsed from a NIF's NiControllerManager.
// Evaluates independently of the scene timeline (free-running wall-clock).
struct NifAnimClip {
    float                    duration = 0.f;  // loop length in seconds
    std::vector<NifNodeAnim> nodes;

    bool empty() const { return nodes.empty(); }

    // Fill outLocals with (nodeName → local transform matrix) at time t.
    // t is wrapped to [0, duration).  Only animated nodes appear in the map.
    void Evaluate(float t,
                  std::unordered_map<std::string, glm::mat4>& outLocals) const;
};

// Parse NiControllerManager/NiControllerSequence data from an already-loaded NIF.
// Returns an empty clip if no supported controller data is present.
NifAnimClip ParseNifAnim(nifly::NifFile& nif);

// ── Per-channel weight curve for a single morph delta ─────────────────────────
struct NifMorphChannel {
    std::vector<float> times;
    std::vector<float> weights;
};

// Vertex-morph animation parsed from a NiGeomMorpherController on a shape.
// morphs[0] = base absolute positions; morphs[1..n] = delta positions.
// Evaluates to blended positions: base + Σ(weight[m] × delta[m]).
struct NifShapeMorphAnim {
    float                               duration = 0.f;
    std::vector<glm::vec3>              base;     // absolute base vertex positions
    std::vector<std::vector<glm::vec3>> deltas;   // [channel][vertex]
    std::vector<NifMorphChannel>        channels; // weight curve per delta

    bool empty() const { return deltas.empty(); }

    // Write blended positions into `out` (resized to base.size()).
    // t is wrapped to [0, duration).
    void Evaluate(float t, std::vector<glm::vec3>& out) const;
};

// Parse NiGeomMorpherController data from a shape controller.
// numVerts: expected vertex count — returns empty if mismatch.
NifShapeMorphAnim ParseNifShapeMorph(nifly::NifFile& nif,
                                     nifly::NiGeomMorpherController& gmc,
                                     uint32_t numVerts);
