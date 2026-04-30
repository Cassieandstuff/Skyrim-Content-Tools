#include "asset/MuFeeConfig.h"
#include <fstream>
#include <algorithm>
#include <cstdio>

// ── NormalizeMuFeePath ─────────────────────────────────────────────────────────

std::string NormalizeMuFeePath(std::string path)
{
    // Lowercase + convert slashes to backslashes.
    for (char& c : path) {
        if (c == '/') c = '\\';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    // Strip leading "meshes\" prefix if present (plugin records may include it;
    // the MFEE ini never does — normalise to the no-prefix form for both sides).
    if (path.rfind("meshes\\", 0) == 0)
        path = path.substr(7);  // len("meshes\") == 7
    return path;
}

// ── LoadMuFeeConfig ────────────────────────────────────────────────────────────

static std::string Trim(std::string s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
    return s;
}

std::unordered_map<std::string, std::string>
LoadMuFeeConfig(const std::string& iniPath)
{
    std::unordered_map<std::string, std::string> result;

    std::ifstream f(iniPath);
    if (!f) return result;

    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        // Strip trailing CR / whitespace.
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == '\n' ||
                line.back() == ' '  || line.back() == '\t'))
            line.pop_back();

        // Skip blank lines and comment lines.
        if (line.empty() || line[0] == '#') continue;

        // Expect "ExtensionFile = lhs, rhs"  (key is case-insensitive)
        static constexpr std::string_view kPrefix = "ExtensionFile";
        if (line.size() < kPrefix.size()) continue;

        {
            std::string lineStart = line.substr(0, kPrefix.size());
            std::transform(lineStart.begin(), lineStart.end(),
                           lineStart.begin(), ::tolower);
            if (lineStart != "extensionfile") continue;
        }

        const auto eqPos = line.find('=', kPrefix.size());
        if (eqPos == std::string::npos) continue;

        const std::string valPart = line.substr(eqPos + 1);
        const auto commaPos = valPart.find(',');
        if (commaPos == std::string::npos) continue;

        std::string lhs = Trim(valPart.substr(0, commaPos));
        std::string rhs = Trim(valPart.substr(commaPos + 1));
        if (lhs.empty() || rhs.empty()) continue;

        result[NormalizeMuFeePath(lhs)] = NormalizeMuFeePath(rhs);
    }

    fprintf(stderr, "[MFEE] Loaded %d TRI mapping(s) from '%s'\n",
            (int)result.size(), iniPath.c_str());
    return result;
}
