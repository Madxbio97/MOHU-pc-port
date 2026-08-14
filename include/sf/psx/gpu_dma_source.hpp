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

  std::uint32_t word_address{invalid_address};
  std::uint32_t transfer_root{invalid_address};
  GpuDmaSourceKind kind{GpuDmaSourceKind::none};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return word_address != invalid_address;
  }

  friend constexpr bool operator==(const GpuDmaWordSource &,
                                   const GpuDmaWordSource &) = default;
};

} // namespace sf::psx
