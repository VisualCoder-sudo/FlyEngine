# Test executables.
#
# These link FlyEngineCore so they exercise the same code the editor and player
# do, rather than a copy that can drift. They are separate targets from the
# editor/player so `ctest` can run them without a window or a GL context.
#
# Only enabled when the top-level FLYENGINE_BUILD_TESTS option is ON, because
# building them means building the whole engine core.

# Unit tests for TechTools::MemoryTracker (include/Engine/TechnicalTools.hpp,
# src/Engine/TechnicalTools.cpp). The tracker only calls raylib's pure size
# helpers and reads /proc, so it needs no window; only Draw() is out of scope
# because that needs an ImGui context.
add_executable(memory_tracker_test memory_tracker_test.cpp)

# The callstack capture resolves symbols through dladdr(), which can only see
# symbols in the dynamic symbol table. This mirrors ENABLE_EXPORTS on FlyPlayer
# so the test exercises the same symbol resolution the shipping player gets.
if(UNIX AND NOT APPLE)
    set_target_properties(memory_tracker_test PROPERTIES ENABLE_EXPORTS ON)
endif()

target_link_libraries(memory_tracker_test PRIVATE FlyEngineCore FlyRL)

add_test(NAME memory_tracker COMMAND memory_tracker_test)

# Parity tests for the backend math layer (include/Engine/Backend/Math.hpp).
#
# These pin down the raylib <-> column-major cglm conversion that the Sokol
# migration depends on. cglm is header-only and this needs no window or GL
# context, so it deliberately does NOT link FlyEngineCore: it must stay runnable
# even while the engine itself is mid-migration and not compiling.
#
# raymath.h is header-only too (RMAPI is empty in a header-only build), so no
# raylib library is linked either.
add_executable(math_backend_parity_test math_backend_parity_test.cpp)

target_include_directories(math_backend_parity_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/Engine/Backend
    ${CMAKE_SOURCE_DIR}/src/Engine/Backend/cglm/include
    ${CMAKE_SOURCE_DIR}/include/rl
)

add_test(NAME math_backend_parity COMMAND math_backend_parity_test)

# Depth-range tests for the backend math layer.
#
# OpenGL projects to NDC depth [-1, +1]; Vulkan and Metal project to [0, +1].
# Both backends are compiled into one binary, so the range is chosen per call
# site (math::Perspective vs math::PerspectiveZO) rather than by a build-wide
# macro. A mismatch does not fail to compile -- it renders at the wrong depth --
# so these assertions exist to catch it. Same reasoning as above for not
# linking FlyEngineCore: header-only, no window, no GL context.
add_executable(projection_depth_range_test projection_depth_range_test.cpp)

target_include_directories(projection_depth_range_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/Engine/Backend
    ${CMAKE_SOURCE_DIR}/src/Engine/Backend/cglm/include
)

add_test(NAME projection_depth_range COMMAND projection_depth_range_test)

# Windowed smoke test for the sokol-backed raylib layer. Needs a display and a
# GPU, so it is built but deliberately NOT registered with ctest (CI runners
# have no GPU); run it by hand: build/tests/rl_smoke_test
add_executable(rl_smoke_test rl_smoke_test.cpp)
target_link_libraries(rl_smoke_test PRIVATE FlyRL)
target_compile_definitions(rl_smoke_test PRIVATE FLY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Known-answer test for the texture manager's SHA-256.
add_executable(texture_hash_test texture_hash_test.cpp)
target_link_libraries(texture_hash_test PRIVATE FlyEngineCore FlyRL)
add_test(NAME texture_hash COMMAND texture_hash_test)

# Property test for the city polygon helpers (park outlines / triangulation).
add_executable(city_geometry_test city_geometry_test.cpp
    ${CMAKE_SOURCE_DIR}/src/CityGen/CityGeometry.cpp
    ${CMAKE_SOURCE_DIR}/src/Terrain/Water/WaterNoise.cpp)
target_include_directories(city_geometry_test PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/include/rl)
add_test(NAME city_geometry COMMAND city_geometry_test)

# Districts (downtown/suburb height + style, painting, save/load). Runs the real City code; needs a GL context
# for the mesh upload, so it is built but not registered with ctest: build/tests/city_district_test
add_executable(city_district_test city_district_test.cpp)
target_link_libraries(city_district_test PRIVATE FlyEngineCore FlyRL)
