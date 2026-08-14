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
  return stream.frameWords().size() == stream.frameSourceAddresses().size() &&
         stream.frameWords().size() == stream.frameProjections().size() &&
         stream.frameWords().size() ==
             stream.frameProjectionIdentities().size();
}

void testCommandCapture() {
  mohu::GpuCommandStream gpu;
  require(gpu.readStatus() == 0x14802000U, "GPU reset status mismatch");
  require(gpu.writeGp0(0xe1000400U) && gpu.writeGp0(0xe3000000U),
          "GP0 command capture failed");
  require(gpu.totalGp0Words() == 2U && gpu.frameWords().size() == 2U &&
              gpu.firstWords().size() == 2U && aligned(gpu),
          "GP0 command vectors diverged");
  require(gpu.frameSourceAddresses()[0U] ==
                  mohu::GpuCommandStream::direct_source_address &&
              gpu.frameProjectionIdentities()[0U] == 0U,
          "Direct GP0 provenance mismatch");

  gpu.beginFrame();
  require(gpu.frameWords().empty() && gpu.frameSourceAddresses().empty() &&
              gpu.frameProjections().empty() &&
              gpu.frameProjectionIdentities().empty() && aligned(gpu),
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

  require(gpu.writeGp0(0xe1000000U) &&
              gpu.writeGp0FromRam(projected.packed_sxy, 0x1234U, &projected,
                                  source_identity),
          "Projected GP0 capture failed");
  require(gpu.frameSourceAddresses()[1U] == 0x1234U &&
              !gpu.frameProjections()[0U].valid &&
              gpu.frameProjections()[1U] == projected &&
              gpu.frameProjectionIdentities()[0U] == 0U &&
              gpu.frameProjectionIdentities()[1U] == source_identity &&
              aligned(gpu),
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

void testControlCommands() {
  mohu::GpuCommandStream gpu;
  gpu.writeGp1(0x03000001U);
  require(!gpu.displayState().enabled &&
              (gpu.readStatus() & (1U << 23U)) != 0U,
          "GP1 display disable mismatch");
  gpu.writeGp1(0x05019040U);
  require(gpu.displayState().x == 64U && gpu.displayState().y == 100U,
          "GP1 display origin mismatch");
  gpu.writeGp1(0x08000035U);
  require(gpu.displayState().width == 320U &&
              gpu.displayState().height == 480U &&
              gpu.displayState().rgb24 && gpu.displayState().interlaced,
          "GP1 display mode mismatch");
  gpu.writeGp1(0x04000002U);
  require(((gpu.readStatus() >> 29U) & 3U) == 2U &&
              (gpu.readStatus() & (1U << 25U)) != 0U,
          "GP1 DMA direction mismatch");

  require(gpu.writeGp0(0x20000000U), "GP0 setup failed");
  gpu.writeGp1(0x01000000U);
  require(gpu.commandBufferEpoch() == 1U && gpu.frameWords().empty(),
          "GP1 command-buffer reset mismatch");
  gpu.writeGp1(0x00000000U);
  require(gpu.commandBufferEpoch() == 2U &&
              gpu.readStatus() == 0x14802000U &&
              gpu.displayState().enabled &&
              gpu.displayState().width == 256U &&
              gpu.displayState().height == 240U,
          "Full GP1 reset mismatch");

  std::uint32_t readback{0xffffffffU};
  require(!gpu.readGp0(readback) && readback == 0U,
          "Unsupported VRAM readback mismatch");
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
    testProjectionSidecars();
    testControlCommands();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
