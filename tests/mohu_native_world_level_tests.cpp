#include "mohu/native_world_level.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t bsd_info_offset = 2048U;
constexpr std::size_t face_offset = 100U;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

void put16(std::vector<std::uint8_t> &bytes, std::size_t offset,
           std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value);
  bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}

void put32(std::vector<std::uint8_t> &bytes, std::size_t offset,
           std::uint32_t value) {
  put16(bytes, offset, static_cast<std::uint16_t>(value));
  put16(bytes, offset + 2U, static_cast<std::uint16_t>(value >> 16U));
}

[[nodiscard]] std::vector<std::byte>
makeBsd(std::string_view pattern = "data\\msn2\\lvl1\\tsp0\\2_1_c",
        std::uint32_t count = 7U, std::uint32_t first = 1U,
        std::uint32_t last = 4U, std::uint32_t reserved = 0U) {
  std::vector<std::uint8_t> bytes(bsd_info_offset + 144U);
  const auto copied = std::min<std::size_t>(pattern.size(), 127U);
  for (std::size_t index{}; index < copied; ++index) {
    bytes[bsd_info_offset + index] = static_cast<std::uint8_t>(pattern[index]);
  }
  put32(bytes, bsd_info_offset + 128U, count);
  put32(bytes, bsd_info_offset + 132U, last);
  put32(bytes, bsd_info_offset + 136U, first);
  put32(bytes, bsd_info_offset + 140U, reserved);
  std::vector<std::byte> result(bytes.size());
  for (std::size_t index{}; index < bytes.size(); ++index) {
    result[index] = static_cast<std::byte>(bytes[index]);
  }
  return result;
}

[[nodiscard]] std::vector<std::byte> makeTsp() {
  constexpr std::size_t node_offset = 72U;
  constexpr std::size_t position_offset = 116U;
  constexpr std::size_t color_offset = 148U;
  constexpr std::size_t material_offset = 164U;
  std::vector<std::uint8_t> bytes(176U);
  put16(bytes, 0U, 1U);
  put16(bytes, 2U, 3U);
  put32(bytes, 4U, 1U);
  put32(bytes, 8U, node_offset);
  put32(bytes, 12U, 2U);
  put32(bytes, 16U, face_offset);
  put32(bytes, 20U, 4U);
  put32(bytes, 24U, position_offset);
  put32(bytes, 28U, 0U);
  put32(bytes, 32U, color_offset);
  put32(bytes, 36U, 4U);
  put32(bytes, 40U, color_offset);
  put32(bytes, 44U, 0U);
  put32(bytes, 48U, material_offset);
  put32(bytes, 52U, 0U);
  put32(bytes, 56U, material_offset);
  put32(bytes, 60U, 176U);
  put32(bytes, 64U, 1U);
  put32(bytes, 68U, material_offset);

  put16(bytes, node_offset + 0U, static_cast<std::uint16_t>(-10));
  put16(bytes, node_offset + 2U, static_cast<std::uint16_t>(-20));
  put16(bytes, node_offset + 4U, static_cast<std::uint16_t>(-30));
  put16(bytes, node_offset + 6U, 10U);
  put16(bytes, node_offset + 8U, 20U);
  put16(bytes, node_offset + 10U, 30U);
  put32(bytes, node_offset + 12U, 2U);
  put32(bytes, node_offset + 24U, 0U);

  put32(bytes, face_offset, 0x00010000U);
  put16(bytes, face_offset + 4U, 2U);
  put16(bytes, face_offset + 6U, 0U);
  put32(bytes, face_offset + 8U, 0x00004003U);
  put32(bytes, face_offset + 12U, 0x1fff1fffU);
  for (std::size_t index{}; index < 4U; ++index) {
    const auto offset = position_offset + index * 8U;
    put16(bytes, offset, static_cast<std::uint16_t>(index * 10U));
    put16(bytes, offset + 2U, static_cast<std::uint16_t>(index * 20U));
    put16(bytes, offset + 4U, static_cast<std::uint16_t>(index * 30U));
    put16(bytes, offset + 6U, 104U);
    bytes[color_offset + index * 4U] = static_cast<std::uint8_t>(index + 1U);
  }
  bytes[material_offset + 0U] = 1U;
  bytes[material_offset + 1U] = 2U;
  put16(bytes, material_offset + 2U, 0x1234U);
  bytes[material_offset + 4U] = 3U;
  bytes[material_offset + 5U] = 4U;
  put16(bytes, material_offset + 6U, 0x00a1U);
  bytes[material_offset + 8U] = 5U;
  bytes[material_offset + 9U] = 6U;

  std::vector<std::byte> result(bytes.size());
  for (std::size_t index{}; index < bytes.size(); ++index) {
    result[index] = static_cast<std::byte>(bytes[index]);
  }
  return result;
}

} // namespace

int main() {
  try {
    const auto bsd = makeBsd();
    const auto info = mohu::parseNativeWorldLevelInfo(bsd);
    require(static_cast<bool>(info), "valid BSD TSP info was rejected");
    require(info.info.tsp_path_prefix == "DATA/MSN2/LVL1/TSP0/2_1_C" &&
                info.info.compartment_count == 7U &&
                info.info.first_initial_compartment == 1U &&
                info.info.last_initial_compartment == 4U,
            "BSD TSP info was decoded incorrectly");

    std::vector<std::string> requested;
    const mohu::NativeWorldFileReader reader =
        [&requested](std::string_view path) {
          requested.emplace_back(path);
          return makeTsp();
        };
    const auto loaded = mohu::loadNativeWorldLevel(bsd, reader);
    require(static_cast<bool>(loaded), "valid initial TSP set was rejected");
    require(loaded.level.initial_compartments.size() == 4U &&
                requested ==
                    std::vector<std::string>{"DATA/MSN2/LVL1/TSP0/2_1_C1.TSP",
                                             "DATA/MSN2/LVL1/TSP0/2_1_C2.TSP",
                                             "DATA/MSN2/LVL1/TSP0/2_1_C3.TSP",
                                             "DATA/MSN2/LVL1/TSP0/2_1_C4.TSP"},
            "initial TSP files were not loaded in retail order");
    for (std::size_t index{}; index < loaded.level.initial_compartments.size();
         ++index) {
      const auto &compartment = loaded.level.initial_compartments[index];
      require(compartment.number == index + 1U &&
                  compartment.mesh.opaque_indices.size() == 6U &&
                  compartment.mesh.nodes.size() == 1U,
              "initial TSP indexed mesh mismatch");
    }

    const auto truncated = mohu::parseNativeWorldLevelInfo(
        std::span<const std::byte>{bsd}.first(2048U));
    require(!truncated &&
                truncated.error == mohu::NativeWorldLevelError::truncated_bsd,
            "truncated BSD TSP info was accepted");
    const auto traversal =
        mohu::parseNativeWorldLevelInfo(makeBsd("data\\..\\2_1_c"));
    require(!traversal && traversal.error ==
                              mohu::NativeWorldLevelError::invalid_tsp_pattern,
            "unsafe TSP path pattern was accepted");
    const auto bad_range =
        mohu::parseNativeWorldLevelInfo(makeBsd("data\\2_1_c", 7U, 3U, 2U));
    require(!bad_range &&
                bad_range.error ==
                    mohu::NativeWorldLevelError::invalid_initial_range,
            "invalid initial TSP range was accepted");

    std::uint32_t read_count{};
    const mohu::NativeWorldFileReader failing_reader =
        [&read_count](std::string_view) -> std::vector<std::byte> {
      ++read_count;
      if (read_count == 3U) {
        throw std::runtime_error{"missing TSP"};
      }
      return makeTsp();
    };
    const auto missing = mohu::loadNativeWorldLevel(bsd, failing_reader);
    require(!missing &&
                missing.error == mohu::NativeWorldLevelError::tsp_read_failed &&
                missing.failed_compartment == 3U &&
                missing.level.initial_compartments.empty(),
            "failed TSP load exposed a partial native level");

    const mohu::NativeWorldFileReader corrupt_reader = [](std::string_view) {
      return std::vector<std::byte>(16U);
    };
    const auto corrupt = mohu::loadNativeWorldLevel(bsd, corrupt_reader);
    require(!corrupt &&
                corrupt.error == mohu::NativeWorldLevelError::invalid_tsp &&
                corrupt.failed_compartment == 1U &&
                corrupt.tsp_error == mohu::TspSceneError::truncated_header &&
                corrupt.level.initial_compartments.empty(),
            "invalid TSP exposed a partial native level");

    std::cout << "mohu_native_world_level_tests: ok\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mohu_native_world_level_tests: " << error.what() << '\n';
    return 1;
  }
}
