#include "mohu/level_lighting.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mohu {
namespace {

struct LightingProfile {
  LightingRgb sky;
  LightingRgb ground;
  LightingRgb key;
  LightingRgb lightning;
  LightingVector key_direction;
  bool storm{};
  bool wind{};
};

constexpr LightingVector default_key{-0.24F, -0.76F, -0.60F};

[[nodiscard]] constexpr LightingProfile
profileForLevel(std::uint8_t level) noexcept {
  switch (level) {
  case 1U:
  case 2U:
    return {{0.98F, 1.01F, 1.09F},
            {0.74F, 0.78F, 0.87F},
            {0.18F, 0.16F, 0.14F},
            {0.72F, 0.80F, 1.00F},
            default_key};
  case 3U:
  case 4U:
    return {{0.96F, 0.99F, 1.07F},
            {0.73F, 0.76F, 0.83F},
            {0.21F, 0.17F, 0.12F},
            {0.72F, 0.80F, 1.00F},
            default_key};
  case 5U:
    return {{1.13F, 1.08F, 0.97F},
            {0.91F, 0.84F, 0.70F},
            {0.27F, 0.22F, 0.14F},
            {0.82F, 0.88F, 1.00F},
            default_key};
  case 6U:
  case 7U:
    return {{0.82F, 0.87F, 0.98F},
            {0.65F, 0.69F, 0.77F},
            {0.12F, 0.14F, 0.20F},
            {0.78F, 0.87F, 1.08F},
            default_key,
            true,
            true};
  case 8U:
    return {{0.99F, 0.87F, 0.69F},
            {0.80F, 0.68F, 0.53F},
            {0.13F, 0.10F, 0.07F},
            {0.84F, 0.88F, 0.96F},
            default_key,
            false,
            true};
  case 9U:
    return {{0.91F, 0.98F, 1.10F},
            {0.68F, 0.74F, 0.84F},
            {0.13F, 0.15F, 0.20F},
            {0.72F, 0.80F, 1.00F},
            default_key};
  case 10U:
  case 11U:
    return {{1.12F, 1.10F, 1.03F},
            {0.94F, 0.91F, 0.82F},
            {0.24F, 0.22F, 0.17F},
            {0.82F, 0.88F, 1.00F},
            default_key};
  case 12U:
    return {{0.98F, 1.01F, 1.03F},
            {0.83F, 0.85F, 0.86F},
            {0.10F, 0.11F, 0.12F},
            {0.78F, 0.86F, 1.03F},
            default_key};
  case 13U:
    return {{1.03F, 1.03F, 1.01F},
            {0.87F, 0.86F, 0.82F},
            {0.17F, 0.16F, 0.14F},
            {0.80F, 0.87F, 1.00F},
            default_key};
  case 14U:
    return {{0.88F, 0.94F, 1.06F},
            {0.65F, 0.70F, 0.80F},
            {0.13F, 0.14F, 0.19F},
            {0.75F, 0.83F, 1.04F},
            default_key};
  case 15U:
  case 16U:
  case 17U:
    return {{0.94F, 0.97F, 1.04F},
            {0.69F, 0.72F, 0.79F},
            {0.20F, 0.16F, 0.12F},
            {0.75F, 0.83F, 1.04F},
            default_key};
  case 18U:
  case 19U:
  case 20U:
    return {{0.90F, 0.95F, 1.05F},
            {0.67F, 0.71F, 0.80F},
            {0.16F, 0.15F, 0.18F},
            {0.75F, 0.83F, 1.04F},
            default_key};
  case 21U:
  case 22U:
  case 23U:
  case 24U:
    return {{0.96F, 1.00F, 1.08F},
            {0.71F, 0.75F, 0.84F},
            {0.17F, 0.15F, 0.14F},
            {0.72F, 0.80F, 1.00F},
            default_key};
  default:
    return {};
  }
}

[[nodiscard]] constexpr float smootherStep(float value) noexcept {
  value = std::clamp(value, 0.0F, 1.0F);
  return value * value * value * (value * (value * 6.0F - 15.0F) + 10.0F);
}

[[nodiscard]] constexpr float triangleWave(std::uint64_t tick,
                                           std::uint64_t period) noexcept {
  const auto phase =
      static_cast<float>(tick % period) / static_cast<float>(period);
  return phase < 0.5F ? phase * 4.0F - 1.0F : 3.0F - phase * 4.0F;
}

[[nodiscard]] float pulse(float phase, float centre, float radius,
                          float strength) noexcept {
  const auto distance = std::abs(phase - centre) / radius;
  return (1.0F - smootherStep(distance)) * strength;
}

[[nodiscard]] float stormLightning(std::uint8_t level,
                                   std::uint64_t tick) noexcept {
  constexpr auto period = std::uint64_t{251U};
  const auto offset = level == 6U ? 31U : 113U;
  const auto phase = static_cast<float>((tick + offset) % period);
  return std::max(pulse(phase, 12.0F, 12.0F, 0.86F),
                  pulse(phase, 36.0F, 10.0F, 0.43F));
}

[[nodiscard]] constexpr float mix(float first, float second,
                                  float amount) noexcept {
  return first + (second - first) * amount;
}

[[nodiscard]] constexpr float bounded(float value) noexcept {
  return std::clamp(value, 0.35F, 1.75F);
}

} // namespace

LevelLightingFrame buildLevelLightingFrame(std::uint8_t campaign_level,
                                           std::uint64_t tick) noexcept {
  if (campaign_level < 1U || campaign_level > 24U)
    return {};

  const auto profile = profileForLevel(campaign_level);
  auto frame = LevelLightingFrame{
      profile.sky,
      profile.ground,
      profile.key,
      profile.lightning,
      profile.key_direction,
      profile.storm ? stormLightning(campaign_level, tick) : 0.0F,
      true};
  if (profile.wind) {
    const auto wave = triangleWave(tick + campaign_level * 17U, 180U);
    const auto variation = std::sin(wave * std::numbers::pi_v<float> * 0.5F) *
                           (campaign_level == 8U ? 0.035F : 0.018F);
    frame.sky.red += variation;
    frame.sky.green += variation;
    frame.sky.blue += variation;
  }
  return frame;
}

LightingRgb sampleLevelLighting(const LevelLightingFrame &frame,
                                LightingVector normal) noexcept {
  if (!frame.active)
    return {};
  const auto length_squared =
      normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
  if (!std::isfinite(length_squared) || length_squared <= 1.0e-12F)
    return {};
  const auto inverse_length = 1.0F / std::sqrt(length_squared);
  normal.x *= inverse_length;
  normal.y *= inverse_length;
  normal.z *= inverse_length;
  const auto sky_amount = smootherStep(0.5F - normal.y * 0.5F);
  const auto key_dot = normal.x * frame.key_direction.x +
                       normal.y * frame.key_direction.y +
                       normal.z * frame.key_direction.z;
  const auto diffuse = smootherStep((key_dot + 0.20F) / 1.20F);
  return {
      bounded(mix(frame.ground.red, frame.sky.red, sky_amount) +
              frame.key.red * diffuse +
              frame.lightning.red * frame.lightning_strength),
      bounded(mix(frame.ground.green, frame.sky.green, sky_amount) +
              frame.key.green * diffuse +
              frame.lightning.green * frame.lightning_strength),
      bounded(mix(frame.ground.blue, frame.sky.blue, sky_amount) +
              frame.key.blue * diffuse +
              frame.lightning.blue * frame.lightning_strength),
  };
}

LightingRgb
sampleLevelAmbientLighting(const LevelLightingFrame &frame) noexcept {
  if (!frame.active)
    return {};
  constexpr auto sky_weight = 0.55F;
  constexpr auto key_weight = 0.20F;
  return {
      bounded(mix(frame.ground.red, frame.sky.red, sky_weight) +
              frame.key.red * key_weight +
              frame.lightning.red * frame.lightning_strength),
      bounded(mix(frame.ground.green, frame.sky.green, sky_weight) +
              frame.key.green * key_weight +
              frame.lightning.green * frame.lightning_strength),
      bounded(mix(frame.ground.blue, frame.sky.blue, sky_weight) +
              frame.key.blue * key_weight +
              frame.lightning.blue * frame.lightning_strength),
  };
}

std::uint32_t applyLevelLighting(std::uint32_t packed_rgb,
                                 const LightingRgb &multiplier) noexcept {
  if (!std::isfinite(multiplier.red) || !std::isfinite(multiplier.green) ||
      !std::isfinite(multiplier.blue))
    return packed_rgb;
  const auto channel = [](std::uint32_t value, float scale) {
    return static_cast<std::uint32_t>(std::clamp(
        static_cast<int>(static_cast<float>(value) * scale + 0.5F), 0, 255));
  };
  const auto red = channel(packed_rgb & 0xffU, multiplier.red);
  const auto green = channel((packed_rgb >> 8U) & 0xffU, multiplier.green);
  const auto blue = channel((packed_rgb >> 16U) & 0xffU, multiplier.blue);
  return (packed_rgb & 0xff000000U) | red | (green << 8U) | (blue << 16U);
}

} // namespace mohu
