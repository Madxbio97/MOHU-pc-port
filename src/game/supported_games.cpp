#include "sf/game/supported_games.hpp"

#include <array>

namespace sf::game {
namespace {

constexpr core::Sha256Digest medal_of_honor_underground_us_exe{
    std::byte{0x5c}, std::byte{0x45}, std::byte{0x66}, std::byte{0xb7},
    std::byte{0x26}, std::byte{0x4a}, std::byte{0x29}, std::byte{0x55},
    std::byte{0x4a}, std::byte{0xbb}, std::byte{0x9d}, std::byte{0x86},
    std::byte{0x8f}, std::byte{0x23}, std::byte{0xcc}, std::byte{0x40},
    std::byte{0xe9}, std::byte{0x9e}, std::byte{0xbf}, std::byte{0x3b},
    std::byte{0x58}, std::byte{0xb9}, std::byte{0x77}, std::byte{0x6d},
    std::byte{0xfe}, std::byte{0x2f}, std::byte{0x4a}, std::byte{0xe4},
    std::byte{0x65}, std::byte{0xbb}, std::byte{0xd6}, std::byte{0x41},
};

constexpr std::array games{
    SupportedGame{
        "Medal of Honor: Underground",
        "USA / NTSC-U",
        "1.0",
        "SLUS-01270",
        "MOHU_NTSC",
        "SLUS_012.70",
        medal_of_honor_underground_us_exe,
    },
};

} // namespace

std::span<const SupportedGame> supportedGames() noexcept {
    return games;
}

std::optional<SupportedGame> identify(
    std::string_view volume_id,
    const core::Sha256Digest& executable_sha256) noexcept {
    for (const auto& game : games) {
        if (game.volume_id == volume_id && game.executable_sha256 == executable_sha256) {
            return game;
        }
    }
    return std::nullopt;
}

} // namespace sf::game
