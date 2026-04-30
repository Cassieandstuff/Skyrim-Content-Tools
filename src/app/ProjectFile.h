#pragma once
#include <string>

struct AppState;

// ── ProjectFile ───────────────────────────────────────────────────────────────
// Serialises / deserialises the full SCT scene to a JSON .sct file.
// Stores references (file paths, form keys) — not raw animation data.
// On load, skeletons and clips are re-derived from those references.
struct ProjectFile {
    static bool Save(const std::string& path, const AppState& state,
                     char* errOut, int errLen);

    static bool Load(const std::string& path, AppState& state,
                     char* errOut, int errLen);
};
