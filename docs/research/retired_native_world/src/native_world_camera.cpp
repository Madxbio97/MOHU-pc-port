#include "mohu/native_world_camera.hpp"

#include <cmath>

namespace mohu {
namespace {

[[nodiscard]] bool current(const sf::psx::GteExactComponent &component,
                           const sf::psx::GteExactState &exact) noexcept {
  return component.valid && component.generation == exact.generation &&
         component.lineage != 0U && std::isfinite(component.value);
}

[[nodiscard]] constexpr std::int32_t
signedWord(std::uint32_t value) noexcept {
  return static_cast<std::int32_t>(value);
}

[[nodiscard]] constexpr std::int16_t
signedHalf(std::uint32_t value) noexcept {
  return static_cast<std::int16_t>(value & 0xffffU);
}

} // namespace

NativeWorldCamera nativeWorldCameraFromCheckpoint(
    const sf::psx::R3000PgxpTransformCheckpoint &checkpoint) noexcept {
  NativeWorldCamera camera{};
  if (!checkpoint.valid || checkpoint.exact.camera_revision == 0U ||
      checkpoint.exact.projection_revision == 0U) {
    return camera;
  }
  for (std::size_t index{}; index < camera.rotation.size(); ++index) {
    const auto &component = checkpoint.exact.rotation[index];
    if (!current(component, checkpoint.exact)) {
      return {};
    }
    camera.rotation[index] = component.value;
  }
  for (std::size_t index{}; index < camera.translation.size(); ++index) {
    const auto &component = checkpoint.exact.translation[index];
    if (!current(component, checkpoint.exact)) {
      return {};
    }
    camera.translation[index] = component.value;
  }
  camera.projection_h =
      static_cast<double>(signedHalf(checkpoint.gte_witness.control[26U]));
  camera.offset_x =
      static_cast<double>(signedWord(checkpoint.gte_witness.control[24U])) /
      65536.0;
  camera.offset_y =
      static_cast<double>(signedWord(checkpoint.gte_witness.control[25U])) /
      65536.0;
  if (!(camera.projection_h > 0.0) || !std::isfinite(camera.offset_x) ||
      !std::isfinite(camera.offset_y)) {
    return {};
  }
  camera.camera_revision = checkpoint.exact.camera_revision;
  camera.projection_revision = checkpoint.exact.projection_revision;
  camera.valid = true;
  return camera;
}

NativeViewPosition
transformNativeWorldPosition(const NativeWorldCamera &camera,
                             TspPosition position) noexcept {
  if (!camera.valid) {
    return {};
  }
  const std::array<double, 3U> source{static_cast<double>(position.x),
                                     static_cast<double>(position.y),
                                     static_cast<double>(position.z)};
  NativeViewPosition result{};
  auto *destination = &result.x;
  for (std::size_t row{}; row < 3U; ++row) {
    auto accumulator = camera.translation[row] * 4096.0;
    for (std::size_t column{}; column < 3U; ++column) {
      accumulator += camera.rotation[row * 3U + column] * source[column];
    }
    destination[row] = accumulator / 4096.0;
  }
  return result;
}

} // namespace mohu
