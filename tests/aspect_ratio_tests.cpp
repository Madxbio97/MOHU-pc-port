#include "PsyX/PsyX_public.h"

#include "mohu/display_presentation.hpp"
#include "mohu/runtime.hpp"
#include "sf/platform/host.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

bool near(float first, float second) {
  return std::abs(first - second) < 0.0001F;
}

void testOriginalFourThree() {
  const auto widescreen =
      PsyX_CalculatePresentationViewport(1920, 1080, PSYX_ASPECT_ORIGINAL_4_3);
  require(widescreen.x == 240 && widescreen.y == 0 && widescreen.w == 1440 &&
              widescreen.h == 1080,
          "Original mode did not pillarbox a 16:9 drawable to 4:3");

  const auto tall =
      PsyX_CalculatePresentationViewport(1280, 1024, PSYX_ASPECT_ORIGINAL_4_3);
  require(tall.x == 0 && tall.y == 32 && tall.w == 1280 && tall.h == 960,
          "Original mode did not letterbox a tall drawable to 4:3");
}

void testAdaptiveUsesEntireDrawable() {
  const auto widescreen =
      PsyX_CalculatePresentationViewport(1920, 1080, PSYX_ASPECT_ADAPTIVE);
  require(widescreen.x == 0 && widescreen.y == 0 && widescreen.w == 1920 &&
              widescreen.h == 1080,
          "Adaptive mode did not use the complete 16:9 drawable");

  const auto ultrawide =
      PsyX_CalculatePresentationViewport(3440, 1440, PSYX_ASPECT_ADAPTIVE);
  require(ultrawide.x == 0 && ultrawide.y == 0 && ultrawide.w == 3440 &&
              ultrawide.h == 1440,
          "Adaptive mode unexpectedly clamped an ultrawide drawable");
}

void testOutputPreservesSelectedResolutionAspect() {
  const auto wide =
      PsyX_CalculateOutputViewport(3440, 1440, 1280, 720, PSYX_ASPECT_ADAPTIVE);
  require(wide.x == 440 && wide.y == 0 && wide.w == 2560 && wide.h == 1440,
          "Adaptive output stretched a 16:9 internal target to ultrawide");

  const auto four_three =
      PsyX_CalculateOutputViewport(1920, 1080, 1280, 960, PSYX_ASPECT_ADAPTIVE);
  require(four_three.x == 240 && four_three.y == 0 && four_three.w == 1440 &&
              four_three.h == 1080,
          "Adaptive output ignored the selected internal aspect");

  const auto authored = PsyX_CalculateOutputViewport(3440, 1440, 2560, 1080,
                                                     PSYX_ASPECT_ORIGINAL_4_3);
  require(authored.x == 760 && authored.y == 0 && authored.w == 1920 &&
              authored.h == 1440,
          "Authored output did not remain 4:3 on ultrawide");
}

void testAuthoredContentAlwaysUsesFourThree() {
  using sf::platform::AspectRatioMode;
  using sf::platform::PresentationContent;
  require(
      sf::platform::presentationAspectRatio(AspectRatioMode::adaptive,
                                            PresentationContent::gameplay) ==
              AspectRatioMode::adaptive &&
          sf::platform::presentationAspectRatio(
              AspectRatioMode::adaptive, PresentationContent::authored_4_3) ==
              AspectRatioMode::original_4_3,
      "Presentation content did not select its own aspect policy");

  for (const auto size : {PsyXPresentationViewport{0, 0, 1920, 1080},
                          PsyXPresentationViewport{0, 0, 2560, 1440},
                          PsyXPresentationViewport{0, 0, 3440, 1440},
                          PsyXPresentationViewport{0, 0, 3840, 2160}}) {
    const auto viewport = PsyX_CalculatePresentationViewport(
        size.w, size.h, PSYX_ASPECT_ORIGINAL_4_3);
    require(viewport.w * 3 == viewport.h * 4,
            "Authored menu/movie viewport was not 4:3");
  }
}

void testAdaptivePreservesPixelAspect() {
  const auto widescreen =
      PsyX_CalculatePresentationScale(1920, 1080, PSYX_ASPECT_ADAPTIVE);
  require(near(widescreen.x, 0.75F) && near(widescreen.y, 1.0F),
          "Adaptive widescreen did not use an undistorted Hor+ scale");

  const auto tall =
      PsyX_CalculatePresentationScale(1280, 1024, PSYX_ASPECT_ADAPTIVE);
  require(near(tall.x, 1.0F) && near(tall.y, 0.9375F),
          "Adaptive narrow output did not use an undistorted Vert+ scale");

  const auto original =
      PsyX_CalculatePresentationScale(3440, 1440, PSYX_ASPECT_ORIGINAL_4_3);
  require(near(original.x, 1.0F) && near(original.y, 1.0F),
          "Original mode unexpectedly changed its presentation scale");
}

void testAdaptiveWorldFrustumMatchesPresentation() {
  const auto margin = mohu::adaptiveWorldXMargin(1920U, 1080U, true);
  require(margin == 90, "16:9 world frustum margin was not 90 pixels");

  const auto scale =
      PsyX_CalculatePresentationScale(1920, 1080, PSYX_ASPECT_ADAPTIVE);
  const auto native_width = static_cast<float>(mohu::retail_world_x_max);
  const auto left = static_cast<float>(-margin);
  const auto right = static_cast<float>(mohu::retail_world_x_max + margin);
  const auto left_ndc = (2.0F * left / native_width - 1.0F) * scale.x;
  const auto right_ndc = (2.0F * right / native_width - 1.0F) * scale.x;
  require(near(left_ndc, -1.0F) && near(right_ndc, 1.0F),
          "World frustum and PresentationScale disagree at 16:9 edges");

  require(mohu::adaptiveWorldXMargin(1280U, 960U, true) == 0,
          "4:3 unexpectedly enabled world widening");
  require(mohu::adaptiveWorldXMargin(1920U, 1080U, false) == 0,
          "Original presentation unexpectedly enabled world widening");
  require(mohu::adaptiveWorldXMargin(3840U, 1080U, true) == 450,
          "32:9 world frustum did not keep symmetric visible edges");
  require(mohu::adaptiveWorldXMargin(1U, 0U, true) == 0,
          "Invalid output size unexpectedly enabled world widening");
}

void testAdaptiveWorldFrustumAcrossWideAspects() {
  constexpr std::array sizes{
      PsyXPresentationViewport{0, 0, 1920, 1200},
      PsyXPresentationViewport{0, 0, 1920, 1080},
      PsyXPresentationViewport{0, 0, 2560, 1080},
      PsyXPresentationViewport{0, 0, 3840, 1080},
  };
  for (const auto size : sizes) {
    const auto margin =
        mohu::adaptiveWorldXMargin(static_cast<std::uint32_t>(size.w),
                                   static_cast<std::uint32_t>(size.h), true);
    const auto scale =
        PsyX_CalculatePresentationScale(size.w, size.h, PSYX_ASPECT_ADAPTIVE);
    const auto native_width = static_cast<float>(mohu::retail_world_x_max);
    const auto left =
        (2.0F * static_cast<float>(-margin) / native_width - 1.0F) * scale.x;
    const auto right =
        (2.0F * static_cast<float>(mohu::retail_world_x_max + margin) /
             native_width -
         1.0F) *
        scale.x;
    require(std::abs(left + 1.0F) < 0.002F && std::abs(right - 1.0F) < 0.002F,
            "Adaptive culling and Hor+ model scale diverged");
  }
}

void testAdaptiveWorldFrustumCoversEveryRetailCullPath() {
  using Hook = mohu::AdaptiveWorldFrustumHook;
  require(
      mohu::adaptiveWorldFrustumHook(0x80099a28U) == Hook::bsp_upper_x &&
          mohu::adaptiveWorldFrustumHook(0x80099dacU) == Hook::bsp_lower_x &&
          mohu::adaptiveWorldFrustumHook(0x8009ce1cU) == Hook::object_upper_x &&
          mohu::adaptiveWorldFrustumHook(0x8009a8b0U) ==
              Hook::level_triangle_outcode,
      "LEVEL widescreen hook table lost a verified visibility path");
  require(mohu::adaptiveWorldFrustumHook(0x80010c00U) ==
                  Hook::slus_triangle_outcode &&
              mohu::adaptiveWorldFrustumHook(0x800115c4U) ==
                  Hook::slus_triangle_outcode,
          "SLUS static-TSP outcode paths are not widened");
  require(mohu::adaptiveWorldFrustumHook(0x80010c04U) == Hook::none &&
              mohu::adaptiveWorldFrustumHook(0x8009a614U) == Hook::none,
          "Widescreen hook table accepted an unverified instruction");
}

void testRuntimePcDispatchIsExact() {
  using Action = mohu::RuntimePcAction;
  struct Expected {
    std::uint32_t pc;
    Action action;
  };
  constexpr std::array expected{
      Expected{sf::psx::R3000Runtime::return_sentinel, Action::return_sentinel},
      Expected{0x80000080U, Action::exception_vector},
      Expected{0x000000a0U, Action::bios_call_vector},
      Expected{0x000000b0U, Action::bios_call_vector},
      Expected{0x000000c0U, Action::bios_call_vector},
      Expected{0x80037b00U, Action::disc_search_return},
      Expected{0x80039064U, Action::directory_scan_entry},
      Expected{0x80099a28U, Action::frustum_bsp_upper_x},
      Expected{0x80099dacU, Action::frustum_bsp_lower_x},
      Expected{0x8009a2c8U, Action::frustum_bsp_lower_x},
      Expected{0x8009a8b0U, Action::frustum_level_triangle_outcode},
      Expected{0x8009cd7cU, Action::frustum_bsp_lower_x},
      Expected{0x8009ce1cU, Action::frustum_object_upper_x},
      Expected{0x80010c00U, Action::frustum_slus_triangle_outcode},
      Expected{0x800115c4U, Action::frustum_slus_triangle_outcode},
  };
  for (const auto &entry : expected) {
    require(mohu::runtimePcAction(entry.pc) == entry.action,
            "Runtime PC dispatcher lost a special address");
    for (const auto delta : {-8, -4, 4, 8}) {
      const auto neighbour = static_cast<std::uint32_t>(
          static_cast<std::int64_t>(entry.pc) + delta);
      auto is_special = false;
      for (const auto &candidate : expected) {
        is_special = is_special || candidate.pc == neighbour;
      }
      require(is_special || mohu::runtimePcAction(neighbour) == Action::none,
              "Runtime PC dispatcher accepted a neighbouring instruction");
    }
  }
  for (std::uint32_t slot{}; slot < 32U; ++slot) {
    const auto collision = 0x80100000U + slot * 4U;
    require(mohu::runtimePcAction(collision) == Action::none,
            "Runtime PC dispatcher accepted a hash collision");
  }
}

void testInvalidSizeIsBounded() {
  const auto viewport =
      PsyX_CalculatePresentationViewport(0, -1, PSYX_ASPECT_ADAPTIVE);
  require(viewport.x == 0 && viewport.y == 0 && viewport.w == 1 &&
              viewport.h == 1,
          "Viewport dimensions were not bounded to one pixel");
}

void testGuestDisplayGeometryRejectsSingleFrameChanges() {
  mohu::StableGuestDisplayGeometry geometry;
  constexpr mohu::GuestDisplayGeometry gameplay{368U, 240U, false, false};
  constexpr mohu::GuestDisplayGeometry reset{256U, 240U, false, false};

  require(geometry.update(gameplay) == gameplay,
          "First valid guest geometry was not accepted");
  require(geometry.update(reset) == gameplay,
          "One-frame GP1 reset changed the committed scanout geometry");
  require(geometry.update(gameplay) == gameplay,
          "Returning gameplay mode did not cancel the transient candidate");

  for (int frame = 0; frame < 6; ++frame) {
    const auto observed = (frame & 1) == 0 ? reset : gameplay;
    require(geometry.update(observed) == gameplay,
            "Alternating GP1 modes changed presentation geometry");
  }
}

void testGuestDisplayGeometryCommitsRealTransitions() {
  mohu::StableGuestDisplayGeometry geometry;
  constexpr mohu::GuestDisplayGeometry gameplay{368U, 240U, false, false};
  constexpr mohu::GuestDisplayGeometry movie{320U, 240U, true, false};

  require(geometry.update(gameplay) == gameplay,
          "Gameplay geometry initialization failed");
  require(geometry.update(movie) == gameplay,
          "Guest geometry changed before confirmation");
  require(geometry.update(movie) == movie,
          "Stable movie geometry was not committed");
}

void testGuestRenderExtentUsesSelectedResolution() {
  const auto menu = PsyX_CalculateGuestRenderExtent(
      1280, 720, PSYX_ASPECT_ADAPTIVE, 320, 240, 256, 240, 1);
  const auto gameplay = PsyX_CalculateGuestRenderExtent(
      1280, 720, PSYX_ASPECT_ADAPTIVE, 512, 240, 512, 240, 1);
  require(menu.w == 1280 && menu.h == 720 && gameplay.w == 1280 &&
              gameplay.h == 720,
          "Menu and gameplay roots did not share the selected pixel target");

  const auto original = PsyX_CalculateGuestRenderExtent(
      1920, 1080, PSYX_ASPECT_ORIGINAL_4_3, 512, 240, 512, 240, 1);
  require(original.w == 1440 && original.h == 1080,
          "Fixed 4:3 guest root did not use the selected 4:3 viewport");
}

void testEverySelectedResolutionDefinesGuestTarget() {
  constexpr std::array resolutions{
      PsyXPresentationViewport{0, 0, 640, 480},
      PsyXPresentationViewport{0, 0, 800, 600},
      PsyXPresentationViewport{0, 0, 1280, 720},
      PsyXPresentationViewport{0, 0, 1600, 900},
      PsyXPresentationViewport{0, 0, 1920, 1080},
      PsyXPresentationViewport{0, 0, 2560, 1440},
      PsyXPresentationViewport{0, 0, 3440, 1440},
      PsyXPresentationViewport{0, 0, 3840, 2160},
  };
  for (const auto resolution : resolutions) {
    const auto extent = PsyX_CalculateGuestRenderExtent(
        resolution.w, resolution.h, PSYX_ASPECT_ADAPTIVE, 512, 240, 512, 240,
        1);
    require(extent.w == resolution.w && extent.h == resolution.h,
            "Selected resolution did not define the guest root target");
  }
}

void testGuestRenderExtentMapsContainedPages() {
  const auto half_width = PsyX_CalculateGuestRenderExtent(
      1920, 1080, PSYX_ASPECT_ORIGINAL_4_3, 256, 240, 512, 240, 0);
  require(half_width.w == 720 && half_width.h == 1080,
          "Contained 256x240 page did not map to half a 512x240 root");

  const auto quarter = PsyX_CalculateGuestRenderExtent(
      1280, 720, PSYX_ASPECT_ADAPTIVE, 256, 120, 512, 240, 0);
  require(quarter.w == 640 && quarter.h == 360,
          "Contained page did not preserve rational edge proportions");

  const auto undersized_root = PsyX_CalculateGuestRenderExtent(
      128, 128, PSYX_ASPECT_ADAPTIVE, 1024, 512, 64, 64, 1);
  require(undersized_root.w == 1024 && undersized_root.h == 512,
          "Selected target downsampled a wide logical root below PS1 1x");

  const auto undersized_contained = PsyX_CalculateGuestRenderExtent(
      128, 128, PSYX_ASPECT_ADAPTIVE, 256, 240, 512, 240, 0);
  require(undersized_contained.w == 256 && undersized_contained.h == 240,
          "Selected target downsampled a contained PS1 word domain");
}
} // namespace

int main() {
  try {
    testOriginalFourThree();
    testAdaptiveUsesEntireDrawable();
    testOutputPreservesSelectedResolutionAspect();
    testAuthoredContentAlwaysUsesFourThree();
    testAdaptivePreservesPixelAspect();
    testAdaptiveWorldFrustumMatchesPresentation();
    testAdaptiveWorldFrustumAcrossWideAspects();
    testAdaptiveWorldFrustumCoversEveryRetailCullPath();
    testRuntimePcDispatchIsExact();
    testInvalidSizeIsBounded();
    testGuestRenderExtentUsesSelectedResolution();
    testEverySelectedResolutionDefinesGuestTarget();
    testGuestRenderExtentMapsContainedPages();
    testGuestDisplayGeometryRejectsSingleFrameChanges();
    testGuestDisplayGeometryCommitsRealTransitions();
    std::cout << "Aspect ratio tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Aspect ratio tests failed: " << error.what() << '\n';
    return 1;
  }
}
