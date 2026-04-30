#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>

// ── TriDocument ───────────────────────────────────────────────────────────────
//
// Parsed representation of a FaceGen FRTRI003 (.tri) file.
//
// Only "difference morphs" (morphNum section) are loaded — these are the
// per-vertex displacement fields that Skyrim's expression system uses, and
// are where MFEE injects its 52 ARKit blend shapes named "RokokofaceUBE|*".
//
// "Modifier morphs" (addMorphNum section) are skipped — they are used by
// FaceGen's absolute/mask morphs and are not needed for expression animation.
//
// Vertex ordering in `deltas` matches the NIF shape's vertex ordering.
// Apply: finalPos[i] = nifBase[i] + Σ(morphWeights[m] × morph.deltas[i])
// ---------------------------------------------------------------------------

// One difference morph (expression/phoneme blend shape).
struct TriMorph {
    std::string            name;    // e.g. "RokokofaceUBE|jawOpen", "Aah", "BrowUpLeft"
    std::vector<glm::vec3> deltas;  // per-vertex displacement; size == TriDocument::vertexNum
};

struct TriDocument {
    int                    vertexNum = 0;  // base mesh vertex count (must match paired NIF shape)
    std::vector<TriMorph>  morphs;         // all difference morphs, in file order

    bool empty() const { return morphs.empty() && vertexNum == 0; }
};

// Load from a file path (loose file).
TriDocument LoadTriDocument(const std::string& path);

// Load from in-memory bytes (e.g. extracted from a BSA via ResolveAsset).
TriDocument LoadTriDocumentFromBytes(const std::vector<uint8_t>& bytes,
                                     const std::string& debugPath = {});
