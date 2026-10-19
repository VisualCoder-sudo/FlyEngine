cmake_minimum_required(VERSION 3.25)
project(Flyengine LANGUAGES CXX C)
if(WIN32)
    # Marks objects built against the renamed rl_ShowCursor/rl_CloseWindow symbols
    # (include/rl/raylib.h). Changing it forces a full recompile of stale objects.
    add_compile_definitions(FLY_RL_WIN32_SYMBOLS=1)
endif()

# GCC 12 (Debian 12) miscompiles the engine at -O3: the editor viewport renders as a flat clear colour.
# -O2 is fine there, so GNU Release builds use it.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
    set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
endif()

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

# Third-party libraries (Box3D, libcurl) are linked statically: an exported game is a single executable with no
# .so/.dll beside it, and a shared copy would also make FlyPlayer depend on a path inside the build tree. (curl's own
# CMakeLists defaults BUILD_SHARED_LIBS to ON, and that value stuck in the cache of existing build directories.)
set(BUILD_SHARED_LIBS OFF)

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
    list(APPEND FLYENGINE_PLATFORM_LIBS Threads::Threads)
endif()

# Threads::Threads is needed on every platform (FlySqlite links it).
find_package(Threads REQUIRED)

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
        src/Plugin/PluginIdentityValidator.c
        extern/cjson/cJSON.c
)
# The process loader, IPC and sandbox use fork/Unix sockets/dlopen/seccomp and
# are POSIX-only. They are not used by the engine itself on Windows.
if(NOT WIN32)
    target_sources(FlyPluginCore PRIVATE
            src/Plugin/PluginLoader.c
            src/Plugin/PluginIPC.c)
endif()
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
        src/Engine/Backend/VisualScript.cpp
        src/Engine/Frontend/VisualScriptEditor.cpp
        src/Engine/Graphics.cpp
        src/Engine/CityMeshes.cpp
        src/Engine/FrameStats.cpp
        src/Engine/Reflections.cpp
        src/Engine/Lighting.cpp
        src/Engine/Shadows.cpp
        src/Engine/CityInstancing.cpp
        src/Engine/ShapeBatch.cpp
        src/Engine/Particles.cpp
        src/Engine/PostFX.cpp
        src/Engine/Atmosphere.cpp
        src/Engine/Clouds.cpp
        src/Engine/LoadingScreen.cpp
        src/Engine/GameUI.cpp
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
        src/Engine/Occlusion.cpp
        src/Engine/Vfs.cpp
        src/Engine/Export/PackArchive.cpp
        src/Engine/Export/PeIcon.cpp
        src/Engine/Export/ExportManager.cpp
        src/Engine/Frontend/ui.cpp
        src/Engine/Frontend/ui_assets.cpp
        src/Engine/Frontend/ui_preferences.cpp
        src/Engine/Frontend/ui_layout.cpp
        src/Engine/Frontend/ui_terrain_panel.cpp
        src/Engine/Frontend/ui_properties.cpp
        src/Engine/Frontend/ui_explorer.cpp
        src/Engine/Frontend/ui_groups.cpp
        src/Engine/Frontend/PluginHost.cpp
        src/Engine/Frontend/ui_theme.cpp
        src/Engine/Frontend/ui_lighting.cpp
        src/Engine/Frontend/ui_topbar.cpp
        src/Engine/Frontend/ui_tutorial.cpp
        src/Engine/Frontend/ExportDialog.cpp
        src/Engine/Platform/Platform.cpp
        src/Engine/Platform/FileDialog.cpp
        src/Engine/Platform/ProcessRunner.cpp
        src/Terrain/Terrain.cpp
        src/Terrain/TerrainMesh.cpp
        src/Terrain/TerrainIO.cpp
        src/Terrain/TerrainSculpt.cpp
        src/Terrain/TerrainPhysics.cpp
        src/Terrain/TerrainCollider.cpp
        src/Terrain/TerrainEditor.cpp
        src/Terrain/BasicTerrain.cpp
        src/Terrain/Foliage.cpp
        src/Terrain/BasicTerrainIO.cpp
        src/Terrain/BasicTerrainErode.cpp
        src/Terrain/Water/WaterBody.cpp
        src/Terrain/Water/WaterNoise.cpp
        src/Terrain/Water/WaterRipple.cpp
        src/Engine/Frontend/UndoSystem.cpp
        src/Engine/Audio.cpp
        src/Engine/Backend/AudioAssets.cpp
        src/Engine/Backend/SoundRays.cpp
        src/Engine/Backend/miniaudio_impl.c
        src/Engine/Backend/stb/stb_vorbis.c
        src/Engine/Backend/FbxModel.cpp
        src/Engine/Backend/ModelAnimator.cpp
        src/CityGen/City.cpp
        src/CityGen/CityGeometry.cpp
        src/CityGen/CityEditor.cpp
        src/WaterStressTest.cpp
        src/SceneTest.cpp
        src/Engine/TechnicalTools.cpp


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
if(NOT WIN32)
    # Plugin sandbox (seccomp/cgroups, Linux only)
    target_sources(FlyEngineCore PRIVATE src/Plugin/PluginSandbox.c)
endif()

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

# Third-party C for sound: miniaudio (mixer, WAV/MP3/FLAC decoders, the audio device) and stb_vorbis (OGG).
# Built without our warning flags; they are not ours to fix.
set_source_files_properties(
        src/Engine/Backend/miniaudio_impl.c
        src/Engine/Backend/stb/stb_vorbis.c
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")

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
    # The editor launches the FlyPlayer sitting next to it for Play, so building the editor alone (e.g. the
    # IDE's Flyengine target) must rebuild the player too, or Play runs a stale one.
    add_dependencies(Flyengine${suffix} FlyPlayer${suffix})
    # Windows .exe icon
    if(WIN32)
        target_sources(Flyengine${suffix} PRIVATE resources/app.rc)
    endif()
    # The player carries placeholder icons that Export overwrites with the game's own (include/Engine/PeIcon.hpp).
    if(WIN32)
        target_sources(FlyPlayer${suffix} PRIVATE resources/player.rc)
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

# `flyexport <project> --out <dir>`: the editor's Export without the editor (see src/ExportMain.cpp).
add_executable(flyexport src/ExportMain.cpp)
target_link_libraries(flyexport PRIVATE FlyEngineCore FlyRL)
add_dependencies(flyexport FlyPlayer)

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

# Collect the built executables into <source>/export/<platform> after every build.
set(_fly_exe_targets Flyengine FlyPlayer flyexport)
if(FLY_HAS_FALLBACK)
    list(APPEND _fly_exe_targets Flyengine_fallback FlyPlayer_fallback)
endif()
# Dev builds are copied to export/windows, export/linux or export/macos. The Linux release is the AppImage from
# packaging/build_appimage.sh, which lands in export/.
if(WIN32)
    set(FLY_PLATFORM_TAG windows)
elseif(APPLE)
    set(FLY_PLATFORM_TAG macos)
else()
    set(FLY_PLATFORM_TAG linux)
endif()
set(FLY_EXE_DIR "${CMAKE_SOURCE_DIR}/export/${FLY_PLATFORM_TAG}" CACHE PATH "Where built executables are copied")

add_custom_target(fly_collect_exe ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${FLY_EXE_DIR}"
        COMMENT "Copying executables to ${FLY_EXE_DIR}")
# Data the editor reads from next to the executable (see CachedRoots in Platform.cpp).
# (copy_directory_if_different needs CMake 3.26; assets/ is large, so avoid recopying it every build when possible.)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.26)
    set(_fly_copy_dir copy_directory_if_different)
else()
    set(_fly_copy_dir copy_directory)
endif()
add_custom_command(TARGET fly_collect_exe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E ${_fly_copy_dir} "${CMAKE_SOURCE_DIR}/assets" "${FLY_EXE_DIR}/assets"
        COMMAND ${CMAKE_COMMAND} -E ${_fly_copy_dir} "${CMAKE_SOURCE_DIR}/ScriptingSDK" "${FLY_EXE_DIR}/ScriptingSDK")
foreach(t ${_fly_exe_targets})
    add_dependencies(fly_collect_exe ${t})
    add_custom_command(TARGET fly_collect_exe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${t}>"
            "${FLY_EXE_DIR}/$<TARGET_FILE_NAME:${t}>")
endforeach()

# --- flyplugin CLI tool ----------------------------------------------------
if(NOT WIN32)
add_executable(flyplugin src/Plugin/cli/FlyPluginCLI.c)
target_link_libraries(flyplugin PRIVATE FlyPluginCore)

# --- flyplugin-runner (plugin process runner) ------------------------------
add_executable(flyplugin-runner src/Plugin/PluginProcessRunner.c)
target_link_libraries(flyplugin-runner PRIVATE FlyPluginCore)
endif()

# --- LTO (Release only) ----------------------------------------------------
if(FLYENGINE_ENABLE_LTO AND FLYENGINE_IPO_SUPPORTED)
    if(TARGET flyplugin)
        set_target_properties(flyplugin flyplugin-runner PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
    set_target_properties(
            FlyEngineCore FlyRL FlyImGui FlySqlite FlyPluginCore
            ${PROJECT_NAME} FlyPlayer
            PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
elseif(FLYENGINE_ENABLE_LTO)
    message(STATUS "LTO not supported by this toolchain: ${FLYENGINE_IPO_MSG}")
endif()

# --- Shaders ---------------------------------------------------------------
# shaders/*.glsl are compiled by sokol-shdc into headers and linked into FlyRL
# (cmake/FlySokol.cmake); nothing is loaded from disk at runtime.

# --- Install / packaging ---------------------------------------------------

install(TARGETS ${PROJECT_NAME} FlyPlayer
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(NOT WIN32)
    install(TARGETS flyplugin flyplugin-runner
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
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