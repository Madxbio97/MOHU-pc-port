#pragma once

#include "sf/game/controller_bindings.hpp"
#include "sf/game/retail_cheats.hpp"
#include "sf/platform/player_input.hpp"
#include "sf/psx/gte_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace sf::game {
class MissionPackage;
class TitleAssets;
class TitleMovies;
} // namespace sf::game

namespace sf::psx {
struct SpuPcmFrame;
} // namespace sf::psx

namespace sf::platform {

enum class AspectRatioMode {
  original_4_3,
  adaptive,
};

enum class PresentationContent {
  gameplay,
  authored_4_3,
};

[[nodiscard]] constexpr AspectRatioMode
presentationAspectRatio(AspectRatioMode configured,
                        PresentationContent content) noexcept {
  return content == PresentationContent::authored_4_3
             ? AspectRatioMode::original_4_3
             : configured;
}

enum class TextureFilteringMode {
  nearest,
  bilinear,
  trilinear,
  anisotropic,
};

enum class AntialiasingMode {
  disabled,
  smaa,
  fxaa,
};

enum class ControllerProtocol {
  automatic,
  xinput,
  direct_input,
  raw_input,
};

using ControllerButtonBindings = game::ControllerButtonBindings;
inline constexpr auto controller_action_binding_count =
    game::controller_action_count;
using ControllerSettingsCommitCallback =
    std::function<bool(const ControllerButtonBindings &, bool vibration)>;

struct GraphicsSettings {
  int width{1280};
  int height{720};
  int msaa_samples{};
  bool bilinear_filtering{true};
  bool trilinear_filtering{true};
  bool anisotropic_filtering{true};
  bool smaa{true};
  bool fxaa{};
  bool volumetric_fog{};
  bool volumetric_effects{};
  AspectRatioMode aspect_ratio{AspectRatioMode::adaptive};
  bool vsync{true};
  std::uint32_t frame_limit{60U};
  bool fullscreen{};
  ControllerProtocol controller_protocol{ControllerProtocol::automatic};
  ControllerButtonBindings controller_bindings;
  bool controller_vibration{true};
  std::uint32_t mouse_sensitivity_percent{100U};
};

inline constexpr std::array<std::uint32_t, 5U>
    standard_presentation_frame_limits{0U, 30U, 60U, 120U, 240U};

[[nodiscard]] constexpr bool
isStandardPresentationFrameLimit(std::uint32_t frame_limit) noexcept {
  for (const auto standard_limit : standard_presentation_frame_limits) {
    if (frame_limit == standard_limit) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] constexpr TextureFilteringMode
textureFilteringMode(const GraphicsSettings &settings) noexcept {
  if (settings.anisotropic_filtering) {
    return TextureFilteringMode::anisotropic;
  }
  if (settings.trilinear_filtering) {
    return TextureFilteringMode::trilinear;
  }
  return settings.bilinear_filtering ? TextureFilteringMode::bilinear
                                     : TextureFilteringMode::nearest;
}

constexpr void setTextureFilteringMode(GraphicsSettings &settings,
                                       TextureFilteringMode mode) noexcept {
  settings.bilinear_filtering = mode != TextureFilteringMode::nearest;
  settings.trilinear_filtering = mode == TextureFilteringMode::trilinear ||
                                 mode == TextureFilteringMode::anisotropic;
  settings.anisotropic_filtering = mode == TextureFilteringMode::anisotropic;
}

[[nodiscard]] constexpr AntialiasingMode
antialiasingMode(const GraphicsSettings &settings) noexcept {
  if (settings.smaa) {
    return AntialiasingMode::smaa;
  }
  return settings.fxaa ? AntialiasingMode::fxaa : AntialiasingMode::disabled;
}

constexpr void setAntialiasingMode(GraphicsSettings &settings,
                                   AntialiasingMode mode) noexcept {
  settings.smaa = mode == AntialiasingMode::smaa;
  settings.fxaa = mode == AntialiasingMode::fxaa;
  // The retail guest renders into a single-sample offscreen target. Keep the
  // public launcher modes honest and mutually exclusive.
  settings.msaa_samples = 0;
}

enum class MohUndergroundMouseLookMode : std::uint8_t {
  right_stick = 0U,
  directional_buttons = 1U,
};

// Explicit host-action to retail-pad table. The keyboard/mouse section owns
// physical inputs; this table owns the guest action layout selected in MOHU.
struct MohUndergroundRuntimeActionBindings {
  std::array<std::uint16_t, keyboard_mouse_action_count> buttons{};
  MohUndergroundMouseLookMode mouse_look_mode{
      MohUndergroundMouseLookMode::directional_buttons};

  [[nodiscard]] std::uint16_t
  operator[](KeyboardMouseAction action) const noexcept {
    return buttons[static_cast<std::size_t>(action)];
  }
  std::uint16_t &operator[](KeyboardMouseAction action) noexcept {
    return buttons[static_cast<std::size_t>(action)];
  }

  friend bool operator==(const MohUndergroundRuntimeActionBindings &,
                         const MohUndergroundRuntimeActionBindings &) = default;
};

[[nodiscard]] MohUndergroundRuntimeActionBindings
defaultMohUndergroundRuntimeActionBindings() noexcept;

// Raw guest pad sample. Analog byte order matches the DualShock serial
// protocol and PsyCross snapshots: right X/Y, then left X/Y.
struct RuntimePadInput {
  std::uint16_t active_low_buttons{0xffffU};
  std::array<std::uint8_t, 4U> analog{128U, 128U, 128U, 128U};

  friend bool operator==(const RuntimePadInput &,
                         const RuntimePadInput &) = default;
};

using RuntimeFrameCallback = std::function<bool(const RuntimePadInput &)>;
using RuntimeAudioDrainCallback =
    std::function<std::size_t(std::span<psx::SpuPcmFrame>)>;

[[nodiscard]] RuntimePadInput mergeMohUndergroundRuntimeInput(
    RuntimePadInput physical, const KeyboardMouseActionSnapshot &keyboard_mouse,
    const MohUndergroundRuntimeActionBindings &runtime_actions) noexcept;
[[nodiscard]] RuntimePadInput applyMohUndergroundRuntimeMouseLook(
    RuntimePadInput pad, std::int32_t mouse_delta_x, std::int32_t mouse_delta_y,
    bool mouse_look_active, std::uint32_t sensitivity_percent,
    const MohUndergroundRuntimeActionBindings &runtime_actions,
    double movement_camera_yaw = 0.0) noexcept;
[[nodiscard]] std::int32_t
runtimeMouseDeltaForGuestStep(std::int32_t total_delta, std::size_t step_index,
                              std::size_t step_count) noexcept;

struct RuntimeGpuFrame {
  std::span<const std::uint32_t> words;
  std::span<const psx::GteProjectedVertex> projections;
  std::span<const std::uint64_t> projection_identities;
  std::span<const psx::GteProjectedVertex> projection_catalog;
  std::uint16_t display_x{};
  std::uint16_t display_y{};
  std::uint16_t display_width{256U};
  std::uint16_t display_height{240U};
  bool display_enabled{true};
  bool display_rgb24{};
  bool display_interlaced{};
  std::uint64_t command_buffer_epoch{};
};

using RuntimeGpuFrameCallback = std::function<RuntimeGpuFrame()>;



class Host {
public:
  virtual ~Host() = default;
  Host(const Host &) = delete;
  Host &operator=(const Host &) = delete;

  virtual void run() = 0;

protected:
  Host() = default;
};

[[nodiscard]] std::unique_ptr<Host>
createPsyCrossHost(std::string title, GraphicsSettings graphics = {});

[[nodiscard]] std::unique_ptr<Host> createPsyCrossRuntimeHost(
    std::string title, RuntimeFrameCallback frame,
    RuntimeGpuFrameCallback gpu_frame, GraphicsSettings graphics = {},
    KeyboardMouseBindings input = defaultKeyboardMouseBindings(),
    MohUndergroundRuntimeActionBindings runtime_actions =
        defaultMohUndergroundRuntimeActionBindings(),
    RuntimeAudioDrainCallback audio = {});

[[nodiscard]] std::unique_ptr<Host> createPsyCrossTitleHost(
    std::string title, game::TitleAssets assets, game::TitleMovies movies,
    game::MissionPackage initial_mission, std::filesystem::path cue_path,
    std::string supported_game_serial, GraphicsSettings graphics = {},
    KeyboardMouseBindings input = defaultKeyboardMouseBindings(),
    game::RetailCheatState cheats = {},
    ControllerSettingsCommitCallback controller_settings_commit = {});

[[nodiscard]] std::unique_ptr<Host> createPsyCrossSceneHost(
    std::string title, game::MissionPackage mission,
    std::filesystem::path cue_path, GraphicsSettings graphics = {},
    KeyboardMouseBindings input = defaultKeyboardMouseBindings(),
    game::RetailCheatState cheats = {},
    ControllerSettingsCommitCallback controller_settings_commit = {});

} // namespace sf::platform
