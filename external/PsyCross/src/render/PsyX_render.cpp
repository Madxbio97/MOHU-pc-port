#include "PsyX/PsyX_public.h"

#include "../gpu/PsyX_GPU.h"
#include "../platform.h"

#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_render.h"
#include "PsyX/util/timer.h"

#include <algorithm>
#include <array>
#include <assert.h>
#include <cmath>
#include <cstdint>
#include <string.h>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#if defined(_LANGUAGE_C_PLUS_PLUS) || defined(__cplusplus) ||                  \
    defined(c_plusplus)
extern "C" {
#endif

__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

#if defined(_LANGUAGE_C_PLUS_PLUS) || defined(__cplusplus) ||                  \
    defined(c_plusplus)
}
#endif

#endif // def WIN32

#if defined(RENDERER_OGL)

#define USE_PBO 1
#define USE_OFFSCREEN_BLIT 0
#define USE_FRAMEBUFFER_BLIT 1

#else

// OpenGL ES/Web GL has slowdowns and doesn't allow GL_LUMINANCE_ALPHA format as
// framebuffer, so it's disabled
#define USE_PBO (OGLES_VERSION == 3)
#define USE_OFFSCREEN_BLIT (OGLES_VERSION == 3)
#define USE_FRAMEBUFFER_BLIT (OGLES_VERSION == 3)

#endif

extern SDL_Window *g_window;

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

static GLfloat g_maxTextureAnisotropy = 1.0f;

// Every queued guest split carries the DISPENV which was current when its
// vertices were decoded. DrawSync may consume splits from several catch-up
// submits, so projection helpers must not observe the later mutable global
// activeDispEnv while replaying an older split.
static DISPENV g_projectionDisplayEnv{};
static unsigned long long g_projectionEpoch{};
static unsigned long long g_projectionStateRevision{1U};
static unsigned long long g_appliedOffscreenProjectionRevision{};
static int g_appliedOffscreenProjectionValid{};
static int g_projectionDisplayEnvValid{};

static const DISPENV &PsyX_GetProjectionDisplayEnv() {
  return g_projectionDisplayEnvValid ? g_projectionDisplayEnv : activeDispEnv;
}

void GR_ApplyProjectionEpoch(const DISPENV *display, unsigned long long epoch) {
  if (display == nullptr) {
    g_projectionDisplayEnvValid = 0;
    g_projectionEpoch = 0;
    ++g_projectionStateRevision;
    return;
  }
  // DrawSplit publishes the same immutable submit snapshot for every split.
  // Do not make same-target setup query GL state again for each texture run.
  if (g_projectionDisplayEnvValid && g_projectionEpoch == epoch) {
    return;
  }
  g_projectionDisplayEnv = *display;
  g_projectionEpoch = epoch;
  g_projectionDisplayEnvValid = 1;
  ++g_projectionStateRevision;
}

// One host frame submits world, shadows, effects and UI separately. Keep an
// explicit ring large enough that the driver never waits for the VBO used by
// the preceding passes at high presentation rates.
#define MAX_NUM_VERTEX_BUFFERS (8)

// The display width is selected by the game (320, 384, 512, ...). A fixed
// 320x240 pixel aspect rescales every wider mode a second time after GPU
// coordinates have already been normalized by activeDispEnv, producing a
// tall/narrow image. Keep projection, clipping and widescreen mapping on the
// same active display aspect instead.
static float PsyX_GetActiveScreenAspect() {
  const DISPENV &display = PsyX_GetProjectionDisplayEnv();
  if (display.disp.w > 0 && display.disp.h > 0)
    return (float)display.disp.h / (float)display.disp.w;
  return 240.0f / 320.0f;
}

int g_PreviousBlendMode = BM_NONE;
static int g_PreviousBlendUntextured = -1;
int g_PreviousDepthMode = 0;
int g_PreviousDepthWrite = 1;
int g_PreviousDepthFunc = -1;
int g_RequestedDepthMode = 0;
static float g_PreviousDepthRangeLower = -1.0f;
static float g_PreviousDepthRangeUpper = -1.0f;
static unsigned long long g_nativeDepthClearSerial{};
static unsigned long long g_offscreenDepthClearSerial{};
float g_PreviousPolygonOffsetSlope = 0.0f;
float g_PreviousPolygonOffsetUnits = 0.0f;
int g_PreviousStencilMode = 0;
int g_ShadowStencilPhase = 0;
int g_PreviousScissorState = 0;
int g_PreviousOffscreenState = 0;
RECT16 g_PreviousFramebuffer = {0, 0, 0, 0};
RECT16 g_PreviousOffscreen = {0, 0, 0, 0};

ShaderID g_PreviousShader = -1;

TextureID g_vramTexturesDouble[2];
TextureID g_vramTexture;
TextureID g_vramAliasTexture;
TextureID g_rgLutTexture;
int g_vramTextureIdx = 0;

TextureID g_fbTexture = -1;
TextureID g_offscreenRTTexture = -1;

TextureID g_whiteTexture = -1;
TextureID g_lastBoundTexture = -1;

int g_windowWidth = 0;
int g_windowHeight = 0;
static int g_drawableWidth = 0;
static int g_drawableHeight = 0;
static int g_pendingDrawableWidth = 0;
static int g_pendingDrawableHeight = 0;
static unsigned long long g_pendingDrawableRevision = 0;
static unsigned long long g_committedDrawableRevision = 0;

void GR_QueueDrawableSize(int width, int height) {
  width = std::max(width, 1);
  height = std::max(height, 1);
  if (g_pendingDrawableWidth == width && g_pendingDrawableHeight == height)
    return;
  g_pendingDrawableWidth = width;
  g_pendingDrawableHeight = height;
  ++g_pendingDrawableRevision;
}

void GR_GetCommittedDrawableSize(int *width, int *height) {
  if (width != nullptr)
    *width = std::max(g_drawableWidth, 1);
  if (height != nullptr)
    *height = std::max(g_drawableHeight, 1);
}

static void PsyX_UpdateDrawableSize() {
  int width = g_windowWidth > 0 ? g_windowWidth : 1;
  int height = g_windowHeight > 0 ? g_windowHeight : 1;
#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
  if (g_window)
    SDL_GL_GetDrawableSize(g_window, &width, &height);
#endif
  GR_QueueDrawableSize(width, height);
}

static void PsyX_CommitDrawableSize() {
  if (g_committedDrawableRevision == g_pendingDrawableRevision)
    return;
  g_drawableWidth = std::max(g_pendingDrawableWidth, 1);
  g_drawableHeight = std::max(g_pendingDrawableHeight, 1);
  g_committedDrawableRevision = g_pendingDrawableRevision;
}

static PsyXPresentationViewport PsyX_GetLogicalViewport() {
  const DISPENV &display = PsyX_GetProjectionDisplayEnv();
  const int displayWidth = display.disp.w > 0 ? display.disp.w : 320;
  const int displayHeight = display.disp.h > 0 ? display.disp.h : 240;
  PsyXPresentationViewport viewport = {0, 0, displayWidth, displayHeight};
  return viewport;
}

// The PSX framebuffer uses mode-dependent non-square pixels. Original mode
// presents 384x240 gameplay and 320x240 movies at retail 4:3. Adaptive mode
// uses the complete drawable; its vertex transform adds horizontal or vertical
// world view while authored 2D content keeps its original proportions.
PsyXPresentationViewport PsyX_CalculatePresentationViewport(int drawableWidth,
                                                            int drawableHeight,
                                                            int aspectMode) {
  if (drawableWidth < 1)
    drawableWidth = 1;
  if (drawableHeight < 1)
    drawableHeight = 1;

  PsyXPresentationViewport viewport = {0, 0, drawableWidth, drawableHeight};
  if (aspectMode == PSYX_ASPECT_ADAPTIVE)
    return viewport;

  if ((long long)drawableWidth * 3 > (long long)drawableHeight * 4)
    viewport.w = drawableHeight * 4 / 3;
  else
    viewport.h = drawableWidth * 3 / 4;

  if (viewport.w < 1)
    viewport.w = 1;
  if (viewport.h < 1)
    viewport.h = 1;
  viewport.x = (drawableWidth - viewport.w) / 2;
  viewport.y = (drawableHeight - viewport.h) / 2;
  return viewport;
}

PsyXPresentationViewport PsyX_CalculateOutputViewport(int drawableWidth,
                                                      int drawableHeight,
                                                      int renderWidth,
                                                      int renderHeight,
                                                      int aspectMode) {
  if (aspectMode != PSYX_ASPECT_ADAPTIVE) {
    return PsyX_CalculatePresentationViewport(drawableWidth, drawableHeight,
                                              PSYX_ASPECT_ORIGINAL_4_3);
  }
  drawableWidth = std::max(drawableWidth, 1);
  drawableHeight = std::max(drawableHeight, 1);
  renderWidth = std::max(renderWidth, 1);
  renderHeight = std::max(renderHeight, 1);
  PsyXPresentationViewport viewport{0, 0, drawableWidth, drawableHeight};
  if (static_cast<long long>(drawableWidth) * renderHeight >
      static_cast<long long>(drawableHeight) * renderWidth) {
    viewport.w = static_cast<int>(static_cast<long long>(drawableHeight) *
                                  renderWidth / renderHeight);
  } else {
    viewport.h = static_cast<int>(static_cast<long long>(drawableWidth) *
                                  renderHeight / renderWidth);
  }
  viewport.w = std::max(viewport.w, 1);
  viewport.h = std::max(viewport.h, 1);
  viewport.x = (drawableWidth - viewport.w) / 2;
  viewport.y = (drawableHeight - viewport.h) / 2;
  return viewport;
}

static PsyXPresentationViewport PsyX_GetRenderTargetExtent();

static PsyXPresentationViewport PsyX_GetPresentationViewport() {
  const PsyXPresentationViewport target = PsyX_GetRenderTargetExtent();
  return PsyX_CalculateOutputViewport(
      g_drawableWidth > 0 ? g_drawableWidth : std::max(g_windowWidth, 1),
      g_drawableHeight > 0 ? g_drawableHeight : std::max(g_windowHeight, 1),
      target.w, target.h, g_cfg_aspectMode);
}

// The native color/depth target always has the exact launcher-selected extent.
// Keeping allocation separate from the content viewport is important for DSR:
// 3840x2160 must remain a real 3840x2160 target even in original 4:3 mode.
static PsyXPresentationViewport PsyX_GetRenderTargetExtent() {
  const int width =
      g_cfg_renderWidth > 0 ? g_cfg_renderWidth : std::max(g_windowWidth, 1);
  const int height =
      g_cfg_renderHeight > 0 ? g_cfg_renderHeight : std::max(g_windowHeight, 1);
  return PsyXPresentationViewport{0, 0, width, height};
}

// Aspect correction is a viewport inside the exact target, never a smaller
// allocation. Adaptive mode occupies it completely; original mode centers 4:3.
static PsyXPresentationViewport PsyX_GetRenderViewport() {
  const PsyXPresentationViewport target = PsyX_GetRenderTargetExtent();
  return PsyX_CalculatePresentationViewport(target.w, target.h,
                                            g_cfg_aspectMode);
}

PsyXPresentationScale PsyX_CalculatePresentationScale(int drawableWidth,
                                                      int drawableHeight,
                                                      int aspectMode) {
  PsyXPresentationScale scale = {1.0f, 1.0f};
  if (aspectMode != PSYX_ASPECT_ADAPTIVE)
    return scale;

  if (drawableWidth < 1)
    drawableWidth = 1;
  if (drawableHeight < 1)
    drawableHeight = 1;
  const float targetAspect = (float)drawableWidth / (float)drawableHeight;
  const float originalAspect = 4.0f / 3.0f;
  if (targetAspect > originalAspect)
    scale.x = originalAspect / targetAspect;
  else if (targetAspect < originalAspect)
    scale.y = targetAspect / originalAspect;
  return scale;
}

static PsyXPresentationScale PsyX_GetPresentationScale() {
  // Hor+/Vert+ is a property of the final output aspect, not of the internal
  // color target. Otherwise a 4:3 internal target on a 16:9 display would
  // collapse adaptive presentation back to 4:3.
  const PsyXPresentationViewport viewport = PsyX_GetPresentationViewport();
  return PsyX_CalculatePresentationScale(viewport.w, viewport.h,
                                         g_cfg_aspectMode);
}

int g_dbg_wireframeMode = 0;
int g_dbg_texturelessMode = 0;

int g_cfg_pgxpTextureCorrection = 1;
int g_cfg_pgxpZBuffer = 0;
int g_cfg_bilinearFiltering = 0;
int g_cfg_trilinearFiltering = 0;
int g_cfg_anisotropicFiltering = 0;
int g_cfg_smaa = 0;
int g_cfg_fxaa = 0;
int g_cfg_volumetricFog = 0;
int g_cfg_volumetricEffects = 0;
int g_cfg_msaaSamples = 0;
int g_cfg_aspectMode = PSYX_ASPECT_ORIGINAL_4_3;
int g_cfg_composedGuestScanout = 0;
int g_cfg_smaaFinalFrame = 0;
int g_cfg_fxaaFinalFrame = 0;

int vram_need_update = 1;
int framebuffer_need_update = 0;
static int g_requestedSwapInterval = -1000;
static int g_appliedSwapInterval = 0;

static constexpr int gr_vram_dirty_word_bits = 64;
static constexpr int gr_vram_dirty_word_count =
    (VRAM_WIDTH + gr_vram_dirty_word_bits - 1) / gr_vram_dirty_word_bits;

struct GrVRAMDirtyRows {
  std::array<std::array<std::uint64_t, gr_vram_dirty_word_count>, VRAM_HEIGHT>
      rows{};
};

static GrVRAMDirtyRows g_vramDirtyRows[2];

static std::uint64_t GR_VRAMDirtyMask(int word, int x0, int x1) {
  const int word_x0 = word * gr_vram_dirty_word_bits;
  const int begin = std::max(x0 - word_x0, 0);
  const int end = std::min(x1 - word_x0, gr_vram_dirty_word_bits);
  if (begin >= end)
    return 0U;
  const std::uint64_t low =
      begin == 0 ? ~std::uint64_t{} : (~std::uint64_t{} << begin);
  const std::uint64_t high =
      end == gr_vram_dirty_word_bits
          ? ~std::uint64_t{}
          : ((std::uint64_t{1} << end) - std::uint64_t{1});
  return low & high;
}

static bool GR_IsVRAMDirtyPixel(const GrVRAMDirtyRows &dirty, int x, int y) {
  return (dirty.rows[y][x / gr_vram_dirty_word_bits] &
          (std::uint64_t{1} << (x % gr_vram_dirty_word_bits))) != 0U;
}

static void GR_CollectVRAMDirtyRuns(const GrVRAMDirtyRows &dirty, int row,
                                    std::vector<std::array<int, 2>> &runs) {
  runs.clear();
  if (std::none_of(dirty.rows[row].begin(), dirty.rows[row].end(),
                   [](std::uint64_t word) { return word != 0U; }))
    return;
  int x = 0;
  while (x < VRAM_WIDTH) {
    while (x < VRAM_WIDTH && !GR_IsVRAMDirtyPixel(dirty, x, row))
      ++x;
    const int x0 = x;
    while (x < VRAM_WIDTH && GR_IsVRAMDirtyPixel(dirty, x, row))
      ++x;
    if (x0 < x)
      runs.push_back({x0, x});
  }
}

static void GR_ClearVRAMDirtyRect(GrVRAMDirtyRows &dirty, int x, int y, int w,
                                  int h) {
  const int x1 = x + w;
  for (int row = y; row < y + h; ++row) {
    for (int word = x / gr_vram_dirty_word_bits;
         word <= (x1 - 1) / gr_vram_dirty_word_bits; ++word) {
      dirty.rows[row][word] &= ~GR_VRAMDirtyMask(word, x, x1);
    }
  }
}
static void GR_MarkVRAMDirtyRect(GrVRAMDirtyRows &dirty, int x, int y, int w,
                                 int h) {
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(VRAM_WIDTH, x + w);
  const int y1 = std::min(VRAM_HEIGHT, y + h);
  if (x0 >= x1 || y0 >= y1)
    return;
  for (int row = y0; row < y1; ++row) {
    for (int word = x0 / gr_vram_dirty_word_bits;
         word <= (x1 - 1) / gr_vram_dirty_word_bits; ++word) {
      dirty.rows[row][word] |= GR_VRAMDirtyMask(word, x0, x1);
    }
  }
}

static bool GR_IsVRAMDirtyRect(const GrVRAMDirtyRows &dirty, int x, int y,
                               int w, int h) {
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(VRAM_WIDTH, x + w);
  const int y1 = std::min(VRAM_HEIGHT, y + h);
  if (x0 >= x1 || y0 >= y1)
    return false;
  for (int row = y0; row < y1; ++row)
    for (int word = x0 / gr_vram_dirty_word_bits;
         word <= (x1 - 1) / gr_vram_dirty_word_bits; ++word)
      if ((dirty.rows[row][word] & GR_VRAMDirtyMask(word, x0, x1)) != 0U)
        return true;
  return false;
}

namespace {
#if defined(RENDERER_OGL)
static void GR_MarkGuestColorDirty(int x, int y, int width, int height);
#endif
} // namespace

static void GR_ResetVRAMDirtyRects() {
  for (int texture = 0; texture < 2; ++texture) {
    for (auto &row : g_vramDirtyRows[texture].rows)
      row.fill(~std::uint64_t{});
  }
  vram_need_update = 1;
#if defined(RENDERER_OGL)
  GR_MarkGuestColorDirty(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
#endif
}

static void GR_MarkVRAMDirty(int x, int y, int w, int h) {
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(VRAM_WIDTH, x + w);
  const int y1 = std::min(VRAM_HEIGHT, y + h);
  if (x0 >= x1 || y0 >= y1)
    return;

  for (int texture = 0; texture < 2; ++texture) {
    GR_MarkVRAMDirtyRect(g_vramDirtyRows[texture], x0, y0, x1 - x0, y1 - y0);
  }
  vram_need_update = 1;
}

#if defined(__EMSCRIPTEN__) || defined(__RPI__) || defined(__ANDROID__)
#if defined(RENDERER_OGL)
#error It should not be enabled
#endif
#endif

#if USE_OPENGL
typedef struct {
  GLenum fmt;
  GLuint *pbos;
  uint64_t num_pbos;
  uint64_t dx;
  uint64_t num_downloads;

  int width;
  int height;
  int nbytes;            /* number of bytes in the pbo buffer. */
  unsigned char *pixels; /* the downloaded pixels. */
} GrPBO;

int PBO_Init(GrPBO *pbo, GLenum format, int w, int h, int num) {
  if (pbo->pbos) {
    eprinterr("Already initialized. Not necessary to initialize again; or "
              "shutdown first.");
    return -1;
  }

  if (0 >= num) {
    eprinterr("Invalid number of PBOs: %d", num);
    return -2;
  }

  pbo->fmt = format;
  pbo->width = w;
  pbo->height = h;
  pbo->num_pbos = num;

#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

#if USE_PBO
  if (GL_RED == pbo->fmt || GL_GREEN == pbo->fmt || GL_BLUE == pbo->fmt) {
    pbo->nbytes = pbo->width * pbo->height;
  } else if (GL_RGB == pbo->fmt || GL_BGR == pbo->fmt) {
    pbo->nbytes = pbo->width * pbo->height * 3;
  } else if (GL_RGBA == pbo->fmt || GL_BGRA == pbo->fmt) {
    pbo->nbytes = pbo->width * pbo->height * 4;
  } else {
    eprinterr("Unhandled pixel format, use GL_R, GL_RG, GL_RGB or GL_RGBA.");
    return -3;
  }

  if (pbo->nbytes == 0) {
    eprinterr("Invalid width or height given: %d x %d", pbo->width,
              pbo->height);
    return -4;
  }

  pbo->pbos = (GLuint *)malloc(sizeof(GLuint) * num);
  pbo->pixels = (u_char *)malloc(pbo->nbytes);

  glGenBuffers(num, pbo->pbos);
  for (int i = 0; i < num; ++i) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[i]);
    glBufferData(GL_PIXEL_PACK_BUFFER, pbo->nbytes, NULL, GL_STREAM_READ);
  }

  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#endif
  return 0;
}

void PBO_Destroy(GrPBO *pbo) {
#if USE_PBO
  if (pbo->pbos) {
    glDeleteBuffers(pbo->num_pbos, pbo->pbos);

    free(pbo->pbos);
    pbo->num_pbos = 0;
    pbo->pbos = NULL;
  }

#endif
  if (pbo->pixels) {
    free(pbo->pixels);
    pbo->pixels = NULL;
  }

  pbo->num_downloads = 0;
  pbo->dx = 0;
  pbo->fmt = 0;
  pbo->nbytes = 0;
}

void PBO_Download(GrPBO *pbo) {
  unsigned char *ptr;

#if USE_PBO
  if (pbo->num_downloads < pbo->num_pbos) {
    /*
       First we need to make sure all our pbos are bound, so glMap/Unmap will
       read from the oldest bound buffer first.
    */
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
    glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
    glReadPixels(0, 0, pbo->width, pbo->height, pbo->fmt, GL_UNSIGNED_BYTE,
                 0); /* When a GL_PIXEL_PACK_BUFFER is bound, the last 0 is used
                        as offset into the buffer to read into. */
#endif
  } else {
    /* Read from the oldest bound pbo */
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
    ptr = (unsigned char *)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (NULL != ptr) {
      memcpy(pbo->pixels, ptr, pbo->nbytes);
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    } else
      eprintwarn("Failed to map the buffer\n");

    /* Trigger the next read. */
    glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
    glReadPixels(0, 0, pbo->width, pbo->height, GL_RGBA, GL_UNSIGNED_BYTE,
                 pbo->pixels);
#endif
  }

  ++pbo->dx;
  pbo->dx = pbo->dx % pbo->num_pbos;

  pbo->num_downloads++;

  if (pbo->num_downloads == UINT64_MAX)
    pbo->num_downloads = pbo->num_pbos;

  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#else
  // FIXME: THIS is very slow
  // Do not use at all

  // glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); /* just make sure we're not
  // accidentilly using a PBO. */ glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA,
  // GL_UNSIGNED_BYTE, pbo->pixels);
#endif
}

GLuint g_glVertexArray[MAX_NUM_VERTEX_BUFFERS];
GLuint g_glVertexBuffer[MAX_NUM_VERTEX_BUFFERS];
int g_curVertexBuffer = 0;

GLuint g_glBlitFramebuffer;
GrPBO g_glFramebufferPBO;

GLuint g_glVRAMFramebuffer;

GLuint g_glOffscreenFramebuffer;
GLuint g_glOffscreenDepthStencilRenderbuffer;
GLuint g_glOffscreenDepthRenderbuffer;
GLuint g_glOffscreenStencilRenderbuffer;
GrPBO g_glOffscreenPBO;
GLuint g_glGuestFeedbackFramebuffer;
TextureID g_guestFeedbackTexture;
static int g_guestFeedbackWidth{};
static int g_guestFeedbackHeight{};
// Active raster extent is independent of immutable backing storage capacity.
static int g_offscreenTextureWidth{};
static int g_offscreenTextureHeight{};
static int g_offscreenTextureCapacityWidth{};
static int g_offscreenTextureCapacityHeight{};
#if defined(RENDERER_OGL)
static GLuint g_guestPresentationReplayTexture{};
static GLuint g_glGuestPresentationReplayFramebuffer{};
static GLuint g_guestPresentationReplayDepthStencilRenderbuffer{};
static RECT16 g_guestPresentationReplayRect{};
static int g_guestPresentationReplayPixelWidth{};
static int g_guestPresentationReplayPixelHeight{};
static int g_guestPresentationReplayCapacityWidth{};
static int g_guestPresentationReplayCapacityHeight{};
static int g_guestPresentationReplayActive{};
static int g_guestPresentationReplayClosing{};
static int g_guestPresentationReplayFailed{};
static int g_guestPresentationReplayValid{};
#endif

static constexpr std::size_t high_resolution_vram_cache_capacity = 2U;

struct GrHighResolutionVRAMPage {
  GLuint texture{};
  RECT16 rect{};
  int pixel_width{};
  int pixel_height{};
  unsigned long long generation{};
  GrVRAMDirtyRows pending_writes{};
  unsigned long long synced_write_sequence{};
  bool valid{};
};
static const GrHighResolutionVRAMPage *
GR_FindHighResolutionVRAMPage(int x, int y, int width, int height);

GLuint g_glHighResolutionVRAMFramebuffer;
static GLuint g_guestScanoutTexture{};
static int g_guestScanoutWidth{};
static int g_guestScanoutHeight{};
static unsigned long long g_guestScanoutPresentCount{};
static GLuint g_glGuestColorFramebuffer{};
static GLuint g_guestColorTexture{};
static GLuint g_glGuestScanoutFramebuffer{};
#if defined(RENDERER_OGL)
static constexpr GLint g_rgbaRenderTargetInternalFormat = GL_RGBA8;
#else
static constexpr GLint g_rgbaRenderTargetInternalFormat = GL_RGBA;
#endif

static std::array<GrHighResolutionVRAMPage, high_resolution_vram_cache_capacity>
    g_highResolutionVRAMPages{};
static unsigned long long g_highResolutionVRAMGeneration{};

static bool GR_RectanglesOverlap(int first_x, int first_y, int first_width,
                                 int first_height, int second_x, int second_y,
                                 int second_width, int second_height) {
  return first_x < second_x + second_width &&
         second_x < first_x + first_width &&
         first_y < second_y + second_height &&
         second_y < first_y + first_height;
}

static void GR_InvalidateHighResolutionVRAM(int x, int y, int width,
                                            int height) {
  if (width <= 0 || height <= 0)
    return;
  for (auto &page : g_highResolutionVRAMPages) {
    if (page.valid &&
        GR_RectanglesOverlap(page.rect.x, page.rect.y, page.rect.w, page.rect.h,
                             x, y, width, height)) {
      page.valid = false;
    }
  }
}

GLuint g_glNativeFramebuffer;
GLuint g_glNativeColorTexture;
GLuint g_glNativeDepthRenderbuffer;
GLuint g_glNativeStencilRenderbuffer;
GLuint g_glNativeDepthTexture;
GLuint g_glNativeMultisampleFramebuffer;
GLuint g_glNativeMultisampleColorRenderbuffer;
GLuint g_glNativeMultisampleDepthRenderbuffer;
int g_nativeFramebufferWidth;
int g_nativeFramebufferHeight;
int g_nativeFramebufferSamples;
static int g_nativeFramePostprocessed;
static float g_reversedDepthScale = -1.0f;
static float g_reversedDepthBias = 0.0f;
namespace {
void PsyX_DestroyAtmosphere();
void PsyX_DestroySMAA();
void PsyX_DestroyFXAA();
void PsyX_DestroyVolumetrics();
void PsyX_DestroyObjectShadows();
#if defined(RENDERER_OGL)
static void GR_DestroyGuestVRAMConversion();
#endif
} // namespace

#if defined(RENDERER_OGL)
static GLenum g_nativeDepthInternalFormat = GL_DEPTH32F_STENCIL8;

static void PsyX_AllocateNativeDepthTexture(GLenum internalFormat, int width,
                                            int height) {
  glBindTexture(GL_TEXTURE_2D, g_glNativeDepthTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (internalFormat == GL_DEPTH32F_STENCIL8) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8, width, height, 0,
                 GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, NULL);
  } else {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
  }
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                         GL_TEXTURE_2D, g_glNativeDepthTexture, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
}

#endif

static void PsyX_AllocateOffscreenDepthStencil(int width, int height) {
  glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);
#if defined(RENDERER_OGLES) && OGLES_VERSION == 2
  glBindRenderbuffer(GL_RENDERBUFFER, g_glOffscreenDepthRenderbuffer);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, g_glOffscreenDepthRenderbuffer);

  glBindRenderbuffer(GL_RENDERBUFFER, g_glOffscreenStencilRenderbuffer);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER, g_glOffscreenStencilRenderbuffer);
#else
  glBindRenderbuffer(GL_RENDERBUFFER, g_glOffscreenDepthStencilRenderbuffer);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER,
                            g_glOffscreenDepthStencilRenderbuffer);
#endif
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

static GLuint PsyX_GetNativeDrawFramebuffer() {
  if (g_nativeFramePostprocessed)
    return g_glNativeFramebuffer;
  return g_nativeFramebufferSamples > 1 ? g_glNativeMultisampleFramebuffer
                                        : g_glNativeFramebuffer;
}

static int PsyX_EnsureNativeFramebuffer() {
  const PsyXPresentationViewport nativeViewport = PsyX_GetRenderTargetExtent();
  if (g_nativeFramebufferWidth == nativeViewport.w &&
      g_nativeFramebufferHeight == nativeViewport.h &&
      g_nativeFramebufferSamples == g_cfg_msaaSamples) {
    glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
    return 1;
  }

#if defined(RENDERER_OGLES) && OGLES_VERSION == 2
  const GLenum nativeColorFormat = GL_RGBA;
#else
  const GLenum nativeColorFormat = GL_RGBA8;
#endif
  glBindTexture(GL_TEXTURE_2D, g_glNativeColorTexture);
  glTexImage2D(GL_TEXTURE_2D, 0, nativeColorFormat, nativeViewport.w,
               nativeViewport.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glBindTexture(GL_TEXTURE_2D, 0);

  glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeFramebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g_glNativeColorTexture, 0);

#if defined(RENDERER_OGLES) && OGLES_VERSION == 2
  glBindRenderbuffer(GL_RENDERBUFFER, g_glNativeDepthRenderbuffer);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, nativeViewport.w,
                        nativeViewport.h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, g_glNativeDepthRenderbuffer);

  glBindRenderbuffer(GL_RENDERBUFFER, g_glNativeStencilRenderbuffer);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, nativeViewport.w,
                        nativeViewport.h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER, g_glNativeStencilRenderbuffer);
#else
  PsyX_AllocateNativeDepthTexture(g_nativeDepthInternalFormat, nativeViewport.w,
                                  nativeViewport.h);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    // Some older Windows OpenGL drivers expose 32F depth but reject it as a
    // framebuffer attachment. Preserve reversed-Z with their packed D24S8
    // path instead of aborting renderer initialization.
    g_nativeDepthInternalFormat = GL_DEPTH24_STENCIL8;
    PsyX_AllocateNativeDepthTexture(g_nativeDepthInternalFormat,
                                    nativeViewport.w, nativeViewport.h);
    eprintwarn("32F depth unavailable; using reversed D24S8\n");
  }
#endif

  glBindRenderbuffer(GL_RENDERBUFFER, 0);

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    eprinterr("Failed to create native PSX framebuffer (%dx%d)\n",
              nativeViewport.w, nativeViewport.h);
    return 0;
  }

  g_nativeFramebufferSamples = 0;
#if USE_FRAMEBUFFER_BLIT
  if (g_cfg_msaaSamples > 1) {
    glBindRenderbuffer(GL_RENDERBUFFER, g_glNativeMultisampleColorRenderbuffer);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_cfg_msaaSamples,
                                     GL_RGBA8, nativeViewport.w,
                                     nativeViewport.h);

    glBindRenderbuffer(GL_RENDERBUFFER, g_glNativeMultisampleDepthRenderbuffer);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_cfg_msaaSamples,
                                     g_nativeDepthInternalFormat,
                                     nativeViewport.w, nativeViewport.h);

    glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeMultisampleFramebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER,
                              g_glNativeMultisampleColorRenderbuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER,
                              g_glNativeMultisampleDepthRenderbuffer);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE &&
        g_nativeDepthInternalFormat == GL_DEPTH32F_STENCIL8) {
      // Depth blits require compatible source and destination formats. If
      // multisampled D32FS8 is rejected, downgrade both attachments together.
      g_nativeDepthInternalFormat = GL_DEPTH24_STENCIL8;
      glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeFramebuffer);
      PsyX_AllocateNativeDepthTexture(g_nativeDepthInternalFormat,
                                      nativeViewport.w, nativeViewport.h);
      glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeMultisampleFramebuffer);
      glBindRenderbuffer(GL_RENDERBUFFER,
                         g_glNativeMultisampleDepthRenderbuffer);
      glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_cfg_msaaSamples,
                                       GL_DEPTH24_STENCIL8, nativeViewport.w,
                                       nativeViewport.h);
      eprintwarn("32F MSAA depth unavailable; using reversed D24S8\n");
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
      g_nativeFramebufferSamples = g_cfg_msaaSamples;
    } else {
      eprintwarn("MSAA framebuffer is unavailable; falling back to disabled\n");
      g_cfg_msaaSamples = 0;
    }
  }
#else
  g_cfg_msaaSamples = 0;
#endif

  g_nativeFramebufferWidth = nativeViewport.w;
  g_nativeFramebufferHeight = nativeViewport.h;
  eprintf("*Internal render target: %dx%d, MSAA: %dx\n",
          g_nativeFramebufferWidth, g_nativeFramebufferHeight,
          g_nativeFramebufferSamples);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
  return 1;
}

static void PsyX_ResolveNativeFramebuffer(int preserveDepthStencil = 0) {
#if USE_FRAMEBUFFER_BLIT
  if (g_nativeFramebufferSamples <= 1 || g_nativeFramePostprocessed)
    return;

  const int scissorEnabled = g_PreviousScissorState;
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glNativeMultisampleFramebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glNativeFramebuffer);
  GLbitfield mask = GL_COLOR_BUFFER_BIT;
  if (preserveDepthStencil)
    mask |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
  glBlitFramebuffer(0, 0, g_nativeFramebufferWidth, g_nativeFramebufferHeight,
                    0, 0, g_nativeFramebufferWidth, g_nativeFramebufferHeight,
                    mask, GL_NEAREST);
  if (scissorEnabled)
    glEnable(GL_SCISSOR_TEST);
#endif
}

static void PsyX_PresentNativeFramebuffer() {
  if (g_nativeFramebufferWidth <= 0 || g_nativeFramebufferHeight <= 0)
    return;

  const PsyXPresentationViewport viewport = PsyX_GetPresentationViewport();
  const PsyXPresentationViewport source = PsyX_GetRenderViewport();
  const int scissorEnabled = g_PreviousScissorState;
  glDisable(GL_SCISSOR_TEST);
  PsyX_ResolveNativeFramebuffer();

  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glNativeFramebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBlitFramebuffer(source.x, source.y, source.x + source.w,
                    source.y + source.h, viewport.x, viewport.y,
                    viewport.x + viewport.w, viewport.y + viewport.h,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (scissorEnabled)
    glEnable(GL_SCISSOR_TEST);
}

#endif

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
int GR_InitialiseGLContext(char *windowName, int fullscreen) {
  int windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;

#if defined(__ANDROID__)
  windowFlags |= SDL_WINDOW_FULLSCREEN;
#else
  if (fullscreen)
    windowFlags |= SDL_WINDOW_FULLSCREEN;
#endif

  if (g_windowWidth <= 0 || g_windowHeight <= 0)
    windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

  g_window = SDL_CreateWindow(windowName, SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, g_windowWidth,
                              g_windowHeight, windowFlags);

  if (g_window == NULL) {
    eprinterr("Failed to initialise SDL window!\n");
    return 0;
  }

#if defined(RENDERER_OGLES)

#if defined(__ANDROID__)
  // Override to full screen.
  SDL_DisplayMode displayMode;
  if (SDL_GetCurrentDisplayMode(0, &displayMode) == 0) {
    screenWidth = displayMode.w;
    windowWidth = displayMode.w;
    screenHeight = displayMode.h;
    windowHeight = displayMode.h;
  }
#endif

  // SDL_GL_SetAttribute(SDL_GL_CONTEXT_EGL, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, OGLES_VERSION);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

  if (!SDL_GL_CreateContext(g_window)) {
    eprinterr("Failed to initialise - OpenGL ES %d.x is not supported.\n",
              OGLES_VERSION);
    return 0;
  }

#elif defined(RENDERER_OGL)

  int major_version = 3;
  int minor_version = 3;
  int profile = SDL_GL_CONTEXT_PROFILE_CORE;

  // find best OpenGL version
  do {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major_version);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor_version);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);

    if (SDL_GL_CreateContext(g_window))
      break;

    minor_version--;

  } while (minor_version >= 0);

  if (minor_version == -1) {
    eprinterr("Failed to initialise - OpenGL 3.x is not supported. Please "
              "update video drivers.\n");
    return 0;
  }
#endif

  return 1;
}
#endif

int GR_InitialiseGLExt() {
#ifdef USE_GLAD
  GLenum err = gladLoadGL();

  if (err == 0)
    return 0;
#endif

  const char *rend = (const char *)glGetString(GL_RENDERER);
  const char *vendor = (const char *)glGetString(GL_VENDOR);
  eprintf("*Video adapter: %s by %s\n", rend, vendor);

  const char *versionStr = (const char *)glGetString(GL_VERSION);
  eprintf("*OpenGL version: %s\n", versionStr);

  const char *glslVersionStr =
      (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
  eprintf("*GLSL version: %s\n", glslVersionStr);

  if (SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic")) {
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &g_maxTextureAnisotropy);
    if (g_maxTextureAnisotropy < 1.0f)
      g_maxTextureAnisotropy = 1.0f;
  }
  eprintf("*Hardware anisotropy: %.0fx\n", g_maxTextureAnisotropy);

#if USE_FRAMEBUFFER_BLIT
  GLint maxSamples = 0;
  glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
  const int requestedSamples = g_cfg_msaaSamples;
  g_cfg_msaaSamples = 0;
  const int supportedSamples[] = {8, 4, 2};
  for (int samples : supportedSamples) {
    if (samples <= requestedSamples && samples <= maxSamples) {
      g_cfg_msaaSamples = samples;
      break;
    }
  }
  if (requestedSamples > 1 && g_cfg_msaaSamples != requestedSamples)
    eprintwarn("Requested %dx MSAA, using %dx (driver maximum: %d)\n",
               requestedSamples, g_cfg_msaaSamples, maxSamples);
#else
  g_cfg_msaaSamples = 0;
#endif
  eprintf("*MSAA samples: %d\n", g_cfg_msaaSamples);

  return 1;
}

int GR_InitialiseRender(char *windowName, int width, int height,
                        int fullscreen) {
  g_appliedOffscreenProjectionRevision = 0U;
  g_appliedOffscreenProjectionValid = 0;
  g_windowWidth = width;
  g_windowHeight = height;
  g_drawableWidth = width;
  g_drawableHeight = height;
  g_pendingDrawableWidth = width;
  g_pendingDrawableHeight = height;
  g_pendingDrawableRevision = 1;
  g_committedDrawableRevision = 1;

  // Due to debugging in fullscreen
  SDL_SetHint(SDL_HINT_ALLOW_TOPMOST, "0");
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitor");
#endif

#if USE_OPENGL
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
  if (!GR_InitialiseGLContext(windowName, fullscreen)) {
    eprinterr("Failed to Initialise GL Context!\n");
    return 0;
  }
#endif

  if (!GR_InitialiseGLExt()) {
    eprinterr("Failed to Intialise GL extensions\n");
    return 0;
  }
#endif

  return 1;
}

void GR_Shutdown() {
#if USE_OPENGL
  PsyX_DestroyAtmosphere();
  PsyX_DestroySMAA();
  PsyX_DestroyFXAA();
  PsyX_DestroyVolumetrics();
  PsyX_DestroyObjectShadows();
#if defined(RENDERER_OGL)
  GR_DestroyGuestVRAMConversion();
#endif
  glDeleteVertexArrays(MAX_NUM_VERTEX_BUFFERS, g_glVertexArray);
  glDeleteBuffers(MAX_NUM_VERTEX_BUFFERS, g_glVertexBuffer);

  PBO_Destroy(&g_glFramebufferPBO);
  PBO_Destroy(&g_glOffscreenPBO);

  glDeleteFramebuffers(1, &g_glBlitFramebuffer);
  glDeleteFramebuffers(1, &g_glOffscreenFramebuffer);
  glDeleteFramebuffers(1, &g_glHighResolutionVRAMFramebuffer);
  glDeleteFramebuffers(1, &g_glGuestFeedbackFramebuffer);
  glDeleteFramebuffers(1, &g_glGuestColorFramebuffer);
  glDeleteFramebuffers(1, &g_glGuestScanoutFramebuffer);
  for (auto &page : g_highResolutionVRAMPages) {
    if (page.texture != 0U)
      glDeleteTextures(1, &page.texture);
    page = {};
  }
  glDeleteRenderbuffers(1, &g_glOffscreenDepthStencilRenderbuffer);
  glDeleteRenderbuffers(1, &g_glOffscreenDepthRenderbuffer);
  glDeleteRenderbuffers(1, &g_glOffscreenStencilRenderbuffer);
  glDeleteFramebuffers(1, &g_glVRAMFramebuffer);
  glDeleteFramebuffers(1, &g_glNativeFramebuffer);
  glDeleteFramebuffers(1, &g_glNativeMultisampleFramebuffer);
  glDeleteRenderbuffers(1, &g_glNativeDepthRenderbuffer);
  glDeleteRenderbuffers(1, &g_glNativeStencilRenderbuffer);
  glDeleteRenderbuffers(1, &g_glNativeMultisampleColorRenderbuffer);
  glDeleteRenderbuffers(1, &g_glNativeMultisampleDepthRenderbuffer);
  glDeleteTextures(1, &g_glNativeColorTexture);
#if defined(RENDERER_OGL)
  glDeleteTextures(1, &g_glNativeDepthTexture);
  glDeleteTextures(1, &g_guestScanoutTexture);
  glDeleteTextures(1, &g_guestColorTexture);
  glDeleteTextures(1, &g_guestPresentationReplayTexture);
  glDeleteFramebuffers(1, &g_glGuestPresentationReplayFramebuffer);
  glDeleteRenderbuffers(1, &g_guestPresentationReplayDepthStencilRenderbuffer);
  g_guestScanoutTexture = 0U;
  g_guestColorTexture = 0U;
  g_glGuestColorFramebuffer = 0U;
  g_glGuestScanoutFramebuffer = 0U;
  g_guestPresentationReplayTexture = 0U;
  g_glGuestPresentationReplayFramebuffer = 0U;
  g_guestPresentationReplayDepthStencilRenderbuffer = 0U;
  g_guestPresentationReplayRect = {};
  g_guestPresentationReplayPixelWidth = 0;
  g_guestPresentationReplayPixelHeight = 0;
  g_guestPresentationReplayCapacityWidth = 0;
  g_guestPresentationReplayCapacityHeight = 0;
  g_guestPresentationReplayActive = 0;
  g_guestPresentationReplayClosing = 0;
  g_guestPresentationReplayFailed = 0;
  g_guestPresentationReplayValid = 0;
#endif

  GR_DestroyTexture(g_vramTexturesDouble[0]);
  GR_DestroyTexture(g_vramTexturesDouble[1]);
  GR_DestroyTexture(g_vramAliasTexture);

  GR_DestroyTexture(g_whiteTexture);
  GR_DestroyTexture(g_rgLutTexture);
  GR_DestroyTexture(g_fbTexture);
  GR_DestroyTexture(g_offscreenRTTexture);
  GR_DestroyTexture(g_guestFeedbackTexture);
  g_guestFeedbackTexture = 0U;
  g_offscreenRTTexture = 0U;
  g_guestFeedbackWidth = 0;
  g_guestFeedbackHeight = 0;
  g_offscreenTextureWidth = 0;
  g_offscreenTextureHeight = 0;
  g_offscreenTextureCapacityWidth = 0;
  g_offscreenTextureCapacityHeight = 0;
  g_guestScanoutWidth = 0;
  g_guestScanoutHeight = 0;
  g_guestScanoutPresentCount = 0U;
#endif
  g_PreviousDepthRangeLower = -1.0f;
  g_PreviousDepthRangeUpper = -1.0f;
  g_nativeDepthClearSerial = 0U;
  g_offscreenDepthClearSerial = 0U;
  GR_ResetWorldDepthEpochs();
}

int GR_UpdateSwapIntervalState(int swapInterval) {
#if defined(RENDERER_OGL)
  if (g_requestedSwapInterval == swapInterval)
    return g_appliedSwapInterval;

  g_requestedSwapInterval = swapInterval;
  if (SDL_GL_SetSwapInterval(swapInterval) == 0) {
    g_appliedSwapInterval = SDL_GL_GetSwapInterval();
  } else {
    static_cast<void>(SDL_GL_SetSwapInterval(0));
    g_appliedSwapInterval = 0;
  }
  return g_appliedSwapInterval;
#else
  (void)swapInterval;
  return 0;
#endif
}

void GR_BeginScene() {
  PsyX_CommitDrawableSize();
  g_lastBoundTexture = 0;

#if USE_OPENGL
  g_nativeFramePostprocessed = 0;
  PsyX_EnsureNativeFramebuffer();
  GR_SetDepthRange(0.0f, 1.0f);
  // glClear obeys the depth write mask. The previous frame normally ends in
  // the depth-free HUD pass, so restore writes before clearing world depth.
  glDisable(GL_SCISSOR_TEST);
  g_PreviousScissorState = 0;
  glDepthMask(GL_TRUE);
  g_PreviousDepthWrite = -1;
#ifdef RENDERER_OGLES
  glClearDepthf(0.0f);
#else
  glClearDepth(0.0f);
#endif
  glClear(GL_DEPTH_BUFFER_BIT);
  glClear(GL_STENCIL_BUFFER_BIT);
  ++g_nativeDepthClearSerial;
#endif

  GR_UpdateVRAM();
  const PsyXPresentationViewport viewport = PsyX_GetRenderViewport();
  GR_SetViewPort(viewport.x, viewport.y, viewport.w, viewport.h);

  if (g_dbg_wireframeMode) {
    GR_SetWireframe(1);

#if USE_OPENGL
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
#endif
  }
}

void GR_EndScene() {
  // Finish a pending VRAM/offscreen pass before the native display image is
  // stored or presented. This also restores the native framebuffer binding.
#if defined(RENDERER_OGL)
  if (g_guestPresentationReplayActive) {
    g_guestPresentationReplayFailed = 1;
    static_cast<void>(GR_EndGuestPresentationReplay());
  }
#endif
  if (g_PreviousOffscreenState)
    GR_SetOffscreenState(&g_PreviousOffscreen, 0);

  framebuffer_need_update = 1;

  if (g_dbg_wireframeMode)
    GR_SetWireframe(0);

#if USE_OPENGL
  glBindVertexArray(0);
#endif

  if (g_cfg_smaaFinalFrame || g_cfg_fxaaFinalFrame)
    GR_ApplySceneAntialiasing();
}

//----------------------------------------------------------------------------------------

unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];

static void GR_UploadVRAMRegionToAllTextures(int x, int y, int w, int h) {
#if USE_OPENGL
  glActiveTexture(GL_TEXTURE0);
#if defined(RENDERER_OGL) || (defined(RENDERER_OGLES) && OGLES_VERSION >= 3)
  glPixelStorei(GL_UNPACK_ROW_LENGTH, VRAM_WIDTH);
  for (int texture = 0; texture < 2; ++texture) {
    glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[texture]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, VRAM_FORMAT, GL_UNSIGNED_BYTE,
                    vram + y * VRAM_WIDTH + x);
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#else
  for (int texture = 0; texture < 2; ++texture) {
    glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[texture]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VRAM_WIDTH, VRAM_HEIGHT,
                    VRAM_FORMAT, GL_UNSIGNED_BYTE, vram);
  }
#endif
  glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);
#endif
}

static unsigned short g_vramAliasPages[VRAM_ALIAS_WIDTH * VRAM_ALIAS_HEIGHT]{};

// A bounded CPU-side journal for diagnosing texture-page corruption. Every
// LoadImage/MoveImage/ClearImage/framebuffer transfer ultimately reaches one
// of the helpers below, so the scene streamer can report the exact writes
// which touched a page after it was made resident without logging every GPU
// command during normal play.
static constexpr size_t GR_VRAM_WRITE_JOURNAL_CAPACITY = 512;
static std::array<GrVRAMWriteEvent, GR_VRAM_WRITE_JOURNAL_CAPACITY>
    g_vramWriteJournal{};
static unsigned long long g_vramWriteSequence = 0;

static void GR_RecordVRAMWrite(int kind, int source_x, int source_y,
                               int destination_x, int destination_y, int width,
                               int height,
                               bool invalidate_high_resolution = true) {
  if (width <= 0 || height <= 0)
    return;
#if defined(RENDERER_OGL)
  GR_MarkGuestColorDirty(destination_x, destination_y, width, height);
#endif
#if !defined(RENDERER_OGL)
  if (invalidate_high_resolution)
    GR_InvalidateHighResolutionVRAM(destination_x, destination_y, width,
                                    height);
#else
  // Desktop retained pages stay resident. Their per-page write cursor lazily
  // patches only the touched guest rectangles from the packed GPU VRAM before
  // the page is seeded or presented, so gameplay never alternates between a
  // high-resolution page and the quantized sprite fallback.
  (void)invalidate_high_resolution;
  if (kind != GR_VRAM_WRITE_FRAMEBUFFER) {
    for (auto &page : g_highResolutionVRAMPages) {
      if (!page.valid || !GR_RectanglesOverlap(
                             page.rect.x, page.rect.y, page.rect.w, page.rect.h,
                             destination_x, destination_y, width, height))
        continue;
      const int x0 = std::max<int>(page.rect.x, destination_x);
      const int y0 = std::max<int>(page.rect.y, destination_y);
      const int x1 =
          std::min<int>(page.rect.x + page.rect.w, destination_x + width);
      const int y1 =
          std::min<int>(page.rect.y + page.rect.h, destination_y + height);
      GR_MarkVRAMDirtyRect(page.pending_writes, x0, y0, x1 - x0, y1 - y0);
    }
  }
#endif
  const unsigned long long sequence = ++g_vramWriteSequence;
  g_vramWriteJournal[(sequence - 1) % GR_VRAM_WRITE_JOURNAL_CAPACITY] = {
      sequence,      kind,          source_x, source_y,
      destination_x, destination_y, width,    height};
}

unsigned long long GR_GetVRAMWriteSequence() { return g_vramWriteSequence; }

int GR_ReadVRAMWriteEvents(unsigned long long after_sequence,
                           GrVRAMWriteEvent *events, int capacity) {
  if (events == NULL || capacity <= 0)
    return 0;
  const unsigned long long current = g_vramWriteSequence;
  const unsigned long long oldest =
      current > GR_VRAM_WRITE_JOURNAL_CAPACITY
          ? current - GR_VRAM_WRITE_JOURNAL_CAPACITY
          : 0;
  unsigned long long sequence = std::max(after_sequence, oldest) + 1;
  int count = 0;
  while (sequence <= current && count < capacity) {
    events[count++] =
        g_vramWriteJournal[(sequence - 1) % GR_VRAM_WRITE_JOURNAL_CAPACITY];
    ++sequence;
  }
  return count;
}
static u_char rgLUT[LUT_WIDTH * LUT_HEIGHT * sizeof(u_int)];

void GR_ResetDevice() {
  // Drawable size changes only on window/fullscreen/display events. Keeping
  // the HiDPI query here avoids a window-system round trip on every frame.
  PsyX_UpdateDrawableSize();
  g_appliedSwapInterval = -1000;
  GR_UpdateSwapIntervalState(0);
}

typedef struct {
  // shader itself
  ShaderID shader;

#if USE_OPENGL
  GLint projectionLoc;
  GLint projection3DLoc;
  GLint presentationScaleLoc;
  GLint textureFilterModeLoc;
  GLint textureBlendModeLoc;
  GLint texelSizeLoc;
  GLint texLoc;
  GLint lutLoc;
  GLint aliasLoc;
  GLint sceneFogColorEnabledLoc;
  GLint sceneFogGteLoc;
  GLint sceneFogTerrainLoc;
  GLint appliedTextureFilterMode;
  unsigned int appliedSceneFogRevision;
#endif
} PSXGPU_Shader;

PSXGPU_Shader g_gpu_shader_4;
PSXGPU_Shader g_gpu_shader_8;
PSXGPU_Shader g_gpu_shader_16;
PSXGPU_Shader g_gpu_shader_32_rgba;

ShaderID g_PreviousTextureBlendShader = -1;
int g_PreviousTextureBlendMode = -1;

typedef struct {
  TextureID texture;
  GLint filterMode;
} GrTextureFilterCacheEntry;

static std::vector<GrTextureFilterCacheEntry> g_rgbaTextureFilterCache;

#if USE_OPENGL

GLint u_projectionLoc;
GLint u_projection3DLoc;
GLint u_presentationScaleLoc;
GLint u_texelSizeLoc;
GLint u_textureBlendModeLoc;
static PsyXPresentationScale g_presentationScale{1.0F, 1.0F};
static float g_cachedProjection[16]{};
static float g_cachedProjection3D[16]{};
static int g_cachedProjectionValid{};
static int g_cachedProjection3DValid{};

#define GPU_SAMPLE_TEXTURE_4BIT_FUNC                                           \
  "   // returns 16 bit colour\n"                                              \
  "   vec2 samplePSX(vec2 tc) {\n"                                             \
  "       vec2 texel = tc * vec2(0.25, 1.0) + v_page_clut.xy;\n"               \
  "       vec2 comp = PAGE(texel);\n"                                          \
  "       int index = int(fract(tc.x / 4.0 + 0.0001) * 4.0);\n"                \
  "       float v = _idx2(comp, index / 2) * (255.0 / 16.0);\n"                \
  "       float f = floor(v + 0.001);\n"                                       \
  "       vec2 c = vec2( (v - f) * 16.0, f );\n"                               \
  "       vec2 clut_pos = v_page_clut.zw;\n"                                   \
  "       clut_pos.x += mix(c[0], c[1], mod(float(index), 2.0)) * "            \
  "c_VRAMTexel.x;\n"                                                           \
  "       return VRAM(clut_pos);\n"                                            \
  "   }\n"

#define GPU_SAMPLE_TEXTURE_8BIT_FUNC                                           \
  "	// returns 16 bit colour\n"                                                \
  "	vec2 samplePSX(vec2 tc) {\n"                                               \
  "		vec2 texel = tc * vec2(0.5, 1.0) + v_page_clut.xy;\n"                     \
  "		vec2 comp = PAGE(texel);\n"                                               \
  "		vec2 clut_pos = v_page_clut.zw;\n"                                        \
  "		int index = int(mod(tc.x, 2.0));\n"                                       \
  "		clut_pos.x += _idx2(comp, index) * 255.0 * c_VRAMTexel.x;\n"              \
  "		vec2 color_rg = VRAM(clut_pos);\n"                                        \
  "		return VRAM(clut_pos);\n"                                                 \
  "	}\n"

#define GPU_SAMPLE_TEXTURE_16BIT_FUNC                                          \
  "	vec2 samplePSX(vec2 tc) {\n"                                               \
  "		vec2 texel = tc + v_page_clut.xy;\n"                                      \
  "		vec2 color_rg = PAGE(texel);\n"                                           \
  "		return color_rg;\n"                                                       \
  "	}\n"

#if (VRAM_FORMAT == GL_LUMINANCE_ALPHA)

#define GPU_FETCH_VRAM_FUNC                                                    \
  "	const vec2 c_VRAMTexel = vec2(1.0 / 1024.0, 1.0 / 512.0);\n"               \
  "	const vec2 c_AliasTexel = vec2(1.0 / 1024.0, 1.0 / 1024.0);\n"             \
  "	uniform sampler2D s_texture;\n"                                            \
  "	uniform sampler2D s_aliasTexture;\n"                                       \
  "	vec2 VRAM(vec2 uv) { return texture2D(s_texture, uv).ra; }\n"              \
  "	vec2 PAGE(vec2 texel) { return v_alias_page > 0.5 ? "                      \
  "texture2D(s_aliasTexture, texel * c_AliasTexel).ra : VRAM(texel * "         \
  "c_VRAMTexel); }\n"
#else

#define GPU_FETCH_VRAM_FUNC                                                    \
  "	const vec2 c_VRAMTexel = vec2(1.0 / 1024.0, 1.0 / 512.0);\n"               \
  "	const vec2 c_AliasTexel = vec2(1.0 / 1024.0, 1.0 / 1024.0);\n"             \
  "	uniform sampler2D s_texture;\n"                                            \
  "	uniform sampler2D s_aliasTexture;\n"                                       \
  "	vec2 VRAM(vec2 uv) { return texture2D(s_texture, uv).rg; }\n"              \
  "	vec2 PAGE(vec2 texel) { return v_alias_page > 0.5 ? "                      \
  "texture2D(s_aliasTexture, texel * c_AliasTexel).rg : VRAM(texel * "         \
  "c_VRAMTexel); }\n"
#endif

#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)

#define GPU_DITHERING                                                          \
  "	vec4 dither(vec4 color) { return applySceneAtmosphere(color); }\n"

#define GPU_ARRAY_FUNC                                                         \
  "	float _idx2(vec2 array, int idx) { return array[idx]; }\n"

#else

#define GPU_DITHERING                                                          \
  "	vec4 dither(vec4 color) { return applySceneAtmosphere(color); }\n"

#define GPU_ARRAY_FUNC                                                         \
  "	float _idx2(vec2 array, int idx) { return idx == 0 ? array.x : "           \
  "array.y; }\n"

#endif

#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)

#define GPU_MINIFICATION_FILTER                                                \
  "\tvoid accumulateTileTap(vec2 P, float weight, inout vec4 premultiplied, "  \
  "inout float coverageSum, inout float weightSum) {\n"                        \
  "\t\tfloat tapCoverage;\n"                                                   \
  "\t\tvec4 tapColor = bilinearTextureSample(boundedPSX(P), tapCoverage);\n"   \
  "\t\tpremultiplied += tapColor * tapCoverage * weight;\n"                    \
  "\t\tcoverageSum += tapCoverage * weight;\n"                                 \
  "\t\tweightSum += weight;\n"                                                 \
  "\t}\n"                                                                      \
  "\tvec4 edgeSafeBilinearTextureSample(vec2 P, out float coverage) {\n"       \
  "\t\treturn bilinearTextureSample(boundedPSX(P), coverage);\n"               \
  "\t}\n"                                                                      \
  "\tvec4 anisotropicTextureSample(vec2 P, out float coverage) {\n"            \
  "\t\tvec2 boundedP = boundedPSX(P);\n"                                       \
  "\t\tfloat centerCoverage;\n"                                                \
  "\t\tvec4 centerColor = bilinearTextureSample(boundedP, centerCoverage);\n"  \
  "\t\tvec2 axisX = dFdx(P);\n"                                                \
  "\t\tvec2 axisY = dFdy(P);\n"                                                \
  "\t\tfloat covarianceA = axisX.x * axisX.x + axisY.x * axisY.x;\n"           \
  "\t\tfloat covarianceB = axisX.x * axisX.y + axisY.x * axisY.y;\n"           \
  "\t\tfloat covarianceC = axisX.y * axisX.y + axisY.y * axisY.y;\n"           \
  "\t\tfloat discriminant = sqrt(max((covarianceA - covarianceC) * "           \
  "(covarianceA - covarianceC) + 4.0 * covarianceB * covarianceB, 0.0));\n"    \
  "\t\tfloat majorSquared = max(0.5 * (covarianceA + covarianceC + "           \
  "discriminant), 0.0);\n"                                                     \
  "\t\tfloat majorLength = sqrt(majorSquared);\n"                              \
  "\t\tfloat filterBlend = smoothstep(0.75, 1.25, majorLength);\n"             \
  "\t\tif (filterBlend <= 0.0) {\n"                                            \
  "\t\t\tcoverage = centerCoverage;\n"                                         \
  "\t\t\treturn centerColor;\n"                                                \
  "\t\t}\n"                                                                    \
  "\t\tfloat footprintScale = min(1.0, 16.0 / max(majorLength, 0.0001));\n"    \
  "\t\tvec2 footprintX = axisX * footprintScale;\n"                            \
  "\t\tvec2 footprintY = axisY * footprintScale;\n"                            \
  "\t\tvec4 areaPremultiplied = vec4(0.0);\n"                                  \
  "\t\tfloat areaCoverageSum = 0.0;\n"                                         \
  "\t\tfloat areaWeightSum = 0.0;\n"                                           \
  "\t\tfor (int tap = 0; tap < 16; ++tap) {\n"                                 \
  "\t\t\tfloat index = float(tap);\n"                                          \
  "\t\t\tfloat offsetX = (index + 0.5) * (1.0 / 16.0) - 0.5;\n"                \
  "\t\t\tfloat offsetY = mod(index, 2.0) * 0.5 + mod(floor(index * 0.5), "     \
  "2.0) * 0.25 + mod(floor(index * 0.25), 2.0) * 0.125 + mod(floor(index * "   \
  "0.125), 2.0) * 0.0625 + (1.0 / 32.0) - 0.5;\n"                              \
  "\t\t\tvec2 offset = vec2(offsetX, offsetY);\n"                              \
  "\t\t\tfloat weight = exp2(-4.0 * dot(offset, offset));\n"                   \
  "\t\t\taccumulateTileTap(P + footprintX * offset.x + footprintY * "          \
  "offset.y, weight, areaPremultiplied, areaCoverageSum, areaWeightSum);\n"    \
  "\t\t}\n"                                                                    \
  "\t\tfloat filteredCoverage = areaCoverageSum / areaWeightSum;\n"            \
  "\t\tvec4 filteredPremultiplied = areaPremultiplied / areaWeightSum;\n"      \
  "\t\tcoverage = mix(centerCoverage, filteredCoverage, filterBlend);\n"       \
  "\t\tvec4 premultiplied = mix(centerColor * centerCoverage, "                \
  "filteredPremultiplied, filterBlend);\n"                                     \
  "\t\treturn premultiplied / max(coverage, 0.0001);\n"                        \
  "\t}\n"

#else

#define GPU_MINIFICATION_FILTER                                                \
  "\tvec4 edgeSafeBilinearTextureSample(vec2 P, out float coverage) {\n"       \
  "\t\treturn bilinearTextureSample(boundedPSX(P), coverage);\n"               \
  "\t}\n"                                                                      \
  "\tvec4 anisotropicTextureSample(vec2 P, out float coverage) {\n"            \
  "\t\treturn edgeSafeBilinearTextureSample(P, coverage);\n"                   \
  "\t}\n"                                                                      \
  "\tvec4 trilinearTextureSample(vec2 P, out float coverage) {\n"              \
  "\t\treturn edgeSafeBilinearTextureSample(P, coverage);\n"                   \
  "\t}\n"

#endif

#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)

// Indexed PS1 VRAM cannot use hardware-generated mipmaps: averaging palette
// indices creates unrelated colours and a conventional atlas mip chain bleeds
// adjacent tiles. Reconstruct two decoded, tile-clamped mip levels per sample
// and blend them exactly like trilinear filtering.
#undef GPU_MINIFICATION_FILTER
#define GPU_MINIFICATION_FILTER                                                \
  R"PSYX(
	vec4 edgeSafeBilinearTextureSample(vec2 P, out float coverage) {
		return bilinearTextureSample(boundedPSX(P), coverage);
	}

	vec4 decodedPointTextureSample(vec2 P, out float coverage) {
		vec2 rg = samplePSX(floor(boundedPSX(P) + vec2(0.5)));
		coverage = float(rg.r + rg.g > 0.0);
		vec4 color = lut(rg);
		color.w = 1.0 - color.w;
		return color;
	}

	void accumulateDecodedPoint(vec2 P, float weight,
			inout vec4 premultiplied, inout float coverageSum,
			inout float weightSum) {
		float tapCoverage;
		vec4 tapColor = decodedPointTextureSample(P, tapCoverage);
		premultiplied += tapColor * tapCoverage * weight;
		coverageSum += tapCoverage * weight;
		weightSum += weight;
	}

	vec4 resolveTileFilter(vec4 premultiplied, float coverageSum,
			float weightSum, out float coverage) {
		coverage = coverageSum / max(weightSum, 0.0001);
		return (premultiplied / max(weightSum, 0.0001)) /
			max(coverage, 0.0001);
	}

	vec4 tileMipLevelTextureSample(vec2 P, float lod, out float coverage) {
		if (lod < 0.001) {
			return edgeSafeBilinearTextureSample(P, coverage);
		}
		float spread = exp2(lod) * 0.25;
		vec4 premultiplied = vec4(0.0);
		float coverageSum = 0.0;
		float weightSum = 0.0;
		// Keep the authored centre texel dominant. Sparse diagonal-only taps can
		// otherwise turn a solid GMD texel into transparent/foreign atlas colour.
		accumulateDecodedPoint(P, 4.0, premultiplied, coverageSum, weightSum);
		accumulateDecodedPoint(P + vec2(-spread, -spread), 1.0,
			premultiplied, coverageSum, weightSum);
		accumulateDecodedPoint(P + vec2( spread, -spread), 1.0,
			premultiplied, coverageSum, weightSum);
		accumulateDecodedPoint(P + vec2(-spread,  spread), 1.0,
			premultiplied, coverageSum, weightSum);
		accumulateDecodedPoint(P + vec2( spread,  spread), 1.0,
			premultiplied, coverageSum, weightSum);
		return resolveTileFilter(premultiplied, coverageSum, weightSum,
			coverage);
	}

	vec4 trilinearTextureSampleAtLod(vec2 P, float lod,
			out float coverage) {
		float lowLod = floor(lod);
		float highLod = min(lowLod + 1.0, 4.0);
		float lowCoverage;
		float highCoverage;
		vec4 lowColor = tileMipLevelTextureSample(P, lowLod, lowCoverage);
		vec4 highColor = tileMipLevelTextureSample(P, highLod, highCoverage);
		float blend = fract(lod);
		coverage = mix(lowCoverage, highCoverage, blend);
		vec4 premultiplied = mix(lowColor * lowCoverage,
			highColor * highCoverage, blend);
		return premultiplied / max(coverage, 0.0001);
	}

	vec4 trilinearTextureSample(vec2 P, out float coverage) {
		vec2 axisX = dFdx(P);
		vec2 axisY = dFdy(P);
		float footprint = max(length(axisX), length(axisY));
		float lod = clamp(log2(max(footprint, 1.0)), 0.0, 4.0);
		return trilinearTextureSampleAtLod(P, lod, coverage);
	}

	vec4 anisotropicTextureSample(vec2 P, out float coverage) {
		vec2 axisX = dFdx(P);
		vec2 axisY = dFdy(P);
		float covarianceA = axisX.x * axisX.x + axisY.x * axisY.x;
		float covarianceB = axisX.x * axisX.y + axisY.x * axisY.y;
		float covarianceC = axisX.y * axisX.y + axisY.y * axisY.y;
		float discriminant = sqrt(max((covarianceA - covarianceC) *
			(covarianceA - covarianceC) + 4.0 * covarianceB * covarianceB,
			0.0));
		float majorSquared = max(0.5 * (covarianceA + covarianceC +
			discriminant), 0.0);
		float minorSquared = max(0.5 * (covarianceA + covarianceC -
			discriminant), 0.0);
		float majorLength = sqrt(majorSquared);
		float minorLength = sqrt(minorSquared);
		if (majorLength <= 1.0) {
			return edgeSafeBilinearTextureSample(P, coverage);
		}
		float baseLength = max(minorLength, 1.0);
		float anisotropy = majorLength / baseLength;
		float lod = clamp(log2(baseLength), 0.0, 4.0);
		float centerCoverage;
		vec4 centerColor = trilinearTextureSampleAtLod(
			P, lod, centerCoverage);
		float filterBlend = smoothstep(1.0, 1.5, anisotropy);
		if (filterBlend <= 0.0) {
			coverage = centerCoverage;
			return centerColor;
		}
		vec2 majorDirection = covarianceA >= covarianceC
			? vec2(1.0, 0.0) : vec2(0.0, 1.0);
		if (abs(covarianceB) > 0.00001) {
			majorDirection = normalize(
				vec2(majorSquared - covarianceC, covarianceB));
		}
		vec2 majorAxis = majorDirection *
			min(majorLength, baseLength * 16.0);
		vec4 areaPremultiplied = vec4(0.0);
		float areaCoverageSum = 0.0;
		float areaWeightSum = 0.0;
		for (int tap = 0; tap < 16; ++tap) {
			float index = float(tap);
			float position = (index + 0.5) * (1.0 / 16.0) - 0.5;
			float weight = exp2(-4.0 * position * position);
			float tapCoverage;
			vec4 tapColor = trilinearTextureSampleAtLod(
				P + majorAxis * position, lod, tapCoverage);
			areaPremultiplied += tapColor * tapCoverage * weight;
			areaCoverageSum += tapCoverage * weight;
			areaWeightSum += weight;
		}
		float filteredCoverage;
		vec4 filteredColor = resolveTileFilter(
			areaPremultiplied, areaCoverageSum, areaWeightSum,
			filteredCoverage);
		coverage = mix(centerCoverage, filteredCoverage, filterBlend);
		vec4 premultiplied = mix(centerColor * centerCoverage,
			filteredColor * filteredCoverage, filterBlend);
		return premultiplied / max(coverage, 0.0001);
	}
)PSYX"

#endif

#define GPU_FRAGMENT_SAMPLE_SHADER(bit)                                        \
  GPU_FETCH_VRAM_FUNC                                                          \
  GPU_ARRAY_FUNC                                                               \
  GPU_SAMPLE_TEXTURE_##bit##BIT_FUNC                                           \
      "	uniform sampler2D s_rgLut;\n"                                          \
      "	const vec2 c_LUTTexel = vec2(1.0 / 256.0, 1.0 / 256.0);\n"             \
      "	vec4 lut(vec2 rg) { return texture2D(s_rgLut, rg - c_LUTTexel * "      \
      "0.0001); }\n"                                                           \
      "	vec2 boundedPSX(vec2 P) { return clamp(P, v_texbounds.xy, "            \
      "v_texbounds.zw); }\n"                                                   \
      "	vec4 bilinearTextureSample(vec2 P, out float coverage) {\n"            \
      "		// Clamp the coordinate and every footprint tap to this "             \
      "primitive's\n"                                                          \
      "		// inclusive atlas tile. This is clamp-to-edge in texel "             \
      "space, so\n"                                                            \
      "		// filtering cannot pull colours from a neighbouring atlas "          \
      "tile.\n"                                                                \
      "		vec2 safeP = boundedPSX(P);\n"                                        \
      "		vec2 pixel = floor(safeP);\n"                                         \
      "		vec2 frac = safeP - pixel;\n"                                         \
      "		vec2 C11 = samplePSX(boundedPSX(pixel));\n"                           \
      "		vec2 C21 = samplePSX(boundedPSX(pixel + vec2(1.0, 0.0)));\n"          \
      "		vec2 C12 = samplePSX(boundedPSX(pixel + vec2(0.0, 1.0)));\n"          \
      "		vec2 C22 = samplePSX(boundedPSX(pixel + vec2(1.0, 1.0)));\n"          \
      "		vec4 weights = vec4((1.0 - frac.x) * (1.0 - frac.y), frac.x "         \
      "* (1.0 - frac.y), (1.0 - frac.x) * frac.y, frac.x * frac.y);\n"         \
      "		vec4 solid = vec4(float(C11.r + C11.g > 0.0), float(C21.r + "         \
      "C21.g > 0.0), float(C12.r + C12.g > 0.0), float(C22.r + C22.g > "       \
      "0.0));\n"                                                               \
      "		coverage = dot(weights, solid);\n"                                    \
      "		vec4 t = (lut(C11) * weights.x * solid.x + lut(C21) * "               \
      "weights.y * solid.y + lut(C12) * weights.z * solid.z + lut(C22) * "     \
      "weights.w * solid.w) / max(coverage, 0.0001);\n"                        \
      "		t.w = 1.0 - t.w;\n"                                                   \
      "		return t;\n"                                                          \
      "	}\n"                                                                   \
      "	vec4 pointTextureSample(vec2 P, out float coverage) {\n"               \
      "		vec2 rg = samplePSX(floor(boundedPSX(P) + vec2(0.5)));\n"             \
      "		coverage = float(rg.r + rg.g > 0.0);\n"                               \
      "		vec4 t = lut(rg);\n"                                                  \
      "		t.w = 1.0 - t.w;\n"                                                   \
      "		return t;\n"                                                          \
      "	}\n" GPU_MINIFICATION_FILTER "	vec4 nearestTextureSample(vec2 P) {\n"  \
      "		vec2 rg = samplePSX(boundedPSX(P));\n"                                \
      "		float rgm = rg.x + rg.y;\n"                                           \
      "		if (rgm == 0.0) { discard; }\n"                                       \
      "		vec4 t = lut(rg);\n"                                                  \
      "		t.w = 1.0 - t.w;\n"                                                   \
      "		return t;\n"                                                          \
      "	}\n"                                                                   \
      "	uniform int textureFilterMode;\n"                                      \
      "	uniform int textureBlendMode;\n"                                       \
      "	void main() {\n"                                                       \
      "		float coverage = 1.0;\n"                                              \
      "		vec4 color;\n"                                                        \
      "		if (textureFilterMode > 0) {\n"                                       \
      "			color = textureFilterMode > 2\n"                                     \
      "				? anisotropicTextureSample(v_texcoord.xy, "                         \
      "coverage)\n"                                                            \
      "				: textureFilterMode > 1\n"                                          \
      "					? "                                                                \
      "trilinearTextureSample(v_texcoord.xy, coverage)\n"                      \
      "					: "                                                                \
      "edgeSafeBilinearTextureSample(v_texcoord.xy, coverage);\n"              \
      "			float sourceCoverage;\n"                                             \
      "			vec4 sourceColor = pointTextureSample(v_texcoord.xy, "               \
      "sourceCoverage);\n"                                                     \
      "			if (textureBlendMode == 0) {\n"                                      \
      "				if (sourceCoverage < 0.5) { discard; }\n"                           \
      "				float mipConfidence = smoothstep(0.50, 0.75, "                      \
      "coverage);\n"                                                           \
      "				color = mix(sourceColor, color, "                                   \
      "mipConfidence);\n"                                                      \
      "				coverage = 1.0;\n"                                                  \
      "			} else {\n"                                                          \
      "				coverage = smoothstep(0.015, 0.98, "                                \
      "coverage);\n"                                                           \
      "				if (coverage <= 0.0) { discard; }\n"                                \
      "			}\n"                                                                 \
      "		} else {\n"                                                           \
      "			color = nearestTextureSample(v_texcoord.xy);\n"                      \
      "		}\n"                                                                  \
      "		vec4 shaded = dither(color * v_color);\n"                             \
      "		if (textureFilterMode > 0 && textureBlendMode > 0) {\n"               \
      "			shaded.a *= coverage;\n"                                             \
      "			if (textureBlendMode != 1) { shaded.rgb *= coverage; "               \
      "}\n"                                                                    \
      "		}\n"                                                                  \
      "		fragColor = shaded;\n"                                                \
      "	}\n"

static const char *gpu_shader_common = R"(
	varying vec4 v_texcoord;
	NOPERSPECTIVE varying vec4 v_color;
	FLAT varying vec4 v_page_clut;
	FLAT varying float v_alias_page;
	FLAT varying vec4 v_texbounds;
	varying float v_z;
	uniform vec4 u_scene_fog_color_enabled;
	uniform vec4 u_scene_fog_gte;
	uniform vec2 u_scene_fog_terrain;

	vec4 applySceneAtmosphere(vec4 source) {
		if (u_scene_fog_color_enabled.w < 0.5 || v_z <= 0.0)
			return source;
		float cameraDepth = v_z * 128.0 / 1.35;
		float quotient = clamp(
			u_scene_fog_gte.z / max(cameraDepth, 0.001) * 65536.0,
			0.0, 131071.0);
		float gteFog = clamp(
			(u_scene_fog_gte.y + u_scene_fog_gte.x * quotient) /
				16777216.0,
			0.0, 1.0);
		float terrainDistance =
			floor(floor(cameraDepth + 0.5) * 0.75) -
			u_scene_fog_terrain.x;
		float terrainFog = clamp(
			terrainDistance * u_scene_fog_terrain.y, 0.0, 1.0);
		float retailFog = mix(terrainFog, gteFog, u_scene_fog_gte.w);
		float onset = smoothstep(0.04, 0.45, retailFog);
		float air = 1.0 - exp(-sqrt(retailFog) * 0.52);
		float volume = clamp(onset * air * (1.0 - retailFog), 0.0, 0.16);
		source.rgb = mix(source.rgb, u_scene_fog_color_enabled.rgb, volume);
		return source;
	}
)";

const char *gpu_shader_4 = GPU_FRAGMENT_SAMPLE_SHADER(4);
const char *gpu_shader_8 = GPU_FRAGMENT_SAMPLE_SHADER(8);
const char *gpu_shader_16 = GPU_FRAGMENT_SAMPLE_SHADER(16);
const char *gpu_shader_32_rgba =
    "	uniform sampler2D s_texture;\n"
    "	uniform vec2 texelSize;\n"
    "	void main() {\n"
    "		vec2 tc = v_texcoord.xy * texelSize + texelSize * 0.5;\n"
    "		vec4 color = texture2D(s_texture, tc);\n"
    "		if (color.a <= 0.0) discard;\n"
    "		fragColor = dither(color * v_color);\n"
    "	}\n";

#if USE_PGXP
#define GTE_PERSPECTIVE_CORRECTION                                             \
  "	if (a_zw.y > 0.0) {\n"                                                     \
  "		vec4 depthPosition = Projection3D * vec4(0.0, 0.0, a_zw.x, "              \
  "1.0);\n"                                                                    \
  "		vec2 clipPosition = a_zw.zw * vec2(2.0, -2.0);\n"                         \
  "		clipPosition += a_position.xy * a_zw.x * vec2(2.0, -2.0);\n"              \
  "		gl_Position = vec4(clipPosition, depthPosition.z, "                       \
  "depthPosition.w);\n"                                                        \
  "	} else {\n"                                                                \
  "		gl_Position = Projection * vec4(a_position.xy, 0.5, 1.0);\n"              \
  "	}\n"
#else
#define GTE_PERSPECTIVE_CORRECTION                                             \
  "	gl_Position = Projection * vec4(a_position.xy, 0.0, 1.0);\n"
#endif

#define GTE_VERTEX_SHADER                                                      \
  "	attribute vec2 a_position;\n"                                              \
  "	attribute vec2 a_page_clut; // unsigned host TPAGE and CLUT\n"             \
  "	attribute vec4 a_texcoord; // uv, color multiplier, dither\n"              \
  "	attribute vec2 a_precise_uv;\n"                                            \
  "	attribute vec4 a_color;\n"                                                 \
  "	attribute vec4 a_extra; // texcoord.xy ofs, presentation flag, "           \
  "precise-UV flag\n"                                                          \
  "	attribute vec4 a_texbounds; // inclusive primitive UV bounds\n"            \
  "	attribute vec4 a_zw;\n"                                                    \
  "	uniform mat4 Projection;\n"                                                \
  "	uniform mat4 Projection3D;\n"                                              \
  "	uniform vec2 PresentationScale;\n"                                         \
  "	const vec2 c_UVFudge = vec2(0.00025, 0.00025);\n"                          \
  "	void main() {\n"                                                           \
  "		v_texcoord = a_texcoord;\n"                                               \
  "		v_texcoord.xy = a_precise_uv;\n"                                          \
  "		v_texcoord.xy += a_extra.xy * 0.5;\n"                                     \
  "		v_texbounds = a_texbounds;\n"                                             \
  "		v_color = a_color;\n"                                                     \
  "		v_color.xyz *= a_texcoord.z;\n"                                           \
  "		v_alias_page = floor(a_page_clut.x / 1024.0);\n"                          \
  "		float nativePage = mod(a_page_clut.x, 32.0);\n"                           \
  "		float aliasIndex = max(v_alias_page - 1.0, 0.0);\n"                       \
  "		v_page_clut.x = mix(mod(nativePage, 16.0) * 64.0, "                       \
  "mod(aliasIndex, 16.0) * 64.0, step(0.5, v_alias_page));\n"                  \
  "		v_page_clut.y = mix(floor(nativePage / 16.0) * 256.0, "                   \
  "floor(aliasIndex / 16.0) * 256.0, step(0.5, v_alias_page));\n"              \
  "		v_page_clut.z = fract(a_page_clut.y / 64.0);\n"                           \
  "		v_page_clut.w = floor(a_page_clut.y / 64.0) / 512.0;\n"                   \
  "		v_page_clut.xy += c_UVFudge;\n"                                           \
  "		v_page_clut.zw += c_UVFudge;\n" GTE_PERSPECTIVE_CORRECTION     \
  "		gl_Position.xy *= mix(PresentationScale, vec2(1.0), "                     \
  "clamp(a_extra.z, 0.0, 1.0));\n"                                             \
  "		v_z = a_zw.y > 0.0 ? a_zw.x : -1.0;\n"                                    \
  "	}\n"

int GR_Shader_CheckShaderStatus(GLuint shader) {
  char info[1024];
  GLint result;

  glGetShaderiv(shader, GL_COMPILE_STATUS, &result);

  if (result == GL_TRUE)
    return 1;

  glGetShaderInfoLog(shader, sizeof(info), NULL, info);
  if (info[0] && strlen(info) > 8) {
    eprinterr("%s\n", info);
    assert(0);
  }

  return 0;
}

int GR_Shader_CheckProgramStatus(GLuint program) {
  char info[1024];
  GLint result;

  glGetProgramiv(program, GL_LINK_STATUS, &result);

  if (result == GL_TRUE)
    return 1;

  glGetProgramInfoLog(program, sizeof(info), NULL, info);
  if (info[0] && strlen(info) > 8) {
    eprinterr("%s\n", info);
    assert(0);
  }

  return 0;
}

ShaderID GR_Shader_Compile(const char *source, int isPsxShader) {
#if defined(ES2_SHADERS)
  const char *GLSL_HEADER_VERT = R"(
		#version 100
		precision lowp  int;
		precision highp float;
		#define FLAT
		#define NOPERSPECTIVE
	)";

  const char *GLSL_HEADER_FRAG = R"(
		#version 100
		precision lowp  int;
		precision highp float;
		#define FLAT
		#define NOPERSPECTIVE
		#define fragColor gl_FragColor
	)";
#elif defined(ES3_SHADERS)
  const char *GLSL_HEADER_VERT = R"(
		#version 300 es
		precision lowp  int;
		precision highp float;
		#define FLAT flat
		#define NOPERSPECTIVE
		#define varying   out
		#define attribute in
		#define texture2D texture
	)";

  const char *GLSL_HEADER_FRAG = R"(
		#version 300 es
		precision lowp  int;
		precision highp float;
		#define FLAT flat
		#define NOPERSPECTIVE
		#define varying     in
		#define texture2D   texture
		out vec4 fragColor;
	)";
#else
  const char *GLSL_HEADER_VERT = R"(
		#version 140
		precision lowp  int;
		precision highp float;
		#define FLAT flat
		#define NOPERSPECTIVE noperspective
		#define varying   out
		#define attribute in
		#define texture2D texture
	)";

  const char *GLSL_HEADER_FRAG = R"(
		#version 140
		precision lowp  int;
		precision highp float;
		#define FLAT flat
		#define NOPERSPECTIVE noperspective
		#define varying     in
		#define texture2D   texture
		out vec4 fragColor;
	)";
#endif

  char extra_vs_defines[1024];
  char extra_fs_defines[1024];
  extra_vs_defines[0] = 0;
  extra_fs_defines[0] = 0;

  strcat(extra_vs_defines, "#define VERTEX\n");
  strcat(extra_fs_defines, "#define FRAGMENT\n");
  if (g_cfg_bilinearFiltering || g_cfg_trilinearFiltering ||
      g_cfg_anisotropicFiltering) {
    strcat(extra_fs_defines, "#define BILINEAR_FILTER\n");
  }
  if (g_cfg_trilinearFiltering) {
    strcat(extra_fs_defines, "#define TRILINEAR_FILTER\n");
  }

  const char *vs_list_psx[] = {GLSL_HEADER_VERT, extra_vs_defines,
                               gpu_shader_common, GTE_VERTEX_SHADER};
  const char *fs_list_psx[] = {GLSL_HEADER_FRAG, extra_fs_defines,
                               gpu_shader_common, GPU_DITHERING, source};

  const char *vs_list_src[] = {
      GLSL_HEADER_VERT,
      extra_vs_defines,
      source,
  };
  const char *fs_list_src[] = {GLSL_HEADER_FRAG, extra_fs_defines, source};

  const char **vs_list = isPsxShader ? vs_list_psx : vs_list_src;
  const char **fs_list = isPsxShader ? fs_list_psx : fs_list_src;
  const int vs_list_cnt = isPsxShader ? 4 : 3;
  const int fs_list_cnt = isPsxShader ? 5 : 3;

  GLuint program = glCreateProgram();

  {
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, vs_list_cnt, vs_list, NULL);
    glCompileShader(vertexShader);

    if (GR_Shader_CheckShaderStatus(vertexShader) == 0)
      eprinterr("Failed to compile Vertex Shader!\n");

    glAttachShader(program, vertexShader);
    glDeleteShader(vertexShader);
  }

  {
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, fs_list_cnt, fs_list, NULL);
    glCompileShader(fragmentShader);

    if (GR_Shader_CheckShaderStatus(fragmentShader) == 0)
      eprinterr("Failed to compile Fragment Shader!\n");

    glAttachShader(program, fragmentShader);
    glDeleteShader(fragmentShader);
  }

  glBindAttribLocation(program, a_position, "a_position");
  glBindAttribLocation(program, a_page_clut, "a_page_clut");
  glBindAttribLocation(program, a_texcoord, "a_texcoord");
  glBindAttribLocation(program, a_color, "a_color");
  glBindAttribLocation(program, a_extra, "a_extra");
  glBindAttribLocation(program, a_texbounds, "a_texbounds");
  glBindAttribLocation(program, a_precise_uv, "a_precise_uv");

#if USE_PGXP
  glBindAttribLocation(program, a_zw, "a_zw");
#endif

  glLinkProgram(program);
  if (GR_Shader_CheckProgramStatus(program) == 0)
    eprinterr("Failed to link Shader!\n");

  GLint sampler = 0;
  glUseProgram(program);
  glUniform1iv(glGetUniformLocation(program, "s_rgLut"), 1, &sampler);
  glUniform1iv(glGetUniformLocation(program, "s_texture"), 1, &sampler);
  glUseProgram(0);

  return program;
}
#else
#error
#endif

#include "PsyX_atmosphere.inc"
#include "PsyX_fxaa.inc"
#include "PsyX_guest_vram.inc"
#include "PsyX_object_shadows.inc"
#include "PsyX_smaa.inc"
#include "PsyX_volumetrics.inc"

//--------------------------------------------------------------------------------------------

void GR_GenerateCommonTextures() {
  unsigned int whitePixelData = 0xFFFFFFFF;

#if USE_OPENGL
  glGenTextures(1, &g_whiteTexture);
  {
    glBindTexture(GL_TEXTURE_2D, g_whiteTexture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 &whitePixelData);

    glBindTexture(GL_TEXTURE_2D, 0);
  }

  glGenTextures(1, &g_rgLutTexture);
  {
    glBindTexture(GL_TEXTURE_2D, g_rgLutTexture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LUT_WIDTH, LUT_HEIGHT, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, &rgLUT);

    glBindTexture(GL_TEXTURE_2D, 0);
  }
#endif
}

TextureID GR_CreateRGBATexture(int width, int height,
                               u_char *data /*= nullptr*/) {
  TextureID newTexture;
  glGenTextures(1, &newTexture);

  glBindTexture(GL_TEXTURE_2D, newTexture);
  const int filtered = g_cfg_bilinearFiltering || g_cfg_trilinearFiltering ||
                       g_cfg_anisotropicFiltering;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                  filtered ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  filtered ? GL_LINEAR : GL_NEAREST);

  // another WebGL stuff. Texture will be black without clamp to edge
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (g_maxTextureAnisotropy > 1.0f)
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                    g_cfg_anisotropicFiltering ? g_maxTextureAnisotropy : 1.0f);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, data);
  glBindTexture(GL_TEXTURE_2D, 0);

  return newTexture;
}

void GR_UpdateRGBATexture(TextureID texture, int width, int height,
                          const u_char *data) {
  if (texture == 0 || width <= 0 || height <= 0 || data == nullptr)
    return;

#if USE_OPENGL
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
                  GL_UNSIGNED_BYTE, data);
  glBindTexture(GL_TEXTURE_2D, 0);
  g_lastBoundTexture = 0;
#else
#error
#endif
}

void GR_CompilePSXShader(PSXGPU_Shader *sh, const char *source) {
  sh->shader = GR_Shader_Compile(source, true);

#if USE_OPENGL
  sh->textureFilterModeLoc =
      glGetUniformLocation(sh->shader, "textureFilterMode");
  sh->textureBlendModeLoc =
      glGetUniformLocation(sh->shader, "textureBlendMode");
  sh->projectionLoc = glGetUniformLocation(sh->shader, "Projection");
  sh->presentationScaleLoc =
      glGetUniformLocation(sh->shader, "PresentationScale");
  sh->texelSizeLoc = glGetUniformLocation(sh->shader, "texelSize");
  sh->texLoc = glGetUniformLocation(sh->shader, "s_texture");
  sh->lutLoc = glGetUniformLocation(sh->shader, "s_rgLut");
  sh->aliasLoc = glGetUniformLocation(sh->shader, "s_aliasTexture");
  sh->sceneFogColorEnabledLoc =
      glGetUniformLocation(sh->shader, "u_scene_fog_color_enabled");
  sh->sceneFogGteLoc = glGetUniformLocation(sh->shader, "u_scene_fog_gte");
  sh->sceneFogTerrainLoc =
      glGetUniformLocation(sh->shader, "u_scene_fog_terrain");
  sh->appliedTextureFilterMode = -1;
  sh->appliedSceneFogRevision = 0U;
#if USE_PGXP
  sh->projection3DLoc = glGetUniformLocation(sh->shader, "Projection3D");
#endif
  glUseProgram(sh->shader);
  if (sh->texLoc != -1)
    glUniform1i(sh->texLoc, 0);
  if (sh->lutLoc != -1)
    glUniform1i(sh->lutLoc, 1);
  if (sh->aliasLoc != -1)
    glUniform1i(sh->aliasLoc, 2);
#endif
}

void GR_InitialisePSXShaders() {
  GR_CompilePSXShader(&g_gpu_shader_4, gpu_shader_4);
  GR_CompilePSXShader(&g_gpu_shader_8, gpu_shader_8);
  GR_CompilePSXShader(&g_gpu_shader_16, gpu_shader_16);
  GR_CompilePSXShader(&g_gpu_shader_32_rgba, gpu_shader_32_rgba);
#if USE_OPENGL
  glUseProgram(0);
  g_PreviousShader = -1;
#endif
}

u_char GR_Expand5BitColor(u_char value) {
  value &= 31;
  return (u_char)((value << 3) | (value >> 2));
}

void GR_InitRG8LUT() {
  for (u_short y = 0; y < LUT_HEIGHT; y++) {
    u_char *row = rgLUT + y * (LUT_HEIGHT * 4);
    for (u_short x = 0; x < LUT_WIDTH; x++) {
      const u_short c = (y << 8) | x;
      u_char *pixel = row + x * 4;
      pixel[0] = GR_Expand5BitColor((u_char)(c & 31));
      pixel[1] = GR_Expand5BitColor((u_char)((c >> 5) & 31));
      pixel[2] = GR_Expand5BitColor((u_char)((c >> 10) & 31));
      pixel[3] = (u_char)((c >> 15) & 1) << 7;
    }
  }
}

int GR_InitialisePSX() {
  g_PreviousOffscreenState = 0;
  g_PreviousOffscreen = {0, 0, 0, 0};
  g_PreviousDepthRangeLower = -1.0f;
  g_PreviousDepthRangeUpper = -1.0f;
  g_nativeDepthClearSerial = 0U;
  g_offscreenDepthClearSerial = 0U;
#if USE_OPENGL
  g_presentationScale = {1.0F, 1.0F};
#endif
  GR_ResetWorldDepthEpochs();
  g_highResolutionVRAMGeneration = 0;
  for (auto &page : g_highResolutionVRAMPages)
    page = {};
#if defined(RENDERER_OGL)
  GR_ResetGPUVRAMDirty();
  g_guestDisplayWidth = 256;
  g_guestDisplayHeight = 240;
  g_pendingGuestDisplayWidth = 256;
  g_pendingGuestDisplayHeight = 240;
  g_guestRenderConfigWidth = 0;
  g_guestRenderConfigHeight = 0;
  g_guestRenderConfigAspect = -1;
  g_guestRenderConfigComposed = -1;
  g_guestRenderLogicalFallback = false;
  g_guestTextureLimitLogged = false;
  g_nextOffscreenIsRoot = true;
#endif
  SDL_memset(vram, 0, VRAM_WIDTH * VRAM_HEIGHT * sizeof(unsigned short));
  SDL_memset(g_vramAliasPages, 0,
             VRAM_ALIAS_WIDTH * VRAM_ALIAS_HEIGHT * sizeof(unsigned short));
  GR_ResetVRAMDirtyRects();
  GR_InitRG8LUT();
  GR_GenerateCommonTextures();
  GR_InitialisePSXShaders();

#if USE_OPENGL
  glDepthFunc(GL_GEQUAL);
  GR_SetDepthRange(0.0f, 1.0f);
  g_PreviousDepthFunc = GL_GEQUAL;
  glEnable(GL_STENCIL_TEST);
  glBlendColor(0.5f, 0.5f, 0.5f, 0.25f);

  // All display primitives are rasterized at the active PSX DISPENV size.
  // The default framebuffer is used only for the final 4:3 presentation blit.
  glGenTextures(1, &g_glNativeColorTexture);
  glBindTexture(GL_TEXTURE_2D, g_glNativeColorTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &g_glNativeFramebuffer);
#if defined(RENDERER_OGL)
  glGenTextures(1, &g_glNativeDepthTexture);
#else
  glGenRenderbuffers(1, &g_glNativeDepthRenderbuffer);
  glGenRenderbuffers(1, &g_glNativeStencilRenderbuffer);
#endif
  glGenFramebuffers(1, &g_glNativeMultisampleFramebuffer);
  glGenRenderbuffers(1, &g_glNativeMultisampleColorRenderbuffer);
  glGenRenderbuffers(1, &g_glNativeMultisampleDepthRenderbuffer);
  PsyX_InitialiseAtmosphere();
  PsyX_InitialiseSMAA();
  PsyX_InitialiseFXAA();
  PsyX_InitialiseVolumetrics();
  PsyX_InitialiseObjectShadows();

  // gen framebuffer
  {
    memset(&g_glFramebufferPBO, 0, sizeof(g_glFramebufferPBO));
    PBO_Init(&g_glFramebufferPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);

    // make a special texture
    // it will be resized later
    glGenTextures(1, &g_fbTexture);
    {
      glBindTexture(GL_TEXTURE_2D, g_fbTexture);

      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

      // default to VRAM size
      glTexImage2D(GL_TEXTURE_2D, 0, g_rgbaRenderTargetInternalFormat,
                   VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

      glBindTexture(GL_TEXTURE_2D, 0);
    }

    glGenFramebuffers(1, &g_glBlitFramebuffer);
    {
      glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_fbTexture, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                             0, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                             GL_TEXTURE_2D, 0, 0);

      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }

  // gen offscreen RT
  {
    memset(&g_glOffscreenPBO, 0, sizeof(g_glOffscreenPBO));
    PBO_Init(&g_glOffscreenPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);

    // offscreen texture render target
    glGenTextures(1, &g_offscreenRTTexture);
    {
      glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);

      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

      // default to VRAM size
      glTexImage2D(GL_TEXTURE_2D, 0, g_rgbaRenderTargetInternalFormat,
                   VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      g_offscreenTextureWidth = VRAM_WIDTH;
      g_offscreenTextureHeight = VRAM_HEIGHT;
      g_offscreenTextureCapacityWidth = VRAM_WIDTH;
      g_offscreenTextureCapacityHeight = VRAM_HEIGHT;

      glBindTexture(GL_TEXTURE_2D, 0);
    }

#if defined(RENDERER_OGLES) && OGLES_VERSION == 2
    glGenRenderbuffers(1, &g_glOffscreenDepthRenderbuffer);
    glGenRenderbuffers(1, &g_glOffscreenStencilRenderbuffer);
#else
    glGenRenderbuffers(1, &g_glOffscreenDepthStencilRenderbuffer);
#endif
    glGenFramebuffers(1, &g_glOffscreenFramebuffer);
    {
      glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);

      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_offscreenRTTexture, 0);
      PsyX_AllocateOffscreenDepthStencil(VRAM_WIDTH, VRAM_HEIGHT);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        eprinterr("Failed to create offscreen PSX framebuffer (%dx%d)\n",
                  VRAM_WIDTH, VRAM_HEIGHT);
        return 0;
      }

      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }

  glGenFramebuffers(1, &g_glHighResolutionVRAMFramebuffer);
#if defined(RENDERER_OGL)
  glGenTextures(1, &g_guestColorTexture);
  glBindTexture(GL_TEXTURE_2D, g_guestColorTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &g_glGuestColorFramebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, g_glGuestColorFramebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g_guestColorTexture, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    eprintwarn("RGBA8 guest-color sidecar is unavailable; using packed VRAM\n");
    glDeleteFramebuffers(1, &g_glGuestColorFramebuffer);
    glDeleteTextures(1, &g_guestColorTexture);
    g_glGuestColorFramebuffer = 0U;
    g_guestColorTexture = 0U;
  }
  glGenFramebuffers(1, &g_glGuestScanoutFramebuffer);
#endif

  glGenTextures(1, &g_guestFeedbackTexture);
  glBindTexture(GL_TEXTURE_2D, g_guestFeedbackTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, g_rgbaRenderTargetInternalFormat, VRAM_WIDTH,
               VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  g_guestFeedbackWidth = VRAM_WIDTH;
  g_guestFeedbackHeight = VRAM_HEIGHT;
  glGenFramebuffers(1, &g_glGuestFeedbackFramebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, g_glGuestFeedbackFramebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g_guestFeedbackTexture, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    eprinterr("Failed to create 1x guest feedback framebuffer\n");
    return 0;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  // gen VRAM textures.
  // double-buffered
  {
    int i;

    glGenTextures(2, g_vramTexturesDouble);

    for (i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[i]);

      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

      // set storage size
      glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_WIDTH,
                   VRAM_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, NULL);
    }

    g_vramTexture = g_vramTexturesDouble[0];

    glBindTexture(GL_TEXTURE_2D, 0);

    // VRAM framebuffer for offscreen blitting to VRAM
    glGenFramebuffers(1, &g_glVRAMFramebuffer);
    {
      glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_vramTexture, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                             0, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                             GL_TEXTURE_2D, 0, 0);

      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }

  // Host-only page aliases are never attached as PS1 VRAM or a framebuffer.
  // Keeping them separate preserves framebuffer and MoveImage semantics.
  {
    glGenTextures(1, &g_vramAliasTexture);
    glBindTexture(GL_TEXTURE_2D, g_vramAliasTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_ALIAS_WIDTH,
                 VRAM_ALIAS_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE,
                 g_vramAliasPages);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  // gen vertex buffer and index buffer
  {
    int i;

    glGenBuffers(MAX_NUM_VERTEX_BUFFERS, g_glVertexBuffer);
    glGenVertexArrays(MAX_NUM_VERTEX_BUFFERS, g_glVertexArray);

    for (i = 0; i < MAX_NUM_VERTEX_BUFFERS; i++) {
      glBindVertexArray(g_glVertexArray[i]);

      glBindBuffer(GL_ARRAY_BUFFER, g_glVertexBuffer[i]);
      glBufferData(GL_ARRAY_BUFFER, sizeof(GrVertex) * MAX_VERTEX_BUFFER_SIZE,
                   NULL, GL_DYNAMIC_DRAW);

      // Attribute format and source-buffer association are VAO state. Record
      // the immutable stream layout once instead of repeating it for every
      // non-empty ordering-table upload.
      glEnableVertexAttribArray(a_position);
      glEnableVertexAttribArray(a_page_clut);
      glEnableVertexAttribArray(a_texcoord);
      glEnableVertexAttribArray(a_color);
      glEnableVertexAttribArray(a_extra);
      glEnableVertexAttribArray(a_texbounds);
      glEnableVertexAttribArray(a_precise_uv);

#if USE_PGXP
      glVertexAttribPointer(a_position, 2, GL_FLOAT, GL_FALSE, sizeof(GrVertex),
                            &((GrVertex *)NULL)->x);
      glVertexAttribPointer(a_page_clut, 2, GL_FLOAT, GL_FALSE,
                            sizeof(GrVertex), &((GrVertex *)NULL)->page);
      glVertexAttribPointer(a_zw, 4, GL_FLOAT, GL_FALSE, sizeof(GrVertex),
                            &((GrVertex *)NULL)->z);
      glEnableVertexAttribArray(a_zw);
#else
      glVertexAttribPointer(a_position, 2, GL_SHORT, GL_FALSE, sizeof(GrVertex),
                            &((GrVertex *)NULL)->x);
      glVertexAttribPointer(a_page_clut, 2, GL_UNSIGNED_SHORT, GL_FALSE,
                            sizeof(GrVertex), &((GrVertex *)NULL)->page);
#endif
      glVertexAttribPointer(a_texcoord, 4, GL_UNSIGNED_BYTE, GL_FALSE,
                            sizeof(GrVertex), &((GrVertex *)NULL)->u);
      glVertexAttribPointer(a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                            sizeof(GrVertex), &((GrVertex *)NULL)->r);
      glVertexAttribPointer(a_extra, 4, GL_BYTE, GL_FALSE, sizeof(GrVertex),
                            &((GrVertex *)NULL)->tcx);
      glVertexAttribPointer(a_texbounds, 4, GL_UNSIGNED_BYTE, GL_FALSE,
                            sizeof(GrVertex), &((GrVertex *)NULL)->umin);
      glVertexAttribPointer(a_precise_uv, 2, GL_FLOAT, GL_FALSE,
                            sizeof(GrVertex), &((GrVertex *)NULL)->precise_u);
    }

    glBindVertexArray(0);
  }
#else
#error
#endif

#if defined(RENDERER_OGL)
  if (!GR_InitialiseGuestVRAMConversion()) {
    eprintwarn("GPU guest-VRAM conversion is unavailable; using the exact "
               "synchronous logical fallback\n");
  }
#endif
  GR_ResetDevice();

  return 1;
}

void GR_Ortho2D(float left, float right, float bottom, float top, float znear,
                float zfar) {
  float a = 2.0f / (right - left);
  float b = 2.0f / (top - bottom);
  float c = 2.0f / (znear - zfar);

  float x = (left + right) / (left - right);
  float y = (bottom + top) / (bottom - top);

#if USE_OPENGL
  // -1..1
  float z = (znear + zfar) / (znear - zfar);
#endif

  float ortho[16] = {a, 0, 0, 0, 0, b, 0, 0, 0, 0, c, 0, x, y, z, 1};
#if USE_OPENGL
  SDL_memcpy(g_cachedProjection, ortho, sizeof(ortho));
  g_cachedProjectionValid = 1;

  GLint current_program{};
  glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
  if (current_program != 0 && current_program == g_PreviousShader &&
      u_projectionLoc != -1) {
    glUniformMatrix4fv(u_projectionLoc, 1, GL_FALSE, ortho);
  }
#endif
}

void GR_Perspective3D(const float fov, const float width, const float height,
                      const float zNear, const float zFar) {
  float sinF, cosF;
  sinF = sinf(0.5f * fov);
  cosF = cosf(0.5f * fov);

  float h = cosF / sinF;
  float w = (h * height) / width;
  float depthScale;
  float depthBias;
  GR_CalculateReversedDepthProjection(zNear, zFar, &depthScale, &depthBias);
  g_reversedDepthScale = depthScale;
  g_reversedDepthBias = depthBias;

  float persp[16] = {w, 0, 0,          0, 0, h, 0,         0,
                     0, 0, depthScale, 1, 0, 0, depthBias, 0};

#if USE_OPENGL
  SDL_memcpy(g_cachedProjection3D, persp, sizeof(persp));
  g_cachedProjection3DValid = 1;
  GLint current_program{};
  glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
  if (current_program != 0 && current_program == g_PreviousShader &&
      u_projection3DLoc != -1) {
    glUniformMatrix4fv(u_projection3DLoc, 1, GL_FALSE, persp);
  }
#endif
}

void GR_SetupClipMode(const RECT16 *rect, int enable) {
  const DISPENV &display = PsyX_GetProjectionDisplayEnv();
  // [A] isinterlaced dirty hack for widescreen
  const bool scissorOn =
      enable &&
      (display.isinter ||
       (rect->x - display.disp.x > 0 || rect->y - display.disp.y > 0 ||
        rect->w < display.disp.w - 1 || rect->h < display.disp.h - 1));

  GR_SetScissorState(scissorOn);

  if (!scissorOn || display.disp.w <= 0 || display.disp.h <= 0)
    return;

  const float psxScreenWInv = 1.0f / (float)display.disp.w;
  const float psxScreenHInv = 1.0f / (float)display.disp.h;

  // first map to 0..1
  float clipRectX = (float)(rect->x - display.disp.x) * psxScreenWInv;
  float clipRectY = (float)(rect->y - display.disp.y) * psxScreenHInv;
  float clipRectW = (float)(rect->w) * psxScreenWInv;
  float clipRectH = (float)(rect->h) * psxScreenHInv;

#if USE_OPENGL
  // Map directly into the high-resolution framebuffer. OpenGL's scissor origin
  // is bottom-left, while PSX draw environments use a top-left origin.
  const PsyXPresentationViewport viewport = PsyX_GetRenderViewport();
  const int crx = viewport.x + (int)(clipRectX * (float)viewport.w + 0.5f);
  const int cry =
      viewport.y +
      (int)((1.0f - clipRectY - clipRectH) * (float)viewport.h + 0.5f);
  const int crw = (int)(clipRectW * (float)viewport.w + 0.5f);
  const int crh = (int)(clipRectH * (float)viewport.h + 0.5f);

  glScissor(crx, cry, crw, crh);
#endif
}

void GR_CalculateReversedDepthProjection(float zNear, float zFar, float *scale,
                                         float *bias) {
  if (scale == NULL || bias == NULL)
    return;
  if (zNear <= 0.0f || zFar <= zNear) {
    *scale = -1.0f;
    *bias = 0.0f;
    return;
  }
  const float range = zFar - zNear;
  *scale = -(zFar + zNear) / range;
  *bias = (2.0f * zFar * zNear) / range;
}

void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16 *rect) {
  const DISPENV &display = PsyX_GetProjectionDisplayEnv();
#if USE_PGXP
  float psxScreenW, psxScreenH;
  float emuScreenAspect;

  const PsyXPresentationViewport viewport = PsyX_GetLogicalViewport();
  emuScreenAspect = (float)viewport.w / (float)viewport.h;

  psxScreenW = display.disp.w;
  psxScreenH = display.disp.h;

  rect->x = display.screen.x;
  rect->y = display.screen.y;

  rect->w = psxScreenW * emuScreenAspect *
            PsyX_GetActiveScreenAspect(); // windowWidth;
  rect->h = psxScreenH;                   // windowHeight;

  rect->x -= (rect->w - display.disp.w) / 2;

  rect->w += rect->x;
#else
  rect->x = display.screen.x;
  rect->y = display.screen.y;
  rect->w = display.disp.w;
  rect->h = display.disp.h;
#endif
}

void GR_SetShader(const ShaderID shader) {
  if (g_PreviousShader != shader) {
#if USE_OPENGL
    glUseProgram(shader);
#else
#error
#endif

    g_PreviousShader = shader;
  }
}

TextureFilterMode GR_ResolveTextureFilterMode(TextureFilterMode requestedMode,
                                              int bilinearFiltering,
                                              int trilinearFiltering,
                                              int anisotropicFiltering) {
  if (requestedMode == TEXTURE_FILTER_WORLD_ANISOTROPIC && anisotropicFiltering)
    return TEXTURE_FILTER_WORLD_ANISOTROPIC;
  if (requestedMode >= TEXTURE_FILTER_WORLD_TRILINEAR && trilinearFiltering)
    return TEXTURE_FILTER_WORLD_TRILINEAR;
  if (requestedMode != TEXTURE_FILTER_NEAREST &&
      (bilinearFiltering || trilinearFiltering || anisotropicFiltering))
    return TEXTURE_FILTER_BILINEAR;
  return TEXTURE_FILTER_NEAREST;
}

void GR_SetTexture(TextureID texture, TexFormat texFormat,
                   TextureFilterMode filterMode) {
  GLint textureFilterModeLoc = 0;
  PSXGPU_Shader *activeShader = NULL;
  switch (texFormat) {
  case TF_4_BIT:
    activeShader = &g_gpu_shader_4;
    GR_SetShader(g_gpu_shader_4.shader);
    textureFilterModeLoc = g_gpu_shader_4.textureFilterModeLoc;
    u_textureBlendModeLoc = g_gpu_shader_4.textureBlendModeLoc;
    u_projectionLoc = g_gpu_shader_4.projectionLoc;
    u_projection3DLoc = g_gpu_shader_4.projection3DLoc;
    u_presentationScaleLoc = g_gpu_shader_4.presentationScaleLoc;
    u_texelSizeLoc = -1;
    break;
  case TF_8_BIT:
    activeShader = &g_gpu_shader_8;
    GR_SetShader(g_gpu_shader_8.shader);
    textureFilterModeLoc = g_gpu_shader_8.textureFilterModeLoc;
    u_textureBlendModeLoc = g_gpu_shader_8.textureBlendModeLoc;
    u_projectionLoc = g_gpu_shader_8.projectionLoc;
    u_projection3DLoc = g_gpu_shader_8.projection3DLoc;
    u_presentationScaleLoc = g_gpu_shader_8.presentationScaleLoc;
    u_texelSizeLoc = -1;
    break;
  case TF_16_BIT:
    activeShader = &g_gpu_shader_16;
    GR_SetShader(g_gpu_shader_16.shader);
    textureFilterModeLoc = g_gpu_shader_16.textureFilterModeLoc;
    u_textureBlendModeLoc = g_gpu_shader_16.textureBlendModeLoc;
    u_projectionLoc = g_gpu_shader_16.projectionLoc;
    u_projection3DLoc = g_gpu_shader_16.projection3DLoc;
    u_presentationScaleLoc = g_gpu_shader_16.presentationScaleLoc;
    u_texelSizeLoc = -1;
    break;
  case TF_32_BIT_RGBA:
    activeShader = &g_gpu_shader_32_rgba;
    GR_SetShader(g_gpu_shader_32_rgba.shader);
    textureFilterModeLoc = -1;
    u_textureBlendModeLoc = -1;
    u_projectionLoc = g_gpu_shader_32_rgba.projectionLoc;
    u_projection3DLoc = g_gpu_shader_32_rgba.projection3DLoc;
    u_presentationScaleLoc = g_gpu_shader_32_rgba.presentationScaleLoc;
    u_texelSizeLoc = g_gpu_shader_32_rgba.texelSizeLoc;
    break;
  }

#if USE_OPENGL
  if (activeShader != NULL && activeShader->presentationScaleLoc != -1) {
    glUniform2f(activeShader->presentationScaleLoc, g_presentationScale.x,
                g_presentationScale.y);
  }
  if (activeShader != NULL) {
    if (g_cachedProjectionValid && activeShader->projectionLoc != -1)
      glUniformMatrix4fv(activeShader->projectionLoc, 1, GL_FALSE,
                         g_cachedProjection);
    if (g_cachedProjection3DValid && activeShader->projection3DLoc != -1)
      glUniformMatrix4fv(activeShader->projection3DLoc, 1, GL_FALSE,
                         g_cachedProjection3D);
  }
  PsyX_ApplySceneFogUniforms(activeShader);
#endif

  if (g_dbg_texturelessMode) {
    texture = g_whiteTexture;
  }

#if USE_OPENGL
  if (g_lastBoundTexture != texture) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_rgLutTexture);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_vramAliasTexture);

    glActiveTexture(GL_TEXTURE0);
    g_lastBoundTexture = texture;
  }

  // Launcher filtering applies to the complete render path. Anisotropy is a
  // separate world-only enhancement; when it is disabled, world textures
  // follow the same bilinear/nearest selection as all other primitives.
  const GLint effectiveFilterMode =
      static_cast<GLint>(GR_ResolveTextureFilterMode(
          filterMode, g_cfg_bilinearFiltering, g_cfg_trilinearFiltering,
          g_cfg_anisotropicFiltering));
  if (textureFilterModeLoc != -1 && activeShader != NULL &&
      activeShader->appliedTextureFilterMode != effectiveFilterMode) {
    glUniform1i(textureFilterModeLoc, effectiveFilterMode);
    activeShader->appliedTextureFilterMode = effectiveFilterMode;
  }
  if (texFormat == TF_32_BIT_RGBA) {
    auto cached = std::find_if(
        g_rgbaTextureFilterCache.begin(), g_rgbaTextureFilterCache.end(),
        [texture](const auto &entry) { return entry.texture == texture; });
    if (cached == g_rgbaTextureFilterCache.end() ||
        cached->filterMode != effectiveFilterMode) {
      const GLint filter = effectiveFilterMode != TEXTURE_FILTER_NEAREST
                               ? GL_LINEAR
                               : GL_NEAREST;
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
      if (g_maxTextureAnisotropy > 1.0f) {
        const GLfloat anisotropy =
            effectiveFilterMode == TEXTURE_FILTER_WORLD_ANISOTROPIC
                ? g_maxTextureAnisotropy
                : 1.0f;
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                        anisotropy);
      }
      if (cached == g_rgbaTextureFilterCache.end())
        g_rgbaTextureFilterCache.push_back({texture, effectiveFilterMode});
      else
        cached->filterMode = effectiveFilterMode;
    }
  }
#endif
}

void GR_SetTextureBlendMode(BlendMode blendMode) {
#if USE_OPENGL
  if (u_textureBlendModeLoc != -1 &&
      (g_PreviousTextureBlendShader != g_PreviousShader ||
       g_PreviousTextureBlendMode != blendMode)) {
    glUniform1i(u_textureBlendModeLoc, blendMode);
    g_PreviousTextureBlendShader = g_PreviousShader;
    g_PreviousTextureBlendMode = blendMode;
  }
#endif
}

void GR_SetOverrideTextureSize(int width, int height) {
  if (u_texelSizeLoc == -1)
    return;

  // WebGL is fucking around with glUniform2f, so use vector version
  float vec[] = {1.0f / (float)width, 1.0f / (float)height};
  glUniform2fv(u_texelSizeLoc, 1, vec);
}

void GR_DestroyTexture(TextureID texture) {
  if (texture == -1)
    return;

#if USE_OPENGL
  glDeleteTextures(1, &texture);
  g_rgbaTextureFilterCache.erase(
      std::remove_if(
          g_rgbaTextureFilterCache.begin(), g_rgbaTextureFilterCache.end(),
          [texture](const auto &entry) { return entry.texture == texture; }),
      g_rgbaTextureFilterCache.end());
  if (g_lastBoundTexture == texture)
    g_lastBoundTexture = 0;
#else
#error
#endif
}

void GR_ClearVRAM(int x, int y, int w, int h, unsigned char r, unsigned char g,
                  unsigned char b) {
  if (x + w > VRAM_WIDTH)
    w = VRAM_WIDTH - x;

  if (y + h > VRAM_HEIGHT)
    h = VRAM_HEIGHT - y;

  GR_RecordVRAMWrite(GR_VRAM_WRITE_CLEAR, x, y, x, y, w, h);

  if (w <= 0 || h <= 0)
    return;

  GR_ClearGPUVRAMDirty(x, y, w, h);
  GR_MarkVRAMDirty(x, y, w, h);
  u_short *dst = vram + x + y * VRAM_WIDTH;

  // clear VRAM region with given color
  for (int i = 0; i < h; i++) {
    u_short *tmp = dst;

    for (int j = 0; j < w; j++)
      *tmp++ = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);

    dst += VRAM_WIDTH;
  }
}

void GR_Clear(int x, int y, int w, int h, unsigned char r, unsigned char g,
              unsigned char b) {
  framebuffer_need_update = 1;

#if USE_OPENGL
  const int restoreWrite = g_PreviousDepthWrite;
  glDepthMask(GL_TRUE);
  GR_SetDepthRange(0.0f, 1.0f);
  glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (restoreWrite == 0)
    glDepthMask(GL_FALSE);
  if (g_PreviousOffscreenState)
    ++g_offscreenDepthClearSerial;
  else
    ++g_nativeDepthClearSerial;
#endif
}

void GR_SaveVRAM(const char *outputFileName, int x, int y, int width,
                 int height, int bReadFromFrameBuffer) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)

#if USE_OPENGL

#define FLIP_Y (VRAM_HEIGHT - i - 1)

#endif

#if defined(RENDERER_OGL)
  // Save/snapshot boundaries require an exact CPU-visible VRAM image.
  GR_SynchronizeGPUVRAMRect(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
  (void)bReadFromFrameBuffer;
#else
  (void)bReadFromFrameBuffer;
#endif
  const unsigned short *sourceVRAM = vram;

  FILE *fp = fopen(outputFileName, "wb");
  if (fp == NULL)
    return;

  unsigned char TGAheader[12] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  unsigned char header[6];
  header[0] = (width % 256);
  header[1] = (width / 256);
  header[2] = (height % 256);
  header[3] = (height / 256);
  header[4] = 16;
  header[5] = 0;

  fwrite(TGAheader, sizeof(unsigned char), 12, fp);
  fwrite(header, sizeof(unsigned char), 6, fp);

  for (int i = 0; i < VRAM_HEIGHT; i++) {
    fwrite(sourceVRAM + VRAM_WIDTH * FLIP_Y, sizeof(short), VRAM_WIDTH, fp);
  }

  fclose(fp);

#undef FLIP_Y
#endif
}

void GR_CopyRGBAFramebufferToVRAM(u_int *src, int x, int y, int w, int h,
                                  int update_vram, int flip_y,
                                  int source_scale = 1,
                                  bool invalidate_high_resolution = true) {
  source_scale = source_scale < 1 ? 1 : (source_scale > 2 ? 2 : source_scale);
  if (src == nullptr || x < 0 || y < 0 || w <= 0 || h <= 0 ||
      x + w > VRAM_WIDTH || y + h > VRAM_HEIGHT) {
    return;
  }
  assert(x >= 0);
  assert(y >= 0);
  assert(x + w <= VRAM_WIDTH);
  assert(y + h <= VRAM_HEIGHT);
  GR_ClearGPUVRAMDirty(x, y, w, h);
  GR_RecordVRAMWrite(GR_VRAM_WRITE_FRAMEBUFFER, x, y, x, y, w, h,
                     invalidate_high_resolution);

  // Convert directly into the emulated destination. The former two-pass path
  // allocated a full temporary image for every framebuffer feedback read,
  // then immediately copied it again; menus and effects can execute this path
  // several times in one presentation frame.
  const uint *source = (const uint *)src;
  const int source_width = w * source_scale;
  const int source_height = h * source_scale;
  const int center_sample = source_scale / 2;
  const bool diagnostics = SDL_getenv("MOHU_VRAM_CAPTURE") != NULL;
  int sourceNonzero = 0;
  for (int destinationRow = 0; destinationRow < h; ++destinationRow) {
    const int scaled_row = destinationRow * source_scale + center_sample;
    const int sourceRow = flip_y ? source_height - scaled_row - 1 : scaled_row;
    const uint *sourcePixel = source + sourceRow * source_width + center_sample;
    ushort *destinationPixel =
        (ushort *)vram + VRAM_WIDTH * (y + destinationRow) + x;
    for (int column = 0; column < w; ++column) {
      const uint color = sourcePixel[column * source_scale];
      const u_char red = (color >> 3) & 0x1F;
      const u_char green = (color >> 11) & 0x1F;
      const u_char blue = (color >> 19) & 0x1F;
      const u_char alpha = (color >> 24) & 0xff;
      // The PS1 texture STP/mask bit is represented by half alpha in the
      // render target. Opaque texels use full alpha and an empty target uses
      // zero, so preserve even the otherwise-black 0x8000 VRAM word.
      const int semiTransparent = alpha != 0 && alpha < 0xc0;
      if (diagnostics && (red != 0 || green != 0 || blue != 0))
        ++sourceNonzero;
      destinationPixel[column] =
          red | (green << 5) | (blue << 10) | (semiTransparent << 15);
    }
  }

  if (update_vram)
    GR_MarkVRAMDirty(x, y, w, h);

  if (diagnostics) {
    static uint64_t diagnosticReadbacks = 0;
    static bool loggedNonzero = false;
    ++diagnosticReadbacks;
    if (diagnosticReadbacks == 1 || (!loggedNonzero && sourceNonzero != 0) ||
        (diagnosticReadbacks & 1023) == 0) {
      SDL_Log("MOHU offscreen readback: count=%llu rect=%d,%d %dx%d "
              "nonzero=%d destination0=0x%x",
              (unsigned long long)diagnosticReadbacks, x, y, w, h,
              sourceNonzero, vram[x + y * VRAM_WIDTH]);
      loggedNonzero |= sourceNonzero != 0;
    }
  }
}

void GR_ReadFramebufferDataToVRAM() {
  int x, y, w, h;
  if (!g_cfg_framebufferFeedback) {
    framebuffer_need_update = 0;
    return;
  }

  if (!framebuffer_need_update)
    return;

  framebuffer_need_update = 0;

  x = g_PreviousFramebuffer.x;
  y = g_PreviousFramebuffer.y;
  w = g_PreviousFramebuffer.w;
  h = g_PreviousFramebuffer.h;

  // now we can read it back to VRAM texture

#if USE_OPENGL && defined(USE_PBO)
  // read the texture
  if (g_glFramebufferPBO.pixels) {
    glBindTexture(GL_TEXTURE_2D, g_fbTexture);
    PBO_Download(&g_glFramebufferPBO);
    glBindTexture(GL_TEXTURE_2D, 0);
    GR_CopyRGBAFramebufferToVRAM((u_int *)g_glFramebufferPBO.pixels, x, y, w, h,
                                 0, 0);
  }
#endif
}

void GR_SetScissorState(int enable) {
  if (g_PreviousScissorState == enable)
    return;

#if USE_OPENGL
  if (g_PreviousScissorState)
    glDisable(GL_SCISSOR_TEST);
  else
    glEnable(GL_SCISSOR_TEST);
#endif
  g_PreviousScissorState = enable;
}

static void GR_SeedOffscreenColorFromVRAM(const RECT16 *offscreenRect) {
#if USE_OPENGL
  const int logical_width = offscreenRect->w;
  const int logical_height = offscreenRect->h;
  const int width = g_offscreenTextureWidth;
  const int height = g_offscreenTextureHeight;

  ++g_guestSeedCount;
  g_guestSeedPixels += static_cast<unsigned long long>(width) *
                       static_cast<unsigned long long>(height);
#if defined(RENDERER_OGL)
  const auto *retained = GR_FindHighResolutionVRAMPage(
      offscreenRect->x, offscreenRect->y, logical_width, logical_height);
  if (retained != nullptr) {
    const int source_x0 =
        GR_MapGuestEdge(offscreenRect->x - retained->rect.x, retained->rect.w,
                        retained->pixel_width);
    const int source_x1 =
        GR_MapGuestEdge(offscreenRect->x + logical_width - retained->rect.x,
                        retained->rect.w, retained->pixel_width);
    const int source_y0 =
        GR_MapGuestEdge(retained->rect.y + retained->rect.h -
                            (offscreenRect->y + logical_height),
                        retained->rect.h, retained->pixel_height);
    const int source_y1 =
        GR_MapGuestEdge(retained->rect.y + retained->rect.h - offscreenRect->y,
                        retained->rect.h, retained->pixel_height);
    const int scissor_enabled = g_PreviousScissorState;
    if (scissor_enabled)
      glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glHighResolutionVRAMFramebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, retained->texture, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glOffscreenFramebuffer);
    glBlitFramebuffer(source_x0, source_y0, source_x1, source_y1, 0, 0, width,
                      height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);
    if (scissor_enabled)
      glEnable(GL_SCISSOR_TEST);
    return;
  }
  if (GR_UnpackGuestVRAMToOffscreen(*offscreenRect, width, height))
    return;
#endif

  // Exact logical fallback for non-desktop backends or unavailable conversion.
  static std::vector<u_int> seedPixels;
  seedPixels.resize(static_cast<size_t>(width) * height);
  const auto expand5 = [](u_short value) -> u_int {
    value &= 31;
    return static_cast<u_int>((value << 3) | (value >> 2));
  };
  for (int glRow = 0; glRow < height; ++glRow) {
    const int logical_gl_row = std::min(
        static_cast<int>((static_cast<long long>(glRow) * logical_height) /
                         std::max(height, 1)),
        logical_height - 1);
    const int sourceY = offscreenRect->y + logical_height - logical_gl_row - 1;
    const u_short *source = vram + sourceY * VRAM_WIDTH + offscreenRect->x;
    u_int *destination =
        seedPixels.data() + static_cast<std::size_t>(glRow) * width;
    for (int column = 0; column < width; ++column) {
      const int logical_column = std::min(
          static_cast<int>((static_cast<long long>(column) * logical_width) /
                           std::max(width, 1)),
          logical_width - 1);
      const u_short color = source[logical_column];
      const u_int red = expand5(color);
      const u_int green = expand5(color >> 5);
      const u_int blue = expand5(color >> 10);
      const u_int alpha = (color & 0x8000U) != 0U ? 0x80U : 0xffU;
      destination[column] = red | (green << 8) | (blue << 16) | (alpha << 24);
    }
  }

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
                  GL_UNSIGNED_BYTE, seedPixels.data());
  glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);
#else
  (void)offscreenRect;
#endif
}
static const GrHighResolutionVRAMPage *
GR_FindHighResolutionVRAMPage(int x, int y, int width, int height) {
  GR_RefreshGuestRenderConfiguration();
  if (x < 0 || y < 0 || width <= 0 || height <= 0 || x + width > VRAM_WIDTH ||
      y + height > VRAM_HEIGHT) {
    return nullptr;
  }

  const GrHighResolutionVRAMPage *best = nullptr;
  for (auto &page : g_highResolutionVRAMPages) {
    const bool contains = page.valid && x >= page.rect.x && y >= page.rect.y &&
                          x + width <= page.rect.x + page.rect.w &&
                          y + height <= page.rect.y + page.rect.h;
    if (!contains || !GR_SynchronizeHighResolutionVRAMPage(page))
      continue;
    if (best == nullptr || page.generation > best->generation)
      best = &page;
  }
  return best;
}

static bool GR_PatchHighResolutionVRAMPages(const RECT16 &rect) {
#if defined(RENDERER_OGL)
  if (rect.w <= 0 || rect.h <= 0)
    return false;

  const int scissor_enabled = g_PreviousScissorState;
  if (scissor_enabled)
    glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glOffscreenFramebuffer);

  bool has_containing_page = false;
  for (auto &page : g_highResolutionVRAMPages) {
    if (!page.valid || page.texture == 0U ||
        !GR_RectanglesOverlap(page.rect.x, page.rect.y, page.rect.w,
                              page.rect.h, rect.x, rect.y, rect.w, rect.h) ||
        !GR_SynchronizeHighResolutionVRAMPage(page)) {
      continue;
    }

    const int intersection_x0 = std::max<int>(page.rect.x, rect.x);
    const int intersection_y0 = std::max<int>(page.rect.y, rect.y);
    const int intersection_x1 =
        std::min<int>(page.rect.x + page.rect.w, rect.x + rect.w);
    const int intersection_y1 =
        std::min<int>(page.rect.y + page.rect.h, rect.y + rect.h);
    const int source_x0 = GR_MapGuestEdge(intersection_x0 - rect.x, rect.w,
                                          g_offscreenTextureWidth);
    const int source_x1 = GR_MapGuestEdge(intersection_x1 - rect.x, rect.w,
                                          g_offscreenTextureWidth);
    const int source_y0 = GR_MapGuestEdge(rect.y + rect.h - intersection_y1,
                                          rect.h, g_offscreenTextureHeight);
    const int source_y1 = GR_MapGuestEdge(rect.y + rect.h - intersection_y0,
                                          rect.h, g_offscreenTextureHeight);
    const int destination_x0 = GR_MapGuestEdge(intersection_x0 - page.rect.x,
                                               page.rect.w, page.pixel_width);
    const int destination_x1 = GR_MapGuestEdge(intersection_x1 - page.rect.x,
                                               page.rect.w, page.pixel_width);
    const int destination_y0 =
        GR_MapGuestEdge(page.rect.y + page.rect.h - intersection_y1,
                        page.rect.h, page.pixel_height);
    const int destination_y1 =
        GR_MapGuestEdge(page.rect.y + page.rect.h - intersection_y0,
                        page.rect.h, page.pixel_height);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glHighResolutionVRAMFramebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, page.texture, 0);
    glBlitFramebuffer(source_x0, source_y0, source_x1, source_y1,
                      destination_x0, destination_y0, destination_x1,
                      destination_y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    page.synced_write_sequence = GR_GetVRAMWriteSequence();
    const bool contains_rect = rect.x >= page.rect.x && rect.y >= page.rect.y &&
                               rect.x + rect.w <= page.rect.x + page.rect.w &&
                               rect.y + rect.h <= page.rect.y + page.rect.h;
    const bool preserves_raster_density =
        destination_x1 - destination_x0 >= g_offscreenTextureWidth &&
        destination_y1 - destination_y0 >= g_offscreenTextureHeight;
    has_containing_page =
        has_containing_page || (contains_rect && preserves_raster_density);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
  glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);
  if (scissor_enabled)
    glEnable(GL_SCISSOR_TEST);
  return has_containing_page;
#else
  (void)rect;
  return false;
#endif
}

static void GR_CaptureHighResolutionVRAMPage(const RECT16 &rect) {
#if defined(RENDERER_OGL)
  if (rect.w <= 0 || rect.h <= 0)
    return;
  ++g_guestCaptureCount;
  g_guestCapturePixels +=
      static_cast<unsigned long long>(g_offscreenTextureWidth) *
      static_cast<unsigned long long>(g_offscreenTextureHeight);
  if (GR_PatchHighResolutionVRAMPages(rect))
    return;

  GrHighResolutionVRAMPage *destination = nullptr;
  for (auto &page : g_highResolutionVRAMPages) {
    if (page.rect.x == rect.x && page.rect.y == rect.y &&
        page.rect.w == rect.w && page.rect.h == rect.h) {
      destination = &page;
      break;
    }
  }
  if (destination == nullptr) {
    destination = &*std::min_element(
        g_highResolutionVRAMPages.begin(), g_highResolutionVRAMPages.end(),
        [](const auto &left, const auto &right) {
          if (left.valid != right.valid)
            return !left.valid;
          return left.generation < right.generation;
        });
  }

  const int pixel_width = g_offscreenTextureWidth;
  const int pixel_height = g_offscreenTextureHeight;
  if (destination->texture == 0U)
    glGenTextures(1, &destination->texture);
  glBindTexture(GL_TEXTURE_2D, destination->texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (destination->pixel_width != pixel_width ||
      destination->pixel_height != pixel_height) {
    glTexImage2D(GL_TEXTURE_2D, 0, g_rgbaRenderTargetInternalFormat,
                 pixel_width, pixel_height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
  }

  const int scissor_enabled = g_PreviousScissorState;
  if (scissor_enabled)
    glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glOffscreenFramebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glHighResolutionVRAMFramebuffer);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, destination->texture, 0);
  glBlitFramebuffer(0, 0, pixel_width, pixel_height, 0, 0, pixel_width,
                    pixel_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  if (scissor_enabled)
    glEnable(GL_SCISSOR_TEST);

  destination->rect = rect;
  destination->pixel_width = pixel_width;
  destination->pixel_height = pixel_height;
  destination->generation = ++g_highResolutionVRAMGeneration;
  destination->synced_write_sequence = GR_GetVRAMWriteSequence();
  for (auto &row : destination->pending_writes.rows)
    row.fill(0U);
  destination->valid = true;
  glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
  glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);
#else
  (void)rect;
#endif
}

int GR_HasHighResolutionVRAM(int x, int y, int width, int height) {
  return GR_FindHighResolutionVRAMPage(x, y, width, height) != nullptr;
}

int GR_PresentHighResolutionVRAM(int x, int y, int width, int height) {
#if defined(RENDERER_OGL)
  const auto *page = GR_FindHighResolutionVRAMPage(x, y, width, height);
  if (page == nullptr || g_nativeFramebufferWidth <= 0 ||
      g_nativeFramebufferHeight <= 0) {
    return 0;
  }

  const int source_x0 =
      GR_MapGuestEdge(x - page->rect.x, page->rect.w, page->pixel_width);
  const int source_x1 = GR_MapGuestEdge(x + width - page->rect.x, page->rect.w,
                                        page->pixel_width);
  const int source_y0 =
      GR_MapGuestEdge(page->rect.y + page->rect.h - (y + height), page->rect.h,
                      page->pixel_height);
  const int source_y1 = GR_MapGuestEdge(page->rect.y + page->rect.h - y,
                                        page->rect.h, page->pixel_height);
  const PsyXPresentationViewport destination = PsyX_GetRenderViewport();
  const int scissor_enabled = g_PreviousScissorState;
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glHighResolutionVRAMFramebuffer);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, page->texture, 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glNativeFramebuffer);
  glBlitFramebuffer(source_x0, source_y0, source_x1, source_y1, destination.x,
                    destination.y, destination.x + destination.w,
                    destination.y + destination.h, GL_COLOR_BUFFER_BIT,
                    (g_cfg_bilinearFiltering || g_cfg_trilinearFiltering ||
                     g_cfg_anisotropicFiltering)
                        ? GL_LINEAR
                        : GL_NEAREST);
  // The retained page is already single-sample resolved. Do not let EndScene
  // overwrite it with an older multisample color target.
  g_nativeFramePostprocessed = 1;
  glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeFramebuffer);
  if (scissor_enabled)
    glEnable(GL_SCISSOR_TEST);
  return 1;
#else
  (void)x;
  (void)y;
  (void)width;
  (void)height;
  return 0;
#endif
}

#if defined(RENDERER_OGL)
static bool GR_GuestPresentationReplayRectEquals(const RECT16 &left,
                                                 const RECT16 &right) {
  return left.x == right.x && left.y == right.y && left.w == right.w &&
         left.h == right.h;
}

static bool GR_EnsureGuestPresentationReplayTarget(int width, int height) {
  if (width <= 0 || height <= 0)
    return false;

  GLint maximum_texture_size{};
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_texture_size);
  if (width > maximum_texture_size || height > maximum_texture_size)
    return false;

  if (g_guestPresentationReplayTexture == 0U) {
    glGenTextures(1, &g_guestPresentationReplayTexture);
    glBindTexture(GL_TEXTURE_2D, g_guestPresentationReplayTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  if (g_glGuestPresentationReplayFramebuffer == 0U)
    glGenFramebuffers(1, &g_glGuestPresentationReplayFramebuffer);
  if (g_guestPresentationReplayDepthStencilRenderbuffer == 0U) {
    glGenRenderbuffers(1, &g_guestPresentationReplayDepthStencilRenderbuffer);
  }

  const bool resize = width != g_guestPresentationReplayCapacityWidth ||
                      height != g_guestPresentationReplayCapacityHeight;
  if (resize) {
    glBindTexture(GL_TEXTURE_2D, g_guestPresentationReplayTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, g_rgbaRenderTargetInternalFormat, width,
                 height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindRenderbuffer(GL_RENDERBUFFER,
                       g_guestPresentationReplayDepthStencilRenderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, g_glGuestPresentationReplayFramebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g_guestPresentationReplayTexture, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER,
                            g_guestPresentationReplayDepthStencilRenderbuffer);
  const bool complete =
      glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);
  if (!complete) {
    glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
    return false;
  }

  g_guestPresentationReplayCapacityWidth = width;
  g_guestPresentationReplayCapacityHeight = height;
  return true;
}

static void GR_ClearGuestPresentationReplayTarget(unsigned char r,
                                                  unsigned char g,
                                                  unsigned char b) {
  GLboolean color_mask[4]{};
  GLboolean depth_mask{};
  GLint stencil_mask{};
  GLfloat clear_color[4]{};
  GLint scissor_box[4]{};
  glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
  glGetIntegerv(GL_STENCIL_WRITEMASK, &stencil_mask);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);
  glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
  const GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);

  glBindFramebuffer(GL_FRAMEBUFFER, g_glGuestPresentationReplayFramebuffer);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glStencilMask(0xffU);
  glClearColor(r / 255.0F, g / 255.0F, b / 255.0F, 1.0F);
  glClearDepth(0.0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  ++g_offscreenDepthClearSerial;

  glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
  glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
  glDepthMask(depth_mask);
  glStencilMask(static_cast<GLuint>(stencil_mask));
  if (scissor_enabled)
    glEnable(GL_SCISSOR_TEST);
  glScissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
  g_PreviousDepthWrite = -1;
  g_PreviousStencilMode = -1;
}
#endif

int GR_BeginGuestPresentationReplay(const RECT16 *target) {
#if defined(RENDERER_OGL)
  DrawSync(0);
  if (target == nullptr || target->x < 0 || target->y < 0 || target->w <= 0 ||
      target->h <= 0 || target->x + target->w > VRAM_WIDTH ||
      target->y + target->h > VRAM_HEIGHT || g_guestPresentationReplayActive ||
      g_PreviousOffscreenState) {
    return 0;
  }

  const PsyXPresentationViewport output = PsyX_GetRenderTargetExtent();
  const PsyXPresentationViewport extent = PsyX_CalculateGuestRenderExtent(
      output.w, output.h, g_cfg_aspectMode, target->w, target->h,
      std::max(g_pendingGuestDisplayWidth, 1),
      std::max(g_pendingGuestDisplayHeight, 1), 1);
  if (!GR_EnsureGuestPresentationReplayTarget(extent.w, extent.h))
    return 0;

  g_guestPresentationReplayRect = *target;
  g_guestPresentationReplayPixelWidth = extent.w;
  g_guestPresentationReplayPixelHeight = extent.h;
  g_guestPresentationReplayFailed = 0;
  g_guestPresentationReplayValid = 0;
  g_guestPresentationReplayClosing = 0;
  g_guestPresentationReplayActive = 1;
  g_appliedOffscreenProjectionValid = 0;
  GR_SetOffscreenState(target, 1);
  if (!g_PreviousOffscreenState || g_guestPresentationReplayFailed) {
    g_guestPresentationReplayClosing = 1;
    if (g_PreviousOffscreenState) {
      RECT16 completed{};
      GR_SetOffscreenState(&completed, 0);
    }
    g_guestPresentationReplayClosing = 0;
    g_guestPresentationReplayActive = 0;
    g_guestPresentationReplayValid = 0;
    return 0;
  }
  return 1;
#else
  (void)target;
  return 0;
#endif
}

int GR_ClearGuestPresentationReplay(unsigned char r, unsigned char g,
                                    unsigned char b) {
#if defined(RENDERER_OGL)
  if (!g_guestPresentationReplayActive || !g_PreviousOffscreenState)
    return 0;
  DrawSync(0);
  if (g_guestPresentationReplayFailed || !g_PreviousOffscreenState)
    return 0;
  GR_ClearGuestPresentationReplayTarget(r, g, b);
  return 1;
#else
  (void)r;
  (void)g;
  (void)b;
  return 0;
#endif
}

int GR_EndGuestPresentationReplay(void) {
#if defined(RENDERER_OGL)
  if (!g_guestPresentationReplayActive)
    return 0;
  DrawSync(0);
  g_guestPresentationReplayClosing = 1;
  if (g_PreviousOffscreenState) {
    RECT16 completed{};
    GR_SetOffscreenState(&completed, 0);
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
  }
  const int success = g_guestPresentationReplayFailed == 0;
  g_guestPresentationReplayClosing = 0;
  g_guestPresentationReplayActive = 0;
  g_guestPresentationReplayValid = success;
  g_appliedOffscreenProjectionValid = 0;
  return success;
#else
  return 0;
#endif
}

int GR_PresentGuestPresentationReplay(int x, int y, int width, int height) {
#if defined(RENDERER_OGL)
  const bool contained = x >= g_guestPresentationReplayRect.x &&
                         y >= g_guestPresentationReplayRect.y && width > 0 &&
                         height > 0 &&
                         x + width <= g_guestPresentationReplayRect.x +
                                          g_guestPresentationReplayRect.w &&
                         y + height <= g_guestPresentationReplayRect.y +
                                           g_guestPresentationReplayRect.h;
  if (g_guestPresentationReplayActive || !g_guestPresentationReplayValid ||
      !contained || g_nativeFramebufferWidth <= 0 ||
      g_nativeFramebufferHeight <= 0) {
    return 0;
  }

  const int source_x0 = GR_MapGuestEdge(x - g_guestPresentationReplayRect.x,
                                        g_guestPresentationReplayRect.w,
                                        g_guestPresentationReplayPixelWidth);
  const int source_x1 = GR_MapGuestEdge(
      x + width - g_guestPresentationReplayRect.x,
      g_guestPresentationReplayRect.w, g_guestPresentationReplayPixelWidth);
  const int source_y0 = GR_MapGuestEdge(
      g_guestPresentationReplayRect.y + g_guestPresentationReplayRect.h -
          (y + height),
      g_guestPresentationReplayRect.h, g_guestPresentationReplayPixelHeight);
  const int source_y1 = GR_MapGuestEdge(
      g_guestPresentationReplayRect.y + g_guestPresentationReplayRect.h - y,
      g_guestPresentationReplayRect.h, g_guestPresentationReplayPixelHeight);
  const PsyXPresentationViewport destination = PsyX_GetRenderViewport();
  const GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER,
                    g_glGuestPresentationReplayFramebuffer);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glNativeFramebuffer);
  glBlitFramebuffer(source_x0, source_y0, source_x1, source_y1, destination.x,
                    destination.y, destination.x + destination.w,
                    destination.y + destination.h, GL_COLOR_BUFFER_BIT,
                    (g_cfg_bilinearFiltering || g_cfg_trilinearFiltering ||
                     g_cfg_anisotropicFiltering)
                        ? GL_LINEAR
                        : GL_NEAREST);
  g_nativeFramePostprocessed = 1;
  glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeFramebuffer);
  if (scissor_enabled)
    glEnable(GL_SCISSOR_TEST);
  return 1;
#else
  (void)x;
  (void)y;
  (void)width;
  (void)height;
  return 0;
#endif
}

void GR_SetOffscreenState(const RECT16 *offscreenRect, int enable) {
#if defined(RENDERER_OGL)
  const bool presentation_replay = g_guestPresentationReplayActive != 0;
  if (presentation_replay) {
    if (enable && !GR_GuestPresentationReplayRectEquals(
                      *offscreenRect, g_guestPresentationReplayRect)) {
      g_guestPresentationReplayFailed = 1;
      return;
    }
    if (!enable && !g_guestPresentationReplayClosing) {
      g_guestPresentationReplayFailed = 1;
      return;
    }
  }
  const PsyXPresentationViewport pending_target = PsyX_GetRenderTargetExtent();
  const bool guest_geometry_changed =
      !presentation_replay &&
      (g_pendingGuestDisplayWidth != g_guestDisplayWidth ||
       g_pendingGuestDisplayHeight != g_guestDisplayHeight);
  const bool guest_configuration_changed =
      !presentation_replay && (g_guestRenderConfigWidth != pending_target.w ||
                               g_guestRenderConfigHeight != pending_target.h ||
                               g_guestRenderConfigAspect != g_cfg_aspectMode);
#else
  constexpr bool presentation_replay = false;
  constexpr bool guest_geometry_changed = false;
  constexpr bool guest_configuration_changed = false;
#endif
#if defined(RENDERER_OGL)
  const bool same_target =
      g_PreviousOffscreenState == enable &&
      (!enable || (g_PreviousOffscreen.x == offscreenRect->x &&
                   g_PreviousOffscreen.y == offscreenRect->y &&
                   g_PreviousOffscreen.w == offscreenRect->w &&
                   g_PreviousOffscreen.h == offscreenRect->h));
  if (g_appliedOffscreenProjectionValid && same_target &&
      !guest_geometry_changed && !guest_configuration_changed &&
      g_appliedOffscreenProjectionRevision == g_projectionStateRevision) {
    return;
  }
#endif
  if (enable && g_PreviousOffscreenState &&
      (g_PreviousOffscreen.x != offscreenRect->x ||
       g_PreviousOffscreen.y != offscreenRect->y ||
       g_PreviousOffscreen.w != offscreenRect->w ||
       g_PreviousOffscreen.h != offscreenRect->h || guest_geometry_changed ||
       guest_configuration_changed)) {
    // Complete the old page before applying a new rect, stable display domain,
    // or selected output. The recursive disable intentionally keeps the old
    // canonical/config state; the following enable commits and reseeds once.
    const RECT16 next = *offscreenRect;
    GR_SetOffscreenState(&next, 0);
    GR_SetOffscreenState(&next, 1);
    return;
  }

  if (enable && !presentation_replay) {
    GR_ApplyPendingGuestDisplayGeometry();
    GR_RefreshGuestRenderConfiguration();
  }
  const PsyXPresentationViewport logicalViewport = PsyX_GetLogicalViewport();
  const PsyXPresentationViewport renderViewport = PsyX_GetRenderViewport();
#if defined(RENDERER_OGL)
  const GrGuestPixelExtent requested_extent =
      enable && presentation_replay
          ? GrGuestPixelExtent{g_guestPresentationReplayPixelWidth,
                               g_guestPresentationReplayPixelHeight}
      : enable ? GR_CalculateOffscreenPixelExtent(*offscreenRect)
               : GrGuestPixelExtent{std::max(g_offscreenTextureWidth, 1),
                                    std::max(g_offscreenTextureHeight, 1)};
#else
  const GrGuestPixelExtent requested_extent =
      enable ? GR_CalculateOffscreenPixelExtent(*offscreenRect)
             : GrGuestPixelExtent{std::max(g_offscreenTextureWidth, 1),
                                  std::max(g_offscreenTextureHeight, 1)};
#endif
  int offscreen_pixel_width = requested_extent.width;
  int offscreen_pixel_height = requested_extent.height;

#if USE_PGXP
  constexpr float perspectiveFOV = 0.9265f;
  constexpr float perspectiveZNear = PGXP_NEAR_PLANE;
  constexpr float perspectiveZFar = 1000.0f;
#endif
  if (enable) {
    // setup render target viewport
#if USE_PGXP
    GR_Ortho2D(-0.5f, 0.5f, 0.5f, -0.5f, -1.0f, 1.0f);
    // Precise XY is already homogeneous, but it still needs a current Z/W
    // projection while rendering retained guest pages offscreen.
    GR_Perspective3D(perspectiveFOV, 1.0f, 1.0f, perspectiveZNear,
                     perspectiveZFar);
#else
    GR_Ortho2D(0, offscreenRect->w, offscreenRect->h, 0, -1.0f, 1.0f);
#endif
  } else {
    // setup default viewport
#if USE_PGXP
    const float emuScreenAspect =
        (float)logicalViewport.w / (float)logicalViewport.h;

    const float screenAspect = PsyX_GetActiveScreenAspect();
    GR_Ortho2D(-0.5f * emuScreenAspect * screenAspect,
               0.5f * emuScreenAspect * screenAspect, 0.5f, -0.5f, -1.0f, 1.0f);
    GR_Perspective3D(perspectiveFOV, 1.0f,
                     1.0f / (emuScreenAspect * screenAspect), perspectiveZNear,
                     perspectiveZFar);
#else
    const DISPENV &display = PsyX_GetProjectionDisplayEnv();
    GR_Ortho2D(0, display.disp.w, display.disp.h, 0, -1.0f, 1.0f);
#endif
  }

#if USE_OPENGL
  // Apply Hor+/Vert+ while guest geometry is rasterized at the selected target
  // resolution. World primitives outside the original 4:3 aperture become
  // visible; authored HUD and movies remain centred, while explicitly marked
  // fullscreen clears still cover the complete target.
  g_presentationScale =
      enable ? PsyX_CalculatePresentationScale(
                   renderViewport.w, renderViewport.h, g_cfg_aspectMode)
             : PsyX_GetPresentationScale();
  GLint current_program{};
  glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
  if (current_program != 0 && current_program == g_PreviousShader &&
      u_presentationScaleLoc != -1) {
    glUniform2f(u_presentationScaleLoc, g_presentationScale.x,
                g_presentationScale.y);
  }
#endif

  // Reapply the viewport even when the render-target state did not change;
  // DISPENV can switch resolution without switching draw target type.
  if (enable)
    GR_SetViewPort(0, 0, offscreen_pixel_width, offscreen_pixel_height);
  else
    GR_SetViewPort(renderViewport.x, renderViewport.y, renderViewport.w,
                   renderViewport.h);

#if defined(RENDERER_OGL)
  g_appliedOffscreenProjectionRevision = g_projectionStateRevision;
  g_appliedOffscreenProjectionValid = 1;
#endif
  if (g_PreviousOffscreenState == enable)
    return;

  g_PreviousOffscreenState = enable;

#if USE_OPENGL
  if (enable) {
#if defined(RENDERER_OGL)
    if (presentation_replay) {
      g_PreviousOffscreen = *offscreenRect;
      glBindFramebuffer(GL_FRAMEBUFFER, g_glGuestPresentationReplayFramebuffer);
      glDisable(GL_STENCIL_TEST);
      g_PreviousStencilMode = -1;
      GR_ClearGuestPresentationReplayTarget(0U, 0U, 0U);
      return;
    }
#endif
    // Backing storage only grows when the immutable selected target requires
    // it. Root/nested switches change the active viewport without reallocating
    // multi-megabyte color/depth resources.
    if (g_offscreenTextureCapacityWidth < offscreen_pixel_width ||
        g_offscreenTextureCapacityHeight < offscreen_pixel_height) {
      const int capacity_width =
          std::max(g_offscreenTextureCapacityWidth, offscreen_pixel_width);
      const int capacity_height =
          std::max(g_offscreenTextureCapacityHeight, offscreen_pixel_height);
      glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
      glTexImage2D(GL_TEXTURE_2D, 0, g_rgbaRenderTargetInternalFormat,
                   capacity_width, capacity_height, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
      glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);
      PsyX_AllocateOffscreenDepthStencil(capacity_width, capacity_height);
      const auto framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
        g_guestRenderLogicalFallback = true;
        GR_InvalidateAllHighResolutionVRAMPages();
        offscreen_pixel_width = std::max<int>(offscreenRect->w, 1);
        offscreen_pixel_height = std::max<int>(offscreenRect->h, 1);
        glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, g_rgbaRenderTargetInternalFormat,
                     offscreen_pixel_width, offscreen_pixel_height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);
        PsyX_AllocateOffscreenDepthStencil(offscreen_pixel_width,
                                           offscreen_pixel_height);
        const auto fallback_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (fallback_status != GL_FRAMEBUFFER_COMPLETE) {
          eprinterr("Guest framebuffer is incomplete at selected and logical "
                    "resolution (0x%x/0x%x, %dx%d)\n",
                    framebuffer_status, fallback_status, offscreen_pixel_width,
                    offscreen_pixel_height);
          g_PreviousOffscreenState = 0;
          g_offscreenTextureCapacityWidth = 0;
          g_offscreenTextureCapacityHeight = 0;
          glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
          GR_SetViewPort(renderViewport.x, renderViewport.y, renderViewport.w,
                         renderViewport.h);
          return;
        }
        g_offscreenTextureCapacityWidth = offscreen_pixel_width;
        g_offscreenTextureCapacityHeight = offscreen_pixel_height;
        eprintwarn("Selected guest target is unavailable; using logical 1x "
                   "fallback\n");
      } else {
        g_offscreenTextureCapacityWidth = capacity_width;
        g_offscreenTextureCapacityHeight = capacity_height;
      }
    }
    // Allocation can fail closed from the selected target to logical 1x.
    // Apply the final active extent only after that decision so draw and
    // scissor state cannot retain the rejected oversized viewport.
    GR_SetViewPort(0, 0, offscreen_pixel_width, offscreen_pixel_height);
    g_offscreenTextureWidth = offscreen_pixel_width;
    g_offscreenTextureHeight = offscreen_pixel_height;
    g_PreviousOffscreen = *offscreenRect;

    GR_SeedOffscreenColorFromVRAM(offscreenRect);
    glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);
    // Guest framebuffer pages are copied back into emulated VRAM and do not
    // retain the host stencil history across page switches. Host stencil
    // rejection would otherwise discard every guest color fragment.
    glDisable(GL_STENCIL_TEST);

    // The color target was seeded from PS1 VRAM so pixels outside the submitted
    // primitives survive a wide draw area. Only per-pass host state is reset.
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    // The clear overrides cached write state. Force the next transparent/depth
    // or stencil run to publish its masks instead of trusting stale caches.
    g_PreviousDepthWrite = -1;
    g_PreviousStencilMode = -1;
    const GLboolean clear_scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint previous_scissor[4]{};
    glGetIntegerv(GL_SCISSOR_BOX, previous_scissor);
    glEnable(GL_SCISSOR_TEST);
    GR_SetDepthRange(0.0f, 1.0f);
    glScissor(0, 0, offscreen_pixel_width, offscreen_pixel_height);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    ++g_offscreenDepthClearSerial;
    if (!clear_scissor_enabled)
      glDisable(GL_SCISSOR_TEST);
    glScissor(previous_scissor[0], previous_scissor[1], previous_scissor[2],
              previous_scissor[3]);
  } else {
#if defined(RENDERER_OGL)
    if (presentation_replay) {
      glEnable(GL_STENCIL_TEST);
      g_PreviousStencilMode = -1;
      glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
      return;
    }
#endif
    const int scissor_enabled = g_PreviousScissorState;
    if (scissor_enabled)
      glDisable(GL_SCISSOR_TEST);
#if USE_OFFSCREEN_BLIT
    // before drawing set source and target
    {
      glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

      // rebind texture
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_vramTexture, 0);

      // setup draw and read framebuffers
      glBindFramebuffer(GL_READ_FRAMEBUFFER,
                        g_glOffscreenFramebuffer); // source is backbuffer
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

      glBlitFramebuffer(0, 0, offscreen_pixel_width, offscreen_pixel_height,
                        g_PreviousOffscreen.x,
                        g_PreviousOffscreen.y + g_PreviousOffscreen.h,
                        g_PreviousOffscreen.x + g_PreviousOffscreen.w,
                        g_PreviousOffscreen.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);

      // done, unbind
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    }
#endif

#if defined(RENDERER_OGL)
    const bool packed_on_gpu = GR_PackGuestFramebufferToVRAM(
        g_PreviousOffscreen, offscreen_pixel_width, offscreen_pixel_height);
    if (!packed_on_gpu) {
      // Fail closed: a driver without the packed shader/FBO path keeps exact
      // PS1 semantics via one explicit synchronous logical readback.
      glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glOffscreenFramebuffer);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glGuestFeedbackFramebuffer);
      glBlitFramebuffer(0, 0, offscreen_pixel_width, offscreen_pixel_height, 0,
                        0, g_PreviousOffscreen.w, g_PreviousOffscreen.h,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
      glBindFramebuffer(GL_FRAMEBUFFER, g_glGuestFeedbackFramebuffer);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
      glReadPixels(0, 0, g_PreviousOffscreen.w, g_PreviousOffscreen.h, GL_RGBA,
                   GL_UNSIGNED_BYTE, g_glOffscreenPBO.pixels);
      ++g_synchronousVRAMReadbacks;
      GR_CopyRGBAFramebufferToVRAM((u_int *)g_glOffscreenPBO.pixels,
                                   g_PreviousOffscreen.x, g_PreviousOffscreen.y,
                                   g_PreviousOffscreen.w, g_PreviousOffscreen.h,
                                   1, 1, 1, false);
      GR_UploadVRAMRegionToAllTextures(
          g_PreviousOffscreen.x, g_PreviousOffscreen.y, g_PreviousOffscreen.w,
          g_PreviousOffscreen.h);
    }
    GR_CaptureGuestColorFromOffscreen(
        g_PreviousOffscreen, offscreen_pixel_width, offscreen_pixel_height);
#else
    glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
    PBO_Download(&g_glOffscreenPBO);
    GR_CopyRGBAFramebufferToVRAM((u_int *)g_glOffscreenPBO.pixels,
                                 g_PreviousOffscreen.x, g_PreviousOffscreen.y,
                                 g_PreviousOffscreen.w, g_PreviousOffscreen.h,
                                 1, 1, 1, false);
#endif
    GR_CaptureHighResolutionVRAMPage(g_PreviousOffscreen);
    if (scissor_enabled)
      glEnable(GL_SCISSOR_TEST);
    glEnable(GL_STENCIL_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
  }
#endif
}

void GR_StoreFrameBuffer(int x, int y, int w, int h) {
#if USE_OPENGL
  // set storage size first
  if (g_PreviousFramebuffer.w != w || g_PreviousFramebuffer.h != h) {
    glBindTexture(GL_TEXTURE_2D, g_fbTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  g_PreviousFramebuffer.x = x;
  g_PreviousFramebuffer.y = y;
  g_PreviousFramebuffer.w = w;
  g_PreviousFramebuffer.h = h;

#if USE_FRAMEBUFFER_BLIT
  PsyX_ResolveNativeFramebuffer();
  glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

  // before drawing set source and target
  {
    // setup draw and read framebuffers
    glBindFramebuffer(
        GL_READ_FRAMEBUFFER,
        g_glNativeFramebuffer); // source is native PSX framebuffer
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glBlitFramebuffer);

    const PsyXPresentationViewport viewport = PsyX_GetRenderViewport();
    glBlitFramebuffer(viewport.x, viewport.y, viewport.x + viewport.w,
                      viewport.y + viewport.h, x, y + h, x + w, y,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // Blit framebuffer to VRAM screen area

    // before drawing set source and target
    glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

    // rebind vram texture
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           g_vramTexture, 0);

    // setup draw and read framebuffers
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      g_glBlitFramebuffer); // source is backbuffer
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

    glBlitFramebuffer(0, 0, w, h, x, y + h, x + w, y, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);

    // done, unbind
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  }

  // after drawing
  glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
  // The following PBO transfer is ordered after the blit by OpenGL itself.
  // An explicit flush here forced every framebuffer-feedback effect to submit
  // the whole command queue early and caused visible frame-time spikes.
#endif

  GR_ReadFramebufferDataToVRAM();
#endif
}

void GR_CopyVRAM(unsigned short *src, int x, int y, int w, int h, int dst_x,
                 int dst_y) {
  if (w <= 0 || h <= 0)
    return;
  assert(x >= 0 && y >= 0 && dst_x >= 0 && dst_y >= 0);
  assert(dst_x + w <= VRAM_WIDTH && dst_y + h <= VRAM_HEIGHT);

  const bool internalCopy = src == NULL;
  if (internalCopy) {
    assert(x + w <= VRAM_WIDTH && y + h <= VRAM_HEIGHT);
    framebuffer_need_update = 1;
    if (GR_CopyGPUVRAMRect(x, y, w, h, dst_x, dst_y)) {
      return;
    }
    // Compatibility fallback for non-desktop renderers or failed FBO setup.
    GR_SynchronizeGPUVRAMRect(x, y, w, h);
  }
  GR_ClearGPUVRAMDirty(dst_x, dst_y, w, h);
  GR_MarkVRAMDirty(dst_x, dst_y, w, h);

  int stride = w;
  const int sourceY = y;
  GR_RecordVRAMWrite(internalCopy ? GR_VRAM_WRITE_MOVE : GR_VRAM_WRITE_UPLOAD,
                     x, y, dst_x, dst_y, w, h);

  if (internalCopy) {
    src = vram;
    stride = VRAM_WIDTH;
  }

  if (internalCopy && dst_y > sourceY && dst_y < sourceY + h) {
    for (int row = h - 1; row >= 0; --row) {
      SDL_memmove(vram + dst_x + (dst_y + row) * VRAM_WIDTH,
                  vram + x + (sourceY + row) * VRAM_WIDTH,
                  w * sizeof(unsigned short));
    }
  } else {
    src += x + y * stride;
    unsigned short *dst = vram + dst_x + dst_y * VRAM_WIDTH;
    for (int row = 0; row < h; ++row) {
      SDL_memmove(dst, src, w * sizeof(unsigned short));
      dst += VRAM_WIDTH;
      src += stride;
    }
  }
}

void GR_ReadVRAM(unsigned short *dst, int x, int y, int dst_w, int dst_h) {
  GR_SynchronizeGPUVRAMRect(x, y, dst_w, dst_h);
  unsigned short *src = vram + x + VRAM_WIDTH * y;

  for (int i = 0; i < dst_h; i++) {
    SDL_memcpy(dst, src, dst_w * sizeof(short));
    dst += dst_w;
    src += VRAM_WIDTH;
  }
}

void GR_UploadVRAMAliasPage(int page, const unsigned short *src) {
  if (page < 0 || page >= VRAM_ALIAS_PAGE_COUNT || src == NULL)
    return;
  const unsigned short *upload = src;
  const int pageX = (page & 15) * 64;
  const int pageY = (page >> 4) * 256;
  unsigned short *destination =
      g_vramAliasPages + pageX + pageY * VRAM_ALIAS_WIDTH;
  for (int row = 0; row < 256; ++row) {
    SDL_memcpy(destination, src, 64 * sizeof(unsigned short));
    destination += VRAM_ALIAS_WIDTH;
    src += 64;
  }
#if USE_OPENGL
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, g_vramAliasTexture);
  glTexSubImage2D(GL_TEXTURE_2D, 0, pageX, pageY, 64, 256, VRAM_FORMAT,
                  GL_UNSIGNED_BYTE, upload);
  glActiveTexture(GL_TEXTURE0);
  g_lastBoundTexture = 0;
#endif
}

void GR_ReadVRAMAliasPage(int page, unsigned short *dst) {
  if (page < 0 || page >= VRAM_ALIAS_PAGE_COUNT || dst == NULL)
    return;
  const int pageX = (page & 15) * 64;
  const int pageY = (page >> 4) * 256;
  const unsigned short *source =
      g_vramAliasPages + pageX + pageY * VRAM_ALIAS_WIDTH;
  for (int row = 0; row < 256; ++row) {
    SDL_memcpy(dst, source, 64 * sizeof(unsigned short));
    dst += 64;
    source += VRAM_ALIAS_WIDTH;
  }
}

void GR_UpdateVRAM() {
  if (!vram_need_update)
    return;

  vram_need_update = 0;

#if USE_OPENGL
  glActiveTexture(GL_TEXTURE0);
  for (int textureIndex = 0; textureIndex < 2; ++textureIndex) {
    glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[textureIndex]);
    GrVRAMDirtyRows &dirty = g_vramDirtyRows[textureIndex];

#if defined(RENDERER_OGL) || (defined(RENDERER_OGLES) && OGLES_VERSION >= 3)
    // Each alternating object owns an exact per-pixel dirty journal. Distinct
    // CPU writes on one row must never upload the GPU-authoritative gap between
    // them. Identical run layouts on consecutive rows are still coalesced.
    glPixelStorei(GL_UNPACK_ROW_LENGTH, VRAM_WIDTH);
    std::vector<std::array<int, 2>> active_runs;
    std::vector<std::array<int, 2>> row_runs;
    active_runs.reserve(16);
    row_runs.reserve(16);
    int active_first_row = 0;
    for (int row = 0; row <= VRAM_HEIGHT; ++row) {
      if (row < VRAM_HEIGHT)
        GR_CollectVRAMDirtyRuns(dirty, row, row_runs);
      else
        row_runs.clear();
      if (row_runs == active_runs)
        continue;

      const int row_count = row - active_first_row;
      for (const auto &run : active_runs) {
        const int x0 = run[0];
        const int x1 = run[1];
        glTexSubImage2D(GL_TEXTURE_2D, 0, x0, active_first_row, x1 - x0,
                        row_count, VRAM_FORMAT, GL_UNSIGNED_BYTE,
                        vram + active_first_row * VRAM_WIDTH + x0);
        GR_ClearVRAMDirtyRect(dirty, x0, active_first_row, x1 - x0, row_count);
      }
      active_runs = row_runs;
      active_first_row = row;
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#else
    // OpenGL ES 2 has no row-length unpack state. Preserve the complete upload
    // there; desktop and ES3 use the batched dirty path above.
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VRAM_WIDTH, VRAM_HEIGHT,
                    VRAM_FORMAT, GL_UNSIGNED_BYTE, vram);
    for (auto &row : dirty.rows)
      row.fill(0U);
#endif
  }
  g_vramTexture = g_vramTexturesDouble[0];
  g_vramTextureIdx = 0;
  // GR_UpdateVRAM changes the GL binding behind GR_SetTexture's cache. Force
  // the next split to bind its exact texture and restore the LUT sampler.
  g_lastBoundTexture = 0;

#endif
}

void GR_SwapWindow() {
  PsyX_CommitDrawableSize();
#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
  PsyX_PresentNativeFramebuffer();
  SDL_GL_SwapWindow(g_window);
#endif

  // glFinish();
}

void GR_SetDepthState(int testEnable, int writeEnable) {
  const int appliedTest = testEnable && g_cfg_pgxpZBuffer ? 1 : 0;
  const int appliedWrite = appliedTest && writeEnable ? 1 : 0;

#if USE_OPENGL
  if (appliedTest && g_PreviousDepthFunc != GL_GEQUAL) {
    g_PreviousDepthFunc = GL_GEQUAL;
    glDepthFunc(GL_GEQUAL);
  }
  if (g_PreviousDepthMode != appliedTest) {
    g_PreviousDepthMode = appliedTest;
    if (appliedTest)
      glEnable(GL_DEPTH_TEST);
    else
      glDisable(GL_DEPTH_TEST);
  }
  if (g_PreviousDepthWrite != appliedWrite) {
    g_PreviousDepthWrite = appliedWrite;
    glDepthMask(appliedWrite ? GL_TRUE : GL_FALSE);
  }
#endif
}
void GR_SetDepthRange(float lower, float upper) {
  lower = std::max(0.0f, std::min(lower, 1.0f));
  upper = std::max(0.0f, std::min(upper, 1.0f));
  if (g_PreviousDepthRangeLower == lower && g_PreviousDepthRangeUpper == upper)
    return;

#if USE_OPENGL
#ifdef RENDERER_OGLES
  glDepthRangef(lower, upper);
#else
  glDepthRange(static_cast<GLdouble>(lower), static_cast<GLdouble>(upper));
#endif
#endif
  g_PreviousDepthRangeLower = lower;
  g_PreviousDepthRangeUpper = upper;
}
unsigned long long GR_GetDepthClearSerial(int offscreen) {
  return offscreen ? g_offscreenDepthClearSerial : g_nativeDepthClearSerial;
}
void GR_ClearDepthBuffer(void) {
#if USE_OPENGL
  // glClear obeys the depth write mask. Preserve the caller's write state so a
  // transparent run stays test-only after an OT depth discontinuity.
  const int restoreWrite = g_PreviousDepthWrite;
  const int restoreScissor = g_PreviousScissorState;
  GR_SetScissorState(0);
  glDepthMask(GL_TRUE);
  GR_SetDepthRange(0.0f, 1.0f);
#ifdef RENDERER_OGLES
  glClearDepthf(0.0f);
#else
  glClearDepth(0.0f);
#endif
  glClear(GL_DEPTH_BUFFER_BIT);
  if (restoreWrite == 0)
    glDepthMask(GL_FALSE);
  if (g_PreviousOffscreenState)
    ++g_offscreenDepthClearSerial;
  else
    ++g_nativeDepthClearSerial;
  if (restoreScissor)
    GR_SetScissorState(1);
#endif
}

void GR_EnableDepth(int enable) {
  g_RequestedDepthMode = enable ? 1 : 0;
  GR_SetDepthState(enable, enable);
}

void GR_EnableStencil(int enable) {
#if USE_OPENGL
  if (enable)
    glEnable(GL_STENCIL_TEST);
  else
    glDisable(GL_STENCIL_TEST);
#else
  (void)enable;
#endif
}

void GR_SetStencilMode(int drawPrim) {
  if (g_ShadowStencilPhase != 0)
    return;

  if (g_PreviousStencilMode == drawPrim)
    return;

  g_PreviousStencilMode = drawPrim;

#if USE_OPENGL
  if (drawPrim) {
    glStencilFunc(GL_ALWAYS, 1, 0x10);
    glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
  } else {
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
  }
#endif
}

void GR_BeginShadowMask(void) {
  g_ShadowStencilPhase = 1;
#if USE_OPENGL
  // Preserve the retail mask bit (0x01) and reserve bit 0x02 for the native
  // shadow gate. glClear obeys the stencil write mask, so this never erases
  // guest-authored PSX mask state from the opaque world pass.
  glStencilMask(0x02);
  glClearStencil(0);
  glClear(GL_STENCIL_BUFFER_BIT);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  // Accept only unmasked, not-yet-shaded pixels. The first visible shadow
  // fragment flips bit 0x02; overlapping triangles then fail this test.
  glStencilFunc(GL_EQUAL, 0x00, 0x03);
  glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
#endif
}

void GR_EndShadowMask(void) {
#if USE_OPENGL
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  // Remove any mask fragment which failed the shade depth test while retaining
  // the retail PSX bit, then restore the normal mask-test contract.
  glStencilMask(0x02);
  glClearStencil(0);
  glClear(GL_STENCIL_BUFFER_BIT);
  glStencilMask(0xFF);
  glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
  glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
#endif
  g_ShadowStencilPhase = 0;
  g_PreviousStencilMode = 0;
}

void GR_SetBlendModeForPrimitive(BlendMode blendMode, int untextured) {
  untextured = untextured != 0;
  if (g_PreviousBlendMode == blendMode &&
      (blendMode != BM_AVERAGE || g_PreviousBlendUntextured == untextured)) {
    return;
  }

#if USE_OPENGL
  if (blendMode == BM_NONE) {
    if (g_PreviousBlendMode != BM_NONE) {
      glBlendColor(1.f, 1.f, 1.f, 1.f);
      glDisable(GL_BLEND);
    }
  } else {
    if (g_PreviousBlendMode == BM_NONE) {
      glBlendColor(0.25f, 0.25f, 0.25f, 0.5f);
      glEnable(GL_BLEND);
    }

    glBlendEquationSeparate(blendMode == BM_SUBTRACT ? GL_FUNC_REVERSE_SUBTRACT
                                                     : GL_FUNC_ADD,
                            GL_FUNC_ADD);
    switch (blendMode) {
    case BM_AVERAGE:
      glBlendFuncSeparate(untextured ? GL_CONSTANT_ALPHA : GL_SRC_ALPHA,
                          untextured ? GL_ONE_MINUS_CONSTANT_ALPHA
                                     : GL_ONE_MINUS_SRC_ALPHA,
                          GL_ONE, GL_ZERO);
      break;
    case BM_ADD:
    case BM_SUBTRACT:
      glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
      break;
    case BM_ADD_QUATER_SOURCE:
      glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_ONE, GL_ONE, GL_ZERO);
      break;
    }
    // RGB follows the selected PS1 equation. Alpha carries the source STP bit,
    // so it must replace destination alpha instead of participating in that
    // equation and saturating after repeated framebuffer feedback.
  }
#endif

  g_PreviousBlendMode = blendMode;
  g_PreviousBlendUntextured = untextured;
}

void GR_SetBlendMode(BlendMode blendMode) {
  GR_SetBlendModeForPrimitive(blendMode, 0);
}

void GR_SetPolygonOffset(float slope, float units) {
  // GPU split submission restores the generic polygon state for every split.
  // Keep the receiver bias selected by the two-pass shadow operation until
  // that operation ends; otherwise coplanar wall portions flicker per split.
  if (g_ShadowStencilPhase != 0)
    return;

#if USE_OPENGL
  if (g_PreviousPolygonOffsetSlope == slope &&
      g_PreviousPolygonOffsetUnits == units)
    return;
  g_PreviousPolygonOffsetSlope = slope;
  g_PreviousPolygonOffsetUnits = units;
  if (slope == 0.0f && units == 0.0f) {
    glDisable(GL_POLYGON_OFFSET_FILL);
  } else {
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(slope, units);
  }
#endif
}

void GR_SetViewPort(int x, int y, int width, int height) {
#if USE_OPENGL
  glViewport(x, y, width, height);
#endif
}

void GR_SetWireframe(int enable) {
#if defined(RENDERER_OGL)
  glPolygonMode(GL_FRONT_AND_BACK, enable ? GL_LINE : GL_FILL);
#endif
}

void GR_BindVertexBuffer() {
#if USE_OPENGL
  glBindVertexArray(g_glVertexArray[g_curVertexBuffer]);
  // GL_ARRAY_BUFFER is not VAO binding state. Select the matching ring buffer
  // for the upload; its immutable attribute layout already belongs to the VAO.
  glBindBuffer(GL_ARRAY_BUFFER, g_glVertexBuffer[g_curVertexBuffer]);

  g_curVertexBuffer = (g_curVertexBuffer + 1) % MAX_NUM_VERTEX_BUFFERS;
#else
#error
#endif
}

void GR_UpdateVertexBuffer(const GrVertex *vertices, int num_vertices) {
  if (num_vertices > MAX_VERTEX_BUFFER_SIZE) {
    eprinterr("MAX_VERTEX_BUFFER_SIZE reached, expect rendering errors\n");
    num_vertices = MAX_VERTEX_BUFFER_SIZE;
  }

  // assert(num_vertices <= MAX_VERTEX_BUFFER_SIZE);
  GR_BindVertexBuffer();

#if USE_OPENGL
  // One orphaning upload avoids both an in-flight wait and a second driver
  // copy. Allocate only the submitted range; HUD DrawSync calls are small and
  // must not repeatedly reserve the full world-sized stream buffer.
  glBufferData(GL_ARRAY_BUFFER, num_vertices * sizeof(GrVertex), vertices,
               GL_STREAM_DRAW);
#else
#error
#endif
}

void GR_DrawTriangles(int start_vertex, int triangles) {
#if USE_OPENGL
  glDrawArrays(GL_TRIANGLES, start_vertex, triangles * 3);
#else
#error
#endif
}

void GR_PushDebugLabel(const char *label) {
#if USE_OPENGL && !defined(__EMSCRIPTEN__) &&                                  \
    defined(GL_DEBUG_SOURCE_APPLICATION)
  if (!glPushDebugGroup)
    return;
  glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0x8000, strlen(label), label);
#endif
}

void GR_PopDebugLabel() {
#if USE_OPENGL && !defined(__EMSCRIPTEN__) &&                                  \
    defined(GL_DEBUG_SOURCE_APPLICATION)
  if (!glPopDebugGroup)
    return;
  glPopDebugGroup();
#endif
}
