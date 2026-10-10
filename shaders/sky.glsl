// The sky behind the scene (src/Engine/Atmosphere.cpp). Drawn as a full-screen
// pass into the float scene target before any geometry.
@module sky
@include fly_common.glsl

@vs vs_fullscreen
@include_block fly_fullscreen_vs
@end

// One colour (the Sky item without an atmosphere, or the editor's own
// background). The colour arrives already taken back through the tone curve, and
// is divided by the exposure here, so it reaches the screen exactly as picked.
@fs fs_flat
layout(binding=0) uniform fs_flat_params {
    vec4 skyHdr;        // rgb = the colour before exposure and tone curve
};
layout(binding=0) uniform texture2D exposureTex;
layout(binding=0) uniform sampler exposureTex_smp;
in vec2 uv;
in vec2 ndc;
out vec4 fragColor;
void main() {
    float e = textureLod(sampler2D(exposureTex, exposureTex_smp), vec2(0.5), 0.0).r;
    fragColor = vec4(skyHdr.rgb / max(e, 1e-4), 1.0);
}
@end
@program sky_flat vs_fullscreen fs_flat
