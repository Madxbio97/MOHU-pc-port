#pragma once

#include "mohu/native_render_frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mohu {

using NativeClippedVertex = sf::psx::NativeGpuVertex;
using NativeSceneTriangle = sf::psx::NativeGpuTriangle;

inline constexpr float native_scene_near_plane = 32.0F;

struct NativeScenePipelineStats {
  std::uint64_t input_primitives{};
  std::uint64_t output_triangles{};
  std::uint64_t clipped_primitives{};
  std::uint64_t rejected_primitives{};
  std::uint64_t edge_intersections{};
  std::uint64_t reused_intersections{};
  std::uint64_t shared_texture_edges{};
  std::uint64_t texture_domain_merges{};
  std::uint64_t build_failures{};
};

struct NativeClipEdgeSlot {
  std::uint64_t edge_key{};
  std::uint32_t position_index{};
  std::uint32_t generation{};
};

struct NativeTextureEdgeSlot {
  std::uint64_t edge_key{};
  std::uint32_t endpoint_uv{};
  std::uint32_t material_key{};
  std::uint32_t primitive_index{};
  std::uint32_t generation{};
};

struct NativeScenePipelineFrame {
  std::vector<NativeScenePosition> positions;
  std::vector<NativeSceneTriangle> triangles;
  std::vector<NativeClipEdgeSlot> edge_table;
  std::vector<NativeTextureEdgeSlot> texture_edge_table;
  std::vector<std::uint32_t> texture_parents;
  std::vector<std::uint32_t> texture_component_bounds;
  NativeScenePipelineStats stats{};
  std::uint32_t edge_table_generation{1U};
  std::uint32_t texture_edge_table_generation{1U};
  std::size_t cached_edges{};
};

void rebuildNativeScenePipeline(const NativeRenderFrame &source,
                                float near_plane,
                                NativeScenePipelineFrame &result) noexcept;

} // namespace mohu
