#include "mohu/gpu_primitive_matcher.hpp"

#include "sf/psx/gp0_command.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace mohu {
namespace {

constexpr std::size_t maximum_snapshot_words = 1024U * 512U / 2U + 3U;
constexpr std::uint32_t ram_address_mask = 0x001ffffcU;

struct DrawContext {
  std::uint32_t draw_mode{};
  std::uint32_t texture_window{};
  std::uint32_t draw_area_top_left{};
  std::uint32_t draw_area_bottom_right{};
  std::uint32_t draw_offset{};
  std::uint32_t mask_setting{};

  friend bool operator==(const DrawContext &, const DrawContext &) = default;
};

struct ParsedPolygon {
  std::size_t command_word{};
  sf::psx::Gp0CommandLayout layout{};
  DrawContext context{};
  std::array<std::size_t, 4U> coordinate_words{};
  std::uint64_t raw_key{};
  std::uint32_t relative_key{};
  bool raw_eligible{};
  bool relative_eligible{};
};

struct ParsedSnapshot {
  GpuPrimitiveSnapshotView source{};
  std::vector<ParsedPolygon> polygons;
  bool valid{};
};

struct KeyedPolygon {
  std::uint64_t key{};
  std::size_t polygon{};
};

[[nodiscard]] bool
sourceAddressValid(const sf::psx::GpuDmaWordSource &source) noexcept {
  return source.valid() && source.kind != sf::psx::GpuDmaSourceKind::none &&
         source.word_address <= ram_address_mask &&
         (source.word_address & 3U) == 0U;
}

[[nodiscard]] std::uint32_t
relativeAddress(const sf::psx::GpuDmaWordSource &source) noexcept {
  return (source.word_address - source.transfer_root) & ram_address_mask;
}

void updateDrawContext(DrawContext &context, std::uint8_t opcode,
                       std::uint32_t command) noexcept {
  switch (opcode) {
  case 0xe1U:
    context.draw_mode = command & 0x000003ffU;
    break;
  case 0xe2U:
    context.texture_window = command & 0x000fffffU;
    break;
  case 0xe3U:
    context.draw_area_top_left = command & 0x0007ffffU;
    break;
  case 0xe4U:
    context.draw_area_bottom_right = command & 0x0007ffffU;
    break;
  case 0xe5U:
    context.draw_offset = command & 0x003fffffU;
    break;
  case 0xe6U:
    context.mask_setting = command & 3U;
    break;
  default:
    break;
  }
}

[[nodiscard]] std::int32_t signedDrawOffset(std::uint32_t value) noexcept {
  value &= 0x07ffU;
  return static_cast<std::int32_t>(value >= 0x0400U ? value - 0x0800U : value);
}

[[nodiscard]] bool
drawContextsEqual(GpuPrimitiveSnapshotView previous,
                  const DrawContext &previous_context,
                  GpuPrimitiveSnapshotView current,
                  const DrawContext &current_context) noexcept {
  if (previous_context.draw_mode != current_context.draw_mode ||
      previous_context.texture_window != current_context.texture_window ||
      previous_context.mask_setting != current_context.mask_setting) {
    return false;
  }
  const auto local_area_x = [](std::uint32_t command,
                               std::int32_t origin) noexcept {
    return static_cast<std::int32_t>(command & 0x03ffU) - origin;
  };
  const auto local_area_y = [](std::uint32_t command,
                               std::int32_t origin) noexcept {
    return static_cast<std::int32_t>((command >> 10U) & 0x01ffU) - origin;
  };
  const auto local_offset_x = [](std::uint32_t command,
                                 std::int32_t origin) noexcept {
    return signedDrawOffset(command) - origin;
  };
  const auto local_offset_y = [](std::uint32_t command,
                                 std::int32_t origin) noexcept {
    return signedDrawOffset(command >> 11U) - origin;
  };
  return local_area_x(previous_context.draw_area_top_left,
                      previous.raster_origin_x) ==
             local_area_x(current_context.draw_area_top_left,
                          current.raster_origin_x) &&
         local_area_y(previous_context.draw_area_top_left,
                      previous.raster_origin_y) ==
             local_area_y(current_context.draw_area_top_left,
                          current.raster_origin_y) &&
         local_area_x(previous_context.draw_area_bottom_right,
                      previous.raster_origin_x) ==
             local_area_x(current_context.draw_area_bottom_right,
                          current.raster_origin_x) &&
         local_area_y(previous_context.draw_area_bottom_right,
                      previous.raster_origin_y) ==
             local_area_y(current_context.draw_area_bottom_right,
                          current.raster_origin_y) &&
         local_offset_x(previous_context.draw_offset,
                        previous.raster_origin_x) ==
             local_offset_x(current_context.draw_offset,
                            current.raster_origin_x) &&
         local_offset_y(previous_context.draw_offset,
                        previous.raster_origin_y) ==
             local_offset_y(current_context.draw_offset,
                            current.raster_origin_y);
}

[[nodiscard]] bool primitiveSourcesValid(GpuPrimitiveSnapshotView snapshot,
                                         std::size_t command_word,
                                         std::size_t word_count) noexcept {
  for (std::size_t word{}; word < word_count; ++word) {
    if (!sourceAddressValid(snapshot.dma_sources[command_word + word])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool relativeSourcesValid(GpuPrimitiveSnapshotView snapshot,
                                        std::size_t command_word,
                                        std::size_t word_count) noexcept {
  const auto &first = snapshot.dma_sources[command_word];
  if (!primitiveSourcesValid(snapshot, command_word, word_count) ||
      first.kind != sf::psx::GpuDmaSourceKind::linked_list ||
      first.transfer_root == sf::psx::GpuDmaWordSource::invalid_address ||
      first.transfer_root > ram_address_mask ||
      (first.transfer_root & 3U) != 0U) {
    return false;
  }
  for (std::size_t word = 1U; word < word_count; ++word) {
    const auto &source = snapshot.dma_sources[command_word + word];
    if (source.kind != sf::psx::GpuDmaSourceKind::linked_list ||
        source.transfer_root != first.transfer_root) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] ParsedSnapshot parseSnapshot(GpuPrimitiveSnapshotView snapshot) {
  ParsedSnapshot result{.source = snapshot};
  if (snapshot.words.size() != snapshot.dma_sources.size() ||
      snapshot.words.size() > maximum_snapshot_words) {
    return result;
  }

  DrawContext context{};
  auto consumed = std::size_t{};
  while (consumed < snapshot.words.size()) {
    const auto remaining = snapshot.words.subspan(consumed);
    const auto layout = sf::psx::gp0CommandLayout(remaining);
    if (!layout.complete()) {
      return result;
    }
    if (layout.command_class == sf::psx::Gp0CommandClass::polygon) {
      ParsedPolygon polygon{
          .command_word = consumed,
          .layout = layout,
          .context = context,
      };
      for (std::size_t vertex{}; vertex < layout.vertex_count; ++vertex) {
        polygon.coordinate_words[vertex] =
            consumed + layout.coordinate_words[vertex];
      }
      if (primitiveSourcesValid(snapshot, consumed, layout.word_count)) {
        const auto &first = snapshot.dma_sources[consumed];
        polygon.raw_eligible = true;
        polygon.raw_key = (static_cast<std::uint64_t>(first.kind) << 32U) |
                          first.word_address;
      }
      if (relativeSourcesValid(snapshot, consumed, layout.word_count)) {
        polygon.relative_eligible = true;
        polygon.relative_key = relativeAddress(snapshot.dma_sources[consumed]);
      }
      result.polygons.push_back(polygon);
    }
    updateDrawContext(context, layout.opcode, remaining.front());
    consumed += layout.word_count;
  }
  result.valid = true;
  return result;
}

[[nodiscard]] bool isDynamicColorWord(const sf::psx::Gp0CommandLayout &layout,
                                      std::size_t word) noexcept {
  if (word == 0U) {
    return true;
  }
  if (!layout.gouraud) {
    return false;
  }
  return layout.textured ? word % 3U == 0U : word % 2U == 0U;
}

[[nodiscard]] bool isCoordinateWord(const sf::psx::Gp0CommandLayout &layout,
                                    std::size_t word) noexcept {
  for (std::size_t vertex{}; vertex < layout.vertex_count; ++vertex) {
    if (layout.coordinate_words[vertex] == word) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool
staticPayloadEqual(const ParsedSnapshot &previous,
                   const ParsedPolygon &previous_polygon,
                   const ParsedSnapshot &current,
                   const ParsedPolygon &current_polygon) noexcept {
  const auto &left = previous_polygon.layout;
  const auto &right = current_polygon.layout;
  if (left.opcode != right.opcode || left.word_count != right.word_count ||
      left.coordinate_words != right.coordinate_words ||
      left.vertex_count != right.vertex_count ||
      !drawContextsEqual(previous.source, previous_polygon.context,
                         current.source, current_polygon.context)) {
    return false;
  }
  for (std::size_t word{}; word < left.word_count; ++word) {
    if (isCoordinateWord(left, word) || isDynamicColorWord(left, word)) {
      continue;
    }
    if (previous.source.words[previous_polygon.command_word + word] !=
        current.source.words[current_polygon.command_word + word]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
rawSourceTopologyEqual(const ParsedSnapshot &previous,
                       const ParsedPolygon &previous_polygon,
                       const ParsedSnapshot &current,
                       const ParsedPolygon &current_polygon) noexcept {
  for (std::size_t word{}; word < previous_polygon.layout.word_count; ++word) {
    const auto &left =
        previous.source.dma_sources[previous_polygon.command_word + word];
    const auto &right =
        current.source.dma_sources[current_polygon.command_word + word];
    if (left.kind != right.kind || left.word_address != right.word_address) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
relativeSourceTopologyEqual(const ParsedSnapshot &previous,
                            const ParsedPolygon &previous_polygon,
                            const ParsedSnapshot &current,
                            const ParsedPolygon &current_polygon) noexcept {
  for (std::size_t word{}; word < previous_polygon.layout.word_count; ++word) {
    const auto &left =
        previous.source.dma_sources[previous_polygon.command_word + word];
    const auto &right =
        current.source.dma_sources[current_polygon.command_word + word];
    if (left.kind != sf::psx::GpuDmaSourceKind::linked_list ||
        right.kind != sf::psx::GpuDmaSourceKind::linked_list ||
        relativeAddress(left) != relativeAddress(right)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::vector<KeyedPolygon>
keyedPolygons(const ParsedSnapshot &snapshot, bool relative) {
  std::vector<KeyedPolygon> result;
  result.reserve(snapshot.polygons.size());
  for (std::size_t index{}; index < snapshot.polygons.size(); ++index) {
    const auto &polygon = snapshot.polygons[index];
    if (relative ? polygon.relative_eligible : polygon.raw_eligible) {
      result.push_back(
          {relative ? polygon.relative_key : polygon.raw_key, index});
    }
  }
  std::sort(result.begin(), result.end(),
            [](const KeyedPolygon &left, const KeyedPolygon &right) {
              return left.key < right.key;
            });
  return result;
}

[[nodiscard]] GpuPrimitiveMatch makeMatch(const ParsedPolygon &previous,
                                          const ParsedPolygon &current,
                                          GpuPrimitiveMatchKind kind) noexcept {
  return {
      .previous_command_word = previous.command_word,
      .current_command_word = current.command_word,
      .previous_coordinate_words = previous.coordinate_words,
      .current_coordinate_words = current.coordinate_words,
      .vertex_count = previous.layout.vertex_count,
      .kind = kind,
  };
}

[[nodiscard]] std::uint64_t
semanticFingerprint(const ParsedSnapshot &snapshot,
                    const ParsedPolygon &polygon) noexcept {
  auto hash = std::uint64_t{0xcbf29ce484222325ULL};
  const auto mix = [&hash](std::uint64_t value) noexcept {
    hash ^= value;
    hash *= 0x100000001b3ULL;
  };
  mix(polygon.layout.opcode);
  mix(polygon.layout.word_count);
  mix(polygon.layout.vertex_count);
  for (std::size_t vertex{}; vertex < polygon.layout.vertex_count; ++vertex) {
    mix(polygon.layout.coordinate_words[vertex]);
  }
  for (std::size_t word{}; word < polygon.layout.word_count; ++word) {
    if (isCoordinateWord(polygon.layout, word) ||
        isDynamicColorWord(polygon.layout, word)) {
      continue;
    }
    mix(snapshot.source.words[polygon.command_word + word]);
  }
  return hash;
}

[[nodiscard]] std::vector<std::array<std::size_t, 2U>>
matchUniqueSemanticPolygons(const ParsedSnapshot &previous,
                            const ParsedSnapshot &current) {
  auto previous_keys = std::vector<KeyedPolygon>{};
  auto current_keys = std::vector<KeyedPolygon>{};
  previous_keys.reserve(previous.polygons.size());
  current_keys.reserve(current.polygons.size());
  for (std::size_t index{}; index < previous.polygons.size(); ++index) {
    previous_keys.push_back(
        {semanticFingerprint(previous, previous.polygons[index]), index});
  }
  for (std::size_t index{}; index < current.polygons.size(); ++index) {
    current_keys.push_back(
        {semanticFingerprint(current, current.polygons[index]), index});
  }
  const auto by_key = [](const KeyedPolygon &left,
                         const KeyedPolygon &right) noexcept {
    return left.key < right.key;
  };
  std::ranges::sort(previous_keys, by_key);
  std::ranges::sort(current_keys, by_key);

  auto matches = std::vector<std::array<std::size_t, 2U>>{};
  auto previous_begin = std::size_t{};
  auto current_begin = std::size_t{};
  while (previous_begin < previous_keys.size() &&
         current_begin < current_keys.size()) {
    const auto previous_key = previous_keys[previous_begin].key;
    const auto current_key = current_keys[current_begin].key;
    if (previous_key < current_key) {
      ++previous_begin;
      continue;
    }
    if (current_key < previous_key) {
      ++current_begin;
      continue;
    }
    const auto previous_end = static_cast<std::size_t>(std::ranges::find_if(
        previous_keys.begin() + previous_begin, previous_keys.end(),
        [previous_key](const auto &entry) { return entry.key != previous_key; }) -
                                                       previous_keys.begin());
    const auto current_end = static_cast<std::size_t>(std::ranges::find_if(
        current_keys.begin() + current_begin, current_keys.end(),
        [current_key](const auto &entry) { return entry.key != current_key; }) -
                                                     current_keys.begin());
    if (previous_end - previous_begin == 1U &&
        current_end - current_begin == 1U) {
      const auto left = previous_keys[previous_begin].polygon;
      const auto right = current_keys[current_begin].polygon;
      if (staticPayloadEqual(previous, previous.polygons[left], current,
                             current.polygons[right])) {
        matches.push_back({left, right});
      }
    }
    previous_begin = previous_end;
    current_begin = current_end;
  }
  return matches;
}

[[nodiscard]] std::vector<std::array<std::size_t, 2U>>
matchSemanticSequence(const ParsedSnapshot &previous,
                      const ParsedSnapshot &current) {
  const auto previous_count = previous.polygons.size();
  const auto current_count = current.polygons.size();
  if (previous_count == 0U || current_count == 0U) {
    return {};
  }
  const auto required_matches = std::max(previous_count - previous_count / 4U,
                                         current_count - current_count / 4U);
  if (required_matches > std::min(previous_count, current_count)) {
    return {};
  }
  const auto maximum_edit_distance =
      previous_count + current_count - required_matches * 2U;
  // Bound retained history by bytes rather than scene complexity. Frames with
  // a small edit distance remain eligible regardless of polygon count.
  constexpr auto maximum_trace_entries =
      std::size_t{8U * 1024U * 1024U / sizeof(std::int32_t)};
  const auto trace_rows = maximum_edit_distance + 1U;
  if (trace_rows > maximum_trace_entries * 2U / (trace_rows + 1U)) {
    return {};
  }
  const auto trace_capacity = trace_rows * (trace_rows + 1U) / 2U;
  const auto maximum_diagonal = previous_count + current_count;
  const auto offset = static_cast<std::ptrdiff_t>(maximum_diagonal + 1U);
  std::vector<std::ptrdiff_t> frontier(maximum_diagonal * 2U + 3U, -1);
  frontier[static_cast<std::size_t>(offset + 1)] = 0;
  // Store only the active diagonals for each completed edit distance. The old
  // full-frontier snapshots copied O((N+M)D) words and dominated MOHU's GPU
  // submit time whenever two independently allocated packet lists differed.
  std::vector<std::size_t> trace_offsets;
  trace_offsets.reserve(maximum_edit_distance);
  std::vector<std::int32_t> trace;
  trace.reserve(trace_capacity);

  std::vector<std::uint64_t> previous_fingerprints(previous_count);
  std::vector<std::uint64_t> current_fingerprints(current_count);
  for (std::size_t index{}; index < previous_count; ++index) {
    previous_fingerprints[index] =
        semanticFingerprint(previous, previous.polygons[index]);
  }
  for (std::size_t index{}; index < current_count; ++index) {
    current_fingerprints[index] =
        semanticFingerprint(current, current.polygons[index]);
  }
  const auto equal = [&](std::size_t left, std::size_t right) noexcept {
    return previous_fingerprints[left] == current_fingerprints[right] &&
           staticPayloadEqual(previous, previous.polygons[left], current,
                              current.polygons[right]);
  };

  for (std::ptrdiff_t distance{};
       distance <= static_cast<std::ptrdiff_t>(maximum_edit_distance);
       ++distance) {
    for (auto diagonal = -distance; diagonal <= distance; diagonal += 2) {
      const auto slot = static_cast<std::size_t>(offset + diagonal);
      auto x =
          diagonal == -distance || (diagonal != distance &&
                                    frontier[slot - 1U] < frontier[slot + 1U])
              ? frontier[slot + 1U]
              : frontier[slot - 1U] + 1;
      auto y = x - diagonal;
      while (x < static_cast<std::ptrdiff_t>(previous_count) &&
             y < static_cast<std::ptrdiff_t>(current_count) &&
             equal(static_cast<std::size_t>(x), static_cast<std::size_t>(y))) {
        ++x;
        ++y;
      }
      frontier[slot] = x;
      if (x < static_cast<std::ptrdiff_t>(previous_count) ||
          y < static_cast<std::ptrdiff_t>(current_count)) {
        continue;
      }

      std::vector<std::array<std::size_t, 2U>> matches;
      matches.reserve((previous_count + current_count -
                       static_cast<std::size_t>(distance)) /
                      2U);
      auto back_x = static_cast<std::ptrdiff_t>(previous_count);
      auto back_y = static_cast<std::ptrdiff_t>(current_count);
      for (auto back_distance = distance; back_distance > 0; --back_distance) {
        const auto prior_distance = back_distance - 1;
        const auto back_diagonal = back_x - back_y;
        const auto prior_value = [&](std::ptrdiff_t diagonal) {
          const auto row = static_cast<std::size_t>(prior_distance);
          const auto column =
              static_cast<std::size_t>((diagonal + prior_distance) / 2);
          return static_cast<std::ptrdiff_t>(
              trace[trace_offsets[row] + column]);
        };
        const auto prior_diagonal = back_diagonal == -back_distance ||
                                            (back_diagonal != back_distance &&
                                             prior_value(back_diagonal - 1) <
                                                 prior_value(back_diagonal + 1))
                                        ? back_diagonal + 1
                                        : back_diagonal - 1;
        const auto prior_x = prior_value(prior_diagonal);
        const auto prior_y = prior_x - prior_diagonal;
        while (back_x > prior_x && back_y > prior_y) {
          matches.push_back({static_cast<std::size_t>(back_x - 1),
                             static_cast<std::size_t>(back_y - 1)});
          --back_x;
          --back_y;
        }
        back_x = prior_x;
        back_y = prior_y;
      }
      while (back_x > 0 && back_y > 0) {
        matches.push_back({static_cast<std::size_t>(back_x - 1),
                           static_cast<std::size_t>(back_y - 1)});
        --back_x;
        --back_y;
      }
      std::ranges::reverse(matches);
      return matches;
    }
    trace_offsets.push_back(trace.size());
    for (auto diagonal = -distance; diagonal <= distance; diagonal += 2) {
      const auto x = frontier[static_cast<std::size_t>(offset + diagonal)];
      trace.push_back(static_cast<std::int32_t>(x));
    }
  }
  return {};
}

} // namespace

GpuPrimitiveMatchReport
matchGpuPrimitiveSnapshots(GpuPrimitiveSnapshotView previous_view,
                           GpuPrimitiveSnapshotView current_view) {
  GpuPrimitiveMatchReport report;
  const auto previous = parseSnapshot(previous_view);
  const auto current = parseSnapshot(current_view);
  report.previous_polygon_count = previous.polygons.size();
  report.current_polygon_count = current.polygons.size();
  report.input_valid = previous.valid && current.valid;
  if (!report.input_valid) {
    return report;
  }

  std::vector<std::size_t> previous_matches(
      previous.polygons.size(), std::numeric_limits<std::size_t>::max());
  std::vector<std::size_t> current_matches(
      current.polygons.size(), std::numeric_limits<std::size_t>::max());
  const auto previous_raw = keyedPolygons(previous, false);
  const auto current_raw = keyedPolygons(current, false);
  auto previous_index = std::size_t{};
  auto current_index = std::size_t{};
  while (previous_index < previous_raw.size() &&
         current_index < current_raw.size()) {
    if (previous_raw[previous_index].key < current_raw[current_index].key) {
      ++previous_index;
      continue;
    }
    if (current_raw[current_index].key < previous_raw[previous_index].key) {
      ++current_index;
      continue;
    }
    const auto key = previous_raw[previous_index].key;
    auto previous_end = previous_index + 1U;
    auto current_end = current_index + 1U;
    while (previous_end < previous_raw.size() &&
           previous_raw[previous_end].key == key) {
      ++previous_end;
    }
    while (current_end < current_raw.size() &&
           current_raw[current_end].key == key) {
      ++current_end;
    }
    if (previous_end == previous_index + 1U &&
        current_end == current_index + 1U) {
      const auto left = previous_raw[previous_index].polygon;
      const auto right = current_raw[current_index].polygon;
      if (staticPayloadEqual(previous, previous.polygons[left], current,
                             current.polygons[right]) &&
          rawSourceTopologyEqual(previous, previous.polygons[left], current,
                                 current.polygons[right])) {
        previous_matches[left] = right;
        current_matches[right] = left;
        report.matches.push_back(makeMatch(previous.polygons[left],
                                           current.polygons[right],
                                           GpuPrimitiveMatchKind::raw_address));
        ++report.raw_match_count;
      }
    }
    previous_index = previous_end;
    current_index = current_end;
  }

  const auto previous_relative = keyedPolygons(previous, true);
  const auto current_relative = keyedPolygons(current, true);
  report.previous_relative_polygon_count = previous_relative.size();
  report.current_relative_polygon_count = current_relative.size();

  struct RelativeCandidate {
    std::size_t previous{};
    std::size_t current{};
  };
  std::vector<RelativeCandidate> relative_candidates;
  relative_candidates.reserve(
      std::min(previous_relative.size(), current_relative.size()));
  auto relative_coordinate_words = std::size_t{};
  auto relative_previous_index = std::size_t{};
  auto relative_current_index = std::size_t{};
  while (relative_previous_index < previous_relative.size() &&
         relative_current_index < current_relative.size()) {
    if (previous_relative[relative_previous_index].key <
        current_relative[relative_current_index].key) {
      ++relative_previous_index;
      continue;
    }
    if (current_relative[relative_current_index].key <
        previous_relative[relative_previous_index].key) {
      ++relative_current_index;
      continue;
    }

    const auto key = previous_relative[relative_previous_index].key;
    auto previous_end = relative_previous_index + 1U;
    auto current_end = relative_current_index + 1U;
    while (previous_end < previous_relative.size() &&
           previous_relative[previous_end].key == key) {
      ++previous_end;
    }
    while (current_end < current_relative.size() &&
           current_relative[current_end].key == key) {
      ++current_end;
    }
    if (previous_end == relative_previous_index + 1U &&
        current_end == relative_current_index + 1U) {
      ++report.relative_common_key_count;
      const auto left_index =
          previous_relative[relative_previous_index].polygon;
      const auto right_index = current_relative[relative_current_index].polygon;
      const auto &left = previous.polygons[left_index];
      const auto &right = current.polygons[right_index];
      if (!staticPayloadEqual(previous, left, current, right)) {
        ++report.relative_static_reject_count;
      } else if (!relativeSourceTopologyEqual(previous, left, current, right)) {
        ++report.relative_topology_reject_count;
      } else {
        const auto prior_raw = previous_matches[left_index];
        const auto current_raw_match = current_matches[right_index];
        if ((prior_raw != std::numeric_limits<std::size_t>::max() &&
             prior_raw != right_index) ||
            (current_raw_match != std::numeric_limits<std::size_t>::max() &&
             current_raw_match != left_index)) {
          ++report.relative_topology_reject_count;
        } else {
          relative_candidates.push_back({left_index, right_index});
          relative_coordinate_words += left.layout.vertex_count;
        }
      }
    }
    relative_previous_index = previous_end;
    relative_current_index = current_end;
  }
  report.relative_candidate_count = relative_candidates.size();

  const auto covers_three_quarters = [](std::size_t matches,
                                        std::size_t total) noexcept {
    return total != 0U && matches >= total - total / 4U;
  };
  const auto relative_proof =
      relative_candidates.size() >=
          GpuPrimitiveMatchReport::minimum_relative_polygons &&
      relative_coordinate_words >=
          GpuPrimitiveMatchReport::minimum_relative_coordinate_words &&
      covers_three_quarters(relative_candidates.size(),
                            previous_relative.size()) &&
      covers_three_quarters(relative_candidates.size(),
                            current_relative.size());
  report.relative_provenance_proven = relative_proof;

  if (relative_proof) {
    for (const auto &candidate : relative_candidates) {
      const auto left = candidate.previous;
      const auto right = candidate.current;
      if (previous_matches[left] != std::numeric_limits<std::size_t>::max() ||
          current_matches[right] != std::numeric_limits<std::size_t>::max()) {
        continue;
      }
      report.matches.push_back(
          makeMatch(previous.polygons[left], current.polygons[right],
                    GpuPrimitiveMatchKind::linked_list_relative));
      ++report.relative_match_count;
    }
  }

  std::sort(report.matches.begin(), report.matches.end(),
            [](const GpuPrimitiveMatch &left, const GpuPrimitiveMatch &right) {
              return left.current_command_word < right.current_command_word;
            });
  return report;
}

GpuPrimitiveMatchReport
matchGpuPrimitiveSequences(GpuPrimitiveSnapshotView previous_view,
                           GpuPrimitiveSnapshotView current_view) {
  GpuPrimitiveMatchReport report;
  const auto previous = parseSnapshot(previous_view);
  const auto current = parseSnapshot(current_view);
  report.previous_polygon_count = previous.polygons.size();
  report.current_polygon_count = current.polygons.size();
  report.input_valid = previous.valid && current.valid;
  if (!report.input_valid) {
    return report;
  }
  auto sequence_matches = matchSemanticSequence(previous, current);
  const auto unique_matches = matchUniqueSemanticPolygons(previous, current);
  // Ordering-table insertion can reorder otherwise unchanged primitives as
  // their depth changes. A fingerprint that occurs exactly once in both
  // frames is a stronger identity witness than draw order, so prefer the
  // unique set whenever it recovers more of the scene.
  if (unique_matches.size() > sequence_matches.size()) {
    sequence_matches = unique_matches;
  }
  report.matches.reserve(sequence_matches.size());
  for (const auto &indices : sequence_matches) {
    const auto &left = previous.polygons[indices[0U]];
    const auto &right = current.polygons[indices[1U]];
    report.matches.push_back(
        makeMatch(left, right, GpuPrimitiveMatchKind::semantic_sequence));
  }
  return report;
}

} // namespace mohu
