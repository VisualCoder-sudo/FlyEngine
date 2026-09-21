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

# FlyCloud integration: vendored libcurl (static, Schannel TLS), miniz (ZIP),
# and nlohmann/json. The curl tool, docs, examples and tests are disabled so
# only the libcurl static library gets built.
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(BUILD_STATIC_CURL ON CACHE BOOL "" FORCE)
set(CURL_STATIC_CRT ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
set(PERL_EXECUTABLE "PERL_EXECUTABLE-NOTFOUND" CACHE FILEPATH "Disable Perl during the vendored curl build" FORCE)
add_subdirectory(curl)

# Main executable
add_executable(${PROJECT_NAME}
        src/main.cpp
        src/Engine/Engine.cpp
        src/Engine/CameraController.cpp
        src/Engine/Backend/ScatteredObject.cpp
        src/Engine/Frontend/ObjectInteractionManager.cpp
        src/Engine/Frontend/ScenePersistence.cpp
        src/Engine/Graphics.cpp
        src/Engine/Scripts/CommandConsole.cpp
        src/Engine/Backend/fcloudint.cpp
        src/Engine/PhysicsSimulation.cpp
        src/Engine/PhysicsCollision.cpp
        src/Engine/Scripts/ScriptRuntime.cpp
        src/Engine/Scripts/ScriptLauncher.cpp
        src/Engine/Scripts/ScriptCompiler.cpp
        src/Engine/Backend/CoreCLRHost.cpp
        src/Engine/Backend/FlyScriptApi.cpp
        src/Engine/Frontend/ProjectManager.cpp
        src/Engine/Backend/ModelImport.cpp
        src/Engine/Backend/Benchmark.cpp
        src/Engine/Backend/CrashReporter.cpp
        src/Engine/TextureManager.cpp
        src/Engine/Backend/ShaderCache.cpp
        src/Engine/Frontend/ui.cpp
        src/Terrain/Terrain.cpp
        src/Terrain/TerrainMesh.cpp
        src/Terrain/TerrainIO.cpp
        src/Terrain/TerrainSculpt.cpp
        src/Terrain/TerrainPhysics.cpp
        src/Terrain/TerrainEditor.cpp
        src/Terrain/BasicTerrain.cpp
        src/Terrain/Water/WaterBody.cpp
        src/Terrain/Water/WaterNoise.cpp
        src/Engine/Frontend/UndoSystem.cpp
        src/Engine/Backend/FbxModel.cpp
        src/CityGen/City.cpp
        src/CityGen/CityGeometry.cpp
        src/CityGen/CityEditor.cpp
        src/WaterStressTest.cpp

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

        # miniz ZIP archive writer for FlyCloud (public domain)
        extern/miniz/miniz.c
        extern/miniz/miniz_tdef.c
        extern/miniz/miniz_tinfl.c
        extern/miniz/miniz_zip.c
)

# Windows .exe icon (shown in Explorer, shortcuts, and the taskbar before the
# window itself sets its runtime icon). Separate from assets/FlyengineLogo.png,
# which SetWindowIcons() applies once the app is actually running.
if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE resources/app.rc)
endif()

if(MSVC)
    set_target_properties(${PROJECT_NAME} PROPERTIES
            LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )
endif()

target_include_directories(${PROJECT_NAME} PRIVATE include imgui extern/ufbx curl/include extern/miniz extern)
target_link_libraries(${PROJECT_NAME} PRIVATE raylib box3d::box3d libcurl comdlg32 shell32 ole32 winmm opengl32 gdi32)

# libcurl is linked statically
target_compile_definitions(${PROJECT_NAME} PRIVATE CURL_STATICLIB)

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