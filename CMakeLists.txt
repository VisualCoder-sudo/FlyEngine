cmake_minimum_required(VERSION 3.25)
project(Flyengine LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT linking to avoid VCRUNTIME140.dll dependency
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Platform detection
if(WIN32)
    set(PLATFORM_DESKTOP 1)
    add_compile_definitions(PLATFORM_DESKTOP GRAPHICS_API_OPENGL_21)
endif()

# Raylib
add_subdirectory(raylib)

# Box3D physics
add_subdirectory(extern/box3d)

# Main executable
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/Engine.cpp
    src/CameraController.cpp
    src/ScatteredObject.cpp
    src/ObjectInteractionManager.cpp
    src/ScenePersistence.cpp
    src/Graphics.cpp
    src/CommandConsole.cpp
    src/PhysicsSimulation.cpp
    src/PhysicsCollision.cpp
    src/ScriptEditor.cpp
    src/Flyscript.cpp
    src/ProjectManager.cpp
    src/ModelImport.cpp
    src/Benchmark.cpp
    src/CrashReporter.cpp
    src/TextureManager.cpp
    src/ShaderCache.cpp
    src/ui.cpp
    src/Terrain.cpp
    src/TerrainMesh.cpp
    src/TerrainIO.cpp
    src/TerrainSculpt.cpp
    src/TerrainPhysics.cpp
    src/TerrainEditor.cpp
    src/BasicTerrain.cpp
    src/WaterBody.cpp
    src/WaterNoise.cpp
    src/UndoSystem.cpp
    src/FbxModel.cpp

    # Dear ImGui (v1.92.9b): core + demo + OpenGL3 backend + raylib bridge
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
    imgui/imgui_impl_opengl3.cpp
    imgui/imgui_impl_raylib.cpp

    # ufbx FBX parser (MIT)
    extern/ufbx/ufbx.cpp
)

if(MSVC)
    set_target_properties(${PROJECT_NAME} PROPERTIES
            LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )
endif()

target_include_directories(${PROJECT_NAME} PRIVATE include imgui extern/ufbx)
target_link_libraries(${PROJECT_NAME} PRIVATE raylib box3d::box3d comdlg32 shell32 ole32 winmm opengl32 gdi32)

# Copy shaders to build directory
file(GLOB SHADER_FILES "${CMAKE_SOURCE_DIR}/shaders/*.vert" "${CMAKE_SOURCE_DIR}/shaders/*.frag")
foreach(shader ${SHADER_FILES})
    get_filename_component(shader_name ${shader} NAME)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${shader}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${shader_name}"
        COMMENT "Copying ${shader_name} to build directory"
    )
endforeach()

# Also copy to root build dir for working directory
foreach(shader ${SHADER_FILES})
    get_filename_component(shader_name ${shader} NAME)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${shader}"
        "${CMAKE_BINARY_DIR}/${shader_name}"
        COMMENT "Copying ${shader_name} to binary directory"
    )
endforeach()