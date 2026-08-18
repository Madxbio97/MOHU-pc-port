#include "launcher.hpp"

#include "mohu/frontend_menu.hpp"
#include "mohu/runtime.hpp"

#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/localization.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/title.hpp"
#include "sf/platform/host.hpp"

#include <SDL.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

bool environmentFlagEnabled(const char *name,
                            bool enabled_by_default = false) noexcept {
  const auto *value = SDL_getenv(name);
  return value == nullptr || value[0] == '\0' ? enabled_by_default
                                              : std::strcmp(value, "0") != 0;
}

void logSpuDiagnostics(mohu::Runtime &runtime) noexcept {
  std::array<sf::psx::SpuDiagnosticEvent, 128U> events{};
  while (const auto count = runtime.takeSpuDiagnostics(events)) {
    sf::platform::logRuntimeSpuDiagnostics(
        std::span<const sf::psx::SpuDiagnosticEvent>{events}.first(count),
        runtime.droppedSpuDiagnostics());
  }
}

std::optional<int> parseInteger(std::string_view text) {
  int value{};
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

bool parseResolution(std::string_view text,
                     sf::platform::GraphicsSettings &graphics) {
  const auto separator = text.find_first_of("xX");
  if (separator == std::string_view::npos) {
    return false;
  }
  const auto width = parseInteger(text.substr(0, separator));
  const auto height = parseInteger(text.substr(separator + 1));
  if (!width || !height || *width < 320 || *height < 240) {
    return false;
  }
  graphics.width = *width;
  graphics.height = *height;
  return true;
}

enum class LaunchMode {
  game,
  title_test,
  scene_test,
  platform_test,
  verify_only,
};

struct LaunchRequest {
  LaunchMode mode{LaunchMode::game};
  std::filesystem::path cue_path;
};

std::optional<LaunchRequest>
parseLaunchRequest(const std::vector<std::string_view> &arguments) {
  if (arguments.empty()) {
    return LaunchRequest{};
  }
  if (arguments.size() == 1U && !arguments.front().starts_with("--")) {
    return LaunchRequest{LaunchMode::game,
                         std::filesystem::path{arguments.front()}};
  }
  if (arguments.size() != 2U || arguments[1].starts_with("--")) {
    return std::nullopt;
  }

  if (arguments[0] == "--game") {
    return LaunchRequest{LaunchMode::game, std::filesystem::path{arguments[1]}};
  }
  if (arguments[0] == "--title-test") {
    return LaunchRequest{LaunchMode::title_test,
                         std::filesystem::path{arguments[1]}};
  }
  if (arguments[0] == "--scene-test") {
    return LaunchRequest{LaunchMode::scene_test,
                         std::filesystem::path{arguments[1]}};
  }
  if (arguments[0] == "--platform-test") {
    return LaunchRequest{LaunchMode::platform_test,
                         std::filesystem::path{arguments[1]}};
  }
  if (arguments[0] == "--verify" || arguments[0] == "--verify-only") {
    return LaunchRequest{LaunchMode::verify_only,
                         std::filesystem::path{arguments[1]}};
  }
  return std::nullopt;
}

bool supportsMissionSelection(LaunchMode mode) noexcept {
  return mode == LaunchMode::game || mode == LaunchMode::title_test ||
         mode == LaunchMode::scene_test;
}

void printUsage() {
  std::cerr
      << "Usage:\n"
      << "  medal_of_honor_underground\n"
      << "  medal_of_honor_underground [graphics options] --game "
         "<game.bin|game.cue>\n"
      << "  medal_of_honor_underground [graphics options] <game.bin|game.cue>\n"
      << "Development modes:\n"
      << "  medal_of_honor_underground [graphics options] --platform-test "
         "<game.bin|game.cue>\n"
      << "  medal_of_honor_underground --no-launcher --verify-only "
         "<game.bin|game.cue>\n"
      << "Gameplay test option: --all-weapons-test\n"
      << "Graphics options: --fullscreen --windowed --no-launcher "
         "--resolution=WIDTHxHEIGHT "
         "--filter=nearest|bilinear|trilinear|anisotropic "
         "--aa=off|smaa|fxaa "
         "--aspect-adaptive --aspect-4-3 "
         "--vsync --no-vsync --fps-limit=0|20..1000\n"
      << "Legacy filtering/AA aliases remain accepted. The selected output "
         "resolution is also the retail guest rendering resolution.\n"
      << "Controller options: "
         "--controller-backend=auto|xinput|dinput|rawinput\n"
      << "Input options: --mouse-sensitivity=25..400\n";
  std::cerr << "Language options: --language=en --language=ru\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    sf::platform::GraphicsSettings graphics;
    sf::game::RetailCheatState retail_cheats;
    auto input = sf::platform::defaultKeyboardMouseBindings();
    auto runtime_actions =
        sf::platform::defaultMohUndergroundRuntimeActionBindings();
    auto language = sf::game::GameLanguage::english;
    std::error_code executable_path_error;
    const auto executable_path = std::filesystem::absolute(
        std::filesystem::path{argv[0]}, executable_path_error);
    const auto executable_directory = executable_path_error
                                          ? std::filesystem::current_path()
                                          : executable_path.parent_path();
    sf::game::setLocalizationRoot(executable_directory / "locales" / "ru-vit");
    sf::platform::loadLauncherSettings(graphics, input, runtime_actions,
                                       language);
    bool show_launcher = true;
    std::optional<std::uint32_t> requested_mission;
    std::vector<std::string_view> arguments;
    if (argc > 1) {
      arguments.reserve(static_cast<std::size_t>(argc - 1));
    }
    for (int index = 1; index < argc; ++index) {
      const std::string_view argument{argv[index]};
      if (argument == "--fullscreen") {
        graphics.fullscreen = true;
      } else if (argument == "--windowed") {
        graphics.fullscreen = false;
      } else if (argument == "--all-weapons-test") {
        retail_cheats.all_weapons = true;
      } else if (argument == "--no-launcher") {
        show_launcher = false;
      } else if (argument == "--filter=nearest") {
        sf::platform::setTextureFilteringMode(
            graphics, sf::platform::TextureFilteringMode::nearest);
      } else if (argument == "--filter=bilinear") {
        sf::platform::setTextureFilteringMode(
            graphics, sf::platform::TextureFilteringMode::bilinear);
      } else if (argument == "--filter=trilinear") {
        sf::platform::setTextureFilteringMode(
            graphics, sf::platform::TextureFilteringMode::trilinear);
      } else if (argument == "--filter=anisotropic") {
        sf::platform::setTextureFilteringMode(
            graphics, sf::platform::TextureFilteringMode::anisotropic);
      } else if (argument.starts_with("--filter=")) {
        printUsage();
        return 64;
      } else if (argument == "--bilinear") {
        sf::platform::setTextureFilteringMode(
            graphics, sf::platform::TextureFilteringMode::bilinear);
      } else if (argument == "--nearest") {
        sf::platform::setTextureFilteringMode(
            graphics, sf::platform::TextureFilteringMode::nearest);
      } else if (argument == "--trilinear") {
        sf::platform::setTextureFilteringMode(
            graphics, sf::platform::TextureFilteringMode::trilinear);
      } else if (argument == "--no-trilinear") {
        graphics.bilinear_filtering = true;
        graphics.trilinear_filtering = false;
        graphics.anisotropic_filtering = false;
      } else if (argument == "--anisotropic") {
        sf::platform::setTextureFilteringMode(
            graphics, sf::platform::TextureFilteringMode::anisotropic);
      } else if (argument == "--no-anisotropic") {
        graphics.anisotropic_filtering = false;
      } else if (argument == "--aa=smaa") {
        sf::platform::setAntialiasingMode(graphics,
                                          sf::platform::AntialiasingMode::smaa);
      } else if (argument == "--aa=fxaa") {
        sf::platform::setAntialiasingMode(graphics,
                                          sf::platform::AntialiasingMode::fxaa);
      } else if (argument == "--aa=off" || argument == "--aa=none") {
        sf::platform::setAntialiasingMode(
            graphics, sf::platform::AntialiasingMode::disabled);
      } else if (argument.starts_with("--aa=")) {
        printUsage();
        return 64;
      } else if (argument == "--smaa") {
        sf::platform::setAntialiasingMode(graphics,
                                          sf::platform::AntialiasingMode::smaa);
      } else if (argument == "--no-smaa") {
        sf::platform::setAntialiasingMode(
            graphics, sf::platform::AntialiasingMode::disabled);
      } else if (argument == "--fxaa") {
        sf::platform::setAntialiasingMode(graphics,
                                          sf::platform::AntialiasingMode::fxaa);
      } else if (argument == "--no-fxaa") {
        sf::platform::setAntialiasingMode(
            graphics, sf::platform::AntialiasingMode::disabled);
      } else if (argument == "--volumetric-fog") {
        graphics.volumetric_fog = true;
      } else if (argument == "--no-volumetric-fog") {
        graphics.volumetric_fog = false;
      } else if (argument == "--volumetric-effects") {
        graphics.volumetric_effects = true;
      } else if (argument == "--no-volumetric-effects") {
        graphics.volumetric_effects = false;
      } else if (argument == "--vsync") {
        graphics.vsync = true;
      } else if (argument == "--no-vsync") {
        graphics.vsync = false;
      } else if (argument == "--controller-backend=auto") {
        graphics.controller_protocol =
            sf::platform::ControllerProtocol::automatic;
      } else if (argument == "--controller-backend=xinput") {
        graphics.controller_protocol = sf::platform::ControllerProtocol::xinput;
      } else if (argument == "--controller-backend=dinput" ||
                 argument == "--controller-backend=directinput") {
        graphics.controller_protocol =
            sf::platform::ControllerProtocol::direct_input;
      } else if (argument == "--controller-backend=rawinput" ||
                 argument == "--controller-backend=raw") {
        graphics.controller_protocol =
            sf::platform::ControllerProtocol::raw_input;
      } else if (argument.starts_with("--controller-backend=")) {
        printUsage();
        return 64;
      } else if (argument.starts_with("--mouse-sensitivity=")) {
        const auto sensitivity = parseInteger(
            argument.substr(std::string_view{"--mouse-sensitivity="}.size()));
        if (!sensitivity || *sensitivity < 25 || *sensitivity > 400) {
          printUsage();
          return 64;
        }
        graphics.mouse_sensitivity_percent =
            static_cast<std::uint32_t>(*sensitivity);
      } else if (argument.starts_with("--fps-limit=")) {
        const auto limit = parseInteger(
            argument.substr(std::string_view{"--fps-limit="}.size()));
        if (!limit || (*limit != 0 && (*limit < 20 || *limit > 1000))) {
          printUsage();
          return 64;
        }
        graphics.frame_limit = static_cast<std::uint32_t>(*limit);
      } else if (argument == "--language=en" || argument == "--locale=en") {
        language = sf::game::GameLanguage::english;
      } else if (argument == "--language=ru" || argument == "--locale=ru") {
        language = sf::game::GameLanguage::russian_vit;
      } else if (argument == "--widescreen" || argument == "--aspect-auto" ||
                 argument == "--aspect-adaptive") {
        graphics.aspect_ratio = sf::platform::AspectRatioMode::adaptive;
      } else if (argument == "--aspect-4-3") {
        graphics.aspect_ratio = sf::platform::AspectRatioMode::original_4_3;
      } else if (argument.starts_with("--resolution=")) {
        if (!parseResolution(
                argument.substr(std::string_view{"--resolution="}.size()),
                graphics)) {
          printUsage();
          return 64;
        }
      } else if (argument.starts_with("--msaa=")) {
        const auto samples =
            parseInteger(argument.substr(std::string_view{"--msaa="}.size()));
        if (!samples || (*samples != 0 && *samples != 2 && *samples != 4 &&
                         *samples != 8)) {
          printUsage();
          return 64;
        }
        graphics.msaa_samples = *samples;
        graphics.smaa = false;
        graphics.fxaa = false;
      } else if (argument.starts_with("--mission=") ||
                 argument.starts_with("--level=")) {
        const auto separator = argument.find('=');
        const auto mission_number =
            parseInteger(argument.substr(separator + 1U));
        const auto mission_count = sf::game::missionCatalog().size();
        if (!mission_number || *mission_number < 1 ||
            static_cast<std::size_t>(*mission_number) > mission_count) {
          std::cerr << "Mission must be a retail number from 1 to "
                    << mission_count << ".\n";
          printUsage();
          return 64;
        }
        const auto mission_index =
            static_cast<std::uint32_t>(*mission_number - 1);
        if (requested_mission && *requested_mission != mission_index) {
          std::cerr << "Conflicting --mission/--level selections.\n";
          printUsage();
          return 64;
        }
        requested_mission = mission_index;
      } else if (argument == "--mission" || argument == "--level") {
        std::cerr << argument << " requires =N.\n";
        printUsage();
        return 64;
      } else {
        arguments.emplace_back(argument);
      }
    }

    const auto launch = parseLaunchRequest(arguments);
    if (!launch) {
      printUsage();
      return 64;
    }

    const auto supports_mission_selection =
        supportsMissionSelection(launch->mode);
    const auto cheats_enabled = sf::platform::retailCheatMarkerExists();
    if (cheats_enabled && supports_mission_selection) {
      retail_cheats.enableAll();
    }
    if (requested_mission && !supports_mission_selection) {
      std::cerr << "Mission selection requires --game, --title-test or "
                   "--scene-test.\n";
      printUsage();
      return 64;
    }
    if (retail_cheats.all_weapons && !supports_mission_selection) {
      std::cerr << "--all-weapons-test requires --game, --title-test or "
                   "--scene-test.\n";
      printUsage();
      return 64;
    }
    if ((requested_mission || retail_cheats.all_weapons) && !cheats_enabled) {
      const auto message =
          "Mission and inventory overrides require an empty "
          "mohu_cheats file beside medal_of_honor_underground.exe.";
      std::cerr << message << '\n';
      sf::platform::showLauncherError("RESTRICTED ACCESS", message);
      return 64;
    }
    auto cue_path = launch->cue_path;
    if (show_launcher &&
        !sf::platform::showGraphicsLauncher(graphics, input, runtime_actions,
                                            language, cue_path)) {
      return 0;
    }
    if (!sf::game::localizationPackAvailable(language)) {
      const auto message =
          "The selected Russian text pack is missing or incomplete.";
      std::cerr << message << '\n';
      sf::platform::showLauncherError("LANGUAGE PACK MISSING", message);
      return 64;
    }
    sf::game::setGameLanguage(language);
    if (cue_path.empty()) {
      const auto message =
          "No game image was selected. Choose the BIN or CUE from the original "
          "Medal of Honor: Underground USA disc image.";
      std::cerr << message << '\n';
      sf::platform::showLauncherError("DISC IMAGE REQUIRED", message);
      return 64;
    }

    auto disc = sf::game::GameDisc::open(cue_path);
    if (!disc.game()) {
      std::cerr << "Unsupported disc build\n";
      sf::platform::showLauncherError("UNSUPPORTED DISC BUILD",
                                      "Use Medal of Honor: Underground USA "
                                      "(SLUS-01270) in BIN/CUE format.");
      return 2;
    }

    std::cout << "MOHU USA disc verified: " << disc.game()->serial << ".\n";
    if (launch->mode == LaunchMode::verify_only) {
      return 0;
    }
    if (launch->mode == LaunchMode::platform_test) {
      std::cout
          << "Starting PsyCross platform test; close the window to exit.\n";
      auto host = sf::platform::createPsyCrossHost(
          "Medal of Honor: Underground PC - platform test", graphics);
      host->run();
      return 0;
    }

    const auto memory_card_slot_1_path =
        sf::platform::defaultMemoryCardImagePath(0U);
    const auto memory_card_slot_2_path =
        sf::platform::defaultMemoryCardImagePath(1U);
    if (memory_card_slot_1_path.empty() || memory_card_slot_2_path.empty()) {
      throw sf::core::Error{sf::core::ErrorCode::io,
                            "Cannot resolve the Memory Card save paths"};
    }
    auto runtime = std::make_unique<mohu::Runtime>(
        std::move(disc), memory_card_slot_1_path, memory_card_slot_2_path);
    const auto gameplay_aspect = sf::platform::presentationAspectRatio(
        graphics.aspect_ratio, sf::platform::PresentationContent::gameplay);
    const auto spu_diagnostics_enabled =
        environmentFlagEnabled("SF_SPU_DIAGNOSTICS");
    const auto exact_transform_diagnostics_enabled =
        environmentFlagEnabled("SF_EXACT_TRANSFORM_DIAGNOSTICS");
    runtime->setSpuDiagnosticsEnabled(spu_diagnostics_enabled);
    runtime->configureAdaptiveWorldFrustum(
        static_cast<std::uint32_t>(std::max(graphics.width, 1)),
        static_cast<std::uint32_t>(std::max(graphics.height, 1)),
        gameplay_aspect == sf::platform::AspectRatioMode::adaptive);
    using RuntimeClock = std::chrono::steady_clock;
    std::uint64_t next_cpu_perf_log_frame{1U};
    std::uint64_t cpu_perf_samples{};
    std::uint64_t cpu_perf_instruction_base{};
    std::uint64_t cpu_perf_fallback_base{};
    std::chrono::microseconds cpu_perf_total{};
    std::chrono::microseconds cpu_perf_max{};
    bool cpu_perf_started{};
    std::optional<mohu::RuntimeFrameResult> runtime_failure;
    std::uint64_t next_exact_transform_log_frame{120U};
    const auto copy_span = []<typename T>(std::span<const T> values) {
      return std::vector<T>{values.begin(), values.end()};
    };
    auto host = sf::platform::createPsyCrossRuntimeHost(
        "Medal of Honor: Underground PC",
        [&](const sf::platform::RuntimePadInputs &pads,
            const sf::platform::RuntimeMenuInteraction &menu_interaction) {
          if (menu_interaction.selection_valid) {
            static_cast<void>(runtime->selectFrontendMenuTarget(
                menu_interaction.screen_id, menu_interaction.selection));
          }
          if (!cpu_perf_started) {
            cpu_perf_instruction_base = runtime->stats().instructions;
            cpu_perf_fallback_base = runtime->stats().cached_fast_fallbacks;
            cpu_perf_started = true;
          }
          const auto frame_started = RuntimeClock::now();
          mohu::RuntimeControllerInputs controllers{};
          for (std::size_t port{}; port < controllers.size(); ++port) {
            controllers[port].active_low_buttons =
                pads[port].active_low_buttons;
            controllers[port].analog = pads[port].analog;
            controllers[port].connected = pads[port].connected;
          }
          auto frame = runtime->runFrame(controllers);
          const auto frame_elapsed =
              std::chrono::duration_cast<std::chrono::microseconds>(
                  RuntimeClock::now() - frame_started);
          const auto &stats = runtime->stats();
          cpu_perf_total += frame_elapsed;
          cpu_perf_max = std::max(cpu_perf_max, frame_elapsed);
          ++cpu_perf_samples;
          if (stats.frames >= next_cpu_perf_log_frame &&
              cpu_perf_samples != 0U) {
            const auto instructions =
                stats.instructions - cpu_perf_instruction_base;
            const auto fast_fallbacks =
                stats.cached_fast_fallbacks - cpu_perf_fallback_base;
            const auto read_guest_word = [&](std::uint32_t address) {
              std::uint32_t value{};
              static_cast<void>(runtime->cpu().read32(address, value));
              return value;
            };
            constexpr std::uint32_t card_state = 0x800985c0U;
            sf::platform::RuntimeGuestCardDiagnostics card{
                .task = read_guest_word(card_state),
                .result = read_guest_word(card_state + 0x04U),
                .done = read_guest_word(card_state + 0x08U),
                .ports = read_guest_word(card_state + 0x0cU),
                .channel = read_guest_word(card_state + 0x10U),
                .completion_callback = read_guest_word(card_state + 0x44U),
                .task_stack_depth = read_guest_word(0x80087890U),
                .vblank_callback = read_guest_word(0x80031a68U),
                .update_ticks = read_guest_word(card_state + 0x50U),
            };
            for (std::size_t index{}; index < card.software_events.size();
                 ++index) {
              card.software_events[index] = read_guest_word(
                  0x80098690U + static_cast<std::uint32_t>(index) * 4U);
              card.hardware_events[index] = read_guest_word(
                  0x800986a0U + static_cast<std::uint32_t>(index) * 4U);
              card.software_handles[index] = read_guest_word(
                  0x80098670U + static_cast<std::uint32_t>(index) * 4U);
              card.hardware_handles[index] = read_guest_word(
                  0x80098680U + static_cast<std::uint32_t>(index) * 4U);
              card.stack_callbacks[index] = read_guest_word(
                  0x80098660U + static_cast<std::uint32_t>(index) * 4U);
              for (std::size_t word{}; word < card.stack_frames[index].size();
                   ++word) {
                card.stack_frames[index][word] =
                    read_guest_word(0x80098620U + static_cast<std::uint32_t>(
                                                      index * 16U + word * 4U));
              }
            }
            sf::platform::logRuntimeGuestCpuDiagnostics(
                cpu_perf_samples, cpu_perf_total.count() / cpu_perf_samples,
                static_cast<std::uint64_t>(cpu_perf_max.count()),
                instructions / cpu_perf_samples, fast_fallbacks,
                runtime->cpu().state().pc, runtime->cpu().state().gpr[31U],
                runtime->biosState(), card, runtime->machine().currentTick());
            cpu_perf_samples = 0U;
            cpu_perf_total = {};
            cpu_perf_max = {};
            cpu_perf_instruction_base = stats.instructions;
            cpu_perf_fallback_base = stats.cached_fast_fallbacks;
            next_cpu_perf_log_frame = stats.frames + 30U;
          }
          if (exact_transform_diagnostics_enabled &&
              stats.frames >= next_exact_transform_log_frame) {
            sf::platform::logRuntimeExactTransformDiagnostics(
                stats.exact_transform_captures,
                stats.exact_transform_publications,
                stats.exact_transform_rejections,
                stats.exact_composition_entries,
                stats.exact_composition_captures, stats.exact_composition_sites,
                stats.exact_composition_publications,
                runtime->gpuProjectionCatalog());
            next_exact_transform_log_frame = stats.frames + 120U;
          }

          if (!frame.running()) {
            runtime_failure = std::move(frame);
            return sf::platform::RuntimeFrameStep{.running = false};
          }
          const auto &display = runtime->gpuDisplayState();
          const auto gameplay_ready = runtime->gameplayPresentationReady();
          std::vector<sf::platform::RuntimeGpuDisplayPublication>
              display_publications;
          display_publications.reserve(
              runtime->gpuDisplayPublications().size());
          for (const auto &publication : runtime->gpuDisplayPublications()) {
            display_publications.push_back({
                .word_offset = publication.word_offset,
                .sequence = publication.sequence,
                .display_x = publication.display.x,
                .display_y = publication.display.y,
                .display_width = publication.display.width,
                .display_height = publication.display.height,
                .display_enabled = publication.display.enabled,
                .display_rgb24 = publication.display.rgb24,
                .display_interlaced = publication.display.interlaced,
            });
          }
          const auto campaign_level = runtime->activeCampaignLevel();
          const auto atmosphere = runtime->activeAtmosphere();
          const auto frontend_menu = mohu::inspectFrontendMenu(runtime->cpu());
          sf::platform::RuntimeMenuState menu{
              .active = frontend_menu.active,
              .screen_id = frontend_menu.screen_id,
              .selected = frontend_menu.selected,
          };
          menu.targets.reserve(frontend_menu.target_count);
          for (std::size_t index{}; index < frontend_menu.target_count;
               ++index) {
            const auto &target = frontend_menu.targets[index];
            menu.targets.push_back({
                .selection = target.selection,
                .x = target.x,
                .y = target.y,
                .width = target.width,
                .height = target.height,
            });
          }
          std::shared_ptr<const sf::platform::RuntimeGpuFrame> snapshot =
              std::make_shared<sf::platform::RuntimeGpuFrame>(
                  sf::platform::RuntimeGpuFrame{
                      .sequence = stats.frames,
                      .display_publication_sequence =
                          runtime->gpuDisplayPublicationSequence(),
                      .display_publications = std::move(display_publications),
                      .words = copy_span(runtime->gpuCommands()),
                      .projections = copy_span(runtime->gpuProjections()),
                      .projection_identities =
                          copy_span(runtime->gpuProjectionIdentities()),
                      .projection_catalog =
                          copy_span(runtime->gpuProjectionCatalog()),
                      .display_x = display.x,
                      .display_y = display.y,
                      .display_width = display.width,
                      .display_height = display.height,
                      .display_enabled = display.enabled,
                      .display_rgb24 = display.rgb24,
                      .display_interlaced = display.interlaced,
                      .content =
                          gameplay_ready && !display.rgb24 &&
                                  !display.interlaced
                              ? sf::platform::PresentationContent::gameplay
                              : sf::platform::PresentationContent::authored_4_3,
                      .campaign_level = campaign_level,
                      .atmosphere =
                          {
                              .red = atmosphere.red,
                              .green = atmosphere.green,
                              .blue = atmosphere.blue,
                              .dqa = atmosphere.dqa,
                              .dqb = atmosphere.dqb,
                              .projection = atmosphere.projection,
                              .terrain_depth_cue = atmosphere.terrain_depth_cue,
                              .skybox_yaw = atmosphere.skybox_yaw,
                              .skybox_pitch = atmosphere.skybox_pitch,
                              .skybox_vertical_fov =
                                  atmosphere.skybox_vertical_fov,
                              .valid = atmosphere.valid,
                              .skybox_view_valid = atmosphere.skybox_view_valid,
                          },
                      .menu = std::move(menu),
                      .command_buffer_epoch = runtime->gpuCommandBufferEpoch(),
                      .dma_sources = copy_span(runtime->gpuDmaSources()),
                  });
          return sf::platform::RuntimeFrameStep{.frame = std::move(snapshot)};
        },
        graphics, input, runtime_actions,
        [&runtime, spu_diagnostics_enabled](
            std::span<sf::psx::SpuPcmFrame> destination) noexcept {
          const auto count = runtime->takePcm(destination);
          if (spu_diagnostics_enabled)
            logSpuDiagnostics(*runtime);
          return count;
        });
    host->run();

    if (runtime_failure) {
      std::cerr << runtime_failure->detail << '\n';
      sf::platform::showLauncherError("GUEST RUNTIME STOPPED",
                                      runtime_failure->detail);
      return 3;
    }
    const auto &stats = runtime->stats();
    std::cout << "Guest runtime stopped by user after " << stats.frames
              << " frames and " << stats.instructions << " instructions.\n";
    return 0;
  } catch (const sf::core::Error &error) {
    std::cerr << "medal_of_honor_underground: " << error.what() << '\n';
    sf::platform::showLauncherError("STARTUP FAILED", error.what());
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "medal_of_honor_underground: unexpected error: "
              << error.what() << '\n';
    sf::platform::showLauncherError("UNEXPECTED ERROR", error.what());
    return 1;
  }
}
