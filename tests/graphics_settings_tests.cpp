#include "sf/platform/host.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testFilteringModesAreMutuallyExclusive() {
  using namespace sf::platform;
  GraphicsSettings settings;
  const auto verify = [&settings](TextureFilteringMode mode, bool bilinear,
                                  bool trilinear, bool anisotropic) {
    setTextureFilteringMode(settings, mode);
    require(textureFilteringMode(settings) == mode,
            "Filtering mode did not round-trip");
    require(settings.bilinear_filtering == bilinear &&
                settings.trilinear_filtering == trilinear &&
                settings.anisotropic_filtering == anisotropic,
            "Filtering mode produced an incoherent PsyCross flag set");
  };
  verify(TextureFilteringMode::nearest, false, false, false);
  verify(TextureFilteringMode::bilinear, true, false, false);
  verify(TextureFilteringMode::trilinear, true, true, false);
  verify(TextureFilteringMode::anisotropic, true, true, true);
}

void testAntialiasingModesDisableLegacyMsaa() {
  using namespace sf::platform;
  GraphicsSettings settings;
  settings.msaa_samples = 8;
  setAntialiasingMode(settings, AntialiasingMode::disabled);
  require(!settings.smaa && !settings.fxaa && settings.msaa_samples == 0 &&
              antialiasingMode(settings) == AntialiasingMode::disabled,
          "Disabled AA retained a false raw-guest MSAA selection");
  settings.msaa_samples = 4;
  setAntialiasingMode(settings, AntialiasingMode::smaa);
  require(settings.smaa && !settings.fxaa && settings.msaa_samples == 0 &&
              antialiasingMode(settings) == AntialiasingMode::smaa,
          "SMAA did not replace legacy raw-guest MSAA");
  settings.msaa_samples = 2;
  setAntialiasingMode(settings, AntialiasingMode::fxaa);
  require(!settings.smaa && settings.fxaa && settings.msaa_samples == 0 &&
              antialiasingMode(settings) == AntialiasingMode::fxaa,
          "FXAA did not replace legacy raw-guest MSAA");
}

void testControllerDeviceRoutes() {
  using namespace sf::platform;
  const GraphicsSettings settings;
  require(settings.controller_device_indices == std::array<int, 2U>{0, 1},
          "Default controller devices are not routed to distinct players");
  require(isValidControllerDeviceIndex(disabled_controller_device) &&
              isValidControllerDeviceIndex(0) &&
              isValidControllerDeviceIndex(1) &&
              !isValidControllerDeviceIndex(-2) &&
              !isValidControllerDeviceIndex(2),
          "Controller device route validation accepted an invalid index");
  require(areControllerDeviceRoutesValid({0, 1}) &&
              areControllerDeviceRoutesValid({disabled_controller_device, 0}) &&
              areControllerDeviceRoutesValid({1, disabled_controller_device}) &&
              !areControllerDeviceRoutesValid({0, 0}) &&
              !areControllerDeviceRoutesValid({1, 2}),
          "Controller device route validation accepted ambiguous routing");
}

} // namespace

int main() {
  testFilteringModesAreMutuallyExclusive();
  testAntialiasingModesDisableLegacyMsaa();
  testControllerDeviceRoutes();
  std::cout << "Graphics settings tests passed\n";
  return 0;
}
