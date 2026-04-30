#include "asset/BsaReader.h"
#include <lz4.h>
#include <algorithm>
#include <cstring>
#include <cstdio>

// ── LZ4 frame decoder ─────────────────────────────────────────────────────────
// Skyrim SE/AE BSA compressed files are LZ4 frame format (magic 04 22 4D 18).
// We parse the frame header and call LZ4_decompress_safe per block, avoiding
// the lz4frame library entirely.
static bool DecompressLZ4Frame(const uint8_t* src, uint32_t srcSize,
                               std::vector<uint8_t>& out,
                               char* errOut, int errLen)
{
    const uint8_t* p   = src;
    const uint8_t* end = src + srcSize;

    auto need = [&](size_t n) -> bool {
        if (p + n > end) {
            std::snprintf(errOut, errLen, "LZ4F: truncated header");
            return false;
        }
        return true;
    };

    // Magic number
    if (!need(4)) return false;
    if (p[0]!=0x04 || p[1]!=0x22 || p[2]!=0x4D || p[3]!=0x18) {
        std::snprintf(errOut, errLen, "LZ4F: bad magic %02X %02X %02X %02X",
                      p[0], p[1], p[2], p[3]);
        return false;
    }
    p += 4;

    // FLG + BD
    if (!need(2)) return false;
    const uint8_t flg = *p++;
    const bool hasCSize  = (flg & 0x08) != 0;
    const bool hasDictID = (flg & 0x01) != 0;
    p++; // BD — skip

    // Optional content size (8 bytes)
    if (hasCSize) { if (!need(8)) return false; p += 8; }
    // Optional dict ID (4 bytes)
    if (hasDictID) { if (!need(4)) return false; p += 4; }
    // Header checksum (1 byte)
    if (!need(1)) return false; p++;

    // Data blocks
    uint8_t* dst    = out.data();
    uint8_t* dstEnd = dst + out.size();
    uint8_t* dstPtr = dst;

    while (p + 4 <= end) {
        uint32_t bsz;
        std::memcpy(&bsz, p, 4); p += 4;
        if (bsz == 0) break; // End Mark

        const bool uncompressed = (bsz & 0x80000000u) != 0;
        bsz &= 0x7FFFFFFFu;

        if (p + bsz > end) {
            std::snprintf(errOut, errLen, "LZ4F: block data overrun");
            return false;
        }

        if (uncompressed) {
            if (dstPtr + bsz > dstEnd) {
                std::snprintf(errOut, errLen, "LZ4F: output overflow");
                return false;
            }
            std::memcpy(dstPtr, p, bsz);
            dstPtr += bsz;
        } else {
            const int dec = LZ4_decompress_safe(
                reinterpret_cast<const char*>(p),
                reinterpret_cast<char*>(dstPtr),
                static_cast<int>(bsz),
                static_cast<int>(dstEnd - dstPtr));
            if (dec < 0) {
                std::snprintf(errOut, errLen, "LZ4F: block decompress error %d", dec);
                return false;
            }
            dstPtr += dec;
        }
        p += bsz;
    }

    out.resize(static_cast<size_t>(dstPtr - dst));
    return true;
}

// ── Binary layout structs (packed — read directly from file) ──────────────────

#pragma pack(push, 1)

struct BsaHeader {
    char     magic[4];            // "BSA\0"
    uint32_t version;             // 104 = SE, 105 = AE
    uint32_t folderOffset;        // always 36 (sizeof BsaHeader)
    uint32_t archiveFlags;        // bit 2 = compressed by default
    uint32_t folderCount;
    uint32_t fileCount;
    uint32_t totalFolderNameLen;
    uint32_t totalFileNameLen;
    uint32_t fileFlags;
};
static_assert(sizeof(BsaHeader) == 36);

// v104 folder record is 24 bytes (8 bytes of padding vs v103's 16-byte record).
struct FolderRecord {
    uint64_t hash;
    uint32_t fileCount;
    uint32_t pad0;
    uint32_t offset;   // byte offset to this folder's name+file-records block
    uint32_t pad1;
};
static_assert(sizeof(FolderRecord) == 24);

struct FileRecord {
    uint64_t hash;
    uint32_t sizeFlags;  // bits 0-29 = size on disk; bit 30 = compression toggle
    uint32_t offset;     // byte offset to file data from start of BSA
};
static_assert(sizeof(FileRecord) == 16);

#pragma pack(pop)

// ── Open ──────────────────────────────────────────────────────────────────────

bool BsaReader::Open(const std::string& path, char* errOut, int errLen)
{
    Close();
    m_file = fopen(path.c_str(), "rb");
    if (!m_file) {
        std::snprintf(errOut, errLen, "Cannot open BSA: %s", path.c_str());
        return false;
    }

    BsaHeader hdr{};
    if (fread(&hdr, sizeof(hdr), 1, m_file) != 1 ||
        std::memcmp(hdr.magic, "BSA\0", 4) != 0) {
        std::snprintf(errOut, errLen, "Not a valid BSA: %s", path.c_str());
        return false;
    }
    if (hdr.version != 104 && hdr.version != 105) {
        std::snprintf(errOut, errLen,
            "Unsupported BSA version %u in %s (SE v104 / AE v105 only)", hdr.version, path.c_str());
        return false;
    }

    const bool defaultCompressed = (hdr.archiveFlags & 0x4u) != 0;
    m_embedFileNames             = (hdr.archiveFlags & 0x100u) != 0;

    // ── Folder records ────────────────────────────────────────────────────────
    std::vector<FolderRecord> folders(hdr.folderCount);
    for (auto& fr : folders) {
        if (fread(&fr, sizeof(fr), 1, m_file) != 1) {
            std::snprintf(errOut, errLen, "BSA folder record read error: %s", path.c_str());
            return false;
        }
    }

    // ── Per-folder: bstring name + file records ───────────────────────────────
    // bstring = 1-byte length (including null) + chars + null terminator
    struct RawFolder { std::string name; std::vector<FileRecord> files; };
    std::vector<RawFolder> rawFolders(hdr.folderCount);

    for (uint32_t fi = 0; fi < hdr.folderCount; fi++) {
        uint8_t nameLen = 0;
        if (fread(&nameLen, 1, 1, m_file) != 1) {
            std::snprintf(errOut, errLen, "BSA folder name length read error: %s", path.c_str());
            return false;
        }
        if (nameLen > 0) {
            std::string name(nameLen, '\0');
            if (fread(name.data(), 1, nameLen, m_file) != nameLen) {
                std::snprintf(errOut, errLen, "BSA folder name read error: %s", path.c_str());
                return false;
            }
            if (name.back() == '\0') name.pop_back();
            rawFolders[fi].name = std::move(name);
        }

        rawFolders[fi].files.resize(folders[fi].fileCount);
        for (auto& rec : rawFolders[fi].files) {
            if (fread(&rec, sizeof(rec), 1, m_file) != 1) {
                std::snprintf(errOut, errLen, "BSA file record read error: %s", path.c_str());
                return false;
            }
        }
    }

    // ── File name block ───────────────────────────────────────────────────────
    std::string nameBlock(hdr.totalFileNameLen, '\0');
    if (hdr.totalFileNameLen > 0 &&
        fread(nameBlock.data(), 1, hdr.totalFileNameLen, m_file) != hdr.totalFileNameLen) {
        std::snprintf(errOut, errLen, "BSA file name block read error: %s", path.c_str());
        return false;
    }

    // ── Build index ───────────────────────────────────────────────────────────
    m_files.reserve(hdr.fileCount);
    const char* namePtr = nameBlock.c_str();

    for (uint32_t fi = 0; fi < hdr.folderCount; fi++) {
        for (const auto& rec : rawFolders[fi].files) {
            BsaFileInfo info;
            info.internalPath = rawFolders[fi].name + '\\' + namePtr;
            // Normalise to lowercase for case-insensitive matching
            std::transform(info.internalPath.begin(), info.internalPath.end(),
                           info.internalPath.begin(), ::tolower);

            const bool toggle  = (rec.sizeFlags & 0x40000000u) != 0;
            info.compressed    = defaultCompressed ^ toggle;
            info.rawSize       = rec.sizeFlags & 0x3FFFFFFFu;
            info.dataOffset    = rec.offset;

            m_files.push_back(std::move(info));
            namePtr += std::strlen(namePtr) + 1;
        }
    }

    return true;
}

// ── Close ─────────────────────────────────────────────────────────────────────

void BsaReader::Close()
{
    if (m_file) { fclose(m_file); m_file = nullptr; }
    m_files.clear();
}

// ── Find / FindExact ──────────────────────────────────────────────────────────

std::vector<const BsaFileInfo*> BsaReader::Find(const std::string& substr) const
{
    std::vector<const BsaFileInfo*> out;
    for (const auto& f : m_files)
        if (f.internalPath.find(substr) != std::string::npos)
            out.push_back(&f);
    return out;
}

const BsaFileInfo* BsaReader::FindExact(const std::string& internalPath) const
{
    for (const auto& f : m_files)
        if (f.internalPath == internalPath)
            return &f;
    return nullptr;
}

// ── Extract ───────────────────────────────────────────────────────────────────

bool BsaReader::Extract(const BsaFileInfo& info,
                        std::vector<uint8_t>& outBuf,
                        char* errOut, int errLen) const
{
    if (!m_file) { std::snprintf(errOut, errLen, "BSA not open"); return false; }

    if (_fseeki64(m_file, info.dataOffset, SEEK_SET) != 0) {
        std::snprintf(errOut, errLen, "BSA seek error for %s", info.internalPath.c_str());
        return false;
    }

    // When the Embed File Names flag is set (archiveFlags & 0x100), each file's
    // data block is preceded by a bstring (1-byte length + N bytes, no null)
    // containing the file path.  Skip it before reading actual data.
    uint32_t embeddedOverhead = 0;
    if (m_embedFileNames) {
        uint8_t nameLen = 0;
        if (fread(&nameLen, 1, 1, m_file) != 1) {
            std::snprintf(errOut, errLen, "BSA: failed to read embedded name length for %s",
                          info.internalPath.c_str());
            return false;
        }
        if (nameLen > 0 && _fseeki64(m_file, nameLen, SEEK_CUR) != 0) {
            std::snprintf(errOut, errLen, "BSA: failed to skip embedded name for %s",
                          info.internalPath.c_str());
            return false;
        }
        embeddedOverhead = 1u + nameLen;
    }

    const uint32_t dataSize = info.rawSize - embeddedOverhead;

    if (!info.compressed) {
        outBuf.resize(dataSize);
        if (fread(outBuf.data(), 1, dataSize, m_file) != dataSize) {
            std::snprintf(errOut, errLen, "BSA read error for %s", info.internalPath.c_str());
            return false;
        }
        return true;
    }

    // Compressed: 4-byte original size + LZ4 frame data (magic 04 22 4D 18)
    uint32_t origSize = 0;
    if (fread(&origSize, sizeof(origSize), 1, m_file) != 1) {
        std::snprintf(errOut, errLen, "BSA: failed to read orig size for %s",
                      info.internalPath.c_str());
        return false;
    }
    const uint32_t compSize = dataSize - sizeof(uint32_t);

    std::vector<uint8_t> compBuf(compSize);
    if (fread(compBuf.data(), 1, compSize, m_file) != compSize) {
        std::snprintf(errOut, errLen, "BSA: compressed read error for %s",
                      info.internalPath.c_str());
        return false;
    }

    outBuf.resize(origSize);
    return DecompressLZ4Frame(compBuf.data(), compSize, outBuf, errOut, errLen);
}
