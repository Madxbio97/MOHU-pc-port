#pragma once

#include "sf/psx/gpu_dma_source.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mohu {

struct GpuPrimitiveSnapshotView {
  std::span<const std::uint32_t> words;
  std::span<const sf::psx::GpuDmaWordSource> dma_sources;
};

enum class GpuPrimitiveMatchKind : std::uint8_t {
  raw_address,
  linked_list_relative,
};

struct GpuPrimitiveMatch {
  std::size_t previous_command_word{};
  std::size_t current_command_word{};
  std::array<std::size_t, 4U> previous_coordinate_words{};
  std::array<std::size_t, 4U> current_coordinate_words{};
  std::uint8_t vertex_count{};
  GpuPrimitiveMatchKind kind{GpuPrimitiveMatchKind::raw_address};

  friend bool operator==(const GpuPrimitiveMatch &,
                         const GpuPrimitiveMatch &) = default;
};

struct GpuPrimitiveMatchReport {
  static constexpr std::size_t minimum_relative_polygons = 8U;
  static constexpr std::size_t minimum_relative_coordinate_words = 24U;

  std::vector<GpuPrimitiveMatch> matches;
  std::size_t previous_polygon_count{};
  std::size_t current_polygon_count{};
  std::size_t raw_match_count{};
  std::size_t relative_match_count{};
  bool input_valid{};
  bool relative_provenance_proven{};
};

[[nodiscard]] GpuPrimitiveMatchReport
matchGpuPrimitiveSnapshots(GpuPrimitiveSnapshotView previous,
                           GpuPrimitiveSnapshotView current);

} // namespace mohu
