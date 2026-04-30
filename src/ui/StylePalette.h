#pragma once
#include <imgui.h>

// ── SCT UI colour palette ─────────────────────────────────────────────────────
// Shared ImU32 constants used across Timeline, Inspector, and other panels.
// All colours are in RGBA (IM_COL32 takes R, G, B, A order).

namespace SCT {

// ── Background tiers
inline constexpr ImU32 kColBg        = IM_COL32(13,  14,  17,  255);  // window / track area bg
inline constexpr ImU32 kColRuler     = IM_COL32(20,  22,  28,  255);  // ruler strip
inline constexpr ImU32 kColSep       = IM_COL32(40,  44,  55,  255);  // dividers / borders
inline constexpr ImU32 kColHeader    = IM_COL32(22,  26,  36,  255);  // actor-group headers
inline constexpr ImU32 kColLane      = IM_COL32(16,  18,  24,  255);  // even lanes
inline constexpr ImU32 kColLaneAlt   = IM_COL32(18,  20,  27,  255);  // odd lanes

// ── Scrubber / playhead
inline constexpr ImU32 kColScrubber  = IM_COL32(240, 200,  60,  220);

// ── Text
inline constexpr ImU32 kColText      = IM_COL32(160, 170, 185, 200);
inline constexpr ImU32 kColTextDim   = IM_COL32(100, 110, 125, 160);

} // namespace SCT
