#include "asset/DdsLoader.h"

#include <glad/gl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// GL extension enum values not present in the minimal glad loader.
// Values are stable across all vendors/drivers.
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT   0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT  0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT  0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT  0x83F3
#endif
#ifndef GL_COMPRESSED_RED_RGTC1
#define GL_COMPRESSED_RED_RGTC1           0x8DBB
#endif
#ifndef GL_COMPRESSED_RG_RGTC2
#define GL_COMPRESSED_RG_RGTC2            0x8DBD
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM     0x8E8C
#endif

// ── DDS file structures ───────────────────────────────────────────────────────

static constexpr uint32_t kDdsMagic     = 0x20534444u; // "DDS "
static constexpr uint32_t kFourCC_DXT1  = 0x31545844u; // "DXT1"
static constexpr uint32_t kFourCC_DXT3  = 0x33545844u; // "DXT3"
static constexpr uint32_t kFourCC_DXT5  = 0x35545844u; // "DXT5"
static constexpr uint32_t kFourCC_ATI2  = 0x32495441u; // "ATI2" = BC5
static constexpr uint32_t kFourCC_BC4U  = 0x55344342u; // "BC4U"
static constexpr uint32_t kFourCC_BC5U  = 0x55354342u; // "BC5U"
static constexpr uint32_t kFourCC_DX10  = 0x30315844u; // "DX10"

static constexpr uint32_t kDDPF_FOURCC  = 0x00000004u;
static constexpr uint32_t kDDPF_RGB     = 0x00000040u;
static constexpr uint32_t kDDPF_RGBA    = 0x00000041u;

// DX10 DXGI formats we care about.
static constexpr uint32_t kDXGI_BC1     =  71u;
static constexpr uint32_t kDXGI_BC1_SRGB=  72u;
static constexpr uint32_t kDXGI_BC3     =  77u;
static constexpr uint32_t kDXGI_BC3_SRGB=  78u;
static constexpr uint32_t kDXGI_BC4     =  80u; // unsigned
static constexpr uint32_t kDXGI_BC5     =  83u; // unsigned
static constexpr uint32_t kDXGI_BC7     =  98u;
static constexpr uint32_t kDXGI_BC7_SRGB=  99u;
static constexpr uint32_t kDXGI_B8G8R8A8= 87u;
static constexpr uint32_t kDXGI_R8G8B8A8= 28u;

#pragma pack(push, 1)
struct DdsPixelFormat {
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t rgbBitCount;
    uint32_t rBitMask, gBitMask, bBitMask, aBitMask;
};

struct DdsHeader {
    uint32_t       size;
    uint32_t       flags;
    uint32_t       height;
    uint32_t       width;
    uint32_t       pitchOrLinearSize;
    uint32_t       depth;
    uint32_t       mipMapCount;
    uint32_t       reserved1[11];
    DdsPixelFormat ddspf;
    uint32_t       caps;
    uint32_t       caps2;
    uint32_t       caps3;
    uint32_t       caps4;
    uint32_t       reserved2;
};

struct DdsHeaderDX10 {
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};
#pragma pack(pop)

// ── Format resolution ─────────────────────────────────────────────────────────

struct TexFormat {
    GLenum   internalFormat;  // e.g. GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
    GLenum   baseFormat;      // GL_RGBA / GL_RG / GL_RED (for uncompressed path)
    GLenum   dataType;        // GL_UNSIGNED_BYTE (uncompressed only)
    bool     compressed;
    uint32_t blockSize;       // bytes per 4×4 block (compressed) or bytes/pixel (uncompressed)
};

static bool ResolveFormatFromDX10(uint32_t dxgi, TexFormat& out)
{
    switch (dxgi) {
    case kDXGI_BC1:
    case kDXGI_BC1_SRGB:
        out = { GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, true, 8 };  return true;
    case kDXGI_BC3:
    case kDXGI_BC3_SRGB:
        out = { GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, true, 16 }; return true;
    case kDXGI_BC4:
        out = { GL_COMPRESSED_RED_RGTC1,          0, 0, true, 8 };  return true;
    case kDXGI_BC5:
        out = { GL_COMPRESSED_RG_RGTC2,           0, 0, true, 16 }; return true;
    case kDXGI_BC7:
    case kDXGI_BC7_SRGB:
        out = { GL_COMPRESSED_RGBA_BPTC_UNORM,    0, 0, true, 16 }; return true;
    case kDXGI_B8G8R8A8:
        out = { GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, false, 4 }; return true;
    case kDXGI_R8G8B8A8:
        out = { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, false, 4 }; return true;
    default:
        return false;
    }
}

static bool ResolveFormatFromLegacy(const DdsPixelFormat& pf, TexFormat& out)
{
    if (pf.flags & kDDPF_FOURCC) {
        switch (pf.fourCC) {
        case kFourCC_DXT1:
            out = { GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, true, 8 };  return true;
        case kFourCC_DXT3:
            out = { GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, true, 16 }; return true;
        case kFourCC_DXT5:
            out = { GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, true, 16 }; return true;
        case kFourCC_ATI2:
        case kFourCC_BC5U:
            out = { GL_COMPRESSED_RG_RGTC2,           0, 0, true, 16 }; return true;
        case kFourCC_BC4U:
            out = { GL_COMPRESSED_RED_RGTC1,          0, 0, true, 8 };  return true;
        default:
            return false;
        }
    }
    // Uncompressed RGB(A) — handle 32-bit BGRA/RGBA.
    if ((pf.flags & kDDPF_RGB) && pf.rgbBitCount == 32) {
        if (pf.bBitMask == 0x000000FFu)
            out = { GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, false, 4 };
        else
            out = { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, false, 4 };
        return true;
    }
    return false;
}

// ── Shared implementation (operates on raw bytes already in memory) ──────────

static unsigned int DdsUploadFromMemory(const uint8_t* data, size_t size,
                                        const char* logSrc)
{
    if (size < 4 + sizeof(DdsHeader)) return 0;

    size_t cursor = 0;
    auto readBytes = [&](void* dst, size_t n) -> bool {
        if (cursor + n > size) return false;
        std::memcpy(dst, data + cursor, n);
        cursor += n;
        return true;
    };

    uint32_t magic = 0;
    if (!readBytes(&magic, 4) || magic != kDdsMagic) return 0;

    DdsHeader hdr;
    if (!readBytes(&hdr, sizeof(hdr))) return 0;

    const uint32_t w        = hdr.width;
    const uint32_t h        = hdr.height;
    const uint32_t mipCount = (hdr.mipMapCount > 0) ? hdr.mipMapCount : 1;

    TexFormat fmt;
    if ((hdr.ddspf.flags & kDDPF_FOURCC) && hdr.ddspf.fourCC == kFourCC_DX10) {
        DdsHeaderDX10 dx10;
        if (!readBytes(&dx10, sizeof(dx10))) return 0;
        if (!ResolveFormatFromDX10(dx10.dxgiFormat, fmt)) {
            fprintf(stderr, "[DDS] Unsupported DXGI format %u in '%s'\n",
                    dx10.dxgiFormat, logSrc);
            return 0;
        }
    } else {
        if (!ResolveFormatFromLegacy(hdr.ddspf, fmt)) {
            fprintf(stderr, "[DDS] Unsupported legacy format in '%s'\n", logSrc);
            return 0;
        }
    }

    unsigned int texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (int)mipCount - 1);
    // TEST: force GL_LINEAR to diagnose mipmap alpha washout on alpha-tested meshes.
    // If railings become visible at distance, mip averaging is discarding alpha-tested
    // pixels — restore GL_LINEAR_MIPMAP_LINEAR and fix properly (mip bias / threshold).
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    uint32_t mw = w, mh = h;
    bool ok = true;
    for (uint32_t mip = 0; mip < mipCount && ok; mip++) {
        const uint32_t bw = (mw + 3) / 4;
        const uint32_t bh = (mh + 3) / 4;

        GLsizei dataSize;
        if (fmt.compressed)
            dataSize = (GLsizei)(bw * bh * fmt.blockSize);
        else
            dataSize = (GLsizei)(mw * mh * fmt.blockSize);

        if (cursor + (size_t)dataSize > size) { ok = false; break; }
        const uint8_t* mipData = data + cursor;
        cursor += (size_t)dataSize;

        if (fmt.compressed) {
            glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)mip,
                                   fmt.internalFormat,
                                   (GLsizei)mw, (GLsizei)mh, 0,
                                   dataSize, mipData);
        } else {
            glTexImage2D(GL_TEXTURE_2D, (GLint)mip,
                         (GLint)fmt.internalFormat,
                         (GLsizei)mw, (GLsizei)mh, 0,
                         fmt.baseFormat, fmt.dataType, mipData);
        }

        const GLenum err = glGetError();
        if (err != GL_NO_ERROR)
            fprintf(stderr, "[DDS] GL error 0x%X uploading mip %u of '%s'\n",
                    err, mip, logSrc);

        mw = (mw > 1) ? mw / 2 : 1;
        mh = (mh > 1) ? mh / 2 : 1;
    }

    if (!ok) { glDeleteTextures(1, &texId); return 0; }
    return texId;
}

// ── Public entry points ───────────────────────────────────────────────────────

unsigned int DdsLoadGLTexture(const std::string& path)
{
    FILE* f = nullptr;
#if defined(_WIN32)
    fopen_s(&f, path.c_str(), "rb");
#else
    f = fopen(path.c_str(), "rb");
#endif
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long fileLen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileLen <= 0) { fclose(f); return 0; }

    std::vector<uint8_t> buf((size_t)fileLen);
    if (fread(buf.data(), 1, (size_t)fileLen, f) != (size_t)fileLen) {
        fclose(f); return 0;
    }
    fclose(f);

    return DdsUploadFromMemory(buf.data(), buf.size(), path.c_str());
}

unsigned int DdsLoadGLTextureFromBuffer(const uint8_t* data, size_t size)
{
    return DdsUploadFromMemory(data, size, "<buffer>");
}
