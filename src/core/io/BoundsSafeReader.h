#pragma once
#include <cstdint>
#include <cstring>

// ── BoundsSafeReader ──────────────────────────────────────────────────────────
// Thin wrapper around a [p, end) byte range for parsing binary formats.
// Every read that would exceed `end` sets ok=false and returns a zero value.
// Callers check ok after reading a section to bail out gracefully.
//
// align4: advances p to the next 4-byte boundary relative to `base` (usually
//         the start of the enclosing block or file).

struct BoundsSafeReader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok;

    BoundsSafeReader(const uint8_t* start, const uint8_t* e)
        : p(start), end(e), ok(start <= e) {}

    float F32() {
        if (p + 4 > end) { ok = false; return 0.f; }
        float v; std::memcpy(&v, p, 4); p += 4; return v;
    }
    uint16_t U16() {
        if (p + 2 > end) { ok = false; return 0; }
        uint16_t v; std::memcpy(&v, p, 2); p += 2; return v;
    }
    uint8_t U8() {
        if (p >= end) { ok = false; return 0; }
        return *p++;
    }
    void skip(int n) {
        if (n < 0 || p + n > end) { ok = false; p = end; return; }
        p += n;
    }
    void align4(const uint8_t* base) {
        uintptr_t off     = static_cast<uintptr_t>(p - base);
        const uint8_t* aligned = base + ((off + 3u) & ~3u);
        if (aligned > end) { ok = false; p = end; return; }
        p = aligned;
    }
};
