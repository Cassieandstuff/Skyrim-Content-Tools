#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

struct Camera {
    float     azimuth   = 45.0f;   // horizontal rotation (degrees)
    float     elevation = 25.0f;   // vertical angle above XZ plane (degrees)
    float     radius    = 8.0f;    // distance from target
    glm::vec3 target    = {};

    glm::vec3 Eye() const {
        float az = glm::radians(azimuth);
        float el = glm::radians(elevation);
        // Skyrim / SCT world is Z-up: azimuth rotates in XY plane, elevation lifts along Z.
        return target + glm::vec3(
            radius * std::cos(el) * std::sin(az),
            radius * std::cos(el) * std::cos(az),
            radius * std::sin(el)
        );
    }

    glm::mat4 View() const {
        return glm::lookAt(Eye(), target, glm::vec3(0, 0, 1));
    }

    glm::mat4 Proj(float aspect) const {
        return glm::perspective(glm::radians(45.0f), aspect, 0.1f, 5'000'000.0f);
    }

    void Orbit(float dAz, float dEl) {
        azimuth   += dAz;
        elevation  = std::clamp(elevation + dEl, -89.0f, 89.0f);
    }

    void Zoom(float steps) {
        radius = std::max(0.5f, radius * std::pow(0.9f, steps));
    }

    void Pan(float dx, float dy) {
        glm::vec3 eye     = Eye();
        glm::vec3 forward = glm::normalize(target - eye);
        glm::vec3 right   = glm::normalize(glm::cross(forward, glm::vec3(0, 0, 1)));
        glm::vec3 up      = glm::cross(right, forward);
        float     scale   = radius * 0.001f;
        target -= right * (dx * scale) + up * (dy * scale);
    }

    // Rotate in place: keeps the eye position fixed and moves the target so the
    // camera looks in a new direction.  Used by controller right-stick look.
    void LookAround(float dAz, float dEl) {
        const glm::vec3 eye = Eye();
        azimuth   += dAz;
        elevation  = std::clamp(elevation + dEl, -89.f, 89.f);
        const float az = glm::radians(azimuth);
        const float el = glm::radians(elevation);
        target = eye - glm::vec3(
            radius * std::cos(el) * std::sin(az),
            radius * std::cos(el) * std::cos(az),
            radius * std::sin(el));
    }
};
