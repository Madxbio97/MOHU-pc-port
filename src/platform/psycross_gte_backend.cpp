#include "sf/platform/host.hpp"

#include <PsyX/PsyX_gte.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace sf::platform {
namespace {

[[nodiscard]] bool executePsyCrossProjection(
    void *, psx::GteState &state, std::uint32_t instruction,
    bool capture_projection) noexcept {
  std::array<PsyXGuestGteProjection, 3U> captured{};
  std::uint32_t count{};
  if (PsyX_GteExecuteGuestProjection(
          state.data.data(), state.control.data(), instruction,
          captured.data(), &count) == 0) {
    return false;
  }
  if (!capture_projection || count == 0U) {
    return true;
  }

  const auto convert = [](const PsyXGuestGteProjection &source) {
    psx::GteProjectedVertex destination{};
    destination.packed_sxy = source.packed_sxy;
    destination.view_x = source.view_x;
    destination.view_y = source.view_y;
    destination.view_z = source.view_z;
    destination.projective_depth = source.projective_depth;
    destination.screen_x = source.screen_x;
    destination.screen_y = source.screen_y;
    destination.screen_h = source.screen_h;
    destination.screen_offset_x = source.screen_offset_x;
    destination.screen_offset_y = source.screen_offset_y;
    destination.mesh_vertex_id = source.mesh_vertex_id;
    destination.valid = source.valid != 0U;
    destination.exact_transform = source.exact_projection != 0U;
    destination.fractional_transform =
        destination.valid &&
        (destination.screen_x != std::trunc(destination.screen_x) ||
         destination.screen_y != std::trunc(destination.screen_y));
    return destination;
  };

  if (count == 1U) {
    state.projected[0] = state.projected[1];
    state.projected[1] = state.projected[2];
    state.projected[2] = convert(captured[0]);
  } else if (count == 3U) {
    for (std::size_t index{}; index < state.projected.size(); ++index) {
      state.projected[index] = convert(captured[index]);
    }
  }
  return true;
}

} // namespace

psx::GteProjectionCommandBackend
psycrossGteProjectionCommandBackend() noexcept {
  return {nullptr, &executePsyCrossProjection};
}

} // namespace sf::platform
