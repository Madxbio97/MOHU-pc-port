#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sf::psx {
class R3000Runtime;
}

namespace mohu {

struct FrontendMenuTarget {
  std::uint32_t selection{};
  std::int16_t x{};
  std::int16_t y{};
  std::uint16_t width{};
  std::uint16_t height{};
};

struct FrontendMenuState {
  static constexpr std::size_t maximum_targets = 128U;

  bool active{};
  std::uint32_t screen_id{};
  std::uint32_t selected{};
  std::array<FrontendMenuTarget, maximum_targets> targets{};
  std::size_t target_count{};
};

[[nodiscard]] FrontendMenuState
inspectFrontendMenu(const sf::psx::R3000Runtime &runtime) noexcept;

[[nodiscard]] bool selectFrontendMenuTarget(sf::psx::R3000Runtime &runtime,
                                            std::uint32_t screen_id,
                                            std::uint32_t selection) noexcept;

} // namespace mohu
