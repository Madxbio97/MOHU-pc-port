#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

struct SDL_Cursor;

namespace sf::platform {
struct RuntimeMenuState;
}

namespace sf::platform::detail {

struct MenuPointerSample {
  float x{};
  float y{};
  bool inside{};
  bool moved{};
  bool primary_pressed{};
  bool secondary_pressed{};
};

struct MenuHitRegion {
  int x{};
  int y{};
  int width{};
  int height{};
  std::size_t selection{};
};

struct RuntimeMenuPointerAction {
  std::uint16_t active_low_buttons{0xffffU};
  bool selection_valid{};
  std::uint32_t screen_id{};
  std::uint32_t selection{};
};

[[nodiscard]] std::optional<std::size_t>
menuHitTest(const MenuPointerSample &pointer,
            std::span<const MenuHitRegion> regions) noexcept;

class MenuCursor final {
public:
  ~MenuCursor();

  void set(bool enabled) noexcept;
  [[nodiscard]] MenuPointerSample sample(int logical_width,
                                         int logical_height) noexcept;

private:
  int previous_x_{};
  int previous_y_{};
  bool enabled_{};
  bool position_initialized_{};
  bool primary_down_{};
  bool secondary_down_{};
  SDL_Cursor *cursor_{};
};

class RuntimeMenuPointerController final {
public:
  [[nodiscard]] RuntimeMenuPointerAction
  update(const RuntimeMenuState &menu,
         const MenuPointerSample &pointer) noexcept;
  void reset() noexcept;

private:
  std::uint32_t screen_id_{};
  bool screen_initialized_{};
  std::uint32_t pending_screen_id_{};
  std::uint32_t pending_selection_{};
  bool pending_activation_{};
  std::uint32_t target_screen_id_{};
  std::uint32_t target_selection_{};
  bool target_pending_{};
  bool directional_release_pending_{};
};

class RelativeMouseCapture final {
public:
  ~RelativeMouseCapture();

  void set(bool enabled) noexcept;
  [[nodiscard]] bool enabled() const noexcept { return enabled_; }

private:
  bool enabled_{};
};

} // namespace sf::platform::detail
