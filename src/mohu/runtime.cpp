#include "mohu/runtime.hpp"

#include "mohu/adaptive_world_frustum.hpp"
#include "sf/core/error.hpp"
#include "sf/disc/raw_sector_source.hpp"
#include "sf/psx/memory_card_image.hpp"

#include <algorithm>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

namespace mohu {
namespace {

constexpr std::uint64_t guest_instructions_per_frame =
    sf::psx::PsxMachine::cpu_clock_hz / 60U;
constexpr std::uint32_t disc_search_attempt_register = 17U;

struct OpcodeWitness {
  std::uint32_t address;
  std::uint32_t instruction;
};

// SLUS-01270 LEVEL.BIN, loaded at 0x8003b600. These are the only world
// visibility comparisons adjusted by the host hook. Any different overlay is
// rejected before a register is touched.
constexpr std::array adaptive_world_frustum_witnesses{
    OpcodeWitness{0x80099a28U, 0xafa300ccU}, // sw v1,0xcc(sp)
    OpcodeWitness{0x80099dacU, 0x04400033U}, // bltz v0,0x80099e7c
    OpcodeWitness{0x8009a2c8U, 0x04400033U}, // bltz v0,0x8009a398
    OpcodeWitness{0x8009a8b0U, 0x04410003U}, // bgez v0,0x8009a8c0
    OpcodeWitness{0x8009cd7cU, 0x04400088U}, // bltz v0,0x8009cfa0
    OpcodeWitness{0x8009ce1cU, 0x0043102aU}, // slt v0,v0,v1
    // SLUS-01270 static-TSP outcode paths. Both read the retail horizontal
    // bound into t6 and perform a second three-vertex rejection after the
    // LEVEL BSP/object tests above.
    OpcodeWitness{0x80010c00U, 0x05000005U}, // bltz t0,0x80010c18
    OpcodeWitness{0x800115c4U, 0x05000005U}, // bltz t0,0x800115dc
};

std::string biosCallDetail(const sf::psx::R3000Runtime &cpu) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << "Unsupported BIOS vector 0x"
         << std::setw(8) << cpu.state().pc << " call 0x" << std::setw(2)
         << cpu.state().gpr[9] << " ra=0x" << std::setw(8)
         << cpu.state().gpr[31U] << " a0=0x" << std::setw(8)
         << cpu.state().gpr[4U] << " a1=0x" << std::setw(8)
         << cpu.state().gpr[5U] << " a2=0x" << std::setw(8)
         << cpu.state().gpr[6U] << " a3=0x" << std::setw(8)
         << cpu.state().gpr[7U];
  return stream.str();
}

std::string cpuStopDetail(const sf::psx::R3000RunResult &execution) {
  std::ostringstream stream;
  stream << "Guest CPU stopped at 0x" << std::hex << std::setfill('0')
         << std::setw(8) << execution.pc << ": "
         << sf::psx::toString(execution.reason) << " (opcode 0x" << std::setw(8)
         << execution.instruction << ')';
  return stream.str();
}

} // namespace

Runtime::Runtime(sf::game::GameDisc disc) : Runtime(std::move(disc), {}) {}

Runtime::Runtime(sf::game::GameDisc disc,
                 std::filesystem::path memory_card_path)
    : disc_(std::move(disc)), machine_(cpu_),
      bios_(cpu_, &machine_,
            memory_card_path.empty()
                ? sf::psx::MemoryCardCommitCallback{}
                : sf::psx::MemoryCardCommitCallback{
                      [path = memory_card_path](
                          const sf::psx::MemoryCardHleState &state) noexcept {
                        return static_cast<bool>(
                            sf::psx::MemoryCardImage::storeAtomic(path, state));
                      }}) {
  if (!disc_.game() || disc_.game()->serial != "SLUS-01270") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "Unsupported Medal of Honor: Underground disc"};
  }

  // Keep the exact GTE tuple without shadowing every guest ALU operation.
  // Generic MAC preservation is disabled because the exact transform twin
  // supplies the coherent projection used by the direct DMA sidecar.
  cpu_.setPgxpCpuTracking(false);
  cpu_.setPgxpExactTransformTracking(true);
  cpu_.setPgxpVertexIdentityTracking(false);
  cpu_.setPgxpPreserveProjectionPrecision(false);
  if (!cpu_.setPgxpTransformTracking(true)) {
    throw std::bad_alloc{};
  }
  // Carry a frame-local projection handle through the guest CPU and DMA. The
  // renderer accepts W only when every coordinate word names its exact GTE
  // tuple; it never reconstructs camera depth from quantized packet SXY.
  if (!cpu_.setGpuProjectionCatalogTracking(true)) {
    throw std::bad_alloc{};
  }
  gpu_.setProjectionTracking(false);
  gpu_.setProjectionIdentityTracking(true);

  if (!memory_card_path.empty()) {
    auto memory_card = std::make_unique<sf::psx::MemoryCardHleState>();
    const auto result =
        sf::psx::MemoryCardImage::load(memory_card_path, *memory_card);
    if (!result) {
      std::ostringstream detail;
      detail << "Cannot load Memory Card image '" << memory_card_path.string()
             << "': " << sf::psx::toString(result.error);
      if (result.system_error) {
        detail << " (" << result.system_error.message() << ')';
      }
      const auto code = result.error == sf::psx::MemoryCardImageError::io_error
                            ? sf::core::ErrorCode::io
                            : sf::core::ErrorCode::invalid_format;
      throw sf::core::Error{code, detail.str()};
    }
    if (!bios_.restoreMemoryCardState(*memory_card)) {
      throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                            "Decoded Memory Card state is invalid"};
    }
  }

  cpu_.loadExecutable(disc_.executable());
  machine_.attachGpuPort(&gpu_);
  auto source = std::make_unique<sf::disc::RawSectorSource>(
      sf::disc::RawSectorSource::open(disc_.cuePath()));
  const auto sector_count = source->sectorCount();
  if (!media_.attachRawSectorSource(std::move(source), 0U, sector_count)) {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "The game data track is too large for CD transport"};
  }
  machine_.setCdRomMedia(&media_);
}

bool Runtime::configureProjectionDiagnostics(
    bool transform_tracking, bool exact_transform, bool exact_capture,
    bool compact_catalog, bool identity_sidecar,
    bool preserve_projection_precision) noexcept {
  // Hotkey changes are provenance boundaries. Disable consumers first so a
  // carrier produced by the previous diagnostic mode cannot survive it.
  // Every diagnostic mode change is also a direct-shadow provenance boundary.
  static_cast<void>(cpu_.setGpuProjectionCatalogTracking(false));
  gpu_.setProjectionIdentityTracking(false);
  if (!transform_tracking) {
    static_cast<void>(cpu_.setGpuProjectionCatalogTracking(false));
    cpu_.setPgxpExactTransformCaptureEnabled(false);
    cpu_.setPgxpExactTransformTracking(false);
    static_cast<void>(cpu_.setPgxpTransformTracking(false));
    cpu_.setPgxpPreserveProjectionPrecision(false);
    return true;
  }
  if (!cpu_.setPgxpTransformTracking(true))
    return false;
  cpu_.setPgxpPreserveProjectionPrecision(preserve_projection_precision);
  cpu_.setPgxpExactTransformTracking(exact_transform);
  cpu_.setPgxpExactTransformCaptureEnabled(exact_capture);
  if (!cpu_.setGpuProjectionCatalogTracking(compact_catalog))
    return false;
  gpu_.setProjectionIdentityTracking(identity_sidecar);
  return true;
}

void Runtime::configureAdaptiveWorldFrustum(std::uint32_t output_width,
                                            std::uint32_t output_height,
                                            bool adaptive) noexcept {
  adaptive_world_x_margin_ =
      adaptiveWorldXMargin(output_width, output_height, adaptive);
  adaptive_world_frustum_state_ = adaptive_world_x_margin_ > 0
                                      ? AdaptiveWorldFrustumState::pending
                                      : AdaptiveWorldFrustumState::disabled;
}

Runtime::AdaptiveWorldFrustumState
Runtime::validateAdaptiveWorldFrustum() const noexcept {
  for (const auto &witness : adaptive_world_frustum_witnesses) {
    std::uint32_t instruction{};
    if (!cpu_.read32(witness.address, instruction) ||
        instruction != witness.instruction) {
      return AdaptiveWorldFrustumState::rejected;
    }
  }

  std::uint32_t horizontal_bound{};
  std::uint32_t vertical_bound{};
  if (!cpu_.read32(0x800a9340U, horizontal_bound) ||
      !cpu_.read32(0x800a9344U, vertical_bound) ||
      horizontal_bound != static_cast<std::uint32_t>(retail_world_x_max) ||
      vertical_bound != static_cast<std::uint32_t>(retail_world_y_max)) {
    // The code is present, but the LEVEL renderer has not initialized its
    // viewport yet. Retry at the next proven hook instead of rejecting it.
    return AdaptiveWorldFrustumState::pending;
  }
  return AdaptiveWorldFrustumState::active;
}

void Runtime::applyAdaptiveWorldFrustumHook(
    AdaptiveWorldFrustumHook hook) noexcept {
  if (adaptive_world_frustum_state_ == AdaptiveWorldFrustumState::disabled ||
      adaptive_world_frustum_state_ == AdaptiveWorldFrustumState::rejected) {
    return;
  }

  if (adaptive_world_frustum_state_ == AdaptiveWorldFrustumState::pending) {
    adaptive_world_frustum_state_ = validateAdaptiveWorldFrustum();
    if (adaptive_world_frustum_state_ != AdaptiveWorldFrustumState::active) {
      return;
    }
  }

  const auto margin = static_cast<std::uint32_t>(adaptive_world_x_margin_);
  const auto add_to_register = [this](std::uint8_t reg,
                                      std::uint32_t delta) noexcept {
    cpu_.setRegister(reg, cpu_.state().gpr[reg] + delta);
  };
  switch (hook) {
  case AdaptiveWorldFrustumHook::bsp_upper_x:
    // BSP/object tree upper X: W -> W + margin. Lower tests below shift the
    // compared coordinate, producing the symmetric [-margin, W + margin].
    add_to_register(3U, margin); // v1
    break;
  case AdaptiveWorldFrustumHook::bsp_lower_x:
    add_to_register(2U, margin); // v0, lower X
    break;
  case AdaptiveWorldFrustumHook::object_upper_x:
    add_to_register(2U, margin); // v0, local upper X bound
    break;
  case AdaptiveWorldFrustumHook::level_triangle_outcode:
    // Triangle outcodes reuse all three X values for lower and upper tests.
    // Shift X by one margin and the local upper bound by two margins.
    add_to_register(2U, margin);      // v0 = x0
    add_to_register(4U, margin);      // a0 = x1
    add_to_register(9U, margin);      // t1 = x2
    add_to_register(6U, margin * 2U); // a2 = local upper X bound
    break;
  case AdaptiveWorldFrustumHook::slus_triangle_outcode:
    // The shared SLUS TSP renderer repeats the outcode test after the LEVEL
    // visibility pass. Shift only its temporary comparison registers; packet
    // SXY, GTE state, HUD geometry and authored projection remain untouched.
    add_to_register(8U, margin);       // t0 = x0
    add_to_register(9U, margin);       // t1 = x1
    add_to_register(10U, margin);      // t2 = x2
    add_to_register(14U, margin * 2U); // t6 = local upper X bound
    break;
  case AdaptiveWorldFrustumHook::none:
  default:
    break;
  }
}

RuntimeFrameResult Runtime::runFrame(std::uint16_t active_low_buttons,
                                     std::array<std::uint8_t, 4U> analog) {
  gpu_.beginFrame();
  cpu_.beginGpuProjectionFrame();
  machine_.setControllerState(active_low_buttons, analog);
  machine_.pulseVBlank();

  const auto applied_debt =
      std::min(instruction_debt_, guest_instructions_per_frame - 1U);
  instruction_debt_ -= applied_debt;
  const auto frame_budget = guest_instructions_per_frame - applied_debt;
  std::uint64_t frame_instructions{};
  while (frame_instructions < frame_budget || bios_.exceptionActive()) {
    const auto pc = cpu_.state().pc;
    const auto pc_action = runtimePcAction(pc);
    if (pc_action != RuntimePcAction::none &&
        static_cast<std::uint8_t>(pc_action) <=
            static_cast<std::uint8_t>(RuntimePcAction::bios_call_vector)) {
      switch (pc_action) {
      case RuntimePcAction::return_sentinel:
        if (bios_.atEventCallbackReturn()) {
          if (!bios_.completeEventCallback()) {
            return {RuntimeStatus::cpu_stopped,
                    sf::psx::R3000StopReason::unsupported_instruction,
                    "Cannot restore guest CPU after BIOS event callback"};
          }
          continue;
        }
        if (bios_.atExceptionReturn()) {
          if (!bios_.completeException()) {
            return {RuntimeStatus::cpu_stopped,
                    sf::psx::R3000StopReason::unsupported_instruction,
                    "Cannot restore guest CPU after BIOS interrupt"};
          }
          continue;
        }
        break;
      case RuntimePcAction::exception_vector:
        if (!bios_.dispatchException()) {
          return {RuntimeStatus::cpu_stopped,
                  sf::psx::R3000StopReason::unsupported_instruction,
                  "Cannot dispatch guest BIOS interrupt"};
        }
        ++stats_.interrupts;
        continue;
      case RuntimePcAction::bios_call_vector:
        if (!bios_.handleCall()) {
          return {RuntimeStatus::unsupported_bios,
                  sf::psx::R3000StopReason::unsupported_instruction,
                  biosCallDetail(cpu_)};
        }
        ++stats_.bios_calls;
        continue;
      default:
        break;
      }
    }
    if (bios_.asyncServicePending()) {
      const auto async_service = bios_.serviceAsync();
      if (async_service == sf::psx::BiosAsyncServiceResult::failed) {
        return {RuntimeStatus::cpu_stopped,
                sf::psx::R3000StopReason::unsupported_instruction,
                "Cannot complete an asynchronous BIOS operation"};
      }
      if (async_service == sf::psx::BiosAsyncServiceResult::progressed) {
        continue;
      }
    }
    switch (pc_action) {
    case RuntimePcAction::disc_search_return: {
      const auto result = static_cast<std::int32_t>(cpu_.state().gpr[2U]);
      const auto attempt = cpu_.state().gpr[disc_search_attempt_register];
      ++stats_.disc_search_attempts;
      stats_.disc_search_high_water =
          std::max(stats_.disc_search_high_water, attempt);
      stats_.last_disc_search_result = result;
      if (result == 0 || result == -1) {
        ++stats_.disc_search_failures;
        if (attempt >= 32U) {
          ++stats_.disc_search_exhausted;
        }
      } else {
        ++stats_.disc_search_successes;
      }
      break;
    }
    case RuntimePcAction::directory_scan_entry: {
      std::uint32_t extent{};
      std::uint32_t sectors{};
      if (cpu_.read32(cpu_.state().gpr[4U], extent) &&
          cpu_.read32(cpu_.state().gpr[5U], sectors)) {
        ++stats_.disc_directory_scans;
        stats_.last_directory_extent = extent;
        stats_.last_directory_sectors = sectors;
        stats_.max_directory_sectors =
            std::max(stats_.max_directory_sectors, sectors);
      }
      break;
    }
    case RuntimePcAction::frustum_bsp_upper_x:
      applyAdaptiveWorldFrustumHook(AdaptiveWorldFrustumHook::bsp_upper_x);
      break;
    case RuntimePcAction::frustum_bsp_lower_x:
      applyAdaptiveWorldFrustumHook(AdaptiveWorldFrustumHook::bsp_lower_x);
      break;
    case RuntimePcAction::frustum_object_upper_x:
      applyAdaptiveWorldFrustumHook(AdaptiveWorldFrustumHook::object_upper_x);
      break;
    case RuntimePcAction::frustum_level_triangle_outcode:
      applyAdaptiveWorldFrustumHook(
          AdaptiveWorldFrustumHook::level_triangle_outcode);
      break;
    case RuntimePcAction::frustum_slus_triangle_outcode:
      applyAdaptiveWorldFrustumHook(
          AdaptiveWorldFrustumHook::slus_triangle_outcode);
      break;
    default:
      break;
    }
    const auto execution = machine_.step();
    frame_instructions += execution.instructions;
    if (execution.reason == sf::psx::R3000StopReason::running) {
      continue;
    }
    if (execution.reason == sf::psx::R3000StopReason::syscall &&
        bios_.handleKernelSyscall()) {
      continue;
    }
    return {RuntimeStatus::cpu_stopped, execution.reason,
            cpuStopDetail(execution)};
  }

  machine_.synchronizeDevices();
  // GPU DMA may have been armed by the final guest instruction and still be
  // scheduled after the last accounted CPU tick. Consume it while this
  // frame's projection catalog and compact handles still name its packet
  // vertices; the next runFrame() resets both in O(1).
  if (!machine_.completePendingDmaTransfer(sf::psx::DmaChannel::gpu)) {
    return {RuntimeStatus::cpu_stopped, sf::psx::R3000StopReason::memory_fault,
            "Cannot finalize pending GPU DMA before frame reset"};
  }

  ++stats_.frames;
  stats_.instructions += frame_instructions;
  if (frame_instructions > frame_budget) {
    instruction_debt_ += frame_instructions - frame_budget;
  }
  stats_.gpu_words = gpu_.totalGp0Words();
  stats_.gpu_control_words = gpu_.totalGp1Words();
  return {};
}

std::size_t
Runtime::takePcm(std::span<sf::psx::SpuPcmFrame> destination) noexcept {
  return machine_.spu().takePcm(destination);
}

} // namespace mohu
