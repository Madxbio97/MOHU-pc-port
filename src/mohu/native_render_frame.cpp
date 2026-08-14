#include "mohu/native_render_frame.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mohu {
namespace {

[[nodiscard]] bool isProjectedVertex(const sf::psx::GteProjectedVertex &vertex,
                                     std::uint32_t packed_sxy) noexcept {
  return vertex.valid && vertex.packed_sxy == packed_sxy &&
         std::isfinite(vertex.screen_x) && std::isfinite(vertex.screen_y) &&
         std::isfinite(vertex.view_z);
}

[[nodiscard]] bool isExactViewVertex(const sf::psx::GteProjectedVertex &vertex,
                                     std::uint32_t packed_sxy) noexcept {
  return isProjectedVertex(vertex, packed_sxy) &&
         vertex.hasExactTransformProvenance() && std::isfinite(vertex.view_x) &&
         std::isfinite(vertex.view_y) && std::isfinite(vertex.screen_h) &&
         std::isfinite(vertex.screen_offset_x) &&
         std::isfinite(vertex.screen_offset_y) && vertex.screen_h > 0.0F;
}

[[nodiscard]] bool primitiveCameraConsistent(
    const std::array<const sf::psx::GteProjectedVertex *, 4U> &vertices,
    std::size_t vertex_count) noexcept {
  constexpr auto tolerance = 1.0F / 65536.0F;
  const auto &first = *vertices[0U];
  for (std::size_t vertex = 1U; vertex < vertex_count; ++vertex) {
    const auto &current = *vertices[vertex];
    if (std::abs(current.screen_h - first.screen_h) > tolerance ||
        std::abs(current.screen_offset_x - first.screen_offset_x) > tolerance ||
        std::abs(current.screen_offset_y - first.screen_offset_y) > tolerance) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] const sf::psx::GteProjectedVertex *resolveProjection(
    std::size_t word, std::uint32_t packed_sxy,
    std::span<const sf::psx::GteProjectedVertex> projections,
    std::span<const std::uint64_t> projection_identities,
    std::span<const sf::psx::GteProjectedVertex> projection_catalog) noexcept {
  if (word < projections.size() &&
      isProjectedVertex(projections[word], packed_sxy)) {
    return &projections[word];
  }
  if (word >= projection_identities.size()) {
    return nullptr;
  }
  const auto handle = projection_identities[word];
  if (handle == 0U || handle > projection_catalog.size()) {
    return nullptr;
  }
  const auto &projection = projection_catalog[handle - 1U];
  if (projection.source_vertex_id != handle ||
      !isProjectedVertex(projection, packed_sxy)) {
    return nullptr;
  }
  return &projection;
}

[[nodiscard]] constexpr std::int16_t
decodeSigned11(std::uint32_t value) noexcept {
  const auto packed = static_cast<std::int32_t>(value & 0x07ffU);
  return static_cast<std::int16_t>(packed >= 0x0400 ? packed - 0x0800 : packed);
}

void updateDrawState(std::uint32_t word, NativeDrawState &state) noexcept {
  switch (static_cast<std::uint8_t>(word >> 24U)) {
  case 0xe1U:
    state.draw_mode = word & 0x00ffffffU;
    break;
  case 0xe2U:
    state.texture_window = word & 0x000fffffU;
    break;
  case 0xe3U:
    state.area_left = static_cast<std::uint16_t>(word & 0x03ffU);
    state.area_top = static_cast<std::uint16_t>((word >> 10U) & 0x01ffU);
    break;
  case 0xe4U:
    state.area_right = static_cast<std::uint16_t>(word & 0x03ffU);
    state.area_bottom = static_cast<std::uint16_t>((word >> 10U) & 0x01ffU);
    break;
  case 0xe5U:
    state.offset_x = decodeSigned11(word);
    state.offset_y = decodeSigned11(word >> 11U);
    break;
  case 0xe6U:
    state.force_mask_bit = (word & 1U) != 0U;
    state.check_mask_bit = (word & 2U) != 0U;
    break;
  default:
    break;
  }
}

[[nodiscard]] NativeSceneVertex
makeSceneVertex(const sf::psx::GteProjectedVertex &source,
                std::span<const std::uint32_t> command,
                const sf::psx::Gp0CommandLayout &layout,
                std::size_t vertex) noexcept {
  const auto coordinate_word = layout.coordinate_words[vertex];
  const auto color_word =
      layout.gouraud && vertex != 0U ? coordinate_word - 1U : 0U;
  const auto color = command[color_word];
  const auto uv = layout.textured ? command[coordinate_word + 1U] : 0U;
  return NativeSceneVertex{
      .view_x = source.view_x,
      .view_y = source.view_y,
      .view_z = source.view_z,
      .screen_x = source.screen_x,
      .screen_y = source.screen_y,
      .projective_depth = source.projective_depth,
      .screen_h = source.screen_h,
      .screen_offset_x = source.screen_offset_x,
      .screen_offset_y = source.screen_offset_y,
      .packed_sxy = source.packed_sxy,
      .vertex_identity = source.mesh_vertex_id != 0U ? source.mesh_vertex_id
                                                     : source.source_vertex_id,
      .transform_lineage = source.transform_lineage,
      .projection_epoch = source.projection_epoch,
      .u = static_cast<std::uint8_t>(uv),
      .v = static_cast<std::uint8_t>(uv >> 8U),
      .red = static_cast<std::uint8_t>(color),
      .green = static_cast<std::uint8_t>(color >> 8U),
      .blue = static_cast<std::uint8_t>(color >> 16U),
  };
}

[[nodiscard]] NativeScenePrimitive makeScenePrimitive(
    std::size_t word_offset, std::span<const std::uint32_t> command,
    const std::array<const sf::psx::GteProjectedVertex *, 4U> &projections,
    const sf::psx::Gp0CommandLayout &layout,
    const NativeDrawState &draw_state) noexcept {
  NativeScenePrimitive result{
      .draw_state = draw_state,
      .material =
          {
              .textured = layout.textured,
              .gouraud = layout.gouraud,
              .semi_transparent = layout.semi_transparent,
              .raw_texture = (layout.opcode & 1U) != 0U,
          },
      .word_offset = word_offset,
      .word_count = static_cast<std::uint8_t>(layout.word_count),
      .opcode = layout.opcode,
      .vertex_count = layout.vertex_count,
  };
  if (layout.textured) {
    result.material.clut = static_cast<std::uint16_t>(
        command[layout.coordinate_words[0U] + 1U] >> 16U);
    result.material.texture_page = static_cast<std::uint16_t>(
        command[layout.coordinate_words[1U] + 1U] >> 16U);
  } else {
    result.material.texture_page =
        static_cast<std::uint16_t>(draw_state.draw_mode & 0x07ffU);
  }
  for (std::size_t vertex{}; vertex < layout.vertex_count; ++vertex) {
    result.vertices[vertex] =
        makeSceneVertex(*projections[vertex], command, layout, vertex);
  }
  return result;
}

[[nodiscard]] constexpr std::size_t
positionHash(std::uint64_t identity) noexcept {
  identity ^= identity >> 33U;
  identity *= 0xff51afd7ed558ccdULL;
  identity ^= identity >> 33U;
  identity *= 0xc4ceb9fe1a85ec53ULL;
  identity ^= identity >> 33U;
  return static_cast<std::size_t>(identity);
}

[[nodiscard]] std::uint32_t
appendScenePosition(NativeRenderFrame &frame, const NativeSceneVertex &vertex) {
  const auto index = static_cast<std::uint32_t>(frame.scene_positions.size());
  frame.scene_positions.push_back({
      .view_x = vertex.view_x,
      .view_y = vertex.view_y,
      .view_z = vertex.view_z,
      .screen_h = vertex.screen_h,
      .screen_offset_x = vertex.screen_offset_x,
      .screen_offset_y = vertex.screen_offset_y,
      .vertex_identity = vertex.vertex_identity,
      .transform_lineage = vertex.transform_lineage,
      .projection_epoch = vertex.projection_epoch,
  });
  return index;
}

[[nodiscard]] bool
scenePositionMatches(const NativeScenePosition &position,
                     const NativeSceneVertex &vertex) noexcept {
  return position.view_x == vertex.view_x && position.view_y == vertex.view_y &&
         position.view_z == vertex.view_z &&
         position.screen_h == vertex.screen_h &&
         position.screen_offset_x == vertex.screen_offset_x &&
         position.screen_offset_y == vertex.screen_offset_y;
}

void insertPositionSlot(NativeRenderFrame &frame,
                        std::uint32_t position_index) noexcept {
  const auto identity = frame.scene_positions[position_index].vertex_identity;
  const auto mask = frame.position_table.size() - 1U;
  auto slot_index = positionHash(identity) & mask;
  while (frame.position_table[slot_index].generation ==
         frame.position_table_generation) {
    slot_index = (slot_index + 1U) & mask;
  }
  frame.position_table[slot_index] = {
      .vertex_identity = identity,
      .position_index = position_index,
      .generation = frame.position_table_generation,
  };
  ++frame.identified_scene_positions;
}

void growPositionTable(NativeRenderFrame &frame) {
  const auto new_size =
      frame.position_table.empty() ? 16U : frame.position_table.size() * 2U;
  frame.position_table.resize(new_size);
  std::fill(frame.position_table.begin(), frame.position_table.end(),
            NativeScenePositionSlot{});
  frame.position_table_generation = 1U;
  frame.identified_scene_positions = 0U;
  for (std::uint32_t index{}; index < frame.scene_positions.size(); ++index) {
    if (frame.scene_positions[index].vertex_identity != 0U) {
      insertPositionSlot(frame, index);
    }
  }
}

void beginSceneTopology(NativeRenderFrame &frame) {
  frame.scene_positions.clear();
  frame.identified_scene_positions = 0U;
  if (frame.position_table.empty()) {
    growPositionTable(frame);
    return;
  }
  if (++frame.position_table_generation == 0U) {
    std::fill(frame.position_table.begin(), frame.position_table.end(),
              NativeScenePositionSlot{});
    frame.position_table_generation = 1U;
  }
}

[[nodiscard]] std::uint32_t
resolveScenePosition(NativeRenderFrame &frame,
                     const NativeSceneVertex &vertex) {
  if (vertex.vertex_identity == 0U) {
    return appendScenePosition(frame, vertex);
  }
  if ((frame.identified_scene_positions + 1U) * 2U >
      frame.position_table.size()) {
    growPositionTable(frame);
  }
  const auto mask = frame.position_table.size() - 1U;
  auto slot_index = positionHash(vertex.vertex_identity) & mask;
  auto identity_conflict = false;
  while (frame.position_table[slot_index].generation ==
         frame.position_table_generation) {
    const auto &slot = frame.position_table[slot_index];
    if (slot.vertex_identity == vertex.vertex_identity) {
      if (scenePositionMatches(frame.scene_positions[slot.position_index],
                               vertex)) {
        return slot.position_index;
      }
      identity_conflict = true;
    }
    slot_index = (slot_index + 1U) & mask;
  }
  const auto position_index = appendScenePosition(frame, vertex);
  insertPositionSlot(frame, position_index);
  frame.stats.conflicting_scene_vertices += identity_conflict ? 1U : 0U;
  return position_index;
}

[[nodiscard]] NativeRenderDomain
domainFor(sf::psx::Gp0CommandClass command_class) noexcept {
  using enum sf::psx::Gp0CommandClass;
  switch (command_class) {
  case state:
    return NativeRenderDomain::state;
  case polygon:
    return NativeRenderDomain::scene;
  case fill:
  case line:
  case rectangle:
    return NativeRenderDomain::screen;
  case vram_copy:
  case cpu_to_vram:
  case vram_to_cpu:
    return NativeRenderDomain::vram;
  case unknown:
    return NativeRenderDomain::unknown;
  }
  return NativeRenderDomain::unknown;
}

[[nodiscard]] std::array<std::array<std::size_t, 2U>, 4U>
perimeterEdges(std::size_t vertex_count, std::size_t &edge_count) noexcept {
  if (vertex_count == 4U) {
    edge_count = 4U;
    return {{{0U, 1U}, {1U, 3U}, {3U, 2U}, {2U, 0U}}};
  }
  edge_count = 3U;
  return {{{0U, 1U}, {1U, 2U}, {2U, 0U}, {0U, 0U}}};
}

void canonicalizeSeamEdge(NativeSceneSeamEdge &edge) noexcept {
  if (edge.packed[1U] < edge.packed[0U]) {
    std::swap(edge.packed[0U], edge.packed[1U]);
    std::swap(edge.position_indices[0U], edge.position_indices[1U]);
  }
}

[[nodiscard]] bool seamEdgeLess(const NativeSceneSeamEdge &left,
                                const NativeSceneSeamEdge &right) noexcept {
  if (left.draw_context != right.draw_context) {
    return left.draw_context < right.draw_context;
  }
  return left.packed < right.packed;
}

[[nodiscard]] bool sameSeamEdge(const NativeSceneSeamEdge &left,
                                const NativeSceneSeamEdge &right) noexcept {
  return left.draw_context == right.draw_context && left.packed == right.packed;
}

[[nodiscard]] constexpr std::uint64_t
drawGeometryContext(const NativeDrawState &state) noexcept {
  return static_cast<std::uint64_t>(state.area_left) |
         (static_cast<std::uint64_t>(state.area_top) << 10U) |
         (static_cast<std::uint64_t>(state.area_right) << 19U) |
         (static_cast<std::uint64_t>(state.area_bottom) << 29U) |
         ((static_cast<std::uint64_t>(
               static_cast<std::uint16_t>(state.offset_x)) &
           0x07ffU)
          << 38U) |
         ((static_cast<std::uint64_t>(
               static_cast<std::uint16_t>(state.offset_y)) &
           0x07ffU)
          << 49U);
}

[[nodiscard]] bool snapScenePosition(NativeScenePosition &position,
                                     std::uint32_t packed) noexcept {
  if (!std::isfinite(position.view_x) || !std::isfinite(position.view_y) ||
      !std::isfinite(position.view_z) || !std::isfinite(position.screen_h) ||
      !std::isfinite(position.screen_offset_x) ||
      !std::isfinite(position.screen_offset_y) ||
      std::abs(position.view_z) <= 1.0e-6F || position.screen_h <= 0.0F) {
    return false;
  }
  const auto packet_x = static_cast<float>(static_cast<std::int16_t>(packed));
  const auto packet_y =
      static_cast<float>(static_cast<std::int16_t>(packed >> 16U));
  const auto screen_x = position.screen_offset_x +
                        position.view_x * position.screen_h / position.view_z;
  const auto screen_y = position.screen_offset_y +
                        position.view_y * position.screen_h / position.view_z;
  constexpr auto maximum_displacement = 2.0F;
  if (!std::isfinite(screen_x) || !std::isfinite(screen_y) ||
      std::abs(screen_x - packet_x) > maximum_displacement ||
      std::abs(screen_y - packet_y) > maximum_displacement) {
    return false;
  }
  position.view_x = (packet_x - position.screen_offset_x) * position.view_z /
                    position.screen_h;
  position.view_y = (packet_y - position.screen_offset_y) * position.view_z /
                    position.screen_h;
  return true;
}

void stabilizeHybridSeams(std::span<const std::uint32_t> words,
                          NativeRenderFrame &frame) {
  frame.seam_edges.clear();
  frame.seam_snap_targets.clear();
  if (frame.scene_primitives.empty() || frame.stats.exact_view_polygons == 0U ||
      frame.stats.exact_view_polygons == frame.stats.scene_polygons) {
    return;
  }
  const auto fallback_polygons =
      frame.stats.scene_polygons - frame.stats.exact_view_polygons;
  if (fallback_polygons <= std::numeric_limits<std::size_t>::max() / 4U) {
    frame.seam_edges.reserve(static_cast<std::size_t>(fallback_polygons) * 4U);
  }
  NativeDrawState draw_state{};
  for (const auto &command : frame.commands) {
    if (command.domain == NativeRenderDomain::state) {
      updateDrawState(words[command.word_offset], draw_state);
      continue;
    }
    if (command.domain != NativeRenderDomain::scene ||
        command.geometry_source == NativeGeometrySource::exact_view) {
      continue;
    }
    std::size_t edge_count{};
    const auto perimeter =
        perimeterEdges(command.layout.vertex_count, edge_count);
    for (std::size_t edge{}; edge < edge_count; ++edge) {
      const auto first =
          words[command.word_offset +
                command.layout.coordinate_words[perimeter[edge][0U]]];
      const auto second =
          words[command.word_offset +
                command.layout.coordinate_words[perimeter[edge][1U]]];
      NativeSceneSeamEdge candidate{
          .packed = {first, second},
          .draw_context = drawGeometryContext(draw_state),
      };
      if (first != second) {
        canonicalizeSeamEdge(candidate);
        frame.seam_edges.push_back(candidate);
      }
    }
  }
  std::ranges::sort(frame.seam_edges, seamEdgeLess);

  constexpr auto unset_target = std::uint64_t{1} << 32U;
  constexpr auto conflicting_target = std::uint64_t{2} << 32U;
  frame.seam_snap_targets.assign(frame.scene_positions.size(), unset_target);
  const auto markTarget = [&](std::uint32_t position,
                              std::uint32_t packed) noexcept {
    if (position >= frame.seam_snap_targets.size()) {
      return;
    }
    auto &target = frame.seam_snap_targets[position];
    if (target == unset_target) {
      target = packed;
    } else if (target != packed) {
      target = conflicting_target;
    }
  };

  draw_state = {};
  for (const auto &command : frame.commands) {
    if (command.domain == NativeRenderDomain::state) {
      updateDrawState(words[command.word_offset], draw_state);
      continue;
    }
    if (command.geometry_source != NativeGeometrySource::exact_view ||
        command.scene_primitive >= frame.scene_primitives.size()) {
      continue;
    }
    const auto &primitive = frame.scene_primitives[command.scene_primitive];
    std::size_t edge_count{};
    const auto perimeter =
        perimeterEdges(command.layout.vertex_count, edge_count);
    for (std::size_t edge{}; edge < edge_count; ++edge) {
      const auto first_vertex = perimeter[edge][0U];
      const auto second_vertex = perimeter[edge][1U];
      const auto first = words[command.word_offset +
                               command.layout.coordinate_words[first_vertex]];
      const auto second = words[command.word_offset +
                                command.layout.coordinate_words[second_vertex]];
      NativeSceneSeamEdge candidate{
          .packed = {first, second},
          .position_indices =
              {primitive.vertices[first_vertex].position_index,
               primitive.vertices[second_vertex].position_index},
          .draw_context = drawGeometryContext(draw_state),
      };
      if (first == second) {
        continue;
      }
      canonicalizeSeamEdge(candidate);
      const auto group_begin =
          std::lower_bound(frame.seam_edges.begin(), frame.seam_edges.end(),
                           candidate, seamEdgeLess);
      if (group_begin != frame.seam_edges.end() &&
          sameSeamEdge(candidate, *group_begin)) {
        markTarget(candidate.position_indices[0U], candidate.packed[0U]);
        markTarget(candidate.position_indices[1U], candidate.packed[1U]);
        ++frame.stats.hybrid_seam_edges;
      }
    }
  }
  for (std::size_t position{}; position < frame.seam_snap_targets.size();
       ++position) {
    const auto target = frame.seam_snap_targets[position];
    if (target == unset_target) {
      continue;
    }
    if (target == conflicting_target ||
        !snapScenePosition(frame.scene_positions[position],
                           static_cast<std::uint32_t>(target))) {
      ++frame.stats.hybrid_seam_rejections;
      continue;
    }
    ++frame.stats.hybrid_seam_positions;
  }
}

} // namespace

void rebuildNativeRenderFrame(
    std::span<const std::uint32_t> words,
    std::span<const sf::psx::GteProjectedVertex> projections,
    std::span<const std::uint64_t> projection_identities,
    std::span<const sf::psx::GteProjectedVertex> projection_catalog,
    NativeRenderFrame &result) noexcept {
  result.commands.clear();
  result.scene_primitives.clear();
  result.scene_positions.clear();
  result.stats = {};
  try {
    beginSceneTopology(result);
    if (result.commands.capacity() < words.size() / 4U) {
      result.commands.reserve(words.size() / 4U);
    }
    if (result.scene_primitives.capacity() < words.size() / 8U) {
      result.scene_primitives.reserve(words.size() / 8U);
    }
    NativeDrawState draw_state{};
    auto offset = std::size_t{};
    while (offset < words.size()) {
      const auto layout = sf::psx::gp0CommandLayout(words.subspan(offset));
      if (!layout.complete()) {
        result.stats.truncated_words = words.size() - offset;
        break;
      }

      const auto command_words = words.subspan(offset, layout.word_count);
      NativeRenderCommand command{
          .word_offset = offset,
          .word_count = layout.word_count,
          .layout = layout,
          .domain = domainFor(layout.command_class),
      };
      ++result.stats.commands;
      switch (command.domain) {
      case NativeRenderDomain::state:
        updateDrawState(command_words.front(), draw_state);
        ++result.stats.state_commands;
        break;
      case NativeRenderDomain::scene: {
        ++result.stats.scene_polygons;
        std::array<const sf::psx::GteProjectedVertex *, 4U>
            resolved_projections{};
        auto projected = true;
        auto exact_view = true;
        for (std::size_t vertex{}; vertex < layout.vertex_count; ++vertex) {
          const auto word = offset + layout.coordinate_words[vertex];
          const auto *projection =
              resolveProjection(word, words[word], projections,
                                projection_identities, projection_catalog);
          resolved_projections[vertex] = projection;
          projected = projected && projection != nullptr;
          exact_view = exact_view && projection != nullptr &&
                       isExactViewVertex(*projection, words[word]);
        }
        if (exact_view) {
          exact_view = primitiveCameraConsistent(resolved_projections,
                                                 layout.vertex_count);
        }
        if (exact_view) {
          command.geometry_source = NativeGeometrySource::exact_view;
          ++result.stats.exact_view_polygons;
          result.stats.exact_view_vertices += layout.vertex_count;
          command.scene_primitive = result.scene_primitives.size();
          auto primitive = makeScenePrimitive(
              offset, command_words, resolved_projections, layout, draw_state);
          for (std::size_t vertex{}; vertex < layout.vertex_count; ++vertex) {
            auto &scene_vertex = primitive.vertices[vertex];
            result.stats.identified_exact_vertices +=
                scene_vertex.vertex_identity != 0U ? 1U : 0U;
            const auto positions_before = result.scene_positions.size();
            scene_vertex.position_index =
                resolveScenePosition(result, scene_vertex);
            if (result.scene_positions.size() == positions_before) {
              ++result.stats.shared_scene_vertices;
            } else {
              ++result.stats.unique_scene_positions;
            }
          }
          result.scene_primitives.push_back(std::move(primitive));
        } else if (projected) {
          command.geometry_source = NativeGeometrySource::projected;
          ++result.stats.projected_polygons;
        } else {
          command.geometry_source = NativeGeometrySource::legacy;
          ++result.stats.legacy_polygons;
        }
        break;
      }
      case NativeRenderDomain::screen:
        ++result.stats.screen_primitives;
        break;
      case NativeRenderDomain::vram:
        ++result.stats.vram_commands;
        break;
      case NativeRenderDomain::unknown:
        ++result.stats.unknown_commands;
        break;
      }
      result.commands.push_back(command);
      offset += layout.word_count;
    }
    stabilizeHybridSeams(words, result);
  } catch (...) {
    result.commands.clear();
    result.stats = {};
    result.stats.build_failures = 1U;
    result.scene_primitives.clear();
    result.scene_positions.clear();
    result.seam_edges.clear();
    result.seam_snap_targets.clear();
  }
}

} // namespace mohu
