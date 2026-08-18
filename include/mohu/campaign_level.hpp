#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace mohu {

inline constexpr std::array campaign_level_directories{
    std::string_view{"DATA/MSN2/LVL1"}, std::string_view{"DATA/MSN2/LVL2"},
    std::string_view{"DATA/MSN2/LVL3"}, std::string_view{"DATA/MSN2/LVL4"},
    std::string_view{"DATA/MSN3/LVL1"}, std::string_view{"DATA/MSN3/LVL2"},
    std::string_view{"DATA/MSN3/LVL3"}, std::string_view{"DATA/MSN3/LVL4"},
    std::string_view{"DATA/MSN4/LVL1"}, std::string_view{"DATA/MSN4/LVL2"},
    std::string_view{"DATA/MSN4/LVL3"}, std::string_view{"DATA/MSN5/LVL1"},
    std::string_view{"DATA/MSN5/LVL2"}, std::string_view{"DATA/MSN5/LVL3"},
    std::string_view{"DATA/MSN6/LVL1"}, std::string_view{"DATA/MSN6/LVL2"},
    std::string_view{"DATA/MSN6/LVL3"}, std::string_view{"DATA/MSN7/LVL1"},
    std::string_view{"DATA/MSN7/LVL2"}, std::string_view{"DATA/MSN7/LVL3"},
    std::string_view{"DATA/MSN8/LVL1"}, std::string_view{"DATA/MSN8/LVL2"},
    std::string_view{"DATA/MSN8/LVL3"}, std::string_view{"DATA/MSN8/LVL4"},
};

[[nodiscard]] constexpr std::uint8_t
campaignLevelForDiscPath(std::string_view path) noexcept {
  for (std::size_t index{}; index < campaign_level_directories.size();
       ++index) {
    const auto directory = campaign_level_directories[index];
    if (path.size() > directory.size() && path.starts_with(directory) &&
        path[directory.size()] == '/') {
      return static_cast<std::uint8_t>(index + 1U);
    }
  }
  return 0U;
}

} // namespace mohu
