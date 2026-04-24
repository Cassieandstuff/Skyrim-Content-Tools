# ---------------------------------------------------------------------------
# Skyrim Content Tools (SCT)
# C++23 standalone authoring environment — Dear ImGui + OpenGL 4.5
#
# Standalone:  cmake --preset debug   → open build-debug/SkyrimContentTools.sln
#              cmake --preset release → open build/SkyrimContentTools.sln
#
# Workspace:   add_subdirectory(Skyrim-Content-Tools) from parent CMakeLists.txt
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(SkyrimContentTools
        VERSION 0.1.0
        DESCRIPTION "Skyrim Content Tools — character content authoring environment"
        LANGUAGES CXX C
    )
    message(STATUS "SCT: standalone build")
else()
    message(STATUS "SCT: workspace build (parent: ${CMAKE_PROJECT_NAME})")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ── GLFW ──────────────────────────────────────────────────────────────────
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ── glad (bundled pre-generated OpenGL 4.5 Core loader) ───────────────────
# Must come before imgui so imgui_impl_opengl3.cpp can find <glad/gl.h>.
add_library(glad_gl_45_core STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/glad/src/gl.c"
)
target_include_directories(glad_gl_45_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/glad/include"
)

# ── Dear ImGui (docking branch) ───────────────────────────────────────────
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
)
target_include_directories(imgui PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)
# Link glad_gl_45_core so imgui_impl_opengl3.cpp finds <glad/gl.h> in its include path
target_link_libraries(imgui PUBLIC glfw glad_gl_45_core)
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
set_target_properties(imgui PROPERTIES CXX_STANDARD 17)

if(WIN32)
    target_link_libraries(imgui PUBLIC opengl32)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(imgui PUBLIC OpenGL::GL)
endif()

# ── glm (header-only math) ────────────────────────────────────────────────
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

# ── imgui-node-editor ─────────────────────────────────────────────────────
FetchContent_Declare(imgui_node_editor
    GIT_REPOSITORY https://github.com/thedmd/imgui-node-editor.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui_node_editor)

# imgui_extra_math.inl unconditionally defines operator*(float,ImVec2), duplicating
# the same operator imgui.h emits under IMGUI_DEFINE_MATH_OPERATORS. MSVC rejects
# the redefinition with C2264. Our patched copy guards it with the sentinel macro
# (IMGUI_DEFINE_MATH_OPERATORS_IMPLEMENTED) that imgui.h sets when it defines them.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/imgui_extra_math.inl"
    "${imgui_node_editor_SOURCE_DIR}/imgui_extra_math.inl"
    COPYONLY
)

if(NOT TARGET imgui-node-editor)
    add_library(imgui-node-editor STATIC
        "${imgui_node_editor_SOURCE_DIR}/imgui_node_editor.cpp"
        "${imgui_node_editor_SOURCE_DIR}/imgui_node_editor_api.cpp"
        "${imgui_node_editor_SOURCE_DIR}/imgui_canvas.cpp"
        "${imgui_node_editor_SOURCE_DIR}/crude_json.cpp"
    )
    target_include_directories(imgui-node-editor PUBLIC
        "${imgui_node_editor_SOURCE_DIR}"
    )
    target_link_libraries(imgui-node-editor PUBLIC imgui)
    target_compile_definitions(imgui-node-editor PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
endif()

# ---------------------------------------------------------------------------
# SCT application
# ---------------------------------------------------------------------------
add_subdirectory(src)

# ---------------------------------------------------------------------------
# Optional workspace dependency on HKBuild (runtime subprocess, not link-time)
# ---------------------------------------------------------------------------
if(TARGET HKBuild)
    add_dependencies(SkyrimContentTools HKBuild)
    message(STATUS "SCT: build-order dependency on HKBuild added.")
endif()
