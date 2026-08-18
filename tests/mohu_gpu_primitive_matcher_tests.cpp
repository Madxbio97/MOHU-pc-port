#include "mohu/gpu_primitive_matcher.hpp"
#include "sf/psx/gp0_command.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using Source = sf::psx::GpuDmaWordSource;
using SourceKind = sf::psx::GpuDmaSourceKind;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

struct Snapshot {
  std::vector<std::uint32_t> words;
  std::vector<Source> sources;
  std::int32_t raster_origin_x{};
  std::int32_t raster_origin_y{};

  [[nodiscard]] mohu::GpuPrimitiveSnapshotView view() const noexcept {
    return {words, sources, raster_origin_x, raster_origin_y};
  }
};

void appendTriangle(Snapshot &snapshot, std::uint32_t root,
                    std::uint32_t offset, std::uint32_t coordinate_bias,
                    std::uint32_t color = 0x00404040U) {
  const std::array words{
      0x20000000U | color,
      0x00100010U + coordinate_bias,
      0x00200020U + coordinate_bias,
      0x00300030U + coordinate_bias,
  };
  for (std::size_t word{}; word < words.size(); ++word) {
    snapshot.words.push_back(words[word]);
    snapshot.sources.push_back(
        {root + offset + static_cast<std::uint32_t>(word * 4U), root,
         SourceKind::linked_list});
  }
}

void appendTexturedTriangle(Snapshot &snapshot, std::uint32_t root,
                            std::uint32_t offset, std::uint32_t uv_bias = 0U) {
  const std::array words{
      0x24404040U, 0x00100010U, 0x00010001U + uv_bias, 0x00200020U,
      0x00020002U, 0x00300030U, 0x00030003U,
  };
  for (std::size_t word{}; word < words.size(); ++word) {
    snapshot.words.push_back(words[word]);
    snapshot.sources.push_back(
        {root + offset + static_cast<std::uint32_t>(word * 4U), root,
         SourceKind::linked_list});
  }
}

void appendGouraudTriangle(Snapshot &snapshot, std::uint32_t root,
                           std::uint32_t offset, std::uint32_t color_bias,
                           std::uint32_t coordinate_bias) {
  const std::array words{
      0x30010101U + color_bias, 0x00100010U + coordinate_bias,
      0x00020202U + color_bias, 0x00200020U + coordinate_bias,
      0x00030303U + color_bias, 0x00300030U + coordinate_bias,
  };
  for (std::size_t word{}; word < words.size(); ++word) {
    snapshot.words.push_back(words[word]);
    snapshot.sources.push_back(
        {root + offset + static_cast<std::uint32_t>(word * 4U), root,
         SourceKind::linked_list});
  }
}

void appendStateCommand(Snapshot &snapshot, std::uint32_t root,
                        std::uint32_t offset, std::uint32_t command) {
  snapshot.words.push_back(command);
  snapshot.sources.push_back({root + offset, root, SourceKind::linked_list});
}

[[nodiscard]] std::uint32_t drawArea(std::uint32_t opcode, std::uint32_t x,
                                     std::uint32_t y) noexcept {
  return opcode | (x & 0x03ffU) | ((y & 0x01ffU) << 10U);
}

void testRawAddressMatch() {
  Snapshot previous;
  Snapshot current;
  appendTriangle(previous, 0x1000U, 0x100U, 0U, 0x00101010U);
  appendTriangle(current, 0x1800U, 0x100U, 0x00010001U, 0x00202020U);
  for (std::size_t word{}; word < current.sources.size(); ++word) {
    current.sources[word].word_address = previous.sources[word].word_address;
  }

  const auto report =
      mohu::matchGpuPrimitiveSnapshots(previous.view(), current.view());
  require(report.input_valid && report.matches.size() == 1U &&
              report.raw_match_count == 1U &&
              report.relative_match_count == 0U &&
              report.matches.front().kind ==
                  mohu::GpuPrimitiveMatchKind::raw_address &&
              report.matches.front().previous_coordinate_words[0U] == 1U &&
              report.matches.front().current_coordinate_words[2U] == 3U,
          "Raw DMA address did not match one stable packet");
}

void testRawCollisionFailsClosed() {
  Snapshot previous;
  Snapshot current;
  appendTriangle(previous, 0x1000U, 0x100U, 0U);
  appendTriangle(previous, 0x2000U, 0x100U, 0U);
  appendTriangle(current, 0x3000U, 0x100U, 0U);
  appendTriangle(current, 0x4000U, 0x100U, 0U);
  for (auto &source : previous.sources) {
    source.word_address &= 0x0fffU;
  }
  for (auto &source : current.sources) {
    source.word_address &= 0x0fffU;
  }

  const auto report =
      mohu::matchGpuPrimitiveSnapshots(previous.view(), current.view());
  require(report.input_valid && report.matches.empty() &&
              !report.relative_provenance_proven,
          "Colliding raw DMA packets were matched");
}

void testRelativeProofThreshold() {
  Snapshot previous;
  Snapshot current;
  for (std::uint32_t polygon{}; polygon < 7U; ++polygon) {
    const auto offset = 0x100U + polygon * 0x20U;
    appendTriangle(previous, 0x1000U, offset, 0U);
    appendTriangle(current, 0x8000U, offset, polygon + 1U);
  }

  const auto report =
      mohu::matchGpuPrimitiveSnapshots(previous.view(), current.view());
  require(report.input_valid && report.matches.empty() &&
              !report.relative_provenance_proven,
          "Relative DMA provenance bypassed its proof threshold");
}

void testRelativeDoubleBufferProof() {
  Snapshot previous;
  Snapshot current;
  for (std::uint32_t polygon{}; polygon < 8U; ++polygon) {
    const auto offset = 0x100U + polygon * 0x20U;
    appendGouraudTriangle(previous, 0x1000U, offset, polygon, 0U);
    appendGouraudTriangle(current, 0x9000U, offset, polygon + 0x10U,
                          polygon + 1U);
  }

  const auto report =
      mohu::matchGpuPrimitiveSnapshots(previous.view(), current.view());
  require(report.input_valid && report.relative_provenance_proven &&
              report.matches.size() == 8U && report.raw_match_count == 0U &&
              report.relative_match_count == 8U,
          "Proven MOHU-style linked-list double buffer did not match");
  for (const auto &match : report.matches) {
    require(match.vertex_count == 3U &&
                match.kind == mohu::GpuPrimitiveMatchKind::linked_list_relative,
            "Relative match returned invalid polygon topology");
  }
}

void testRelativeDoubleBufferRasterOriginProof() {
  Snapshot previous;
  Snapshot current;
  current.raster_origin_y = 240;
  appendStateCommand(previous, 0x1000U, 0x20U, drawArea(0xe3000000U, 0U, 0U));
  appendStateCommand(previous, 0x1000U, 0x24U,
                     drawArea(0xe4000000U, 319U, 239U));
  appendStateCommand(previous, 0x1000U, 0x28U, 0xe5000000U);
  appendStateCommand(current, 0x9000U, 0x20U, drawArea(0xe3000000U, 0U, 240U));
  appendStateCommand(current, 0x9000U, 0x24U,
                     drawArea(0xe4000000U, 319U, 479U));
  appendStateCommand(current, 0x9000U, 0x28U, 0xe5000000U | (240U << 11U));
  for (std::uint32_t polygon{}; polygon < 8U; ++polygon) {
    const auto offset = 0x100U + polygon * 0x20U;
    appendGouraudTriangle(previous, 0x1000U, offset, polygon, 0U);
    appendGouraudTriangle(current, 0x9000U, offset, polygon + 0x10U,
                          polygon + 1U);
  }

  const auto report =
      mohu::matchGpuPrimitiveSnapshots(previous.view(), current.view());
  require(report.input_valid && report.relative_provenance_proven &&
              report.matches.size() == 8U && report.raw_match_count == 0U &&
              report.relative_match_count == 8U,
          "Page-relative double-buffer draw context did not match");

  current.raster_origin_y = 239;
  const auto mismatched =
      mohu::matchGpuPrimitiveSnapshots(previous.view(), current.view());
  require(mismatched.input_valid && mismatched.matches.empty() &&
              !mismatched.relative_provenance_proven,
          "Incorrect raster origin was accepted");
}

void testRelativeStaticPayloadMismatchFailsClosed() {
  Snapshot previous;
  Snapshot current;
  for (std::uint32_t polygon{}; polygon < 8U; ++polygon) {
    const auto offset = 0x100U + polygon * 0x40U;
    appendTexturedTriangle(previous, 0x1000U, offset);
    appendTexturedTriangle(current, 0x9000U, offset, polygon < 3U ? 1U : 0U);
  }

  const auto report =
      mohu::matchGpuPrimitiveSnapshots(previous.view(), current.view());
  require(report.input_valid && report.matches.empty() &&
              !report.relative_provenance_proven &&
              report.relative_candidate_count == 5U &&
              report.relative_static_reject_count == 3U,
          "Relative proof ignored a changed texture payload");
}

void testRelativePartialPayloadProof() {
  Snapshot previous;
  Snapshot current;
  for (std::uint32_t polygon{}; polygon < 12U; ++polygon) {
    const auto offset = 0x100U + polygon * 0x40U;
    appendTexturedTriangle(previous, 0x1000U, offset);
    appendTexturedTriangle(current, 0x9000U, offset, polygon == 4U ? 1U : 0U);
  }

  const auto report =
      mohu::matchGpuPrimitiveSnapshots(previous.view(), current.view());
  require(report.input_valid && report.relative_provenance_proven &&
              report.matches.size() == 11U &&
              report.relative_candidate_count == 11U &&
              report.relative_static_reject_count == 1U,
          "Safe partial relative proof was rejected");
}

void testRelativeCollisionFailsClosed() {
  Snapshot previous;
  Snapshot current;
  for (std::uint32_t polygon{}; polygon < 8U; ++polygon) {
    const auto offset = polygon < 2U ? 0x100U : 0x100U + polygon * 0x20U;
    appendTriangle(previous, 0x1000U + polygon * 0x1000U, offset, 0U);
    appendTriangle(current, 0x11000U + polygon * 0x1000U, offset, polygon + 1U);
  }

  const auto report =
      mohu::matchGpuPrimitiveSnapshots(previous.view(), current.view());
  require(report.input_valid && !report.relative_provenance_proven &&
              report.matches.empty(),
          "Colliding relative packet offsets were accepted");
}

void testSemanticSequenceSurvivesIndependentPacketArenas() {
  Snapshot previous;
  Snapshot current;
  appendTexturedTriangle(current, 0x9000U, 0x40U, 0x1000U);
  for (std::uint32_t polygon{}; polygon < 10U; ++polygon) {
    appendTexturedTriangle(previous, 0x1000U, 0x100U + polygon * 0x40U,
                           polygon);
    appendTexturedTriangle(current, 0x9000U, 0x800U + polygon * 0x60U, polygon);
    const auto current_coordinate = current.words.size() - 6U;
    current.words[current_coordinate] += polygon + 1U;
    current.words[current_coordinate + 2U] += polygon + 1U;
    current.words[current_coordinate + 4U] += polygon + 1U;
  }

  const auto address_report =
      mohu::matchGpuPrimitiveSnapshots(previous.view(), current.view());
  const auto sequence_report =
      mohu::matchGpuPrimitiveSequences(previous.view(), current.view());
  require(address_report.matches.empty() && sequence_report.input_valid &&
              sequence_report.matches.size() == 10U,
          "Semantic sequence did not recover independent packet arenas");
  for (std::size_t index{}; index < sequence_report.matches.size(); ++index) {
    const auto &match = sequence_report.matches[index];
    require(match.kind == mohu::GpuPrimitiveMatchKind::semantic_sequence &&
                match.previous_command_word == index * 7U &&
                match.current_command_word == (index + 1U) * 7U,
            "Semantic sequence lost monotonic primitive identity");
  }
}

void testSemanticSequenceBacktracksSeveralEdits() {
  Snapshot previous;
  Snapshot current;
  appendTexturedTriangle(current, 0x9000U, 0x40U, 1000U);
  for (std::uint32_t polygon{}; polygon < 64U; ++polygon) {
    appendTexturedTriangle(previous, 0x1000U, 0x100U + polygon * 0x40U,
                           polygon);
    if (polygon == 17U || polygon == 43U) {
      continue;
    }
    appendTexturedTriangle(current, 0x9000U, 0x800U + polygon * 0x60U, polygon);
    const auto coordinate = current.words.size() - 6U;
    current.words[coordinate] += polygon + 1U;
    current.words[coordinate + 2U] += polygon + 1U;
    current.words[coordinate + 4U] += polygon + 1U;
    if (polygon == 29U) {
      appendTexturedTriangle(current, 0x9000U, 0x4000U, 1001U);
    }
  }
  appendTexturedTriangle(current, 0x9000U, 0x5000U, 1002U);

  const auto report =
      mohu::matchGpuPrimitiveSequences(previous.view(), current.view());
  require(report.input_valid && report.matches.size() == 62U,
          "Semantic sequence lost stable packets across several edits");
  auto previous_word = std::size_t{};
  auto current_word = std::size_t{};
  for (const auto &match : report.matches) {
    require(match.previous_command_word >= previous_word &&
                match.current_command_word >= current_word,
            "Semantic sequence backtracking is not monotonic");
    previous_word = match.previous_command_word + 1U;
    current_word = match.current_command_word + 1U;
  }
}

void testSemanticUniqueMatchingSurvivesOrderingTableReorder() {
  Snapshot previous;
  Snapshot current;
  constexpr std::array<std::uint32_t, 10U> order{
      5U, 2U, 9U, 0U, 7U, 3U, 8U, 1U, 6U, 4U};
  for (std::uint32_t polygon{}; polygon < order.size(); ++polygon) {
    appendTexturedTriangle(previous, 0x1000U, 0x100U + polygon * 0x40U,
                           polygon);
  }
  for (const auto polygon : order) {
    appendTexturedTriangle(current, 0x9000U, 0x800U + polygon * 0x60U,
                           polygon);
    const auto coordinate = current.words.size() - 6U;
    current.words[coordinate] += polygon + 1U;
    current.words[coordinate + 2U] += polygon + 1U;
    current.words[coordinate + 4U] += polygon + 1U;
  }

  const auto report =
      mohu::matchGpuPrimitiveSequences(previous.view(), current.view());
  require(report.input_valid && report.matches.size() == order.size(),
          "Unique semantic matching did not survive OT reorder");
  std::array<bool, order.size()> seen{};
  for (const auto &match : report.matches) {
    require(match.kind == mohu::GpuPrimitiveMatchKind::semantic_sequence,
            "Unique semantic match returned the wrong provenance");
    const auto previous_polygon = match.previous_command_word / 7U;
    require(previous_polygon < seen.size() && !seen[previous_polygon],
            "Unique semantic matching duplicated an identity");
    seen[previous_polygon] = true;
  }
}

void testMalformedInputFailsClosed() {
  Snapshot truncated;
  truncated.words = {0x20000000U, 0x00100010U};
  truncated.sources.resize(truncated.words.size());
  Snapshot valid;
  appendTriangle(valid, 0x1000U, 0x100U, 0U);

  auto report =
      mohu::matchGpuPrimitiveSnapshots(truncated.view(), valid.view());
  require(!report.input_valid && report.matches.empty(),
          "Incomplete GP0 packet was accepted");

  truncated.words = valid.words;
  truncated.sources.resize(1U);
  report = mohu::matchGpuPrimitiveSnapshots(truncated.view(), valid.view());
  require(!report.input_valid && report.matches.empty(),
          "Misaligned DMA sidecar was accepted");
}

void testDynamicLightingPacketInterpolation() {
  std::size_t count{};
  const auto flat = sf::psx::gp0PolygonColorWords(0x24U, count);
  require(count == 1U && flat[0U] == 0U,
          "Flat polygon color layout is invalid");
  const auto textured_gouraud =
      sf::psx::gp0PolygonColorWords(0x34U, count);
  require(count == 3U && textured_gouraud[0U] == 0U &&
              textured_gouraud[1U] == 3U &&
              textured_gouraud[2U] == 6U,
          "Textured Gouraud color layout is invalid");
  const auto gouraud_quad = sf::psx::gp0PolygonColorWords(0x38U, count);
  require(count == 4U && gouraud_quad[3U] == 6U,
          "Gouraud quad color layout is invalid");

  constexpr auto previous = std::uint32_t{0x30102030U};
  constexpr auto current = std::uint32_t{0x30f0a050U};
  require(sf::psx::gp0InterpolateRgb(previous, current, 0U) == previous &&
              sf::psx::gp0InterpolateRgb(previous, current, 256U) == current &&
              sf::psx::gp0InterpolateRgb(previous, current, 128U) ==
                  0x30806040U,
          "Dynamic lighting RGB interpolation is invalid");
  require(sf::psx::gp0InterpolateRgb(0xff102030U, 0x00102030U, 0U) ==
              0x00102030U,
          "RGB interpolation did not retain the current packet code");
}

} // namespace

int main() {
  try {
    testRawAddressMatch();
    testRawCollisionFailsClosed();
    testRelativeProofThreshold();
    testRelativeDoubleBufferProof();
    testRelativeDoubleBufferRasterOriginProof();
    testRelativeStaticPayloadMismatchFailsClosed();
    testRelativePartialPayloadProof();
    testRelativeCollisionFailsClosed();
    testSemanticSequenceSurvivesIndependentPacketArenas();
    testSemanticSequenceBacktracksSeveralEdits();
    testSemanticUniqueMatchingSurvivesOrderingTableReorder();
    testMalformedInputFailsClosed();
    testDynamicLightingPacketInterpolation();
    std::cout << "mohu_gpu_primitive_matcher_tests: ok\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mohu_gpu_primitive_matcher_tests: " << error.what() << '\n';
    return 1;
  }
}
