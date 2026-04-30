#include "asset/TriDocument.h"
#include <cstdio>
#include <cstring>
#include <fstream>

// ── Binary reader helper ───────────────────────────────────────────────────────

namespace {

struct BinReader {
    const uint8_t* data;
    size_t         size;
    size_t         pos = 0;

    template<typename T>
    bool read(T& out) {
        if (pos + sizeof(T) > size) return false;
        std::memcpy(&out, data + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }

    bool skip(size_t n) {
        if (pos + n > size) return false;
        pos += n;
        return true;
    }

    // Read a narrow (UTF-8/ASCII) morph name: int32 nameLen (incl. null) + nameLen bytes.
    bool readNameNarrow(std::string& out) {
        int32_t len = 0;
        if (!read(len) || len <= 0) return false;
        if (pos + static_cast<size_t>(len) > size) return false;
        out.assign(reinterpret_cast<const char*>(data + pos),
                   static_cast<size_t>(len - 1)); // exclude null terminator
        pos += static_cast<size_t>(len);
        return true;
    }
};

// ── Geometry skip ──────────────────────────────────────────────────────────────
// FRTRI003 geometry layout (confirmed by binary analysis of MFEE TRI files):
//
//   1. Base vertices:      vertexNum   × 12 bytes  (3 × float32)
//   2. Face indices:       faceNum     × 12 bytes  (3 × uint32 per triangle)
//   3. UV coordinates:     uvVertexNum ×  8 bytes  (2 × float32, if uvVertexNum > 0)
//   4. UV face indices:    faceNum     × 12 bytes  (3 × uint32, if uvVertexNum > 0)
//
// uvVertexNum is stored in header field +0x1C.  It equals vertexNum when each
// vertex has a unique UV coord (1:1 mapping) or less when UVs are shared.
//
static bool SkipGeometry(BinReader& r,
                          int32_t vertexNum, int32_t faceNum, int32_t uvVertexNum,
                          const std::string& dbg)
{
    // 1. Base vertices
    if (!r.skip(static_cast<size_t>(vertexNum) * 12u)) {
        fprintf(stderr, "[TRI] '%s': truncated in base vertices\n", dbg.c_str());
        return false;
    }
    // 2. Face indices (uint32 × 3)
    if (faceNum > 0) {
        if (!r.skip(static_cast<size_t>(faceNum) * 12u)) {
            fprintf(stderr, "[TRI] '%s': truncated in face indices\n", dbg.c_str());
            return false;
        }
    }
    // 3 & 4. UV data (only if UV coords exist)
    if (uvVertexNum > 0) {
        // UV coordinates (float32 × 2)
        if (!r.skip(static_cast<size_t>(uvVertexNum) * 8u)) {
            fprintf(stderr, "[TRI] '%s': truncated in UV coords\n", dbg.c_str());
            return false;
        }
        // UV face indices (uint32 × 3)
        if (faceNum > 0) {
            if (!r.skip(static_cast<size_t>(faceNum) * 12u)) {
                fprintf(stderr, "[TRI] '%s': truncated in UV face indices\n", dbg.c_str());
                return false;
            }
        }
    }
    return true;
}

// ── Core parse ─────────────────────────────────────────────────────────────────

static TriDocument ParseTriBytes(const uint8_t* data, size_t size,
                                  const std::string& debugPath)
{
    if (size < 64) {
        fprintf(stderr, "[TRI] '%s': file too small (%zu bytes)\n",
                debugPath.c_str(), size);
        return {};
    }

    // Magic
    if (std::memcmp(data, "FRTRI003", 8) != 0) {
        fprintf(stderr, "[TRI] '%s': bad magic (not FRTRI003)\n", debugPath.c_str());
        return {};
    }

    BinReader r{ data, size };
    r.pos = 8;

    // Header fields
    // FRTRI003 header layout (64 bytes total, confirmed empirically):
    //   +0x08  int32  vertexNum      — base mesh vertex count
    //   +0x0C  int32  faceNum        — triangle count
    //   +0x10  int32  (reserved=0)
    //   +0x14  int32  (reserved=0)
    //   +0x18  int32  (reserved=0)
    //   +0x1C  int32  uvVertexNum    — unique UV coord count (= vertexNum when 1:1, may be less)
    //   +0x20  int32  morphNum       — diff morph count  (expression morphs)
    //   +0x24  int32  addMorphNum    — modifier morph count (MFEE ARKit blend shapes: 52)
    //   +0x28  int32  addVertexNum   — extra verts for modifier section (0 in MFEE files)
    //   +0x2C  [20 bytes reserved]  → total 64 bytes
    int32_t vertexNum    = 0;
    int32_t faceNum      = 0;
    int32_t uvVertexNum  = 0;
    int32_t morphNum     = 0;
    int32_t addMorphNum  = 0;
    int32_t addVertexNum = 0;

    auto readHeader = [&]() -> bool {
        if (!r.read(vertexNum))    return false;  // +0x08
        if (!r.read(faceNum))      return false;  // +0x0C
        if (!r.skip(12))           return false;  // +0x10 +0x14 +0x18  (reserved, all 0)
        if (!r.read(uvVertexNum))  return false;  // +0x1C  UV vertex count
        if (!r.read(morphNum))     return false;  // +0x20  numDiffMorphs
        if (!r.read(addMorphNum))  return false;  // +0x24  numModMorphs (ARKit blend shapes)
        if (!r.read(addVertexNum)) return false;  // +0x28  numAddVerts
        if (!r.skip(20))           return false;  // +0x2C-0x3F reserved → total 64
        return true;
    };

    if (!readHeader()) {
        fprintf(stderr, "[TRI] '%s': truncated header\n", debugPath.c_str());
        return {};
    }

    fprintf(stderr, "[TRI] '%s': verts=%d faces=%d uvVerts=%d "
                    "diffMorphs=%d modMorphs=%d addVerts=%d\n",
            debugPath.c_str(), vertexNum, faceNum, uvVertexNum,
            morphNum, addMorphNum, addVertexNum);

    // Skip geometry sections
    if (!SkipGeometry(r, vertexNum, faceNum, uvVertexNum, debugPath))
        return {};

    // ── Read difference morphs ─────────────────────────────────────────────────
    // All morph names are narrow ASCII (int32 nameLen incl. null + nameLen bytes).
    TriDocument doc;
    doc.vertexNum = static_cast<int>(vertexNum);
    doc.morphs.reserve(static_cast<size_t>(morphNum));

    for (int m = 0; m < morphNum; ++m) {
        TriMorph morph;

        if (!r.readNameNarrow(morph.name)) {
            fprintf(stderr, "[TRI] '%s': truncated at morph %d name\n",
                    debugPath.c_str(), m);
            return {};
        }

        // baseDiff: scale factor mapping int16 → float.  finalDelta = int16 × baseDiff
        float baseDiff = 0.f;
        if (!r.read(baseDiff)) {
            fprintf(stderr, "[TRI] '%s': truncated at morph %d baseDiff\n",
                    debugPath.c_str(), m);
            return {};
        }

        // Per-vertex signed 16-bit deltas (3 components per vertex)
        morph.deltas.resize(static_cast<size_t>(vertexNum));
        for (int v = 0; v < vertexNum; ++v) {
            int16_t dx = 0, dy = 0, dz = 0;
            if (!r.read(dx) || !r.read(dy) || !r.read(dz)) {
                fprintf(stderr, "[TRI] '%s': truncated at morph %d vertex %d\n",
                        debugPath.c_str(), m, v);
                return {};
            }
            morph.deltas[v] = glm::vec3(
                static_cast<float>(dx) * baseDiff,
                static_cast<float>(dy) * baseDiff,
                static_cast<float>(dz) * baseDiff);
        }

        doc.morphs.push_back(std::move(morph));
    }

    // ── Modifier morphs (addMorphNum) ─────────────────────────────────────────
    // In MFEE extended TRI files the modifier-morph slot holds the 52 ARKit
    // blend shapes.  Same wire format as diff morphs (narrow name + baseDiff +
    // vertexNum × int16[3]).  addVertexNum=0 in MFEE so same vertex count applies.
    doc.morphs.reserve(doc.morphs.size() + static_cast<size_t>(addMorphNum));
    for (int m = 0; m < addMorphNum; ++m) {
        TriMorph morph;

        if (!r.readNameNarrow(morph.name)) {
            fprintf(stderr, "[TRI] '%s': truncated at mod-morph %d name\n",
                    debugPath.c_str(), m);
            break;  // return what we have so far rather than discarding everything
        }

        float baseDiff = 0.f;
        if (!r.read(baseDiff)) {
            fprintf(stderr, "[TRI] '%s': truncated at mod-morph %d baseDiff\n",
                    debugPath.c_str(), m);
            break;
        }

        morph.deltas.resize(static_cast<size_t>(vertexNum));
        bool ok = true;
        for (int v = 0; v < vertexNum; ++v) {
            int16_t dx = 0, dy = 0, dz = 0;
            if (!r.read(dx) || !r.read(dy) || !r.read(dz)) {
                fprintf(stderr, "[TRI] '%s': truncated at mod-morph %d vertex %d\n",
                        debugPath.c_str(), m, v);
                ok = false; break;
            }
            morph.deltas[v] = glm::vec3(
                static_cast<float>(dx) * baseDiff,
                static_cast<float>(dy) * baseDiff,
                static_cast<float>(dz) * baseDiff);
        }
        if (!ok) break;

        doc.morphs.push_back(std::move(morph));
    }

    fprintf(stderr, "[TRI] '%s': %d vert(s), %d total morph(s) (%d diff + %d mod)\n",
            debugPath.c_str(), vertexNum,
            (int)doc.morphs.size(), morphNum, addMorphNum);
    return doc;
}

} // namespace

// ── Public API ─────────────────────────────────────────────────────────────────

TriDocument LoadTriDocument(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        fprintf(stderr, "[TRI] Cannot open '%s'\n", path.c_str());
        return {};
    }
    const std::vector<uint8_t> buf(std::istreambuf_iterator<char>(f), {});
    return ParseTriBytes(buf.data(), buf.size(), path);
}

TriDocument LoadTriDocumentFromBytes(const std::vector<uint8_t>& bytes,
                                      const std::string& debugPath)
{
    if (bytes.empty()) return {};
    return ParseTriBytes(bytes.data(), bytes.size(), debugPath);
}
