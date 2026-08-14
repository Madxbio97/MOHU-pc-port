#include "mohu/tsp_scene.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::size_t face_offset = 100U;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}
void put16(std::vector<std::uint8_t> &bytes, std::size_t offset,
           std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value);
  bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}
void put32(std::vector<std::uint8_t> &bytes, std::size_t offset,
           std::uint32_t value) {
  put16(bytes, offset, static_cast<std::uint16_t>(value));
  put16(bytes, offset + 2U, static_cast<std::uint16_t>(value >> 16U));
}

[[nodiscard]] std::vector<std::uint8_t> makeScene() {
  constexpr std::size_t node_offset = 72U;
  constexpr std::size_t position_offset = 116U;
  constexpr std::size_t color_offset = 148U;
  constexpr std::size_t material_offset = 164U;
  std::vector<std::uint8_t> bytes(176U);
  put16(bytes, 0U, 1U);
  put16(bytes, 2U, 3U);
  put32(bytes, 4U, 1U);
  put32(bytes, 8U, node_offset);
  put32(bytes, 12U, 2U);
  put32(bytes, 16U, face_offset);
  put32(bytes, 20U, 4U);
  put32(bytes, 24U, position_offset);
  put32(bytes, 28U, 0U);
  put32(bytes, 32U, color_offset);
  put32(bytes, 36U, 4U);
  put32(bytes, 40U, color_offset);
  put32(bytes, 44U, 0U);
  put32(bytes, 48U, material_offset);
  put32(bytes, 52U, 0U);
  put32(bytes, 56U, material_offset);
  put32(bytes, 60U, 176U);
  put32(bytes, 64U, 1U);
  put32(bytes, 68U, material_offset);

  put16(bytes, node_offset + 0U, static_cast<std::uint16_t>(-10));
  put16(bytes, node_offset + 2U, static_cast<std::uint16_t>(-20));
  put16(bytes, node_offset + 4U, static_cast<std::uint16_t>(-30));
  put16(bytes, node_offset + 6U, 10U);
  put16(bytes, node_offset + 8U, 20U);
  put16(bytes, node_offset + 10U, 30U);
  put32(bytes, node_offset + 12U, 2U);
  put32(bytes, node_offset + 24U, 0U);

  put32(bytes, face_offset, 0x00010000U);
  put16(bytes, face_offset + 4U, 2U);
  put16(bytes, face_offset + 6U, 0U);
  put32(bytes, face_offset + 8U, 0x00004003U);
  put32(bytes, face_offset + 12U, 0x1fff1fffU);

  for (std::size_t index{}; index < 4U; ++index) {
    const auto offset = position_offset + index * 8U;
    put16(bytes, offset, static_cast<std::uint16_t>(index * 10U));
    put16(bytes, offset + 2U, static_cast<std::uint16_t>(index * 20U));
    put16(bytes, offset + 4U, static_cast<std::uint16_t>(index * 30U));
    put16(bytes, offset + 6U, 104U);
    bytes[color_offset + index * 4U] =
        static_cast<std::uint8_t>(index + 1U);
  }
  bytes[material_offset + 0U] = 1U;
  bytes[material_offset + 1U] = 2U;
  put16(bytes, material_offset + 2U, 0x1234U);
  bytes[material_offset + 4U] = 3U;
  bytes[material_offset + 5U] = 4U;
  put16(bytes, material_offset + 6U, 0x40a1U);
  bytes[material_offset + 8U] = 5U;
  bytes[material_offset + 9U] = 6U;
  return bytes;
}

} // namespace

int main() {
  try {
    auto bytes = makeScene();
    const auto loaded = mohu::loadTspScene(bytes);
    require(static_cast<bool>(loaded), "valid TSP scene was rejected");
    require(loaded.scene.positions.size() == 4U, "position count mismatch");
    require(loaded.scene.colors.size() == 4U, "color count mismatch");
    require(loaded.scene.materials.size() == 1U, "material count mismatch");
    require(loaded.scene.nodes.size() == 1U, "node count mismatch");
    require(loaded.scene.triangles.size() == 2U, "triangle count mismatch");
    require(loaded.scene.triangles[0].position_indices ==
                std::array<std::uint16_t, 3U>{0U, 1U, 2U},
            "seed triangle mismatch");
    require(loaded.scene.triangles[1].position_indices ==
                std::array<std::uint16_t, 3U>{1U, 3U, 2U},
            "strip winding was not decoded");
    require(loaded.scene.triangles[1].uv ==
                std::array<mohu::TspUv, 3U>{{{1U, 2U}, {5U, 6U}, {3U, 4U}}},
            "strip UV winding was not decoded");
    require(loaded.scene.materials[0].semiTransparent(),
            "material flags were not decoded");

    bytes[face_offset + 4U] = 9U;
    const auto invalid = mohu::loadTspScene(bytes);
    require(!invalid &&
                invalid.error == mohu::TspSceneError::invalid_position_index,
            "invalid position index was accepted");
    std::cout << "mohu_tsp_scene_tests: ok\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mohu_tsp_scene_tests: " << error.what() << '\n';
    return 1;
  }
}
