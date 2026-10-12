/**********************************************************************************************
*
*   rlgl subset for FlyEngine's sokol backend
*
*   raylib's rlgl.h is an OpenGL abstraction. This header keeps the part of its API
*   that is not tied to OpenGL objects -- immediate-mode vertex submission, the
*   matrix stack, global render state and emulated texture units -- implemented by
*   src/Engine/RL on top of sokol_gfx. Functions that create or bind raw OpenGL
*   objects (VAOs, framebuffers, shader programs) are intentionally absent.
*
*   Based on rlgl.h from raylib (zlib/libpng license, Copyright (c) 2014-2025 Ramon Santamaria).
*
**********************************************************************************************/
#ifndef RLGL_H
#define RLGL_H

#include "raylib.h"

#define RLGL_VERSION  "5.0-fly"

#ifndef RL_MALLOC
    #define RL_MALLOC(sz)       malloc(sz)
#endif
#ifndef RL_CALLOC
    #define RL_CALLOC(n,sz)     calloc(n,sz)
#endif
#ifndef RL_REALLOC
    #define RL_REALLOC(n,sz)    realloc(n,sz)
#endif
#ifndef RL_FREE
    #define RL_FREE(p)          free(p)
#endif

#define RL_DEFAULT_BATCH_BUFFER_ELEMENTS   8192
#define RL_CULL_DISTANCE_NEAR              0.05
#define RL_CULL_DISTANCE_FAR               4000.0

// Primitive assembly draw modes
#define RL_LINES                                0x0001
#define RL_TRIANGLES                            0x0004
#define RL_QUADS                                0x0007

// Matrix modes
#define RL_MODELVIEW                            0x1700
#define RL_PROJECTION                           0x1701
#define RL_TEXTURE                              0x1702

#define RL_FLOAT                                0x1406
#define RL_UNSIGNED_BYTE                        0x1401

// OpenGL version reported by rlGetVersion() (kept for source compatibility)
typedef enum {
    RL_OPENGL_11_SOFTWARE = 0,
    RL_OPENGL_11,
    RL_OPENGL_21,
    RL_OPENGL_33,
    RL_OPENGL_43,
    RL_OPENGL_ES_20,
    RL_OPENGL_ES_30
} rlGlVersion;

#if defined(__cplusplus)
extern "C" {
#endif

// Matrix operations
RLAPI void rlMatrixMode(int mode);
RLAPI void rlPushMatrix(void);
RLAPI void rlPopMatrix(void);
RLAPI void rlLoadIdentity(void);
RLAPI void rlTranslatef(float x, float y, float z);
RLAPI void rlRotatef(float angle, float x, float y, float z);
RLAPI void rlScalef(float x, float y, float z);
RLAPI void rlMultMatrixf(const float *matf);

// Immediate-mode vertex submission
RLAPI void rlBegin(int mode);
RLAPI void rlEnd(void);
RLAPI void rlVertex2i(int x, int y);
RLAPI void rlVertex2f(float x, float y);
RLAPI void rlVertex3f(float x, float y, float z);
RLAPI void rlTexCoord2f(float x, float y);
RLAPI void rlNormal3f(float x, float y, float z);
RLAPI void rlColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
RLAPI void rlColor3f(float x, float y, float z);
RLAPI void rlColor4f(float x, float y, float z, float w);
RLAPI void rlSetTexture(unsigned int id);
RLAPI bool rlCheckRenderBatchLimit(int vCount);
RLAPI void rlDrawRenderBatchActive(void);

// Global render state
RLAPI void rlEnableDepthTest(void);
RLAPI void rlDisableDepthTest(void);
RLAPI void rlEnableDepthMask(void);
RLAPI void rlDisableDepthMask(void);
RLAPI void rlEnableWireMode(void);
RLAPI void rlDisableWireMode(void);
RLAPI bool rlIsWireMode(void);
RLAPI void rlEnableScissorTest(void);
RLAPI void rlDisableScissorTest(void);
RLAPI void rlScissor(int x, int y, int width, int height);   // origin: bottom-left, like glScissor
RLAPI void rlSetBlendMode(int mode);

// Emulated texture units: a sampler uniform holding unit N reads whatever
// texture was last enabled on unit N.
RLAPI void rlActiveTextureSlot(int slot);
RLAPI void rlEnableTexture(unsigned int id);
RLAPI void rlDisableTexture(void);
RLAPI unsigned int rlGetTextureIdDefault(void);

// Matrix state
RLAPI Matrix rlGetMatrixModelview(void);
RLAPI Matrix rlGetMatrixProjection(void);
RLAPI Matrix rlGetMatrixTransform(void);
RLAPI void rlSetMatrixProjection(Matrix proj);
RLAPI void rlSetMatrixModelview(Matrix view);
RLAPI void rlSetClipPlanes(double nearPlane, double farPlane);
RLAPI double rlGetCullDistanceNear(void);
RLAPI double rlGetCullDistanceFar(void);

// Persistent vertex buffers (used for per-instance data). Returns 0 on failure.
RLAPI unsigned int rlLoadVertexBuffer(const void *buffer, int size, bool dynamic);
RLAPI void rlUnloadVertexBuffer(unsigned int vboId);

RLAPI int rlGetVersion(void);

// Textures and default shader (used by raylib's model/texture modules)
RLAPI const char *rlGetPixelFormatName(unsigned int format);
RLAPI void rlUnloadTexture(unsigned int id);
RLAPI unsigned int rlGetShaderIdDefault(void);
RLAPI int *rlGetShaderLocsDefault(void);
RLAPI void rlUpdateVertexBuffer(unsigned int bufferId, const void *data, int dataSize, int offset);

// OpenGL object calls kept only so raylib's model module compiles; they are
// reached by GPU-skinning/animation paths the engine does not use, and do nothing.
#define RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION    0
#define RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD    1
#define RL_DEFAULT_SHADER_ATTRIB_LOCATION_NORMAL      2
#define RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR       3
#define RL_DEFAULT_SHADER_ATTRIB_LOCATION_TANGENT     4
#define RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD2   5
RLAPI void rlEnableShader(unsigned int id);
RLAPI void rlDisableShader(void);
RLAPI void rlSetUniformMatrices(int locIndex, const Matrix *mat, int count);
RLAPI bool rlEnableVertexArray(unsigned int vaoId);
RLAPI void rlDisableVertexArray(void);
RLAPI void rlEnableVertexAttribute(unsigned int index);
RLAPI void rlSetVertexAttribute(unsigned int index, int compSize, int type, bool normalized, int stride, int offset);

#if defined(__cplusplus)
}
#endif

#endif // RLGL_H
