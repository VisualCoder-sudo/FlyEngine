// Lit shader family (gfx::GetLitShader / GetRoadShader / instanced city
// buildings): directional light, ambient, 3x3 PCF shadow map and a subtle
// underwater tint. Ported from the GLSL 330 sources that used to be embedded in
// src/Engine/Graphics.cpp.
@module lit
@include fly_common.glsl

@block lit_vs_outputs
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;
out vec4 fragShadowCoord;
out vec3 fragWorldPos;
@end

@block lit_vs_inputs
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
@end

@vs vs
@include_block fly_clip
layout(binding=0) uniform vs_params {
    mat4 mvp;
    mat4 matNormal;
    mat4 matModel;
    mat4 lightVP;
};
@include_block lit_vs_inputs
@include_block lit_vs_outputs
void main() {
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    fragShadowCoord = lightVP * matModel * vec4(vertexPosition, 1.0);
    fragWorldPos = vec3(matModel * vec4(vertexPosition, 1.0));
    gl_Position = fly_clip(mvp * vec4(vertexPosition, 1.0));
}
@end

// Road variant: identical outputs plus the height above road elevation, which
// orders the stacked road layers in the fragment depth bias.
@vs vs_road
@include_block fly_clip
layout(binding=0) uniform vs_params {
    mat4 mvp;
    mat4 matNormal;
    mat4 matModel;
    mat4 lightVP;
};
@include_block lit_vs_inputs
@include_block lit_vs_outputs
out float fragLayer;
void main() {
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    fragShadowCoord = lightVP * matModel * vec4(vertexPosition, 1.0);
    fragWorldPos = vec3(matModel * vec4(vertexPosition, 1.0));
    gl_Position = fly_clip(mvp * vec4(vertexPosition, 1.0));
    fragLayer = max(vertexPosition.y - 0.04, 0.0);
}
@end

// Instanced variant: the model matrix comes from the per-instance transform.
// The per-instance tint rides in the (otherwise unused) bottom row of the
// transform, so every colour of a shape shares one draw call; an all-zero tint
// (e.g. editor markers) means "no per-instance tint".
@vs vs_instanced
@include_block fly_clip
layout(binding=0) uniform vs_inst_params {
    mat4 mvp;
    mat4 lightVP;
};
@include_block lit_vs_inputs
in vec4 instanceTransform0;
in vec4 instanceTransform1;
in vec4 instanceTransform2;
in vec4 instanceTransform3;
@include_block lit_vs_outputs
void main() {
    // Built from column vectors rather than by assigning model[i][3]: writing
    // individual elements of a mat4 was miscompiled by at least one OpenGL driver
    // (NVIDIA), which lost the instance's scale whenever a tint was present.
    vec3 instTint = vec3(instanceTransform0.w, instanceTransform1.w, instanceTransform2.w);
    mat4 model = mat4(vec4(instanceTransform0.xyz, 0.0),
                      vec4(instanceTransform1.xyz, 0.0),
                      vec4(instanceTransform2.xyz, 0.0),
                      instanceTransform3);
    if (dot(instTint, instTint) < 1e-6) instTint = vec3(1.0);

    vec4 worldPos = model * vec4(vertexPosition, 1.0);
    fragTexCoord = vertexTexCoord;
    fragColor = vec4(vertexColor.rgb * instTint, vertexColor.a);
    fragNormal = normalize(mat3(model) * vertexNormal);
    fragShadowCoord = lightVP * worldPos;
    fragWorldPos = worldPos.xyz;
    gl_Position = fly_clip(mvp * worldPos);
}
@end

@block lit_fs_common
@include_block fly_rt_uv
layout(binding=0) uniform texture2D texture0;
layout(binding=0) uniform sampler texture0_smp;
layout(binding=1) uniform texture2D shadowMap;
layout(binding=1) uniform sampler shadowMap_smp;
@image_sample_type shadowMap depth
@sampler_type shadowMap_smp comparison

in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;
in vec4 fragShadowCoord;
in vec3 fragWorldPos;
out vec4 finalColor;

float ShadowCalculation(vec3 normal) {
    // Combined anti-acne scheme (shared by every lit mesh).
    //
    // (A) NORMAL-BASED BIAS - shift the shadow comparison point along the
    //     surface normal in WORLD space before projecting to light space, a
    //     fixed world-units offset that stays correct at every camera altitude.
    vec4 biasedLightPos = lightVP * vec4(fragWorldPos + normalize(normal) * 0.02, 1.0);
    vec3 ndc = biasedLightPos.xyz / biasedLightPos.w;
    vec2 uvGL = ndc.xy * 0.5 + 0.5;
    float currentDepth = ndc.z * 0.5 + 0.5;
    float shadow = 0.0;

    if (uvGL.x >= 0.0 && uvGL.x <= 1.0 && uvGL.y >= 0.0 && uvGL.y <= 1.0 && currentDepth <= 1.0) {
        // (B) SLOPE-SCALE DEPTH BIAS - grows as the surface tilts away from the
        //     light, where acne is worst.
        float facing = 1.0 - dot(normal, normalize(-lightDir));
        float bias = max(0.0037 * facing * facing, 0.00093); // tuned for the 1..430 shadow depth range
        vec2 uv = fly_rt_uv(ndc.xy);
        vec2 texelSize = 1.0 / vec2(textureSize(sampler2DShadow(shadowMap, shadowMap_smp), 0));

        // 3x3 PCF; each tap is itself a filtered hardware comparison.
        for (int x = -1; x <= 1; ++x) {
            for (int y = -1; y <= 1; ++y) {
                vec3 tap = vec3(uv + vec2(x, y) * texelSize, currentDepth - bias);
                shadow += 1.0 - texture(sampler2DShadow(shadowMap, shadowMap_smp), tap);
            }
        }
        shadow /= 9.0;
    }
    return shadow;
}

vec3 ShadeLit(out float alpha) {
    vec3 normal = normalize(fragNormal);
    float diffuse = max(dot(normal, -lightDir), 0.0);
    float shadow = ShadowCalculation(normal) * shadowsEnabled;

    vec4 texelColor = texture(sampler2D(texture0, texture0_smp), fragTexCoord);
    vec3 lit = (ambient + (1.0 - shadow) * diffuse) * texelColor.rgb * colDiffuse.rgb * fragColor.rgb;

    float depthBelow = waterSurfaceY - fragWorldPos.y;
    if (depthBelow > 0.0) {
        float t = clamp(depthBelow * 0.3, 0.0, 1.0);
        vec3 waterTint = vec3(0.6, 0.75, 0.9);
        lit = mix(lit, lit * waterTint, t * 0.25);
        float luma = dot(lit, vec3(0.299, 0.587, 0.114));
        lit = mix(lit, vec3(luma), t * 0.1);
    }
    alpha = texelColor.a * colDiffuse.a;
    return lit;
}
@end

@fs fs
layout(binding=1) uniform fs_params {
    vec4 colDiffuse;
    vec3 lightDir;
    float shadowsEnabled;
    vec3 ambient;
    float waterSurfaceY;
    mat4 lightVP;
};
@include_block lit_fs_common
void main() {
    float alpha;
    vec3 lit = ShadeLit(alpha);
    finalColor = vec4(lit, alpha);
}
@end

// Road fragment shader: the shared lit shading plus an exact per-fragment
// world-space depth bias (metres toward the camera), so parallel road layers
// never z-fight and roads win against the near-coplanar ground plane at
// altitude. Window depth follows OpenGL's 0.5*ndc+0.5 on every backend (see
// fly_clip), so the formulas below are backend-independent.
@fs fs_road
layout(binding=1) uniform fs_road_params {
    vec4 colDiffuse;
    vec3 lightDir;
    float shadowsEnabled;
    vec3 ambient;
    float waterSurfaceY;
    mat4 lightVP;
    mat4 matProjection;
};
@include_block lit_fs_common
in float fragLayer;
void main() {
    float alpha;
    vec3 lit = ShadeLit(alpha);
    finalColor = vec4(lit, alpha);
    float roadDepth;
    if (matProjection[3][3] > 0.5) {                                   // orthographic
        float dz = 0.02 + fragLayer * 2.0;
        roadDepth = gl_FragCoord.z + 0.5 * matProjection[2][2] * dz;
    } else {                                                            // perspective
        float B = matProjection[3][2];
        float invW = gl_FragCoord.w;
        float w = 1.0 / max(invW, 1e-6);
        float depthStep = 1.2e-7 * w * w / max(-B, 1e-3);               // metres per depth step
        float dz = max(0.02, depthStep * 3.0) + fragLayer * max(2.0, depthStep * 150.0);
        dz = min(dz, 6.0);
        roadDepth = gl_FragCoord.z + 0.5 * B * dz * invW * invW;
    }
    gl_FragDepth = clamp(roadDepth, 0.0, 1.0);
}
@end

@program lit vs fs
@program lit_road vs_road fs_road
@program lit_instanced vs_instanced fs
