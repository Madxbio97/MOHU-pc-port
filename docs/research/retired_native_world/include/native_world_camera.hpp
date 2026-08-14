#pragma once

#include "mohu/tsp_scene.hpp"
#include "sf/psx/r3000_runtime.hpp"

#include <array>
#include <cstdint>

namespace mohu {

struct NativeWorldCamera {
  std::array<double, 9U> rotation{};
  std::array<double, 3U> translation{};
  double projection_h{};
  double offset_x{};
  double offset_y{};
  std::uint64_t camera_revision{};
  std::uint64_t projection_revision{};
  bool valid{};
};

struct NativeViewPosition {
  double x{};
  double y{};
  double z{};

  friend constexpr bool operator==(const NativeViewPosition &,
                                   const NativeViewPosition &) = default;
};

[[nodiscard]] NativeWorldCamera nativeWorldCameraFromCheckpoint(
    const sf::psx::R3000PgxpTransformCheckpoint &checkpoint) noexcept;
[[nodiscard]] NativeViewPosition
transformNativeWorldPosition(const NativeWorldCamera &camera,
                             TspPosition position) noexcept;

} // namespace mohu
