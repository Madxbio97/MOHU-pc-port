#include "mohu/native_world_level.hpp"

#include "sf/game/game_disc.hpp"

#include <new>
#include <utility>

namespace mohu {
namespace {

constexpr std::size_t bsd_header_size = 2048U;
constexpr std::size_t tsp_pattern_size = 128U;
constexpr std::size_t tsp_info_size =
    tsp_pattern_size + 4U * sizeof(std::uint32_t);
constexpr std::uint32_t maximum_compartments = 64U;
constexpr std::uint32_t maximum_initial_compartments = 4U;

[[nodiscard]] std::uint32_t readLe32(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] bool safePathCharacter(unsigned char character) noexcept {
  return (character >= '0' && character <= '9') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z') || character == '_' ||
         character == '-' || character == '.';
}

[[nodiscard]] char uppercaseAscii(unsigned char character) noexcept {
  if (character >= 'a' && character <= 'z') {
    return static_cast<char>(character - ('a' - 'A'));
  }
  return static_cast<char>(character);
}

[[nodiscard]] bool appendPathComponent(std::string_view component,
                                       std::string &normalized) {
  if (component.empty() || component == "." || component == "..") {
    return false;
  }
  if (!normalized.empty()) {
    normalized.push_back('/');
  }
  for (const auto character : component) {
    const auto byte = static_cast<unsigned char>(character);
    if (!safePathCharacter(byte)) {
      return false;
    }
    normalized.push_back(uppercaseAscii(byte));
  }
  return true;
}

[[nodiscard]] bool normalizeTspPattern(std::string_view source,
                                       std::string &normalized) {
  normalized.clear();
  if (source.empty() || source.front() == '/' || source.front() == '\\') {
    return false;
  }
  std::size_t component_start{};
  for (std::size_t index{}; index <= source.size(); ++index) {
    if (index != source.size() && source[index] != '/' &&
        source[index] != '\\') {
      continue;
    }
    if (!appendPathComponent(
            source.substr(component_start, index - component_start),
            normalized)) {
      return false;
    }
    component_start = index + 1U;
  }
  return normalized.ends_with("_C");
}

[[nodiscard]] NativeWorldLevelLoadResult
loadFailure(NativeWorldLevelLoadResult result, NativeWorldLevelError error,
            std::uint32_t compartment = 0U,
            TspSceneError tsp_error = TspSceneError::none) noexcept {
  result.level.initial_compartments.clear();
  result.error = error;
  result.failed_compartment = compartment;
  result.tsp_error = tsp_error;
  return result;
}

} // namespace

NativeWorldLevelInfoResult
parseNativeWorldLevelInfo(std::span<const std::byte> bsd) noexcept {
  NativeWorldLevelInfoResult result{};
  if (bsd.size() < bsd_header_size + tsp_info_size) {
    result.error = NativeWorldLevelError::truncated_bsd;
    return result;
  }
  try {
    const auto info = bsd.subspan(bsd_header_size, tsp_info_size);
    auto pattern_length = std::size_t{};
    while (pattern_length < tsp_pattern_size &&
           info[pattern_length] != std::byte{}) {
      ++pattern_length;
    }
    if (pattern_length == 0U || pattern_length == tsp_pattern_size) {
      result.error = NativeWorldLevelError::invalid_tsp_pattern;
      return result;
    }
    std::string pattern;
    pattern.reserve(pattern_length);
    for (std::size_t index{}; index < pattern_length; ++index) {
      pattern.push_back(
          static_cast<char>(std::to_integer<unsigned char>(info[index])));
    }
    if (!normalizeTspPattern(pattern, result.info.tsp_path_prefix)) {
      result.info = {};
      result.error = NativeWorldLevelError::invalid_tsp_pattern;
      return result;
    }

    result.info.compartment_count = readLe32(info, tsp_pattern_size);
    result.info.last_initial_compartment =
        readLe32(info, tsp_pattern_size + sizeof(std::uint32_t));
    result.info.first_initial_compartment =
        readLe32(info, tsp_pattern_size + 2U * sizeof(std::uint32_t));
    const auto reserved =
        readLe32(info, tsp_pattern_size + 3U * sizeof(std::uint32_t));
    if (result.info.compartment_count == 0U ||
        result.info.compartment_count > maximum_compartments ||
        reserved != 0U) {
      result.info = {};
      result.error = NativeWorldLevelError::invalid_compartment_count;
      return result;
    }
    const auto first = result.info.first_initial_compartment;
    const auto last = result.info.last_initial_compartment;
    if (first == 0U || last < first || last > result.info.compartment_count ||
        last - first + 1U > maximum_initial_compartments) {
      result.info = {};
      result.error = NativeWorldLevelError::invalid_initial_range;
      return result;
    }
    return result;
  } catch (const std::bad_alloc &) {
    result.info = {};
    result.error = NativeWorldLevelError::out_of_memory;
    return result;
  }
}

NativeWorldLevelLoadResult
loadNativeWorldLevel(std::span<const std::byte> bsd,
                     const NativeWorldFileReader &read_file) noexcept {
  NativeWorldLevelLoadResult result{};
  auto parsed = parseNativeWorldLevelInfo(bsd);
  if (!parsed) {
    result.error = parsed.error;
    return result;
  }
  const auto first_initial = parsed.info.first_initial_compartment;
  const auto last_initial = parsed.info.last_initial_compartment;
  try {
    result.level.info = std::move(parsed.info);
    if (!read_file) {
      return loadFailure(std::move(result),
                         NativeWorldLevelError::tsp_read_failed, first_initial);
    }
    const auto initial_count = last_initial - first_initial + 1U;
    result.level.initial_compartments.reserve(initial_count);
    for (auto compartment = first_initial; compartment <= last_initial;
         ++compartment) {
      auto path = result.level.info.tsp_path_prefix +
                  std::to_string(compartment) + ".TSP";
      std::vector<std::byte> bytes;
      try {
        bytes = read_file(path);
      } catch (const std::bad_alloc &) {
        throw;
      } catch (...) {
        return loadFailure(std::move(result),
                           NativeWorldLevelError::tsp_read_failed, compartment);
      }
      if (bytes.empty()) {
        return loadFailure(std::move(result),
                           NativeWorldLevelError::tsp_read_failed, compartment);
      }
      const auto byte_view = std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()};
      auto scene = loadTspScene(byte_view);
      if (!scene) {
        if (scene.error == TspSceneError::out_of_memory) {
          return loadFailure(std::move(result),
                             NativeWorldLevelError::out_of_memory, compartment,
                             scene.error);
        }
        return loadFailure(std::move(result),
                           NativeWorldLevelError::invalid_tsp, compartment,
                           scene.error);
      }
      auto mesh = buildNativeWorldMesh(scene.scene);
      if (!mesh) {
        const auto error = mesh.error == NativeWorldMeshError::out_of_memory
                               ? NativeWorldLevelError::out_of_memory
                               : NativeWorldLevelError::invalid_mesh;
        return loadFailure(std::move(result), error, compartment);
      }
      result.level.initial_compartments.push_back(
          {compartment, std::move(path), std::move(mesh.mesh)});
    }
    return result;
  } catch (const std::bad_alloc &) {
    return loadFailure(std::move(result), NativeWorldLevelError::out_of_memory);
  }
}

NativeWorldLevelLoadResult
loadNativeWorldLevel(sf::game::GameDisc &disc,
                     std::span<const std::byte> bsd) noexcept {
  try {
    const NativeWorldFileReader reader = [&disc](std::string_view path) {
      return disc.image().readFile(std::string{path});
    };
    return loadNativeWorldLevel(bsd, reader);
  } catch (const std::bad_alloc &) {
    NativeWorldLevelLoadResult result{};
    result.error = NativeWorldLevelError::out_of_memory;
    return result;
  }
}

const char *nativeWorldLevelErrorMessage(NativeWorldLevelError error) noexcept {
  switch (error) {
  case NativeWorldLevelError::none:
    return "none";
  case NativeWorldLevelError::truncated_bsd:
    return "truncated BSD TSP info";
  case NativeWorldLevelError::invalid_tsp_pattern:
    return "invalid BSD TSP path pattern";
  case NativeWorldLevelError::invalid_compartment_count:
    return "invalid BSD TSP compartment count";
  case NativeWorldLevelError::invalid_initial_range:
    return "invalid BSD initial TSP range";
  case NativeWorldLevelError::tsp_read_failed:
    return "TSP file could not be read";
  case NativeWorldLevelError::invalid_tsp:
    return "invalid TSP scene";
  case NativeWorldLevelError::invalid_mesh:
    return "invalid native TSP mesh";
  case NativeWorldLevelError::out_of_memory:
    return "out of memory";
  }
  return "unknown native world level error";
}

} // namespace mohu
