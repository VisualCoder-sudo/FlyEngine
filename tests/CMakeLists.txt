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

# Player jump regression: one jump press after landing from any height must give a full jump.
# Opens a window (engine ctor), so built but not registered with ctest: build/tests/character_jump_test
add_executable(character_jump_test character_jump_test.cpp)
target_link_libraries(character_jump_test PRIVATE FlyEngineCore FlyRL)

# Stepped tower mesh: its base ledge must face up (it was culled from above). Opens a window, so built but
# not registered with ctest: build/tests/city_tower_mesh_test
add_executable(city_tower_mesh_test city_tower_mesh_test.cpp)
target_link_libraries(city_tower_mesh_test PRIVATE FlyEngineCore FlyRL)

# Routed traffic (destinations, trips, route fields, rush hours). Opens a window, so built but not registered
# with ctest: build/tests/city_traffic_route_test
add_executable(city_traffic_route_test city_traffic_route_test.cpp)
target_link_libraries(city_traffic_route_test PRIVATE FlyEngineCore FlyRL)

# Sidewalk furniture placement + close-up screenshots (city_prop_<name>.png). Opens a window, so built but not
# registered with ctest: build/tests/city_props_test
add_executable(city_props_test city_props_test.cpp)
target_link_libraries(city_props_test PRIVATE FlyEngineCore FlyRL)

# Public transit (auto + manual lines, buses serving stops, save/load). Opens a window, so built but not
# registered with ctest: build/tests/city_transit_test
add_executable(city_transit_test city_transit_test.cpp)
target_link_libraries(city_transit_test PRIVATE FlyEngineCore FlyRL)

# Sloped roads on regular and irregular layouts (top-down / oblique screenshots). Opens a window, so built but
# not registered with ctest: build/tests/city_slope_test
add_executable(city_slope_test city_slope_test.cpp)
target_link_libraries(city_slope_test PRIVATE FlyEngineCore FlyRL)

# Numeric audit of the road/pad/park surface on random irregular cities (cliffs, spikes). Opens a window, so built
# but not registered with ctest: build/tests/city_surface_audit
add_executable(city_surface_audit city_surface_audit.cpp)
target_link_libraries(city_surface_audit PRIVATE FlyEngineCore FlyRL)

# Lighting items (sun, ambient, sky, fog): renders and save/load. Opens a window, so built but not registered with
# ctest: build/tests/lighting_test
add_executable(lighting_test lighting_test.cpp)
target_link_libraries(lighting_test PRIVATE FlyEngineCore FlyRL)

# Terrain integration (snap the city to a BasicTerrain, grade limit, shape the terrain to the city). Opens a window,
# so built but not registered with ctest: build/tests/city_terrain_test
add_executable(city_terrain_test city_terrain_test.cpp)
target_link_libraries(city_terrain_test PRIVATE FlyEngineCore FlyRL)

# Sun path, weather, lamps at dusk, water lighting. Opens a window, so it is not registered with ctest: build/tests/weather_test
add_executable(weather_test weather_test.cpp)
target_link_libraries(weather_test PRIVATE FlyEngineCore FlyRL)

# Holes in the city surface (ground plane hidden, magenta sky). Opens a window: build/tests/city_hole_audit
add_executable(city_hole_audit city_hole_audit.cpp)
target_link_libraries(city_hole_audit PRIVATE FlyEngineCore FlyRL)

# Stepped tower facade close-ups. Opens a window: build/tests/city_tower_facade_test
add_executable(city_tower_facade_test city_tower_facade_test.cpp)
target_link_libraries(city_tower_facade_test PRIVATE FlyEngineCore FlyRL)

# Smooth node heights (Max road grade). Opens a window: build/tests/city_grade_test
add_executable(city_grade_test city_grade_test.cpp)
target_link_libraries(city_grade_test PRIVATE FlyEngineCore FlyRL)

# Parks and plazas. Opens a window: build/tests/city_park_test
add_executable(city_park_test city_park_test.cpp)
target_link_libraries(city_park_test PRIVATE FlyEngineCore FlyRL)

# Big-city cost: generation, frame time, draw calls. Opens a window: build/tests/city_lod_test [grid]
add_executable(city_lod_test city_lod_test.cpp)
target_link_libraries(city_lod_test PRIVATE FlyEngineCore FlyRL)

# "City attempting to load [n%]" progress message. Opens a window: build/tests/city_loading_test
add_executable(city_loading_test city_loading_test.cpp)
target_link_libraries(city_loading_test PRIVATE FlyEngineCore FlyRL)

# Unit tests for the compiler-output parser behind the editor's script error
# panel (src/Engine/Scripts/ScriptDiagnostics.cpp). Pure string handling, so it
# links neither FlyEngineCore nor a window.
add_executable(script_diagnostics_test
    script_diagnostics_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Scripts/ScriptDiagnostics.cpp)
target_include_directories(script_diagnostics_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME script_diagnostics COMMAND script_diagnostics_test)
