#pragma once

#include "sf/psx/gp0_command.hpp"
#include "sf/psx/gte_runtime.hpp"
#include "sf/psx/native_gpu_scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace mohu {

enum class NativeRenderDomain : std::uint8_t {
  state,
  scene,
  screen,
  vram,
  unknown,
};

enum class NativeGeometrySource : std::uint8_t {
  none,
  exact_view,
  projected,
  legacy,
};

struct NativeSceneVertex {
  float view_x{};
  float view_y{};
  float view_z{};
  float screen_x{};
  float screen_y{};
  float projective_depth{};
  float screen_h{};
  float screen_offset_x{};
  float screen_offset_y{};
  std::uint32_t packed_sxy{};
  std::uint64_t vertex_identity{};
  std::uint64_t transform_lineage{};
  std::uint64_t projection_epoch{};
  std::uint32_t position_index{std::numeric_limits<std::uint32_t>::max()};
  std::uint8_t u{};
  std::uint8_t v{};
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};
};

using NativeScenePosition = sf::psx::NativeGpuPosition;

struct NativeScenePositionSlot {
  std::uint64_t vertex_identity{};
  std::uint32_t position_index{};
  std::uint32_t generation{};
};

struct NativeDrawState {
  std::uint32_t draw_mode{};
  std::uint32_t texture_window{};
  std::uint16_t area_left{};
  std::uint16_t area_top{};
  std::uint16_t area_right{1023U};
  std::uint16_t area_bottom{511U};
  std::int16_t offset_x{};
  std::int16_t offset_y{};
  bool force_mask_bit{};
  bool check_mask_bit{};
};

using NativeSceneMaterial = sf::psx::NativeGpuMaterial;

struct NativeScenePrimitive {
  std::array<NativeSceneVertex, 4U> vertices{};
  NativeDrawState draw_state{};
  NativeSceneMaterial material{};
  std::size_t word_offset{};
  std::uint8_t word_count{};
  std::uint8_t opcode{};
  std::uint8_t vertex_count{};
};

struct NativeRenderCommand {
  std::size_t word_offset{};
  std::size_t word_count{};
  sf::psx::Gp0CommandLayout layout{};
  NativeRenderDomain domain{NativeRenderDomain::unknown};
  NativeGeometrySource geometry_source{NativeGeometrySource::none};
  static constexpr auto no_scene_primitive =
      std::numeric_limits<std::size_t>::max();
  std::size_t scene_primitive{no_scene_primitive};
};

struct NativeRenderFrameStats {
  std::uint64_t commands{};
  std::uint64_t state_commands{};
  std::uint64_t scene_polygons{};
  std::uint64_t exact_view_polygons{};
  std::uint64_t unique_scene_positions{};
  std::uint64_t shared_scene_vertices{};
  std::uint64_t conflicting_scene_vertices{};
  std::uint64_t exact_view_vertices{};
  std::uint64_t identified_exact_vertices{};
  std::uint64_t projected_polygons{};
  std::uint64_t legacy_polygons{};
  std::uint64_t screen_primitives{};
  std::uint64_t vram_commands{};
  std::uint64_t unknown_commands{};
  std::uint64_t truncated_words{};
  std::uint64_t build_failures{};
  std::uint64_t hybrid_seam_edges{};
  std::uint64_t hybrid_seam_positions{};
  std::uint64_t hybrid_seam_rejections{};
};

struct NativeSceneSeamEdge {
  std::array<std::uint32_t, 2U> packed{};
  std::array<std::uint32_t, 2U> position_indices{
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max()};
  std::uint64_t draw_context{};
};

struct NativeRenderFrame {
  std::vector<NativeScenePosition> scene_positions;
  std::vector<NativeScenePositionSlot> position_table;
  std::vector<NativeSceneSeamEdge> seam_edges;
  std::vector<std::uint64_t> seam_snap_targets;
  std::uint32_t position_table_generation{1U};
  std::size_t identified_scene_positions{};
  std::vector<NativeRenderCommand> commands;
  std::vector<NativeScenePrimitive> scene_primitives;
  NativeRenderFrameStats stats{};
};

void rebuildNativeRenderFrame(
    std::span<const std::uint32_t> words,
    std::span<const sf::psx::GteProjectedVertex> projections,
    std::span<const std::uint64_t> projection_identities,
    std::span<const sf::psx::GteProjectedVertex> projection_catalog,
    NativeRenderFrame &result) noexcept;

} // namespace mohu
