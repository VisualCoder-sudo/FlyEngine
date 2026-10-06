// Test-only precompiled-header replica.
//
// FlyEngineCore force-includes <raylib.h> and <imgui.h> into every translation
// unit (see target_precompile_headers in the top-level CMakeLists.txt). That
// matters for the backend sources under test: it means a backend TU sees raylib's
// global Color, Camera3D and MouseButton whether or not it includes raylib.h
// itself.
//
// The consequence is a failure mode that is easy to misdiagnose. SokolBackend.cpp
// originally declared `using gpu::MouseButton;` and never included raylib.h, so it
// compiled perfectly on its own -- and then failed inside the real engine build
// with:
//
//     error: 'bool SokolBackend::IsMouseButtonDown(gpu::WindowHandle, int)'
//            marked 'override', but does not override
//
// which points at the interface method rather than at the name collision that
// actually caused it. gpu::MouseButton and raylib's ::MouseButton are distinct
// enums, and with both visible the bare name is ambiguous.
//
// Including this header first from every backend test source reproduces that
// environment, so the collision surfaces here -- as an ordinary compile error --
// rather than surviving until a full engine build. A compile flag cannot do this
// job portably: GCC/Clang's -include takes a single file, and MSVC's /FI takes a
// single file too, so there is no flag form that force-includes two headers
// without a wrapper of some kind.
//
// The gpu type names that collide with a raylib global are exactly Color,
// Camera3D and MouseButton. They must be spelled gpu:: everywhere in a backend
// source; see the note in SokolBackend.cpp.

#include <raylib.h>
#include <imgui.h>
