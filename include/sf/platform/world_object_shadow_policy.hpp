#pragma once

#include "sf/game/dynamic_lighting.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sf::platform {

// Renderer-derived facts for one separate map prop. Classification stays at
// the call site, which already owns the authoritative geometry/material
// branches; this policy only enforces the camera-independent caster contract.
struct WorldObjectShadowCasterFacts {
  game::DynamicLightPoint anchor{};
  double bounding_radius{};
  std::size_t triangle_count{};
  bool resident{};
  bool opaque_geometry{};
  bool authoritative_transform{};
  bool actor_shadow_owned{};
  bool embedded_world_geometry{};
};

[[nodiscard]] inline bool worldObjectShadowCasterEligible(
    const WorldObjectShadowCasterFacts &facts) noexcept {
  const auto finite_anchor = std::isfinite(facts.anchor.x) &&
                             std::isfinite(facts.anchor.y) &&
                             std::isfinite(facts.anchor.z);
  const auto finite_positive_bounds =
      std::isfinite(facts.bounding_radius) && facts.bounding_radius > 0.0;
  return facts.resident && facts.opaque_geometry &&
         facts.authoritative_transform && !facts.actor_shadow_owned &&
         !facts.embedded_world_geometry && facts.triangle_count != 0U &&
         finite_anchor && finite_positive_bounds;
}

// Object shadows must not inherit the camera observer used to bound the
// general lighting frame. Build the bounded frame around this caster instead,
// so the same nearby authored lights remain selected when more than 32 sources
// are resident and the camera moves. Short-lived and actor-owned lights are
// intentionally absent from the stable map-object silhouette.
[[nodiscard]] inline game::DynamicShadowProjection
selectWorldObjectShadowProjection(
    std::span<const game::PersistentDynamicLightState> persistent_lights,
    const WorldObjectShadowCasterFacts &facts,
    game::DynamicLightPoint receiver_normal,
    std::uint64_t animation_tick = 0U) noexcept {
  if (!worldObjectShadowCasterEligible(facts)) {
    return {};
  }
  const auto caster_frame = game::buildDynamicLightFrame(
      persistent_lights, {}, facts.anchor, {}, animation_tick);
  return game::selectDynamicShadowProjection(caster_frame, facts.anchor,
                                             receiver_normal);
}

} // namespace sf::platform
