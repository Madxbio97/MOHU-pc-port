#include "mohu/native_scene_pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace mohu {
namespace {

[[nodiscard]] constexpr std::size_t edgeHash(std::uint64_t edge) noexcept {
  edge ^= edge >> 33U;
  edge *= 0xff51afd7ed558ccdULL;
  edge ^= edge >> 33U;
  return static_cast<std::size_t>(edge);
}

[[nodiscard]] constexpr std::uint64_t edgeKey(std::uint32_t first,
                                              std::uint32_t second) noexcept {
  const auto low = std::min(first, second);
  const auto high = std::max(first, second);
  return static_cast<std::uint64_t>(low) |
         (static_cast<std::uint64_t>(high) << 32U);
}

void beginEdgeTable(std::size_t primitive_count,
                    NativeScenePipelineFrame &frame) {
  auto required = std::size_t{16U};
  const auto requested = primitive_count * 8U;
  while (required < requested) {
    required *= 2U;
  }
  if (frame.edge_table.size() < required) {
    frame.edge_table.resize(required);
    std::fill(frame.edge_table.begin(), frame.edge_table.end(),
              NativeClipEdgeSlot{});
    frame.edge_table_generation = 1U;
  } else if (++frame.edge_table_generation == 0U) {
    std::fill(frame.edge_table.begin(), frame.edge_table.end(),
              NativeClipEdgeSlot{});
    frame.edge_table_generation = 1U;
  }
  frame.cached_edges = 0U;
}

[[nodiscard]] NativeClippedVertex
makeClipVertex(const NativeSceneVertex &source) noexcept {
  return {
      .u = static_cast<float>(source.u),
      .v = static_cast<float>(source.v),
      .red = static_cast<float>(source.red),
      .green = static_cast<float>(source.green),
      .blue = static_cast<float>(source.blue),
      .position_index = source.position_index,
  };
}

[[nodiscard]] NativeClippedVertex
interpolateVertex(const NativeClippedVertex &first,
                  const NativeClippedVertex &second, double amount,
                  std::uint32_t position_index) noexcept {
  const auto interpolate = [amount](float left, float right) {
    return static_cast<float>(static_cast<double>(left) +
                              (static_cast<double>(right) - left) * amount);
  };
  return {
      .u = interpolate(first.u, second.u),
      .v = interpolate(first.v, second.v),
      .red = interpolate(first.red, second.red),
      .green = interpolate(first.green, second.green),
      .blue = interpolate(first.blue, second.blue),
      .position_index = position_index,
  };
}

[[nodiscard]] std::uint32_t resolveIntersection(NativeScenePipelineFrame &frame,
                                                std::uint32_t first_index,
                                                std::uint32_t second_index,
                                                float near_plane,
                                                double &canonical_amount) {
  const auto low_index = std::min(first_index, second_index);
  const auto high_index = std::max(first_index, second_index);
  const auto &low = frame.positions[low_index];
  const auto &high = frame.positions[high_index];
  const auto denominator =
      static_cast<double>(high.view_z) - static_cast<double>(low.view_z);
  canonical_amount = std::clamp(
      (static_cast<double>(near_plane) - low.view_z) / denominator, 0.0, 1.0);

  const auto key = edgeKey(first_index, second_index);
  const auto mask = frame.edge_table.size() - 1U;
  auto slot_index = edgeHash(key) & mask;
  while (frame.edge_table[slot_index].generation ==
         frame.edge_table_generation) {
    const auto &slot = frame.edge_table[slot_index];
    if (slot.edge_key == key) {
      ++frame.stats.reused_intersections;
      return slot.position_index;
    }
    slot_index = (slot_index + 1U) & mask;
  }

  const auto interpolate = [canonical_amount](float first, float second) {
    return static_cast<float>(static_cast<double>(first) +
                              (static_cast<double>(second) - first) *
                                  canonical_amount);
  };
  const auto position_index =
      static_cast<std::uint32_t>(frame.positions.size());
  frame.positions.push_back({
      .view_x = interpolate(low.view_x, high.view_x),
      .view_y = interpolate(low.view_y, high.view_y),
      .view_z = near_plane,
      .screen_h = interpolate(low.screen_h, high.screen_h),
      .screen_offset_x = interpolate(low.screen_offset_x, high.screen_offset_x),
      .screen_offset_y = interpolate(low.screen_offset_y, high.screen_offset_y),
  });
  frame.edge_table[slot_index] = {
      .edge_key = key,
      .position_index = position_index,
      .generation = frame.edge_table_generation,
  };
  ++frame.cached_edges;
  ++frame.stats.edge_intersections;
  return position_index;
}

[[nodiscard]] bool validPrimitive(const NativeScenePrimitive &primitive,
                                  std::size_t position_count) noexcept {
  if (primitive.vertex_count != 3U && primitive.vertex_count != 4U) {
    return false;
  }
  for (std::size_t vertex{}; vertex < primitive.vertex_count; ++vertex) {
    if (primitive.vertices[vertex].position_index >= position_count) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::uint32_t
textureBounds(const NativeScenePrimitive &primitive) noexcept {
  auto minimum_u = std::uint8_t{255U};
  auto minimum_v = std::uint8_t{255U};
  auto maximum_u = std::uint8_t{};
  auto maximum_v = std::uint8_t{};
  for (std::size_t vertex{}; vertex < primitive.vertex_count; ++vertex) {
    minimum_u = std::min(minimum_u, primitive.vertices[vertex].u);
    minimum_v = std::min(minimum_v, primitive.vertices[vertex].v);
    maximum_u = std::max(maximum_u, primitive.vertices[vertex].u);
    maximum_v = std::max(maximum_v, primitive.vertices[vertex].v);
  }
  return static_cast<std::uint32_t>(minimum_u) |
         (static_cast<std::uint32_t>(minimum_v) << 8U) |
         (static_cast<std::uint32_t>(maximum_u) << 16U) |
         (static_cast<std::uint32_t>(maximum_v) << 24U);
}

[[nodiscard]] constexpr std::uint32_t
mergeTextureBounds(std::uint32_t first, std::uint32_t second) noexcept {
  const auto minimum_u = std::min(first & 0xffU, second & 0xffU);
  const auto minimum_v =
      std::min((first >> 8U) & 0xffU, (second >> 8U) & 0xffU);
  const auto maximum_u =
      std::max((first >> 16U) & 0xffU, (second >> 16U) & 0xffU);
  const auto maximum_v =
      std::max((first >> 24U) & 0xffU, (second >> 24U) & 0xffU);
  return minimum_u | (minimum_v << 8U) | (maximum_u << 16U) |
         (maximum_v << 24U);
}

[[nodiscard]] std::uint32_t textureRoot(NativeScenePipelineFrame &frame,
                                        std::uint32_t primitive) noexcept {
  auto root = primitive;
  while (frame.texture_parents[root] != root) {
    root = frame.texture_parents[root];
  }
  while (frame.texture_parents[primitive] != primitive) {
    const auto parent = frame.texture_parents[primitive];
    frame.texture_parents[primitive] = root;
    primitive = parent;
  }
  return root;
}

void mergeTextureDomains(NativeScenePipelineFrame &frame, std::uint32_t first,
                         std::uint32_t second) noexcept {
  auto first_root = textureRoot(frame, first);
  auto second_root = textureRoot(frame, second);
  if (first_root == second_root) {
    return;
  }
  if (first_root > second_root) {
    std::swap(first_root, second_root);
  }
  frame.texture_parents[second_root] = first_root;
  frame.texture_component_bounds[first_root] =
      mergeTextureBounds(frame.texture_component_bounds[first_root],
                         frame.texture_component_bounds[second_root]);
  ++frame.stats.texture_domain_merges;
}

[[nodiscard]] constexpr std::uint32_t
textureMaterialKey(const NativeScenePrimitive &primitive) noexcept {
  return static_cast<std::uint32_t>(primitive.material.texture_page) |
         (static_cast<std::uint32_t>(primitive.material.clut) << 16U);
}

void insertTextureEdge(NativeScenePipelineFrame &frame,
                       const NativeScenePrimitive &primitive,
                       std::uint32_t primitive_index, std::size_t first,
                       std::size_t second) noexcept {
  const auto &first_vertex = primitive.vertices[first];
  const auto &second_vertex = primitive.vertices[second];
  if (first_vertex.position_index == second_vertex.position_index) {
    return;
  }
  const auto first_is_low =
      first_vertex.position_index < second_vertex.position_index;
  const auto &low_vertex = first_is_low ? first_vertex : second_vertex;
  const auto &high_vertex = first_is_low ? second_vertex : first_vertex;
  const auto key =
      edgeKey(low_vertex.position_index, high_vertex.position_index);
  const auto endpoint_uv = static_cast<std::uint32_t>(low_vertex.u) |
                           (static_cast<std::uint32_t>(low_vertex.v) << 8U) |
                           (static_cast<std::uint32_t>(high_vertex.u) << 16U) |
                           (static_cast<std::uint32_t>(high_vertex.v) << 24U);
  const auto material_key = textureMaterialKey(primitive);
  const auto signature =
      key ^ (static_cast<std::uint64_t>(endpoint_uv) * 0x9e3779b97f4a7c15ULL) ^
      (static_cast<std::uint64_t>(material_key) * 0xc2b2ae3d27d4eb4fULL);
  const auto mask = frame.texture_edge_table.size() - 1U;
  auto slot_index = edgeHash(signature) & mask;
  while (frame.texture_edge_table[slot_index].generation ==
         frame.texture_edge_table_generation) {
    const auto &slot = frame.texture_edge_table[slot_index];
    if (slot.edge_key == key && slot.endpoint_uv == endpoint_uv &&
        slot.material_key == material_key) {
      ++frame.stats.shared_texture_edges;
      mergeTextureDomains(frame, primitive_index, slot.primitive_index);
      return;
    }
    slot_index = (slot_index + 1U) & mask;
  }
  frame.texture_edge_table[slot_index] = {
      .edge_key = key,
      .endpoint_uv = endpoint_uv,
      .material_key = material_key,
      .primitive_index = primitive_index,
      .generation = frame.texture_edge_table_generation,
  };
}

void buildTextureDomains(const NativeRenderFrame &source,
                         NativeScenePipelineFrame &frame) {
  const auto primitive_count = source.scene_primitives.size();
  auto required = std::size_t{16U};
  const auto requested = primitive_count * 8U;
  while (required < requested) {
    required *= 2U;
  }
  if (frame.texture_edge_table.size() < required) {
    frame.texture_edge_table.resize(required);
    std::fill(frame.texture_edge_table.begin(), frame.texture_edge_table.end(),
              NativeTextureEdgeSlot{});
    frame.texture_edge_table_generation = 1U;
  } else if (++frame.texture_edge_table_generation == 0U) {
    std::fill(frame.texture_edge_table.begin(), frame.texture_edge_table.end(),
              NativeTextureEdgeSlot{});
    frame.texture_edge_table_generation = 1U;
  }

  frame.texture_parents.resize(primitive_count);
  frame.texture_component_bounds.resize(primitive_count);
  for (std::size_t index{}; index < primitive_count; ++index) {
    frame.texture_parents[index] = static_cast<std::uint32_t>(index);
    const auto &primitive = source.scene_primitives[index];
    frame.texture_component_bounds[index] =
        validPrimitive(primitive, source.scene_positions.size())
            ? textureBounds(primitive)
            : 0U;
  }

  constexpr std::array triangle_edges{std::pair{0U, 1U}, std::pair{1U, 2U},
                                      std::pair{2U, 0U}};
  constexpr std::array quad_edges{std::pair{0U, 1U}, std::pair{1U, 3U},
                                  std::pair{3U, 2U}, std::pair{2U, 0U}};
  for (std::size_t index{}; index < primitive_count; ++index) {
    const auto &primitive = source.scene_primitives[index];
    if (!primitive.material.textured ||
        !validPrimitive(primitive, source.scene_positions.size())) {
      continue;
    }
    const auto primitive_index = static_cast<std::uint32_t>(index);
    if (primitive.vertex_count == 3U) {
      for (const auto [first, second] : triangle_edges) {
        insertTextureEdge(frame, primitive, primitive_index, first, second);
      }
    } else {
      for (const auto [first, second] : quad_edges) {
        insertTextureEdge(frame, primitive, primitive_index, first, second);
      }
    }
  }
  for (std::size_t index{}; index < primitive_count; ++index) {
    frame.texture_component_bounds[index] =
        frame.texture_component_bounds[textureRoot(
            frame, static_cast<std::uint32_t>(index))];
  }
}

void appendTriangle(NativeScenePipelineFrame &frame,
                    const NativeClippedVertex &first,
                    const NativeClippedVertex &second,
                    const NativeClippedVertex &third,
                    const NativeScenePrimitive &primitive,
                    std::uint32_t source_primitive) {
  if (first.position_index == second.position_index ||
      second.position_index == third.position_index ||
      third.position_index == first.position_index) {
    return;
  }
  frame.triangles.push_back({
      .vertices = {first, second, third},
      .material = primitive.material,
      .draw_state =
          {
              .draw_mode = primitive.draw_state.draw_mode,
              .texture_window = primitive.draw_state.texture_window,
              .area_left = primitive.draw_state.area_left,
              .area_top = primitive.draw_state.area_top,
              .area_right = primitive.draw_state.area_right,
              .area_bottom = primitive.draw_state.area_bottom,
              .offset_x = primitive.draw_state.offset_x,
              .offset_y = primitive.draw_state.offset_y,
              .force_mask_bit = primitive.draw_state.force_mask_bit,
              .check_mask_bit = primitive.draw_state.check_mask_bit,
          },
      .source_word_offset = primitive.word_offset,
      .source_primitive = source_primitive,
      .texture_bounds = frame.texture_component_bounds[source_primitive],
      .source_word_count = primitive.word_count,
      .source_opcode = primitive.opcode,
  });
  ++frame.stats.output_triangles;
}

void markSourceTriangleBatch(NativeScenePipelineFrame &frame,
                             std::size_t first_triangle) noexcept {
  const auto triangle_count = frame.triangles.size() - first_triangle;
  if (triangle_count == 0U) {
    return;
  }
  const auto source_triangle_count = static_cast<std::uint8_t>(triangle_count);
  for (std::size_t triangle{}; triangle < triangle_count; ++triangle) {
    auto &output = frame.triangles[first_triangle + triangle];
    output.source_triangle_ordinal = static_cast<std::uint8_t>(triangle);
    output.source_triangle_count = source_triangle_count;
  }
}

void clipTriangle(const std::array<NativeClippedVertex, 3U> &input,
                  const NativeScenePrimitive &primitive,
                  std::uint32_t primitive_index, float near_plane,
                  NativeScenePipelineFrame &frame) {
  std::array<NativeClippedVertex, 6U> output{};
  auto output_count = std::size_t{};
  auto previous = input.back();
  auto previous_distance =
      frame.positions[previous.position_index].view_z - near_plane;
  auto previous_inside = previous_distance >= 0.0F;
  for (std::size_t vertex{}; vertex < input.size(); ++vertex) {
    const auto current = input[vertex];
    const auto current_distance =
        frame.positions[current.position_index].view_z - near_plane;
    const auto current_inside = current_distance >= 0.0F;
    if (current_inside != previous_inside && current_distance != 0.0F &&
        previous_distance != 0.0F) {
      auto amount = 0.0;
      const auto intersection =
          resolveIntersection(frame, previous.position_index,
                              current.position_index, near_plane, amount);
      const auto previous_is_low =
          previous.position_index < current.position_index;
      const auto &low_vertex = previous_is_low ? previous : current;
      const auto &high_vertex = previous_is_low ? current : previous;
      output[output_count++] =
          interpolateVertex(low_vertex, high_vertex, amount, intersection);
    }
    if (current_inside) {
      output[output_count++] = current;
    }
    previous = current;
    previous_distance = current_distance;
    previous_inside = current_inside;
  }
  for (std::size_t vertex = 1U; vertex + 1U < output_count; ++vertex) {
    appendTriangle(frame, output[0U], output[vertex], output[vertex + 1U],
                   primitive, primitive_index);
  }
}

void clipPrimitive(const NativeScenePrimitive &primitive,
                   std::uint32_t primitive_index, float near_plane,
                   NativeScenePipelineFrame &frame) {
  auto any_outside = false;
  for (std::size_t vertex{}; vertex < primitive.vertex_count; ++vertex) {
    any_outside =
        any_outside ||
        frame.positions[primitive.vertices[vertex].position_index].view_z <
            near_plane;
  }
  if (!any_outside) {
    appendTriangle(frame, makeClipVertex(primitive.vertices[0U]),
                   makeClipVertex(primitive.vertices[1U]),
                   makeClipVertex(primitive.vertices[2U]), primitive,
                   primitive_index);
    if (primitive.vertex_count == 4U) {
      appendTriangle(frame, makeClipVertex(primitive.vertices[1U]),
                     makeClipVertex(primitive.vertices[2U]),
                     makeClipVertex(primitive.vertices[3U]), primitive,
                     primitive_index);
    }
    return;
  }

  ++frame.stats.clipped_primitives;
  const auto triangles_before = frame.triangles.size();
  clipTriangle({makeClipVertex(primitive.vertices[0U]),
                makeClipVertex(primitive.vertices[1U]),
                makeClipVertex(primitive.vertices[2U])},
               primitive, primitive_index, near_plane, frame);
  if (primitive.vertex_count == 4U) {
    clipTriangle({makeClipVertex(primitive.vertices[1U]),
                  makeClipVertex(primitive.vertices[2U]),
                  makeClipVertex(primitive.vertices[3U])},
                 primitive, primitive_index, near_plane, frame);
  }
  if (frame.triangles.size() == triangles_before) {
    ++frame.stats.rejected_primitives;
  }
}

} // namespace

void rebuildNativeScenePipeline(const NativeRenderFrame &source,
                                float near_plane,
                                NativeScenePipelineFrame &result) noexcept {
  result.positions.clear();
  result.triangles.clear();
  result.stats = {};
  if (!std::isfinite(near_plane) || near_plane <= 0.0F) {
    result.stats.build_failures = 1U;
    return;
  }
  try {
    result.positions.assign(source.scene_positions.begin(),
                            source.scene_positions.end());
    if (result.triangles.capacity() < source.scene_primitives.size() * 3U) {
      result.triangles.reserve(source.scene_primitives.size() * 3U);
    }
    beginEdgeTable(source.scene_primitives.size(), result);
    buildTextureDomains(source, result);
    for (std::size_t primitive{}; primitive < source.scene_primitives.size();
         ++primitive) {
      ++result.stats.input_primitives;
      const auto &source_primitive = source.scene_primitives[primitive];
      if (!validPrimitive(source_primitive, source.scene_positions.size())) {
        ++result.stats.rejected_primitives;
        continue;
      }
      const auto first_triangle = result.triangles.size();
      clipPrimitive(source_primitive, static_cast<std::uint32_t>(primitive),
                    near_plane, result);
      markSourceTriangleBatch(result, first_triangle);
    }
  } catch (...) {
    result.positions.clear();
    result.triangles.clear();
    result.stats = {};
    result.stats.build_failures = 1U;
  }
}

} // namespace mohu
