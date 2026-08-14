#include "PsyX_GPU.h"

#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_render.h"

#include "../PsyX_main.h"

#include <algorithm>
#include <array>
#include <assert.h>
#include <cmath>
#include <math.h>
#include <string.h>

#define GET_TPAGE_FORMAT(tpage) ((TexFormat)((tpage >> 7) & 0x3))
#define GET_TPAGE_BLEND(tpage) ((BlendMode)(((tpage >> 5) & 3) + 1))

#define GET_TPAGE_DITHER(tpage) ((tpage >> 9) & 0x1)

#define GET_CLUT_X(clut) ((clut & 0x3F) << 4)
#define GET_CLUT_Y(clut) (clut >> 6)

#if USE_EXTENDED_PRIM_POINTERS
OT_TAG prim_terminator = {UINTPTR_MAX, 0}; // P_TAG with zero primLength
#else
OT_TAG prim_terminator = {0x00FFFFFFu, 0}; // P_TAG with zero primLength
#endif

DISPENV currentDispEnv;
DISPENV activeDispEnv;
DRAWENV activeDrawEnv;

static DISPENV g_guestProjectionDispEnv;
static unsigned long long g_guestProjectionEpoch;
static int g_guestProjectionDispEnvValid;

static const DISPENV &ProjectionDispEnv() {
  return g_guestProjectionDispEnvValid ? g_guestProjectionDispEnv
                                       : activeDispEnv;
}

static bool SameProjectionDispEnv(const DISPENV &left, const DISPENV &right) {
  return left.disp.x == right.disp.x && left.disp.y == right.disp.y &&
         left.disp.w == right.disp.w && left.disp.h == right.disp.h &&
         left.screen.x == right.screen.x && left.screen.y == right.screen.y &&
         left.screen.w == right.screen.w && left.screen.h == right.screen.h &&
         left.isinter == right.isinter && left.isrgb24 == right.isrgb24;
}

void GR_BeginGuestProjectionEpoch(unsigned long long epoch, int width,
                                  int height, int rgb24, int interlaced) {
  g_guestProjectionDispEnv = activeDispEnv;
  g_guestProjectionDispEnv.disp.x = 0;
  g_guestProjectionDispEnv.disp.y = 0;
  g_guestProjectionDispEnv.disp.w = std::max(width, 1);
  g_guestProjectionDispEnv.disp.h = std::max(height, 1);
  g_guestProjectionDispEnv.screen = {0, 0, 0, 0};
  g_guestProjectionDispEnv.isrgb24 = rgb24 ? 1 : 0;
  g_guestProjectionDispEnv.isinter = interlaced ? 1 : 0;
  g_guestProjectionEpoch = epoch;
  g_guestProjectionDispEnvValid = 1;
}

static const char *currentSplitDebugText = nullptr;
TextureID overrideTexture = 0;
int overrideTextureWidth = 0;
int overrideTextureHeight = 0;
extern int g_RequestedDepthMode;

int g_GPUDisabledState = 0;
int g_DrawPrimMode = 0;

struct GPUDrawSplit {
  DRAWENV drawenv;
  DISPENV dispenv;
  unsigned long long projectionEpoch;

  BlendMode blendMode;

  TexFormat texFormat;
  TextureID textureId;
  TextureFilterMode textureFilterMode;

  int drawPrimMode;

  u_short startVertex;
  u_short numVerts;

  const char *debugText;
};

#define MAX_DRAW_SPLITS 4096
// The largest supported PS1 packet expands to three independent line quads.
// Flush before parsing it so packet expansion can never write past the fixed
// CPU stream buffer. The flush preserves OT order and starts a fresh batch.
#define MAX_VERTICES_PER_PRIMITIVE 18

GrVertex g_vertexBuffer[MAX_VERTEX_BUFFER_SIZE];
GPUDrawSplit g_splits[MAX_DRAW_SPLITS];

int g_vertexIndex = 0;
int g_splitIndex = 0;

void ClearSplits() {
  currentSplitDebugText = nullptr;
  g_vertexIndex = 0;
  g_splitIndex = 0;
  g_splits[0].texFormat = (TexFormat)0xFFFF;
}

template <class T> void DrawEnvDimensions(T &width, T &height) {
  if (activeDrawEnv.dfe) {
    width = ProjectionDispEnv().disp.w;
    height = ProjectionDispEnv().disp.h;
  } else {
    width = activeDrawEnv.clip.w;
    height = activeDrawEnv.clip.h;
  }
}

void DrawEnvOffset(float &ofsX, float &ofsY) {
  const int targetX =
      activeDrawEnv.dfe ? ProjectionDispEnv().disp.x : activeDrawEnv.clip.x;
  const int targetY =
      activeDrawEnv.dfe ? ProjectionDispEnv().disp.y : activeDrawEnv.clip.y;
  ofsX = activeDrawEnv.ofs[0] - targetX;
  ofsY = activeDrawEnv.ofs[1] - targetY;
}

// remaps screen coordinates to [0..1]
// without clamping
inline void ScreenCoordsToEmulator(GrVertex *vertex, int count) {
#if USE_PGXP
  float w, h;
  DrawEnvDimensions(w, h);

  while (count--) {
    // Precise PGXP vertices are normalized when the exact GTE projection is
    // applied. Legacy screen-space vertices still need conversion here.
    if (vertex[count].scr_h == 0.0f) {
      vertex[count].x = vertex[count].x / w - 0.5f;
      vertex[count].y = vertex[count].y / h - 0.5f;
    }
  }
#endif
}

void LineSwapSourceVerts(VERTTYPE *&p0, VERTTYPE *&p1, unsigned char *&c0,
                         unsigned char *&c1) {
  // swap line coordinates for left-to-right and up-to-bottom direction
  if ((p0[0] > p1[0]) || (p0[1] > p1[1] && p0[0] == p1[0])) {
    VERTTYPE *tmp = p0;
    p0 = p1;
    p1 = tmp;

    unsigned char *tmpCol = c0;
    c0 = c1;
    c1 = tmpCol;
  }
}

void MakeLineArray(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1,
                   ushort gteidx) {
  const VERTTYPE dx = p1[0] - p0[0];
  const VERTTYPE dy = p1[1] - p0[1];

  float ofsX, ofsY;
  DrawEnvOffset(ofsX, ofsY);

  memset(vertex, 0, sizeof(GrVertex) * 4);

  if (dx > abs((short)dy)) { // horizontal
    vertex[0].x = p0[0] + ofsX;
    vertex[0].y = p0[1] + ofsY;

    vertex[1].x = p1[0] + ofsX + 1;
    vertex[1].y = p1[1] + ofsY;

    vertex[2].x = vertex[1].x;
    vertex[2].y = vertex[1].y + 1;

    vertex[3].x = vertex[0].x;
    vertex[3].y = vertex[0].y + 1;
  } else { // vertical
    vertex[0].x = p0[0] + ofsX;
    vertex[0].y = p0[1] + ofsY;

    vertex[1].x = p1[0] + ofsX;
    vertex[1].y = p1[1] + ofsY + 1;

    vertex[2].x = vertex[1].x + 1;
    vertex[2].y = vertex[1].y;

    vertex[3].x = vertex[0].x + 1;
    vertex[3].y = vertex[0].y;
  } // TODO diagonal line alignment

#if USE_PGXP
  vertex[0].scr_h = vertex[1].scr_h = vertex[2].scr_h = vertex[3].scr_h = 0.0f;
#endif

  ScreenCoordsToEmulator(vertex, 4);
}

inline bool LookupVertexPGXP(PGXPVData *result, const VERTTYPE *p,
                             ushort gteidx, int lookupOfs) {
#if USE_PGXP
  const uint lookup = PGXP_LOOKUP_VALUE(p[0], p[1]);
  const int cacheIndex = static_cast<int>(gteidx) + lookupOfs;
  if (result == nullptr || gteidx == 0xffff || cacheIndex < 0 ||
      cacheIndex >= 0xffff || !g_cfg_pgxpTextureCorrection ||
      !PGXP_GetCacheDataExact(result, static_cast<ushort>(cacheIndex))) {
    return false;
  }
  const bool lookupMatches = result->lookup == lookup;
  const bool perspectiveValid =
      lookupMatches && std::isfinite(result->px) && std::isfinite(result->py) &&
      std::isfinite(result->pz) && std::isfinite(result->scr_h) &&
      result->scr_h > 0.0f &&
      (result->pz > 0.0f || result->exact_projection != 0U);
  const bool preciseScreenValid = result->precise_screen_position != 0U &&
                                  lookupMatches && std::isfinite(result->sx) &&
                                  std::isfinite(result->sy);
  return perspectiveValid || preciseScreenValid;
#else
  (void)result;
  (void)p;
  (void)gteidx;
  (void)lookupOfs;
  return false;
#endif
}

inline bool HasPerspectivePGXP(const PGXPVData &vd) {
#if USE_PGXP
  return std::isfinite(vd.px) && std::isfinite(vd.py) && std::isfinite(vd.pz) &&
         std::isfinite(vd.scr_h) && vd.scr_h > 0.0f &&
         (vd.pz > 0.0f || vd.exact_projection != 0U);
#else
  (void)vd;
  return false;
#endif
}

inline void ApplyVertexPGXP(GrVertex *v, const PGXPVData &vd, float ofsX,
                            float ofsY, bool usePerspective) {
#if USE_PGXP
  if (usePerspective) {
    float dispW, dispH;
    DrawEnvDimensions(dispW, dispH);

    // Preserve the exact unsaturated projection which generated the packet.
    // Only the framebuffer-page part of OFX/OFY is wrapped to the display.
    // C fmod keeps the dividend sign; negative camera offsets therefore used
    // to jump one complete display page as the camera crossed zero.
    const auto positiveModulo = [](float value, float extent) {
      const float remainder = fmodf(value, extent);
      return remainder < 0.0f ? remainder + extent : remainder;
    };
    const float centerX = positiveModulo(vd.ofx, dispW);
    const float centerY = positiveModulo(vd.ofy, dispH);
    const float targetCenterX = centerX + ofsX;
    const float targetCenterY = centerY + ofsY;
    v->z = vd.pz;
    v->scr_h = vd.scr_h;
    if (vd.exact_projection) {
      // Keep the camera anchor separate from the view-space numerator. The
      // primitive clipper can reconstruct one shared near-plane edge before
      // the shader receives a strictly positive W.
      v->x = targetCenterX / dispW - 0.5f;
      v->y = targetCenterY / dispH - 0.5f;
      v->clip_x = vd.px * vd.scr_h / dispW;
      v->clip_y = vd.py * vd.scr_h / dispH;
    } else {
      const float screenX = vd.sx - vd.ofx + targetCenterX;
      const float screenY = vd.sy - vd.ofy + targetCenterY;
      v->x = screenX / dispW - 0.5f;
      v->y = screenY / dispH - 0.5f;
      v->clip_x = v->clip_y = 0.0f;
    }
  } else if (vd.precise_screen_position != 0U) {
    v->x = vd.sx + ofsX;
    v->y = vd.sy + ofsY;
  }

  if (vd.precise_texcoord) {
    v->precise_u = vd.precise_u;
    v->precise_v = vd.precise_v;
    v->umin = static_cast<unsigned char>(vd.texture_bounds);
    v->vmin = static_cast<unsigned char>(vd.texture_bounds >> 8);
    v->umax = static_cast<unsigned char>(vd.texture_bounds >> 16);
    v->vmax = static_cast<unsigned char>(vd.texture_bounds >> 24);
    v->_p1 = 1;
  }
#else
  (void)v;
  (void)vd;
  (void)ofsX;
  (void)ofsY;
  (void)usePerspective;
#endif
}

void MakeVertexTriangle(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1,
                        VERTTYPE *p2, ushort gteidx) {
  assert(p0);
  assert(p1);
  assert(p2);

  float ofsX, ofsY;
  DrawEnvOffset(ofsX, ofsY);

  memset(vertex, 0, sizeof(GrVertex) * 3);

  vertex[0].x = p0[0] + ofsX;
  vertex[0].y = p0[1] + ofsY;

  vertex[1].x = p1[0] + ofsX;
  vertex[1].y = p1[1] + ofsY;

  vertex[2].x = p2[0] + ofsX;
  vertex[2].y = p2[1] + ofsY;

  // Perspective correction is primitive-atomic. Mixing one legacy W=1
  // vertex with precise depth vertices produces catastrophic texture warping.
  PGXPVData precise[3]{};
  const bool found[3]{LookupVertexPGXP(&precise[0], p0, gteidx, -3),
                      LookupVertexPGXP(&precise[1], p1, gteidx, -2),
                      LookupVertexPGXP(&precise[2], p2, gteidx, -1)};
  const bool usePerspective =
      found[0] && found[1] && found[2] && HasPerspectivePGXP(precise[0]) &&
      HasPerspectivePGXP(precise[1]) && HasPerspectivePGXP(precise[2]);
  const bool usePreciseScreen = found[0] && found[1] && found[2];
  if (usePreciseScreen) {
    for (int index = 0; index < 3; ++index) {
      ApplyVertexPGXP(&vertex[index], precise[index], ofsX, ofsY,
                      usePerspective);
    }
  }

  ScreenCoordsToEmulator(vertex, 3);
}

void MakeVertexQuad(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1, VERTTYPE *p2,
                    VERTTYPE *p3, ushort gteidx) {
  assert(p0);
  assert(p1);
  assert(p2);
  assert(p3);

  float ofsX, ofsY;
  DrawEnvOffset(ofsX, ofsY);

  memset(vertex, 0, sizeof(GrVertex) * 4);

  vertex[0].x = p0[0] + ofsX;
  vertex[0].y = p0[1] + ofsY;

  vertex[1].x = p1[0] + ofsX;
  vertex[1].y = p1[1] + ofsY;

  vertex[2].x = p2[0] + ofsX;
  vertex[2].y = p2[1] + ofsY;

  vertex[3].x = p3[0] + ofsX;
  vertex[3].y = p3[1] + ofsY;

  // Primitive parsing swaps packet x2/x3 into TL,TR,BR,BL order. Match the
  // exact GTE cache entries to those packet vertices, not to the local slots.
  PGXPVData precise[4]{};
  const bool found[4]{LookupVertexPGXP(&precise[0], p0, gteidx, -4),
                      LookupVertexPGXP(&precise[1], p1, gteidx, -3),
                      LookupVertexPGXP(&precise[2], p2, gteidx, -1),
                      LookupVertexPGXP(&precise[3], p3, gteidx, -2)};
  const bool usePerspective =
      found[0] && found[1] && found[2] && found[3] &&
      HasPerspectivePGXP(precise[0]) && HasPerspectivePGXP(precise[1]) &&
      HasPerspectivePGXP(precise[2]) && HasPerspectivePGXP(precise[3]);
  const bool usePreciseScreen = found[0] && found[1] && found[2] && found[3];
  if (usePreciseScreen) {
    for (int index = 0; index < 4; ++index) {
      ApplyVertexPGXP(&vertex[index], precise[index], ofsX, ofsY,
                      usePerspective);
    }
  }

  ScreenCoordsToEmulator(vertex, 4);
}

void MakeVertexRect(GrVertex *vertex, VERTTYPE *p0, short w, short h,
                    ushort gteidx) {
  assert(p0);

  float ofsX, ofsY;
  DrawEnvOffset(ofsX, ofsY);

  memset(vertex, 0, sizeof(GrVertex) * 4);

  vertex[0].x = p0[0] + ofsX;
  vertex[0].y = p0[1] + ofsY;

  vertex[1].x = vertex[0].x;
  vertex[1].y = vertex[0].y + h;

  vertex[2].x = vertex[0].x + w;
  vertex[2].y = vertex[0].y + h;

  vertex[3].x = vertex[0].x + w;
  vertex[3].y = vertex[0].y;

#if USE_PGXP
  vertex[0].scr_h = vertex[1].scr_h = vertex[2].scr_h = vertex[3].scr_h = 0.0f;
#endif

  ScreenCoordsToEmulator(vertex, 4);
}

static void SetTexcoordFilteringState(GrVertex *vertex, int count,
                                      int exclusiveFarEdge) {
  // World UVs address inclusive texel centres and need no shift. Native
  // sprites/HUD use u+width/v+height as an exclusive far edge, so only that
  // convention receives the half-texel correction. Bounds keep every tap in
  // the primitive's authored atlas tile.
  const bool filtered = g_cfg_bilinearFiltering || g_cfg_trilinearFiltering ||
                        g_cfg_anisotropicFiltering;
  const char offset = filtered && exclusiveFarEdge ? -1 : 0;
  unsigned char minU = 255;
  unsigned char minV = 255;
  unsigned char maxU = 0;
  unsigned char maxV = 0;
  bool hasPreciseTexcoords = false;
  for (int i = 0; i < count; i++) {
    if (vertex[i]._p1)
      hasPreciseTexcoords = true;
    else {
      vertex[i].precise_u = static_cast<float>(vertex[i].u);
      vertex[i].precise_v = static_cast<float>(vertex[i].v);
    }
    if (vertex[i].u < minU)
      minU = vertex[i].u;
    if (vertex[i].v < minV)
      minV = vertex[i].v;
    if (vertex[i].u > maxU)
      maxU = vertex[i].u;
    if (vertex[i].v > maxV)
      maxV = vertex[i].v;
  }
  if (hasPreciseTexcoords) {
    minU = 255;
    minV = 255;
    maxU = 0;
    maxV = 0;
    for (int i = 0; i < count; ++i) {
      if (vertex[i].umin < minU)
        minU = vertex[i].umin;
      if (vertex[i].vmin < minV)
        minV = vertex[i].vmin;
      if (vertex[i].umax > maxU)
        maxU = vertex[i].umax;
      if (vertex[i].vmax > maxV)
        maxV = vertex[i].vmax;
    }
  }
  if (exclusiveFarEdge) {
    if (maxU > minU)
      --maxU;
    if (maxV > minV)
      --maxV;
  }
  for (int i = 0; i < count; i++) {
    vertex[i].tcx = offset;
    vertex[i].tcy = offset;
    vertex[i].umin = minU;
    vertex[i].vmin = minV;
    vertex[i].umax = maxU;
    vertex[i].vmax = maxV;
  }
}

void MakeTexcoordQuad(GrVertex *vertex, unsigned char *uv0, unsigned char *uv1,
                      unsigned char *uv2, unsigned char *uv3, u_short page,
                      u_short clut, unsigned char dither) {
  assert(uv0);
  assert(uv1);
  assert(uv2);
  assert(uv3);

  const unsigned char bright = 2;

  vertex[0].u = uv0[0];
  vertex[0].v = uv0[1];
  vertex[0].bright = bright;
  vertex[0].dither = dither;
  vertex[0].page = page;
  vertex[0].clut = clut;

  vertex[1].u = uv1[0];
  vertex[1].v = uv1[1];
  vertex[1].bright = bright;
  vertex[1].dither = dither;
  vertex[1].page = page;
  vertex[1].clut = clut;

  vertex[2].u = uv2[0];
  vertex[2].v = uv2[1];
  vertex[2].bright = bright;
  vertex[2].dither = dither;
  vertex[2].page = page;
  vertex[2].clut = clut;

  vertex[3].u = uv3[0];
  vertex[3].v = uv3[1];
  vertex[3].bright = bright;
  vertex[3].dither = dither;
  vertex[3].page = page;
  vertex[3].clut = clut;
  // Native screen sprites/HUD in this backend use u+width/v+height as an
  // exclusive far edge. GTE-projected world quads retain authored inclusive
  // corner UVs. Preserve both conventions while deriving atlas bounds.
  int exclusiveFarEdge = 0;
#if USE_PGXP
  exclusiveFarEdge = vertex[0].scr_h == 0.0f && vertex[1].scr_h == 0.0f &&
                     vertex[2].scr_h == 0.0f && vertex[3].scr_h == 0.0f &&
                     vertex[0].y == vertex[1].y && vertex[1].x == vertex[2].x &&
                     vertex[2].y == vertex[3].y && vertex[3].x == vertex[0].x;
#endif
  SetTexcoordFilteringState(vertex, 4, exclusiveFarEdge);
}

void MakeTexcoordTriangle(GrVertex *vertex, unsigned char *uv0,
                          unsigned char *uv1, unsigned char *uv2, u_short page,
                          u_short clut, unsigned char dither) {
  assert(uv0);
  assert(uv1);
  assert(uv2);

  const unsigned char bright = 2;

  vertex[0].u = uv0[0];
  vertex[0].v = uv0[1];
  vertex[0].bright = bright;
  vertex[0].dither = dither;
  vertex[0].page = page;
  vertex[0].clut = clut;

  vertex[1].u = uv1[0];
  vertex[1].v = uv1[1];
  vertex[1].bright = bright;
  vertex[1].dither = dither;
  vertex[1].page = page;
  vertex[1].clut = clut;

  vertex[2].u = uv2[0];
  vertex[2].v = uv2[1];
  vertex[2].bright = bright;
  vertex[2].dither = dither;
  vertex[2].page = page;
  vertex[2].clut = clut;
  SetTexcoordFilteringState(vertex, 3, 0);
}

void MakeTexcoordRect(GrVertex *vertex, unsigned char *uv, u_short page,
                      u_short clut, short w, short h) {
  assert(uv);

  // Native sprites use an exclusive far edge. 256 is therefore a valid
  // endpoint for the last texel of a TPAGE; clamping it to 255 shrinks the UV
  // span while leaving the rectangle geometry unchanged. Retained x2 pages
  // and the ordinary VRAM scanout then sample different rows/columns.
  const int requestedFarU = int(uv[0]) + int(w);
  const int requestedFarV = int(uv[1]) + int(h);
  // Keep the existing clamp for sprites which actually cross the TPAGE.
  // Hardware-accurate 8-bit wrapping is a separate change; this fix is only
  // for the previously unrepresentable exact exclusive endpoint.
  const int farU = requestedFarU == 256 ? 256 : std::min(requestedFarU, 255);
  const int farV = requestedFarV == 256 ? 256 : std::min(requestedFarV, 255);
  const unsigned char packedFarU =
      static_cast<unsigned char>(std::min(farU, 255));
  const unsigned char packedFarV =
      static_cast<unsigned char>(std::min(farV, 255));

  const unsigned char bright = 2;
  const unsigned char dither = 0;

  vertex[0].u = uv[0];
  vertex[0].v = uv[1];
  vertex[0].bright = bright;
  vertex[0].dither = dither;
  vertex[0].page = page;
  vertex[0].clut = clut;

  vertex[1].u = uv[0];
  vertex[1].v = packedFarV;
  vertex[1].bright = bright;
  vertex[1].dither = dither;
  vertex[1].page = page;
  vertex[1].clut = clut;

  vertex[2].u = packedFarU;
  vertex[2].v = packedFarV;
  vertex[2].bright = bright;
  vertex[2].dither = dither;
  vertex[2].page = page;
  vertex[2].clut = clut;

  vertex[3].u = packedFarU;
  vertex[3].v = uv[1];
  vertex[3].bright = bright;
  vertex[3].dither = dither;
  vertex[3].page = page;
  vertex[3].clut = clut;

  SetTexcoordFilteringState(vertex, 4, 1);
#if USE_PGXP
  // GrVertex keeps the legacy UV in a byte, but the shader consumes the float
  // coordinate. Preserve the exclusive 256 endpoint there and keep filtering
  // bounds inclusive at texel 255.
  if (farU == 256) {
    vertex[2].precise_u = vertex[3].precise_u = 256.0f;
    for (int i = 0; i < 4; ++i) {
      vertex[i].umin = uv[0];
      vertex[i].umax = 255;
    }
  }
  if (farV == 256) {
    vertex[1].precise_v = vertex[2].precise_v = 256.0f;
    for (int i = 0; i < 4; ++i) {
      vertex[i].vmin = uv[1];
      vertex[i].vmax = 255;
    }
  }
#endif
}

void MakeTexcoordLineZero(GrVertex *vertex, unsigned char dither) {
  const unsigned char bright = 1;

  vertex[0].u = 0;
  vertex[0].v = 0;
  vertex[0].bright = bright;
  vertex[0].dither = dither;
  vertex[0].page = 0;
  vertex[0].clut = 0;

  vertex[1].u = 0;
  vertex[1].v = 0;
  vertex[1].bright = bright;
  vertex[1].dither = dither;
  vertex[1].page = 0;
  vertex[1].clut = 0;

  vertex[2].u = 0;
  vertex[2].v = 0;
  vertex[2].bright = bright;
  vertex[2].dither = dither;
  vertex[2].page = 0;
  vertex[2].clut = 0;

  vertex[3].u = 0;
  vertex[3].v = 0;
  vertex[3].bright = bright;
  vertex[3].dither = dither;
  vertex[3].page = 0;
  vertex[3].clut = 0;
}

void MakeTexcoordTriangleZero(GrVertex *vertex, unsigned char dither) {
  const unsigned char bright = 1;

  vertex[0].u = 0;
  vertex[0].v = 0;
  vertex[0].bright = bright;
  vertex[0].dither = dither;
  vertex[0].page = 0;
  vertex[0].clut = 0;

  vertex[1].u = 0;
  vertex[1].v = 0;
  vertex[1].bright = bright;
  vertex[1].dither = dither;
  vertex[1].page = 0;
  vertex[1].clut = 0;

  vertex[2].u = 0;
  vertex[2].v = 0;
  vertex[2].bright = bright;
  vertex[2].dither = dither;
  vertex[2].page = 0;
  vertex[2].clut = 0;
}

void MakeTexcoordQuadZero(GrVertex *vertex, unsigned char dither) {
  const unsigned char bright = 1;

  vertex[0].u = 0;
  vertex[0].v = 0;
  vertex[0].bright = bright;
  vertex[0].dither = dither;
  vertex[0].page = 0;
  vertex[0].clut = 0;

  vertex[1].u = 0;
  vertex[1].v = 0;
  vertex[1].bright = bright;
  vertex[1].dither = dither;
  vertex[1].page = 0;
  vertex[1].clut = 0;

  vertex[2].u = 0;
  vertex[2].v = 0;
  vertex[2].bright = bright;
  vertex[2].dither = dither;
  vertex[2].page = 0;
  vertex[2].clut = 0;

  vertex[3].u = 0;
  vertex[3].v = 0;
  vertex[3].bright = bright;
  vertex[3].dither = dither;
  vertex[3].page = 0;
  vertex[3].clut = 0;

#if USE_PGXP
  // Solid full-display tiles are fades/clears, not 4:3 content. Mark them so
  // the widescreen vertex transform covers the entire output while HUD,
  // sprites and movies remain centred at their authored aspect ratio.
  const bool fullDisplayTile =
      vertex[0].scr_h == 0.0f && vertex[1].scr_h == 0.0f &&
      vertex[2].scr_h == 0.0f && vertex[3].scr_h == 0.0f &&
      vertex[0].x <= -0.499f && vertex[0].y <= -0.499f &&
      vertex[2].x >= 0.499f && vertex[2].y >= 0.499f;
  if (fullDisplayTile)
    vertex[0]._p0 = vertex[1]._p0 = vertex[2]._p0 = vertex[3]._p0 = 1;
#endif
}

void MakeColourNoShade(GrVertex *vertex, int n) {
  --n;
  while (n >= 0) {
    vertex[n].r = 128;
    vertex[n].g = 128;
    vertex[n].b = 128;
    vertex[n].a = 255;
    --n;
  }
}

void MakeColourLine(GrVertex *vertex, bool shadeTexOn, unsigned char *col0,
                    unsigned char *col1) {
  if (!shadeTexOn) {
    MakeColourNoShade(vertex, 4);
    return;
  }
  assert(col0);
  assert(col1);

  vertex[0].r = col0[0];
  vertex[0].g = col0[1];
  vertex[0].b = col0[2];
  vertex[0].a = 255;

  vertex[1].r = col1[0];
  vertex[1].g = col1[1];
  vertex[1].b = col1[2];
  vertex[1].a = 255;

  vertex[2].r = col1[0];
  vertex[2].g = col1[1];
  vertex[2].b = col1[2];
  vertex[2].a = 255;

  vertex[3].r = col0[0];
  vertex[3].g = col0[1];
  vertex[3].b = col0[2];
  vertex[3].a = 255;
}

void MakeColourTriangle(GrVertex *vertex, bool shadeTexOn, unsigned char *col0,
                        unsigned char *col1, unsigned char *col2) {
  if (!shadeTexOn) {
    MakeColourNoShade(vertex, 3);
    return;
  }

  assert(col0);
  assert(col1);
  assert(col2);

  vertex[0].r = col0[0];
  vertex[0].g = col0[1];
  vertex[0].b = col0[2];
  vertex[0].a = 255;

  vertex[1].r = col1[0];
  vertex[1].g = col1[1];
  vertex[1].b = col1[2];
  vertex[1].a = 255;

  vertex[2].r = col2[0];
  vertex[2].g = col2[1];
  vertex[2].b = col2[2];
  vertex[2].a = 255;
}

void MakeColourQuad(GrVertex *vertex, bool shadeTexOn, unsigned char *col0,
                    unsigned char *col1, unsigned char *col2,
                    unsigned char *col3) {
  if (!shadeTexOn) {
    MakeColourNoShade(vertex, 4);
    return;
  }

  assert(col0);
  assert(col1);
  assert(col2);
  assert(col3);

  vertex[0].r = col0[0];
  vertex[0].g = col0[1];
  vertex[0].b = col0[2];
  vertex[0].a = 255;

  vertex[1].r = col1[0];
  vertex[1].g = col1[1];
  vertex[1].b = col1[2];
  vertex[1].a = 255;

  vertex[2].r = col2[0];
  vertex[2].g = col2[1];
  vertex[2].b = col2[2];
  vertex[2].a = 255;

  vertex[3].r = col3[0];
  vertex[3].g = col3[1];
  vertex[3].b = col3[2];
  vertex[3].a = 255;
}

void TriangulateQuad() {
  /*
  Triangulate like this:

  v0--v1
  |  / |
  | /  |
  v2--v3

  NOTE: v2 swapped with v3 during primitive parsing but it not shown here
  */

  g_vertexBuffer[g_vertexIndex + 4] = g_vertexBuffer[g_vertexIndex + 3];

  g_vertexBuffer[g_vertexIndex + 5] = g_vertexBuffer[g_vertexIndex + 2];
  g_vertexBuffer[g_vertexIndex + 2] = g_vertexBuffer[g_vertexIndex + 3];
  g_vertexBuffer[g_vertexIndex + 3] = g_vertexBuffer[g_vertexIndex + 1];
}

static GrVertex NearPlaneIntersection(const GrVertex &left,
                                      const GrVertex &right) {
  const auto *near_vertex = &left;
  const auto *far_vertex = &right;
  if (near_vertex->z > far_vertex->z)
    std::swap(near_vertex, far_vertex);

  const auto denominator = static_cast<double>(far_vertex->z) - near_vertex->z;
  const auto t =
      (static_cast<double>(PGXP_NEAR_PLANE) - near_vertex->z) / denominator;
  const auto blend = [t](float a, float b) {
    return static_cast<float>(static_cast<double>(a) +
                              (static_cast<double>(b) - a) * t);
  };
  const auto blendByte = [t](unsigned char a, unsigned char b) {
    const auto value =
        std::lround(static_cast<double>(a) + (static_cast<double>(b) - a) * t);
    return static_cast<unsigned char>(std::max(0L, std::min(value, 255L)));
  };

  auto result = *near_vertex;
  result.x = blend(near_vertex->x, far_vertex->x);
  result.y = blend(near_vertex->y, far_vertex->y);
  result.z = PGXP_NEAR_PLANE;
  result.scr_h = blend(near_vertex->scr_h, far_vertex->scr_h);
  result.clip_x = blend(near_vertex->clip_x, far_vertex->clip_x);
  result.clip_y = blend(near_vertex->clip_y, far_vertex->clip_y);
  result.u = blendByte(near_vertex->u, far_vertex->u);
  result.v = blendByte(near_vertex->v, far_vertex->v);
  result.r = blendByte(near_vertex->r, far_vertex->r);
  result.g = blendByte(near_vertex->g, far_vertex->g);
  result.b = blendByte(near_vertex->b, far_vertex->b);
  result.a = blendByte(near_vertex->a, far_vertex->a);
  result.precise_u = blend(near_vertex->precise_u, far_vertex->precise_u);
  result.precise_v = blend(near_vertex->precise_v, far_vertex->precise_v);
  return result;
}

static int ClipNearPlaneTriangles(GrVertex *vertices, int count) {
#if USE_PGXP
  auto needs_clipping = false;
  for (auto index = 0; index < count; ++index) {
    const auto &vertex = vertices[index];
    if (vertex.scr_h <= 0.0f || !std::isfinite(vertex.z) ||
        !std::isfinite(vertex.clip_x) || !std::isfinite(vertex.clip_y))
      return count;
    needs_clipping = needs_clipping || vertex.z < PGXP_NEAR_PLANE;
  }
  if (!needs_clipping)
    return count;

  std::array<GrVertex, 12> output{};
  auto output_count = 0;
  for (auto triangle = 0; triangle < count; triangle += 3) {
    std::array<GrVertex, 4> clipped{};
    auto clipped_count = 0;
    auto previous = vertices[triangle + 2];
    auto previous_inside = previous.z >= PGXP_NEAR_PLANE;
    for (auto index = 0; index < 3; ++index) {
      const auto current = vertices[triangle + index];
      const auto current_inside = current.z >= PGXP_NEAR_PLANE;
      if (current_inside != previous_inside)
        clipped[clipped_count++] = NearPlaneIntersection(previous, current);
      if (current_inside)
        clipped[clipped_count++] = current;
      previous = current;
      previous_inside = current_inside;
    }
    for (auto index = 2; index < clipped_count; ++index) {
      output[output_count++] = clipped[0];
      output[output_count++] = clipped[index - 1];
      output[output_count++] = clipped[index];
    }
  }
  std::copy_n(output.begin(), output_count, vertices);
  return output_count;
#else
  return count;
#endif
}

//------------------------------------------------------------------------------------------------------------------------

static void
AddSplit(bool semiTrans, bool textured,
         TextureFilterMode textureFilterMode = TEXTURE_FILTER_NEAREST) {
  int tpage = activeDrawEnv.tpage;
  GPUDrawSplit &curSplit = g_splits[g_splitIndex];
  const DISPENV &projectionDispEnv = ProjectionDispEnv();

  BlendMode blendMode = semiTrans ? GET_TPAGE_BLEND(tpage) : BM_NONE;
  TexFormat texFormat = textured ? GET_TPAGE_FORMAT(tpage) : TF_32_BIT_RGBA;
  TextureID textureId = textured ? g_vramTexture : g_whiteTexture;

  if (textured && overrideTexture != 0) {
    // override texture format, zero tpage
    texFormat = TF_32_BIT_RGBA;
    textureId = overrideTexture;
  }

  // FIXME: compare drawing environment too?
  if (curSplit.blendMode == blendMode && curSplit.texFormat == texFormat &&
      curSplit.textureId == textureId &&
      curSplit.textureFilterMode == textureFilterMode &&
      curSplit.drawPrimMode == g_DrawPrimMode &&
      curSplit.drawenv.clip.x == activeDrawEnv.clip.x &&
      curSplit.drawenv.clip.y == activeDrawEnv.clip.y &&
      curSplit.drawenv.clip.w == activeDrawEnv.clip.w &&
      curSplit.drawenv.clip.h == activeDrawEnv.clip.h &&
      curSplit.drawenv.dfe == activeDrawEnv.dfe &&
      curSplit.projectionEpoch == g_guestProjectionEpoch &&
      SameProjectionDispEnv(curSplit.dispenv, projectionDispEnv) &&
      curSplit.debugText == currentSplitDebugText) {
    return;
  }

  curSplit.numVerts = g_vertexIndex - curSplit.startVertex;

  if (g_splitIndex + 1 >= MAX_DRAW_SPLITS) {
    eprinterr("MAX_DRAW_SPLITS reached (too many blend modes, texture formats, "
              "drawEnv clip rects, dfe switches), expect rendering errors\n");
    return;
  }

  GPUDrawSplit &split = g_splits[++g_splitIndex];
  split.blendMode = blendMode;
  split.texFormat = texFormat;
  split.textureId = textureId;
  split.textureFilterMode = textureFilterMode;
  split.drawPrimMode = g_DrawPrimMode;
  split.drawenv = activeDrawEnv;
  split.dispenv = projectionDispEnv;
  split.projectionEpoch = g_guestProjectionEpoch;
  split.debugText = currentSplitDebugText;

  split.drawenv.tw.w = overrideTextureWidth;
  split.drawenv.tw.h = overrideTextureHeight;

  split.startVertex = g_vertexIndex;
  split.numVerts = 0;
}

int GR_UsesWorldDepth(const GrVertex *triangle, int depthRequested) {
#if USE_PGXP
  if (!depthRequested || triangle == nullptr)
    return 0;

  // scr_h is populated only by a primitive-atomic perspective tuple. Native
  // screen-space/HUD and affine fallback vertices keep it at zero. Equal-W
  // camera-facing walls are still world geometry and must participate in
  // depth, otherwise intersecting models can leak through them.
  if (triangle[0].scr_h <= 0.0f || triangle[1].scr_h <= 0.0f ||
      triangle[2].scr_h <= 0.0f || !std::isfinite(triangle[0].z) ||
      !std::isfinite(triangle[1].z) || !std::isfinite(triangle[2].z))
    return 0;
  // Fully clipped triangles must not advance or clear the painter-order
  // depth heuristic even though they retain a valid homogeneous tuple.
  if (std::max({triangle[0].z, triangle[1].z, triangle[2].z}) < PGXP_NEAR_PLANE)
    return 0;
  return 1;
#else
  (void)triangle;
  (void)depthRequested;
  return 0;
#endif
}

constexpr unsigned int WORLD_DEPTH_BAND_COUNT = 64U;

struct WorldDepthTargetEpoch {
  unsigned long long clearSerial{~0ULL};
  unsigned int band{};
  bool rawSinceDepth{};
  bool painterOnly{};
};

struct WorldDepthEpoch {
  WorldDepthTargetEpoch screen{};
  WorldDepthTargetEpoch offscreen{};
};

static WorldDepthEpoch g_worldDepthEpoch{};
static unsigned long long g_worldDepthBandAdvances[2]{};
static unsigned long long g_worldDepthPainterFallbacks[2]{};

void GR_ResetWorldDepthEpochs() {
  g_worldDepthEpoch = {};
  g_worldDepthBandAdvances[0] = g_worldDepthBandAdvances[1] = 0U;
  g_worldDepthPainterFallbacks[0] = g_worldDepthPainterFallbacks[1] = 0U;
}

unsigned long long GR_GetWorldDepthBandAdvanceCount(int offscreen) {
  return g_worldDepthBandAdvances[offscreen ? 1 : 0];
}

unsigned long long GR_GetWorldDepthPainterFallbackCount(int offscreen) {
  return g_worldDepthPainterFallbacks[offscreen ? 1 : 0];
}

static void SynchronizeWorldDepthEpoch(WorldDepthTargetEpoch &target,
                                       int offscreen) {
  const auto clearSerial = GR_GetDepthClearSerial(offscreen);
  if (target.clearSerial == clearSerial)
    return;
  target = {};
  target.clearSerial = clearSerial;
}

void DrawSplit(const GPUDrawSplit &split, WorldDepthEpoch &depthEpoch) {
  GR_ApplyProjectionEpoch(&split.dispenv, split.projectionEpoch);
  if (split.debugText)
    GR_PushDebugLabel(split.debugText);

  GR_SetStencilMode(split.drawPrimMode); // draw with mask 0x16

  GR_SetTexture(split.textureId, split.texFormat, split.textureFilterMode);
  GR_SetTextureBlendMode(split.blendMode);

  if (split.texFormat == TF_32_BIT_RGBA)
    GR_SetOverrideTextureSize(
        split.textureId == g_whiteTexture ? 1 : split.drawenv.tw.w,
        split.textureId == g_whiteTexture ? 1 : split.drawenv.tw.h);

  const bool drawOnScreen = split.drawenv.dfe;
  if (split.drawenv.clip.w <= 0 || split.drawenv.clip.h <= 0)
    return;

  GR_SetupClipMode(&split.drawenv.clip, drawOnScreen);
  GR_SetOffscreenState(&split.drawenv.clip, !drawOnScreen);

  GR_SetBlendModeForPrimitive(split.blendMode,
                              split.textureId == g_whiteTexture);
  const int offscreen = drawOnScreen ? 0 : 1;
  auto &target = drawOnScreen ? depthEpoch.screen : depthEpoch.offscreen;
  SynchronizeWorldDepthEpoch(target, offscreen);
  auto &rawSinceDepth = target.rawSinceDepth;
  GR_SetPolygonOffset(0.0f, 0.0f);

  const bool depthRequested = g_RequestedDepthMode != 0;
  const bool depthWrite = split.blendMode == BM_NONE;
  int runStart = split.startVertex;
  int runVertices = 0;
  bool runUsesDepth = false;

  const auto flushRun = [&]() {
    if (runVertices == 0)
      return;
    if (runUsesDepth) {
      if (depthRequested && g_cfg_pgxpZBuffer && rawSinceDepth &&
          !target.painterOnly) {
        if (target.band + 1U < WORLD_DEPTH_BAND_COUNT) {
          ++target.band;
          ++g_worldDepthBandAdvances[offscreen];
        } else {
          target.painterOnly = true;
          ++g_worldDepthPainterFallbacks[offscreen];
        }
        rawSinceDepth = false;
      }
      if (!target.painterOnly) {
        const auto lower = static_cast<float>(target.band) /
                           static_cast<float>(WORLD_DEPTH_BAND_COUNT);
        const auto upper = static_cast<float>(target.band + 1U) /
                           static_cast<float>(WORLD_DEPTH_BAND_COUNT);
        GR_SetDepthRange(lower, upper);
        GR_SetDepthState(1, depthWrite ? 1 : 0);
      } else {
        GR_SetDepthState(0, 0);
      }
    } else {
      GR_SetDepthState(0, 0);
    }
    GR_DrawTriangles(runStart, runVertices / 3);
    if (!runUsesDepth && depthRequested && g_cfg_pgxpZBuffer)
      rawSinceDepth = true;
    runVertices = 0;
  };

  for (int vertexIndex = split.startVertex;
       vertexIndex < split.startVertex + split.numVerts; vertexIndex += 3) {
    const GrVertex *triangle = &g_vertexBuffer[vertexIndex];
    const bool usesDepth = GR_UsesWorldDepth(triangle, depthRequested) != 0;
    if (runVertices != 0 && runUsesDepth != usesDepth)
      flushRun();
    if (runVertices == 0) {
      runStart = vertexIndex;
      runUsesDepth = usesDepth;
    }
    runVertices += 3;
  }
  flushRun();

  if (split.debugText)
    GR_PopDebugLabel();
}

extern int g_dbg_polygonSelected;

//
// Draws all polygons after AggregatePTAG
//
void DrawAllSplits() {
#ifdef _DEBUG
  if (g_dbg_emulatorPaused) {
    for (int i = 0; i < 3; i++) {
      GrVertex *vert = &g_vertexBuffer[g_dbg_polygonSelected + i];
      vert->r = 255;
      vert->g = 0;
      vert->b = 0;

      eprintf("==========================================\n");
      eprintf("POLYGON: %d\n", g_dbg_polygonSelected);
#if USE_PGXP
      eprintf("X: %.2f Y: %.2f\n", (float)vert->x, (float)vert->y);
      eprintf("U: %.2f V: %.2f\n", (float)vert->u, (float)vert->v);
      eprintf("TP: %d CLT: %d\n", (int)vert->page, (int)vert->clut);
#else
      eprintf("X: %d Y: %d\n", vert->x, vert->y);
      eprintf("U: %d V: %d\n", vert->u, vert->v);
      eprintf("TP: %d CLT: %d\n", vert->page, vert->clut);
#endif

      eprintf("==========================================\n");
    }

    PsyX_UpdateInput();
  }
#endif // _DEBUG

  // Empty ordering tables carry no drawable state. Avoid a zero-byte VBO
  // orphan/upload for optional presentation passes.
  if (g_vertexIndex == 0) {
    GR_SetDepthRange(0.0f, 1.0f);
    ClearSplits();
    return;
  }
  // next code ideally should be called before EndScene
  GR_UpdateVertexBuffer(g_vertexBuffer, g_vertexIndex);

  // Valid perspective world geometry uses reversed depth. Opaque primitives
  // establish it; transparent primitives test without writing. Legacy, HUD
  // and affine fallback packets retain strict GP0 painter order, so missing
  // projection metadata cannot inject an arbitrary Z value.
  for (int splitIndex = 1; splitIndex <= g_splitIndex; ++splitIndex)
    DrawSplit(g_splits[splitIndex], g_worldDepthEpoch);
  GR_SetDepthRange(0.0f, 1.0f);

  ClearSplits();
}

// forward declarations
int ParsePrimitive(P_TAG *polyTag);

void ParsePrimitivesLinkedList(u_long *p, int singlePrimitive,
                               int preservePgxpIndex) {
  if (!p)
    return;

  // setup single primitive flag (needed for AddSplits)
  g_DrawPrimMode = singlePrimitive;

  if (singlePrimitive) {
    P_TAG *polyTag = reinterpret_cast<P_TAG *>(p);
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
    // force PGXP off
    if (!preservePgxpIndex)
      polyTag->pgxp_index = 0xFFFF;
#endif
    if (g_vertexIndex >= MAX_VERTEX_BUFFER_SIZE - MAX_VERTICES_PER_PRIMITIVE) {
      GPUDrawSplit &lastSplit = g_splits[g_splitIndex];
      lastSplit.numVerts = g_vertexIndex - lastSplit.startVertex;
      DrawAllSplits();
    }
    ParsePrimitive(polyTag);

    GPUDrawSplit &lastSplit = g_splits[g_splitIndex];
    lastSplit.numVerts = g_vertexIndex - lastSplit.startVertex;
  } else {
    // walk OT_TAG linked list
    for (uintptr_t basePacket = reinterpret_cast<uintptr_t>(p);;
         basePacket = reinterpret_cast<uintptr_t>(nextPrim(basePacket))) {
      const int tagLength = getlen(basePacket);
      if (tagLength > 0) {
        if (tagLength > 32) {
          eprinterr("got invalid tag length %d, code %d\n", tagLength,
                    reinterpret_cast<P_TAG *>(basePacket)->code);
        }

        uintptr_t currentPacket = basePacket;
        const uintptr_t endPacket =
            basePacket + (tagLength + P_LEN) * sizeof(u_int);
        int primLength = 0;
        while (currentPacket < endPacket) {
          if (g_vertexIndex >=
              MAX_VERTEX_BUFFER_SIZE - MAX_VERTICES_PER_PRIMITIVE) {
            GPUDrawSplit &lastSplit = g_splits[g_splitIndex];
            lastSplit.numVerts = g_vertexIndex - lastSplit.startVertex;
            DrawAllSplits();
          }
          primLength = ParsePrimitive(reinterpret_cast<P_TAG *>(currentPacket));
          currentPacket += (primLength + P_LEN) * sizeof(u_int);
        }

        if (currentPacket != endPacket) {
          eprinterr("did not output valid primitive or ptag length is not "
                    "valid (diff=%d)\n",
                    endPacket - currentPacket);
        }
      }

      GPUDrawSplit &lastSplit = g_splits[g_splitIndex];
      lastSplit.numVerts = g_vertexIndex - lastSplit.startVertex;

      if (isendprim(basePacket))
        break;
    }
  }
}

inline int IsNull(POLY_FT3 *poly) {
  return poly->x0 == -1 && poly->y0 == -1 && poly->x1 == -1 && poly->y1 == -1 &&
         poly->x2 == -1 && poly->y2 == -1;
}

static bool IsExactProjectedVertex(VERTTYPE *vertex, ushort gteIndex,
                                   int cacheOffset) {
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
  PGXPVData cached;
  return LookupVertexPGXP(&cached, vertex, gteIndex, cacheOffset);
#else
  (void)vertex;
  (void)gteIndex;
  (void)cacheOffset;
  return true;
#endif
}

static TextureFilterMode TriangleTextureFilter(ushort gteIndex, VERTTYPE *p0,
                                               VERTTYPE *p1, VERTTYPE *p2) {
  return IsExactProjectedVertex(p0, gteIndex, -3) &&
                 IsExactProjectedVertex(p1, gteIndex, -2) &&
                 IsExactProjectedVertex(p2, gteIndex, -1)
             ? TEXTURE_FILTER_WORLD_ANISOTROPIC
             : TEXTURE_FILTER_BILINEAR;
}

static TextureFilterMode QuadTextureFilter(ushort gteIndex, VERTTYPE *p0,
                                           VERTTYPE *p1, VERTTYPE *p2,
                                           VERTTYPE *p3) {
  return IsExactProjectedVertex(p0, gteIndex, -4) &&
                 IsExactProjectedVertex(p1, gteIndex, -3) &&
                 IsExactProjectedVertex(p2, gteIndex, -1) &&
                 IsExactProjectedVertex(p3, gteIndex, -2)
             ? TEXTURE_FILTER_WORLD_ANISOTROPIC
             : TEXTURE_FILTER_BILINEAR;
}

static int ProcessFlatLines(P_TAG *polyTag) {
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
  const u_short gteIndex = polyTag->pgxp_index;
#else
  const u_short gteIndex = 0xFFFF;
#endif

  const bool shadeTexOn = true;
  const bool semiTrans = (polyTag->code & 2);
  const int primSubType = polyTag->code & 0x0C;

  switch (primSubType) {
  case 0x0: {
    LINE_F2 *poly = (LINE_F2 *)polyTag;

    AddSplit(semiTrans, false);

    VERTTYPE *p0 = &poly->x0;
    VERTTYPE *p1 = &poly->x1;
    unsigned char *c0 = &poly->r0;
    unsigned char *c1 = c0;

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    LineSwapSourceVerts(p0, p1, c0, c1);
    MakeLineArray(firstVertex, p0, p1, gteIndex);
    MakeTexcoordLineZero(firstVertex, 0);
    MakeColourLine(firstVertex, shadeTexOn, c0, c1);

    TriangulateQuad();

    g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 3;
  }
  case 0x8: // TODO (unused)
  {
    LINE_F3 *poly = (LINE_F3 *)polyTag;

    AddSplit(semiTrans, false);

    {
      VERTTYPE *p0 = &poly->x0;
      VERTTYPE *p1 = &poly->x1;
      unsigned char *c0 = &poly->r0;
      unsigned char *c1 = c0;

      GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
      LineSwapSourceVerts(p0, p1, c0, c1);
      MakeLineArray(firstVertex, p0, p1, gteIndex);
      MakeTexcoordLineZero(firstVertex, 0);
      MakeColourLine(firstVertex, shadeTexOn, c0, c1);

      TriangulateQuad();

      g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
      polygon_count++;
#endif
    }

    {
      VERTTYPE *p0 = &poly->x1;
      VERTTYPE *p1 = &poly->x2;
      unsigned char *c0 = &poly->r0;
      unsigned char *c1 = c0;

      GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
      LineSwapSourceVerts(p0, p1, c0, c1);
      MakeLineArray(firstVertex, p0, p1, gteIndex);
      MakeTexcoordLineZero(firstVertex, 0);
      MakeColourLine(firstVertex, shadeTexOn, c0, c1);

      TriangulateQuad();

      g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
      polygon_count++;
#endif
    }

    return 5;
  }
  case 0xc: {
    int i;
    LINE_F4 *poly = (LINE_F4 *)polyTag;

    AddSplit(semiTrans, false);

    {
      VERTTYPE *p0 = &poly->x0;
      VERTTYPE *p1 = &poly->x1;
      unsigned char *c0 = &poly->r0;
      unsigned char *c1 = c0;

      GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
      LineSwapSourceVerts(p0, p1, c0, c1);
      MakeLineArray(firstVertex, p0, p1, gteIndex);
      MakeTexcoordLineZero(firstVertex, 0);
      MakeColourLine(firstVertex, shadeTexOn, c0, c1);

      TriangulateQuad();

      g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
      polygon_count++;
#endif
    }

    {
      VERTTYPE *p0 = &poly->x1;
      VERTTYPE *p1 = &poly->x2;
      unsigned char *c0 = &poly->r0;
      unsigned char *c1 = c0;

      GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
      LineSwapSourceVerts(p0, p1, c0, c1);
      MakeLineArray(firstVertex, p0, p1, gteIndex);
      MakeTexcoordLineZero(firstVertex, 0);
      MakeColourLine(firstVertex, shadeTexOn, c0, c1);

      TriangulateQuad();

      g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
      polygon_count++;
#endif
    }

    {
      VERTTYPE *p0 = &poly->x2;
      VERTTYPE *p1 = &poly->x3;
      unsigned char *c0 = &poly->r0;
      unsigned char *c1 = c0;

      GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
      LineSwapSourceVerts(p0, p1, c0, c1);
      MakeLineArray(firstVertex, p0, p1, gteIndex);
      MakeTexcoordLineZero(firstVertex, 0);
      MakeColourLine(firstVertex, shadeTexOn, c0, c1);

      TriangulateQuad();

      g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
      polygon_count++;
#endif
    }

    return 6;
  }
  }
  return 0;
}

static int ProcessGouraudLines(P_TAG *polyTag) {
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
  const u_short gteIndex = polyTag->pgxp_index;
#else
  const u_short gteIndex = 0xFFFF;
#endif

  const bool shadeTexOn = true;
  const bool semiTrans = (polyTag->code & 2);
  const int primSubType = polyTag->code & 0x0C;

  switch (primSubType) {
  case 0x0: {
    LINE_G2 *poly = (LINE_G2 *)polyTag;

    AddSplit(semiTrans, false);

    VERTTYPE *p0 = &poly->x0;
    VERTTYPE *p1 = &poly->x1;
    unsigned char *c0 = &poly->r0;
    unsigned char *c1 = &poly->r1;

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    LineSwapSourceVerts(p0, p1, c0, c1);
    MakeLineArray(firstVertex, p0, p1, gteIndex);
    MakeTexcoordLineZero(firstVertex, 0);
    MakeColourLine(firstVertex, shadeTexOn, c0, c1);

    TriangulateQuad();

    g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 4;
  }
  case 0x8: {
    // TODO: LINE_G3
    return 7;
  }
  case 0xC: {
    // TODO: LINE_G4
    return 9;
  }
  }
  return 0;
}

static int ProcessFlatPoly(P_TAG *polyTag) {
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
  const u_short gteIndex = polyTag->pgxp_index;
#else
  const u_short gteIndex = 0xFFFF;
#endif

  const bool shadeTexOn = (polyTag->code & 1) == 0;
  const bool semiTrans = (polyTag->code & 2);
  const int primSubType = polyTag->code & 0x0C;

  switch (primSubType) {
  case 0x0: {
    POLY_F3 *poly = (POLY_F3 *)polyTag;

    AddSplit(semiTrans, false);

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
    MakeTexcoordTriangleZero(firstVertex, 0);
    MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0,
                       &poly->r0);

    g_vertexIndex += ClipNearPlaneTriangles(firstVertex, 3);

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 4;
  }
  case 0x4: {
    POLY_FT3 *poly = (POLY_FT3 *)polyTag;
    activeDrawEnv.tpage = poly->tpage;

    // It is an official hack from SCE devs to not use DR_TPAGE and instead use
    // null polygon
    if (!IsNull(poly)) {
      AddSplit(
          semiTrans, true,
          TriangleTextureFilter(gteIndex, &poly->x0, &poly->x1, &poly->x2));

      GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
      MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2,
                         gteIndex);
      MakeTexcoordTriangle(
          firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut,
          GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
      MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0,
                         &poly->r0);

      g_vertexIndex += ClipNearPlaneTriangles(firstVertex, 3);

#if defined(DEBUG_POLY_COUNT)
      polygon_count++;
#endif
    }
    return 7;
  }
  case 0x8: {
    POLY_F4 *poly = (POLY_F4 *)polyTag;

    AddSplit(semiTrans, false);

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2,
                   gteIndex);
    MakeTexcoordQuadZero(firstVertex, 0);
    MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0,
                   &poly->r0);

    TriangulateQuad();

    g_vertexIndex += ClipNearPlaneTriangles(firstVertex, 6);
#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 5;
  }
  case 0xC: {
    POLY_FT4 *poly = (POLY_FT4 *)polyTag;
    activeDrawEnv.tpage = poly->tpage;

    AddSplit(semiTrans, true,
             QuadTextureFilter(gteIndex, &poly->x0, &poly->x1, &poly->x3,
                               &poly->x2));

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2,
                   gteIndex);
    MakeTexcoordQuad(
        firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage,
        poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
    MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0,
                   &poly->r0);

    TriangulateQuad();

    g_vertexIndex += ClipNearPlaneTriangles(firstVertex, 6);

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 9;
  }
  }
  return 0;
}

static int ProcessGouraudPoly(P_TAG *polyTag) {
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
  const u_short gteIndex = polyTag->pgxp_index;
#else
  const u_short gteIndex = 0xFFFF;
#endif

  const bool shadeTexOn = true;
  const bool semiTrans = (polyTag->code & 2);
  const int primSubType = polyTag->code & 0x0C;

  switch (primSubType) {
  case 0x0: {
    POLY_G3 *poly = (POLY_G3 *)polyTag;

    AddSplit(semiTrans, false);

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
    MakeTexcoordTriangleZero(firstVertex, 1);
    MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1,
                       &poly->r2);

    g_vertexIndex += ClipNearPlaneTriangles(firstVertex, 3);

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 6;
  }
  case 0x4: {
    POLY_GT3 *poly = (POLY_GT3 *)polyTag;
    activeDrawEnv.tpage = poly->tpage;

    AddSplit(semiTrans, true,
             TriangleTextureFilter(gteIndex, &poly->x0, &poly->x1, &poly->x2));

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
    MakeTexcoordTriangle(
        firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut,
        GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
    MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1,
                       &poly->r2);

    g_vertexIndex += ClipNearPlaneTriangles(firstVertex, 3);

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 9;
  }
  case 0x8: {
    POLY_G4 *poly = (POLY_G4 *)polyTag;

    AddSplit(semiTrans, false);

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2,
                   gteIndex);
    MakeTexcoordQuadZero(firstVertex, 1);
    MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3,
                   &poly->r2);

    TriangulateQuad();

    g_vertexIndex += ClipNearPlaneTriangles(firstVertex, 6);

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 8;
  }
  case 0xC: {
    POLY_GT4 *poly = (POLY_GT4 *)polyTag;
    activeDrawEnv.tpage = poly->tpage;

    AddSplit(semiTrans, true,
             QuadTextureFilter(gteIndex, &poly->x0, &poly->x1, &poly->x3,
                               &poly->x2));

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2,
                   gteIndex);
    MakeTexcoordQuad(
        firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage,
        poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
    MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3,
                   &poly->r2);

    TriangulateQuad();

    g_vertexIndex += ClipNearPlaneTriangles(firstVertex, 6);

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 12;
  }
  }
  return 0;
}

static int ProcessTileAndSprt(P_TAG *polyTag) {
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
  const u_short gteIndex = polyTag->pgxp_index;
#else
  const u_short gteIndex = 0xFFFF;
#endif

  // NOTE: TILE does not support switching shadeTex on real PSX
  const bool shadeTexOn = (polyTag->code & 1) == 0;
  const bool semiTrans = (polyTag->code & 2);

  switch (polyTag->code & 0xFD) {
  case 0x60: {
    TILE *poly = (TILE *)polyTag;

    AddSplit(semiTrans, false);

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h, gteIndex);
    MakeTexcoordQuadZero(firstVertex, 0);
    MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0,
                   &poly->r0);

    TriangulateQuad();

    g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 3;
  }
  case 0x64: {
    SPRT *poly = (SPRT *)polyTag;

    AddSplit(semiTrans, true, TEXTURE_FILTER_BILINEAR);

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h, gteIndex);
    MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut,
                     poly->w, poly->h);
    MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0,
                   &poly->r0);

    TriangulateQuad();

    g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 4;
  }
  case 0x68: {
    TILE_1 *poly = (TILE_1 *)polyTag;

    AddSplit(semiTrans, false);

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexRect(firstVertex, &poly->x0, 1, 1, gteIndex);
    MakeTexcoordQuadZero(firstVertex, 0);
    MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0,
                   &poly->r0);

    TriangulateQuad();

    g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 2;
  }
  case 0x70: {
    TILE_8 *poly = (TILE_8 *)polyTag;

    AddSplit(semiTrans, false);

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexRect(firstVertex, &poly->x0, 8, 8, gteIndex);
    MakeTexcoordQuadZero(firstVertex, 0);
    MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0,
                   &poly->r0);

    TriangulateQuad();

    g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 2;
  }
  case 0x74: {
    SPRT_8 *poly = (SPRT_8 *)polyTag;

    AddSplit(semiTrans, true, TEXTURE_FILTER_BILINEAR);

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexRect(firstVertex, &poly->x0, 8, 8, gteIndex);
    MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 8,
                     8);
    MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0,
                   &poly->r0);

    TriangulateQuad();

    g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 3;
  }
  case 0x78: {
    TILE_16 *poly = (TILE_16 *)polyTag;

    AddSplit(semiTrans, false);

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexRect(firstVertex, &poly->x0, 16, 16, gteIndex);
    MakeTexcoordQuadZero(firstVertex, 0);
    MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0,
                   &poly->r0);

    TriangulateQuad();

    g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 2;
  }
  case 0x7C: {
    SPRT_16 *poly = (SPRT_16 *)polyTag;

    AddSplit(semiTrans, true, TEXTURE_FILTER_BILINEAR);

    GrVertex *firstVertex = &g_vertexBuffer[g_vertexIndex];
    MakeVertexRect(firstVertex, &poly->x0, 16, 16, gteIndex);
    MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut,
                     16, 16);
    MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0,
                   &poly->r0);

    TriangulateQuad();

    g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
    polygon_count++;
#endif
    return 3;
  }
  }
  return 0;
}

static int ProcessDrawEnv(P_TAG *polyTag) {
  const u_int *codePtr = (u_int *)&polyTag->pad0;
  int processedLongs = 0;
  for (int i = 0; i < polyTag->len; ++i) {
    const u_int code = codePtr[i];
    const int primSubType = code >> 24 & 0x0F;

    switch (primSubType) {
    case 0x1: {
      // DR_TPAGE
      activeDrawEnv.tpage = (code & 0x1FF);
      activeDrawEnv.dtd = (code >> 9) & 1;
      activeDrawEnv.dfe = (code >> 10) & 1;
      break;
    }
    case 0x2: {
      // DR_TWIN
      activeDrawEnv.tw.w = (code & 0x1F);
      activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
      activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
      activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
      break;
    }
    case 0x3: {
      // DR_AREA
      activeDrawEnv.clip.x = code & 1023;
      activeDrawEnv.clip.y = (code >> 10) & 511;
      break;
    }
    case 0x4: {
      // DR_AREA (second part)
      const int right = code & 1023;
      const int bottom = (code >> 10) & 511;
      const int width = right - activeDrawEnv.clip.x + 1;
      const int height = bottom - activeDrawEnv.clip.y + 1;
      activeDrawEnv.clip.w =
          width > 0 && activeDrawEnv.clip.x + width <= 1024 ? width : 0;
      activeDrawEnv.clip.h =
          height > 0 && activeDrawEnv.clip.y + height <= 512 ? height : 0;
      break;
    }
    case 0x5: {
      // DR_OFFSET
      activeDrawEnv.ofs[0] =
          static_cast<short>(static_cast<int>(code << 21) >> 21);
      activeDrawEnv.ofs[1] =
          static_cast<short>(static_cast<int>(code << 10) >> 21);
      break;
    }
    case 0x6: {
      // MaskSetOR = (*cb & 1) ? 0x8000 : 0x0000;
      // MaskEvalAND = (*cb & 2) ? 0x8000 : 0x0000;
      break;
    }
    case 0:
      // proceed to next primitive tag
      return processedLongs;
    }
    ++processedLongs;
  }

  return processedLongs;
}

static int ProcessPsyXPrims(P_TAG *polyTag) {
  const int primType = polyTag->code & 0xF0;
  const int primSubType = polyTag->code & 0x0F;

  switch (primSubType) {
  case 0x01: {
    DR_PSYX_TEX *psytex = (DR_PSYX_TEX *)polyTag;
    overrideTexture = psytex->code[0] & 0xFFFFFF;
    overrideTextureWidth = psytex->code[1] & 0xFFF;
    overrideTextureHeight = psytex->code[1] >> 16 & 0xFFF;
    return 2;
  }
  case 0x02: {
    // [A] Psy-X custom texture packet
    DR_PSYX_DBGMARKER *psydbg = (DR_PSYX_DBGMARKER *)polyTag;
    currentSplitDebugText = psydbg->text;
    return 2;
  }
  }

  return 0;
}

// Processes primitive
// returns processed primitive primLength in longs
int ParsePrimitive(P_TAG *polyTag) {
  const int primType = polyTag->code & 0xF0;

  int primLength = 0;

  switch (primType) {
  case 0x00: {
    const int primSubType = polyTag->code & 0x0F;
    if (primSubType == 0x0) {
      primLength = 3;
    } else if (primSubType == 0x1) {
      DR_MOVE *drmove = (DR_MOVE *)polyTag;

      const int y = drmove->code[3] >> 0x10 & 0xFFFF;
      const int x = drmove->code[3] & 0xFFFF;

      RECT16 rect;
      *(uint *)&rect.x = *(uint *)&drmove->code[2];
      *(uint *)&rect.w = *(uint *)&drmove->code[4];

      MoveImage(&rect, x, y);
      primLength = 5;
    }
    break;
  }
  case 0x20:
    // Flat polygons
    primLength = ProcessFlatPoly(polyTag);
    break;
  case 0x30:
    // Gouraud shaded polygons
    primLength = ProcessGouraudPoly(polyTag);
    break;
  case 0x40:
    // Flat (single colour) Lines
    primLength = ProcessFlatLines(polyTag);
    break;
  case 0x50:
    // Gouraud lines
    primLength = ProcessGouraudLines(polyTag);
    break;
  case 0x60:
  case 0x70:
    // TILE and SPRT
    primLength = ProcessTileAndSprt(polyTag);
    break;
  case 0xA0:
    // DR_LOAD
    {
      DR_LOAD *drload = (DR_LOAD *)polyTag;

      RECT16 rect;
      *(uint *)&rect.x = *(uint *)&drload->code[1];
      *(uint *)&rect.w = *(uint *)&drload->code[2];

      LoadImage(&rect, (u_long *)drload->p);
      // Emulator_UpdateVRAM();			// FIXME: should it be updated
      // immediately?

      // FIXME: is there othercommands?
    }
    primLength = getlen(polyTag);
    break;
  case 0xB0:
    // [A] Psy-X custom primitives
    primLength = ProcessPsyXPrims(polyTag);
    break;
  case 0xE0:
    // Draw Env setup
    primLength = ProcessDrawEnv(polyTag);
    break;
    // default:
    //	eprinterr("got %0x primitive\n", primType);
  }

  if (primLength == 0) {
    eprinterr("Unhandled zero length %0x primitive\n", primType);
  }

  return primLength;
}

void GR_EndGuestProjectionEpoch(void) {
  g_guestProjectionDispEnvValid = 0;
  g_guestProjectionEpoch = 0;
  GR_ApplyProjectionEpoch(nullptr, 0);
}
