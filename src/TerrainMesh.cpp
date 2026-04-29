#include "TerrainMesh.h"
#include <glm/glm.hpp>
#include <cmath>
#include <algorithm>

MeshData GenerateTerrainMesh(const LandRecord& land, int cellX, int cellY)
{
    // Skyrim exterior cell: 33×33 vertex grid, 128-unit step in X and Y.
    // Origin of vertex (0,0) is at (cellX*4096, cellY*4096) in world space.
    constexpr int    N    = 33;
    constexpr float  STEP = 128.f;
    const float      ox   = (float)cellX * 4096.f;
    const float      oy   = (float)cellY * 4096.f;

    MeshData md;
    md.positions.reserve(N * N);
    md.normals.reserve(N * N);
    md.uvs.reserve(N * N);
    if (land.hasColors)
        md.vertexColors.reserve(N * N);

    // Build positions
    for (int row = 0; row < N; row++) {
        for (int col = 0; col < N; col++) {
            md.positions.push_back({
                ox + col * STEP,
                oy + row * STEP,
                land.heights[row][col]
            });
            md.uvs.push_back({
                (float)col / (float)(N - 1),
                (float)row / (float)(N - 1)
            });
            if (land.hasColors) {
                md.vertexColors.push_back({
                    land.colors[row][col][0],
                    land.colors[row][col][1],
                    land.colors[row][col][2]
                });
            }
        }
    }

    // Compute normals from finite differences of surrounding heights.
    // For an interior vertex (row,col):
    //   dh/dx ≈ (h[row][col+1] - h[row][col-1]) / (2 * STEP)
    //   dh/dy ≈ (h[row+1][col] - h[row-1][col]) / (2 * STEP)
    //   normal = normalize(-dhdx, -dhdy, 1) in XYZ (Skyrim Z-up)
    auto h = [&](int r, int c) -> float {
        r = std::clamp(r, 0, N - 1);
        c = std::clamp(c, 0, N - 1);
        return land.heights[r][c];
    };

    for (int row = 0; row < N; row++) {
        for (int col = 0; col < N; col++) {
            const float dhdx = (h(row, col + 1) - h(row, col - 1)) / (2.f * STEP);
            const float dhdy = (h(row + 1, col) - h(row - 1, col)) / (2.f * STEP);
            md.normals.push_back(glm::normalize(glm::vec3(-dhdx, -dhdy, 1.f)));
        }
    }

    // Build indices: two triangles per quad, counter-clockwise winding (GL front-face).
    // Quad at (row, col) uses vertices at (row,col), (row+1,col), (row,col+1), (row+1,col+1).
    md.indices.reserve((N - 1) * (N - 1) * 6);
    for (int row = 0; row < N - 1; row++) {
        for (int col = 0; col < N - 1; col++) {
            const uint16_t bl = (uint16_t)(row       * N + col);
            const uint16_t br = (uint16_t)(row       * N + col + 1);
            const uint16_t tl = (uint16_t)((row + 1) * N + col);
            const uint16_t tr = (uint16_t)((row + 1) * N + col + 1);
            // Triangle 1: bl, br, tl
            md.indices.push_back(bl);
            md.indices.push_back(br);
            md.indices.push_back(tl);
            // Triangle 2: br, tr, tl
            md.indices.push_back(br);
            md.indices.push_back(tr);
            md.indices.push_back(tl);
        }
    }

    return md;
}
