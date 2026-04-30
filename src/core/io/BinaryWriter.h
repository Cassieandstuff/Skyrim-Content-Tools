#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ── BinaryWriter ──────────────────────────────────────────────────────────────
// Sequential write cursor into a growable byte buffer.
// Counterpart to BoundsSafeReader — same endianness (little-endian).
// Needed by HkxWriter for binary HKX output.
class BinaryWriter {
public:
    void WriteU8 (uint8_t  v) { append(&v, 1); }
    void WriteU16(uint16_t v) { append(&v, 2); }
    void WriteU32(uint32_t v) { append(&v, 4); }
    void WriteU64(uint64_t v) { append(&v, 8); }
    void WriteI8 (int8_t   v) { append(&v, 1); }
    void WriteI16(int16_t  v) { append(&v, 2); }
    void WriteI32(int32_t  v) { append(&v, 4); }
    void WriteI64(int64_t  v) { append(&v, 8); }
    void WriteF32(float    v) { append(&v, 4); }
    void WriteF64(double   v) { append(&v, 8); }

    void WriteBytes(const void* data, size_t len) { append(data, len); }

    // Null-terminated string (writes length prefix as uint32 then chars + null).
    void WriteString(const std::string& s) {
        auto len = static_cast<uint32_t>(s.size() + 1);
        WriteU32(len);
        append(s.data(), s.size());
        WriteU8(0);
    }

    // Raw null-terminated write (no length prefix).
    void WriteCString(const std::string& s) {
        append(s.data(), s.size());
        WriteU8(0);
    }

    // Pad to the given alignment boundary (from the start of the buffer).
    void Align(size_t align) {
        const size_t rem = buf_.size() % align;
        if (rem) { const size_t pad = align - rem; buf_.resize(buf_.size() + pad, 0); }
    }

    // Current write position.
    size_t Size() const { return buf_.size(); }

    // Access the completed buffer.
    const std::vector<uint8_t>& Buffer() const { return buf_; }
          std::vector<uint8_t>  Release()       { return std::move(buf_); }

private:
    std::vector<uint8_t> buf_;

    void append(const void* data, size_t len) {
        const auto* p = static_cast<const uint8_t*>(data);
        buf_.insert(buf_.end(), p, p + len);
    }
};
