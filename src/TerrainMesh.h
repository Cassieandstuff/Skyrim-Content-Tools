#pragma once
#include "AppState.h"
#include "renderer/ISceneRenderer.h"

// Build a MeshData for one exterior cell's terrain from decoded VHGT/VCLR data.
// The resulting mesh has 33×33 = 1089 vertices and 32×32×2 = 2048 triangles.
// Positions are in Skyrim world space: X = cellX*4096 + col*128, Y = cellY*4096 + row*128.
// Normals are computed from cross-products of adjacent vertices.
// vertexColors is filled from land.colors when land.hasColors; otherwise left empty.
MeshData GenerateTerrainMesh(const LandRecord& land, int cellX, int cellY);
