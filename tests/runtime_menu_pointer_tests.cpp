#include "psycross_runtime_guards.hpp"
#include "sf/platform/host.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error{message};
}

sf::platform::RuntimeMenuState menu(std::uint32_t selected) {
  return {
      .active = true,
      .screen_id = 1U,
      .selected = selected,
      .targets =
          {
              {.selection = 0U, .x = 10, .y = 10, .width = 80U, .height = 20U},
              {.selection = 1U, .x = 10, .y = 40, .width = 80U, .height = 20U},
              {.selection = 2U, .x = 10, .y = 70, .width = 80U, .height = 20U},
          },
  };
}

void testHoverNavigatesAndClickActivates() {
  sf::platform::detail::RuntimeMenuPointerController controller;
  sf::platform::detail::MenuPointerSample pointer{
      .x = 30.0F,
      .y = 75.0F,
      .inside = true,
      .moved = true,
  };
  auto action = controller.update(menu(0U), pointer);
  require(action.active_low_buttons == 0xffbfU && !action.selection_valid,
          "Hover did not navigate down toward the authored option");
  pointer.moved = false;
  action = controller.update(menu(1U), pointer);
  require(action.active_low_buttons == 0xffffU && !action.selection_valid,
          "Directional navigation was not released");
  action = controller.update(menu(1U), pointer);
  require(action.active_low_buttons == 0xffbfU && !action.selection_valid,
          "Hover did not continue toward the authored option");
  action = controller.update(menu(2U), pointer);
  require(action.active_low_buttons == 0xffffU && !action.selection_valid,
          "Final directional navigation was not released");
  action = controller.update(menu(2U), pointer);
  require(action.active_low_buttons == 0xffffU && !action.selection_valid,
          "Stationary cursor kept driving the selected option");
  action = controller.update(menu(1U), pointer);
  require(action.active_low_buttons == 0xffffU && !action.selection_valid,
          "Stationary cursor fought later guest navigation");

  pointer.primary_pressed = true;
  action = controller.update(menu(1U), pointer);
  require(action.active_low_buttons == 0xffbfU && !action.selection_valid,
          "Click did not navigate toward the hovered option");
  pointer.primary_pressed = false;
  action = controller.update(menu(2U), pointer);
  require(action.active_low_buttons == 0xffffU && !action.selection_valid,
          "Click navigation was not released before activation");
  action = controller.update(menu(2U), pointer);
  require(action.active_low_buttons == 0xbfffU && !action.selection_valid,
          "Staged click did not activate the selected option");
  action = controller.update(menu(2U), pointer);
  require(action.active_low_buttons == 0xffffU && !action.selection_valid,
          "Menu activation was not released");
}

void testBackAndOverlappingAuthoredZones() {
  sf::platform::detail::RuntimeMenuPointerController controller;
  auto overlapping = menu(1U);
  overlapping.targets[0].x = overlapping.targets[1].x;
  overlapping.targets[0].y = overlapping.targets[1].y;
  sf::platform::detail::MenuPointerSample pointer{
      .x = 30.0F,
      .y = 45.0F,
      .inside = true,
      .moved = true,
  };
  auto action = controller.update(overlapping, pointer);
  require(action.active_low_buttons == 0xffffU && !action.selection_valid,
          "An overlapping conditional option displaced the active one");
  pointer.moved = false;
  pointer.secondary_pressed = true;
  action = controller.update(overlapping, pointer);
  require(action.active_low_buttons == 0xefffU && !action.selection_valid,
          "Right click did not map to menu Back");
  pointer.secondary_pressed = false;
  action = controller.update(overlapping, pointer);
  require(action.active_low_buttons == 0xffffU, "Menu Back was not released");
}

void testGridHoverDoesNotUseGuestDirections() {
  sf::platform::detail::RuntimeMenuPointerController controller;
  auto grid = menu(0U);
  grid.targets[1] = {
      .selection = 1U,
      .x = 120,
      .y = 10,
      .width = 80U,
      .height = 20U,
  };
  const sf::platform::detail::MenuPointerSample pointer{
      .x = 140.0F,
      .y = 15.0F,
      .inside = true,
      .moved = true,
  };
  const auto action = controller.update(grid, pointer);
  require(action.active_low_buttons == 0xffdfU && !action.selection_valid,
          "A grid option did not navigate in its authored direction");
}

void testPointerOutsideOptionsDoesNothing() {
  sf::platform::detail::RuntimeMenuPointerController controller;
  const sf::platform::detail::MenuPointerSample pointer{
      .x = 300.0F,
      .y = 200.0F,
      .inside = true,
      .moved = true,
      .primary_pressed = true,
  };
  const auto action = controller.update(menu(0U), pointer);
  require(action.active_low_buttons == 0xffffU && !action.selection_valid,
          "Click outside authored menu zones generated input");
}

} // namespace

int main() {
  try {
    testHoverNavigatesAndClickActivates();
    testBackAndOverlappingAuthoredZones();
    testGridHoverDoesNotUseGuestDirections();
    testPointerOutsideOptionsDoesNothing();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "Runtime menu-pointer tests passed\n";
  return 0;
}
