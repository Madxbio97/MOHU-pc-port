#pragma once

#include "mohu/adaptive_world_frustum.hpp"
#include "mohu/gpu_command_stream.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/legacy_virtual_cd.hpp"
#include "sf/psx/bios_hle.hpp"
#include "sf/psx/machine.hpp"
#include "sf/psx/r3000_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace mohu {

enum class RuntimePcAction : std::uint8_t {
  none,
  return_sentinel,
  exception_vector,
  bios_call_vector,
  disc_search_return,
  directory_scan_entry,
  multiplayer_renderer_boundary,
  frustum_bsp_upper_x,
  frustum_bsp_lower_x,
  frustum_object_upper_x,
  frustum_level_triangle_outcode,
  frustum_slus_triangle_outcode,
};

enum class RuntimePcScope : std::uint8_t {
  none,
  any,
  singleplayer,
  multiplayer,
};

namespace detail {

struct RuntimePcActionEntry {
  std::uint32_t pc{};
  RuntimePcAction action{};
  RuntimePcScope scope{};
};

[[nodiscard]] constexpr std::size_t
runtimePcActionIndex(std::uint32_t pc) noexcept {
  const auto word_pc = pc >> 2U;
  return (word_pc ^ (word_pc >> 9U)) & 63U;
}

inline constexpr auto runtime_pc_actions = [] {
  std::array<RuntimePcActionEntry, 64U> result{};
  const auto put = [&result](std::uint32_t pc, RuntimePcAction action,
                             RuntimePcScope scope = RuntimePcScope::any) {
    result[runtimePcActionIndex(pc)] = {pc, action, scope};
  };
  put(0x80000080U, RuntimePcAction::exception_vector);
  put(sf::psx::R3000Runtime::return_sentinel,
      RuntimePcAction::return_sentinel);
  put(0x000000a0U, RuntimePcAction::bios_call_vector);
  put(0x000000b0U, RuntimePcAction::bios_call_vector);
  put(0x000000c0U, RuntimePcAction::bios_call_vector);
  put(0x80037b00U, RuntimePcAction::disc_search_return);
  put(0x80039064U, RuntimePcAction::directory_scan_entry);
  put(0x80097084U, RuntimePcAction::multiplayer_renderer_boundary,
      RuntimePcScope::multiplayer);
  put(0x80099a28U, RuntimePcAction::frustum_bsp_upper_x,
      RuntimePcScope::singleplayer);
  put(0x80099dacU, RuntimePcAction::frustum_bsp_lower_x,
      RuntimePcScope::singleplayer);
  put(0x8009a2c8U, RuntimePcAction::frustum_bsp_lower_x,
      RuntimePcScope::singleplayer);
  put(0x8009a8b0U, RuntimePcAction::frustum_level_triangle_outcode,
      RuntimePcScope::singleplayer);
  put(0x8009cd7cU, RuntimePcAction::frustum_bsp_lower_x,
      RuntimePcScope::singleplayer);
  put(0x8009ce1cU, RuntimePcAction::frustum_object_upper_x,
      RuntimePcScope::singleplayer);
  put(0x800941a8U, RuntimePcAction::frustum_bsp_upper_x,
      RuntimePcScope::multiplayer);
  put(0x800942f8U, RuntimePcAction::frustum_bsp_lower_x,
      RuntimePcScope::multiplayer);
  put(0x80094428U, RuntimePcAction::frustum_bsp_lower_x,
      RuntimePcScope::multiplayer);
  put(0x80094ad0U, RuntimePcAction::frustum_level_triangle_outcode,
      RuntimePcScope::multiplayer);
  put(0x80096cc8U, RuntimePcAction::frustum_bsp_lower_x,
      RuntimePcScope::multiplayer);
  put(0x80096d68U, RuntimePcAction::frustum_object_upper_x,
      RuntimePcScope::multiplayer);
  put(0x80010c00U, RuntimePcAction::frustum_slus_triangle_outcode);
  put(0x800115c4U, RuntimePcAction::frustum_slus_triangle_outcode);
  return result;
}();

} // namespace detail

[[nodiscard]] constexpr RuntimePcAction
runtimePcAction(std::uint32_t pc) noexcept {
  const auto &entry = detail::runtime_pc_actions[
      detail::runtimePcActionIndex(pc)];
  return entry.pc == pc ? entry.action : RuntimePcAction::none;
}

[[nodiscard]] constexpr RuntimePcScope
runtimePcScope(std::uint32_t pc) noexcept {
  const auto &entry = detail::runtime_pc_actions[
      detail::runtimePcActionIndex(pc)];
  return entry.pc == pc ? entry.scope : RuntimePcScope::none;
}

enum class RuntimeStatus {
  running,
  unsupported_bios,
  cpu_stopped,
};

struct RuntimeFrameResult {
  RuntimeStatus status{RuntimeStatus::running};
  sf::psx::R3000StopReason stop_reason{sf::psx::R3000StopReason::running};
  std::string detail;

  [[nodiscard]] bool running() const noexcept {
    return status == RuntimeStatus::running;
  }
};

struct RuntimeControllerInput {
  std::uint16_t active_low_buttons{0xffffU};
  std::array<std::uint8_t, 4U> analog{128U, 128U, 128U, 128U};
  bool connected{true};

  friend bool operator==(const RuntimeControllerInput &,
                         const RuntimeControllerInput &) = default;
};

using RuntimeControllerInputs = std::array<RuntimeControllerInput, 2U>;

struct RuntimeStats {
  std::uint64_t frames{};
  std::uint64_t instructions{};
  std::uint64_t cached_fast_fallbacks{};
  std::uint64_t bios_calls{};
  std::uint64_t interrupts{};
  std::uint64_t gpu_words{};
  std::uint64_t gpu_control_words{};
  std::uint64_t disc_search_attempts{};
  std::uint64_t disc_search_failures{};
  std::uint64_t disc_search_successes{};
  std::uint64_t disc_search_exhausted{};
  std::uint32_t disc_search_high_water{};
  std::int32_t last_disc_search_result{};
  std::uint64_t disc_directory_scans{};
  std::uint32_t last_directory_extent{};
  std::uint32_t last_directory_sectors{};
  std::uint32_t max_directory_sectors{};
  std::uint64_t exact_transform_captures{};
  std::uint64_t exact_transform_publications{};
  std::uint64_t exact_transform_rejections{};
  std::uint64_t exact_composition_entries{};
  std::uint64_t exact_composition_captures{};
  std::uint64_t exact_composition_sites{};
  std::uint64_t exact_composition_publications{};
  std::uint64_t idle_loop_fast_forwards{};
  std::uint64_t idle_ticks_fast_forwarded{};
};

struct RuntimeAtmosphereState {
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

class Runtime final {
public:
  explicit Runtime(sf::game::GameDisc disc);
  Runtime(sf::game::GameDisc disc, std::filesystem::path memory_card_path);
  Runtime(sf::game::GameDisc disc,
          std::filesystem::path memory_card_slot_1_path,
          std::filesystem::path memory_card_slot_2_path);

  [[nodiscard]] RuntimeFrameResult
  runFrame(const RuntimeControllerInputs &controllers);

  [[nodiscard]] RuntimeFrameResult
  runFrame(std::uint16_t active_low_buttons,
           std::array<std::uint8_t, 4U> analog = {128U, 128U, 128U, 128U});

  [[nodiscard]] bool selectFrontendMenuTarget(std::uint32_t screen_id,
                                              std::uint32_t selection) noexcept;

  [[nodiscard]] std::size_t
  takePcm(std::span<sf::psx::SpuPcmFrame> destination) noexcept;
  void setSpuDiagnosticsEnabled(bool enabled) {
    machine_.spu().setDiagnosticsEnabled(enabled);
  }
  [[nodiscard]] std::size_t takeSpuDiagnostics(
      std::span<sf::psx::SpuDiagnosticEvent> destination) noexcept;
  [[nodiscard]] std::uint64_t droppedSpuDiagnostics() const noexcept {
    return machine_.spu().droppedDiagnostics();
  }

  void configureAdaptiveWorldFrustum(std::uint32_t output_width,
                                     std::uint32_t output_height,
                                     bool adaptive) noexcept;

  [[nodiscard]] bool gameplayPresentationReady() const noexcept;
  [[nodiscard]] std::uint8_t activeCampaignLevel() const noexcept {
    if (!gameplayPresentationReady() || multiplayerOverlayLoaded())
      return 0U;
    return active_campaign_level_ >= 1U && active_campaign_level_ <= 24U
               ? active_campaign_level_
               : 1U;
  }
  [[nodiscard]] RuntimeAtmosphereState activeAtmosphere() const noexcept;

  [[nodiscard]] const RuntimeStats &stats() const noexcept { return stats_; }
  [[nodiscard]] const sf::psx::R3000Runtime &cpu() const noexcept {
    return cpu_;
  }
  [[nodiscard]] const sf::psx::PsxMachine &machine() const noexcept {
    return machine_;
  }
  [[nodiscard]] std::span<const std::uint32_t> gpuCommands() const noexcept {
    return gpu_.frameWords();
  }
  [[nodiscard]] std::span<const sf::psx::GteProjectedVertex>
  gpuProjections() const noexcept {
    return gpu_.frameProjections();
  }
  [[nodiscard]] std::span<const std::uint64_t>
  gpuProjectionIdentities() const noexcept {
    return gpu_.frameProjectionIdentities();
  }
  [[nodiscard]] std::span<const sf::psx::GpuDmaWordSource>
  gpuDmaSources() const noexcept {
    return gpu_.frameDmaSources();
  }
  [[nodiscard]] std::span<const sf::psx::GteProjectedVertex>
  gpuProjectionCatalog() const noexcept {
    return cpu_.gpuProjectionCatalog();
  }
  [[nodiscard]] std::span<const GpuDisplayPublication>
  gpuDisplayPublications() const noexcept {
    return gpu_.frameDisplayPublications();
  }
  void setGteProjectionCommandBackend(
      sf::psx::GteProjectionCommandBackend backend) noexcept {
    cpu_.setGteProjectionCommandBackend(backend);
  }
  void setGpuProjectionIdentityTracking(bool enabled) noexcept {
    gpu_.setProjectionIdentityTracking(enabled);
  }
  [[nodiscard]] bool gpuProjectionIdentityTracking() const noexcept {
    return gpu_.projectionIdentityTracking();
  }
  [[nodiscard]] std::uint64_t gpuCommandBufferEpoch() const noexcept {
    return gpu_.commandBufferEpoch();
  }
  [[nodiscard]] std::uint64_t gpuDisplayPublicationSequence() const noexcept {
    return gpu_.displayPublicationSequence();
  }
  [[nodiscard]] std::span<const std::uint32_t> firstGpuWords() const noexcept {
    return gpu_.firstWords();
  }
  [[nodiscard]] const GpuDisplayState &gpuDisplayState() const noexcept {
    return gpu_.displayState();
  }
  [[nodiscard]] const sf::psx::BiosHleState &biosState() const noexcept {
    return bios_.state();
  }

private:
  enum class AdaptiveWorldFrustumState : std::uint8_t {
    disabled,
    pending,
    active,
    rejected,
  };

  void applyAdaptiveWorldFrustumHook(AdaptiveWorldFrustumHook hook) noexcept;
  [[nodiscard]] bool gameplayOverlayLoaded() const noexcept;
  [[nodiscard]] bool multiplayerOverlayLoaded() const noexcept;
  [[nodiscard]] AdaptiveWorldFrustumState
  validateAdaptiveWorldFrustum() const noexcept;

  struct ExactTransformCapture {
    std::uint32_t caller_stack{};
    std::uint32_t destination{};
    std::array<double, 9U> rotation{};
    std::array<double, 3U> translation{};
    bool valid{};
  };
  struct ExactMatrixCompositionCapture {
    std::uint32_t caller_stack{};
    std::uint32_t output{};
    std::array<double, 9U> lhs_rotation{};
    std::array<double, 3U> lhs_translation{};
    std::array<double, 9U> rhs_rotation{};
    std::array<double, 3U> rhs_translation{};
    bool valid{};
  };

  void captureExactMatrixComposition() noexcept;
  void publishExactMatrixComposition() noexcept;

  void updateActiveLevel() noexcept;
  void updateGameplayPresentation() noexcept;
  void captureExactTransformInput() noexcept;
  void publishExactTransformOutput() noexcept;

  sf::game::GameDisc disc_;
  sf::psx::R3000Runtime cpu_;
  sf::psx::PsxMachine machine_;
  sf::psx::BiosHle bios_;
  GpuCommandStream gpu_;
  sf::game::LegacyVirtualCd media_;
  std::uint64_t instruction_debt_{};
  RuntimeStats stats_{};
  std::int32_t adaptive_world_x_margin_{};
  std::int32_t adaptive_multiplayer_world_x_margin_{};
  AdaptiveWorldFrustumState adaptive_world_frustum_state_{};
  bool gameplay_presentation_ready_{};
  struct LevelExtent {
    std::uint32_t first_lba{};
    std::uint32_t last_lba{};
    std::uint8_t campaign_level{};
  };
  std::vector<LevelExtent> level_extents_;
  std::uint8_t active_campaign_level_{};
  std::array<ExactTransformCapture, 4U> exact_transform_captures_{};
  ExactMatrixCompositionCapture exact_matrix_composition_{};
};

} // namespace mohu
