#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mohu {

struct TspPosition {
  std::int16_t x{};
  std::int16_t y{};
  std::int16_t z{};
};

struct TspUv {
  std::uint8_t u{};
  std::uint8_t v{};
  friend constexpr bool operator==(const TspUv &, const TspUv &) = default;
};

struct TspColor {
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};
  std::uint8_t code{};
};

struct TspMaterial {
  std::array<TspUv, 3U> uv{};
  std::uint16_t clut{};
  std::uint16_t texture_page{};

  [[nodiscard]] constexpr bool semiTransparent() const noexcept {
    return (texture_page & 0x4000U) != 0U;
  }
  [[nodiscard]] constexpr std::uint8_t colorMode() const noexcept {
    return static_cast<std::uint8_t>((texture_page >> 7U) & 0x3U);
  }
  [[nodiscard]] constexpr std::uint8_t blendMode() const noexcept {
    return static_cast<std::uint8_t>((texture_page >> 5U) & 0x3U);
  }
};

struct TspTriangle {
  std::array<std::uint16_t, 3U> position_indices{};
  std::array<TspUv, 3U> uv{};
  std::uint16_t material_index{};
  std::uint32_t source_face_offset{};
};

struct TspNode {
  TspPosition minimum{};
  TspPosition maximum{};
  std::uint32_t first_triangle{};
  std::uint32_t triangle_count{};
  std::array<std::int32_t, 3U> children{-1, -1, -1};
};

struct TspScene {
  std::uint16_t id{};
  std::uint16_t version{};
  std::vector<TspPosition> positions;
  std::vector<TspColor> colors;
  std::vector<TspMaterial> materials;
  std::vector<TspTriangle> triangles;
  std::vector<TspNode> nodes;
};

enum class TspSceneError : std::uint8_t {
  none,
  truncated_header,
  unsupported_version,
  invalid_count,
  invalid_section,
  invalid_node,
  invalid_face_stream,
  invalid_position_index,
  invalid_material_index,
  invalid_child_reference,
  invalid_vertex_padding,
  out_of_memory,
};

struct TspSceneLoadResult {
  TspScene scene;
  TspSceneError error{TspSceneError::none};
  std::size_t error_offset{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == TspSceneError::none;
  }
};

[[nodiscard]] TspSceneLoadResult
loadTspScene(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] const char *tspSceneErrorMessage(TspSceneError error) noexcept;

} // namespace mohu
