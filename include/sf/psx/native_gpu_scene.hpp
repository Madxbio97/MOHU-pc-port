#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sf::psx {

struct NativeGpuPosition {
  float view_x{};
  float view_y{};
  float view_z{};
  float screen_h{};
  float screen_offset_x{};
  float screen_offset_y{};
  std::uint64_t vertex_identity{};
  std::uint64_t transform_lineage{};
  std::uint64_t projection_epoch{};
};

struct NativeGpuVertex {
  float u{};
  float v{};
  float red{};
  float green{};
  float blue{};
  std::uint32_t position_index{};
};

struct NativeGpuMaterial {
  std::uint16_t clut{};
  std::uint16_t texture_page{};
  bool textured{};
  bool gouraud{};
  bool semi_transparent{};
  bool raw_texture{};
};

struct NativeGpuDrawState {
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

  friend bool operator==(const NativeGpuDrawState &,
                         const NativeGpuDrawState &) = default;
};

struct NativeGpuTriangle {
  std::array<NativeGpuVertex, 3U> vertices{};
  NativeGpuMaterial material{};
  NativeGpuDrawState draw_state{};
  std::size_t source_word_offset{};
  std::uint32_t source_primitive{};
  std::uint8_t source_triangle_ordinal{};
  std::uint8_t source_triangle_count{1U};
  std::uint32_t texture_bounds{};
  std::uint8_t source_word_count{};
  std::uint8_t source_opcode{};
};

} // namespace sf::psx
