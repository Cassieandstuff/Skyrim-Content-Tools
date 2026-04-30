#include "ControllerInput.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <cmath>

// Rescale axis after dead-zone removal so the output still reaches ±1.
static float ApplyDeadzone(float v, float dz) {
    if (std::abs(v) < dz) return 0.f;
    return (v - std::copysign(dz, v)) / (1.f - dz);
}

bool ControllerInput::ApplyFreecam(Camera& cam, float dt) {
    // Find the first connected joystick that has a recognised gamepad mapping.
    int jid = -1;
    for (int i = 0; i <= GLFW_JOYSTICK_LAST; ++i) {
        if (glfwJoystickPresent(i) && glfwJoystickIsGamepad(i)) {
            jid = i;
            break;
        }
    }
    if (jid < 0) return false;

    GLFWgamepadstate gs;
    if (!glfwGetGamepadState(jid, &gs)) return false;

    const float leftX  = ApplyDeadzone(gs.axes[GLFW_GAMEPAD_AXIS_LEFT_X],  deadzone);
    const float leftY  = ApplyDeadzone(gs.axes[GLFW_GAMEPAD_AXIS_LEFT_Y],  deadzone);
    const float rightX = ApplyDeadzone(gs.axes[GLFW_GAMEPAD_AXIS_RIGHT_X], deadzone);
    const float rightY = ApplyDeadzone(gs.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y], deadzone);

    // Triggers: GLFW reports -1 (unpressed) → +1 (fully pressed); normalise to [0, 1].
    const float lt = (gs.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]  + 1.f) * 0.5f;
    const float rt = (gs.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.f) * 0.5f;

    const bool anyInput = leftX || leftY || rightX || rightY || lt > 0.02f || rt > 0.02f;
    if (!anyInput) return true;  // connected but idle

    const float moveSpeed = cam.radius * moveSpeedScale;

    // ── Left stick: walk in the camera's horizontal plane ────────────────────
    // World is Z-up.  Project the camera forward direction onto XY so up-stick
    // always means "walk toward what you're looking at" regardless of elevation.
    // GLFW convention: stick up = negative Y axis value.
    if (leftX != 0.f || leftY != 0.f) {
        const float az = glm::radians(cam.azimuth);
        // forward: direction from eye toward target, projected & normalised in XY.
        const glm::vec3 fwd  = glm::vec3(-std::sin(az), -std::cos(az), 0.f);
        // right: cross(forward, world-up) — west when looking south, east when north, etc.
        const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.f, 0.f, 1.f)));
        cam.target += fwd   * (-leftY) * moveSpeed * dt;
        cam.target += right *   leftX  * moveSpeed * dt;
    }

    // ── Triggers: vertical movement ──────────────────────────────────────────
    const float vert = rt - lt;
    if (std::abs(vert) > 0.02f)
        cam.target.z += vert * moveSpeed * dt;

    // ── Right stick: 1st-person look-around ──────────────────────────────────
    // LookAround() keeps the eye position fixed; only the viewing direction and
    // target change.  Positive rightX = turn right (azimuth increases toward west
    // when looking south).  Negative rightY (stick up) = look up (elevation +).
    if (rightX != 0.f || rightY != 0.f)
        cam.LookAround(rightX * lookSpeed * dt, -rightY * lookSpeed * dt);

    return true;
}
