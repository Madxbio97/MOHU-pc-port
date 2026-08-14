#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace sf::platform::detail {

struct PgxpDrawContext {
  std::uint32_t draw_area_top_left{};
  std::uint32_t draw_area_bottom_right{};
  std::uint32_t draw_offset{};
  std::uint32_t draw_mode{};

  friend bool operator==(const PgxpDrawContext &,
                         const PgxpDrawContext &) = default;
};

struct PgxpPreciseVertex {
  float screen_x{};
  float screen_y{};
  float view_z{};
  bool valid{};
};

struct PgxpPolygonCandidate {
  std::array<std::uint32_t, 4U> packed_xy{};
  std::array<PgxpPreciseVertex, 4U> precise_vertices{};
  std::uint8_t vertex_count{};
  PgxpDrawContext context{};
  bool precise_eligible{};
};

struct PgxpPolygonDecision {
  bool allow_precise{};
  bool coherence_fallback{};
};

namespace pgxp_coherence_detail {

struct LineKey {
  std::int32_t direction_x{};
  std::int32_t direction_y{};
  std::int64_t intercept{};
  PgxpDrawContext context{};

  friend bool operator==(const LineKey &, const LineKey &) = default;
};

struct EdgeRecord {
  LineKey key{};
  std::int64_t start{};
  std::int64_t end{};
  std::size_t polygon{};
  PgxpPreciseVertex precise_start{};
  PgxpPreciseVertex precise_end{};
};

[[nodiscard]] inline bool lineKeyLess(const LineKey &left,
                                      const LineKey &right) noexcept {
  if (left.context.draw_area_top_left != right.context.draw_area_top_left) {
    return left.context.draw_area_top_left < right.context.draw_area_top_left;
  }
  if (left.context.draw_area_bottom_right !=
      right.context.draw_area_bottom_right) {
    return left.context.draw_area_bottom_right <
           right.context.draw_area_bottom_right;
  }
  if (left.context.draw_offset != right.context.draw_offset) {
    return left.context.draw_offset < right.context.draw_offset;
  }
  if (left.context.draw_mode != right.context.draw_mode) {
    return left.context.draw_mode < right.context.draw_mode;
  }
  if (left.direction_x != right.direction_x) {
    return left.direction_x < right.direction_x;
  }
  if (left.direction_y != right.direction_y) {
    return left.direction_y < right.direction_y;
  }
  return left.intercept < right.intercept;
}

struct ScreenPoint {
  std::int32_t x{};
  std::int32_t y{};
};

[[nodiscard]] inline ScreenPoint unpackPoint(std::uint32_t packed) noexcept {
  return {
      static_cast<std::int16_t>(packed & 0xffffU),
      static_cast<std::int16_t>((packed >> 16U) & 0xffffU),
  };
}

struct PreciseSample {
  float screen_x{};
  float screen_y{};
  float view_z{};
};

[[nodiscard]] inline PreciseSample sampleEdge(const EdgeRecord &edge,
                                              std::int64_t position) noexcept {
  const auto extent = static_cast<float>(edge.end - edge.start);
  const auto amount = static_cast<float>(position - edge.start) / extent;
  const auto interpolate = [amount](float first, float second) {
    return first + (second - first) * amount;
  };
  return {
      interpolate(edge.precise_start.screen_x, edge.precise_end.screen_x),
      interpolate(edge.precise_start.screen_y, edge.precise_end.screen_y),
      interpolate(edge.precise_start.view_z, edge.precise_end.view_z),
  };
}

[[nodiscard]] inline bool depthCompatible(float left, float right) noexcept {
  constexpr auto absolute_tolerance = 1.0F / 4096.0F;
  constexpr auto relative_tolerance = 1.0F / 65536.0F;
  const auto tolerance =
      std::max(absolute_tolerance,
               std::max(std::abs(left), std::abs(right)) * relative_tolerance);
  return std::isfinite(left) && std::isfinite(right) &&
         std::abs(left - right) <= tolerance;
}

[[nodiscard]] inline bool
preciseEdgesCompatible(const EdgeRecord &left, const EdgeRecord &right,
                       std::int64_t overlap_start,
                       std::int64_t overlap_end) noexcept {
  if (!left.precise_start.valid || !left.precise_end.valid ||
      !right.precise_start.valid || !right.precise_end.valid) {
    return true;
  }
  constexpr auto screen_tolerance = 1.0F / 16.0F;
  const auto samples_match = [screen_tolerance](const PreciseSample &first,
                                                const PreciseSample &second) {
    return std::isfinite(first.screen_x) && std::isfinite(first.screen_y) &&
           std::isfinite(second.screen_x) && std::isfinite(second.screen_y) &&
           std::abs(first.screen_x - second.screen_x) <= screen_tolerance &&
           std::abs(first.screen_y - second.screen_y) <= screen_tolerance;
  };
  if (!samples_match(sampleEdge(left, overlap_start),
                     sampleEdge(right, overlap_start)) ||
      !samples_match(sampleEdge(left, overlap_end),
                     sampleEdge(right, overlap_end))) {
    return false;
  }

  // Only compare depth where both packet edges name the identical packed
  // endpoint. View Z is not affine in screen space, so interpolating it for a
  // clipped subsegment would reject valid perspective geometry.
  return (left.start != right.start ||
          depthCompatible(left.precise_start.view_z,
                          right.precise_start.view_z)) &&
         (left.start != right.end ||
          depthCompatible(left.precise_start.view_z,
                          right.precise_end.view_z)) &&
         (left.end != right.start ||
          depthCompatible(left.precise_end.view_z,
                          right.precise_start.view_z)) &&
         (left.end != right.end ||
          depthCompatible(left.precise_end.view_z, right.precise_end.view_z));
}

} // namespace pgxp_coherence_detail

[[nodiscard]] inline std::vector<PgxpPolygonDecision>
planPgxpPolygonCoherence(std::span<const PgxpPolygonCandidate> polygons) {
  std::vector<PgxpPolygonDecision> decisions(polygons.size());
  if (polygons.empty()) {
    return decisions;
  }


  using pgxp_coherence_detail::EdgeRecord;
  std::vector<EdgeRecord> edges;
  edges.reserve(polygons.size() * 4U);
  constexpr std::array<std::array<std::uint8_t, 2U>, 3U> triangle_edges{{
      {0U, 1U},
      {1U, 2U},
      {2U, 0U},
  }};
  // PS1 quads are a strip: packet vertices 0,1,2,3 have the boundary
  // 0-1-3-2. The 1-2 diagonal is internal and must not connect meshes.
  constexpr std::array<std::array<std::uint8_t, 2U>, 4U> quad_edges{{
      {0U, 1U},
      {1U, 3U},
      {3U, 2U},
      {2U, 0U},
  }};

  for (std::size_t polygon_index{}; polygon_index < polygons.size();
       ++polygon_index) {
    const auto &polygon = polygons[polygon_index];
    const auto add_edge = [&](std::uint8_t first_vertex,
                              std::uint8_t second_vertex) {
      using pgxp_coherence_detail::unpackPoint;
      const auto first = unpackPoint(polygon.packed_xy[first_vertex]);
      const auto second = unpackPoint(polygon.packed_xy[second_vertex]);
      auto direction_x = second.x - first.x;
      auto direction_y = second.y - first.y;
      if (direction_x == 0 && direction_y == 0) {
        return;
      }
      const auto absolute_x = direction_x < 0 ? -direction_x : direction_x;
      const auto absolute_y = direction_y < 0 ? -direction_y : direction_y;
      const auto divisor = std::gcd(absolute_x, absolute_y);
      direction_x /= divisor;
      direction_y /= divisor;
      if (direction_x < 0 || (direction_x == 0 && direction_y < 0)) {
        direction_x = -direction_x;
        direction_y = -direction_y;
      }
      const auto intercept = static_cast<std::int64_t>(direction_x) * first.y -
                             static_cast<std::int64_t>(direction_y) * first.x;
      const auto first_position =
          static_cast<std::int64_t>(direction_x) * first.x +
          static_cast<std::int64_t>(direction_y) * first.y;
      const auto second_position =
          static_cast<std::int64_t>(direction_x) * second.x +
          static_cast<std::int64_t>(direction_y) * second.y;
      auto precise_first = polygon.precise_vertices[first_vertex];
      auto precise_second = polygon.precise_vertices[second_vertex];
      auto start = first_position;
      auto end = second_position;
      if (end < start) {
        std::swap(start, end);
        std::swap(precise_first, precise_second);
      }
      edges.push_back({{direction_x, direction_y, intercept, polygon.context},
                       start,
                       end,
                       polygon_index,
                       precise_first,
                       precise_second});
    };

    if (polygon.vertex_count == 3U) {
      for (const auto &edge : triangle_edges) {
        add_edge(edge[0], edge[1]);
      }
    } else if (polygon.vertex_count == 4U) {
      for (const auto &edge : quad_edges) {
        add_edge(edge[0], edge[1]);
      }
    }
  }

  std::sort(edges.begin(), edges.end(),
            [](const EdgeRecord &left, const EdgeRecord &right) {
              if (pgxp_coherence_detail::lineKeyLess(left.key, right.key)) {
                return true;
              }
              if (pgxp_coherence_detail::lineKeyLess(right.key, left.key)) {
                return false;
              }
              if (left.start != right.start) {
                return left.start < right.start;
              }
              if (left.end != right.end) {
                return left.end < right.end;
              }
              return left.polygon < right.polygon;
            });
  std::vector<std::uint8_t> polygon_consistent(polygons.size(), 1U);
  std::vector<std::size_t> active_edges;
  active_edges.reserve(edges.size());
  for (std::size_t first{}; first < edges.size();) {
    auto last = first + 1U;
    while (last < edges.size() && edges[last].key == edges[first].key) {
      ++last;
    }
    active_edges.clear();
    for (auto edge = first; edge < last; ++edge) {
      // A point-only touch is not a shared edge. Positive overlap also joins
      // software-clipped subsegments whose endpoints differ from the original
      // boundary, without the broad false coupling of shared-vertex matching.
      auto retained_edges = std::size_t{};
      for (const auto previous : active_edges) {
        if (edges[previous].end <= edges[edge].start) {
          continue;
        }
        active_edges[retained_edges++] = previous;
        const auto overlap_start =
            std::max(edges[previous].start, edges[edge].start);
        const auto overlap_end =
            std::min(edges[previous].end, edges[edge].end);
        const auto previous_polygon = edges[previous].polygon;
        const auto current_polygon = edges[edge].polygon;
        // Coherence is deliberately edge-local. A legacy/clipped neighbour is
        // not allowed to demote a whole transitive wall component from precise
        // to packed coordinates on alternating frames. Only two independently
        // precise polygons which disagree about their actual shared edge are
        // downgraded together.
        if (polygons[previous_polygon].precise_eligible &&
            polygons[current_polygon].precise_eligible &&
            !pgxp_coherence_detail::preciseEdgesCompatible(
                edges[previous], edges[edge], overlap_start, overlap_end)) {
          polygon_consistent[previous_polygon] = 0U;
          polygon_consistent[current_polygon] = 0U;
        }
      }
      active_edges.resize(retained_edges);
      active_edges.push_back(edge);
    }
    first = last;
  }

  for (std::size_t index{}; index < polygons.size(); ++index) {
    const auto allow =
        polygons[index].precise_eligible && polygon_consistent[index] != 0U;
    decisions[index] = {
        .allow_precise = allow,
        .coherence_fallback = polygons[index].precise_eligible && !allow,
    };
  }
  return decisions;
}

} // namespace sf::platform::detail
