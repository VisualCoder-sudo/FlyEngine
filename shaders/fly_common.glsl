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

// The other way: the clip-space position of a render target's UV.
@block fly_uv_ndc
vec2 fly_uv_ndc(vec2 rtUv) {
#if SOKOL_GLSL
    return rtUv * 2.0 - 1.0;
#else
    return vec2(rtUv.x * 2.0 - 1.0, 1.0 - rtUv.y * 2.0);
#endif
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

// Light nodes (SpotLight / PointLight / SurfaceLight under objects), filled per frame by gfx::SetNodeLights:
//   nodeLightCount  x = how many lights are lit
//   nodeLightPos    xyz = world position, w = range (m)
//   nodeLightDir    xyz = unit axis it points along, w = kind (0 point, 1 spot, 2 surface)
//   nodeLightColor  rgb = linear colour x brightness, w = falloff (0 linear, 1 inverse square)
//   nodeLightCone   x = cos of the outer cone angle, y = cos of the inner one, z = first shadow tile (-1 = none),
//                   w = angle one shadow texel covers (negative = six tiles, a cube)
//   nodeLightU/V    surface lights: xyz = half-extent of the emitting rectangle along its two sides (world)
@block lit_light_uniforms
    vec4 nodeLightCount;
    vec4 nodeLightPos[32];
    vec4 nodeLightDir[32];
    vec4 nodeLightColor[32];
    vec4 nodeLightCone[32];
    vec4 nodeLightU[32];
    vec4 nodeLightV[32];
    mat4 nodeShadowVP[16];     // the light-node shadow tiles' view-projections (see gfx::shadow::PlanNodeShadows)
    vec4 nodeShadowRect[16];   // xy = tile centre, zw = half size, in the tiles' NDC
@end

// Light from the light nodes (the uniforms are above). The includer declares the texture nodeShadowMap
// (depth) and the comparison sampler nodeShadow_smp, and has fly_rt_uv in scope.
@block lit_node_lights
// Light from the light nodes: linear light on a surface at `pos` facing `n`, before the surface colour.
float NodeLightFalloff(float kind, float dist, float range) {
    if (kind < 0.5) return clamp(1.0 - dist / range, 0.0, 1.0);
    float x = dist / range;
    float window = clamp(1.0 - x * x * x * x, 0.0, 1.0);
    float q = dist / (range * 0.25);
    return window * window / (1.0 + q * q);
}

// 1 = lit, 0 = in the shadow of something, for light i at distance dist from the point (pos, normal n).
float NodeShadow(int i, vec3 pos, vec3 n, vec3 lightPos, float dist) {
    float first = nodeLightCone[i].z;
    if (first < -0.5) return 1.0;
    float texelAngle = nodeLightCone[i].w;
    int tile = int(first + 0.5);
    if (texelAngle < 0.0) {
        // A cube of six tiles: the face the point lies in (+x, -x, +y, -y, +z, -z).
        vec3 d = pos - lightPos;
        vec3 a = abs(d);
        int face = (a.x >= a.y && a.x >= a.z) ? (d.x >= 0.0 ? 0 : 1) : (a.y >= a.z ? (d.y >= 0.0 ? 2 : 3) : (d.z >= 0.0 ? 4 : 5));
        tile += face;
    }
    float texel = dist * abs(texelAngle);              // metres one shadow texel covers here
    vec4 q = nodeShadowVP[tile] * vec4(pos + n * texel * 1.5, 1.0);
    if (q.w <= 0.0) return 1.0;
    vec3 ndc = q.xyz / q.w;
    vec4 rect = nodeShadowRect[tile];
    vec2 local = (ndc.xy - rect.xy) / rect.zw;
    if (abs(local.x) > 0.97 || abs(local.y) > 0.97) return 1.0;
    // Depth bias in depth-buffer units: a distance in metres, scaled by how fast the depth changes there.
    float range = max(nodeLightPos[i].w, 0.1);
    float depthPerMetre = (range * 0.05) / ((range - 0.05) * max(q.w * q.w, 1e-3));
    float bias = (texel * 1.0 + 0.01) * depthPerMetre;
    float depth = ndc.z * 0.5 + 0.5 - bias;
    vec2 uv = fly_rt_uv(ndc.xy);
    const float px = 1.0 / 4096.0;
    float lit = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            lit += texture(sampler2DShadow(nodeShadowMap, nodeShadow_smp), vec3(uv + vec2(x, y) * px, depth));
    return lit / 9.0;
}

vec3 NodeLights(vec3 pos, vec3 n) {
    vec3 sum = vec3(0.0);
    int count = int(nodeLightCount.x + 0.5);
    for (int i = 0; i < 32; ++i) {
        if (i >= count) break;
        vec3 lpos = nodeLightPos[i].xyz;
        float range = max(nodeLightPos[i].w, 0.01);
        float kind = nodeLightDir[i].w;
        vec3 axis = nodeLightDir[i].xyz;
        if (kind > 1.5) {
            // Surface light: the nearest point on the emitting rectangle.
            vec3 u = nodeLightU[i].xyz;
            vec3 v = nodeLightV[i].xyz;
            vec3 d0 = pos - lpos;
            lpos += u * clamp(dot(d0, u) / max(dot(u, u), 1e-6), -1.0, 1.0) + v * clamp(dot(d0, v) / max(dot(v, v), 1e-6), -1.0, 1.0);
        }
        vec3 toLight = lpos - pos;
        float dist = length(toLight);
        if (dist >= range) continue;
        vec3 L = toLight / max(dist, 1e-4);
        float att = NodeLightFalloff(nodeLightColor[i].w, dist, range);
        if (kind > 0.5) {
            float cd = dot(-L, axis);
            att *= smoothstep(nodeLightCone[i].x, nodeLightCone[i].y, cd);
            if (kind > 1.5) att *= max(cd, 0.0);   // a surface emits like a lamp panel: brightest straight out
        }
        float ndl = max(dot(n, L), 0.0);
        if (att * ndl > 0.001) att *= NodeShadow(i, pos, n, lpos, dist);
        sum += nodeLightColor[i].rgb * att * ndl;
    }
    return sum;
}
@end
