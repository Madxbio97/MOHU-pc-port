#include "sf/platform/host.hpp"

#include "mohu/display_presentation.hpp"
#include "psycross_audio_output.hpp"
#include "psycross_guest_gpu.hpp"
#include "psycross_mission_start.hpp"
#include "psycross_movie_player.hpp"
#include "psycross_runtime_guards.hpp"
#include "psycross_scene_viewer.hpp"
#include "psycross_skybox.hpp"
#include "psycross_video_mode.hpp"
#include "psycross_window_mode.hpp"
#include "volumetric_atlas_texture.hpp"

#include "sf/core/error.hpp"
#include "sf/game/campaign.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/retail_cheats.hpp"
#include "sf/game/title.hpp"
#include "sf/platform/audio_output_policy.hpp"
#include "sf/psx/bios_hle.hpp"
#include "sf/psx/r3000_runtime.hpp"
#include "sf/psx/spu.hpp"

#include <PsyX/PsyX_globals.h>
#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>
#include <SDL.h>
#include <psx/libetc.h>
#include <psx/libgpu.h>
#include <psx/libgte.h>
#include <psx/libpad.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace sf::platform {
namespace {

detail::StandaloneMovieSkipPolicy
endingMovieSkipPolicy(const game::MissionDefinition &definition) noexcept {
  const auto catalog = game::missionCatalog();
  if (!catalog.empty() && definition.index == catalog.back().index) {
    // EOL/SILO.STR contains the credits and the post-credits scene.  It is a
    // single retail stream, so allowing a carried confirm/cancel edge from the
    // save menu to skip it loses the entire campaign ending.
    return detail::StandaloneMovieSkipPolicy::prevent;
  }
  return detail::StandaloneMovieSkipPolicy::allow;
}

void configureGraphics(const GraphicsSettings &settings) noexcept {
  const bool postprocess_aa = settings.smaa || settings.fxaa;
  g_cfg_msaaSamples = postprocess_aa ? 0 : settings.msaa_samples;
  g_cfg_bilinearFiltering = settings.bilinear_filtering ? 1 : 0;
  g_cfg_trilinearFiltering = settings.trilinear_filtering ? 1 : 0;
  g_cfg_anisotropicFiltering = settings.anisotropic_filtering ? 1 : 0;
  g_cfg_smaa = settings.smaa ? 1 : 0;
  g_cfg_fxaa = settings.fxaa ? 1 : 0;
  g_cfg_volumetricFog = settings.volumetric_fog ? 1 : 0;
  g_cfg_volumetricEffects = settings.volumetric_effects ? 1 : 0;
  g_cfg_aspectMode = settings.aspect_ratio == AspectRatioMode::adaptive
                         ? PSYX_ASPECT_ADAPTIVE
                         : PSYX_ASPECT_ORIGINAL_4_3;
  g_cfg_composedGuestScanout = 0;
  g_cfg_smaaFinalFrame = 0;
  g_cfg_fxaaFinalFrame = 0;
  g_cfg_renderWidth = std::max(settings.width, 1);
  g_cfg_renderHeight = std::max(settings.height, 1);
  g_cfg_swapInterval = settings.vsync ? 1 : 0;
  // Presentation is native: no game code samples the displayed framebuffer
  // through PSX VRAM. Hardware remains at 60 VBlank/s while MOHU's authored
  // gameplay, animation and rendering update at 30 Hz. Avoid the legacy
  // readback and busy VBlank paths, which otherwise steal presentation time.
  g_cfg_framebufferFeedback = 0;
  g_cfg_vblankThread = 0;
}

void configureControllerProtocol(ControllerProtocol protocol) noexcept {
  const auto set = [](const char *name, bool enabled) {
    static_cast<void>(
        SDL_SetHintWithPriority(name, enabled ? "1" : "0", SDL_HINT_OVERRIDE));
  };

  // Fix the Windows joystick driver set before SDL initializes it. Forced
  // modes disable competing drivers so one physical pad is opened once.
  const auto automatic = protocol == ControllerProtocol::automatic;
  set(SDL_HINT_XINPUT_ENABLED,
      automatic || protocol == ControllerProtocol::xinput);
  set(SDL_HINT_DIRECTINPUT_ENABLED,
      automatic || protocol == ControllerProtocol::direct_input);
  set(SDL_HINT_JOYSTICK_RAWINPUT,
      automatic || protocol == ControllerProtocol::raw_input);
  set(SDL_HINT_JOYSTICK_RAWINPUT_CORRELATE_XINPUT, true);
  set(SDL_HINT_JOYSTICK_WGI, automatic);
  set(SDL_HINT_JOYSTICK_HIDAPI, automatic);
  set(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, true);
  set(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, true);
}

void configureControllerDevices(const GraphicsSettings &settings) noexcept {
  constexpr std::array default_devices{0, 1};
  constexpr auto disabled_psycross_slot = -2;
  const auto &devices =
      areControllerDeviceRoutesValid(settings.controller_device_indices)
          ? settings.controller_device_indices
          : default_devices;
  for (std::size_t slot{}; slot < devices.size(); ++slot) {
    const auto device = devices[slot];
    g_cfg_controllerToSlotMapping[slot] =
        isValidControllerDeviceIndex(device) &&
                device != disabled_controller_device
            ? device
            : disabled_psycross_slot;
  }
}

void configurePresentation(const GraphicsSettings &settings) noexcept {
  // SDL's high-resolution timer and GL context both exist only after
  // PsyX_Initialise. Apply the two independent presentation controls here:
  // swap interval removes tearing, while the software cap controls cadence.
  PsyX_EnableSwapInterval(settings.vsync ? 1 : 0);
  PsyX_SetSwapInterval(1);
  PsyX_SetFrameLimit(static_cast<int>(settings.frame_limit));
  if (settings.volumetric_effects) {
    using namespace detail::volumetric_atlas_texture;
    static_assert(rgba.size() == static_cast<std::size_t>(width) * height * 4U);
    if (GR_UploadVolumetricDensityAtlas(static_cast<int>(width),
                                        static_cast<int>(height),
                                        rgba.data()) == 0) {
      // The renderer keeps its procedural volume path, and ultimately the
      // exact retail sprite fallback, if the authored atlas cannot upload.
      PsyX_Log_Warning(
          "Volumetric density atlas is unavailable; using procedural shapes\n");
    }
  }
}

void configureInput() {
  g_cfg_keyboardMapping.kc_dpad_up =
      PsyX_LookupKeyboardMapping("NONE", g_cfg_keyboardMapping.kc_dpad_up);
  g_cfg_keyboardMapping.kc_dpad_down =
      PsyX_LookupKeyboardMapping("NONE", g_cfg_keyboardMapping.kc_dpad_down);
  g_cfg_keyboardMapping.kc_dpad_left =
      PsyX_LookupKeyboardMapping("NONE", g_cfg_keyboardMapping.kc_dpad_left);
  g_cfg_keyboardMapping.kc_dpad_right =
      PsyX_LookupKeyboardMapping("NONE", g_cfg_keyboardMapping.kc_dpad_right);
  // PC actions are sampled directly by the player-input adapter. Keep them
  // out of PsyCross's merged virtual pad so Left Shift cannot turn a physical
  // R1 target-lock into the PC run action and Space cannot become Select.
  g_cfg_keyboardMapping.kc_r1 =
      PsyX_LookupKeyboardMapping("NONE", g_cfg_keyboardMapping.kc_r1);
  g_cfg_keyboardMapping.kc_l1 =
      PsyX_LookupKeyboardMapping("NONE", g_cfg_keyboardMapping.kc_l1);
  g_cfg_keyboardMapping.kc_select =
      PsyX_LookupKeyboardMapping("NONE", g_cfg_keyboardMapping.kc_select);
}

std::uint16_t readHostButtons(const PADRAW &pad) noexcept;
struct RuntimePadSample {
  RuntimePadInputs pads;
  std::int32_t mouse_delta_x{};
  std::int32_t mouse_delta_y{};
  bool mouse_look_active{};
};

RuntimePadSample sampleRuntimePadInput(
    const KeyboardMouseBindings &bindings,
    const ControllerButtonBindings &controller_bindings,
    const ControllerButtonBindings &controller_bindings_player_2,
    const MohUndergroundRuntimeActionBindings &runtime_actions,
    detail::RelativeMouseCapture &mouse_capture, bool menu_active) noexcept;

enum class RuntimePerfPresentation {
  interpolated,
  cached,
  full,
};

struct RuntimeGeometryTraceCounters {
  std::uint64_t polygons{};
  std::uint64_t precise{};
  std::uint64_t exact{};
  std::uint64_t integer{};
  std::uint64_t missing{};
  std::uint64_t nonfinite{};
  std::uint64_t packet_mismatch{};
  std::uint64_t hazard{};
  std::uint64_t depth_saturation{};
  std::uint64_t divide_overflow{};
  std::uint64_t screen{};
  std::uint64_t camera{};
  std::uint64_t near_plane{};
  std::uint64_t identity_recovered{};
  std::uint64_t catalog_recovered{};
  std::uint64_t plane_recovered{};
  std::uint64_t high_resolution_presents{};
  std::uint64_t fallback_presents{};
  std::uint64_t replay_captured{};
  std::uint64_t replay_promoted{};
  std::uint64_t replay_skipped_vram{};
  std::uint64_t replay_interpolated{};
  std::uint64_t replay_rejected{};
};

[[nodiscard]] RuntimeGeometryTraceCounters
captureGeometryTraceCounters(const detail::PsyCrossGuestGpu &gpu) noexcept {
  return {
      .polygons = gpu.polygonPrimitives(),
      .precise = gpu.precisePrimitives(),
      .exact = gpu.exactPrecisePrimitives(),
      .integer = gpu.integerPrecisePrimitives(),
      .missing = gpu.missingProjectionPrimitives(),
      .nonfinite = gpu.nonfiniteProjectionPrimitives(),
      .packet_mismatch = gpu.packetMismatchPrimitives(),
      .hazard = gpu.projectionHazardPrimitives(),
      .depth_saturation = gpu.depthSaturationPrimitives(),
      .divide_overflow = gpu.divideOverflowPrimitives(),
      .screen =
          gpu.screenSaturationPrimitives() + gpu.screenMismatchPrimitives(),
      .camera = gpu.cameraMismatchPrimitives(),
      .near_plane = gpu.nearPlanePrimitives(),
      .identity_recovered = gpu.identityRecoveredPrimitives(),
      .catalog_recovered = gpu.projectionCatalogPrimitives(),
      .plane_recovered = gpu.planeRecoveredPrimitives(),
      .high_resolution_presents = gpu.highResolutionPresents(),
      .fallback_presents = gpu.fallbackPresents(),
      .replay_captured = gpu.capturedPresentationReplayEvents(),
      .replay_promoted = gpu.promotedPresentationReplayFrames(),
      .replay_skipped_vram = gpu.skippedPresentationReplayVramCommands(),
      .replay_interpolated = gpu.interpolatedPresentationReplayFrames(),
      .replay_rejected = gpu.rejectedPresentationReplayFrames(),
  };
}

class RuntimeGeometryTrace final {
public:
  void notePresentation(RuntimePerfPresentation presentation,
                        float interpolation_alpha = 0.0F) noexcept {
    if (!enabled()) {
      return;
    }
    const auto code = static_cast<std::uint64_t>(presentation) + 1U;
    presentation_pattern_ = (presentation_pattern_ << 2U) | code;
    presentation_pattern_samples_ =
        std::min<std::uint32_t>(presentation_pattern_samples_ + 1U, 32U);
    switch (presentation) {
    case RuntimePerfPresentation::interpolated:
      ++interpolated_presentations_;
      if (interpolated_presentations_ == 1U) {
        minimum_interpolation_alpha_ = interpolation_alpha;
        maximum_interpolation_alpha_ = interpolation_alpha;
      } else {
        minimum_interpolation_alpha_ =
            std::min(minimum_interpolation_alpha_, interpolation_alpha);
        maximum_interpolation_alpha_ =
            std::max(maximum_interpolation_alpha_, interpolation_alpha);
      }
      last_interpolation_alpha_ = interpolation_alpha;
      break;
    case RuntimePerfPresentation::cached:
      ++cached_presentations_;
      break;
    case RuntimePerfPresentation::full:
      ++full_presentations_;
      break;
    }
  }

  void logFrame(const RuntimeGpuFrame &frame,
                const mohu::GuestDisplayGeometry &stable,
                const RuntimeGpuDisplayPublication &active,
                const detail::PsyCrossGuestGpu &gpu,
                const RuntimeGeometryTraceCounters &before,
                double authored_frame_seconds, std::size_t pending_batches,
                std::size_t pending_words) noexcept {
    if (!enabled()) {
      return;
    }

    const auto now = SDL_GetPerformanceCounter();
    if (!initialized_) {
      initialized_ = true;
      frequency_ = SDL_GetPerformanceFrequency();
      origin_ = now;
      PsyX_Log_Info("[GeometryTrace][start] clock_hz=%llu "
                    "disable_with=MOHU_GEOMETRY_TRACE=0\n",
                    static_cast<unsigned long long>(frequency_));
    }
    const auto elapsed_ms = frequency_ != 0U
                                ? static_cast<double>(now - origin_) * 1'000.0 /
                                      static_cast<double>(frequency_)
                                : 0.0;

    std::size_t valid_catalog{};
    std::size_t eligible_catalog{};
    std::size_t exact_catalog{};
    std::size_t fractional_catalog{};
    std::size_t macro_fractional_catalog{};
    std::size_t unclamped_catalog{};
    std::size_t depth_le_32{};
    std::size_t depth_le_64{};
    std::size_t depth_le_128{};
    auto minimum_positive_depth = std::numeric_limits<float>::max();
    for (const auto &projection : frame.projection_catalog) {
      if (!projection.valid || !std::isfinite(projection.view_z)) {
        continue;
      }
      ++valid_catalog;
      eligible_catalog += projection.pgxpEligible() ? 1U : 0U;
      exact_catalog += projection.exact_transform ? 1U : 0U;
      fractional_catalog += projection.fractional_transform ? 1U : 0U;
      constexpr auto enhanced_transform_sources =
          psx::GteProjectedVertex::enhanced_rotation |
          psx::GteProjectedVertex::enhanced_translation |
          psx::GteProjectedVertex::enhanced_vector;
      macro_fractional_catalog +=
          projection.exact_transform && projection.fractional_transform &&
                  (projection.enhanced_sources & enhanced_transform_sources) ==
                      0U
              ? 1U
              : 0U;
      unclamped_catalog += projection.hasUnclampedView() ? 1U : 0U;
      if (projection.view_z <= 0.0F) {
        continue;
      }
      minimum_positive_depth =
          std::min(minimum_positive_depth, projection.view_z);
      depth_le_32 += projection.view_z <= 32.0F ? 1U : 0U;
      depth_le_64 += projection.view_z <= 64.0F ? 1U : 0U;
      depth_le_128 += projection.view_z <= 128.0F ? 1U : 0U;
    }
    if (minimum_positive_depth == std::numeric_limits<float>::max()) {
      minimum_positive_depth = 0.0F;
    }

    const auto after = captureGeometryTraceCounters(gpu);
    const auto delta = [](std::uint64_t current,
                          std::uint64_t previous) noexcept {
      return current >= previous ? current - previous : 0U;
    };
    const auto primitive_fallback =
        delta(after.polygons, before.polygons) >=
                delta(after.precise, before.precise)
            ? delta(after.polygons, before.polygons) -
                  delta(after.precise, before.precise)
            : 0U;

    const auto &replay = gpu.currentPresentationReplayFrame();
    auto replay_events = std::size_t{};
    for (const auto &page : replay.pages) {
      replay_events += page.events.size();
    }
    const auto *first_page = replay.pages.empty() ? nullptr : &replay.pages[0];
    const auto *first_write =
        replay.vram_writes.empty() ? nullptr : &replay.vram_writes[0];
    auto last_clear_sequence = std::uint64_t{};
    for (const auto &page : replay.pages) {
      for (const auto &event : page.events) {
        if (event.kind == detail::PresentationReplayEventKind::clear) {
          last_clear_sequence = std::max(last_clear_sequence, event.sequence);
        }
      }
    }
    const auto empty_target = detail::PresentationReplayDrawTarget{};
    const auto &page_target =
        first_page != nullptr ? first_page->target : empty_target;
    const auto &write_target =
        first_write != nullptr ? first_write->target : empty_target;
    const auto &write_source =
        first_write != nullptr ? first_write->source : empty_target;

    PsyX_Log_Info(
        "[GeometryTrace][frame] t_ms=%.3f seq=%llu pub=%llu epoch=%llu "
        "words=%zu pubs=%zu content=%u level=%u aspect=%d "
        "display_raw=%u,%u,%ux%u,%u,%u,%u "
        "display_stable=%ux%u,%u,%u active=%u,%u,%ux%u,%u,%u,%u "
        "path_prev(i/c/f/pattern/samples)=%llu/%llu/%llu/0x%016llx/%u "
        "alpha(min/max/last)=%.3f/%.3f/%.3f authored_ms=%.3f "
        "atomic(pending_batches/pending_words)=%zu/%zu "
        "pgxp(poly/precise/exact/integer/fallback/missing)="
        "%llu/%llu/%llu/%llu/%llu/%llu "
        "reject(nonfinite/packet/hazard/depth/divide/screen/camera/near)="
        "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
        "recover(identity/catalog/plane)=%llu/%llu/%llu "
        "catalog(total/valid/eligible/exact/fractional/macro_q12/unclamped/"
        "min_z/z32/z64/z128)="
        "%zu/%zu/%zu/%zu/%zu/%zu/%zu/%.3f/%zu/%zu/%zu "
        "scanout(high/fallback)=%llu/%llu "
        "replay(delta_capture/promote/skip/interpolate/reject)="
        "%llu/%llu/%llu/%llu/%llu "
        "replay_state(ready/reason/gen/pages/events/clears/writes/unknown/"
        "vram)=%u/%u/%llu/%zu/%zu/%zu/%zu/%zu/%u "
        "match(valid/proven/prev/current/raw/relative/total)="
        "%u/%u/%zu/%zu/%zu/%zu/%zu "
        "relative(prev/current/common/candidate/static/topology)="
        "%zu/%zu/%zu/%zu/%zu/%zu "
        "replay_page0=%u,%u,%ux%u "
        "vram_first(op/source/target/clear_seq/write_seq)="
        "0x%02x/%u,%u/%u,%u,%ux%u/%llu/%llu\n",
        elapsed_ms, static_cast<unsigned long long>(frame.sequence),
        static_cast<unsigned long long>(frame.display_publication_sequence),
        static_cast<unsigned long long>(frame.command_buffer_epoch),
        frame.words.size(), frame.display_publications.size(),
        static_cast<unsigned int>(frame.content),
        static_cast<unsigned int>(frame.campaign_level), g_cfg_aspectMode,
        static_cast<unsigned int>(frame.display_x),
        static_cast<unsigned int>(frame.display_y),
        static_cast<unsigned int>(frame.display_width),
        static_cast<unsigned int>(frame.display_height),
        frame.display_enabled ? 1U : 0U, frame.display_rgb24 ? 1U : 0U,
        frame.display_interlaced ? 1U : 0U,
        static_cast<unsigned int>(stable.width),
        static_cast<unsigned int>(stable.height), stable.rgb24 ? 1U : 0U,
        stable.interlaced ? 1U : 0U,
        static_cast<unsigned int>(active.display_x),
        static_cast<unsigned int>(active.display_y),
        static_cast<unsigned int>(active.display_width),
        static_cast<unsigned int>(active.display_height),
        active.display_enabled ? 1U : 0U, active.display_rgb24 ? 1U : 0U,
        active.display_interlaced ? 1U : 0U,
        static_cast<unsigned long long>(interpolated_presentations_),
        static_cast<unsigned long long>(cached_presentations_),
        static_cast<unsigned long long>(full_presentations_),
        static_cast<unsigned long long>(presentation_pattern_),
        presentation_pattern_samples_, minimum_interpolation_alpha_,
        maximum_interpolation_alpha_, last_interpolation_alpha_,
        authored_frame_seconds * 1'000.0, pending_batches, pending_words,
        static_cast<unsigned long long>(delta(after.polygons, before.polygons)),
        static_cast<unsigned long long>(delta(after.precise, before.precise)),
        static_cast<unsigned long long>(delta(after.exact, before.exact)),
        static_cast<unsigned long long>(delta(after.integer, before.integer)),
        static_cast<unsigned long long>(primitive_fallback),
        static_cast<unsigned long long>(delta(after.missing, before.missing)),
        static_cast<unsigned long long>(
            delta(after.nonfinite, before.nonfinite)),
        static_cast<unsigned long long>(
            delta(after.packet_mismatch, before.packet_mismatch)),
        static_cast<unsigned long long>(delta(after.hazard, before.hazard)),
        static_cast<unsigned long long>(
            delta(after.depth_saturation, before.depth_saturation)),
        static_cast<unsigned long long>(
            delta(after.divide_overflow, before.divide_overflow)),
        static_cast<unsigned long long>(delta(after.screen, before.screen)),
        static_cast<unsigned long long>(delta(after.camera, before.camera)),
        static_cast<unsigned long long>(
            delta(after.near_plane, before.near_plane)),
        static_cast<unsigned long long>(
            delta(after.identity_recovered, before.identity_recovered)),
        static_cast<unsigned long long>(
            delta(after.catalog_recovered, before.catalog_recovered)),
        static_cast<unsigned long long>(
            delta(after.plane_recovered, before.plane_recovered)),
        frame.projection_catalog.size(), valid_catalog, eligible_catalog,
        exact_catalog, fractional_catalog, macro_fractional_catalog,
        unclamped_catalog, minimum_positive_depth, depth_le_32, depth_le_64,
        depth_le_128,
        static_cast<unsigned long long>(delta(after.high_resolution_presents,
                                              before.high_resolution_presents)),
        static_cast<unsigned long long>(
            delta(after.fallback_presents, before.fallback_presents)),
        static_cast<unsigned long long>(
            delta(after.replay_captured, before.replay_captured)),
        static_cast<unsigned long long>(
            delta(after.replay_promoted, before.replay_promoted)),
        static_cast<unsigned long long>(
            delta(after.replay_skipped_vram, before.replay_skipped_vram)),
        static_cast<unsigned long long>(
            delta(after.replay_interpolated, before.replay_interpolated)),
        static_cast<unsigned long long>(
            delta(after.replay_rejected, before.replay_rejected)),
        gpu.presentationReplayReady() ? 1U : 0U,
        static_cast<unsigned int>(gpu.lastPresentationReplayRejectReason()),
        static_cast<unsigned long long>(replay.generation), replay.pages.size(),
        replay_events, replay.deferred_clears.size(), replay.vram_writes.size(),
        replay.unknown_vram_write_sequences.size(),
        replay.contains_vram_commands ? 1U : 0U,
        gpu.lastPresentationReplayMatchInputValid() ? 1U : 0U,
        gpu.lastPresentationReplayRelativeProvenanceProven() ? 1U : 0U,
        gpu.lastPresentationReplayPreviousPolygons(),
        gpu.lastPresentationReplayCurrentPolygons(),
        gpu.lastPresentationReplayRawMatches(),
        gpu.lastPresentationReplayRelativeMatches(),
        gpu.lastPresentationReplayTotalMatches(),
        gpu.lastPresentationReplayPreviousRelativePolygons(),
        gpu.lastPresentationReplayCurrentRelativePolygons(),
        gpu.lastPresentationReplayRelativeCommonKeys(),
        gpu.lastPresentationReplayRelativeCandidates(),
        gpu.lastPresentationReplayRelativeStaticRejects(),
        gpu.lastPresentationReplayRelativeTopologyRejects(),
        static_cast<unsigned int>(page_target.x),
        static_cast<unsigned int>(page_target.y),
        static_cast<unsigned int>(page_target.width),
        static_cast<unsigned int>(page_target.height),
        first_write != nullptr ? static_cast<unsigned int>(first_write->opcode)
                               : 0U,
        static_cast<unsigned int>(write_source.x),
        static_cast<unsigned int>(write_source.y),
        static_cast<unsigned int>(write_target.x),
        static_cast<unsigned int>(write_target.y),
        static_cast<unsigned int>(write_target.width),
        static_cast<unsigned int>(write_target.height),
        static_cast<unsigned long long>(last_clear_sequence),
        first_write != nullptr
            ? static_cast<unsigned long long>(first_write->sequence)
            : 0ULL);

    interpolated_presentations_ = 0U;
    cached_presentations_ = 0U;
    full_presentations_ = 0U;
    presentation_pattern_ = 0U;
    presentation_pattern_samples_ = 0U;
    minimum_interpolation_alpha_ = 0.0F;
    maximum_interpolation_alpha_ = 0.0F;
    last_interpolation_alpha_ = 0.0F;
  }

private:
  [[nodiscard]] static bool enabled() noexcept {
    static const auto value = [] {
      const auto *setting = SDL_getenv("MOHU_GEOMETRY_TRACE");
      return setting != nullptr && setting[0] != '\0' &&
             std::strcmp(setting, "0") != 0;
    }();
    return value;
  }

  std::uint64_t frequency_{};
  std::uint64_t origin_{};
  std::uint64_t interpolated_presentations_{};
  std::uint64_t cached_presentations_{};
  std::uint64_t full_presentations_{};
  std::uint64_t presentation_pattern_{};
  std::uint32_t presentation_pattern_samples_{};
  float minimum_interpolation_alpha_{};
  float maximum_interpolation_alpha_{};
  float last_interpolation_alpha_{};
  bool initialized_{};
};

class RuntimePerformanceDiagnostics final {
public:
  [[nodiscard]] std::uint64_t beginLoop() noexcept {
    if (!enabled()) {
      return 0U;
    }
    if (frequency_ == 0U) {
      frequency_ = SDL_GetPerformanceFrequency();
    }
    const auto now = SDL_GetPerformanceCounter();
    if (window_started_ == 0U) {
      window_started_ = now;
    }
    return now;
  }

  void addInput(std::uint64_t ticks) noexcept { input_ticks_ += ticks; }
  void addGuest(std::uint64_t ticks) noexcept { guest_ticks_ += ticks; }
  void addGpu(std::uint64_t ticks) noexcept { gpu_ticks_ += ticks; }
  void addAudio(std::uint64_t ticks) noexcept { audio_ticks_ += ticks; }
  void addPresent(std::uint64_t ticks) noexcept { present_ticks_ += ticks; }
  void addGuestStep() noexcept { ++guest_steps_; }
  void addGpuSubmission() noexcept { ++gpu_submissions_; }

  void finishLoop(std::uint64_t started, RuntimePerfPresentation presentation,
                  const RuntimeGuestCadencePolicy &cadence,
                  const detail::PsyCrossGuestGpu &guest_gpu) noexcept {
    if (started == 0U) {
      return;
    }
    const auto now = SDL_GetPerformanceCounter();
    const auto loop_ticks = now - started;
    loop_ticks_ += loop_ticks;
    maximum_loop_ticks_ = std::max(maximum_loop_ticks_, loop_ticks);
    ++loops_;
    switch (presentation) {
    case RuntimePerfPresentation::interpolated:
      ++interpolated_presentations_;
      break;
    case RuntimePerfPresentation::cached:
      ++cached_presentations_;
      break;
    case RuntimePerfPresentation::full:
      ++full_presentations_;
      break;
    }

    if (frequency_ == 0U || now - window_started_ < frequency_ * 2U) {
      return;
    }
    const auto accounted_ticks = input_ticks_ + guest_ticks_ + gpu_ticks_ +
                                 audio_ticks_ + present_ticks_;
    const auto outside_ticks =
        loop_ticks_ > accounted_ticks ? loop_ticks_ - accounted_ticks : 0U;
    const auto window_ticks = now - window_started_;
    const auto rate = [this, window_ticks](std::uint64_t count) noexcept {
      return frequency_ != 0U && window_ticks != 0U
                 ? static_cast<double>(count) * frequency_ / window_ticks
                 : 0.0;
    };
    PsyX_Log_Info(
        "[PerfDiag][runtime] fps(present/logic/gpu)=%.1f/%.1f/%.1f "
        "loops=%llu guest_steps=%llu gpu_submits=%llu "
        "path(interp/cached/full)=%llu/%llu/%llu "
        "loop_ms(avg/max)=%.3f/%.3f phase_ms/loop(input/guest/gpu/audio/"
        "present/outside)=%.3f/%.3f/%.3f/%.3f/%.3f/%.3f "
        "cadence_ms(max/backlog/dropped)=%.3f/%.3f/%.3f resyncs=%llu\n",
        rate(loops_), rate(guest_steps_), rate(gpu_submissions_),
        static_cast<unsigned long long>(loops_),
        static_cast<unsigned long long>(guest_steps_),
        static_cast<unsigned long long>(gpu_submissions_),
        static_cast<unsigned long long>(interpolated_presentations_),
        static_cast<unsigned long long>(cached_presentations_),
        static_cast<unsigned long long>(full_presentations_),
        milliseconds(loop_ticks_, loops_),
        milliseconds(maximum_loop_ticks_, 1U),
        milliseconds(input_ticks_, loops_), milliseconds(guest_ticks_, loops_),
        milliseconds(gpu_ticks_, loops_), milliseconds(audio_ticks_, loops_),
        milliseconds(present_ticks_, loops_),
        milliseconds(outside_ticks, loops_),
        cadence.maximumElapsedSeconds() * 1'000.0,
        cadence.backlogSeconds() * 1'000.0, cadence.droppedSeconds() * 1'000.0,
        static_cast<unsigned long long>(cadence.lateRecoveryCount()));

    const auto polygons = guest_gpu.polygonPrimitives();
    const auto precise = guest_gpu.precisePrimitives();
    const auto fallback = polygons >= precise ? polygons - precise : 0U;
    const auto coverage =
        polygons != 0U ? static_cast<double>(precise) * 100.0 / polygons : 0.0;
    PsyX_Log_Info(
        "[PerfDiag][pgxp] total polygons=%llu precise=%llu coverage=%.1f%% "
        "exact=%llu integer=%llu candidates=%llu partial=%llu fallback=%llu\n",
        static_cast<unsigned long long>(polygons),
        static_cast<unsigned long long>(precise), coverage,
        static_cast<unsigned long long>(guest_gpu.exactPrecisePrimitives()),
        static_cast<unsigned long long>(guest_gpu.integerPrecisePrimitives()),
        static_cast<unsigned long long>(guest_gpu.preciseCandidates()),
        static_cast<unsigned long long>(
            guest_gpu.partialProjectionPrimitives()),
        static_cast<unsigned long long>(fallback));
    PsyX_Log_Info(
        "[PerfDiag][pgxp] reject(missing/nonfinite/packet/hazard/ir/depth/"
        "divide/screen/reproject/camera/near)="
        "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
        "recover(identity/catalog/plane_v/plane_p/plane_reject/ambiguous)="
        "%llu/%llu/%llu/%llu/%llu/%llu "
        "replay(captured/promoted/vram/ready/interpolated/rejected)="
        "%llu/%llu/%llu/%u/%llu/%llu\n",
        static_cast<unsigned long long>(
            guest_gpu.missingProjectionPrimitives()),
        static_cast<unsigned long long>(
            guest_gpu.nonfiniteProjectionPrimitives()),
        static_cast<unsigned long long>(guest_gpu.packetMismatchPrimitives()),
        static_cast<unsigned long long>(guest_gpu.projectionHazardPrimitives()),
        static_cast<unsigned long long>(guest_gpu.irSaturationPrimitives()),
        static_cast<unsigned long long>(guest_gpu.depthSaturationPrimitives()),
        static_cast<unsigned long long>(guest_gpu.divideOverflowPrimitives()),
        static_cast<unsigned long long>(guest_gpu.screenSaturationPrimitives() +
                                        guest_gpu.screenMismatchPrimitives()),
        static_cast<unsigned long long>(
            guest_gpu.reprojectionMismatchPrimitives()),
        static_cast<unsigned long long>(guest_gpu.cameraMismatchPrimitives()),
        static_cast<unsigned long long>(guest_gpu.nearPlanePrimitives()),
        static_cast<unsigned long long>(
            guest_gpu.identityRecoveredPrimitives()),
        static_cast<unsigned long long>(
            guest_gpu.projectionCatalogPrimitives()),
        static_cast<unsigned long long>(guest_gpu.planeRecoveredVertices()),
        static_cast<unsigned long long>(guest_gpu.planeRecoveredPrimitives()),
        static_cast<unsigned long long>(
            guest_gpu.planeRecoveryRejectedPrimitives()),
        static_cast<unsigned long long>(
            guest_gpu.projectionCatalogAmbiguities()),
        static_cast<unsigned long long>(
            guest_gpu.capturedPresentationReplayEvents()),
        static_cast<unsigned long long>(
            guest_gpu.promotedPresentationReplayFrames()),
        static_cast<unsigned long long>(
            guest_gpu.skippedPresentationReplayVramCommands()),
        guest_gpu.presentationReplayReady() ? 1U : 0U,
        static_cast<unsigned long long>(
            guest_gpu.interpolatedPresentationReplayFrames()),
        static_cast<unsigned long long>(
            guest_gpu.rejectedPresentationReplayFrames()));
    PsyX_Log_Info(
        "[PerfDiag][replay] reject_reason(page/target/clear/match/required/"
        "coverage/replay_page/mapping/backend)=%u\n",
        static_cast<unsigned int>(
            guest_gpu.lastPresentationReplayRejectReason()));
    const auto missing_group = [&guest_gpu](std::uint8_t base) noexcept {
      return guest_gpu.missingProjectionPrimitives(base) +
             guest_gpu.missingProjectionPrimitives(base + 1U) +
             guest_gpu.missingProjectionPrimitives(base + 2U) +
             guest_gpu.missingProjectionPrimitives(base + 3U);
    };
    PsyX_Log_Info("[PerfDiag][pgxp] missing_type(F3/FT3/F4/FT4/G3/GT3/G4/GT4)="
                  "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
                  "missing_vertices(1/2/3/4)=%llu/%llu/%llu/%llu\n",
                  static_cast<unsigned long long>(missing_group(0x20U)),
                  static_cast<unsigned long long>(missing_group(0x24U)),
                  static_cast<unsigned long long>(missing_group(0x28U)),
                  static_cast<unsigned long long>(missing_group(0x2cU)),
                  static_cast<unsigned long long>(missing_group(0x30U)),
                  static_cast<unsigned long long>(missing_group(0x34U)),
                  static_cast<unsigned long long>(missing_group(0x38U)),
                  static_cast<unsigned long long>(missing_group(0x3cU)),
                  static_cast<unsigned long long>(
                      guest_gpu.missingProjectionVertexBucket(1U)),
                  static_cast<unsigned long long>(
                      guest_gpu.missingProjectionVertexBucket(2U)),
                  static_cast<unsigned long long>(
                      guest_gpu.missingProjectionVertexBucket(3U)),
                  static_cast<unsigned long long>(
                      guest_gpu.missingProjectionVertexBucket(4U)));
    PsyX_Log_Info(
        "[PerfDiag][pgxp-dma] unavailable=%llu mixed=%llu overflow=%llu\n",
        static_cast<unsigned long long>(
            guest_gpu.missingProjectionDmaUnavailablePrimitives()),
        static_cast<unsigned long long>(
            guest_gpu.missingProjectionDmaMixedPrimitives()),
        static_cast<unsigned long long>(
            guest_gpu.missingProjectionDmaOverflowPrimitives()));
    std::array<const detail::MissingProjectionDmaDiagnostic *, 4U>
        top_missing_sources{};
    for (const auto &entry : guest_gpu.missingProjectionDmaDiagnostics()) {
      if (entry.primitives == 0U) {
        continue;
      }
      for (std::size_t rank{}; rank < top_missing_sources.size(); ++rank) {
        if (top_missing_sources[rank] != nullptr &&
            top_missing_sources[rank]->primitives >= entry.primitives) {
          continue;
        }
        for (std::size_t shifted = top_missing_sources.size() - 1U;
             shifted > rank; --shifted) {
          top_missing_sources[shifted] = top_missing_sources[shifted - 1U];
        }
        top_missing_sources[rank] = &entry;
        break;
      }
    }
    for (std::size_t rank{}; rank < top_missing_sources.size(); ++rank) {
      const auto *entry = top_missing_sources[rank];
      if (entry == nullptr) {
        break;
      }
      PsyX_Log_Info(
          "[PerfDiag][pgxp-dma] top=%u kind=%u opcode=0x%02x "
          "root=0x%08x words=0x%08x-0x%08x primitives=%llu full=%llu "
          "missing_vertices=%llu\n",
          static_cast<unsigned int>(rank + 1U),
          static_cast<unsigned int>(entry->kind),
          static_cast<unsigned int>(entry->opcode), entry->transfer_root,
          entry->first_word_address, entry->last_word_address,
          static_cast<unsigned long long>(entry->primitives),
          static_cast<unsigned long long>(entry->fully_missing_primitives),
          static_cast<unsigned long long>(entry->missing_vertices));
    }
    resetWindow(now);
  }

private:
  [[nodiscard]] static bool enabled() noexcept {
    static const auto value = [] {
      const auto *setting = SDL_getenv("SF_PERF_DIAGNOSTICS");
      return setting != nullptr && setting[0] != '\0' &&
             std::strcmp(setting, "0") != 0;
    }();
    return value;
  }

  [[nodiscard]] double milliseconds(std::uint64_t ticks,
                                    std::uint64_t divisor) const noexcept {
    if (frequency_ == 0U || divisor == 0U) {
      return 0.0;
    }
    return static_cast<double>(ticks) * 1'000.0 /
           (static_cast<double>(frequency_) * static_cast<double>(divisor));
  }

  void resetWindow(std::uint64_t now) noexcept {
    const auto frequency = frequency_;
    *this = {};
    frequency_ = frequency;
    window_started_ = now;
  }

  std::uint64_t frequency_{};
  std::uint64_t window_started_{};
  std::uint64_t loops_{};
  std::uint64_t guest_steps_{};
  std::uint64_t gpu_submissions_{};
  std::uint64_t interpolated_presentations_{};
  std::uint64_t cached_presentations_{};
  std::uint64_t full_presentations_{};
  std::uint64_t loop_ticks_{};
  std::uint64_t maximum_loop_ticks_{};
  std::uint64_t input_ticks_{};
  std::uint64_t guest_ticks_{};
  std::uint64_t gpu_ticks_{};
  std::uint64_t audio_ticks_{};
  std::uint64_t present_ticks_{};
};

class PsyCrossHost final : public Host {
public:
  PsyCrossHost(std::string title, GraphicsSettings graphics,
               RuntimeFrameCallback frame = {},
               KeyboardMouseBindings input = defaultKeyboardMouseBindings(),
               MohUndergroundRuntimeActionBindings runtime_actions =
                   defaultMohUndergroundRuntimeActionBindings(),
               RuntimeAudioDrainCallback audio = {})
      : title_(title.begin(), title.end()), graphics_(graphics),
        frame_(std::move(frame)), input_(std::move(input)),
        runtime_actions_(std::move(runtime_actions)), audio_(std::move(audio)) {
    title_.push_back('\0');
  }

  void run() override {
    configureGraphics(graphics_);
    if (frame_) {
      // Raw guest pages use the selected output resolution directly while
      // preserving the PS1's exact logical 1024x512 VRAM contract.
      g_cfg_composedGuestScanout = 1;
      // Native-target MSAA cannot affect the single-sample guest offscreen
      // pass. SMAA is applied after that pass is closed at GR_EndScene.
      g_cfg_msaaSamples = 0;
      g_cfg_smaaFinalFrame = graphics_.smaa ? 1 : 0;
      g_cfg_fxaaFinalFrame = graphics_.fxaa ? 1 : 0;
      g_cfg_volumetricFog = 1;
    }
    // Raw pages use one reversed camera-space W domain. Fallback packets keep
    // PS1 painter order.
    g_cfg_pgxpTextureCorrection = frame_ ? 1 : g_cfg_pgxpTextureCorrection;
    g_cfg_pgxpZBuffer = frame_ ? 1 : g_cfg_pgxpZBuffer;
    guest_gpu_.setGeometryOptions(true, true, true, false, false, false, true);
    guest_gpu_.setCoplanarClippingRecovery(false);
    guest_gpu_.setRuntimeGeometryPolicy(false, false, false);

    guest_gpu_.setModelAnimationInterpolationEnabled(true);
    configureControllerProtocol(graphics_.controller_protocol);
    configureControllerDevices(graphics_);
    PsyX_Initialise(title_.data(), graphics_.width, graphics_.height, 0);
    PsyX_Log_Info("Controller routing: P1=device %d P2=device %d "
                  "(-1 disabled)\n",
                  graphics_.controller_device_indices[0U],
                  graphics_.controller_device_indices[1U]);
    configurePresentation(graphics_);
    [[maybe_unused]] detail::PsyCrossWindowMode window_mode{
        graphics_.fullscreen};
    detail::configurePsyCrossVideoMode(detail::gameplay_video_mode, true);
    if (frame_) {
      GR_EnableDepth(1);
    }
    std::unique_ptr<detail::PsyCrossSkybox> skybox;
    if (frame_) {
      skybox = std::make_unique<detail::PsyCrossSkybox>();
    }
    std::unique_ptr<detail::PsyCrossAudioOutput> runtime_audio;
    std::array<psx::SpuPcmFrame, 4'096U> runtime_audio_scratch{};
    if (audio_) {
      // PsyX owns the process OpenAL context, so create the runtime source only
      // after platform initialization has completed.
      runtime_audio =
          std::make_unique<detail::PsyCrossAudioOutput>(2U, "retail-spu");
    }
    detail::RelativeMouseCapture runtime_mouse_capture;
    detail::MenuCursor runtime_menu_cursor;
    detail::RuntimeMenuPointerController runtime_menu_pointer;
    RuntimeMenuState runtime_menu;
    // Never compound an expensive guest frame with a multi-frame catch-up
    // burst. Keep one additional frame of debt, however: the elapsed sample
    // following a guest step includes that step's execution time. Clamping the
    // accumulator to one frame discarded this time and slowed the guest clock.
    RuntimeGuestCadencePolicy runtime_cadence{60.0, 1U, 2U, 2U};
    RuntimePresentationInterpolationClock presentation_clock{30.0};
    auto presentation_alpha = 1.0;
    auto runtime_previous_counter = SDL_GetPerformanceCounter();
    const auto runtime_counter_frequency = SDL_GetPerformanceFrequency();
    auto runtime_audio_diagnostic_counter = runtime_previous_counter;
    std::uint64_t guest_projection_epoch{};
    auto runtime_frame_ready = false;
    RuntimeAtomicFrameTracker runtime_frames;
    RuntimePerformanceDiagnostics performance_diagnostics;
    RuntimeGeometryTrace geometry_trace;
    RuntimeGpuDisplayPublication active_display;
    mohu::StableGuestDisplayGeometry runtime_display_geometry;
    auto active_display_valid = false;
    std::optional<PresentationContent> active_content;

    struct PendingGpuSubmission {
      std::shared_ptr<const RuntimeGpuFrame> frame;
      RuntimeGpuDisplayPublication display;
      std::size_t begin{};
      std::size_t end{};
    };
    std::vector<PendingGpuSubmission> pending_gpu_submissions;
    std::optional<std::uint64_t> pending_gpu_epoch;

    const auto prepareRuntimeDisplay =
        [this](const RuntimeGpuDisplayPublication &display,
               PresentationContent content) {
          const auto aspect =
              presentationAspectRatio(graphics_.aspect_ratio, content);
          const auto next_aspect = aspect == AspectRatioMode::adaptive
                                       ? PSYX_ASPECT_ADAPTIVE
                                       : PSYX_ASPECT_ORIGINAL_4_3;
          // Frontend and loading overlays can carry valid GTE tuples even
          // though they are screen-space artwork. Never let those tuples
          // inherit gameplay depth and reject a full-screen background.
          g_cfg_pgxpZBuffer = content == PresentationContent::gameplay ? 1 : 0;

          if (g_cfg_aspectMode != next_aspect) {
            PsyX_Log_Info(
                "Runtime aspect: %s, guest=%ux%u rgb24=%u interlaced=%u\n",
                next_aspect == PSYX_ASPECT_ADAPTIVE ? "adaptive" : "4:3",
                static_cast<unsigned int>(display.display_width),
                static_cast<unsigned int>(display.display_height),
                display.display_rgb24 ? 1U : 0U,
                display.display_interlaced ? 1U : 0U);
            g_cfg_aspectMode = next_aspect;
          }
          GR_SetGuestDisplayGeometry(static_cast<int>(display.display_width),
                                     static_cast<int>(display.display_height));
        };

    const auto presentRuntimeDisplay =
        [this](const RuntimeGpuDisplayPublication &display) {
          guest_gpu_.presentDisplay(
              display.display_x, display.display_y, display.display_width,
              display.display_height, display.display_enabled,
              display.display_rgb24, display.display_interlaced);
        };
    for (;;) {
      const auto diagnostic_loop_started = performance_diagnostics.beginLoop();
      auto diagnostic_phase_started = diagnostic_loop_started;
      auto diagnostic_present_started = diagnostic_loop_started;
      auto visual_dirty = false;
      if (frame_) {
        PsyX_UpdateInput();
        const auto runtime_counter = SDL_GetPerformanceCounter();
        const auto elapsed_seconds =
            runtime_counter_frequency != 0U
                ? static_cast<double>(runtime_counter -
                                      runtime_previous_counter) /
                      static_cast<double>(runtime_counter_frequency)
                : 1.0 / 60.0;
        runtime_previous_counter = runtime_counter;
        presentation_alpha = presentation_clock.advance(elapsed_seconds);
        const auto guest_steps = runtime_cadence.advance(elapsed_seconds);
        if (diagnostic_phase_started != 0U) {
          const auto now = SDL_GetPerformanceCounter();
          performance_diagnostics.addInput(now - diagnostic_phase_started);
          diagnostic_phase_started = now;
        }
        if (guest_steps != 0U) {
          const auto menu_active = runtime_menu.active;
          runtime_menu_cursor.set(menu_active &&
                                  SDL_GetKeyboardFocus() != nullptr);
          auto menu_pointer = runtime_menu_cursor.sample(512, 240);
          const auto input_sample = sampleRuntimePadInput(
              input_, graphics_.controller_bindings,
              graphics_.controller_bindings_player_2, runtime_actions_,
              runtime_mouse_capture, menu_active);
          auto runtime_running = true;
          for (std::size_t step{}; step < guest_steps; ++step) {
            auto pads = input_sample.pads;
            pads[0U] = applyMohUndergroundRuntimeMouseLook(
                pads[0U],
                runtimeMouseDeltaForGuestStep(input_sample.mouse_delta_x, step,
                                              guest_steps),
                runtimeMouseDeltaForGuestStep(input_sample.mouse_delta_y, step,
                                              guest_steps),
                input_sample.mouse_look_active,
                graphics_.mouse_sensitivity_percent, runtime_actions_);
            const auto menu_action =
                runtime_menu_pointer.update(runtime_menu, menu_pointer);
            pads[0U].active_low_buttons = static_cast<std::uint16_t>(
                pads[0U].active_low_buttons & menu_action.active_low_buttons);
            const RuntimeMenuInteraction menu_interaction{
                .selection_valid = menu_action.selection_valid,
                .screen_id = menu_action.screen_id,
                .selection = menu_action.selection,
            };
            menu_pointer.moved = false;
            menu_pointer.primary_pressed = false;
            menu_pointer.secondary_pressed = false;
            const auto guest_started = diagnostic_loop_started != 0U
                                           ? SDL_GetPerformanceCounter()
                                           : 0U;
            const auto runtime_step = frame_(pads, menu_interaction);
            if (!runtime_step.running) {
              runtime_running = false;
              break;
            }
            if (guest_started != 0U) {
              performance_diagnostics.addGuest(SDL_GetPerformanceCounter() -
                                               guest_started);
            }
            performance_diagnostics.addGuestStep();
            if (runtime_step.frame) {
              const auto gpu_started = diagnostic_loop_started != 0U
                                           ? SDL_GetPerformanceCounter()
                                           : 0U;
              const auto &gpu_frame = *runtime_step.frame;
              runtime_menu = gpu_frame.menu;
              guest_gpu_.setCampaignLighting(gpu_frame.campaign_level,
                                             gpu_frame.sequence);
              if (skybox) {
                skybox->setLevel(gpu_frame.campaign_level);
                skybox->setView(gpu_frame.atmosphere.skybox_yaw,
                                gpu_frame.atmosphere.skybox_pitch,
                                gpu_frame.atmosphere.skybox_vertical_fov,
                                gpu_frame.atmosphere.skybox_view_valid);
              }
              GR_EnableSceneFog(0);
              const auto geometry_trace_before =
                  captureGeometryTraceCounters(guest_gpu_);
              const auto observation = runtime_frames.observe(
                  gpu_frame.sequence, gpu_frame.display_publication_sequence);
              if (!observation.valid) {
                throw core::Error{
                    core::ErrorCode::invalid_argument,
                    "Runtime published a stale or non-monotonic frame"};
              }
              const auto gpu_frame_owner = runtime_step.frame;
              const auto stabilize_geometry_changes =
                  gpu_frame.content == PresentationContent::gameplay;
              const auto stable_geometry = runtime_display_geometry.update(
                  {gpu_frame.display_width, gpu_frame.display_height,
                   gpu_frame.display_rgb24, gpu_frame.display_interlaced},
                  stabilize_geometry_changes);
              const auto stabilize_geometry =
                  [stable_geometry, stabilize_geometry_changes](
                      RuntimeGpuDisplayPublication display) {
                    if (!stabilize_geometry_changes) {
                      return display;
                    }
                    display.display_width = stable_geometry.width;
                    display.display_height = stable_geometry.height;
                    display.display_rgb24 = stable_geometry.rgb24;
                    display.display_interlaced = stable_geometry.interlaced;
                    return display;
                  };

              const auto final_display =
                  stabilize_geometry(RuntimeGpuDisplayPublication{
                      .word_offset = gpu_frame.words.size(),
                      .sequence = gpu_frame.display_publication_sequence,
                      .display_x = gpu_frame.display_x,
                      .display_y = gpu_frame.display_y,
                      .display_width = gpu_frame.display_width,
                      .display_height = gpu_frame.display_height,
                      .display_enabled = gpu_frame.display_enabled,
                      .display_rgb24 = gpu_frame.display_rgb24,
                      .display_interlaced = gpu_frame.display_interlaced,
                  });
              const auto bootstrap_display = !active_display_valid;
              if (bootstrap_display) {
                active_display = final_display;
                active_display_valid = true;
              }

              const auto aligned_slice = [](const RuntimeGpuFrame &frame,
                                            const auto &values,
                                            std::size_t offset,
                                            std::size_t count) {
                const auto span = std::span{values};
                if (span.empty()) {
                  return span;
                }
                if (span.size() != frame.words.size()) {
                  throw core::Error{core::ErrorCode::invalid_argument,
                                    "Runtime GPU sidecar is not word-aligned"};
                }
                return span.subspan(offset, count);
              };
              const auto queue_range = [&](std::size_t offset,
                                           std::size_t end) {
                if (end < offset || end > gpu_frame.words.size()) {
                  throw core::Error{
                      core::ErrorCode::invalid_argument,
                      "Runtime GP1 boundary is outside the GPU batch"};
                }
                if (end == offset) {
                  return;
                }
                pending_gpu_epoch = gpu_frame.command_buffer_epoch;
                pending_gpu_submissions.push_back({
                    .frame = gpu_frame_owner,
                    .display = active_display,
                    .begin = offset,
                    .end = end,
                });
              };
              const auto flush_queued_ranges = [&] {
                for (const auto &batch : pending_gpu_submissions) {
                  const auto &frame = *batch.frame;
                  const auto count = batch.end - batch.begin;
                  guest_gpu_.setCampaignLighting(frame.campaign_level,
                                                 frame.sequence);
                  if (skybox) {
                    skybox->setLevel(frame.campaign_level);
                    skybox->setView(frame.atmosphere.skybox_yaw,
                                    frame.atmosphere.skybox_pitch,
                                    frame.atmosphere.skybox_vertical_fov,
                                    frame.atmosphere.skybox_view_valid);
                  }
                  prepareRuntimeDisplay(batch.display, frame.content);
                  GR_BeginGuestProjectionEpoch(
                      ++guest_projection_epoch,
                      static_cast<int>(batch.display.display_width),
                      static_cast<int>(batch.display.display_height),
                      batch.display.display_rgb24 ? 1 : 0,
                      batch.display.display_interlaced ? 1 : 0);
                  GR_BeginGuestSubmit();
                  guest_gpu_.submit(
                      std::span{frame.words}.subspan(batch.begin, count),
                      aligned_slice(frame, frame.projections, batch.begin,
                                    count),
                      aligned_slice(frame, frame.projection_identities,
                                    batch.begin, count),
                      frame.command_buffer_epoch, frame.projection_catalog,
                      aligned_slice(frame, frame.dma_sources, batch.begin,
                                    count));
                  performance_diagnostics.addGpuSubmission();
                }
                pending_gpu_submissions.clear();
                pending_gpu_epoch.reset();
              };

              if (pending_gpu_epoch &&
                  *pending_gpu_epoch != gpu_frame.command_buffer_epoch) {
                auto pending_words = std::size_t{};
                for (const auto &batch : pending_gpu_submissions) {
                  pending_words += batch.end - batch.begin;
                }
                PsyX_Log_Info(
                    "[MReturnTrace][epoch-flush] from=%llu to=%llu "
                    "batches=%zu words=%zu\n",
                    static_cast<unsigned long long>(*pending_gpu_epoch),
                    static_cast<unsigned long long>(
                        gpu_frame.command_buffer_epoch),
                    pending_gpu_submissions.size(), pending_words);
                // An epoch change retires the guest command buffer, not the
                // GPU work already accepted from it. Each pending range owns
                // an immutable frame snapshot, so commit it before admitting
                // commands from the replacement overlay/buffer.
                flush_queued_ranges();
              }

              const auto content_transition =
                  active_content && *active_content != gpu_frame.content;
              if (content_transition) {
                // Execute every accepted guest command under the display and
                // aspect which owned it before changing presentation mode.
                // Dropping this tail would desynchronize the host VRAM from
                // the emulated GPU; presenting it would expose a partial old
                // frame. The new scanout below remains the only visible one.
                flush_queued_ranges();
                // LEVEL draws the return-to-base still immediately before
                // SHELL replaces its command buffer. That completed page is
                // the background inherited by the authored 4:3 loader; the
                // shell intentionally does not reload MRETURN.RSC. Preserve
                // the pending replay while entering authored content, then
                // promote it through presentDisplay() below. Entering a new
                // gameplay scene must still reject frontend replay state.
                if (gpu_frame.content == PresentationContent::gameplay) {
                  guest_gpu_.resetPresentationHistory();
                }
                presentation_clock.reset();
                presentation_alpha = 1.0;
                runtime_frame_ready = false;
                PsyX_Log_Info("Runtime content: %s -> %s\n",
                              *active_content == PresentationContent::gameplay
                                  ? "gameplay"
                                  : "authored-4:3",
                              gpu_frame.content == PresentationContent::gameplay
                                  ? "gameplay"
                                  : "authored-4:3");
                if (gpu_frame.content != PresentationContent::gameplay) {
                  detail::PresentationReplayDrawTarget inherited_page{};
                  if (guest_gpu_.selectPendingPresentationReplayPage(
                          active_display.display_width,
                          active_display.display_height, inherited_page)) {
                    PsyX_Log_Info(
                        "[MReturnTrace][page-handoff] active=%u,%u,%ux%u "
                        "captured=%u,%u,%ux%u\n",
                        static_cast<unsigned int>(active_display.display_x),
                        static_cast<unsigned int>(active_display.display_y),
                        static_cast<unsigned int>(active_display.display_width),
                        static_cast<unsigned int>(
                            active_display.display_height),
                        static_cast<unsigned int>(inherited_page.x),
                        static_cast<unsigned int>(inherited_page.y),
                        static_cast<unsigned int>(inherited_page.width),
                        static_cast<unsigned int>(inherited_page.height));
                    // The outgoing overlay owns this completed page. SHELL
                    // intentionally inherits it without issuing a GP1 flip.
                    active_display.display_x = inherited_page.x;
                    active_display.display_y = inherited_page.y;
                  }
                }
                prepareRuntimeDisplay(active_display, gpu_frame.content);
                presentRuntimeDisplay(active_display);
                visual_dirty = true;
              }
              active_content = gpu_frame.content;

              auto consumed_words = std::size_t{};
              auto validated_word_offset = std::size_t{};
              auto previous_publication_sequence = std::uint64_t{};
              for (const auto &publication : gpu_frame.display_publications) {
                if (publication.word_offset < validated_word_offset ||
                    publication.word_offset > gpu_frame.words.size() ||
                    (previous_publication_sequence != 0U &&
                     publication.sequence <= previous_publication_sequence) ||
                    publication.sequence >
                        gpu_frame.display_publication_sequence) {
                  throw core::Error{
                      core::ErrorCode::invalid_argument,
                      "Runtime GP1 publications are not monotonic"};
                }
                validated_word_offset = publication.word_offset;
                previous_publication_sequence = publication.sequence;
              }

              // Consecutive GP1 states at one GP0 boundary are one atomic
              // control transaction. Retail overlay changes commonly reset
              // and immediately restore the display mode before drawing any
              // pixel. Only the final state can own an observable image;
              // presenting the intermediate reset discards the prepared
              // return-to-base/loading page.
              for (std::size_t publication_index{};
                   publication_index < gpu_frame.display_publications.size();) {
                auto final_index = publication_index;
                while (final_index + 1U <
                           gpu_frame.display_publications.size() &&
                       gpu_frame.display_publications[final_index + 1U]
                               .word_offset ==
                           gpu_frame.display_publications[publication_index]
                               .word_offset) {
                  ++final_index;
                }
                const auto &publication =
                    gpu_frame.display_publications[final_index];
                queue_range(consumed_words, publication.word_offset);
                flush_queued_ranges();
                active_display = stabilize_geometry(publication);
                prepareRuntimeDisplay(active_display, gpu_frame.content);
                presentRuntimeDisplay(active_display);
                visual_dirty = true;
                consumed_words = publication.word_offset;
                publication_index = final_index + 1U;
              }
              if (!gpu_frame.display_publications.empty() &&
                  previous_publication_sequence !=
                      gpu_frame.display_publication_sequence) {
                throw core::Error{core::ErrorCode::invalid_argument,
                                  "Runtime omitted the final GP1 publication"};
              }
              if (observation.publication &&
                  gpu_frame.display_publications.empty()) {
                if (!bootstrap_display) {
                  throw core::Error{
                      core::ErrorCode::invalid_argument,
                      "Runtime omitted a GP1 publication boundary"};
                }
                active_display = final_display;
                prepareRuntimeDisplay(active_display, gpu_frame.content);
                presentRuntimeDisplay(active_display);
                visual_dirty = true;
              }
              if (observation.publication) {
                presentation_clock.publishAuthoredFrame();
                presentation_alpha = 0.0;
              }
              queue_range(consumed_words, gpu_frame.words.size());
              if (requiresIncrementalGpuFlush(gpu_frame.content) &&
                  !pending_gpu_submissions.empty()) {
                // Retail loading/menu code often draws into the currently
                // displayed page without a GP1 flip. Commit such guest-frame
                // updates now so progress bars and return-to-base screens do
                // not wait for the first gameplay publication.
                flush_queued_ranges();
                prepareRuntimeDisplay(active_display, gpu_frame.content);
                presentRuntimeDisplay(active_display);
                visual_dirty = true;
              }
              if (gpu_started != 0U) {
                performance_diagnostics.addGpu(SDL_GetPerformanceCounter() -
                                               gpu_started);
              }
              auto pending_gpu_words = std::size_t{};
              for (const auto &batch : pending_gpu_submissions) {
                pending_gpu_words += batch.end - batch.begin;
              }
              geometry_trace.logFrame(
                  gpu_frame, stable_geometry, active_display, guest_gpu_,
                  geometry_trace_before, 1.0 / 30.0,
                  pending_gpu_submissions.size(), pending_gpu_words);
            }
          }
          if (!runtime_running) {
            PsyX_Log_Info(
                "Runtime cadence: max host interval %.3f ms, dropped wall "
                "debt %.3f ms, remaining backlog %.3f ms, audio resyncs "
                "%llu\n",
                runtime_cadence.maximumElapsedSeconds() * 1'000.0,
                runtime_cadence.droppedSeconds() * 1'000.0,
                runtime_cadence.backlogSeconds() * 1'000.0,
                static_cast<unsigned long long>(
                    runtime_cadence.lateRecoveryCount()));
            break;
          }
        }
        const auto audio_started =
            diagnostic_loop_started != 0U ? SDL_GetPerformanceCounter() : 0U;
        if (runtime_audio) {
          if (runtime_cadence.lateRecoveryStartedForLastAdvance()) {
            runtime_audio->reset("guest-late-recovery");
          }
          if (guest_steps != 0U) {
            const auto drain_mode =
                runtime_cadence.suppressAudioForLastAdvance()
                    ? RuntimeAudioDrainMode::discard
                    : RuntimeAudioDrainMode::queue;
            const auto valid = drainRuntimeAudioFrames(
                std::span<psx::SpuPcmFrame>{runtime_audio_scratch}, drain_mode,
                [this](std::span<psx::SpuPcmFrame> destination) {
                  return audio_(destination);
                },
                [&runtime_audio](std::span<const psx::SpuPcmFrame> frames) {
                  runtime_audio->queue(frames);
                });
            if (!valid) {
              throw core::Error{
                  core::ErrorCode::invalid_argument,
                  "Runtime audio callback exceeded its destination capacity"};
            }
            runtime_audio->flush();
          }
          runtime_audio->update();
          if (diagnostic_loop_started != 0U &&
              runtime_counter_frequency != 0U &&
              runtime_counter - runtime_audio_diagnostic_counter >=
                  runtime_counter_frequency * 2U) {
            runtime_audio->logDiagnostics("runtime");
            runtime_audio_diagnostic_counter = runtime_counter;
          }
        }
        if (audio_started != 0U) {
          performance_diagnostics.addAudio(SDL_GetPerformanceCounter() -
                                           audio_started);
        }
        diagnostic_present_started =
            diagnostic_loop_started != 0U ? SDL_GetPerformanceCounter() : 0U;
        if (active_content &&
            *active_content == PresentationContent::gameplay &&
            runtime_frame_ready && guest_gpu_.presentationReplayReady() &&
            guest_gpu_.presentInterpolatedDisplay(
                static_cast<float>(presentation_alpha))) {
          geometry_trace.notePresentation(
              RuntimePerfPresentation::interpolated,
              static_cast<float>(presentation_alpha));
          if (diagnostic_present_started != 0U) {
            performance_diagnostics.addPresent(SDL_GetPerformanceCounter() -
                                               diagnostic_present_started);
          }
          performance_diagnostics.finishLoop(
              diagnostic_loop_started, RuntimePerfPresentation::interpolated,
              runtime_cadence, guest_gpu_);
          continue;
        }
        const auto presentation_mode =
            runtimeHostPresentationMode(visual_dirty);
        if (presentation_mode == RuntimeHostPresentationMode::cached &&
            runtime_frame_ready && frame_ && PsyX_PresentCachedFrame() != 0) {
          geometry_trace.notePresentation(RuntimePerfPresentation::cached);
          if (diagnostic_present_started != 0U) {
            performance_diagnostics.addPresent(SDL_GetPerformanceCounter() -
                                               diagnostic_present_started);
          }
          performance_diagnostics.finishLoop(diagnostic_loop_started,
                                             RuntimePerfPresentation::cached,
                                             runtime_cadence, guest_gpu_);
          continue;
        }
      }
      static_cast<void>(PsyX_BeginScene());
      DrawSync(0);
      PsyX_EndScene();
      if (frame_) {
        geometry_trace.notePresentation(RuntimePerfPresentation::full);
      }
      if (diagnostic_present_started != 0U) {
        performance_diagnostics.addPresent(SDL_GetPerformanceCounter() -
                                           diagnostic_present_started);
      }
      performance_diagnostics.finishLoop(diagnostic_loop_started,
                                         RuntimePerfPresentation::full,
                                         runtime_cadence, guest_gpu_);
      if (frame_) {
        runtime_frame_ready = true;
      }
    }
  }

private:
  std::vector<char> title_;
  GraphicsSettings graphics_;
  RuntimeFrameCallback frame_;
  KeyboardMouseBindings input_;
  MohUndergroundRuntimeActionBindings runtime_actions_;
  RuntimeAudioDrainCallback audio_;
  detail::PsyCrossGuestGpu guest_gpu_;
};

void uploadTitleNoticeFont() {
  // Keep PsyCross's debug font clear of the title TIMs at x=896..950 and the
  // movie upload rectangle at x=0..319, y=256..495.
  FntLoad(960, 256);
}

void configureTitleNotice() {
  uploadTitleNoticeFont();
  static_cast<void>(FntOpen(54, 70, 252, 154, 2, 256));
}

game::TitleSaveSlots
loadTitleSaveSlots(const std::filesystem::path &path) noexcept {
  const auto loaded = game::loadTitleSaveSlotsFile(path);
  if (loaded.status == game::TitleSaveLoadStatus::invalid) {
    PsyX_Log_Error("Ignoring invalid or unreadable title save file\n");
  } else if (loaded.status == game::TitleSaveLoadStatus::recovered) {
    PsyX_Log_Info("Recovered title save from the last complete backup\n");
  }
  return loaded.slots;
}

bool storeTitleSaveSlots(const std::filesystem::path &path,
                         const game::TitleSaveSlots &slots) noexcept {
  const auto stored = game::storeTitleSaveSlotsFile(path, slots);
  if (!stored) {
    PsyX_Log_Error("Campaign progress could not be persisted\n");
  }
  return stored;
}

enum class SaveStoreDecision {
  stored,
  continue_without_saving,
  return_to_title,
};

std::uint16_t readHostButtons(const PADRAW &pad) noexcept {
  return static_cast<std::uint16_t>(pad.buttons[0]) |
         (static_cast<std::uint16_t>(pad.buttons[1]) << 8U);
}

template <std::size_t Capacity> class FixedMenuHitRegions final {
public:
  void add(int x, int y, int width, int height,
           std::size_t selection) noexcept {
    if (size_ < regions_.size()) {
      regions_[size_++] = detail::MenuHitRegion{x, y, width, height, selection};
    }
  }

  [[nodiscard]] std::span<const detail::MenuHitRegion> span() const noexcept {
    return std::span<const detail::MenuHitRegion>{regions_}.first(size_);
  }

private:
  std::array<detail::MenuHitRegion, Capacity> regions_{};
  std::size_t size_{};
};

FixedMenuHitRegions<6U>
titleMenuHitRegions(const game::TitleMenu &menu,
                    const game::TitleAssets &assets) noexcept {
  FixedMenuHitRegions<6U> regions;
  switch (menu.phase()) {
  case game::TitlePhase::searching:
  case game::TitlePhase::menu: {
    constexpr auto title_layout_width = 384U;
    constexpr auto title_movie_width = 320U;
    const auto scale_x = [](unsigned int value) {
      return static_cast<int>(
          (value * title_movie_width + title_layout_width / 2U) /
          title_layout_width);
    };
    for (std::size_t index{}; index < game::TitleMenu::item_count; ++index) {
      if (!menu.itemEnabled(index)) {
        continue;
      }
      const auto &sprite = assets.sprite(static_cast<game::TitleVisual>(index));
      regions.add(
          scale_x(static_cast<unsigned int>(std::max<int>(sprite.x, 0))),
          std::max<int>(sprite.y, 0), scale_x(sprite.image.displayWidth()),
          sprite.image.displayHeight(), index);
    }
    break;
  }
  case game::TitlePhase::load_slots:
    for (std::size_t index{}; index < game::title_save_slot_count; ++index) {
      regions.add(30, static_cast<int>(52 + index * 27U), 260, 20, index);
    }
    regions.add(30, 187, 260, 20, game::title_save_slot_count);
    break;
  case game::TitlePhase::select_difficulty:
    for (std::size_t index{}; index < game::TitleMenu::difficulty_count;
         ++index) {
      regions.add(42, static_cast<int>(100 + index * 34U), 236, 23, index);
    }
    break;
  case game::TitlePhase::agent_warning:
    regions.add(42, 202, 236, 24, 0U);
    break;
  }
  return regions;
}

FixedMenuHitRegions<5U>
campaignSaveHitRegions(const game::CampaignSaveMenu &menu) noexcept {
  FixedMenuHitRegions<5U> regions;
  switch (menu.phase()) {
  case game::CampaignSavePhase::prompt:
  case game::CampaignSavePhase::overwrite:
    regions.add(66, 101, 58, 17, 0U);
    regions.add(144, 101, 58, 17, 1U);
    break;
  case game::CampaignSavePhase::slots:
    for (std::size_t index{}; index < game::title_save_slot_count; ++index) {
      regions.add(52, static_cast<int>(76 + index * 24U), 165, 17, index);
    }
    break;
  case game::CampaignSavePhase::complete:
    break;
  }
  return regions;
}

KeyboardMouseActionSnapshot
sampleHostKeyboardMouseActions(const KeyboardMouseBindings &bindings,
                               bool mouse_enabled = true) {
  int keyboard_count{};
  const auto *keyboard = SDL_GetKeyboardState(&keyboard_count);
  const auto keyboard_state =
      keyboard != nullptr && keyboard_count > 0
          ? std::span<const std::uint8_t>{keyboard, static_cast<std::size_t>(
                                                        keyboard_count)}
          : std::span<const std::uint8_t>{};
  const auto mouse_buttons = SDL_GetMouseState(nullptr, nullptr);
  return sampleKeyboardMouseActions(
      bindings, KeyboardMouseDeviceState{
                    .keyboard = keyboard_state,
                    .mouse_left = mouse_enabled &&
                                  (mouse_buttons & SDL_BUTTON_LMASK) != 0U,
                    .mouse_right = mouse_enabled &&
                                   (mouse_buttons & SDL_BUTTON_RMASK) != 0U,
                    .mouse_middle = mouse_enabled &&
                                    (mouse_buttons & SDL_BUTTON_MMASK) != 0U,
                    .mouse_x1 = mouse_enabled &&
                                (mouse_buttons & SDL_BUTTON_X1MASK) != 0U,
                    .mouse_x2 = mouse_enabled &&
                                (mouse_buttons & SDL_BUTTON_X2MASK) != 0U,
                    .mouse_wheel_delta = detail::consumePsyCrossMouseWheel(),
                });
}

RuntimePadSample sampleRuntimePadInput(
    const KeyboardMouseBindings &bindings,
    const ControllerButtonBindings &controller_bindings,
    const ControllerButtonBindings &controller_bindings_player_2,
    const MohUndergroundRuntimeActionBindings &runtime_actions,
    detail::RelativeMouseCapture &mouse_capture, bool menu_active) noexcept {
  const auto actions = sampleHostKeyboardMouseActions(bindings, !menu_active);
  RuntimePadSample sample;
  sample.pads[1U].connected = false;
  for (std::size_t slot{}; slot < sample.pads.size(); ++slot) {
    PsyXControllerSnapshot snapshot{};
    if (PsyX_Pad_GetControllerSnapshot(static_cast<int>(slot), &snapshot) ==
            0 ||
        snapshot.connected == 0U) {
      continue;
    }
    auto &physical = sample.pads[slot];
    physical.connected = true;
    physical.active_low_buttons =
        static_cast<std::uint16_t>(snapshot.buttons[0]) |
        (static_cast<std::uint16_t>(snapshot.buttons[1]) << 8U);
    std::copy_n(snapshot.analog, physical.analog.size(),
                physical.analog.begin());
  }
  sample.pads[0U] = applyMohUndergroundControllerBindings(sample.pads[0U],
                                                          controller_bindings);
  sample.pads[1U] = applyMohUndergroundControllerBindings(
      sample.pads[1U], controller_bindings_player_2);
  sample.pads[0U] = mergeMohUndergroundRuntimeInput(sample.pads[0U], actions,
                                                    runtime_actions);
  mouse_capture.set(!menu_active && SDL_GetKeyboardFocus() != nullptr);
  auto mouse_delta_x = int{};
  auto mouse_delta_y = int{};
  if (mouse_capture.enabled()) {
    SDL_GetRelativeMouseState(&mouse_delta_x, &mouse_delta_y);
  }
  sample.mouse_delta_x = static_cast<std::int32_t>(mouse_delta_x);
  sample.mouse_delta_y = static_cast<std::int32_t>(mouse_delta_y);
  sample.mouse_look_active = mouse_capture.enabled();
  return sample;
}
SaveStoreDecision
storeTitleSaveSlotsWithRecovery(const std::filesystem::path &path,
                                const game::TitleSaveSlots &slots, PADRAW &pad,
                                std::uint16_t &previous_buttons,
                                const KeyboardMouseBindings &bindings) {
  if (storeTitleSaveSlots(path, slots)) {
    return SaveStoreDecision::stored;
  }

  configureTitleNotice();
  constexpr std::uint16_t retry_buttons = 0x4000U | 0x08U;
  constexpr std::uint16_t continue_buttons = 0x8000U;
  constexpr std::uint16_t title_buttons = 0x2000U | 0x01U;
  const auto input_name = [&](KeyboardMouseAction action) {
    return std::string{keyboardMouseInputName(bindings[action])};
  };
  const auto notice =
      "       SAVE FAILED\n\n  " + input_name(KeyboardMouseAction::interact) +
      "  Retry\n  " + input_name(KeyboardMouseAction::fire) +
      "  Continue without saving\n  " + input_name(KeyboardMouseAction::pause) +
      "  Return to title\n\n"
      "Campaign progress remains active.";
  auto keyboard_initialized = false;
  auto interact_was_down = false;
  auto fire_was_down = false;
  auto pause_was_down = false;
  for (;;) {
    PsyX_UpdateInput();
    const auto buttons = readHostButtons(pad);
    const auto pressed =
        static_cast<std::uint16_t>(~buttons & previous_buttons);
    previous_buttons = buttons;
    const auto actions = sampleHostKeyboardMouseActions(bindings);
    const auto interact_down = actions[KeyboardMouseAction::interact];
    const auto fire_down = actions[KeyboardMouseAction::fire];
    const auto pause_down = actions[KeyboardMouseAction::pause];
    const auto interact_pressed =
        keyboard_initialized && interact_down && !interact_was_down;
    const auto fire_pressed =
        keyboard_initialized && fire_down && !fire_was_down;
    const auto pause_pressed =
        keyboard_initialized && pause_down && !pause_was_down;
    keyboard_initialized = true;
    interact_was_down = interact_down;
    fire_was_down = fire_down;
    pause_was_down = pause_down;

    if ((pressed & title_buttons) != 0U || pause_pressed) {
      return SaveStoreDecision::return_to_title;
    }
    if ((pressed & continue_buttons) != 0U || fire_pressed) {
      PsyX_Log_Info("Continuing campaign without durable save progress\n");
      return SaveStoreDecision::continue_without_saving;
    }
    if (((pressed & retry_buttons) != 0U || interact_pressed) &&
        storeTitleSaveSlots(path, slots)) {
      return SaveStoreDecision::stored;
    }

    if (PsyX_BeginScene() != 0) {
      char format[] = "%s";
      static_cast<void>(FntPrint(format, notice.c_str()));
      static_cast<void>(FntFlush());
      PsyX_EndScene();
    }
  }
}

ControllerMenuSample
controllerMenuSample(const PsyXControllerSnapshot &snapshot) noexcept;
ControllerMenuSample updateTitleInputPromptBindings(
    detail::PsyCrossCampaignSaveRenderer &renderer,
    const KeyboardMouseBindings &keyboard_bindings,
    const KeyboardMouseActionSnapshot &keyboard_actions,
    InputPromptBindings &active_bindings, int &previous_controller_instance);

game::CampaignSaveResult runCampaignSaveMenu(
    const game::MissionPackage &mission, const game::TitleSaveSlots &slots,
    PADRAW &pad, std::uint16_t &previous_buttons,
    detail::PsyCrossUiAudio &ui_audio, const KeyboardMouseBindings &bindings) {
  // Gameplay already owns the correct 384x240 presentation target. Reuse its
  // original font/ACD renderer instead of switching to the debug-font movie
  // target whose VRAM page has been overwritten by the mission renderer.
  detail::PsyCrossCampaignSaveRenderer renderer{mission, bindings};
  auto active_prompt_bindings = keyboardMouseInputPromptBindings(bindings);
  auto previous_controller_instance = -1;

  PsyX_Log_Info("Campaign save UI entered\n");
  game::CampaignSaveMenu menu;
  ControllerMenuNavigator analog_navigation;
  auto first_frame_presented = false;
  detail::MenuCursor menu_cursor;
  menu_cursor.set(true);
  constexpr std::uint16_t previous_buttons_mask = 0x80U | 0x10U;
  constexpr std::uint16_t next_buttons_mask = 0x20U | 0x40U;
  constexpr std::uint16_t confirm_buttons_mask = 0x4000U | 0x8000U | 0x08U;
  constexpr std::uint16_t cancel_buttons_mask = 0x2000U | 0x01U;
  auto keyboard_initialized = false;
  auto interact_was_down = false;
  auto pause_was_down = false;
  for (;;) {
    PsyX_UpdateInput();
    const auto pointer = menu_cursor.sample(game::PauseMenu::screen_width,
                                            game::PauseMenu::screen_height);
    const auto pointer_regions = campaignSaveHitRegions(menu);
    const auto pointer_hit =
        detail::menuHitTest(pointer, pointer_regions.span());
    ui_audio.update();
    const auto buttons = readHostButtons(pad);
    const auto pressed =
        static_cast<std::uint16_t>(~buttons & previous_buttons);
    previous_buttons = buttons;
    const auto actions = sampleHostKeyboardMouseActions(bindings);
    const auto interact_down = actions[KeyboardMouseAction::interact];
    const auto pause_down = actions[KeyboardMouseAction::pause];
    const auto interact_pressed =
        keyboard_initialized && interact_down && !interact_was_down;
    const auto pause_pressed =
        keyboard_initialized && pause_down && !pause_was_down;
    keyboard_initialized = true;
    interact_was_down = interact_down;
    pause_was_down = pause_down;
    const auto menu_sample = updateTitleInputPromptBindings(
        renderer, bindings, actions, active_prompt_bindings,
        previous_controller_instance);
    const auto analog = analog_navigation.update(menu_sample);
    const game::CampaignSaveInput input{
        .previous = (pressed & previous_buttons_mask) != 0U || analog.previous,
        .next = (pressed & next_buttons_mask) != 0U || analog.next,
        .confirm = (pressed & confirm_buttons_mask) != 0U || interact_pressed ||
                   (pointer.primary_pressed && pointer_hit),
        .cancel = (pressed & cancel_buttons_mask) != 0U || pause_pressed ||
                  pointer.secondary_pressed,
        .pointer_selection =
            pointer.moved ? pointer_hit : std::optional<std::size_t>{},
    };
    const auto previous_phase = menu.phase();
    const auto previous_save_selection = menu.saveSelected();
    const auto previous_overwrite_selection = menu.overwriteSelected();
    const auto previous_slot = menu.slotSelection();
    const auto result = menu.update(input, slots);
    if (input.cancel) {
      ui_audio.play(detail::PsyCrossUiCue::cancel);
    } else if (input.confirm) {
      ui_audio.play(detail::PsyCrossUiCue::confirm);
    } else if (menu.phase() != previous_phase ||
               menu.saveSelected() != previous_save_selection ||
               menu.overwriteSelected() != previous_overwrite_selection ||
               menu.slotSelection() != previous_slot) {
      ui_audio.play(detail::PsyCrossUiCue::navigate);
    }
    if (result.decision != game::CampaignSaveDecision::none) {
      return result;
    }

    // Draw into either a fresh scene or the still-open implicit scene left by
    // the terminal guest packet. Ending that scene before covering it used to
    // swap the old, un-faded gameplay backbuffer to the window for one frame.
    // The ACD renderer starts with an opaque full-screen black tile, so it is
    // also the correct recovery path for an inherited scene.
    static_cast<void>(PsyX_BeginScene());
    renderer.draw(menu, slots);
    PsyX_EndScene();
    if (!first_frame_presented) {
      first_frame_presented = true;
      PsyX_Log_Info("Campaign save UI first frame presented\n");
    }
  }
}

ControllerMenuSample
controllerMenuSample(const PsyXControllerSnapshot &snapshot) noexcept {
  if (snapshot.connected == 0U) {
    return {};
  }
  // Menus must remain usable after either gameplay stick layout is selected.
  // Collapse both physical sticks to the strongest axis; the stateful menu
  // navigator below rejects center noise and emits one edge per gesture.
  const auto axis =
      dominantControllerMenuAxis(snapshot.analog[0], snapshot.analog[1],
                                 snapshot.analog[2], snapshot.analog[3]);
  return ControllerMenuSample{.connected = true,
                              .instance_id = snapshot.instanceId,
                              .horizontal = 128U,
                              .vertical = axis};
}

ControllerMenuSample updateTitleInputPromptBindings(
    detail::PsyCrossCampaignSaveRenderer &renderer,
    const KeyboardMouseBindings &keyboard_bindings,
    const KeyboardMouseActionSnapshot &keyboard_actions,
    InputPromptBindings &active_bindings, int &previous_controller_instance) {
  PsyXControllerSnapshot snapshot{};
  static_cast<void>(PsyX_Pad_GetControllerSnapshot(0, &snapshot));
  const auto menu_sample = controllerMenuSample(snapshot);
  auto keyboard_mouse_activity = false;
  for (const auto held : keyboard_actions.held) {
    keyboard_mouse_activity = keyboard_mouse_activity || held;
  }
  const auto buttons = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(snapshot.buttons[0]) |
      (static_cast<std::uint16_t>(snapshot.buttons[1]) << 8U));
  const auto controller_held = static_cast<std::uint16_t>(~buttons);
  constexpr int prompt_axis_deadzone = 56;
  const auto axis_offset = static_cast<int>(menu_sample.vertical) - 128;
  const auto controller_activity = controller_held != 0U ||
                                   axis_offset < -prompt_axis_deadzone ||
                                   axis_offset > prompt_axis_deadzone;
  const auto controller_identity_changed =
      menu_sample.connected &&
      snapshot.instanceId != previous_controller_instance;
  if (!menu_sample.connected || keyboard_mouse_activity) {
    active_bindings = keyboardMouseInputPromptBindings(keyboard_bindings);
  } else if (controller_activity || controller_identity_changed ||
             active_bindings.device == InputPromptDevice::controller) {
    active_bindings =
        detail::titleControllerInputPromptBindings(snapshot.family);
  }
  renderer.setInputPromptBindings(active_bindings);
  previous_controller_instance =
      menu_sample.connected ? snapshot.instanceId : -1;
  return menu_sample;
}

std::vector<u_long> packWords(std::span<const std::uint16_t> words) {
  std::vector<u_long> packed((words.size() + 1U) / 2U);
  for (std::size_t index = 0; index < words.size(); ++index) {
    const auto shift = static_cast<unsigned int>((index & 1U) * 16U);
    packed[index / 2U] |= static_cast<u_long>(words[index]) << shift;
  }
  return packed;
}

RECT16 blockRect(const assets::TimBlock &block) {
  const auto checked = [](std::uint16_t value) {
    if (value > static_cast<std::uint16_t>(std::numeric_limits<short>::max())) {
      throw core::Error{core::ErrorCode::unsupported,
                        "TIM VRAM coordinate exceeds PsyCross range"};
    }
    return static_cast<short>(value);
  };
  return RECT16{
      checked(block.x),
      checked(block.y),
      checked(block.width_words),
      checked(block.height),
  };
}

void uploadBlock(const assets::TimBlock &block) {
  auto rect = blockRect(block);
  auto packed = packWords(block.words);
  LoadImage(&rect, packed.data());
}

int texturePageMode(assets::TimPixelMode mode) {
  switch (mode) {
  case assets::TimPixelMode::indexed4:
    return 0;
  case assets::TimPixelMode::indexed8:
    return 1;
  case assets::TimPixelMode::direct16:
    return 2;
  case assets::TimPixelMode::direct24:
    return 3;
  }
  return 2;
}

void uploadTitleAssets(const game::TitleAssets &assets) {
  for (const auto &sprite : assets.sprites()) {
    if (sprite.image.clut()) {
      uploadBlock(*sprite.image.clut());
    }
    uploadBlock(sprite.image.pixels());
  }
  DrawSync(0);
}

void drawTitleSprite(const game::TitleSprite &source, std::uint8_t brightness) {
  if (source.image.mode() != assets::TimPixelMode::indexed8 ||
      !source.image.clut()) {
    throw core::Error{core::ErrorCode::unsupported,
                      "Title menu sprites must use an 8-bit indexed TIM"};
  }

  const auto &pixels = source.image.pixels();
  const auto &clut = *source.image.clut();
  constexpr unsigned int texture_page_word_width = 64U;
  constexpr unsigned int texture_page_height = 256U;
  constexpr unsigned int indexed8_pixels_per_word = 2U;
  const auto u = static_cast<unsigned int>(pixels.x % texture_page_word_width) *
                 indexed8_pixels_per_word;
  const auto v = static_cast<unsigned int>(pixels.y % texture_page_height);
  const auto width = static_cast<unsigned int>(source.image.displayWidth());
  const auto height = static_cast<unsigned int>(source.image.displayHeight());
  if (u + width > 256U || v + height > 256U) {
    throw core::Error{core::ErrorCode::unsupported,
                      "Title menu TIM crosses its texture-page boundary"};
  }

  DR_TPAGE page{};
  // TITLE.OVL assigns ABR 1 (background + foreground) to all four sprites.
  // Title sprites are part of the visible 320x240 movie frame. Keep DFE
  // enabled so PsyCross composes them into the active display target, not its
  // VRAM-copy offscreen path.
  SetDrawTPage(&page, 1, 1, GetTPage(1, 1, pixels.x, pixels.y));
  DrawPrim(&page);

  // TITLE.OVL authored these positions for the regular 384x240 display, but
  // MOVIE.OVL presents TITLE.STR in a 320x240 target. A raw SPRT kept the
  // 384-wide coordinates and consequently pushed VIDEO.TIM through the right
  // edge of the 4:3 movie. Reproject the horizontal layout into the active
  // movie canvas; FT4 keeps the complete source image while independently
  // scaling its destination width.
  constexpr auto title_layout_width = 384U;
  constexpr auto title_movie_width = 320U;
  const auto scale_title_x = [](unsigned int value) {
    return (value * title_movie_width + title_layout_width / 2U) /
           title_layout_width;
  };
  const auto destination_x = scale_title_x(
      static_cast<unsigned int>(std::max<std::int16_t>(source.x, 0)));
  const auto destination_width = scale_title_x(width);
  const auto destination_y =
      static_cast<unsigned int>(std::max<std::int16_t>(source.y, 0));

  POLY_FT4 sprite{};
  setPolyFT4(&sprite);
  setSemiTrans(&sprite, 1);
  setRGB0(&sprite, brightness, brightness, brightness);
  sprite.tpage = GetTPage(1, 1, pixels.x, pixels.y);
  sprite.clut = GetClut(clut.x, clut.y);
  setXY4(&sprite, static_cast<float>(destination_x),
         static_cast<float>(destination_y),
         static_cast<float>(destination_x + destination_width),
         static_cast<float>(destination_y), static_cast<float>(destination_x),
         static_cast<float>(destination_y + height),
         static_cast<float>(destination_x + destination_width),
         static_cast<float>(destination_y + height));
  setUV4(&sprite, static_cast<u_char>(u), static_cast<u_char>(v),
         static_cast<u_char>(u + width), static_cast<u_char>(v),
         static_cast<u_char>(u), static_cast<u_char>(v + height),
         static_cast<u_char>(u + width), static_cast<u_char>(v + height));
  DrawPrim(&sprite);
}

class ControllerSettingsPersistence final {
public:
  ControllerSettingsPersistence(
      GraphicsSettings &graphics,
      const ControllerSettingsCommitCallback &callback) noexcept
      : graphics_(graphics), callback_(callback) {}

  [[nodiscard]] bool commit(const ControllerButtonBindings &bindings,
                            bool vibration) noexcept {
    graphics_.controller_bindings = bindings;
    graphics_.controller_vibration = vibration;
    return persistCurrent();
  }

  void retry() noexcept {
    if (!pending_) {
      return;
    }
    if (!persistCurrent()) {
      PsyX_Log_Warning("Controller settings persistence remains pending\n");
    }
  }

private:
  [[nodiscard]] bool persistCurrent() noexcept {
    if (!callback_) {
      pending_ = false;
      return true;
    }
    try {
      pending_ = !callback_(graphics_.controller_bindings,
                            graphics_.controller_vibration);
    } catch (...) {
      pending_ = true;
    }
    return !pending_;
  }

  GraphicsSettings &graphics_;
  const ControllerSettingsCommitCallback &callback_;
  bool pending_{};
};

class PsyCrossTitleHost final : public Host {
public:
  PsyCrossTitleHost(std::string title, game::TitleAssets assets,
                    game::TitleMovies movies,
                    game::MissionPackage initial_mission,
                    std::filesystem::path cue_path,
                    std::string supported_game_serial,
                    GraphicsSettings graphics, KeyboardMouseBindings input,
                    game::RetailCheatState cheats,
                    ControllerSettingsCommitCallback controller_settings_commit)
      : title_(title.begin(), title.end()), assets_(std::move(assets)),
        movies_(std::move(movies)),
        initial_mission_(std::move(initial_mission)),
        cue_path_(std::move(cue_path)),
        supported_game_serial_(std::move(supported_game_serial)),
        graphics_(graphics), input_(input), cheats_(cheats),
        controller_settings_commit_(std::move(controller_settings_commit)) {
    title_.push_back('\0');
  }

  void run() override {
    ControllerSettingsPersistence controller_settings_persistence{
        graphics_, controller_settings_commit_};
    configureGraphics(graphics_);
    configureControllerProtocol(graphics_.controller_protocol);
    configureControllerDevices(graphics_);
    PsyX_Initialise(title_.data(), graphics_.width, graphics_.height, 0);
    configurePresentation(graphics_);
    [[maybe_unused]] detail::PsyCrossWindowMode window_mode{
        graphics_.fullscreen};
    configureInput();
    detail::configurePsyCrossVideoMode(detail::gameplay_video_mode, true);

    uploadTitleAssets(assets_);
    configureTitleNotice();
    const auto save_location =
        game::defaultTitleSaveLocation(cue_path_, supported_game_serial_);
    const auto migration = game::migrateLegacyTitleSaveSlotsFile(save_location);
    if (migration == game::TitleSaveMigrationStatus::migrated) {
      PsyX_Log_Info("Migrated campaign save to the user-data directory\n");
    } else if (migration == game::TitleSaveMigrationStatus::failed) {
      PsyX_Log_Error("Legacy campaign save migration failed\n");
    }
    const auto &save_path = save_location.primary;
    menu_.setSaveSlots(loadTitleSaveSlots(save_path));
    PADRAW pad{};
    PadInitDirect(reinterpret_cast<unsigned char *>(&pad), nullptr);
    PadStartCom();

    std::uint16_t previous_buttons = 0xffffU;
    bool title_keyboard_initialized{};
    bool title_interact_was_down{};
    bool title_pause_was_down{};
    detail::PsyCrossUiAudio ui_audio{cue_path_};
    detail::PsyCrossMoviePlayer movie_player;
    detail::MenuCursor title_cursor;
    auto active_title_prompt_bindings =
        keyboardMouseInputPromptBindings(input_);
    auto previous_title_controller_instance = -1;
    detail::PsyCrossCampaignSaveRenderer title_load_renderer{initial_mission_,
                                                             input_};
    const detail::MovieOverlayCallbacks overlay{
        [this, &pad, &ui_audio, &title_keyboard_initialized,
         &title_interact_was_down, &title_pause_was_down, &title_load_renderer,
         &active_title_prompt_bindings, &previous_title_controller_instance,
         &title_cursor](std::uint16_t pressed, std::uint32_t movie_frame) {
          ui_audio.update();
          title_cursor.set(true);
          const auto pointer = title_cursor.sample(
              game::TitleMenu::screen_width, game::TitleMenu::screen_height);
          const auto pointer_regions = titleMenuHitRegions(menu_, assets_);
          const auto pointer_hit =
              detail::menuHitTest(pointer, pointer_regions.span());
          const auto actions = sampleHostKeyboardMouseActions(input_);
          const auto interact_down = actions[KeyboardMouseAction::interact];
          const auto pause_down = actions[KeyboardMouseAction::pause];
          const auto interact_pressed = title_keyboard_initialized &&
                                        interact_down &&
                                        !title_interact_was_down;
          const auto pause_pressed =
              title_keyboard_initialized && pause_down && !title_pause_was_down;
          title_keyboard_initialized = true;
          title_interact_was_down = interact_down;
          title_pause_was_down = pause_down;
          const auto menu_sample = updateTitleInputPromptBindings(
              title_load_renderer, input_, actions,
              active_title_prompt_bindings, previous_title_controller_instance);
          const auto analog = title_menu_navigation_.update(menu_sample);
          const game::TitleInput input{
              .previous = (pressed & (0x80U | 0x10U)) != 0 || analog.previous,
              .next = (pressed & (0x20U | 0x40U)) != 0 || analog.next,
              .confirm = (pressed & (0x4000U | 0x8000U | 0x08U)) != 0 ||
                         interact_pressed ||
                         (pointer.primary_pressed && pointer_hit),
              .cancel = (pressed & (0x2000U | 0x01U)) != 0 || pause_pressed ||
                        pointer.secondary_pressed,
              .confirm_down = ((~readHostButtons(pad)) &
                               (0x4000U | 0x8000U | 0x08U)) != 0U ||
                              interact_down || pointer.primary_pressed,
              .pointer_selection =
                  pointer.moved ? pointer_hit : std::optional<std::size_t>{},
          };
          const auto previous_selection = menu_.selection();
          const auto previous_phase = menu_.phase();
          const auto previous_slot = menu_.loadSlotSelection();
          const auto previous_difficulty = menu_.difficultySelection();
          const auto command = menu_.update(input, movie_frame);
          if ((previous_phase == game::TitlePhase::load_slots ||
               previous_phase == game::TitlePhase::select_difficulty ||
               previous_phase == game::TitlePhase::agent_warning) &&
              menu_.phase() == game::TitlePhase::menu) {
            // The retail font atlas shares VRAM pages with TITLE.HOG. Restore
            // the title sprites before returning from a font-backed picker.
            uploadTitleAssets(assets_);
          }
          if (input.cancel) {
            ui_audio.play(detail::PsyCrossUiCue::cancel);
          } else if (input.confirm && (command != game::TitleCommand::none ||
                                       menu_.phase() != previous_phase)) {
            ui_audio.play(detail::PsyCrossUiCue::confirm);
          } else if (menu_.selection() != previous_selection ||
                     menu_.loadSlotSelection() != previous_slot ||
                     menu_.difficultySelection() != previous_difficulty) {
            ui_audio.play(detail::PsyCrossUiCue::navigate);
          }
          if (command == game::TitleCommand::exit ||
              command == game::TitleCommand::new_game ||
              command == game::TitleCommand::load_game ||
              command == game::TitleCommand::training_video) {
            selected_command_ = command;
            title_cursor.set(false);
            PsyX_Log_Info("Title command accepted: %s\n",
                          game::titleCommandName(command).data());
            return false;
          }
          return true;
        },
        [this, &title_load_renderer] {
          if (menu_.phase() == game::TitlePhase::load_slots) {
            title_load_renderer.drawLoadSlots(menu_.saveSlots(),
                                              menu_.loadSlotSelection());
          } else if (menu_.phase() == game::TitlePhase::select_difficulty) {
            title_load_renderer.drawDifficultySelection(menu_);
          } else if (menu_.phase() == game::TitlePhase::agent_warning) {
            title_load_renderer.drawAgentModeWarning();
          } else {
            for (std::size_t index = 0; index < game::TitleMenu::visual_count;
                 ++index) {
              const auto visual = static_cast<game::TitleVisual>(index);
              const auto brightness = menu_.brightness(visual);
              if (brightness != 0) {
                drawTitleSprite(assets_.sprite(visual), brightness);
              }
            }
          }
        },
        [this]() -> const game::DiscMovie * {
          if (selected_command_ == game::TitleCommand::new_game &&
              !initial_mission_.openingMovie().path.empty()) {
            return &initial_mission_.openingMovie();
          }
          if (selected_command_ == game::TitleCommand::training_video) {
            return &movies_.trainingMovie();
          }
          return nullptr;
        },
    };
    auto play_startup_movies = true;
    for (;;) {
      selected_command_ = game::TitleCommand::none;
      // Baseline the first live title frame. A stick held through startup,
      // training video, or a return from gameplay must be released before it
      // can move the selection.
      title_menu_navigation_.reset();
      previous_buttons = movie_player.play(movies_, pad, previous_buttons,
                                           overlay, play_startup_movies);
      title_cursor.set(false);
      if (selected_command_ == game::TitleCommand::training_video) {
        menu_.completeSearch();
        play_startup_movies = false;
        continue;
      }
      play_startup_movies = false;

      if (selected_command_ != game::TitleCommand::new_game &&
          selected_command_ != game::TitleCommand::load_game) {
        break;
      }
      auto save_slots = menu_.saveSlots();
      const auto title_played_opening_movie =
          selected_command_ == game::TitleCommand::new_game &&
          !initial_mission_.openingMovie().path.empty();
      std::optional<game::CampaignProgress> campaign;
      auto campaign_carry = std::optional<game::CampaignCarryState>{};
      if (selected_command_ == game::TitleCommand::load_game) {
        campaign = game::CampaignProgress::resume(save_slots,
                                                  menu_.loadSlotSelection());
        if (!campaign) {
          PsyX_Log_Error("Load Game rejected invalid selected slot\n");
          uploadTitleAssets(assets_);
          configureTitleNotice();
          menu_.setSaveSlots(loadTitleSaveSlots(save_path));
          continue;
        }
        PsyX_Log_Info("Load Game: slot=%zu mission=%u\n",
                      *campaign->saveSlot() + 1U, campaign->missionIndex());
        campaign_carry = save_slots[*campaign->saveSlot()].carry;
      } else {
        campaign = game::CampaignProgress::startUnsaved(
            initial_mission_.definition().index, title_played_opening_movie,
            menu_.selectedDifficulty());
        if (!campaign) {
          PsyX_Log_Error("New Game rejected an invalid campaign cursor\n");
          uploadTitleAssets(assets_);
          configureTitleNotice();
          menu_.setSaveSlots(loadTitleSaveSlots(save_path));
          continue;
        }
        PsyX_Log_Info(
            "New Game FMV complete; opening unsaved mission %u "
            "on %s difficulty\n",
            campaign->missionIndex() + 1U,
            game::campaignDifficultyDisplayName(campaign->difficulty()).data());
      }
      controller_settings_persistence.retry();
      detail::PsyCrossMissionStart mission_start;
      const auto commit_controller_settings =
          [&controller_settings_persistence](
              const ControllerButtonBindings &bindings, bool vibration) {
            return controller_settings_persistence.commit(bindings, vibration);
          };
      detail::PsyCrossSceneViewer scene_viewer{input_,
                                               cheats_,
                                               campaign->difficulty(),
                                               graphics_.controller_bindings,
                                               graphics_.controller_vibration,
                                               commit_controller_settings};
      std::optional<game::MissionPackage> loaded_mission;
      auto exit_application = false;
      while (campaign->active()) {
        const auto mission_index = campaign->missionIndex();
        game::MissionPackage *mission{};
        if (mission_index == initial_mission_.definition().index) {
          mission = &initial_mission_;
        } else {
          auto disc = game::GameDisc::open(cue_path_);
          loaded_mission.emplace(
              game::MissionPackage::load(disc, mission_index));
          mission = &*loaded_mission;
        }
        if (campaign->pendingEndingMovieMission()) {
          PsyX_Log_Info("Recovering interrupted EOL for mission %u\n",
                        mission_index + 1U);
          if (!mission->endingMovie().path.empty()) {
            previous_buttons = movie_player.playStandalone(
                mission->endingMovie(), pad, previous_buttons,
                endingMovieSkipPolicy(mission->definition()));
          }
          auto candidate_campaign = *campaign;
          auto candidate_slots = save_slots;
          const auto advance =
              candidate_campaign.completeMission(candidate_slots);
          if (advance == game::CampaignAdvance::invalid) {
            PsyX_Log_Error("Pending EOL transition is inconsistent\n");
            break;
          }
          const auto store_decision = storeTitleSaveSlotsWithRecovery(
              save_path, candidate_slots, pad, previous_buttons, input_);
          if (store_decision == SaveStoreDecision::return_to_title) {
            break;
          }
          *campaign = candidate_campaign;
          save_slots = candidate_slots;
          campaign_carry = campaign->saveSlot()
                               ? save_slots[*campaign->saveSlot()].carry
                               : std::nullopt;
          if (advance == game::CampaignAdvance::campaign_complete) {
            PsyX_Log_Info("Campaign complete\n");
            break;
          }
          continue;
        }
        if (campaign->openingMovieRequired(mission->definition())) {
          previous_buttons = movie_player.playStandalone(
              mission->openingMovie(), pad, previous_buttons);
        }
        campaign->markOpeningMovieHandled();
        // Every campaign entry owns a mission-start loading boundary. DLFs
        // with authored directive text use it verbatim; continuation maps use
        // their catalog title instead of silently skipping the briefing UI.
        previous_buttons = mission_start.run(
            *mission, pad, previous_buttons, input_, campaign_carry,
            campaign->difficulty() == game::CampaignDifficulty::agent);

        const auto &definition = mission->definition();
        std::cout << "Starting mission " << (definition.index + 1U) << ": "
                  << definition.title << " [" << definition.resource_name
                  << "]\nMounted " << mission->archive().entries().size()
                  << " FOG files, " << mission->textureFileCount()
                  << " textures and " << mission->worldModelCount()
                  << " world models\n";
        const auto scene_result =
            scene_viewer.run(*mission, pad, previous_buttons, cue_path_,
                             campaign->maximumUnlockedMission(),
                             mission_start.takePreloadedGameplay(),
                             mission_start.takePreloadedAudio());
        controller_settings_persistence.retry();
        previous_buttons = scene_result.previous_buttons;
        if (scene_result.reason == detail::SceneExitReason::mission_selected &&
            scene_result.selected_mission) {
          const auto selected = *scene_result.selected_mission;
          if (!campaign->selectUnlockedMission(selected)) {
            // Reaching this branch requires the explicit all-missions cheat;
            // normal mission selection can never move beyond the high-water
            // mark of the loaded save.
            auto replacement = game::CampaignProgress::startUnsaved(
                selected, false, campaign->difficulty());
            if (!replacement) {
              PsyX_Log_Error("Pause mission selection rejected mission %u\n",
                             selected + 1U);
              break;
            }
            campaign = std::move(replacement);
          }
          campaign_carry.reset();
          loaded_mission.reset();
          PsyX_Log_Info("Pause mission selection: mission=%u\n", selected + 1U);
          continue;
        }
        if (scene_result.reason == detail::SceneExitReason::mission_complete) {
          const auto next_mission = mission_index + 1U;
          const auto carry_for_next =
              game::campaignMissionsShareCarry(mission_index, next_mission)
                  ? scene_result.carry
                  : std::nullopt;
          if (game::campaignMissionsShareCarry(mission_index, next_mission) &&
              !carry_for_next) {
            // A terminal overlay can retire its live inventory table before
            // the native host observes the EOL request. The scene viewer
            // normally supplies its last coherent snapshot; if even that is
            // unavailable, continuing with mission defaults is safer than
            // tearing down the entire campaign and returning to title.
            PsyX_Log_Warning(
                "Campaign transition has no coherent player carry; "
                "continuing with mission defaults\n");
          }
          const auto save_result = runCampaignSaveMenu(
              *mission, save_slots, pad, previous_buttons, ui_audio, input_);
          auto completion_is_saved = false;
          if (save_result.decision == game::CampaignSaveDecision::save &&
              save_result.slot) {
            auto staged_campaign = *campaign;
            auto staged_slots = save_slots;
            if (!staged_campaign.stageMissionCompletionInSlot(
                    staged_slots, *save_result.slot, carry_for_next)) {
              PsyX_Log_Error("Campaign save transaction rejected state\n");
              break;
            }
            // The selected slot remains on this mission with a pending EOL.
            // A shutdown during the following movie therefore resumes the
            // exact retail handoff instead of skipping it.
            const auto store_decision = storeTitleSaveSlotsWithRecovery(
                save_path, staged_slots, pad, previous_buttons, input_);
            if (store_decision == SaveStoreDecision::return_to_title) {
              break;
            }
            if (store_decision == SaveStoreDecision::stored) {
              *campaign = staged_campaign;
              save_slots = staged_slots;
              completion_is_saved = true;
            }
          }
          if (!mission->endingMovie().path.empty()) {
            previous_buttons = movie_player.playStandalone(
                mission->endingMovie(), pad, previous_buttons,
                endingMovieSkipPolicy(mission->definition()));
          }

          auto candidate_campaign = *campaign;
          auto candidate_slots = save_slots;
          const auto advance =
              completion_is_saved
                  ? candidate_campaign.completeMission(candidate_slots)
                  : candidate_campaign.completeMissionWithoutSaving();
          if (advance == game::CampaignAdvance::invalid) {
            PsyX_Log_Error("Campaign transition rejected inconsistent state\n");
            break;
          }
          if (completion_is_saved) {
            const auto store_decision = storeTitleSaveSlotsWithRecovery(
                save_path, candidate_slots, pad, previous_buttons, input_);
            if (store_decision == SaveStoreDecision::return_to_title) {
              break;
            }
          }
          *campaign = candidate_campaign;
          save_slots = candidate_slots;
          campaign_carry = advance == game::CampaignAdvance::next_mission
                               ? carry_for_next
                               : std::nullopt;
          if (advance == game::CampaignAdvance::campaign_complete) {
            PsyX_Log_Info("Campaign complete\n");
            break;
          }
          continue;
        }
        exit_application =
            scene_result.reason == detail::SceneExitReason::exit_application;
        break;
      }
      if (exit_application) {
        break;
      }
      uploadTitleAssets(assets_);
      configureTitleNotice();
      menu_.completeSearch();
      menu_.setSaveSlots(loadTitleSaveSlots(save_path));
    }
    PadStopCom();
  }

private:
  std::vector<char> title_;
  game::TitleAssets assets_;
  game::TitleMovies movies_;
  game::MissionPackage initial_mission_;
  std::filesystem::path cue_path_;
  std::string supported_game_serial_;
  game::TitleMenu menu_;
  game::TitleCommand selected_command_{game::TitleCommand::none};
  ControllerMenuNavigator title_menu_navigation_;
  GraphicsSettings graphics_;
  KeyboardMouseBindings input_;
  game::RetailCheatState cheats_;
  ControllerSettingsCommitCallback controller_settings_commit_;
};

class PsyCrossSceneHost final : public Host {
public:
  PsyCrossSceneHost(std::string title, game::MissionPackage mission,
                    std::filesystem::path cue_path, GraphicsSettings graphics,
                    KeyboardMouseBindings input, game::RetailCheatState cheats,
                    ControllerSettingsCommitCallback controller_settings_commit)
      : title_(title.begin(), title.end()), mission_(std::move(mission)),
        cue_path_(std::move(cue_path)), graphics_(graphics), input_(input),
        cheats_(cheats),
        controller_settings_commit_(std::move(controller_settings_commit)) {
    title_.push_back('\0');
  }

  void run() override {
    ControllerSettingsPersistence controller_settings_persistence{
        graphics_, controller_settings_commit_};
    configureGraphics(graphics_);
    configureControllerProtocol(graphics_.controller_protocol);
    configureControllerDevices(graphics_);
    PsyX_Initialise(title_.data(), graphics_.width, graphics_.height, 0);
    configurePresentation(graphics_);
    [[maybe_unused]] detail::PsyCrossWindowMode window_mode{
        graphics_.fullscreen};
    configureInput();
    detail::configurePsyCrossVideoMode(detail::gameplay_video_mode, true);

    PADRAW pad{};
    PadInitDirect(reinterpret_cast<unsigned char *>(&pad), nullptr);
    PadStartCom();
    detail::PsyCrossMoviePlayer movie_player;
    auto previous_buttons = std::uint16_t{0xffffU};
    if (!mission_.openingMovie().path.empty()) {
      previous_buttons = movie_player.playStandalone(mission_.openingMovie(),
                                                     pad, previous_buttons);
    }
    detail::PsyCrossMissionStart mission_start;
    previous_buttons =
        mission_start.run(mission_, pad, previous_buttons, input_);
    controller_settings_persistence.retry();
    const auto commit_controller_settings =
        [&controller_settings_persistence](
            const ControllerButtonBindings &bindings, bool vibration) {
          return controller_settings_persistence.commit(bindings, vibration);
        };
    detail::PsyCrossSceneViewer scene_viewer{input_,
                                             cheats_,
                                             game::CampaignDifficulty::original,
                                             graphics_.controller_bindings,
                                             graphics_.controller_vibration,
                                             commit_controller_settings};
    const auto result = scene_viewer.run(mission_, pad, previous_buttons,
                                         cue_path_, mission_.definition().index,
                                         mission_start.takePreloadedGameplay(),
                                         mission_start.takePreloadedAudio());
    controller_settings_persistence.retry();
    if (result.reason == detail::SceneExitReason::mission_complete &&
        !mission_.endingMovie().path.empty()) {
      static_cast<void>(movie_player.playStandalone(
          mission_.endingMovie(), pad, result.previous_buttons,
          endingMovieSkipPolicy(mission_.definition())));
    }
    PadStopCom();
  }

private:
  std::vector<char> title_;
  game::MissionPackage mission_;
  std::filesystem::path cue_path_;
  GraphicsSettings graphics_;
  KeyboardMouseBindings input_;
  game::RetailCheatState cheats_;
  ControllerSettingsCommitCallback controller_settings_commit_;
};

} // namespace

void logRuntimeCpuAccelerationDiagnostics(
    std::uint64_t frames, std::uint64_t fast_forwards,
    std::uint64_t skipped_ticks) noexcept {
  const auto skipped_per_frame =
      frames != 0U ? static_cast<double>(skipped_ticks) / frames : 0.0;
  PsyX_Log_Info("[PerfDiag][cpu-accel] frames=%llu fast_forwards=%llu "
                "skipped_ticks=%llu skipped_per_frame=%.1f\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(fast_forwards),
                static_cast<unsigned long long>(skipped_ticks),
                skipped_per_frame);
}

void logRuntimeGuestCpuDiagnostics(
    std::uint64_t frames, std::uint64_t average_microseconds,
    std::uint64_t maximum_microseconds, std::uint64_t instructions_per_frame,
    std::uint64_t fast_fallbacks, std::uint32_t guest_pc,
    std::uint32_t guest_ra, const psx::BiosHleState &bios_state,
    const RuntimeGuestCardDiagnostics &card,
    std::uint64_t machine_tick) noexcept {
  const auto total_instructions = instructions_per_frame * frames;
  const auto fast_coverage =
      total_instructions != 0U
          ? 100.0 *
                static_cast<double>(
                    total_instructions -
                    std::min(fast_fallbacks, total_instructions)) /
                static_cast<double>(total_instructions)
          : 0.0;
  PsyX_Log_Info("[PerfDiag][guest-cpu] frames=%llu avg_us=%llu max_us=%llu "
                "instructions_per_frame=%llu fast=%.1f%% fallback=%llu "
                "scale_percent=100 pc=%08X ra=%08X tick=%llu\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(average_microseconds),
                static_cast<unsigned long long>(maximum_microseconds),
                static_cast<unsigned long long>(instructions_per_frame),
                fast_coverage, static_cast<unsigned long long>(fast_fallbacks),
                guest_pc, guest_ra,
                static_cast<unsigned long long>(machine_tick));
  PsyX_Log_Info(
      "[CardDiag] init=%u/%u/%u op=%u status=%08X channel=%08X "
      "submit=%llu complete=%llu event=%08X/%08X callbacks=%llu/%llu "
      "guest=%u/%u/%u ports=%08X channel=%08X stack=%08X cb=%08X "
      "ticks=%u done_cb=%08X sw=%u,%u,%u,%u hw=%u,%u,%u,%u\n",
      static_cast<unsigned>(bios_state.memory_card_initialized),
      static_cast<unsigned>(bios_state.memory_card_started),
      static_cast<unsigned>(bios_state.memory_card_filesystem_initialized),
      static_cast<unsigned>(bios_state.memory_card_operation),
      bios_state.memory_card_status, bios_state.memory_card_channel,
      static_cast<unsigned long long>(bios_state.memory_card_submissions),
      static_cast<unsigned long long>(bios_state.memory_card_completions),
      bios_state.memory_card_last_event_class,
      bios_state.memory_card_last_event_spec,
      static_cast<unsigned long long>(bios_state.event_callbacks_started),
      static_cast<unsigned long long>(bios_state.event_callbacks_completed),
      card.task, card.result, card.done, card.ports, card.channel,
      card.task_stack_depth, card.vblank_callback, card.update_ticks,
      card.completion_callback, card.software_events[0U],
      card.software_events[1U], card.software_events[2U],
      card.software_events[3U], card.hardware_events[0U],
      card.hardware_events[1U], card.hardware_events[2U],
      card.hardware_events[3U]);
  PsyX_Log_Info("[CardDiag][events] sw_handle=%08X,%08X,%08X,%08X "
                "hw_handle=%08X,%08X,%08X,%08X\n",
                card.software_handles[0U], card.software_handles[1U],
                card.software_handles[2U], card.software_handles[3U],
                card.hardware_handles[0U], card.hardware_handles[1U],
                card.hardware_handles[2U], card.hardware_handles[3U]);
  PsyX_Log_Info("[CardDiag][stack] "
                "0=%08X:%08X,%08X,%08X,%08X "
                "1=%08X:%08X,%08X,%08X,%08X "
                "2=%08X:%08X,%08X,%08X,%08X "
                "3=%08X:%08X,%08X,%08X,%08X\n",
                card.stack_callbacks[0U], card.stack_frames[0U][0U],
                card.stack_frames[0U][1U], card.stack_frames[0U][2U],
                card.stack_frames[0U][3U], card.stack_callbacks[1U],
                card.stack_frames[1U][0U], card.stack_frames[1U][1U],
                card.stack_frames[1U][2U], card.stack_frames[1U][3U],
                card.stack_callbacks[2U], card.stack_frames[2U][0U],
                card.stack_frames[2U][1U], card.stack_frames[2U][2U],
                card.stack_frames[2U][3U], card.stack_callbacks[3U],
                card.stack_frames[3U][0U], card.stack_frames[3U][1U],
                card.stack_frames[3U][2U], card.stack_frames[3U][3U]);
}

void logRuntimeExactTransformDiagnostics(
    std::uint64_t captures, std::uint64_t publications,
    std::uint64_t rejections, std::uint64_t composition_entries,
    std::uint64_t composition_captures, std::uint64_t composition_sites,
    std::uint64_t composition_publications,
    std::span<const psx::GteProjectedVertex> projections) noexcept {
  std::uint64_t valid{};
  std::uint64_t exact{};
  std::uint64_t enhanced_rotation{};
  std::uint64_t enhanced_translation{};
  std::uint64_t enhanced_vector{};
  for (const auto &projection : projections) {
    if (!projection.valid)
      continue;
    ++valid;
    exact += projection.exact_transform ? 1U : 0U;
    enhanced_rotation += (projection.enhanced_sources &
                          psx::GteProjectedVertex::enhanced_rotation) != 0U;
    enhanced_translation +=
        (projection.enhanced_sources &
         psx::GteProjectedVertex::enhanced_translation) != 0U;
    enhanced_vector += (projection.enhanced_sources &
                        psx::GteProjectedVertex::enhanced_vector) != 0U;
  }
  PsyX_Log_Info(
      "[PerfDiag][exact-transform] capture/publish/reject=%llu/%llu/%llu "
      "compose(entry/capture/site/publish)=%llu/%llu/%llu/%llu "
      "catalog(valid/exact/enhanced_r/t/v)=%llu/%llu/%llu/%llu/%llu\n",
      static_cast<unsigned long long>(captures),
      static_cast<unsigned long long>(publications),
      static_cast<unsigned long long>(rejections),
      static_cast<unsigned long long>(composition_entries),
      static_cast<unsigned long long>(composition_captures),
      static_cast<unsigned long long>(composition_sites),
      static_cast<unsigned long long>(composition_publications),
      static_cast<unsigned long long>(valid),
      static_cast<unsigned long long>(exact),
      static_cast<unsigned long long>(enhanced_rotation),
      static_cast<unsigned long long>(enhanced_translation),
      static_cast<unsigned long long>(enhanced_vector));
}

void logRuntimeSpuDiagnostics(std::span<const psx::SpuDiagnosticEvent> events,
                              std::uint64_t dropped_events) noexcept {
  for (const auto &event : events) {
    switch (event.type) {
    case psx::SpuDiagnosticType::dma_write:
    case psx::SpuDiagnosticType::dma_read:
      PsyX_Log_Info(
          "[SpuTrace][dma] seq=%llu frame=%llu dir=%s ram=%06X "
          "spu=%05X..%05X words=%u/%u hash=%08X\n",
          static_cast<unsigned long long>(event.sequence),
          static_cast<unsigned long long>(event.mixed_frame),
          event.type == psx::SpuDiagnosticType::dma_write ? "ram->spu"
                                                          : "spu->ram",
          event.ram_address, event.start_address, event.end_address,
          event.word_count, event.expected_word_count, event.fingerprint);
      break;
    case psx::SpuDiagnosticType::key_on:
      PsyX_Log_Info(
          "[SpuTrace][kon] seq=%llu frame=%llu pc=%08X voice=%u "
          "mask=%06X start=%05X repeat=%05X pitch=%04X vol=%04X/%04X "
          "adsr=%04X/%04X adpcm=%02X/%02X hash=%08X\n",
          static_cast<unsigned long long>(event.sequence),
          static_cast<unsigned long long>(event.mixed_frame), event.producer_pc,
          static_cast<unsigned int>(event.voice), event.mask,
          event.start_address, event.repeat_address, event.pitch,
          event.volume_left, event.volume_right, event.adsr_low,
          event.adsr_high, static_cast<unsigned int>(event.adpcm_header),
          static_cast<unsigned int>(event.adpcm_flags), event.fingerprint);
      break;
    case psx::SpuDiagnosticType::key_off:
      PsyX_Log_Info("[SpuTrace][koff] seq=%llu frame=%llu pc=%08X mask=%06X\n",
                    static_cast<unsigned long long>(event.sequence),
                    static_cast<unsigned long long>(event.mixed_frame),
                    event.producer_pc, event.mask);
      break;
    }
  }

  static std::uint64_t reported_drops{};
  if (dropped_events != reported_drops) {
    PsyX_Log_Warning("[SpuTrace][overflow] dropped=%llu\n",
                     static_cast<unsigned long long>(dropped_events));
    reported_drops = dropped_events;
  }
}

std::unique_ptr<Host>
createPsyCrossRuntimeHost(std::string title, RuntimeFrameCallback frame,
                          GraphicsSettings graphics,
                          KeyboardMouseBindings input,
                          MohUndergroundRuntimeActionBindings runtime_actions,
                          RuntimeAudioDrainCallback audio) {
  return std::make_unique<PsyCrossHost>(
      std::move(title), graphics, std::move(frame), std::move(input),
      std::move(runtime_actions), std::move(audio));
}

std::unique_ptr<Host> createPsyCrossHost(std::string title,
                                         GraphicsSettings graphics) {
  return std::make_unique<PsyCrossHost>(std::move(title), graphics);
}

std::unique_ptr<Host> createPsyCrossTitleHost(
    std::string title, game::TitleAssets assets, game::TitleMovies movies,
    game::MissionPackage initial_mission, std::filesystem::path cue_path,
    std::string supported_game_serial, GraphicsSettings graphics,
    KeyboardMouseBindings input, game::RetailCheatState cheats,
    ControllerSettingsCommitCallback controller_settings_commit) {
  return std::make_unique<PsyCrossTitleHost>(
      std::move(title), std::move(assets), std::move(movies),
      std::move(initial_mission), std::move(cue_path),
      std::move(supported_game_serial), graphics, input, cheats,
      std::move(controller_settings_commit));
}

std::unique_ptr<Host> createPsyCrossSceneHost(
    std::string title, game::MissionPackage mission,
    std::filesystem::path cue_path, GraphicsSettings graphics,
    KeyboardMouseBindings input, game::RetailCheatState cheats,
    ControllerSettingsCommitCallback controller_settings_commit) {
  return std::make_unique<PsyCrossSceneHost>(
      std::move(title), std::move(mission), std::move(cue_path), graphics,
      input, cheats, std::move(controller_settings_commit));
}

} // namespace sf::platform
