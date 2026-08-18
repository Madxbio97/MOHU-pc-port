#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace mohu {

inline constexpr std::int32_t retail_world_x_max = 540;
inline constexpr std::int32_t retail_world_y_max = 240;

enum class AdaptiveWorldFrustumHook : std::uint8_t {
  none,
  bsp_upper_x,
  bsp_lower_x,
  multiplayer_bsp_force_visible,
  object_upper_x,
  level_triangle_outcode,
  slus_triangle_outcode,
};

// The retail level uses three independent visibility paths. Keep the lookup
// constexpr and exact: the overwhelmingly common path is one indexed key
// check. The runtime validates the instruction at
// every listed address before applying any register adjustment.
[[nodiscard]] constexpr AdaptiveWorldFrustumHook
adaptiveWorldFrustumHook(std::uint32_t guest_pc) noexcept {
  struct Entry {
    std::uint32_t pc{};
    AdaptiveWorldFrustumHook hook{};
  };
  // The low four instruction-index bits are unique for all eight verified
  // sites. This exact-key table replaces a sparse switch/binary-search in the
  // 33.9-million-instruction-per-second interpreter hot path.
  constexpr std::array<Entry, 16U> hooks{
      Entry{0x80010c00U, AdaptiveWorldFrustumHook::slus_triangle_outcode},
      Entry{0x800115c4U, AdaptiveWorldFrustumHook::slus_triangle_outcode},
      Entry{0x8009a2c8U, AdaptiveWorldFrustumHook::bsp_lower_x},
      Entry{},
      Entry{},
      Entry{},
      Entry{},
      Entry{0x8009ce1cU, AdaptiveWorldFrustumHook::object_upper_x},
      Entry{},
      Entry{},
      Entry{0x80099a28U, AdaptiveWorldFrustumHook::bsp_upper_x},
      Entry{0x80099dacU, AdaptiveWorldFrustumHook::bsp_lower_x},
      Entry{0x8009a8b0U, AdaptiveWorldFrustumHook::level_triangle_outcode},
      Entry{},
      Entry{},
      Entry{0x8009cd7cU, AdaptiveWorldFrustumHook::bsp_lower_x},
  };
  const auto &entry = hooks[(guest_pc >> 2U) & 0x0fU];
  return entry.pc == guest_pc ? entry.hook : AdaptiveWorldFrustumHook::none;
}

// The host presentation keeps the retail 4:3 projection and scales X by
// (4/3) / output_aspect. Widen only the guest world-culling interval by the
// inverse amount, symmetrically around the retail screen centre. The original
// projected coordinates remain untouched, so HUD and fullscreen packets keep
// their authored geometry.
[[nodiscard]] constexpr std::int32_t
adaptiveWorldXMargin(std::uint32_t output_width, std::uint32_t output_height,
                     bool adaptive) noexcept {
  if (!adaptive || output_width == 0U || output_height == 0U) {
    return 0;
  }

  const auto aspect_numerator = std::uint64_t{output_width} * 3U;
  const auto aspect_denominator = std::uint64_t{output_height} * 4U;
  if (aspect_numerator <= aspect_denominator) {
    return 0;
  }

  const auto expanded_width =
      (std::uint64_t{retail_world_x_max} * aspect_numerator +
       aspect_denominator / 2U) /
      aspect_denominator;
  const auto margin = static_cast<std::int32_t>(
      (expanded_width - std::uint64_t{retail_world_x_max}) / 2U);
  // SXY is signed 11-bit. Keeping the widened right edge at or below 1023
  // supports up to 32:9 while avoiding a culling promise GTE cannot project.
  return std::clamp(margin, 0, 1023 - retail_world_x_max);
}

} // namespace mohu
