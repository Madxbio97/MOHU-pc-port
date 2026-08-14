#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sf::psx {

enum class Gp0CommandClass : std::uint8_t {
  state,
  fill,
  polygon,
  line,
  rectangle,
  vram_copy,
  cpu_to_vram,
  vram_to_cpu,
  unknown,
};

struct Gp0CommandLayout {
  std::uint8_t opcode{};
  Gp0CommandClass command_class{Gp0CommandClass::unknown};
  std::size_t word_count{};
  std::array<std::size_t, 4U> coordinate_words{};
  std::uint8_t vertex_count{};
  bool textured{};
  bool gouraud{};
  bool quad{};
  bool semi_transparent{};

  [[nodiscard]] constexpr bool complete() const noexcept {
    return word_count != 0U;
  }
};

[[nodiscard]] constexpr std::uint16_t
gp0TransferWidth(std::uint32_t size) noexcept {
  return static_cast<std::uint16_t>((((size & 0xffffU) - 1U) & 0x03ffU) + 1U);
}

[[nodiscard]] constexpr std::uint16_t
gp0TransferHeight(std::uint32_t size) noexcept {
  return static_cast<std::uint16_t>(
      ((((size >> 16U) & 0xffffU) - 1U) & 0x01ffU) + 1U);
}

[[nodiscard]] constexpr bool
gp0IsPolylineTerminator(std::uint32_t word) noexcept {
  return (word & 0xf000f000U) == 0x50005000U;
}

[[nodiscard]] constexpr std::size_t
gp0FixedCommandLength(std::uint8_t opcode) noexcept {
  if (opcode == 0x02U) {
    return 3U;
  }
  if (opcode < 0x20U) {
    return 1U;
  }
  if (opcode < 0x24U) {
    return 4U;
  }
  if (opcode < 0x28U) {
    return 7U;
  }
  if (opcode < 0x2cU) {
    return 5U;
  }
  if (opcode < 0x30U) {
    return 9U;
  }
  if (opcode < 0x34U) {
    return 6U;
  }
  if (opcode < 0x38U) {
    return 9U;
  }
  if (opcode < 0x3cU) {
    return 8U;
  }
  if (opcode < 0x40U) {
    return 12U;
  }
  if (opcode < 0x48U) {
    return 3U;
  }
  if (opcode < 0x50U) {
    return 0U;
  }
  if (opcode < 0x58U) {
    return 4U;
  }
  if (opcode < 0x60U) {
    return 0U;
  }
  if (opcode < 0x64U) {
    return 3U;
  }
  if (opcode < 0x68U) {
    return 4U;
  }
  if (opcode < 0x6cU) {
    return 2U;
  }
  if (opcode < 0x70U) {
    return 3U;
  }
  if (opcode < 0x74U) {
    return 2U;
  }
  if (opcode < 0x78U) {
    return 3U;
  }
  if (opcode < 0x7cU) {
    return 2U;
  }
  if (opcode < 0x80U) {
    return 3U;
  }
  if (opcode < 0xa0U) {
    return 4U;
  }
  return 1U;
}

[[nodiscard]] constexpr std::size_t
gp0CommandLength(std::span<const std::uint32_t> words) noexcept {
  if (words.empty()) {
    return 0U;
  }
  const auto opcode = static_cast<std::uint8_t>(words.front() >> 24U);
  if ((opcode >= 0x48U && opcode < 0x50U) ||
      (opcode >= 0x58U && opcode < 0x60U)) {
    const auto minimum = opcode < 0x50U ? 4U : 5U;
    if (words.size() < minimum) {
      return 0U;
    }
    for (auto index = minimum - 1U; index < words.size(); ++index) {
      if (gp0IsPolylineTerminator(words[index])) {
        return index + 1U;
      }
    }
    return 0U;
  }
  if (opcode >= 0xa0U && opcode < 0xc0U) {
    if (words.size() < 3U) {
      return 0U;
    }
    const auto pixels = static_cast<std::size_t>(gp0TransferWidth(words[2U])) *
                        gp0TransferHeight(words[2U]);
    const auto length = 3U + (pixels + 1U) / 2U;
    return length <= words.size() ? length : 0U;
  }
  const auto length =
      opcode >= 0xc0U && opcode < 0xe0U ? 3U : gp0FixedCommandLength(opcode);
  return length <= words.size() ? length : 0U;
}

[[nodiscard]] constexpr std::array<std::size_t, 4U>
gp0PolygonCoordinateWords(std::uint8_t opcode) noexcept {
  if (opcode < 0x24U) {
    return {1U, 2U, 3U, 0U};
  }
  if (opcode < 0x28U) {
    return {1U, 3U, 5U, 0U};
  }
  if (opcode < 0x2cU) {
    return {1U, 2U, 3U, 4U};
  }
  if (opcode < 0x30U) {
    return {1U, 3U, 5U, 7U};
  }
  if (opcode < 0x34U) {
    return {1U, 3U, 5U, 0U};
  }
  if (opcode < 0x38U) {
    return {1U, 4U, 7U, 0U};
  }
  if (opcode < 0x3cU) {
    return {1U, 3U, 5U, 7U};
  }
  return {1U, 4U, 7U, 10U};
}

[[nodiscard]] constexpr Gp0CommandClass
gp0CommandClass(std::uint8_t opcode) noexcept {
  if (opcode == 0x02U) {
    return Gp0CommandClass::fill;
  }
  if (opcode < 0x20U || opcode >= 0xe0U) {
    return Gp0CommandClass::state;
  }
  if (opcode < 0x40U) {
    return Gp0CommandClass::polygon;
  }
  if (opcode < 0x60U) {
    return Gp0CommandClass::line;
  }
  if (opcode < 0x80U) {
    return Gp0CommandClass::rectangle;
  }
  if (opcode < 0xa0U) {
    return Gp0CommandClass::vram_copy;
  }
  if (opcode < 0xc0U) {
    return Gp0CommandClass::cpu_to_vram;
  }
  if (opcode < 0xe0U) {
    return Gp0CommandClass::vram_to_cpu;
  }
  return Gp0CommandClass::unknown;
}

[[nodiscard]] constexpr Gp0CommandLayout
gp0CommandLayout(std::span<const std::uint32_t> words) noexcept {
  Gp0CommandLayout result{};
  if (words.empty()) {
    return result;
  }
  result.opcode = static_cast<std::uint8_t>(words.front() >> 24U);
  result.command_class = gp0CommandClass(result.opcode);
  result.word_count = gp0CommandLength(words);
  result.textured = (result.opcode & 0x04U) != 0U;
  result.semi_transparent = (result.opcode & 0x02U) != 0U;
  if (result.command_class == Gp0CommandClass::polygon) {
    result.gouraud = (result.opcode & 0x10U) != 0U;
    result.quad = (result.opcode & 0x08U) != 0U;
    result.vertex_count = result.quad ? 4U : 3U;
    result.coordinate_words = gp0PolygonCoordinateWords(result.opcode);
  }
  return result;
}

} // namespace sf::psx
