#include "mohu/frontend_menu.hpp"

#include "sf/psx/r3000_runtime.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace mohu {
namespace {

constexpr std::uint32_t frontend_overlay_marker = 0x8003b600U;
constexpr std::uint32_t active_screen_pointer = 0x80084698U;
constexpr std::uint32_t shell_overlay_id = 7U;
constexpr std::uint16_t logical_width = 512U;
constexpr std::uint16_t logical_height = 240U;

constexpr std::uint32_t nested_selection_flag = 0x80000000U;
constexpr std::uint32_t nested_selection_component_mask = 0xffffU;

[[nodiscard]] bool isGuestRamPointer(std::uint32_t address) noexcept {
  return address >= 0x80000000U && address < 0x80200000U;
}

[[nodiscard]] bool readWord(const sf::psx::R3000Runtime &runtime,
                            std::uint32_t address,
                            std::uint32_t &value) noexcept {
  return isGuestRamPointer(address) && runtime.read32(address, value);
}

[[nodiscard]] bool readName(const sf::psx::R3000Runtime &runtime,
                            std::uint32_t address,
                            std::array<char, 32U> &name) noexcept {
  for (std::size_t index{}; index + 1U < name.size(); ++index) {
    std::uint8_t value{};
    if (!runtime.read8(address + static_cast<std::uint32_t>(index), value)) {
      return false;
    }
    name[index] = static_cast<char>(value);
    if (value == 0U) {
      return index != 0U;
    }
  }
  name.back() = '\0';
  return false;
}

[[nodiscard]] std::uint32_t screenHash(std::string_view name) noexcept {
  auto hash = std::uint32_t{2166136261U};
  for (const auto character : name) {
    hash ^= static_cast<std::uint8_t>(character);
    hash *= 16777619U;
  }
  return hash;
}

[[nodiscard]] bool readVisualExtent(const sf::psx::R3000Runtime &runtime,
                                    std::uint32_t visual, std::uint16_t &width,
                                    std::uint16_t &height) noexcept {
  std::uint8_t rotated{};
  std::uint8_t explicit_extent{};
  if (!runtime.read8(visual + 0x36U, rotated) ||
      !runtime.read8(visual + 0x37U, explicit_extent)) {
    return false;
  }

  std::uint32_t descriptor_slot{};
  std::uint32_t descriptor{};
  const auto descriptor_extent =
      readWord(runtime, visual + 0x1cU, descriptor_slot) &&
      readWord(runtime, descriptor_slot, descriptor) &&
      isGuestRamPointer(descriptor) &&
      runtime.read16(descriptor + 0x20U, width) &&
      runtime.read16(descriptor + 0x24U, height) && width != 0U && height != 0U;
  if (!descriptor_extent) {
    if (explicit_extent == 0U || !runtime.read16(visual + 0x32U, width) ||
        !runtime.read16(visual + 0x34U, height) || width == 0U ||
        height == 0U) {
      return false;
    }
  }
  if (rotated != 0U) {
    std::swap(width, height);
  }
  return true;
}

[[nodiscard]] bool readRectangle(const sf::psx::R3000Runtime &runtime,
                                 std::uint32_t visual,
                                 FrontendMenuTarget &target) noexcept {
  std::uint16_t raw_x{};
  std::uint16_t raw_y{};
  if (!isGuestRamPointer(visual) || !runtime.read16(visual + 0x2eU, raw_x) ||
      !runtime.read16(visual + 0x30U, raw_y) ||
      !readVisualExtent(runtime, visual, target.width, target.height)) {
    return false;
  }
  target.x = static_cast<std::int16_t>(raw_x);
  target.y = static_cast<std::int16_t>(raw_y);
  if (target.width > logical_width || target.height > logical_height) {
    return false;
  }
  const auto right = static_cast<std::int32_t>(target.x) + target.width;
  const auto bottom = static_cast<std::int32_t>(target.y) + target.height;
  return right > 0 && bottom > 0 && target.x < logical_width &&
         target.y < logical_height;
}

[[nodiscard]] bool readTargetRectangle(const sf::psx::R3000Runtime &runtime,
                                       std::uint32_t child,
                                       FrontendMenuTarget &target) noexcept {
  std::uint8_t disabled{};
  if (!runtime.read8(child + 0x51U, disabled) || disabled != 0U) {
    return false;
  }

  constexpr std::array<std::uint32_t, 2U> visual_offsets{0x74U, 0x70U};
  for (const auto offset : visual_offsets) {
    std::uint32_t visual{};
    if (!readWord(runtime, child + offset, visual)) {
      return false;
    }
    if (visual == 0U) {
      continue;
    }
    std::uint8_t hidden{};
    if (!runtime.read8(visual + 0x39U, hidden)) {
      return false;
    }
    if (hidden == 0U && readRectangle(runtime, visual, target)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool isKnownNestedContainer(const sf::psx::R3000Runtime &runtime,
                                          std::string_view screen_name,
                                          std::uint32_t child) noexcept {
  std::array<char, 32U> child_name{};
  if (!readName(runtime, child, child_name)) {
    return false;
  }
  const auto name = std::string_view{child_name.data()};
  if (screen_name == "m_enigma" && name == "m_enigma_i_keys") {
    return true;
  }
  if (screen_name == "m_pname" && name == "m_pname_i_keys") {
    return true;
  }
  return screen_name == "m_mission" &&
         (name == "m_mission_mission_boxes" || name == "m_mission_level_boxes");
}

[[nodiscard]] constexpr std::uint32_t
nestedSelection(std::uint32_t outer, std::uint32_t inner) noexcept {
  return nested_selection_flag | ((outer & 0x7fffU) << 16U) |
         (inner & nested_selection_component_mask);
}

struct NestedTargetList {
  bool navigation{};
  bool selected_valid{};
  std::uint32_t selected{};
};

[[nodiscard]] NestedTargetList
collectNestedTargets(const sf::psx::R3000Runtime &runtime,
                     std::string_view screen_name, std::uint32_t child,
                     std::uint32_t outer_selection, bool outer_selected,
                     FrontendMenuState &result) noexcept {
  std::uint8_t child_disabled{};
  std::uint32_t first{};
  std::uint32_t selected_visual{};
  std::uint32_t visual_count{};
  std::uint32_t binding{};
  if (!runtime.read8(child + 0x51U, child_disabled) ||
      !readWord(runtime, child + 0x78U, first) ||
      !readWord(runtime, child + 0x74U, selected_visual) ||
      !runtime.read32(child + 0x80U, visual_count) ||
      !runtime.read32(child + 0x84U, binding)) {
    return {};
  }

  const auto known_container =
      isKnownNestedContainer(runtime, screen_name, child);
  const auto navigation = known_container || isGuestRamPointer(binding);
  if (!navigation || first == 0U || visual_count == 0U ||
      outer_selection > 0x7fffU) {
    return {};
  }
  if (child_disabled != 0U) {
    return {.navigation = true};
  }
  if (!isGuestRamPointer(first) ||
      (binding != 0U && !isGuestRamPointer(binding)) ||
      visual_count > FrontendMenuState::maximum_targets) {
    return {.navigation = true};
  }

  NestedTargetList list{.navigation = true};
  std::array<std::uint32_t, FrontendMenuState::maximum_targets> visited{};
  auto visual = first;
  for (std::size_t visit{}; visit < visual_count; ++visit) {
    if (!isGuestRamPointer(visual)) {
      return {.navigation = true};
    }
    for (std::size_t previous{}; previous < visit; ++previous) {
      if (visited[previous] == visual) {
        return {.navigation = true};
      }
    }
    visited[visit] = visual;

    std::uint32_t inner_selection{};
    std::uint32_t next{};
    std::uint8_t hidden{};
    std::uint8_t disabled{};
    if (!runtime.read32(visual + 0xd8U, inner_selection) ||
        !runtime.read32(visual + 0xd0U, next) ||
        !runtime.read8(visual + 0x39U, hidden) ||
        !runtime.read8(visual + 0x3aU, disabled)) {
      return {.navigation = true};
    }
    if (inner_selection <= nested_selection_component_mask && hidden == 0U &&
        disabled == 0U &&
        result.target_count < FrontendMenuState::maximum_targets) {
      FrontendMenuTarget target{
          .selection = nestedSelection(outer_selection, inner_selection)};
      if (readRectangle(runtime, visual, target)) {
        result.targets[result.target_count++] = target;
        if (outer_selected && visual == selected_visual) {
          list.selected = target.selection;
          list.selected_valid = true;
        }
      }
    }
    visual = next;
    if ((visit + 1U < visual_count && visual == 0U) ||
        (visit + 1U == visual_count && visual != 0U)) {
      return {.navigation = true};
    }
  }
  return list;
}

[[nodiscard]] bool selectNestedTarget(sf::psx::R3000Runtime &runtime,
                                      std::string_view screen_name,
                                      std::uint32_t root, std::uint32_t child,
                                      std::uint32_t outer_selection,
                                      std::uint32_t requested) noexcept {
  if ((requested & nested_selection_flag) == 0U || outer_selection > 0x7fffU ||
      ((requested >> 16U) & 0x7fffU) != outer_selection) {
    return false;
  }

  std::uint8_t child_disabled{};
  std::uint32_t visual{};
  std::uint32_t visual_count{};
  std::uint32_t nested_binding{};
  if (!runtime.read8(child + 0x51U, child_disabled) || child_disabled != 0U ||
      !readWord(runtime, child + 0x78U, visual) ||
      !runtime.read32(child + 0x80U, visual_count) ||
      !runtime.read32(child + 0x84U, nested_binding) || visual_count == 0U ||
      visual_count > FrontendMenuState::maximum_targets ||
      (!isKnownNestedContainer(runtime, screen_name, child) &&
       !isGuestRamPointer(nested_binding)) ||
      (nested_binding != 0U && !isGuestRamPointer(nested_binding))) {
    return false;
  }

  const auto requested_inner = requested & nested_selection_component_mask;
  std::array<std::uint32_t, FrontendMenuState::maximum_targets> visited{};
  for (std::size_t visit{}; visit < visual_count; ++visit) {
    if (!isGuestRamPointer(visual)) {
      return false;
    }
    for (std::size_t previous{}; previous < visit; ++previous) {
      if (visited[previous] == visual) {
        return false;
      }
    }
    visited[visit] = visual;

    std::uint32_t inner_selection{};
    std::uint32_t next{};
    std::uint8_t hidden{};
    std::uint8_t disabled{};
    FrontendMenuTarget rectangle{};
    if (!runtime.read32(visual + 0xd8U, inner_selection) ||
        !runtime.read32(visual + 0xd0U, next) ||
        !runtime.read8(visual + 0x39U, hidden) ||
        !runtime.read8(visual + 0x3aU, disabled)) {
      return false;
    }
    if (inner_selection == requested_inner && hidden == 0U && disabled == 0U &&
        readRectangle(runtime, visual, rectangle)) {
      std::uint32_t root_binding{};
      if (!runtime.read32(root + 0x2cU, root_binding) ||
          (root_binding != 0U && !isGuestRamPointer(root_binding))) {
        return false;
      }
      if ((root_binding != 0U &&
           !runtime.write32(root_binding, outer_selection)) ||
          (nested_binding != 0U &&
           !runtime.write32(nested_binding, inner_selection)) ||
          !runtime.write32(root + 0x1cU, child) ||
          !runtime.write32(child + 0x74U, visual)) {
        return false;
      }
      return true;
    }
    visual = next;
  }
  return false;
}

} // namespace

FrontendMenuState
inspectFrontendMenu(const sf::psx::R3000Runtime &runtime) noexcept {
  FrontendMenuState result;
  std::uint32_t overlay{};
  std::uint32_t root{};
  if (!runtime.read32(frontend_overlay_marker, overlay) ||
      overlay != shell_overlay_id ||
      !runtime.read32(active_screen_pointer, root) ||
      !isGuestRamPointer(root)) {
    return result;
  }

  std::array<char, 32U> name{};
  if (!readName(runtime, root, name)) {
    return result;
  }
  const auto screen_name = std::string_view{name.data()};
  if (!screen_name.starts_with("m_") || screen_name.starts_with("m_movie_")) {
    return result;
  }

  std::uint32_t selected_child{};
  std::uint32_t child{};
  if (!readWord(runtime, root + 0x1cU, selected_child) ||
      !readWord(runtime, root + 0x20U, child)) {
    return result;
  }

  std::array<std::uint32_t, FrontendMenuState::maximum_targets> visited{};
  auto selected_resolved = false;
  for (std::size_t visit{};
       child != 0U && visit < FrontendMenuState::maximum_targets; ++visit) {
    if (!isGuestRamPointer(child)) {
      return {};
    }
    for (std::size_t previous{}; previous < visit; ++previous) {
      if (visited[previous] == child) {
        return {};
      }
    }
    visited[visit] = child;

    std::uint32_t selection{};
    std::uint32_t next{};
    if (!readWord(runtime, child + 0x38U, selection) ||
        !readWord(runtime, child + 0xb4U, next)) {
      return {};
    }
    FrontendMenuTarget target{.selection = selection};
    const auto nested =
        collectNestedTargets(runtime, screen_name, child, selection,
                             child == selected_child, result);
    if (nested.navigation) {
      if (nested.selected_valid) {
        result.selected = nested.selected;
        selected_resolved = true;
      }
    } else if (readTargetRectangle(runtime, child, target) &&
               result.target_count < FrontendMenuState::maximum_targets) {
      result.targets[result.target_count++] = target;
      if (child == selected_child) {
        result.selected = selection;
        selected_resolved = true;
      }
    }
    child = next;
  }

  if (result.target_count == 0U) {
    return {};
  }
  result.screen_id = screenHash(screen_name);
  if (!selected_resolved && selected_child != 0U) {
    std::uint32_t selected{};
    if (readWord(runtime, selected_child + 0x38U, selected)) {
      result.selected = selected;
    }
  }
  if (!selected_resolved) {
    result.selected = result.targets.front().selection;
  }
  result.active = true;
  return result;
}

bool selectFrontendMenuTarget(sf::psx::R3000Runtime &runtime,
                              std::uint32_t screen_id,
                              std::uint32_t selection) noexcept {
  std::uint32_t overlay{};
  std::uint32_t root{};
  if (!runtime.read32(frontend_overlay_marker, overlay) ||
      overlay != shell_overlay_id ||
      !runtime.read32(active_screen_pointer, root) ||
      !isGuestRamPointer(root)) {
    return false;
  }

  std::array<char, 32U> name{};
  if (!readName(runtime, root, name)) {
    return false;
  }
  const auto screen_name = std::string_view{name.data()};
  if (!screen_name.starts_with("m_") || screen_name.starts_with("m_movie_") ||
      screenHash(screen_name) != screen_id) {
    return false;
  }

  std::uint32_t child{};
  if (!readWord(runtime, root + 0x20U, child)) {
    return false;
  }
  std::array<std::uint32_t, FrontendMenuState::maximum_targets> visited{};
  for (std::size_t visit{};
       child != 0U && visit < FrontendMenuState::maximum_targets; ++visit) {
    if (!isGuestRamPointer(child)) {
      return false;
    }
    for (std::size_t previous{}; previous < visit; ++previous) {
      if (visited[previous] == child) {
        return false;
      }
    }
    visited[visit] = child;

    std::uint32_t child_selection{};
    std::uint32_t next{};
    if (!readWord(runtime, child + 0x38U, child_selection) ||
        !readWord(runtime, child + 0xb4U, next)) {
      return false;
    }
    FrontendMenuTarget target{.selection = child_selection};
    if (selectNestedTarget(runtime, screen_name, root, child, child_selection,
                           selection)) {
      return true;
    }
    if (child_selection == selection &&
        readTargetRectangle(runtime, child, target)) {
      std::uint32_t binding{};
      if (!readWord(runtime, root + 0x2cU, binding) ||
          (binding != 0U && !isGuestRamPointer(binding))) {
        return false;
      }
      if (binding != 0U && !runtime.write32(binding, selection)) {
        return false;
      }
      return runtime.write32(root + 0x1cU, child);
    }
    child = next;
  }
  return false;
}

} // namespace mohu
