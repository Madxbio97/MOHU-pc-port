#include "mohu/runtime.hpp"

#include "mohu/adaptive_world_frustum.hpp"
#include "mohu/campaign_level.hpp"
#include "mohu/frontend_menu.hpp"
#include "sf/core/error.hpp"
#include "sf/core/sha256.hpp"
#include "sf/disc/raw_sector_source.hpp"
#include "sf/game/localization.hpp"
#include "sf/psx/gp0_command.hpp"
#include "sf/psx/memory_card_image.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <memory>
#include <numbers>
#include <sstream>
#include <string_view>
#include <utility>

namespace mohu {
namespace {

constexpr sf::psx::CpuClockScale guest_cpu_clock_scale{1U, 1U};
constexpr std::uint64_t guest_instructions_per_frame =
    sf::psx::PsxMachine::cpu_clock_hz * guest_cpu_clock_scale.numerator /
    guest_cpu_clock_scale.denominator / 60U;
static_assert(guest_instructions_per_frame ==
              sf::psx::PsxMachine::cpu_clock_hz / 60U);
constexpr std::uint32_t disc_search_attempt_register = 17U;

constexpr std::string_view moption_disc_path = "DATA/SCR1/MOPTION.RSC";
constexpr std::string_view moption_localized_path =
    "mohu/DATA/SCR1/MOPTION.RSC";
constexpr std::string_view moption_source_sha256 =
    "98573dded0b56986bf7f04581f6f7e2a6310d7146b44598f72232c2708b232ef";
constexpr std::string_view moption_localized_sha256 =
    "71f3af191163d705a7532a1129e8428f12b4c28974d464f45050a70e49b7c4aa";

struct OpcodeWitness {
  std::uint32_t address;
  std::uint32_t instruction;
};

// SLUS-01270 gameplay overlays are loaded at 0x8003b600. SHELL.BIN replaces
// the first word with 7, so these exact markers reject stale gameplay code
// left above the smaller frontend overlay.
constexpr OpcodeWitness singleplayer_overlay_marker{0x8003b600U,
                                                     0x00000005U};
constexpr OpcodeWitness multiplayer_overlay_marker{0x8003b600U,
                                                    0x00000006U};
constexpr OpcodeWitness frontend_overlay_marker{0x8003b600U, 0x00000007U};
constexpr std::array singleplayer_overlay_witnesses{
    OpcodeWitness{0x80099a28U, 0xafa300ccU}, // sw v1,0xcc(sp)
    OpcodeWitness{0x80099dacU, 0x04400033U}, // bltz v0,0x80099e7c
    OpcodeWitness{0x8009a2c8U, 0x04400033U}, // bltz v0,0x8009a398
    OpcodeWitness{0x8009a8b0U, 0x04410003U}, // bgez v0,0x8009a8c0
    OpcodeWitness{0x8009cd7cU, 0x04400088U}, // bltz v0,0x8009cfa0
    OpcodeWitness{0x8009ce1cU, 0x0043102aU}, // slt v0,v0,v1
};

// LEVEL2P.BIN has its own verified world-renderer layout. These opcodes bind
// marker 6 to the split-screen renderer instead of treating any resident
// multiplayer overlay or menu as a gameplay frame.
constexpr std::array multiplayer_overlay_witnesses{
    OpcodeWitness{0x800941a8U, 0xafa300ccU}, // sw v1,0xcc(sp)
    OpcodeWitness{0x80094390U, 0x0043102aU}, // slt v0,v0,v1
    OpcodeWitness{0x80094394U, 0x1440008cU}, // bnez v0,0x800945c8
    OpcodeWitness{0x80094428U, 0x04400067U}, // bltz v0,0x800945c8
    OpcodeWitness{0x80094ad0U, 0x04410003U}, // bgez v0,0x80094ae0
    OpcodeWitness{0x80096cc8U, 0x04400088U}, // bltz v0,0x80096eec
    OpcodeWitness{0x80096d68U, 0x0043102aU}, // slt v0,v0,v1
    OpcodeWitness{0x800945c8U, 0x1080000eU}, // beqz a0,0x80094604
    OpcodeWitness{0x80096d6cU, 0x1440005fU}, // bnez v0,0x80096eec
    OpcodeWitness{0x80097084U, 0x0c01a0e1U}, // jal 0x80068384
};

// SLUS-01270 static-TSP outcode paths. Both read the retail horizontal bound
// into t6 and perform a second three-vertex rejection after the LEVEL
// BSP/object tests above.
constexpr std::array adaptive_static_frustum_witnesses{
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

Runtime::Runtime(sf::game::GameDisc disc) : Runtime(std::move(disc), {}, {}) {}

Runtime::Runtime(sf::game::GameDisc disc,
                 std::filesystem::path memory_card_path)
    : Runtime(std::move(disc), std::move(memory_card_path), {}) {}

RuntimeAtmosphereState Runtime::activeAtmosphere() const noexcept {
  constexpr std::uint32_t camera_controller_pointer = 0x80115d84U;
  constexpr std::uint32_t terrain_depth_cue_address = 0x80116458U;
  constexpr std::uint32_t camera_target_x_offset = 0x444U;
  constexpr std::uint32_t camera_target_y_offset = 0x448U;
  constexpr std::uint32_t camera_target_z_offset = 0x44cU;
  constexpr std::uint32_t matrix_translation_x_offset = 0x14U;
  constexpr std::uint32_t matrix_translation_y_offset = 0x18U;
  constexpr std::uint32_t matrix_translation_z_offset = 0x1cU;
  constexpr std::uint32_t renderer_projection_flags_offset = 0x04U;
  constexpr std::uint32_t renderer_fog_dqa_offset = 0xe4U;
  constexpr std::uint32_t renderer_fog_dqb_offset = 0xe8U;
  constexpr std::uint32_t renderer_fog_rgb_offset = 0xecU;

  RuntimeAtmosphereState result;
  std::uint32_t controller{};
  std::uint32_t renderer{};
  std::uint32_t projection_flags{};
  std::uint32_t raw_dqa{};
  std::uint32_t raw_dqb{};
  std::uint32_t raw_rgb{};
  if (!gameplayOverlayLoaded() || multiplayerOverlayLoaded() ||
      !cpu_.read32(camera_controller_pointer, controller) || controller == 0U ||
      !cpu_.read32(controller, renderer) || renderer == 0U ||
      !cpu_.read32(renderer + renderer_projection_flags_offset,
                   projection_flags) ||
      !cpu_.read32(renderer + renderer_fog_dqa_offset, raw_dqa) ||
      !cpu_.read32(renderer + renderer_fog_dqb_offset, raw_dqb) ||
      !cpu_.read32(renderer + renderer_fog_rgb_offset, raw_rgb) ||
      !cpu_.read32(terrain_depth_cue_address, result.terrain_depth_cue)) {
    return result;
  }

  const auto renderer_flags =
      static_cast<std::uint16_t>(projection_flags >> 16U);
  const auto terrain_shift = result.terrain_depth_cue >> 16U;
  const auto terrain_threshold = result.terrain_depth_cue & 0xffffU;
  result.projection = static_cast<std::uint16_t>(projection_flags);
  result.dqa = std::bit_cast<std::int32_t>(raw_dqa);
  result.dqb = std::bit_cast<std::int32_t>(raw_dqb);
  result.red = static_cast<std::uint8_t>(raw_rgb);
  result.green = static_cast<std::uint8_t>(raw_rgb >> 8U);
  result.blue = static_cast<std::uint8_t>(raw_rgb >> 16U);
  result.valid = result.projection != 0U && (renderer_flags & 1U) != 0U &&
                 terrain_shift <= 15U && terrain_threshold < 0x1000U &&
                 result.dqa >= std::numeric_limits<std::int16_t>::min() &&
                 result.dqa <= std::numeric_limits<std::int16_t>::max();

  std::uint32_t camera_matrix{};
  std::array<std::uint32_t, 6U> view_words{};
  if (cpu_.read32(renderer, camera_matrix) && camera_matrix != 0U &&
      cpu_.read32(camera_matrix + matrix_translation_x_offset,
                  view_words[0U]) &&
      cpu_.read32(camera_matrix + matrix_translation_y_offset,
                  view_words[1U]) &&
      cpu_.read32(camera_matrix + matrix_translation_z_offset,
                  view_words[2U]) &&
      cpu_.read32(controller + camera_target_x_offset, view_words[3U]) &&
      cpu_.read32(controller + camera_target_y_offset, view_words[4U]) &&
      cpu_.read32(controller + camera_target_z_offset, view_words[5U])) {
    const auto signed_word = [](std::uint32_t word) noexcept {
      return static_cast<double>(std::bit_cast<std::int32_t>(word));
    };
    const auto dx = signed_word(view_words[3U]) - signed_word(view_words[0U]);
    const auto dy = -signed_word(view_words[4U]) - signed_word(view_words[1U]);
    const auto dz = signed_word(view_words[5U]) - signed_word(view_words[2U]);
    const auto horizontal = std::hypot(dx, dz);
    const auto distance = std::hypot(horizontal, dy);
    if (std::isfinite(distance) && distance > 1.0 && result.projection != 0U) {
      result.skybox_yaw = static_cast<float>(std::atan2(dx, dz));
      result.skybox_pitch = static_cast<float>(std::atan2(dy, horizontal));
      result.skybox_vertical_fov = static_cast<float>(
          2.0 * std::atan(120.0 / static_cast<double>(result.projection)));
      result.skybox_view_valid = std::isfinite(result.skybox_yaw) &&
                                 std::isfinite(result.skybox_pitch) &&
                                 std::isfinite(result.skybox_vertical_fov);
    }
  }
  return result;
}

Runtime::Runtime(sf::game::GameDisc disc,
                 std::filesystem::path memory_card_slot_1_path,
                 std::filesystem::path memory_card_slot_2_path)
    : disc_(std::move(disc)), machine_(cpu_, guest_cpu_clock_scale),
      bios_(
          cpu_, &machine_,
          memory_card_slot_1_path.empty()
              ? sf::psx::MemoryCardCommitCallback{}
              : sf::psx::MemoryCardCommitCallback{[path =
                                                       memory_card_slot_1_path](
                                                      const sf::psx::
                                                          MemoryCardHleState
                                                              &state) noexcept {
                  return static_cast<bool>(
                      sf::psx::MemoryCardImage::storeAtomic(path, state));
                }},
          memory_card_slot_2_path.empty()
              ? sf::psx::MemoryCardCommitCallback{}
              : sf::psx::MemoryCardCommitCallback{
                    [path = memory_card_slot_2_path](
                        const sf::psx::MemoryCardHleState &state) noexcept {
                      return static_cast<bool>(
                          sf::psx::MemoryCardImage::storeAtomic(path, state));
                    }}) {
  if (!disc_.game() || disc_.game()->serial != "SLUS-01270") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "Unsupported Medal of Honor: Underground disc"};
  }

  const auto collect_level_extents = [&](auto &&self,
                                         const std::string &path) -> void {
    for (const auto &entry : disc_.image().list(path)) {
      const auto child = path.empty() ? entry.name : path + '/' + entry.name;
      if (entry.is_directory) {
        self(self, child);
        continue;
      }
      const auto campaign_level = campaignLevelForDiscPath(child);
      if (campaign_level == 0U) {
        continue;
      }
      const auto sectors = (static_cast<std::uint64_t>(entry.size) +
                            sf::psx::CdRomMedia::sector_size - 1U) /
                           sf::psx::CdRomMedia::sector_size;
      const auto last = static_cast<std::uint64_t>(entry.extent_lba) + sectors;
      if (last <= std::numeric_limits<std::uint32_t>::max()) {
        level_extents_.push_back({entry.extent_lba,
                                  static_cast<std::uint32_t>(last),
                                  campaign_level});
      }
    }
  };
  collect_level_extents(collect_level_extents, "DATA");

  // Keep the exact GTE tuple without shadowing every guest ALU operation.
  // Generic MAC preservation is disabled because the exact transform twin
  // supplies the coherent projection used by the direct DMA sidecar.
  cpu_.setPgxpCpuTracking(false);
  cpu_.setPgxpExactTransformTracking(true);
  cpu_.setPgxpVertexIdentityTracking(false);
  cpu_.setPgxpPreserveProjectionPrecision(true);
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

  const std::array memory_card_paths{memory_card_slot_1_path,
                                     memory_card_slot_2_path};
  for (std::uint8_t slot{}; slot < memory_card_paths.size(); ++slot) {
    const auto &memory_card_path = memory_card_paths[slot];
    if (memory_card_path.empty()) {
      continue;
    }
    auto memory_card = std::make_unique<sf::psx::MemoryCardHleState>();
    const auto result =
        sf::psx::MemoryCardImage::load(memory_card_path, *memory_card);
    if (!result) {
      std::ostringstream detail;
      detail << "Cannot load Memory Card slot "
             << static_cast<unsigned>(slot + 1U) << " image '"
             << memory_card_path.string()
             << "': " << sf::psx::toString(result.error);
      if (result.system_error) {
        detail << " (" << result.system_error.message() << ')';
      }
      const auto code = result.error == sf::psx::MemoryCardImageError::io_error
                            ? sf::core::ErrorCode::io
                            : sf::core::ErrorCode::invalid_format;
      throw sf::core::Error{code, detail.str()};
    }
    if (!bios_.restoreMemoryCardState(*memory_card, slot)) {
      throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                            "Decoded Memory Card slot state is invalid"};
    }
  }

  cpu_.loadExecutable(disc_.executable());
  for (const auto &entry : detail::runtime_pc_actions) {
    if (entry.action != RuntimePcAction::none) {
      cpu_.setExecutionBreakpoint(entry.pc, true);
    }
  }
  constexpr std::array exact_transform_boundaries{
      0x8009be98U,
      0x8009bf3cU,
      0x8001d578U,
      0x8001d67cU,
  };
  for (const auto pc : exact_transform_boundaries) {
    cpu_.setExecutionBreakpoint(pc, true);
  }
  machine_.attachGpuPort(&gpu_);
  auto source = std::make_unique<sf::disc::RawSectorSource>(
      sf::disc::RawSectorSource::open(disc_.cuePath()));
  if (sf::game::russianLanguageActive()) {
    const auto localized = sf::game::readLocalizedAsset(moption_localized_path);
    if (!localized) {
      throw sf::core::Error{sf::core::ErrorCode::io,
                            "Russian MOPTION localization asset is missing"};
    }
    const auto path = std::string{moption_disc_path};
    const auto entry = disc_.image().find(path);
    const auto original = disc_.image().readFile(path);
    if (entry.is_directory || entry.size != original.size() ||
        localized->size() != original.size() ||
        sf::core::toHex(sf::core::sha256(original)) != moption_source_sha256 ||
        sf::core::toHex(sf::core::sha256(*localized)) !=
            moption_localized_sha256) {
      throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                            "Russian MOPTION source provenance mismatch"};
    }
    if (!source->addUserDataOverlay(entry.extent_lba, *localized)) {
      throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                            "Russian MOPTION sector overlay is invalid"};
    }
  }
  const auto sector_count = source->sectorCount();
  if (!media_.attachRawSectorSource(std::move(source), 0U, sector_count)) {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "The game data track is too large for CD transport"};
  }
  machine_.setCdRomMedia(&media_);
  // Level data is accelerated; XA/movie traffic remains hardware-timed in
  // CdRomController.
  machine_.cdrom().setDataReadSpeedup(4U);
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

bool Runtime::multiplayerOverlayLoaded() const noexcept {
  const auto matches = [this](const OpcodeWitness &witness) noexcept {
    std::uint32_t value{};
    return cpu_.read32(witness.address, value) && value == witness.instruction;
  };
  return matches(multiplayer_overlay_marker) &&
         std::all_of(multiplayer_overlay_witnesses.begin(),
                     multiplayer_overlay_witnesses.end(), matches);
}

bool Runtime::gameplayOverlayLoaded() const noexcept {
  const auto matches = [this](const OpcodeWitness &witness) noexcept {
    std::uint32_t value{};
    return cpu_.read32(witness.address, value) && value == witness.instruction;
  };
  const auto matches_all = [&matches](const auto &witnesses) noexcept {
    return std::all_of(witnesses.begin(), witnesses.end(), matches);
  };
  return (matches(singleplayer_overlay_marker) &&
          matches_all(singleplayer_overlay_witnesses)) ||
         multiplayerOverlayLoaded();
}

bool Runtime::gameplayPresentationReady() const noexcept {
  return gameplay_presentation_ready_;
}

void Runtime::updateActiveLevel() noexcept {
  const auto &cdrom = machine_.cdrom();
  const auto count = static_cast<std::size_t>(cdrom.recentLbaCount());
  if (count == 0U) {
    return;
  }

  const auto recent_lbas = cdrom.recentLbas();
  const auto capacity = recent_lbas.size();
  const auto first =
      (static_cast<std::size_t>(cdrom.recentLbaCursor()) + capacity - count) %
      capacity;
  for (std::size_t offset{}; offset < count; ++offset) {
    const auto lba = recent_lbas[(first + offset) % capacity];
    const auto found = std::ranges::find_if(
        level_extents_, [lba](const LevelExtent &extent) noexcept {
          return lba >= extent.first_lba && lba < extent.last_lba;
        });
    if (found != level_extents_.end()) {
      active_campaign_level_ = found->campaign_level;
    }
  }
}

void Runtime::updateGameplayPresentation() noexcept {
  // LEVEL.BIN is loaded before the loading screen has finished. Do not switch
  // presentation merely because its overlay is resident: the exact renderer
  // hooks below prove the first real world frame. Keep that proof latched until
  // the frontend replaces LEVEL.BIN.
  if (!gameplayOverlayLoaded()) {
    gameplay_presentation_ready_ = false;
  }
}

Runtime::AdaptiveWorldFrustumState
Runtime::validateAdaptiveWorldFrustum() const noexcept {
  if (!gameplayOverlayLoaded()) {
    return AdaptiveWorldFrustumState::rejected;
  }
  for (const auto &witness : adaptive_static_frustum_witnesses) {
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
  const auto scope = runtimePcScope(cpu_.state().pc);
  const auto multiplayer = multiplayerOverlayLoaded();
  if ((scope == RuntimePcScope::singleplayer && multiplayer) ||
      (scope == RuntimePcScope::multiplayer && !multiplayer)) {
    return;
  }

  // These addresses are opcode-witnessed LEVEL renderer entry points. They
  // are a semantic gameplay boundary, unlike overlay residency or a guessed
  // primitive shape, and are reached before the frame is published to host.
  if (gameplayOverlayLoaded()) {
    gameplay_presentation_ready_ = true;
  }

  if (multiplayer) {
    // LEVEL2P uses the same host-side Hor+ projection as LEVEL. Admit every
    // coarse BSP sector independently of output resolution, then disable only
    // horizontal rejection in its object/triangle paths. Per-triangle
    // vertical, depth and near-plane rejection remain retail-authored.
    switch (hook) {
    case AdaptiveWorldFrustumHook::multiplayer_bsp_force_visible:
      // Admit every coarse BSP sector. Exact triangle/object rejection below
      // still removes geometry outside the real viewport or behind the camera.
      cpu_.setRegister(4U, 1U); // a0: sector-visible result
      break;
    case AdaptiveWorldFrustumHook::bsp_upper_x:
    case AdaptiveWorldFrustumHook::bsp_lower_x:
    case AdaptiveWorldFrustumHook::object_upper_x:
      // These hooks run on the proven rejecting branch after its comparison.
      cpu_.setRegister(2U, 0U); // v0: branch condition / signed screen X
      break;
    case AdaptiveWorldFrustumHook::level_triangle_outcode:
      // LEVEL2P has already split SXY into X/Y temporaries. Clearing X makes
      // every triangle horizontally eligible without changing its packet.
      cpu_.setRegister(2U, 0U); // v0 = x0
      cpu_.setRegister(4U, 0U); // a0 = x1
      cpu_.setRegister(9U, 0U); // t1 = x2
      break;
    case AdaptiveWorldFrustumHook::slus_triangle_outcode:
      // The resident TSP paths still hold vertices 1/2 as packed Y:X words.
      // Preserve signed Y in the upper half while clearing only screen X.
      cpu_.setRegister(8U, 0U); // t0 = sign-extended x0; y0 is already t3
      cpu_.setRegister(9U, cpu_.state().gpr[9U] & 0xffff0000U);
      cpu_.setRegister(10U, cpu_.state().gpr[10U] & 0xffff0000U);
      break;
    case AdaptiveWorldFrustumHook::none:
    default:
      break;
    }
    return;
  }

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
  if (margin == 0U)
    return;
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
  case AdaptiveWorldFrustumHook::multiplayer_bsp_force_visible:
  default:
    break;
  }
}

void Runtime::captureExactTransformInput() noexcept {
  auto capture = ExactTransformCapture{};
  const auto &state = cpu_.state();
  capture.caller_stack = state.gpr[29U];
  capture.destination = state.gpr[7U];

  std::array<std::uint32_t, 3U> translation_words{};
  std::array<std::uint32_t, 3U> angle_words{};
  std::array<std::uint32_t, 3U> scale_words{};
  for (std::size_t component{}; component < 3U; ++component) {
    const auto offset = static_cast<std::uint32_t>(component * 4U);
    if (!cpu_.read32(state.gpr[4U] + offset, translation_words[component]) ||
        !cpu_.read32(state.gpr[5U] + offset, angle_words[component]) ||
        !cpu_.read32(state.gpr[6U] + offset, scale_words[component])) {
      ++stats_.exact_transform_rejections;
      return;
    }
  }

  constexpr auto angle_radians = 2.0 * std::numbers::pi / (4096.0 * 65536.0);
  constexpr auto matrix_scale = 4096.0;
  const auto angle = [&angle_words](std::size_t component) noexcept {
    return static_cast<double>(
        std::bit_cast<std::int32_t>(angle_words[component]));
  };
  const auto x = angle(0U) * angle_radians;
  const auto y = angle(1U) * angle_radians;
  const auto z = angle(2U) * angle_radians;
  const auto sx = std::sin(x);
  const auto cx = std::cos(x);
  const auto sy = std::sin(y);
  const auto cy = std::cos(y);
  const auto sz = std::sin(z);
  const auto cz = std::cos(z);

  const std::array rotation{
      cz * cy * matrix_scale,
      -sz * cy * matrix_scale,
      sy * matrix_scale,
      (sz * cx + cz * sx * sy) * matrix_scale,
      (cz * cx - sz * sx * sy) * matrix_scale,
      -cy * sx * matrix_scale,
      (sz * sx - cz * cx * sy) * matrix_scale,
      (cz * sx + sz * cx * sy) * matrix_scale,
      cy * cx * matrix_scale,
  };
  for (std::size_t row{}; row < 3U; ++row) {
    const auto scale =
        static_cast<double>(std::bit_cast<std::int32_t>(scale_words[row])) /
        16.0;
    if (!std::isfinite(scale)) {
      ++stats_.exact_transform_rejections;
      return;
    }
    for (std::size_t column{}; column < 3U; ++column) {
      capture.rotation[row * 3U + column] =
          rotation[row * 3U + column] * scale / matrix_scale;
    }
    capture.translation[row] = static_cast<double>(std::bit_cast<std::int32_t>(
                                   translation_words[row])) /
                               65536.0;
  }

  const auto available =
      std::ranges::find_if(exact_transform_captures_,
                           [](const auto &entry) { return !entry.valid; });
  if (available == exact_transform_captures_.end()) {
    ++stats_.exact_transform_rejections;
    return;
  }
  capture.valid = true;
  *available = capture;
  ++stats_.exact_transform_captures;
}

void Runtime::publishExactTransformOutput() noexcept {
  const auto &state = cpu_.state();
  const auto caller_stack = state.gpr[29U] + 0x40U;
  const auto destination = state.gpr[17U];
  const auto found =
      std::ranges::find_if(exact_transform_captures_, [&](const auto &capture) {
        return capture.valid && capture.caller_stack == caller_stack &&
               capture.destination == destination;
      });
  if (found == exact_transform_captures_.end()) {
    ++stats_.exact_transform_rejections;
    return;
  }
  const auto published = cpu_.publishExactTransform(
      found->destination, found->rotation, found->translation);
  *found = {};
  if (published) {
    ++stats_.exact_transform_publications;
  } else {
    ++stats_.exact_transform_rejections;
  }
}
void Runtime::captureExactMatrixComposition() noexcept {
  ++stats_.exact_composition_entries;
  ExactMatrixCompositionCapture capture{};
  const auto &state = cpu_.state();
  capture.caller_stack = state.gpr[29U];
  capture.output = state.gpr[5U];
  bool lhs_enhanced{};
  bool rhs_enhanced{};
  if (!cpu_.captureTransformForComposition(state.gpr[4U], capture.lhs_rotation,
                                           capture.lhs_translation,
                                           lhs_enhanced) ||
      !cpu_.captureTransformForComposition(capture.output, capture.rhs_rotation,
                                           capture.rhs_translation,
                                           rhs_enhanced) ||
      (!lhs_enhanced && !rhs_enhanced)) {
    exact_matrix_composition_ = {};
    return;
  }
  capture.valid = true;
  exact_matrix_composition_ = capture;
  ++stats_.exact_composition_captures;
}

void Runtime::publishExactMatrixComposition() noexcept {
  ++stats_.exact_composition_sites;
  const auto &state = cpu_.state();
  if (!exact_matrix_composition_.valid ||
      exact_matrix_composition_.caller_stack != state.gpr[29U]) {
    return;
  }
  const auto published = cpu_.publishComposedExactTransform(
      exact_matrix_composition_.lhs_rotation,
      exact_matrix_composition_.lhs_translation,
      exact_matrix_composition_.rhs_rotation,
      exact_matrix_composition_.rhs_translation,
      exact_matrix_composition_.output);
  exact_matrix_composition_ = {};
  if (published) {
    ++stats_.exact_composition_publications;
  } else {
    ++stats_.exact_transform_rejections;
  }
}

bool Runtime::selectFrontendMenuTarget(std::uint32_t screen_id,
                                       std::uint32_t selection) noexcept {
  return mohu::selectFrontendMenuTarget(cpu_, screen_id, selection);
}

RuntimeFrameResult Runtime::runFrame(std::uint16_t active_low_buttons,
                                     std::array<std::uint8_t, 4U> analog) {
  RuntimeControllerInputs controllers{};
  controllers[0U].active_low_buttons = active_low_buttons;
  controllers[0U].analog = analog;
  controllers[1U].connected = false;
  return runFrame(controllers);
}

RuntimeFrameResult
Runtime::runFrame(const RuntimeControllerInputs &controllers) {
  std::uint32_t overlay_marker{};
  if (cpu_.read32(frontend_overlay_marker.address, overlay_marker) &&
      overlay_marker == frontend_overlay_marker.instruction) {
    gameplay_presentation_ready_ = false;
  }
  gpu_.beginFrame();
  cpu_.beginGpuProjectionFrame();
  for (std::size_t port{}; port < controllers.size(); ++port) {
    const auto &controller = controllers[port];
    machine_.setControllerState(port, controller.active_low_buttons,
                                controller.analog, controller.connected);
  }
  machine_.pulseVBlank();

  const auto applied_debt =
      std::min(instruction_debt_, guest_instructions_per_frame - 1U);
  instruction_debt_ -= applied_debt;
  const auto frame_budget = guest_instructions_per_frame - applied_debt;
  std::uint64_t frame_instructions{};
  while (frame_instructions < frame_budget || bios_.exceptionActive()) {
    const auto pc = cpu_.state().pc;
    auto scalar_boundary = false;
    if (pc == 0x8009be98U) {
      captureExactTransformInput();
      scalar_boundary = true;
    } else if (pc == 0x8009bf3cU) {
      publishExactTransformOutput();
      scalar_boundary = true;
    } else if (pc == 0x8001d578U) {
      captureExactMatrixComposition();
      scalar_boundary = true;
    } else if (pc == 0x8001d67cU) {
      publishExactMatrixComposition();
      scalar_boundary = true;
    }
    const auto pc_action = runtimePcAction(pc);
    scalar_boundary = scalar_boundary || pc_action != RuntimePcAction::none;
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
    const auto async_service_pending = bios_.asyncServicePending();
    auto async_service_wait_ticks = std::uint64_t{};
    if (async_service_pending) {
      const auto async_service = bios_.serviceAsync();
      if (async_service == sf::psx::BiosAsyncServiceResult::failed) {
        return {RuntimeStatus::cpu_stopped,
                sf::psx::R3000StopReason::unsupported_instruction,
                "Cannot complete an asynchronous BIOS operation"};
      }
      if (async_service == sf::psx::BiosAsyncServiceResult::progressed) {
        continue;
      }
      async_service_wait_ticks = bios_.asyncServiceTicksRemaining();
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
        const auto last = static_cast<std::uint64_t>(extent) + sectors;
        if (sectors != 0U &&
            last <= std::numeric_limits<std::uint32_t>::max()) {
          const auto found = std::ranges::find_if(
              level_extents_, [extent, last](const LevelExtent &candidate) {
                return candidate.first_lba == extent &&
                       candidate.last_lba == last;
              });
          if (found != level_extents_.end()) {
            active_campaign_level_ = found->campaign_level;
          }
        }
      }
      break;
    }
    case RuntimePcAction::multiplayer_renderer_boundary:
      if (multiplayerOverlayLoaded()) {
        gameplay_presentation_ready_ = true;
      }
      break;
    case RuntimePcAction::frustum_bsp_upper_x:
      applyAdaptiveWorldFrustumHook(AdaptiveWorldFrustumHook::bsp_upper_x);
      break;
    case RuntimePcAction::frustum_bsp_lower_x:
      applyAdaptiveWorldFrustumHook(AdaptiveWorldFrustumHook::bsp_lower_x);
      break;
    case RuntimePcAction::frustum_multiplayer_bsp_force_visible:
      applyAdaptiveWorldFrustumHook(
          AdaptiveWorldFrustumHook::multiplayer_bsp_force_visible);
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

    auto cached_budget = frame_instructions < frame_budget
                             ? frame_budget - frame_instructions
                             : 0U;
    if (async_service_wait_ticks != 0U) {
      cached_budget = std::min(cached_budget, async_service_wait_ticks);
    }
    const auto execution = !scalar_boundary && cached_budget != 0U
                               ? machine_.runCached(cached_budget)
                               : machine_.step();
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

  updateActiveLevel();
  updateGameplayPresentation();
  ++stats_.frames;
  stats_.instructions += frame_instructions;
  stats_.cached_fast_fallbacks = cpu_.cachedFastFallbacks();
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

std::size_t Runtime::takeSpuDiagnostics(
    std::span<sf::psx::SpuDiagnosticEvent> destination) noexcept {
  return machine_.spu().takeDiagnostics(destination);
}

} // namespace mohu
