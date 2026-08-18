#include "mohu/frontend_menu.hpp"
#include "sf/psx/r3000_runtime.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error{message};
}

void writeName(sf::psx::R3000Runtime &runtime, std::uint32_t address,
               std::string_view name) {
  for (std::size_t index{}; index < name.size(); ++index) {
    require(runtime.write8(address + static_cast<std::uint32_t>(index),
                           static_cast<std::uint8_t>(name[index])),
            "Could not write a frontend name");
  }
  require(runtime.write8(address + static_cast<std::uint32_t>(name.size()), 0U),
          "Could not terminate a frontend name");
}

void writeTarget(sf::psx::R3000Runtime &runtime, std::uint32_t child,
                 std::uint32_t visual, std::uint32_t selection, std::int16_t x,
                 std::int16_t y, std::uint16_t width, std::uint16_t height,
                 std::uint32_t next) {
  require(runtime.write32(child + 0x38U, selection) &&
              runtime.write32(child + 0x74U, visual) &&
              runtime.write32(child + 0xb4U, next) &&
              runtime.write16(visual + 0x2eU, static_cast<std::uint16_t>(x)) &&
              runtime.write16(visual + 0x30U, static_cast<std::uint16_t>(y)) &&
              runtime.write16(visual + 0x32U, width) &&
              runtime.write16(visual + 0x34U, height) &&
              runtime.write8(visual + 0x37U, 1U),
          "Could not build a frontend target");
}

void writeNestedVisual(sf::psx::R3000Runtime &runtime, std::uint32_t visual,
                       std::uint32_t descriptor_slot, std::uint32_t descriptor,
                       std::uint32_t selection, std::int16_t x, std::int16_t y,
                       std::uint16_t width, std::uint16_t height,
                       std::uint32_t next) {
  require(runtime.write32(visual + 0x1cU, descriptor_slot) &&
              runtime.write16(visual + 0x2eU, static_cast<std::uint16_t>(x)) &&
              runtime.write16(visual + 0x30U, static_cast<std::uint16_t>(y)) &&
              runtime.write32(visual + 0xd0U, next) &&
              runtime.write32(visual + 0xd8U, selection) &&
              runtime.write32(descriptor_slot, descriptor) &&
              runtime.write16(descriptor + 0x20U, width) &&
              runtime.write16(descriptor + 0x24U, height),
          "Could not build a nested frontend visual");
}

void testExactFrontendTargets() {
  sf::psx::R3000Runtime runtime;
  constexpr std::uint32_t root = 0x80010000U;
  constexpr std::uint32_t first = 0x80010100U;
  constexpr std::uint32_t second = 0x80010200U;
  constexpr std::uint32_t first_visual = 0x80010300U;
  constexpr std::uint32_t second_visual = 0x80010400U;
  constexpr std::uint32_t selection_binding = 0x80010500U;
  require(runtime.write32(0x8003b600U, 7U) &&
              runtime.write32(0x80084698U, root) &&
              runtime.write32(root + 0x1cU, second) &&
              runtime.write32(root + 0x20U, first) &&
              runtime.write32(root + 0x2cU, selection_binding) &&
              runtime.write32(selection_binding, 1U),
          "Could not build the frontend root");
  writeName(runtime, root, "m_option");
  writeTarget(runtime, first, first_visual, 0U, 12, 24, 80U, 18U, second);
  writeTarget(runtime, second, second_visual, 1U, 110, 60, 96U, 22U, 0U);

  const auto menu = mohu::inspectFrontendMenu(runtime);
  require(menu.active && menu.screen_id != 0U && menu.selected == 1U &&
              menu.target_count == 2U,
          "Frontend root state was not recovered");
  require(menu.targets[0].selection == 0U && menu.targets[0].x == 12 &&
              menu.targets[0].y == 24 && menu.targets[0].width == 80U &&
              menu.targets[0].height == 18U &&
              menu.targets[1].selection == 1U && menu.targets[1].x == 110 &&
              menu.targets[1].y == 60,
          "Frontend authored rectangles were not recovered exactly");

  require(mohu::selectFrontendMenuTarget(runtime, menu.screen_id, 0U),
          "An authored frontend option could not be selected directly");
  std::uint32_t selected_child{};
  std::uint32_t bound_selection{};
  require(runtime.read32(root + 0x1cU, selected_child) &&
              runtime.read32(selection_binding, bound_selection) &&
              selected_child == first && bound_selection == 0U,
          "Direct frontend selection did not update both guest states");
  require(!mohu::selectFrontendMenuTarget(runtime, menu.screen_id + 1U, 1U),
          "A stale screen identity changed the current frontend");
  require(runtime.read32(root + 0x1cU, selected_child) &&
              selected_child == first,
          "Rejected frontend selection still modified guest state");
}

void testVisibleFallbackAndDisabledTargets() {
  sf::psx::R3000Runtime runtime;
  constexpr std::uint32_t root = 0x80011000U;
  constexpr std::uint32_t enabled = 0x80011100U;
  constexpr std::uint32_t disabled = 0x80011200U;
  constexpr std::uint32_t hidden_visual = 0x80011300U;
  constexpr std::uint32_t fallback_visual = 0x80011400U;
  constexpr std::uint32_t disabled_visual = 0x80011500U;
  require(runtime.write32(0x8003b600U, 7U) &&
              runtime.write32(0x80084698U, root) &&
              runtime.write32(root + 0x1cU, enabled) &&
              runtime.write32(root + 0x20U, enabled),
          "Could not build a visibility frontend root");
  writeName(runtime, root, "m_visibility");
  writeTarget(runtime, enabled, hidden_visual, 0U, 1, 2, 30U, 10U, disabled);
  writeTarget(runtime, disabled, disabled_visual, 1U, 80, 90, 40U, 12U, 0U);
  require(runtime.write8(hidden_visual + 0x39U, 1U) &&
              runtime.write32(enabled + 0x70U, fallback_visual) &&
              runtime.write16(fallback_visual + 0x2eU, 25U) &&
              runtime.write16(fallback_visual + 0x30U, 35U) &&
              runtime.write16(fallback_visual + 0x32U, 90U) &&
              runtime.write16(fallback_visual + 0x34U, 18U) &&
              runtime.write8(fallback_visual + 0x37U, 1U) &&
              runtime.write8(disabled + 0x51U, 1U),
          "Could not build visible fallback targets");

  const auto menu = mohu::inspectFrontendMenu(runtime);
  require(menu.active && menu.target_count == 1U &&
              menu.targets[0].selection == 0U && menu.targets[0].x == 25 &&
              menu.targets[0].y == 35 && menu.targets[0].width == 90U &&
              menu.targets[0].height == 18U,
          "Visible fallback or disabled target state was ignored");
  require(!mohu::selectFrontendMenuTarget(runtime, menu.screen_id, 1U),
          "A disabled frontend target was selected directly");
}

void testTextureDescriptorExtent() {
  sf::psx::R3000Runtime runtime;
  constexpr std::uint32_t root = 0x80012000U;
  constexpr std::uint32_t child = 0x80012100U;
  constexpr std::uint32_t visual = 0x80012200U;
  constexpr std::uint32_t descriptor_slot = 0x80012300U;
  constexpr std::uint32_t descriptor = 0x80012400U;
  require(runtime.write32(0x8003b600U, 7U) &&
              runtime.write32(0x80084698U, root) &&
              runtime.write32(root + 0x1cU, child) &&
              runtime.write32(root + 0x20U, child),
          "Could not build a textured frontend root");
  writeName(runtime, root, "m_textured");
  writeTarget(runtime, child, visual, 0U, 40, 50, 1U, 1U, 0U);
  require(runtime.write8(visual + 0x37U, 0U) &&
              runtime.write32(visual + 0x1cU, descriptor_slot) &&
              runtime.write32(descriptor_slot, descriptor) &&
              runtime.write16(descriptor + 0x20U, 104U) &&
              runtime.write16(descriptor + 0x24U, 20U),
          "Could not build a texture descriptor extent");

  auto menu = mohu::inspectFrontendMenu(runtime);
  require(menu.active && menu.target_count == 1U && menu.targets[0].x == 40 &&
              menu.targets[0].y == 50 && menu.targets[0].width == 104U &&
              menu.targets[0].height == 20U,
          "Texture descriptor dimensions were not used for hit testing");

  require(runtime.write8(visual + 0x36U, 1U),
          "Could not rotate a textured frontend target");
  menu = mohu::inspectFrontendMenu(runtime);
  require(menu.active && menu.target_count == 1U &&
              menu.targets[0].width == 20U && menu.targets[0].height == 104U,
          "Rotated texture dimensions were not swapped");
}

void testNestedFrontendGridTargets() {
  sf::psx::R3000Runtime runtime;
  constexpr std::uint32_t root = 0x80013000U;
  constexpr std::uint32_t child = 0x80013100U;
  constexpr std::uint32_t first = 0x80013200U;
  constexpr std::uint32_t second = 0x80013300U;
  constexpr std::uint32_t disabled = 0x80013400U;
  constexpr std::uint32_t first_slot = 0x80013500U;
  constexpr std::uint32_t first_descriptor = 0x80013600U;
  constexpr std::uint32_t second_slot = 0x80013700U;
  constexpr std::uint32_t second_descriptor = 0x80013800U;
  constexpr std::uint32_t disabled_slot = 0x80013900U;
  constexpr std::uint32_t disabled_descriptor = 0x80013a00U;
  constexpr std::uint32_t outer_binding = 0x80013b00U;
  constexpr std::uint32_t inner_binding = 0x80013b04U;
  require(runtime.write32(0x8003b600U, 7U) &&
              runtime.write32(0x80084698U, root) &&
              runtime.write32(root + 0x1cU, child) &&
              runtime.write32(root + 0x20U, child) &&
              runtime.write32(root + 0x2cU, outer_binding) &&
              runtime.write32(child + 0x38U, 3U) &&
              runtime.write32(child + 0x74U, second) &&
              runtime.write32(child + 0x78U, first) &&
              runtime.write32(child + 0x80U, 3U) &&
              runtime.write32(child + 0x84U, inner_binding),
          "Could not build a nested frontend root");
  writeName(runtime, root, "m_enigma");
  writeName(runtime, child, "m_enigma_i_keys");
  writeNestedVisual(runtime, first, first_slot, first_descriptor, 10U, 20, 30,
                    18U, 16U, second);
  writeNestedVisual(runtime, second, second_slot, second_descriptor, 11U, 44,
                    30, 18U, 16U, disabled);
  writeNestedVisual(runtime, disabled, disabled_slot, disabled_descriptor, 12U,
                    68, 30, 18U, 16U, 0U);
  require(runtime.write16(first + 0x32U, 0x41U) &&
              runtime.write16(first + 0x34U, 0x309U) &&
              runtime.write8(first + 0x37U, 1U),
          "Could not add Enigma character metadata");
  require(runtime.write8(disabled + 0x3aU, 1U),
          "Could not disable a nested frontend visual");

  const auto menu = mohu::inspectFrontendMenu(runtime);
  require(menu.active && menu.target_count == 2U && menu.targets[0].x == 20 &&
              menu.targets[0].y == 30 && menu.targets[1].x == 44 &&
              menu.targets[1].y == 30 && menu.targets[0].width == 18U &&
              menu.targets[0].height == 16U &&
              menu.selected == menu.targets[1].selection &&
              menu.targets[0].selection != menu.targets[1].selection,
          "Nested frontend grid was not exposed as exact pointer targets");
  require(mohu::selectFrontendMenuTarget(runtime, menu.screen_id,
                                         menu.targets[0].selection),
          "Nested frontend target could not be selected directly");

  std::uint32_t selected_child{};
  std::uint32_t selected_visual{};
  std::uint32_t outer_value{};
  std::uint32_t inner_value{};
  require(runtime.read32(root + 0x1cU, selected_child) &&
              runtime.read32(child + 0x74U, selected_visual) &&
              runtime.read32(outer_binding, outer_value) &&
              runtime.read32(inner_binding, inner_value) &&
              selected_child == child && selected_visual == first &&
              outer_value == 3U && inner_value == 10U,
          "Nested pointer selection did not update the complete guest state");
  require(!mohu::selectFrontendMenuTarget(runtime, menu.screen_id,
                                          menu.targets[1].selection + 1U),
          "Unknown nested selection changed the guest state");
}

void testNonInteractiveScreensFailClosed() {
  sf::psx::R3000Runtime runtime;
  constexpr std::uint32_t root = 0x80010000U;
  require(runtime.write32(0x8003b600U, 7U) &&
              runtime.write32(0x80084698U, root),
          "Could not build the movie root");
  writeName(runtime, root, "m_movie_ea");
  require(!mohu::inspectFrontendMenu(runtime).active,
          "A movie exposed a menu cursor");

  writeName(runtime, root, "m_option");
  constexpr std::uint32_t child = 0x80010100U;
  constexpr std::uint32_t visual = 0x80010200U;
  require(runtime.write32(root + 0x1cU, child) &&
              runtime.write32(root + 0x20U, child),
          "Could not build a malformed root");
  writeTarget(runtime, child, visual, 0U, 20, 20, 50U, 20U, child);
  require(!mohu::inspectFrontendMenu(runtime).active,
          "A cyclic target list was accepted");
}

} // namespace

int main() {
  try {
    testExactFrontendTargets();
    testVisibleFallbackAndDisabledTargets();
    testTextureDescriptorExtent();
    testNestedFrontendGridTargets();
    testNonInteractiveScreensFailClosed();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "MOHU frontend-menu tests passed\n";
  return 0;
}
