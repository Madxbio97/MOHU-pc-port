#include "mohu/native_world_mesh.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

[[nodiscard]] mohu::TspScene makeScene(bool transparent) {
  mohu::TspScene scene{};
  scene.positions = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {10, 10, 0}};
  scene.colors.resize(4U, {128U, 128U, 128U, 0U});
  scene.materials.push_back({{{{1U, 2U}, {3U, 4U}, {5U, 6U}}}, 0x10U,
                             static_cast<std::uint16_t>(transparent ? 0x4000U
                                                                    : 0U)});
  scene.triangles.push_back(
      {{{0U, 1U, 2U}}, {{{1U, 2U}, {3U, 4U}, {5U, 6U}}}, 0U, 0U});
  scene.triangles.push_back(
      {{{1U, 3U, 2U}}, {{{1U, 2U}, {5U, 6U}, {3U, 4U}}}, 0U, 4U});
  scene.nodes.push_back(
      {{0, 0, 0}, {10, 10, 0}, 0U, 2U, {-1, -1, -1}});
  return scene;
}

} // namespace

int main() {
  try {
    const auto opaque = mohu::buildNativeWorldMesh(makeScene(false));
    require(static_cast<bool>(opaque), "valid scene was rejected");
    require(opaque.mesh.vertices.size() == 6U,
            "corner attributes were deduplicated incorrectly");
    require(opaque.mesh.opaque_indices.size() == 6U,
            "opaque index count mismatch");
    require(opaque.mesh.transparent_indices.empty(),
            "opaque geometry entered transparent pass");
    require(opaque.mesh.nodes[0].opaque_index_count == 6U,
            "node index range mismatch");
    require(opaque.mesh.vertices[0].source_position == 0U,
            "canonical source position was lost");

    const auto transparent = mohu::buildNativeWorldMesh(makeScene(true));
    require(static_cast<bool>(transparent), "transparent scene was rejected");
    require(transparent.mesh.opaque_indices.empty(),
            "transparent geometry entered opaque pass");
    require(transparent.mesh.transparent_indices.size() == 6U,
            "transparent index count mismatch");
    std::cout << "mohu_native_world_mesh_tests: ok\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mohu_native_world_mesh_tests: " << error.what() << '\n';
    return 1;
  }
}
