#pragma once
#include <string>
#include <unordered_map>

// ── MuFacialExpressionExtended (MFEE) config loader ──────────────────────────
//
// Parses "SKSE/plugins/MuFacialExpressionExtended/performance capture.ini"
// from the game Data folder.  Each enabled "ExtensionFile = lhs, rhs" line
// maps a vanilla expression TRI path to an extended ARKit TRI path.
//
// Both keys and values in the returned map are in canonical form:
//   • lowercase
//   • backslash path separators
//   • relative to Data folder — NO "meshes\" prefix
//
// Usage (AppState):
//   mfeeExtendedTris = LoadMuFeeConfig(dataFolder + "/SKSE/plugins/...");
//
// Lookup (AddActorFromRecord):
//   auto it = mfeeExtendedTris.find(NormalizeMuFeePath(rawTriPath));
//   if (it != mfeeExtendedTris.end()) { /* it->second is the extended path */ }
// ---------------------------------------------------------------------------

// Normalise a TRI path to the canonical MFEE map key form.
// Converts slashes to backslashes, lowercases, and strips any leading
// "meshes\" prefix so plugin-record paths and ini paths match each other.
std::string NormalizeMuFeePath(std::string path);

// Parse the MFEE "performance capture.ini".
// Returns an empty map if the file is absent or unreadable.
std::unordered_map<std::string, std::string>
LoadMuFeeConfig(const std::string& iniPath);
