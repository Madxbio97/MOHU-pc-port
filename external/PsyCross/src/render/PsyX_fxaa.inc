#if defined(RENDERER_OGL)

namespace {

struct PsyXFXAAResources {
  GLuint program{};
  GLuint framebuffer{};
  GLuint vertexArray{};
  GLuint outputTexture{};
  GLint metrics{-1};
  GLint sourceRect{-1};
  int width{};
  int height{};
  bool available{};
};

PsyXFXAAResources g_fxaa;

constexpr const char *kFXAAShader = R"PSYX(
#ifdef VERTEX
varying vec2 v_texcoord;
uniform vec4 u_source_rect;
void main() {
  vec2 position = gl_VertexID == 0 ? vec2(0.0, 0.0)
                : gl_VertexID == 1 ? vec2(2.0, 0.0)
                                   : vec2(0.0, 2.0);
  v_texcoord = u_source_rect.xy + position * u_source_rect.zw;
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
#else
varying vec2 v_texcoord;
uniform sampler2D u_color;
uniform vec4 u_metrics;
uniform vec4 u_source_rect;

vec2 clampFXAA(vec2 uv) {
  vec2 inset = u_metrics.xy * 0.5;
  return clamp(uv, u_source_rect.xy + inset,
               u_source_rect.xy + u_source_rect.zw - inset);
}

vec3 sampleFXAA(vec2 uv) {
  return texture2D(u_color, clampFXAA(uv)).rgb;
}

float lumaFXAA(vec3 color) {
  return dot(color, vec3(0.299, 0.587, 0.114));
}

void main() {
  vec3 rgbM = sampleFXAA(v_texcoord);
  vec3 rgbNW = sampleFXAA(v_texcoord + vec2(-1.0, -1.0) * u_metrics.xy);
  vec3 rgbNE = sampleFXAA(v_texcoord + vec2(1.0, -1.0) * u_metrics.xy);
  vec3 rgbSW = sampleFXAA(v_texcoord + vec2(-1.0, 1.0) * u_metrics.xy);
  vec3 rgbSE = sampleFXAA(v_texcoord + vec2(1.0, 1.0) * u_metrics.xy);

  float lumaM = lumaFXAA(rgbM);
  float lumaNW = lumaFXAA(rgbNW);
  float lumaNE = lumaFXAA(rgbNE);
  float lumaSW = lumaFXAA(rgbSW);
  float lumaSE = lumaFXAA(rgbSE);
  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  float lumaRange = lumaMax - lumaMin;

  if (lumaRange < max(0.0312, lumaMax * 0.125)) {
    fragColor = vec4(rgbM, 1.0);
    return;
  }

  vec2 direction;
  direction.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
  direction.y = (lumaNW + lumaSW) - (lumaNE + lumaSE);
  float directionReduce =
      max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 0.0078125);
  float reciprocalMinimum =
      1.0 / (min(abs(direction.x), abs(direction.y)) + directionReduce);
  direction = clamp(direction * reciprocalMinimum, vec2(-8.0), vec2(8.0)) *
              u_metrics.xy;

  vec3 rgbA = 0.5 * (
      sampleFXAA(v_texcoord + direction * (1.0 / 3.0 - 0.5)) +
      sampleFXAA(v_texcoord + direction * (2.0 / 3.0 - 0.5)));
  vec3 rgbB = rgbA * 0.5 + 0.25 * (
      sampleFXAA(v_texcoord + direction * -0.5) +
      sampleFXAA(v_texcoord + direction * 0.5));
  float lumaB = lumaFXAA(rgbB);
  fragColor = vec4(lumaB < lumaMin || lumaB > lumaMax ? rgbA : rgbB, 1.0);
}
#endif
)PSYX";

void PsyX_SetFXAATexture(GLuint texture, int width, int height) {
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
}

void PsyX_DestroyFXAA() {
  if (g_fxaa.program != 0)
    glDeleteProgram(g_fxaa.program);
  glDeleteFramebuffers(1, &g_fxaa.framebuffer);
  glDeleteVertexArrays(1, &g_fxaa.vertexArray);
  glDeleteTextures(1, &g_fxaa.outputTexture);
  g_fxaa = {};
}

void PsyX_InitialiseFXAA() {
  if (!g_cfg_fxaa)
    return;

  g_fxaa.program = GR_Shader_Compile(kFXAAShader, 0);
  GLint linked = GL_FALSE;
  glGetProgramiv(g_fxaa.program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    eprintwarn("FXAA is unavailable; continuing without it\n");
    PsyX_DestroyFXAA();
    g_cfg_fxaa = 0;
    return;
  }

  g_fxaa.metrics = glGetUniformLocation(g_fxaa.program, "u_metrics");
  g_fxaa.sourceRect = glGetUniformLocation(g_fxaa.program, "u_source_rect");
  glUseProgram(g_fxaa.program);
  glUniform1i(glGetUniformLocation(g_fxaa.program, "u_color"), 0);
  glUseProgram(0);
  glGenFramebuffers(1, &g_fxaa.framebuffer);
  glGenVertexArrays(1, &g_fxaa.vertexArray);
  glGenTextures(1, &g_fxaa.outputTexture);
  g_fxaa.available = true;
  eprintf("*FXAA: enabled (quality preset)\n");
}

bool PsyX_EnsureFXAATarget(int width, int height) {
  if (g_fxaa.width == width && g_fxaa.height == height)
    return true;
  if (width <= 0 || height <= 0)
    return false;

  PsyX_SetFXAATexture(g_fxaa.outputTexture, width, height);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, g_fxaa.framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g_fxaa.outputTexture, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    eprintwarn("FXAA framebuffer is incomplete; disabling FXAA\n");
    g_fxaa.available = false;
    g_cfg_fxaa = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());
    return false;
  }
  g_fxaa.width = width;
  g_fxaa.height = height;
  return true;
}

} // namespace

void GR_ApplySceneFXAA(void) {
  if (!g_cfg_fxaa || !g_fxaa.available || g_nativeFramebufferWidth <= 0 ||
      g_nativeFramebufferHeight <= 0 ||
      !PsyX_EnsureFXAATarget(g_nativeFramebufferWidth,
                             g_nativeFramebufferHeight)) {
    return;
  }

  const int savedBlendMode = g_PreviousBlendMode;
  const int savedDepthMode = g_PreviousDepthMode;
  const int savedDepthWrite = g_PreviousDepthWrite;
  const int savedStencilMode = g_PreviousStencilMode;
  const GLboolean savedStencilTest = glIsEnabled(GL_STENCIL_TEST);
  const GLboolean savedScissorTest = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean savedFramebufferSrgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
  GLint savedViewport[4]{};
  glGetIntegerv(GL_VIEWPORT, savedViewport);

  PsyX_ResolveNativeFramebuffer();
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  const PsyXPresentationViewport viewport = PsyX_GetRenderViewport();
  const float width = static_cast<float>(g_nativeFramebufferWidth);
  const float height = static_cast<float>(g_nativeFramebufferHeight);
  const float metrics[4]{1.0F / width, 1.0F / height, width, height};
  const float sourceRect[4]{
      static_cast<float>(viewport.x) / width,
      static_cast<float>(viewport.y) / height,
      static_cast<float>(viewport.w) / width,
      static_cast<float>(viewport.h) / height,
  };

  glBindFramebuffer(GL_FRAMEBUFFER, g_fxaa.framebuffer);
  glViewport(0, 0, g_nativeFramebufferWidth, g_nativeFramebufferHeight);
  glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  glViewport(viewport.x, viewport.y, viewport.w, viewport.h);
  glUseProgram(g_fxaa.program);
  glUniform4fv(g_fxaa.metrics, 1, metrics);
  glUniform4fv(g_fxaa.sourceRect, 1, sourceRect);
  glBindVertexArray(g_fxaa.vertexArray);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_glNativeColorTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fxaa.framebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glNativeFramebuffer);
  glBlitFramebuffer(viewport.x, viewport.y, viewport.x + viewport.w,
                    viewport.y + viewport.h, viewport.x, viewport.y,
                    viewport.x + viewport.w, viewport.y + viewport.h,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
  g_nativeFramePostprocessed = 1;
  glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeFramebuffer);

  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
  glBindVertexArray(0);
  glViewport(savedViewport[0], savedViewport[1], savedViewport[2],
             savedViewport[3]);
  if (savedScissorTest)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  if (savedFramebufferSrgb)
    glEnable(GL_FRAMEBUFFER_SRGB);
  else
    glDisable(GL_FRAMEBUFFER_SRGB);
  if (savedStencilTest)
    glEnable(GL_STENCIL_TEST);
  else
    glDisable(GL_STENCIL_TEST);

  g_PreviousBlendMode = BM_NONE;
  if (savedBlendMode != BM_NONE)
    GR_SetBlendMode(static_cast<BlendMode>(savedBlendMode));
  g_PreviousDepthMode = -1;
  g_PreviousDepthWrite = -1;
  g_PreviousDepthFunc = -1;
  GR_SetDepthState(savedDepthMode, savedDepthWrite);
  g_PreviousStencilMode = savedStencilMode == 0 ? 1 : 0;
  GR_SetStencilMode(savedStencilMode);
  g_PreviousScissorState = savedScissorTest ? 1 : 0;
  g_PreviousShader = -1;
  g_PreviousTextureBlendShader = -1;
  g_PreviousTextureBlendMode = -1;
  g_lastBoundTexture = -1;
}

#else

namespace {
void PsyX_InitialiseFXAA() {}
void PsyX_DestroyFXAA() {}
} // namespace
void GR_ApplySceneFXAA(void) {}

#endif

void GR_ApplySceneAntialiasing(void) {
  if (g_cfg_smaa)
    GR_ApplySceneSMAA();
  else if (g_cfg_fxaa)
    GR_ApplySceneFXAA();
}