#pragma once

#include "mohu/native_world_mesh.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sf::game {
class GameDisc;
}

namespace mohu {

struct NativeWorldLevelInfo {
  std::string tsp_path_prefix;
  std::uint32_t compartment_count{};
  std::uint32_t first_initial_compartment{};
  std::uint32_t last_initial_compartment{};
};

struct NativeWorldCompartment {
  std::uint32_t number{};
  std::string iso_path;
  NativeWorldMesh mesh;
};

struct NativeWorldLevel {
  NativeWorldLevelInfo info;
  std::vector<NativeWorldCompartment> initial_compartments;
};

enum class NativeWorldLevelError : std::uint8_t {
  none,
  truncated_bsd,
  invalid_tsp_pattern,
  invalid_compartment_count,
  invalid_initial_range,
  tsp_read_failed,
  invalid_tsp,
  invalid_mesh,
  out_of_memory,
};

struct NativeWorldLevelInfoResult {
  NativeWorldLevelInfo info;
  NativeWorldLevelError error{NativeWorldLevelError::none};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == NativeWorldLevelError::none;
  }
};

struct NativeWorldLevelLoadResult {
  NativeWorldLevel level;
  NativeWorldLevelError error{NativeWorldLevelError::none};
  TspSceneError tsp_error{TspSceneError::none};
  std::uint32_t failed_compartment{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == NativeWorldLevelError::none;
  }
};

using NativeWorldFileReader =
    std::function<std::vector<std::byte>(std::string_view)>;

[[nodiscard]] NativeWorldLevelInfoResult
parseNativeWorldLevelInfo(std::span<const std::byte> bsd) noexcept;

[[nodiscard]] NativeWorldLevelLoadResult
loadNativeWorldLevel(std::span<const std::byte> bsd,
                     const NativeWorldFileReader &read_file) noexcept;

[[nodiscard]] NativeWorldLevelLoadResult
loadNativeWorldLevel(sf::game::GameDisc &disc,
                     std::span<const std::byte> bsd) noexcept;

[[nodiscard]] const char *
nativeWorldLevelErrorMessage(NativeWorldLevelError error) noexcept;

} // namespace mohu
