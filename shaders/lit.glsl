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
    // Layer = height above the road surface at this spot (written by the city mesh builder in
    // the otherwise-unused texcoord.x), NOT absolute y: raised roads/pads must not be pushed
    // toward the camera by their altitude.
    fragLayer = max(vertexTexCoord.x, 0.0);
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
out vec4 fragInst;      // xyz = instance scale (metres), w = 1 for tinted city buildings (0 = plain marker)
out float fragBaseY;    // world y of the building's bottom face
out float fragFound;    // height of the buried foundation / plinth above that face (m): no windows below it
void main() {
    // Built from column vectors rather than by assigning model[i][3]: writing
    // individual elements of a mat4 was miscompiled by at least one OpenGL driver
    // (NVIDIA), which lost the instance's scale whenever a tint was present.
    vec3 instTint = vec3(instanceTransform0.w, instanceTransform1.w, instanceTransform2.w);
    // The matrix's w (m15) is 1 for everything; city buildings put 1 + foundation height there.
    fragFound = max(instanceTransform3.w - 1.0, 0.0);
    mat4 model = mat4(vec4(instanceTransform0.xyz, 0.0),
                      vec4(instanceTransform1.xyz, 0.0),
                      vec4(instanceTransform2.xyz, 0.0),
                      vec4(instanceTransform3.xyz, 1.0));
    // Tint sign encodes the mode: all-zero = plain marker, negative = prop (vertex colours x |tint|, no facade).
    float isBuilding = dot(instTint, instTint) < 1e-6 ? 0.0 : 1.0;
    if (instTint.x < 0.0) { isBuilding = 2.0; instTint = abs(instTint); }
    if (isBuilding < 0.5) instTint = vec3(1.0);
    vec3 instScale = vec3(length(instanceTransform0.xyz), length(instanceTransform1.xyz), length(instanceTransform2.xyz));
    fragInst = vec4(instScale, isBuilding);
    fragBaseY = instanceTransform3.y - instScale.y * 0.5;

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
    vec4 biasedLightPos = lightVP * vec4(fragWorldPos + normalize(normal) * 0.05, 1.0);
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

vec3 ShadeLitWith(vec3 baseColor, float baseAlpha, out float alpha) {
    vec3 normal = normalize(fragNormal);
    float diffuse = max(dot(normal, -lightDir), 0.0);
    float shadow = ShadowCalculation(normal) * shadowsEnabled;

    vec3 lit = (ambient + (1.0 - shadow) * diffuse * sunColor.rgb) * baseColor * colDiffuse.rgb;

    float depthBelow = waterSurfaceY - fragWorldPos.y;
    if (depthBelow > 0.0) {
        float t = clamp(depthBelow * 0.3, 0.0, 1.0);
        vec3 waterTint = vec3(0.6, 0.75, 0.9);
        lit = mix(lit, lit * waterTint, t * 0.25);
        float luma = dot(lit, vec3(0.299, 0.587, 0.114));
        lit = mix(lit, vec3(luma), t * 0.1);
    }
    alpha = baseAlpha * colDiffuse.a;
    return lit;
}

// Depth fog: 1 / gl_FragCoord.w is the view depth of a perspective camera (and 1 for an orthographic one: no fog there).
vec3 ApplyFog(vec3 c) {
    float dist = 1.0 / max(gl_FragCoord.w, 1e-6);
    float f = clamp(1.0 - exp(-fogParams.a * dist), 0.0, 1.0);
    return mix(c, fogParams.rgb, f);
}

vec3 ShadeLit(out float alpha) {
    vec4 texelColor = texture(sampler2D(texture0, texture0_smp), fragTexCoord);
    return ShadeLitWith(texelColor.rgb * fragColor.rgb, texelColor.a, alpha);
}
@end

@fs fs
layout(binding=1) uniform fs_params {
    vec4 colDiffuse;
    vec3 lightDir;
    float shadowsEnabled;
    vec3 ambient;
    float waterSurfaceY;
    vec4 sunColor;      // rgb = sun colour (its intensity is the length of lightDir)
    vec4 fogParams;     // rgb = fog colour, a = density per metre (0 = off)
    mat4 lightVP;
};
@include_block lit_fs_common
void main() {
    float alpha;
    vec3 lit = ShadeLit(alpha);
    finalColor = vec4(ApplyFog(lit), alpha);
}
@end

// Night light pools: warm light from the nearest street lamps and car headlights (positions are fed per frame
// by the city: xyz = world position, w = radius), added on top of the lit surface. Included after a fragment
// shader declares nightAmount, lightCount and nightLights.
@block night_glow
vec3 NightGlow(vec3 baseColor, vec3 pos, vec3 n) {
    vec3 sum = vec3(0.0);
    int count = int(lightCount);
    for (int i = 0; i < 32; ++i) {
        if (i >= count) break;
        vec3 d = nightLights[i].xyz - pos;
        float dist = length(d);
        float att = clamp(1.0 - dist / nightLights[i].w, 0.0, 1.0);
        att *= att;
        float facing = max(dot(n, d / max(dist, 0.001)), 0.2);
        sum += vec3(1.0, 0.80, 0.50) * att * facing;
    }
    return baseColor * sum * nightAmount * 2.4;
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
    vec4 sunColor;      // rgb = sun colour (its intensity is the length of lightDir)
    vec4 fogParams;     // rgb = fog colour, a = density per metre (0 = off)
    float nightAmount;
    float lightCount;
    vec4 nightLights[32];
    mat4 lightVP;
    mat4 matProjection;
};
@include_block lit_fs_common
@include_block night_glow
in float fragLayer;
void main() {
    float alpha;
    vec3 lit = ShadeLit(alpha);
    if (nightAmount > 0.02 && lightCount > 0.5)
        lit += NightGlow(texture(sampler2D(texture0, texture0_smp), fragTexCoord).rgb * fragColor.rgb, fragWorldPos, normalize(fragNormal));
    finalColor = vec4(ApplyFog(lit), alpha);
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

// City building fragment shader: procedural facade (floors, window bays, lit windows, roof and
// base band) instead of the texture, shape-agnostic because it works in world space. Anything
// that is not a tinted building (editor node markers) falls back to the plain lit look.
@fs fs_building
layout(binding=1) uniform fs_building_params {
    vec4 colDiffuse;
    vec3 lightDir;
    float shadowsEnabled;
    vec3 ambient;
    float waterSurfaceY;
    vec4 sunColor;      // rgb = sun colour (its intensity is the length of lightDir)
    vec4 fogParams;     // rgb = fog colour, a = density per metre (0 = off)
    float nightAmount;      // 0 = day, 1 = night: scales the emissive window / lamp / car light glow
    float lightCount;
    vec4 nightLights[32];
    mat4 lightVP;
};
@include_block lit_fs_common
@include_block night_glow
in vec4 fragInst;
in float fragBaseY;
in float fragFound;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

void main() {
    float alpha;
    vec3 lit;
    vec3 emit = vec3(0.0);
    vec3 glowBase = fragColor.rgb;
    if (fragInst.w < 0.5) {
        lit = ShadeLit(alpha);
    } else if (fragInst.w > 1.5) {
        lit = ShadeLitWith(fragColor.rgb, 1.0, alpha);   // prop
        if (fragColor.a < 0.99) emit = fragColor.rgb * nightAmount * 1.6;   // lamp heads, car lights: glow at night
    } else {
        vec3 n = normalize(fragNormal);
        vec3 wall = fragColor.rgb;
        vec3 base = wall;
        if (n.y > 0.7) {
            base = wall * 0.72;                                    // roof
        } else if (n.y > -0.5) {
            vec3 t = normalize(cross(vec3(0.0, 1.0, 0.0), n));
            float u = dot(fragWorldPos, t);
            float y = fragWorldPos.y - fragBaseY;
            const float floorH = 3.4;
            const float bayW = 3.2;
            float topEdge = fragInst.y;                            // building height (storeys * floorH + any foundation)
            // Floors are counted down from the roof, so the top is never chopped; any foundation sits at the bottom.
            float fy = (topEdge - y) / floorH;
            float floorIdx = floor(fy);
            float fv = fract(fy);
            float bu = u / bayW;
            float bayIdx = floor(bu);
            float fu = fract(bu);
            // Below fragFound the building is a plain plinth (it is mostly buried by a sloped pad: windows there would be
            // cut diagonally by the ground); the first floor and its windows start above it.
            bool plinth = y < fragFound;
            bool groundFloor = y < fragFound + floorH;
            bool inGlass = !plinth && fu > 0.18 && fu < 0.82 && fv > 0.22 && fv < 0.78;
            if (groundFloor) inGlass = !plinth && fu > 0.1 && fu < 0.9 && fv > 0.12 && fv < 0.7;
            float r = hash21(vec2(bayIdx, floorIdx) + floor(fragBaseY));
            if (inGlass) {
                vec3 glass = mix(vec3(0.16, 0.22, 0.30), vec3(0.34, 0.44, 0.56), fv);
                // Lit windows only appear as it gets dark (none in daylight: every window is blue glass), more of them the darker it is.
                if (r > mix(1.001, 0.42, clamp(nightAmount * 1.6, 0.0, 1.0))) {
                    glass = vec3(0.95, 0.82, 0.48) * 0.9;
                    emit = glass * nightAmount * 1.1;
                }
                base = glass;
            } else {
                float band = smoothstep(0.0, 0.06, fv) * (1.0 - smoothstep(0.94, 1.0, fv));
                base = wall * (0.82 + 0.18 * band);                   // slab edges shade the wall
                if (groundFloor) base *= 0.88;
                if (plinth) base = wall * 0.74;                       // concrete-ish plinth
            }
        }
        lit = ShadeLitWith(base, 1.0, alpha);
        glowBase = base;
    }
    if (nightAmount > 0.02 && lightCount > 0.5 && fragInst.w > 0.5)
        emit += NightGlow(glowBase, fragWorldPos, normalize(fragNormal));
    finalColor = vec4(ApplyFog(lit + emit), alpha);
}
@end

@program lit vs fs
@program lit_road vs_road fs_road
@program lit_instanced vs_instanced fs_building
