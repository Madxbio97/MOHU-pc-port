#include "psycross_runtime_guards.hpp"

#include "sf/platform/host.hpp"

#include <PsyX/PsyX_public.h>
#include <SDL.h>

#include <algorithm>
#include <array>

namespace sf::platform::detail {

std::optional<std::size_t>
menuHitTest(const MenuPointerSample &pointer,
            std::span<const MenuHitRegion> regions) noexcept {
  if (!pointer.inside) {
    return std::nullopt;
  }
  for (const auto &region : regions) {
    if (region.width > 0 && region.height > 0 && pointer.x >= region.x &&
        pointer.y >= region.y && pointer.x < region.x + region.width &&
        pointer.y < region.y + region.height) {
      return region.selection;
    }
  }
  return std::nullopt;
}

namespace {

constexpr std::array<SDL_Point, 7U> menu_cursor_outline{{
    {2, 1},
    {2, 23},
    {8, 17},
    {13, 29},
    {18, 27},
    {13, 15},
    {22, 15},
}};

[[nodiscard]] bool cursorContains(float x, float y) noexcept {
  auto inside = false;
  auto previous = menu_cursor_outline.size() - 1U;
  for (std::size_t current{}; current < menu_cursor_outline.size(); ++current) {
    const auto &a = menu_cursor_outline[current];
    const auto &b = menu_cursor_outline[previous];
    const auto crosses = (a.y > y) != (b.y > y);
    if (crosses) {
      const auto edge_x = static_cast<float>(b.x - a.x) *
                              (y - static_cast<float>(a.y)) /
                              static_cast<float>(b.y - a.y) +
                          static_cast<float>(a.x);
      if (x < edge_x) {
        inside = !inside;
      }
    }
    previous = current;
  }
  return inside;
}

[[nodiscard]] SDL_Cursor *createMenuCursor() noexcept {
  constexpr auto width = 26;
  constexpr auto height = 32;
  auto *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
                                                 SDL_PIXELFORMAT_ARGB8888);
  if (surface == nullptr) {
    return nullptr;
  }
  static_cast<void>(SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND));
  static_cast<void>(SDL_FillRect(surface, nullptr,
                                 SDL_MapRGBA(surface->format, 0U, 0U, 0U, 0U)));
  if (SDL_LockSurface(surface) != 0) {
    SDL_FreeSurface(surface);
    return nullptr;
  }

  const auto shadow = SDL_MapRGBA(surface->format, 4U, 5U, 7U, 120U);
  const auto outline = SDL_MapRGBA(surface->format, 27U, 25U, 22U, 255U);
  const auto parchment = SDL_MapRGBA(surface->format, 218U, 205U, 159U, 255U);
  const auto accent = SDL_MapRGBA(surface->format, 118U, 35U, 31U, 255U);
  auto *pixels = static_cast<std::uint8_t *>(surface->pixels);
  const auto put = [&](int x, int y, std::uint32_t color) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
      return;
    }
    auto *row = reinterpret_cast<std::uint32_t *>(pixels + y * surface->pitch);
    row[x] = color;
  };

  for (auto y = 0; y < height; ++y) {
    for (auto x = 0; x < width; ++x) {
      if (cursorContains(static_cast<float>(x) - 1.5F,
                         static_cast<float>(y) - 1.5F)) {
        put(x, y, shadow);
      }
    }
  }
  for (auto y = 0; y < height; ++y) {
    for (auto x = 0; x < width; ++x) {
      if (!cursorContains(static_cast<float>(x) + 0.5F,
                          static_cast<float>(y) + 0.5F)) {
        continue;
      }
      auto edge = false;
      for (auto offset_y = -1; offset_y <= 1 && !edge; ++offset_y) {
        for (auto offset_x = -1; offset_x <= 1; ++offset_x) {
          edge |= !cursorContains(static_cast<float>(x + offset_x) + 0.5F,
                                  static_cast<float>(y + offset_y) + 0.5F);
        }
      }
      const auto fill =
          !edge && x <= 4 && y >= 6 && y <= 20 ? accent : parchment;
      put(x, y, edge ? outline : fill);
    }
  }
  SDL_UnlockSurface(surface);
  auto *cursor = SDL_CreateColorCursor(surface, 2, 1);
  SDL_FreeSurface(surface);
  return cursor;
}

} // namespace

MenuCursor::~MenuCursor() {
  set(false);
  if (cursor_ != nullptr) {
    SDL_FreeCursor(cursor_);
  }
}

void MenuCursor::set(bool enabled) noexcept {
  if (enabled_ == enabled) {
    return;
  }
  if (enabled) {
    static_cast<void>(SDL_SetRelativeMouseMode(SDL_FALSE));
    if (cursor_ == nullptr) {
      cursor_ = createMenuCursor();
    }
    SDL_SetCursor(cursor_ != nullptr ? cursor_ : SDL_GetDefaultCursor());
    SDL_GetRelativeMouseState(nullptr, nullptr);
    static_cast<void>(SDL_ShowCursor(SDL_ENABLE));
    const auto buttons = SDL_GetMouseState(&previous_x_, &previous_y_);
    primary_down_ = (buttons & SDL_BUTTON_LMASK) != 0U;
    secondary_down_ = (buttons & SDL_BUTTON_RMASK) != 0U;
    position_initialized_ = false;
    enabled_ = true;
    return;
  }
  SDL_SetCursor(SDL_GetDefaultCursor());
  static_cast<void>(SDL_ShowCursor(SDL_DISABLE));
  enabled_ = false;
  position_initialized_ = false;
  primary_down_ = false;
  secondary_down_ = false;
}

MenuPointerSample MenuCursor::sample(int logical_width,
                                     int logical_height) noexcept {
  MenuPointerSample result;
  if (!enabled_ || logical_width <= 0 || logical_height <= 0) {
    return result;
  }

  auto x = 0;
  auto y = 0;
  const auto buttons = SDL_GetMouseState(&x, &y);
  const auto primary_down = (buttons & SDL_BUTTON_LMASK) != 0U;
  const auto secondary_down = (buttons & SDL_BUTTON_RMASK) != 0U;
  result.primary_pressed = primary_down && !primary_down_;
  result.secondary_pressed = secondary_down && !secondary_down_;
  result.moved = !position_initialized_ || x != previous_x_ || y != previous_y_;
  previous_x_ = x;
  previous_y_ = y;
  position_initialized_ = true;
  primary_down_ = primary_down;
  secondary_down_ = secondary_down;

  auto window_width = 0;
  auto window_height = 0;
  PsyX_GetScreenSize(&window_width, &window_height);
  const auto viewport = PsyX_CalculateOutputViewport(
      std::max(window_width, 1), std::max(window_height, 1),
      std::max(g_cfg_renderWidth, 1), std::max(g_cfg_renderHeight, 1),
      g_cfg_aspectMode);
  result.inside = x >= viewport.x && y >= viewport.y &&
                  x < viewport.x + viewport.w && y < viewport.y + viewport.h;
  if (result.inside && viewport.w > 0 && viewport.h > 0) {
    result.x = static_cast<float>(x - viewport.x) * logical_width / viewport.w;
    result.y = static_cast<float>(y - viewport.y) * logical_height / viewport.h;
  }
  return result;
}

namespace {

[[nodiscard]] bool contains(const RuntimeMenuTarget &target, float x,
                            float y) noexcept {
  return x >= target.x && y >= target.y &&
         x < static_cast<float>(target.x) + target.width &&
         y < static_cast<float>(target.y) + target.height;
}

} // namespace

RuntimeMenuPointerAction RuntimeMenuPointerController::update(
    const RuntimeMenuState &menu, const MenuPointerSample &pointer) noexcept {
  constexpr auto neutral = std::uint16_t{0xffffU};
  constexpr auto triangle = std::uint16_t{0x1000U};
  constexpr auto cross = std::uint16_t{0x4000U};
  RuntimeMenuPointerAction result;

  if (!menu.active || menu.targets.empty()) {
    reset();
    return result;
  }
  const auto screen_changed =
      !screen_initialized_ || screen_id_ != menu.screen_id;
  if (screen_changed) {
    screen_id_ = menu.screen_id;
    screen_initialized_ = true;
    pending_activation_ = false;
  }
  if (pointer.secondary_pressed) {
    pending_activation_ = false;
    result.active_low_buttons = static_cast<std::uint16_t>(neutral & ~triangle);
    return result;
  }

  if (pending_activation_) {
    if (pending_screen_id_ != menu.screen_id) {
      pending_activation_ = false;
      return result;
    }
    result.selection_valid = true;
    result.screen_id = pending_screen_id_;
    result.selection = pending_selection_;
    if (menu.selected == pending_selection_) {
      pending_activation_ = false;
      result.active_low_buttons = static_cast<std::uint16_t>(neutral & ~cross);
    }
    return result;
  }

  if (!(pointer.moved || screen_changed || pointer.primary_pressed) ||
      !pointer.inside) {
    return result;
  }
  const RuntimeMenuTarget *target{};
  for (const auto &candidate : menu.targets) {
    if (!contains(candidate, pointer.x, pointer.y)) {
      continue;
    }
    if (candidate.selection == menu.selected) {
      target = &candidate;
      break;
    }
    if (target == nullptr) {
      target = &candidate;
    }
  }
  if (target == nullptr) {
    return result;
  }

  result.selection_valid = true;
  result.screen_id = menu.screen_id;
  result.selection = target->selection;
  if (pointer.primary_pressed) {
    pending_screen_id_ = menu.screen_id;
    pending_selection_ = target->selection;
    pending_activation_ = true;
  }
  return result;
}

void RuntimeMenuPointerController::reset() noexcept {
  screen_initialized_ = false;
  pending_activation_ = false;
}

RelativeMouseCapture::~RelativeMouseCapture() { set(false); }

void RelativeMouseCapture::set(bool enabled) noexcept {
  if (enabled_ == enabled) {
    return;
  }
  if (SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE) == 0) {
    enabled_ = enabled;
  }
  SDL_GetRelativeMouseState(nullptr, nullptr);
}

} // namespace sf::platform::detail
