#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// ── BsaReader ─────────────────────────────────────────────────────────────────
// Read-only parser for Skyrim SE/AE BSA archives (version 104, LZ4 compression).
// LE (v103 / zlib) is intentionally unsupported.
//
// Usage:
//   BsaReader bsa;
//   if (!bsa.Open(path, err, sizeof(err))) ...
//   for (const auto* fi : bsa.Find("skeleton"))   // substring on lowercase path
//       bsa.Extract(*fi, buf, err, sizeof(err));
// ─────────────────────────────────────────────────────────────────────────────

struct BsaFileInfo {
    std::string internalPath;  // lowercase backslash path: "meshes\actors\character\..."
    uint32_t    dataOffset = 0;
    uint32_t    rawSize    = 0;  // bytes on disk (includes 4-byte origSize header if compressed)
    bool        compressed = false;
};

class BsaReader {
public:
    BsaReader()  = default;
    ~BsaReader() { Close(); }

    BsaReader(const BsaReader&)            = delete;
    BsaReader& operator=(const BsaReader&) = delete;

    // Open and index the BSA.  Returns false and writes a message on failure.
    bool Open(const std::string& bsaPath, char* errOut, int errLen);
    void Close();
    bool IsOpen() const { return m_file != nullptr; }

    // All files whose internalPath contains `substr` (case-sensitive on the
    // already-lowercased paths, so pass lowercase substrings).
    std::vector<const BsaFileInfo*> Find(const std::string& substr) const;

    // Exact lookup by full lowercase internal path.  Returns nullptr if absent.
    const BsaFileInfo* FindExact(const std::string& internalPath) const;

    // Decompress (if needed) and return raw file bytes.
    bool Extract(const BsaFileInfo& info,
                 std::vector<uint8_t>& outBuf,
                 char* errOut, int errLen) const;

private:
    FILE*                    m_file = nullptr;
    std::vector<BsaFileInfo> m_files;
    bool                     m_embedFileNames = false; // archiveFlags & 0x100
};
