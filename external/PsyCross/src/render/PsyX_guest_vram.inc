// Resolution-derived raw guest targets and GPU-authoritative PS1 VRAM.
// Included by PsyX_render.cpp after GR_Shader_Compile is available.

namespace {

constexpr int guest_vram_dirty_word_bits = 64;
constexpr int guest_vram_dirty_word_count =
    (VRAM_WIDTH + guest_vram_dirty_word_bits - 1) / guest_vram_dirty_word_bits;

struct GrGuestPixelExtent {
  int width{1};
  int height{1};
};

#if defined(RENDERER_OGL)

struct GrGuestConversionState {
  GLint program{};
  GLint vertex_array{};
  GLint active_texture{};
  GLint texture0{};
  GLint read_framebuffer{};
  GLint draw_framebuffer{};
  GLint viewport[4]{};
  GLboolean blend{};
  GLboolean depth{};
  GLboolean stencil{};
  GLboolean scissor{};
  GLboolean framebuffer_srgb{};
  GLboolean dither{};
  GLboolean cull_face{};
  GLboolean rasterizer_discard{};
  GLint read_buffer{};
  GLint draw_buffer{};
  GLint polygon_mode[2]{};
  GLboolean color_mask[4]{};
};

static std::array<std::array<std::uint64_t, guest_vram_dirty_word_count>,
                  VRAM_HEIGHT>
    g_gpuDirtyVRAM{};
static std::array<std::array<std::uint64_t, guest_vram_dirty_word_count>,
                  VRAM_HEIGHT>
    g_guestColorDirtyVRAM{};
static unsigned long long g_synchronousVRAMReadbacks{};
static unsigned long long g_guestVRAMPackCount{};
static unsigned long long g_guestVRAMPackPixels{};
static unsigned long long g_guestSeedCount{};
static unsigned long long g_guestSeedPixels{};
static unsigned long long g_guestCaptureCount{};
static unsigned long long g_guestCapturePixels{};
static GLuint g_guestVRAMConversionProgram{};
static bool g_guestVRAMConversionAvailable{};
static GLuint g_guestVRAMConversionVertexArray{};
static GLint g_guestVRAMModeLocation{-1};
static GLint g_guestVRAMRectLocation{-1};
static int g_guestDisplayWidth{256};
static int g_guestDisplayHeight{240};
static int g_pendingGuestDisplayWidth{256};
static int g_pendingGuestDisplayHeight{240};
static int g_guestRenderConfigWidth{};
static int g_guestRenderConfigHeight{};
static int g_guestRenderConfigAspect{-1};
static int g_guestRenderConfigComposed{-1};
static int g_maxGuestTextureSize{1};
static bool g_guestRenderLogicalFallback{};
static bool g_guestTextureLimitLogged{};
static bool g_nextOffscreenIsRoot{true};

static std::uint64_t GR_GuestDirtyMask(int word, int x0, int x1) {
  const int word_x0 = word * guest_vram_dirty_word_bits;
  const int begin = std::max(x0 - word_x0, 0);
  const int end = std::min(x1 - word_x0, guest_vram_dirty_word_bits);
  if (begin >= end)
    return 0U;
  const std::uint64_t low =
      begin == 0 ? ~std::uint64_t{} : (~std::uint64_t{} << begin);
  const std::uint64_t high =
      end == guest_vram_dirty_word_bits
          ? ~std::uint64_t{}
          : ((std::uint64_t{1} << end) - std::uint64_t{1});
  return low & high;
}

static bool GR_ClipGuestVRAMRect(int &x, int &y, int &width, int &height) {
  const int x1 = std::min(VRAM_WIDTH, x + width);
  const int y1 = std::min(VRAM_HEIGHT, y + height);
  x = std::max(x, 0);
  y = std::max(y, 0);
  width = x1 - x;
  height = y1 - y;
  return width > 0 && height > 0;
}

static void GR_MarkGuestColorDirty(int x, int y, int width, int height) {
  if (!GR_ClipGuestVRAMRect(x, y, width, height))
    return;
  const int x1 = x + width;
  for (int row = y; row < y + height; ++row) {
    for (int word = x / guest_vram_dirty_word_bits;
         word <= (x1 - 1) / guest_vram_dirty_word_bits; ++word) {
      g_guestColorDirtyVRAM[row][word] |= GR_GuestDirtyMask(word, x, x1);
    }
  }
}

static void GR_ClearGuestColorDirty(int x, int y, int width, int height) {
  if (!GR_ClipGuestVRAMRect(x, y, width, height))
    return;
  const int x1 = x + width;
  for (int row = y; row < y + height; ++row) {
    for (int word = x / guest_vram_dirty_word_bits;
         word <= (x1 - 1) / guest_vram_dirty_word_bits; ++word) {
      g_guestColorDirtyVRAM[row][word] &= ~GR_GuestDirtyMask(word, x, x1);
    }
  }
}

static bool GR_IsGuestColorDirty(int x, int y, int width, int height) {
  if (!GR_ClipGuestVRAMRect(x, y, width, height))
    return false;
  const int x1 = x + width;
  for (int row = y; row < y + height; ++row) {
    for (int word = x / guest_vram_dirty_word_bits;
         word <= (x1 - 1) / guest_vram_dirty_word_bits; ++word) {
      if ((g_guestColorDirtyVRAM[row][word] & GR_GuestDirtyMask(word, x, x1)) !=
          0U)
        return true;
    }
  }
  return false;
}

static bool GR_IsGuestColorPixelDirty(int x, int y) {
  return (g_guestColorDirtyVRAM[y][x / guest_vram_dirty_word_bits] &
          (std::uint64_t{1} << (x % guest_vram_dirty_word_bits))) != 0U;
}

static void GR_ResetGPUVRAMDirty() {
  for (auto &row : g_gpuDirtyVRAM)
    row.fill(0U);
  for (auto &row : g_guestColorDirtyVRAM)
    row.fill(~std::uint64_t{});
  g_synchronousVRAMReadbacks = 0U;
  g_guestVRAMPackCount = 0U;
  g_guestVRAMPackPixels = 0U;
  g_guestSeedCount = 0U;
  g_guestSeedPixels = 0U;
  g_guestCaptureCount = 0U;
  g_guestCapturePixels = 0U;
}

static void GR_MarkGPUVRAMDirty(int x, int y, int width, int height) {
  if (!GR_ClipGuestVRAMRect(x, y, width, height))
    return;
  const int x1 = x + width;
  for (int row = y; row < y + height; ++row) {
    for (int word = x / guest_vram_dirty_word_bits;
         word <= (x1 - 1) / guest_vram_dirty_word_bits; ++word) {
      g_gpuDirtyVRAM[row][word] |= GR_GuestDirtyMask(word, x, x1);
    }
  }
}

static void GR_ClearGPUVRAMDirty(int x, int y, int width, int height) {
  if (!GR_ClipGuestVRAMRect(x, y, width, height))
    return;
  const int x1 = x + width;
  for (int row = y; row < y + height; ++row) {
    for (int word = x / guest_vram_dirty_word_bits;
         word <= (x1 - 1) / guest_vram_dirty_word_bits; ++word) {
      g_gpuDirtyVRAM[row][word] &= ~GR_GuestDirtyMask(word, x, x1);
    }
  }
}

static bool GR_IsGPUVRAMDirty(int x, int y, int width, int height) {
  if (!GR_ClipGuestVRAMRect(x, y, width, height))
    return false;
  const int x1 = x + width;
  for (int row = y; row < y + height; ++row) {
    for (int word = x / guest_vram_dirty_word_bits;
         word <= (x1 - 1) / guest_vram_dirty_word_bits; ++word) {
      if ((g_gpuDirtyVRAM[row][word] & GR_GuestDirtyMask(word, x, x1)) != 0U)
        return true;
    }
  }
  return false;
}

static void GR_InvalidateAllHighResolutionVRAMPages() {
  for (auto &page : g_highResolutionVRAMPages)
    page.valid = false;
}

static void GR_RefreshGuestRenderConfiguration() {
  const PsyXPresentationViewport target = PsyX_GetRenderTargetExtent();
  if (g_guestRenderConfigWidth == target.w &&
      g_guestRenderConfigHeight == target.h &&
      g_guestRenderConfigAspect == g_cfg_aspectMode &&
      g_guestRenderConfigComposed == g_cfg_composedGuestScanout) {
    return;
  }
  g_guestRenderConfigWidth = target.w;
  g_guestRenderConfigHeight = target.h;
  g_guestRenderConfigAspect = g_cfg_aspectMode;
  g_guestRenderConfigComposed = g_cfg_composedGuestScanout;
  g_guestRenderLogicalFallback = false;
  GR_InvalidateAllHighResolutionVRAMPages();
}

static void GR_ApplyPendingGuestDisplayGeometry() {
  if (g_guestDisplayWidth == g_pendingGuestDisplayWidth &&
      g_guestDisplayHeight == g_pendingGuestDisplayHeight) {
    return;
  }
  g_guestDisplayWidth = g_pendingGuestDisplayWidth;
  g_guestDisplayHeight = g_pendingGuestDisplayHeight;
  GR_InvalidateAllHighResolutionVRAMPages();
}

static int GR_MapGuestEdge(int logical_edge, int logical_extent,
                           int pixel_extent) {
  if (logical_extent <= 0 || pixel_extent <= 0)
    return 0;
  const auto numerator = static_cast<long long>(logical_edge) * pixel_extent;
  return static_cast<int>((numerator + logical_extent / 2) / logical_extent);
}

static GrGuestPixelExtent
GR_GuestSubrectExtent(const GrHighResolutionVRAMPage &page, int x, int y,
                      int width, int height) {
  const int x0 =
      GR_MapGuestEdge(x - page.rect.x, page.rect.w, page.pixel_width);
  const int x1 =
      GR_MapGuestEdge(x + width - page.rect.x, page.rect.w, page.pixel_width);
  const int y0 =
      GR_MapGuestEdge(y - page.rect.y, page.rect.h, page.pixel_height);
  const int y1 =
      GR_MapGuestEdge(y + height - page.rect.y, page.rect.h, page.pixel_height);
  // Never resample a PS1 word domain below logical 1x: untouched packed
  // pixels must survive seed/pack round trips exactly.
  return {std::max(x1 - x0, std::max(width, 1)),
          std::max(y1 - y0, std::max(height, 1))};
}

static GrGuestPixelExtent GR_CalculateOffscreenPixelExtent(const RECT16 &rect) {
  GR_RefreshGuestRenderConfiguration();
  const bool root_page =
      g_nextOffscreenIsRoot ||
      (rect.w >= g_guestDisplayWidth && rect.h >= g_guestDisplayHeight);
  g_nextOffscreenIsRoot = false;
  if (g_guestRenderLogicalFallback)
    return {std::max<int>(rect.w, 1), std::max<int>(rect.h, 1)};
  if (!root_page) {
    const auto *retained =
        GR_FindHighResolutionVRAMPage(rect.x, rect.y, rect.w, rect.h);
    if (retained != nullptr)
      return GR_GuestSubrectExtent(*retained, rect.x, rect.y, rect.w, rect.h);
  }

  const auto extent = PsyX_CalculateGuestRenderExtent(
      g_guestRenderConfigWidth, g_guestRenderConfigHeight, g_cfg_aspectMode,
      rect.w, rect.h, g_guestDisplayWidth, g_guestDisplayHeight,
      root_page ? 1 : 0);
  if (extent.w <= g_maxGuestTextureSize && extent.h <= g_maxGuestTextureSize) {
    return {extent.w, extent.h};
  }

  g_guestRenderLogicalFallback = true;
  GR_InvalidateAllHighResolutionVRAMPages();
  if (!g_guestTextureLimitLogged) {
    g_guestTextureLimitLogged = true;
    eprintwarn("Guest target %dx%d exceeds GL_MAX_TEXTURE_SIZE=%d; using "
               "logical 1x fallback\n",
               extent.w, extent.h, g_maxGuestTextureSize);
  }
  return {std::max<int>(rect.w, 1), std::max<int>(rect.h, 1)};
}

static GrGuestConversionState GR_SaveGuestConversionState() {
  GrGuestConversionState state{};
  glGetIntegerv(GL_CURRENT_PROGRAM, &state.program);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &state.vertex_array);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &state.active_texture);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.texture0);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &state.read_framebuffer);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state.draw_framebuffer);
  glGetIntegerv(GL_READ_BUFFER, &state.read_buffer);
  glGetIntegerv(GL_DRAW_BUFFER, &state.draw_buffer);
  glGetIntegerv(GL_VIEWPORT, state.viewport);
  glGetIntegerv(GL_POLYGON_MODE, state.polygon_mode);
  state.blend = glIsEnabled(GL_BLEND);
  state.depth = glIsEnabled(GL_DEPTH_TEST);
  state.stencil = glIsEnabled(GL_STENCIL_TEST);
  state.scissor = glIsEnabled(GL_SCISSOR_TEST);
  state.framebuffer_srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
  state.dither = glIsEnabled(GL_DITHER);
  state.cull_face = glIsEnabled(GL_CULL_FACE);
  state.rasterizer_discard = glIsEnabled(GL_RASTERIZER_DISCARD);
  glGetBooleanv(GL_COLOR_WRITEMASK, state.color_mask);
  return state;
}

static void GR_SetGuestConversionState(GLuint source_texture) {
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_DITHER);
  glDisable(GL_CULL_FACE);
  glDisable(GL_RASTERIZER_DISCARD);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glUseProgram(g_guestVRAMConversionProgram);
  glBindVertexArray(g_guestVRAMConversionVertexArray);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, source_texture);
}

static void
GR_RestoreGuestConversionState(const GrGuestConversionState &state) {
  glBindFramebuffer(GL_READ_FRAMEBUFFER, state.read_framebuffer);
  glReadBuffer(state.read_buffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state.draw_framebuffer);
  glDrawBuffer(state.draw_buffer);
  glViewport(state.viewport[0], state.viewport[1], state.viewport[2],
             state.viewport[3]);
  glUseProgram(state.program);
  glBindVertexArray(state.vertex_array);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, state.texture0);
  glActiveTexture(state.active_texture);
  state.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
  state.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
  state.stencil ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
  state.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
  state.framebuffer_srgb ? glEnable(GL_FRAMEBUFFER_SRGB)
                         : glDisable(GL_FRAMEBUFFER_SRGB);
  state.dither ? glEnable(GL_DITHER) : glDisable(GL_DITHER);
  state.cull_face ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
  state.rasterizer_discard ? glEnable(GL_RASTERIZER_DISCARD)
                           : glDisable(GL_RASTERIZER_DISCARD);
  // Core-profile glPolygonMode accepts GL_FRONT_AND_BACK only; PsyCross
  // likewise publishes one mode for both faces.
  glPolygonMode(GL_FRONT_AND_BACK, state.polygon_mode[0]);
  glColorMask(state.color_mask[0], state.color_mask[1], state.color_mask[2],
              state.color_mask[3]);
}

static const char *guest_vram_conversion_shader = R"(
#ifdef VERTEX
varying vec2 v_uv;
void main() {
  vec2 position;
  if (gl_VertexID == 0)
    position = vec2(0.0, 0.0);
  else if (gl_VertexID == 1)
    position = vec2(2.0, 0.0);
  else
    position = vec2(0.0, 2.0);
  v_uv = position;
  gl_Position = vec4(position * 2.0 - vec2(1.0), 0.0, 1.0);
}
#endif

#ifdef FRAGMENT
varying vec2 v_uv;
uniform sampler2D u_source;
uniform int u_mode;
uniform ivec4 u_rect;

float expand5(float value) {
  return (value * 8.0 + floor(value / 4.0)) / 255.0;
}

void main() {
  int local_x = clamp(int(floor(v_uv.x * float(u_rect.z))), 0, u_rect.z - 1);
  int local_y = clamp(int(floor(v_uv.y * float(u_rect.w))), 0, u_rect.w - 1);
  if (u_mode == 0) {
    vec4 color = texelFetch(u_source,
                            ivec2(local_x, u_rect.w - local_y - 1), 0);
    float red = floor((floor(color.r * 255.0 + 0.5)) / 8.0);
    float green = floor((floor(color.g * 255.0 + 0.5)) / 8.0);
    float blue = floor((floor(color.b * 255.0 + 0.5)) / 8.0);
    float alpha = floor(color.a * 255.0 + 0.5);
    float stp = alpha > 0.0 && alpha < 192.0 ? 1.0 : 0.0;
    float encoded_sample = red + green * 32.0 + blue * 1024.0 + stp * 32768.0;
    float high = floor(encoded_sample / 256.0);
    float low = encoded_sample - high * 256.0;
    fragColor = vec4(low / 255.0, high / 255.0, 0.0, 1.0);
  } else if (u_mode == 1) {
    int source_x = (u_rect.x + local_x) % 1024;
    int source_y = (u_rect.y + u_rect.w - local_y - 1) % 512;
    source_x = source_x < 0 ? source_x + 1024 : source_x;
    source_y = source_y < 0 ? source_y + 512 : source_y;
    vec4 source_sample = texelFetch(
        u_source, ivec2(source_x, source_y), 0);
    float encoded_sample =
        floor(source_sample.r * 255.0 + 0.5) +
        floor(source_sample.g * 255.0 + 0.5) * 256.0;
    float red = mod(encoded_sample, 32.0);
    float green = mod(floor(encoded_sample / 32.0), 32.0);
    float blue = mod(floor(encoded_sample / 1024.0), 32.0);
    float alpha = encoded_sample >= 32768.0 ? (128.0 / 255.0) : 1.0;
    fragColor = vec4(expand5(red), expand5(green), expand5(blue), alpha);
  } else {
    int source_x = (u_rect.x + local_x) % 1024;
    int guest_y = (u_rect.y + u_rect.w - local_y - 1) % 512;
    source_x = source_x < 0 ? source_x + 1024 : source_x;
    guest_y = guest_y < 0 ? guest_y + 512 : guest_y;
    fragColor = texelFetch(u_source, ivec2(source_x, 511 - guest_y), 0);
  }
}
#endif
)";

static int GR_InitialiseGuestVRAMConversion() {
  g_guestVRAMConversionAvailable = false;
  GLint maximum_texture_size{};
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_texture_size);
  g_maxGuestTextureSize = std::max(maximum_texture_size, 1);
  g_guestVRAMConversionProgram =
      GR_Shader_Compile(guest_vram_conversion_shader, 0);
  if (g_guestVRAMConversionProgram == 0U)
    return 0;
  g_guestVRAMModeLocation =
      glGetUniformLocation(g_guestVRAMConversionProgram, "u_mode");
  g_guestVRAMRectLocation =
      glGetUniformLocation(g_guestVRAMConversionProgram, "u_rect");
  const GLint source_location =
      glGetUniformLocation(g_guestVRAMConversionProgram, "u_source");
  if (g_guestVRAMModeLocation == -1 || g_guestVRAMRectLocation == -1 ||
      source_location == -1) {
    GR_DestroyGuestVRAMConversion();
    return 0;
  }
  glUseProgram(g_guestVRAMConversionProgram);
  glUniform1i(source_location, 0);
  glUseProgram(0);
  glGenVertexArrays(1, &g_guestVRAMConversionVertexArray);
  if (g_guestVRAMConversionVertexArray == 0U) {
    GR_DestroyGuestVRAMConversion();
    return 0;
  }

  GLint previous_draw_framebuffer{};
  GLint previous_draw_buffer{};
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw_framebuffer);
  glGetIntegerv(GL_DRAW_BUFFER, &previous_draw_buffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);
  glDrawBuffer(GL_COLOR_ATTACHMENT0);
  bool framebuffer_complete = true;
  for (int texture = 0; texture < 2; ++texture) {
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_vramTexturesDouble[texture], 0);
    framebuffer_complete =
        framebuffer_complete && glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) ==
                                    GL_FRAMEBUFFER_COMPLETE;
  }
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, g_vramTexturesDouble[0], 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous_draw_framebuffer);
  glDrawBuffer(previous_draw_buffer);
  if (!framebuffer_complete) {
    GR_DestroyGuestVRAMConversion();
    return 0;
  }
  g_guestVRAMConversionAvailable = true;
  return 1;
}

static void GR_DestroyGuestVRAMConversion() {
  if (g_guestVRAMConversionProgram != 0U)
    glDeleteProgram(g_guestVRAMConversionProgram);
  if (g_guestVRAMConversionVertexArray != 0U)
    glDeleteVertexArrays(1, &g_guestVRAMConversionVertexArray);
  g_guestVRAMConversionProgram = 0U;
  g_guestVRAMConversionVertexArray = 0U;
  g_guestVRAMConversionAvailable = false;
}

static int GR_WrapGuestCoordinate(int value, int extent) {
  value %= extent;
  return value < 0 ? value + extent : value;
}

static bool GR_DecodeGuestColorLinearRect(int x, int y, int width, int height) {
  if (g_guestColorTexture == 0U || g_glGuestColorFramebuffer == 0U)
    return false;
  if (!GR_IsGuestColorDirty(x, y, width, height))
    return true;
  if (!g_guestVRAMConversionAvailable)
    return false;

  GR_UpdateVRAM();
  const auto state = GR_SaveGuestConversionState();
  GR_SetGuestConversionState(g_vramTexturesDouble[0]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glGuestColorFramebuffer);
  glDrawBuffer(GL_COLOR_ATTACHMENT0);
  glUniform1i(g_guestVRAMModeLocation, 1);

  std::vector<std::pair<int, int>> active_runs;
  int active_y = y;
  const int x1 = x + width;
  const int y1 = y + height;
  for (int row = y; row <= y1; ++row) {
    std::vector<std::pair<int, int>> row_runs;
    if (row < y1) {
      int column = x;
      while (column < x1) {
        while (column < x1 && !GR_IsGuestColorPixelDirty(column, row))
          ++column;
        const int run_x = column;
        while (column < x1 && GR_IsGuestColorPixelDirty(column, row))
          ++column;
        if (run_x < column)
          row_runs.emplace_back(run_x, column);
      }
    }
    if (row_runs == active_runs)
      continue;

    const int run_height = row - active_y;
    for (const auto &run : active_runs) {
      const int run_width = run.second - run.first;
      glViewport(run.first, VRAM_HEIGHT - (active_y + run_height), run_width,
                 run_height);
      glUniform4i(g_guestVRAMRectLocation, run.first, active_y, run_width,
                  run_height);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      GR_ClearGuestColorDirty(run.first, active_y, run_width, run_height);
    }
    active_runs = std::move(row_runs);
    active_y = row;
  }
  GR_RestoreGuestConversionState(state);
  return true;
}

static bool GR_SynchronizeGuestColorRect(int x, int y, int width, int height) {
  if (width <= 0 || height <= 0 || width > VRAM_WIDTH || height > VRAM_HEIGHT ||
      g_guestColorTexture == 0U || g_glGuestColorFramebuffer == 0U)
    return false;
  x = GR_WrapGuestCoordinate(x, VRAM_WIDTH);
  y = GR_WrapGuestCoordinate(y, VRAM_HEIGHT);
  const int first_width = std::min(width, VRAM_WIDTH - x);
  const int first_height = std::min(height, VRAM_HEIGHT - y);
  const int second_width = width - first_width;
  const int second_height = height - first_height;
  const int xs[2] = {x, 0};
  const int widths[2] = {first_width, second_width};
  const int ys[2] = {y, 0};
  const int heights[2] = {first_height, second_height};
  for (int yi = 0; yi < 2; ++yi) {
    if (heights[yi] <= 0)
      continue;
    for (int xi = 0; xi < 2; ++xi) {
      if (widths[xi] > 0 && !GR_DecodeGuestColorLinearRect(
                                xs[xi], ys[yi], widths[xi], heights[yi]))
        return false;
    }
  }
  return true;
}

static bool GR_CaptureGuestColorFromOffscreen(const RECT16 &rect,
                                              int pixel_width,
                                              int pixel_height) {
  if (g_guestColorTexture == 0U || g_glGuestColorFramebuffer == 0U ||
      rect.x < 0 || rect.y < 0 || rect.w <= 0 || rect.h <= 0 ||
      rect.x + rect.w > VRAM_WIDTH || rect.y + rect.h > VRAM_HEIGHT)
    return false;
  GLint read_framebuffer{}, draw_framebuffer{}, read_buffer{}, draw_buffer{};
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
  glGetIntegerv(GL_READ_BUFFER, &read_buffer);
  glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glOffscreenFramebuffer);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glGuestColorFramebuffer);
  glDrawBuffer(GL_COLOR_ATTACHMENT0);
  glBlitFramebuffer(0, 0, pixel_width, pixel_height, rect.x,
                    VRAM_HEIGHT - (rect.y + rect.h), rect.x + rect.w,
                    VRAM_HEIGHT - rect.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  GR_ClearGuestColorDirty(rect.x, rect.y, rect.w, rect.h);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read_framebuffer);
  glReadBuffer(read_buffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_framebuffer);
  glDrawBuffer(draw_buffer);
  if (scissor)
    glEnable(GL_SCISSOR_TEST);
  return true;
}

static bool GR_UnpackGuestVRAMToOffscreen(const RECT16 &rect, int pixel_width,
                                          int pixel_height) {
  if (GR_SynchronizeGuestColorRect(rect.x, rect.y, rect.w, rect.h)) {
    const int source_x = GR_WrapGuestCoordinate(rect.x, VRAM_WIDTH);
    const int source_y = GR_WrapGuestCoordinate(rect.y, VRAM_HEIGHT);
    if (source_x + rect.w <= VRAM_WIDTH && source_y + rect.h <= VRAM_HEIGHT) {
      const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
      glDisable(GL_SCISSOR_TEST);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glGuestColorFramebuffer);
      glReadBuffer(GL_COLOR_ATTACHMENT0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glOffscreenFramebuffer);
      glDrawBuffer(GL_COLOR_ATTACHMENT0);
      glBlitFramebuffer(source_x, VRAM_HEIGHT - (source_y + rect.h),
                        source_x + rect.w, VRAM_HEIGHT - source_y, 0, 0,
                        pixel_width, pixel_height, GL_COLOR_BUFFER_BIT,
                        GL_NEAREST);
      glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);
      if (scissor)
        glEnable(GL_SCISSOR_TEST);
      return true;
    }
    if (g_guestVRAMConversionAvailable) {
      const auto state = GR_SaveGuestConversionState();
      GR_SetGuestConversionState(g_guestColorTexture);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glOffscreenFramebuffer);
      glDrawBuffer(GL_COLOR_ATTACHMENT0);
      glViewport(0, 0, pixel_width, pixel_height);
      glUniform1i(g_guestVRAMModeLocation, 2);
      glUniform4i(g_guestVRAMRectLocation, rect.x, rect.y, rect.w, rect.h);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      GR_RestoreGuestConversionState(state);
      return true;
    }
  }
  if (!g_guestVRAMConversionAvailable)
    return false;
  GR_UpdateVRAM();
  const auto state = GR_SaveGuestConversionState();
  GR_SetGuestConversionState(g_vramTexturesDouble[0]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glOffscreenFramebuffer);
  glDrawBuffer(GL_COLOR_ATTACHMENT0);
  glViewport(0, 0, pixel_width, pixel_height);
  glUniform1i(g_guestVRAMModeLocation, 1);
  glUniform4i(g_guestVRAMRectLocation, rect.x, rect.y, rect.w, rect.h);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  GR_RestoreGuestConversionState(state);
  return true;
}

static bool GR_ResolveGuestColorScanout(int x, int y, int width, int height) {
  if (!GR_SynchronizeGuestColorRect(x, y, width, height) ||
      g_glGuestScanoutFramebuffer == 0U)
    return false;

  if (g_guestScanoutTexture == 0U || g_guestScanoutWidth != width ||
      g_guestScanoutHeight != height) {
    const auto state = GR_SaveGuestConversionState();
    if (g_guestScanoutTexture == 0U) {
      glGenTextures(1, &g_guestScanoutTexture);
      glBindTexture(GL_TEXTURE_2D, g_guestScanoutTexture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
      glBindTexture(GL_TEXTURE_2D, g_guestScanoutTexture);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glGuestScanoutFramebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_guestScanoutTexture, 0);
    const bool complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) ==
                          GL_FRAMEBUFFER_COMPLETE;
    GR_RestoreGuestConversionState(state);
    if (!complete)
      return false;
    g_guestScanoutWidth = width;
    g_guestScanoutHeight = height;
  }

  const int source_x = GR_WrapGuestCoordinate(x, VRAM_WIDTH);
  const int source_y = GR_WrapGuestCoordinate(y, VRAM_HEIGHT);
  const int first_width = std::min(width, VRAM_WIDTH - source_x);
  const int first_height = std::min(height, VRAM_HEIGHT - source_y);
  const int second_width = width - first_width;
  const int second_height = height - first_height;
  const int source_xs[2] = {source_x, 0};
  const int source_widths[2] = {first_width, second_width};
  const int destination_xs[2] = {0, first_width};
  const int source_ys[2] = {source_y, 0};
  const int source_heights[2] = {first_height, second_height};
  const int destination_ys[2] = {second_height, 0};
  const GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glGuestColorFramebuffer);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glGuestScanoutFramebuffer);
  glDrawBuffer(GL_COLOR_ATTACHMENT0);
  for (int yi = 0; yi < 2; ++yi) {
    if (source_heights[yi] <= 0)
      continue;
    for (int xi = 0; xi < 2; ++xi) {
      if (source_widths[xi] <= 0)
        continue;
      glBlitFramebuffer(
          source_xs[xi], VRAM_HEIGHT - (source_ys[yi] + source_heights[yi]),
          source_xs[xi] + source_widths[xi], VRAM_HEIGHT - source_ys[yi],
          destination_xs[xi], destination_ys[yi],
          destination_xs[xi] + source_widths[xi],
          destination_ys[yi] + source_heights[yi], GL_COLOR_BUFFER_BIT,
          GL_NEAREST);
    }
  }
  if (scissor_enabled)
    glEnable(GL_SCISSOR_TEST);
  return true;
}

static bool GR_ResolvePackedGuestScanout(int x, int y, int width, int height) {
  if (!g_guestVRAMConversionAvailable || width <= 0 || height <= 0 ||
      width > VRAM_WIDTH || height > VRAM_HEIGHT)
    return false;

  GR_UpdateVRAM();
  const auto state = GR_SaveGuestConversionState();
  if (g_guestScanoutTexture == 0U) {
    glGenTextures(1, &g_guestScanoutTexture);
  }
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_guestScanoutTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (g_guestScanoutWidth != width || g_guestScanoutHeight != height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    g_guestScanoutWidth = width;
    g_guestScanoutHeight = height;
  }

  GR_SetGuestConversionState(g_vramTexturesDouble[0]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glHighResolutionVRAMFramebuffer);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, g_guestScanoutTexture, 0);
  glDrawBuffer(GL_COLOR_ATTACHMENT0);
  if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) !=
      GL_FRAMEBUFFER_COMPLETE) {
    GR_RestoreGuestConversionState(state);
    return false;
  }
  glViewport(0, 0, width, height);
  glUniform1i(g_guestVRAMModeLocation, 1);
  glUniform4i(g_guestVRAMRectLocation, x, y, width, height);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  GR_RestoreGuestConversionState(state);
  return true;
}

static bool
GR_SynchronizeHighResolutionVRAMPage(GrHighResolutionVRAMPage &page) {
  if (!page.valid || page.texture == 0U)
    return false;

  const auto current_sequence = GR_GetVRAMWriteSequence();
  if (page.synced_write_sequence >= current_sequence)
    return true;
  if (!g_guestVRAMConversionAvailable) {
    page.valid = false;
    return false;
  }

  if (!GR_IsVRAMDirtyRect(page.pending_writes, page.rect.x, page.rect.y,
                          page.rect.w, page.rect.h)) {
    page.synced_write_sequence = current_sequence;
    return true;
  }

  GR_UpdateVRAM();
  const auto state = GR_SaveGuestConversionState();
  GR_SetGuestConversionState(g_vramTexturesDouble[0]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glHighResolutionVRAMFramebuffer);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, page.texture, 0);
  glDrawBuffer(GL_COLOR_ATTACHMENT0);
  if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) !=
      GL_FRAMEBUFFER_COMPLETE) {
    GR_RestoreGuestConversionState(state);
    page.valid = false;
    return false;
  }

  const auto refresh_rect = [&page](int x, int y, int width, int height) {
    const int intersection_x0 = std::max<int>(page.rect.x, x);
    const int intersection_y0 = std::max<int>(page.rect.y, y);
    const int intersection_x1 =
        std::min<int>(page.rect.x + page.rect.w, x + width);
    const int intersection_y1 =
        std::min<int>(page.rect.y + page.rect.h, y + height);
    if (intersection_x0 >= intersection_x1 ||
        intersection_y0 >= intersection_y1) {
      return;
    }

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
    glViewport(destination_x0, destination_y0,
               std::max(destination_x1 - destination_x0, 1),
               std::max(destination_y1 - destination_y0, 1));
    glUniform1i(g_guestVRAMModeLocation, 1);
    glUniform4i(g_guestVRAMRectLocation, intersection_x0, intersection_y0,
                intersection_x1 - intersection_x0,
                intersection_y1 - intersection_y0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  };

  std::vector<std::array<int, 2>> active_runs;
  std::vector<std::array<int, 2>> row_runs;
  int active_first_row = page.rect.y;
  for (int row = page.rect.y; row <= page.rect.y + page.rect.h; ++row) {
    row_runs.clear();
    if (row < page.rect.y + page.rect.h) {
      GR_CollectVRAMDirtyRuns(page.pending_writes, row, row_runs);
      row_runs.erase(std::remove_if(row_runs.begin(), row_runs.end(),
                                    [&page](const auto &run) {
                                      return run[1] <= page.rect.x ||
                                             run[0] >=
                                                 page.rect.x + page.rect.w;
                                    }),
                     row_runs.end());
      for (auto &run : row_runs) {
        run[0] = std::max<int>(run[0], page.rect.x);
        run[1] = std::min<int>(run[1], page.rect.x + page.rect.w);
      }
    }
    if (row_runs == active_runs)
      continue;
    const int row_count = row - active_first_row;
    for (const auto &run : active_runs)
      refresh_rect(run[0], active_first_row, run[1] - run[0], row_count);
    active_runs = row_runs;
    active_first_row = row;
  }

  GR_ClearVRAMDirtyRect(page.pending_writes, page.rect.x, page.rect.y,
                        page.rect.w, page.rect.h);
  GR_RestoreGuestConversionState(state);
  page.synced_write_sequence = current_sequence;
  return true;
}

static bool GR_PackGuestFramebufferToVRAM(const RECT16 &rect, int pixel_width,
                                          int pixel_height) {
  if (!g_guestVRAMConversionAvailable || rect.w <= 0 || rect.h <= 0 ||
      rect.w > g_guestFeedbackWidth || rect.h > g_guestFeedbackHeight)
    return false;

  ++g_guestVRAMPackCount;
  g_guestVRAMPackPixels += static_cast<unsigned long long>(pixel_width) *
                           static_cast<unsigned long long>(pixel_height);
  GR_UpdateVRAM();
  const auto state = GR_SaveGuestConversionState();
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glOffscreenFramebuffer);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glGuestFeedbackFramebuffer);
  glDrawBuffer(GL_COLOR_ATTACHMENT0);
  glBlitFramebuffer(0, 0, pixel_width, pixel_height, 0, 0, rect.w, rect.h,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);

  GR_SetGuestConversionState(g_guestFeedbackTexture);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);
  glDrawBuffer(GL_COLOR_ATTACHMENT0);
  glViewport(rect.x, rect.y, rect.w, rect.h);
  glUniform1i(g_guestVRAMModeLocation, 0);
  glUniform4i(g_guestVRAMRectLocation, 0, 0, rect.w, rect.h);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, g_vramTexturesDouble[0], 0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, g_vramTexturesDouble[0], 0);
  GR_RestoreGuestConversionState(state);

  g_vramTexture = g_vramTexturesDouble[0];
  g_vramTextureIdx = 0;
  GR_RecordVRAMWrite(GR_VRAM_WRITE_FRAMEBUFFER, rect.x, rect.y, rect.x, rect.y,
                     rect.w, rect.h, false);
  GR_MarkGPUVRAMDirty(rect.x, rect.y, rect.w, rect.h);
  return true;
}

// GP0(80) is a VRAM-to-VRAM operation. Keep the packed PS1 word domain on
// the GPU instead of synchronously downloading the source into the CPU
// shadow and uploading it again. Texture 1 is a temporary snapshot so
// overlapping copies retain memmove semantics; the final blit mirrors the
// destination back into both resident VRAM textures.
static bool GR_CopyGPUVRAMRect(int source_x, int source_y, int width,
                               int height, int destination_x,
                               int destination_y) {
  if (!g_guestVRAMConversionAvailable || width <= 0 || height <= 0 ||
      source_x < 0 || source_y < 0 || destination_x < 0 || destination_y < 0 ||
      source_x + width > VRAM_WIDTH || source_y + height > VRAM_HEIGHT ||
      destination_x + width > VRAM_WIDTH ||
      destination_y + height > VRAM_HEIGHT) {
    return false;
  }

  // Publish any pending CPU writes before taking the source snapshot.
  GR_UpdateVRAM();
  const auto state = GR_SaveGuestConversionState();
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glVRAMFramebuffer);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glHighResolutionVRAMFramebuffer);
  glDrawBuffer(GL_COLOR_ATTACHMENT0);

  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, g_vramTexturesDouble[0], 0);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, g_vramTexturesDouble[1], 0);
  const bool complete =
      glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) ==
          GL_FRAMEBUFFER_COMPLETE &&
      glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (!complete) {
    GR_RestoreGuestConversionState(state);
    return false;
  }

  // Snapshot source at its original coordinates.
  glBlitFramebuffer(source_x, source_y, source_x + width, source_y + height,
                    source_x, source_y, source_x + width, source_y + height,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);

  // Copy the stable snapshot into texture 0's destination.
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glHighResolutionVRAMFramebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);
  glBlitFramebuffer(source_x, source_y, source_x + width, source_y + height,
                    destination_x, destination_y, destination_x + width,
                    destination_y + height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  // Keep texture 1 a complete mirror for later CPU uploads/FBO uses.
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glVRAMFramebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glHighResolutionVRAMFramebuffer);
  glBlitFramebuffer(destination_x, destination_y, destination_x + width,
                    destination_y + height, destination_x, destination_y,
                    destination_x + width, destination_y + height,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
  GR_RestoreGuestConversionState(state);

  g_vramTexture = g_vramTexturesDouble[0];
  g_vramTextureIdx = 0;
  GR_RecordVRAMWrite(GR_VRAM_WRITE_MOVE, source_x, source_y, destination_x,
                     destination_y, width, height);
  GR_MarkGPUVRAMDirty(destination_x, destination_y, width, height);
  return true;
}

static void GR_SynchronizeGPUVRAMRect(int x, int y, int width, int height) {
  if (!GR_ClipGuestVRAMRect(x, y, width, height) ||
      !GR_IsGPUVRAMDirty(x, y, width, height)) {
    return;
  }
  GR_UpdateVRAM();

  GLint previous_read_framebuffer{};
  GLint previous_pack_buffer{};
  GLint previous_pack_row_length{};
  GLint previous_pack_image_height{};
  GLint previous_pack_skip_pixels{};
  GLint previous_pack_skip_rows{};
  GLint previous_pack_alignment{};
  GLboolean previous_pack_swap_bytes{};
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_framebuffer);
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous_pack_buffer);
  glGetIntegerv(GL_PACK_ROW_LENGTH, &previous_pack_row_length);
  glGetIntegerv(GL_PACK_IMAGE_HEIGHT, &previous_pack_image_height);
  glGetIntegerv(GL_PACK_SKIP_PIXELS, &previous_pack_skip_pixels);
  glGetIntegerv(GL_PACK_SKIP_ROWS, &previous_pack_skip_rows);
  glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
  glGetBooleanv(GL_PACK_SWAP_BYTES, &previous_pack_swap_bytes);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
  glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_PACK_SKIP_ROWS, 0);
  glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glVRAMFramebuffer);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, g_vramTexturesDouble[0], 0);
  if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) ==
      GL_FRAMEBUFFER_COMPLETE) {
    glPixelStorei(GL_PACK_ROW_LENGTH, VRAM_WIDTH);
    glPixelStorei(GL_PACK_ALIGNMENT, 2);
    glReadPixels(x, y, width, height, VRAM_FORMAT, GL_UNSIGNED_BYTE,
                 vram + y * VRAM_WIDTH + x);
    GR_ClearGPUVRAMDirty(x, y, width, height);
  } else {
    GLint previous_active_texture{};
    GLint previous_texture{};
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[0]);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 2);
    glGetTexImage(GL_TEXTURE_2D, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, vram);
    glBindTexture(GL_TEXTURE_2D, previous_texture);
    glActiveTexture(previous_active_texture);
    for (auto &row : g_gpuDirtyVRAM)
      row.fill(0U);
  }
  ++g_synchronousVRAMReadbacks;
  glPixelStorei(GL_PACK_ROW_LENGTH, previous_pack_row_length);
  glPixelStorei(GL_PACK_IMAGE_HEIGHT, previous_pack_image_height);
  glPixelStorei(GL_PACK_SKIP_PIXELS, previous_pack_skip_pixels);
  glPixelStorei(GL_PACK_SKIP_ROWS, previous_pack_skip_rows);
  glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
  glPixelStorei(GL_PACK_SWAP_BYTES, previous_pack_swap_bytes);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, previous_pack_buffer);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read_framebuffer);
}

#else

static bool g_guestRenderLogicalFallback{};

static void GR_ClearGPUVRAMDirty(int, int, int, int) {}
static void GR_SynchronizeGPUVRAMRect(int, int, int, int) {}
static bool GR_CopyGPUVRAMRect(int, int, int, int, int, int) { return false; }
static void GR_RefreshGuestRenderConfiguration() {}
static void GR_ApplyPendingGuestDisplayGeometry() {}
static void GR_InvalidateAllHighResolutionVRAMPages() {
  for (auto &page : g_highResolutionVRAMPages)
    page.valid = false;
}
static int GR_MapGuestEdge(int logical_edge, int logical_extent,
                           int pixel_extent) {
  if (logical_extent <= 0 || pixel_extent <= 0)
    return 0;
  const auto numerator = static_cast<long long>(logical_edge) * pixel_extent;
  return static_cast<int>((numerator + logical_extent / 2) / logical_extent);
}
static GrGuestPixelExtent GR_CalculateOffscreenPixelExtent(const RECT16 &rect) {
  return {std::max<int>(rect.w, 1), std::max<int>(rect.h, 1)};
}

#endif

} // namespace

int GR_PresentComposedGuestScanout(int x, int y, int width, int height) {
#if defined(RENDERER_OGL)
  if (g_cfg_composedGuestScanout == 0 || g_nativeFramebufferWidth <= 0 ||
      g_nativeFramebufferHeight <= 0 || width <= 0 || height <= 0) {
    return 0;
  }

  const int source_x = GR_WrapGuestCoordinate(x, VRAM_WIDTH);
  const int source_y = GR_WrapGuestCoordinate(y, VRAM_HEIGHT);
  const bool nonwrapped =
      source_x + width <= VRAM_WIDTH && source_y + height <= VRAM_HEIGHT;
  const bool sidecar_ready =
      GR_SynchronizeGuestColorRect(source_x, source_y, width, height);
  const bool direct_sidecar =
      sidecar_ready && nonwrapped && g_cfg_bilinearFiltering == 0 &&
      g_cfg_trilinearFiltering == 0 && g_cfg_anisotropicFiltering == 0;
  const bool scanout_sidecar = sidecar_ready && !direct_sidecar &&
                               GR_ResolveGuestColorScanout(x, y, width, height);
  const bool packed_fallback =
      !direct_sidecar && !scanout_sidecar &&
      GR_ResolvePackedGuestScanout(x, y, width, height);
  if (!direct_sidecar && !scanout_sidecar && !packed_fallback)
    return 0;

  const PsyXPresentationViewport destination = PsyX_GetRenderViewport();
  const GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  if (direct_sidecar) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glGuestColorFramebuffer);
  } else if (scanout_sidecar) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glGuestScanoutFramebuffer);
  } else {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glHighResolutionVRAMFramebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_guestScanoutTexture, 0);
  }
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glNativeFramebuffer);
  const int blit_x0 = direct_sidecar ? source_x : 0;
  const int blit_y0 = direct_sidecar ? VRAM_HEIGHT - (source_y + height) : 0;
  const int blit_x1 = direct_sidecar ? source_x + width : width;
  const int blit_y1 = direct_sidecar ? VRAM_HEIGHT - source_y : height;
  glBlitFramebuffer(blit_x0, blit_y0, blit_x1, blit_y1, destination.x,
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
  ++g_guestScanoutPresentCount;
  return 1;
#else
  (void)x;
  (void)y;
  (void)width;
  (void)height;
  return 0;
#endif
}

unsigned long long GR_GetGuestScanoutPresentCount(void) {
#if defined(RENDERER_OGL)
  return g_guestScanoutPresentCount;
#else
  return 0U;
#endif
}

PsyXPresentationViewport PsyX_CalculateGuestRenderExtent(
    int outputWidth, int outputHeight, int aspectMode, int logicalWidth,
    int logicalHeight, int canonicalWidth, int canonicalHeight, int rootPage) {
  const auto target = PsyX_CalculatePresentationViewport(
      std::max(outputWidth, 1), std::max(outputHeight, 1), aspectMode);
  logicalWidth = std::max(logicalWidth, 1);
  logicalHeight = std::max(logicalHeight, 1);
  canonicalWidth = std::max(canonicalWidth, 1);
  canonicalHeight = std::max(canonicalHeight, 1);

  // Root identity comes from the guest-submit boundary, never a transient GP1
  // width heuristic. Contained pages are resolved against their retained
  // parent before this helper is called.
  if (rootPage != 0)
    return {0, 0, std::max(target.w, logicalWidth),
            std::max(target.h, logicalHeight)};

  const auto scaled_width =
      (static_cast<long long>(target.w) * logicalWidth + canonicalWidth / 2) /
      canonicalWidth;
  const auto scaled_height =
      (static_cast<long long>(target.h) * logicalHeight + canonicalHeight / 2) /
      canonicalHeight;
  return {0, 0, std::max<int>(static_cast<int>(scaled_width), logicalWidth),
          std::max<int>(static_cast<int>(scaled_height), logicalHeight)};
}

void GR_SetGuestDisplayGeometry(int width, int height) {
#if defined(RENDERER_OGL)
  const int next_width = std::min(std::max(width, 1), VRAM_WIDTH);
  const int next_height = std::min(std::max(height, 1), VRAM_HEIGHT);
  if (next_width == g_pendingGuestDisplayWidth &&
      next_height == g_pendingGuestDisplayHeight) {
    return;
  }

  // Publish a stable geometry transition transactionally. Raw catch-up
  // submits can leave vertices batched without presenting, so flush them while
  // the old canonical domain is still current, then pack/close that old page.
  DrawSync(0);
  if (g_PreviousOffscreenState != 0) {
    RECT16 completed_page{};
    GR_SetOffscreenState(&completed_page, 0);
  }
  g_pendingGuestDisplayWidth = next_width;
  g_pendingGuestDisplayHeight = next_height;
#else
  (void)width;
  (void)height;
#endif
}

void GR_BeginGuestSubmit(void) {
#if defined(RENDERER_OGL)
  // A presented frame or a transactional geometry change has closed its prior
  // page. Catch-up submits with unchanged geometry may keep a page active, so
  // they must not relabel the first contained switch as another root.
  g_nextOffscreenIsRoot = g_PreviousOffscreenState == 0;
#endif
}

unsigned long long GR_GetSynchronousVRAMReadbackCount(void) {
#if defined(RENDERER_OGL)
  return g_synchronousVRAMReadbacks;
#else
  return 0U;
#endif
}

unsigned long long GR_GetGuestVRAMPackCount(void) {
#if defined(RENDERER_OGL)
  return g_guestVRAMPackCount;
#else
  return 0U;
#endif
}
unsigned long long GR_GetGuestVRAMPackPixels(void) {
#if defined(RENDERER_OGL)
  return g_guestVRAMPackPixels;
#else
  return 0U;
#endif
}
unsigned long long GR_GetGuestSeedCount(void) {
#if defined(RENDERER_OGL)
  return g_guestSeedCount;
#else
  return 0U;
#endif
}
unsigned long long GR_GetGuestSeedPixels(void) {
#if defined(RENDERER_OGL)
  return g_guestSeedPixels;
#else
  return 0U;
#endif
}
unsigned long long GR_GetGuestCaptureCount(void) {
#if defined(RENDERER_OGL)
  return g_guestCaptureCount;
#else
  return 0U;
#endif
}
unsigned long long GR_GetGuestCapturePixels(void) {
#if defined(RENDERER_OGL)
  return g_guestCapturePixels;
#else
  return 0U;
#endif
}
