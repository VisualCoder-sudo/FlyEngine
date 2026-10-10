# FlyRL: the raylib-compatible API implemented on sokol_app + sokol_gfx
# (src/Engine/RL), the sokol implementation unit, raylib's CPU-side modules
# (shapes, images, text, model loaders) and the offline-compiled shaders.
#
# Inputs:
#   FLYENGINE_GRAPHICS_BACKEND  VULKAN | GL | D3D11
#   FLYENGINE_SOKOL_SHDC        path to sokol-shdc (downloaded when unset)

set(FLY_SHADERS
    ${CMAKE_SOURCE_DIR}/shaders/rl_default.glsl
    ${CMAKE_SOURCE_DIR}/shaders/rl_blit.glsl
    ${CMAKE_SOURCE_DIR}/shaders/lit.glsl
    ${CMAKE_SOURCE_DIR}/shaders/terrain.glsl
    ${CMAKE_SOURCE_DIR}/shaders/terrain_paint.glsl
    ${CMAKE_SOURCE_DIR}/shaders/water.glsl
    ${CMAKE_SOURCE_DIR}/shaders/sky.glsl
    ${CMAKE_SOURCE_DIR}/shaders/clouds.glsl
    ${CMAKE_SOURCE_DIR}/shaders/post.glsl)
set(FLY_SHADER_INCLUDES
    ${CMAKE_SOURCE_DIR}/shaders/fly_common.glsl
    ${CMAKE_SOURCE_DIR}/shaders/fly_atmosphere.glsl)

# --- Backend selection ------------------------------------------------------
string(TOUPPER "${FLYENGINE_GRAPHICS_BACKEND}" FLY_BACKEND)
if(FLY_BACKEND STREQUAL "VULKAN")
    set(FLY_SOKOL_DEFINE SOKOL_VULKAN)
elseif(FLY_BACKEND STREQUAL "GL")
    set(FLY_SOKOL_DEFINE SOKOL_GLCORE)
elseif(FLY_BACKEND STREQUAL "D3D11")
    if(NOT WIN32)
        message(FATAL_ERROR "FLYENGINE_GRAPHICS_BACKEND=D3D11 requires Windows")
    endif()
    set(FLY_SOKOL_DEFINE SOKOL_D3D11)
else()
    message(FATAL_ERROR "FLYENGINE_GRAPHICS_BACKEND must be VULKAN, GL or D3D11 (got '${FLYENGINE_GRAPHICS_BACKEND}')")
endif()
message(STATUS "Graphics backend: ${FLY_BACKEND} (${FLY_SOKOL_DEFINE})")

# --- sokol-shdc -------------------------------------------------------------
# The compiler must match the vendored sokol headers (sokol/): newer releases can
# generate code that needs newer headers. So the download is pinned to one
# sokol-tools-bin commit (2026-08-09) and verified by SHA-256. To upgrade, change
# FLY_SHDC_COMMIT and the hashes together, and rebuild all shaders.
set(FLY_SHDC_COMMIT af9a893b745c9302114791cde57371b66af9aed6)
if(NOT FLYENGINE_SOKOL_SHDC)
    if(WIN32)
        set(_shdc_rel win32/sokol-shdc.exe)
        set(_shdc_hash bd616287f9ea689d53c6d260e443ee733e61ae1b73a9b37adc482ead0364d561)
    elseif(APPLE)
        if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_shdc_rel osx_arm64/sokol-shdc)
            set(_shdc_hash 92db37975ad7ff3c3c9bc27cba1503287377cb287ebabf60d1c6b597abfa3244)
        else()
            set(_shdc_rel osx/sokol-shdc)
            set(_shdc_hash 8b4a6ac1172ec0d90dd41d611067d5e87a51e78dc28acb216cfc341d880b1d78)
        endif()
    elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_shdc_rel linux_arm64/sokol-shdc)
        set(_shdc_hash 446b4bcea0c81d3ae529bc0d93533ea661b017f5b9ec2b2293a4c85f5fdcb639)
    else()
        set(_shdc_rel linux/sokol-shdc)
        set(_shdc_hash ed35e89ef381d521a499096ed4ada85e4d135d8011e151cca6b7d893c43b21df)
    endif()
    get_filename_component(_shdc_name ${_shdc_rel} NAME)
    set(_shdc_path ${CMAKE_SOURCE_DIR}/tools/sokol-shdc/${_shdc_name})
    # A binary left over from an earlier (unpinned) download is only kept if it
    # is the pinned one.
    if(EXISTS ${_shdc_path})
        file(SHA256 ${_shdc_path} _shdc_have)
        if(NOT _shdc_have STREQUAL _shdc_hash)
            message(STATUS "sokol-shdc at ${_shdc_path} is not the pinned version; re-downloading")
            file(REMOVE ${_shdc_path})
        endif()
    endif()
    if(NOT EXISTS ${_shdc_path})
        message(STATUS "Downloading sokol-shdc ${FLY_SHDC_COMMIT} (${_shdc_rel})")
        file(DOWNLOAD
             https://raw.githubusercontent.com/floooh/sokol-tools-bin/${FLY_SHDC_COMMIT}/bin/${_shdc_rel}
             ${_shdc_path} STATUS _dl_status TLS_VERIFY ON
             EXPECTED_HASH SHA256=${_shdc_hash})
        list(GET _dl_status 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            file(REMOVE ${_shdc_path})
            message(FATAL_ERROR "Could not download sokol-shdc (${_dl_status}). "
                                "Place the pinned binary at ${_shdc_path} or set -DFLYENGINE_SOKOL_SHDC=<path>.")
        endif()
        if(NOT WIN32)
            file(CHMOD ${_shdc_path} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        endif()
    endif()
    set(FLYENGINE_SOKOL_SHDC ${_shdc_path} CACHE FILEPATH "Path to sokol-shdc" FORCE)
endif()

if(MSVC)
    set(_shdc_errfmt msvc)
else()
    set(_shdc_errfmt gcc)
endif()

set(FLY_SHADER_GEN_DIR ${CMAKE_BINARY_DIR}/shaders_gen)
file(MAKE_DIRECTORY ${FLY_SHADER_GEN_DIR})
set(_shader_headers "")
foreach(src ${FLY_SHADERS})
    get_filename_component(name ${src} NAME_WE)
    set(out ${FLY_SHADER_GEN_DIR}/${name}.glsl.h)
    add_custom_command(
        OUTPUT ${out}
        # Every backend's code is generated (and #ifdef'd), so switching
        # FLYENGINE_GRAPHICS_BACKEND never needs a shader rebuild.
        COMMAND ${FLYENGINE_SOKOL_SHDC} -i ${src} -o ${out} -l glsl410:spirv_vk:hlsl5
                --reflection --ifdef --errfmt=${_shdc_errfmt} --no-log-cmdline
        DEPENDS ${src} ${FLY_SHADER_INCLUDES}
        COMMENT "sokol-shdc ${name}.glsl"
        VERBATIM)
    list(APPEND _shader_headers ${out})
endforeach()
string(REPLACE ";" "|" _shader_headers_arg "${_shader_headers}")
set(FLY_SHADER_REGISTRY ${FLY_SHADER_GEN_DIR}/shaders_registry.cpp)
add_custom_command(
    OUTPUT ${FLY_SHADER_REGISTRY}
    COMMAND ${CMAKE_COMMAND} -DOUT=${FLY_SHADER_REGISTRY} -DHEADERS=${_shader_headers_arg}
            -P ${CMAKE_SOURCE_DIR}/cmake/GenShaderRegistry.cmake
    DEPENDS ${_shader_headers} ${CMAKE_SOURCE_DIR}/cmake/GenShaderRegistry.cmake
    COMMENT "Generating shader registry"
    VERBATIM)
# One target owns the generated files, so several libraries (primary and
# fallback backend) can depend on them without racing to generate them.
add_custom_target(FlyShaders DEPENDS ${FLY_SHADER_REGISTRY} ${_shader_headers})

# --- FlyRL library ----------------------------------------------------------
# The engine code does not depend on which graphics API is used: only this
# library does. fly_add_rl() builds it for one backend, so a second copy can be
# linked into fallback executables (see below).
option(FLYENGINE_VULKAN_VALIDATION "Use VK_LAYER_KHRONOS_validation in non-debug builds too" OFF)

# Headers only: what engine code compiles against.
add_library(FlyRLHeaders INTERFACE)
target_include_directories(FlyRLHeaders INTERFACE ${CMAKE_SOURCE_DIR}/include/rl)

function(fly_add_rl target backend)
    if(backend STREQUAL "VULKAN")
        set(sokol_define SOKOL_VULKAN)
    elseif(backend STREQUAL "GL")
        set(sokol_define SOKOL_GLCORE)
    else()
        set(sokol_define SOKOL_D3D11)
    endif()
add_library(${target} STATIC
    src/Engine/RL/sokol_impl.c
    src/Engine/RL/rl_fallback.c
    src/Engine/RL/sokol_imgui_impl.cpp
    src/Engine/RL/rl_core.cpp
    src/Engine/RL/rl_gfx.cpp
    src/Engine/RL/rl_batch.cpp
    src/Engine/RL/rl_shader.cpp
    src/Engine/RL/rl_textures.cpp
    src/Engine/RL/rl_models.cpp
    src/Engine/RL/rl_misc.cpp
    src/Engine/RL/rl_raymath.c
    # raylib's CPU-side modules (zlib license), compiled against the rlgl
    # subset in include/rl/rlgl.h. Functions that need OpenGL objects are
    # fenced off with RL_SOKOL_BACKEND and implemented in the files above.
    src/Engine/RL/raylib/rshapes.c
    src/Engine/RL/raylib/rtextures.c
    src/Engine/RL/raylib/rtext.c
    src/Engine/RL/raylib/rmodels.c
    ${FLY_SHADER_REGISTRY}
    ${FLY_SHADERS})
set_source_files_properties(${FLY_SHADERS} PROPERTIES HEADER_FILE_ONLY ON)
add_dependencies(${target} FlyShaders)

target_compile_definitions(${target} PUBLIC ${sokol_define} RL_SOKOL_BACKEND)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # sokol_gfx's own validation layer.
    target_compile_definitions(${target} PRIVATE FLY_SOKOL_DEBUG)
endif()
# Khronos validation layers (Vulkan backend). Debug builds already use them
# whenever they are installed (Arch: vulkan-validation-layers, Windows: LunarG
# SDK); this option turns them on in optimized builds too.
if(FLYENGINE_VULKAN_VALIDATION)
    target_compile_definitions(${target} PRIVATE FLY_VULKAN_VALIDATION)
endif()
target_include_directories(${target}
    PUBLIC
        include/rl
    PRIVATE
        sokol
        sokol/util
        src/Engine/RL
        src/Engine/RL/raylib
        ${FLY_SHADER_GEN_DIR})
target_link_libraries(${target} PRIVATE FlyImGui)
set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # raylib's modules and the stb/cgltf/par_shapes headers are third-party C.
    set_source_files_properties(
        src/Engine/RL/raylib/rshapes.c src/Engine/RL/raylib/rtextures.c
        src/Engine/RL/raylib/rtext.c src/Engine/RL/raylib/rmodels.c
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

# --- Platform / API libraries -------------------------------------------------
if(backend STREQUAL "VULKAN")
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        target_link_libraries(${target} PUBLIC Vulkan::Vulkan)
    else()
        # Headers: use the vendored copy when no SDK/system headers exist.
        set(_vk_vendored ${CMAKE_SOURCE_DIR}/src/Engine/Backend/vulkanh/include)
        find_path(FLY_VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h HINTS ENV VULKAN_SDK PATH_SUFFIXES include)
        if(NOT FLY_VULKAN_INCLUDE_DIR AND EXISTS ${_vk_vendored}/vulkan/vulkan.h)
            set(FLY_VULKAN_INCLUDE_DIR ${_vk_vendored})
        endif()
        find_library(FLY_VULKAN_LOADER NAMES vulkan vulkan-1 HINTS ENV VULKAN_SDK PATH_SUFFIXES lib Lib)
        if(NOT FLY_VULKAN_INCLUDE_DIR OR NOT FLY_VULKAN_LOADER)
            message(FATAL_ERROR
                "Vulkan backend: headers or loader not found.\n"
                "  Linux:   install the Vulkan loader (Arch: vulkan-icd-loader, Debian: libvulkan-dev)\n"
                "  Windows: install the LunarG Vulkan SDK\n"
                "or configure with -DFLYENGINE_GRAPHICS_BACKEND=GL")
        endif()
        target_include_directories(${target} PUBLIC ${FLY_VULKAN_INCLUDE_DIR})
        target_link_libraries(${target} PUBLIC ${FLY_VULKAN_LOADER})
    endif()
elseif(backend STREQUAL "GL")
    if(WIN32)
        target_link_libraries(${target} PUBLIC opengl32)
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(${target} PUBLIC OpenGL::GL)
    endif()
elseif(backend STREQUAL "D3D11")
    target_link_libraries(${target} PUBLIC d3d11 dxgi)
endif()

if(WIN32)
    target_link_libraries(${target} PUBLIC kernel32 user32 shell32 gdi32 dwmapi)
elseif(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    if(NOT X11_Xi_FOUND OR NOT X11_Xcursor_FOUND)
        message(FATAL_ERROR "sokol_app needs libXi and libXcursor development files")
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(${target} PUBLIC X11::X11 X11::Xi X11::Xcursor Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
endfunction()

fly_add_rl(FlyRL ${FLY_BACKEND})

# --- Fallback backend --------------------------------------------------------
# sokol's Vulkan backend needs Vulkan 1.3 and VK_EXT_descriptor_buffer. Machines
# without a driver that has them cannot run it, so alongside the Vulkan build we
# make a second set of executables on the universally available API (OpenGL on
# Linux, D3D11 on Windows). On startup the Vulkan build checks the GPU and, if it
# is unsuitable, starts the "-fallback" executable next to it (rl_core.cpp).
option(FLYENGINE_BUILD_FALLBACK_BACKEND
       "With the Vulkan backend, also build Flyengine-fallback / FlyPlayer-fallback (OpenGL; D3D11 on Windows)" ON)
set(FLY_HAS_FALLBACK OFF)
if(FLY_BACKEND STREQUAL "VULKAN" AND FLYENGINE_BUILD_FALLBACK_BACKEND)
    set(FLY_HAS_FALLBACK ON)
    if(WIN32)
        set(FLY_FALLBACK_BACKEND D3D11)
    else()
        set(FLY_FALLBACK_BACKEND GL)
    endif()
    message(STATUS "Fallback backend: ${FLY_FALLBACK_BACKEND}")
    fly_add_rl(FlyRL_fallback ${FLY_FALLBACK_BACKEND})
    target_compile_definitions(FlyRL PRIVATE FLY_FALLBACK_SUFFIX="-fallback")
endif()
