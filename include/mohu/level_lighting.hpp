#pragma once

#include <cstdint>

namespace mohu {

struct LightingRgb {
  float red{1.0F};
  float green{1.0F};
  float blue{1.0F};

  friend bool operator==(const LightingRgb &, const LightingRgb &) = default;
};

struct LightingVector {
  float x{};
  float y{};
  float z{};
};

struct LevelLightingFrame {
  LightingRgb sky{};
  LightingRgb ground{};
  LightingRgb key{};
  LightingRgb lightning{};
  LightingVector key_direction{};
  float lightning_strength{};
  bool active{};
};

[[nodiscard]] LevelLightingFrame
buildLevelLightingFrame(std::uint8_t campaign_level,
                        std::uint64_t tick) noexcept;

[[nodiscard]] LightingRgb
sampleLevelLighting(const LevelLightingFrame &frame,
                    LightingVector surface_normal) noexcept;

[[nodiscard]] LightingRgb
sampleLevelAmbientLighting(const LevelLightingFrame &frame) noexcept;

[[nodiscard]] std::uint32_t
applyLevelLighting(std::uint32_t packed_rgb,
                   const LightingRgb &multiplier) noexcept;

} // namespace mohu
