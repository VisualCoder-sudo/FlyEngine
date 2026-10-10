cmake_minimum_required(VERSION 3.25)
project(Flyengine LANGUAGES CXX C)

# C++20 (designated initializers are convenient for sokol's descriptor structs).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# GNUInstallDirs has to come before anything that reads CMAKE_INSTALL_FULL_*
# or CMAKE_INSTALL_*DIR, so it goes at the very top.
include(GNUInstallDirs)

# --- Build type / tooling --------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER   ${CCACHE_PROGRAM})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

option(FLYENGINE_ENABLE_LTO "Enable link-time optimization for Flyengine targets in Release" ON)
if(FLYENGINE_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT FLYENGINE_IPO_SUPPORTED OUTPUT FLYENGINE_IPO_MSG LANGUAGES C CXX)
endif()

# Where the running binary will look for its bundled assets.
#
# The same variable is both the install destination and the value compiled into
# the binary as FLYENGINE_DATA_DIR, so the two can never drift apart. It stays
# overridable so a packager can relocate the data, and so a developer build can
# be pointed at the source tree without touching the filesystem layout.
#
# At runtime platform::ResolveAsset/ResolveShader also try the working
# directory, its parents, and several locations relative to the executable, so
# a build directory nested inside the source tree keeps working with nothing
# set at all.
set(FLYENGINE_DATA_DIR "${CMAKE_INSTALL_FULL_DATADIR}/flyengine"
        CACHE PATH "Install location for Flyengine's shaders and data files")

# With the default value, install to a prefix-relative path so DESTDIR, CPack
# and relocatable prefixes work. A user-supplied override is used verbatim.
if(FLYENGINE_DATA_DIR STREQUAL "${CMAKE_INSTALL_FULL_DATADIR}/flyengine")
    set(FLYENGINE_INSTALL_DATA_DIR "${CMAKE_INSTALL_DATADIR}/flyengine")
else()
    set(FLYENGINE_INSTALL_DATA_DIR "${FLYENGINE_DATA_DIR}")
endif()

# --- Toolchain / platform settings -----------------------------------------
if(MSVC)
    # Static CRT linking to avoid VCRUNTIME140.dll dependency
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/MP /utf-8 /Zc:__cplusplus /permissive-)
endif()

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

if(UNIX AND NOT APPLE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

# Platform detection.
if(UNIX)
    add_compile_definitions(PLATFORM_DESKTOP_UNIX)
endif()

# --- Feature switches ------------------------------------------------------
# Native C/C++ scripting (cross-platform, no external runtime)
option(FLYENGINE_NATIVE_SCRIPTS "Enable native C/C++ plugin scripting" ON)

option(FLYENGINE_BUILD_TESTS "Build the tests/ targets (requires test infrastructure)" OFF)

# --- Graphics backend ---------------------------------------------------------
# The engine renders through sokol_gfx (via the raylib-compatible layer in
# src/Engine/RL). The native API is chosen at compile time:
#   VULKAN  default; needs the Vulkan loader (+ headers, vendored if absent)
#   GL      OpenGL 4.1 core; the fallback for machines without a usable
#           Vulkan driver (sokol's Vulkan backend needs Vulkan 1.3 and
#           VK_EXT_descriptor_buffer)
#   D3D11   Windows only; the mature fallback there
set(FLYENGINE_GRAPHICS_BACKEND "VULKAN" CACHE STRING "Graphics API: VULKAN, GL or D3D11")
set_property(CACHE FLYENGINE_GRAPHICS_BACKEND PROPERTY STRINGS VULKAN GL D3D11)

# Dear ImGui demo window. Turn OFF for lean release builds if nothing calls
# ImGui::ShowDemoWindow.
option(FLYENGINE_IMGUI_DEMO "Compile imgui_demo.cpp" ON)

# Validate packaging/desktop files with desktop-file-validate and xmllint when
# they are present. Off by default so a build does not depend on devtools.
option(FLYENGINE_ENABLE_DESKTOP_VALIDATION "Validate .desktop/MIME files at build time" OFF)

# --- Dependencies ----------------------------------------------------------
# Window, input and rendering: sokol + the raylib-compatible layer (FlyRL).
include(cmake/FlySokol.cmake)

# Box3D physics
add_subdirectory(extern/box3d)

# FlyCloud integration: vendored libcurl (static), miniz (ZIP), and
# nlohmann/json. TLS is Schannel on Windows and OpenSSL elsewhere. Set
# -DFLYENGINE_ENABLE_FLYCLOUD=OFF to drop the libcurl dependency entirely
# (fcloud console commands become no-ops).
option(FLYENGINE_ENABLE_FLYCLOUD "Build the FlyCloud integration (vendored libcurl, miniz, nlohmann/json)" ON)

if(FLYENGINE_ENABLE_FLYCLOUD)
    # The curl tool, docs, examples and tests are disabled so only the
    # libcurl static library gets built.
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(BUILD_STATIC_CURL ON CACHE BOOL "" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
    if(MSVC)
        set(CURL_STATIC_CRT ON CACHE BOOL "" FORCE)
    endif()
    if(WIN32)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    else()
        # Needs libssl-dev (Debian/Ubuntu) / openssl-devel (Fedora) / openssl (Arch)
        set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
    endif()
    # curl/CMakeLists.txt unconditionally runs find_package(Perl) and then does
    # add_subdirectory(docs) when Perl is found -- but curl/docs was not
    # vendored, so a Linux box with perl installed fails to configure. Note that
    # setting PERL_EXECUTABLE to a bogus value does NOT work here: find_package
    # re-searchs and overwrites that cache entry. Disabling the search is the
    # reliable fix, and perl is only needed for the man page anyway.
    set(CMAKE_DISABLE_FIND_PACKAGE_Perl ON)
    add_subdirectory(curl)
    set(FLYCLOUD_INCLUDE_DIRS curl/include extern/miniz extern)
    set(FLYCLOUD_LIBS libcurl)
    set(FLYCLOUD_DEFINES CURL_STATICLIB FLYENGINE_ENABLE_FLYCLOUD)
else()
    set(FLYCLOUD_INCLUDE_DIRS)
    set(FLYCLOUD_LIBS)
    set(FLYCLOUD_DEFINES)
endif()

# --- Platform libraries ----------------------------------------------------
# Windows-only system libraries: common dialogs (commdlg32), shell folder +
# ShellExecute + PIDL helpers (shell32), CoTaskMem (ole32), timers (winmm) and
# GDI. None of these exist on Linux, so they must not be requested there.
# Graphics/windowing libraries come from FlyRL (cmake/FlySokol.cmake).
if(WIN32)
    set(FLYENGINE_PLATFORM_LIBS comdlg32 shell32 ole32 winmm gdi32)
elseif(UNIX)
    # dl  : dynamic loading for the CLR host and miniz
    # m   : box3d maths helpers on glibc
    set(FLYENGINE_PLATFORM_LIBS ${CMAKE_DL_LIBS} m)
    find_package(Threads REQUIRED)
    list(APPEND FLYENGINE_PLATFORM_LIBS Threads::Threads)
endif()

# --- SQLite (compiled once, shared by every executable) --------------------
add_library(FlySqlite STATIC extern/sqlite/sqlite3.c)
target_include_directories(FlySqlite PUBLIC extern/sqlite)
target_link_libraries(FlySqlite PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
set_target_properties(FlySqlite PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- Plugin core (C) -------------------------------------------------------
# Shared by the engine, flyplugin and flyplugin-runner so cJSON and the plugin
# sources are compiled exactly once. The manifest parser calls into cJSON, so
# cJSON lives here too (compiling it per-executable used to leave FlyPlayer
# with undefined cJSON_* references).
add_library(FlyPluginCore STATIC
        src/Plugin/ManifestParser.c
        src/Plugin/VersionCompat.c
        src/Plugin/PluginLoader.c
        src/Plugin/PluginIdentityValidator.c
        src/Plugin/PluginIPC.c
        extern/cjson/cJSON.c
)
target_include_directories(FlyPluginCore PUBLIC include extern/cjson)
target_link_libraries(FlyPluginCore PUBLIC ${CMAKE_DL_LIBS})
set_target_properties(FlyPluginCore PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- Engine Core Library ---------------------------------------------------
add_library(FlyEngineCore STATIC
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
        src/Engine/Scripts/ScriptDiagnostics.cpp
        src/Engine/Backend/FlyScriptApi.cpp
        src/Engine/Scripts/NativeScriptHost.cpp
        src/Engine/Scripts/CPluginAPI.cpp
        src/Engine/Frontend/ProjectManager.cpp
        src/Engine/Backend/ModelImport.cpp
        src/Engine/Backend/CharacterController.cpp
        src/Engine/Backend/Benchmark.cpp
        src/Engine/Backend/CrashReporter.cpp
        src/Engine/TextureManager.cpp
        src/Engine/Frontend/ui.cpp
        src/Engine/Platform/Platform.cpp
        src/Engine/Platform/FileDialog.cpp
        src/Engine/Platform/ProcessRunner.cpp
        src/Terrain/Terrain.cpp
        src/Terrain/TerrainMesh.cpp
        src/Terrain/TerrainIO.cpp
        src/Terrain/TerrainSculpt.cpp
        src/Terrain/TerrainPhysics.cpp
        src/Terrain/TerrainEditor.cpp
        src/Terrain/BasicTerrain.cpp
        src/Terrain/BasicTerrainIO.cpp
        src/Terrain/BasicTerrainErode.cpp
        src/Terrain/Water/WaterBody.cpp
        src/Terrain/Water/WaterNoise.cpp
        src/Terrain/Water/WaterRipple.cpp
        src/Engine/Frontend/UndoSystem.cpp
        src/Engine/Backend/FbxModel.cpp
        src/CityGen/City.cpp
        src/CityGen/CityGeometry.cpp
        src/CityGen/CityEditor.cpp
        src/WaterStressTest.cpp
        src/SceneTest.cpp
        src/Engine/TechnicalTools.cpp

        # Plugin sandbox (the rest of the plugin system is in FlyPluginCore)
        src/Plugin/PluginSandbox.c

        # ufbx FBX parser (MIT)
        extern/ufbx/ufbx.cpp
)

# Dear ImGui (v1.92.9b) core. A separate library because both the engine and the
# graphics layer (sokol_imgui) use it. Rendering and input go through sokol_imgui
# (src/Engine/RL/sokol_imgui_impl.cpp).
add_library(FlyImGui STATIC
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp)
if(FLYENGINE_IMGUI_DEMO)
    target_sources(FlyImGui PRIVATE imgui/imgui_demo.cpp)
endif()
target_include_directories(FlyImGui PUBLIC imgui)
set_target_properties(FlyImGui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# cglm backs the math helpers in include/Engine/Backend/Math.hpp (column-major,
# with explicit [0,1]/[-1,1] depth-range projections). PUBLIC because that
# header is part of FlyEngineCore's public interface.
target_include_directories(FlyEngineCore PUBLIC src/Engine/Backend/cglm/include)

# miniz deflate/inflate (public domain). Compiled unconditionally, not just for
# FlyCloud: BasicTerrain persistence deflates its heightmap and splatmap, and
# making that depend on an unrelated feature flag would mean terrains silently
# failing to save in a -DFLYENGINE_ENABLE_FLYCLOUD=OFF build. These three files
# are pure compression and reference no mz_zip_* symbols (only miniz_zip.c does).
target_sources(FlyEngineCore PRIVATE
        extern/miniz/miniz.c
        extern/miniz/miniz_tdef.c
        extern/miniz/miniz_tinfl.c
)

if(FLYENGINE_ENABLE_FLYCLOUD)
    # miniz ZIP archive writer, used only by FlyCloud
    target_sources(FlyEngineCore PRIVATE
            extern/miniz/miniz_zip.c
    )
endif()

target_include_directories(FlyEngineCore PUBLIC include imgui extern/ufbx extern/miniz ${FLYCLOUD_INCLUDE_DIRS})
# FlyScriptABI.h: the C ABI shared with compiled game scripts.
target_include_directories(FlyEngineCore PUBLIC ScriptingSDK/include)
target_link_libraries(FlyEngineCore PUBLIC
        FlyRLHeaders FlyImGui box3d::box3d
        FlySqlite FlyPluginCore
        ${FLYCLOUD_LIBS} ${FLYENGINE_PLATFORM_LIBS}
        )

# libcurl is linked statically
target_compile_definitions(FlyEngineCore PUBLIC ${FLYCLOUD_DEFINES})

if(FLYENGINE_NATIVE_SCRIPTS)
    target_compile_definitions(FlyEngineCore PUBLIC FLYENGINE_NATIVE_SCRIPTS)
endif()

# Where the executable looks for shaders/ and assets/ at runtime.
target_compile_definitions(FlyEngineCore PRIVATE
        FLYENGINE_DATA_DIR="${FLYENGINE_DATA_DIR}")

# Precompiled headers (C++ only) -- skipped for third-party sources.
# <raylib.h> is FlyEngine's raylib-compatible API (include/rl/raylib.h).
target_precompile_headers(FlyEngineCore PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:<raylib.h$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<imgui.h$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<string$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<vector$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<unordered_map$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<memory$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<algorithm$<ANGLE-R>>"
)
set_source_files_properties(
        extern/ufbx/ufbx.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# --- Executables ------------------------------------------------------------
# The editor and the player, once per graphics layer: the primary backend
# (Flyengine, FlyPlayer) and, when built, the fallback (Flyengine-fallback,
# FlyPlayer-fallback).
function(fly_add_executables suffix rl_lib)
    add_executable(Flyengine${suffix} src/main.cpp include/Engine/Backend/audio.h)
    add_executable(FlyPlayer${suffix} src/PlayerMain.cpp include/Engine/Backend/audio.h)
    if(NOT suffix STREQUAL "")
        set_target_properties(Flyengine${suffix} PROPERTIES OUTPUT_NAME Flyengine-fallback)
        set_target_properties(FlyPlayer${suffix} PROPERTIES OUTPUT_NAME FlyPlayer-fallback)
    endif()
    # Windows .exe icon
    if(WIN32)
        target_sources(Flyengine${suffix} PRIVATE resources/app.rc)
    endif()
    foreach(exe Flyengine${suffix} FlyPlayer${suffix})
        # GUI subsystem on Windows (no console window), regular main() still works.
        set_target_properties(${exe} PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(${exe} PRIVATE /ENTRY:mainCRTStartup)
        endif()
        # Export host symbols so plugins can resolve them (Linux; Windows uses
        # __declspec(dllexport) on the plugin API instead).
        if(UNIX AND NOT APPLE)
            set_target_properties(${exe} PROPERTIES ENABLE_EXPORTS ON)
        endif()
        # Portable whole-archive (GNU ld, lld, MSVC /WHOLEARCHIVE, Apple -force_load).
        # The graphics layer comes after the engine, which depends on it.
        target_link_libraries(${exe} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,FlyEngineCore>" ${rl_lib})
    endforeach()
endfunction()

fly_add_executables("" FlyRL)
if(FLY_HAS_FALLBACK)
    fly_add_executables("_fallback" FlyRL_fallback)
endif()

# Create case-insensitive symlinks for FlyPlayer on Linux
if(UNIX AND NOT APPLE)
    add_custom_command(TARGET FlyPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E create_symlink
            FlyPlayer
            $<TARGET_FILE_DIR:FlyPlayer>/flyplayer
            COMMAND ${CMAKE_COMMAND} -E create_symlink
            FlyPlayer
            $<TARGET_FILE_DIR:FlyPlayer>/FLYPLAYER
            COMMAND ${CMAKE_COMMAND} -E create_symlink
            FlyPlayer
            $<TARGET_FILE_DIR:FlyPlayer>/Flyplayer
            COMMENT "Creating case-insensitive symlinks for FlyPlayer"
    )
endif()

# --- flyplugin CLI tool ----------------------------------------------------
add_executable(flyplugin src/Plugin/cli/FlyPluginCLI.c)
target_link_libraries(flyplugin PRIVATE FlyPluginCore)

# --- flyplugin-runner (plugin process runner) ------------------------------
add_executable(flyplugin-runner src/Plugin/PluginProcessRunner.c)
target_link_libraries(flyplugin-runner PRIVATE FlyPluginCore)

# --- LTO (Release only) ----------------------------------------------------
if(FLYENGINE_ENABLE_LTO AND FLYENGINE_IPO_SUPPORTED)
    set_target_properties(
            FlyEngineCore FlyRL FlyImGui FlySqlite FlyPluginCore
            ${PROJECT_NAME} FlyPlayer flyplugin flyplugin-runner
            PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
elseif(FLYENGINE_ENABLE_LTO)
    message(STATUS "LTO not supported by this toolchain: ${FLYENGINE_IPO_MSG}")
endif()

# --- Shaders ---------------------------------------------------------------
# shaders/*.glsl are compiled by sokol-shdc into headers and linked into FlyRL
# (cmake/FlySokol.cmake); nothing is loaded from disk at runtime.

# --- Install / packaging ---------------------------------------------------

install(TARGETS ${PROJECT_NAME} FlyPlayer flyplugin flyplugin-runner
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(FLY_HAS_FALLBACK)
    install(TARGETS Flyengine_fallback FlyPlayer_fallback
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets/"
        DESTINATION "${FLYENGINE_INSTALL_DATA_DIR}/assets"
        PATTERN "*.ico" EXCLUDE)
# The C++ script SDK: the editor compiles projects' Scripts/*.cpp against it.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/ScriptingSDK/include"
                  "${CMAKE_SOURCE_DIR}/ScriptingSDK/src"
                  "${CMAKE_SOURCE_DIR}/ScriptingSDK/samples"
        DESTINATION "${FLYENGINE_INSTALL_DATA_DIR}/ScriptingSDK")

if(UNIX AND NOT APPLE)
    install(FILES packaging/flyengine.desktop
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES packaging/flyengine.xml
            DESTINATION "${CMAKE_INSTALL_DATADIR}/mime/packages")

    file(GLOB_RECURSE HICOLOR_ICONS CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/assets/icons/hicolor/*/apps/flyengine.png")
    foreach(icon ${HICOLOR_ICONS})
        file(RELATIVE_PATH rel
                "${CMAKE_SOURCE_DIR}/assets/icons/hicolor" "${icon}")
        get_filename_component(rel_dir "${rel}" DIRECTORY)
        install(FILES "${icon}"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${rel_dir}")
    endforeach()

    if(FLYENGINE_ENABLE_DESKTOP_VALIDATION)
        enable_testing()
        find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
        find_program(XML_LINT xmllint)
        foreach(fly_desktop packaging/flyengine.desktop)
            if(DESKTOP_FILE_VALIDATE)
                add_test(NAME desktop_file_validate
                        COMMAND ${DESKTOP_FILE_VALIDATE} "${CMAKE_SOURCE_DIR}/${fly_desktop}")
            else()
                message(STATUS "desktop-file-validate not found; skipping .desktop validation")
            endif()
        endforeach()
        if(XML_LINT AND EXISTS "${CMAKE_SOURCE_DIR}/packaging/flyengine.xml")
            add_test(NAME mime_xml_valid
                    COMMAND ${XML_LINT} --noout
                    "${CMAKE_SOURCE_DIR}/packaging/flyengine.xml")
        endif()
    endif()
endif()

# --- Tests -----------------------------------------------------------------
if(FLYENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()