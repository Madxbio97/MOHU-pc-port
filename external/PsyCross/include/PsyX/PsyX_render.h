#ifndef EMULATOR_H
#define EMULATOR_H

#include "PsyX/PsyX_config.h"

/*
 * Platform specific emulator setup
 */
#if (defined(_WIN32) || defined(__APPLE__) || defined(__linux__)) &&           \
    !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__RPI__)
#define RENDERER_OGL
#define USE_GLAD
#elif defined(__RPI__)
#define RENDERER_OGLES
#define OGLES_VERSION (3)
#elif defined(__EMSCRIPTEN__)
#define RENDERER_OGLES
#define OGLES_VERSION (2)
#elif defined(__ANDROID__)
#define RENDERER_OGLES
#define OGLES_VERSION (3)
#endif
#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
#define USE_OPENGL 1
#else
#define USE_OPENGL 0
#endif

#if OGLES_VERSION == 2
#define ES2_SHADERS
#elif OGLES_VERSION == 3
#define ES3_SHADERS
#endif

/*
 * OpenGL
 */

#if defined(RENDERER_OGL)

#define GL_GLEXT_PROTOTYPES

#if defined(USE_GLAD)
#include "common/glad.h"
#endif

#elif defined(RENDERER_OGLES)

#define GL_GLEXT_PROTOTYPES

#if defined(USE_GLAD)
#include "common/glad.h"
#else
#ifdef __EMSCRIPTEN__
#include <GL/gl.h>
#else
#if OGLES_VERSION == 2
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#elif OGLES_VERSION == 3
#include <GLES3/gl3.h>
#endif
#endif
#endif

#include <EGL/egl.h>

#endif

// setup renderer texture formats
#if defined(RENDERER_OGL)
#define TEXTURE_FORMAT GL_UNSIGNED_SHORT_1_5_5_5_REV
#elif defined(RENDERER_OGLES)
#define TEXTURE_FORMAT GL_UNSIGNED_SHORT_5_5_5_1
#endif

#include "psx/types.h"

#include "common/pgxp_defs.h"

#include "psx/libgte.h"
#include "psx/libgpu.h"

#include <stddef.h>
#include <stdio.h>

#ifndef NULL
#define NULL 0
#endif

/*
// FIXME: enable when needed
#if defined(RENDERER_OGLES)

#	define glGenVertexArrays       glGenVertexArraysOES
#	define glBindVertexArray       glBindVertexArrayOES
#	define glDeleteVertexArrays    glDeleteVertexArraysOES

#endif
*/

#if defined(RENDERER_OGL)
#define VRAM_FORMAT GL_RG
#define VRAM_INTERNAL_FORMAT GL_RG32F
#elif defined(RENDERER_OGLES)
#define VRAM_FORMAT GL_LUMINANCE_ALPHA
#define VRAM_INTERNAL_FORMAT GL_LUMINANCE_ALPHA
#endif

#define LUT_WIDTH (256)
#define LUT_HEIGHT (256)

#define VRAM_WIDTH (1024)
#define VRAM_HEIGHT (512)

// Host-only texture pages used by native ports when a widened scene needs
// more simultaneously resident material aliases than fit in PS1 VRAM.  They
// deliberately live in a separate GL texture: framebuffer, MoveImage and
// every guest VRAM address retain the exact 1024x512 retail layout.
#define VRAM_ALIAS_PAGE_COUNT (63)
#define VRAM_ALIAS_WIDTH (1024)
#define VRAM_ALIAS_HEIGHT (1024)

#define TPAGE_WIDTH (256)
#define TPAGE_HEIGHT (256)

#define MAX_VERTEX_BUFFER_SIZE (1 << (sizeof(ushort) * 8))

#define PGXP_NEAR_PLANE (0.25f)

#pragma pack(push, 1)
typedef struct {
#if USE_PGXP
  float x, y, page, clut;
  float z, scr_h, clip_x, clip_y;
#else
  short x, y;
  u_short page, clut;
#endif

  u_char u, v, bright, dither;
  u_char r, g, b, a;
  u_char umin, vmin, umax, vmax;

  char tcx, tcy, _p0, _p1;
  float precise_u, precise_v;
} GrVertex;
#pragma pack(pop)

typedef enum {
  GR_VOLUME_FIRE = 0,
  GR_VOLUME_EXPLOSION = 1,
  GR_VOLUME_SMOKE = 2,
  GR_VOLUME_FOG = 3,
  GR_VOLUME_LIGHT_HALO = 4,
} GrVolumetricEffectKind;

/* Camera-space analytic volume. Coordinates and radii use PGXP camera units
 * (one unit is 128 guest world units); positive Y projects down the screen.
 * Retail owns position, size, colour and lifetime. The native renderer only
 * replaces the camera-facing sprite used to present that state. */
typedef struct {
  float center_x;
  float center_y;
  float center_z;
  float radius_x;
  float radius_y;
  float radius_z;
  float red;
  float green;
  float blue;
  float density;
  float emission;
  float phase;
  float seed;
  int kind;
} GrVolumetricEffect;

/* Expanded opaque caster geometry in PGXP camera units. The scene owns the
 * object transform; the backend never derives shadow placement from the
 * camera. */
typedef struct {
  float x;
  float y;
  float z;
} GrObjectShadowVertex;

/* Object-local orthographic shadow frustum, expressed in camera space. The
 * right/up/forward basis must be unit length. Forward points from the light,
 * through the caster, towards receiving scene geometry. Extents, reach and
 * center share the same PGXP camera-unit scale as the vertices. */
typedef struct {
  int first_vertex;
  int vertex_count;
  float center_x;
  float center_y;
  float center_z;
  float right_x;
  float right_y;
  float right_z;
  float up_x;
  float up_y;
  float up_z;
  float forward_x;
  float forward_y;
  float forward_z;
  float extent_x;
  float extent_y;
  float depth_extent;
  float maximum_reach;
  float darkness;
} GrObjectShadowCaster;

typedef enum {
  a_position,
  a_page_clut,
  a_zw,
  a_texcoord,
  a_color,
  a_extra,
  a_texbounds,
  a_precise_uv,
} ShaderAttrib;

typedef enum {
  BM_NONE,
  BM_AVERAGE,
  BM_ADD,
  BM_SUBTRACT,
  BM_ADD_QUATER_SOURCE
} BlendMode;

typedef enum {
  TF_4_BIT,
  TF_8_BIT,
  TF_16_BIT,

  TF_32_BIT_RGBA // custom texture
} TexFormat;

typedef enum {
  TEXTURE_FILTER_NEAREST,
  TEXTURE_FILTER_BILINEAR,
  TEXTURE_FILTER_WORLD_TRILINEAR,
  TEXTURE_FILTER_WORLD_ANISOTROPIC
} TextureFilterMode;

#if defined(RENDERER_OGLES) || defined(RENDERER_OGL)
typedef uint TextureID;
typedef uint ShaderID;
#else
#error
#endif

#if defined(_LANGUAGE_C_PLUS_PLUS) || defined(__cplusplus) ||                  \
    defined(c_plusplus)
extern "C" {
#endif

extern TextureID g_whiteTexture;
extern TextureID g_vramTexture;

extern void GR_SwapWindow();

// PSX VRAM operations
enum GrVRAMWriteKind {
  GR_VRAM_WRITE_UPLOAD = 1,
  GR_VRAM_WRITE_MOVE = 2,
  GR_VRAM_WRITE_CLEAR = 3,
  GR_VRAM_WRITE_FRAMEBUFFER = 4,
};

typedef struct GrVRAMWriteEvent {
  unsigned long long sequence;
  int kind;
  int source_x;
  int source_y;
  int destination_x;
  int destination_y;
  int width;
  int height;
} GrVRAMWriteEvent;

extern void GR_SaveVRAM(const char *outputFileName, int x, int y, int width,
                        int height, int bReadFromFrameBuffer);
extern void GR_CopyVRAM(unsigned short *src, int x, int y, int w, int h,
                        int dst_x, int dst_y);
extern void GR_ReadVRAM(unsigned short *dst, int x, int y, int dst_w,
                        int dst_h);
extern void GR_UploadVRAMAliasPage(int page, const unsigned short *src);
extern void GR_ReadVRAMAliasPage(int page, unsigned short *dst);
extern unsigned long long GR_GetVRAMWriteSequence();
extern int GR_ReadVRAMWriteEvents(unsigned long long after_sequence,
                                  GrVRAMWriteEvent *events, int capacity);

extern void GR_StoreFrameBuffer(int x, int y, int w, int h);
extern void GR_UpdateVRAM();
/* Commits the de-jittered GP1 geometry used to size new guest draw pages. */
extern void GR_SetGuestDisplayGeometry(int width, int height);
extern void GR_BeginGuestSubmit(void);
/* Freezes the de-jittered display geometry used while parsing one raw guest
 * submission. Draw splits retain this epoch and never consult a later
 * mutable DISPENV while they are finally flushed. */
extern void GR_BeginGuestProjectionEpoch(unsigned long long epoch, int width,
                                         int height, int rgb24, int interlaced);
extern void GR_EndGuestProjectionEpoch(void);
/* Window-system resize is queued immediately but becomes visible only when
 * the next outer scene begins. */
extern void GR_QueueDrawableSize(int width, int height);
extern void GR_GetCommittedDrawableSize(int *width, int *height);
/* Counts explicit CPU synchronization points, never ordinary guest frames. */
extern unsigned long long GR_GetSynchronousVRAMReadbackCount(void);
/* Diagnostic counters for resolution-scaled guest work. */
extern unsigned long long GR_GetGuestVRAMPackCount(void);
extern unsigned long long GR_GetGuestVRAMPackPixels(void);
extern unsigned long long GR_GetGuestSeedCount(void);
extern unsigned long long GR_GetGuestSeedPixels(void);
extern unsigned long long GR_GetGuestCaptureCount(void);
extern unsigned long long GR_GetGuestCapturePixels(void);
extern void GR_ReadFramebufferDataToVRAM();

extern TextureID GR_CreateRGBATexture(int width, int height,
                                      u_char *data /*= nullptr*/);
extern void GR_UpdateRGBATexture(TextureID texture, int width, int height,
                                 const u_char *data);
extern u_char GR_Expand5BitColor(u_char value);
extern void GR_CalculateReversedDepthProjection(float zNear, float zFar,
                                                float *scale, float *bias);
extern ShaderID GR_Shader_Compile(const char *source, int isPsxShader);

extern void GR_SetShader(const ShaderID shader);
extern void GR_ApplyProjectionEpoch(const DISPENV *display,
                                    unsigned long long epoch);
extern void GR_Perspective3D(const float fov, const float width,
                             const float height, const float zNear,
                             const float zFar);
extern void GR_Ortho2D(float left, float right, float bottom, float top,
                       float znear, float zfar);

extern void GR_SetBlendMode(BlendMode blendMode);
extern void GR_SetBlendModeForPrimitive(BlendMode blendMode, int untextured);
extern void GR_SetPolygonOffset(float slope, float units);
extern void GR_SetStencilMode(int drawPrim);
extern void GR_EnableStencil(int enable);
extern void GR_BeginShadowMask(void);
extern void GR_EndShadowMask(void);
extern void GR_EnableDepth(int enable);
extern void GR_SetDepthState(int testEnable, int writeEnable);
extern void GR_SetDepthRange(float lower, float upper);
extern void GR_ClearDepthBuffer(void);
extern unsigned long long GR_GetDepthClearSerial(int offscreen);
extern int GR_UsesWorldDepth(const GrVertex *triangle, int depthRequested);
extern unsigned long long GR_GetWorldDepthBandAdvanceCount(int offscreen);
extern unsigned long long GR_GetWorldDepthPainterFallbackCount(int offscreen);
extern void GR_SetScissorState(int enable);
extern void GR_SetOffscreenState(const RECT16 *offscreenRect, int enable);
/* Opens an isolated guest-sized color/depth target for presentation-only
 * drawing. The target starts black and can never be packed into PS1 VRAM or
 * retained as an authoritative guest page. Returns zero unless the regular
 * guest offscreen target is idle and the complete rectangle is valid. */
extern int GR_BeginGuestPresentationReplay(const RECT16 *target);
/* Flushes queued replay primitives, then clears only the isolated target. */
extern int GR_ClearGuestPresentationReplay(unsigned char r, unsigned char g,
                                           unsigned char b);
/* Flushes and closes the isolated target without any guest VRAM side effect.
 * A failed target switch makes the completed replay unavailable. */
extern int GR_EndGuestPresentationReplay(void);
/* Blits a successfully completed replay into the native presentation target.
 * The requested rectangle must be contained by the replay target. */
extern int GR_PresentGuestPresentationReplay(int x, int y, int width,
                                             int height);
/* Returns non-zero when the requested 16-bit non-interlaced display rectangle
 * is available in a retained high-resolution guest draw page. */
extern int GR_HasHighResolutionVRAM(int x, int y, int width, int height);
/* Blits that retained page to the native internal target. PsyX_BeginScene must
 * have been called by the client. Returns zero when the exact display region
 * is unavailable so callers can fall back to ordinary PS1 VRAM scanout. */
extern int GR_PresentHighResolutionVRAM(int x, int y, int width, int height);
/* Resolves a complete packed guest display into one logical RGBA8 image and
 * scales that image once into the selected native target. This avoids filter
 * discontinuities at the SPRT/TPAGE boundaries used by the compatibility
 * fallback. Returns zero when the GPU conversion path is unavailable. */
extern int GR_PresentComposedGuestScanout(int x, int y, int width, int height);
extern unsigned long long GR_GetGuestScanoutPresentCount(void);
extern void GR_SetupClipMode(const RECT16 *clipRect, int enable);
extern void GR_SetViewPort(int x, int y, int width, int height);
extern TextureFilterMode
GR_ResolveTextureFilterMode(TextureFilterMode requestedMode,
                            int bilinearFiltering, int trilinearFiltering,
                            int anisotropicFiltering);
extern void GR_SetSceneFogParameters(int enable, unsigned char red,
                                     unsigned char green, unsigned char blue,
                                     int dqa, int dqb, int projection,
                                     unsigned int terrainDepthCue);
extern void GR_EnableSceneFog(int enable);
extern void GR_ApplySceneSMAA(void);
extern void GR_ApplySceneFXAA(void);
extern void GR_ApplySceneAntialiasing(void);
extern int GR_BeginVolumetricFrame(int enable);
extern int GR_VolumetricEffectsAvailable(void);
extern int GR_UploadVolumetricDensityAtlas(int width, int height,
                                           const unsigned char *rgba);
extern void GR_DrawVolumetricEffects(const GrVolumetricEffect *effects,
                                     int count, int projection,
                                     int logicalWidth, int logicalHeight,
                                     float timeSeconds);
extern int GR_ObjectShadowsAvailable(void);
extern void GR_DrawObjectShadows(const GrObjectShadowVertex *vertices,
                                 int vertexCount,
                                 const GrObjectShadowCaster *casters,
                                 int casterCount, int projection,
                                 int logicalWidth, int logicalHeight);
extern void GR_SetTexture(TextureID texture, TexFormat texFormat,
                          TextureFilterMode filterMode);
extern void GR_SetTextureBlendMode(BlendMode blendMode);
extern void GR_SetOverrideTextureSize(int width, int height);
extern void GR_SetWireframe(int enable);

extern void GR_DestroyTexture(TextureID texture);
extern void GR_Clear(int x, int y, int w, int h, unsigned char r,
                     unsigned char g, unsigned char b);
extern void GR_ClearVRAM(int x, int y, int w, int h, unsigned char r,
                         unsigned char g, unsigned char b);
extern void GR_UpdateVertexBuffer(const GrVertex *vertices, int count);
extern void GR_DrawTriangles(int start_vertex, int triangles);

extern void GR_PushDebugLabel(const char *label);
extern void GR_PopDebugLabel();

#if defined(_LANGUAGE_C_PLUS_PLUS) || defined(__cplusplus) ||                  \
    defined(c_plusplus)
}
#endif

#endif
