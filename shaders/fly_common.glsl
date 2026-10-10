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

// The scene is lit in linear light and stored in float render targets; colours
// that people pick (vertex colours, tints, textures) are sRGB. These convert.
@block fly_color
vec3 fly_srgb_to_linear(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
vec3 fly_linear_to_srgb(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
float fly_luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
@end

// Vertex stage of every full-screen pass (DrawFullscreen): one triangle that
// covers the target. `uv` addresses the same point in another render target of
// any size; `ndc` is its clip-space position (y up on every backend).
@block fly_fullscreen_vs
out vec2 uv;
out vec2 ndc;
void main() {
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    ndc = pos * 2.0 - 1.0;
    uv = pos;
#if !SOKOL_GLSL
    uv.y = 1.0 - uv.y;
#endif
    gl_Position = vec4(ndc, 0.0, 1.0);
}
@end

// Camera of the frame, for full-screen passes that need world positions:
//   camProj   = (1/P00, 1/P11, P20, P21) of the projection, jitter included
//               (orthographic: the last two are P30, P31)
//   camDepth  = (P22, P32, orthographic ? 1 : 0, far plane)
//   camRight/camUp/camFwd = the camera's axes in world space, camPos its position.
// Window depth is 0.5 * ndc.z + 0.5 on every backend (see fly_clip).
@block fly_camera
// Distance along the view axis (metres) of a depth-buffer value.
float fly_view_depth(float depth, vec4 camDepth) {
    float z = depth * 2.0 - 1.0;
    if (camDepth.z > 0.5) return -(z - camDepth.y) / camDepth.x;
    return camDepth.y / (z + camDepth.x);
}
// View-space position of a pixel: xy scale with the view depth (perspective) or not (orthographic).
vec3 fly_view_pos(vec2 ndcPos, float viewDepth, vec4 camProj, vec4 camDepth) {
    if (camDepth.z > 0.5) return vec3((ndcPos.x - camProj.z) * camProj.x, (ndcPos.y - camProj.w) * camProj.y, -viewDepth);
    return vec3((ndcPos.x + camProj.z) * camProj.x * viewDepth, (ndcPos.y + camProj.w) * camProj.y * viewDepth, -viewDepth);
}
@end
