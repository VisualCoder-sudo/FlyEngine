// Shared helpers for FlyEngine's sokol-shdc shaders.
//
// Every shader is written against OpenGL conventions (clip-space depth in
// [-w, w], render targets stored bottom-up). The two helpers below adapt that to
// the backend being compiled for, so the same math -- shadow bias, the road
// depth bias, reflection lookups -- holds on every backend:
//
//   fly_clip(): on Vulkan/D3D11 clip-space depth is [0, w]. Remapping z keeps
//   window-space depth identical to OpenGL's 0.5*ndc+0.5, which is what the
//   depth-comparison math in these shaders assumes.
//
//   fly_rt_uv(): turns an NDC position into the UV of the same point inside a
//   render target. Vulkan/D3D11 store render targets top-down, so V flips.

@block fly_clip
vec4 fly_clip(vec4 p) {
#if SOKOL_GLSL
    return p;
#else
    return vec4(p.x, p.y, (p.z + p.w) * 0.5, p.w);
#endif
}
@end

@block fly_rt_uv
vec2 fly_rt_uv(vec2 ndc) {
    vec2 uv = ndc * 0.5 + 0.5;
#if !SOKOL_GLSL
    uv.y = 1.0 - uv.y;
#endif
    return uv;
}
@end
