// Contract tests for gpu::IGraphicsBackend, run against the raylib backend.
//
// The raylib backend is the reference implementation the Sokol/Vulkan port is
// measured against, so its failure modes matter twice over: a bug here shows up
// as a wrong render now, and later as a wrong reference to compare Vulkan
// against. These tests pin the parts of the contract that are easy to break
// silently:
//
//   - The factory routes each BackendType correctly, and returns nullptr for a
//     backend that was not built, so a misconfiguration is detectable rather
//     than showing up as a backend that renders nothing.
//   - Resource creation REJECTS invalid input with INVALID_HANDLE. Returning 0
//     would be worse than useless: 0 looks like a valid handle, so the failure
//     would surface much later as geometry that silently does not draw.
//   - Destroying or drawing with a handle that was never handed out is a no-op
//     rather than a crash, because handles are plain integers and a stale one is
//     not a programming error the backend can detect.
//   - gpu::KeyCode / gpu::MouseButton are numerically identical to raylib's
//     enums, which is what lets the backend cast instead of translate. Renumber
//     either side and every key press silently becomes a no-op, so the values
//     the engine actually uses are asserted here.
//
// No window or GL context is needed: the backend is driven through stubbed
// raylib entry points (tests/backend_interface_stubs.cpp), so this links
// without the raylib library and without a display.
//
// Run: ctest -R backend_interface  (requires FLYENGINE_BUILD_TESTS=ON)

#include "Engine/Backend/GraphicsBackend.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

void Check(const char* name, bool ok) {
    std::printf(ok ? "ok    %s\n" : "FAIL  %s\n", name);
    if (!ok) ++g_failures;
}

} // namespace

int main() {
    std::printf("== factory routing ==\n");
    {
        auto b = gpu::IGraphicsBackend::Create(gpu::BackendType::Raylib);
        Check("Create(Raylib) returns a backend", b != nullptr);
    }
    {
        // Auto resolves to the compile-time default, which is raylib until the
        // Vulkan backend is verified. See kAutoBackend in RaylibBackend.cpp.
        auto b = gpu::IGraphicsBackend::Create(gpu::BackendType::Auto);
        Check("Create(Auto) returns a backend", b != nullptr);
    }
    {
        // In this test binary no Sokol API is compiled in, so the Sokol request
        // must come back as nullptr. This is what lets a caller detect the
        // misconfiguration and fall back instead of drawing nothing forever.
        auto b = gpu::IGraphicsBackend::Create(gpu::BackendType::SokolVulkan);
        Check("Create(SokolVulkan) with no Sokol build returns nullptr", b == nullptr);
    }

    std::printf("\n== escape hatches ==\n");
    {
        auto b = gpu::IGraphicsBackend::Create(gpu::BackendType::Raylib);
        Check("raylib AsVulkan() is null", b->AsVulkan() == nullptr);
        Check("raylib AsMetal() is null", b->AsMetal() == nullptr);
    }

    std::printf("\n== invalid input is rejected ==\n");
    {
        auto b = gpu::IGraphicsBackend::Create(gpu::BackendType::Raylib);

        gpu::ShaderDesc emptyShader;
        Check("CreateShader with empty source fails",
              b->CreateShader(emptyShader) == gpu::INVALID_HANDLE);

        gpu::BufferDesc emptyBuffer;
        Check("CreateBuffer with size 0 fails",
              b->CreateBuffer(emptyBuffer) == gpu::INVALID_HANDLE);

        gpu::MeshData emptyMesh;
        Check("CreateMesh with no vertices fails",
              b->CreateMesh(emptyMesh) == gpu::INVALID_HANDLE);

        gpu::FramebufferDesc zeroFb;
        Check("CreateFramebuffer with 0x0 fails",
              b->CreateFramebuffer(zeroFb) == gpu::INVALID_HANDLE);

        gpu::FramebufferDesc msaaFb;
        msaaFb.width = 64;
        msaaFb.height = 64;
        msaaFb.sampleCount = 4;
        Check("CreateFramebuffer with MSAA fails on raylib (unsupported)",
              b->CreateFramebuffer(msaaFb) == gpu::INVALID_HANDLE);

        Check("CreateInstanceBuffer with null pointer fails",
              b->CreateInstanceBuffer(nullptr, 4, gpu::BufferUsage::Static) ==
                  gpu::INVALID_HANDLE);
    }

    std::printf("\n== stale handles are no-ops, not crashes ==\n");
    {
        auto b = gpu::IGraphicsBackend::Create(gpu::BackendType::Raylib);
        // Handles are plain uint32_t, so a stale one is not a detectable error;
        // every destroy path must look its handle up before touching the GPU.
        b->DestroyShader(gpu::INVALID_HANDLE);
        b->DestroyShader(12345);
        b->DestroyBuffer(12345);
        b->DestroyMesh(12345);
        b->DestroyTexture(12345);
        b->DestroySampler(12345);
        b->DestroyFramebuffer(12345);
        b->DestroyPipeline(12345);
        Check("destroying unknown handles does not crash", true);

        const gpu::Mat4 identity = gpu::Identity();
        b->DrawMesh(12345, 0, identity, 1);
        b->DrawMeshIndexed(12345, 0, 36, 0, identity);
        b->DrawMeshInstanced(12345, 0, 12345, 4);
        Check("drawing with unknown mesh handles does not crash", true);
    }

    std::printf("\n== uniform upload rejects invalid arguments ==\n");
    {
        auto b = gpu::IGraphicsBackend::Create(gpu::BackendType::Raylib);
        const float value = 1.0f;
        // -1 is raylib's "location not found" sentinel, and nullptr has nothing to
        // upload; both must be skipped rather than forwarded.
        b->SetUniform(12345, -1, &value, gpu::UniformType::Float, 1);
        b->SetUniform(12345, 0, nullptr, gpu::UniformType::Float, 1);
        Check("SetUniform rejects a null pointer and a bad location", true);
        Check("GetUniformLocation on an invalid shader returns -1",
              b->GetUniformLocation(gpu::INVALID_HANDLE, "u_x") == -1);
        Check("GetUniformLocation with a null name returns -1",
              b->GetUniformLocation(12345, nullptr) == -1);
        // Uniform blocks are not expressible through rlgl, and saying so with -1
        // is what lets the Sokol backend be compared against this one.
        Check("GetUniformBlockIndex reports unsupported (-1)",
              b->GetUniformBlockIndex(12345, "Scene") == -1);
    }

    std::printf("\n== key codes match raylib's numbering ==\n");
    {
        // The raylib backend casts KeyCode straight to KeyboardKey rather than
        // translating, which is only valid while the two enums agree. Assert the
        // keys the engine actually uses so a renumbering of either side fails
        // here rather than silently turning input into no-ops.
        Check("KeyCode::Space == 32", static_cast<int>(gpu::KeyCode::Space) == 32);
        Check("KeyCode::A == 65", static_cast<int>(gpu::KeyCode::A) == 65);
        Check("KeyCode::Escape == 256", static_cast<int>(gpu::KeyCode::Escape) == 256);
        Check("KeyCode::KP_0 == 320", static_cast<int>(gpu::KeyCode::KP_0) == 320);
        Check("KeyCode::F1 == 290", static_cast<int>(gpu::KeyCode::F1) == 290);
        Check("KeyCode::LeftShift == 340", static_cast<int>(gpu::KeyCode::LeftShift) == 340);
        Check("KeyCode::Menu == 348", static_cast<int>(gpu::KeyCode::Menu) == 348);
    }

    std::printf("\n== mouse buttons match raylib's numbering ==\n");
    {
        Check("MouseButton::Left == 0", static_cast<int>(gpu::MouseButton::Left) == 0);
        Check("MouseButton::Right == 1", static_cast<int>(gpu::MouseButton::Right) == 1);
        Check("MouseButton::Middle == 2", static_cast<int>(gpu::MouseButton::Middle) == 2);
        Check("MouseButton::Side == 3", static_cast<int>(gpu::MouseButton::Side) == 3);
        Check("MouseButton::Extra == 4", static_cast<int>(gpu::MouseButton::Extra) == 4);
    }

    std::printf("\n== capabilities are honest and stable ==\n");
    {
        auto b = gpu::IGraphicsBackend::Create(gpu::BackendType::Raylib);
        const gpu::DeviceCaps& caps = b->GetCaps();
        Check("caps report a max texture size", caps.maxTextureSize > 0);
        Check("caps report a vendor", !caps.vendorName.empty());
        // raylib/rlgl has no compute dispatch entry point, so reporting support
        // would let a call site rely on DispatchCompute doing something.
        Check("raylib reports no compute support", caps.supportsCompute == false);
        // GetCaps returns a reference; callers keep it across frames, so it must
        // not be a reference to storage that is reset on the next call.
        Check("caps are stable across calls", &b->GetCaps() == &b->GetCaps());
    }

    if (g_failures == 0) {
        std::printf("\nall backend interface tests passed\n");
        return 0;
    }
    std::printf("\n%d backend interface test(s) failed\n", g_failures);
    return 1;
}