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
      previous_polygon.context != current_polygon.context) {
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

[[nodiscard]] bool uniqueKeys(std::span<const KeyedPolygon> polygons) noexcept {
  for (std::size_t index = 1U; index < polygons.size(); ++index) {
    if (polygons[index - 1U].key == polygons[index].key) {
      return false;
    }
  }
  return true;
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
  auto relative_coordinate_words = std::size_t{};
  auto relative_proof = previous_relative.size() == current_relative.size() &&
                        uniqueKeys(previous_relative) &&
                        uniqueKeys(current_relative);
  if (relative_proof) {
    for (std::size_t index{}; index < previous_relative.size(); ++index) {
      const auto &left_key = previous_relative[index];
      const auto &right_key = current_relative[index];
      const auto &left = previous.polygons[left_key.polygon];
      const auto &right = current.polygons[right_key.polygon];
      if (left_key.key != right_key.key ||
          !staticPayloadEqual(previous, left, current, right) ||
          !relativeSourceTopologyEqual(previous, left, current, right)) {
        relative_proof = false;
        break;
      }
      const auto prior_raw = previous_matches[left_key.polygon];
      const auto current_raw_match = current_matches[right_key.polygon];
      if ((prior_raw != std::numeric_limits<std::size_t>::max() &&
           prior_raw != right_key.polygon) ||
          (current_raw_match != std::numeric_limits<std::size_t>::max() &&
           current_raw_match != left_key.polygon)) {
        relative_proof = false;
        break;
      }
      relative_coordinate_words += left.layout.vertex_count;
    }
  }
  relative_proof =
      relative_proof &&
      previous_relative.size() >=
          GpuPrimitiveMatchReport::minimum_relative_polygons &&
      relative_coordinate_words >=
          GpuPrimitiveMatchReport::minimum_relative_coordinate_words;
  report.relative_provenance_proven = relative_proof;

  if (relative_proof) {
    for (std::size_t index{}; index < previous_relative.size(); ++index) {
      const auto left = previous_relative[index].polygon;
      const auto right = current_relative[index].polygon;
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

} // namespace mohu
