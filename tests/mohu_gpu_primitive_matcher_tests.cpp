#include "mohu/gpu_primitive_matcher.hpp"

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

  [[nodiscard]] mohu::GpuPrimitiveSnapshotView view() const noexcept {
    return {words, sources};
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

void testRelativeStaticPayloadMismatchFailsClosed() {
  Snapshot previous;
  Snapshot current;
  for (std::uint32_t polygon{}; polygon < 8U; ++polygon) {
    const auto offset = 0x100U + polygon * 0x40U;
    appendTexturedTriangle(previous, 0x1000U, offset);
    appendTexturedTriangle(current, 0x9000U, offset, polygon == 4U ? 1U : 0U);
  }

  const auto report =
      mohu::matchGpuPrimitiveSnapshots(previous.view(), current.view());
  require(report.input_valid && report.matches.empty() &&
              !report.relative_provenance_proven,
          "Relative proof ignored a changed texture payload");
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

} // namespace

int main() {
  try {
    testRawAddressMatch();
    testRawCollisionFailsClosed();
    testRelativeProofThreshold();
    testRelativeDoubleBufferProof();
    testRelativeStaticPayloadMismatchFailsClosed();
    testRelativeCollisionFailsClosed();
    testMalformedInputFailsClosed();
    std::cout << "mohu_gpu_primitive_matcher_tests: ok\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mohu_gpu_primitive_matcher_tests: " << error.what() << '\n';
    return 1;
  }
}
