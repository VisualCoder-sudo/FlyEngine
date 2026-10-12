// raylib texture API on sokol_gfx images.
#include "rl_internal.hpp"
#include "rlgl.h"

#include "sokol_app.h"

#include <algorithm>
#include <cstring>

namespace rli {

namespace {

std::vector<unsigned int> g_pendingIds;
std::vector<unsigned int> g_deferredUpdates;   // dynamic textures updated twice in one frame

// Maps a raylib pixel format to the sokol format it is uploaded as, and the
// raylib format the CPU data must be converted to first (or -1 if none).
struct FormatMapping {
    sg_pixel_format sg;
    int convertTo;
};

FormatMapping MapFormat(int format) {
    switch (format) {
        case PIXELFORMAT_UNCOMPRESSED_R8G8B8A8: return { SG_PIXELFORMAT_RGBA8, -1 };
        case PIXELFORMAT_UNCOMPRESSED_R32: return { SG_PIXELFORMAT_R32F, -1 };
        case PIXELFORMAT_UNCOMPRESSED_R32G32B32A32: return { SG_PIXELFORMAT_RGBA32F, -1 };
        case PIXELFORMAT_UNCOMPRESSED_R32G32B32: return { SG_PIXELFORMAT_RGBA32F, PIXELFORMAT_UNCOMPRESSED_R32G32B32A32 };
        case PIXELFORMAT_UNCOMPRESSED_R16: return { SG_PIXELFORMAT_R16F, -1 };
        case PIXELFORMAT_UNCOMPRESSED_R16G16B16A16: return { SG_PIXELFORMAT_RGBA16F, -1 };
        case PIXELFORMAT_UNCOMPRESSED_R16G16B16: return { SG_PIXELFORMAT_RGBA16F, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16 };
        case PIXELFORMAT_COMPRESSED_DXT1_RGB:
        case PIXELFORMAT_COMPRESSED_DXT1_RGBA: return { SG_PIXELFORMAT_BC1_RGBA, -1 };
        case PIXELFORMAT_COMPRESSED_DXT3_RGBA: return { SG_PIXELFORMAT_BC2_RGBA, -1 };
        case PIXELFORMAT_COMPRESSED_DXT5_RGBA: return { SG_PIXELFORMAT_BC3_RGBA, -1 };
        default:
            // Grayscale, gray+alpha, RGB and 16-bit packed formats: sokol has no
            // RGB8 and no channel swizzles, so expand them to RGBA8 (the same
            // result GL's swizzled grayscale sampling produced).
            return { SG_PIXELFORMAT_RGBA8, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    }
}

bool IsCompressed(int format) { return format >= PIXELFORMAT_COMPRESSED_DXT1_RGB; }

void Materialize(unsigned int id, TextureRec& t) {
    if (t.image.id != 0 || t.pending.empty()) return;
    Image img{};
    img.data = t.pending.data();
    img.width = t.width;
    img.height = t.height;
    img.mipmaps = t.mipmaps;
    img.format = t.format;

    Image withMips{};
    if (t.pendingMips && t.mipmaps <= 1 && !IsCompressed(t.format)) {
        withMips = ImageCopy(img);
        ImageMipmaps(&withMips);
        img = withMips;
    }

    sg_image_desc d{};
    d.width = t.width;
    d.height = t.height;
    d.pixel_format = t.sgFormat;
    d.num_mipmaps = img.mipmaps;
    d.label = "fly-texture";
    if (t.dynamic) {
        d.usage.immutable = false;
        d.usage.dynamic_update = true;
    }
    size_t offset = 0;
    int w = t.width, h = t.height;
    for (int level = 0; level < img.mipmaps && level < SG_MAX_MIPMAPS; ++level) {
        const int size = GetPixelDataSize(w, h, img.format);
        if (!t.dynamic) d.data.mip_levels[level] = { (const uint8_t*)img.data + offset, (size_t)size };
        offset += (size_t)size;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    t.image = sg_make_image(&d);
    if (t.dynamic) {
        sg_image_data data{};
        size_t off = 0;
        w = t.width; h = t.height;
        for (int level = 0; level < img.mipmaps && level < SG_MAX_MIPMAPS; ++level) {
            const int size = GetPixelDataSize(w, h, img.format);
            data.mip_levels[level] = { (const uint8_t*)img.data + off, (size_t)size };
            off += (size_t)size;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        sg_update_image(t.image, &data);
        t.lastUpdateFrame = (uint32_t)Gfx().frameCounter;
    }
    sg_view_desc vd{};
    vd.texture.image = t.image;
    t.view = sg_make_view(&vd);
    t.mipmaps = img.mipmaps;
    if (withMips.data) UnloadImage(withMips);
    std::vector<uint8_t>().swap(t.pending);
    t.pendingMips = false;
    (void)id;
}

} // namespace

TextureRec* GetTextureForBinding(unsigned int id) {
    TextureRec* t = GetTexture(id);
    if (t && t->image.id == 0 && !t->pending.empty()) Materialize(id, *t);
    return (t && t->image.id) ? t : nullptr;
}

void ApplyDeferredTextureUpdates() {
    std::vector<unsigned int> ids;
    ids.swap(g_deferredUpdates);
    for (unsigned int id : ids) {
        TextureRec* t = GetTexture(id);
        if (!t || t->image.id == 0 || t->pending.empty()) continue;
        sg_image_data data{};
        data.mip_levels[0] = { t->pending.data(), t->pending.size() };
        sg_update_image(t->image, &data);
        t->lastUpdateFrame = (uint32_t)Gfx().frameCounter;
        std::vector<uint8_t>().swap(t->pending);
    }
}

void ResetTextureState() {
    g_pendingIds.clear();
    g_deferredUpdates.clear();
}

void MaterializePendingTextures() {
    for (unsigned int id : g_pendingIds) {
        TextureRec* t = GetTexture(id);
        if (t) Materialize(id, *t);
    }
    g_pendingIds.clear();
}

unsigned int CreatePendingTexture(const Image& source) {
    GfxState& g = Gfx();
    if (!g.initialized || !source.data || source.width <= 0 || source.height <= 0) return 0;
    const FormatMapping fm = MapFormat(source.format);
    Image img = source;
    Image converted{};
    if (fm.convertTo >= 0) {
        converted = ImageCopy(source);
        ImageFormat(&converted, fm.convertTo);
        img = converted;
    }
    unsigned int id;
    if (!g.freeTextures.empty()) {
        id = g.freeTextures.back();
        g.freeTextures.pop_back();
    } else {
        g.textures.emplace_back();
        id = (unsigned int)g.textures.size() - 1;
    }
    TextureRec& t = g.textures[id];
    t = TextureRec{};
    t.width = img.width;
    t.height = img.height;
    t.mipmaps = img.mipmaps > 0 ? img.mipmaps : 1;
    t.format = img.format;
    t.sgFormat = fm.sg;
    size_t bytes = 0;
    int w = img.width, h = img.height;
    for (int level = 0; level < t.mipmaps; ++level) {
        bytes += (size_t)GetPixelDataSize(w, h, img.format);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    t.pending.assign((const uint8_t*)img.data, (const uint8_t*)img.data + bytes);
    t.alive = true;
    if (converted.data) UnloadImage(converted);
    g_pendingIds.push_back(id);
    return id;
}

} // namespace rli

using namespace rli;

Texture2D LoadTextureFromImage(Image image) {
    Texture2D tex{};
    if (image.width == 0 || image.height == 0 || !image.data) {
        TraceLog(LOG_WARNING, "IMAGE: Data is not valid to load texture");
        return tex;
    }
    tex.id = CreatePendingTexture(image);
    tex.width = image.width;
    tex.height = image.height;
    tex.mipmaps = image.mipmaps > 0 ? image.mipmaps : 1;
    tex.format = image.format;
    return tex;
}

TextureCubemap LoadTextureCubemap(Image, int) {
    TraceLog(LOG_WARNING, "TEXTURE: cubemaps are not supported by the sokol backend");
    return TextureCubemap{};
}

void UnloadTexture(Texture2D texture) {
    if (texture.id == 0 || texture.id == Gfx().whiteTexture) return;
    TextureRec* t = GetTexture(texture.id);
    if (!t || t->attachment) return;
    DestroyTexture(texture.id);
}

void UpdateTexture(Texture2D texture, const void* pixels) {
    TextureRec* t = GetTexture(texture.id);
    if (!t || !pixels || t->attachment || t->depth) return;
    const int size = GetPixelDataSize(t->width, t->height, t->format);
    if (t->image.id == 0) {
        // Still pending: just replace the CPU copy.
        if (t->pending.size() >= (size_t)size) std::memcpy(t->pending.data(), pixels, (size_t)size);
        return;
    }
    if (!t->dynamic) {
        // First update of an immutable texture: re-create it as updatable,
        // keeping the same id so the caller's Texture2D stays valid.
        if (t->view.id) sg_destroy_view(t->view);
        sg_destroy_image(t->image);
        t->image = {};
        t->view = {};
        t->dynamic = true;
        t->mipmaps = 1;
        t->pending.assign((const uint8_t*)pixels, (const uint8_t*)pixels + size);
        Materialize(texture.id, *t);
        return;
    }
    const uint32_t frame = (uint32_t)Gfx().frameCounter;
    if (t->lastUpdateFrame == frame) {
        // sokol allows one update per image per frame: keep the latest pixels and
        // apply them at the start of the next frame instead of dropping them.
        t->pending.assign((const uint8_t*)pixels, (const uint8_t*)pixels + size);
        if (std::find(g_deferredUpdates.begin(), g_deferredUpdates.end(), texture.id) == g_deferredUpdates.end())
            g_deferredUpdates.push_back(texture.id);
        return;
    }
    sg_image_data data{};
    data.mip_levels[0] = { pixels, (size_t)size };
    sg_update_image(t->image, &data);
    t->lastUpdateFrame = frame;
}

void UpdateTextureRec(Texture2D texture, Rectangle rec, const void* pixels) {
    // Partial updates are only used for full-size rectangles in practice.
    if ((int)rec.x == 0 && (int)rec.y == 0 && (int)rec.width == texture.width && (int)rec.height == texture.height) {
        UpdateTexture(texture, pixels);
    } else {
        TraceLog(LOG_WARNING, "TEXTURE: partial UpdateTextureRec() is not supported by the sokol backend");
    }
}

void GenTextureMipmaps(Texture2D* texture) {
    if (!texture) return;
    TextureRec* t = GetTexture(texture->id);
    if (!t) return;
    if (t->image.id != 0) {
        TraceLog(LOG_WARNING, "TEXTURE: [ID %u] GenTextureMipmaps() after first use has no effect", texture->id);
        return;
    }
    if (IsCompressed(t->format) || t->dynamic) return;
    t->pendingMips = true;
    int levels = 1, w = t->width, h = t->height;
    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        levels++;
    }
    texture->mipmaps = levels;
}

void SetTextureFilter(Texture2D texture, int filter) {
    TextureRec* t = GetTexture(texture.id);
    if (!t) return;
    BatchFlush();
    t->filter = filter;
}

void SetTextureWrap(Texture2D texture, int wrap) {
    TextureRec* t = GetTexture(texture.id);
    if (!t) return;
    BatchFlush();
    t->wrap = wrap;
}

Image LoadImageFromTexture(Texture2D) {
    TraceLog(LOG_WARNING, "TEXTURE: GPU readback is not supported by the sokol backend");
    return Image{};
}

Image LoadImageFromScreen(void) {
    TraceLog(LOG_WARNING, "TEXTURE: screen readback is not supported by the sokol backend");
    return Image{};
}

// ---------------------------------------------------------------------------
// rlgl texture helpers used by raylib's modules
// ---------------------------------------------------------------------------
void rlUnloadTexture(unsigned int id) { UnloadTexture(Texture2D{ id, 1, 1, 1, 1 }); }

const char* rlGetPixelFormatName(unsigned int format) {
    switch (format) {
        case PIXELFORMAT_UNCOMPRESSED_GRAYSCALE: return "GRAYSCALE";
        case PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA: return "GRAY_ALPHA";
        case PIXELFORMAT_UNCOMPRESSED_R5G6B5: return "R5G6B5";
        case PIXELFORMAT_UNCOMPRESSED_R8G8B8: return "R8G8B8";
        case PIXELFORMAT_UNCOMPRESSED_R5G5B5A1: return "R5G5B5A1";
        case PIXELFORMAT_UNCOMPRESSED_R4G4B4A4: return "R4G4B4A4";
        case PIXELFORMAT_UNCOMPRESSED_R8G8B8A8: return "R8G8B8A8";
        case PIXELFORMAT_UNCOMPRESSED_R32: return "R32";
        case PIXELFORMAT_UNCOMPRESSED_R32G32B32: return "R32G32B32";
        case PIXELFORMAT_UNCOMPRESSED_R32G32B32A32: return "R32G32B32A32";
        case PIXELFORMAT_UNCOMPRESSED_R16: return "R16";
        case PIXELFORMAT_UNCOMPRESSED_R16G16B16: return "R16G16B16";
        case PIXELFORMAT_UNCOMPRESSED_R16G16B16A16: return "R16G16B16A16";
        case PIXELFORMAT_COMPRESSED_DXT1_RGB: return "DXT1_RGB";
        case PIXELFORMAT_COMPRESSED_DXT1_RGBA: return "DXT1_RGBA";
        case PIXELFORMAT_COMPRESSED_DXT3_RGBA: return "DXT3_RGBA";
        case PIXELFORMAT_COMPRESSED_DXT5_RGBA: return "DXT5_RGBA";
        default: return "UNKNOWN";
    }
}
