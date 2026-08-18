#pragma once

#include <cstdint>

namespace sf::psx {

enum class GpuDmaSourceKind : std::uint8_t {
  none,
  linear,
  linked_list,
};

struct GpuDmaWordSource {
  static constexpr std::uint32_t invalid_address = 0xffffffffU;
  static constexpr std::uint32_t invalid_ordering_depth = 0xffffffffU;

  std::uint32_t word_address{invalid_address};
  std::uint32_t transfer_root{invalid_address};
  GpuDmaSourceKind kind{GpuDmaSourceKind::none};
  std::uint32_t ordering_depth{invalid_ordering_depth};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return word_address != invalid_address;
  }
  [[nodiscard]] constexpr bool hasOrderingDepth() const noexcept {
    return kind == GpuDmaSourceKind::linked_list &&
           ordering_depth != invalid_ordering_depth;
  }

  friend constexpr bool operator==(const GpuDmaWordSource &,
                                   const GpuDmaWordSource &) = default;
};

} // namespace sf::psx
