#pragma once

#include <cstdint>

namespace mohu {

// GP1(08) can briefly expose reset/default geometry while overlays and the
// display controller are changing state. The display address must remain
// frame-accurate because it selects the current VRAM page, but accepting every
// one-frame geometry change makes the complete host image pulse between
// different shapes. Commit a new scanout geometry only after it is observed on
// two consecutive guest frames.
struct GuestDisplayGeometry {
  std::uint16_t width{256U};
  std::uint16_t height{240U};
  bool rgb24{};
  bool interlaced{};

  friend constexpr bool operator==(const GuestDisplayGeometry &,
                                   const GuestDisplayGeometry &) = default;
};

class StableGuestDisplayGeometry final {
public:
  static constexpr std::uint8_t confirmation_frames = 2U;

  [[nodiscard]] constexpr GuestDisplayGeometry
  update(GuestDisplayGeometry observed) noexcept {
    if (!initialized_) {
      initialized_ = true;
      committed_ = observed;
      candidate_ = observed;
      candidate_frames_ = 0U;
      return committed_;
    }

    if (observed == committed_) {
      candidate_ = committed_;
      candidate_frames_ = 0U;
      return committed_;
    }

    if (observed != candidate_) {
      candidate_ = observed;
      candidate_frames_ = 1U;
      return committed_;
    }

    if (candidate_frames_ < confirmation_frames) {
      ++candidate_frames_;
    }
    if (candidate_frames_ >= confirmation_frames) {
      committed_ = candidate_;
      candidate_frames_ = 0U;
    }
    return committed_;
  }

  [[nodiscard]] constexpr GuestDisplayGeometry
  update(GuestDisplayGeometry observed, bool stabilize) noexcept {
    if (!stabilize) {
      reset();
      return observed;
    }
    return update(observed);
  }

  constexpr void reset() noexcept {
    committed_ = {};
    candidate_ = {};
    candidate_frames_ = 0U;
    initialized_ = false;
  }

  [[nodiscard]] constexpr bool initialized() const noexcept {
    return initialized_;
  }

private:
  GuestDisplayGeometry committed_{};
  GuestDisplayGeometry candidate_{};
  std::uint8_t candidate_frames_{};
  bool initialized_{};
};

} // namespace mohu
