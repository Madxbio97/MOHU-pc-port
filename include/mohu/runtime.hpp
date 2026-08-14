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

namespace mohu {

enum class RuntimePcAction : std::uint8_t {
  none,
  return_sentinel,
  exception_vector,
  bios_call_vector,
  disc_search_return,
  directory_scan_entry,
  frustum_bsp_upper_x,
  frustum_bsp_lower_x,
  frustum_object_upper_x,
  frustum_level_triangle_outcode,
  frustum_slus_triangle_outcode,
};

[[nodiscard]] constexpr RuntimePcAction
runtimePcAction(std::uint32_t pc) noexcept {
  struct Entry {
    std::uint32_t pc{};
    RuntimePcAction action{};
  };
  constexpr auto entries = [] {
    std::array<Entry, 32U> result{};
    result[1U] = {0x80000080U, RuntimePcAction::exception_vector};
    result[3U] = {sf::psx::R3000Runtime::return_sentinel,
                  RuntimePcAction::return_sentinel};
    result[5U] = {0x8009cd7cU, RuntimePcAction::frustum_bsp_lower_x};
    result[9U] = {0x000000a0U, RuntimePcAction::bios_call_vector};
    result[13U] = {0x000000b0U, RuntimePcAction::bios_call_vector};
    result[16U] = {0x80099dacU, RuntimePcAction::frustum_bsp_lower_x};
    result[17U] = {0x000000c0U, RuntimePcAction::bios_call_vector};
    result[22U] = {0x80037b00U, RuntimePcAction::disc_search_return};
    result[23U] = {0x8009a2c8U, RuntimePcAction::frustum_bsp_lower_x};
    result[24U] = {0x80010c00U, RuntimePcAction::frustum_slus_triangle_outcode};
    result[25U] = {0x80039064U, RuntimePcAction::directory_scan_entry};
    result[26U] = {0x800115c4U, RuntimePcAction::frustum_slus_triangle_outcode};
    result[27U] = {0x8009ce1cU, RuntimePcAction::frustum_object_upper_x};
    result[29U] = {0x8009a8b0U,
                   RuntimePcAction::frustum_level_triangle_outcode};
    result[30U] = {0x80099a28U, RuntimePcAction::frustum_bsp_upper_x};
    return result;
  }();
  const auto &entry = entries[((pc >> 2U) ^ (pc >> 7U)) & 31U];
  return entry.pc == pc ? entry.action : RuntimePcAction::none;
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

struct RuntimeStats {
  std::uint64_t frames{};
  std::uint64_t instructions{};
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
};

class Runtime final {
public:
  explicit Runtime(sf::game::GameDisc disc);
  Runtime(sf::game::GameDisc disc, std::filesystem::path memory_card_path);

  [[nodiscard]] RuntimeFrameResult
  runFrame(std::uint16_t active_low_buttons,
           std::array<std::uint8_t, 4U> analog = {128U, 128U, 128U, 128U});

  [[nodiscard]] std::size_t
  takePcm(std::span<sf::psx::SpuPcmFrame> destination) noexcept;

  void configureAdaptiveWorldFrustum(std::uint32_t output_width,
                                     std::uint32_t output_height,
                                     bool adaptive) noexcept;

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
  [[nodiscard]] AdaptiveWorldFrustumState
  validateAdaptiveWorldFrustum() const noexcept;

  sf::game::GameDisc disc_;
  sf::psx::R3000Runtime cpu_;
  sf::psx::PsxMachine machine_;
  sf::psx::BiosHle bios_;
  GpuCommandStream gpu_;
  sf::game::LegacyVirtualCd media_;
  std::uint64_t instruction_debt_{};
  RuntimeStats stats_{};
  std::int32_t adaptive_world_x_margin_{};
  AdaptiveWorldFrustumState adaptive_world_frustum_state_{};
};

} // namespace mohu
