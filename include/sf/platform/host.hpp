#pragma once

#include "sf/game/controller_bindings.hpp"
#include "sf/game/retail_cheats.hpp"
#include "sf/platform/player_input.hpp"
#include "sf/psx/gpu_dma_source.hpp"
#include "sf/psx/gte_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sf::game {
class MissionPackage;
class TitleAssets;
class TitleMovies;
} // namespace sf::game

namespace sf::psx {
struct BiosHleState;
struct SpuDiagnosticEvent;
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

// Menus, movies and loading screens may update the currently displayed VRAM
// page without publishing a new GP1 display state. Gameplay remains atomic at
// its display publications, while authored 4:3 content must expose those
// incremental updates (notably loading indicators) at the guest-frame edge.
[[nodiscard]] constexpr bool
requiresIncrementalGpuFlush(PresentationContent content) noexcept {
  return content == PresentationContent::authored_4_3;
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

inline constexpr int disabled_controller_device = -1;
inline constexpr int maximum_controller_device_index = 1;
[[nodiscard]] constexpr bool isValidControllerDeviceIndex(int index) noexcept {
  return index >= disabled_controller_device &&
         index <= maximum_controller_device_index;
}
[[nodiscard]] constexpr bool
areControllerDeviceRoutesValid(const std::array<int, 2U> &routes) noexcept {
  return isValidControllerDeviceIndex(routes[0U]) &&
         isValidControllerDeviceIndex(routes[1U]) &&
         (routes[0U] == disabled_controller_device ||
          routes[1U] == disabled_controller_device || routes[0U] != routes[1U]);
}

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
  std::array<int, 2U> controller_device_indices{0, 1};
  ControllerButtonBindings controller_bindings;
  ControllerButtonBindings controller_bindings_player_2;
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
  bool connected{true};

  friend bool operator==(const RuntimePadInput &,
                         const RuntimePadInput &) = default;
};

inline constexpr std::size_t runtime_controller_count = 2U;
using RuntimePadInputs = std::array<RuntimePadInput, runtime_controller_count>;

using RuntimeAudioDrainCallback =
    std::function<std::size_t(std::span<psx::SpuPcmFrame>)>;

void logRuntimeSpuDiagnostics(std::span<const psx::SpuDiagnosticEvent> events,
                              std::uint64_t dropped_events) noexcept;
void logRuntimeExactTransformDiagnostics(
    std::uint64_t captures, std::uint64_t publications,
    std::uint64_t rejections, std::uint64_t composition_entries,
    std::uint64_t composition_captures, std::uint64_t composition_sites,
    std::uint64_t composition_publications,
    std::span<const psx::GteProjectedVertex> projections) noexcept;
void logRuntimeCpuAccelerationDiagnostics(std::uint64_t frames,
                                          std::uint64_t fast_forwards,
                                          std::uint64_t skipped_ticks) noexcept;
struct RuntimeGuestCardDiagnostics {
  std::uint32_t task{};
  std::uint32_t result{};
  std::uint32_t done{};
  std::uint32_t ports{};
  std::uint32_t channel{};
  std::uint32_t completion_callback{};
  std::uint32_t task_stack_depth{};
  std::uint32_t vblank_callback{};
  std::uint32_t update_ticks{};
  std::array<std::uint32_t, 4U> software_events{};
  std::array<std::uint32_t, 4U> hardware_events{};
  std::array<std::uint32_t, 4U> software_handles{};
  std::array<std::uint32_t, 4U> hardware_handles{};
  std::array<std::array<std::uint32_t, 4U>, 4U> stack_frames{};
  std::array<std::uint32_t, 4U> stack_callbacks{};
};

void logRuntimeGuestCpuDiagnostics(
    std::uint64_t frames, std::uint64_t average_microseconds,
    std::uint64_t maximum_microseconds, std::uint64_t instructions_per_frame,
    std::uint64_t fast_fallbacks, std::uint32_t guest_pc,
    std::uint32_t guest_ra, const psx::BiosHleState &bios_state,
    const RuntimeGuestCardDiagnostics &card,
    std::uint64_t machine_tick) noexcept;

[[nodiscard]] RuntimePadInput mergeMohUndergroundRuntimeInput(
    RuntimePadInput physical, const KeyboardMouseActionSnapshot &keyboard_mouse,
    const MohUndergroundRuntimeActionBindings &runtime_actions) noexcept;
[[nodiscard]] RuntimePadInput applyMohUndergroundControllerBindings(
    RuntimePadInput physical,
    const ControllerButtonBindings &bindings) noexcept;
[[nodiscard]] RuntimePadInput applyMohUndergroundRuntimeMouseLook(
    RuntimePadInput pad, std::int32_t mouse_delta_x, std::int32_t mouse_delta_y,
    bool mouse_look_active, std::uint32_t sensitivity_percent,
    const MohUndergroundRuntimeActionBindings &runtime_actions) noexcept;
[[nodiscard]] std::int32_t
runtimeMouseDeltaForGuestStep(std::int32_t total_delta, std::size_t step_index,
                              std::size_t step_count) noexcept;

struct RuntimeGpuDisplayPublication {
  std::size_t word_offset{};
  std::uint64_t sequence{};
  std::uint16_t display_x{};
  std::uint16_t display_y{};
  std::uint16_t display_width{256U};
  std::uint16_t display_height{240U};
  bool display_enabled{true};
  bool display_rgb24{};
  bool display_interlaced{};
};

struct RuntimeMenuTarget {
  std::uint32_t selection{};
  std::int16_t x{};
  std::int16_t y{};
  std::uint16_t width{};
  std::uint16_t height{};
};

struct RuntimeMenuState {
  bool active{};
  std::uint32_t screen_id{};
  std::uint32_t selected{};
  std::vector<RuntimeMenuTarget> targets;
};

struct RuntimeGpuAtmosphere {
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};
  std::int32_t dqa{};
  std::int32_t dqb{};
  std::uint16_t projection{};
  std::uint32_t terrain_depth_cue{};
  float skybox_yaw{};
  float skybox_pitch{};
  float skybox_vertical_fov{};
  bool valid{};
  bool skybox_view_valid{};
};

struct RuntimeGpuFrame {
  std::uint64_t sequence{};
  std::uint64_t display_publication_sequence{};
  std::vector<RuntimeGpuDisplayPublication> display_publications;
  std::vector<std::uint32_t> words;
  std::vector<psx::GteProjectedVertex> projections;
  std::vector<std::uint64_t> projection_identities;
  std::vector<psx::GteProjectedVertex> projection_catalog;
  std::uint16_t display_x{};
  std::uint16_t display_y{};
  std::uint16_t display_width{256U};
  std::uint16_t display_height{240U};
  bool display_enabled{true};
  bool display_rgb24{};
  bool display_interlaced{};
  PresentationContent content{PresentationContent::authored_4_3};
  std::uint8_t campaign_level{};
  RuntimeGpuAtmosphere atmosphere;
  RuntimeMenuState menu;
  std::uint64_t command_buffer_epoch{};
  std::vector<psx::GpuDmaWordSource> dma_sources;
};

struct RuntimeFrameStep {
  bool running{true};
  std::shared_ptr<const RuntimeGpuFrame> frame;
};

struct RuntimeMenuInteraction {
  bool selection_valid{};
  std::uint32_t screen_id{};
  std::uint32_t selection{};
};

using RuntimeFrameCallback = std::function<RuntimeFrameStep(
    const RuntimePadInputs &, const RuntimeMenuInteraction &)>;

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
    GraphicsSettings graphics = {},
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
