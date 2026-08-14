#pragma once

#include "mohu/native_world_camera.hpp"
#include "sf/psx/gte_runtime.hpp"
#include "sf/psx/r3000_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mohu {

inline constexpr std::size_t native_world_record_count = 100U;
inline constexpr std::size_t native_world_vertices_per_record = 4U;
inline constexpr std::size_t maximum_native_world_projection_overrides = 400U;
inline constexpr std::size_t maximum_native_world_snapshot_segments = 8U;

struct NativeWorldFrameSnapshot {
  NativeWorldCamera camera{};
  std::array<std::array<TspPosition, native_world_vertices_per_record>,
             native_world_record_count>
      positions{};
  std::uint8_t packet_buffer{};
  bool valid{};
};

struct NativeWorldFrameSnapshotSegment {
  std::size_t word_begin{};
  NativeWorldFrameSnapshot snapshot{};
};

struct NativeWorldProjectionOverride {
  std::size_t word_index{};
  sf::psx::GteProjectedVertex projection{};
};

struct NativeWorldFrameStats {
  std::uint32_t polygon_commands{};
  std::array<std::uint32_t, 512U> polygon_source_pages{};
  std::uint32_t candidate_quads{};
  std::uint32_t coordinate_words{};
  std::uint32_t complete_quads{};
  std::uint32_t incomplete_quads{};
  std::uint32_t invalid_geometry_quads{};
  std::uint32_t witness_rejected_quads{};
  float maximum_witness_error{};
};

[[nodiscard]] NativeWorldFrameSnapshot
captureNativeWorldFrameSnapshot(const sf::psx::R3000Runtime &cpu,
                                const NativeWorldCamera &camera) noexcept;

[[nodiscard]] NativeWorldFrameStats overrideNativeWorldFrameProjections(
    std::span<const std::uint32_t> words,
    std::span<const std::uint32_t> source_addresses,
    std::span<NativeWorldProjectionOverride> projections,
    std::span<const NativeWorldFrameSnapshotSegment> snapshots) noexcept;

[[nodiscard]] NativeWorldFrameStats overrideNativeWorldFrameProjections(
    std::span<const std::uint32_t> words,
    std::span<const std::uint32_t> source_addresses,
    std::span<NativeWorldProjectionOverride> projections,
    const NativeWorldFrameSnapshot &snapshot) noexcept;

} // namespace mohu
