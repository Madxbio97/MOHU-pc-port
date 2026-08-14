#include "mohu/native_world_frame.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace mohu {
namespace {

constexpr std::uint32_t world_record_base = 0x000a0594U;
constexpr std::uint32_t world_record_stride = 0x80U;
constexpr std::uint32_t world_packet_buffer_selector = 0x000a9354U;
constexpr std::uint32_t invalid_source_address = 0xffffffffU;
constexpr std::uint32_t physical_address_mask = 0x001fffffU;
constexpr std::array<std::size_t, 4U> coordinate_words{1U, 3U, 5U, 7U};
constexpr double minimum_projection_depth = 1.0 / 65536.0;
constexpr double maximum_native_world_witness_error = 1.0;

struct WorldPacket {
  std::uint32_t record_base{};
  std::uint8_t buffer{};
  bool valid{};
};

[[nodiscard]] WorldPacket worldPacket(std::uint32_t source_address) noexcept {
  if (source_address == invalid_source_address) {
    return {};
  }
  source_address &= physical_address_mask;
  if (source_address < world_record_base) {
    return {};
  }
  const auto relative = source_address - world_record_base;
  const auto record = relative / world_record_stride;
  if (record >= native_world_record_count) {
    return {};
  }
  const auto within = relative % world_record_stride;
  if (within == 0x34U) {
    return {world_record_base + record * world_record_stride, 0U, true};
  }
  if (within == 0x5cU) {
    return {world_record_base + record * world_record_stride, 1U, true};
  }
  return {};
}

[[nodiscard]] bool readPosition(const sf::psx::R3000Runtime &cpu,
                                std::uint32_t record_base, std::uint8_t corner,
                                TspPosition &position) noexcept {
  const auto address =
      record_base + 4U + static_cast<std::uint32_t>(corner) * 8U;
  std::uint16_t x{};
  std::uint16_t y{};
  std::uint16_t z{};
  return cpu.read16(address, x) && cpu.read16(address + 2U, y) &&
         cpu.read16(address + 4U, z) &&
         (position = {static_cast<std::int16_t>(x),
                      static_cast<std::int16_t>(y),
                      static_cast<std::int16_t>(z)},
          true);
}

[[nodiscard]] bool
commandIsComplete(std::span<const std::uint32_t> source_addresses,
                  std::size_t command_index) noexcept {
  const auto source = source_addresses[command_index] & physical_address_mask;
  for (std::size_t word{}; word < 9U; ++word) {
    if (source_addresses[command_index + word] == invalid_source_address ||
        (source_addresses[command_index + word] & physical_address_mask) !=
            source + static_cast<std::uint32_t>(word * 4U)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] constexpr std::size_t
fixedCommandLength(std::uint8_t opcode) noexcept {
  if (opcode == 0x02U)
    return 3U;
  if (opcode < 0x20U)
    return 1U;
  if (opcode < 0x24U)
    return 4U;
  if (opcode < 0x28U)
    return 7U;
  if (opcode < 0x2cU)
    return 5U;
  if (opcode < 0x30U)
    return 9U;
  if (opcode < 0x34U)
    return 6U;
  if (opcode < 0x38U)
    return 9U;
  if (opcode < 0x3cU)
    return 8U;
  if (opcode < 0x40U)
    return 12U;
  if (opcode < 0x48U)
    return 3U;
  if (opcode < 0x50U)
    return 0U;
  if (opcode < 0x58U)
    return 4U;
  if (opcode < 0x60U)
    return 0U;
  if (opcode < 0x64U)
    return 3U;
  if (opcode < 0x68U)
    return 4U;
  if (opcode < 0x6cU)
    return 2U;
  if (opcode < 0x70U)
    return 3U;
  if (opcode < 0x74U)
    return 2U;
  if (opcode < 0x78U)
    return 3U;
  if (opcode < 0x7cU)
    return 2U;
  if (opcode < 0x80U)
    return 3U;
  if (opcode < 0xa0U)
    return 4U;
  return 1U;
}

[[nodiscard]] std::size_t
commandLength(std::span<const std::uint32_t> words) noexcept {
  if (words.empty())
    return 0U;
  const auto opcode = static_cast<std::uint8_t>(words.front() >> 24U);
  if ((opcode >= 0x48U && opcode < 0x50U) ||
      (opcode >= 0x58U && opcode < 0x60U)) {
    const auto minimum = opcode < 0x50U ? 4U : 5U;
    if (words.size() < minimum)
      return 0U;
    for (std::size_t index = minimum - 1U; index < words.size(); ++index) {
      if ((words[index] & 0xf000f000U) == 0x50005000U)
        return index + 1U;
    }
    return 0U;
  }
  if (opcode >= 0xa0U && opcode < 0xc0U) {
    if (words.size() < 3U)
      return 0U;
    const auto width = (((words[2U] & 0xffffU) - 1U) & 0x03ffU) + 1U;
    const auto height = ((((words[2U] >> 16U) & 0xffffU) - 1U) & 0x01ffU) + 1U;
    return 3U + (static_cast<std::size_t>(width) * height + 1U) / 2U;
  }
  if (opcode >= 0xc0U && opcode < 0xe0U)
    return 3U;
  return fixedCommandLength(opcode);
}

[[nodiscard]] constexpr std::int16_t packedX(std::uint32_t sxy) noexcept {
  return static_cast<std::int16_t>(sxy);
}

[[nodiscard]] constexpr std::int16_t packedY(std::uint32_t sxy) noexcept {
  return static_cast<std::int16_t>(sxy >> 16U);
}

[[nodiscard]] std::uint64_t meshVertexId(const TspPosition &position) noexcept {
  const auto packed =
      (static_cast<std::uint64_t>(static_cast<std::uint16_t>(position.x))
       << 32U) |
      (static_cast<std::uint64_t>(static_cast<std::uint16_t>(position.y))
       << 16U) |
      static_cast<std::uint16_t>(position.z);
  return packed + 1U;
}

[[nodiscard]] sf::psx::GteProjectedVertex
makeProjection(std::uint32_t packed_sxy, const TspPosition &position,
               const NativeViewPosition &view, const NativeWorldCamera &camera,
               std::uint32_t record_base, std::uint8_t buffer,
               std::uint8_t corner) noexcept {
  sf::psx::GteProjectedVertex projection{};
  projection.packed_sxy = packed_sxy;
  projection.view_x = static_cast<float>(view.x);
  projection.view_y = static_cast<float>(view.y);
  projection.view_z = static_cast<float>(view.z);
  projection.screen_h = static_cast<float>(camera.projection_h);
  projection.screen_offset_x = static_cast<float>(camera.offset_x);
  projection.screen_offset_y = static_cast<float>(camera.offset_y);
  if (std::abs(view.z) <= minimum_projection_depth) {
    projection.screen_x = static_cast<float>(packedX(packed_sxy));
    projection.screen_y = static_cast<float>(packedY(packed_sxy));
  } else {
    projection.screen_x = static_cast<float>(
        camera.offset_x + camera.projection_h * view.x / view.z);
    projection.screen_y = static_cast<float>(
        camera.offset_y + camera.projection_h * view.y / view.z);
  }
  projection.valid = true;
  projection.exact_transform = true;
  projection.fractional_transform = true;
  projection.source_vertex_id =
      (static_cast<std::uint64_t>(record_base) << 8U) |
      (static_cast<std::uint64_t>(buffer) << 4U) |
      static_cast<std::uint64_t>(corner + 1U);
  projection.mesh_vertex_id = meshVertexId(position);
  projection.transform_lineage = camera.camera_revision;
  projection.projection_epoch = camera.projection_revision;
  return projection;
}

[[nodiscard]] const NativeWorldFrameSnapshot *
snapshotForWord(std::span<const NativeWorldFrameSnapshotSegment> snapshots,
                std::size_t word_index) noexcept {
  const NativeWorldFrameSnapshot *selected{};
  for (const auto &segment : snapshots) {
    if (segment.word_begin > word_index) {
      break;
    }
    selected = &segment.snapshot;
  }
  return selected;
}

} // namespace

NativeWorldFrameSnapshot
captureNativeWorldFrameSnapshot(const sf::psx::R3000Runtime &cpu,
                                const NativeWorldCamera &camera) noexcept {
  NativeWorldFrameSnapshot snapshot{};
  if (!camera.valid) {
    return snapshot;
  }
  snapshot.camera = camera;
  std::uint32_t packet_buffer{};
  if (!cpu.read32(world_packet_buffer_selector, packet_buffer) ||
      packet_buffer > 1U) {
    return {};
  }
  snapshot.packet_buffer = static_cast<std::uint8_t>(packet_buffer);
  for (std::size_t record{}; record < snapshot.positions.size(); ++record) {
    const auto record_base =
        world_record_base +
        static_cast<std::uint32_t>(record) * world_record_stride;
    for (std::size_t corner{}; corner < snapshot.positions[record].size();
         ++corner) {
      if (!readPosition(cpu, record_base, static_cast<std::uint8_t>(corner),
                        snapshot.positions[record][corner])) {
        return {};
      }
    }
  }
  snapshot.valid = true;
  return snapshot;
}

NativeWorldFrameStats overrideNativeWorldFrameProjections(
    std::span<const std::uint32_t> words,
    std::span<const std::uint32_t> source_addresses,
    std::span<NativeWorldProjectionOverride> projections,
    std::span<const NativeWorldFrameSnapshotSegment> snapshots) noexcept {
  NativeWorldFrameStats stats{};
  if (words.size() != source_addresses.size()) {
    return stats;
  }
  for (std::size_t index{}; index < snapshots.size(); ++index) {
    if (snapshots[index].word_begin > words.size() ||
        (index != 0U &&
         snapshots[index - 1U].word_begin >= snapshots[index].word_begin)) {
      return stats;
    }
  }

  for (std::size_t consumed{}; consumed < words.size();) {
    const auto length = commandLength(words.subspan(consumed));
    if (length == 0U || consumed + length > words.size())
      break;
    const auto opcode = static_cast<std::uint8_t>(words[consumed] >> 24U);
    if (opcode >= 0x20U && opcode < 0x40U &&
        source_addresses[consumed] != invalid_source_address) {
      const auto source = source_addresses[consumed] & physical_address_mask;
      const auto page = static_cast<std::size_t>(source >> 12U);
      if (page < stats.polygon_source_pages.size()) {
        ++stats.polygon_source_pages[page];
      }
      ++stats.polygon_commands;
    }
    consumed += length;
  }

  for (std::size_t index{}; index < words.size(); ++index) {
    if (((words[index] >> 24U) & 0xfcU) != 0x2cU) {
      continue;
    }
    const auto packet = worldPacket(source_addresses[index]);
    if (!packet.valid) {
      continue;
    }
    ++stats.candidate_quads;

    if (index + 9U > words.size() ||
        !commandIsComplete(source_addresses, index) ||
        stats.coordinate_words + coordinate_words.size() > projections.size()) {
      ++stats.incomplete_quads;
      continue;
    }

    const auto *snapshot = snapshotForWord(snapshots, index);
    if (snapshot == nullptr || !snapshot->valid || !snapshot->camera.valid ||
        snapshot->packet_buffer != packet.buffer) {
      ++stats.invalid_geometry_quads;
      continue;
    }
    const auto &camera = snapshot->camera;
    const auto record_index = static_cast<std::size_t>(
        (packet.record_base - world_record_base) / world_record_stride);
    const auto &positions = snapshot->positions[record_index];
    std::array<NativeViewPosition, 4U> views{};
    bool coherent = true;
    for (std::uint8_t corner{}; corner < 4U; ++corner) {
      views[corner] = transformNativeWorldPosition(camera, positions[corner]);
      coherent = std::isfinite(views[corner].x) &&
                 std::isfinite(views[corner].y) &&
                 std::isfinite(views[corner].z);
      if (!coherent) {
        break;
      }
    }
    if (!coherent) {
      ++stats.invalid_geometry_quads;
      continue;
    }

    double quad_witness_error{};
    for (std::uint8_t corner{}; corner < 4U; ++corner) {
      const auto coordinate = coordinate_words[corner];
      const auto zero_depth =
          std::abs(views[corner].z) <= minimum_projection_depth;
      const auto screen_x =
          zero_depth ? static_cast<double>(packedX(words[index + coordinate]))
                     : camera.offset_x + camera.projection_h * views[corner].x /
                                             views[corner].z;
      const auto screen_y =
          zero_depth ? static_cast<double>(packedY(words[index + coordinate]))
                     : camera.offset_y + camera.projection_h * views[corner].y /
                                             views[corner].z;
      const auto error =
          std::max(std::abs(screen_x - packedX(words[index + coordinate])),
                   std::abs(screen_y - packedY(words[index + coordinate])));
      quad_witness_error = std::max(quad_witness_error, error);
    }
    stats.maximum_witness_error = std::max(
        stats.maximum_witness_error, static_cast<float>(quad_witness_error));
    if (quad_witness_error > maximum_native_world_witness_error) {
      ++stats.witness_rejected_quads;
      continue;
    }

    for (std::uint8_t corner{}; corner < 4U; ++corner) {
      const auto coordinate = coordinate_words[corner];
      projections[stats.coordinate_words + corner] = {
          index + coordinate,
          makeProjection(words[index + coordinate], positions[corner],
                         views[corner], camera, packet.record_base,
                         packet.buffer, corner)};
    }
    stats.coordinate_words += 4U;
    ++stats.complete_quads;
    index += 8U;
  }
  return stats;
}

NativeWorldFrameStats overrideNativeWorldFrameProjections(
    std::span<const std::uint32_t> words,
    std::span<const std::uint32_t> source_addresses,
    std::span<NativeWorldProjectionOverride> projections,
    const NativeWorldFrameSnapshot &snapshot) noexcept {
  const std::array snapshots{NativeWorldFrameSnapshotSegment{0U, snapshot}};
  return overrideNativeWorldFrameProjections(words, source_addresses,
                                             projections, snapshots);
}

} // namespace mohu
