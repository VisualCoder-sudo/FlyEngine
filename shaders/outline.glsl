// Hover outline: the model drawn again with front faces culled, every vertex pushed outward by a fixed
// number of pixels, in one flat colour. Only the rim around the silhouette stays visible.
@module outline
@include fly_common.glsl

@vs vs
@include_block fly_clip
layout(binding=0) uniform vs_params {
    mat4 mvp;
    vec2 invViewport;   // 2 / width, 2 / height: one pixel in clip space at w = 1
    float thickness;    // pixels
};
in vec3 vertexPosition;
in vec3 vertexNormal;
void main() {
    // Per-face normals split a cube's corners apart; blending in the direction away from the model's
    // centre keeps the hull closed there.
    vec3 dir = normalize(vertexNormal + normalize(vertexPosition + vec3(1e-4)));
    vec4 c = mvp * vec4(vertexPosition, 1.0);
    vec4 tip = mvp * vec4(vertexPosition + dir, 1.0);
    vec2 d = tip.xy / tip.w - c.xy / c.w;
    float len = length(d);
    // Vertices at or behind the camera plane have no usable screen direction (floors reach past the camera).
    if (c.w < 0.05 || tip.w < 0.05) len = 0.0;
    if (len > 1e-5) c.xy += (d / len) * invViewport * thickness * c.w;
    // Nudge the hull back so it never wins the depth test against the surface it hugs (flat or thin
    // shapes put the hull in the same plane as their own faces); only the pixel-wide rim stays visible.
    c.z += c.w * 1e-4;
    gl_Position = fly_clip(c);
}
@end

@fs fs
@include_block fly_color
layout(binding=1) uniform fs_params {
    vec4 colDiffuse;
    float flyLinearTarget;
};
out vec4 finalColor;
void main() {
    finalColor = colDiffuse;
    if (flyLinearTarget > 0.5) finalColor.rgb = fly_srgb_to_linear(finalColor.rgb);
}
@end

@program outline vs fs
