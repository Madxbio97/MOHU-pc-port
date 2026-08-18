#include "mohu/level_lighting.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error{message};
}

void testInactiveOutsideCampaign() {
  const auto frame = mohu::buildLevelLightingFrame(0U, 0U);
  require(!frame.active,
          "Non-gameplay content unexpectedly enabled level lighting");
  require(mohu::sampleLevelLighting(frame, {0.0F, -1.0F, 0.0F}) ==
              mohu::LightingRgb{},
          "Inactive level lighting was not neutral");
  require(mohu::buildLevelLightingFrame(24U, 0U).active,
          "Last campaign level has no lighting profile");
  require(!mohu::buildLevelLightingFrame(25U, 0U).active,
          "Out-of-range campaign level enabled lighting");
}

void testProfilesMatchTheirAtmosphere() {
  const auto paris = mohu::sampleLevelLighting(
      mohu::buildLevelLightingFrame(1U, 0U), {0.0F, -1.0F, 0.0F});
  const auto desert = mohu::sampleLevelLighting(
      mohu::buildLevelLightingFrame(5U, 0U), {0.0F, -1.0F, 0.0F});
  require(paris.blue > paris.red, "Paris night lost its cool sky illumination");
  require(desert.red > desert.blue,
          "Desert morning lost its warm sky illumination");
}

void testAmbientLightingIsSpatiallyStable() {
  const auto frame = mohu::buildLevelLightingFrame(1U, 50U);
  const auto ambient = mohu::sampleLevelAmbientLighting(frame);
  require(ambient.red > 0.8F && ambient.red < 1.1F &&
              ambient.blue > ambient.red,
          "Ambient level light lost the authored night palette");
  require(mohu::sampleLevelAmbientLighting({}) == mohu::LightingRgb{},
          "Inactive ambient light was not neutral");
}

void testStormIsDeterministicAndDynamic() {
  auto maximum = 0.0F;
  auto minimum = 2.0F;
  auto maximum_step = 0.0F;
  float previous{};
  for (std::uint64_t tick{}; tick < 600U; ++tick) {
    const auto first = mohu::buildLevelLightingFrame(6U, tick);
    const auto repeated = mohu::buildLevelLightingFrame(6U, tick);
    require(first.lightning_strength == repeated.lightning_strength,
            "Storm lighting is not deterministic");
    maximum = std::max(maximum, first.lightning_strength);
    minimum = std::min(minimum, first.lightning_strength);
    if (tick != 0U)
      maximum_step =
          std::max(maximum_step, std::abs(first.lightning_strength - previous));
    previous = first.lightning_strength;
  }
  require(maximum > 0.75F && minimum == 0.0F && maximum_step < 0.14F,
          "Storm lighting lost its smooth bounded lightning events");
}

void testPackedColorPreservesCommandAndDarkness() {
  constexpr auto packed = std::uint32_t{0x2ca08040U};
  const auto lit = mohu::applyLevelLighting(packed, {1.25F, 0.5F, 1.5F});
  require((lit & 0xff000000U) == (packed & 0xff000000U),
          "Lighting changed the GP0 command byte");
  require((lit & 0xffU) == 80U && ((lit >> 8U) & 0xffU) == 64U &&
              ((lit >> 16U) & 0xffU) == 240U,
          "Lighting scaled GP0 channels incorrectly");
  require(mohu::applyLevelLighting(0x3c000000U, {1.75F, 1.75F, 1.75F}) ==
              0x3c000000U,
          "Environment lighting exposed authored blackness");
}

void testInvalidNormalFailsNeutral() {
  const auto frame = mohu::buildLevelLightingFrame(10U, 0U);
  require(mohu::sampleLevelLighting(frame, {}) == mohu::LightingRgb{},
          "Degenerate surface normal changed lighting");
  require(mohu::sampleLevelLighting(
              frame, {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F}) ==
              mohu::LightingRgb{},
          "Non-finite surface normal changed lighting");
}

} // namespace

int main() {
  try {
    testInactiveOutsideCampaign();
    testProfilesMatchTheirAtmosphere();
    testAmbientLightingIsSpatiallyStable();
    testStormIsDeterministicAndDynamic();
    testPackedColorPreservesCommandAndDarkness();
    testInvalidNormalFailsNeutral();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "MOHU level lighting tests passed\n";
  return 0;
}
