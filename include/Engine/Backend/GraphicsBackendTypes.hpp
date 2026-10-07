#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <optional>

// ============================================================================
// Everything below is BACKEND-AGNOSTIC and therefore lives in namespace gpu,
// never in the global namespace.
//
// Note the name: `gpu`, not `gfx`. namespace gfx already belongs to the engine's
// own render API (include/Engine/Graphics.hpp -- shadow passes, reflection
// passes, instanced city batches) and is used that way in ~20 files. Reusing it
// here would silently merge two unrelated APIs into one namespace and produce
// ambiguities like two different GetLitShader() overload sets. gpu is the
// thin, backend-facing layer underneath that.
//
// This is not cosmetic. The raylib backend includes <raylib.h>, which declares
// its own global `Color`, `Camera3D`, `MouseButton`, `Rectangle`, ... types. If
// these types were global, a translation unit that includes both headers is
// ill-formed (redefinition), so no backend could ever be compiled alongside its
// native API. The Sokol backend hits the same wall with sokol_gfx's `sg_`
// prefix, and the Metal backend with MTLPixelFormat. Namespacing is what makes
// "one engine header, many native backends" possible at all.
//
// Math operations live in namespace math (Math.hpp), which re-exports the types
// below so `math::Vec3` and `gpu::Vec3` name the same thing.
// ============================================================================

namespace gpu {

// ============================================================================
// Math Types (operations in Math.hpp, namespace math)
// ============================================================================

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };
// Layout-compatible with cglm's `mat4` (which is `vec4[4]`, i.e. float[4][4]).
// Standard-layout and 16-byte aligned so it can be reinterpret_cast<> to mat4
// for direct hand-off to cglm/Sokol/Vulkan without a copy.
struct alignas(16) Mat4 { float m[4][4]; };
struct Quat { float x, y, z, w; };
struct Color { uint8_t r, g, b, a; };

// ============================================================================
// Camera
// ============================================================================

enum class ProjectionType : uint8_t {
    Perspective = 0,
    Orthographic = 1
};

struct Camera3D {
    Vec3 position{};
    Vec3 target{};
    Vec3 up{0, 1, 0};
    float fovy = 45.0f;
    ProjectionType projection = ProjectionType::Perspective;
};

// ============================================================================
// Opaque Handles (32-bit, backend-agnostic)
// ============================================================================

using ShaderHandle       = uint32_t;
using PipelineHandle     = uint32_t;
using MeshHandle         = uint32_t;
using TextureHandle      = uint32_t;
using BufferHandle       = uint32_t;  // Vertex, index, uniform, storage, instance
using FramebufferHandle  = uint32_t;
using SamplerHandle      = uint32_t;
using WindowHandle       = uint32_t;

constexpr uint32_t INVALID_HANDLE = 0xFFFFFFFFu;

// ============================================================================
// Enums
// ============================================================================

enum class ShaderStage : uint8_t { Vertex, Fragment, Compute };

enum class UniformType : uint8_t {
    Float, Vec2, Vec3, Vec4,
    Int, IVec2, IVec3, IVec4,
    Mat4, Sampler2D
};

enum class TextureFormat : uint16_t {
    // Uncompressed
    R8, RG8, RGB8, RGBA8,
    R16, RG16, RGB16, RGBA16,
    R16F, RG16F, RGB16F, RGBA16F,
    R32F, RG32F, RGB32F, RGBA32F,
    // sRGB
    RGB8_SRGB, RGBA8_SRGB,
    // Depth/Stencil
    D16, D24, D32F,
    D24S8, D32F_S8,
    // Compressed (BC/ASTC/ETC)
    BC1_RGB, BC1_RGBA,
    BC3, BC4, BC5, BC6H, BC7,
    ETC2_RGB, ETC2_RGBA,
    ASTC_4x4, ASTC_5x4, ASTC_6x5, ASTC_8x5, ASTC_10x5, ASTC_12x10
};

enum class BufferUsage : uint8_t { Static, Dynamic, Stream };
enum class BufferType : uint8_t { Vertex, Index, Uniform, Storage, Instance };

enum class FilterMode : uint8_t { Nearest, Linear, NearestMipNearest, LinearMipLinear };
enum class WrapMode : uint8_t { Clamp, Repeat, MirrorRepeat, ClampToBorder };

enum class CullMode : uint8_t { None, Front, Back };
enum class FrontFace : uint8_t { CCW, CW };

enum class BlendFactor : uint8_t {
    Zero, One,
    SrcColor, OneMinusSrcColor,
    DstColor, OneMinusDstColor,
    SrcAlpha, OneMinusSrcAlpha,
    DstAlpha, OneMinusDstAlpha,
    SrcAlphaSaturate
};

enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };

enum class CompareFunc : uint8_t {
    Never, Less, Equal, LEQual, Greater, NotEqual, GEqual, Always
};

enum class StencilOp : uint8_t {
    Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap
};

enum class IndexType : uint8_t { UInt16, UInt32 };

enum class KeyCode : uint16_t {
    // Printable
    Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
    D0 = 48, D1, D2, D3, D4, D5, D6, D7, D8, D9,
    Semicolon = 59, Equal = 61,
    A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    // Function
    F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, F13, F14, F15,
    F16, F17, F18, F19, F20, F21, F22, F23, F24, F25,
    // Modifiers
    LeftShift = 340, LeftControl, LeftAlt, LeftSuper,
    RightShift, RightControl, RightAlt, RightSuper,
    Menu = 348,
    // Navigation
    Escape = 256, Enter, Tab, Backspace, Insert, Delete,
    Right, Left, Down, Up,
    PageUp, PageDown, Home, End,
    // Keypad
    KP_0 = 320, KP_1, KP_2, KP_3, KP_4, KP_5, KP_6, KP_7, KP_8, KP_9,
    KP_Decimal, KP_Divide, KP_Multiply, KP_Subtract, KP_Add, KP_Enter, KP_Equal,
    // Misc
    CapsLock = 280, ScrollLock, NumLock, PrintScreen, Pause
};

enum class MouseButton : uint8_t { Left = 0, Right, Middle, Side, Extra };
enum class CursorType : uint8_t {
    Arrow, IBeam, Crosshair, Hand, HResize, VResize
};

enum class BackendType : uint8_t { Auto, Raylib, SokolVulkan, SokolMetal, SokolD3D11, SokolGL };

// ============================================================================
// Descriptors (POD, no backend deps)
// ============================================================================

struct VertexAttribute {
    uint32_t location = 0;
    uint32_t offset = 0;
    TextureFormat format = TextureFormat::RGBA32F;  // Reuse format enum
    uint32_t stride = 0;
    bool instanced = false;
    uint32_t instanceDivisor = 1;
};

struct VertexLayout {
    std::vector<VertexAttribute> attributes;
    uint32_t stride = 0;
};

struct UniformMemberDesc {
    std::string name;
    uint32_t offset = 0;
    UniformType type = UniformType::Float;
    uint32_t arrayCount = 1;
};

struct UniformBlockDesc {
    uint32_t binding = 0;
    uint32_t size = 0;
    std::vector<UniformMemberDesc> members;
};

struct TextureBindingDesc {
    uint32_t binding = 0;
    uint32_t set = 0;
};

struct SamplerBindingDesc {
    uint32_t binding = 0;
    uint32_t set = 0;
};

struct ShaderDesc {
    std::string vsSource;           // GLSL source (for hot-reload)
    std::string fsSource;
    std::vector<uint32_t> vsBytecode;  // SPIR-V (for production)
    std::vector<uint32_t> fsBytecode;
    VertexLayout vertexLayout;
    std::vector<UniformBlockDesc> uniformBlocks;
    std::vector<TextureBindingDesc> textures;
    std::vector<SamplerBindingDesc> samplers;
};

struct BufferDesc {
    BufferType type = BufferType::Vertex;
    BufferUsage usage = BufferUsage::Static;
    uint32_t size = 0;
    const void* initialData = nullptr;
    bool persistentMap = false;  // For dynamic buffers
};

struct MeshData {
    std::vector<uint8_t> vertexData;   // Interleaved per layout
    std::vector<uint32_t> indexData;   // 32-bit indices
    VertexLayout layout;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    bool dynamic = false;
};

struct TextureData {
    uint32_t width = 0, height = 0, depth = 1;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    TextureFormat format = TextureFormat::RGBA8;
    BufferUsage usage = BufferUsage::Static;
    std::vector<std::vector<uint8_t>> mipData;  // Per mip level
};

struct FramebufferDesc {
    uint32_t width = 0, height = 0;
    std::vector<TextureHandle> colorAttachments;
    TextureHandle depthAttachment = INVALID_HANDLE;
    uint32_t sampleCount = 1;  // MSAA
};

struct PipelineDesc {
    ShaderHandle shader = INVALID_HANDLE;
    VertexLayout vertexLayout;
    CullMode cullMode = CullMode::Back;
    FrontFace frontFace = FrontFace::CCW;
    bool depthWrite = true;
    CompareFunc depthCompare = CompareFunc::LEQual;
    bool depthTest = true;
    BlendFactor srcBlend = BlendFactor::One;
    BlendFactor dstBlend = BlendFactor::Zero;
    BlendOp blendOp = BlendOp::Add;
    bool blendEnable = false;
    uint32_t sampleCount = 1;
    bool wireframe = false;
};

struct SamplerDesc {
    FilterMode minFilter = FilterMode::Linear;
    FilterMode magFilter = FilterMode::Linear;
    WrapMode wrapU = WrapMode::Repeat;
    WrapMode wrapV = WrapMode::Repeat;
    WrapMode wrapW = WrapMode::Repeat;
    float mipLodBias = 0.0f;
    float maxAnisotropy = 1.0f;
    CompareFunc compareFunc = CompareFunc::Never;  // For shadow samplers
    float minLod = 0.0f;
    float maxLod = 1000.0f;
};

struct RenderPassDesc {
    struct AttachmentOp {
        enum class LoadOp { Load, Clear, DontCare } loadOp = LoadOp::Clear;
        enum class StoreOp { Store, DontCare } storeOp = StoreOp::Store;
        Color clearColor{0, 0, 0, 255};
        float clearDepth = 1.0f;
        uint32_t clearStencil = 0;
    };
    std::vector<AttachmentOp> colorOps;
    AttachmentOp depthOp;
    AttachmentOp stencilOp;
};

struct WindowDesc {
    uint32_t width = 1280;
    uint32_t height = 720;
    const char* title = "Flyengine";
    bool resizable = true;
    bool fullscreen = false;
    bool vsync = true;
    uint32_t msaaSamples = 1;
    BackendType backend = BackendType::Auto;
};

struct DeviceCaps {
    // Limits
    uint32_t maxTextureSize = 0;
    uint32_t maxUniformBufferSize = 0;
    uint32_t maxStorageBufferSize = 0;
    uint32_t maxVertexAttributes = 0;
    uint32_t maxVertexBindings = 0;
    uint32_t maxColorAttachments = 0;
    uint32_t maxDescriptorSets = 0;
    float maxAnisotropy = 1.0f;
    // Features
    bool supportsCompute = false;
    bool supportsGeometryShader = false;
    bool supportsTessellation = false;
    bool supportsBindless = false;
    bool supportsRayTracing = false;
    bool supportsMeshShaders = false;
    // Formats
    bool supportsBC = false;
    bool supportsASTC = false;
    bool supportsETC2 = false;
    // Vendor
    std::string vendorName;
    std::string deviceName;
    std::string driverVersion;
};

// ============================================================================
// Asset Types
// ============================================================================

struct MeshAsset {
    std::vector<MeshData> meshes;  // One per primitive
    std::vector<TextureHandle> textures;  // Pre-loaded
    std::vector<Mat4> nodeTransforms;
    std::vector<std::string> meshNames;
};

struct TextureAsset {
    TextureHandle handle = INVALID_HANDLE;
    uint32_t width = 0, height = 0;
    TextureFormat format = TextureFormat::RGBA8;
    uint32_t mipLevels = 1;
};

} // namespace gpu