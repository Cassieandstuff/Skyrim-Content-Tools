#pragma once

// ── GlSceneRenderer GLSL sources ──────────────────────────────────────────────
// All shader strings are versioned 450 core (GL 4.5 / OpenGL 4.5 core profile).
// Keep paired shaders adjacent; note which fragment shader each vertex shader
// is paired with.

// ── Primitive: coloured lines and points ─────────────────────────────────────
// Paired: kPrimFS

static const char* kPrimVS = R"(#version 450 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col;
uniform mat4 u_vp;
out vec4 v_col;
void main() {
    gl_Position  = u_vp * vec4(a_pos, 1.0);
    gl_PointSize = 5.0;
    v_col = a_col;
}
)";

static const char* kPrimFS = R"(#version 450 core
in  vec4 v_col;
out vec4 f_col;
void main() { f_col = v_col; }
)";

// ── Static mesh: Blinn-Phong + controllable scene light ───────────────────────
// Paired with kMeshFS. Also used by kSkinnedVS (shared fragment shader).
//
// u_alphaMode: 0=opaque 1=alphatest 2=alphablend 3=additive 4=alphatestblend
// u_useVtxColor: 1 = use v_vtxCol (normalised to 0-1) * tint as base, overrides hasTex
// u_terrainTex: 1 = terrain mode — tile diffuse by u_terrainTile, multiply by vtxcol*2

static const char* kMeshVS = R"(#version 450 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_norm;
layout(location = 2) in vec2 a_uv;
layout(location = 5) in vec3 a_vtxCol;
uniform mat4 u_mvp;
uniform mat4 u_model;
out vec3 v_norm;
out vec3 v_worldPos;
out vec2 v_uv;
out vec3 v_vtxCol;
void main() {
    vec4 wp     = u_model * vec4(a_pos, 1.0);
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    v_worldPos  = wp.xyz;
    v_norm   = mat3(transpose(inverse(u_model))) * a_norm;
    v_uv     = a_uv;
    v_vtxCol = a_vtxCol;
}
)";

static const char* kMeshFS = R"(#version 450 core
in  vec3 v_norm;
in  vec3 v_worldPos;
in  vec2 v_uv;
in  vec3 v_vtxCol;
uniform vec4      u_tint;
uniform int       u_hasTex;
uniform int       u_useVtxColor;     // 1 = use per-vertex colour as base
uniform int       u_terrainTex;      // 1 = terrain: tiled diffuse * vtxcol*2
uniform float     u_terrainTile;     // UV tiling scale for terrain mode
uniform sampler2D u_diffuse;
uniform vec3      u_lightDir;
uniform vec3      u_lightColor;
uniform vec3      u_ambientColor;
uniform vec3      u_camPos;
uniform int       u_alphaMode;       // 0=opaque 1=alphatest 2=alphablend 3=additive 4=alphatestblend
uniform float     u_alphaThreshold;  // normalised discard threshold (alphatest/alphatestblend)
out vec4 f_col;
void main() {
    vec3 n = normalize(v_norm);

    vec4 base;
    if (u_terrainTex == 1) {
        vec4 tex = texture(u_diffuse, v_uv * u_terrainTile);
        base = (u_useVtxColor == 1) ? tex * vec4(v_vtxCol * 2.0, 1.0) : tex;
    }
    else if (u_useVtxColor == 1)
        base = vec4(v_vtxCol, 1.0) * u_tint;
    else if (u_hasTex == 1)
        base = texture(u_diffuse, v_uv);
    else
        base = u_tint;

    if (u_alphaMode == 0 && base.a < 0.01) base = vec4(0.72, 0.72, 0.76, 1.0);
    if ((u_alphaMode == 1 || u_alphaMode == 4) && base.a < u_alphaThreshold) discard;

    float diff = max(dot(n, u_lightDir), 0.0);

    vec3 V    = normalize(u_camPos - v_worldPos);
    vec3 H    = normalize(u_lightDir + V);
    float spec = pow(max(dot(n, H), 0.0), 32.0) * diff * 0.15;

    vec3 lit = u_ambientColor * base.rgb
             + u_lightColor   * base.rgb * diff
             + u_lightColor   * spec;

    f_col = vec4(lit, base.a);
}
)";

// ── Skinned mesh: bone transforms via SSBO at binding 0 ──────────────────────
// Paired with kMeshFS (shared fragment shader).

static const char* kSkinnedVS = R"(#version 450 core
layout(location = 0) in vec3  a_pos;
layout(location = 1) in vec3  a_norm;
layout(location = 2) in vec2  a_uv;
layout(location = 3) in uvec4 a_boneIdx;
layout(location = 4) in vec4  a_boneWt;
layout(std430, binding = 0) readonly buffer BoneBlock { mat4 u_bones[]; };
uniform mat4 u_vp;
uniform mat4 u_model;
out vec3 v_norm;
out vec3 v_worldPos;
out vec2 v_uv;
out vec3 v_vtxCol;
void main() {
    mat4 skin = a_boneWt.x * u_bones[a_boneIdx.x]
              + a_boneWt.y * u_bones[a_boneIdx.y]
              + a_boneWt.z * u_bones[a_boneIdx.z]
              + a_boneWt.w * u_bones[a_boneIdx.w];
    vec4 wp     = u_model * skin * vec4(a_pos, 1.0);
    gl_Position = u_vp * wp;
    v_worldPos  = wp.xyz;
    v_norm   = mat3(transpose(inverse(u_model))) * mat3(skin) * a_norm;
    v_uv     = a_uv;
    v_vtxCol = vec3(1.0);  // skinned meshes don't use vertex colour
}
)";

// ── Terrain: 6-layer VTXT blend + VCLR modulation ────────────────────────────
// u_layer[0] = base BTXT diffuse.  u_layer[1-5] = ATXT diffuse textures.
// u_blend[0-4] = 33×33 float blend maps (GL_R32F) for alpha layers 1-5.
// UVs: v_uv (0-1 cell-normalised) used for both blend maps and tiled diffuse.
// Paired: kTerrainFS

static const char* kTerrainVS = R"(#version 450 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_norm;
layout(location = 2) in vec2 a_uv;
layout(location = 5) in vec3 a_vtxCol;
uniform mat4 u_vp;
out vec3 v_norm;
out vec3 v_worldPos;
out vec2 v_uv;
out vec3 v_vtxCol;
void main() {
    gl_Position = u_vp * vec4(a_pos, 1.0);
    v_worldPos  = a_pos;
    v_norm      = a_norm;
    v_uv        = a_uv;
    v_vtxCol    = a_vtxCol;
}
)";

static const char* kTerrainFS = R"(#version 450 core
in  vec3 v_norm;
in  vec3 v_worldPos;
in  vec2 v_uv;
in  vec3 v_vtxCol;
uniform sampler2D u_layer[6];
uniform sampler2D u_blend[5];
uniform float     u_tileRate[6];
uniform int       u_layerCount;    // 1-6
uniform int       u_useVtxColor;   // 1 = apply VCLR * 2 modulation
uniform vec3      u_lightDir;
uniform vec3      u_lightColor;
uniform vec3      u_ambientColor;
uniform vec3      u_camPos;
out vec4 f_col;
void main() {
    vec3 n = normalize(v_norm);

    vec4 col = texture(u_layer[0], v_uv * u_tileRate[0]);
    if (u_layerCount > 1) col = mix(col, texture(u_layer[1], v_uv * u_tileRate[1]), texture(u_blend[0], v_uv).r);
    if (u_layerCount > 2) col = mix(col, texture(u_layer[2], v_uv * u_tileRate[2]), texture(u_blend[1], v_uv).r);
    if (u_layerCount > 3) col = mix(col, texture(u_layer[3], v_uv * u_tileRate[3]), texture(u_blend[2], v_uv).r);
    if (u_layerCount > 4) col = mix(col, texture(u_layer[4], v_uv * u_tileRate[4]), texture(u_blend[3], v_uv).r);
    if (u_layerCount > 5) col = mix(col, texture(u_layer[5], v_uv * u_tileRate[5]), texture(u_blend[4], v_uv).r);

    if (u_useVtxColor == 1) col.rgb *= v_vtxCol * 2.0;

    float diff = max(dot(n, u_lightDir), 0.0);
    vec3  V    = normalize(u_camPos - v_worldPos);
    vec3  H    = normalize(u_lightDir + V);
    float spec = pow(max(dot(n, H), 0.0), 32.0) * diff * 0.15;
    vec3 lit = u_ambientColor * col.rgb
             + u_lightColor   * col.rgb * diff
             + u_lightColor   * spec;
    f_col = vec4(lit, 1.0);
}
)";
