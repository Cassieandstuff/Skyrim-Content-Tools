#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

// ── Interpolation ─────────────────────────────────────────────────────────────
// Central home for all curve and blending math in SCT.
//
// Linear / slerp wrappers give call sites a single namespace to reach for
// instead of mixing glm::mix, glm::slerp, and hand-rolled lerp.
//
// Bezier and Hermite are forward-direction only (given t → value).  The
// inverse (given value → t, needed for a curve editor time axis) will be added
// alongside TinySpline when the curve editor feature is built.

namespace Interp {

// ── Linear ────────────────────────────────────────────────────────────────────

inline float Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

inline glm::vec3 Lerp(glm::vec3 a, glm::vec3 b, float t)
{
    return glm::mix(a, b, t);
}

inline glm::quat Slerp(glm::quat a, glm::quat b, float t)
{
    return glm::slerp(a, b, t);
}

// ── Easing ────────────────────────────────────────────────────────────────────
// All functions expect t in [0, 1] and return [0, 1].

// Smooth cubic: slow at both ends, fast in the middle (Ken Perlin's smoothstep).
inline float SmoothStep(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// Accelerate from rest.
inline float EaseIn(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return t * t;
}

// Decelerate to rest.
inline float EaseOut(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return 1.f - (1.f - t) * (1.f - t);
}

// Accelerate then decelerate (C1 continuous cubic).
inline float EaseInOut(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return t < 0.5f ? 2.f * t * t
                    : 1.f - 2.f * (1.f - t) * (1.f - t);
}

// ── Timeline blend ramp ───────────────────────────────────────────────────────
// Returns the blend weight [0, 1] for a SequenceItem at playhead position t.
// Weight ramps linearly from 0 → 1 over blendIn seconds at the head and
// 1 → 0 over blendOut seconds at the tail.  A value of 0 for either ramp
// means "no ramp" (weight stays at 1 for that end).

inline float BlendWeight(float t, float seqStart, float seqEnd,
                         float blendIn, float blendOut)
{
    float w = 1.f;
    if (blendIn  > 0.f) w = std::min(w, (t - seqStart) / blendIn);
    if (blendOut > 0.f) w = std::min(w, (seqEnd - t)   / blendOut);
    return std::clamp(w, 0.f, 1.f);
}

// ── Cubic Bezier ─────────────────────────────────────────────────────────────
// Evaluates a cubic Bezier curve at parameter t ∈ [0, 1].
// p0/p3 are endpoints; p1/p2 are control handles.
//
// NOTE: this is the forward direction (t → value).  To evaluate at a specific
// time coordinate on a curve editor's X axis, you first need to invert the
// time bezier to find t — that will be handled by TinySpline when the curve
// editor is built.

inline float CubicBezier(float p0, float p1, float p2, float p3, float t)
{
    const float u  = 1.f - t;
    const float uu = u * u;
    const float tt = t * t;
    return uu*u*p0 + 3.f*uu*t*p1 + 3.f*u*tt*p2 + tt*t*p3;
}

inline glm::vec3 CubicBezier(glm::vec3 p0, glm::vec3 p1,
                              glm::vec3 p2, glm::vec3 p3, float t)
{
    const float u  = 1.f - t;
    const float uu = u * u;
    const float tt = t * t;
    return uu*u*p0 + 3.f*uu*t*p1 + 3.f*u*tt*p2 + tt*t*p3;
}

// ── Cubic Hermite ─────────────────────────────────────────────────────────────
// Interpolates between p0 and p1 using incoming tangent m0 and outgoing
// tangent m1 at t ∈ [0, 1].
//
// This is the standard form used by Maya, Blender, and most DCC tools for
// animation curve keyframes (tcb/hermite keys).  Tangents are in value/time
// units, not normalised — scale them to match the time interval before calling.

inline float Hermite(float p0, float m0, float p1, float m1, float t)
{
    const float t2  = t * t;
    const float t3  = t2 * t;
    return ( 2.f*t3 - 3.f*t2 + 1.f) * p0
         + ( t3     - 2.f*t2 + t)   * m0
         + (-2.f*t3 + 3.f*t2)       * p1
         + ( t3     - t2)           * m1;
}

inline glm::vec3 Hermite(glm::vec3 p0, glm::vec3 m0,
                          glm::vec3 p1, glm::vec3 m1, float t)
{
    const float t2  = t * t;
    const float t3  = t2 * t;
    const float h00 =  2.f*t3 - 3.f*t2 + 1.f;
    const float h10 =  t3     - 2.f*t2 + t;
    const float h01 = -2.f*t3 + 3.f*t2;
    const float h11 =  t3     - t2;
    return h00*p0 + h10*m0 + h01*p1 + h11*m1;
}

} // namespace Interp
