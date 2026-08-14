#define SDL_MAIN_HANDLED

#include "psycross_guest_gpu.hpp"
#include "sf/psx/gte_runtime.hpp"
#include <PsyX/PsyX_gte.h>

#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>
#include <PsyX/common/glad.h>
#include <SDL.h>
#include <psx/libgpu.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <tuple>
#include <vector>

extern SDL_Window *g_window;

void MakeTexcoordRect(GrVertex *vertex, unsigned char *uv, u_short page,
                      u_short clut, short w, short h);

namespace {

constexpr int output_size = 64;

bool containsColor(
    const std::array<std::uint16_t, output_size * output_size> &pixels,
    std::uint16_t color) {
  return std::ranges::any_of(
      pixels, [color](const auto pixel) { return (pixel & 0x7fffU) == color; });
}

void finishOffscreenPass() {
  DrawSync(0);
  RECT16 target{};
  GR_SetOffscreenState(&target, 0);
}

bool validatesTPageExclusiveEdges() {
  std::array<GrVertex, 4U> u_edge{};
  std::array<GrVertex, 4U> v_edge{};
  std::array<GrVertex, 4U> crossing_edge{};
  std::array<float, 4U> expected_x{};
  std::array<float, 4U> expected_y{};
  for (std::size_t vertex{}; vertex < u_edge.size(); ++vertex) {
    expected_x[vertex] = static_cast<float>(10U + vertex * 3U);
    expected_y[vertex] = static_cast<float>(20U + vertex * 5U);
    u_edge[vertex].x = expected_x[vertex];
    u_edge[vertex].y = expected_y[vertex];
    v_edge[vertex].x = expected_x[vertex];
    v_edge[vertex].y = expected_y[vertex];
  }

  unsigned char u_edge_uv[2U]{0U, 32U};
  MakeTexcoordRect(u_edge.data(), u_edge_uv, 0U, 0U, 256, 1);
  unsigned char v_edge_uv[2U]{17U, 192U};
  MakeTexcoordRect(v_edge.data(), v_edge_uv, 0U, 0U, 1, 64);
  unsigned char crossing_edge_uv[2U]{250U, 32U};
  MakeTexcoordRect(crossing_edge.data(), crossing_edge_uv, 0U, 0U, 16, 1);

  bool geometry_unchanged = true;
  bool u_bounds_exact = true;
  bool v_bounds_exact = true;
  for (std::size_t vertex{}; vertex < u_edge.size(); ++vertex) {
    geometry_unchanged = geometry_unchanged &&
                         u_edge[vertex].x == expected_x[vertex] &&
                         u_edge[vertex].y == expected_y[vertex] &&
                         v_edge[vertex].x == expected_x[vertex] &&
                         v_edge[vertex].y == expected_y[vertex];
    u_bounds_exact = u_bounds_exact && u_edge[vertex].umin == 0U &&
                     u_edge[vertex].umax == 255U;
    v_bounds_exact = v_bounds_exact && v_edge[vertex].vmin == 192U &&
                     v_edge[vertex].vmax == 255U;
  }
  const auto u_far_edge_exact =
      u_edge[0].precise_u == 0.0F && u_edge[1].precise_u == 0.0F &&
      u_edge[2].precise_u == 256.0F && u_edge[3].precise_u == 256.0F;
  const auto v_far_edge_exact =
      v_edge[0].precise_v == 192.0F && v_edge[1].precise_v == 256.0F &&
      v_edge[2].precise_v == 256.0F && v_edge[3].precise_v == 192.0F;
  const auto crossing_clamp_preserved =
      std::all_of(crossing_edge.cbegin(), crossing_edge.cend(),
                  [](const auto &vertex) {
                    return vertex.umin == 250U && vertex.umax == 254U;
                  }) &&
      crossing_edge[2].precise_u == 255.0F &&
      crossing_edge[3].precise_u == 255.0F;
  return geometry_unchanged && u_bounds_exact && v_bounds_exact &&
         u_far_edge_exact && v_far_edge_exact && crossing_clamp_preserved;
}

} // namespace

int main() {
  SDL_SetMainReady();
  if (PsyX_ResolveSwapInterval(1, 60, 30) != 2 ||
      PsyX_ResolveSwapInterval(1, 60, 60) != 1 ||
      PsyX_ResolveSwapInterval(1, 120, 120) != 1 ||
      PsyX_ResolveSwapInterval(1, 240, 120) != 2 ||
      PsyX_ResolveSwapInterval(1, 240, 240) != 1 ||
      PsyX_ResolveSwapInterval(1, 60, 120) != 0 ||
      PsyX_ResolveSwapInterval(1, 144, 60) != 0 ||
      PsyX_ResolveSwapInterval(0, 240, 240) != 0) {
    std::cerr << "Presentation cadence resolution is unstable\n";
    return 202;
  }
  if (PsyX_ShouldUseSoftwareFrameLimit(1, 60) != 0 ||
      PsyX_ShouldUseSoftwareFrameLimit(0, 60) == 0 ||
      PsyX_ShouldUseSoftwareFrameLimit(0, 0) != 0) {
    std::cerr << "Presentation limiter ownership is unstable\n";
    return 203;
  }
  g_cfg_framebufferFeedback = 0;
  g_cfg_vblankThread = 0;
  g_cfg_composedGuestScanout = 0;
  g_cfg_smaa = 1;
  g_cfg_fxaa = 1;
  g_cfg_smaaFinalFrame = 0;
  g_cfg_fxaaFinalFrame = 0;
  g_cfg_renderWidth = 128;
  g_cfg_renderHeight = 128;
  g_cfg_aspectMode = PSYX_ASPECT_ADAPTIVE;
  g_cfg_bilinearFiltering = 0;
  g_cfg_msaaSamples = 0;
  char title[] = "MOHU PsyCross render test";
  PsyX_Initialise(title, 320, 240, 0);
  if (g_window == nullptr) {
    std::cerr << "PsyCross did not create a window\n";
    return 1;
  }

  // Production RTPS/RTPT is executed by PsyCross's GTE. Lock its integer
  // register result to the headless core and its unsaturated projection tuple
  // to the same view/screen coordinates used by perspective correction.
  sf::psx::GteState reference_gte{};
  reference_gte.control[0U] = 0x00001000U;
  reference_gte.control[2U] = 0x00001000U;
  reference_gte.control[4U] = 0x00001000U;
  reference_gte.control[7U] = 4096U;
  reference_gte.control[24U] = 160U << 16U;
  reference_gte.control[25U] = 120U << 16U;
  reference_gte.control[26U] = 320U;
  reference_gte.data[0U] = 256U | (128U << 16U);
  reference_gte.data[1U] = 2048U;
  const auto projection_input = reference_gte;
  auto psycross_data = reference_gte.data;
  auto psycross_control = reference_gte.control;
  constexpr auto rtps_instruction = std::uint32_t{0x4a180001U};
  if (!sf::psx::GteRuntime::executeCommand(reference_gte, rtps_instruction,
                                           nullptr, true)) {
    std::cerr << "Reference GTE rejected RTPS fixture\n";
    PsyX_Shutdown();
    return 78;
  }
  std::array<PsyXGuestGteProjection, 3U> psycross_projection{};
  std::uint32_t psycross_projection_count{};
  const auto psycross_executed = PsyX_GteExecuteGuestProjection(
      psycross_data.data(), psycross_control.data(), rtps_instruction,
      psycross_projection.data(), &psycross_projection_count);
  const auto &reference_projection = reference_gte.projected[2U];
  const auto &actual_projection = psycross_projection[0U];
  auto repeated_data = projection_input.data;
  auto repeated_control = projection_input.control;
  std::array<PsyXGuestGteProjection, 3U> repeated_projection{};
  std::uint32_t repeated_count{};
  const auto repeated_executed = PsyX_GteExecuteGuestProjection(
      repeated_data.data(), repeated_control.data(), rtps_instruction,
      repeated_projection.data(), &repeated_count);
  auto rtpt_data = projection_input.data;
  rtpt_data[2U] = rtpt_data[0U];
  rtpt_data[3U] = rtpt_data[1U];
  rtpt_data[4U] = rtpt_data[0U];
  rtpt_data[5U] = rtpt_data[1U];
  auto rtpt_control = projection_input.control;
  std::array<PsyXGuestGteProjection, 3U> rtpt_projection{};
  std::uint32_t rtpt_count{};
  constexpr auto rtpt_instruction = std::uint32_t{0x4a280030U};
  const auto rtpt_executed = PsyX_GteExecuteGuestProjection(
      rtpt_data.data(), rtpt_control.data(), rtpt_instruction,
      rtpt_projection.data(), &rtpt_count);
  auto changed_matrix_data = projection_input.data;
  auto changed_matrix_control = projection_input.control;
  changed_matrix_control[0U] -= 1U;
  std::array<PsyXGuestGteProjection, 3U> changed_matrix_projection{};
  std::uint32_t changed_matrix_count{};
  const auto changed_matrix_executed = PsyX_GteExecuteGuestProjection(
      changed_matrix_data.data(), changed_matrix_control.data(),
      rtps_instruction, changed_matrix_projection.data(),
      &changed_matrix_count);
  const auto close = [](float left, float right) {
    return std::abs(left - right) <= 1.0e-4F;
  };
  if (psycross_executed == 0 || psycross_projection_count != 1U ||
      psycross_data != reference_gte.data ||
      psycross_control != reference_gte.control ||
      actual_projection.packed_sxy != reference_projection.packed_sxy ||
      !close(actual_projection.view_x, reference_projection.view_x) ||
      !close(actual_projection.view_y, reference_projection.view_y) ||
      !close(actual_projection.view_z, reference_projection.view_z) ||
      !close(actual_projection.screen_x, reference_projection.screen_x) ||
      !close(actual_projection.screen_y, reference_projection.screen_y) ||
      !close(actual_projection.screen_h, reference_projection.screen_h) ||
      actual_projection.mesh_vertex_id == 0U || repeated_executed == 0 ||
      repeated_count != 1U ||
      repeated_projection[0U].mesh_vertex_id !=
          actual_projection.mesh_vertex_id ||
      rtpt_executed == 0 || rtpt_count != 3U ||
      std::ranges::any_of(rtpt_projection,
                          [&](const auto &projection) {
                            return projection.mesh_vertex_id !=
                                   actual_projection.mesh_vertex_id;
                          }) ||
      changed_matrix_executed == 0 || changed_matrix_count != 1U ||
      changed_matrix_projection[0U].mesh_vertex_id ==
          actual_projection.mesh_vertex_id ||
      actual_projection.valid == 0U) {
    std::cerr << "PsyCross guest GTE RTPS diverged from integer state or "
                 "projection tuple\n";
    PsyX_Shutdown();
    return 79;
  }
  SDL_HideWindow(g_window);

  if (!validatesTPageExclusiveEdges()) {
    std::cerr << "TPAGE exclusive edge lost precise UV 256, bound 255, or "
                 "changed rectangle geometry\n";
    PsyX_Shutdown();
    return 37;
  }

  DISPENV display{};
  SetDefDispEnv(&display, 0, 0, 320, 240);
  PutDispEnv(&display);

  // Raw scanout and VRAM synchronization can request the native target before
  // a draw split has selected a PSX shader.
  while (glGetError() != GL_NO_ERROR) {
  }
  RECT16 native_target{};
  GR_SetOffscreenState(&native_target, 0);
  if (glGetError() != GL_NO_ERROR) {
    std::cerr << "Native target synchronization used projection uniforms "
                 "without an active PSX shader\n";
    PsyX_Shutdown();
    return 44;
  }
  // Projection setters can run while no PSX program is bound. Every shader
  // selected afterwards must receive the same cached 2D/3D matrices.
  GR_SetShader(0);
  GR_Ortho2D(-2.0F, 6.0F, 5.0F, -3.0F, -4.0F, 12.0F);
  GR_Perspective3D(0.8F, 4.0F, 3.0F, 1.0F, 101.0F);
  const auto shader_has_cached_matrices = [&](TexFormat format) {
    GR_SetTexture(g_vramTexture, format, TEXTURE_FILTER_NEAREST);
    GLint program{};
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    const auto projection = glGetUniformLocation(program, "Projection");
    const auto projection3d = glGetUniformLocation(program, "Projection3D");
    if (program == 0 || projection == -1 || projection3d == -1)
      return false;
    std::array<GLfloat, 16U> ortho{};
    std::array<GLfloat, 16U> perspective{};
    glGetUniformfv(program, projection, ortho.data());
    glGetUniformfv(program, projection3d, perspective.data());
    const auto near = [](float value, float expected) {
      return value > expected - 0.0001F && value < expected + 0.0001F;
    };
    return near(ortho[0], 0.25F) && near(ortho[5], -0.25F) &&
           near(ortho[12], -0.5F) && near(ortho[13], 0.25F) &&
           near(perspective[10], -1.02F) && near(perspective[14], 2.02F);
  };
  if (!shader_has_cached_matrices(TF_4_BIT) ||
      !shader_has_cached_matrices(TF_8_BIT) ||
      !shader_has_cached_matrices(TF_16_BIT) ||
      !shader_has_cached_matrices(TF_32_BIT_RGBA)) {
    std::cerr << "Projection cache was not republished on every shader bind\n";
    PsyX_Shutdown();
    return 59;
  }
  GR_SetOffscreenState(&native_target, 0);

  // Prove that camera-space W reaches real smooth interpolation, not merely
  // the PGXP cache. The same quad samples blue with affine W=1 and red when
  // its right edge has W=4. Its corrected pass also shades black-to-white
  // from left to right: the center must remain half intensity under affine
  // vertex-color interpolation, while perspective color would be much darker.
  constexpr int perspective_texture_width = 64;
  std::array<unsigned char, perspective_texture_width * 4U>
      perspective_texture_pixels{};
  for (int x{}; x < perspective_texture_width; ++x) {
    const auto pixel = static_cast<std::size_t>(x) * 4U;
    perspective_texture_pixels[pixel] = x < 24 ? 255U : 0U;
    perspective_texture_pixels[pixel + 2U] = x < 24 ? 0U : 255U;
    perspective_texture_pixels[pixel + 3U] = 255U;
  }
  const auto perspective_texture = GR_CreateRGBATexture(
      perspective_texture_width, 1, perspective_texture_pixels.data());
  const auto make_perspective_quad = [](bool corrected) {
    std::array<GrVertex, 6U> vertices{};
    const auto set = [&](std::size_t index, float x, float y, float u, float w,
                         unsigned char shade) {
      auto &vertex = vertices[index];
      vertex.x = x;
      vertex.y = y;
      vertex.z = w;
      vertex.scr_h = corrected ? 320.0F : 0.0F;
      vertex.precise_u = u;
      vertex.precise_v = 0.0F;
      vertex.bright = 1U;
      vertex.r = vertex.g = vertex.b = corrected ? shade : 255U;
      vertex.a = 255U;
    };
    set(0U, -0.5F, -0.5F, 0.0F, 1.0F, 0U);
    set(1U, -0.5F, 0.5F, 0.0F, 1.0F, 0U);
    set(2U, 0.5F, 0.5F, 63.0F, 4.0F, 255U);
    set(3U, -0.5F, -0.5F, 0.0F, 1.0F, 0U);
    set(4U, 0.5F, 0.5F, 63.0F, 4.0F, 255U);
    set(5U, 0.5F, -0.5F, 63.0F, 4.0F, 255U);
    return vertices;
  };
  const auto render_perspective_sample = [&](bool corrected) {
    static_cast<void>(PsyX_BeginScene());
    GR_SetOffscreenState(&native_target, 0);
    GR_SetScissorState(0);
    GR_EnableDepth(0);
    GR_SetBlendMode(BM_NONE);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    GR_Ortho2D(-0.5F, 0.5F, 0.5F, -0.5F, -1.0F, 1.0F);
    GR_Perspective3D(0.9265F, 1.0F, 1.0F, 0.25F, 1000.0F);
    GR_SetTexture(perspective_texture, TF_32_BIT_RGBA, TEXTURE_FILTER_NEAREST);
    GR_SetOverrideTextureSize(perspective_texture_width, 1);
    const auto vertices = make_perspective_quad(corrected);
    GR_UpdateVertexBuffer(vertices.data(), static_cast<int>(vertices.size()));
    GR_DrawTriangles(0, 2);
    std::array<GLint, 4U> viewport{};
    glGetIntegerv(GL_VIEWPORT, viewport.data());
    std::array<unsigned char, 4U> sample{};
    glReadPixels(viewport[0] + viewport[2] / 2, viewport[1] + viewport[3] / 2,
                 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sample.data());
    const auto error = glGetError();
    PsyX_EndScene();
    return std::pair{sample, error};
  };
  const auto [affine_sample, affine_error] = render_perspective_sample(false);
  const auto [corrected_sample, corrected_error] =
      render_perspective_sample(true);

  // Reversed depth rejects farther geometry while equal-depth polygons retain
  // GP0 painter order.
  const auto default_zbuffer = g_cfg_pgxpZBuffer;
  g_cfg_pgxpZBuffer = 1;
  const auto render_depth_sample =
      [&](float first_depth, std::array<unsigned char, 3U> first_color,
          float second_depth, std::array<unsigned char, 3U> second_color) {
        static_cast<void>(PsyX_BeginScene());
        GR_SetOffscreenState(&native_target, 0);
        GR_SetScissorState(0);
        GR_EnableDepth(1);
        GR_SetDepthState(1, 1);
        GR_SetBlendMode(BM_NONE);
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        GR_Ortho2D(-0.5F, 0.5F, 0.5F, -0.5F, -1.0F, 1.0F);
        GR_Perspective3D(0.9265F, 1.0F, 1.0F, PGXP_NEAR_PLANE, 1000.0F);
        GR_SetTexture(g_whiteTexture, TF_32_BIT_RGBA, TEXTURE_FILTER_NEAREST);
        GR_SetOverrideTextureSize(1, 1);

        std::array<GrVertex, 6U> vertices{};
        const auto set_triangle =
            [&](std::size_t first, float depth,
                const std::array<unsigned char, 3U> &color) {
              constexpr std::array positions{
                  std::array{-0.45F, -0.45F},
                  std::array{0.45F, -0.45F},
                  std::array{0.0F, 0.45F},
              };
              for (std::size_t index{}; index < positions.size(); ++index) {
                auto &vertex = vertices[first + index];
                vertex.x = positions[index][0U];
                vertex.y = positions[index][1U];
                vertex.z = depth;
                vertex.scr_h = 320.0F;
                vertex.precise_u = vertex.precise_v = 0.0F;
                vertex.r = color[0U];
                vertex.g = color[1U];
                vertex.b = color[2U];
                vertex.a = 255U;
                vertex.bright = 1U;
              }
            };
        set_triangle(0U, first_depth, first_color);
        set_triangle(3U, second_depth, second_color);
        GR_UpdateVertexBuffer(vertices.data(),
                              static_cast<int>(vertices.size()));
        GR_DrawTriangles(0, 1);
        GR_DrawTriangles(3, 1);

        std::array<GLint, 4U> viewport{};
        glGetIntegerv(GL_VIEWPORT, viewport.data());
        std::array<unsigned char, 4U> sample{};
        glReadPixels(viewport[0] + viewport[2] / 2,
                     viewport[1] + viewport[3] / 2, 1, 1, GL_RGBA,
                     GL_UNSIGNED_BYTE, sample.data());
        const auto error = glGetError();
        GR_EnableDepth(0);
        PsyX_EndScene();
        return std::pair{sample, error};
      };
  const auto [ordered_depth_sample, ordered_depth_error] =
      render_depth_sample(1.0F, {0U, 255U, 0U}, 8.0F, {255U, 0U, 0U});
  const auto [coplanar_depth_sample, coplanar_depth_error] =
      render_depth_sample(2.0F, {0U, 0U, 255U}, 2.0F, {255U, 0U, 0U});
  if (ordered_depth_error != GL_NO_ERROR ||
      coplanar_depth_error != GL_NO_ERROR || ordered_depth_sample[1U] < 200U ||
      ordered_depth_sample[0U] > 32U || coplanar_depth_sample[0U] < 200U ||
      coplanar_depth_sample[2U] > 32U) {
    std::cerr << "Reversed depth did not stabilize overlapping geometry; "
              << "ordered="
              << static_cast<unsigned int>(ordered_depth_sample[0U]) << ','
              << static_cast<unsigned int>(ordered_depth_sample[1U])
              << " coplanar="
              << static_cast<unsigned int>(coplanar_depth_sample[0U]) << ','
              << static_cast<unsigned int>(coplanar_depth_sample[2U]) << '\n';
    PsyX_Shutdown();
    return 91;
  }
  // OT order is not monotonic in camera depth. A disjoint farther primitive
  // must not erase depth already written by an earlier near primitive.
  const auto render_gp0_depth_order = [&] {
    GR_BeginGuestProjectionEpoch(90, 320, 240, 0, 0);
    GR_BeginGuestSubmit();
    static_cast<void>(PsyX_BeginScene());
    PGXP_ClearCache();
    GR_SetScissorState(0);
    GR_EnableDepth(1);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    DRAWENV depth_draw{};
    SetDefDrawEnv(&depth_draw, 0, 0, 320, 240);
    depth_draw.dtd = 0;
    depth_draw.dfe = 1;
    depth_draw.isbg = 0;
    PutDrawEnv(&depth_draw);

    const auto emit_triangle = [&](const std::array<short, 6U> &positions,
                                   float depth,
                                   const std::array<unsigned char, 3U> &color) {
      POLY_F3 polygon{};
      setPolyF3(&polygon);
      setRGB0(&polygon, color[0U], color[1U], color[2U]);
      setXY3(&polygon, positions[0U], positions[1U], positions[2U],
             positions[3U], positions[4U], positions[5U]);

      std::array<PGXPVData, 3U> vertices{};
      vertices[0U].lookup = PGXP_LOOKUP_VALUE(polygon.x0, polygon.y0);
      vertices[1U].lookup = PGXP_LOOKUP_VALUE(polygon.x1, polygon.y1);
      vertices[2U].lookup = PGXP_LOOKUP_VALUE(polygon.x2, polygon.y2);
      for (std::size_t vertex{}; vertex < vertices.size(); ++vertex) {
        auto &precise = vertices[vertex];
        precise.pz = depth;
        precise.sx = static_cast<float>(positions[vertex * 2U]);
        precise.sy = static_cast<float>(positions[vertex * 2U + 1U]);
        precise.scr_h = 320.0F;
        precise.precise_screen_position = 1U;
        if (PGXP_EmitCacheData(&precise) == static_cast<u_short>(0xffffU)) {
          return false;
        }
      }
      const auto cache_end = PGXP_GetIndex(1);
      if (cache_end == static_cast<u_short>(0xffffU))
        return false;
      DrawPrimPGXP(&polygon, cache_end);
      return true;
    };

    constexpr std::array<short, 6U> left_triangle{
        20, 40, 140, 40, 80, 200,
    };
    constexpr std::array<short, 6U> right_triangle{
        180, 40, 300, 40, 240, 200,
    };
    const auto emitted = emit_triangle(left_triangle, 1.0F, {0U, 255U, 0U}) &&
                         emit_triangle(right_triangle, 8.0F, {0U, 0U, 255U}) &&
                         emit_triangle(left_triangle, 8.0F, {255U, 0U, 0U});
    DrawSync(0);

    std::array<GLint, 4U> viewport{};
    glGetIntegerv(GL_VIEWPORT, viewport.data());
    std::array<unsigned char, 4U> left_sample{};
    std::array<unsigned char, 4U> right_sample{};
    glReadPixels(viewport[0] + viewport[2] / 4, viewport[1] + viewport[3] / 2,
                 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left_sample.data());
    glReadPixels(viewport[0] + viewport[2] * 3 / 4,
                 viewport[1] + viewport[3] / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                 right_sample.data());
    const auto error = glGetError();
    GR_EnableDepth(0);
    PsyX_EndScene();
    return std::tuple{emitted, left_sample, right_sample, error};
  };
  const auto [gp0_depth_emitted, gp0_left_sample, gp0_right_sample,
              gp0_depth_error] = render_gp0_depth_order();
  if (!gp0_depth_emitted || gp0_depth_error != GL_NO_ERROR ||
      gp0_left_sample[1U] < 200U || gp0_left_sample[0U] > 32U ||
      gp0_right_sample[2U] < 200U || gp0_right_sample[0U] > 32U) {
    std::cerr << "GP0 DrawPrim depth was cleared by a disjoint farther "
                 "primitive; emitted="
              << gp0_depth_emitted
              << " left=" << static_cast<unsigned int>(gp0_left_sample[0U])
              << ',' << static_cast<unsigned int>(gp0_left_sample[1U]) << ','
              << static_cast<unsigned int>(gp0_left_sample[2U])
              << " right=" << static_cast<unsigned int>(gp0_right_sample[0U])
              << ',' << static_cast<unsigned int>(gp0_right_sample[1U]) << ','
              << static_cast<unsigned int>(gp0_right_sample[2U]) << '\n';
    PsyX_Shutdown();
    return 92;
  }
  g_cfg_pgxpZBuffer = default_zbuffer;

  const auto previous_zbuffer = g_cfg_pgxpZBuffer;
  g_cfg_pgxpZBuffer = 1;
  const auto render_mixed_gp0_painter_order = [&] {
    GR_BeginGuestProjectionEpoch(91, 320, 240, 0, 0);
    GR_BeginGuestSubmit();
    static_cast<void>(PsyX_BeginScene());
    PGXP_ClearCache();
    GR_SetScissorState(0);
    GR_EnableDepth(1);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    DRAWENV draw{};
    SetDefDrawEnv(&draw, 0, 0, 320, 240);
    draw.dtd = 0;
    draw.dfe = 1;
    draw.isbg = 0;
    PutDrawEnv(&draw);

    constexpr std::array<short, 6U> exact_positions{
        80, 40, 240, 40, 160, 200,
    };
    constexpr std::array<short, 6U> raw_positions{
        250, 80, 310, 80, 280, 160,
    };
    const auto make_polygon = [&](const std::array<short, 6U> &positions,
                                  const std::array<unsigned char, 3U> &color) {
      POLY_F3 polygon{};
      setPolyF3(&polygon);
      setRGB0(&polygon, color[0U], color[1U], color[2U]);
      setXY3(&polygon, positions[0U], positions[1U], positions[2U],
             positions[3U], positions[4U], positions[5U]);
      return polygon;
    };
    const auto emit_exact = [&](float depth,
                                const std::array<unsigned char, 3U> &color) {
      auto polygon = make_polygon(exact_positions, color);
      std::array<PGXPVData, 3U> vertices{};
      vertices[0U].lookup = PGXP_LOOKUP_VALUE(polygon.x0, polygon.y0);
      vertices[1U].lookup = PGXP_LOOKUP_VALUE(polygon.x1, polygon.y1);
      vertices[2U].lookup = PGXP_LOOKUP_VALUE(polygon.x2, polygon.y2);
      for (std::size_t vertex{}; vertex < vertices.size(); ++vertex) {
        auto &precise = vertices[vertex];
        precise.pz = depth;
        precise.sx = static_cast<float>(exact_positions[vertex * 2U]);
        precise.sy = static_cast<float>(exact_positions[vertex * 2U + 1U]);
        precise.scr_h = 320.0F;
        precise.precise_screen_position = 1U;
        if (PGXP_EmitCacheData(&precise) == static_cast<u_short>(0xffffU)) {
          return false;
        }
      }
      const auto cache_end = PGXP_GetIndex(1);
      if (cache_end == static_cast<u_short>(0xffffU)) {
        return false;
      }
      DrawPrimPGXP(&polygon, cache_end);
      return true;
    };
    const auto emit_raw = [&](const std::array<unsigned char, 3U> &color) {
      auto polygon = make_polygon(raw_positions, color);
      DrawPrim(&polygon);
    };

    const auto emitted = emit_exact(1.0F, {0U, 255U, 0U});
    emit_raw({0U, 0U, 255U});
    const auto completed = emit_exact(8.0F, {255U, 0U, 0U});
    DrawSync(0);

    std::array<GLint, 4U> viewport{};
    glGetIntegerv(GL_VIEWPORT, viewport.data());
    std::array<unsigned char, 4U> exact_sample{};
    std::array<unsigned char, 4U> raw_sample{};
    glReadPixels(viewport[0] + viewport[2] / 2, viewport[1] + viewport[3] / 2,
                 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, exact_sample.data());
    glReadPixels(viewport[0] + viewport[2] * 7 / 8,
                 viewport[1] + viewport[3] / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                 raw_sample.data());
    const auto error = glGetError();
    GR_EnableDepth(0);
    PsyX_EndScene();
    return std::tuple{emitted && completed, exact_sample, raw_sample, error};
  };
  const auto [mixed_depth_emitted, mixed_depth_sample, mixed_raw_sample,
              mixed_depth_error] = render_mixed_gp0_painter_order();
  g_cfg_pgxpZBuffer = previous_zbuffer;
  if (!mixed_depth_emitted || mixed_depth_error != GL_NO_ERROR ||
      mixed_depth_sample[0U] < 200U || mixed_depth_sample[1U] > 32U ||
      mixed_depth_sample[2U] > 32U || mixed_raw_sample[2U] < 200U ||
      mixed_raw_sample[0U] > 32U || mixed_raw_sample[1U] > 32U) {
    std::cerr << "Depth-reset mixed GP0 packets lost painter order; emitted="
              << mixed_depth_emitted
              << " sample=" << static_cast<unsigned int>(mixed_depth_sample[0U])
              << ',' << static_cast<unsigned int>(mixed_depth_sample[1U]) << ','
              << static_cast<unsigned int>(mixed_depth_sample[2U])
              << " raw=" << static_cast<unsigned int>(mixed_raw_sample[0U])
              << ',' << static_cast<unsigned int>(mixed_raw_sample[1U]) << ','
              << static_cast<unsigned int>(mixed_raw_sample[2U]) << '\n';
    PsyX_Shutdown();
    return 93;
  }

  constexpr std::array<short, 6U> depth_epoch_positions{
      80, 40, 240, 40, 160, 200,
  };
  constexpr std::array<short, 6U> depth_epoch_raw_positions{
      250, 80, 310, 80, 280, 160,
  };
  const auto make_depth_epoch_polygon =
      [](const std::array<short, 6U> &positions,
         const std::array<unsigned char, 3U> &color) {
        POLY_F3 polygon{};
        setPolyF3(&polygon);
        setRGB0(&polygon, color[0U], color[1U], color[2U]);
        setXY3(&polygon, positions[0U], positions[1U], positions[2U],
               positions[3U], positions[4U], positions[5U]);
        return polygon;
      };
  const auto emit_depth_epoch_exact =
      [&](const std::array<short, 6U> &positions, float depth,
          const std::array<unsigned char, 3U> &color) {
        auto polygon = make_depth_epoch_polygon(positions, color);
        std::array<PGXPVData, 3U> vertices{};
        vertices[0U].lookup = PGXP_LOOKUP_VALUE(polygon.x0, polygon.y0);
        vertices[1U].lookup = PGXP_LOOKUP_VALUE(polygon.x1, polygon.y1);
        vertices[2U].lookup = PGXP_LOOKUP_VALUE(polygon.x2, polygon.y2);
        for (std::size_t vertex{}; vertex < vertices.size(); ++vertex) {
          auto &precise = vertices[vertex];
          precise.pz = depth;
          precise.sx = static_cast<float>(positions[vertex * 2U]);
          precise.sy = static_cast<float>(positions[vertex * 2U + 1U]);
          precise.scr_h = 320.0F;
          precise.precise_screen_position = 1U;
          if (PGXP_EmitCacheData(&precise) == static_cast<u_short>(0xffffU)) {
            return false;
          }
        }
        const auto cache_end = PGXP_GetIndex(1);
        if (cache_end == static_cast<u_short>(0xffffU))
          return false;
        DrawPrimPGXP(&polygon, cache_end);
        return true;
      };
  const auto emit_depth_epoch_raw =
      [&](const std::array<short, 6U> &positions,
          const std::array<unsigned char, 3U> &color) {
        auto polygon = make_depth_epoch_polygon(positions, color);
        DrawPrim(&polygon);
      };

  g_cfg_pgxpZBuffer = 1;
  const auto render_cross_batch_depth_epoch = [&] {
    GR_BeginGuestProjectionEpoch(92, 320, 240, 0, 0);
    GR_BeginGuestSubmit();
    static_cast<void>(PsyX_BeginScene());
    PGXP_ClearCache();
    GR_SetScissorState(0);
    GR_EnableDepth(1);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    GR_ClearDepthBuffer();

    DRAWENV draw{};
    SetDefDrawEnv(&draw, 0, 0, 320, 240);
    draw.dtd = 0;
    draw.dfe = 1;
    draw.isbg = 0;
    PutDrawEnv(&draw);

    const auto clear_before = GR_GetDepthClearSerial(0);
    const auto advances_before = GR_GetWorldDepthBandAdvanceCount(0);
    const auto fallback_before = GR_GetWorldDepthPainterFallbackCount(0);
    const auto emitted =
        emit_depth_epoch_exact(depth_epoch_positions, 1.0F, {0U, 255U, 0U});
    emit_depth_epoch_raw(depth_epoch_raw_positions, {0U, 0U, 255U});
    DrawSync(0);
    const auto completed =
        emit_depth_epoch_exact(depth_epoch_positions, 8.0F, {255U, 0U, 0U});
    DrawSync(0);

    std::array<GLint, 4U> viewport{};
    glGetIntegerv(GL_VIEWPORT, viewport.data());
    std::array<unsigned char, 4U> sample{};
    glReadPixels(viewport[0] + viewport[2] / 2, viewport[1] + viewport[3] / 2,
                 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sample.data());
    const auto error = glGetError();
    const auto state_valid =
        GR_GetDepthClearSerial(0) == clear_before &&
        GR_GetWorldDepthBandAdvanceCount(0) == advances_before + 1U &&
        GR_GetWorldDepthPainterFallbackCount(0) == fallback_before;
    GR_EnableDepth(0);
    PsyX_EndScene();
    return std::tuple{emitted && completed, state_valid, sample, error};
  };
  const auto [cross_batch_emitted, cross_batch_state, cross_batch_sample,
              cross_batch_error] = render_cross_batch_depth_epoch();
  if (!cross_batch_emitted || !cross_batch_state ||
      cross_batch_error != GL_NO_ERROR || cross_batch_sample[0U] < 200U ||
      cross_batch_sample[1U] > 32U || cross_batch_sample[2U] > 32U) {
    std::cerr << "Cross-DrawSync logical depth epoch was lost; emitted="
              << cross_batch_emitted << " state=" << cross_batch_state
              << " sample=" << static_cast<unsigned int>(cross_batch_sample[0U])
              << ',' << static_cast<unsigned int>(cross_batch_sample[1U]) << ','
              << static_cast<unsigned int>(cross_batch_sample[2U]) << '\n';
    g_cfg_pgxpZBuffer = previous_zbuffer;
    PsyX_Shutdown();
    return 110;
  }

  const auto render_depth_epoch_overflow = [&] {
    GR_BeginGuestProjectionEpoch(93, 320, 240, 0, 0);
    GR_BeginGuestSubmit();
    static_cast<void>(PsyX_BeginScene());
    PGXP_ClearCache();
    GR_SetScissorState(0);
    GR_EnableDepth(1);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    GR_ClearDepthBuffer();

    DRAWENV screen_draw{};
    SetDefDrawEnv(&screen_draw, 0, 0, 320, 240);
    screen_draw.dtd = 0;
    screen_draw.dfe = 1;
    screen_draw.isbg = 0;
    PutDrawEnv(&screen_draw);

    const auto clear_before = GR_GetDepthClearSerial(0);
    const auto screen_advances_before = GR_GetWorldDepthBandAdvanceCount(0);
    const auto screen_fallback_before = GR_GetWorldDepthPainterFallbackCount(0);
    const auto offscreen_advances_before = GR_GetWorldDepthBandAdvanceCount(1);
    const auto offscreen_fallback_before =
        GR_GetWorldDepthPainterFallbackCount(1);
    auto emitted =
        emit_depth_epoch_exact(depth_epoch_positions, 1.0F, {0U, 255U, 0U});
    for (unsigned int epoch{}; epoch < 65U; ++epoch) {
      emit_depth_epoch_raw(depth_epoch_raw_positions, {0U, 0U, 255U});
      emitted =
          emit_depth_epoch_exact(depth_epoch_positions, 8.0F, {255U, 0U, 0U}) &&
          emitted;
    }
    DrawSync(0);

    std::array<GLint, 4U> viewport{};
    glGetIntegerv(GL_VIEWPORT, viewport.data());
    std::array<unsigned char, 4U> sample{};
    glReadPixels(viewport[0] + viewport[2] / 2, viewport[1] + viewport[3] / 2,
                 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sample.data());
    const auto screen_valid =
        GR_GetDepthClearSerial(0) == clear_before &&
        GR_GetWorldDepthBandAdvanceCount(0) == screen_advances_before + 63U &&
        GR_GetWorldDepthPainterFallbackCount(0) ==
            screen_fallback_before + 1U &&
        GR_GetWorldDepthBandAdvanceCount(1) == offscreen_advances_before &&
        GR_GetWorldDepthPainterFallbackCount(1) == offscreen_fallback_before;

    DRAWENV offscreen_draw{};
    SetDefDrawEnv(&offscreen_draw, 0, 256, 64, 64);
    offscreen_draw.dtd = 0;
    offscreen_draw.dfe = 0;
    offscreen_draw.isbg = 0;
    PutDrawEnv(&offscreen_draw);
    constexpr std::array<short, 6U> offscreen_exact_positions{
        8, 264, 56, 264, 32, 312,
    };
    constexpr std::array<short, 6U> offscreen_raw_positions{
        2, 258, 6, 258, 4, 262,
    };
    emitted = emit_depth_epoch_exact(offscreen_exact_positions, 1.0F,
                                     {0U, 255U, 0U}) &&
              emitted;
    emit_depth_epoch_raw(offscreen_raw_positions, {0U, 0U, 255U});
    emitted = emit_depth_epoch_exact(offscreen_exact_positions, 8.0F,
                                     {255U, 0U, 0U}) &&
              emitted;
    DrawSync(0);
    const auto targets_independent =
        GR_GetWorldDepthBandAdvanceCount(0) == screen_advances_before + 63U &&
        GR_GetWorldDepthPainterFallbackCount(0) ==
            screen_fallback_before + 1U &&
        GR_GetWorldDepthBandAdvanceCount(1) == offscreen_advances_before + 1U &&
        GR_GetWorldDepthPainterFallbackCount(1) == offscreen_fallback_before;
    const auto error = glGetError();
    finishOffscreenPass();
    GR_EnableDepth(0);
    PsyX_EndScene();
    return std::tuple{emitted, screen_valid, targets_independent, sample,
                      error};
  };
  const auto [overflow_emitted, overflow_screen_valid,
              overflow_targets_independent, overflow_sample, overflow_error] =
      render_depth_epoch_overflow();
  g_cfg_pgxpZBuffer = previous_zbuffer;
  if (!overflow_emitted || !overflow_screen_valid ||
      !overflow_targets_independent || overflow_error != GL_NO_ERROR ||
      overflow_sample[0U] < 200U || overflow_sample[1U] > 32U ||
      overflow_sample[2U] > 32U) {
    std::cerr << "Logical depth epoch overflow was not fail-closed; emitted="
              << overflow_emitted << " screen=" << overflow_screen_valid
              << " targets=" << overflow_targets_independent
              << " sample=" << static_cast<unsigned int>(overflow_sample[0U])
              << ',' << static_cast<unsigned int>(overflow_sample[1U]) << ','
              << static_cast<unsigned int>(overflow_sample[2U]) << '\n';
    PsyX_Shutdown();
    return 111;
  }

  // Two triangles share an edge which crosses W=0. Fixed-function clipping
  // must create one continuous near-plane boundary without a black diagonal.
  const auto render_homogeneous_clip_seam = [&] {
    static_cast<void>(PsyX_BeginScene());
    GR_SetOffscreenState(&native_target, 0);
    GR_SetScissorState(0);
    GR_EnableDepth(0);
    GR_SetBlendMode(BM_NONE);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    GR_Ortho2D(-0.5F, 0.5F, 0.5F, -0.5F, -1.0F, 1.0F);
    GR_Perspective3D(0.9265F, 1.0F, 1.0F, PGXP_NEAR_PLANE, 1000.0F);
    GR_SetTexture(perspective_texture, TF_32_BIT_RGBA, TEXTURE_FILTER_NEAREST);
    GR_SetOverrideTextureSize(perspective_texture_width, 1);

    std::array<GrVertex, 6U> vertices{};
    const auto set = [&](std::size_t index, float view_x, float view_y,
                         float view_z) {
      auto &vertex = vertices[index];
      vertex.x = vertex.y = 0.0F;
      vertex.z = view_z;
      vertex.scr_h = 320.0F;
      vertex.clip_x = view_x;
      vertex.clip_y = view_y;
      vertex.precise_u = 0.0F;
      vertex.precise_v = 0.0F;
      vertex.umin = vertex.vmin = vertex.vmax = 0U;
      vertex.umax = 63U;
      vertex.bright = 255U;
      vertex.r = vertex.g = vertex.b = vertex.a = 255U;
    };
    set(0U, -0.25F, 0.0F, 1.0F);
    set(1U, -0.25F, -0.25F, 1.0F);
    set(2U, 0.0F, 0.0F, -0.5F);
    set(3U, -0.25F, 0.0F, 1.0F);
    set(4U, 0.0F, 0.0F, -0.5F);
    set(5U, -0.25F, 0.25F, 1.0F);
    GR_UpdateVertexBuffer(vertices.data(), static_cast<int>(vertices.size()));
    GR_DrawTriangles(0, 2);

    std::array<GLint, 4U> viewport{};
    glGetIntegerv(GL_VIEWPORT, viewport.data());
    std::array<std::array<unsigned char, 4U>, 3U> samples{};
    constexpr std::array<float, 3U> sample_x{0.08F, 0.125F, 0.20F};
    for (std::size_t index{}; index < samples.size(); ++index) {
      glReadPixels(viewport[0] +
                       static_cast<int>(sample_x[index] * viewport[2]),
                   viewport[1] + viewport[3] / 2, 1, 1, GL_RGBA,
                   GL_UNSIGNED_BYTE, samples[index].data());
    }
    const auto error = glGetError();
    PsyX_EndScene();
    const auto continuous =
        std::ranges::all_of(samples, [](const auto &sample) {
          return sample[0U] > 200U && sample[1U] < 32U && sample[2U] < 32U;
        });
    return std::pair{continuous, error};
  };
  const auto [clip_seam_continuous, clip_seam_error] =
      render_homogeneous_clip_seam();
  GR_SetOverrideTextureSize(0, 0);
  GR_DestroyTexture(perspective_texture);
  if (affine_error != GL_NO_ERROR || corrected_error != GL_NO_ERROR ||
      clip_seam_error != GL_NO_ERROR || !clip_seam_continuous ||
      affine_sample[2U] < 200U || affine_sample[0U] > 32U ||
      corrected_sample[0U] < 96U || corrected_sample[0U] > 160U ||
      corrected_sample[2U] > 32U) {
    std::cerr << "Perspective W did not affect real texture interpolation; "
              << "vertex color must remain affine; "
              << "affine=" << static_cast<unsigned int>(affine_sample[0U])
              << ',' << static_cast<unsigned int>(affine_sample[2U])
              << " corrected="
              << static_cast<unsigned int>(corrected_sample[0U]) << ','
              << static_cast<unsigned int>(corrected_sample[2U])
              << " clip_seam=" << clip_seam_continuous << '\n';
    PsyX_Shutdown();
    return 87;
  }
  GR_SetOffscreenState(&native_target, 0);

  // A resize event observed by a nested BeginScene must remain pending until
  // the next outer scene boundary.
  static_cast<void>(PsyX_BeginScene());
  int drawable_before_w{};
  int drawable_before_h{};
  GR_GetCommittedDrawableSize(&drawable_before_w, &drawable_before_h);
  GR_QueueDrawableSize(drawable_before_w + 17, drawable_before_h + 11);
  const auto nested_scene = PsyX_BeginScene();
  int drawable_nested_w{};
  int drawable_nested_h{};
  GR_GetCommittedDrawableSize(&drawable_nested_w, &drawable_nested_h);
  PsyX_EndScene();
  static_cast<void>(PsyX_BeginScene());
  int drawable_after_w{};
  int drawable_after_h{};
  GR_GetCommittedDrawableSize(&drawable_after_w, &drawable_after_h);
  PsyX_EndScene();
  GR_QueueDrawableSize(drawable_before_w, drawable_before_h);
  static_cast<void>(PsyX_BeginScene());
  PsyX_EndScene();
  if (nested_scene != 0 || drawable_nested_w != drawable_before_w ||
      drawable_nested_h != drawable_before_h ||
      drawable_after_w != drawable_before_w + 17 ||
      drawable_after_h != drawable_before_h + 11) {
    std::cerr << "Drawable resize committed inside a nested scene\n";
    PsyX_Shutdown();
    return 60;
  }

  const auto cached_clear_serial = GR_GetDepthClearSerial(0);
  GR_QueueDrawableSize(drawable_before_w + 9, drawable_before_h + 7);
  const auto cached_present = PsyX_PresentCachedFrame();
  int cached_drawable_w{};
  int cached_drawable_h{};
  GR_GetCommittedDrawableSize(&cached_drawable_w, &cached_drawable_h);
  GR_QueueDrawableSize(drawable_before_w, drawable_before_h);
  const auto restored_cached_present = PsyX_PresentCachedFrame();
  if (cached_present == 0 || restored_cached_present == 0 ||
      cached_drawable_w != drawable_before_w + 9 ||
      cached_drawable_h != drawable_before_h + 7 ||
      GR_GetDepthClearSerial(0) != cached_clear_serial) {
    std::cerr << "Cached present reran scene work or ignored drawable resize\n";
    PsyX_Shutdown();
    return 103;
  }
  // Catch-up submits can queue vertices decoded under different display modes
  // before their shared DrawSync. Each split must preserve the dimensions used
  // for both vertex normalization and projection replay.
  GR_BeginGuestProjectionEpoch(101, 64, 64, 0, 0);
  GR_BeginGuestSubmit();
  static_cast<void>(PsyX_BeginScene());
  GR_SetScissorState(0);
  glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  DRAWENV epoch_draw{};
  SetDefDrawEnv(&epoch_draw, 0, 0, 320, 240);
  epoch_draw.dtd = 0;
  epoch_draw.dfe = 1;
  epoch_draw.isbg = 0;
  PutDrawEnv(&epoch_draw);

  TILE epoch_red{};
  SetTile(&epoch_red);
  setRGB0(&epoch_red, 255, 0, 0);
  setXY0(&epoch_red, 0, 0);
  setWH(&epoch_red, 16, 16);
  DrawPrim(&epoch_red);

  GR_BeginGuestProjectionEpoch(102, 128, 64, 0, 0);
  GR_BeginGuestSubmit();
  TILE epoch_green{};
  SetTile(&epoch_green);
  setRGB0(&epoch_green, 0, 255, 0);
  setXY0(&epoch_green, 96, 0);
  setWH(&epoch_green, 32, 16);
  DrawPrim(&epoch_green);
  // Returning to a native LIBGPU display ends the raw guest epoch. The next
  // native primitive must normalize against PutDispEnv, not stale gameplay.
  PutDispEnv(&display);

  TILE epoch_blue{};
  SetTile(&epoch_blue);
  setRGB0(&epoch_blue, 0, 0, 255);
  setXY0(&epoch_blue, 140, 120);
  setWH(&epoch_blue, 40, 60);
  DrawPrim(&epoch_blue);

  DrawSync(0);

  std::array<std::uint8_t, 128U * 128U * 4U> epoch_frame{};
  glReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, epoch_frame.data());
  std::size_t red_left{};
  std::size_t red_right{};
  std::size_t green_left{};
  std::size_t green_right{};
  std::size_t blue_center{};
  for (std::size_t y{}; y < 128U; ++y) {
    for (std::size_t x{}; x < 128U; ++x) {
      const auto pixel = (y * 128U + x) * 4U;
      const bool red = epoch_frame[pixel] > 96U &&
                       epoch_frame[pixel] > epoch_frame[pixel + 1U] * 2U;
      const bool green = epoch_frame[pixel + 1U] > 96U &&
                         epoch_frame[pixel + 1U] > epoch_frame[pixel] * 2U;
      const bool blue = epoch_frame[pixel + 2U] > 96U &&
                        epoch_frame[pixel + 2U] > epoch_frame[pixel] * 2U;
      red_left += red && x < 48U;
      red_right += red && x >= 80U;
      green_left += green && x < 48U;
      green_right += green && x >= 80U;
      blue_center += blue && x >= 48U && x < 80U;
    }
  }
  PsyX_EndScene();
  if (red_left < 700U || green_right < 700U || red_right != 0U ||
      green_left != 0U || blue_center < 350U) {
    std::cerr << "Queued submits did not preserve split-local display "
                 "normalization/projection; colours="
              << red_left << '/' << red_right << '/' << green_left << '/'
              << green_right << '/' << blue_center << '\n';
    PsyX_Shutdown();
    return 58;
  }

  // Page seeding clears depth/stencil with writable masks. A transparent run
  // on the next page must republish GL_DEPTH_WRITEMASK=false even when the
  // renderer cache remembers the previous transparent page.
  GR_SetGuestDisplayGeometry(output_size, output_size);
  GR_BeginGuestSubmit();
  static_cast<void>(PsyX_BeginScene());
  GR_SetTexture(g_vramTexture, TF_16_BIT, TEXTURE_FILTER_NEAREST);
  RECT16 transparent_page_a{700, 0, output_size, output_size};
  GR_SetOffscreenState(&transparent_page_a, 1);
  GR_SetDepthState(1, 0);
  GLboolean transparent_page_a_write{GL_TRUE};
  glGetBooleanv(GL_DEPTH_WRITEMASK, &transparent_page_a_write);
  finishOffscreenPass();

  GR_BeginGuestSubmit();
  RECT16 transparent_page_b{700, 80, output_size, output_size};
  GR_SetOffscreenState(&transparent_page_b, 1);
  GR_SetDepthState(1, 0);
  GLboolean transparent_page_b_write{GL_TRUE};
  glGetBooleanv(GL_DEPTH_WRITEMASK, &transparent_page_b_write);
  finishOffscreenPass();
  GR_SetDepthState(0, 0);
  PsyX_EndScene();
  if (transparent_page_a_write != GL_FALSE ||
      transparent_page_b_write != GL_FALSE) {
    std::cerr << "Cross-page transparent run inherited a writable depth mask\n";
    PsyX_Shutdown();
    return 57;
  }

  const auto attached_storage = [] {
    GLint attachment_type{};
    GLint attachment{};
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                          &attachment_type);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                          &attachment);
    std::array<GLint, 3U> result{attachment_type, 0, 0};
    if (attachment_type == GL_TEXTURE) {
      GLint previous_texture{};
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(attachment));
      glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &result[1]);
      glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &result[2]);
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    }
    return result;
  };

  // The selected output owns one backing allocation. A contained draw changes
  // only the active viewport and patches its retained root; it must not
  // reallocate the multi-megabyte color/depth target.
  g_cfg_renderWidth = 1280;
  g_cfg_renderHeight = 720;
  GR_SetGuestDisplayGeometry(output_size, output_size);
  GR_BeginGuestSubmit();
  static_cast<void>(PsyX_BeginScene());
  GR_SetTexture(g_vramTexture, TF_16_BIT, TEXTURE_FILTER_NEAREST);
  RECT16 capacity_root{0, 96, output_size, output_size};
  const auto readbacks_before_ordinary_frame =
      GR_GetSynchronousVRAMReadbackCount();
  GR_SetOffscreenState(&capacity_root, 1);
  std::array<GLint, 4U> root_viewport{};
  glGetIntegerv(GL_VIEWPORT, root_viewport.data());
  const auto root_storage = attached_storage();

  // A catch-up submit can begin while the root remains active. It must not
  // relabel the first contained switch as another full-resolution root.
  GR_SetGuestDisplayGeometry(output_size, output_size);
  GR_BeginGuestSubmit();
  RECT16 capacity_nested{0, 96, output_size / 2, output_size / 2};
  GR_SetOffscreenState(&capacity_nested, 1);
  std::array<GLint, 4U> nested_viewport{};
  glGetIntegerv(GL_VIEWPORT, nested_viewport.data());
  const auto nested_storage = attached_storage();
  const auto nested_framebuffer_status =
      glCheckFramebufferStatus(GL_FRAMEBUFFER);
  finishOffscreenPass();
  const bool capacity_valid =
      root_viewport == std::array<GLint, 4U>{0, 0, 1280, 720} &&
      nested_viewport == std::array<GLint, 4U>{0, 0, 640, 360} &&
      root_storage[0] == GL_TEXTURE && root_storage[1] >= 1280 &&
      root_storage[2] >= 720 && nested_storage == root_storage &&
      GR_HasHighResolutionVRAM(0, 96, output_size, output_size) != 0 &&
      GR_GetSynchronousVRAMReadbackCount() == readbacks_before_ordinary_frame &&
      nested_framebuffer_status == GL_FRAMEBUFFER_COMPLETE;
  if (!capacity_valid) {
    std::cerr << "Selected-resolution root/nested target changed storage or "
                 "performed a synchronous ordinary-frame readback\n";
    PsyX_Shutdown();
    return 40;
  }

  // A stable geometry transition may occur during an unpresented catch-up
  // submit. Queue a complete old-page draw without DrawSync; publishing the
  // new geometry must flush and close it before the identical rect is reused.
  constexpr int transition_x = 256;
  constexpr int transition_y = 96;
  RECT16 transition_rect{transition_x, transition_y, output_size, output_size};
  GR_SetGuestDisplayGeometry(output_size, output_size);
  GR_BeginGuestSubmit();
  DRAWENV old_transition_draw{};
  SetDefDrawEnv(&old_transition_draw, transition_x, transition_y, output_size,
                output_size);
  old_transition_draw.dtd = 0;
  old_transition_draw.dfe = 0;
  old_transition_draw.isbg = 0;
  PutDrawEnv(&old_transition_draw);
  TILE old_transition_tile{};
  SetTile(&old_transition_tile);
  setRGB0(&old_transition_tile, 255, 0, 0);
  setXY0(&old_transition_tile, 0, 0);
  setWH(&old_transition_tile, output_size, output_size);
  DrawPrim(&old_transition_tile);

  const auto before_geometry_transition = GR_GetSynchronousVRAMReadbackCount();
  GR_SetGuestDisplayGeometry(output_size / 2, output_size / 2);
  const auto after_geometry_boundary = GR_GetSynchronousVRAMReadbackCount();
  const bool old_transition_page_retained =
      GR_HasHighResolutionVRAM(transition_x, transition_y, output_size,
                               output_size) != 0;
  std::array<std::uint16_t, output_size * output_size> old_transition_pixels{};
  GR_ReadVRAM(old_transition_pixels.data(), transition_x, transition_y,
              output_size, output_size);
  const auto after_old_transition_read = GR_GetSynchronousVRAMReadbackCount();
  const auto old_transition_red =
      std::ranges::count_if(old_transition_pixels, [](const auto pixel) {
        return (pixel & 0x7fffU) == 0x001fU;
      });
  const auto old_transition_zero =
      std::ranges::count_if(old_transition_pixels, [](const auto pixel) {
        return (pixel & 0x7fffU) == 0x0000U;
      });
  const bool old_transition_exact =
      old_transition_red ==
      static_cast<std::ptrdiff_t>(old_transition_pixels.size());
  if (after_geometry_boundary != before_geometry_transition ||
      after_old_transition_read != before_geometry_transition + 1U ||
      !old_transition_page_retained || !old_transition_exact) {
    std::cerr << "Geometry publication did not flush/close the queued old "
                 "guest page exactly; counters="
              << before_geometry_transition << '/' << after_geometry_boundary
              << '/' << after_old_transition_read
              << " retained=" << old_transition_page_retained
              << " red=" << old_transition_red
              << " zero=" << old_transition_zero << " first=0x" << std::hex
              << old_transition_pixels.front() << " center=0x"
              << old_transition_pixels[old_transition_pixels.size() / 2U]
              << std::dec << '\n';
    PsyX_Shutdown();
    return 54;
  }

  GR_BeginGuestSubmit();
  DRAWENV new_transition_draw{};
  SetDefDrawEnv(&new_transition_draw, transition_x, transition_y, output_size,
                output_size);
  new_transition_draw.dtd = 0;
  new_transition_draw.dfe = 0;
  new_transition_draw.isbg = 0;
  PutDrawEnv(&new_transition_draw);
  TILE new_transition_tile{};
  SetTile(&new_transition_tile);
  setRGB0(&new_transition_tile, 0, 0, 255);
  setXY0(&new_transition_tile, 0, 0);
  setWH(&new_transition_tile, output_size, output_size);
  DrawPrim(&new_transition_tile);
  DrawSync(0);
  std::array<GLint, 4U> transitioned_viewport{};
  glGetIntegerv(GL_VIEWPORT, transitioned_viewport.data());
  RECT16 completed_transition{};
  GR_SetOffscreenState(&completed_transition, 0);
  const auto before_new_transition_read = GR_GetSynchronousVRAMReadbackCount();
  std::array<std::uint16_t, output_size * output_size> new_transition_pixels{};
  GR_ReadVRAM(new_transition_pixels.data(), transition_x, transition_y,
              output_size, output_size);
  const auto after_new_transition_read = GR_GetSynchronousVRAMReadbackCount();
  const bool new_transition_exact =
      std::ranges::all_of(new_transition_pixels, [](const auto pixel) {
        return (pixel & 0x7fffU) == 0x7c00U;
      });
  if (transitioned_viewport != std::array<GLint, 4U>{0, 0, 1280, 720} ||
      before_new_transition_read != after_old_transition_read ||
      after_new_transition_read != before_new_transition_read + 1U ||
      !new_transition_exact) {
    std::cerr << "Queued new-geometry page mixed with the old guest domain\n";
    PsyX_Shutdown();
    return 55;
  }

  // Reconfigure the selected target while the same page is active. Storage
  // remains capacity-sized while the page is closed/reseeded at the new extent.
  GR_BeginGuestSubmit();
  GR_SetOffscreenState(&transition_rect, 1);
  g_cfg_renderWidth = 640;
  g_cfg_renderHeight = 360;
  GR_SetOffscreenState(&transition_rect, 1);
  std::array<GLint, 4U> reconfigured_viewport{};
  glGetIntegerv(GL_VIEWPORT, reconfigured_viewport.data());
  const auto reconfigured_storage = attached_storage();
  finishOffscreenPass();
  g_cfg_renderWidth = 1280;
  g_cfg_renderHeight = 720;
  if (reconfigured_viewport != std::array<GLint, 4U>{0, 0, 640, 360} ||
      reconfigured_storage != root_storage ||
      GR_GetSynchronousVRAMReadbackCount() != after_new_transition_read) {
    std::cerr << "Active selected-output reconfigure left stale extent/storage "
                 "state\n";
    PsyX_Shutdown();
    return 56;
  }
  GR_SetGuestDisplayGeometry(output_size, output_size);
  GR_BeginGuestSubmit();
  const auto readbacks_before_lazy_sync = GR_GetSynchronousVRAMReadbackCount();
  std::array<std::uint16_t, output_size * output_size> synchronized_zero{};
  GR_ReadVRAM(synchronized_zero.data(), 0, 96, output_size, output_size);
  const auto after_first_sync = GR_GetSynchronousVRAMReadbackCount();
  GR_ReadVRAM(synchronized_zero.data(), 0, 96, output_size, output_size);
  if (after_first_sync != readbacks_before_lazy_sync + 1U ||
      GR_GetSynchronousVRAMReadbackCount() != after_first_sync ||
      std::ranges::any_of(synchronized_zero,
                          [](const auto pixel) { return pixel != 0U; })) {
    std::cerr << "Lazy guest VRAM synchronization was not exact/on-demand\n";
    PsyX_Shutdown();
    return 39;
  }
  PsyX_EndScene();

  // Upload an asymmetric 2x2 PS1 pattern, inspect the unpacked render target
  // directly, then pack it back. This catches an orientation error even when
  // matching pack/unpack flips would otherwise cancel in a round trip.
  RECT16 orientation_rect{900, 100, 2, 2};
  std::array<std::uint16_t, 4U> orientation_words{0x001fU, 0x03e0U, 0x7c00U,
                                                  0x7fffU};
  LoadImage(&orientation_rect,
            reinterpret_cast<u_long *>(orientation_words.data()));
  GR_UpdateVRAM();
  g_cfg_renderWidth = 8;
  g_cfg_renderHeight = 8;
  GR_SetGuestDisplayGeometry(2, 2);
  GR_BeginGuestSubmit();
  static_cast<void>(PsyX_BeginScene());
  GR_SetTexture(g_vramTexture, TF_16_BIT, TEXTURE_FILTER_NEAREST);
  GR_SetOffscreenState(&orientation_rect, 1);
  const auto read_orientation_pixel = [](int x, int y) {
    std::array<unsigned char, 4U> pixel{};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    return pixel;
  };
  const auto bottom_left = read_orientation_pixel(1, 1);
  const auto bottom_right = read_orientation_pixel(6, 1);
  const auto top_left = read_orientation_pixel(1, 6);
  const auto top_right = read_orientation_pixel(6, 6);
  const auto is_rgb = [](const auto &pixel, unsigned char red,
                         unsigned char green, unsigned char blue) {
    return pixel[0] == red && pixel[1] == green && pixel[2] == blue;
  };
  const bool unpack_orientation_exact =
      is_rgb(bottom_left, 0, 0, 255) && is_rgb(bottom_right, 255, 255, 255) &&
      is_rgb(top_left, 255, 0, 0) && is_rgb(top_right, 0, 255, 0);
  finishOffscreenPass();
  std::array<std::uint16_t, 4U> orientation_round_trip{};
  GR_ReadVRAM(orientation_round_trip.data(), orientation_rect.x,
              orientation_rect.y, orientation_rect.w, orientation_rect.h);
  if (!unpack_orientation_exact ||
      orientation_round_trip != orientation_words) {
    std::cerr << "Guest VRAM GPU conversion changed asymmetric orientation or "
                 "packed PS1 words\n";
    PsyX_EndScene();
    PsyX_Shutdown();
    return 50;
  }
  PsyX_EndScene();

  // Two disjoint CPU uploads on one row must not upload their stale CPU gap
  // over a framebuffer word which is still authoritative on the GPU.
  constexpr int dirty_gap_x = 920;
  constexpr int dirty_gap_y = 120;
  RECT16 dirty_gap_rect{dirty_gap_x + 1, dirty_gap_y, 1, 1};
  const int saved_composed_scanout = g_cfg_composedGuestScanout;
  g_cfg_composedGuestScanout = 1;
  GR_SetGuestDisplayGeometry(1, 1);
  GR_BeginGuestSubmit();
  static_cast<void>(PsyX_BeginScene());
  GR_SetTexture(g_vramTexture, TF_16_BIT, TEXTURE_FILTER_NEAREST);
  GR_SetOffscreenState(&dirty_gap_rect, 1);
  GR_Clear(0, 0, 1, 1, 255, 0, 0);
  finishOffscreenPass();
  PsyX_EndScene();

  RECT16 dirty_left_rect{dirty_gap_x, dirty_gap_y, 1, 1};
  RECT16 dirty_right_rect{dirty_gap_x + 2, dirty_gap_y, 1, 1};
  std::array<std::uint16_t, 2U> dirty_left_words{0x03e0U, 0U};
  std::array<std::uint16_t, 2U> dirty_right_words{0x7c00U, 0U};
  LoadImage(&dirty_left_rect,
            reinterpret_cast<u_long *>(dirty_left_words.data()));
  LoadImage(&dirty_right_rect,
            reinterpret_cast<u_long *>(dirty_right_words.data()));
  GR_UpdateVRAM();
  std::array<std::uint16_t, 3U> dirty_gap_result{};
  GR_ReadVRAM(dirty_gap_result.data(), dirty_gap_x, dirty_gap_y,
              static_cast<int>(dirty_gap_result.size()), 1);
  g_cfg_composedGuestScanout = saved_composed_scanout;
  if (dirty_gap_result !=
      std::array<std::uint16_t, 3U>{0x03e0U, 0x001fU, 0x7c00U}) {
    std::cerr << "Exact VRAM dirty mask overwrote GPU-authoritative gap: "
              << std::hex << dirty_gap_result[0] << ',' << dirty_gap_result[1]
              << ',' << dirty_gap_result[2] << std::dec << '\n';
    PsyX_Shutdown();
    return 74;
  }

  g_cfg_renderWidth = 128;
  g_cfg_renderHeight = 128;
  GR_SetGuestDisplayGeometry(output_size, output_size);
  GR_BeginGuestSubmit();
  DRAWENV solid_draw{};
  SetDefDrawEnv(&solid_draw, 0, 128, output_size, output_size);
  solid_draw.dtd = 0;
  solid_draw.dfe = 0;
  solid_draw.isbg = 0;
  PutDrawEnv(&solid_draw);

  TILE tile{};
  SetTile(&tile);
  setRGB0(&tile, 255, 0, 0);
  setXY0(&tile, 0, 0);
  setWH(&tile, output_size, output_size);
  DrawPrim(&tile);
  DrawSync(0);
  std::array<GLint, 4U> scaled_viewport{};
  glGetIntegerv(GL_VIEWPORT, scaled_viewport.data());
  const auto scaled_framebuffer_status =
      glCheckFramebufferStatus(GL_FRAMEBUFFER);
  const auto scaled_framebuffer_error = glGetError();
  RECT16 completed_target{};
  GR_SetOffscreenState(&completed_target, 0);
  if (scaled_viewport != std::array<GLint, 4U>{0, 0, 128, 128} ||
      scaled_framebuffer_status != GL_FRAMEBUFFER_COMPLETE ||
      scaled_framebuffer_error != GL_NO_ERROR) {
    std::cerr << "Selected-resolution guest target was not a complete 128x128 "
                 "framebuffer; "
              << "viewport=" << scaled_viewport[0] << ',' << scaled_viewport[1]
              << ',' << scaled_viewport[2] << ',' << scaled_viewport[3]
              << " status=0x" << std::hex << scaled_framebuffer_status
              << " error=0x" << scaled_framebuffer_error << std::dec << '\n';
    PsyX_Shutdown();
    return 24;
  }

  if (GR_HasHighResolutionVRAM(0, 128, output_size, output_size) == 0) {
    std::cerr << "Guest page was not retained at selected resolution\n";
    PsyX_Shutdown();
    return 18;
  }

  // Exercise the production GP0(80) adapter while the source page is still
  // GPU-authoritative. A non-wrapped move stays entirely in packed GPU VRAM;
  // the two explicit verification reads below synchronize source and
  // destination independently.
  sf::platform::detail::PsyCrossGuestGpu move_gpu;
  constexpr int no_wrap_destination_x = 256;
  const std::array<std::uint32_t, 4U> no_wrap_move{
      0x80000000U,
      128U << 16U,
      static_cast<std::uint32_t>(no_wrap_destination_x) | (128U << 16U),
      static_cast<std::uint32_t>(output_size) |
          (static_cast<std::uint32_t>(output_size) << 16U),
  };
  const auto before_no_wrap_move = GR_GetSynchronousVRAMReadbackCount();
  move_gpu.submit(no_wrap_move);
  const auto after_no_wrap_move = GR_GetSynchronousVRAMReadbackCount();
  const bool move_stayed_on_gpu = after_no_wrap_move == before_no_wrap_move;
  std::array<std::uint16_t, output_size * output_size> no_wrap_source{};
  std::array<std::uint16_t, output_size * output_size> no_wrap_destination{};
  GR_ReadVRAM(no_wrap_source.data(), 0, 128, output_size, output_size);
  GR_ReadVRAM(no_wrap_destination.data(), no_wrap_destination_x, 128,
              output_size, output_size);
  if (!move_stayed_on_gpu ||
      GR_GetSynchronousVRAMReadbackCount() != before_no_wrap_move + 2U ||
      no_wrap_source != no_wrap_destination ||
      !containsColor(no_wrap_destination, 0x001fU)) {
    std::cerr << "Production GP0(80) no-wrap move left the GPU or was not "
                 "exact\n";
    PsyX_Shutdown();
    return 48;
  }

  GR_ClearVRAM(700, 400, 1, 1, 0, 0, 0);
  if (GR_HasHighResolutionVRAM(0, 128, output_size, output_size) == 0) {
    std::cerr << "Non-overlapping VRAM write invalidated retained page\n";
    PsyX_Shutdown();
    return 19;
  }
  GR_ClearVRAM(1, 129, 1, 1, 0, 0, 0);
  if (GR_HasHighResolutionVRAM(0, 128, output_size, output_size) == 0) {
    std::cerr << "Overlapping VRAM write discarded the stable retained page\n";
    PsyX_Shutdown();
    return 20;
  }

  // A source that wraps in both axes is partitioned into four rectangles, not
  // one readback per row. Distinct quadrant colours also lock wrap ordering.
  const auto render_wrap_chunk = [](int x, int y, unsigned char red,
                                    unsigned char green, unsigned char blue) {
    constexpr int chunk_width = 4;
    constexpr int chunk_height = 2;
    GR_SetGuestDisplayGeometry(chunk_width, chunk_height);
    GR_BeginGuestSubmit();
    DRAWENV draw{};
    SetDefDrawEnv(&draw, x, y, chunk_width, chunk_height);
    draw.dtd = 0;
    draw.dfe = 0;
    draw.isbg = 0;
    PutDrawEnv(&draw);
    TILE chunk{};
    SetTile(&chunk);
    setRGB0(&chunk, red, green, blue);
    setXY0(&chunk, 0, 0);
    setWH(&chunk, chunk_width, chunk_height);
    DrawPrim(&chunk);
    finishOffscreenPass();
  };
  render_wrap_chunk(1020, 510, 255, 0, 0);
  render_wrap_chunk(0, 510, 0, 255, 0);
  render_wrap_chunk(1020, 0, 0, 0, 255);
  render_wrap_chunk(0, 0, 255, 255, 255);

  constexpr int wrapped_width = 8;
  constexpr int wrapped_height = 4;
  constexpr int wrapped_destination_x = 500;
  constexpr int wrapped_destination_y = 300;
  const std::array<std::uint32_t, 4U> wrapped_move{
      0x80000000U,
      1020U | (510U << 16U),
      static_cast<std::uint32_t>(wrapped_destination_x) |
          (static_cast<std::uint32_t>(wrapped_destination_y) << 16U),
      static_cast<std::uint32_t>(wrapped_width) |
          (static_cast<std::uint32_t>(wrapped_height) << 16U),
  };
  const auto before_wrapped_move = GR_GetSynchronousVRAMReadbackCount();
  move_gpu.submit(wrapped_move);
  const auto after_wrapped_move = GR_GetSynchronousVRAMReadbackCount();
  std::array<std::uint16_t, wrapped_width * wrapped_height> wrapped_pixels{};
  GR_ReadVRAM(wrapped_pixels.data(), wrapped_destination_x,
              wrapped_destination_y, wrapped_width, wrapped_height);
  bool wrapped_exact =
      after_wrapped_move == before_wrapped_move + 4U &&
      GR_GetSynchronousVRAMReadbackCount() == after_wrapped_move;
  for (int y = 0; y < wrapped_height; ++y) {
    for (int x = 0; x < wrapped_width; ++x) {
      const auto expected =
          y < 2 ? (x < 4 ? 0x001fU : 0x03e0U) : (x < 4 ? 0x7c00U : 0x7fffU);
      wrapped_exact =
          wrapped_exact &&
          (wrapped_pixels[static_cast<std::size_t>(y) * wrapped_width + x] &
           0x7fffU) == expected;
    }
  }
  if (!wrapped_exact) {
    std::cerr << "Production GP0(80) wrapped move was not exact or bounded to "
                 "four rectangular readbacks\n";
    PsyX_Shutdown();
    return 49;
  }
  GR_SetGuestDisplayGeometry(output_size, output_size);
  GR_BeginGuestSubmit();

  std::array<std::uint16_t, output_size * output_size> solid_pixels{};
  GR_ReadVRAM(solid_pixels.data(), 0, 128, output_size, output_size);
  if (!containsColor(solid_pixels, 0x001fU)) {
    const auto nonzero = std::ranges::count_if(
        solid_pixels, [](const auto pixel) { return pixel != 0U; });
    std::cerr << "Solid offscreen primitive produced no red pixels; nonzero="
              << nonzero << " first=0x" << std::hex << solid_pixels.front()
              << std::dec << '\n';
    PsyX_EndScene();
    PsyX_Shutdown();
    return 2;
  }
  PsyX_EndScene();

  constexpr int texture_x = 512;
  constexpr int texture_y = 0;
  constexpr int clut_x = 512;
  constexpr int clut_y = 250;
  std::vector<std::uint16_t> texture_words(output_size / 2 * output_size,
                                           0x0101U);
  RECT16 texture_rect{texture_x, texture_y, output_size / 2, output_size};
  LoadImage(&texture_rect, reinterpret_cast<u_long *>(texture_words.data()));

  std::array<std::uint16_t, 256> clut{};
  clut[1] = 0x03e0U;
  RECT16 clut_rect{clut_x, clut_y, static_cast<short>(clut.size()), 1};
  LoadImage(&clut_rect, reinterpret_cast<u_long *>(clut.data()));

  DRAWENV textured_draw{};
  SetDefDrawEnv(&textured_draw, 0, 256, output_size, output_size);
  textured_draw.dtd = 0;
  textured_draw.dfe = 0;
  textured_draw.isbg = 0;
  PutDrawEnv(&textured_draw);

  DR_TPAGE page{};
  SetDrawTPage(&page, 0, 0, GetTPage(1, 0, texture_x, texture_y));
  DrawPrim(&page);

  SPRT sprite{};
  SetSprt(&sprite);
  setRGB0(&sprite, 128, 128, 128);
  setXY0(&sprite, 0, 0);
  setUV0(&sprite, 0, 0);
  setWH(&sprite, output_size, output_size);
  setClut(&sprite, clut_x, clut_y);
  DrawPrim(&sprite);
  finishOffscreenPass();

  std::array<std::uint16_t, output_size * output_size> textured_pixels{};
  GR_ReadVRAM(textured_pixels.data(), 0, 256, output_size, output_size);
  if (!containsColor(textured_pixels, 0x03e0U)) {
    const auto nonzero =
        std::ranges::count_if(textured_pixels, [](const auto pixel) {
          return (pixel & 0x7fffU) != 0U;
        });
    std::cerr << "8-bit textured offscreen primitive failed; nonzero="
              << nonzero << '\n';
    PsyX_EndScene();
    PsyX_Shutdown();
    return 3;
  }

  PsyX_EndScene();

  sf::platform::detail::PsyCrossGuestGpu guest_gpu;
  std::vector<std::uint32_t> commands;
  const auto append_upload =
      [&commands](int x, int y, int width, int height,
                  std::span<const std::uint16_t> pixels) {
        commands.push_back(0xa0000000U);
        commands.push_back(static_cast<std::uint32_t>(x) |
                           (static_cast<std::uint32_t>(y) << 16U));
        commands.push_back(static_cast<std::uint32_t>(width) |
                           (static_cast<std::uint32_t>(height) << 16U));
        for (std::size_t pixel = 0; pixel < pixels.size(); pixel += 2U) {
          const auto low = static_cast<std::uint32_t>(pixels[pixel]);
          const auto high = pixel + 1U < pixels.size()
                                ? static_cast<std::uint32_t>(pixels[pixel + 1U])
                                : 0U;
          commands.push_back(low | (high << 16U));
        }
      };
  append_upload(texture_x, texture_y, output_size / 2, output_size,
                texture_words);
  append_upload(clut_x, clut_y, static_cast<int>(clut.size()), 1, clut);

  constexpr int guest_output_y = 384;
  commands.push_back(0xe3000000U |
                     (static_cast<std::uint32_t>(guest_output_y) << 10U));
  commands.push_back(
      0xe4000000U | static_cast<std::uint32_t>(output_size - 1) |
      (static_cast<std::uint32_t>(guest_output_y + output_size - 1) << 10U));
  commands.push_back(0xe5000000U |
                     (static_cast<std::uint32_t>(guest_output_y) << 11U));
  commands.push_back(0xe1000000U | static_cast<std::uint32_t>(
                                       GetTPage(1, 0, texture_x, texture_y)));
  commands.push_back(0x64808080U);
  commands.push_back(0U);
  commands.push_back(static_cast<std::uint32_t>(GetClut(clut_x, clut_y))
                     << 16U);
  commands.push_back(static_cast<std::uint32_t>(output_size) |
                     (static_cast<std::uint32_t>(output_size) << 16U));

  guest_gpu.submit(commands);
  DrawSync(0);
  PsyX_EndScene();

  std::array<std::uint16_t, output_size * output_size> guest_pixels{};
  GR_ReadVRAM(guest_pixels.data(), 0, guest_output_y, output_size, output_size);
  if (!containsColor(guest_pixels, 0x03e0U)) {
    const auto nonzero = std::ranges::count_if(
        guest_pixels, [](const auto pixel) { return (pixel & 0x7fffU) != 0U; });
    std::cerr << "Raw GP0 guest textured primitive failed; nonzero=" << nonzero
              << " submitted=" << guest_gpu.submittedCommands()
              << " unsupported=" << guest_gpu.unsupportedCommands() << '\n';
    PsyX_Shutdown();
    return 4;
  }
  if (guest_gpu.unsupportedCommands() != 0U) {
    std::cerr << "Raw GP0 guest stream reported unsupported commands\n";
    PsyX_Shutdown();
    return 5;
  }

  // Retail can use a full-VRAM draw area for a tiny primitive while texture
  // pages are already resident elsewhere in that area. The offscreen bridge
  // must preserve pixels which the primitive does not touch.
  constexpr int preserved_x = 896;
  constexpr int preserved_y = 0;
  constexpr int preserved_width = 64;
  constexpr int preserved_height = 256;
  constexpr std::uint16_t preserved_color = 0x1234U;
  // Bit 15 is also the high bit of the fourth packed 4-bit texel and of the
  // second packed 8-bit texel. Cover both transparent black and mixed words.
  constexpr std::array<std::uint16_t, 12U> stp_roundtrip_words{
      0x0000U, 0x8000U, 0x000fU, 0x800fU, 0x00f0U, 0x80f0U,
      0x0f00U, 0x8f00U, 0x7000U, 0xf000U, 0x1234U, 0x9234U};
  std::vector<std::uint16_t> preserved_texture(
      preserved_width * preserved_height, preserved_color);
  std::ranges::copy(stp_roundtrip_words, preserved_texture.begin());
  RECT16 preserved_rect{preserved_x, preserved_y, preserved_width,
                        preserved_height};
  LoadImage(&preserved_rect,
            reinterpret_cast<u_long *>(preserved_texture.data()));
  GR_UpdateVRAM();

  DRAWENV wide_draw{};
  SetDefDrawEnv(&wide_draw, 0, 0, 1024, 512);
  wide_draw.dtd = 0;
  wide_draw.dfe = 0;
  wide_draw.isbg = 0;
  PutDrawEnv(&wide_draw);
  TILE tiny_tile{};
  SetTile(&tiny_tile);
  setRGB0(&tiny_tile, 255, 0, 0);
  setXY0(&tiny_tile, 0, 0);
  setWH(&tiny_tile, 1, 1);
  DrawPrim(&tiny_tile);
  DrawSync(0);
  PsyX_EndScene();

  std::vector<std::uint16_t> preserved_result(preserved_texture.size());
  GR_ReadVRAM(preserved_result.data(), preserved_x, preserved_y,
              preserved_width, preserved_height);
  std::size_t corrupted{};
  for (std::size_t pixel{}; pixel < preserved_result.size(); ++pixel) {
    if (preserved_result[pixel] != preserved_texture[pixel]) {
      ++corrupted;
    }
  }
  if (corrupted != 0) {
    std::cerr << "Wide offscreen draw changed exact resident VRAM words; "
              << "corrupted=" << corrupted << '\n';
    PsyX_Shutdown();
    return 6;
  }

  // A textured semitransparent draw must also write its source STP bit. Using
  // the RGB add factors for alpha saturates it to opaque and the next VRAM
  // texture read then changes packed 4/8-bit indices.
  constexpr int blend_size = 8;
  constexpr int blend_texture_x = 768;
  constexpr int blend_texture_y = 256;
  constexpr int blend_target_x = 128;
  constexpr int blend_target_y = 128;
  const int saved_blend_aspect_mode = g_cfg_aspectMode;
  g_cfg_aspectMode = PSYX_ASPECT_ORIGINAL_4_3;
  std::array<std::uint16_t, blend_size * blend_size> blend_texture{};
  blend_texture.fill(0x8010U);
  RECT16 blend_texture_rect{blend_texture_x, blend_texture_y, blend_size,
                            blend_size};
  LoadImage(&blend_texture_rect,
            reinterpret_cast<u_long *>(blend_texture.data()));
  std::array<std::uint16_t, blend_size * blend_size> blend_destination{};
  blend_destination.fill(0x0010U);
  RECT16 blend_target_rect{blend_target_x, blend_target_y, blend_size,
                           blend_size};
  LoadImage(&blend_target_rect,
            reinterpret_cast<u_long *>(blend_destination.data()));
  GR_UpdateVRAM();

  DRAWENV blend_draw{};
  SetDefDrawEnv(&blend_draw, blend_target_x, blend_target_y, blend_size,
                blend_size);
  blend_draw.dtd = 0;
  blend_draw.dfe = 0;
  blend_draw.isbg = 0;
  PutDrawEnv(&blend_draw);
  DR_TPAGE blend_page{};
  SetDrawTPage(&blend_page, 0, 0,
               GetTPage(2, 1, blend_texture_x, blend_texture_y));
  DrawPrim(&blend_page);
  SPRT blend_sprite{};
  SetSprt(&blend_sprite);
  setSemiTrans(&blend_sprite, 1);
  setRGB0(&blend_sprite, 128, 128, 128);
  setXY0(&blend_sprite, 0, 0);
  setUV0(&blend_sprite, 0, 0);
  setWH(&blend_sprite, blend_size, blend_size);
  DrawPrim(&blend_sprite);
  finishOffscreenPass();

  std::array<std::uint16_t, blend_size * blend_size> blend_result{};
  GR_ReadVRAM(blend_result.data(), blend_target_x, blend_target_y, blend_size,
              blend_size);
  g_cfg_aspectMode = saved_blend_aspect_mode;
  const auto retained_stp = std::ranges::all_of(
      blend_result, [](const auto pixel) { return (pixel & 0x8000U) != 0U; });
  if (!retained_stp) {
    std::cerr << "Semitransparent framebuffer feedback lost source STP\n";
    PsyX_Shutdown();
    return 7;
  }
  PsyX_EndScene();

  // LEVEL alternates a 512x240 draw page behind the displayed page. Exercise
  // its first captured world primitive independently of the 2D sprite path.
  constexpr int level_draw_y = 240;
  constexpr int level_draw_width = 512;
  constexpr int level_draw_height = 240;
  std::vector<std::uint16_t> level_texture(64U * 256U, 0x1111U);
  RECT16 level_texture_rect{896, 0, 64, 256};
  LoadImage(&level_texture_rect,
            reinterpret_cast<u_long *>(level_texture.data()));
  std::array<std::uint16_t, 16> level_clut{};
  level_clut[1] = 0x7fffU;
  RECT16 level_clut_rect{0, 498, 16, 1};
  LoadImage(&level_clut_rect, reinterpret_cast<u_long *>(level_clut.data()));
  GR_ClearVRAM(0, level_draw_y, level_draw_width, level_draw_height, 0, 0, 0);
  GR_UpdateVRAM();

  sf::platform::detail::PsyCrossGuestGpu level_gpu;
  constexpr std::array<std::uint32_t, 15> level_gt3{
      // Exact LEVEL draw state captured at guest frame 7368.
      0xe303c000U,
      0xe4077dffU,
      0xe5078000U,
      0xe100060aU,
      0xe2000000U,
      0xe6000000U,
      // Raw GT3 geometry, UVs, CLUT and tpage from that frame. Only colours
      // are normalized so a successful draw is unambiguous in VRAM.
      0x34808080U,
      0x00230055U,
      0x7c80bf9fU,
      0x00808080U,
      0xfff0002eU,
      0x300e8080U,
      0x00808080U,
      0xfff60054U,
      0x0000809fU,
  };
  level_gpu.submit(level_gt3);
  DrawSync(0);
  PsyX_EndScene();

  std::vector<std::uint16_t> level_pixels(level_draw_width * level_draw_height);
  GR_ReadVRAM(level_pixels.data(), 0, level_draw_y, level_draw_width,
              level_draw_height);
  const auto level_nonzero = std::ranges::count_if(
      level_pixels, [](const auto pixel) { return (pixel & 0x7fffU) != 0U; });
  if (level_nonzero == 0) {
    std::cerr << "LEVEL-like raw GT3 produced no VRAM pixels; submitted="
              << level_gpu.submittedCommands()
              << " unsupported=" << level_gpu.unsupportedCommands() << '\n';
    PsyX_Shutdown();
    return 8;
  }
  if (level_gpu.submittedCommands() != 7U ||
      level_gpu.unsupportedCommands() != 0U) {
    std::cerr << "LEVEL-like raw GT3 command accounting mismatch; nonzero="
              << level_nonzero << " submitted=" << level_gpu.submittedCommands()
              << " unsupported=" << level_gpu.unsupportedCommands() << '\n';
    PsyX_Shutdown();
    return 9;
  }

  // Raw retail packets do not execute PsyCross's recompiled addPrim macro.
  // Verify that SWC2/DMA provenance reaches the real PGXP cache and that a
  // partially tagged primitive falls back atomically instead of mixing W=1
  // and perspective-correct vertices.
  PsyX_EndScene();
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu pgxp_gpu;
  constexpr std::array<std::uint32_t, 7U> pgxp_triangle{
      0xe3000000U, 0xe400fc3fU, 0xe5000000U, 0x20ff0000U,
      0x00040004U, 0x00040014U, 0x00140004U,
  };
  std::array<sf::psx::GteProjectedVertex, pgxp_triangle.size()>
      pgxp_projections{};
  const auto set_projection = [&](std::size_t word, float screen_x,
                                  float screen_y) {
    auto &projection = pgxp_projections[word];
    projection.packed_sxy = pgxp_triangle[word];
    projection.view_x = screen_x * 1000.0F / 320.0F;
    projection.view_y = screen_y * 1000.0F / 320.0F;
    projection.view_z = 1000.0F;
    projection.projective_depth = 1000.0F;
    projection.screen_x = screen_x;
    projection.screen_y = screen_y;
    projection.screen_h = 320.0F;
    projection.valid = true;
  };
  set_projection(4U, 4.25F, 4.5F);
  set_projection(5U, 20.5F, 4.75F);
  set_projection(6U, 4.75F, 20.25F);
  pgxp_gpu.submit(pgxp_triangle, pgxp_projections);

  PGXPVData first_precise{};
  PGXPVData last_precise{};
  if (pgxp_gpu.precisePrimitives() != 1U || PGXP_GetIndex(0) != 3U ||
      PGXP_GetCacheDataExact(&first_precise, 0U) == 0 ||
      PGXP_GetCacheDataExact(&last_precise, 2U) == 0 ||
      first_precise.sx != 4.25F || first_precise.sy != 4.5F ||
      last_precise.sx != 4.75F || last_precise.sy != 20.25F ||
      first_precise.precise_screen_position == 0U || first_precise.pz <= 0.0F ||
      first_precise.scr_h != 320.0F) {
    std::cerr << "Raw PGXP triangle lost precise GTE/DMA provenance; "
              << "precise=" << pgxp_gpu.precisePrimitives()
              << " cache=" << PGXP_GetIndex(0) << '\n';
    PsyX_Shutdown();
    return 10;
  }
  if (PGXP_GetIndex(1) != static_cast<u_short>(0xffffU)) {
    std::cerr << "Raw PGXP primitive leaked its transform latch to native "
                 "addPrim\n";
    PsyX_Shutdown();
    return 17;
  }

  constexpr std::array<std::uint32_t, 4U> fallback_triangle{
      pgxp_triangle[3], pgxp_triangle[4], pgxp_triangle[5], pgxp_triangle[6]};
  std::array<sf::psx::GteProjectedVertex, fallback_triangle.size()>
      fallback_projections{};
  fallback_projections[1] = pgxp_projections[4];
  fallback_projections[2] = pgxp_projections[5];
  // Deliberately leave vertex 2 invalid.
  pgxp_gpu.submit(fallback_triangle, fallback_projections);
  if (pgxp_gpu.precisePrimitives() != 1U || PGXP_GetIndex(0) != 3U) {
    std::cerr << "Incomplete raw PGXP primitive did not fall back atomically; "
              << "precise=" << pgxp_gpu.precisePrimitives()
              << " cache=" << PGXP_GetIndex(0) << '\n';
    PsyX_Shutdown();
    return 11;
  }
  auto non_finite_projections = fallback_projections;
  non_finite_projections[3] = pgxp_projections[6];
  non_finite_projections[2].screen_x = std::numeric_limits<float>::quiet_NaN();
  pgxp_gpu.submit(fallback_triangle, non_finite_projections);
  if (pgxp_gpu.precisePrimitives() != 1U ||
      pgxp_gpu.nonfiniteProjectionPrimitives() != 1U ||
      PGXP_GetIndex(0) != 3U) {
    std::cerr << "Non-finite PGXP tuple escaped raw packet fallback; precise="
              << pgxp_gpu.precisePrimitives() << " cache=" << PGXP_GetIndex(0)
              << '\n';
    PsyX_Shutdown();
    return 16;
  }

  // PsyCross triangulates quads as x0,x1,x3,x2. Its exact PGXP lookups use
  // cache offsets end-4,end-3,end-1,end-2, so the cache itself must remain in
  // raw packet order x0,x1,x2,x3. Lock this for both textured quad layouts.
  constexpr std::array<std::uint32_t, 9U> pgxp_ft4{
      0x2cffffffU, 0x00080008U, 0x00000000U, 0x00080018U, 0x00000010U,
      0x00180008U, 0x00001000U, 0x00180018U, 0x00001010U,
  };
  std::array<sf::psx::GteProjectedVertex, pgxp_ft4.size()> ft4_projections{};
  const auto make_projection = [](std::uint32_t packed, float screen_x,
                                  float screen_y) {
    sf::psx::GteProjectedVertex projection{};
    projection.packed_sxy = packed;
    projection.view_x = screen_x * 1000.0F / 320.0F;
    projection.view_y = screen_y * 1000.0F / 320.0F;
    projection.view_z = 1000.0F;
    projection.projective_depth = 1000.0F;
    projection.screen_x = screen_x;
    projection.screen_y = screen_y;
    projection.screen_h = 320.0F;
    projection.valid = true;
    return projection;
  };
  constexpr std::array<std::size_t, 4U> ft4_words{1U, 3U, 5U, 7U};
  constexpr std::array<float, 4U> ft4_screen_x{8.25F, 24.5F, 8.75F, 24.875F};
  constexpr std::array<float, 4U> ft4_screen_y{8.5F, 8.75F, 24.5F, 24.75F};
  for (std::size_t vertex{}; vertex < ft4_words.size(); ++vertex) {
    const auto word = ft4_words[vertex];
    ft4_projections[word] = make_projection(
        pgxp_ft4[word], ft4_screen_x[vertex], ft4_screen_y[vertex]);
  }
  const auto ft4_cache_base = PGXP_GetIndex(0);
  pgxp_gpu.submit(pgxp_ft4, ft4_projections);
  bool ft4_order_valid = pgxp_gpu.precisePrimitives() == 2U &&
                         PGXP_GetIndex(0) == ft4_cache_base + 4U;
  for (std::size_t vertex{}; vertex < ft4_words.size(); ++vertex) {
    PGXPVData cached{};
    ft4_order_valid =
        ft4_order_valid &&
        PGXP_GetCacheDataExact(
            &cached, static_cast<u_short>(ft4_cache_base + vertex)) != 0 &&
        cached.sx == ft4_screen_x[vertex];
  }
  if (!ft4_order_valid) {
    std::cerr << "Raw PGXP FT4 cache order no longer matches quad lookup\n";
    PsyX_Shutdown();
    return 14;
  }

  constexpr std::array<std::uint32_t, 12U> pgxp_gt4{
      0x3cffffffU, 0x00280008U, 0x00000000U, 0x00ffffffU,
      0x00280018U, 0x00000010U, 0x00ffffffU, 0x00380008U,
      0x00001000U, 0x00ffffffU, 0x00380018U, 0x00001010U,
  };
  std::array<sf::psx::GteProjectedVertex, pgxp_gt4.size()> gt4_projections{};
  constexpr std::array<std::size_t, 4U> gt4_words{1U, 4U, 7U, 10U};
  constexpr std::array<float, 4U> gt4_screen_x{8.375F, 24.625F, 8.875F,
                                               24.9375F};
  constexpr std::array<float, 4U> gt4_screen_y{40.5F, 40.75F, 56.5F, 56.75F};
  for (std::size_t vertex{}; vertex < gt4_words.size(); ++vertex) {
    const auto word = gt4_words[vertex];
    gt4_projections[word] = make_projection(
        pgxp_gt4[word], gt4_screen_x[vertex], gt4_screen_y[vertex]);
  }
  const auto gt4_cache_base = PGXP_GetIndex(0);
  pgxp_gpu.submit(pgxp_gt4, gt4_projections);
  bool gt4_order_valid = pgxp_gpu.precisePrimitives() == 3U &&
                         PGXP_GetIndex(0) == gt4_cache_base + 4U;
  for (std::size_t vertex{}; vertex < gt4_words.size(); ++vertex) {
    PGXPVData cached{};
    gt4_order_valid =
        gt4_order_valid &&
        PGXP_GetCacheDataExact(
            &cached, static_cast<u_short>(gt4_cache_base + vertex)) != 0 &&
        cached.sx == gt4_screen_x[vertex];
  }
  if (!gt4_order_valid) {
    std::cerr << "Raw PGXP GT4 cache order no longer matches quad lookup\n";
    PsyX_Shutdown();
    return 15;
  }

  const auto precise_before_bad_sidecars = pgxp_gpu.precisePrimitives();
  const auto cache_before_bad_sidecars = PGXP_GetIndex(0);
  const auto reprojection_before_bad_sidecars =
      pgxp_gpu.reprojectionMismatchPrimitives();
  auto mismatched_packed_vertex = gt4_projections;
  mismatched_packed_vertex[gt4_words[2]].screen_x += 128.0F;
  pgxp_gpu.submit(pgxp_gt4, mismatched_packed_vertex);
  if (pgxp_gpu.precisePrimitives() != precise_before_bad_sidecars ||
      pgxp_gpu.reprojectionMismatchPrimitives() !=
          reprojection_before_bad_sidecars + 1U ||
      PGXP_GetIndex(0) != cache_before_bad_sidecars) {
    std::cerr << "Screen mismatch escaped primitive-atomic raw fallback\n";
    PsyX_Shutdown();
    return 41;
  }

  const auto precise_before_bad_camera = pgxp_gpu.precisePrimitives();
  const auto cache_before_bad_camera = PGXP_GetIndex(0);
  const auto camera_before_bad_camera = pgxp_gpu.cameraMismatchPrimitives();
  auto mismatched_camera_vertex = gt4_projections;
  auto &camera_vertex = mismatched_camera_vertex[gt4_words[2]];
  camera_vertex.screen_h = 640.0F;
  camera_vertex.view_x =
      (camera_vertex.screen_x - camera_vertex.screen_offset_x) *
      camera_vertex.view_z / camera_vertex.screen_h;
  camera_vertex.view_y =
      (camera_vertex.screen_y - camera_vertex.screen_offset_y) *
      camera_vertex.view_z / camera_vertex.screen_h;
  pgxp_gpu.submit(pgxp_gt4, mismatched_camera_vertex);
  if (pgxp_gpu.precisePrimitives() != precise_before_bad_camera ||
      pgxp_gpu.cameraMismatchPrimitives() != camera_before_bad_camera + 1U ||
      PGXP_GetIndex(0) != cache_before_bad_camera) {
    std::cerr << "Camera mismatch escaped primitive-atomic raw fallback\n";
    PsyX_Shutdown();
    return 42;
  }

  // Hardware-SZ projective depth is captured independently for every
  // address/raw-validated coordinate. Re-publishing an otherwise identical
  // transform or mixing exact and legacy screen XY must not demote the whole
  // textured primitive when H/OFX/OFY remain coherent.
  auto exact_context = gt4_projections;
  for (const auto word : gt4_words) {
    exact_context[word].exact_transform = true;
    exact_context[word].transform_lineage = 0x1111U;
    exact_context[word].projection_epoch = 0x2222U;
  }
  const auto precise_before_stale_context = pgxp_gpu.precisePrimitives();
  const auto camera_before_stale_context = pgxp_gpu.cameraMismatchPrimitives();
  const auto cache_before_stale_context = PGXP_GetIndex(0);
  auto stale_context = exact_context;
  stale_context[gt4_words[2U]].transform_lineage = 0x3333U;
  pgxp_gpu.submit(pgxp_gt4, stale_context);
  std::array<PGXPVData, 4U> stale_context_cache{};
  auto stale_context_perspective = true;
  for (std::size_t vertex{}; vertex < stale_context_cache.size(); ++vertex) {
    stale_context_perspective =
        stale_context_perspective &&
        PGXP_GetCacheDataExact(
            &stale_context_cache[vertex],
            static_cast<u_short>(cache_before_stale_context + vertex)) != 0 &&
        stale_context_cache[vertex].scr_h > 0.0F &&
        stale_context_cache[vertex].pz > 0.0F &&
        stale_context_cache[vertex].precise_screen_position != 0U;
  }
  if (pgxp_gpu.precisePrimitives() != precise_before_stale_context + 1U ||
      pgxp_gpu.cameraMismatchPrimitives() != camera_before_stale_context ||
      PGXP_GetIndex(0) != cache_before_stale_context + 4U ||
      !stale_context_perspective) {
    std::cerr << "Stale transform lineage escaped primitive-atomic fallback\n";
    PsyX_Shutdown();
    return 94;
  }

  const auto precise_before_mixed_context = pgxp_gpu.precisePrimitives();
  const auto camera_before_mixed_context = pgxp_gpu.cameraMismatchPrimitives();
  const auto cache_before_mixed_context = PGXP_GetIndex(0);
  auto mixed_context = exact_context;
  mixed_context[gt4_words[2U]].exact_transform = false;
  pgxp_gpu.submit(pgxp_gt4, mixed_context);
  std::array<PGXPVData, 4U> mixed_context_cache{};
  auto mixed_context_perspective = true;
  for (std::size_t vertex{}; vertex < mixed_context_cache.size(); ++vertex) {
    mixed_context_perspective =
        mixed_context_perspective &&
        PGXP_GetCacheDataExact(
            &mixed_context_cache[vertex],
            static_cast<u_short>(cache_before_mixed_context + vertex)) != 0 &&
        mixed_context_cache[vertex].scr_h > 0.0F &&
        mixed_context_cache[vertex].pz > 0.0F &&
        mixed_context_cache[vertex].precise_screen_position != 0U;
  }
  if (pgxp_gpu.precisePrimitives() != precise_before_mixed_context + 1U ||
      pgxp_gpu.cameraMismatchPrimitives() != camera_before_mixed_context ||
      PGXP_GetIndex(0) != cache_before_mixed_context + 4U ||
      !mixed_context_perspective) {
    std::cerr << "Mixed exact/legacy tuple escaped primitive-atomic fallback\n";
    PsyX_Shutdown();
    return 95;
  }
  DrawSync(0);

  // Adjacent raw quads are one coherence unit: a precise/legacy split on their
  // shared edge would visibly crack. Exercise both the all-precise case and
  // two fail-closed hazards with both packets in the same submit.
  constexpr std::array<std::uint32_t, 10U> adjacent_f4_words{
      0x28ffffffU, 0x00080008U, 0x00080018U, 0x00180008U, 0x00180018U,
      0x28ffffffU, 0x00080018U, 0x00080028U, 0x00180018U, 0x00180028U,
  };
  const auto make_adjacent_projections = [&] {
    std::array<sf::psx::GteProjectedVertex, 10U> projections{};
    constexpr std::array<std::size_t, 8U> coordinate_words{1U, 2U, 3U, 4U,
                                                           6U, 7U, 8U, 9U};
    constexpr std::array<float, 8U> screen_x{8.25F,  24.25F, 8.25F,  24.25F,
                                             24.25F, 40.25F, 24.25F, 40.25F};
    constexpr std::array<float, 8U> screen_y{8.25F, 8.25F, 24.25F, 24.25F,
                                             8.25F, 8.25F, 24.25F, 24.25F};
    for (std::size_t vertex{}; vertex < coordinate_words.size(); ++vertex) {
      const auto word = coordinate_words[vertex];
      projections[word] = make_projection(adjacent_f4_words[word],
                                          screen_x[vertex], screen_y[vertex]);
    }
    return projections;
  };

  // The compact catalog fallback resolves shared packed vertices to one exact
  // tuple; conflicting tuples make every touching primitive fall back.
  constexpr std::array<std::size_t, 8U> catalog_coordinate_words{
      1U, 2U, 3U, 4U, 6U, 7U, 8U, 9U};
  const auto catalog_projections = make_adjacent_projections();
  std::array<sf::psx::GteProjectedVertex, 8U> packed_catalog{};
  for (std::size_t index{}; index < packed_catalog.size(); ++index) {
    packed_catalog[index] =
        catalog_projections[catalog_coordinate_words[index]];
  }

  // The retail path transports a compact catalog handle with coordinate
  // words. Shared source vertices reuse one exact record, so no packed-SXY
  // search, canonical hash, or edge snapping is needed.
  constexpr std::array<std::size_t, 6U> handle_catalog_words{1U, 2U, 3U,
                                                             4U, 7U, 9U};
  std::array<sf::psx::GteProjectedVertex, 6U> handle_catalog{};
  for (std::size_t index{}; index < handle_catalog.size(); ++index) {
    handle_catalog[index] = catalog_projections[handle_catalog_words[index]];
    handle_catalog[index].source_vertex_id = index + 1U;
    handle_catalog[index].mesh_vertex_id = 0x1000U + index;
  }
  handle_catalog[1U].mesh_vertex_id = 0x2001U;
  handle_catalog[3U].mesh_vertex_id = 0x2002U;
  std::array<std::uint64_t, adjacent_f4_words.size()> handle_words{};
  handle_words[1U] = 1U;
  handle_words[2U] = 2U;
  handle_words[3U] = 3U;
  handle_words[4U] = 4U;
  handle_words[6U] = 2U;
  handle_words[7U] = 5U;
  handle_words[8U] = 4U;
  handle_words[9U] = 6U;

  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu handle_catalog_gpu;
  handle_catalog_gpu.submit(adjacent_f4_words, {}, handle_words, 7000U,
                            handle_catalog);
  if (handle_catalog_gpu.precisePrimitives() != 2U ||
      handle_catalog_gpu.projectionCatalogBuilds() != 0U ||
      handle_catalog_gpu.projectionCatalogPrimitives() != 2U ||
      handle_catalog_gpu.projectionCatalogAmbiguities() != 0U ||
      handle_catalog_gpu.coherenceEdgeBuilds() != 0U ||
      handle_catalog_gpu.coherenceSnappedPrimitives() != 0U ||
      PGXP_GetIndex(0) != 8U) {
    std::cerr << "Compact projection handles did not preserve shared exact "
                 "vertices without packed-SXY recovery\n";
    PsyX_Shutdown();
    return 80;
  }

  // Identity words can become active one submit before either the direct
  // projection sidecar or compact catalog. Treat that frame as legacy input;
  // recovery has no witness to inspect and must not index an empty vector.
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu identity_only_gpu;
  identity_only_gpu.submit(adjacent_f4_words, {}, handle_words, 70000U, {});
  if (identity_only_gpu.polygonPrimitives() != 2U ||
      identity_only_gpu.precisePrimitives() != 0U ||
      identity_only_gpu.identityCanonicalBuilds() != 0U ||
      PGXP_GetIndex(0) != 0U) {
    std::cerr << "Identity-only transport did not fail closed without a "
                 "projection witness\n";
    PsyX_Shutdown();
    return 103;
  }

  // A sparse native-world sidecar and compact GTE handles share one submit.
  // Direct provenance wins for its vertex; every empty direct slot must fall
  // through to the validated catalog handle without packed-SXY recovery.
  auto hybrid_direct_projections = make_adjacent_projections();
  for (auto &projection : hybrid_direct_projections)
    projection = {};
  auto hybrid_override = catalog_projections[1U];
  hybrid_override.exact_transform = true;
  hybrid_override.transform_lineage = 0x7000U;
  hybrid_override.projection_epoch = 0x7001U;
  hybrid_override.source_vertex_id = 0xabcdefU;
  hybrid_override.mesh_vertex_id = 0x123456U;
  hybrid_direct_projections[1U] = hybrid_override;
  auto hybrid_catalog = handle_catalog;
  for (auto &projection : hybrid_catalog) {
    projection.exact_transform = true;
    projection.transform_lineage = 0x7000U;
    projection.projection_epoch = 0x7001U;
  }
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu hybrid_transport_gpu;
  hybrid_transport_gpu.submit(adjacent_f4_words, hybrid_direct_projections,
                              handle_words, 70001U, hybrid_catalog);
  PGXPVData hybrid_direct{};
  PGXPVData hybrid_catalog_vertex{};
  if (hybrid_transport_gpu.precisePrimitives() != 2U ||
      hybrid_transport_gpu.missingProjectionPrimitives() != 0U ||
      hybrid_transport_gpu.projectionCatalogBuilds() != 0U ||
      PGXP_GetCacheDataExact(&hybrid_direct, 0U) == 0 ||
      PGXP_GetCacheDataExact(&hybrid_catalog_vertex, 1U) == 0 ||
      hybrid_direct.sx != hybrid_override.screen_x ||
      hybrid_catalog_vertex.sx != hybrid_catalog[1U].screen_x ||
      PGXP_GetIndex(0) != 8U) {
    std::cerr << "Sparse native/compact hybrid transport lost a vertex\n";
    PsyX_Shutdown();
    return 181;
  }

  // The same source mesh vertex can be projected and published more than
  // once, producing different compact catalog handles. Its stable GTE mesh
  // identity must still select one canonical tuple for every polygon.
  std::array<sf::psx::GteProjectedVertex, 8U> repeated_vertex_catalog{};
  std::array<std::uint64_t, adjacent_f4_words.size()> repeated_vertex_handles{};
  constexpr std::array<std::uint64_t, 8U> repeated_mesh_identities{
      0x3001U, 0x3002U, 0x3003U, 0x3004U, 0x3002U, 0x3005U, 0x3004U, 0x3006U};
  for (std::size_t index{}; index < repeated_vertex_catalog.size(); ++index) {
    repeated_vertex_catalog[index] =
        catalog_projections[catalog_coordinate_words[index]];
    repeated_vertex_catalog[index].source_vertex_id = index + 1U;
    repeated_vertex_catalog[index].mesh_vertex_id =
        repeated_mesh_identities[index];
    repeated_vertex_handles[catalog_coordinate_words[index]] = index + 1U;
  }
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu repeated_vertex_gpu;
  repeated_vertex_gpu.setRuntimeGeometryPolicy(false, true, false);
  repeated_vertex_gpu.submit(adjacent_f4_words, {}, repeated_vertex_handles,
                             7001U, repeated_vertex_catalog);
  PGXPVData repeated_top_left{};
  PGXPVData repeated_top_right{};
  PGXPVData repeated_bottom_left{};
  PGXPVData repeated_bottom_right{};
  const auto repeated_shared_vertices =
      PGXP_GetCacheDataExact(&repeated_top_left, 1U) != 0 &&
      PGXP_GetCacheDataExact(&repeated_top_right, 4U) != 0 &&
      PGXP_GetCacheDataExact(&repeated_bottom_left, 3U) != 0 &&
      PGXP_GetCacheDataExact(&repeated_bottom_right, 6U) != 0 &&
      repeated_top_left.sx == repeated_top_right.sx &&
      repeated_top_left.sy == repeated_top_right.sy &&
      repeated_top_left.pz == repeated_top_right.pz &&
      repeated_bottom_left.sx == repeated_bottom_right.sx &&
      repeated_bottom_left.sy == repeated_bottom_right.sy &&
      repeated_bottom_left.pz == repeated_bottom_right.pz;
  if (repeated_vertex_gpu.precisePrimitives() != 2U ||
      repeated_vertex_gpu.sharedMeshBuilds() != 1U ||
      repeated_vertex_gpu.sharedMeshConflicts() != 0U ||
      repeated_vertex_gpu.sharedMeshFractionalVertices() != 8U ||
      repeated_vertex_gpu.sharedMeshSnappedVertices() != 0U ||
      !repeated_shared_vertices || PGXP_GetIndex(0) != 8U) {
    std::cerr << "Repeated GTE mesh vertex did not reuse one canonical "
                 "projection across catalog handles\n";
    PsyX_Shutdown();
    return 87;
  }

  auto ambiguous_mesh_catalog = repeated_vertex_catalog;
  ambiguous_mesh_catalog[4U].screen_x += 0.5F;
  ambiguous_mesh_catalog[4U].view_x =
      (ambiguous_mesh_catalog[4U].screen_x -
       ambiguous_mesh_catalog[4U].screen_offset_x) *
      ambiguous_mesh_catalog[4U].view_z / ambiguous_mesh_catalog[4U].screen_h;
  ambiguous_mesh_catalog[6U].screen_y += 0.5F;
  ambiguous_mesh_catalog[6U].view_y =
      (ambiguous_mesh_catalog[6U].screen_y -
       ambiguous_mesh_catalog[6U].screen_offset_y) *
      ambiguous_mesh_catalog[6U].view_z / ambiguous_mesh_catalog[6U].screen_h;
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu ambiguous_mesh_gpu;
  ambiguous_mesh_gpu.submit(adjacent_f4_words, {}, repeated_vertex_handles,
                            7002U, ambiguous_mesh_catalog);
  std::array<PGXPVData, 8U> ambiguous_mesh_cache{};
  auto ambiguous_mesh_cache_valid = true;
  for (std::size_t vertex{}; vertex < ambiguous_mesh_cache.size(); ++vertex) {
    ambiguous_mesh_cache_valid =
        ambiguous_mesh_cache_valid &&
        PGXP_GetCacheDataExact(&ambiguous_mesh_cache[vertex],
                               static_cast<u_short>(vertex)) != 0;
  }
  const auto ambiguous_local_projections_preserved =
      ambiguous_mesh_cache[1U].sx == 24.25F &&
      ambiguous_mesh_cache[4U].sx == 24.75F &&
      ambiguous_mesh_cache[3U].sy == 24.25F &&
      ambiguous_mesh_cache[6U].sy == 24.75F;
  if (!ambiguous_mesh_cache_valid || !ambiguous_local_projections_preserved ||
      ambiguous_mesh_gpu.precisePrimitives() != 2U ||
      ambiguous_mesh_gpu.sharedMeshBuilds() != 1U ||
      ambiguous_mesh_gpu.sharedMeshConflicts() != 2U ||
      ambiguous_mesh_gpu.sharedMeshFractionalVertices() != 8U ||
      ambiguous_mesh_gpu.sharedMeshSnappedVertices() != 0U ||
      ambiguous_mesh_gpu.sharedMeshPacketFallbackVertices() != 4U ||
      PGXP_GetIndex(0) != 8U) {
    std::cerr << "Ambiguous mesh identity quantized local projections\n";
    PsyX_Shutdown();
    return 187;
  }

  // Production keeps exact per-packet PGXP projections authoritative. Source
  // mesh identities are transport diagnostics, not a frame-wide temporal
  // cache: canonicalizing them costs a hash probe per vertex and can pin an
  // animated detail to an earlier projection.
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu production_geometry_gpu;
  production_geometry_gpu.setRuntimeGeometryPolicy(true, false, false);
  production_geometry_gpu.submit(adjacent_f4_words, {}, repeated_vertex_handles,
                                 7003U, ambiguous_mesh_catalog);
  std::array<PGXPVData, 8U> production_geometry_cache{};
  auto production_geometry_cache_valid = true;
  for (std::size_t vertex{}; vertex < production_geometry_cache.size();
       ++vertex) {
    production_geometry_cache_valid =
        production_geometry_cache_valid &&
        PGXP_GetCacheDataExact(&production_geometry_cache[vertex],
                               static_cast<u_short>(vertex)) != 0;
  }
  const auto production_local_projections_preserved =
      production_geometry_cache[1U].sx == 24.25F &&
      production_geometry_cache[4U].sx == 24.75F &&
      production_geometry_cache[3U].sy == 24.25F &&
      production_geometry_cache[6U].sy == 24.75F;
  if (!production_geometry_cache_valid ||
      !production_local_projections_preserved ||
      production_geometry_gpu.precisePrimitives() != 2U ||
      production_geometry_gpu.sharedMeshBuilds() != 0U ||
      production_geometry_gpu.sharedMeshConflicts() != 0U ||
      production_geometry_gpu.sharedMeshFractionalVertices() != 0U ||
      production_geometry_gpu.sharedMeshPacketFallbackVertices() != 0U ||
      PGXP_GetIndex(0) != 8U) {
    std::cerr << "Production geometry policy enabled shared-mesh "
                 "canonicalization\n";
    PsyX_Shutdown();
    return 188;
  }

  // Retail software clipping can publish a quad after one generated corner
  // has lost its compact GTE handle. Three camera-space vertices still define
  // the polygon plane, so recover the fourth ray/plane intersection and its W
  // instead of dropping the entire textured primitive to affine rendering.
  const auto clipped_quad_words =
      std::span<const std::uint32_t>{adjacent_f4_words}.first(5U);
  std::array<sf::psx::GteProjectedVertex, 3U> clipped_quad_catalog{
      catalog_projections[1U], catalog_projections[2U],
      catalog_projections[3U]};
  for (std::size_t index{}; index < clipped_quad_catalog.size(); ++index) {
    clipped_quad_catalog[index].source_vertex_id = index + 1U;
    clipped_quad_catalog[index].mesh_vertex_id = 0x4001U + index;
  }
  std::array<std::uint64_t, 5U> clipped_quad_handles{};
  clipped_quad_handles[1U] = 1U;
  clipped_quad_handles[2U] = 2U;
  clipped_quad_handles[3U] = 3U;
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu clipped_quad_gpu;
  clipped_quad_gpu.setCoherenceEdgeSnapping(true);
  clipped_quad_gpu.submit(clipped_quad_words, {}, clipped_quad_handles, 7002U,
                          clipped_quad_catalog);
  if (clipped_quad_gpu.precisePrimitives() != 0U ||
      clipped_quad_gpu.projectionCatalogPrimitives() != 0U ||
      clipped_quad_gpu.missingProjectionPrimitives() != 1U ||
      PGXP_GetIndex(0) != 0U) {
    std::cerr << "Incomplete clipped quad did not fall back atomically\n";
    PsyX_Shutdown();
    return 88;
  }

  // Production accepts only transported handles. A missing handle demotes W
  // for the entire affected primitive, but the other address/raw-validated
  // screen positions stay precise. No packed-SXY/plane scan is allowed.
  auto clipped_handle_words = handle_words;
  clipped_handle_words[6U] = 0U;
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu strict_handle_gpu;
  strict_handle_gpu.submit(adjacent_f4_words, {}, clipped_handle_words, 7003U,
                           handle_catalog);
  if (strict_handle_gpu.precisePrimitives() != 1U ||
      strict_handle_gpu.projectionCatalogBuilds() != 0U ||
      strict_handle_gpu.projectionCatalogPrimitives() != 1U ||
      strict_handle_gpu.missingProjectionPrimitives() != 1U ||
      strict_handle_gpu.identityRecoveredVertices() != 0U ||
      strict_handle_gpu.identityRecoveredPrimitives() != 0U ||
      strict_handle_gpu.planeRecoveredPrimitives() != 0U ||
      strict_handle_gpu.coherenceEdgeBuilds() != 0U || PGXP_GetIndex(0) != 4U) {
    std::cerr << "Missing compact handle did not fail closed without "
                 "packed-SXY recovery\n";
    PsyX_Shutdown();
    return 91;
  }

  // Keep the retired heuristic available only as an explicit diagnostic.
  // It is never enabled by the MOHU runtime because packed SXY is not vertex
  // identity and cannot safely provide another vertex's camera-space W.
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu clipped_handle_gpu;
  clipped_handle_gpu.setGeometryOptions(true, true, true, true, true, true,
                                        true);
  clipped_handle_gpu.submit(adjacent_f4_words, {}, clipped_handle_words, 7004U,
                            handle_catalog);
  if (clipped_handle_gpu.precisePrimitives() != 2U ||
      clipped_handle_gpu.projectionCatalogBuilds() != 1U ||
      clipped_handle_gpu.projectionCatalogPrimitives() != 2U ||
      clipped_handle_gpu.projectionCatalogAmbiguities() != 0U ||
      clipped_handle_gpu.identityRecoveredVertices() != 1U ||
      clipped_handle_gpu.identityRecoveredPrimitives() != 1U ||
      clipped_handle_gpu.coherenceEdgeBuilds() != 0U ||
      clipped_handle_gpu.coherenceSnappedPrimitives() != 0U ||
      PGXP_GetIndex(0) != 8U) {
    std::cerr << "Frame-local world mesh did not recover a clipped shared "
                 "vertex from its unique PsyCross catalog witness\n";
    PsyX_Shutdown();
    return 85;
  }

  // Default PGXP keeps the coherent exact view tuple. Hardware reciprocal
  // depth remains a legacy fallback and must not flatten close exact vertices
  // to H/2 or quantize fractional motion back to the GP0 packet.
  auto trusted_screen_catalog = handle_catalog;
  for (auto &projection : trusted_screen_catalog) {
    projection.exact_transform = true;
    projection.transform_lineage = 0x7010U;
    projection.projection_epoch = 0x7011U;
  }
  auto &trusted_screen = trusted_screen_catalog[0U];
  const auto trusted_packed_x = static_cast<float>(
      static_cast<std::int16_t>(trusted_screen.packed_sxy & 0xffffU));
  trusted_screen.screen_x = trusted_packed_x + 2.25F;
  trusted_screen.view_x =
      (trusted_screen.screen_x - trusted_screen.screen_offset_x) *
      trusted_screen.view_z / trusted_screen.screen_h;
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu trusted_screen_gpu;
  trusted_screen_gpu.submit(adjacent_f4_words, {}, handle_words, 7010U,
                            trusted_screen_catalog);
  PGXPVData trusted_screen_vertex{};
  if (trusted_screen_gpu.precisePrimitives() != 2U ||
      trusted_screen_gpu.reprojectionMismatchPrimitives() != 0U ||
      PGXP_GetCacheDataExact(&trusted_screen_vertex, 0U) == 0 ||
      trusted_screen_vertex.sx != trusted_screen.screen_x ||
      trusted_screen_vertex.pz != trusted_screen.view_z / 128.0F ||
      PGXP_GetIndex(0) != 8U) {
    std::cerr << "Exact screen XY lost signed view depth\n";
    PsyX_Shutdown();
    return 81;
  }

  auto saturated_screen_catalog = trusted_screen_catalog;
  auto &saturated_screen = saturated_screen_catalog[0U];
  saturated_screen.screen_saturated = true;
  saturated_screen.screen_x = 2048.25F;
  saturated_screen.view_x =
      (saturated_screen.screen_x - saturated_screen.screen_offset_x) *
      saturated_screen.view_z / saturated_screen.screen_h;
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu saturated_screen_gpu;
  saturated_screen_gpu.submit(adjacent_f4_words, {}, handle_words, 7011U,
                              saturated_screen_catalog);
  PGXPVData saturated_screen_vertex{};
  if (saturated_screen_gpu.precisePrimitives() != 2U ||
      saturated_screen_gpu.screenSaturationPrimitives() != 0U ||
      saturated_screen_gpu.reprojectionMismatchPrimitives() != 0U ||
      PGXP_GetCacheDataExact(&saturated_screen_vertex, 0U) == 0 ||
      saturated_screen_vertex.exact_projection == 0U ||
      saturated_screen_vertex.sx != saturated_screen.screen_x ||
      saturated_screen_vertex.pz != saturated_screen.view_z / 128.0F ||
      PGXP_GetIndex(0) != 8U) {
    std::cerr << "Coherent exact projection was rejected at the PS1 screen "
                 "clamp\n";
    PsyX_Shutdown();
    return 189;
  }

  auto untoleranced_screen_catalog = trusted_screen_catalog;
  for (auto &projection : untoleranced_screen_catalog)
    projection.exact_transform = false;
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu untoleranced_screen_gpu;
  untoleranced_screen_gpu.submit(adjacent_f4_words, {}, handle_words, 7011U,
                                 untoleranced_screen_catalog);
  PGXPVData untoleranced_vertex{};
  if (untoleranced_screen_gpu.precisePrimitives() != 2U ||
      untoleranced_screen_gpu.screenMismatchPrimitives() != 0U ||
      PGXP_GetCacheDataExact(&untoleranced_vertex, 0U) == 0 ||
      untoleranced_vertex.precise_screen_position == 0U ||
      untoleranced_vertex.scr_h == 0.0F || PGXP_GetIndex(0) != 8U) {
    std::cerr << "Disabled geometry tolerance rejected valid provenance\n";
    PsyX_Shutdown();
    return 82;
  }

  auto near_plane_catalog = handle_catalog;
  for (auto &projection : near_plane_catalog) {
    projection.exact_transform = true;
    projection.transform_lineage = 0x7012U;
    projection.projection_epoch = 0x7013U;
  }
  auto &near_plane = near_plane_catalog[0U];
  near_plane.view_z = 8.0F;
  near_plane.projective_depth = near_plane.screen_h * 0.5F;
  near_plane.view_x = (near_plane.screen_x - near_plane.screen_offset_x) *
                      near_plane.view_z / near_plane.screen_h;
  near_plane.view_y = (near_plane.screen_y - near_plane.screen_offset_y) *
                      near_plane.view_z / near_plane.screen_h;
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu near_plane_gpu;
  near_plane_gpu.submit(adjacent_f4_words, {}, handle_words, 7012U,
                        near_plane_catalog);
  PGXPVData near_plane_cache{};
  if (near_plane_gpu.precisePrimitives() != 2U ||
      near_plane_gpu.nearPlanePrimitives() != 0U ||
      near_plane_gpu.reprojectionMismatchPrimitives() != 0U ||
      PGXP_GetCacheDataExact(&near_plane_cache, 0U) == 0 ||
      near_plane_cache.exact_projection == 0U ||
      near_plane_cache.pz != near_plane.view_z / 128.0F ||
      near_plane_cache.sx != near_plane.screen_x ||
      near_plane_cache.sy != near_plane.screen_y || PGXP_GetIndex(0) != 8U) {
    std::cerr << "Coherent close projection fell back before GPU clipping\n";
    PsyX_Shutdown();
    return 83;
  }

  auto behind_plane_catalog = handle_catalog;
  for (auto &projection : behind_plane_catalog) {
    projection.exact_transform = true;
    projection.transform_lineage = 0x7014U;
    projection.projection_epoch = 0x7015U;
  }
  auto &behind_plane = behind_plane_catalog[0U];
  behind_plane.view_z = -64.0F;
  behind_plane.projective_depth = behind_plane.screen_h * 0.5F;
  behind_plane.view_x = (behind_plane.screen_x - behind_plane.screen_offset_x) *
                        behind_plane.view_z / behind_plane.screen_h;
  behind_plane.view_y = (behind_plane.screen_y - behind_plane.screen_offset_y) *
                        behind_plane.view_z / behind_plane.screen_h;
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu behind_plane_gpu;
  behind_plane_gpu.submit(adjacent_f4_words, {}, handle_words, 7014U,
                          behind_plane_catalog);
  PGXPVData behind_plane_cache{};
  if (behind_plane_gpu.precisePrimitives() != 2U ||
      behind_plane_gpu.nearPlanePrimitives() != 0U ||
      behind_plane_gpu.screenMismatchPrimitives() != 0U ||
      PGXP_GetCacheDataExact(&behind_plane_cache, 0U) == 0 ||
      behind_plane_cache.exact_projection == 0U ||
      behind_plane_cache.pz != behind_plane.view_z / 128.0F ||
      behind_plane_cache.sx != behind_plane.screen_x ||
      behind_plane_cache.sy != behind_plane.screen_y ||
      PGXP_GetIndex(0) != 8U) {
    std::cerr << "Behind-plane exact tuple lost signed depth or screen XY\n";
    PsyX_Shutdown();
    return 88;
  }

  // Perspective correction uses the transported camera-space Z, not packed
  // SXY. Shared handles must reuse one W while distinct vertices retain their
  // unequal W values across both adjacent primitives.
  auto perspective_catalog = handle_catalog;
  const auto set_handle_depth = [](auto &projection, float depth) {
    const auto scale = depth / projection.view_z;
    projection.view_x *= scale;
    projection.view_y *= scale;
    projection.view_z = depth;
    projection.projective_depth = depth;
  };
  set_handle_depth(perspective_catalog[1U], 500.0F);
  set_handle_depth(perspective_catalog[3U], 1500.0F);
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu perspective_catalog_gpu;
  perspective_catalog_gpu.submit(adjacent_f4_words, {}, handle_words, 7013U,
                                 perspective_catalog);
  PGXPVData shared_top_left{};
  PGXPVData shared_top_right{};
  PGXPVData shared_bottom_left{};
  PGXPVData shared_bottom_right{};
  const auto perspective_w_preserved =
      PGXP_GetCacheDataExact(&shared_top_left, 1U) != 0 &&
      PGXP_GetCacheDataExact(&shared_top_right, 4U) != 0 &&
      PGXP_GetCacheDataExact(&shared_bottom_left, 3U) != 0 &&
      PGXP_GetCacheDataExact(&shared_bottom_right, 6U) != 0 &&
      shared_top_left.pz == shared_top_right.pz &&
      shared_bottom_left.pz == shared_bottom_right.pz &&
      shared_top_left.pz != shared_bottom_left.pz &&
      shared_top_left.sx == 24.25F && shared_top_left.sy == 8.25F &&
      shared_top_right.sx == shared_top_left.sx &&
      shared_top_right.sy == shared_top_left.sy &&
      shared_bottom_left.sx == 24.25F && shared_bottom_left.sy == 24.25F &&
      shared_bottom_right.sx == shared_bottom_left.sx &&
      shared_bottom_right.sy == shared_bottom_left.sy;
  if (perspective_catalog_gpu.precisePrimitives() != 2U ||
      perspective_catalog_gpu.projectionCatalogPrimitives() != 2U ||
      perspective_catalog_gpu.sharedMeshBuilds() != 1U ||
      perspective_catalog_gpu.sharedMeshConflicts() != 0U ||
      perspective_catalog_gpu.sharedMeshFractionalVertices() != 8U ||
      perspective_catalog_gpu.sharedMeshSnappedVertices() != 0U ||
      !perspective_w_preserved || PGXP_GetIndex(0) != 8U) {
    std::cerr << "Shared mesh lost canonical fractional vertices or W\n";
    PsyX_Shutdown();
    return 84;
  }

  // Dense Memory-PGXP sidecars are word-local. Independently projected
  // vertices may quantize to the same SXY while retaining different subpixel
  // positions; a frame-global packed-SXY policy must not erase either one.
  auto conflicting_mesh = make_adjacent_projections();
  for (const auto word : {6U, 8U}) {
    conflicting_mesh[word].screen_x += 0.5F;
    conflicting_mesh[word].view_x = (conflicting_mesh[word].screen_x -
                                     conflicting_mesh[word].screen_offset_x) *
                                    conflicting_mesh[word].view_z /
                                    conflicting_mesh[word].screen_h;
  }
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu conflicting_mesh_gpu;
  conflicting_mesh_gpu.submit(adjacent_f4_words, conflicting_mesh);
  std::array<PGXPVData, 8U> conflicting_cache{};
  auto conflicting_cache_valid = true;
  for (std::size_t index{}; index < conflicting_cache.size(); ++index) {
    conflicting_cache_valid =
        conflicting_cache_valid &&
        PGXP_GetCacheDataExact(&conflicting_cache[index],
                               static_cast<u_short>(index)) != 0;
  }
  const auto independent_subpixels_preserved =
      conflicting_cache[1U].sx == 24.25F && conflicting_cache[1U].sy == 8.25F &&
      conflicting_cache[4U].sx == 24.75F && conflicting_cache[4U].sy == 8.25F &&
      conflicting_cache[3U].sx == 24.25F &&
      conflicting_cache[3U].sy == 24.25F &&
      conflicting_cache[6U].sx == 24.75F &&
      conflicting_cache[6U].sy == 24.25F && conflicting_cache[0U].sx == 8.25F &&
      conflicting_cache[5U].sx == 40.25F;
  if (!conflicting_cache_valid || !independent_subpixels_preserved ||
      conflicting_mesh_gpu.precisePrimitives() != 2U ||
      conflicting_mesh_gpu.sharedMeshBuilds() != 0U ||
      conflicting_mesh_gpu.sharedMeshConflicts() != 0U ||
      conflicting_mesh_gpu.sharedMeshFractionalVertices() != 0U ||
      conflicting_mesh_gpu.sharedMeshSnappedVertices() != 0U) {
    std::cerr << "Dense Memory-PGXP vertices were coupled by packed SXY\n";
    PsyX_Shutdown();
    return 86;
  }

  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu packed_catalog_gpu;
  packed_catalog_gpu.setCoherenceEdgeSnapping(true);
  packed_catalog_gpu.submit(adjacent_f4_words, {}, {}, 7001U, packed_catalog);
  PGXPVData packed_catalog_vertex{};
  if (packed_catalog_gpu.precisePrimitives() != 2U ||
      packed_catalog_gpu.projectionCatalogBuilds() != 1U ||
      packed_catalog_gpu.projectionCatalogPrimitives() != 2U ||
      packed_catalog_gpu.projectionCatalogAmbiguities() != 0U ||
      packed_catalog_gpu.sharedMeshBuilds() != 0U ||
      PGXP_GetCacheDataExact(&packed_catalog_vertex, 0U) == 0 ||
      packed_catalog_vertex.sx != 8.25F || packed_catalog_vertex.sy != 8.25F ||
      PGXP_GetIndex(0) != 8U) {
    std::cerr << "Frame-local catalog did not recover fractional packet "
                 "geometry\n";
    PsyX_Shutdown();
    return 75;
  }

  auto ambiguous_catalog = packed_catalog;
  auto &conflicting_shared_top = ambiguous_catalog[4U];
  conflicting_shared_top.screen_x += 0.5F;
  conflicting_shared_top.view_x = conflicting_shared_top.screen_x *
                                  conflicting_shared_top.view_z /
                                  conflicting_shared_top.screen_h;
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu ambiguous_catalog_gpu;
  ambiguous_catalog_gpu.setCoherenceEdgeSnapping(true);
  ambiguous_catalog_gpu.submit(adjacent_f4_words, {}, {}, 7002U,
                               ambiguous_catalog);
  if (ambiguous_catalog_gpu.precisePrimitives() != 0U ||
      ambiguous_catalog_gpu.projectionCatalogBuilds() != 1U ||
      ambiguous_catalog_gpu.projectionCatalogPrimitives() != 0U ||
      ambiguous_catalog_gpu.projectionCatalogAmbiguities() != 2U ||
      PGXP_GetIndex(0) != 0U) {
    std::cerr << "Conflicting packed projection was not rejected for every "
                 "touching primitive\n";
    PsyX_Shutdown();
    return 76;
  }

  // A software-clipped neighbour may have no catalog entry. The resolved
  // primitive must keep its unequal W values while sharing packed X/Y with
  // the fallback edge, otherwise the floor opens into a visible crack.
  auto boundary_catalog_source = packed_catalog;
  const auto set_catalog_depth = [&](std::size_t index, float depth) {
    auto &projection = boundary_catalog_source[index];
    const auto scale = depth / projection.view_z;
    projection.view_x *= scale;
    projection.view_y *= scale;
    projection.view_z = depth;
    projection.projective_depth = depth;
  };
  set_catalog_depth(1U, 500.0F);
  boundary_catalog_source[4U] = boundary_catalog_source[1U];
  set_catalog_depth(3U, 1500.0F);
  boundary_catalog_source[6U] = boundary_catalog_source[3U];
  const std::array<sf::psx::GteProjectedVertex, 7U> boundary_catalog{
      boundary_catalog_source[0U], boundary_catalog_source[1U],
      boundary_catalog_source[2U], boundary_catalog_source[3U],
      boundary_catalog_source[4U], boundary_catalog_source[6U],
      boundary_catalog_source[7U]};
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu boundary_catalog_gpu;
  boundary_catalog_gpu.setCoherenceEdgeSnapping(true);
  boundary_catalog_gpu.submit(adjacent_f4_words, {}, {}, 7003U,
                              boundary_catalog);
  PGXPVData boundary_top{};
  PGXPVData boundary_bottom{};
  const auto boundary_preserved =
      PGXP_GetCacheDataExact(&boundary_top, 1U) != 0 &&
      PGXP_GetCacheDataExact(&boundary_bottom, 3U) != 0 &&
      boundary_top.sx == 24.0F && boundary_top.sy == 8.0F &&
      boundary_bottom.sx == 24.0F && boundary_bottom.sy == 24.0F &&
      boundary_top.pz != 0.0F && boundary_bottom.pz != 0.0F &&
      boundary_top.pz != boundary_bottom.pz;
  if (boundary_catalog_gpu.precisePrimitives() != 1U ||
      boundary_catalog_gpu.projectionCatalogBuilds() != 1U ||
      boundary_catalog_gpu.projectionCatalogPrimitives() != 1U ||
      boundary_catalog_gpu.projectionCatalogAmbiguities() != 0U ||
      boundary_catalog_gpu.coherenceEdgeBuilds() != 1U ||
      boundary_catalog_gpu.coherenceSnappedPrimitives() != 1U ||
      boundary_catalog_gpu.coherenceSnappedVertices() != 2U ||
      !boundary_preserved || PGXP_GetIndex(0) != 4U) {
    std::cerr << "Catalog precise/fallback edge did not preserve W and close "
                 "the shared boundary\n";
    PsyX_Shutdown();
    return 77;
  }
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu adjacent_precise_gpu;
  adjacent_precise_gpu.setCoherenceEdgeSnapping(true);
  const auto adjacent_precise_projections = make_adjacent_projections();
  std::array<std::uint64_t, adjacent_f4_words.size()>
      adjacent_precise_identities{};
  constexpr std::array<std::size_t, 8U> precise_coordinate_words{
      1U, 2U, 3U, 4U, 6U, 7U, 8U, 9U};
  constexpr std::array<std::uint64_t, 8U> precise_vertex_identities{
      11U, 12U, 13U, 14U, 12U, 15U, 14U, 16U};
  for (std::size_t vertex{}; vertex < precise_coordinate_words.size();
       ++vertex) {
    adjacent_precise_identities[precise_coordinate_words[vertex]] =
        precise_vertex_identities[vertex];
  }
  GR_BeginGuestSubmit();
  adjacent_precise_gpu.submit(adjacent_f4_words, adjacent_precise_projections,
                              adjacent_precise_identities, 6901U);
  const auto precise_latch = PGXP_GetIndex(1);
  if (adjacent_precise_gpu.polygonPrimitives() != 2U ||
      adjacent_precise_gpu.preciseCandidates() != 2U ||
      adjacent_precise_gpu.partialProjectionPrimitives() != 0U ||
      adjacent_precise_gpu.coherenceFallbackPrimitives() != 0U ||
      adjacent_precise_gpu.precisePrimitives() != 2U ||
      adjacent_precise_gpu.coherenceEdgeBuilds() != 0U ||
      adjacent_precise_gpu.coherenceSnappedPrimitives() != 0U ||
      adjacent_precise_gpu.coherenceSnappedVertices() != 0U ||
      adjacent_precise_gpu.identityCanonicalBuilds() != 0U ||
      adjacent_precise_gpu.identityRecoveredVertices() != 0U ||
      adjacent_precise_gpu.identityConflictPrimitives() != 0U ||
      PGXP_GetIndex(0) != 8U || precise_latch != 0xffffU) {
    std::cerr << "Adjacent eligible F4 packets were not emitted as one "
                 "all-precise coherence unit\n";
    PsyX_Shutdown();
    return 51;
  }
  DrawSync(0);

  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu default_coherence_gpu;
  auto default_coherence_projections = make_adjacent_projections();
  default_coherence_projections[6] = {};
  GR_BeginGuestSubmit();
  default_coherence_gpu.submit(adjacent_f4_words,
                               default_coherence_projections);
  std::array<PGXPVData, 4U> default_precise_cache{};
  auto default_precise_fractional = true;
  constexpr std::array<float, 4U> expected_default_x{8.25F, 24.25F, 8.25F,
                                                     24.25F};
  for (std::size_t index{}; index < default_precise_cache.size(); ++index) {
    default_precise_fractional =
        default_precise_fractional &&
        PGXP_GetCacheDataExact(&default_precise_cache[index],
                               static_cast<u_short>(index)) != 0 &&
        default_precise_cache[index].sx == expected_default_x[index] &&
        (default_precise_cache[index].sy == 8.25F ||
         default_precise_cache[index].sy == 24.25F);
  }
  if (default_coherence_gpu.coherenceEdgeBuilds() != 0U ||
      default_coherence_gpu.coherenceSnappedPrimitives() != 0U ||
      default_coherence_gpu.coherenceSnappedVertices() != 0U ||
      default_coherence_gpu.sharedMeshBuilds() != 0U ||
      default_coherence_gpu.precisePrimitives() != 1U ||
      !default_precise_fractional || PGXP_GetIndex(0) != 4U) {
    std::cerr << "Fallback packet lost bounded affine XY or retained W\n";
    PsyX_Shutdown();
    return 69;
  }
  DrawSync(0);

  // Production coherence is demand-only: exact geometry which crosses the
  // near plane may bridge a missing neighbour, while the far case above
  // stays untouched. The bridge preserves signed W and the exact GPU path.
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu near_default_coherence_gpu;
  near_default_coherence_gpu.setCoherenceEdgeSnapping(true);
  auto near_default_projections = make_adjacent_projections();
  constexpr std::array<std::size_t, 4U> near_left_words{1U, 2U, 3U, 4U};
  constexpr std::array<float, 4U> near_left_depths{40.0F, 16.0F, 40.0F, 40.0F};
  for (std::size_t vertex{}; vertex < near_left_words.size(); ++vertex) {
    auto &projection = near_default_projections[near_left_words[vertex]];
    projection.view_z = near_left_depths[vertex];
    projection.projective_depth =
        std::max(projection.screen_h * 0.5F, projection.view_z);
    projection.view_x = (projection.screen_x - projection.screen_offset_x) *
                        projection.view_z / projection.screen_h;
    projection.view_y = (projection.screen_y - projection.screen_offset_y) *
                        projection.view_z / projection.screen_h;
    projection.exact_transform = true;
  }
  near_default_projections[6U] = {};
  GR_BeginGuestSubmit();
  near_default_coherence_gpu.submit(adjacent_f4_words,
                                    near_default_projections);
  if (near_default_coherence_gpu.polygonPrimitives() != 2U ||
      near_default_coherence_gpu.preciseCandidates() != 1U ||
      near_default_coherence_gpu.precisePrimitives() != 1U ||
      near_default_coherence_gpu.coherenceEdgeBuilds() != 1U ||
      near_default_coherence_gpu.coherenceSnappedPrimitives() != 1U ||
      near_default_coherence_gpu.coherenceSnappedVertices() != 2U ||
      PGXP_GetIndex(0) != 4U) {
    std::cerr << "Near exact/fallback edge did not use the demand-only "
                 "homogeneous bridge; candidate="
              << near_default_coherence_gpu.preciseCandidates()
              << " precise=" << near_default_coherence_gpu.precisePrimitives()
              << " edge=" << near_default_coherence_gpu.coherenceEdgeBuilds()
              << " snapped="
              << near_default_coherence_gpu.coherenceSnappedPrimitives() << '/'
              << near_default_coherence_gpu.coherenceSnappedVertices()
              << " cache=" << PGXP_GetIndex(0) << '\n';
    PsyX_Shutdown();
    return 89;
  }
  DrawSync(0);

  // W=0 has no finite screen position. Reject the whole primitive to raw GP0
  // instead of manufacturing a homogeneous bridge from undefined geometry.
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu zero_w_coherence_gpu;
  zero_w_coherence_gpu.setCoherenceEdgeSnapping(true);
  auto zero_w_projections = near_default_projections;
  zero_w_projections[2U].view_x = 3.0F;
  zero_w_projections[2U].view_y = -2.0F;
  zero_w_projections[2U].view_z = 0.0F;
  zero_w_projections[2U].projective_depth =
      zero_w_projections[2U].screen_h * 0.5F;
  GR_BeginGuestSubmit();
  zero_w_coherence_gpu.submit(adjacent_f4_words, zero_w_projections);
  if (zero_w_coherence_gpu.preciseCandidates() != 0U ||
      zero_w_coherence_gpu.precisePrimitives() != 0U ||
      zero_w_coherence_gpu.coherenceEdgeBuilds() != 1U ||
      zero_w_coherence_gpu.coherenceSnappedPrimitives() != 0U ||
      zero_w_coherence_gpu.coherenceSnappedVertices() != 0U ||
      PGXP_GetIndex(0) != 0U) {
    std::cerr << "Undefined W escaped primitive-atomic raw fallback\n";
    PsyX_Shutdown();
    return 90;
  }
  DrawSync(0);

  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu adjacent_missing_gpu;
  adjacent_missing_gpu.setCoherenceEdgeSnapping(true);
  auto adjacent_missing_projections = make_adjacent_projections();
  // The left quad has a different 3D depth at the same packed shared-edge
  // SXY. A screen-position cache must never lend that W to the right quad.
  adjacent_missing_projections[2].view_x *= 0.5F;
  adjacent_missing_projections[2].view_y *= 0.5F;
  adjacent_missing_projections[2].view_z *= 0.5F;
  adjacent_missing_projections[6] = {};
  GR_BeginGuestSubmit();
  adjacent_missing_gpu.submit(adjacent_f4_words, adjacent_missing_projections);
  PGXPVData snapped_top{};
  PGXPVData snapped_bottom{};
  PGXPVData untouched_top{};
  PGXPVData untouched_bottom{};
  const auto shared_edge_snapped =
      PGXP_GetCacheDataExact(&snapped_top, 1U) != 0 &&
      PGXP_GetCacheDataExact(&snapped_bottom, 3U) != 0 &&
      PGXP_GetCacheDataExact(&untouched_top, 0U) != 0 &&
      PGXP_GetCacheDataExact(&untouched_bottom, 2U) != 0 &&
      snapped_top.sx == 24.0F && snapped_top.sy == 8.0F &&
      snapped_bottom.sx == 24.0F && snapped_bottom.sy == 24.0F &&
      untouched_top.sx == 8.25F && untouched_bottom.sx == 8.25F &&
      snapped_top.pz != 0.0F && snapped_bottom.pz != 0.0F;
  const auto missing_latch = PGXP_GetIndex(1);
  if (adjacent_missing_gpu.polygonPrimitives() != 2U ||
      adjacent_missing_gpu.preciseCandidates() != 1U ||
      adjacent_missing_gpu.partialProjectionPrimitives() != 1U ||
      adjacent_missing_gpu.coherenceFallbackPrimitives() != 0U ||
      adjacent_missing_gpu.precisePrimitives() != 1U ||
      adjacent_missing_gpu.coherenceEdgeBuilds() != 1U ||
      adjacent_missing_gpu.coherenceSnappedPrimitives() != 1U ||
      adjacent_missing_gpu.coherenceSnappedVertices() != 2U ||
      !shared_edge_snapped || PGXP_GetIndex(0) != 4U ||
      missing_latch != 0xffffU) {
    std::cerr << "Precise/fallback shared edge was not snapped locally\n";
    PsyX_Shutdown();
    return 52;
  }
  DrawSync(0);

  // A collinear subsegment without shared endpoints is not mesh identity.
  // Coupling it to the longer edge collapses unrelated subpixel geometry as
  // the packet coordinates change during animation.
  constexpr std::array<std::uint32_t, 8U> clipped_subsegment_words{
      0x20ffffffU, 0x00080008U, 0x00080028U, 0x00180008U,
      0x20ffffffU, 0x00080010U, 0x00080020U, 0x00000010U,
  };
  std::array<sf::psx::GteProjectedVertex, clipped_subsegment_words.size()>
      clipped_subsegment_projections{};
  clipped_subsegment_projections[1] =
      make_projection(clipped_subsegment_words[1], 8.25F, 8.25F);
  clipped_subsegment_projections[2] =
      make_projection(clipped_subsegment_words[2], 40.25F, 8.25F);
  clipped_subsegment_projections[3] =
      make_projection(clipped_subsegment_words[3], 8.25F, 24.25F);
  clipped_subsegment_projections[5] =
      make_projection(clipped_subsegment_words[5], 16.0F, 8.0F);
  clipped_subsegment_projections[6] = {};
  clipped_subsegment_projections[7] =
      make_projection(clipped_subsegment_words[7], 16.0F, 0.0F);

  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu clipped_subsegment_gpu;
  clipped_subsegment_gpu.setCoherenceEdgeSnapping(true);
  GR_BeginGuestSubmit();
  clipped_subsegment_gpu.submit(clipped_subsegment_words,
                                clipped_subsegment_projections);
  PGXPVData clipped_edge_first{};
  PGXPVData clipped_edge_second{};
  PGXPVData clipped_edge_untouched{};
  const auto clipped_edge_stable =
      PGXP_GetCacheDataExact(&clipped_edge_first, 0U) != 0 &&
      PGXP_GetCacheDataExact(&clipped_edge_second, 1U) != 0 &&
      PGXP_GetCacheDataExact(&clipped_edge_untouched, 2U) != 0 &&
      clipped_edge_first.sx == 8.25F && clipped_edge_first.sy == 8.25F &&
      clipped_edge_second.sx == 40.25F && clipped_edge_second.sy == 8.25F &&
      clipped_edge_untouched.sx == 8.25F && clipped_edge_untouched.sy == 24.25F;
  if (clipped_subsegment_gpu.polygonPrimitives() != 2U ||
      clipped_subsegment_gpu.preciseCandidates() != 1U ||
      clipped_subsegment_gpu.precisePrimitives() != 1U ||
      clipped_subsegment_gpu.coherenceEdgeBuilds() != 1U ||
      clipped_subsegment_gpu.coherenceSnappedPrimitives() != 0U ||
      clipped_subsegment_gpu.coherenceSnappedVertices() != 0U ||
      !clipped_edge_stable || PGXP_GetIndex(0) != 3U) {
    std::cerr << "Collinear subsegment was incorrectly coupled to a distinct "
                 "precise edge; polygons="
              << clipped_subsegment_gpu.polygonPrimitives()
              << " candidates=" << clipped_subsegment_gpu.preciseCandidates()
              << " precise=" << clipped_subsegment_gpu.precisePrimitives()
              << " builds=" << clipped_subsegment_gpu.coherenceEdgeBuilds()
              << " snapped="
              << clipped_subsegment_gpu.coherenceSnappedPrimitives() << '/'
              << clipped_subsegment_gpu.coherenceSnappedVertices()
              << " cache=" << PGXP_GetIndex(0)
              << " edge=" << clipped_edge_stable
              << " first=" << clipped_edge_first.sx << ','
              << clipped_edge_first.sy << " second=" << clipped_edge_second.sx
              << ',' << clipped_edge_second.sy
              << " untouched=" << clipped_edge_untouched.sx << ','
              << clipped_edge_untouched.sy << '\n';
    PsyX_Shutdown();
    return 66;
  }
  DrawSync(0);

  // Equal packet coordinates under different GP0 draw offsets occupy
  // different framebuffer edges and must never be coupled by coherence.
  constexpr std::array<std::uint32_t, 11U> offset_isolated_words{
      0xe5000000U, 0x20ffffffU, 0x00080008U, 0x00080028U,
      0x00180008U, 0xe5000020U, 0x20ffffffU, 0x00080008U,
      0x00080028U, 0x00000008U, 0xe5000000U,
  };
  std::array<sf::psx::GteProjectedVertex, offset_isolated_words.size()>
      offset_isolated_projections{};
  offset_isolated_projections[2] =
      make_projection(offset_isolated_words[2], 8.25F, 8.25F);
  offset_isolated_projections[3] =
      make_projection(offset_isolated_words[3], 40.25F, 8.25F);
  offset_isolated_projections[4] =
      make_projection(offset_isolated_words[4], 8.25F, 24.25F);
  offset_isolated_projections[7] =
      make_projection(offset_isolated_words[7], 8.0F, 8.0F);
  offset_isolated_projections[8] = {};
  offset_isolated_projections[9] =
      make_projection(offset_isolated_words[9], 8.0F, 0.0F);

  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu offset_isolated_gpu;
  offset_isolated_gpu.setCoherenceEdgeSnapping(true);
  GR_BeginGuestSubmit();
  offset_isolated_gpu.submit(offset_isolated_words,
                             offset_isolated_projections);
  PGXPVData offset_edge_first{};
  PGXPVData offset_edge_second{};
  const auto offset_edge_untouched =
      PGXP_GetCacheDataExact(&offset_edge_first, 0U) != 0 &&
      PGXP_GetCacheDataExact(&offset_edge_second, 1U) != 0 &&
      offset_edge_first.sx == 8.25F && offset_edge_first.sy == 8.25F &&
      offset_edge_second.sx == 40.25F && offset_edge_second.sy == 8.25F;
  if (offset_isolated_gpu.polygonPrimitives() != 2U ||
      offset_isolated_gpu.preciseCandidates() != 1U ||
      offset_isolated_gpu.precisePrimitives() != 1U ||
      offset_isolated_gpu.coherenceEdgeBuilds() != 1U ||
      offset_isolated_gpu.coherenceSnappedPrimitives() != 0U ||
      offset_isolated_gpu.coherenceSnappedVertices() != 0U ||
      !offset_edge_untouched || PGXP_GetIndex(0) != 3U) {
    std::cerr << "Different GP0 draw offsets coupled identical packed edges\n";
    PsyX_Shutdown();
    return 67;
  }
  DrawSync(0);

  // Exact-transform metadata carries a coherent signed view tuple. Geometry
  // crossing the renderer near plane must remain precise for primitive-time
  // view-space clipping instead of falling back to affine interpolation.
  auto close_z_projections = make_adjacent_projections();
  constexpr std::array<std::size_t, 4U> close_z_words{6U, 7U, 8U, 9U};
  constexpr std::array<float, 4U> alternating_z{-64.0F, 2000.0F, -64.0F,
                                                2000.0F};
  for (std::size_t vertex{}; vertex < close_z_words.size(); ++vertex) {
    auto &projection = close_z_projections[close_z_words[vertex]];
    projection.screen_h = 1.0F;
    projection.view_z = alternating_z[vertex];
    projection.projective_depth =
        std::max(projection.screen_h * 0.5F, projection.view_z);
    projection.view_x = projection.screen_x * projection.view_z;
    projection.view_y = projection.screen_y * projection.view_z;
    projection.exact_transform = true;
    projection.divide_overflow = (vertex & 1U) == 0U;
  }

  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu close_z_gpu;
  close_z_gpu.setGeometryOptions(true, true, true, true, false, false, true);
  close_z_gpu.setCoherenceEdgeSnapping(true);
  GR_BeginGuestSubmit();
  close_z_gpu.submit(adjacent_f4_words, close_z_projections);
  PGXPVData close_vertex{};
  PGXPVData far_vertex{};
  const auto signed_depth_preserved =
      PGXP_GetCacheDataExact(&close_vertex, 4U) != 0 &&
      PGXP_GetCacheDataExact(&far_vertex, 5U) != 0 &&
      close_vertex.exact_projection != 0U &&
      far_vertex.exact_projection != 0U &&
      close_vertex.pz == alternating_z[0U] / 128.0F &&
      far_vertex.pz == alternating_z[1U] / 128.0F;
  if (close_z_gpu.polygonPrimitives() != 2U ||
      close_z_gpu.preciseCandidates() != 2U ||
      close_z_gpu.precisePrimitives() != 2U ||
      close_z_gpu.projectionHazardPrimitives() != 0U ||
      close_z_gpu.nearPlanePrimitives() != 0U ||
      close_z_gpu.coherenceEdgeBuilds() != 0U ||
      close_z_gpu.coherenceSnappedPrimitives() != 0U ||
      close_z_gpu.coherenceSnappedVertices() != 0U || !signed_depth_preserved ||
      PGXP_GetIndex(0) != 8U) {
    std::cerr << "Close-Z exact quad did not reach view-space clipping\n";
    PsyX_Shutdown();
    return 68;
  }
  DrawSync(0);

  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu adjacent_mismatch_gpu;
  adjacent_mismatch_gpu.setCoherenceEdgeSnapping(true);
  auto adjacent_mismatch_projections = make_adjacent_projections();
  auto &mismatched_shared_vertex = adjacent_mismatch_projections[6];
  mismatched_shared_vertex.screen_x = 25.05F;
  mismatched_shared_vertex.view_x =
      mismatched_shared_vertex.screen_x * 1000.0F / 320.0F;
  GR_BeginGuestSubmit();
  adjacent_mismatch_gpu.submit(adjacent_f4_words,
                               adjacent_mismatch_projections);
  const auto mismatch_latch = PGXP_GetIndex(1);
  if (adjacent_mismatch_gpu.polygonPrimitives() != 2U ||
      adjacent_mismatch_gpu.preciseCandidates() != 2U ||
      adjacent_mismatch_gpu.partialProjectionPrimitives() != 0U ||
      adjacent_mismatch_gpu.coherenceFallbackPrimitives() != 0U ||
      adjacent_mismatch_gpu.precisePrimitives() != 2U ||
      PGXP_GetIndex(0) != 8U || mismatch_latch != 0xffffU) {
    std::cerr << "Screen-collinear F4 packets were incorrectly coupled by "
                 "the PGXP policy\n";
    PsyX_Shutdown();
    return 53;
  }
  DrawSync(0);

  // SF emits one exact projection for each source vertex and reuses it for
  // every adjacent polygon. Raw GP0 packets now carry the same invariant as a
  // source identity: the right packet can recover its missing shared-edge
  // sidecar without searching by SXY or borrowing an unrelated W.
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu identity_recovery_gpu;
  auto identity_recovery_projections = make_adjacent_projections();
  std::array<std::uint64_t, adjacent_f4_words.size()> recovery_identities{};
  constexpr std::array<std::uint64_t, 8U> adjacent_vertex_identities{
      101U, 102U, 103U, 104U, 102U, 105U, 104U, 106U,
  };
  constexpr std::array<std::size_t, 8U> adjacent_coordinate_words{
      1U, 2U, 3U, 4U, 6U, 7U, 8U, 9U,
  };
  const auto set_identity_depth = [](auto &projection, float view_z) {
    projection.view_z = view_z;
    projection.projective_depth = std::max(projection.screen_h * 0.5F, view_z);
    projection.view_x = (projection.screen_x - projection.screen_offset_x) *
                        view_z / projection.screen_h;
    projection.view_y = (projection.screen_y - projection.screen_offset_y) *
                        view_z / projection.screen_h;
  };
  set_identity_depth(identity_recovery_projections[2], 640.0F);
  set_identity_depth(identity_recovery_projections[7], 1600.0F);
  set_identity_depth(identity_recovery_projections[9], 2000.0F);
  for (std::size_t vertex{}; vertex < adjacent_coordinate_words.size();
       ++vertex) {
    recovery_identities[adjacent_coordinate_words[vertex]] =
        adjacent_vertex_identities[vertex];
  }
  identity_recovery_projections[6] = {};
  GR_BeginGuestSubmit();
  identity_recovery_gpu.submit(adjacent_f4_words, identity_recovery_projections,
                               recovery_identities, 7001U);
  PGXPVData recovered_left_edge{};
  PGXPVData recovered_right_edge{};
  PGXPVData unequal_depth_vertex{};
  const auto recovered_edge_coherent =
      PGXP_GetCacheDataExact(&recovered_left_edge, 1U) != 0 &&
      PGXP_GetCacheDataExact(&recovered_right_edge, 4U) != 0 &&
      PGXP_GetCacheDataExact(&unequal_depth_vertex, 5U) != 0 &&
      recovered_left_edge.sx == recovered_right_edge.sx &&
      recovered_left_edge.sy == recovered_right_edge.sy &&
      recovered_left_edge.pz == recovered_right_edge.pz &&
      recovered_left_edge.scr_h == recovered_right_edge.scr_h &&
      recovered_right_edge.pz != unequal_depth_vertex.pz;
  if (identity_recovery_gpu.polygonPrimitives() != 2U ||
      identity_recovery_gpu.preciseCandidates() != 2U ||
      identity_recovery_gpu.precisePrimitives() != 2U ||
      identity_recovery_gpu.identityCanonicalBuilds() != 1U ||
      identity_recovery_gpu.identityRecoveredVertices() != 1U ||
      identity_recovery_gpu.identityRecoveredPrimitives() != 1U ||
      identity_recovery_gpu.identityConflictPrimitives() != 0U ||
      PGXP_GetIndex(0) != 8U || !recovered_edge_coherent) {
    std::cerr << "Source identity did not recover one coherent shared-edge "
                 "projection; precise="
              << identity_recovery_gpu.precisePrimitives() << " recovered="
              << identity_recovery_gpu.identityRecoveredVertices() << '/'
              << identity_recovery_gpu.identityRecoveredPrimitives()
              << " conflicts="
              << identity_recovery_gpu.identityConflictPrimitives() << '\n';
    PsyX_Shutdown();
    return 61;
  }
  DrawSync(0);

  // Re-publishing the same geometric projection may legitimately change its
  // transport lineage/epoch. Those fields must not poison the source identity
  // or prevent a second shared edge from being recovered.
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu republished_identity_gpu;
  auto republished_projections = make_adjacent_projections();
  republished_projections[2].transform_lineage = 5001U;
  republished_projections[2].projection_epoch = 6001U;
  republished_projections[6].transform_lineage = 5002U;
  republished_projections[6].projection_epoch = 6002U;
  republished_projections[8] = {};
  std::array<std::uint64_t, adjacent_f4_words.size()> republished_identities{};
  republished_identities[2] = 151U;
  republished_identities[6] = 151U;
  republished_identities[4] = 152U;
  republished_identities[8] = 152U;
  GR_BeginGuestSubmit();
  republished_identity_gpu.submit(adjacent_f4_words, republished_projections,
                                  republished_identities, 7004U);
  if (republished_identity_gpu.precisePrimitives() != 2U ||
      republished_identity_gpu.identityRecoveredVertices() != 1U ||
      republished_identity_gpu.identityRecoveredPrimitives() != 1U ||
      republished_identity_gpu.identityConflictPrimitives() != 0U ||
      PGXP_GetIndex(0) != 8U) {
    std::cerr << "Publication metadata poisoned an unchanged source "
                 "projection; precise="
              << republished_identity_gpu.precisePrimitives() << " conflicts="
              << republished_identity_gpu.identityConflictPrimitives() << '\n';
    PsyX_Shutdown();
    return 65;
  }
  DrawSync(0);

  // Equal packed SXY is not identity. Give the visible left edge a different
  // source identity and depth from the missing right edge; only the complete
  // left primitive may use PGXP.
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu distinct_identity_gpu;
  auto distinct_identity_projections = make_adjacent_projections();
  distinct_identity_projections[2].view_x *= 0.5F;
  distinct_identity_projections[2].view_y *= 0.5F;
  distinct_identity_projections[2].view_z *= 0.5F;
  distinct_identity_projections[6] = {};
  std::array<std::uint64_t, adjacent_f4_words.size()> distinct_identities{};
  distinct_identities[2] = 201U;
  distinct_identities[6] = 202U;
  GR_BeginGuestSubmit();
  distinct_identity_gpu.submit(adjacent_f4_words, distinct_identity_projections,
                               distinct_identities, 7002U);
  if (distinct_identity_gpu.precisePrimitives() != 1U ||
      distinct_identity_gpu.identityRecoveredVertices() != 0U ||
      distinct_identity_gpu.identityRecoveredPrimitives() != 0U ||
      distinct_identity_gpu.identityConflictPrimitives() != 0U ||
      PGXP_GetIndex(0) != 4U) {
    std::cerr << "Equal SXY with distinct source identities reused an "
                 "unrelated depth\n";
    PsyX_Shutdown();
    return 62;
  }
  DrawSync(0);

  // A source identity with two full, disagreeing projection witnesses is
  // poisoned for this submit. Both touching primitives must fall back as
  // complete units instead of choosing whichever witness was inserted first.
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu conflicting_identity_gpu;
  auto conflicting_identity_projections = make_adjacent_projections();
  auto &conflicting_witness = conflicting_identity_projections[6];
  conflicting_witness.screen_x += 0.5F;
  conflicting_witness.view_x = conflicting_witness.screen_x *
                               conflicting_witness.view_z /
                               conflicting_witness.screen_h;
  std::array<std::uint64_t, adjacent_f4_words.size()> conflicting_identities{};
  conflicting_identities[2] = 301U;
  conflicting_identities[6] = 301U;
  // Trigger the demand path independently while keeping both disagreeing
  // witnesses full: word 8 recovers from the coherent copy at word 4.
  conflicting_identity_projections[8] = {};
  conflicting_identities[4] = 302U;
  conflicting_identities[8] = 302U;
  GR_BeginGuestSubmit();
  conflicting_identity_gpu.submit(adjacent_f4_words,
                                  conflicting_identity_projections,
                                  conflicting_identities, 7003U);
  if (conflicting_identity_gpu.precisePrimitives() != 0U ||
      conflicting_identity_gpu.identityCanonicalBuilds() != 1U ||
      conflicting_identity_gpu.identityRecoveredVertices() != 0U ||
      conflicting_identity_gpu.identityConflictPrimitives() != 2U ||
      PGXP_GetIndex(0) != 4U) {
    std::cerr << "Conflicting source identity did not fail both primitives "
                 "closed; precise="
              << conflicting_identity_gpu.precisePrimitives() << " builds="
              << conflicting_identity_gpu.identityCanonicalBuilds()
              << " recovered="
              << conflicting_identity_gpu.identityRecoveredVertices()
              << " conflicts="
              << conflicting_identity_gpu.identityConflictPrimitives()
              << " cache=" << PGXP_GetIndex(0) << '\n';
    PsyX_Shutdown();
    return 63;
  }
  DrawSync(0);

  // An epoch change discards an incomplete command and its source identity.
  // The next epoch must not recover a missing vertex from that stale witness.
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu identity_epoch_gpu;
  constexpr std::uint64_t stale_identity = 401U;
  const std::array<std::uint32_t, 2U> incomplete_epoch_words{
      fallback_triangle[0], fallback_triangle[1]};
  std::array<sf::psx::GteProjectedVertex, 2U> incomplete_epoch_projections{};
  incomplete_epoch_projections[1] = pgxp_projections[4];
  const std::array<std::uint64_t, 2U> incomplete_epoch_identities{
      0U, stale_identity};
  identity_epoch_gpu.submit(incomplete_epoch_words,
                            incomplete_epoch_projections,
                            incomplete_epoch_identities, 8001U);
  identity_epoch_gpu.submit(std::span<const std::uint32_t>{},
                            std::span<const sf::psx::GteProjectedVertex>{},
                            std::span<const std::uint64_t>{}, 8002U);
  std::array<sf::psx::GteProjectedVertex, fallback_triangle.size()>
      fresh_epoch_projections{};
  fresh_epoch_projections[2] = pgxp_projections[5];
  fresh_epoch_projections[3] = pgxp_projections[6];
  std::array<std::uint64_t, fallback_triangle.size()> fresh_epoch_identities{};
  fresh_epoch_identities[1] = stale_identity;
  identity_epoch_gpu.submit(fallback_triangle, fresh_epoch_projections,
                            fresh_epoch_identities, 8002U);
  if (identity_epoch_gpu.submittedCommands() != 1U ||
      identity_epoch_gpu.precisePrimitives() != 0U ||
      identity_epoch_gpu.identityRecoveredVertices() != 0U ||
      PGXP_GetIndex(0) != 0U) {
    std::cerr << "Projection identity or pending command leaked across an "
                 "epoch boundary\n";
    PsyX_Shutdown();
    return 64;
  }
  DrawSync(0);
  PsyX_EndScene();

  // Submit a fractional coloured PGXP triangle without an intervening DrawSync.
  // presentDisplay must flush it, capture the exact display page and use the
  // retained selected-resolution page in the production scanout path.
  GR_ClearVRAM(0, 0, output_size, output_size, 0, 0, 0);
  GR_UpdateVRAM();
  PGXP_ClearCache();
  sf::platform::detail::PsyCrossGuestGpu precise_scanout_gpu;
  GR_BeginGuestSubmit();
  precise_scanout_gpu.submit(pgxp_triangle, pgxp_projections);
  precise_scanout_gpu.presentDisplay(0, 0, output_size, output_size, true,
                                     false, false);
  if (GR_HasHighResolutionVRAM(0, 0, output_size, output_size) == 0) {
    std::cerr << "Pending PGXP page was not retained by production scanout\n";
    PsyX_EndScene();
    PsyX_Shutdown();
    return 21;
  }
  std::array<unsigned char, 128U * 128U * 4U> precise_scanout{};
  glReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE,
               precise_scanout.data());
  bool mixed_guest_pixel{};
  std::size_t mixed_x{};
  std::size_t mixed_y{};
  std::size_t precise_coloured_pixels{};
  for (std::size_t pixel{}; pixel < precise_scanout.size(); pixel += 4U) {
    precise_coloured_pixels += precise_scanout[pixel] != 0U ||
                                       precise_scanout[pixel + 1U] != 0U ||
                                       precise_scanout[pixel + 2U] != 0U
                                   ? 1U
                                   : 0U;
  }
  for (std::size_t y{}; y < 128U && !mixed_guest_pixel; y += 2U) {
    for (std::size_t x{}; x < 128U; x += 2U) {
      bool has_colour{};
      bool has_black{};
      for (std::size_t dy{}; dy < 2U; ++dy) {
        for (std::size_t dx{}; dx < 2U; ++dx) {
          const auto pixel = ((y + dy) * 128U + x + dx) * 4U;
          const auto red = precise_scanout[pixel] >> 3U;
          const auto green = precise_scanout[pixel + 1U] >> 3U;
          const auto blue = precise_scanout[pixel + 2U] >> 3U;
          has_colour |= red != 0U || green != 0U || blue != 0U;
          has_black |= red == 0U && green == 0U && blue == 0U;
        }
      }
      if (has_colour && has_black) {
        mixed_guest_pixel = true;
        mixed_x = x;
        mixed_y = y;
        break;
      }
    }
  }
  if (!mixed_guest_pixel) {
    std::cerr
        << "Production PGXP scanout has no mixed 2x2 coverage block; coloured="
        << precise_coloured_pixels << '\n';
    PsyX_EndScene();
    PsyX_Shutdown();
    return 23;
  }

  // A partial draw into the same guest page must seed from the retained
  // selected-resolution page. Otherwise the untouched fractional edge collapses
  // to 1x here.
  constexpr std::array<std::uint32_t, 6U> partial_draw{
      0xe3000000U, 0xe400fc3fU, 0xe5000000U,
      0x6000ff00U, 0x003c003cU, 0x00010001U,
  };
  precise_scanout_gpu.submit(partial_draw);
  DrawSync(0);
  RECT16 completed_partial{};
  GR_SetOffscreenState(&completed_partial, 0);
  std::array<unsigned char, 128U * 128U * 4U> post_host_boundary{};
  glReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE,
               post_host_boundary.data());
  if (!std::ranges::equal(precise_scanout, post_host_boundary)) {
    std::cerr << "Later guest draw overwrote the already presented page at "
                 "the guest-to-host boundary\n";
    PsyX_EndScene();
    PsyX_Shutdown();
    return 26;
  }
  PsyX_EndScene();

  // The partial page was captured at DrawSync but was intentionally not
  // presented above. Present it now from the retained cache and verify that
  // the untouched fractional edge survived its seed pass.
  static_cast<void>(PsyX_BeginScene());
  precise_scanout_gpu.presentDisplay(0, 0, output_size, output_size, true,
                                     false, false);
  std::array<unsigned char, 128U * 128U * 4U> partial_scanout{};
  glReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE,
               partial_scanout.data());
  bool retained_edge_matches = true;
  for (std::size_t dy{}; dy < 2U; ++dy) {
    for (std::size_t dx{}; dx < 2U; ++dx) {
      const auto pixel = ((mixed_y + dy) * 128U + mixed_x + dx) * 4U;
      retained_edge_matches = retained_edge_matches &&
                              std::equal(precise_scanout.data() + pixel,
                                         precise_scanout.data() + pixel + 3U,
                                         partial_scanout.data() + pixel);
    }
  }
  PsyX_EndScene();
  if (!retained_edge_matches) {
    std::cerr << "Partial redraw collapsed untouched retained "
                 "selected-resolution coverage\n";
    PsyX_Shutdown();
    return 25;
  }

  struct ExpectedRgbPatch {
    std::size_t x{};
    std::size_t y{};
    std::size_t width{};
    std::size_t height{};
    std::array<unsigned char, 3U> rgb{};
  };
  constexpr std::size_t retained_scanout_size = output_size * 2U;
  const auto scanout_has_exact_patches =
      [](const auto &before, const auto &after,
         std::span<const ExpectedRgbPatch> patches) {
        std::size_t expected_changed_pixels{};
        for (const auto &patch : patches) {
          expected_changed_pixels += patch.width * patch.height;
        }

        std::size_t changed_pixels{};
        for (std::size_t y{}; y < retained_scanout_size; ++y) {
          for (std::size_t x{}; x < retained_scanout_size; ++x) {
            const ExpectedRgbPatch *expected_patch{};
            for (const auto &patch : patches) {
              if (x >= patch.x && x < patch.x + patch.width && y >= patch.y &&
                  y < patch.y + patch.height) {
                expected_patch = &patch;
                break;
              }
            }

            const auto pixel = (y * retained_scanout_size + x) * 4U;
            const bool changed =
                !std::equal(before.data() + pixel, before.data() + pixel + 3U,
                            after.data() + pixel);
            if (changed) {
              ++changed_pixels;
            }
            if (expected_patch != nullptr) {
              if (!std::equal(expected_patch->rgb.begin(),
                              expected_patch->rgb.end(),
                              after.data() + pixel)) {
                std::cerr << "Patch colour mismatch at " << x << ',' << y
                          << " got=" << static_cast<unsigned>(after[pixel])
                          << ',' << static_cast<unsigned>(after[pixel + 1U])
                          << ',' << static_cast<unsigned>(after[pixel + 2U])
                          << '\n';
                return false;
              }
            } else if (changed) {
              std::cerr << "Unexpected retained change at " << x << ',' << y
                        << " before=" << static_cast<unsigned>(before[pixel])
                        << ',' << static_cast<unsigned>(before[pixel + 1U])
                        << ',' << static_cast<unsigned>(before[pixel + 2U])
                        << " after=" << static_cast<unsigned>(after[pixel])
                        << ',' << static_cast<unsigned>(after[pixel + 1U])
                        << ',' << static_cast<unsigned>(after[pixel + 2U])
                        << '\n';
              return false;
            }
          }
        }
        return changed_pixels == expected_changed_pixels;
      };

  // A later, contained draw area must patch the retained full scanout instead
  // of replacing it with a small exact-rectangle cache entry. Alternating
  // between the retained selected-resolution path and the logical VRAM fallback
  // makes every fractional edge visibly shimmer even when the guest camera is
  // stationary. Keep the existing fractional edge inside the nested rectangle
  // while the new tile stays away from it, so this also covers contained-page
  // seeding.
  constexpr std::array<std::uint32_t, 6U> nested_draw{
      0xe3000000U, 0xe4007c1fU, 0xe5000000U,
      0x600000ffU, 0x001e001eU, 0x00010001U,
  };
  while (glGetError() != GL_NO_ERROR) {
  }
  precise_scanout_gpu.submit(nested_draw);
  DrawSync(0);
  RECT16 completed_nested{};
  GR_SetOffscreenState(&completed_nested, 0);
  const bool nested_full_page_retained =
      GR_HasHighResolutionVRAM(0, 0, output_size, output_size) != 0;
  precise_scanout_gpu.presentDisplay(0, 0, output_size, output_size, true,
                                     false, false);
  std::array<unsigned char, 128U * 128U * 4U> nested_scanout{};
  glReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE,
               nested_scanout.data());
  const auto nested_gl_error = glGetError();
  constexpr std::array<ExpectedRgbPatch, 1U> nested_expected{{
      {30U * 2U, 73U, 2U, 2U, {255U, 0U, 0U}},
  }};
  const bool nested_patch_exact = scanout_has_exact_patches(
      partial_scanout, nested_scanout, nested_expected);
  bool nested_edge_matches = true;
  for (std::size_t dy{}; dy < 2U; ++dy) {
    for (std::size_t dx{}; dx < 2U; ++dx) {
      const auto pixel = ((mixed_y + dy) * 128U + mixed_x + dx) * 4U;
      nested_edge_matches =
          nested_edge_matches && std::equal(partial_scanout.data() + pixel,
                                            partial_scanout.data() + pixel + 3U,
                                            nested_scanout.data() + pixel);
    }
  }
  PsyX_EndScene();
  if (!nested_full_page_retained || !nested_edge_matches ||
      !nested_patch_exact || nested_gl_error != GL_NO_ERROR) {
    std::cerr << "Contained draw area failed exact 2x cache patch; retained="
              << nested_full_page_retained << " edge=" << nested_edge_matches
              << " patch=" << nested_patch_exact << " gl=0x" << std::hex
              << nested_gl_error << std::dec << '\n';
    PsyX_Shutdown();
    return 32;
  }

  // A raw game frame alternates between two VRAM draw pages. Rendering the
  // same projected scene into either page must produce the same native image;
  // the page address is storage state, not a presentation-space translation.
  // Then move the exact projection by half a logical pixel per frame. At 2x,
  // every frame must be the previous image translated by exactly one physical
  // pixel even while the destination page alternates. The legacy path has no
  // subpixel sidecar, so the identical packed packet remains stationary and a
  // one-pixel packed-coordinate step moves it by exactly two physical pixels.
  constexpr int stability_page_a_y = 0;
  constexpr int stability_page_b_y = output_size;
  constexpr int stability_native_size = output_size * 2;
  using StabilityFrame =
      std::array<unsigned char,
                 stability_native_size * stability_native_size * 4>;

  sf::platform::detail::PsyCrossGuestGpu stability_gpu;
  bool stability_pgxp_submission_valid = true;
  const auto capture_stability_frame = [&](int page_y, float exact_x_shift,
                                           int packed_x_shift, bool precise) {
    GR_ClearVRAM(0, page_y, output_size, output_size, 0, 0, 0);
    GR_UpdateVRAM();
    GR_BeginGuestSubmit();

    const auto pack_xy = [](int x, int y) {
      return static_cast<std::uint32_t>(static_cast<std::uint16_t>(x)) |
             (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y)) << 16U);
    };
    const std::array<std::uint32_t, 7U> commands{
        0xe3000000U | (static_cast<std::uint32_t>(page_y) << 10U),
        0xe4000000U | static_cast<std::uint32_t>(output_size - 1) |
            (static_cast<std::uint32_t>(page_y + output_size - 1) << 10U),
        0xe5000000U | (static_cast<std::uint32_t>(page_y) << 11U),
        0x20ff0000U,
        pack_xy(8 + packed_x_shift, 8),
        pack_xy(24 + packed_x_shift, 8),
        pack_xy(8 + packed_x_shift, 24),
    };

    if (precise) {
      PGXP_ClearCache();
      std::array<sf::psx::GteProjectedVertex, 7U> projections{};
      const auto set_stability_projection =
          [&](std::size_t word, float screen_x, float screen_y) {
            auto &projection = projections[word];
            projection.packed_sxy = commands[word];
            projection.view_x = (screen_x - 32.0F) * 1000.0F / 320.0F;
            projection.view_y = (screen_y - 32.0F) * 1000.0F / 320.0F;
            projection.view_z = 1000.0F;
            projection.projective_depth = 1000.0F;
            projection.screen_x = screen_x;
            projection.screen_y = screen_y;
            projection.screen_h = 320.0F;
            projection.screen_offset_x = 32.0F;
            projection.screen_offset_y = 32.0F;
            projection.valid = true;
          };
      set_stability_projection(4U, 8.25F + exact_x_shift, 8.25F);
      set_stability_projection(5U, 24.25F + exact_x_shift, 8.25F);
      set_stability_projection(6U, 8.25F + exact_x_shift, 24.25F);
      const auto precise_before = stability_gpu.precisePrimitives();
      stability_gpu.submit(commands, projections);
      stability_pgxp_submission_valid =
          stability_pgxp_submission_valid &&
          stability_gpu.precisePrimitives() == precise_before + 1U;
    } else {
      stability_gpu.submit(commands);
    }

    stability_gpu.presentDisplay(0, static_cast<std::uint16_t>(page_y),
                                 output_size, output_size, true, false, false);
    StabilityFrame frame{};
    glReadPixels(0, 0, stability_native_size, stability_native_size, GL_RGBA,
                 GL_UNSIGNED_BYTE, frame.data());
    PsyX_EndScene();
    return frame;
  };

  const auto rgb_nonzero = [](const StabilityFrame &frame) {
    for (std::size_t pixel{}; pixel < frame.size(); pixel += 4U) {
      if (frame[pixel] != 0U || frame[pixel + 1U] != 0U ||
          frame[pixel + 2U] != 0U) {
        return true;
      }
    }
    return false;
  };
  const auto rgb_translated_right = [](const StabilityFrame &before,
                                       const StabilityFrame &after,
                                       std::size_t physical_pixels) {
    for (std::size_t y{}; y < stability_native_size; ++y) {
      for (std::size_t x{}; x < stability_native_size; ++x) {
        for (std::size_t channel{}; channel < 3U; ++channel) {
          const auto after_index =
              (y * stability_native_size + x) * 4U + channel;
          const auto expected =
              x >= physical_pixels
                  ? before[(y * stability_native_size + (x - physical_pixels)) *
                               4U +
                           channel]
                  : 0U;
          if (after[after_index] != expected) {
            return false;
          }
        }
      }
    }
    return true;
  };

  const auto pgxp_page_a =
      capture_stability_frame(stability_page_a_y, 0.0F, 0, true);
  const auto pgxp_page_b =
      capture_stability_frame(stability_page_b_y, 0.0F, 0, true);
  if (!stability_pgxp_submission_valid || !rgb_nonzero(pgxp_page_a)) {
    std::cerr
        << "Temporal stability scene did not use PGXP or drew no pixels\n";
    PsyX_Shutdown();
    return 27;
  }
  if (!std::ranges::equal(pgxp_page_a, pgxp_page_b)) {
    std::cerr << "Identical PGXP scene shifted between VRAM buffer pages\n";
    PsyX_Shutdown();
    return 28;
  }

  // Exercise both retained backbuffers through more contained draw-area
  // switches than the old exact-rectangle cache could hold. Each small pass
  // must update its containing page in place; neither full scanout may fall
  // back to the downsampled logical VRAM path.
  bool alternating_pages_retained = true;
  while (glGetError() != GL_NO_ERROR) {
  }
  for (std::uint32_t pass{}; pass < 8U; ++pass) {
    const auto page_y =
        pass % 2U == 0U ? stability_page_a_y : stability_page_b_y;
    const auto inset = 56U + pass % 4U;
    const auto absolute_y = static_cast<std::uint32_t>(page_y) + inset;
    const std::array<std::uint32_t, 6U> contained_commands{
        0xe3000000U | inset | (absolute_y << 10U),
        0xe4000000U | inset | (absolute_y << 10U),
        0xe5000000U | (static_cast<std::uint32_t>(page_y) << 11U),
        0x6000ff00U,
        inset | (inset << 16U),
        0x00010001U,
    };
    stability_gpu.submit(contained_commands);
    DrawSync(0);
    RECT16 completed_contained{};
    GR_SetOffscreenState(&completed_contained, 0);
    alternating_pages_retained =
        alternating_pages_retained &&
        GR_HasHighResolutionVRAM(0, stability_page_a_y, output_size,
                                 output_size) != 0 &&
        GR_HasHighResolutionVRAM(0, stability_page_b_y, output_size,
                                 output_size) != 0;
  }

  stability_gpu.presentDisplay(0, stability_page_a_y, output_size, output_size,
                               true, false, false);
  StabilityFrame churn_page_a{};
  glReadPixels(0, 0, stability_native_size, stability_native_size, GL_RGBA,
               GL_UNSIGNED_BYTE, churn_page_a.data());
  PsyX_EndScene();
  stability_gpu.presentDisplay(0, stability_page_b_y, output_size, output_size,
                               true, false, false);
  StabilityFrame churn_page_b{};
  glReadPixels(0, 0, stability_native_size, stability_native_size, GL_RGBA,
               GL_UNSIGNED_BYTE, churn_page_b.data());
  const auto churn_gl_error = glGetError();
  PsyX_EndScene();

  constexpr std::array<ExpectedRgbPatch, 2U> churn_page_a_expected{{
      {56U * 2U, (output_size - 57U) * 2U, 2U, 2U, {0U, 255U, 0U}},
      {58U * 2U, (output_size - 59U) * 2U, 2U, 2U, {0U, 255U, 0U}},
  }};
  constexpr std::array<ExpectedRgbPatch, 2U> churn_page_b_expected{{
      {57U * 2U, (output_size - 58U) * 2U, 2U, 2U, {0U, 255U, 0U}},
      {59U * 2U, (output_size - 60U) * 2U, 2U, 2U, {0U, 255U, 0U}},
  }};
  const bool alternating_pages_content_exact =
      scanout_has_exact_patches(pgxp_page_a, churn_page_a,
                                churn_page_a_expected) &&
      scanout_has_exact_patches(pgxp_page_b, churn_page_b,
                                churn_page_b_expected);
  if (!alternating_pages_retained || !alternating_pages_content_exact ||
      churn_gl_error != GL_NO_ERROR) {
    std::cerr << "Contained A/B churn failed exact 2x cache patch; retained="
              << alternating_pages_retained
              << " content=" << alternating_pages_content_exact << " gl=0x"
              << std::hex << churn_gl_error << std::dec << '\n';
    PsyX_Shutdown();
    return 33;
  }

  const auto pgxp_half_page_a =
      capture_stability_frame(stability_page_a_y, 0.5F, 0, true);
  const auto pgxp_one_page_b =
      capture_stability_frame(stability_page_b_y, 1.0F, 1, true);
  const auto pgxp_one_half_page_a =
      capture_stability_frame(stability_page_a_y, 1.5F, 1, true);
  if (!stability_pgxp_submission_valid ||
      !rgb_translated_right(pgxp_page_b, pgxp_half_page_a, 1U) ||
      !rgb_translated_right(pgxp_half_page_a, pgxp_one_page_b, 1U) ||
      !rgb_translated_right(pgxp_one_page_b, pgxp_one_half_page_a, 1U)) {
    std::cerr << "PGXP half-pixel motion was not a monotonic 1px x2 shift "
                 "across alternating pages\n";
    PsyX_Shutdown();
    return 29;
  }

  const auto precise_before_legacy = stability_gpu.precisePrimitives();
  const auto legacy_page_a =
      capture_stability_frame(stability_page_a_y, 0.0F, 0, false);
  const auto legacy_page_b =
      capture_stability_frame(stability_page_b_y, 0.5F, 0, false);
  const auto legacy_one_page_a =
      capture_stability_frame(stability_page_a_y, 1.0F, 1, false);
  if (stability_gpu.precisePrimitives() != precise_before_legacy ||
      !std::ranges::equal(legacy_page_a, legacy_page_b)) {
    std::cerr << "Legacy packed scene changed without an integer SXY step\n";
    PsyX_Shutdown();
    return 30;
  }
  if (!rgb_translated_right(legacy_page_b, legacy_one_page_a, 2U)) {
    std::cerr << "Legacy one-pixel SXY step was not a 2px x2 shift\n";
    PsyX_Shutdown();
    return 31;
  }

  // The legacy trail check below reads the single-sample draw target.
  g_cfg_msaaSamples = 0;

  // Native scanout uses the regular PS1 texture shader, where word zero is
  // transparent. Verify that an empty guest framebuffer replaces the previous
  // host frame instead of exposing it through those discarded fragments.
  PsyX_EndScene();
  DISPENV trail_display{};
  SetDefDispEnv(&trail_display, 0, 0, output_size, output_size);
  PutDispEnv(&trail_display);
  DRAWENV trail_draw{};
  SetDefDrawEnv(&trail_draw, 0, 0, output_size, output_size);
  trail_draw.dtd = 0;
  trail_draw.dfe = 1;
  trail_draw.isbg = 0;
  PutDrawEnv(&trail_draw);
  TILE previous_frame{};
  SetTile(&previous_frame);
  setRGB0(&previous_frame, 255, 0, 255);
  setXY0(&previous_frame, 0, 0);
  setWH(&previous_frame, output_size, output_size);
  DrawPrim(&previous_frame);
  DrawSync(0);

  std::array<unsigned char, 4> previous_color{};
  std::array<GLint, 4> viewport{};
  glGetIntegerv(GL_VIEWPORT, viewport.data());
  glReadPixels(viewport[0] + viewport[2] / 2, viewport[1] + viewport[3] / 2, 1,
               1, GL_RGBA, GL_UNSIGNED_BYTE, previous_color.data());
  if (previous_color[0] < 128U || previous_color[1] > 64U ||
      previous_color[2] < 128U) {
    std::cerr << "Could not seed native framebuffer for scanout trail test; "
              << "rgba=" << static_cast<unsigned int>(previous_color[0]) << ','
              << static_cast<unsigned int>(previous_color[1]) << ','
              << static_cast<unsigned int>(previous_color[2]) << ','
              << static_cast<unsigned int>(previous_color[3]) << '\n';
    PsyX_Shutdown();
    return 12;
  }
  PsyX_EndScene();

  GR_ClearVRAM(0, 0, output_size, output_size, 0, 0, 0);
  GR_UpdateVRAM();
  sf::platform::detail::PsyCrossGuestGpu scanout_gpu;
  scanout_gpu.presentDisplay(0, 0, output_size, output_size, true, false,
                             false);
  DrawSync(0);

  std::array<unsigned char, 4> empty_scanout_color{};
  glGetIntegerv(GL_VIEWPORT, viewport.data());
  glReadPixels(viewport[0] + viewport[2] / 2, viewport[1] + viewport[3] / 2, 1,
               1, GL_RGBA, GL_UNSIGNED_BYTE, empty_scanout_color.data());
  if (empty_scanout_color[0] > 4U || empty_scanout_color[1] > 4U ||
      empty_scanout_color[2] > 4U) {
    std::cerr << "Empty guest scanout retained the previous native frame; "
              << "rgba=" << static_cast<unsigned int>(empty_scanout_color[0])
              << ',' << static_cast<unsigned int>(empty_scanout_color[1]) << ','
              << static_cast<unsigned int>(empty_scanout_color[2]) << ','
              << static_cast<unsigned int>(empty_scanout_color[3]) << '\n';
    PsyX_Shutdown();
    return 13;
  }
  PsyX_EndScene();

  // The raw runtime can present a retained selected-resolution page or fall
  // back to sampling the authoritative logical VRAM. Compare every output pixel
  // for a non-uniform source in both aspect modes: a handful of samples or a
  // solid colour would miss a one-pixel mapping shift between the two paths.
  constexpr int wide_target_width = 160;
  constexpr int wide_target_height = 90;
  constexpr int parity_page_y = 192;
  g_cfg_renderWidth = wide_target_width;
  g_cfg_renderHeight = wide_target_height;
  g_cfg_msaaSamples = 0;

  sf::platform::detail::PsyCrossGuestGpu parity_gpu;
  using ParityFrame = std::vector<unsigned char>;
  struct ParityCapture {
    ParityFrame retained{};
    ParityFrame fallback{};
    std::array<GLint, 4U> retained_viewport{};
    std::array<GLint, 4U> fallback_viewport{};
    GLenum retained_gl_error{GL_NO_ERROR};
    GLenum fallback_gl_error{GL_NO_ERROR};
    bool retained_available{};
    bool fallback_forced{};
  };

  std::array<std::uint16_t, output_size * output_size> parity_pattern{};
  for (int y = 0; y < output_size; ++y) {
    for (int x = 0; x < output_size; ++x) {
      const auto red =
          static_cast<std::uint16_t>(1 + ((x / 3 + y / 11 * 7) % 31));
      const auto green =
          static_cast<std::uint16_t>(1 + ((x / 7 * 5 + y / 5) % 31));
      const auto blue =
          static_cast<std::uint16_t>(1 + ((x / 13 * 3 + y / 3 * 11) % 31));
      parity_pattern[static_cast<std::size_t>(y * output_size + x)] =
          static_cast<std::uint16_t>(red | (green << 5U) | (blue << 10U));
    }
  }
  // The seed draw below must be visually identical to the uploaded texel.
  parity_pattern.front() = 0x001fU;
  RECT16 parity_rect{0, parity_page_y, output_size, output_size};

  const auto clear_native_target = [] {
    const auto scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    if (scissor_enabled != GL_FALSE) {
      glEnable(GL_SCISSOR_TEST);
    }
  };

  const auto capture_parity = [&](int aspect_mode) {
    ParityCapture capture{
        ParityFrame(wide_target_width * wide_target_height * 4U),
        ParityFrame(wide_target_width * wide_target_height * 4U),
    };
    g_cfg_aspectMode = aspect_mode;
    while (glGetError() != GL_NO_ERROR) {
    }

    LoadImage(&parity_rect, reinterpret_cast<u_long *>(parity_pattern.data()));
    GR_UpdateVRAM();
    DRAWENV parity_draw{};
    SetDefDrawEnv(&parity_draw, 0, parity_page_y, output_size, output_size);
    parity_draw.dtd = 0;
    parity_draw.dfe = 0;
    parity_draw.isbg = 0;
    PutDrawEnv(&parity_draw);
    TILE parity_seed{};
    SetTile(&parity_seed);
    setRGB0(&parity_seed, 255, 0, 0);
    setXY0(&parity_seed, 0, 0);
    setWH(&parity_seed, 1, 1);
    DrawPrim(&parity_seed);
    DrawSync(0);
    RECT16 completed_parity{};
    GR_SetOffscreenState(&completed_parity, 0);
    capture.retained_available =
        GR_HasHighResolutionVRAM(0, parity_page_y, output_size, output_size) !=
        0;

    clear_native_target();
    parity_gpu.presentDisplay(0, parity_page_y, output_size, output_size, true,
                              false, false);
    glGetIntegerv(GL_VIEWPORT, capture.retained_viewport.data());
    glReadPixels(0, 0, wide_target_width, wide_target_height, GL_RGBA,
                 GL_UNSIGNED_BYTE, capture.retained.data());
    capture.retained_gl_error = glGetError();
    PsyX_EndScene();

    // Writing the same colour keeps the authoritative source identical. The
    // retained page must stay selected and patch only that logical pixel from
    // packed GPU VRAM; returning to the sprite fallback would reintroduce the
    // whole-frame shimmer this regression is designed to catch.
    GR_ClearVRAM(0, parity_page_y, 1, 1, 255, 0, 0);
    GR_UpdateVRAM();
    capture.fallback_forced =
        GR_HasHighResolutionVRAM(0, parity_page_y, output_size, output_size) !=
        0;
    static_cast<void>(PsyX_BeginScene());
    clear_native_target();
    parity_gpu.presentDisplay(0, parity_page_y, output_size, output_size, true,
                              false, false);
    DrawSync(0);
    glGetIntegerv(GL_VIEWPORT, capture.fallback_viewport.data());
    glReadPixels(0, 0, wide_target_width, wide_target_height, GL_RGBA,
                 GL_UNSIGNED_BYTE, capture.fallback.data());
    capture.fallback_gl_error = glGetError();
    PsyX_EndScene();
    return capture;
  };

  const auto verify_parity = [&](const ParityCapture &capture,
                                 int aspect_mode) {
    const std::array<GLint, 4U> expected_viewport =
        aspect_mode == PSYX_ASPECT_ADAPTIVE
            ? std::array<GLint, 4U>{0, 0, wide_target_width, wide_target_height}
            : std::array<GLint, 4U>{20, 0, 120, wide_target_height};
    std::size_t retained_content_pixels{};
    std::size_t fallback_content_pixels{};
    std::size_t outside_content_pixels{};
    std::size_t retained_transitions{};
    std::size_t decoded_colour_matches{};
    const auto decode_colour = [](const ParityFrame &frame, std::size_t pixel) {
      const auto decode_channel = [](unsigned char channel) {
        return (static_cast<unsigned int>(channel) * 31U + 127U) / 255U;
      };
      return decode_channel(frame[pixel]) |
             (decode_channel(frame[pixel + 1U]) << 5U) |
             (decode_channel(frame[pixel + 2U]) << 10U);
    };
    for (int y = 0; y < wide_target_height; ++y) {
      for (int x = 0; x < wide_target_width; ++x) {
        const auto pixel =
            static_cast<std::size_t>(y * wide_target_width + x) * 4U;
        const auto retained_nonblack = capture.retained[pixel] != 0U ||
                                       capture.retained[pixel + 1U] != 0U ||
                                       capture.retained[pixel + 2U] != 0U;
        const auto fallback_nonblack = capture.fallback[pixel] != 0U ||
                                       capture.fallback[pixel + 1U] != 0U ||
                                       capture.fallback[pixel + 2U] != 0U;
        const auto inside = x >= expected_viewport[0] &&
                            x < expected_viewport[0] + expected_viewport[2] &&
                            y >= expected_viewport[1] &&
                            y < expected_viewport[1] + expected_viewport[3];
        if (inside) {
          retained_content_pixels += retained_nonblack ? 1U : 0U;
          fallback_content_pixels += fallback_nonblack ? 1U : 0U;
          decoded_colour_matches +=
              decode_colour(capture.retained, pixel) ==
                      decode_colour(capture.fallback, pixel)
                  ? 1U
                  : 0U;
          if (x > expected_viewport[0]) {
            const auto previous = pixel - 4U;
            const auto changed =
                capture.retained[pixel] != capture.retained[previous] ||
                capture.retained[pixel + 1U] !=
                    capture.retained[previous + 1U] ||
                capture.retained[pixel + 2U] != capture.retained[previous + 2U];
            retained_transitions += changed ? 1U : 0U;
          }
        } else {
          outside_content_pixels += retained_nonblack ? 1U : 0U;
          outside_content_pixels += fallback_nonblack ? 1U : 0U;
        }
      }
    }
    const auto aperture_pixels =
        static_cast<std::size_t>(expected_viewport[2] * expected_viewport[3]);
    // Both captures must use the same retained source. Decoded-colour parity
    // remains a full-aperture mapping oracle while the dense transition
    // pattern catches any whole-frame shift.
    // The explicit one-word refresh may cover a few destination samples at a
    // rational output scale. Everything else must remain bit-identical in
    // decoded PS1 colour; this is deliberately far stricter than the former
    // retained-vs-sprite 85% tolerance.
    constexpr std::size_t maximum_patched_output_pixels = 8U;
    const auto mapping_stable =
        decoded_colour_matches + maximum_patched_output_pixels >=
        aperture_pixels;
    const auto passed =
        capture.retained_available && capture.fallback_forced &&
        capture.retained_viewport == expected_viewport &&
        capture.fallback_viewport == expected_viewport &&
        capture.retained_gl_error == GL_NO_ERROR &&
        capture.fallback_gl_error == GL_NO_ERROR && mapping_stable &&
        retained_content_pixels == aperture_pixels &&
        fallback_content_pixels == aperture_pixels &&
        outside_content_pixels == 0U && retained_transitions > 128U;
    if (!passed) {
      std::cerr << "Structural retained/fallback scanout parity failed; mode="
                << aspect_mode << " retained=" << capture.retained_available
                << " fallback=" << capture.fallback_forced
                << " viewport=" << capture.retained_viewport[0] << ','
                << capture.retained_viewport[1] << ','
                << capture.retained_viewport[2] << ','
                << capture.retained_viewport[3]
                << " fallback_viewport=" << capture.fallback_viewport[0] << ','
                << capture.fallback_viewport[1] << ','
                << capture.fallback_viewport[2] << ','
                << capture.fallback_viewport[3]
                << " retained_content=" << retained_content_pixels
                << " fallback_content=" << fallback_content_pixels
                << " outside=" << outside_content_pixels
                << " transitions=" << retained_transitions << " gl=0x"
                << std::hex << capture.retained_gl_error << "/0x"
                << capture.fallback_gl_error << std::dec
                << " decoded=" << decoded_colour_matches << '/'
                << aperture_pixels << '\n';
    }
    return passed;
  };

  const auto adaptive_parity = capture_parity(PSYX_ASPECT_ADAPTIVE);
  const auto original_parity = capture_parity(PSYX_ASPECT_ORIGINAL_4_3);
  if (!verify_parity(adaptive_parity, PSYX_ASPECT_ADAPTIVE) ||
      !verify_parity(original_parity, PSYX_ASPECT_ORIGINAL_4_3)) {
    PsyX_Shutdown();
    return 35;
  }

  // This is deliberately a renderer-state test rather than another call to
  // the pure viewport helper. It changes the active guest GP1/DISPENV
  // geometry, re-enters the native renderer and reads the actual GL viewport.
  struct ViewportObservation {
    std::array<GLint, 4U> viewport{};
    GLenum gl_error{GL_NO_ERROR};
    bool geometry_active{};
  };
  const auto observe_renderer_viewport = [](int aspect_mode, int guest_width,
                                            int guest_height) {
    g_cfg_aspectMode = aspect_mode;
    DISPENV guest_display{};
    SetDefDispEnv(&guest_display, 0, 0, guest_width, guest_height);
    PutDispEnv(&guest_display);
    DISPENV active_display{};
    GetDispEnv(&active_display);
    while (glGetError() != GL_NO_ERROR) {
    }
    static_cast<void>(PsyX_BeginScene());
    RECT16 native_target{};
    GR_SetOffscreenState(&native_target, 0);
    ViewportObservation observation{};
    glGetIntegerv(GL_VIEWPORT, observation.viewport.data());
    observation.gl_error = glGetError();
    observation.geometry_active = active_display.disp.w == guest_width &&
                                  active_display.disp.h == guest_height;
    PsyX_EndScene();
    return observation;
  };
  const auto verify_viewport_invariance = [&](int aspect_mode) {
    const std::array<GLint, 4U> expected =
        aspect_mode == PSYX_ASPECT_ADAPTIVE
            ? std::array<GLint, 4U>{0, 0, wide_target_width, wide_target_height}
            : std::array<GLint, 4U>{20, 0, 120, wide_target_height};
    const auto gameplay = observe_renderer_viewport(aspect_mode, 368, 240);
    const auto gp1_reset = observe_renderer_viewport(aspect_mode, 256, 224);
    const auto passed =
        gameplay.geometry_active && gp1_reset.geometry_active &&
        gameplay.viewport == expected && gp1_reset.viewport == expected &&
        gameplay.viewport == gp1_reset.viewport &&
        gameplay.gl_error == GL_NO_ERROR && gp1_reset.gl_error == GL_NO_ERROR;
    if (!passed) {
      std::cerr << "Active DISPENV changed native output aperture; mode="
                << aspect_mode << " gameplay=" << gameplay.viewport[0] << ','
                << gameplay.viewport[1] << ',' << gameplay.viewport[2] << ','
                << gameplay.viewport[3] << " reset=" << gp1_reset.viewport[0]
                << ',' << gp1_reset.viewport[1] << ',' << gp1_reset.viewport[2]
                << ',' << gp1_reset.viewport[3]
                << " active=" << gameplay.geometry_active << '/'
                << gp1_reset.geometry_active << " gl=0x" << std::hex
                << gameplay.gl_error << "/0x" << gp1_reset.gl_error << std::dec
                << '\n';
    }
    return passed;
  };
  if (!verify_viewport_invariance(PSYX_ASPECT_ADAPTIVE) ||
      !verify_viewport_invariance(PSYX_ASPECT_ORIGINAL_4_3)) {
    PsyX_Shutdown();
    return 36;
  }

  // Raw guest SMAA is a final-frame operation: GR_EndScene first closes any
  // pending selected-resolution offscreen page, then post-processes the
  // completed native scanout. Exercise that exact hook once with the hidden
  // test window.
  g_cfg_aspectMode = PSYX_ASPECT_ADAPTIVE;
  g_cfg_smaaFinalFrame = 1;
  while (glGetError() != GL_NO_ERROR) {
  }
  DRAWENV smaa_draw{};
  SetDefDrawEnv(&smaa_draw, 0, 0, wide_target_width, wide_target_height);
  smaa_draw.dtd = 0;
  smaa_draw.dfe = 1;
  smaa_draw.isbg = 0;
  PutDrawEnv(&smaa_draw);
  static_cast<void>(PsyX_BeginScene());
  TILE smaa_tile{};
  SetTile(&smaa_tile);
  setRGB0(&smaa_tile, 255, 255, 255);
  setXY0(&smaa_tile, 7, 9);
  setWH(&smaa_tile, 31, 17);
  DrawPrim(&smaa_tile);
  DrawSync(0);
  PsyX_EndScene();
  const auto smaa_error = glGetError();
  g_cfg_smaaFinalFrame = 0;
  if (smaa_error != GL_NO_ERROR) {
    std::cerr << "Final-frame SMAA failed after raw guest target resolve; gl=0x"
              << std::hex << smaa_error << std::dec << '\n';
    PsyX_Shutdown();
    return 43;
  }

  g_cfg_smaa = 0;
  g_cfg_fxaaFinalFrame = 1;
  while (glGetError() != GL_NO_ERROR) {
  }
  static_cast<void>(PsyX_BeginScene());
  DrawPrim(&smaa_tile);
  DrawSync(0);
  PsyX_EndScene();
  const auto fxaa_error = glGetError();
  g_cfg_fxaaFinalFrame = 0;
  g_cfg_smaa = 1;
  if (fxaa_error != GL_NO_ERROR) {
    std::cerr << "Final-frame FXAA failed after raw guest target resolve; gl=0x"
              << std::hex << fxaa_error << std::dec << '\n';
    PsyX_Shutdown();
    return 96;
  }

  // Production raw guest mode rasterizes the retained root page at the
  // selected target and presents it without a logical-resolution round trip.
  g_cfg_composedGuestScanout = 1;
  g_cfg_renderWidth = wide_target_width;
  g_cfg_renderHeight = wide_target_height;
  g_cfg_aspectMode = PSYX_ASPECT_ADAPTIVE;
  g_cfg_bilinearFiltering = 1;
  g_cfg_smaaFinalFrame = 0;
  const auto baseline_pack_count = GR_GetGuestVRAMPackCount();
  const auto baseline_pack_pixels = GR_GetGuestVRAMPackPixels();
  const auto baseline_seed_count = GR_GetGuestSeedCount();
  const auto baseline_seed_pixels = GR_GetGuestSeedPixels();
  const auto baseline_capture_count = GR_GetGuestCaptureCount();
  const auto baseline_readbacks = GR_GetSynchronousVRAMReadbackCount();

  constexpr int baseline_page_y = 384;
  RECT16 baseline_rect{0, baseline_page_y, output_size, output_size};
  GR_SetGuestDisplayGeometry(output_size, output_size);
  GR_BeginGuestSubmit();
  static_cast<void>(PsyX_BeginScene());
  GR_SetTexture(g_vramTexture, TF_16_BIT, TEXTURE_FILTER_NEAREST);
  GR_SetOffscreenState(&baseline_rect, 1);
  std::array<GLint, 4U> baseline_guest_viewport{};
  glGetIntegerv(GL_VIEWPORT, baseline_guest_viewport.data());
  DRAWENV baseline_draw{};
  SetDefDrawEnv(&baseline_draw, 0, baseline_page_y, output_size, output_size);
  baseline_draw.dtd = 0;
  baseline_draw.dfe = 0;
  baseline_draw.isbg = 0;
  PutDrawEnv(&baseline_draw);
  TILE baseline_tile{};
  SetTile(&baseline_tile);
  setRGB0(&baseline_tile, 255, 0, 0);
  setXY0(&baseline_tile, 0, 0);
  setWH(&baseline_tile, 8, 8);
  DrawPrim(&baseline_tile);
  finishOffscreenPass();

  sf::platform::detail::PsyCrossGuestGpu baseline_gpu;
  baseline_gpu.presentDisplay(0, baseline_page_y, output_size, output_size,
                              true, false, false);
  DrawSync(0);
  std::array<GLint, 4U> baseline_output_viewport{};
  glGetIntegerv(GL_VIEWPORT, baseline_output_viewport.data());
  PsyX_EndScene();

  const auto baseline_valid =
      baseline_guest_viewport ==
          std::array<GLint, 4U>{0, 0, wide_target_width, wide_target_height} &&
      baseline_output_viewport ==
          std::array<GLint, 4U>{0, 0, wide_target_width, wide_target_height} &&
      GR_GetGuestVRAMPackCount() == baseline_pack_count + 1U &&
      GR_GetGuestVRAMPackPixels() ==
          baseline_pack_pixels + wide_target_width * wide_target_height &&
      GR_GetGuestSeedCount() == baseline_seed_count + 1U &&
      GR_GetGuestSeedPixels() ==
          baseline_seed_pixels + wide_target_width * wide_target_height &&
      GR_GetGuestCaptureCount() == baseline_capture_count + 1U &&
      GR_GetSynchronousVRAMReadbackCount() == baseline_readbacks &&
      GR_HasHighResolutionVRAM(0, baseline_page_y, output_size, output_size) ==
          1 &&
      baseline_gpu.highResolutionPresents() == 1U &&
      baseline_gpu.fallbackPresents() == 0U && glGetError() == GL_NO_ERROR;
  if (!baseline_valid) {
    std::cerr << "High-resolution guest route changed raster/presentation; "
              << "guest=" << baseline_guest_viewport[2] << 'x'
              << baseline_guest_viewport[3]
              << " output=" << baseline_output_viewport[2] << 'x'
              << baseline_output_viewport[3]
              << " pack=" << GR_GetGuestVRAMPackCount() - baseline_pack_count
              << " seed=" << GR_GetGuestSeedCount() - baseline_seed_count
              << " capture="
              << GR_GetGuestCaptureCount() - baseline_capture_count
              << " readback="
              << GR_GetSynchronousVRAMReadbackCount() - baseline_readbacks
              << " present=" << baseline_gpu.highResolutionPresents() << '/'
              << baseline_gpu.fallbackPresents() << '\n';
    PsyX_Shutdown();
    return 63;
  }

  // Bilinear presentation must interpolate through the PS1 TPAGE boundary
  // instead of filtering two independently submitted 256-pixel sprites.
  constexpr int seam_width = 320;
  constexpr int seam_height = 4;
  constexpr int seam_output_width = 640;
  constexpr int seam_output_height = 8;
  constexpr int seam_page_y = 320;
  std::array<std::uint16_t, seam_width * seam_height> seam_pattern{};
  for (int row = 0; row < seam_height; ++row) {
    auto *const begin = seam_pattern.data() + row * seam_width;
    std::fill(begin, begin + 256, static_cast<std::uint16_t>(0x001fU));
    std::fill(begin + 256, begin + seam_width,
              static_cast<std::uint16_t>(0x7c00U));
  }
  RECT16 seam_rect{0, seam_page_y, seam_width, seam_height};
  LoadImage(&seam_rect, reinterpret_cast<u_long *>(seam_pattern.data()));
  GR_UpdateVRAM();

  g_cfg_renderWidth = seam_output_width;
  g_cfg_renderHeight = seam_output_height;
  g_cfg_aspectMode = PSYX_ASPECT_ADAPTIVE;
  g_cfg_bilinearFiltering = 1;
  GR_SetGuestDisplayGeometry(seam_width, seam_height);
  while (glGetError() != GL_NO_ERROR) {
  }
  static_cast<void>(PsyX_BeginScene());
  clear_native_target();
  const auto scanout_present_count = GR_GetGuestScanoutPresentCount();
  sf::platform::detail::PsyCrossGuestGpu seam_gpu;
  seam_gpu.presentDisplay(0, seam_page_y, seam_width, seam_height, true, false,
                          false);
  DrawSync(0);

  std::array<GLint, 4U> seam_viewport{};
  glGetIntegerv(GL_VIEWPORT, seam_viewport.data());
  std::array<unsigned char, 4U * 4U> seam_pixels{};
  glReadPixels(510, seam_output_height / 2, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE,
               seam_pixels.data());
  const auto seam_gl_error = glGetError();
  PsyX_EndScene();

  const auto red = [&](std::size_t pixel) { return seam_pixels[pixel * 4U]; };
  const auto green = [&](std::size_t pixel) {
    return seam_pixels[pixel * 4U + 1U];
  };
  const auto blue = [&](std::size_t pixel) {
    return seam_pixels[pixel * 4U + 2U];
  };
  const auto mirrored = [](unsigned char left, unsigned char right) {
    const auto high = std::max(left, right);
    const auto low = std::min(left, right);
    return static_cast<unsigned int>(high - low) <= 2U;
  };
  const auto seam_valid =
      seam_viewport ==
          std::array<GLint, 4U>{0, 0, seam_output_width, seam_output_height} &&
      red(0) >= 248U && blue(0) <= 8U && red(3) <= 8U && blue(3) >= 248U &&
      red(1) >= 176U && red(1) <= 208U && blue(1) >= 48U && blue(1) <= 80U &&
      red(2) >= 48U && red(2) <= 80U && blue(2) >= 176U && blue(2) <= 208U &&
      mirrored(red(1), blue(2)) && mirrored(blue(1), red(2)) &&
      green(0) <= 8U && green(1) <= 8U && green(2) <= 8U && green(3) <= 8U &&
      GR_GetGuestScanoutPresentCount() == scanout_present_count + 1U &&
      seam_gpu.highResolutionPresents() == 0U &&
      seam_gpu.fallbackPresents() == 1U && seam_gl_error == GL_NO_ERROR;
  if (!seam_valid) {
    std::cerr << "Composed scanout did not filter seamlessly across TPAGE "
                 "255/256; viewport="
              << seam_viewport[0] << ',' << seam_viewport[1] << ','
              << seam_viewport[2] << ',' << seam_viewport[3] << " rgba=";
    for (std::size_t pixel = 0; pixel < 4U; ++pixel) {
      std::cerr << (pixel == 0U ? "" : "/")
                << static_cast<unsigned int>(red(pixel)) << ','
                << static_cast<unsigned int>(green(pixel)) << ','
                << static_cast<unsigned int>(blue(pixel));
    }
    std::cerr << " count="
              << GR_GetGuestScanoutPresentCount() - scanout_present_count
              << " present=" << seam_gpu.highResolutionPresents() << '/'
              << seam_gpu.fallbackPresents() << " gl=0x" << std::hex
              << seam_gl_error << std::dec << '\n';
    PsyX_Shutdown();
    return 70;
  }
  // The packed PS1 words remain authoritative for guest reads, while the
  // composed scanout retains the exact RGBA8 colour produced by the raster.
  constexpr int exact_width = 8;
  constexpr int exact_height = 8;
  constexpr int exact_page_y = 448;
  constexpr std::array<unsigned char, 3U> exact_colour{37U, 93U, 171U};
  constexpr std::array<unsigned char, 3U> patch_colour{211U, 47U, 89U};
  using ExactFrame = std::array<unsigned char, exact_width * exact_height * 4U>;
  const auto capture_exact_frame = [] {
    ExactFrame frame{};
    glReadPixels(0, 0, exact_width, exact_height, GL_RGBA, GL_UNSIGNED_BYTE,
                 frame.data());
    return frame;
  };
  const auto exact_pixel_is = [](const ExactFrame &frame, int x, int y,
                                 const std::array<unsigned char, 3U> &colour) {
    const auto pixel = static_cast<std::size_t>(y * exact_width + x) * 4U;
    return frame[pixel] == colour[0] && frame[pixel + 1U] == colour[1] &&
           frame[pixel + 2U] == colour[2];
  };

  g_cfg_renderWidth = exact_width;
  g_cfg_renderHeight = exact_height;
  g_cfg_aspectMode = PSYX_ASPECT_ADAPTIVE;
  g_cfg_bilinearFiltering = 0;
  GR_SetGuestDisplayGeometry(exact_width, exact_height);
  while (glGetError() != GL_NO_ERROR) {
  }

  RECT16 exact_rect{0, exact_page_y, exact_width, exact_height};
  GR_BeginGuestSubmit();
  static_cast<void>(PsyX_BeginScene());
  GR_SetTexture(g_vramTexture, TF_16_BIT, TEXTURE_FILTER_NEAREST);
  GR_SetOffscreenState(&exact_rect, 1);
  DRAWENV exact_draw{};
  SetDefDrawEnv(&exact_draw, 0, exact_page_y, exact_width, exact_height);
  exact_draw.dtd = 0;
  exact_draw.dfe = 0;
  exact_draw.isbg = 0;
  PutDrawEnv(&exact_draw);
  TILE exact_tile{};
  SetTile(&exact_tile);
  setRGB0(&exact_tile, exact_colour[0], exact_colour[1], exact_colour[2]);
  setXY0(&exact_tile, 0, 0);
  setWH(&exact_tile, exact_width, exact_height);
  DrawPrim(&exact_tile);
  DrawSync(0);
  std::array<unsigned char, 4U> exact_raw_pixel{};
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, exact_raw_pixel.data());
  const auto exact_blend_enabled = glIsEnabled(GL_BLEND);
  RECT16 completed_exact{};
  GR_SetOffscreenState(&completed_exact, 0);

  std::array<std::uint16_t, exact_width * exact_height> exact_words{};
  GR_ReadVRAM(exact_words.data(), exact_rect.x, exact_rect.y, exact_rect.w,
              exact_rect.h);
  const auto exact_readbacks_before_present =
      GR_GetSynchronousVRAMReadbackCount();
  clear_native_target();
  sf::platform::detail::PsyCrossGuestGpu exact_gpu;
  exact_gpu.presentDisplay(exact_rect.x, exact_rect.y, exact_rect.w,
                           exact_rect.h, true, false, false);
  DrawSync(0);
  const auto exact_frame = capture_exact_frame();
  const auto exact_gl_error = glGetError();
  const auto exact_readbacks_after_present =
      GR_GetSynchronousVRAMReadbackCount();
  PsyX_EndScene();

  const auto packed_words_exact =
      std::ranges::all_of(exact_words, [](std::uint16_t word) {
        return word == static_cast<std::uint16_t>(0x5564U);
      });
  bool exact_frame_exact = true;
  for (int y = 0; y < exact_height; ++y) {
    for (int x = 0; x < exact_width; ++x)
      exact_frame_exact =
          exact_frame_exact && exact_pixel_is(exact_frame, x, y, exact_colour);
  }
  if (!packed_words_exact || !exact_frame_exact ||
      exact_readbacks_after_present != exact_readbacks_before_present ||
      exact_gl_error != GL_NO_ERROR) {
    std::cerr << "RGBA8 guest colour did not diverge cleanly from RGB555; "
              << "word=0x" << std::hex << exact_words.front() << std::dec
              << " rgba=" << static_cast<unsigned int>(exact_frame[0]) << ','
              << static_cast<unsigned int>(exact_frame[1]) << ','
              << static_cast<unsigned int>(exact_frame[2]) << ','
              << static_cast<unsigned int>(exact_frame[3]) << " readback="
              << exact_readbacks_after_present - exact_readbacks_before_present
              << " gl=0x" << std::hex << exact_gl_error << std::dec << '\n';
    std::cerr << "ordinary-tile code=0x" << std::hex
              << static_cast<unsigned int>(exact_tile.code) << std::dec
              << " blend=" << static_cast<unsigned int>(exact_blend_enabled)
              << " raw-alpha=" << static_cast<unsigned int>(exact_raw_pixel[3])
              << '\n';
    PsyX_Shutdown();
    return 71;
  }

  // Untextured semitransparency averages RGB by a constant half. Its opaque
  // RGBA sentinel must still replace alpha so the packed mask/STP bit is clear.
  g_cfg_aspectMode = PSYX_ASPECT_ORIGINAL_4_3;

  RECT16 average_rect{16, exact_page_y, exact_width, exact_height};
  std::array<std::uint16_t, exact_width * exact_height> average_seed{};
  LoadImage(&average_rect, reinterpret_cast<u_long *>(average_seed.data()));
  GR_UpdateVRAM();
  GR_BeginGuestSubmit();
  static_cast<void>(PsyX_BeginScene());
  GR_SetTexture(g_vramTexture, TF_16_BIT, TEXTURE_FILTER_NEAREST);
  GR_SetOffscreenState(&average_rect, 1);
  DRAWENV average_draw{};
  SetDefDrawEnv(&average_draw, average_rect.x, average_rect.y, exact_width,
                exact_height);
  average_draw.dtd = 0;
  average_draw.dfe = 0;
  average_draw.isbg = 0;
  PutDrawEnv(&average_draw);
  TILE average_tile{};
  SetTile(&average_tile);
  setSemiTrans(&average_tile, 1);
  setRGB0(&average_tile, 248, 0, 0);
  setXY0(&average_tile, 0, 0);
  setWH(&average_tile, exact_width, exact_height);
  DrawPrim(&average_tile);
  DrawSync(0);
  std::array<unsigned char, 4U> average_raw_pixel{};
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, average_raw_pixel.data());
  const auto average_blend_enabled = glIsEnabled(GL_BLEND);
  RECT16 completed_average{};
  GR_SetOffscreenState(&completed_average, 0);
  std::array<std::uint16_t, exact_width * exact_height> average_word{};
  GR_ReadVRAM(average_word.data(), average_rect.x, average_rect.y,
              average_rect.w, average_rect.h);
  const auto average_gl_error = glGetError();
  PsyX_EndScene();

  if (average_tile.code != 0x62U || average_blend_enabled == GL_FALSE ||
      average_raw_pixel[0] != 124U || average_raw_pixel[1] != 0U ||
      average_raw_pixel[2] != 0U || average_raw_pixel[3] != 255U ||
      !std::ranges::all_of(average_word,
                           [](auto word) { return word == 0x000fU; }) ||
      average_gl_error != GL_NO_ERROR) {
    std::cerr << "Untextured BM_AVERAGE lost constant-half RGB or set STP; "
              << "code=0x" << std::hex
              << static_cast<unsigned int>(average_tile.code) << " word=0x"
              << average_word[0] << std::dec
              << " blend=" << static_cast<unsigned int>(average_blend_enabled)
              << " rgba=" << static_cast<unsigned int>(average_raw_pixel[0])
              << ',' << static_cast<unsigned int>(average_raw_pixel[1]) << ','
              << static_cast<unsigned int>(average_raw_pixel[2]) << ','
              << static_cast<unsigned int>(average_raw_pixel[3]) << " gl=0x"
              << std::hex << average_gl_error << std::dec << '\n';
    PsyX_Shutdown();
    return 74;
  }

  // Untextured quarter-add uses the RGB blend constant (0.25), while alpha
  // still replaces the destination and therefore must not create an STP bit.
  RECT16 quarter_rect{32, exact_page_y, exact_width, exact_height};
  std::array<std::uint16_t, exact_width * exact_height> quarter_seed{};
  LoadImage(&quarter_rect, reinterpret_cast<u_long *>(quarter_seed.data()));
  GR_UpdateVRAM();
  GR_BeginGuestSubmit();
  static_cast<void>(PsyX_BeginScene());
  GR_SetTexture(g_vramTexture, TF_16_BIT, TEXTURE_FILTER_NEAREST);
  GR_SetOffscreenState(&quarter_rect, 1);
  DRAWENV quarter_draw{};
  SetDefDrawEnv(&quarter_draw, quarter_rect.x, quarter_rect.y, exact_width,
                exact_height);
  quarter_draw.dtd = 0;
  quarter_draw.dfe = 0;
  quarter_draw.isbg = 0;
  PutDrawEnv(&quarter_draw);
  DR_TPAGE quarter_page{};
  SetDrawTPage(&quarter_page, 0, 0, GetTPage(2, 3, 0, 0));
  DrawPrim(&quarter_page);
  TILE quarter_tile{};
  SetTile(&quarter_tile);
  setSemiTrans(&quarter_tile, 1);
  setRGB0(&quarter_tile, 248, 0, 0);
  setXY0(&quarter_tile, 0, 0);
  setWH(&quarter_tile, exact_width, exact_height);
  DrawPrim(&quarter_tile);
  DrawSync(0);
  std::array<unsigned char, 4U> quarter_raw_pixel{};
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, quarter_raw_pixel.data());
  RECT16 completed_quarter{};
  GR_SetOffscreenState(&completed_quarter, 0);
  std::array<std::uint16_t, exact_width * exact_height> quarter_word{};
  GR_ReadVRAM(quarter_word.data(), quarter_rect.x, quarter_rect.y,
              quarter_rect.w, quarter_rect.h);
  const auto quarter_gl_error = glGetError();
  PsyX_EndScene();
  if (quarter_raw_pixel[0] != 62U || quarter_raw_pixel[1] != 0U ||
      quarter_raw_pixel[2] != 0U || quarter_raw_pixel[3] != 255U ||
      !std::ranges::all_of(quarter_word,
                           [](auto word) { return word == 0x0007U; }) ||
      quarter_gl_error != GL_NO_ERROR) {
    std::cerr << "Untextured BM_ADD_QUATER_SOURCE used the wrong factor or "
                 "set STP; word=0x"
              << std::hex << quarter_word[0] << std::dec
              << " rgba=" << static_cast<unsigned int>(quarter_raw_pixel[0])
              << ',' << static_cast<unsigned int>(quarter_raw_pixel[1]) << ','
              << static_cast<unsigned int>(quarter_raw_pixel[2]) << ','
              << static_cast<unsigned int>(quarter_raw_pixel[3]) << " gl=0x"
              << std::hex << quarter_gl_error << std::dec << '\n';
    PsyX_Shutdown();
    return 79;
  }

  // Reopening the same page must seed from RGBA8. A contained patch may change
  // its four pixels, but every untouched pixel must retain all eight bits.
  GR_BeginGuestSubmit();
  g_cfg_aspectMode = PSYX_ASPECT_ADAPTIVE;

  static_cast<void>(PsyX_BeginScene());
  GR_SetTexture(g_vramTexture, TF_16_BIT, TEXTURE_FILTER_NEAREST);
  GR_SetOffscreenState(&exact_rect, 1);
  PutDrawEnv(&exact_draw);
  TILE exact_patch{};
  SetTile(&exact_patch);
  setRGB0(&exact_patch, patch_colour[0], patch_colour[1], patch_colour[2]);
  setXY0(&exact_patch, 2, 3);
  setWH(&exact_patch, 2, 2);
  DrawPrim(&exact_patch);
  finishOffscreenPass();
  const auto patch_readbacks_before_present =
      GR_GetSynchronousVRAMReadbackCount();
  clear_native_target();
  exact_gpu.presentDisplay(exact_rect.x, exact_rect.y, exact_rect.w,
                           exact_rect.h, true, false, false);
  DrawSync(0);
  const auto patched_frame = capture_exact_frame();
  const auto patch_gl_error = glGetError();
  const auto patch_readbacks_after_present =
      GR_GetSynchronousVRAMReadbackCount();
  PsyX_EndScene();

  bool patch_frame_exact = true;
  for (int y = 0; y < exact_height; ++y) {
    for (int x = 0; x < exact_width; ++x) {
      const int guest_y = exact_height - 1 - y;
      const bool inside_patch = x >= 2 && x < 4 && guest_y >= 3 && guest_y < 5;
      patch_frame_exact =
          patch_frame_exact &&
          exact_pixel_is(patched_frame, x, y,
                         inside_patch ? patch_colour : exact_colour);
    }
  }
  if (!patch_frame_exact ||
      patch_readbacks_after_present != patch_readbacks_before_present ||
      patch_gl_error != GL_NO_ERROR) {
    std::cerr << "RGBA8 page reseed quantized untouched pixels; readback="
              << patch_readbacks_after_present - patch_readbacks_before_present
              << " gl=0x" << std::hex << patch_gl_error << std::dec << '\n';
    PsyX_Shutdown();
    return 72;
  }

  // A packed LoadImage invalidates only its guest pixel. Presentation lazily
  // decodes that pixel with PS1 orientation and leaves every exact host pixel.
  constexpr int upload_x = 6;
  constexpr int upload_y = 1;
  RECT16 upload_rect{static_cast<short>(exact_rect.x + upload_x),
                     static_cast<short>(exact_rect.y + upload_y), 1, 1};
  std::array<std::uint16_t, 2U> upload_words{0x801fU, 0U};
  LoadImage(&upload_rect, reinterpret_cast<u_long *>(upload_words.data()));
  const auto upload_readbacks_before_present =
      GR_GetSynchronousVRAMReadbackCount();
  static_cast<void>(PsyX_BeginScene());
  clear_native_target();
  exact_gpu.presentDisplay(exact_rect.x, exact_rect.y, exact_rect.w,
                           exact_rect.h, true, false, false);
  DrawSync(0);
  const auto uploaded_frame = capture_exact_frame();
  const auto upload_gl_error = glGetError();
  const auto upload_readbacks_after_present =
      GR_GetSynchronousVRAMReadbackCount();
  PsyX_EndScene();

  const int upload_native_y = exact_height - 1 - upload_y;
  std::size_t changed_pixels{};
  bool upload_frame_exact = true;
  constexpr std::array<unsigned char, 3U> upload_colour{255U, 0U, 0U};
  for (int y = 0; y < exact_height; ++y) {
    for (int x = 0; x < exact_width; ++x) {
      const auto pixel = static_cast<std::size_t>(y * exact_width + x) * 4U;
      const bool changed =
          uploaded_frame[pixel] != patched_frame[pixel] ||
          uploaded_frame[pixel + 1U] != patched_frame[pixel + 1U] ||
          uploaded_frame[pixel + 2U] != patched_frame[pixel + 2U];
      changed_pixels += changed ? 1U : 0U;
      upload_frame_exact =
          upload_frame_exact &&
          (x == upload_x && y == upload_native_y
               ? exact_pixel_is(uploaded_frame, x, y, upload_colour)
               : !changed);
    }
  }
  if (!upload_frame_exact || changed_pixels != 1U ||
      upload_readbacks_after_present != upload_readbacks_before_present ||
      upload_gl_error != GL_NO_ERROR) {
    const auto upload_pixel =
        static_cast<std::size_t>(upload_native_y * exact_width + upload_x) * 4U;
    std::cerr << "LoadImage dirtied the wrong RGBA8 scanout pixels; changed="
              << changed_pixels << " rgba="
              << static_cast<unsigned int>(uploaded_frame[upload_pixel]) << ','
              << static_cast<unsigned int>(uploaded_frame[upload_pixel + 1U])
              << ','
              << static_cast<unsigned int>(uploaded_frame[upload_pixel + 2U])
              << " readback="
              << upload_readbacks_after_present -
                     upload_readbacks_before_present
              << " gl=0x" << std::hex << upload_gl_error << std::dec << '\n';
    PsyX_Shutdown();
    return 73;
  }

  // Resizing the selected target must only change the final presentation
  // scale; the retained logical RGBA8 page is not redrawn or requantized.
  constexpr int resized_width = exact_width * 2;
  constexpr int resized_height = exact_height * 2;
  g_cfg_renderWidth = resized_width;
  g_cfg_renderHeight = resized_height;
  GR_SetGuestDisplayGeometry(exact_width, exact_height);
  const auto resize_readbacks_before = GR_GetSynchronousVRAMReadbackCount();
  static_cast<void>(PsyX_BeginScene());
  clear_native_target();
  exact_gpu.presentDisplay(exact_rect.x, exact_rect.y, exact_rect.w,
                           exact_rect.h, true, false, false);
  DrawSync(0);
  std::array<unsigned char, resized_width * resized_height * 4U>
      resized_frame{};
  glReadPixels(0, 0, resized_width, resized_height, GL_RGBA, GL_UNSIGNED_BYTE,
               resized_frame.data());
  const auto resize_gl_error = glGetError();
  const auto resize_readbacks_after = GR_GetSynchronousVRAMReadbackCount();
  PsyX_EndScene();
  bool resize_exact = true;
  for (int y = 0; y < resized_height; ++y) {
    for (int x = 0; x < resized_width; ++x) {
      const auto destination =
          static_cast<std::size_t>(y * resized_width + x) * 4U;
      const auto source =
          static_cast<std::size_t>((y / 2) * exact_width + x / 2) * 4U;
      resize_exact =
          resize_exact && std::equal(uploaded_frame.data() + source,
                                     uploaded_frame.data() + source + 3U,
                                     resized_frame.data() + destination);
    }
  }
  if (!resize_exact || resize_readbacks_after != resize_readbacks_before ||
      resize_gl_error != GL_NO_ERROR) {
    std::cerr << "Resolution change requantized retained RGBA8 scanout; "
              << "readback=" << resize_readbacks_after - resize_readbacks_before
              << " gl=0x" << std::hex << resize_gl_error << std::dec << '\n';
    PsyX_Shutdown();
    return 75;
  }

  // Resolve a display rectangle crossing both VRAM axes. Distinct red levels
  // lock X order and the guest-top/native-bottom Y orientation simultaneously.
  constexpr int wrap_size = 4;
  constexpr int wrap_x = VRAM_WIDTH - 2;
  constexpr int wrap_y = VRAM_HEIGHT - 2;
  for (int y = 0; y < wrap_size; ++y) {
    for (int x = 0; x < wrap_size; ++x) {
      RECT16 pixel_rect{static_cast<short>((wrap_x + x) % VRAM_WIDTH),
                        static_cast<short>((wrap_y + y) % VRAM_HEIGHT), 1, 1};
      std::array<std::uint16_t, 2U> pixel_word{
          static_cast<std::uint16_t>(1 + x + y * wrap_size), 0U};
      LoadImage(&pixel_rect, reinterpret_cast<u_long *>(pixel_word.data()));
    }
  }
  g_cfg_renderWidth = wrap_size;
  g_cfg_renderHeight = wrap_size;
  GR_SetGuestDisplayGeometry(wrap_size, wrap_size);
  const auto wrap_readbacks_before = GR_GetSynchronousVRAMReadbackCount();
  static_cast<void>(PsyX_BeginScene());
  clear_native_target();
  exact_gpu.presentDisplay(wrap_x, wrap_y, wrap_size, wrap_size, true, false,
                           false);
  DrawSync(0);
  std::array<unsigned char, wrap_size * wrap_size * 4U> wrap_frame{};
  glReadPixels(0, 0, wrap_size, wrap_size, GL_RGBA, GL_UNSIGNED_BYTE,
               wrap_frame.data());
  const auto wrap_gl_error = glGetError();
  const auto wrap_readbacks_after = GR_GetSynchronousVRAMReadbackCount();
  PsyX_EndScene();
  bool wrap_exact = true;
  for (int y = 0; y < wrap_size; ++y) {
    for (int x = 0; x < wrap_size; ++x) {
      const auto pixel = static_cast<std::size_t>(y * wrap_size + x) * 4U;
      const auto guest_y = wrap_size - 1 - y;
      const auto expected =
          GR_Expand5BitColor(static_cast<u_char>(1 + x + guest_y * wrap_size));
      wrap_exact = wrap_exact && wrap_frame[pixel] == expected &&
                   wrap_frame[pixel + 1U] == 0U && wrap_frame[pixel + 2U] == 0U;
    }
  }
  if (!wrap_exact || wrap_readbacks_after != wrap_readbacks_before ||
      wrap_gl_error != GL_NO_ERROR) {
    std::cerr << "Wrapped X/Y RGBA8 scanout changed order or orientation; "
              << "readback=" << wrap_readbacks_after - wrap_readbacks_before
              << " gl=0x" << std::hex << wrap_gl_error << std::dec << '\n';
    PsyX_Shutdown();
    return 76;
  }

  // MoveImage is a packed-word operation. It quantizes the destination while
  // the exact source sidecar remains untouched.
  RECT16 move_source{32, exact_page_y, 2, 2};
  RECT16 move_destination{40, exact_page_y, 2, 2};
  std::array<std::uint16_t, 4U> move_destination_seed{};
  LoadImage(&move_destination,
            reinterpret_cast<u_long *>(move_destination_seed.data()));
  GR_UpdateVRAM();
  g_cfg_renderWidth = move_source.w;
  g_cfg_renderHeight = move_source.h;
  GR_SetGuestDisplayGeometry(move_source.w, move_source.h);
  GR_BeginGuestSubmit();
  static_cast<void>(PsyX_BeginScene());
  GR_SetTexture(g_vramTexture, TF_16_BIT, TEXTURE_FILTER_NEAREST);
  GR_SetOffscreenState(&move_source, 1);
  DRAWENV move_draw{};
  SetDefDrawEnv(&move_draw, move_source.x, move_source.y, move_source.w,
                move_source.h);
  move_draw.dtd = 0;
  move_draw.dfe = 0;
  move_draw.isbg = 0;
  PutDrawEnv(&move_draw);
  TILE move_tile{};
  SetTile(&move_tile);
  setRGB0(&move_tile, exact_colour[0], exact_colour[1], exact_colour[2]);
  setXY0(&move_tile, 0, 0);
  setWH(&move_tile, move_source.w, move_source.h);
  DrawPrim(&move_tile);
  finishOffscreenPass();
  const auto move_readbacks_before = GR_GetSynchronousVRAMReadbackCount();
  MoveImage(&move_source, move_destination.x, move_destination.y);
  clear_native_target();
  exact_gpu.presentDisplay(move_source.x, move_source.y, move_source.w,
                           move_source.h, true, false, false);
  DrawSync(0);
  std::array<unsigned char, 2U * 2U * 4U> move_source_frame{};
  glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, move_source_frame.data());
  clear_native_target();
  exact_gpu.presentDisplay(move_destination.x, move_destination.y,
                           move_destination.w, move_destination.h, true, false,
                           false);
  DrawSync(0);
  std::array<unsigned char, 2U * 2U * 4U> move_destination_frame{};
  glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE,
               move_destination_frame.data());
  const auto move_gl_error = glGetError();
  const auto move_readbacks_after = GR_GetSynchronousVRAMReadbackCount();
  PsyX_EndScene();
  const std::array<unsigned char, 3U> quantized_colour{
      GR_Expand5BitColor(4U), GR_Expand5BitColor(11U), GR_Expand5BitColor(21U)};
  bool move_exact = true;
  for (std::size_t pixel = 0; pixel < 4U; ++pixel) {
    const auto offset = pixel * 4U;
    move_exact = move_exact &&
                 std::equal(exact_colour.begin(), exact_colour.end(),
                            move_source_frame.data() + offset) &&
                 std::equal(quantized_colour.begin(), quantized_colour.end(),
                            move_destination_frame.data() + offset);
  }
  if (!move_exact || move_readbacks_after != move_readbacks_before ||
      move_gl_error != GL_NO_ERROR) {
    std::cerr
        << "MoveImage quantized source RGBA8 or retained exact destination; "
        << "readback=" << move_readbacks_after - move_readbacks_before
        << " gl=0x" << std::hex << move_gl_error << std::dec << '\n';
    PsyX_Shutdown();
    return 77;
  }

  // More writes than the legacy journal capacity must not force a full sidecar
  // decode and quantize an unrelated exact page.
  std::array<std::uint16_t, 2U> sparse_word{0x03e0U, 0U};
  for (int write = 0; write < 513; ++write) {
    RECT16 sparse_rect{static_cast<short>(100 + (write % 257) * 3),
                       static_cast<short>(100 + (write / 257) * 3), 1, 1};
    LoadImage(&sparse_rect, reinterpret_cast<u_long *>(sparse_word.data()));
  }
  const auto sparse_readbacks_before = GR_GetSynchronousVRAMReadbackCount();
  static_cast<void>(PsyX_BeginScene());
  clear_native_target();
  exact_gpu.presentDisplay(move_source.x, move_source.y, move_source.w,
                           move_source.h, true, false, false);
  DrawSync(0);
  std::array<unsigned char, 2U * 2U * 4U> sparse_frame{};
  glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, sparse_frame.data());
  const auto sparse_gl_error = glGetError();
  const auto sparse_readbacks_after = GR_GetSynchronousVRAMReadbackCount();
  PsyX_EndScene();
  bool sparse_exact = true;
  for (std::size_t pixel = 0; pixel < 4U; ++pixel) {
    sparse_exact =
        sparse_exact && std::equal(exact_colour.begin(), exact_colour.end(),
                                   sparse_frame.data() + pixel * 4U);
  }
  if (!sparse_exact || sparse_readbacks_after != sparse_readbacks_before ||
      sparse_gl_error != GL_NO_ERROR) {
    std::cerr << "Sparse dirty overflow requantized untouched RGBA8; readback="
              << sparse_readbacks_after - sparse_readbacks_before << " gl=0x"
              << std::hex << sparse_gl_error << std::dec << '\n';
    PsyX_Shutdown();
    return 78;
  }

  // Capture happens after geometry policy resolution and owns every sidecar.
  // Presentation promotes the completed page before the following submit.
  sf::platform::detail::PsyCrossGuestGpu replay_capture_gpu;
  replay_capture_gpu.setPresentationInterpolationEnabled(true);
  constexpr std::uint16_t capture_x = 320U;
  constexpr std::uint16_t capture_y = 256U;
  constexpr std::uint16_t capture_size = 16U;
  std::vector<std::uint32_t> capture_words{
      (0xe3000000U | capture_x |
       (static_cast<std::uint32_t>(capture_y) * 0x400U)),
      (0xe4000000U | (capture_x + capture_size - 1U) |
       (static_cast<std::uint32_t>(capture_y + capture_size - 1U) * 0x400U)),
      0xe5000000U,
      0x02000000U,
      static_cast<std::uint32_t>(capture_x) |
          (static_cast<std::uint32_t>(capture_y) << 16U),
      static_cast<std::uint32_t>(capture_size) |
          (static_cast<std::uint32_t>(capture_size) << 16U),
      0x200000f8U,
      (static_cast<std::uint32_t>(capture_x + 1U) |
       (static_cast<std::uint32_t>(capture_y + 1U) * 0x10000U)),
      (static_cast<std::uint32_t>(capture_x + 12U) |
       (static_cast<std::uint32_t>(capture_y + 1U) * 0x10000U)),
      (static_cast<std::uint32_t>(capture_x + 1U) |
       (static_cast<std::uint32_t>(capture_y + 12U) * 0x10000U)),
  };
  std::vector<sf::psx::GteProjectedVertex> capture_projections(
      capture_words.size());
  for (const auto word : {7U, 8U, 9U}) {
    auto &projection = capture_projections[word];
    projection.packed_sxy = capture_words[word];
    const auto screen_x = static_cast<float>(
        static_cast<std::int16_t>(capture_words[word] & 0xffffU));
    const auto screen_y = static_cast<float>(
        static_cast<std::int16_t>(capture_words[word] >> 16U));
    projection.view_x = screen_x * 1000.0F / 320.0F;
    projection.view_y = screen_y * 1000.0F / 320.0F;
    projection.view_z = 1000.0F;
    projection.projective_depth = 1000.0F;
    projection.screen_x = screen_x;
    projection.screen_y = screen_y;
    projection.screen_h = 320.0F;
    projection.valid = true;
  }
  std::vector<sf::psx::GpuDmaWordSource> capture_dma(capture_words.size());
  for (std::size_t word{}; word < capture_dma.size(); ++word) {
    capture_dma[word] = {static_cast<std::uint32_t>(0x00010000U + word * 4U),
                         0x00010000U, sf::psx::GpuDmaSourceKind::linked_list};
  }
  const auto expected_capture_words = capture_words;
  const auto expected_capture_dma = std::vector<sf::psx::GpuDmaWordSource>{
      capture_dma.begin() + 6, capture_dma.end()};
  replay_capture_gpu.submit(capture_words, capture_projections, {}, 0U, {},
                            capture_dma);
  capture_words.assign(capture_words.size(), 0U);
  capture_projections.assign(capture_projections.size(), {});
  capture_dma.assign(capture_dma.size(), {});
  if (replay_capture_gpu.presentationReplayReady() ||
      replay_capture_gpu.currentPresentationReplayFrame().generation != 0U ||
      replay_capture_gpu.capturedPresentationReplayEvents() != 2U) {
    std::cerr << "Replay capture published before display promotion\n";
    PsyX_Shutdown();
    return 206;
  }
  replay_capture_gpu.presentDisplay(capture_x, capture_y, capture_size,
                                    capture_size, true, false, false);
  const auto &captured_frame =
      replay_capture_gpu.currentPresentationReplayFrame();
  const auto captured_generation = captured_frame.generation;
  const auto *captured_page =
      captured_frame.pages.size() == 1U ? &captured_frame.pages[0] : nullptr;
  const sf::platform::detail::PresentationReplayDrawEvent *captured_event{};
  if (captured_page != nullptr) {
    const auto found = std::ranges::find(
        captured_page->events,
        sf::platform::detail::PresentationReplayEventKind::draw,
        &sf::platform::detail::PresentationReplayDrawEvent::kind);
    if (found != captured_page->events.end()) {
      captured_event = &*found;
    }
  }
  const auto capture_owned =
      captured_event != nullptr && captured_page->events.size() == 2U &&
      std::ranges::equal(
          captured_page->commandWords(*captured_event),
          std::span<const std::uint32_t>{expected_capture_words}.subspan(6U)) &&
      std::ranges::equal(captured_page->dmaSources(*captured_event),
                         expected_capture_dma) &&
      captured_page->resolvedProjections(*captured_event).size() == 4U &&
      captured_page->target ==
          sf::platform::detail::PresentationReplayDrawTarget{
              capture_x, capture_y, capture_size, capture_size};
  if (replay_capture_gpu.presentationReplayReady() || !capture_owned ||
      replay_capture_gpu.promotedPresentationReplayFrames() != 1U) {
    std::cerr << "Resolved replay event was not promoted as owning data; ready="
              << replay_capture_gpu.presentationReplayReady()
              << " generation=" << captured_frame.generation
              << " pages=" << captured_frame.pages.size()
              << " unsafe=" << captured_frame.contains_vram_commands;
    if (!captured_frame.pages.empty()) {
      std::cerr << " events=" << captured_frame.pages[0].events.size()
                << " target=" << captured_frame.pages[0].target.x << ','
                << captured_frame.pages[0].target.y << ','
                << captured_frame.pages[0].target.width << ','
                << captured_frame.pages[0].target.height;
    }
    std::cerr << '\n';
    PsyX_Shutdown();
    return 207;
  }

  std::vector<std::uint32_t> second_capture{
      (0xe3000000U | capture_x |
       (static_cast<std::uint32_t>(capture_y) * 0x400U)),
      (0xe4000000U | (capture_x + capture_size - 1U) |
       (static_cast<std::uint32_t>(capture_y + capture_size - 1U) * 0x400U)),
      0xe5000000U,
      0x02000000U,
      static_cast<std::uint32_t>(capture_x) |
          (static_cast<std::uint32_t>(capture_y) << 16U),
      static_cast<std::uint32_t>(capture_size) |
          (static_cast<std::uint32_t>(capture_size) << 16U),
      0x200000f8U,
      (static_cast<std::uint32_t>(capture_x + 2U) |
       (static_cast<std::uint32_t>(capture_y + 2U) * 0x10000U)),
      (static_cast<std::uint32_t>(capture_x + 13U) |
       (static_cast<std::uint32_t>(capture_y + 2U) * 0x10000U)),
      (static_cast<std::uint32_t>(capture_x + 2U) |
       (static_cast<std::uint32_t>(capture_y + 13U) * 0x10000U)),
      0x60000040U,
      static_cast<std::uint32_t>(capture_x + 4U) |
          (static_cast<std::uint32_t>(capture_y + 4U) * 0x10000U),
      2U | (2U << 16U),
  };
  std::vector<sf::psx::GteProjectedVertex> second_projections(
      second_capture.size());
  for (const auto word : {7U, 8U, 9U}) {
    auto &projection = second_projections[word];
    projection.packed_sxy = second_capture[word];
    const auto screen_x = static_cast<float>(
        static_cast<std::int16_t>(second_capture[word] & 0xffffU));
    const auto screen_y = static_cast<float>(
        static_cast<std::int16_t>(second_capture[word] >> 16U));
    projection.view_x = screen_x * 1000.0F / 320.0F;
    projection.view_y = screen_y * 1000.0F / 320.0F;
    projection.view_z = 1000.0F;
    projection.projective_depth = 1000.0F;
    projection.screen_x = screen_x;
    projection.screen_y = screen_y;
    projection.screen_h = 320.0F;
    projection.valid = true;
  }
  std::vector<sf::psx::GpuDmaWordSource> second_dma(second_capture.size());
  std::ranges::copy(expected_capture_dma,
                    second_dma.begin() + static_cast<std::ptrdiff_t>(6U));
  replay_capture_gpu.submit(second_capture, second_projections, {}, 0U, {},
                            second_dma);
  replay_capture_gpu.presentDisplay(capture_x, capture_y, capture_size,
                                    capture_size, true, false, false);
  if (replay_capture_gpu.previousPresentationReplayFrame().generation !=
          captured_generation ||
      replay_capture_gpu.currentPresentationReplayFrame().generation ==
          replay_capture_gpu.previousPresentationReplayFrame().generation ||
      !replay_capture_gpu.presentationReplayReady()) {
    std::cerr << "Replay history did not rotate previous/current frames\n";
    PsyX_Shutdown();
    return 208;
  }

  if (replay_capture_gpu.presentInterpolatedDisplay(1.0F)) {
    std::cerr << "Replay accepted current-frame alpha endpoint\n";
    PsyX_Shutdown();
    return 210;
  }
  const auto replay_write_before = GR_GetVRAMWriteSequence();
  const auto replay_pack_before = GR_GetGuestVRAMPackCount();
  const auto replay_capture_before = GR_GetGuestCaptureCount();
  const auto replay_readback_before = GR_GetSynchronousVRAMReadbackCount();
  if (!replay_capture_gpu.presentInterpolatedDisplay(0.5F) ||
      replay_capture_gpu.interpolatedPresentationReplayFrames() != 1U ||
      replay_capture_gpu.replayedPresentationStateCommands() != 6U ||
      GR_GetVRAMWriteSequence() != replay_write_before ||
      GR_GetGuestVRAMPackCount() != replay_pack_before ||
      GR_GetGuestCaptureCount() != replay_capture_before ||
      GR_GetSynchronousVRAMReadbackCount() != replay_readback_before) {
    std::cerr << "Interpolated replay failed or mutated authoritative VRAM\n";
    PsyX_Shutdown();
    return 211;
  }

  auto cut_capture = second_capture;
  auto cut_projections = second_projections;
  for (const auto word : {7U, 8U, 9U}) {
    cut_capture[word] += 3U;
    auto &projection = cut_projections[word];
    projection.packed_sxy = cut_capture[word];
    projection.screen_x += 3.0F;
    projection.view_x =
        projection.screen_x * projection.view_z / projection.screen_h;
  }
  replay_capture_gpu.submit(cut_capture, cut_projections, {}, 0U, {},
                            second_dma);
  replay_capture_gpu.presentDisplay(capture_x, capture_y, capture_size,
                                    capture_size, true, false, false);
  if (replay_capture_gpu.presentationReplayReady() ||
      replay_capture_gpu.presentInterpolatedDisplay(0.5F) ||
      replay_capture_gpu.interpolatedPresentationReplayFrames() != 1U) {
    std::cerr << "Global camera-cut guard partially interpolated a frame\n";
    PsyX_Shutdown();
    return 212;
  }

  constexpr std::array unsafe_vram_command{
      0x02000000U,
      static_cast<std::uint32_t>(capture_x) |
          (static_cast<std::uint32_t>(capture_y) << 16U),
      1U | (1U << 16U)};
  replay_capture_gpu.submit(unsafe_vram_command);
  replay_capture_gpu.presentDisplay(capture_x, capture_y, capture_size,
                                    capture_size, true, false, false);
  if (replay_capture_gpu.presentationReplayReady() ||
      !replay_capture_gpu.currentPresentationReplayFrame()
           .contains_vram_commands ||
      replay_capture_gpu.skippedPresentationReplayVramCommands() != 1U) {
    std::cerr << "VRAM command entered draw-only replay history\n";
    PsyX_Shutdown();
    return 209;
  }
  DrawSync(0);
  PsyX_EndScene();

  // Presentation replay owns a fresh native-resolution scratch target. Four
  // repeats of the same semitransparent primitive must be pixel-identical and
  // must not touch any authoritative guest storage or retained-page counters.
  constexpr int replay_size = 8;
  RECT16 replay_rect{64, 448, replay_size, replay_size};
  std::array<std::uint16_t, replay_size * replay_size> replay_seed{};
  for (std::size_t pixel{}; pixel < replay_seed.size(); ++pixel) {
    replay_seed[pixel] = static_cast<std::uint16_t>((1U + pixel % 31U) |
                                                    ((pixel * 3U % 31U) << 5U));
  }
  LoadImage(&replay_rect, reinterpret_cast<u_long *>(replay_seed.data()));
  GR_UpdateVRAM();
  std::array<std::uint16_t, replay_size * replay_size> replay_vram_before{};
  GR_ReadVRAM(replay_vram_before.data(), replay_rect.x, replay_rect.y,
              replay_rect.w, replay_rect.h);
  const auto replay_write_sequence = GR_GetVRAMWriteSequence();
  const auto replay_pack_count = GR_GetGuestVRAMPackCount();
  const auto replay_seed_count = GR_GetGuestSeedCount();
  const auto replay_capture_count = GR_GetGuestCaptureCount();
  const auto replay_readback_count = GR_GetSynchronousVRAMReadbackCount();

  g_cfg_renderWidth = replay_size;
  g_cfg_renderHeight = replay_size;
  g_cfg_aspectMode = PSYX_ASPECT_ADAPTIVE;
  g_cfg_bilinearFiltering = 0;
  GR_SetGuestDisplayGeometry(replay_size, replay_size);
  DISPENV replay_display{};
  SetDefDispEnv(&replay_display, 0, 0, replay_size, replay_size);
  PutDispEnv(&replay_display);

  using ReplayFrame = std::array<unsigned char, replay_size * replay_size * 4U>;
  std::array<ReplayFrame, 4U> replay_frames{};
  auto replay_ok = true;
  for (std::size_t repeat{}; repeat < replay_frames.size(); ++repeat) {
    static_cast<void>(PsyX_BeginScene());
    replay_ok = replay_ok && GR_BeginGuestPresentationReplay(&replay_rect) != 0;
    DRAWENV replay_draw{};
    SetDefDrawEnv(&replay_draw, replay_rect.x, replay_rect.y, replay_rect.w,
                  replay_rect.h);
    replay_draw.dtd = 0;
    replay_draw.dfe = 0;
    replay_draw.isbg = 0;
    PutDrawEnv(&replay_draw);
    replay_ok = replay_ok && GR_ClearGuestPresentationReplay(0U, 0U, 0U) != 0;
    TILE replay_tile{};
    SetTile(&replay_tile);
    setSemiTrans(&replay_tile, 1);
    setRGB0(&replay_tile, 248, 0, 0);
    setXY0(&replay_tile, 0, 0);
    setWH(&replay_tile, replay_size, replay_size);
    DrawPrim(&replay_tile);
    replay_ok = replay_ok && GR_EndGuestPresentationReplay() != 0;
    replay_ok = replay_ok && GR_PresentGuestPresentationReplay(
                                 replay_rect.x, replay_rect.y, replay_rect.w,
                                 replay_rect.h) != 0;
    glReadPixels(0, 0, replay_size, replay_size, GL_RGBA, GL_UNSIGNED_BYTE,
                 replay_frames[repeat].data());
    PsyX_EndScene();
  }

  std::array<std::uint16_t, replay_size * replay_size> replay_vram_after{};
  GR_ReadVRAM(replay_vram_after.data(), replay_rect.x, replay_rect.y,
              replay_rect.w, replay_rect.h);
  const auto replay_pixels_stable =
      std::ranges::all_of(replay_frames, [&](const auto &frame) {
        return frame == replay_frames[0];
      });
  const auto replay_nonempty = replay_frames[0][0] != 0U ||
                               replay_frames[0][1] != 0U ||
                               replay_frames[0][2] != 0U;
  const auto replay_gl_error = glGetError();
  if (!replay_ok || !replay_pixels_stable || !replay_nonempty ||
      replay_vram_after != replay_vram_before ||
      GR_GetVRAMWriteSequence() != replay_write_sequence ||
      GR_GetGuestVRAMPackCount() != replay_pack_count ||
      GR_GetGuestSeedCount() != replay_seed_count ||
      GR_GetGuestCaptureCount() != replay_capture_count ||
      GR_GetSynchronousVRAMReadbackCount() != replay_readback_count ||
      replay_gl_error != GL_NO_ERROR) {
    std::cerr << "Presentation replay mutated guest state or accumulated "
                 "semitransparency; write="
              << GR_GetVRAMWriteSequence() - replay_write_sequence
              << " pack=" << GR_GetGuestVRAMPackCount() - replay_pack_count
              << " seed=" << GR_GetGuestSeedCount() - replay_seed_count
              << " capture=" << GR_GetGuestCaptureCount() - replay_capture_count
              << " readback="
              << GR_GetSynchronousVRAMReadbackCount() - replay_readback_count
              << " rgba=" << static_cast<unsigned int>(replay_frames[0][0])
              << ',' << static_cast<unsigned int>(replay_frames[0][1]) << ','
              << static_cast<unsigned int>(replay_frames[0][2]) << " gl=0x"
              << std::hex << replay_gl_error << std::dec << '\n';
    PsyX_Shutdown();
    return 204;
  }

  // A draw-area switch during replay stays inside scratch but poisons the
  // completed image. It must never be presentable or fall through to VRAM.
  static_cast<void>(PsyX_BeginScene());
  const auto fail_closed_begin =
      GR_BeginGuestPresentationReplay(&replay_rect) != 0;
  DRAWENV mismatched_replay_draw{};
  SetDefDrawEnv(&mismatched_replay_draw, replay_rect.x + 1, replay_rect.y,
                replay_rect.w - 1, replay_rect.h);
  mismatched_replay_draw.dtd = 0;
  mismatched_replay_draw.dfe = 0;
  mismatched_replay_draw.isbg = 0;
  PutDrawEnv(&mismatched_replay_draw);
  TILE mismatched_replay_tile{};
  SetTile(&mismatched_replay_tile);
  setRGB0(&mismatched_replay_tile, 0, 248, 0);
  setXY0(&mismatched_replay_tile, 0, 0);
  setWH(&mismatched_replay_tile, replay_size, replay_size);
  DrawPrim(&mismatched_replay_tile);
  const auto fail_closed_end = GR_EndGuestPresentationReplay();
  const auto fail_closed_present = GR_PresentGuestPresentationReplay(
      replay_rect.x, replay_rect.y, replay_rect.w, replay_rect.h);
  PsyX_EndScene();
  if (!fail_closed_begin || fail_closed_end != 0 || fail_closed_present != 0 ||
      GR_GetVRAMWriteSequence() != replay_write_sequence ||
      GR_GetGuestVRAMPackCount() != replay_pack_count ||
      GR_GetGuestCaptureCount() != replay_capture_count ||
      glGetError() != GL_NO_ERROR) {
    std::cerr << "Presentation replay target switch did not fail closed\n";
    PsyX_Shutdown();
    return 205;
  }

  PsyX_EndScene();

  PsyX_Shutdown();
  std::cout << "PsyCross offscreen render tests passed\n";
  return 0;
}
