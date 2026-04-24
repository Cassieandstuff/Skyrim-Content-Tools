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
        return target + glm::vec3(
            radius * std::cos(el) * std::sin(az),
            radius * std::sin(el),
            radius * std::cos(el) * std::cos(az)
        );
    }

    glm::mat4 View() const {
        return glm::lookAt(Eye(), target, glm::vec3(0, 1, 0));
    }

    glm::mat4 Proj(float aspect) const {
        return glm::perspective(glm::radians(45.0f), aspect, 0.1f, 500.0f);
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
        glm::vec3 right   = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
        glm::vec3 up      = glm::cross(right, forward);
        float     scale   = radius * 0.001f;
        target -= right * (dx * scale) + up * (dy * scale);
    }
};
