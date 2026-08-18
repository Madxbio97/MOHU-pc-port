#pragma once

#include "sf/game/localization.hpp"
#include "sf/platform/host.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace sf::platform {

void loadLauncherSettings(GraphicsSettings &graphics,
                          KeyboardMouseBindings &input,
                          MohUndergroundRuntimeActionBindings &runtime_actions,
                          game::GameLanguage &language) noexcept;

[[nodiscard]] std::filesystem::path
defaultMemoryCardImagePath(std::uint8_t slot = 0U) noexcept;

[[nodiscard]] bool
saveLauncherControllerSettings(const ControllerButtonBindings &bindings,
                               bool vibration) noexcept;

[[nodiscard]] bool
showGraphicsLauncher(GraphicsSettings &settings, KeyboardMouseBindings &input,
                     MohUndergroundRuntimeActionBindings &runtime_actions,
                     game::GameLanguage &language,
                     std::filesystem::path &cue_path);

[[nodiscard]] bool retailCheatMarkerExists() noexcept;

void showLauncherError(std::string_view title,
                       std::string_view message) noexcept;

} // namespace sf::platform
