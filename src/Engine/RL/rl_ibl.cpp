// Image-based lighting: the sky and the ground baked into cube maps for the lit
// shaders (fly_ibl in shaders/fly_common.glsl), drawn by shaders/ibl.glsl.
//
// Written straight against sokol_gfx rather than through the raylib layer: each cube
// pass draws all six faces at once into six colour attachments (one per face of one
// mip), which the single-attachment render targets of the raylib layer cannot. A bake
// is 1 + 6 + 1 + 6 passes on a GPU with six attachments (most), or six times as many
// on one with fewer.
//
// Every cube pass LOADs its attachments: sokol's Vulkan backend transitions a whole
// image at once, and a CLEAR (or DONTCARE) would let it discard the faces and mips
// drawn by the passes before. The passes overwrite every texel anyway.
#include "rl_internal.hpp"

#include "ibl.glsl.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace rli {

namespace {

constexpr int kEnvSize = 128;        // the capture's face size (mip 0)
constexpr int kEnvMips = 6;          // 128 .. 4
constexpr int kSpecSize = 128;       // the prefiltered cube
constexpr int kSpecMips = 6;         // roughness 0, 0.2, .. 1
constexpr int kIrrSize = 32;
constexpr int kLutSize = 128;
constexpr int kPrefilterSamples = 64;
constexpr sg_pixel_format kFormat = SG_PIXELFORMAT_RGBA16F;

struct Ibl {
    bool ready = false;              // resources exist
    bool baked = false;              // the cubes hold a bake
    bool sixFaces = true;            // the GPU takes six colour attachments
    unsigned int envTex = 0, specTex = 0, irrTex = 0, lutTex = 0, scaleTex = 0;   // TextureRec ids
    sg_view envAtt[kEnvMips][6]{};
    sg_view specAtt[kSpecMips][6]{};
    sg_view irrAtt[6]{};
    sg_view lutAtt{}, scaleAtt{};
    sg_shader normShd{}, capShd{}, irrShd{}, preShd{}, brdfShd{};
    sg_pipeline normPip{}, capPip{}, irrPip{}, prePip{}, brdfPip{};
    sg_sampler linearClamp{}, trilinearClamp{};
};
Ibl g_ibl;

unsigned int MakeCube(int size, int mips, const char* label) {
    sg_image_desc d{};
    d.type = SG_IMAGETYPE_CUBE;
    d.usage.color_attachment = true;
    d.width = size;
    d.height = size;
    d.num_mipmaps = mips;
    d.pixel_format = kFormat;
    d.label = label;
    const unsigned int id = CreateTexture(d, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, false, true);
    TextureRec& t = *GetTexture(id);
    t.cube = true;
    t.filter = mips > 1 ? TEXTURE_FILTER_TRILINEAR : TEXTURE_FILTER_BILINEAR;
    t.wrap = TEXTURE_WRAP_CLAMP;
    return id;
}

unsigned int Make2D(int size, const char* label) {
    sg_image_desc d{};
    d.usage.color_attachment = true;
    d.width = size;
    d.height = size;
    d.pixel_format = kFormat;
    d.label = label;
    const unsigned int id = CreateTexture(d, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, false, true);
    TextureRec& t = *GetTexture(id);
    t.filter = TEXTURE_FILTER_BILINEAR;
    t.wrap = TEXTURE_WRAP_CLAMP;
    return id;
}

sg_view Attachment(unsigned int tex, int mip, int face) {
    sg_view_desc vd{};
    vd.color_attachment.image = GetTexture(tex)->image;
    vd.color_attachment.mip_level = mip;
    vd.color_attachment.slice = face;
    vd.label = "fly-ibl-attachment";
    return sg_make_view(&vd);
}

sg_pipeline Pipeline(sg_shader shd, int colors, const char* label) {
    sg_pipeline_desc d{};
    d.shader = shd;
    d.color_count = colors;
    for (int i = 0; i < colors; ++i) d.colors[i].pixel_format = kFormat;
    d.depth.pixel_format = SG_PIXELFORMAT_NONE;
    d.sample_count = 1;
    d.label = label;
    return sg_make_pipeline(&d);
}

bool Valid(sg_pipeline p) { return sg_query_pipeline_state(p) == SG_RESOURCESTATE_VALID; }

bool Init() {
    Ibl& b = g_ibl;
    if (b.ready) return true;
    if (!sg_query_pixelformat(kFormat).render || !sg_query_pixelformat(kFormat).filter) {
        TraceLog(LOG_WARNING, "IBL: RGBA16F is not renderable and filterable here; no baked environment");
        return false;
    }
    const sg_backend be = sg_query_backend();
    // FLY_IBL_ONE_FACE=1 takes the one-face-per-pass path even where six attachments are available (debugging aid).
    b.sixFaces = sg_query_limits().max_color_attachments >= 6 && !std::getenv("FLY_IBL_ONE_FACE");
    b.normShd = sg_make_shader(ibl_ibl_norm_shader_desc(be));
    b.capShd = sg_make_shader(b.sixFaces ? ibl_ibl_capture6_shader_desc(be) : ibl_ibl_capture1_shader_desc(be));
    b.irrShd = sg_make_shader(b.sixFaces ? ibl_ibl_irradiance6_shader_desc(be) : ibl_ibl_irradiance1_shader_desc(be));
    b.preShd = sg_make_shader(b.sixFaces ? ibl_ibl_prefilter6_shader_desc(be) : ibl_ibl_prefilter1_shader_desc(be));
    b.brdfShd = sg_make_shader(ibl_ibl_brdf_shader_desc(be));
    const int faces = b.sixFaces ? 6 : 1;
    b.normPip = Pipeline(b.normShd, 1, "fly-ibl-norm");
    b.capPip = Pipeline(b.capShd, faces, "fly-ibl-capture");
    b.irrPip = Pipeline(b.irrShd, faces, "fly-ibl-irradiance");
    b.prePip = Pipeline(b.preShd, faces, "fly-ibl-prefilter");
    b.brdfPip = Pipeline(b.brdfShd, 1, "fly-ibl-brdf");
    if (!Valid(b.normPip) || !Valid(b.capPip) || !Valid(b.irrPip) || !Valid(b.prePip) || !Valid(b.brdfPip)) {
        TraceLog(LOG_WARNING, "IBL: shaders failed to build for %s; no baked environment", GetGraphicsBackendName());
        return false;
    }

    b.envTex = MakeCube(kEnvSize, kEnvMips, "fly-ibl-env");
    b.specTex = MakeCube(kSpecSize, kSpecMips, "fly-ibl-specular");
    b.irrTex = MakeCube(kIrrSize, 1, "fly-ibl-irradiance");
    b.lutTex = Make2D(kLutSize, "fly-ibl-brdf");
    b.scaleTex = Make2D(1, "fly-ibl-sky-scale");
    for (int m = 0; m < kEnvMips; ++m)
        for (int f = 0; f < 6; ++f) b.envAtt[m][f] = Attachment(b.envTex, m, f);
    for (int m = 0; m < kSpecMips; ++m)
        for (int f = 0; f < 6; ++f) b.specAtt[m][f] = Attachment(b.specTex, m, f);
    for (int f = 0; f < 6; ++f) b.irrAtt[f] = Attachment(b.irrTex, 0, f);
    b.lutAtt = Attachment(b.lutTex, 0, 0);
    b.scaleAtt = Attachment(b.scaleTex, 0, 0);

    sg_sampler_desc sd{};
    sd.min_filter = SG_FILTER_LINEAR;
    sd.mag_filter = SG_FILTER_LINEAR;
    sd.mipmap_filter = SG_FILTER_NEAREST;
    sd.wrap_u = sd.wrap_v = sd.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    sd.label = "fly-ibl-linear";
    b.linearClamp = sg_make_sampler(&sd);
    sd.mipmap_filter = SG_FILTER_LINEAR;
    sd.label = "fly-ibl-trilinear";
    b.trilinearClamp = sg_make_sampler(&sd);
    b.ready = true;
    return true;
}

// One full-screen triangle into `atts` (all LOADed, see the top of the file). `bind` may be null (no textures).
void Pass(const sg_view* atts, int count, sg_pipeline pip, const sg_bindings* bind, const void* ub, size_t ubSize, const char* label) {
    sg_pass pass{};
    for (int i = 0; i < count; ++i) {
        pass.attachments.colors[i] = atts[i];
        pass.action.colors[i].load_action = SG_LOADACTION_LOAD;
        pass.action.colors[i].store_action = SG_STOREACTION_STORE;
    }
    pass.label = label;
    sg_begin_pass(&pass);
    sg_apply_pipeline(pip);
    if (bind) sg_apply_bindings(bind);
    sg_apply_uniforms(0, sg_range{ ub, ubSize });
    sg_draw(0, 3, 1);
    sg_end_pass();
}

// Each cube pass: six faces of one mip at once, or one face at a time.
template <typename Params>
void CubePasses(sg_view (&atts)[6], sg_pipeline pip, const sg_bindings& bind, Params params, const char* label) {
    if (g_ibl.sixFaces) {
        Pass(atts, 6, pip, &bind, &params, sizeof(params), label);
        return;
    }
    for (int f = 0; f < 6; ++f) {
        params.iblFace[1] = (float)f;
        Pass(&atts[f], 1, pip, &bind, &params, sizeof(params), label);
    }
}

void BakeBrdfLut() {
    Ibl& b = g_ibl;
    ibl_fs_brdf_params_t p{};
    p.iblFace[0] = (float)kLutSize;
    Pass(&b.lutAtt, 1, b.brdfPip, nullptr, &p, sizeof(p), "fly-ibl-brdf");
    GetTexture(b.lutTex)->rendered = true;
}

void Copy3(float* dst, const float* src) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; }

} // namespace

void IblShutdown() {
    // sg_shutdown() releases the sokol objects; the textures go with GfxState.
    g_ibl = Ibl{};
}

} // namespace rli

using namespace rli;

void BakeEnvironment(const FlyEnvironment* env) {
    GfxState& g = Gfx();
    if (!env || !g.initialized || !Init()) return;
    Ibl& b = g_ibl;
    BatchFlush();
    EndPass();

    if (!GetTexture(b.lutTex)->rendered) BakeBrdfLut();

    // The sky-view table, when there is an atmosphere to read.
    sg_view skyView = GetTextureForBinding(g.whiteTexture)->view;
    if (env->atmosphere) {
        TextureRec* t = GetTextureForBinding(env->skyView.id);
        if (t && !t->cube && !t->volume && !t->depth && (!t->attachment || t->rendered)) skyView = t->view;
    }
    const float mode[4] = { env->atmosphere ? 1.0f : 0.0f, std::clamp(env->overcast, 0.0f, 1.0f), 0.0f, 0.0f };

    // 1. How much the raw sky has to be scaled by.
    {
        ibl_fs_norm_params_t p{};
        std::memcpy(p.envMode, mode, sizeof(mode));
        std::memcpy(p.atPlanet, env->planet, sizeof(p.atPlanet));
        Copy3(p.envZenith, env->zenith);
        Copy3(p.envHorizon, env->horizon);
        Copy3(p.envAmbientSky, env->ambientSky);
        sg_bindings bind{};
        bind.views[VIEW_ibl_skyViewLut] = skyView;
        bind.samplers[SMP_ibl_skyViewLut_smp] = b.linearClamp;
        Pass(&b.scaleAtt, 1, b.normPip, &bind, &p, sizeof(p), "fly-ibl-norm");
    }

    // 2. The surroundings, every mip drawn directly (the sky is smooth: nothing to alias).
    {
        ibl_fs_capture_params_t p{};
        std::memcpy(p.envMode, mode, sizeof(mode));
        std::memcpy(p.atPlanet, env->planet, sizeof(p.atPlanet));
        Copy3(p.envZenith, env->zenith);
        Copy3(p.envHorizon, env->horizon);
        Copy3(p.envGround, env->ambientGround);
        sg_bindings bind{};
        bind.views[VIEW_ibl_skyViewLut] = skyView;
        bind.samplers[SMP_ibl_skyViewLut_smp] = b.linearClamp;
        bind.views[VIEW_ibl_skyScale] = GetTexture(b.scaleTex)->view;
        bind.samplers[SMP_ibl_skyScale_smp] = b.linearClamp;
        for (int m = 0; m < kEnvMips; ++m) {
            p.iblFace[0] = (float)(kEnvSize >> m);
            CubePasses(b.envAtt[m], b.capPip, bind, p, "fly-ibl-capture");
        }
    }

    sg_bindings envBind{};
    envBind.views[VIEW_ibl_envCube] = GetTexture(b.envTex)->view;
    envBind.samplers[SMP_ibl_envCube_smp] = b.trilinearClamp;

    // 3. Diffuse: read from an 8x8 mip, plenty for light this smooth.
    {
        ibl_fs_irradiance_params_t p{};
        p.iblFace[0] = (float)kIrrSize;
        p.iblFace[2] = 4.0f;
        CubePasses(b.irrAtt, b.irrPip, envBind, p, "fly-ibl-irradiance");
    }

    // 4. Specular: one roughness per mip.
    {
        ibl_fs_prefilter_params_t p{};
        p.iblFilter[1] = (float)kEnvSize;
        p.iblFilter[2] = (float)(kEnvMips - 1);
        p.iblFilter[3] = (float)kPrefilterSamples;
        for (int m = 0; m < kSpecMips; ++m) {
            p.iblFace[0] = (float)(kSpecSize >> m);
            p.iblFilter[0] = (float)m / (float)(kSpecMips - 1);
            CubePasses(b.specAtt[m], b.prePip, envBind, p, "fly-ibl-prefilter");
        }
    }

    GetTexture(b.envTex)->rendered = true;
    GetTexture(b.specTex)->rendered = true;
    GetTexture(b.irrTex)->rendered = true;
    GetTexture(b.scaleTex)->rendered = true;
    if (!b.baked) TraceLog(LOG_INFO, "IBL: environment baked on %s (%s)", GetGraphicsBackendName(),
                           b.sixFaces ? "six faces per pass" : "one face per pass");
    b.baked = true;
    ResetAppliedState();
}

bool IsEnvironmentBaked(void) { return g_ibl.baked; }

TextureCubemap GetEnvSpecular(void) {
    if (!g_ibl.baked) return TextureCubemap{};
    return TextureCubemap{ g_ibl.specTex, kSpecSize, kSpecSize, kSpecMips, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16 };
}

TextureCubemap GetEnvIrradiance(void) {
    if (!g_ibl.baked) return TextureCubemap{};
    return TextureCubemap{ g_ibl.irrTex, kIrrSize, kIrrSize, 1, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16 };
}

Texture2D GetEnvBrdfLut(void) {
    if (!g_ibl.baked) return Texture2D{};
    return Texture2D{ g_ibl.lutTex, kLutSize, kLutSize, 1, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16 };
}

int GetEnvSpecularMips(void) { return kSpecMips; }
