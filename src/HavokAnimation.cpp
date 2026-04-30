#include "HavokAnimation.h"
#include "core/io/BoundsSafeReader.h"
#include <pugixml.hpp>
#include <glm/gtc/quaternion.hpp>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string_view>
#include <filesystem>

namespace {

// ── Shared XML tuple parsers ──────────────────────────────────────────────────

const char* ParseTuple3(const char* p, float& x, float& y, float& z)
{
    while (*p && *p != '(') ++p;
    if (!*p) return p; ++p;
    char* ep;
    x = std::strtof(p, &ep); p = ep;
    y = std::strtof(p, &ep); p = ep;
    z = std::strtof(p, &ep); p = ep;
    while (*p && *p != ')') ++p;
    if (*p) ++p;
    return p;
}

const char* ParseTuple4(const char* p, float& x, float& y, float& z, float& w)
{
    while (*p && *p != '(') ++p;
    if (!*p) return p; ++p;
    char* ep;
    x = std::strtof(p, &ep); p = ep;
    y = std::strtof(p, &ep); p = ep;
    z = std::strtof(p, &ep); p = ep;
    w = std::strtof(p, &ep); p = ep;
    while (*p && *p != ')') ++p;
    if (*p) ++p;
    return p;
}

// BoundsSafeReader is defined in core/io/BoundsSafeReader.h; alias locally.
using Reader = BoundsSafeReader;

// ── ThreeComp40 quaternion ────────────────────────────────────────────────────
//
// 40-bit layout (5 bytes, LE):
//   bits  0–11: Va (12-bit, component A)
//   bits 12–23: Vb (12-bit, component B)
//   bits 24–35: Vc (12-bit, component C)
//   bits 36–37: resultShift — index of the dropped component in {rx,ry,rz,rw}
//               (0=rx/X dropped, 1=ry/Y, 2=rz/Z, 3=rw/W)
//   bit  38:    sign of dropped component (0=positive, after normalization flip)
//
// The three stored components are the remaining {rx,ry,rz,rw} in index order,
// quantized from [-1/√2, 1/√2] → [0, 4095].

static glm::quat DecTC40(const uint8_t* b)
{
    uint32_t Va = b[0] | ((uint32_t)(b[1] & 0xF) << 8);
    uint32_t Vb = (uint32_t)((b[1] >> 4) & 0xF) | ((uint32_t)b[2] << 4);
    uint32_t Vc = b[3] | ((uint32_t)(b[4] & 0xF) << 8);
    const int  rs   = (b[4] >> 4) & 0x3;
    const bool sign = (b[4] >> 6) & 0x1;

    constexpr float kInvSqrt2 = 0.70710678118f;
    auto dq = [&](uint32_t q) { return (q / 4095.f) * (2.f * kInvSqrt2) - kInvSqrt2; };

    float s[3] = { dq(Va), dq(Vb), dq(Vc) };
    float d    = std::sqrt(std::max(0.f, 1.f - s[0]*s[0] - s[1]*s[1] - s[2]*s[2]));
    if (sign) d = -d;

    float c[4]; c[rs] = d;
    int si = 0;
    for (int i = 0; i < 4; i++) if (i != rs) c[i] = s[si++];

    return glm::quat(c[3], c[0], c[1], c[2]); // glm: (w, x, y, z)
}

// ── Vectorized scalar channel (translation or scale) ─────────────────────────
//
// SDK wire layout (n follows NURBS Book convention: max ctrl pt index, n+1 total):
//
//   IF any dynamic components (mask & 0xF0):
//     uint16 n         — max ctrl pt index (= numCtrlPts - 1)
//     uint8  deg       — shared degree
//     uint8[n+deg+2]   — shared knot vector (n+1 ctrl pts → n+deg+2 knots)
//
//   For each component c = 0,1,2,3 (hkVector4 is 4D: X,Y,Z,W):
//     if mask bit c   (static):  hkReal value  (4 bytes)
//     if mask bit c+4 (dynamic): hkReal min, hkReal max  (8 bytes)
//
//   IF any dynamic:
//     uint8[(n+1) * numDyn * bpc]  — ctrl pts, row-major: [ctrlPt][dynComp]
//       bpc = 1 (BITS8) or 2 (BITS16) from quatType translation/scale field

struct VecCurve {
    float          sV[4]  = {};    // static value per component (X,Y,Z,W)
    float          mn[4]  = {};    // dynamic min per component
    float          mx[4]  = {};    // dynamic max per component
    uint8_t        mask   = 0;
    int            numDyn = 0;     // number of dynamic components (including W)
    int            bpc    = 2;     // bytes per ctrl pt component (1 or 2)
    int            n      = 0;     // max ctrl pt index (n+1 ctrl pts total)
    int            deg    = 0;     // degree (shared)
    const uint8_t* K      = nullptr; // knots (n+deg+2 bytes)
    const uint8_t* C      = nullptr; // ctrl pts ((n+1) * numDyn * bpc bytes)

    static VecCurve Rd(Reader& r, uint8_t mask_, int bytesPerComp, const uint8_t* base) {
        VecCurve vc;
        vc.mask = mask_; vc.bpc = bytesPerComp;

        // Shared NURBS header — only present when any component is dynamic
        if (mask_ & 0xF0) {
            vc.n   = (int)r.U16();
            vc.deg = (int)r.U8();
            vc.K   = r.p; r.skip(vc.n + vc.deg + 2);  // n+1 ctrl pts → n+deg+2 knots
            // Knot array is bytes; align back to 4 before the F32 min/max reads.
            r.align4(base);
        }

        // Per-component scalar data — all 4 components (hkVector4 includes W)
        for (int i = 0; i < 4; i++) {
            if ((mask_ >> i) & 1) {
                if (i < 3) vc.sV[i] = r.F32(); else r.skip(4);  // skip W static
            }
            if ((mask_ >> (i+4)) & 1) {
                if (i < 3) { vc.mn[i] = r.F32(); vc.mx[i] = r.F32(); }
                else          r.skip(8);  // skip W dynamic min+max
                vc.numDyn++;
            }
        }

        // Ctrl pts — row-major layout, only when dynamic components exist
        if (mask_ & 0xF0) {
            vc.C = r.p; r.skip((vc.n + 1) * vc.numDyn * bytesPerComp);
        }

        return vc;
    }

    glm::vec3 Eval(int qt) const {
        glm::vec3 out(0.f);
        for (int i = 0; i < 3; i++)
            if ((mask >> i) & 1) out[i] = sV[i];
        if (numDyn == 0 || n < 0) return out;

        // ── Find knot span (binary search) ───────────────────────────────────────
        // K has n+deg+2 entries; span in [deg, n] s.t. K[span] <= qt < K[span+1].
        int span;
        if      (qt >= (int)K[n + 1]) span = n;
        else if (qt <= (int)K[0])     span = deg;
        else {
            int lo = deg, hi = n;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                if ((int)K[mid] <= qt) lo = mid; else hi = mid - 1;
            }
            span = lo;
        }

        // ── De Boor evaluation ────────────────────────────────────────────────────
        // For each dynamic component, run the de Boor recursion independently.
        // Working array d[0..deg]: initialised from ctrl pts P[span-deg .. span].
        // Knot access: K[j + span - deg] (lower) and K[j + span - r + 1] (upper),
        // both within [0, n+deg+1] for span in [deg,n] and r in [1,deg], j in [r,deg].
        int di = 0;
        for (int i = 0; i < 3; i++) {
            if (!((mask >> (i+4)) & 1)) continue;
            const float range = mx[i] - mn[i];

            auto unpack = [&](int ci) -> float {
                ci = std::clamp(ci, 0, n);
                const uint8_t* p = C + (ci * numDyn + di) * bpc;
                if (bpc == 1) return mn[i] + (*p / 255.f) * range;
                uint16_t v; std::memcpy(&v, p, 2);
                return mn[i] + (v / 65535.f) * range;
            };

            float d[4];                             // deg <= 3 always
            for (int j = 0; j <= deg; j++)
                d[j] = unpack(span - deg + j);

            for (int r = 1; r <= deg; r++) {
                for (int j = deg; j >= r; j--) {
                    const float klo = (float)K[j + span - deg];
                    const float khi = (float)K[j + span - r + 1];
                    const float a   = (khi > klo)
                                    ? std::clamp(((float)qt - klo) / (khi - klo), 0.f, 1.f)
                                    : 0.f;
                    d[j] = (1.f - a) * d[j-1] + a * d[j];
                }
            }

            out[i] = d[deg];
            di++;
        }
        return out;
    }
};

// ── ThreeComp40 spline channel ────────────────────────────────────────────────
//
// Wire layout (n = max ctrl pt index, n+1 total ctrl pts):
//   uint16 n          — max ctrl pt index
//   uint8  deg        — degree
//   uint8[n+deg+2]    — knots
//   uint8[(n+1) * 5]  — ctrl pts (5 bytes each, THREECOMP40)

struct TC40 {
    int            n;    // max ctrl pt index (n+1 ctrl pts)
    int            deg;
    const uint8_t* k;    // knots (n+deg+2 bytes)
    const uint8_t* q;    // ctrl pts (5 bytes each)

    static TC40 Rd(Reader& r) {
        TC40 c = {};
        c.n   = (int)r.U16();
        c.deg = (int)r.U8();
        c.k   = r.p; r.skip(c.n + c.deg + 2);   // n+deg+2 knots
        c.q   = r.p; r.skip((c.n + 1) * 5);      // n+1 ctrl pts
        return c;
    }

    glm::quat Eval(int fi) const
    {
        if (n < 0) return DecTC40(q);

        // Clamp to endpoints
        if (fi <= (int)(uint8_t)k[0])    return DecTC40(q);
        if (fi >= (int)(uint8_t)k[n + 1]) return DecTC40(q + n * 5);

        // ── Find knot span (binary search) ───────────────────────────────────────
        int span;
        {
            int lo = deg, hi = n;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                if ((int)(uint8_t)k[mid] <= fi) lo = mid; else hi = mid - 1;
            }
            span = lo;
        }

        // ── De Boor evaluation with slerp ─────────────────────────────────────────
        // Working array d[0..deg]: ctrl pts P[span-deg .. span].
        // At each recursion level, blend adjacent entries via slerp (shortest path).
        glm::quat d[4];                             // deg <= 3 always
        for (int j = 0; j <= deg; j++) {
            int idx = std::clamp(span - deg + j, 0, n);
            d[j] = DecTC40(q + idx * 5);
        }

        for (int r = 1; r <= deg; r++) {
            for (int j = deg; j >= r; j--) {
                const float klo = (float)(uint8_t)k[j + span - deg];
                const float khi = (float)(uint8_t)k[j + span - r + 1];
                const float a   = (khi > klo)
                                ? std::clamp(((float)fi - klo) / (khi - klo), 0.f, 1.f)
                                : 0.f;
                // Ensure shortest-arc slerp
                if (glm::dot(d[j-1], d[j]) < 0.f) d[j] = -d[j];
                d[j] = glm::normalize(glm::slerp(d[j-1], d[j], a));
            }
        }

        return d[deg];
    }
};

// ── Decompress one block ──────────────────────────────────────────────────────
//
// Mask section layout (interleaved, 4 bytes per transform track):
//   [quatType, posType, rotType, scaleType] × nTracks
//   floatType × nFloatTracks
//   (maskAndQuantizationSize bytes total, already 4-byte aligned)
//
// Returns false if data runs out of bounds (format mismatch).

static bool DecompBlock(
    const uint8_t*                base,
    size_t                        dataLen,
    uint32_t                      offset,
    int                           nTracks,
    int                           nFloatTracks,
    int                           maskQSize,
    const std::vector<uint32_t>&  transformOffsets,
    int                           blockIdx,
    int                           blockStart,
    int                           framesInBlock,
    std::vector<PoseChannel>&     out)
{
    if (static_cast<size_t>(offset) >= dataLen) return false;

    const uint8_t* blockBase = base + offset;
    const uint8_t* dataEnd   = base + dataLen;
    const int      totalFrames = (int)(out.size() / nTracks);

    // Read the interleaved mask section: [quatType, posType, rotType, scaleType] per track
    std::vector<uint8_t> quatT(nTracks), posT(nTracks), rotT(nTracks), scaleT(nTracks);
    {
        Reader r(blockBase, dataEnd);
        for (int i = 0; i < nTracks; i++) {
            quatT[i]  = r.U8();
            posT[i]   = r.U8();
            rotT[i]   = r.U8();
            scaleT[i] = r.U8();
        }
        if (!r.ok) return false;
    }

    // maskQSize tells us exactly where the track data starts within the block.
    // transformOffsets[blockIdx*nTracks + tk] is a BYTE offset from base (data.data())
    // to the start of each track's data.  Fall back to sequential reading if not present.
    const int  toBase      = blockIdx * nTracks;   // first index for this block
    const bool haveOffsets = !transformOffsets.empty() &&
                             toBase + nTracks <= (int)transformOffsets.size();

    // Sequential reader for fallback path
    Reader rSeq(blockBase + (maskQSize > 0 ? maskQSize : nTracks*4 + nFloatTracks),
                dataEnd);
    if (maskQSize > 0) {
        // align4 is already baked into maskQSize for the Havok SDK
    } else {
        rSeq.align4(base);
    }

    for (int tk = 0; tk < nTracks; tk++) {
        // Seek to this track's data: prefer transformOffsets (direct), else sequential.
        // transformOffsets stores BYTE offsets from base (data.data()), not from blockBase,
        // and is NOT multiplied — the SDK stores raw byte offsets.
        const uint8_t* tkDataStart = haveOffsets
            ? base + (ptrdiff_t)transformOffsets[toBase + tk]
            : rSeq.p;
        if (tkDataStart >= dataEnd && (posT[tk]|rotT[tk]|scaleT[tk]) != 0) {
            fprintf(stderr, "[SCT] FAIL tk=%d — offset %td >= dataLen %td\n",
                    tk, tkDataStart - base, (ptrdiff_t)dataLen);
            return false;
        }

        Reader r(tkDataStart, dataEnd);

        // ── Translation ───────────────────────────────────────────────────────
        // quatType bits 0-1: translation quantization (0=BITS8 → 1 bpc, 1=BITS16 → 2 bpc)
        const int transBpc = ((quatT[tk] & 0x03) == 0) ? 1 : 2;
        VecCurve pos = VecCurve::Rd(r, posT[tk], transBpc, base);
        if (!r.ok) {
            fprintf(stderr, "[SCT] FAIL tk=%d pos p=%02X q=%02X start=%td cur=%td\n",
                    tk, posT[tk], quatT[tk], tkDataStart-base, r.p-base);
            return false;
        }

        // ── 4-byte align before rotation ──────────────────────────────────────
        r.align4(base);

        // ── Rotation ──────────────────────────────────────────────────────────
        glm::quat sR(1.f, 0.f, 0.f, 0.f);
        TC40 rc{}; bool dR = false;
        if (rotT[tk] & 0xF0) {
            rc = TC40::Rd(r);
            if (!r.ok) {
                fprintf(stderr, "[SCT] FAIL tk=%d rot-dyn r=%02X start=%td cur=%td\n",
                        tk, rotT[tk], tkDataStart-base, r.p-base);
                return false;
            }
            dR = true;
        } else if (rotT[tk] != 0) {
            if (r.p + 5 > dataEnd) { return false; }
            sR = DecTC40(r.p); r.skip(5);
        }

        // ── 4-byte align before scale ──────────────────────────────────────────
        r.align4(base);

        // ── Scale ─────────────────────────────────────────────────────────────
        // quatType bits 6-7: scale quantization (0=BITS8, 1=BITS16)
        const int scaleBpc = (((quatT[tk] >> 6) & 0x03) == 0) ? 1 : 2;
        VecCurve::Rd(r, scaleT[tk], scaleBpc, base);  // advance reader; scale unused for now
        if (!r.ok) {
            fprintf(stderr, "[SCT] FAIL tk=%d scale s=%02X q=%02X\n",
                    tk, scaleT[tk], quatT[tk]);
            return false;
        }

        // Advance sequential reader past this track
        if (!haveOffsets) rSeq.p = r.p;

        // ── Write output frames ────────────────────────────────────────────────
        for (int fi = 0; fi < framesInBlock; fi++) {
            const int af = blockStart + fi;
            if (af >= totalFrames) break;
            auto& ch = out[(size_t)af * nTracks + tk];
            ch.localT = pos.Eval(fi);
            ch.localR = dR ? rc.Eval(fi) : sR;
        }
    }
    return true;
}

} // namespace

// ── Public loaders ────────────────────────────────────────────────────────────

static bool ParseAnimationDoc(const pugi::xml_document& doc, const char* name,
                               AnimClip& out, char* errOut, int errLen)
{

    auto section = doc.child("hkpackfile").child("hksection");

    // Detect animation class (interleaved uncompressed or spline compressed)
    pugi::xml_node animNode;
    bool isSpline = false;
    for (auto node : section.children("hkobject")) {
        std::string_view cls = node.attribute("class").as_string();
        if (cls == "hkaInterleavedUncompressedAnimation" ||
            cls == "hkaSplineCompressedAnimation")
        {
            animNode = node;
            isSpline = (cls == "hkaSplineCompressedAnimation");
            break;
        }
    }
    if (!animNode) {
        std::snprintf(errOut, errLen, "No supported animation class found "
                      "(expected hkaInterleavedUncompressedAnimation or hkaSplineCompressedAnimation)");
        return false;
    }

    auto param = [&](const char* n) {
        return animNode.find_child_by_attribute("hkparam", "name", n);
    };

    const float duration  = param("duration").text().as_float();
    const int   numTracks = param("numberOfTransformTracks").text().as_int();
    if (numTracks <= 0) { std::snprintf(errOut, errLen, "Zero transform tracks"); return false; }

    int numFrames = 0;
    std::vector<PoseChannel> transforms;

    if (!isSpline) {
        // ── hkaInterleavedUncompressedAnimation ───────────────────────────────
        auto tfNode = param("transforms");
        if (!tfNode) { std::snprintf(errOut, errLen, "Missing transforms"); return false; }
        const int numelems = tfNode.attribute("numelements").as_int();
        if (numelems <= 0 || numelems % numTracks != 0) {
            std::snprintf(errOut, errLen,
                "transforms numelements=%d not divisible by numTracks=%d", numelems, numTracks);
            return false;
        }
        numFrames = numelems / numTracks;
        transforms.resize(numelems);

        const char* p = tfNode.text().as_string();
        for (int i = 0; i < numelems; i++) {
            float tx, ty, tz, qx, qy, qz, qw, sx, sy, sz;
            p = ParseTuple3(p, tx, ty, tz);
            p = ParseTuple4(p, qx, qy, qz, qw);
            p = ParseTuple3(p, sx, sy, sz);
            transforms[i].localT = { tx, ty, tz };
            transforms[i].localR = glm::quat(qw, qx, qy, qz); // glm: (w,x,y,z)
        }
    } else {
        // ── hkaSplineCompressedAnimation ──────────────────────────────────────
        numFrames = param("numFrames").text().as_int();
        const int numBlocks = param("numBlocks").text().as_int();
        const int maxFpb    = param("maxFramesPerBlock").text().as_int();
        const int nFloat    = param("numberOfFloatTracks").text().as_int(0);

        if (numFrames <= 0 || numBlocks <= 0 || maxFpb <= 1) {
            std::snprintf(errOut, errLen, "Invalid spline animation dimensions");
            return false;
        }

        // blockOffsets: absolute byte positions in data[]
        std::vector<uint32_t> blockOffsets;
        {
            auto n = param("blockOffsets");
            if (!n) { std::snprintf(errOut, errLen, "Missing blockOffsets"); return false; }
            std::istringstream ss(n.text().as_string());
            uint32_t v;
            while (ss >> v) blockOffsets.push_back(v);
        }
        if ((int)blockOffsets.size() < numBlocks) {
            std::snprintf(errOut, errLen, "blockOffsets too short"); return false;
        }

        // data: array of decimal byte values
        std::vector<uint8_t> data;
        {
            auto n = param("data");
            if (!n) { std::snprintf(errOut, errLen, "Missing data"); return false; }
            data.reserve(n.attribute("numelements").as_int(0));
            std::istringstream ss(n.text().as_string());
            int v;
            while (ss >> v) data.push_back(static_cast<uint8_t>(v));
        }

        // maskAndQuantizationSize: actual byte size of the per-block mask section
        const int maskQSize = param("maskAndQuantizationSize").text().as_int(0);

        // transformOffsets: per-track byte offsets from block start, in multiples of 4
        std::vector<uint32_t> transformOffsets;
        {
            auto n = param("transformOffsets");
            if (n) {
                std::istringstream ss(n.text().as_string());
                uint32_t v;
                while (ss >> v) transformOffsets.push_back(v);
            }
        }

        transforms.resize(static_cast<size_t>(numFrames) * numTracks);
        const uint8_t* base = data.data();

        for (int b = 0; b < numBlocks; b++) {
            const int bs = b * (maxFpb - 1);
            const int be = std::min(bs + maxFpb - 1, numFrames - 1);
            if (!DecompBlock(base, data.size(), blockOffsets[b],
                             numTracks, nFloat, maskQSize,
                             transformOffsets, b, bs, be - bs + 1, transforms))
            {
                std::snprintf(errOut, errLen,
                    "Block %d decompression failed (offset=%u, data=%zu B) — "
                    "spline format mismatch", b, blockOffsets[b], data.size());
                return false;
            }
        }
    }

    // ── hkaAnimationBinding — track-to-bone index map ─────────────────────────
    pugi::xml_node bindNode;
    for (auto node : section.children("hkobject"))
        if (std::string_view(node.attribute("class").as_string()) == "hkaAnimationBinding")
            { bindNode = node; break; }

    std::vector<int> trackToBone(numTracks);
    {
        int filled = 0;
        if (bindNode) {
            auto n = bindNode.find_child_by_attribute(
                "hkparam", "name", "transformTrackToBoneIndices");
            if (n) {
                std::istringstream ss(n.text().as_string());
                int v;
                while (ss >> v && filled < numTracks) trackToBone[filled++] = v;
            }
        }

        // Fall back to identity when binding is absent or the index list is empty
        // (hkxcmd emits an empty text node for identity mappings).
        if (filled < numTracks)
            for (int i = 0; i < numTracks; i++) trackToBone[i] = i;
    }

    out.name        = name ? name : "";
    out.duration    = duration;
    out.numTracks   = numTracks;
    out.numFrames   = numFrames;
    out.transforms  = std::move(transforms);
    out.trackToBone = std::move(trackToBone);
    return true;
}

bool LoadHavokAnimationXml(const char* path, AnimClip& out, char* errOut, int errLen)
{
    pugi::xml_document doc;
    auto res = doc.load_file(path);
    if (!res) { std::snprintf(errOut, errLen, "XML: %s", res.description()); return false; }
    std::string stem = std::filesystem::path(path).stem().string();
    return ParseAnimationDoc(doc, stem.c_str(), out, errOut, errLen);
}

bool LoadHavokAnimationXmlFromBuffer(const char* xmlData, int xmlLen,
                                     const char* name,
                                     AnimClip& out, char* errOut, int errLen)
{
    pugi::xml_document doc;
    auto res = doc.load_buffer(xmlData, static_cast<size_t>(xmlLen));
    if (!res) { std::snprintf(errOut, errLen, "XML: %s", res.description()); return false; }
    return ParseAnimationDoc(doc, name, out, errOut, errLen);
}
