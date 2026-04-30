#pragma once

// ── Platform ──────────────────────────────────────────────────────────────────
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

// ── STL ───────────────────────────────────────────────────────────────────────
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ── Math ─────────────────────────────────────────────────────────────────────
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

// ── UI ────────────────────────────────────────────────────────────────────────
#include <imgui.h>
#include <imgui_internal.h>

// ── Serialization ─────────────────────────────────────────────────────────────
#include <nlohmann/json.hpp>
