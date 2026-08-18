#include "mohu/gpu_command_stream.hpp"
#include "sf/psx/gp0_command.hpp"
#include "sf/psx/machine.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

[[nodiscard]] bool aligned(const mohu::GpuCommandStream &stream) noexcept {
  const auto word_count = stream.frameWords().size();
  const auto empty_or_aligned = [word_count](auto sidecar) noexcept {
    return sidecar.empty() || sidecar.size() == word_count;
  };
  return empty_or_aligned(stream.frameProjections()) &&
         empty_or_aligned(stream.frameProjectionIdentities()) &&
         empty_or_aligned(stream.frameDmaSources());
}

void testCommandCapture() {
  mohu::GpuCommandStream gpu;
  require(gpu.readStatus() == 0x14802000U, "GPU reset status mismatch");
  require(gpu.writeGp0(0xe1000400U) && gpu.writeGp0(0xe3000000U),
          "GP0 command capture failed");
  require(gpu.totalGp0Words() == 2U && gpu.frameWords().size() == 2U &&
              gpu.firstWords().size() == 2U && aligned(gpu),
          "GP0 command vectors diverged");
  require(gpu.frameProjectionIdentities().empty() &&
              gpu.frameDmaSources().empty(),
          "Empty provenance sidecars were materialized");

  gpu.beginFrame();
  require(gpu.frameWords().empty() && gpu.frameProjections().empty() &&
              gpu.frameProjectionIdentities().empty() &&
              gpu.frameDmaSources().empty() && aligned(gpu),
          "Frame boundary retained command data");
  require(gpu.firstWords().size() == 2U,
          "Frame boundary discarded lifetime diagnostics");
}

void testProjectionSidecars() {
  mohu::GpuCommandStream gpu;
  sf::psx::GteProjectedVertex projected{};
  projected.packed_sxy = 0x0014000aU;
  projected.screen_x = 10.25F;
  projected.screen_y = 20.5F;
  projected.view_z = 1000.0F;
  projected.valid = true;
  constexpr std::uint64_t source_identity = 0x123456789abcdef0ULL;
  constexpr sf::psx::GpuDmaWordSource dma_source{
      0x1234U, 0x1000U, sf::psx::GpuDmaSourceKind::linked_list};

  require(gpu.writeGp0(0xe1000000U) &&
              gpu.writeGp0FromRam(projected.packed_sxy, dma_source, &projected,
                                  source_identity),
          "Projected GP0 capture failed");
  require(!gpu.frameProjections()[0U].valid &&
              gpu.frameProjections()[1U] == projected &&
              gpu.frameProjectionIdentities()[0U] == 0U &&
              gpu.frameProjectionIdentities()[1U] == source_identity &&
              !gpu.frameDmaSources()[0U].valid() &&
              gpu.frameDmaSources()[1U] == dma_source && aligned(gpu),
          "Projection sidecar detached from GP0 words");

  constexpr std::array<std::size_t, 2U> word_indices{0U, 1U};
  constexpr std::array<std::uint16_t, 2U> identities{11U, 12U};
  require(gpu.overrideFrameProjectionIdentities(word_indices, identities) &&
              gpu.frameProjectionIdentities()[0U] == 11U &&
              gpu.frameProjectionIdentities()[1U] == 12U,
          "Projection identity batch was not committed");
  constexpr std::array<std::size_t, 2U> invalid_indices{0U, 2U};
  require(!gpu.overrideFrameProjectionIdentities(invalid_indices, identities) &&
              gpu.frameProjectionIdentities()[0U] == 11U &&
              gpu.frameProjectionIdentities()[1U] == 12U,
          "Invalid identity batch committed partially");

  gpu.setProjectionTracking(false);
  gpu.setProjectionIdentityTracking(false);
  gpu.beginFrame();
  require(gpu.writeGp0FromRam(projected.packed_sxy, 0x1234U, &projected,
                              source_identity) &&
              gpu.frameProjections().empty() &&
              gpu.frameProjectionIdentities().empty(),
          "Disabled projection sidecars still captured data");
}

void testLazyTransferSidecars() {
  mohu::GpuCommandStream gpu;
  constexpr sf::psx::GpuDmaWordSource dma_source{
      0x2000U, 0x2000U, sf::psx::GpuDmaSourceKind::linear};
  constexpr std::array upload{0xa0000000U, 0x00000000U, 0x00010010U,
                              0x80112233U, 0xa0445566U, 0x80778899U,
                              0xa0aabbccU, 0x80112233U, 0xa0445566U,
                              0x80778899U, 0xa0aabbccU};
  for (std::size_t index{}; index < upload.size(); ++index) {
    require(gpu.writeGp0FromRam(upload[index], dma_source, nullptr, 0U),
            "CPU-to-VRAM payload capture failed");
  }
  require(gpu.frameWords().size() == upload.size() &&
              gpu.frameProjectionIdentities().empty() &&
              gpu.frameDmaSources().empty() && aligned(gpu),
          "CPU-to-VRAM-only stream retained an unused provenance sidecar");

  require(gpu.writeGp0FromRam(0xe1000000U, dma_source, nullptr, 0U),
          "Post-upload GP0 provenance capture failed");
  require(gpu.frameDmaSources().size() == gpu.frameWords().size() &&
              gpu.frameDmaSources().back().valid() &&
              !gpu.frameDmaSources().front().valid() && aligned(gpu),
          "CPU-to-VRAM payload permanently disabled later provenance");

  gpu.beginFrame();
  constexpr std::array fill_with_upload_lookalike{0x02000000U, 0xa0000000U,
                                                  0x00010001U, 0xe1000000U};
  for (std::size_t index{}; index < fill_with_upload_lookalike.size();
       ++index) {
    require(gpu.writeGp0FromRam(fill_with_upload_lookalike[index], dma_source,
                                nullptr, index + 1U),
            "Fill/lookalike command capture failed");
  }
  require(gpu.frameWords().size() == fill_with_upload_lookalike.size() &&
              gpu.frameProjectionIdentities().size() ==
                  fill_with_upload_lookalike.size() &&
              gpu.frameDmaSources().size() ==
                  fill_with_upload_lookalike.size() &&
              aligned(gpu),
          "Fill payload lookalike disabled aligned provenance");
}

void testControlCommands() {
  mohu::GpuCommandStream gpu;
  gpu.writeGp1(0x03000001U);
  require(!gpu.displayState().enabled &&
              (gpu.readStatus() & (1U << 23U)) != 0U &&
              gpu.displayPublicationSequence() == 1U,
          "GP1 display disable publication mismatch");
  gpu.writeGp1(0x05019040U);
  require(gpu.displayState().x == 64U && gpu.displayState().y == 100U &&
              gpu.displayPublicationSequence() == 2U,
          "GP1 display origin mismatch");
  gpu.writeGp1(0x05019040U);
  require(gpu.displayPublicationSequence() == 3U,
          "Repeated GP1 display origin did not publish a new frame");
  gpu.writeGp1(0x08000035U);
  require(gpu.displayState().width == 320U &&
              gpu.displayState().height == 480U && gpu.displayState().rgb24 &&
              gpu.displayState().interlaced &&
              gpu.displayPublicationSequence() == 4U,
          "GP1 display mode mismatch");
  gpu.writeGp1(0x04000002U);
  require(((gpu.readStatus() >> 29U) & 3U) == 2U &&
              (gpu.readStatus() & (1U << 25U)) != 0U,
          "GP1 DMA direction mismatch");

  require(gpu.writeGp0(0x20000000U), "GP0 setup failed");
  gpu.writeGp1(0x01000000U);
  require(gpu.commandBufferEpoch() == 1U && gpu.frameWords().empty() &&
              gpu.frameDmaSources().empty(),
          "GP1 command-buffer reset mismatch");
  gpu.writeGp1(0x00000000U);
  require(gpu.commandBufferEpoch() == 2U && gpu.readStatus() == 0x14802000U &&
              gpu.displayState().enabled && gpu.displayState().width == 256U &&
              gpu.displayState().height == 240U &&
              gpu.displayPublicationSequence() == 5U,
          "Full GP1 reset mismatch");

  std::uint32_t readback{0xffffffffU};
  require(!gpu.readGp0(readback) && readback == 0U,
          "Unsupported VRAM readback mismatch");
}

void testDisplayPublicationBoundaries() {
  mohu::GpuCommandStream gpu;
  gpu.beginFrame();
  constexpr std::array first_draw{0x20000000U, 0U, 0U, 0U};
  for (const auto word : first_draw) {
    require(gpu.writeGp0(word), "First draw capture failed");
  }
  gpu.writeGp1(0x05019040U);
  constexpr std::array second_draw{0x02000000U, 0U, 0x00010001U};
  for (const auto word : second_draw) {
    require(gpu.writeGp0(word), "Second draw capture failed");
  }
  gpu.writeGp1(0x05032080U);

  const auto publications = gpu.frameDisplayPublications();
  require(publications.size() == 2U,
          "GP1 publications were not captured independently");
  require(publications[0U].word_offset == first_draw.size() &&
              publications[0U].sequence == 1U &&
              publications[0U].display.x == 64U &&
              publications[0U].display.y == 100U,
          "First GP1 publication boundary mismatch");
  require(publications[1U].word_offset ==
                  first_draw.size() + second_draw.size() &&
              publications[1U].sequence == 2U &&
              publications[1U].display.x == 128U &&
              publications[1U].display.y == 200U,
          "Second GP1 publication boundary mismatch");
}

void testCommandLengths() {
  constexpr std::array polygon{0x24000000U, 0U, 0U, 0U, 0U, 0U, 0U};
  constexpr std::array polyline{0x48000000U, 0U, 0U, 0x50005000U};
  constexpr std::array upload{0xa0000000U, 0U, 0x00020002U, 0U, 0U};
  constexpr std::array truncated{0x24000000U, 0U};
  static_assert(sf::psx::gp0CommandLength(polygon) == 7U);
  static_assert(sf::psx::gp0CommandLength(polyline) == 4U);
  static_assert(sf::psx::gp0CommandLength(upload) == 5U);
  static_assert(sf::psx::gp0CommandLength(truncated) == 0U);
}

} // namespace

int main() {
  try {
    testCommandLengths();
    testCommandCapture();
    testLazyTransferSidecars();
    testProjectionSidecars();
    testControlCommands();
    testDisplayPublicationBoundaries();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
