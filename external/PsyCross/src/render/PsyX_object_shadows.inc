#if defined(RENDERER_OGL)

namespace {

constexpr int kObjectShadowResolution = 512;
constexpr int kObjectShadowMaximumCasters = 16;
// Keep the implementation available for later tuning, but do not allocate or
// submit scene-object shadows in the current release.
constexpr bool kObjectShadowsEnabled = false;

struct PsyXObjectShadowResources {
  GLuint depthProgram = 0;
  GLuint maskProgram = 0;
  GLuint compositeProgram = 0;
  GLuint shadowFramebuffer = 0;
  GLuint shadowTexture = 0;
  GLuint maskFramebuffer = 0;
  GLuint maskTexture = 0;
  GLuint casterVertexArray = 0;
  GLuint casterVertexBuffer = 0;
  GLuint fullscreenVertexArray = 0;

  GLint depthCenter = -1;
  GLint depthRight = -1;
  GLint depthUp = -1;
  GLint depthForward = -1;
  GLint depthExtents = -1;

  GLint maskSceneDepth = -1;
  GLint maskShadowMap = -1;
  GLint maskTargetSize = -1;
  GLint maskViewport = -1;
  GLint maskView = -1;
  GLint maskPresentationScale = -1;
  GLint maskDepthProjection = -1;
  GLint maskCenter = -1;
  GLint maskRight = -1;
  GLint maskUp = -1;
  GLint maskForward = -1;
  GLint maskExtents = -1;
  GLint maskReachDarkness = -1;
  GLint maskLayer = -1;

  GLint compositeMask = -1;
  GLint compositeTargetSize = -1;

  int maskWidth = 0;
  int maskHeight = 0;
  bool available = false;
};

PsyXObjectShadowResources g_objectShadows;

constexpr const char *kObjectShadowDepthShader = R"PSYX(
#ifdef VERTEX
attribute vec3 a_position;
uniform vec3 u_center;
uniform vec3 u_right;
uniform vec3 u_up;
uniform vec3 u_forward;
uniform vec4 u_extents;

void main() {
  vec3 relative = a_position - u_center;
  float localX = dot(relative, u_right) / max(u_extents.x, 0.0001);
  float localY = dot(relative, u_up) / max(u_extents.y, 0.0001);
  float localZ = (dot(relative, u_forward) + u_extents.z) /
                 max(u_extents.w, 0.0001);
  gl_Position = vec4(localX, localY, localZ * 2.0 - 1.0, 1.0);
}
#else
void main() {
  fragColor = vec4(0.0);
}
#endif
)PSYX";

constexpr const char *kObjectShadowMaskShader = R"PSYX(
#ifdef VERTEX
void main() {
  vec2 position = gl_VertexID == 0 ? vec2(0.0, 0.0)
                : gl_VertexID == 1 ? vec2(2.0, 0.0)
                                   : vec2(0.0, 2.0);
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
#else
uniform sampler2D u_scene_depth;
uniform sampler2DArray u_shadow_map;
uniform vec2 u_target_size;
uniform vec4 u_viewport;
uniform vec3 u_view;
uniform vec2 u_presentation_scale;
uniform vec2 u_depth_projection;
uniform vec3 u_center;
uniform vec3 u_right;
uniform vec3 u_up;
uniform vec3 u_forward;
uniform vec4 u_extents;
uniform vec3 u_reach_darkness;
uniform float u_shadow_layer;

bool reconstructReceiverAt(vec2 fragment, out vec3 receiver) {
  if (fragment.x < u_viewport.x || fragment.y < u_viewport.y ||
      fragment.x >= u_viewport.x + u_viewport.z ||
      fragment.y >= u_viewport.y + u_viewport.w)
    return false;

  vec2 targetUv = fragment / u_target_size;
  float stored = texture2D(u_scene_depth, targetUv).r;
  // Native scene depth is reversed and clears to zero. Never project a
  // shadow onto the colour-only sky/background.
  if (stored <= 0.0000001)
    return false;

  float ndcDepth = stored * 2.0 - 1.0;
  float denominator = ndcDepth - u_depth_projection.x;
  if (abs(denominator) < 0.000001)
    return false;
  float cameraDepth = u_depth_projection.y / denominator;
  if (cameraDepth <= 0.25 || cameraDepth >= 1000.0)
    return false;

  vec2 viewportUv = (fragment - u_viewport.xy) / u_viewport.zw;
  vec2 ndc = viewportUv * 2.0 - 1.0;
  vec2 scale = max(u_presentation_scale, vec2(0.0001));
  vec3 ray = vec3(ndc.x * u_view.x / (2.0 * u_view.z * scale.x),
                  -ndc.y * u_view.y / (2.0 * u_view.z * scale.y), 1.0);
  receiver = ray * cameraDepth;
  return true;
}

float receiverCoherence(bool valid, vec3 neighbour, vec3 center,
                        float pixelFootprint) {
  if (!valid)
    return 0.0;
  float relativeDepth =
      abs(neighbour.z - center.z) / max(abs(center.z), 0.25);
  float relativeConfidence =
      1.0 - smoothstep(0.003, 0.020, relativeDepth);
  float spatialDistance = length(neighbour - center);
  float spatialConfidence = 1.0 - smoothstep(
      max(pixelFootprint * 3.0, 0.0001),
      max(pixelFootprint * 8.0, 0.0002), spatialDistance);
  return min(relativeConfidence, spatialConfidence);
}

bool reconstructReceiverFrame(out vec3 receiver, out vec3 tangentX,
                              out vec3 tangentY, out vec3 normal,
                              out float confidence) {
  if (!reconstructReceiverAt(gl_FragCoord.xy, receiver))
    return false;
  vec3 left = receiver;
  vec3 right = receiver;
  vec3 down = receiver;
  vec3 up = receiver;
  bool hasLeft =
      reconstructReceiverAt(gl_FragCoord.xy + vec2(-1.0, 0.0), left);
  bool hasRight =
      reconstructReceiverAt(gl_FragCoord.xy + vec2(1.0, 0.0), right);
  bool hasDown =
      reconstructReceiverAt(gl_FragCoord.xy + vec2(0.0, -1.0), down);
  bool hasUp =
      reconstructReceiverAt(gl_FragCoord.xy + vec2(0.0, 1.0), up);
  vec2 scale = max(u_presentation_scale, vec2(0.0001));
  float projection = max(u_view.z, 0.0001);
  float cameraDepth = max(abs(receiver.z), 0.25);
  float footprintX = cameraDepth * u_view.x /
                     (projection * scale.x * max(u_viewport.z, 1.0));
  float footprintY = cameraDepth * u_view.y /
                     (projection * scale.y * max(u_viewport.w, 1.0));
  float leftConfidence =
      receiverCoherence(hasLeft, left, receiver, footprintX);
  float rightConfidence =
      receiverCoherence(hasRight, right, receiver, footprintX);
  float downConfidence =
      receiverCoherence(hasDown, down, receiver, footprintY);
  float upConfidence =
      receiverCoherence(hasUp, up, receiver, footprintY);

  // At a depth edge use the coherent side instead of spanning two surfaces.
  // Both tangents still point in the positive screen direction.
  tangentX = rightConfidence >= leftConfidence ? right - receiver
                                                : receiver - left;
  tangentY = upConfidence >= downConfidence ? up - receiver
                                             : receiver - down;
  confidence = min(max(leftConfidence, rightConfidence),
                   max(downConfidence, upConfidence));
  if (confidence <= 0.0001)
    return false;

  vec3 unnormalised = cross(tangentX, tangentY);
  float normalLength = length(unnormalised);
  if (normalLength <= 0.000001)
    return false;
  normal = unnormalised / normalLength;
  if (dot(normal, -receiver) < 0.0)
    normal = -normal;
  return true;
}

void main() {
  vec3 receiver;
  vec3 tangentX;
  vec3 tangentY;
  vec3 receiverNormal;
  float receiverConfidence;
  if (!reconstructReceiverFrame(receiver, tangentX, tangentY, receiverNormal,
                                receiverConfidence))
    discard;

  float facingFade =
      smoothstep(0.04, 0.22, dot(receiverNormal, -u_forward));
  if (facingFade <= 0.0001)
    discard;

  vec3 relative = receiver - u_center;
  vec2 local = vec2(dot(relative, u_right) / max(u_extents.x, 0.0001),
                    dot(relative, u_up) / max(u_extents.y, 0.0001));
  if (any(greaterThan(abs(local), vec2(1.0))))
    discard;

  float receiverZ = dot(relative, u_forward);
  float totalDepth = max(u_extents.w, 0.0001);
  float receiverDepth = (receiverZ + u_extents.z) / totalDepth;
  if (receiverDepth <= 0.0 || receiverDepth >= 1.0)
    discard;

  vec2 shadowUv = local * 0.5 + 0.5;
  float texel = u_reach_darkness.z;
  vec2 tangentUvX =
      vec2(dot(tangentX, u_right) / max(u_extents.x, 0.0001),
           dot(tangentX, u_up) / max(u_extents.y, 0.0001)) * 0.5;
  vec2 tangentUvY =
      vec2(dot(tangentY, u_right) / max(u_extents.x, 0.0001),
           dot(tangentY, u_up) / max(u_extents.y, 0.0001)) * 0.5;
  float tangentDepthX = dot(tangentX, u_forward);
  float tangentDepthY = dot(tangentY, u_forward);
  float determinant = tangentUvX.x * tangentUvY.y -
                      tangentUvX.y * tangentUvY.x;
  if (abs(determinant) <= 0.0000000001)
    discard;
  vec2 depthGradient = vec2(
      (tangentDepthX * tangentUvY.y - tangentDepthY * tangentUvX.y) /
          determinant,
      (tangentUvX.x * tangentDepthY - tangentDepthX * tangentUvY.x) /
          determinant);
  float gradientMagnitude = min(abs(depthGradient.x) + abs(depthGradient.y),
                                totalDepth / max(texel, 0.000001));
  float slopeBias = min(gradientMagnitude * texel * 0.08,
                        totalDepth / 2048.0);
  float bias = max(totalDepth / 65536.0, 0.00015) + slopeBias;
  float reach = max(u_reach_darkness.x, 0.0001);
  float coverage = 0.0;

  vec2 texelPosition = shadowUv / texel - vec2(0.5);
  vec2 baseTexel = floor(texelPosition);
  vec2 fraction = fract(texelPosition);
  for (int y = 0; y <= 1; ++y) {
    for (int x = 0; x <= 1; ++x) {
      vec2 offset = vec2(float(x), float(y));
      vec2 sampleUv = (baseTexel + offset + vec2(0.5)) * texel;
      float storedDepth =
          texture(u_shadow_map, vec3(sampleUv, u_shadow_layer)).r;
      if (storedDepth >= 0.999999)
        continue;
      float storedZ = storedDepth * totalDepth - u_extents.z;
      float sampleReceiverZ =
          receiverZ + dot(depthGradient, sampleUv - shadowUv);
      float separation = sampleReceiverZ - storedZ;
      if (separation <= bias || separation >= reach * 0.85)
        continue;
      float reachFade =
          1.0 - smoothstep(reach * 0.25, reach * 0.85, separation);
      float weightX = x == 0 ? 1.0 - fraction.x : fraction.x;
      float weightY = y == 0 ? 1.0 - fraction.y : fraction.y;
      coverage += reachFade * weightX * weightY;
    }
  }

  float edge = max(abs(local.x), abs(local.y));
  float edgeFade = 1.0 - smoothstep(0.82, 0.995, edge);
  float darkness = clamp(u_reach_darkness.y, 0.0, 0.75);
  float mask = coverage * edgeFade * darkness * receiverConfidence *
               facingFade;
  if (mask <= 0.0001)
    discard;
  fragColor = vec4(mask, 0.0, 0.0, 1.0);
}
#endif
)PSYX";

constexpr const char *kObjectShadowCompositeShader = R"PSYX(
#ifdef VERTEX
void main() {
  vec2 position = gl_VertexID == 0 ? vec2(0.0, 0.0)
                : gl_VertexID == 1 ? vec2(2.0, 0.0)
                                   : vec2(0.0, 2.0);
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
#else
uniform sampler2D u_shadow_mask;
uniform vec2 u_target_size;

void main() {
  float mask = texture2D(u_shadow_mask,
                         gl_FragCoord.xy / u_target_size).r;
  fragColor = vec4(0.0, 0.0, 0.0, clamp(mask, 0.0, 0.75));
}
#endif
)PSYX";

bool PsyX_ObjectShadowProgramLinked(GLuint program) {
  if (program == 0)
    return false;
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  return linked == GL_TRUE;
}

bool PsyX_FiniteObjectShadowVector(float x, float y, float z) {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool PsyX_ValidObjectShadowCaster(const GrObjectShadowCaster &caster,
                                  int vertexCount) {
  if (caster.first_vertex < 0 || caster.vertex_count < 3 ||
      caster.first_vertex > vertexCount ||
      caster.vertex_count > vertexCount - caster.first_vertex)
    return false;
  if (!PsyX_FiniteObjectShadowVector(caster.center_x, caster.center_y,
                                     caster.center_z) ||
      !PsyX_FiniteObjectShadowVector(caster.right_x, caster.right_y,
                                     caster.right_z) ||
      !PsyX_FiniteObjectShadowVector(caster.up_x, caster.up_y, caster.up_z) ||
      !PsyX_FiniteObjectShadowVector(caster.forward_x, caster.forward_y,
                                     caster.forward_z))
    return false;
  if (!std::isfinite(caster.extent_x) || !std::isfinite(caster.extent_y) ||
      !std::isfinite(caster.depth_extent) ||
      !std::isfinite(caster.maximum_reach) || !std::isfinite(caster.darkness) ||
      caster.extent_x <= 0.0001F || caster.extent_y <= 0.0001F ||
      caster.depth_extent <= 0.0001F || caster.maximum_reach <= 0.0001F ||
      caster.darkness <= 0.0001F)
    return false;

  const auto lengthSquared = [](float x, float y, float z) {
    return x * x + y * y + z * z;
  };
  const float rightLength =
      lengthSquared(caster.right_x, caster.right_y, caster.right_z);
  const float upLength = lengthSquared(caster.up_x, caster.up_y, caster.up_z);
  const float forwardLength =
      lengthSquared(caster.forward_x, caster.forward_y, caster.forward_z);
  return rightLength > 0.64F && rightLength < 1.44F && upLength > 0.64F &&
         upLength < 1.44F && forwardLength > 0.64F && forwardLength < 1.44F;
}

bool PsyX_EnsureObjectShadowMask(int width, int height) {
  if (width <= 0 || height <= 0)
    return false;
  if (g_objectShadows.maskWidth == width &&
      g_objectShadows.maskHeight == height)
    return true;

  GLint savedActiveTexture = GL_TEXTURE0;
  GLint savedTexture = 0;
  GLint savedDrawFramebuffer = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTexture);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTexture);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &savedDrawFramebuffer);

  glBindTexture(GL_TEXTURE_2D, g_objectShadows.maskTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, width, height, 0, GL_RED,
               GL_HALF_FLOAT, nullptr);
  glBindFramebuffer(GL_FRAMEBUFFER, g_objectShadows.maskFramebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g_objectShadows.maskTexture, 0);
  const GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &drawBuffer);
  bool complete =
      glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (!complete) {
    // R16F is core on the desktop path, but keep older drivers functional
    // instead of retrying an incomplete allocation every frame.
    glBindTexture(GL_TEXTURE_2D, g_objectShadows.maskTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED,
                 GL_UNSIGNED_BYTE, nullptr);
    complete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
      eprintwarn("R16F object-shadow mask unavailable; using R8\n");
    } else {
      eprintwarn("Object-shadow mask framebuffer is unavailable; pass disabled\n");
      g_objectShadows.available = false;
    }
  }
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(savedTexture));
  glActiveTexture(static_cast<GLenum>(savedActiveTexture));
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                    static_cast<GLuint>(savedDrawFramebuffer));
  if (!complete) {
    g_objectShadows.maskWidth = 0;
    g_objectShadows.maskHeight = 0;
    return false;
  }
  g_objectShadows.maskWidth = width;
  g_objectShadows.maskHeight = height;
  return true;
}

bool PsyX_ObjectShadowScissor(const GrObjectShadowCaster &caster,
                              int projection, int logicalWidth,
                              int logicalHeight,
                              const PsyXPresentationViewport &viewport,
                              const PsyXPresentationScale &scale,
                              GLint output[4]) {
  const float halfReach = caster.maximum_reach * 0.5F;
  const float centerX = caster.center_x + caster.forward_x * halfReach;
  const float centerY = caster.center_y + caster.forward_y * halfReach;
  const float centerZ = caster.center_z + caster.forward_z * halfReach;
  const float axialRadius = caster.depth_extent + halfReach;
  const float radius =
      std::sqrt(caster.extent_x * caster.extent_x +
                caster.extent_y * caster.extent_y + axialRadius * axialRadius);
  if (centerZ + radius <= 0.25F)
    return false;

  const float front = std::max(centerZ - radius, 0.251F);
  const float safeCenterDepth = std::max(centerZ, 0.251F);
  const float logicalCenterX =
      static_cast<float>(logicalWidth) * 0.5F +
      centerX * static_cast<float>(projection) / safeCenterDepth;
  const float logicalCenterY =
      static_cast<float>(logicalHeight) * 0.5F +
      centerY * static_cast<float>(projection) / safeCenterDepth;
  const float halfWidth =
      (radius + std::abs(centerX) * radius / safeCenterDepth) *
      static_cast<float>(projection) / front;
  const float halfHeight =
      (radius + std::abs(centerY) * radius / safeCenterDepth) *
      static_cast<float>(projection) / front;

  const auto mapX = [&](float logical) {
    const float ndc =
        ((logical / static_cast<float>(logicalWidth)) - 0.5F) * 2.0F * scale.x;
    return static_cast<float>(viewport.x) +
           (ndc * 0.5F + 0.5F) * static_cast<float>(viewport.w);
  };
  const auto mapY = [&](float logical) {
    const float ndc = -((logical / static_cast<float>(logicalHeight)) - 0.5F) *
                      2.0F * scale.y;
    return static_cast<float>(viewport.y) +
           (ndc * 0.5F + 0.5F) * static_cast<float>(viewport.h);
  };

  const float left = mapX(logicalCenterX - halfWidth);
  const float right = mapX(logicalCenterX + halfWidth);
  const float bottom = mapY(logicalCenterY + halfHeight);
  const float top = mapY(logicalCenterY - halfHeight);
  const int minimumX = std::max(viewport.x, static_cast<int>(std::floor(left)));
  const int maximumX =
      std::min(viewport.x + viewport.w, static_cast<int>(std::ceil(right)));
  const int minimumY =
      std::max(viewport.y, static_cast<int>(std::floor(bottom)));
  const int maximumY =
      std::min(viewport.y + viewport.h, static_cast<int>(std::ceil(top)));
  if (minimumX >= maximumX || minimumY >= maximumY)
    return false;
  output[0] = minimumX;
  output[1] = minimumY;
  output[2] = maximumX - minimumX;
  output[3] = maximumY - minimumY;
  return true;
}

void PsyX_InitialiseObjectShadows() {
  if (!kObjectShadowsEnabled) {
    eprintf("*Object shadows: disabled\n");
    return;
  }

  g_objectShadows.depthProgram = GR_Shader_Compile(kObjectShadowDepthShader, 0);
  g_objectShadows.maskProgram = GR_Shader_Compile(kObjectShadowMaskShader, 0);
  g_objectShadows.compositeProgram =
      GR_Shader_Compile(kObjectShadowCompositeShader, 0);
  if (!PsyX_ObjectShadowProgramLinked(g_objectShadows.depthProgram) ||
      !PsyX_ObjectShadowProgramLinked(g_objectShadows.maskProgram) ||
      !PsyX_ObjectShadowProgramLinked(g_objectShadows.compositeProgram)) {
    eprintwarn("Object shadow shaders are unavailable; pass disabled\n");
    PsyX_DestroyObjectShadows();
    return;
  }

  g_objectShadows.depthCenter =
      glGetUniformLocation(g_objectShadows.depthProgram, "u_center");
  g_objectShadows.depthRight =
      glGetUniformLocation(g_objectShadows.depthProgram, "u_right");
  g_objectShadows.depthUp =
      glGetUniformLocation(g_objectShadows.depthProgram, "u_up");
  g_objectShadows.depthForward =
      glGetUniformLocation(g_objectShadows.depthProgram, "u_forward");
  g_objectShadows.depthExtents =
      glGetUniformLocation(g_objectShadows.depthProgram, "u_extents");

  g_objectShadows.maskSceneDepth =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_scene_depth");
  g_objectShadows.maskShadowMap =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_shadow_map");
  g_objectShadows.maskTargetSize =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_target_size");
  g_objectShadows.maskViewport =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_viewport");
  g_objectShadows.maskView =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_view");
  g_objectShadows.maskPresentationScale =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_presentation_scale");
  g_objectShadows.maskDepthProjection =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_depth_projection");
  g_objectShadows.maskCenter =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_center");
  g_objectShadows.maskRight =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_right");
  g_objectShadows.maskUp =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_up");
  g_objectShadows.maskForward =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_forward");
  g_objectShadows.maskExtents =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_extents");
  g_objectShadows.maskReachDarkness =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_reach_darkness");
  g_objectShadows.maskLayer =
      glGetUniformLocation(g_objectShadows.maskProgram, "u_shadow_layer");
  g_objectShadows.compositeMask =
      glGetUniformLocation(g_objectShadows.compositeProgram, "u_shadow_mask");
  g_objectShadows.compositeTargetSize =
      glGetUniformLocation(g_objectShadows.compositeProgram, "u_target_size");

  glGenFramebuffers(1, &g_objectShadows.shadowFramebuffer);
  glGenTextures(1, &g_objectShadows.shadowTexture);
  glBindTexture(GL_TEXTURE_2D_ARRAY, g_objectShadows.shadowTexture);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
  const GLfloat borderDepth[] = {1.0F, 1.0F, 1.0F, 1.0F};
  glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, borderDepth);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24,
               kObjectShadowResolution, kObjectShadowResolution,
               kObjectShadowMaximumCasters, 0, GL_DEPTH_COMPONENT,
               GL_UNSIGNED_INT, nullptr);

  glBindFramebuffer(GL_FRAMEBUFFER, g_objectShadows.shadowFramebuffer);
  glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            g_objectShadows.shadowTexture, 0, 0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  const bool shadowComplete =
      glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

  glGenFramebuffers(1, &g_objectShadows.maskFramebuffer);
  glGenTextures(1, &g_objectShadows.maskTexture);
  glGenVertexArrays(1, &g_objectShadows.casterVertexArray);
  glGenBuffers(1, &g_objectShadows.casterVertexBuffer);
  glBindVertexArray(g_objectShadows.casterVertexArray);
  glBindBuffer(GL_ARRAY_BUFFER, g_objectShadows.casterVertexBuffer);
  glEnableVertexAttribArray(a_position);
  glVertexAttribPointer(a_position, 3, GL_FLOAT, GL_FALSE,
                        sizeof(GrObjectShadowVertex), nullptr);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glGenVertexArrays(1, &g_objectShadows.fullscreenVertexArray);

  glUseProgram(g_objectShadows.maskProgram);
  glUniform1i(g_objectShadows.maskSceneDepth, 0);
  glUniform1i(g_objectShadows.maskShadowMap, 1);
  glUseProgram(g_objectShadows.compositeProgram);
  glUniform1i(g_objectShadows.compositeMask, 0);
  glUseProgram(0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, PsyX_GetNativeDrawFramebuffer());

  if (!shadowComplete) {
    eprintwarn("Object shadow-map framebuffer is unavailable; pass disabled\n");
    PsyX_DestroyObjectShadows();
    return;
  }
  g_objectShadows.available = true;
  eprintf("*Object shadows: %dx%d local shadow maps (%d casters)\n",
          kObjectShadowResolution, kObjectShadowResolution,
          kObjectShadowMaximumCasters);
}

void PsyX_DestroyObjectShadows() {
  if (g_objectShadows.casterVertexBuffer != 0)
    glDeleteBuffers(1, &g_objectShadows.casterVertexBuffer);
  if (g_objectShadows.casterVertexArray != 0)
    glDeleteVertexArrays(1, &g_objectShadows.casterVertexArray);
  if (g_objectShadows.fullscreenVertexArray != 0)
    glDeleteVertexArrays(1, &g_objectShadows.fullscreenVertexArray);
  if (g_objectShadows.maskTexture != 0)
    glDeleteTextures(1, &g_objectShadows.maskTexture);
  if (g_objectShadows.shadowTexture != 0)
    glDeleteTextures(1, &g_objectShadows.shadowTexture);
  if (g_objectShadows.maskFramebuffer != 0)
    glDeleteFramebuffers(1, &g_objectShadows.maskFramebuffer);
  if (g_objectShadows.shadowFramebuffer != 0)
    glDeleteFramebuffers(1, &g_objectShadows.shadowFramebuffer);
  if (g_objectShadows.depthProgram != 0)
    glDeleteProgram(g_objectShadows.depthProgram);
  if (g_objectShadows.maskProgram != 0)
    glDeleteProgram(g_objectShadows.maskProgram);
  if (g_objectShadows.compositeProgram != 0)
    glDeleteProgram(g_objectShadows.compositeProgram);
  g_objectShadows = {};
}

struct PsyXObjectShadowDraw {
  const GrObjectShadowCaster *caster = nullptr;
  int layer = 0;
};

void PsyX_LogObjectShadowDrawCounts(int casterCount, int drawCount,
                                     int scissorAccepted) {
  static std::uint64_t callSequence = 0U;
  static std::uint64_t lastLogSequence = 0U;
  static int lastCasterCount = -1;
  static int lastDrawCount = -1;
  static int lastScissorAccepted = -1;
  ++callSequence;
  const auto firstSample = lastCasterCount < 0;
  const auto changed = casterCount != lastCasterCount ||
                       drawCount != lastDrawCount ||
                       scissorAccepted != lastScissorAccepted;
  constexpr std::uint64_t minimumLogInterval = 120U;
  if (!firstSample &&
      (!changed || callSequence - lastLogSequence < minimumLogInterval)) {
    return;
  }
  eprintf("*Object shadow draw: casters=%d valid=%d scissorAccepted=%d\n",
          casterCount, drawCount, scissorAccepted);
  lastCasterCount = casterCount;
  lastDrawCount = drawCount;
  lastScissorAccepted = scissorAccepted;
  lastLogSequence = callSequence;
}

} // namespace

int GR_ObjectShadowsAvailable(void) {
  return g_objectShadows.available ? 1 : 0;
}

void GR_DrawObjectShadows(const GrObjectShadowVertex *vertices, int vertexCount,
                          const GrObjectShadowCaster *casters, int casterCount,
                          int projection, int logicalWidth, int logicalHeight) {
  if (!g_objectShadows.available || vertices == nullptr || vertexCount <= 0 ||
      casters == nullptr || casterCount <= 0 || projection <= 0 ||
      logicalWidth <= 0 || logicalHeight <= 0 ||
      g_nativeFramebufferWidth <= 0 || g_nativeFramebufferHeight <= 0)
    return;
  if (!PsyX_EnsureObjectShadowMask(g_nativeFramebufferWidth,
                                   g_nativeFramebufferHeight))
    return;

  std::array<PsyXObjectShadowDraw, kObjectShadowMaximumCasters> draws{};
  auto drawCount = 0;
  for (int index = 0;
       index < casterCount && drawCount < kObjectShadowMaximumCasters;
       ++index) {
    if (!PsyX_ValidObjectShadowCaster(casters[index], vertexCount))
      continue;
    draws[static_cast<std::size_t>(drawCount)] =
        PsyXObjectShadowDraw{&casters[index], drawCount};
    ++drawCount;
  }
  if (drawCount == 0) {
    PsyX_LogObjectShadowDrawCounts(casterCount, 0, 0);
    return;
  }

  const int savedBlendMode = g_PreviousBlendMode;
  const int savedDepthMode = g_PreviousDepthMode;
  const int savedDepthWrite = g_PreviousDepthWrite;
  const int savedStencilMode = g_PreviousStencilMode;
  const GLboolean savedBlendTest = glIsEnabled(GL_BLEND);
  const GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean savedStencilTest = glIsEnabled(GL_STENCIL_TEST);
  const GLboolean savedScissorTest = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean savedCullFace = glIsEnabled(GL_CULL_FACE);
  const GLboolean savedPolygonOffset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
  const GLboolean savedFramebufferSrgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
  GLboolean savedColorMask[4] = {};
  GLboolean savedDepthMask = GL_TRUE;
  GLfloat savedPolygonOffsetFactor = 0.0F;
  GLfloat savedPolygonOffsetUnits = 0.0F;
  GLint savedViewport[4] = {};
  GLint savedScissorBox[4] = {};
  GLint savedCullMode = GL_BACK;
  GLint savedFrontFace = GL_CCW;
  GLint savedDepthFunc = GL_GEQUAL;
  GLint savedBlendEquationRgb = GL_FUNC_ADD;
  GLint savedBlendEquationAlpha = GL_FUNC_ADD;
  GLint savedBlendSourceRgb = GL_ONE;
  GLint savedBlendDestinationRgb = GL_ZERO;
  GLint savedBlendSourceAlpha = GL_ONE;
  GLint savedBlendDestinationAlpha = GL_ZERO;
  GLfloat savedClearColor[4] = {};
  GLdouble savedClearDepth = 0.0;
  glGetBooleanv(GL_COLOR_WRITEMASK, savedColorMask);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &savedDepthMask);
  glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &savedPolygonOffsetFactor);
  glGetFloatv(GL_POLYGON_OFFSET_UNITS, &savedPolygonOffsetUnits);
  glGetIntegerv(GL_DEPTH_FUNC, &savedDepthFunc);
  glGetIntegerv(GL_VIEWPORT, savedViewport);
  glGetIntegerv(GL_SCISSOR_BOX, savedScissorBox);
  glGetIntegerv(GL_CULL_FACE_MODE, &savedCullMode);
  glGetIntegerv(GL_FRONT_FACE, &savedFrontFace);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, savedClearColor);
  glGetDoublev(GL_DEPTH_CLEAR_VALUE, &savedClearDepth);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &savedBlendEquationRgb);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &savedBlendEquationAlpha);
  glGetIntegerv(GL_BLEND_SRC_RGB, &savedBlendSourceRgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &savedBlendDestinationRgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &savedBlendSourceAlpha);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &savedBlendDestinationAlpha);

  PsyX_ResolveNativeFramebuffer(1);
  g_nativeFramePostprocessed = 1;

  glBindVertexArray(g_objectShadows.casterVertexArray);
  glBindBuffer(GL_ARRAY_BUFFER, g_objectShadows.casterVertexBuffer);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(vertexCount) *
                   sizeof(GrObjectShadowVertex),
               vertices, GL_STREAM_DRAW);
  glBindFramebuffer(GL_FRAMEBUFFER, g_objectShadows.shadowFramebuffer);
  glViewport(0, 0, kObjectShadowResolution, kObjectShadowResolution);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(1.5F, 2.0F);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glClearDepth(1.0);
  glUseProgram(g_objectShadows.depthProgram);

  for (auto drawIndex = 0; drawIndex < drawCount; ++drawIndex) {
    const auto &draw = draws[static_cast<std::size_t>(drawIndex)];
    const auto &caster = *draw.caster;
    const float totalDepth = caster.depth_extent * 2.0F + caster.maximum_reach;
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              g_objectShadows.shadowTexture, 0, draw.layer);
    glClear(GL_DEPTH_BUFFER_BIT);
    glUniform3f(g_objectShadows.depthCenter, caster.center_x, caster.center_y,
                caster.center_z);
    glUniform3f(g_objectShadows.depthRight, caster.right_x, caster.right_y,
                caster.right_z);
    glUniform3f(g_objectShadows.depthUp, caster.up_x, caster.up_y, caster.up_z);
    glUniform3f(g_objectShadows.depthForward, caster.forward_x,
                caster.forward_y, caster.forward_z);
    glUniform4f(g_objectShadows.depthExtents, caster.extent_x, caster.extent_y,
                caster.depth_extent, totalDepth);
    const int drawCount = caster.vertex_count - caster.vertex_count % 3;
    glDrawArrays(GL_TRIANGLES, caster.first_vertex, drawCount);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, g_objectShadows.maskFramebuffer);
  glViewport(0, 0, g_nativeFramebufferWidth, g_nativeFramebufferHeight);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_FALSE);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
  glClear(GL_COLOR_BUFFER_BIT);

  const auto viewport = PsyX_GetRenderViewport();
  const auto scale = PsyX_GetPresentationScale();
  glViewport(viewport.x, viewport.y, viewport.w, viewport.h);
  glEnable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendEquation(GL_MAX);
  glBlendFunc(GL_ONE, GL_ONE);
  glUseProgram(g_objectShadows.maskProgram);
  glBindVertexArray(g_objectShadows.fullscreenVertexArray);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_glNativeDepthTexture);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D_ARRAY, g_objectShadows.shadowTexture);
  glUniform2f(g_objectShadows.maskTargetSize,
              static_cast<float>(g_nativeFramebufferWidth),
              static_cast<float>(g_nativeFramebufferHeight));
  glUniform4f(g_objectShadows.maskViewport, static_cast<float>(viewport.x),
              static_cast<float>(viewport.y), static_cast<float>(viewport.w),
              static_cast<float>(viewport.h));
  glUniform3f(g_objectShadows.maskView, static_cast<float>(logicalWidth),
              static_cast<float>(logicalHeight),
              static_cast<float>(projection));
  glUniform2f(g_objectShadows.maskPresentationScale, scale.x, scale.y);
  glUniform2f(g_objectShadows.maskDepthProjection, g_reversedDepthScale,
              g_reversedDepthBias);

  auto scissorAccepted = 0;
  for (auto drawIndex = 0; drawIndex < drawCount; ++drawIndex) {
    const auto &draw = draws[static_cast<std::size_t>(drawIndex)];
    const auto &caster = *draw.caster;
    GLint scissor[4] = {};
    if (!PsyX_ObjectShadowScissor(caster, projection, logicalWidth,
                                  logicalHeight, viewport, scale, scissor))
      continue;
    ++scissorAccepted;
    const float totalDepth = caster.depth_extent * 2.0F + caster.maximum_reach;
    glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
    glUniform3f(g_objectShadows.maskCenter, caster.center_x, caster.center_y,
                caster.center_z);
    glUniform3f(g_objectShadows.maskRight, caster.right_x, caster.right_y,
                caster.right_z);
    glUniform3f(g_objectShadows.maskUp, caster.up_x, caster.up_y, caster.up_z);
    glUniform3f(g_objectShadows.maskForward, caster.forward_x, caster.forward_y,
                caster.forward_z);
    glUniform4f(g_objectShadows.maskExtents, caster.extent_x, caster.extent_y,
                caster.depth_extent, totalDepth);
    glUniform3f(g_objectShadows.maskReachDarkness, caster.maximum_reach,
                caster.darkness, 1.0F / kObjectShadowResolution);
    glUniform1f(g_objectShadows.maskLayer, static_cast<float>(draw.layer));
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }

  PsyX_LogObjectShadowDrawCounts(casterCount, drawCount, scissorAccepted);

  glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeFramebuffer);
  glViewport(viewport.x, viewport.y, viewport.w, viewport.h);
  glDisable(GL_SCISSOR_TEST);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
  glUseProgram(g_objectShadows.compositeProgram);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_objectShadows.maskTexture);
  glUniform2f(g_objectShadows.compositeTargetSize,
              static_cast<float>(g_nativeFramebufferWidth),
              static_cast<float>(g_nativeFramebufferHeight));
  glDrawArrays(GL_TRIANGLES, 0, 3);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glUseProgram(0);
  glBindFramebuffer(GL_FRAMEBUFFER, g_glNativeFramebuffer);
  glViewport(savedViewport[0], savedViewport[1], savedViewport[2],
             savedViewport[3]);
  glScissor(savedScissorBox[0], savedScissorBox[1], savedScissorBox[2],
            savedScissorBox[3]);
  glClearColor(savedClearColor[0], savedClearColor[1], savedClearColor[2],
               savedClearColor[3]);
  glClearDepth(savedClearDepth);
  glColorMask(savedColorMask[0], savedColorMask[1], savedColorMask[2],
              savedColorMask[3]);
  glCullFace(static_cast<GLenum>(savedCullMode));
  glFrontFace(static_cast<GLenum>(savedFrontFace));
  glPolygonOffset(savedPolygonOffsetFactor, savedPolygonOffsetUnits);
  if (savedCullFace)
    glEnable(GL_CULL_FACE);
  else
    glDisable(GL_CULL_FACE);
  if (savedPolygonOffset)
    glEnable(GL_POLYGON_OFFSET_FILL);
  else
    glDisable(GL_POLYGON_OFFSET_FILL);
  if (savedFramebufferSrgb)
    glEnable(GL_FRAMEBUFFER_SRGB);
  else
    glDisable(GL_FRAMEBUFFER_SRGB);
  if (savedScissorTest)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  if (savedStencilTest)
    glEnable(GL_STENCIL_TEST);
  else
    glDisable(GL_STENCIL_TEST);

  g_PreviousBlendMode = BM_NONE;
  glDisable(GL_BLEND);
  if (savedBlendMode != BM_NONE)
    GR_SetBlendMode(static_cast<BlendMode>(savedBlendMode));
  glBlendEquationSeparate(static_cast<GLenum>(savedBlendEquationRgb),
                          static_cast<GLenum>(savedBlendEquationAlpha));
  glBlendFuncSeparate(static_cast<GLenum>(savedBlendSourceRgb),
                      static_cast<GLenum>(savedBlendDestinationRgb),
                      static_cast<GLenum>(savedBlendSourceAlpha),
                      static_cast<GLenum>(savedBlendDestinationAlpha));
  if (savedBlendTest)
    glEnable(GL_BLEND);
  else
    glDisable(GL_BLEND);
  g_PreviousDepthMode = -1;
  g_PreviousDepthWrite = -1;
  g_PreviousDepthFunc = -1;
  GR_SetDepthState(savedDepthMode, savedDepthWrite);
  if (savedDepthTest)
    glEnable(GL_DEPTH_TEST);
  else
    glDisable(GL_DEPTH_TEST);
  glDepthMask(savedDepthMask);
  glDepthFunc(static_cast<GLenum>(savedDepthFunc));
  g_PreviousDepthMode = savedDepthTest ? 1 : 0;
  g_PreviousDepthWrite = savedDepthMask ? 1 : 0;
  g_PreviousDepthFunc = savedDepthFunc;
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
void PsyX_InitialiseObjectShadows() {}
void PsyX_DestroyObjectShadows() {}
} // namespace

int GR_ObjectShadowsAvailable(void) { return 0; }
void GR_DrawObjectShadows(const GrObjectShadowVertex *, int,
                          const GrObjectShadowCaster *, int, int, int, int) {}

#endif
