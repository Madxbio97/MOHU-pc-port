#include "mohu/gpu_command_stream.hpp"
#include "sf/platform/pgxp_polygon_coherence.hpp"
#include "sf/psx/machine.hpp"
#include "sf/psx/r3000_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace allocation_fault {

std::size_t countdown{};

void failAfter(std::size_t successful_allocations) noexcept {
  countdown = successful_allocations + 1U;
}

} // namespace allocation_fault

void *operator new(std::size_t size) {
  if (allocation_fault::countdown != 0U &&
      --allocation_fault::countdown == 0U) {
    throw std::bad_alloc{};
  }
  if (void *memory = std::malloc(size != 0U ? size : 1U)) {
    return memory;
  }
  throw std::bad_alloc{};
}

void operator delete(void *memory) noexcept { std::free(memory); }

void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

[[nodiscard]] constexpr std::uint32_t packPoint(std::int16_t x,
                                                std::int16_t y) noexcept {
  return static_cast<std::uint16_t>(x) |
         (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y)) << 16U);
}

[[nodiscard]] sf::platform::detail::PgxpPolygonCandidate
makeQuad(std::int16_t left, std::int16_t top, std::int16_t right,
         std::int16_t bottom, bool precise_eligible = true) {
  using sf::platform::detail::PgxpPolygonCandidate;
  PgxpPolygonCandidate polygon{};
  polygon.packed_xy = {packPoint(left, top), packPoint(right, top),
                       packPoint(left, bottom), packPoint(right, bottom)};
  polygon.vertex_count = 4U;
  polygon.precise_eligible = precise_eligible;
  constexpr auto view_z = 1000.0F;
  for (std::size_t vertex{}; vertex < polygon.precise_vertices.size();
       ++vertex) {
    const auto packed = polygon.packed_xy[vertex];
    polygon.precise_vertices[vertex] = {
        .screen_x =
            static_cast<float>(static_cast<std::int16_t>(packed & 0xffffU)) +
            0.25F,
        .screen_y =
            static_cast<float>(static_cast<std::int16_t>(packed >> 16U)) + 0.5F,
        .view_z = view_z,
        .valid = precise_eligible,
    };
  }
  return polygon;
}

[[nodiscard]] std::size_t preciseCount(
    std::span<const sf::platform::detail::PgxpPolygonDecision> decisions) {
  return static_cast<std::size_t>(
      std::count_if(decisions.begin(), decisions.end(),
                    [](const auto &item) { return item.allow_precise; }));
}

} // namespace

int main() {
  try {
    mohu::GpuCommandStream gpu;
    const auto aligned = [](const mohu::GpuCommandStream &stream) {
      return stream.frameWords().size() == stream.frameProjections().size() &&
             stream.frameWords().size() ==
                 stream.frameSourceAddresses().size() &&
             stream.frameWords().size() ==
                 stream.frameProjectionIdentities().size();
    };
    static_assert(noexcept(gpu.writeGp0(0U)));
    static_assert(noexcept(gpu.writeGp0FromRam(0U, 0U, nullptr, 0U)));
    constexpr std::array polygon_packet{0x24000000U, 0U, 0U, 0U, 0U, 0U, 0U};
    constexpr std::array polyline_packet{0x48000000U, 0U, 0U, 0x50005000U};
    constexpr std::array upload_packet{0xa0000000U, 0U, 0x00020002U, 0U, 0U};
    constexpr std::array truncated_polygon{0x24000000U, 0U};
    static_assert(sf::psx::gp0CommandLength(polygon_packet) == 7U);
    static_assert(sf::psx::gp0CommandLength(polyline_packet) == 4U);
    static_assert(sf::psx::gp0CommandLength(upload_packet) == 5U);
    static_assert(sf::psx::gp0CommandLength(truncated_polygon) == 0U);
    constexpr auto polygon_layout = sf::psx::gp0CommandLayout(polygon_packet);
    static_assert(
        polygon_layout.command_class == sf::psx::Gp0CommandClass::polygon &&
        polygon_layout.textured && polygon_layout.vertex_count == 3U &&
        polygon_layout.coordinate_words[0U] == 1U &&
        polygon_layout.coordinate_words[1U] == 3U &&
        polygon_layout.coordinate_words[2U] == 5U);
    for (std::size_t successful_allocations = 0U; successful_allocations < 4U;
         ++successful_allocations) {
      mohu::GpuCommandStream allocation_gpu;
      allocation_fault::failAfter(successful_allocations);
      require(!allocation_gpu.writeGp0(0xe1000000U) &&
                  allocation_gpu.frameWords().empty() &&
                  allocation_gpu.frameSourceAddresses().empty() &&
                  allocation_gpu.frameProjections().empty() &&
                  allocation_gpu.frameProjectionIdentities().empty() &&
                  aligned(allocation_gpu),
              "Failed GP0 append did not preserve vector alignment");
      require(allocation_gpu.writeGp0(0xe1000000U) && aligned(allocation_gpu),
              "GPU command stream was unusable after allocation failure");
    }
    require(gpu.readStatus() == 0x14802000U, "GPU reset status mismatch");
    require(gpu.writeGp0(0xe1000400U) && gpu.writeGp0(0xe3000000U) &&
                gpu.totalGp0Words() == 2U && gpu.frameWords().size() == 2U &&
                gpu.firstWords().size() == 2U && aligned(gpu) &&
                gpu.frameSourceAddresses()[0] ==
                    mohu::GpuCommandStream::direct_source_address &&
                gpu.frameSourceAddresses()[1] ==
                    mohu::GpuCommandStream::direct_source_address &&
                gpu.frameProjectionIdentities()[0] == 0U &&
                gpu.frameProjectionIdentities()[1] == 0U,
            "GP0 command capture mismatch");

    gpu.beginFrame();
    require(gpu.frameWords().empty() && gpu.frameSourceAddresses().empty() &&
                gpu.frameProjectionIdentities().empty() &&
                gpu.firstWords().size() == 2U && aligned(gpu),
            "GPU frame boundary discarded diagnostics");

    gpu.writeGp1(0x03000000U);
    require((gpu.readStatus() & (1U << 23U)) == 0U,
            "GP1 display enable mismatch");
    gpu.writeGp1(0x03000001U);
    require((gpu.readStatus() & (1U << 23U)) != 0U,
            "GP1 display disable mismatch");
    require(!gpu.displayState().enabled, "GP1 display state disable mismatch");
    gpu.writeGp1(0x05019040U);
    require(gpu.displayState().x == 64U && gpu.displayState().y == 100U,
            "GP1 display start mismatch");
    gpu.writeGp1(0x08000041U);
    require(gpu.displayState().width == 368U &&
                gpu.displayState().height == 240U && !gpu.displayState().rgb24,
            "GP1 display mode mismatch");
    gpu.writeGp1(0x08000035U);
    require(gpu.displayState().width == 320U &&
                gpu.displayState().height == 480U && gpu.displayState().rgb24,
            "GP1 interlaced RGB24 mode mismatch");
    gpu.writeGp1(0x04000002U);
    require(((gpu.readStatus() >> 29U) & 3U) == 2U &&
                (gpu.readStatus() & (1U << 25U)) != 0U,
            "GP1 DMA direction/status mismatch");

    std::uint32_t readback{0xffffffffU};
    require(!gpu.readGp0(readback) && readback == 0U,
            "Unsupported VRAM readback mismatch");
    gpu.writeGp1(0x00000000U);
    require(gpu.readStatus() == 0x14802000U && gpu.totalGp1Words() == 7U &&
                gpu.displayState().enabled &&
                gpu.displayState().width == 256U &&
                gpu.displayState().height == 240U,
            "GP1 reset/count mismatch");

    mohu::GpuCommandStream reset_epoch_gpu;
    require(reset_epoch_gpu.commandBufferEpoch() == 0U &&
                reset_epoch_gpu.writeGp0(0x20000000U),
            "GPU command-buffer epoch initial state mismatch");
    reset_epoch_gpu.writeGp1(0x03000000U);
    require(reset_epoch_gpu.commandBufferEpoch() == 0U &&
                reset_epoch_gpu.frameWords().size() == 1U,
            "Non-reset GP1 command changed command-buffer epoch");
    reset_epoch_gpu.writeGp1(0x01000000U);
    require(reset_epoch_gpu.commandBufferEpoch() == 1U &&
                reset_epoch_gpu.frameWords().empty() &&
                reset_epoch_gpu.frameSourceAddresses().empty() &&
                reset_epoch_gpu.frameProjections().empty() &&
                reset_epoch_gpu.frameProjectionIdentities().empty() &&
                aligned(reset_epoch_gpu),
            "GP1 command-buffer reset did not advance its epoch");
    reset_epoch_gpu.writeGp1(0x02000000U);
    require(reset_epoch_gpu.commandBufferEpoch() == 1U,
            "GP1 IRQ acknowledgement changed command-buffer epoch");
    require(reset_epoch_gpu.writeGp0(0x20000000U) && aligned(reset_epoch_gpu),
            "GPU command vectors diverged before full reset");
    reset_epoch_gpu.writeGp1(0x00000000U);
    require(reset_epoch_gpu.commandBufferEpoch() == 2U &&
                reset_epoch_gpu.frameWords().empty() &&
                reset_epoch_gpu.frameSourceAddresses().empty() &&
                reset_epoch_gpu.frameProjections().empty() &&
                reset_epoch_gpu.frameProjectionIdentities().empty() &&
                aligned(reset_epoch_gpu),
            "Full GP1 reset did not clear the aligned command vectors");

    mohu::GpuCommandStream precise_gpu;
    sf::psx::GteProjectedVertex projected{};
    projected.packed_sxy = 0x0014000aU;
    projected.screen_x = 10.25F;
    projected.screen_y = 20.5F;
    projected.view_z = 1000.0F;
    projected.valid = true;
    constexpr std::uint64_t source_identity = 0x123456789abcdef0ULL;
    require(precise_gpu.writeGp0(0xe1000000U) &&
                precise_gpu.writeGp0FromRam(projected.packed_sxy, 0x1234U,
                                            &projected, source_identity) &&
                precise_gpu.frameWords().size() == 2U &&
                precise_gpu.frameSourceAddresses().size() == 2U &&
                precise_gpu.frameSourceAddresses()[0] ==
                    mohu::GpuCommandStream::direct_source_address &&
                precise_gpu.frameSourceAddresses()[1] == 0x1234U &&
                precise_gpu.frameProjections().size() == 2U &&
                precise_gpu.frameProjectionIdentities().size() == 2U &&
                !precise_gpu.frameProjections()[0].valid &&
                precise_gpu.frameProjections()[1] == projected &&
                precise_gpu.frameProjectionIdentities()[0] == 0U &&
                precise_gpu.frameProjectionIdentities()[1] == source_identity &&
                aligned(precise_gpu),
            "GPU command stream detached precise DMA sidecar or identity");
    constexpr std::array<std::size_t, 2U> identity_words{0U, 1U};
    constexpr std::array<std::uint16_t, 2U> replacement_identities{11U, 12U};
    require(precise_gpu.overrideFrameProjectionIdentities(
                identity_words, replacement_identities) &&
                precise_gpu.frameProjectionIdentities()[0] == 11U &&
                precise_gpu.frameProjectionIdentities()[1] == 12U,
            "Projection identity batch was not committed");
    constexpr std::array<std::size_t, 2U> invalid_identity_words{0U, 2U};
    constexpr std::array<std::uint16_t, 2U> rejected_identities{21U, 22U};
    require(!precise_gpu.overrideFrameProjectionIdentities(
                invalid_identity_words, rejected_identities) &&
                precise_gpu.frameProjectionIdentities()[0] == 11U &&
                precise_gpu.frameProjectionIdentities()[1] == 12U,
            "Invalid projection identity batch committed partially");
    precise_gpu.beginFrame();
    require(precise_gpu.frameWords().empty() &&
                precise_gpu.frameSourceAddresses().empty() &&
                precise_gpu.frameProjections().empty() &&
                precise_gpu.frameProjectionIdentities().empty() &&
                aligned(precise_gpu),
            "GPU frame boundary retained a stale precise sidecar");
    require(
        precise_gpu.writeGp0FromRam(projected.packed_sxy, 0x1234U, nullptr) &&
            precise_gpu.frameSourceAddresses().front() == 0x1234U &&
            !precise_gpu.frameProjections().front().valid &&
            precise_gpu.frameProjectionIdentities().front() == 0U &&
            aligned(precise_gpu),
        "Missing GPU provenance did not use the legacy fallback");

    {
      mohu::GpuCommandStream render_gpu;
      const auto appendVertex =
          [&render_gpu](std::uint32_t packed, bool exact,
                        std::uint64_t identity = 0U, float view_x_offset = 0.0F,
                        float screen_h = 256.0F,
                        std::uint64_t transform_lineage = 7U,
                        std::uint64_t projection_epoch = 9U) {
            sf::psx::GteProjectedVertex vertex{};
            vertex.packed_sxy = packed;
            vertex.view_z = 512.0F;
            vertex.screen_h = screen_h;
            vertex.screen_x = static_cast<float>(
                                  static_cast<std::int16_t>(packed & 0xffffU)) +
                              view_x_offset;
            vertex.screen_y =
                static_cast<float>(static_cast<std::int16_t>(packed >> 16U));
            vertex.view_x = vertex.screen_x * vertex.view_z / vertex.screen_h;
            vertex.view_y = vertex.screen_y * vertex.view_z / vertex.screen_h;
            vertex.valid = true;
            vertex.exact_transform = exact;
            vertex.mesh_vertex_id =
                identity != 0U ? identity : 0x100000000ULL | packed;
            vertex.transform_lineage = transform_lineage;
            vertex.projection_epoch = projection_epoch;
            return render_gpu.writeGp0FromRam(packed, 0x1000U, &vertex);
          };

      require(render_gpu.writeGp0(0xe1000123U) &&
                  render_gpu.writeGp0(0x24112233U) &&
                  appendVertex(packPoint(10, 20), true) &&
                  render_gpu.writeGp0(0x004200aaU) &&
                  appendVertex(packPoint(30, 20), true) &&
                  render_gpu.writeGp0(0x008701bbU) &&
                  appendVertex(packPoint(10, 40), true) &&
                  render_gpu.writeGp0(0x000002ccU) &&
                  render_gpu.writeGp0(0x60000000U) &&
                  render_gpu.writeGp0(packPoint(0, 0)) &&
                  render_gpu.writeGp0(0x00100010U),
              "Could not build typed render command fixture");

      const auto &native_frame = render_gpu.nativeRenderFrame();
      require(native_frame.commands.size() == 3U &&
                  native_frame.scene_primitives.size() == 1U &&
                  native_frame.scene_positions.size() == 3U &&
                  native_frame.stats.commands == 3U &&
                  native_frame.stats.state_commands == 1U &&
                  native_frame.stats.scene_polygons == 1U &&
                  native_frame.stats.exact_view_polygons == 1U &&
                  native_frame.stats.identified_exact_vertices == 3U &&
                  native_frame.stats.exact_view_vertices == 3U &&
                  native_frame.stats.unique_scene_positions == 3U &&
                  native_frame.stats.shared_scene_vertices == 0U &&
                  native_frame.stats.projected_polygons == 0U &&
                  native_frame.stats.legacy_polygons == 0U &&
                  native_frame.stats.screen_primitives == 1U &&
                  native_frame.stats.truncated_words == 0U &&
                  native_frame.commands[1U].domain ==
                      mohu::NativeRenderDomain::scene &&
                  native_frame.commands[1U].geometry_source ==
                      mohu::NativeGeometrySource::exact_view &&
                  native_frame.commands[1U].scene_primitive == 0U,
              "Typed render contract misclassified exact scene geometry");
      const auto &primitive = native_frame.scene_primitives.front();
      require(primitive.word_offset == 1U && primitive.vertex_count == 3U &&
                  primitive.draw_state.draw_mode == 0x0123U &&
                  primitive.material.textured && !primitive.material.gouraud &&
                  !primitive.material.semi_transparent &&
                  !primitive.material.raw_texture &&
                  primitive.material.clut == 0x0042U &&
                  primitive.material.texture_page == 0x0087U &&
                  primitive.vertices[0U].u == 0xaaU &&
                  primitive.vertices[0U].v == 0U &&
                  primitive.vertices[0U].red == 0x33U &&
                  primitive.vertices[0U].green == 0x22U &&
                  primitive.vertices[0U].blue == 0x11U &&
                  primitive.vertices[0U].vertex_identity ==
                      (0x100000000ULL | packPoint(10, 20)) &&
                  primitive.vertices[0U].transform_lineage == 7U &&
                  primitive.vertices[0U].projection_epoch == 9U &&
                  primitive.vertices[0U].position_index == 0U,
              "Native scene primitive lost material or vertex data");
      const auto *cached_commands = native_frame.commands.data();
      require(render_gpu.nativeRenderFrame().commands.data() == cached_commands,
              "Typed render contract rebuilt an unchanged frame");
      const auto *cached_scene = native_frame.scene_primitives.data();
      require(render_gpu.nativeRenderFrame().scene_primitives.data() ==
                  cached_scene,
              "Native scene buffer rebuilt an unchanged frame");

      require(render_gpu.writeGp0(0x20000000U) &&
                  appendVertex(packPoint(50, 20), false) &&
                  appendVertex(packPoint(70, 20), false) &&
                  appendVertex(packPoint(50, 40), false) &&
                  render_gpu.writeGp0(0x20010203U) &&
                  appendVertex(packPoint(10, 20), true) &&
                  appendVertex(packPoint(30, 20), true) &&
                  appendVertex(packPoint(30, 40), true),
              "Could not append projected render fixture");
      const auto &mixed_frame = render_gpu.nativeRenderFrame();
      require(
          mixed_frame.stats.commands == 5U &&
              mixed_frame.stats.scene_polygons == 3U &&
              mixed_frame.stats.exact_view_polygons == 2U &&
              mixed_frame.stats.projected_polygons == 1U &&
              mixed_frame.scene_primitives.size() == 2U &&
              mixed_frame.scene_positions.size() == 4U &&
              mixed_frame.stats.unique_scene_positions == 4U &&
              mixed_frame.stats.shared_scene_vertices == 2U &&
              mixed_frame.scene_primitives[1U].vertices[0U].position_index ==
                  0U &&
              mixed_frame.scene_primitives[1U].vertices[1U].position_index ==
                  1U &&
              mixed_frame.stats.legacy_polygons == 0U,
          "Typed render contract lost projected fallback geometry");

      render_gpu.beginFrame();
      constexpr auto seam_identity = std::uint64_t{0x4000U};
      require(
          render_gpu.writeGp0(0x20010203U) &&
              appendVertex(packPoint(10, 10), true, seam_identity, 0.5F) &&
              appendVertex(packPoint(20, 10), true, seam_identity + 1U, 0.5F) &&
              appendVertex(packPoint(10, 20), true, seam_identity + 2U, 0.5F) &&
              render_gpu.writeGp0(0x20040506U) &&
              appendVertex(packPoint(20, 10), false) &&
              appendVertex(packPoint(10, 20), false) &&
              appendVertex(packPoint(20, 20), false) &&
              render_gpu.writeGp0(0x20070809U) &&
              appendVertex(packPoint(20, 10), true, seam_identity + 1U, 0.5F) &&
              appendVertex(packPoint(30, 10), true, seam_identity + 3U, 0.5F) &&
              appendVertex(packPoint(30, 20), true, seam_identity + 4U, 0.5F),
          "Could not build native/fallback seam fixture");
      const auto &seam_frame = render_gpu.nativeRenderFrame();
      const auto shared_position =
          seam_frame.scene_primitives[0U].vertices[1U].position_index;
      const auto &seam_position = seam_frame.scene_positions[shared_position];
      const auto seam_screen_x =
          seam_position.screen_offset_x +
          seam_position.view_x * seam_position.screen_h / seam_position.view_z;
      const auto seam_screen_y =
          seam_position.screen_offset_y +
          seam_position.view_y * seam_position.screen_h / seam_position.view_z;
      require(
          seam_frame.stats.exact_view_polygons == 2U &&
              seam_frame.stats.projected_polygons == 1U &&
              seam_frame.stats.hybrid_seam_edges == 1U &&
              seam_frame.stats.hybrid_seam_positions == 2U &&
              seam_frame.stats.hybrid_seam_rejections == 0U &&
              seam_screen_x == 20.0F && seam_screen_y == 10.0F &&
              seam_frame.scene_primitives[1U].vertices[0U].position_index ==
                  shared_position,
          "Native/fallback edge did not propagate packet-exact seam positions");

      render_gpu.beginFrame();
      constexpr auto small_identity = std::uint64_t{0x4800U};
      require(
          render_gpu.writeGp0(0x20010203U) &&
              appendVertex(packPoint(10, 10), true, small_identity, 0.25F) &&
              appendVertex(packPoint(10, 12), true, small_identity + 1U,
                           0.75F) &&
              appendVertex(packPoint(11, 11), true, small_identity + 2U,
                           0.25F) &&
              render_gpu.writeGp0(0x20040506U) &&
              appendVertex(packPoint(10, 11), false) &&
              appendVertex(packPoint(10, 12), false) &&
              appendVertex(packPoint(11, 12), false),
          "Could not build fractional small-geometry fixture");
      const auto &small_frame = render_gpu.nativeRenderFrame();
      const auto small_first_index =
          small_frame.scene_primitives[0U].vertices[0U].position_index;
      const auto small_second_index =
          small_frame.scene_primitives[0U].vertices[1U].position_index;
      const auto &small_first = small_frame.scene_positions[small_first_index];
      const auto &small_second =
          small_frame.scene_positions[small_second_index];
      const auto projected_x = [](const mohu::NativeScenePosition &position) {
        return position.screen_offset_x +
               position.view_x * position.screen_h / position.view_z;
      };
      require(small_frame.stats.exact_view_polygons == 1U &&
                  small_frame.stats.projected_polygons == 1U &&
                  small_frame.stats.hybrid_seam_edges == 0U &&
                  small_frame.stats.hybrid_seam_positions == 0U &&
                  projected_x(small_first) == 10.25F &&
                  projected_x(small_second) == 10.75F,
              "Collinear fallback subedge flattened fractional small geometry");

      render_gpu.beginFrame();
      constexpr auto shared_identity = std::uint64_t{0x5000U};
      require(
          render_gpu.writeGp0(0x20010203U) &&
              appendVertex(packPoint(10, 10), true, shared_identity) &&
              appendVertex(packPoint(20, 10), true, shared_identity + 1U) &&
              appendVertex(packPoint(10, 20), true, shared_identity + 2U) &&
              render_gpu.writeGp0(0x20040506U) &&
              appendVertex(packPoint(10, 10), true, shared_identity, 1.0F) &&
              appendVertex(packPoint(20, 10), true, shared_identity + 1U) &&
              appendVertex(packPoint(10, 20), true, shared_identity + 2U),
          "Could not build conflicting identity fixture");
      const auto &conflict_frame = render_gpu.nativeRenderFrame();
      require(
          conflict_frame.stats.commands == 2U &&
              conflict_frame.stats.exact_view_polygons == 2U &&
              conflict_frame.scene_primitives.size() == 2U &&
              conflict_frame.scene_positions.size() == 4U &&
              conflict_frame.stats.unique_scene_positions == 4U &&
              conflict_frame.stats.shared_scene_vertices == 2U &&
              conflict_frame.stats.conflicting_scene_vertices == 1U &&
              conflict_frame.scene_primitives[0U].vertices[0U].position_index ==
                  0U &&
              conflict_frame.scene_primitives[1U].vertices[0U].position_index ==
                  3U &&
              conflict_frame.scene_primitives[1U].vertices[1U].position_index ==
                  1U &&
              conflict_frame.scene_primitives[1U].vertices[2U].position_index ==
                  2U,
          "Conflicting mesh identity collapsed distinct scene positions");

      render_gpu.beginFrame();
      require(render_gpu.writeGp0(0x20010203U) &&
                  appendVertex(packPoint(10, 10), true) &&
                  appendVertex(packPoint(20, 10), true, 0U, 0.0F, 255.0F) &&
                  appendVertex(packPoint(10, 20), true),
              "Could not build mixed-camera native fixture");
      const auto &mixed_camera_frame = render_gpu.nativeRenderFrame();
      require(mixed_camera_frame.stats.exact_view_polygons == 0U &&
                  mixed_camera_frame.stats.projected_polygons == 1U &&
                  mixed_camera_frame.scene_primitives.empty() &&
                  mixed_camera_frame.scene_positions.empty() &&
                  mixed_camera_frame.commands[0U].geometry_source ==
                      mohu::NativeGeometrySource::projected,
              "Mixed camera tuple entered the native scene pipeline");

      render_gpu.beginFrame();
      require(
          render_gpu.writeGp0(0x20010203U) &&
              appendVertex(packPoint(10, 10), true) &&
              appendVertex(packPoint(20, 10), true, 0U, 0.0F, 256.0F, 0U, 9U) &&
              appendVertex(packPoint(10, 20), true),
          "Could not build incomplete exact-provenance fixture");
      const auto &incomplete_exact_frame = render_gpu.nativeRenderFrame();
      require(incomplete_exact_frame.stats.exact_view_polygons == 0U &&
                  incomplete_exact_frame.stats.projected_polygons == 1U &&
                  incomplete_exact_frame.scene_primitives.empty() &&
                  incomplete_exact_frame.scene_positions.empty() &&
                  incomplete_exact_frame.commands[0U].geometry_source ==
                      mohu::NativeGeometrySource::projected,
              "Mixed incomplete/exact tuple entered native scene");

      render_gpu.beginFrame();
      require(render_gpu.writeGp0(0x20000000U) &&
                  render_gpu.nativeRenderFrame().stats.commands == 0U &&
                  render_gpu.nativeRenderFrame().scene_primitives.empty() &&
                  render_gpu.nativeRenderFrame().scene_positions.empty() &&
                  render_gpu.nativeRenderFrame().stats.truncated_words == 1U,
              "Typed render contract accepted a truncated GP0 command");

      render_gpu.beginFrame();
      require(render_gpu.writeGp0(0xe5002ffdU) &&
                  render_gpu.writeGp0(0x3f010203U) &&
                  appendVertex(packPoint(10, 10), true) &&
                  render_gpu.writeGp0(0x00210011U) &&
                  render_gpu.writeGp0(0x00040506U) &&
                  appendVertex(packPoint(20, 10), true) &&
                  render_gpu.writeGp0(0x00430022U) &&
                  render_gpu.writeGp0(0x00070809U) &&
                  appendVertex(packPoint(10, 20), true) &&
                  render_gpu.writeGp0(0x00000033U) &&
                  render_gpu.writeGp0(0x000a0b0cU) &&
                  appendVertex(packPoint(20, 20), true) &&
                  render_gpu.writeGp0(0x00000044U),
              "Could not build gouraud quad scene fixture");
      const auto &quad_frame = render_gpu.nativeRenderFrame();
      const auto &quad = quad_frame.scene_primitives.front();
      require(
          quad_frame.stats.exact_view_polygons == 1U &&
              quad_frame.stats.identified_exact_vertices == 4U &&
              quad_frame.scene_primitives.size() == 1U &&
              quad.vertex_count == 4U && quad.material.textured &&
              quad_frame.scene_positions.size() == 4U &&
              quad_frame.stats.unique_scene_positions == 4U &&
              quad_frame.stats.shared_scene_vertices == 0U &&
              quad.material.gouraud && quad.material.semi_transparent &&
              quad.material.raw_texture && quad.material.clut == 0x21U &&
              quad.material.texture_page == 0x43U &&
              quad.draw_state.offset_x == -3 && quad.draw_state.offset_y == 5 &&
              quad.vertices[1U].u == 0x22U && quad.vertices[1U].red == 0x06U &&
              quad.vertices[1U].green == 0x05U &&
              quad.vertices[1U].blue == 0x04U && quad.vertices[3U].u == 0x44U &&
              quad.vertices[3U].red == 0x0cU &&
              quad.vertices[3U].green == 0x0bU &&
              quad.vertices[3U].blue == 0x0aU,
          "Gouraud quad scene primitive decoded incorrect packet fields");
    }

    {
      mohu::GpuCommandStream compact_gpu;
      compact_gpu.setProjectionTracking(false);
      std::array<sf::psx::GteProjectedVertex, 3U> catalog{};
      const std::array coordinates{packPoint(10, 20), packPoint(30, 20),
                                   packPoint(10, 40)};
      for (std::size_t vertex{}; vertex < catalog.size(); ++vertex) {
        const auto packed = coordinates[vertex];
        auto &projection = catalog[vertex];
        projection.packed_sxy = packed;
        projection.view_x =
            static_cast<float>(static_cast<std::int16_t>(packed & 0xffffU)) *
            2.0F;
        projection.view_y =
            static_cast<float>(static_cast<std::int16_t>(packed >> 16U)) * 2.0F;
        projection.view_z = 512.0F;
        projection.projective_depth = projection.view_z;
        projection.screen_x = projection.view_x * 0.5F;
        projection.screen_y = projection.view_y * 0.5F;
        projection.screen_h = 256.0F;
        projection.source_vertex_id = vertex + 1U;
        projection.mesh_vertex_id = 0x2000U + vertex;
        projection.transform_lineage = 4U;
        projection.projection_epoch = 6U;
        projection.valid = true;
        projection.exact_transform = true;
      }
      require(compact_gpu.writeGp0(0x20010203U),
              "Could not begin compact catalog polygon");
      for (std::size_t vertex{}; vertex < catalog.size(); ++vertex) {
        require(compact_gpu.writeGp0FromRam(
                    coordinates[vertex],
                    0x2000U + static_cast<std::uint32_t>(vertex) * 4U, nullptr,
                    vertex + 1U),
                "Could not append compact catalog handle");
      }

      const auto &native = compact_gpu.nativeRenderFrame(catalog);
      const auto &pipeline = compact_gpu.nativeScenePipeline(catalog);
      require(compact_gpu.frameProjections().empty() &&
                  native.stats.exact_view_polygons == 1U &&
                  native.scene_positions.size() == 3U &&
                  native.scene_positions[0U].screen_h == 256.0F &&
                  pipeline.triangles.size() == 1U &&
                  pipeline.triangles[0U].source_word_offset == 0U &&
                  pipeline.triangles[0U].source_word_count == 4U &&
                  pipeline.triangles[0U].source_opcode == 0x20U,
              "Compact catalog did not reach the native scene pipeline");

      compact_gpu.beginFrame();
      require(
          compact_gpu.writeGp0(0x20010203U) &&
              compact_gpu.writeGp0FromRam(coordinates[0U], 0x2000U, nullptr,
                                          1U) &&
              compact_gpu.writeGp0FromRam(coordinates[1U], 0x2004U, nullptr,
                                          4U) &&
              compact_gpu.writeGp0FromRam(coordinates[2U], 0x2008U, nullptr,
                                          3U) &&
              compact_gpu.nativeRenderFrame(catalog).stats.legacy_polygons ==
                  1U,
          "Invalid compact handle did not fail closed");
    }

    {
      constexpr std::uint32_t gpu_dma = 0x1f8010a0U;
      constexpr std::uint32_t dpcr = 0x1f8010f0U;
      constexpr std::array payloads{0xa1a2a3a4U, 0xb1b2b3b4U, 0xc1c2c3c4U};
      constexpr std::array source_addresses{0x00001004U, 0x00001008U,
                                            0x00002004U};

      sf::psx::R3000Runtime dma_runtime;
      sf::psx::PsxMachine dma_machine{dma_runtime};
      mohu::GpuCommandStream dma_gpu;
      dma_machine.attachGpuPort(&dma_gpu);
      require(dma_runtime.write32(0x1000U, 0x02002000U) &&
                  dma_runtime.write32(0x1004U, payloads[0]) &&
                  dma_runtime.write32(0x1008U, payloads[1]) &&
                  dma_runtime.write32(0x2000U, 0x01800000U) &&
                  dma_runtime.write32(0x2004U, payloads[2]) &&
                  dma_runtime.write32(dpcr,
                                      dma_machine.dma().dpcr() | 0x00000800U) &&
                  dma_runtime.write32(gpu_dma, 0x1000U) &&
                  dma_runtime.write32(gpu_dma + 8U, 0x01000401U),
              "Could not start provenance linked-list GPU DMA");
      dma_machine.advanceTicks(31U);
      require(dma_gpu.frameWords().size() == payloads.size() &&
                  dma_gpu.frameSourceAddresses().size() ==
                      source_addresses.size() &&
                  std::equal(dma_gpu.frameWords().begin(),
                             dma_gpu.frameWords().end(), payloads.begin()) &&
                  std::equal(dma_gpu.frameSourceAddresses().begin(),
                             dma_gpu.frameSourceAddresses().end(),
                             source_addresses.begin()) &&
                  aligned(dma_gpu),
              "Linked-list GPU DMA detached RAM source provenance");
      dma_gpu.writeGp1(0x01000000U);
      require(dma_gpu.frameWords().empty() &&
                  dma_gpu.frameSourceAddresses().empty() && aligned(dma_gpu),
              "GP1 reset retained linked-list source provenance");
    }

    mohu::GpuCommandStream identity_disabled_gpu;
    identity_disabled_gpu.setProjectionIdentityTracking(false);
    require(!identity_disabled_gpu.projectionIdentityTracking() &&
                identity_disabled_gpu.writeGp0(0xe1000000U) &&
                identity_disabled_gpu.writeGp0FromRam(projected.packed_sxy,
                                                      0x1234U, &projected,
                                                      source_identity) &&
                identity_disabled_gpu.frameWords().size() == 2U &&
                identity_disabled_gpu.frameSourceAddresses().size() == 2U &&
                identity_disabled_gpu.frameSourceAddresses()[0] ==
                    mohu::GpuCommandStream::direct_source_address &&
                identity_disabled_gpu.frameSourceAddresses()[1] == 0x1234U &&
                identity_disabled_gpu.frameProjections().size() == 2U &&
                identity_disabled_gpu.frameProjectionIdentities().empty() &&
                !identity_disabled_gpu.frameProjections()[0].valid &&
                identity_disabled_gpu.frameProjections()[1] == projected,
            "Identity fast-disable dropped or changed precise sidecars");
    identity_disabled_gpu.setProjectionIdentityTracking(true);
    require(identity_disabled_gpu.projectionIdentityTracking() &&
                identity_disabled_gpu.frameProjectionIdentities().empty(),
            "Identity transport toggle retained stale frame identities");

    using sf::platform::detail::planPgxpPolygonCoherence;
    const std::array exact_pair{
        makeQuad(0, 0, 10, 10),
        makeQuad(10, 0, 20, 10),
    };
    const auto exact_pair_decisions = planPgxpPolygonCoherence(exact_pair);
    require(preciseCount(exact_pair_decisions) == 2U &&
                !exact_pair_decisions[0].coherence_fallback &&
                !exact_pair_decisions[1].coherence_fallback,
            "Matching reversed shared edge did not remain fully precise");

    auto subpixel_mismatch = exact_pair;
    // Still inside the primitive's packed-vs-float eligibility tolerance, but
    // far enough apart to open a visible high-resolution shared edge.
    subpixel_mismatch[1].precise_vertices[0].screen_x += 0.8F;
    const auto subpixel_decisions = planPgxpPolygonCoherence(subpixel_mismatch);
    require(preciseCount(subpixel_decisions) == 0U &&
                subpixel_decisions[0].coherence_fallback &&
                subpixel_decisions[1].coherence_fallback,
            "Inconsistent eligible shared-edge endpoints were mixed precise");

    auto depth_mismatch = exact_pair;
    depth_mismatch[1].precise_vertices[0].view_z += 1.0F;
    const auto depth_decisions = planPgxpPolygonCoherence(depth_mismatch);
    require(preciseCount(depth_decisions) == 0U &&
                depth_decisions[0].coherence_fallback &&
                depth_decisions[1].coherence_fallback,
            "Incompatible precise depth on a shared endpoint was accepted");

    std::array nontransitive_overlap{
        makeQuad(0, 0, 10, 10),
        makeQuad(10, 1, 20, 20),
        makeQuad(10, 2, 30, 9),
    };
    for (const auto vertex : {0U, 2U}) {
      nontransitive_overlap[1].precise_vertices[vertex].screen_x += 0.05F;
      nontransitive_overlap[2].precise_vertices[vertex].screen_x += 0.10F;
    }
    const auto nontransitive_decisions =
        planPgxpPolygonCoherence(nontransitive_overlap);
    require(preciseCount(nontransitive_decisions) == 1U &&
                nontransitive_decisions[0].coherence_fallback &&
                nontransitive_decisions[1].allow_precise &&
                nontransitive_decisions[2].coherence_fallback,
            "Local edge mismatch propagated through a compatible neighbour");

    const std::array clipped_subsegment{
        makeQuad(0, 0, 10, 20),
        makeQuad(10, 5, 20, 15, false),
    };
    const auto clipped_decisions = planPgxpPolygonCoherence(clipped_subsegment);
    require(preciseCount(clipped_decisions) == 1U &&
                clipped_decisions[0].allow_precise &&
                !clipped_decisions[0].coherence_fallback &&
                !clipped_decisions[1].allow_precise,
            "Legacy clipped subsegment demoted a precise neighbour");

    const std::array transitive_strip{
        makeQuad(0, 0, 10, 10),
        makeQuad(10, 0, 20, 10),
        makeQuad(20, 0, 30, 10, false),
    };
    const auto transitive_decisions =
        planPgxpPolygonCoherence(transitive_strip);
    require(preciseCount(transitive_decisions) == 2U &&
                transitive_decisions[0].allow_precise &&
                transitive_decisions[1].allow_precise &&
                !transitive_decisions[2].allow_precise,
            "Legacy fallback propagated across a precise polygon strip");

    auto different_context = exact_pair;
    different_context[1].context.draw_mode = 1U;
    different_context[1].precise_eligible = false;
    for (auto &vertex : different_context[1].precise_vertices) {
      vertex.valid = false;
    }
    const auto context_decisions = planPgxpPolygonCoherence(different_context);
    require(context_decisions[0].allow_precise &&
                !context_decisions[0].coherence_fallback &&
                !context_decisions[1].allow_precise,
            "Unrelated draw contexts were coupled by identical screen edges");

    const std::array point_touch{
        makeQuad(0, 0, 10, 10),
        makeQuad(10, 10, 20, 20, false),
    };
    const auto point_touch_decisions = planPgxpPolygonCoherence(point_touch);
    require(point_touch_decisions[0].allow_precise &&
                !point_touch_decisions[0].coherence_fallback,
            "Point-only contact was treated as a shared polygon edge");

    auto internal_diagonal = makeQuad(0, 0, 1, 1, false);
    internal_diagonal.vertex_count = 3U;
    internal_diagonal.packed_xy = {packPoint(10, 0), packPoint(0, 10),
                                   packPoint(20, 20), 0U};
    for (auto &vertex : internal_diagonal.precise_vertices) {
      vertex.valid = false;
    }
    const std::array diagonal_only_contact{
        makeQuad(0, 0, 10, 10),
        internal_diagonal,
    };
    const auto diagonal_decisions =
        planPgxpPolygonCoherence(diagonal_only_contact);
    require(diagonal_decisions[0].allow_precise &&
                !diagonal_decisions[0].coherence_fallback,
            "Quad strip's internal 1-2 diagonal coupled unrelated polygons");

    const std::array signed_extent{
        makeQuad(-32768, -10, 32767, 0),
        makeQuad(-100, 1, 100, 10, false),
    };
    const auto signed_extent_decisions =
        planPgxpPolygonCoherence(signed_extent);
    require(signed_extent_decisions[0].allow_precise,
            "Non-overlapping signed-extreme edges overflowed coherence math");

    std::cout << "MOHU GPU command stream tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "MOHU GPU command stream test failure: " << error.what()
              << '\n';
    return 1;
  }
}
