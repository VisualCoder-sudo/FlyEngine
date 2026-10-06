// Copies the offscreen main target to the swapchain (one fullscreen triangle).
@module rl_blit

@vs vs
out vec2 uv;
void main() {
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    uv = pos;
#if !SOKOL_GLSL
    uv.y = 1.0 - uv.y;
#endif
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs fs
layout(binding=0) uniform texture2D tex;
layout(binding=0) uniform sampler tex_smp;
in vec2 uv;
out vec4 frag_color;
void main() {
    frag_color = vec4(texture(sampler2D(tex, tex_smp), uv).rgb, 1.0);
}
@end

@program rl_blit vs fs
