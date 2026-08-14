#pragma once

#include "sf/platform/host.hpp"

#include <cstdint>

namespace sf::platform {

struct RuntimePresentationSample {
  std::uint16_t display_width{256U};
  std::uint16_t display_height{240U};
  bool rgb24{};
  bool interlaced{};
  bool has_scene_geometry{};
};

class RuntimePresentationPolicy final {
public:
  [[nodiscard]] PresentationContent
  update(RuntimePresentationSample sample) noexcept {
    if (isAuthoredMode(sample)) {
      content_ = PresentationContent::authored_4_3;
      ambiguous_frames_ = 0U;
      return content_;
    }
    if (isGameplayMode(sample) || sample.has_scene_geometry) {
      content_ = PresentationContent::gameplay;
      ambiguous_frames_ = 0U;
      return content_;
    }
    if (content_ == PresentationContent::gameplay &&
        ++ambiguous_frames_ >= ambiguous_authored_confirmation_frames) {
      content_ = PresentationContent::authored_4_3;
      ambiguous_frames_ = 0U;
    }
    return content_;
  }

  [[nodiscard]] PresentationContent content() const noexcept {
    return content_;
  }

  static constexpr std::uint8_t ambiguous_authored_confirmation_frames = 2U;

private:
  [[nodiscard]] static constexpr bool
  isAuthoredMode(RuntimePresentationSample sample) noexcept {
    const auto authored_width =
        sample.display_width == 256U || sample.display_width == 320U;
    return sample.rgb24 || sample.interlaced ||
           (authored_width && sample.display_height <= 240U);
  }

  [[nodiscard]] static constexpr bool
  isGameplayMode(RuntimePresentationSample sample) noexcept {
    const auto gameplay_width =
        sample.display_width == 368U || sample.display_width == 384U;
    return gameplay_width && sample.display_height <= 240U;
  }

  PresentationContent content_{PresentationContent::authored_4_3};
  std::uint8_t ambiguous_frames_{};
};

} // namespace sf::platform
