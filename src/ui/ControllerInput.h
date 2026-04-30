#pragma once
#include "ui/Camera.h"

// Polls GLFW gamepad state and applies Skyrim-style freecam movement.
//
// Mapping:
//   Left  stick X/Y  — strafe / forward-back in camera's horizontal plane
//   Right stick X/Y  — 1st-person look (eye stays fixed, only direction changes)
//   Left  trigger    — descend (move camera down in Z)
//   Right trigger    — ascend  (move camera up in Z)
//
// Call ApplyFreecam() once per frame from ViewportPanel::Draw().
struct ControllerInput {
    // Move speed = camera.radius * moveSpeedScale  (units / sec at full deflection).
    float moveSpeedScale = 0.5f;
    // Degrees per second at full right-stick deflection.
    float lookSpeed      = 90.f;
    // Stick dead-zone radius (raw axis values below this are zeroed).
    float deadzone       = 0.15f;

    // Apply controller state to cam.  Returns true if a gamepad is connected.
    bool ApplyFreecam(Camera& cam, float dt);
};
