file(GLOB_RECURSE SCT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE SCT_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

add_executable(SkyrimContentTools WIN32 ${SCT_SOURCES} ${SCT_HEADERS})

target_compile_features(SkyrimContentTools PRIVATE cxx_std_23)

target_include_directories(SkyrimContentTools PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(SkyrimContentTools PRIVATE
    imgui
    imgui-node-editor
    glfw
)

if(DEFINED HKBUILD_EXE)
    target_compile_definitions(SkyrimContentTools PRIVATE HKBUILD_EXE="${HKBUILD_EXE}")
endif()

if(MSVC)
    target_compile_options(SkyrimContentTools PRIVATE
        /W4 /wd4100 /wd4505 /MP
    )
    set_target_properties(SkyrimContentTools PROPERTIES
        WIN32_EXECUTABLE $<CONFIG:Release>
    )
endif()
