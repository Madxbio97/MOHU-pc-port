#include "mohu/native_world_mesh.hpp"
#include "mohu/tsp_scene.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: mohu_scene_probe <level.tsp>\n";
    return 2;
  }
  std::ifstream input{argv[1], std::ios::binary};
  if (!input) {
    std::cerr << "failed to open " << argv[1] << '\n';
    return 2;
  }
  const std::vector<std::uint8_t> bytes{
      std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  const auto loaded = mohu::loadTspScene(bytes);
  if (!loaded) {
    std::cerr << "TSP load failed at 0x" << std::hex << loaded.error_offset
              << ": " << mohu::tspSceneErrorMessage(loaded.error) << '\n';
    return 1;
  }
  const auto transparent = static_cast<std::size_t>(std::count_if(
      loaded.scene.materials.begin(), loaded.scene.materials.end(),
      [](const mohu::TspMaterial &material) {
        return material.semiTransparent();
      }));
  const auto mesh = mohu::buildNativeWorldMesh(loaded.scene);
  if (!mesh) {
    std::cerr << "native mesh build failed\n";
    return 1;
  }
  std::cout << "TSP v" << loaded.scene.version
            << " nodes=" << loaded.scene.nodes.size()
            << " positions=" << loaded.scene.positions.size()
            << " triangles=" << loaded.scene.triangles.size()
            << " materials=" << loaded.scene.materials.size()
            << " transparent_materials=" << transparent
            << " gpu_vertices=" << mesh.mesh.vertices.size()
            << " opaque_indices=" << mesh.mesh.opaque_indices.size()
            << " transparent_indices=" << mesh.mesh.transparent_indices.size()
            << '\n';
  return 0;
}
