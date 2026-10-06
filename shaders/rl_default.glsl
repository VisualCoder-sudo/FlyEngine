// raylib's default shader: texture * tint * vertex color. Used by the
// immediate-mode batch (shapes, text, debug lines) and by meshes drawn with a
// default material.
@module rl_default
@include fly_common.glsl

@vs vs
@include_block fly_clip
layout(binding=0) uniform vs_params {
    mat4 mvp;
};
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
out vec2 fragTexCoord;
out vec4 fragColor;
void main() {
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = fly_clip(mvp * vec4(vertexPosition, 1.0));
}
@end

@fs fs
layout(binding=1) uniform fs_params {
    vec4 colDiffuse;
};
layout(binding=0) uniform texture2D texture0;
layout(binding=0) uniform sampler texture0_smp;
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;
void main() {
    finalColor = texture(sampler2D(texture0, texture0_smp), fragTexCoord) * colDiffuse * fragColor;
}
@end

@program rl_default vs fs
