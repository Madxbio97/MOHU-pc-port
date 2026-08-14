#include "sf/platform/host.hpp"
#include "sf/platform/runtime_presentation_policy.hpp"

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

void testRuntimePresentationPolicy() {
  using namespace sf::platform;
  RuntimePresentationPolicy policy;
  require(policy.content() == PresentationContent::authored_4_3,
          "Runtime presentation did not start in safe authored mode");
  require(policy.update({512U, 240U, false, false}) ==
              PresentationContent::authored_4_3,
          "Ambiguous frontend mode became widescreen");
  require(policy.update({368U, 240U, false, false}) ==
              PresentationContent::gameplay,
          "Stable gameplay display mode did not become adaptive");
  require(policy.update({512U, 240U, false, false}) ==
              PresentationContent::gameplay,
          "One ambiguous frame changed gameplay presentation");
  require(policy.update({512U, 240U, false, false}) ==
              PresentationContent::authored_4_3,
          "Stable frontend transition remained adaptive");
  require(policy.update({384U, 240U, false, false}) ==
              PresentationContent::gameplay,
          "384-wide gameplay mode did not become adaptive");
}

} // namespace

int main() {
  testFilteringModesAreMutuallyExclusive();
  testAntialiasingModesDisableLegacyMsaa();
  testRuntimePresentationPolicy();
  std::cout << "Graphics settings tests passed\n";
  return 0;
}
