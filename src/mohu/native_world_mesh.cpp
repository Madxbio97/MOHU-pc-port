#include "mohu/native_world_mesh.hpp"

#include <limits>
#include <new>
#include <unordered_map>

namespace mohu {
namespace {

[[nodiscard]] constexpr std::uint64_t vertexKey(
    std::uint16_t position, std::uint16_t material, TspUv uv) noexcept {
  return static_cast<std::uint64_t>(position) |
         (static_cast<std::uint64_t>(material) << 16U) |
         (static_cast<std::uint64_t>(uv.u) << 32U) |
         (static_cast<std::uint64_t>(uv.v) << 40U);
}

[[nodiscard]] bool validScene(const TspScene &scene) noexcept {
  if (scene.positions.size() > std::numeric_limits<std::uint16_t>::max() ||
      scene.materials.size() > std::numeric_limits<std::uint16_t>::max() ||
      scene.colors.size() < scene.positions.size()) {
    return false;
  }
  for (const auto &triangle : scene.triangles) {
    if (triangle.material_index >= scene.materials.size()) {
      return false;
    }
    for (const auto position : triangle.position_indices) {
      if (position >= scene.positions.size()) {
        return false;
      }
    }
  }
  for (const auto &node : scene.nodes) {
    if (node.first_triangle > scene.triangles.size() ||
        node.triangle_count > scene.triangles.size() - node.first_triangle) {
      return false;
    }
  }
  return true;
}

} // namespace

NativeWorldMeshBuildResult
buildNativeWorldMesh(const TspScene &scene) noexcept {
  NativeWorldMeshBuildResult result{};
  if (!validScene(scene)) {
    result.error = NativeWorldMeshError::invalid_scene;
    return result;
  }
  try {
    auto &mesh = result.mesh;
    const auto corner_count = scene.triangles.size() * 3U;
    mesh.vertices.reserve(corner_count);
    mesh.opaque_indices.reserve(corner_count);
    mesh.transparent_indices.reserve(corner_count / 8U);
    mesh.nodes.reserve(scene.nodes.size());
    std::unordered_map<std::uint64_t, std::uint32_t> vertices;
    vertices.reserve(corner_count);

    const auto append_vertex = [&](const TspTriangle &triangle,
                                   std::size_t corner) {
      const auto source_position = triangle.position_indices[corner];
      const auto key = vertexKey(source_position, triangle.material_index,
                                 triangle.uv[corner]);
      if (const auto found = vertices.find(key); found != vertices.end()) {
        return found->second;
      }
      const auto index = static_cast<std::uint32_t>(mesh.vertices.size());
      const auto &material = scene.materials[triangle.material_index];
      mesh.vertices.push_back({scene.positions[source_position],
                               triangle.uv[corner],
                               scene.colors[source_position], material.clut,
                               material.texture_page, source_position});
      vertices.emplace(key, index);
      return index;
    };

    for (const auto &source_node : scene.nodes) {
      NativeWorldNode node{};
      node.minimum = source_node.minimum;
      node.maximum = source_node.maximum;
      node.first_opaque_index =
          static_cast<std::uint32_t>(mesh.opaque_indices.size());
      node.first_transparent_index =
          static_cast<std::uint32_t>(mesh.transparent_indices.size());
      const auto end = source_node.first_triangle + source_node.triangle_count;
      for (auto triangle_index = source_node.first_triangle;
           triangle_index < end; ++triangle_index) {
        const auto &triangle = scene.triangles[triangle_index];
        const auto transparent =
            scene.materials[triangle.material_index].semiTransparent();
        auto &indices = transparent ? mesh.transparent_indices
                                    : mesh.opaque_indices;
        for (std::size_t corner{}; corner < 3U; ++corner) {
          indices.push_back(append_vertex(triangle, corner));
        }
      }
      node.opaque_index_count =
          static_cast<std::uint32_t>(mesh.opaque_indices.size()) -
          node.first_opaque_index;
      node.transparent_index_count =
          static_cast<std::uint32_t>(mesh.transparent_indices.size()) -
          node.first_transparent_index;
      mesh.nodes.push_back(node);
    }
    return result;
  } catch (const std::bad_alloc &) {
    result.mesh = {};
    result.error = NativeWorldMeshError::out_of_memory;
    return result;
  }
}

} // namespace mohu
