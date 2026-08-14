#include "mohu/native_world_frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error{message};
}
void writePosition(sf::psx::R3000Runtime &cpu, std::uint32_t address,
                   std::int16_t x, std::int16_t y, std::int16_t z) {
  require(cpu.write16(address, static_cast<std::uint16_t>(x)) &&
              cpu.write16(address + 2U, static_cast<std::uint16_t>(y)) &&
              cpu.write16(address + 4U, static_cast<std::uint16_t>(z)) &&
              cpu.write16(address + 6U, 104U),
          "cannot write world position fixture");
}
} // namespace

int main() {
  try {
    constexpr std::uint32_t record = 0x000a0594U;
    constexpr std::uint32_t packet = record + 0x30U;
    sf::psx::R3000Runtime cpu;
    writePosition(cpu, record + 4U, 10, 20, 1000);
    writePosition(cpu, record + 12U, 30, 20, 1000);
    writePosition(cpu, record + 20U, 10, 40, 1000);
    writePosition(cpu, record + 28U, 30, 40, 1000);
    mohu::NativeWorldCamera camera{};
    camera.rotation = {4096.0, 0.0, 0.0, 0.0, 4096.0, 0.0, 0.0, 0.0, 4096.0};
    camera.projection_h = 400.0;
    camera.offset_x = 256.0;
    camera.offset_y = 120.0;
    camera.camera_revision = 7U;
    camera.projection_revision = 8U;
    camera.valid = true;
    std::vector<std::uint32_t> words{0x2c808080U, 0x00800104U, 0U,
                                     0x0080010cU, 0U,          0x00880104U,
                                     0U,          0x0088010cU, 0U};
    std::vector<std::uint32_t> sources;
    for (std::uint32_t word{}; word < words.size(); ++word)
      sources.push_back(packet + 4U + word * 4U);
    std::array<mohu::NativeWorldProjectionOverride, 4U> projections{};
    const auto snapshot = mohu::captureNativeWorldFrameSnapshot(cpu, camera);
    require(snapshot.valid, "world generation snapshot was not captured");
    writePosition(cpu, record + 4U, 3000, 3000, 100);
    const auto stats = mohu::overrideNativeWorldFrameProjections(
        words, sources, projections, snapshot);
    require(stats.candidate_quads == 1U && stats.coordinate_words == 4U &&
                stats.complete_quads == 1U &&
                stats.witness_rejected_quads == 0U,
            "world quad was not recognized atomically");
    require(projections[0].word_index == 1U &&
                projections[1].word_index == 3U &&
                projections[2].word_index == 5U &&
                projections[3].word_index == 7U &&
                std::ranges::all_of(
                    projections,
                    [](const auto &entry) { return entry.projection.valid; }),
            "world coordinate projections are incomplete");
    require(projections[0].projection.view_x == 10.0F &&
                projections[0].projection.view_y == 20.0F &&
                projections[0].projection.view_z == 1000.0F,
            "pre-GTE source position was not preserved");
    require(std::abs(projections[0].projection.screen_x - 260.0F) < 0.00001F &&
                std::abs(projections[0].projection.screen_y - 128.0F) <
                    0.00001F,
            "native perspective projection mismatch");

    sources[3] = 0xffffffffU;
    projections = {};
    const auto incomplete = mohu::overrideNativeWorldFrameProjections(
        words, sources, projections, snapshot);
    require(incomplete.candidate_quads == 1U &&
                incomplete.incomplete_quads == 1U &&
                incomplete.coordinate_words == 0U,
            "incomplete DMA quad was not rejected atomically");

    sources[3] = packet + 4U + 3U * 4U;
    words[1] = 0x02000200U;
    const auto mismatched = mohu::overrideNativeWorldFrameProjections(
        words, sources, projections, snapshot);
    require(mismatched.witness_rejected_quads == 1U &&
                mismatched.complete_quads == 0U &&
                mismatched.coordinate_words == 0U &&
                mismatched.maximum_witness_error > 100.0F,
            "camera-incoherent native world projection did not fail closed");

    words[1] = 0x00800104U;
    writePosition(cpu, record + 4U, 10, 20, 1000);
    writePosition(cpu, record + 12U, 30, 20, 1000);
    writePosition(cpu, record + 20U, 10, 40, 1000);
    writePosition(cpu, record + 28U, 30, 40, 1000);
    require(cpu.write32(0x000a9354U, 0U), "cannot select world buffer zero");
    const auto generation_a =
        mohu::captureNativeWorldFrameSnapshot(cpu, camera);
    writePosition(cpu, record + 4U, 110, 20, 1000);
    writePosition(cpu, record + 12U, 130, 20, 1000);
    writePosition(cpu, record + 20U, 110, 40, 1000);
    writePosition(cpu, record + 28U, 130, 40, 1000);
    require(cpu.write32(0x000a9354U, 1U), "cannot select world buffer one");
    const auto generation_b =
        mohu::captureNativeWorldFrameSnapshot(cpu, camera);
    std::vector<std::uint32_t> segmented_words = words;
    segmented_words.insert(segmented_words.end(),
                           {0x2c808080U, 0x0080012cU, 0U, 0x00800134U, 0U,
                            0x0088012cU, 0U, 0x00880134U, 0U});
    std::vector<std::uint32_t> segmented_sources = sources;
    constexpr auto second_packet = record + 0x58U;
    for (std::uint32_t word{}; word < 9U; ++word) {
      segmented_sources.push_back(second_packet + 4U + word * 4U);
    }
    const std::array generations{
        mohu::NativeWorldFrameSnapshotSegment{0U, generation_a},
        mohu::NativeWorldFrameSnapshotSegment{9U, generation_b}};
    std::array<mohu::NativeWorldProjectionOverride, 8U> segmented_projections{};
    const auto segmented = mohu::overrideNativeWorldFrameProjections(
        segmented_words, segmented_sources, segmented_projections, generations);
    require(segmented.complete_quads == 2U &&
                segmented.coordinate_words == 8U &&
                segmented_projections[0].projection.view_x == 10.0F &&
                segmented_projections[4].projection.view_x == 110.0F,
            "GP0 generations did not retain their immutable world snapshots");

    const std::array wrong_buffer{
        mohu::NativeWorldFrameSnapshotSegment{0U, generation_a}};
    const auto selector_mismatch = mohu::overrideNativeWorldFrameProjections(
        std::span{segmented_words}.subspan(9U),
        std::span{segmented_sources}.subspan(9U), segmented_projections,
        wrong_buffer);
    require(selector_mismatch.coordinate_words == 0U &&
                selector_mismatch.invalid_geometry_quads == 1U,
            "world packet accepted a snapshot from the other retail buffer");

    writePosition(cpu, record + 4U, 10, 20, -100);
    writePosition(cpu, record + 12U, 30, 20, 0);
    writePosition(cpu, record + 20U, 10, 40, 1000);
    writePosition(cpu, record + 28U, 30, 40, 1000);
    require(cpu.write32(0x000a9354U, 0U), "cannot restore world buffer zero");
    words[1] = 0x002800d8U;
    const auto eye_crossing_snapshot =
        mohu::captureNativeWorldFrameSnapshot(cpu, camera);
    const auto eye_crossing = mohu::overrideNativeWorldFrameProjections(
        words, sources, projections, eye_crossing_snapshot);
    require(eye_crossing.complete_quads == 1U &&
                eye_crossing.invalid_geometry_quads == 0U &&
                projections[0].projection.view_z == -100.0F &&
                projections[1].projection.view_z == 0.0F &&
                std::isfinite(projections[1].projection.screen_x) &&
                std::isfinite(projections[1].projection.screen_y),
            "signed exact world quad did not reach homogeneous clipping");
    std::cout << "mohu_native_world_frame_tests: ok\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mohu_native_world_frame_tests: " << error.what() << '\n';
    return 1;
  }
}
