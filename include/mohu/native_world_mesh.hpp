#pragma once

#include "mohu/tsp_scene.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mohu {

struct NativeWorldVertex {
  TspPosition position{};
  TspUv uv{};
  TspColor color{};
  std::uint16_t clut{};
  std::uint16_t texture_page{};
  std::uint16_t source_position{};
};

struct NativeWorldNode {
  TspPosition minimum{};
  TspPosition maximum{};
  std::uint32_t first_opaque_index{};
  std::uint32_t opaque_index_count{};
  std::uint32_t first_transparent_index{};
  std::uint32_t transparent_index_count{};
};

struct NativeWorldMesh {
  std::vector<NativeWorldVertex> vertices;
  std::vector<std::uint32_t> opaque_indices;
  std::vector<std::uint32_t> transparent_indices;
  std::vector<NativeWorldNode> nodes;
};

enum class NativeWorldMeshError : std::uint8_t {
  none,
  invalid_scene,
  out_of_memory,
};

struct NativeWorldMeshBuildResult {
  NativeWorldMesh mesh;
  NativeWorldMeshError error{NativeWorldMeshError::none};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == NativeWorldMeshError::none;
  }
};

[[nodiscard]] NativeWorldMeshBuildResult
buildNativeWorldMesh(const TspScene &scene) noexcept;

} // namespace mohu
