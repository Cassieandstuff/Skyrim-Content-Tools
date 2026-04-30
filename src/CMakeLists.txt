file(GLOB_RECURSE SCT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE SCT_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

add_executable(SkyrimContentTools WIN32 ${SCT_SOURCES} ${SCT_HEADERS})

target_compile_features(SkyrimContentTools PRIVATE cxx_std_23)
target_precompile_headers(SkyrimContentTools PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/pch.h>")

target_include_directories(SkyrimContentTools PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${lz4_SOURCE_DIR}/lib"
)

target_sources(SkyrimContentTools PRIVATE "${lz4_SOURCE_DIR}/lib/lz4.c")

target_link_libraries(SkyrimContentTools PRIVATE
    imgui
    imgui-node-editor
    glad_gl_45_core
    glm::glm
    glfw
    pugixml
    nlohmann_json::nlohmann_json
    nifly
)

if(WIN32)
    target_link_libraries(SkyrimContentTools PRIVATE comdlg32)
endif()


# ── SctBridge (.NET) ──────────────────────────────────────────────────────────
# Build the C# bridge that wraps HKX2E and expose [UnmanagedCallersOnly] entry
# points loaded at runtime via hostfxr. Output always goes to sct_bridge/ so
# the post-build step can copy it to whichever config the exe landed in.

set(SCT_BRIDGE_OUT_DIR "${CMAKE_BINARY_DIR}/sct_bridge")

# Generate the .csproj into the build tree so VS doesn't pick it up as a
# standalone project target from the source tree.
set(SCT_BRIDGE_CSPROJ "${CMAKE_BINARY_DIR}/sct-bridge/SctBridge.csproj")
configure_file(
    "${CMAKE_SOURCE_DIR}/sct-bridge/SctBridge.csproj.in"
    "${SCT_BRIDGE_CSPROJ}"
    @ONLY
)

add_custom_command(
    OUTPUT  "${SCT_BRIDGE_OUT_DIR}/SctBridge.dll"
    # publish (not build) avoids the dotnet build --output multi-project type
    # conflict bug that appears in .NET 7+ SDK when --output is used with
    # project graphs containing multiple projects (e.g. HKX2ELibrary.sln).
    # --no-self-contained = framework-dependent; runtime must be on the machine.
    COMMAND dotnet publish "${SCT_BRIDGE_CSPROJ}"
            -c Release
            --no-self-contained
            --output "${SCT_BRIDGE_OUT_DIR}"
            --nologo
            "-p:HKX2EPath=${hkx2e_SOURCE_DIR}/HKX2/HKX2E.csproj"
    DEPENDS "${SCT_BRIDGE_CSPROJ}"
            "${hkx2e_SOURCE_DIR}/HKX2/HKX2E.csproj"
    COMMENT "Publishing SctBridge (.NET bridge for HKX2E)"
    VERBATIM
)
add_custom_target(SctBridge DEPENDS "${SCT_BRIDGE_OUT_DIR}/SctBridge.dll")
add_dependencies(SkyrimContentTools SctBridge)

# Copy the entire publish output next to the SCT exe after each build.
# This includes SctBridge.dll, SctBridge.runtimeconfig.json, SctBridge.deps.json,
# HKX2E.dll, and any other NuGet dependency DLLs the bridge needs at runtime.
add_custom_command(TARGET SkyrimContentTools POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SCT_BRIDGE_OUT_DIR}"
            "$<TARGET_FILE_DIR:SkyrimContentTools>"
    COMMENT "Deploying SctBridge artifacts to exe directory"
    VERBATIM
)

if(MSVC)
    target_compile_options(SkyrimContentTools PRIVATE
        /W4 /wd4100 /wd4505 /MP
    )
    set_target_properties(SkyrimContentTools PROPERTIES
        WIN32_EXECUTABLE $<CONFIG:Release>
    )
endif()
