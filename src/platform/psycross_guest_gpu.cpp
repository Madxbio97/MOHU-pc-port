#include "psycross_guest_gpu.hpp"

#include "sf/psx/gp0_command.hpp"

#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>
#include <SDL.h>
#include <psx/libgpu.h>
#include <psx/libgte.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>

#if !USE_PGXP
#error "The MOHU raw guest renderer requires PsyCross PGXP support"
#endif

namespace sf::platform::detail {
namespace {

constexpr std::size_t maximum_buffered_words = 1024U * 512U / 2U + 3U;
constexpr std::uint16_t vram_width = 1024U;
constexpr std::uint16_t vram_height = 512U;

[[nodiscard]] std::uint16_t transferWidth(std::uint32_t size) noexcept {
  return sf::psx::gp0TransferWidth(size);
}

[[nodiscard]] std::uint16_t transferHeight(std::uint32_t size) noexcept {
  return sf::psx::gp0TransferHeight(size);
}

#if USE_PGXP
void convertCoordinateWord(std::uint32_t *packet,
                           std::span<const std::uint32_t> command,
                           std::size_t word) {
  auto *coordinates = reinterpret_cast<std::uint16_t *>(packet + P_LEN + word);
  const auto packed = command[word];
  const auto x = static_cast<std::int16_t>(packed & 0xffffU);
  const auto y = static_cast<std::int16_t>(packed >> 16U);
  coordinates[0] = static_cast<std::uint16_t>(to_half_float(x));
  coordinates[1] = static_cast<std::uint16_t>(to_half_float(y));
}

[[nodiscard]] std::array<std::size_t, 4U>
polygonCoordinateWords(std::uint8_t opcode, std::size_t &count) noexcept {
  count = (opcode & 0x08U) != 0U ? 4U : 3U;
  return sf::psx::gp0PolygonCoordinateWords(opcode);
}

[[nodiscard]] std::array<std::array<std::size_t, 2U>, 4U>
polygonPerimeterEdges(std::size_t vertex_count,
                      std::size_t &edge_count) noexcept {
  if (vertex_count == 4U) {
    edge_count = 4U;
    // PS1 quads are packet ordered TL,TR,BL,BR. Match the same perimeter
    // order used by PsyCross's two-triangle expansion.
    return {{{0U, 1U}, {1U, 3U}, {3U, 2U}, {2U, 0U}}};
  }
  edge_count = 3U;
  return {{{0U, 1U}, {1U, 2U}, {2U, 0U}, {0U, 0U}}};
}

using CoherenceLineInterval = std::array<std::int64_t, 5U>;
constexpr auto coherence_direction_x = std::size_t{0U};
constexpr auto coherence_direction_y = std::size_t{1U};
constexpr auto coherence_line_constant = std::size_t{2U};
constexpr auto coherence_interval_begin = std::size_t{3U};
constexpr auto coherence_interval_end = std::size_t{4U};

[[nodiscard]] std::int64_t positiveGcd(std::int64_t first,
                                       std::int64_t second) noexcept {
  first = std::abs(first);
  second = std::abs(second);
  while (second != 0) {
    const auto remainder = first % second;
    first = second;
    second = remainder;
  }
  return first;
}

[[nodiscard]] bool normalizePackedEdge(std::uint32_t first,
                                       std::uint32_t second,
                                       CoherenceLineInterval &result) noexcept {
  const auto first_x =
      static_cast<std::int64_t>(static_cast<std::int16_t>(first & 0xffffU));
  const auto first_y =
      static_cast<std::int64_t>(static_cast<std::int16_t>(first >> 16U));
  const auto second_x =
      static_cast<std::int64_t>(static_cast<std::int16_t>(second & 0xffffU));
  const auto second_y =
      static_cast<std::int64_t>(static_cast<std::int16_t>(second >> 16U));
  auto direction_x = second_x - first_x;
  auto direction_y = second_y - first_y;
  const auto divisor = positiveGcd(direction_x, direction_y);
  if (divisor == 0) {
    return false;
  }
  direction_x /= divisor;
  direction_y /= divisor;
  if (direction_x < 0 || (direction_x == 0 && direction_y < 0)) {
    direction_x = -direction_x;
    direction_y = -direction_y;
  }

  // int16 endpoints and reduced int17 directions fit these cross/dot
  // products comfortably in int64, including corner-to-corner diagonals.
  const auto line_constant = direction_x * first_y - direction_y * first_x;
  const auto first_interval = direction_x * first_x + direction_y * first_y;
  const auto second_interval = direction_x * second_x + direction_y * second_y;
  result = {direction_x, direction_y, line_constant,
            std::min(first_interval, second_interval),
            std::max(first_interval, second_interval)};
  return true;
}

[[nodiscard]] bool canonicalProjectionWitnessValid(
    const psx::GteProjectedVertex &projection) noexcept;

void snapProjectionToPacket(psx::GteProjectedVertex &projection) noexcept {
  const auto packed_x = static_cast<float>(
      static_cast<std::int16_t>(projection.packed_sxy & 0xffffU));
  const auto packed_y = static_cast<float>(
      static_cast<std::int16_t>(projection.packed_sxy >> 16U));
  projection.screen_x = packed_x;
  projection.screen_y = packed_y;
  projection.view_x = (packed_x - projection.screen_offset_x) *
                      projection.view_z / projection.screen_h;
  projection.view_y = (packed_y - projection.screen_offset_y) *
                      projection.view_z / projection.screen_h;
  projection.exact_transform = false;
  projection.fractional_transform = false;
  projection.enhanced_sources = 0U;
}

[[nodiscard]] bool
snapExactProjectionToPacket(psx::GteProjectedVertex &projection) noexcept {
  if (!projection.valid || !projection.exact_transform ||
      !std::isfinite(projection.view_z) ||
      std::abs(projection.view_z) <= 1.0e-6F ||
      !std::isfinite(projection.screen_h) || projection.screen_h <= 0.0F) {
    return false;
  }
  const auto packed_x = static_cast<float>(
      static_cast<std::int16_t>(projection.packed_sxy & 0xffffU));
  const auto packed_y = static_cast<float>(
      static_cast<std::int16_t>(projection.packed_sxy >> 16U));
  constexpr auto maximum_displacement = 2.0F;
  if (std::abs(projection.screen_x - packed_x) > maximum_displacement ||
      std::abs(projection.screen_y - packed_y) > maximum_displacement) {
    return false;
  }
  const auto view_x = (packed_x - projection.screen_offset_x) *
                      projection.view_z / projection.screen_h;
  const auto view_y = (packed_y - projection.screen_offset_y) *
                      projection.view_z / projection.screen_h;
  if (!std::isfinite(view_x) || !std::isfinite(view_y))
    return false;
  projection.screen_x = packed_x;
  projection.screen_y = packed_y;
  projection.view_x = view_x;
  projection.view_y = view_y;
  return true;
}

[[nodiscard]] bool reconstructMissingQuadProjection(
    std::span<const std::uint32_t> command,
    const std::array<std::size_t, 4U> &coordinate_words,
    std::array<psx::GteProjectedVertex, 12U> &projections,
    std::size_t missing_vertex) noexcept {
  std::array<const psx::GteProjectedVertex *, 3U> known{};
  std::size_t known_count{};
  for (std::size_t vertex{}; vertex < coordinate_words.size(); ++vertex) {
    if (vertex == missing_vertex)
      continue;
    const auto &projection = projections[coordinate_words[vertex]];
    if (!canonicalProjectionWitnessValid(projection) ||
        known_count >= known.size()) {
      return false;
    }
    known[known_count++] = &projection;
  }
  if (known_count != known.size())
    return false;

  constexpr auto camera_tolerance = 1.0F / 65536.0F;
  for (std::size_t index = 1U; index < known.size(); ++index) {
    if (std::abs(known[index]->screen_h - known[0U]->screen_h) >
            camera_tolerance ||
        std::abs(known[index]->screen_offset_x - known[0U]->screen_offset_x) >
            camera_tolerance ||
        std::abs(known[index]->screen_offset_y - known[0U]->screen_offset_y) >
            camera_tolerance) {
      return false;
    }
  }

  const auto subtract = [](const psx::GteProjectedVertex &left,
                           const psx::GteProjectedVertex &right) {
    return std::array<double, 3U>{
        static_cast<double>(left.view_x) - right.view_x,
        static_cast<double>(left.view_y) - right.view_y,
        static_cast<double>(left.view_z) - right.view_z};
  };
  const auto first_edge = subtract(*known[1U], *known[0U]);
  const auto second_edge = subtract(*known[2U], *known[0U]);
  const std::array<double, 3U> normal{
      first_edge[1U] * second_edge[2U] - first_edge[2U] * second_edge[1U],
      first_edge[2U] * second_edge[0U] - first_edge[0U] * second_edge[2U],
      first_edge[0U] * second_edge[1U] - first_edge[1U] * second_edge[0U]};
  const auto plane_distance = normal[0U] * known[0U]->view_x +
                              normal[1U] * known[0U]->view_y +
                              normal[2U] * known[0U]->view_z;

  const auto missing_word = coordinate_words[missing_vertex];
  const auto packed = command[missing_word];
  const auto screen_x =
      static_cast<double>(static_cast<std::int16_t>(packed & 0xffffU));
  const auto screen_y =
      static_cast<double>(static_cast<std::int16_t>(packed >> 16U));
  const auto screen_h = static_cast<double>(known[0U]->screen_h);
  const std::array<double, 3U> ray{
      (screen_x - known[0U]->screen_offset_x) / screen_h,
      (screen_y - known[0U]->screen_offset_y) / screen_h, 1.0};
  const auto denominator =
      normal[0U] * ray[0U] + normal[1U] * ray[1U] + normal[2U] * ray[2U];
  if (!std::isfinite(plane_distance) || !std::isfinite(denominator) ||
      std::abs(denominator) <= 1.0e-9) {
    return false;
  }
  const auto depth = plane_distance / denominator;
  const auto minimum_known_depth =
      std::min({known[0U]->view_z, known[1U]->view_z, known[2U]->view_z});
  const auto maximum_known_depth =
      std::max({known[0U]->view_z, known[1U]->view_z, known[2U]->view_z});
  if (!std::isfinite(depth) || depth <= screen_h * 0.5 ||
      depth < static_cast<double>(minimum_known_depth) * 0.25 ||
      depth > static_cast<double>(maximum_known_depth) * 4.0) {
    return false;
  }

  auto &result = projections[missing_word];
  result = {};
  result.packed_sxy = packed;
  result.view_x = static_cast<float>(ray[0U] * depth);
  result.view_y = static_cast<float>(ray[1U] * depth);
  result.view_z = static_cast<float>(depth);
  result.projective_depth = result.view_z;
  result.screen_x = static_cast<float>(screen_x);
  result.screen_y = static_cast<float>(screen_y);
  result.screen_h = known[0U]->screen_h;
  result.screen_offset_x = known[0U]->screen_offset_x;
  result.screen_offset_y = known[0U]->screen_offset_y;
  result.valid = std::isfinite(result.view_x) && std::isfinite(result.view_y) &&
                 std::isfinite(result.view_z);
  return result.valid;
}

void convertPrimitiveCoordinates(std::uint8_t opcode,
                                 std::span<const std::uint32_t> command,
                                 std::uint32_t *packet) {
  const auto convert = [&](std::size_t word) {
    convertCoordinateWord(packet, command, word);
  };

  if (opcode < 0x24U) {
    convert(1U);
    convert(2U);
    convert(3U);
  } else if (opcode < 0x28U) {
    convert(1U);
    convert(3U);
    convert(5U);
  } else if (opcode < 0x2cU) {
    convert(1U);
    convert(2U);
    convert(3U);
    convert(4U);
  } else if (opcode < 0x30U) {
    convert(1U);
    convert(3U);
    convert(5U);
    convert(7U);
  } else if (opcode < 0x34U) {
    convert(1U);
    convert(3U);
    convert(5U);
  } else if (opcode < 0x38U) {
    convert(1U);
    convert(4U);
    convert(7U);
  } else if (opcode < 0x3cU) {
    convert(1U);
    convert(3U);
    convert(5U);
    convert(7U);
  } else if (opcode < 0x40U) {
    convert(1U);
    convert(4U);
    convert(7U);
    convert(10U);
  } else if (opcode < 0x48U) {
    convert(1U);
    convert(2U);
  } else if (opcode < 0x50U) {
    for (std::size_t word = 1U; word + 1U < command.size(); ++word) {
      convert(word);
    }
  } else if (opcode < 0x58U) {
    convert(1U);
    convert(3U);
  } else if (opcode < 0x60U) {
    for (std::size_t word = 1U; word + 1U < command.size(); word += 2U) {
      convert(word);
    }
  } else if (opcode < 0x64U) {
    convert(1U);
    convert(2U);
  } else if (opcode < 0x68U) {
    convert(1U);
    convert(3U);
  } else {
    convert(1U);
  }
}

enum class PreciseRejectReason {
  none,
  missing,
  nonfinite,
  packet_mismatch,
  hazard,
  ir_saturation,
  depth_saturation,
  divide_overflow,
  screen_saturation,
  screen_mismatch,
  reprojection_mismatch,
  camera_mismatch,
  near_plane,
};
[[nodiscard]] constexpr bool
requiresRawPacketFallback(PreciseRejectReason reason) noexcept {
  switch (reason) {
  case PreciseRejectReason::none:
  case PreciseRejectReason::missing:
  case PreciseRejectReason::near_plane:
    return false;
  case PreciseRejectReason::nonfinite:
  case PreciseRejectReason::packet_mismatch:
  case PreciseRejectReason::hazard:
  case PreciseRejectReason::ir_saturation:
  case PreciseRejectReason::depth_saturation:
  case PreciseRejectReason::divide_overflow:
  case PreciseRejectReason::screen_saturation:
  case PreciseRejectReason::screen_mismatch:
  case PreciseRejectReason::reprojection_mismatch:
  case PreciseRejectReason::camera_mismatch:
    return true;
  }
  return true;
}

// Legacy PGXP values still depend on the PS1 divide and screen clamps. Exact
// transform twins do not: they carry a coherent, unclamped camera-space tuple
// which the GPU can project and clip in homogeneous space, including vertices
// on the far side of the near plane.
constexpr float minimum_legacy_precise_view_depth = 32.0F;

[[nodiscard]] bool
preparePrecisePrimitive(std::uint8_t opcode,
                        std::span<const std::uint32_t> command,
                        std::span<const psx::GteProjectedVertex> projections,
                        std::array<PGXPVData, 4U> *prepared_vertices = nullptr,
                        PreciseRejectReason *reject_reason = nullptr,
                        bool use_projective_depth = true) {
  const auto reject = [reject_reason](PreciseRejectReason reason) {
    if (reject_reason != nullptr) {
      *reject_reason = reason;
    }
    return false;
  };
  if (reject_reason != nullptr) {
    *reject_reason = PreciseRejectReason::none;
  }
  if (opcode < 0x20U || opcode >= 0x40U ||
      projections.size() != command.size()) {
    return reject(PreciseRejectReason::missing);
  }

  constexpr auto maximum_reprojection_error = 0.25F;
  constexpr auto camera_state_tolerance = 1.0F / 65536.0F;
  float primitive_screen_h{};
  float primitive_offset_x{}, primitive_offset_y{};

  std::size_t vertex_count{};
  const auto coordinate_words = polygonCoordinateWords(opcode, vertex_count);
  std::array<PGXPVData, 4U> vertices{};
  auto use_exact_view_depth = true;
  for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
    const auto &source = projections[coordinate_words[vertex]];
    if (!source.valid || !source.exact_transform) {
      use_exact_view_depth = false;
      break;
    }
  }
  for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
    const auto word = coordinate_words[vertex];
    const auto &source = projections[word];
    if (!source.valid) {
      return reject(PreciseRejectReason::missing);
    }
    const auto raster_depth = use_exact_view_depth || !use_projective_depth
                                  ? source.view_z
                                  : source.projective_depth;
    const auto finite =
        std::isfinite(source.view_x) && std::isfinite(source.view_y) &&
        std::isfinite(source.view_z) && std::isfinite(source.screen_x) &&
        std::isfinite(raster_depth) && std::isfinite(source.screen_y) &&
        std::isfinite(source.screen_h) &&
        std::isfinite(source.screen_offset_x) &&
        std::isfinite(source.screen_offset_y);
    if (!finite) {
      return reject(PreciseRejectReason::nonfinite);
    }
    if (source.packed_sxy != command[word]) {
      return reject(PreciseRejectReason::packet_mismatch);
    }
    if (!source.exact_transform && source.ir_saturated) {
      return reject(PreciseRejectReason::ir_saturation);
    }
    if (!source.exact_transform && source.depth_saturated) {
      return reject(PreciseRejectReason::depth_saturation);
    }
    if (!source.exact_transform && source.divide_overflow) {
      return reject(PreciseRejectReason::divide_overflow);
    }
    if (!source.exact_transform && source.screen_saturated) {
      return reject(PreciseRejectReason::screen_saturation);
    }
    if (!source.pgxpEligible()) {
      return reject(PreciseRejectReason::hazard);
    }
    if (source.screen_h <= 0.0F ||
        (!use_exact_view_depth && raster_depth <= 0.0F)) {
      return reject(PreciseRejectReason::screen_mismatch);
    }
    if (!source.exact_transform &&
        source.view_z <= minimum_legacy_precise_view_depth) {
      return reject(PreciseRejectReason::near_plane);
    }
    const auto projection_consistent = [&source] {
      const auto reprojected_x = source.screen_offset_x + source.view_x *
                                                              source.screen_h /
                                                              source.view_z;
      const auto reprojected_y = source.screen_offset_y + source.view_y *
                                                              source.screen_h /
                                                              source.view_z;
      return std::isfinite(reprojected_x) && std::isfinite(reprojected_y) &&
             std::abs(reprojected_x - source.screen_x) <=
                 maximum_reprojection_error &&
             std::abs(reprojected_y - source.screen_y) <=
                 maximum_reprojection_error;
    }();
    const auto camera_consistent =
        vertex == 0U ||
        (std::abs(source.screen_h - primitive_screen_h) <=
             camera_state_tolerance &&
         std::abs(source.screen_offset_x - primitive_offset_x) <=
             camera_state_tolerance &&
         std::abs(source.screen_offset_y - primitive_offset_y) <=
             camera_state_tolerance);
    // Every exact vertex owns a coherent view tuple. Different object/bone
    // publications may legitimately meet in one primitive; common H/OFX/OFY
    // is the only camera-space invariant required for homogeneous clipping.
    if (!projection_consistent) {
      return reject(PreciseRejectReason::reprojection_mismatch);
    }
    if (!camera_consistent) {
      return reject(PreciseRejectReason::camera_mismatch);
    }
    if (vertex == 0U) {
      primitive_screen_h = source.screen_h;
      primitive_offset_x = source.screen_offset_x;
      primitive_offset_y = source.screen_offset_y;
    }

    auto &destination = vertices[vertex];
    auto raster_x = source.screen_x;
    auto raster_y = source.screen_y;
    if (use_exact_view_depth) {
      destination.px = source.view_x / 128.0F;
      destination.py = source.view_y / 128.0F;
      destination.pz = raster_depth / 128.0F;
    } else {
      const auto reciprocal_limited = raster_depth <= source.screen_h * 0.5F;
      if (reciprocal_limited) {
        raster_x = static_cast<float>(
            static_cast<std::int16_t>(source.packed_sxy & 0xffffU));
        raster_y = static_cast<float>(
            static_cast<std::int16_t>(source.packed_sxy >> 16U));
      }
      destination.px = (raster_x - source.screen_offset_x) * raster_depth /
                       source.screen_h / 128.0F;
      destination.py = (raster_y - source.screen_offset_y) * raster_depth /
                       source.screen_h / 128.0F;
      destination.pz = raster_depth / 128.0F;
    }
    destination.sx = raster_x;
    destination.sy = raster_y;
    destination.scr_h = source.screen_h;
    destination.ofx = source.screen_offset_x;
    destination.ofy = source.screen_offset_y;
    destination.exact_projection = source.exact_transform ? 1U : 0U;
    destination.precise_screen_position = 1U;
  }

  if (prepared_vertices != nullptr) {
    *prepared_vertices = vertices;
  }
  return true;
}

[[nodiscard]] bool prepareAffineScreenPrimitive(
    std::uint8_t opcode, std::span<const std::uint32_t> command,
    std::span<const psx::GteProjectedVertex> projections,
    std::array<PGXPVData, 4U> &vertices) {
  if (opcode < 0x20U || opcode >= 0x40U ||
      projections.size() != command.size()) {
    return false;
  }

  std::size_t vertex_count{};
  const auto coordinate_words = polygonCoordinateWords(opcode, vertex_count);
  for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
    const auto word = coordinate_words[vertex];
    const auto &source = projections[word];
    if (!source.valid || source.packed_sxy != command[word] ||
        !std::isfinite(source.screen_x) || !std::isfinite(source.screen_y)) {
      return false;
    }

    auto &destination = vertices[vertex];
    destination.sx = source.screen_x;
    destination.sy = source.screen_y;
    destination.precise_screen_position = 1U;
  }
  return true;
}

[[nodiscard]] u_short
emitPrecisePrimitive(std::uint8_t opcode,
                     std::span<const std::uint32_t> command,
                     std::span<const psx::GteProjectedVertex> projections,
                     const std::uint32_t *packet, bool allow_perspective,
                     bool allow_precise_screen, bool use_projective_depth) {
  constexpr auto no_pgxp = static_cast<u_short>(0xffffU);
  std::array<PGXPVData, 4U> vertices{};
  const auto perspective_prepared =
      allow_perspective &&
      preparePrecisePrimitive(opcode, command, projections, &vertices, nullptr,
                              use_projective_depth);
  if (!allow_precise_screen)
    return no_pgxp;
  if (!perspective_prepared) {
    vertices = {};
    if (!prepareAffineScreenPrimitive(opcode, command, projections, vertices)) {
      return no_pgxp;
    }
  }
  std::size_t vertex_count{};
  const auto coordinate_words = polygonCoordinateWords(opcode, vertex_count);
  for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
    vertices[vertex].lookup = packet[P_LEN + coordinate_words[vertex]];
  }

  const auto cache_mark = PGXP_MarkCache();
  for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
    if (PGXP_EmitCacheData(&vertices[vertex]) == no_pgxp) {
      PGXP_RewindCache(cache_mark);
      return no_pgxp;
    }
  }
  // Raw packets bypass addPrim, so consume the transform latch here. Leaving
  // it set would let the next native addPrim attach this raw cache range.
  const auto cache_end = PGXP_GetIndex(1);
  if (cache_end == no_pgxp) {
    PGXP_RewindCache(cache_mark);
    return no_pgxp;
  }
  return cache_end;
}

[[nodiscard]] bool
probePrecisePrimitive(std::uint8_t opcode,
                      std::span<const std::uint32_t> command,
                      std::span<const psx::GteProjectedVertex> projections,
                      PreciseRejectReason *reject_reason = nullptr,
                      bool use_projective_depth = true) {
  return preparePrecisePrimitive(opcode, command, projections, nullptr,
                                 reject_reason, use_projective_depth);
}

#endif

[[nodiscard]] bool canonicalProjectionWitnessValid(
    const psx::GteProjectedVertex &projection) noexcept {
  if (!projection.valid || !projection.pgxpEligible() ||
      (!projection.exact_transform &&
       (projection.ir_saturated || projection.depth_saturated ||
        projection.divide_overflow || projection.screen_saturated))) {
    return false;
  }
  const auto finite =
      std::isfinite(projection.view_x) && std::isfinite(projection.view_y) &&
      std::isfinite(projection.view_z) && std::isfinite(projection.screen_x) &&
      std::isfinite(projection.screen_y) &&
      std::isfinite(projection.screen_h) &&
      std::isfinite(projection.screen_offset_x) &&
      std::isfinite(projection.screen_offset_y);
  if (!finite || projection.screen_h <= 0.0F ||
      (!projection.exact_transform && projection.view_z <= 0.0F)) {
    return false;
  }

  constexpr auto maximum_reprojection_error = 0.25F;
  if (projection.exact_transform) {
    return true;
  }
  const auto reprojected_x =
      projection.screen_offset_x +
      projection.view_x * projection.screen_h / projection.view_z;
  const auto reprojected_y =
      projection.screen_offset_y +
      projection.view_y * projection.screen_h / projection.view_z;
  return projection.view_z > minimum_legacy_precise_view_depth &&
         std::isfinite(reprojected_x) && std::isfinite(reprojected_y) &&
         std::abs(reprojected_x - projection.screen_x) <=
             maximum_reprojection_error &&
         std::abs(reprojected_y - projection.screen_y) <=
             maximum_reprojection_error;
}

[[nodiscard]] bool canonicalProjectionWitnessMatches(
    const psx::GteProjectedVertex &left,
    const psx::GteProjectedVertex &right) noexcept {
  // Publication lineage/epoch can change when an unchanged source vertex is
  // copied back into a packet. They are transport metadata, not geometry.
  // Everything which affects projection, W or eligibility remains part of
  // the witness and therefore poisons the identity on disagreement.
  return left.packed_sxy == right.packed_sxy && left.view_x == right.view_x &&
         left.view_y == right.view_y && left.view_z == right.view_z &&
         left.screen_x == right.screen_x && left.screen_y == right.screen_y &&
         left.screen_h == right.screen_h &&
         left.screen_offset_x == right.screen_offset_x &&
         left.screen_offset_y == right.screen_offset_y &&
         left.ir_saturated == right.ir_saturated &&
         left.depth_saturated == right.depth_saturated &&
         left.divide_overflow == right.divide_overflow &&
         left.screen_saturated == right.screen_saturated &&
         left.valid == right.valid &&
         left.exact_transform == right.exact_transform &&
         left.fractional_transform == right.fractional_transform &&
         left.enhanced_sources == right.enhanced_sources;
}

[[nodiscard]] std::size_t
canonicalProjectionHash(std::uint64_t identity) noexcept {
  identity ^= identity >> 33U;
  identity *= 0xff51afd7ed558ccdULL;
  identity ^= identity >> 33U;
  identity *= 0xc4ceb9fe1a85ec53ULL;
  identity ^= identity >> 33U;
  return static_cast<std::size_t>(identity);
}

[[nodiscard]] std::size_t
sharedMeshVertexHash(std::uint64_t mesh_vertex_id,
                     std::uint32_t draw_offset) noexcept {
  return canonicalProjectionHash(
      mesh_vertex_id ^
      (static_cast<std::uint64_t>(draw_offset) * 0x9e3779b97f4a7c15ULL));
}

[[nodiscard]] std::uint64_t sharedMeshProjectionIdentity(
    const psx::GteProjectedVertex &projection) noexcept {
  return projection.mesh_vertex_id;
}
[[nodiscard]] std::size_t
coherenceLineHash(const CoherenceLineInterval &edge,
                  const PgxpDrawContext &draw_context) noexcept {
  const auto directions =
      (static_cast<std::uint64_t>(
           static_cast<std::uint32_t>(edge[coherence_direction_x]))
       << 32U) |
      static_cast<std::uint32_t>(edge[coherence_direction_y]);
  const auto constant =
      static_cast<std::uint64_t>(edge[coherence_line_constant]);
  auto hash = canonicalProjectionHash(
      static_cast<std::uint64_t>(canonicalProjectionHash(directions)) ^
      static_cast<std::uint64_t>(canonicalProjectionHash(constant)));
  for (const auto word :
       {draw_context.draw_mode, draw_context.draw_area_top_left,
        draw_context.draw_area_bottom_right, draw_context.draw_offset}) {
    hash = canonicalProjectionHash(
        static_cast<std::uint64_t>(hash) ^
        static_cast<std::uint64_t>(canonicalProjectionHash(word)));
  }
  return hash;
}

[[nodiscard]] PgxpDrawContext currentPgxpDrawContext() noexcept {
  DRAWENV draw{};
  GetDrawEnv(&draw);
  const auto clip_x = static_cast<std::uint32_t>(draw.clip.x) & 0x03ffU;
  const auto clip_y = static_cast<std::uint32_t>(draw.clip.y) & 0x01ffU;
  const auto right =
      static_cast<std::uint32_t>(draw.clip.w > 0 ? draw.clip.x + draw.clip.w - 1
                                                 : draw.clip.x) &
      0x03ffU;
  const auto bottom =
      static_cast<std::uint32_t>(draw.clip.h > 0 ? draw.clip.y + draw.clip.h - 1
                                                 : draw.clip.y) &
      0x01ffU;
  return {(static_cast<std::uint32_t>(draw.tpage) & 0x01ffU) |
              (static_cast<std::uint32_t>(draw.dtd != 0) << 9U),
          clip_x | (clip_y << 10U), right | (bottom << 10U),
          (static_cast<std::uint32_t>(draw.ofs[0]) & 0x07ffU) |
              ((static_cast<std::uint32_t>(draw.ofs[1]) & 0x07ffU) << 11U)};
}

void updatePgxpDrawContext(PgxpDrawContext &context, std::uint8_t opcode,
                           std::uint32_t command) noexcept {
  switch (opcode) {
  case 0xe1U:
    context.draw_mode = command & 0x03ffU;
    break;
  case 0xe3U:
    context.draw_area_top_left = command & 0x0007ffffU;
    break;
  case 0xe4U:
    context.draw_area_bottom_right = command & 0x0007ffffU;
    break;
  case 0xe5U:
    context.draw_offset = command & 0x003fffffU;
    break;
  default:
    break;
  }
}

} // namespace
void PsyCrossGuestGpu::presentDisplay(std::uint16_t source_x,
                                      std::uint16_t source_y,
                                      std::uint16_t width, std::uint16_t height,
                                      bool enabled, bool rgb24,
                                      bool interlaced) {

  const auto scanout_width = std::min(static_cast<unsigned int>(width),
                                      static_cast<unsigned int>(vram_width));
  const auto scanout_height = std::min(static_cast<unsigned int>(height),
                                       static_cast<unsigned int>(vram_height));
  if (scanout_width == 0U || scanout_height == 0U) {
    return;
  }
  GR_SetGuestDisplayGeometry(static_cast<int>(scanout_width),
                             static_cast<int>(scanout_height));

  DISPENV display{};
  SetDefDispEnv(&display, 0, 0, static_cast<int>(scanout_width),
                static_cast<int>(scanout_height));
  display.isrgb24 = static_cast<u_char>(rgb24);
  display.isinter = static_cast<u_char>(interlaced);
  PutDispEnv(&display);
  // Presentation fallback primitives are decoded after the queued guest
  // batches have been flushed. Give them an explicit immutable epoch instead
  // of inheriting the last gameplay submit's projection dimensions.
  GR_BeginGuestProjectionEpoch(0, static_cast<int>(scanout_width),
                               static_cast<int>(scanout_height), rgb24,
                               interlaced);

  DRAWENV previous{};
  GetDrawEnv(&previous);
  DRAWENV scanout{};
  SetDefDrawEnv(&scanout, 0, 0, static_cast<int>(scanout_width),
                static_cast<int>(scanout_height));
  scanout.dtd = 0;
  scanout.dfe = 1;
  scanout.isbg = 0;
  PutDrawEnv(&scanout);

  // A completed guest page is retained at the raw renderer's internal scale.
  // Present it before queuing the next backbuffer, while keeping 1x PS1 VRAM
  // as the authoritative texture/readback source for the emulated machine.
  if (enabled && !rgb24 && !interlaced) {
    // Runtime cadence can submit several guest steps before the host DrawSync.
    // Finish the pending page before selecting the current display cache.
    static_cast<void>(PsyX_BeginScene());
    DrawSync(0);
    RECT16 completed_page{};
    GR_SetOffscreenState(&completed_page, 0);
    if (GR_PresentHighResolutionVRAM(static_cast<int>(source_x),
                                     static_cast<int>(source_y),
                                     static_cast<int>(scanout_width),
                                     static_cast<int>(scanout_height)) != 0) {
      ++high_resolution_presents_;
      PutDrawEnv(&previous);
      return;
    }
    if (GR_PresentComposedGuestScanout(static_cast<int>(source_x),
                                       static_cast<int>(source_y),
                                       static_cast<int>(scanout_width),
                                       static_cast<int>(scanout_height)) != 0) {
      ++fallback_presents_;
      PutDrawEnv(&previous);
      return;
    }
  }
  if (enabled) {
    ++fallback_presents_;
  }

  // A zero PS1 texel is transparent to the regular texture shader. Clear the
  // native scanout aperture explicitly so pixels which the guest did not draw
  // cannot retain colour from an older host frame (the visible "solitaire"
  // trail). This also gives GP1 display-disable its required black output.
  TILE background{};
  SetTile(&background);
  setRGB0(&background, 0, 0, 0);
  setXY0(&background, 0, 0);
  setWH(&background, static_cast<short>(scanout_width),
        static_cast<short>(scanout_height));
  DrawPrim(&background);
  if (!enabled) {
    PutDrawEnv(&previous);
    return;
  }

  auto remaining_height = scanout_height;
  auto output_y = 0U;
  auto row_y = static_cast<unsigned int>(source_y % vram_height);
  while (remaining_height != 0U) {
    const auto page_y = row_y & ~255U;
    const auto texture_v = row_y - page_y;
    const auto block_height =
        std::min({remaining_height, 256U - texture_v,
                  static_cast<unsigned int>(vram_height) - row_y});
    auto remaining_width = scanout_width;
    auto output_x = 0U;
    auto row_x = static_cast<unsigned int>(source_x % vram_width);
    while (remaining_width != 0U) {
      const auto page_x = row_x & ~63U;
      const auto texture_u = row_x - page_x;
      const auto block_width =
          std::min({remaining_width, 256U - texture_u,
                    static_cast<unsigned int>(vram_width) - row_x});

      DR_TPAGE page{};
      SetDrawTPage(
          &page, 1, 0,
          GetTPage(2, 0, static_cast<int>(page_x), static_cast<int>(page_y)));
      DrawPrim(&page);

      SPRT sprite{};
      SetSprt(&sprite);
      setRGB0(&sprite, 128, 128, 128);
      setXY0(&sprite, static_cast<short>(output_x),
             static_cast<short>(output_y));
      setUV0(&sprite, static_cast<u_char>(texture_u),
             static_cast<u_char>(texture_v));
      setWH(&sprite, static_cast<short>(block_width),
            static_cast<short>(block_height));
      DrawPrim(&sprite);

      remaining_width -= block_width;
      output_x += block_width;
      row_x = (row_x + block_width) % vram_width;
    }
    remaining_height -= block_height;
    output_y += block_height;
    row_y = (row_y + block_height) % vram_height;
  }

  PutDrawEnv(&previous);
}

std::size_t
PsyCrossGuestGpu::commandLength(std::span<const std::uint32_t> words) noexcept {
  return sf::psx::gp0CommandLength(words);
}

void PsyCrossGuestGpu::rebuildCanonicalProjectionTable() {
  ++identity_canonical_builds_;
  canonical_projection_overflow_ = false;
  if (++canonical_projection_generation_ == 0U) {
    for (auto &entry : canonical_projection_table_) {
      entry.generation = 0U;
    }
    canonical_projection_generation_ = 1U;
  }

  auto witness_count = std::size_t{};
  const auto aligned_size = std::min(pending_projections_.size(),
                                     pending_projection_identities_.size());
  for (std::size_t index{}; index < aligned_size; ++index) {
    witness_count +=
        pending_projection_identities_[index] != 0U &&
                canonicalProjectionWitnessValid(pending_projections_[index])
            ? 1U
            : 0U;
  }
  if (witness_count == 0U) {
    return;
  }

  constexpr auto minimum_capacity = std::size_t{16U};
  const auto maximum_size = std::numeric_limits<std::size_t>::max();
  const auto required =
      witness_count <= maximum_size / 2U ? witness_count * 2U : maximum_size;
  auto capacity = minimum_capacity;
  while (capacity < required && capacity <= maximum_size / 2U) {
    capacity *= 2U;
  }
  if (capacity < required) {
    canonical_projection_overflow_ = true;
    return;
  }
  if (canonical_projection_table_.size() < capacity) {
    canonical_projection_table_.resize(capacity);
  }

  const auto mask = canonical_projection_table_.size() - 1U;
  for (std::size_t index{}; index < aligned_size; ++index) {
    const auto identity = pending_projection_identities_[index];
    const auto &projection = pending_projections_[index];
    if (identity == 0U || !canonicalProjectionWitnessValid(projection)) {
      continue;
    }

    auto slot = canonicalProjectionHash(identity) & mask;
    auto inserted = false;
    for (std::size_t probe{}; probe < canonical_projection_table_.size();
         ++probe) {
      auto &entry = canonical_projection_table_[slot];
      if (entry.generation != canonical_projection_generation_) {
        entry.identity = identity;
        entry.projection = projection;
        entry.generation = canonical_projection_generation_;
        entry.ambiguous = false;
        inserted = true;
        break;
      }
      if (entry.identity == identity) {
        if (!canonicalProjectionWitnessMatches(entry.projection, projection)) {
          entry.ambiguous = true;
        }
        inserted = true;
        break;
      }
      slot = (slot + 1U) & mask;
    }
    if (!inserted) {
      // There is intentionally no eviction: a saturated table makes every
      // identity lookup fail closed for this submit.
      canonical_projection_overflow_ = true;
      return;
    }
  }
}

const PsyCrossGuestGpu::CanonicalProjectionEntry *
PsyCrossGuestGpu::findCanonicalProjection(
    std::uint64_t identity) const noexcept {
  if (identity == 0U || canonical_projection_overflow_ ||
      canonical_projection_table_.empty()) {
    return nullptr;
  }
  const auto mask = canonical_projection_table_.size() - 1U;
  auto slot = canonicalProjectionHash(identity) & mask;
  for (std::size_t probe{}; probe < canonical_projection_table_.size();
       ++probe) {
    const auto &entry = canonical_projection_table_[slot];
    if (entry.generation != canonical_projection_generation_) {
      return nullptr;
    }
    if (entry.identity == identity) {
      return &entry;
    }
    slot = (slot + 1U) & mask;
  }
  return nullptr;
}

bool PsyCrossGuestGpu::rebuildPackedProjectionTable(
    std::span<const psx::GteProjectedVertex> catalog) {
  packed_projection_overflow_ = false;
  if (catalog.empty()) {
    return false;
  }
  ++projection_catalog_builds_;
  if (++packed_projection_generation_ == 0U) {
    for (auto &entry : packed_projection_table_) {
      entry.generation = 0U;
    }
    packed_projection_generation_ = 1U;
  }

  constexpr auto table_capacity = std::size_t{32768U};
  if (packed_projection_table_.size() != table_capacity) {
    try {
      packed_projection_table_.resize(table_capacity);
    } catch (...) {
      packed_projection_overflow_ = true;
      return false;
    }
  }

  const auto mask = packed_projection_table_.size() - 1U;
  auto inserted_count = std::size_t{};
  for (const auto &projection : catalog) {
    if (!canonicalProjectionWitnessValid(projection)) {
      continue;
    }
    auto slot = canonicalProjectionHash(projection.packed_sxy) & mask;
    auto inserted = false;
    for (std::size_t probe{}; probe < packed_projection_table_.size();
         ++probe) {
      auto &entry = packed_projection_table_[slot];
      if (entry.generation != packed_projection_generation_) {
        entry.packed_sxy = projection.packed_sxy;
        entry.projection = projection;
        entry.generation = packed_projection_generation_;
        entry.ambiguous = false;
        ++inserted_count;
        inserted = true;
        break;
      }
      if (entry.packed_sxy == projection.packed_sxy) {
        if (!canonicalProjectionWitnessMatches(entry.projection, projection)) {
          entry.ambiguous = true;
        }
        inserted = true;
        break;
      }
      slot = (slot + 1U) & mask;
    }
    if (!inserted) {
      packed_projection_overflow_ = true;
      return false;
    }
  }
  return inserted_count != 0U;
}

const PsyCrossGuestGpu::PackedProjectionEntry *
PsyCrossGuestGpu::findPackedProjection(
    std::uint32_t packed_sxy) const noexcept {
  if (packed_projection_overflow_ || packed_projection_table_.empty()) {
    return nullptr;
  }
  const auto mask = packed_projection_table_.size() - 1U;
  auto slot = canonicalProjectionHash(packed_sxy) & mask;
  for (std::size_t probe{}; probe < packed_projection_table_.size(); ++probe) {
    const auto &entry = packed_projection_table_[slot];
    if (entry.generation != packed_projection_generation_) {
      return nullptr;
    }
    if (entry.packed_sxy == packed_sxy) {
      return &entry;
    }
    slot = (slot + 1U) & mask;
  }
  return nullptr;
}

void PsyCrossGuestGpu::beginSharedMeshTable(std::size_t word_count) noexcept {
  ++shared_mesh_builds_;
  shared_mesh_overflow_ = false;
  shared_mesh_policy_active_ = false;
  if (++shared_mesh_generation_ == 0U) {
    for (auto &entry : shared_mesh_vertex_table_) {
      entry.generation = 0U;
    }
    shared_mesh_generation_ = 1U;
  }

  constexpr auto minimum_capacity = std::size_t{1024U};
  constexpr auto maximum_capacity = std::size_t{65536U};
  if (word_count == 0U || word_count > maximum_capacity) {
    shared_mesh_overflow_ = true;
    return;
  }
  const auto required = std::max(minimum_capacity, word_count);
  auto capacity = minimum_capacity;
  while (capacity < required) {
    capacity *= 2U;
  }
  try {
    if (shared_mesh_vertex_table_.size() < capacity) {
      shared_mesh_vertex_table_.resize(capacity);
    }
  } catch (...) {
    shared_mesh_overflow_ = true;
    return;
  }
  shared_mesh_policy_active_ = !shared_mesh_vertex_table_.empty();
}

void PsyCrossGuestGpu::recordSharedMeshVertex(
    std::uint32_t draw_offset, const psx::GteProjectedVertex *projection,
    bool precise) noexcept {
  if (!shared_mesh_policy_active_ || shared_mesh_overflow_ ||
      shared_mesh_vertex_table_.empty()) {
    return;
  }
  if (!precise || projection == nullptr || projection->mesh_vertex_id == 0U) {
    return;
  }

  const auto mesh_vertex_id = sharedMeshProjectionIdentity(*projection);
  const auto mask = shared_mesh_vertex_table_.size() - 1U;
  auto slot = sharedMeshVertexHash(mesh_vertex_id, draw_offset) & mask;
  for (std::size_t probe{}; probe < shared_mesh_vertex_table_.size(); ++probe) {
    auto &entry = shared_mesh_vertex_table_[slot];
    if (entry.generation != shared_mesh_generation_) {
      entry = {};
      entry.mesh_vertex_id = mesh_vertex_id;
      entry.draw_offset = draw_offset;
      entry.generation = shared_mesh_generation_;
      entry.projection = *projection;
      entry.has_projection = true;
      ++shared_mesh_unique_vertices_;
      return;
    }
    if (entry.mesh_vertex_id == mesh_vertex_id &&
        entry.draw_offset == draw_offset) {
      ++shared_mesh_reused_vertices_;
      if (!entry.has_projection) {
        entry.projection = *projection;
        entry.has_projection = true;
        return;
      }
      if (!canonicalProjectionWitnessMatches(entry.projection, *projection) &&
          !entry.ambiguous) {
        entry.ambiguous = true;
        ++shared_mesh_conflicts_;
      }
      return;
    }
    slot = (slot + 1U) & mask;
  }
  shared_mesh_overflow_ = true;
  shared_mesh_policy_active_ = false;
}

const PsyCrossGuestGpu::SharedMeshVertexEntry *
PsyCrossGuestGpu::findSharedMeshVertex(
    std::uint64_t mesh_vertex_id, std::uint32_t draw_offset) const noexcept {
  if (!shared_mesh_policy_active_ || shared_mesh_overflow_ ||
      shared_mesh_vertex_table_.empty() || mesh_vertex_id == 0U) {
    return nullptr;
  }
  const auto mask = shared_mesh_vertex_table_.size() - 1U;
  auto slot = sharedMeshVertexHash(mesh_vertex_id, draw_offset) & mask;
  for (std::size_t probe{}; probe < shared_mesh_vertex_table_.size(); ++probe) {
    const auto &entry = shared_mesh_vertex_table_[slot];
    if (entry.generation != shared_mesh_generation_) {
      return nullptr;
    }
    if (entry.mesh_vertex_id == mesh_vertex_id &&
        entry.draw_offset == draw_offset) {
      return &entry;
    }
    slot = (slot + 1U) & mask;
  }
  return nullptr;
}

bool PsyCrossGuestGpu::coherencePolicyDemanded(
    bool missing_only) const noexcept {
  if (pending_projections_.size() != pending_.size()) {
    return false;
  }
  auto consumed = std::size_t{};
  while (consumed < pending_.size()) {
    const auto remaining =
        std::span<const std::uint32_t>{pending_}.subspan(consumed);
    const auto length = commandLength(remaining);
    if (length == 0U || remaining.size() < length) {
      break;
    }
    const auto command = remaining.first(length);
    const auto opcode = static_cast<std::uint8_t>(command.front() >> 24U);
    if (opcode >= 0x20U && opcode < 0x40U) {
      const auto command_projections =
          std::span<const psx::GteProjectedVertex>{pending_projections_}
              .subspan(consumed, length);
      std::size_t vertex_count{};
      const auto coordinate_words =
          polygonCoordinateWords(opcode, vertex_count);
      for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
        const auto word = coordinate_words[vertex];
        const auto &projection = command_projections[word];
        if (missing_only && !projection.valid)
          return true;
        if (missing_only)
          continue;

        const auto finite = std::isfinite(projection.view_x) &&
                            std::isfinite(projection.view_y) &&
                            std::isfinite(projection.view_z) &&
                            std::isfinite(projection.screen_x) &&
                            std::isfinite(projection.screen_y) &&
                            std::isfinite(projection.screen_h) &&
                            std::isfinite(projection.screen_offset_x) &&
                            std::isfinite(projection.screen_offset_y);
        if (!projection.valid || !projection.pgxpEligible() ||
            (!projection.exact_transform &&
             (projection.ir_saturated || projection.depth_saturated ||
              projection.divide_overflow || projection.screen_saturated)) ||
            projection.packed_sxy != command[word] || !finite ||
            (!projection.exact_transform &&
             projection.view_z <= minimum_legacy_precise_view_depth) ||
            projection.screen_h <= 0.0F) {
          return true;
        }
      }
    }
    consumed += length;
  }
  return false;
}

void PsyCrossGuestGpu::prepareCoherenceEdgePolicy(
    bool use_packed_catalog, std::size_t catalog_first_word, bool missing_only,
    bool near_only) {
  constexpr auto unclassified = std::uint8_t{0U};
  constexpr auto precise = std::uint8_t{1U};
  constexpr auto fallback = std::uint8_t{2U};
  coherence_precise_status_.assign(pending_.size(), unclassified);
  coherence_reject_reasons_.assign(
      pending_.size(), static_cast<std::uint8_t>(PreciseRejectReason::none));
  coherence_draw_contexts_.assign(pending_.size(), {});
  coherence_edge_scratch_.clear();
  coherence_edge_active_ = false;
  coherence_edge_near_only_ = near_only;
  coherence_policy_active_ = true;
  auto draw_context = currentPgxpDrawContext();

  auto consumed = std::size_t{};
  while (consumed < pending_.size()) {
    const auto remaining =
        std::span<const std::uint32_t>{pending_}.subspan(consumed);
    const auto length = commandLength(remaining);
    if (length == 0U || remaining.size() < length) {
      break;
    }
    const auto command = remaining.first(length);
    const auto opcode = static_cast<std::uint8_t>(command.front() >> 24U);
    coherence_draw_contexts_[consumed] = draw_context;
    if (opcode >= 0x20U && opcode < 0x40U) {
      std::array<psx::GteProjectedVertex, 12U> resolved_storage{};
      auto command_projections = std::span<const psx::GteProjectedVertex>{};
      auto catalog_resolution_failed = false;
      if (use_packed_catalog && consumed >= catalog_first_word) {
        std::size_t vertex_count{};
        const auto coordinate_words =
            polygonCoordinateWords(opcode, vertex_count);
        for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
          const auto word = coordinate_words[vertex];
          const auto *entry = findPackedProjection(command[word]);
          if (entry == nullptr || entry->ambiguous) {
            catalog_resolution_failed = true;
            break;
          }
          resolved_storage[word] = entry->projection;
        }
        if (!catalog_resolution_failed) {
          command_projections =
              std::span<const psx::GteProjectedVertex>{resolved_storage}.first(
                  length);
        }
      } else if (!pending_projections_.empty()) {
        command_projections =
            std::span<const psx::GteProjectedVertex>{pending_projections_}
                .subspan(consumed, length);
      }
      PreciseRejectReason reject_reason{};
      const auto is_precise =
          !catalog_resolution_failed &&
          probePrecisePrimitive(opcode, command, command_projections,
                                &reject_reason);
      if (catalog_resolution_failed) {
        reject_reason = PreciseRejectReason::missing;
      }
      coherence_precise_status_[consumed] = is_precise ? precise : fallback;
      coherence_reject_reasons_[consumed] =
          static_cast<std::uint8_t>(reject_reason);
      if (!is_precise &&
          (!missing_only || reject_reason == PreciseRejectReason::missing)) {
        std::size_t vertex_count{};
        const auto coordinate_words =
            polygonCoordinateWords(opcode, vertex_count);
        std::size_t edge_count{};
        const auto perimeter = polygonPerimeterEdges(vertex_count, edge_count);
        for (std::size_t edge{}; edge < edge_count; ++edge) {
          const auto first = command[coordinate_words[perimeter[edge][0]]];
          const auto second = command[coordinate_words[perimeter[edge][1]]];
          CoherenceLineInterval normalized{};
          if (normalizePackedEdge(first, second, normalized)) {
            coherence_edge_scratch_.push_back(
                {static_cast<std::int32_t>(normalized[coherence_direction_x]),
                 static_cast<std::int32_t>(normalized[coherence_direction_y]),
                 normalized[coherence_line_constant],
                 normalized[coherence_interval_begin],
                 normalized[coherence_interval_end], draw_context, 0U});
          }
        }
      }
    }
    updatePgxpDrawContext(draw_context, opcode, command.front());
    consumed += length;
  }

  if (coherence_edge_scratch_.empty()) {
    return;
  }
  ++coherence_edge_builds_;
  if (++coherence_edge_generation_ == 0U) {
    for (auto &entry : coherence_edge_table_) {
      entry.generation = 0U;
    }
    coherence_edge_generation_ = 1U;
  }

  constexpr auto minimum_capacity = std::size_t{16U};
  const auto maximum_size = std::numeric_limits<std::size_t>::max();
  const auto required = coherence_edge_scratch_.size() <= maximum_size / 2U
                            ? coherence_edge_scratch_.size() * 2U
                            : maximum_size;
  auto capacity = minimum_capacity;
  while (capacity < required && capacity <= maximum_size / 2U) {
    capacity *= 2U;
  }
  if (capacity < required) {
    return;
  }
  if (coherence_edge_table_.size() < capacity) {
    coherence_edge_table_.resize(capacity);
  }
  const auto mask = coherence_edge_table_.size() - 1U;
  for (const auto &edge : coherence_edge_scratch_) {
    const CoherenceLineInterval normalized{
        edge.direction_x, edge.direction_y, edge.line_constant,
        edge.interval_begin, edge.interval_end};
    auto slot = coherenceLineHash(normalized, edge.draw_context) & mask;
    for (std::size_t probe{}; probe < coherence_edge_table_.size(); ++probe) {
      auto &entry = coherence_edge_table_[slot];
      if (entry.generation != coherence_edge_generation_) {
        entry = edge;
        entry.generation = coherence_edge_generation_;
        break;
      }
      if (entry.direction_x == edge.direction_x &&
          entry.direction_y == edge.direction_y &&
          entry.line_constant == edge.line_constant &&
          entry.interval_begin == edge.interval_begin &&
          entry.interval_end == edge.interval_end &&
          entry.draw_context == edge.draw_context) {
        break;
      }
      slot = (slot + 1U) & mask;
    }
  }
  coherence_edge_active_ = true;
}
bool PsyCrossGuestGpu::hasFallbackEdge(
    std::uint32_t first, std::uint32_t second,
    const PgxpDrawContext &draw_context) const noexcept {
  if (!coherence_edge_active_ || coherence_edge_table_.empty()) {
    return false;
  }
  CoherenceLineInterval edge{};
  if (!normalizePackedEdge(first, second, edge)) {
    return false;
  }
  const auto mask = coherence_edge_table_.size() - 1U;
  auto slot = coherenceLineHash(edge, draw_context) & mask;
  for (std::size_t probe{}; probe < coherence_edge_table_.size(); ++probe) {
    const auto &entry = coherence_edge_table_[slot];
    if (entry.generation != coherence_edge_generation_) {
      return false;
    }
    if (entry.direction_x == edge[coherence_direction_x] &&
        entry.direction_y == edge[coherence_direction_y] &&
        entry.line_constant == edge[coherence_line_constant] &&
        entry.draw_context == draw_context &&
        entry.interval_begin == edge[coherence_interval_begin] &&
        entry.interval_end == edge[coherence_interval_end]) {
      return true;
    }
    slot = (slot + 1U) & mask;
  }
  return false;
}
void PsyCrossGuestGpu::clearWrapped(std::uint16_t x, std::uint16_t y,
                                    std::uint16_t width, std::uint16_t height,
                                    std::uint32_t color) {
  auto remaining_height = static_cast<unsigned int>(height);
  auto destination_y = static_cast<unsigned int>(y);
  while (remaining_height != 0U) {
    const auto block_height =
        std::min(remaining_height,
                 static_cast<unsigned int>(vram_height) - destination_y);
    auto remaining_width = static_cast<unsigned int>(width);
    auto destination_x = static_cast<unsigned int>(x);
    while (remaining_width != 0U) {
      const auto block_width =
          std::min(remaining_width,
                   static_cast<unsigned int>(vram_width) - destination_x);
      RECT16 rectangle{
          static_cast<short>(destination_x),
          static_cast<short>(destination_y),
          static_cast<short>(block_width),
          static_cast<short>(block_height),
      };
      GR_ClearVRAM(rectangle.x, rectangle.y, rectangle.w, rectangle.h,
                   static_cast<u_char>(color), static_cast<u_char>(color >> 8U),
                   static_cast<u_char>(color >> 16U));
      remaining_width -= block_width;
      destination_x = 0U;
    }
    remaining_height -= block_height;
    destination_y = 0U;
  }
}

void PsyCrossGuestGpu::uploadWrapped(std::uint16_t x, std::uint16_t y,
                                     std::uint16_t width, std::uint16_t height,
                                     const std::uint16_t *pixels) {
  if (static_cast<unsigned int>(x) + width <= vram_width &&
      static_cast<unsigned int>(y) + height <= vram_height) {
    RECT16 rectangle{static_cast<short>(x), static_cast<short>(y),
                     static_cast<short>(width), static_cast<short>(height)};
    LoadImage(&rectangle,
              reinterpret_cast<u_long *>(const_cast<std::uint16_t *>(pixels)));
    return;
  }

  for (std::uint16_t row = 0U; row < height; ++row) {
    const auto destination_y =
        static_cast<std::uint16_t>((y + row) % vram_height);
    auto remaining_width = static_cast<unsigned int>(width);
    auto destination_x = static_cast<unsigned int>(x);
    auto source_offset = static_cast<std::size_t>(row) * width;
    while (remaining_width != 0U) {
      const auto block_width =
          std::min(remaining_width,
                   static_cast<unsigned int>(vram_width) - destination_x);
      RECT16 rectangle{
          static_cast<short>(destination_x),
          static_cast<short>(destination_y),
          static_cast<short>(block_width),
          1,
      };
      LoadImage(&rectangle,
                reinterpret_cast<u_long *>(
                    const_cast<std::uint16_t *>(pixels + source_offset)));
      remaining_width -= block_width;
      source_offset += block_width;
      destination_x = 0U;
    }
  }
}

void PsyCrossGuestGpu::moveWrapped(std::uint16_t source_x,
                                   std::uint16_t source_y,
                                   std::uint16_t destination_x,
                                   std::uint16_t destination_y,
                                   std::uint16_t width, std::uint16_t height) {
  const bool source_wraps =
      static_cast<unsigned int>(source_x) + width > vram_width ||
      static_cast<unsigned int>(source_y) + height > vram_height;
  const bool destination_wraps =
      static_cast<unsigned int>(destination_x) + width > vram_width ||
      static_cast<unsigned int>(destination_y) + height > vram_height;
  if (!source_wraps && !destination_wraps) {
    RECT16 rectangle{static_cast<short>(source_x), static_cast<short>(source_y),
                     static_cast<short>(width), static_cast<short>(height)};
    MoveImage(&rectangle, destination_x, destination_y);
    return;
  }

  transfer_words_.resize(static_cast<std::size_t>(width) * height);
  if (!source_wraps) {
    RECT16 rectangle{static_cast<short>(source_x), static_cast<short>(source_y),
                     static_cast<short>(width), static_cast<short>(height)};
    StoreImage(&rectangle, reinterpret_cast<u_long *>(transfer_words_.data()));
  } else {
    std::vector<std::uint16_t> chunk_words;
    auto remaining_height = static_cast<unsigned int>(height);
    auto chunk_y = static_cast<unsigned int>(source_y);
    auto destination_row = 0U;
    while (remaining_height != 0U) {
      const auto chunk_height = std::min(
          remaining_height, static_cast<unsigned int>(vram_height) - chunk_y);
      auto remaining_width = static_cast<unsigned int>(width);
      auto chunk_x = static_cast<unsigned int>(source_x);
      auto destination_column = 0U;
      while (remaining_width != 0U) {
        const auto chunk_width = std::min(
            remaining_width, static_cast<unsigned int>(vram_width) - chunk_x);
        chunk_words.resize(static_cast<std::size_t>(chunk_width) *
                           chunk_height);
        RECT16 rectangle{
            static_cast<short>(chunk_x), static_cast<short>(chunk_y),
            static_cast<short>(chunk_width), static_cast<short>(chunk_height)};
        StoreImage(&rectangle, reinterpret_cast<u_long *>(chunk_words.data()));
        for (auto row = 0U; row < chunk_height; ++row) {
          std::memcpy(
              transfer_words_.data() +
                  static_cast<std::size_t>(destination_row + row) * width +
                  destination_column,
              chunk_words.data() + static_cast<std::size_t>(row) * chunk_width,
              static_cast<std::size_t>(chunk_width) * sizeof(std::uint16_t));
        }
        remaining_width -= chunk_width;
        destination_column += chunk_width;
        chunk_x = 0U;
      }
      remaining_height -= chunk_height;
      destination_row += chunk_height;
      chunk_y = 0U;
    }
  }
  uploadWrapped(destination_x, destination_y, width, height,
                transfer_words_.data());
}

void PsyCrossGuestGpu::dispatch(
    std::span<const std::uint32_t> command,
    std::span<const psx::GteProjectedVertex> projections,
    bool precise_candidate, bool allow_precise, bool allow_precise_screen,
    bool use_projective_depth) {
  const auto opcode = static_cast<std::uint8_t>(command.front() >> 24U);
  const auto synchronize_vram = [] {
    DrawSync(0);
    RECT16 target{};
    GR_SetOffscreenState(&target, 0);
  };

  if (opcode == 0x02U && command.size() == 3U) {
    synchronize_vram();
    clearWrapped(static_cast<std::uint16_t>(command[1] & 0x03ffU),
                 static_cast<std::uint16_t>((command[1] >> 16U) & 0x01ffU),
                 transferWidth(command[2]), transferHeight(command[2]),
                 command[0]);
    GR_UpdateVRAM();
    return;
  }
  if (opcode >= 0x80U && opcode < 0xa0U && command.size() == 4U) {
    synchronize_vram();
    moveWrapped(static_cast<std::uint16_t>(command[1] & 0x03ffU),
                static_cast<std::uint16_t>((command[1] >> 16U) & 0x01ffU),
                static_cast<std::uint16_t>(command[2] & 0x03ffU),
                static_cast<std::uint16_t>((command[2] >> 16U) & 0x01ffU),
                transferWidth(command[3]), transferHeight(command[3]));
    GR_UpdateVRAM();
    return;
  }
  if (opcode >= 0xa0U && opcode < 0xc0U && command.size() >= 3U) {
    synchronize_vram();
    uploadWrapped(static_cast<std::uint16_t>(command[1] & 0x03ffU),
                  static_cast<std::uint16_t>((command[1] >> 16U) & 0x01ffU),
                  transferWidth(command[2]), transferHeight(command[2]),
                  reinterpret_cast<const std::uint16_t *>(command.data() + 3U));
    GR_UpdateVRAM();
    return;
  }
  if ((opcode >= 0x20U && opcode < 0x80U) ||
      (opcode >= 0xe1U && opcode <= 0xe6U)) {
    std::array<std::uint32_t, P_LEN + 16U> packet{};
    if (command.size() > packet.size() - P_LEN) {
      ++unsupported_commands_;
      return;
    }
    auto *tag = reinterpret_cast<P_TAG *>(packet.data());
    setlen(tag, command.size());
    std::memcpy(packet.data() + P_LEN, command.data(), command.size_bytes());
    if (opcode >= 0x20U && opcode < 0x40U && (opcode & 0x04U) != 0U) {
      // Retail packets can leave non-hardware bits set in their embedded
      // CLUT/TPAGE halfwords. The PS1 ignores them, but PsyCross reserves
      // TPAGE bits 10..15 for native texture-atlas aliases. Mask raw guest
      // polygons at the adapter boundary so a value such as LEVEL's 0x300e
      // still samples native VRAM page 14.
      constexpr std::uint32_t ps1_clut_word_mask = 0x7fffffffU;
      constexpr std::uint32_t ps1_tpage_word_mask = 0x03ffffffU;
      packet[P_LEN + 2U] &= ps1_clut_word_mask;
      const auto tpage_word = opcode < 0x30U ? 4U : 5U;
      packet[P_LEN + tpage_word] &= ps1_tpage_word_mask;
    }
    if (opcode == 0xe1U) {
      // GP0(E1).10 controls writes into the current PS1 display area. PsyCross
      // uses DRAWENV::dfe to select the native framebuffer instead, so raw
      // guest commands must keep rendering into emulated VRAM.
      packet[P_LEN] &= ~(1U << 10U);
    }
#if USE_PGXP
    if (opcode >= 0x20U && opcode < 0x40U) {
      ++polygon_primitives_;
      if (precise_candidate) {
        ++precise_candidates_;
      }
      if (precise_candidate && !allow_precise) {
        ++coherence_fallback_primitives_;
      }
      std::size_t vertex_count{};
      const auto coordinate_words =
          polygonCoordinateWords(opcode, vertex_count);
      auto projected_vertices = std::size_t{};
      for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
        const auto word = coordinate_words[vertex];
        if (word < projections.size() && projections[word].valid &&
            projections[word].packed_sxy == command[word]) {
          ++projected_vertices;
        }
      }
      if (projected_vertices != 0U && projected_vertices < vertex_count) {
        ++partial_projection_primitives_;
      }
    }
    if (opcode >= 0x20U && opcode < 0x80U) {
      convertPrimitiveCoordinates(opcode, command, packet.data());
    }
    const auto pgxp_index =
        opcode >= 0x20U && opcode < 0x40U
            ? emitPrecisePrimitive(opcode, command, projections, packet.data(),
                                   allow_precise, allow_precise_screen,
                                   use_projective_depth)
            : static_cast<u_short>(0xffffU);
    if (pgxp_index != static_cast<u_short>(0xffffU)) {
      DrawPrimPGXP(tag, pgxp_index);
      if (allow_precise) {
        ++precise_primitives_;
      }
    } else {
      DrawPrim(tag);
    }
#else
    DrawPrim(tag);
#endif
    return;
  }
  if (opcode != 0x00U && opcode != 0x01U &&
      !(opcode >= 0xc0U && opcode < 0xe0U)) {
    ++unsupported_commands_;
  }
}

void PsyCrossGuestGpu::submit(
    std::span<const std::uint32_t> words,
    std::span<const psx::GteProjectedVertex> projections,
    std::span<const std::uint64_t> projection_identities,
    std::uint64_t command_buffer_epoch,
    std::span<const psx::GteProjectedVertex> projection_catalog,
    std::span<const psx::GpuDmaWordSource> dma_sources) {
  if (command_buffer_epoch != command_buffer_epoch_) {
    pending_.clear();
    pending_projections_.clear();
    pending_projection_identities_.clear();
    pending_dma_sources_.clear();
    command_buffer_epoch_ = command_buffer_epoch;
  }
  const auto pending_words_before_append = pending_.size();
  pending_.insert(pending_.end(), words.begin(), words.end());
  const auto projection_transport_active =
      !pending_projections_.empty() || !projections.empty();
  if (projection_transport_active && pending_projections_.empty() &&
      pending_words_before_append != 0U) {
    pending_projections_.resize(pending_words_before_append);
  }
  if (projection_transport_active && projections.size() == words.size()) {
    pending_projections_.insert(pending_projections_.end(), projections.begin(),
                                projections.end());
  } else if (projection_transport_active) {
    pending_projections_.resize(pending_.size());
  }
  if (projection_transport_active &&
      pending_projections_.size() != pending_.size()) {
    pending_projections_.resize(pending_.size());
  }
  const auto identity_transport_active =
      !pending_projection_identities_.empty() || !projection_identities.empty();
  if (identity_transport_active) {
    if (pending_projection_identities_.empty() &&
        pending_words_before_append != 0U) {
      pending_projection_identities_.resize(pending_words_before_append);
    }
    if (projection_identities.size() == words.size()) {
      pending_projection_identities_.insert(
          pending_projection_identities_.end(), projection_identities.begin(),
          projection_identities.end());
    } else {
      pending_projection_identities_.resize(pending_.size());
    }
    if (pending_projection_identities_.size() != pending_.size()) {
      pending_projection_identities_.resize(pending_.size());
    }
  }
  const auto dma_source_transport_active =
      !pending_dma_sources_.empty() || !dma_sources.empty();
  if (dma_source_transport_active) {
    if (pending_dma_sources_.empty() && pending_words_before_append != 0U) {
      pending_dma_sources_.resize(pending_words_before_append);
    }
    if (dma_sources.size() == words.size()) {
      pending_dma_sources_.insert(pending_dma_sources_.end(),
                                  dma_sources.begin(), dma_sources.end());
    } else {
      pending_dma_sources_.resize(pending_.size());
    }
    if (pending_dma_sources_.size() != pending_.size()) {
      pending_dma_sources_.resize(pending_.size());
    }
  }
  const auto catalog_handle_active =
      identity_transport_active && !projection_catalog.empty();
  // A retail software-clipped packet may lose its compact handle even though
  // the same shared vertex was projected elsewhere in this frame. Build the
  // packed witness index only when such a hole is actually present. Unique
  // witnesses recover one canonical PsyCross vertex; collisions fail closed.
  const auto catalog_recovery_demanded =
      coherence_edge_snapping_enabled_ && quad_recovery_enabled_ &&
      [&] {
        if (!catalog_handle_active)
          return false;
        auto scanned = std::size_t{};
        while (scanned < pending_.size()) {
          const auto remaining =
              std::span<const std::uint32_t>{pending_}.subspan(scanned);
          const auto length = commandLength(remaining);
          if (length == 0U || remaining.size() < length)
            break;
          const auto command = remaining.first(length);
          const auto opcode = static_cast<std::uint8_t>(command.front() >> 24U);
          if (opcode >= 0x20U && opcode < 0x40U &&
              scanned >= pending_words_before_append) {
            std::size_t vertex_count{};
            const auto coordinate_words =
                polygonCoordinateWords(opcode, vertex_count);
            for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
              const auto word = coordinate_words[vertex];
              const auto handle =
                  pending_projection_identities_[scanned + word];
              if (handle == 0U || handle > projection_catalog.size())
                return true;
              const auto &projection = projection_catalog[handle - 1U];
              if (!projection.valid || projection.source_vertex_id != handle ||
                  projection.packed_sxy != command[word]) {
                return true;
              }
            }
          }
          scanned += length;
        }
        return false;
      }();
  // Packed-SXY and plane recovery are diagnostic fallbacks, never production
  // provenance. A quantized screen coordinate cannot identify camera-space W.
  const auto packed_catalog_active =
      coherence_edge_snapping_enabled_ &&
      (!catalog_handle_active || catalog_recovery_demanded) &&
      rebuildPackedProjectionTable(projection_catalog);

  const auto resolve_original_projection =
      [&](std::size_t command_start, std::size_t word,
          std::span<const std::uint32_t> command)
      -> const psx::GteProjectedVertex * {
    const auto absolute_word = command_start + word;
    if (!pending_projections_.empty() &&
        absolute_word < pending_projections_.size()) {
      const auto &projection = pending_projections_[absolute_word];
      if (projection.valid && projection.packed_sxy == command[word]) {
        return &projection;
      }
    }
    if (catalog_handle_active && command_start >= pending_words_before_append &&
        absolute_word < pending_projection_identities_.size()) {
      const auto handle = pending_projection_identities_[absolute_word];
      if (handle != 0U && handle <= projection_catalog.size()) {
        const auto &projection = projection_catalog[handle - 1U];
        if (projection.valid && projection.source_vertex_id == handle &&
            projection.packed_sxy == command[word]) {
          return &projection;
        }
      }
    }
    if (packed_catalog_active && command_start >= pending_words_before_append) {
      const auto *entry = findPackedProjection(command[word]);
      if (entry != nullptr && !entry->ambiguous) {
        return &entry->projection;
      }
    }
    return nullptr;
  };

  // CPU clipping in the retail game commonly rebuilds two packet vertices
  // from integer halfwords, so their compact GTE handles are no longer
  // available. Recover those vertices only when the packet shares a complete
  // edge with an unambiguous, already precise coplanar primitive. Packed SXY
  // remains the witness while camera-space Z/W comes from the shared plane.
  // This pass is demand-only and never enables the expensive CPU-wide PGXP
  // carrier.
  plane_recovered_word_projections_.clear();
  projection_plane_edges_.clear();
  if (catalog_recovery_demanded && packed_catalog_active) {
    const auto make_plane =
        [](const std::array<psx::GteProjectedVertex, 12U> &projections,
           const std::array<std::size_t, 4U> &coordinate_words,
           std::size_t vertex_count, ProjectionPlane &plane) noexcept {
          constexpr auto camera_tolerance = 1.0F / 65536.0F;
          const auto &first = projections[coordinate_words[0U]];
          if (!canonicalProjectionWitnessValid(first))
            return false;
          for (std::size_t vertex = 1U; vertex < vertex_count; ++vertex) {
            const auto &projection = projections[coordinate_words[vertex]];
            if (!canonicalProjectionWitnessValid(projection) ||
                std::abs(projection.screen_h - first.screen_h) >
                    camera_tolerance ||
                std::abs(projection.screen_offset_x - first.screen_offset_x) >
                    camera_tolerance ||
                std::abs(projection.screen_offset_y - first.screen_offset_y) >
                    camera_tolerance) {
              return false;
            }
          }
          for (std::size_t second = 1U; second + 1U < vertex_count; ++second) {
            for (std::size_t third = second + 1U; third < vertex_count;
                 ++third) {
              const auto &b = projections[coordinate_words[second]];
              const auto &c = projections[coordinate_words[third]];
              const auto ab_x = static_cast<double>(b.view_x) - first.view_x;
              const auto ab_y = static_cast<double>(b.view_y) - first.view_y;
              const auto ab_z = static_cast<double>(b.view_z) - first.view_z;
              const auto ac_x = static_cast<double>(c.view_x) - first.view_x;
              const auto ac_y = static_cast<double>(c.view_y) - first.view_y;
              const auto ac_z = static_cast<double>(c.view_z) - first.view_z;
              auto normal_x = ab_y * ac_z - ab_z * ac_y;
              auto normal_y = ab_z * ac_x - ab_x * ac_z;
              auto normal_z = ab_x * ac_y - ab_y * ac_x;
              const auto magnitude =
                  std::sqrt(normal_x * normal_x + normal_y * normal_y +
                            normal_z * normal_z);
              if (!std::isfinite(magnitude) || magnitude <= 1.0e-7)
                continue;
              normal_x /= magnitude;
              normal_y /= magnitude;
              normal_z /= magnitude;
              if (normal_x < -1.0e-9 ||
                  (std::abs(normal_x) <= 1.0e-9 && normal_y < -1.0e-9) ||
                  (std::abs(normal_x) <= 1.0e-9 &&
                   std::abs(normal_y) <= 1.0e-9 && normal_z < 0.0)) {
                normal_x = -normal_x;
                normal_y = -normal_y;
                normal_z = -normal_z;
              }
              const auto distance = normal_x * first.view_x +
                                    normal_y * first.view_y +
                                    normal_z * first.view_z;
              if (!std::isfinite(distance))
                continue;
              plane = {normal_x,
                       normal_y,
                       normal_z,
                       distance,
                       first.screen_h,
                       first.screen_offset_x,
                       first.screen_offset_y,
                       true};
              return true;
            }
          }
          return false;
        };
    const auto planes_compatible = [](const ProjectionPlane &left,
                                      const ProjectionPlane &right) noexcept {
      if (!left.valid || !right.valid)
        return false;
      const auto normal_dot = left.normal_x * right.normal_x +
                              left.normal_y * right.normal_y +
                              left.normal_z * right.normal_z;
      const auto distance_tolerance = std::max(
          0.5,
          std::max(std::abs(left.distance), std::abs(right.distance)) * 0.002);
      return normal_dot >= 0.9995 &&
             std::abs(left.distance - right.distance) <= distance_tolerance &&
             std::abs(left.screen_h - right.screen_h) <= 1.0F / 65536.0F &&
             std::abs(left.screen_offset_x - right.screen_offset_x) <=
                 1.0F / 65536.0F &&
             std::abs(left.screen_offset_y - right.screen_offset_y) <=
                 1.0F / 65536.0F;
    };
    const auto edge_less = [](const ProjectionPlaneEdgeEntry &left,
                              const ProjectionPlaneEdgeEntry &right) noexcept {
      return std::tie(left.first_sxy, left.second_sxy, left.draw_area_top_left,
                      left.draw_area_bottom_right, left.draw_offset) <
             std::tie(right.first_sxy, right.second_sxy,
                      right.draw_area_top_left, right.draw_area_bottom_right,
                      right.draw_offset);
    };
    const auto same_edge = [](const ProjectionPlaneEdgeEntry &left,
                              const ProjectionPlaneEdgeEntry &right) noexcept {
      return left.first_sxy == right.first_sxy &&
             left.second_sxy == right.second_sxy &&
             left.draw_area_top_left == right.draw_area_top_left &&
             left.draw_area_bottom_right == right.draw_area_bottom_right &&
             left.draw_offset == right.draw_offset;
    };
    const auto make_edge = [](std::uint32_t first, std::uint32_t second,
                              const PgxpDrawContext &context,
                              const ProjectionPlane &plane) noexcept {
      if (second < first)
        std::swap(first, second);
      return ProjectionPlaneEdgeEntry{first,
                                      second,
                                      context.draw_area_top_left,
                                      context.draw_area_bottom_right,
                                      context.draw_offset,
                                      plane,
                                      false};
    };

    projection_plane_edges_.reserve(pending_.size());
    auto plane_context = currentPgxpDrawContext();
    auto plane_consumed = std::size_t{};
    while (plane_consumed < pending_.size()) {
      const auto remaining =
          std::span<const std::uint32_t>{pending_}.subspan(plane_consumed);
      const auto length = commandLength(remaining);
      if (length == 0U || remaining.size() < length)
        break;
      const auto command = remaining.first(length);
      const auto opcode = static_cast<std::uint8_t>(command.front() >> 24U);
      if (opcode >= 0x20U && opcode < 0x40U) {
        std::size_t vertex_count{};
        const auto coordinate_words =
            polygonCoordinateWords(opcode, vertex_count);
        std::array<psx::GteProjectedVertex, 12U> projections_for_plane{};
        auto complete = true;
        for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
          const auto word = coordinate_words[vertex];
          const auto *projection =
              resolve_original_projection(plane_consumed, word, command);
          if (projection == nullptr) {
            complete = false;
            break;
          }
          projections_for_plane[word] = *projection;
        }
        ProjectionPlane plane{};
        if (complete &&
            probePrecisePrimitive(
                opcode, command,
                std::span<const psx::GteProjectedVertex>{projections_for_plane}
                    .first(length)) &&
            make_plane(projections_for_plane, coordinate_words, vertex_count,
                       plane)) {
          std::size_t edge_count{};
          const auto perimeter =
              polygonPerimeterEdges(vertex_count, edge_count);
          for (std::size_t edge{}; edge < edge_count; ++edge) {
            projection_plane_edges_.push_back(
                make_edge(command[coordinate_words[perimeter[edge][0U]]],
                          command[coordinate_words[perimeter[edge][1U]]],
                          plane_context, plane));
          }
        }
      }
      updatePgxpDrawContext(plane_context, opcode, command.front());
      plane_consumed += length;
    }
    std::sort(projection_plane_edges_.begin(), projection_plane_edges_.end(),
              edge_less);
    auto compacted = std::size_t{};
    for (const auto &edge : projection_plane_edges_) {
      if (compacted != 0U &&
          same_edge(projection_plane_edges_[compacted - 1U], edge)) {
        auto &current = projection_plane_edges_[compacted - 1U];
        if (!planes_compatible(current.plane, edge.plane)) {
          current.ambiguous = true;
        }
      } else {
        projection_plane_edges_[compacted++] = edge;
      }
    }
    projection_plane_edges_.resize(compacted);

    if (!projection_plane_edges_.empty()) {
      plane_recovered_word_projections_.assign(pending_.size(), {});
      const auto find_plane_edge = [&](std::uint32_t first,
                                       std::uint32_t second,
                                       const PgxpDrawContext &context)
          -> const ProjectionPlaneEdgeEntry * {
        const auto key = make_edge(first, second, context, {});
        const auto found =
            std::lower_bound(projection_plane_edges_.begin(),
                             projection_plane_edges_.end(), key, edge_less);
        return found != projection_plane_edges_.end() && same_edge(*found, key)
                   ? &*found
                   : nullptr;
      };
      const auto reconstruct_on_plane =
          [](std::uint32_t packed, const ProjectionPlane &plane,
             psx::GteProjectedVertex &result) noexcept {
            const auto screen_x = static_cast<double>(
                static_cast<std::int16_t>(packed & 0xffffU));
            const auto screen_y =
                static_cast<double>(static_cast<std::int16_t>(packed >> 16U));
            const auto ray_x =
                (screen_x - plane.screen_offset_x) / plane.screen_h;
            const auto ray_y =
                (screen_y - plane.screen_offset_y) / plane.screen_h;
            const auto denominator = plane.normal_x * ray_x +
                                     plane.normal_y * ray_y + plane.normal_z;
            if (!std::isfinite(denominator) ||
                std::abs(denominator) <= 1.0e-9) {
              return false;
            }
            const auto depth = plane.distance / denominator;
            if (!std::isfinite(depth) || depth <= plane.screen_h * 0.5 ||
                depth > 1.0e7) {
              return false;
            }
            result = {};
            result.packed_sxy = packed;
            result.view_x = static_cast<float>(ray_x * depth);
            result.view_y = static_cast<float>(ray_y * depth);
            result.view_z = static_cast<float>(depth);
            result.screen_x = static_cast<float>(screen_x);
            result.screen_y = static_cast<float>(screen_y);
            result.screen_h = plane.screen_h;
            result.screen_offset_x = plane.screen_offset_x;
            result.screen_offset_y = plane.screen_offset_y;
            result.valid = std::isfinite(result.view_x) &&
                           std::isfinite(result.view_y) &&
                           std::isfinite(result.view_z);
            return result.valid;
          };

      auto recovery_context = currentPgxpDrawContext();
      auto recovery_consumed = std::size_t{};
      while (recovery_consumed < pending_.size()) {
        const auto remaining =
            std::span<const std::uint32_t>{pending_}.subspan(recovery_consumed);
        const auto length = commandLength(remaining);
        if (length == 0U || remaining.size() < length)
          break;
        const auto command = remaining.first(length);
        const auto opcode = static_cast<std::uint8_t>(command.front() >> 24U);
        if (opcode >= 0x20U && opcode < 0x40U) {
          std::size_t vertex_count{};
          const auto coordinate_words =
              polygonCoordinateWords(opcode, vertex_count);
          std::array<psx::GteProjectedVertex, 12U> recovered{};
          std::array<bool, 4U> missing{};
          auto missing_count = std::size_t{};
          for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
            const auto word = coordinate_words[vertex];
            const auto *projection =
                resolve_original_projection(recovery_consumed, word, command);
            if (projection == nullptr) {
              missing[vertex] = true;
              ++missing_count;
            } else {
              recovered[word] = *projection;
            }
          }
          if (missing_count != 0U) {
            const ProjectionPlane *candidate{};
            auto incompatible = false;
            std::size_t edge_count{};
            const auto perimeter =
                polygonPerimeterEdges(vertex_count, edge_count);
            for (std::size_t edge{}; edge < edge_count; ++edge) {
              const auto *entry = find_plane_edge(
                  command[coordinate_words[perimeter[edge][0U]]],
                  command[coordinate_words[perimeter[edge][1U]]],
                  recovery_context);
              if (entry == nullptr || entry->ambiguous)
                continue;
              if (candidate == nullptr) {
                candidate = &entry->plane;
              } else if (!planes_compatible(*candidate, entry->plane)) {
                incompatible = true;
                break;
              }
            }
            auto valid = candidate != nullptr && !incompatible;
            for (std::size_t vertex{}; valid && vertex < vertex_count;
                 ++vertex) {
              if (missing[vertex]) {
                valid = reconstruct_on_plane(
                    command[coordinate_words[vertex]], *candidate,
                    recovered[coordinate_words[vertex]]);
              } else {
                const auto &projection = recovered[coordinate_words[vertex]];
                const auto residual = candidate->normal_x * projection.view_x +
                                      candidate->normal_y * projection.view_y +
                                      candidate->normal_z * projection.view_z -
                                      candidate->distance;
                const auto tolerance =
                    std::max(0.5, std::abs(candidate->distance) * 0.002);
                valid =
                    std::isfinite(residual) && std::abs(residual) <= tolerance;
              }
            }
            valid =
                valid &&
                probePrecisePrimitive(
                    opcode, command,
                    std::span<const psx::GteProjectedVertex>{recovered}.first(
                        length));
            if (valid) {
              for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
                if (missing[vertex]) {
                  plane_recovered_word_projections_[recovery_consumed +
                                                    coordinate_words[vertex]] =
                      recovered[coordinate_words[vertex]];
                }
              }
              plane_recovered_vertices_ += missing_count;
              ++plane_recovered_primitives_;
            } else if (candidate != nullptr) {
              ++plane_recovery_rejected_primitives_;
            }
          }
        }
        updatePgxpDrawContext(recovery_context, opcode, command.front());
        recovery_consumed += length;
      }
    }
  }

  const auto resolve_projection = [&](std::size_t command_start,
                                      std::size_t word,
                                      std::span<const std::uint32_t> command)
      -> const psx::GteProjectedVertex * {
    const auto absolute_word = command_start + word;
    if (absolute_word < plane_recovered_word_projections_.size()) {
      const auto &projection = plane_recovered_word_projections_[absolute_word];
      if (projection.valid && projection.packed_sxy == command[word]) {
        return &projection;
      }
    }
    return resolve_original_projection(command_start, word, command);
  };

  // Only an explicit source-mesh identity may canonicalize projections across
  // primitives. Packed SXY is a quantized witness, not vertex identity; using
  // it globally was the reason precise MOHU geometry was snapped back to the
  // original integer coordinates.
  shared_mesh_policy_active_ = false;
  shared_mesh_overflow_ = false;
  const auto shared_mesh_demanded =
      shared_mesh_canonicalization_enabled_ &&
      std::ranges::any_of(projection_catalog, [](const auto &projection) {
        return projection.valid && projection.mesh_vertex_id != 0U;
      });
  if (shared_mesh_demanded) {
    beginSharedMeshTable(pending_.size());
  }
  if (shared_mesh_demanded && shared_mesh_prepass_enabled_) {
    auto mesh_context = currentPgxpDrawContext();
    auto mesh_consumed = std::size_t{};
    while (mesh_consumed < pending_.size()) {
      const auto remaining =
          std::span<const std::uint32_t>{pending_}.subspan(mesh_consumed);
      const auto length = commandLength(remaining);
      if (length == 0U || remaining.size() < length)
        break;
      const auto command = remaining.first(length);
      const auto opcode = static_cast<std::uint8_t>(command.front() >> 24U);
      if (opcode >= 0x20U && opcode < 0x40U) {
        std::array<psx::GteProjectedVertex, 12U> mesh_projections{};
        std::size_t vertex_count{};
        const auto coordinate_words =
            polygonCoordinateWords(opcode, vertex_count);
        auto complete = true;
        std::size_t missing_count{};
        std::size_t missing_vertex{};
        for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
          const auto word = coordinate_words[vertex];
          const auto *projection =
              resolve_projection(mesh_consumed, word, command);
          if (projection == nullptr) {
            complete = false;
            ++missing_count;
            missing_vertex = vertex;
          } else {
            mesh_projections[word] = *projection;
          }
        }
        if (quad_recovery_enabled_ && missing_count == 1U &&
            vertex_count == 4U) {
          const auto quad_words = std::array<std::size_t, 4U>{
              coordinate_words[0U], coordinate_words[1U], coordinate_words[2U],
              coordinate_words[3U]};
          complete = reconstructMissingQuadProjection(
              command, quad_words, mesh_projections, missing_vertex);
        }
        const auto mesh_span =
            std::span<const psx::GteProjectedVertex>{mesh_projections}.first(
                length);
        const auto precise =
            complete && probePrecisePrimitive(opcode, command, mesh_span);
        for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
          const auto word = coordinate_words[vertex];
          recordSharedMeshVertex(mesh_context.draw_offset,
                                 precise ? &mesh_projections[word] : nullptr,
                                 precise);
        }
      }
      updatePgxpDrawContext(mesh_context, opcode, command.front());
      mesh_consumed += length;
    }
  }
  // The transport emits nonzero identities only for coordinate words. Most
  // retail frames already carry complete sidecars for every one of them, so
  // avoid constructing or probing the canonical table unless a coordinate
  // actually needs recovery. The full projection validation still happens in
  // preparePrecisePrimitive.
  coherence_policy_active_ = false;
  coherence_edge_active_ = false;
  coherence_edge_near_only_ = false;
  auto identity_recovery_active = false;
  if (identity_transport_active && !catalog_handle_active &&
      !pending_projections_.empty()) {
    const auto aligned_size =
        std::min({pending_.size(), pending_projections_.size(),
                  pending_projection_identities_.size()});
    for (std::size_t index{}; index < aligned_size; ++index) {
      const auto identity = pending_projection_identities_[index];
      const auto &projection = pending_projections_[index];
      if (identity != 0U && (!projection.valid || !projection.pgxpEligible() ||
                             projection.packed_sxy != pending_[index])) {
        identity_recovery_active = true;
        break;
      }
    }
  }
  if (identity_recovery_active) {
    rebuildCanonicalProjectionTable();
  }
  // Retail clipping can leave one raw packet next to a catalog-resolved
  // packet. Give their shared edge one screen position while preserving the
  // precise polygon's Z/W for perspective-correct interpolation.
  if (coherence_edge_snapping_enabled_ && catalog_recovery_demanded &&
      packed_catalog_active && !identity_recovery_active) {
    prepareCoherenceEdgePolicy(true, pending_words_before_append, true, true);
  } else if (!catalog_handle_active && packed_catalog_active &&
             !identity_recovery_active) {
    prepareCoherenceEdgePolicy(true, pending_words_before_append, false, false);
  } else if (coherence_edge_snapping_enabled_ && !identity_recovery_active &&
             std::ranges::any_of(pending_projections_,
                                 [](const auto &projection) {
                                   return projection.valid &&
                                          projection.exact_transform &&
                                          std::isfinite(projection.view_z) &&
                                          projection.view_z <=
                                              minimum_legacy_precise_view_depth;
                                 }) &&
             coherencePolicyDemanded(true)) {
    // The normal path stays single-pass. Only a close exact primitive next
    // to a genuinely missing sidecar pays for the edge table.
    prepareCoherenceEdgePolicy(false, 0U, true, true);
  } else if (coherence_edge_snapping_enabled_ &&
             !coherence_near_only_enabled_ && !identity_recovery_active &&
             coherencePolicyDemanded(false)) {
    prepareCoherenceEdgePolicy(false, 0U, false, false);
  }

  auto shared_mesh_context = currentPgxpDrawContext();
  auto consumed = std::size_t{};
  while (consumed < pending_.size()) {
    const auto remaining =
        std::span<const std::uint32_t>{pending_}.subspan(consumed);
    const auto length = commandLength(remaining);
    if (length == 0U || remaining.size() < length) {
      break;
    }
    const auto command = remaining.first(length);
    const auto opcode = static_cast<std::uint8_t>(command.front() >> 24U);
    auto command_projections = std::span<const psx::GteProjectedVertex>{};
    if (!pending_projections_.empty()) {
      command_projections =
          std::span<const psx::GteProjectedVertex>{pending_projections_}
              .subspan(consumed, length);
    }
    const auto polygon = opcode >= 0x20U && opcode < 0x40U;

    // SF's stable world path emits every shared source vertex from one exact
    // projection record. Recreate that invariant for raw GP0 packets using
    // transported source identities. SXY is only a witness; it is never a
    // lookup key and can never lend another vertex its Z/W.
    std::array<psx::GteProjectedVertex, 12U> resolved_storage{};
    auto resolved_projections = command_projections;
    auto catalog_resolution_failed = false;
    auto catalog_ambiguous = false;
    auto catalog_resolved = false;
    auto identity_resolution_failed = false;
    auto identity_ambiguous = false;
    auto recovered_vertices = std::size_t{};
    if (polygon && !command_projections.empty() &&
        (catalog_handle_active || packed_catalog_active) &&
        consumed >= pending_words_before_append) {
      std::copy(command_projections.begin(), command_projections.end(),
                resolved_storage.begin());
      std::size_t vertex_count{};
      const auto coordinate_words =
          polygonCoordinateWords(opcode, vertex_count);
      std::size_t missing_count{};
      std::size_t missing_vertex{};
      for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
        const auto word = coordinate_words[vertex];
        const auto &direct = command_projections[word];
        if (direct.valid && direct.packed_sxy == command[word])
          continue;
        const auto *resolved = resolve_projection(consumed, word, command);
        if (resolved == nullptr) {
          ++missing_count;
          missing_vertex = vertex;
          const auto *entry = findPackedProjection(command[word]);
          catalog_ambiguous =
              catalog_ambiguous || (entry != nullptr && entry->ambiguous);
          continue;
        }
        resolved_storage[word] = *resolved;
        ++recovered_vertices;
      }
      if (quad_recovery_enabled_ && missing_count == 1U &&
          vertex_count == 4U) {
        const auto quad_words = std::array<std::size_t, 4U>{
            coordinate_words[0U], coordinate_words[1U], coordinate_words[2U],
            coordinate_words[3U]};
        if (reconstructMissingQuadProjection(
                command, quad_words, resolved_storage, missing_vertex)) {
          missing_count = 0U;
          ++recovered_vertices;
          ++plane_recovered_vertices_;
          ++plane_recovered_primitives_;
        } else {
          ++plane_recovery_rejected_primitives_;
        }
      }
      catalog_resolution_failed = missing_count != 0U;
      if (!catalog_resolution_failed) {
        resolved_projections =
            std::span<const psx::GteProjectedVertex>{resolved_storage}.first(
                length);
        catalog_resolved = recovered_vertices != 0U;
      }
    } else if (polygon && command_projections.empty() &&
               catalog_handle_active &&
               consumed >= pending_words_before_append) {
      std::size_t vertex_count{};
      const auto coordinate_words =
          polygonCoordinateWords(opcode, vertex_count);
      const auto command_identities =
          std::span<const std::uint64_t>{pending_projection_identities_}
              .subspan(consumed, length);
      std::size_t directly_missing_count{};
      std::size_t missing_count{};
      std::size_t missing_vertex{};
      for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
        const auto word = coordinate_words[vertex];
        const auto handle = command_identities[word];
        const auto direct_catalog_witness = [&] {
          if (handle == 0U || handle > projection_catalog.size()) {
            return false;
          }
          const auto &projection = projection_catalog[handle - 1U];
          return projection.valid && projection.source_vertex_id == handle &&
                 projection.packed_sxy == command[word];
        }();
        directly_missing_count += direct_catalog_witness ? 0U : 1U;

        const auto *resolved = resolve_projection(consumed, word, command);
        if (resolved != nullptr) {
          resolved_storage[word] = *resolved;
          if (!direct_catalog_witness) {
            ++recovered_vertices;
          }
          continue;
        }
        if (handle != 0U && handle <= projection_catalog.size()) {
          const auto &projection = projection_catalog[handle - 1U];
          if (projection.valid && projection.source_vertex_id == handle &&
              projection.packed_sxy == command[word]) {
            resolved_storage[word] = projection;
            continue;
          }
        }
        const auto *canonical = packed_catalog_active
                                    ? findPackedProjection(command[word])
                                    : nullptr;
        if (canonical == nullptr || canonical->ambiguous) {
          ++missing_count;
          missing_vertex = vertex;
          catalog_ambiguous = catalog_ambiguous ||
                              (canonical != nullptr && canonical->ambiguous);
          continue;
        }
        resolved_storage[word] = canonical->projection;
        ++recovered_vertices;
      }
      ++missing_projection_vertex_buckets_[std::min(
          directly_missing_count,
          missing_projection_vertex_buckets_.size() - 1U)];
      if (quad_recovery_enabled_ && packed_catalog_active &&
          missing_count == 1U && vertex_count == 4U) {
        const auto quad_words = std::array<std::size_t, 4U>{
            coordinate_words[0U], coordinate_words[1U], coordinate_words[2U],
            coordinate_words[3U]};
        if (reconstructMissingQuadProjection(
                command, quad_words, resolved_storage, missing_vertex)) {
          missing_count = 0U;
          ++recovered_vertices;
          ++plane_recovered_vertices_;
          ++plane_recovered_primitives_;
        } else {
          ++plane_recovery_rejected_primitives_;
        }
      }
      catalog_resolution_failed = missing_count != 0U;
      resolved_projections =
          std::span<const psx::GteProjectedVertex>{resolved_storage}.first(
              length);
      if (!catalog_resolution_failed) {
        catalog_resolved = true;
      }
    } else if (polygon && command_projections.empty() &&
               packed_catalog_active &&
               consumed >= pending_words_before_append) {
      std::size_t vertex_count{};
      const auto coordinate_words =
          polygonCoordinateWords(opcode, vertex_count);
      for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
        const auto word = coordinate_words[vertex];
        const auto *entry = findPackedProjection(command[word]);
        if (entry == nullptr || entry->ambiguous) {
          catalog_resolution_failed = true;
          catalog_ambiguous = entry != nullptr && entry->ambiguous;
          break;
        }
        resolved_storage[word] = entry->projection;
      }
      if (!catalog_resolution_failed) {
        resolved_projections =
            std::span<const psx::GteProjectedVertex>{resolved_storage}.first(
                length);
        catalog_resolved = true;
      }
    }
    if (polygon && identity_recovery_active) {
      std::copy(command_projections.begin(), command_projections.end(),
                resolved_storage.begin());
      resolved_projections =
          std::span<const psx::GteProjectedVertex>{resolved_storage}.first(
              length);
      std::size_t vertex_count{};
      const auto coordinate_words =
          polygonCoordinateWords(opcode, vertex_count);
      const auto command_identities =
          std::span<const std::uint64_t>{pending_projection_identities_}
              .subspan(consumed, length);
      for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
        const auto word = coordinate_words[vertex];
        const auto identity = command_identities[word];
        if (identity == 0U) {
          continue;
        }
        const auto *canonical = findCanonicalProjection(identity);
        if (canonical == nullptr || canonical->ambiguous ||
            canonical->projection.packed_sxy != command[word]) {
          identity_resolution_failed = true;
          identity_ambiguous = canonical != nullptr && canonical->ambiguous;
          break;
        }
        recovered_vertices += !resolved_storage[word].valid ? 1U : 0U;
        resolved_storage[word] = canonical->projection;
      }
    }

    PreciseRejectReason reject_reason{};
    auto precise_candidate = false;
    if (polygon && !identity_resolution_failed && coherence_policy_active_ &&
        consumed < coherence_precise_status_.size() &&
        coherence_precise_status_[consumed] != 0U) {
      precise_candidate = coherence_precise_status_[consumed] == 1U;
      reject_reason =
          static_cast<PreciseRejectReason>(coherence_reject_reasons_[consumed]);
    } else {
      precise_candidate =
          polygon && !identity_resolution_failed &&
          !catalog_resolution_failed &&
          probePrecisePrimitive(opcode, command, resolved_projections,
                                &reject_reason,
                                projective_depth_enabled_);
    }

    if (precise_candidate && shared_mesh_policy_active_) {
      std::copy(resolved_projections.begin(), resolved_projections.end(),
                resolved_storage.begin());
      std::size_t vertex_count{};
      const auto coordinate_words =
          polygonCoordinateWords(opcode, vertex_count);
      for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
        const auto word = coordinate_words[vertex];
        const auto mesh_identity =
            sharedMeshProjectionIdentity(resolved_storage[word]);
        if (mesh_identity == 0U) {
          ++shared_mesh_fractional_vertices_;
          continue;
        }
        if (!shared_mesh_prepass_enabled_) {
          recordSharedMeshVertex(shared_mesh_context.draw_offset,
                                 &resolved_storage[word], true);
        }
        const auto *mesh_vertex = findSharedMeshVertex(
            mesh_identity, shared_mesh_context.draw_offset);
        if (mesh_vertex != nullptr && mesh_vertex->has_projection &&
            !mesh_vertex->ambiguous) {
          resolved_storage[word] = mesh_vertex->projection;
          ++shared_mesh_fractional_vertices_;
        } else {
          ++shared_mesh_packet_fallback_vertices_;
          ++shared_mesh_fractional_vertices_;
        }
      }
      resolved_projections =
          std::span<const psx::GteProjectedVertex>{resolved_storage}.first(
              length);
    }

    if (precise_candidate && coherence_edge_active_) {
      std::size_t vertex_count{};
      const auto coordinate_words =
          polygonCoordinateWords(opcode, vertex_count);
      auto near_exact_visible = !coherence_edge_near_only_;
      if (coherence_edge_near_only_) {
        auto has_near_exact = false;
        auto maximum_depth = -std::numeric_limits<float>::infinity();
        for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
          const auto &projection =
              resolved_projections[coordinate_words[vertex]];
          has_near_exact =
              has_near_exact ||
              (projection.exact_transform &&
               projection.view_z <= minimum_legacy_precise_view_depth);
          maximum_depth = std::max(maximum_depth, projection.view_z);
        }
        near_exact_visible = has_near_exact &&
                             maximum_depth >= minimum_legacy_precise_view_depth;
      }
      if (near_exact_visible) {
        std::size_t edge_count{};
        const auto perimeter = polygonPerimeterEdges(vertex_count, edge_count);
        std::array<bool, 4U> snap_vertex{};
        for (std::size_t edge{}; edge < edge_count; ++edge) {
          const auto first_vertex = perimeter[edge][0];
          const auto second_vertex = perimeter[edge][1];
          if (hasFallbackEdge(command[coordinate_words[first_vertex]],
                              command[coordinate_words[second_vertex]],
                              coherence_draw_contexts_[consumed])) {
            snap_vertex[first_vertex] = true;
            snap_vertex[second_vertex] = true;
          }
        }
        const auto requested_vertices = static_cast<std::size_t>(std::count(
            snap_vertex.begin(),
            snap_vertex.begin() + static_cast<std::ptrdiff_t>(vertex_count),
            true));
        if (requested_vertices != 0U) {
          std::copy(resolved_projections.begin(), resolved_projections.end(),
                    resolved_storage.begin());
          auto snapped_vertices = std::size_t{};
          for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
            if (snap_vertex[vertex]) {
              auto &projection = resolved_storage[coordinate_words[vertex]];
              if (coherence_edge_near_only_) {
                snapped_vertices +=
                    snapExactProjectionToPacket(projection) ? 1U : 0U;
              } else {
                snapProjectionToPacket(projection);
                ++snapped_vertices;
              }
            }
          }
          if (snapped_vertices != 0U) {
            resolved_projections =
                std::span<const psx::GteProjectedVertex>{resolved_storage}
                    .first(length);
            ++coherence_snapped_primitives_;
            coherence_snapped_vertices_ += snapped_vertices;
          }
        }
      }
    }
    if (polygon && identity_resolution_failed) {
      reject_reason = PreciseRejectReason::missing;
      if (identity_ambiguous) {
        ++identity_ambiguous_primitives_;
      }
    }
    if (polygon && catalog_resolution_failed) {
      reject_reason = PreciseRejectReason::missing;
      if (catalog_ambiguous) {
        ++projection_catalog_ambiguities_;
      }
    }
    if (polygon && precise_candidate && catalog_handle_active) {
      std::size_t vertex_count{};
      const auto coordinate_words =
          polygonCoordinateWords(opcode, vertex_count);
      auto fully_exact = true;
      auto fully_enhanced = true;
      auto enhanced_rotation = true;
      auto enhanced_translation = true;
      auto enhanced_vector = true;
      auto exact_view_depth = !projective_depth_enabled_;
      for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
        const auto word = coordinate_words[vertex];
        if (word >= resolved_projections.size()) {
          fully_exact = fully_enhanced = enhanced_rotation = false;
          enhanced_translation = enhanced_vector = exact_view_depth = false;
          continue;
        }
        const auto &projection = resolved_projections[word];
        fully_exact = fully_exact && projection.exact_transform;
        fully_enhanced = fully_enhanced && projection.fractional_transform;
        enhanced_rotation = enhanced_rotation &&
                            (projection.enhanced_sources &
                             psx::GteProjectedVertex::enhanced_rotation) != 0U;
        enhanced_translation =
            enhanced_translation &&
            (projection.enhanced_sources &
             psx::GteProjectedVertex::enhanced_translation) != 0U;
        enhanced_vector =
            enhanced_vector && (projection.enhanced_sources &
                                psx::GteProjectedVertex::enhanced_vector) != 0U;
        exact_view_depth = exact_view_depth && projection.exact_transform;
      }
      if (fully_exact) {
        ++exact_precise_primitives_;
        enhanced_precise_primitives_ += fully_enhanced ? 1U : 0U;
        enhanced_rotation_primitives_ += enhanced_rotation ? 1U : 0U;
        enhanced_translation_primitives_ += enhanced_translation ? 1U : 0U;
        enhanced_vector_primitives_ += enhanced_vector ? 1U : 0U;
        exact_view_depth_primitives_ += exact_view_depth ? 1U : 0U;
      } else {
        ++integer_precise_primitives_;
      }
    }
    if (precise_candidate) {
      identity_recovered_vertices_ += recovered_vertices;
      identity_recovered_primitives_ += recovered_vertices != 0U ? 1U : 0U;
      projection_catalog_primitives_ += catalog_resolved ? 1U : 0U;
    }
    if (polygon && !precise_candidate) {
      switch (reject_reason) {
      case PreciseRejectReason::missing:
        ++missing_projection_primitives_;
        ++missing_projection_by_opcode_[opcode - 0x20U];
        break;
      case PreciseRejectReason::nonfinite:
        ++nonfinite_projection_primitives_;
        break;
      case PreciseRejectReason::packet_mismatch:
        ++packet_mismatch_primitives_;
        break;
      case PreciseRejectReason::hazard:
        ++projection_hazard_primitives_;
        break;
      case PreciseRejectReason::ir_saturation:
        ++projection_hazard_primitives_;
        ++ir_saturation_primitives_;
        break;
      case PreciseRejectReason::depth_saturation:
        ++projection_hazard_primitives_;
        ++depth_saturation_primitives_;
        break;
      case PreciseRejectReason::divide_overflow:
        ++projection_hazard_primitives_;
        ++divide_overflow_primitives_;
        break;
      case PreciseRejectReason::screen_saturation:
        ++projection_hazard_primitives_;
        ++screen_saturation_primitives_;
        break;
      case PreciseRejectReason::screen_mismatch:
        ++screen_mismatch_primitives_;
        break;
      case PreciseRejectReason::reprojection_mismatch:
        ++reprojection_mismatch_primitives_;
        break;
      case PreciseRejectReason::camera_mismatch:
        ++camera_mismatch_primitives_;
        break;
      case PreciseRejectReason::near_plane:
        ++near_plane_primitives_;
        break;
      case PreciseRejectReason::none:
        break;
      }
    }
    const auto hard_geometry_reject =
        polygon && requiresRawPacketFallback(reject_reason);
    // W is primitive-atomic, but verified per-vertex screen positions remain
    // useful when one handle or one projection context is unavailable. This
    // matches DuckStation's default PGXP contract: all vertices use affine W
    // after any W failure, while address/raw-validated XY stays precise. It
    // keeps shared edges anchored instead of alternating an entire polygon
    // between precise and quantized packet coordinates.
    const auto dispatch_projections =
        hard_geometry_reject ||
                (polygon && catalog_handle_active &&
                 !coherence_edge_snapping_enabled_ &&
                 !atomic_fallback_enabled_ && !precise_candidate)
            ? std::span<const psx::GteProjectedVertex>{}
            : resolved_projections;
    const auto allow_perspective = geometry_enabled_ &&
                                   perspective_correction_enabled_ &&
                                   precise_candidate;
    const auto allow_precise_screen =
        geometry_enabled_ && precise_screen_position_enabled_;
    dispatch(command, dispatch_projections, precise_candidate,
             allow_perspective, allow_precise_screen,
             projective_depth_enabled_);
    ++submitted_commands_;
    updatePgxpDrawContext(shared_mesh_context, opcode, command.front());
    consumed += length;
  }
  if (consumed != 0U) {
    pending_.erase(pending_.begin(),
                   pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
    if (!pending_projections_.empty()) {
      pending_projections_.erase(pending_projections_.begin(),
                                 pending_projections_.begin() +
                                     static_cast<std::ptrdiff_t>(consumed));
    }
    if (!pending_projection_identities_.empty()) {
      pending_projection_identities_.erase(
          pending_projection_identities_.begin(),
          pending_projection_identities_.begin() +
              static_cast<std::ptrdiff_t>(consumed));
    }
    if (!pending_dma_sources_.empty()) {
      pending_dma_sources_.erase(
          pending_dma_sources_.begin(),
          pending_dma_sources_.begin() +
              static_cast<std::ptrdiff_t>(consumed));
    }
  }
  if (pending_.size() > maximum_buffered_words) {
    unsupported_commands_ += pending_.size();
    pending_.clear();
    pending_projections_.clear();
    pending_projection_identities_.clear();
    pending_dma_sources_.clear();
  }
}

} // namespace sf::platform::detail
