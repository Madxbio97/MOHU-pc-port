#include "mohu/native_world_camera.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

[[nodiscard]] sf::psx::GteExactComponent component(double value,
                                                   std::uint32_t generation) {
  return {.value = value,
          .lineage = 1U,
          .generation = generation,
          .valid = true,
          .enhanced = true};
}

} // namespace

int main() {
  try {
    sf::psx::R3000PgxpTransformCheckpoint checkpoint{};
    checkpoint.valid = true;
    checkpoint.exact.generation = 7U;
    checkpoint.exact.camera_revision = 11U;
    checkpoint.exact.projection_revision = 12U;
    checkpoint.exact.rotation = {
        component(4096.0, 7U), component(0.0, 7U), component(0.0, 7U),
        component(0.0, 7U), component(4096.0, 7U), component(0.0, 7U),
        component(0.0, 7U), component(0.0, 7U), component(4096.0, 7U)};
    checkpoint.exact.translation = {component(100.25, 7U),
                                    component(-50.5, 7U),
                                    component(900.75, 7U)};
    checkpoint.gte_witness.control[24U] =
        static_cast<std::uint32_t>(256 * 65536);
    checkpoint.gte_witness.control[25U] =
        static_cast<std::uint32_t>(120 * 65536);
    checkpoint.gte_witness.control[26U] = 400U;

    const auto camera = mohu::nativeWorldCameraFromCheckpoint(checkpoint);
    require(camera.valid, "coherent camera was rejected");
    require(camera.offset_x == 256.0 && camera.offset_y == 120.0,
            "projection offsets mismatch");
    const auto view =
        mohu::transformNativeWorldPosition(camera, {10, 20, 30});
    require(std::abs(view.x - 110.25) < 0.000001 &&
                std::abs(view.y + 30.5) < 0.000001 &&
                std::abs(view.z - 930.75) < 0.000001,
            "exact world transform mismatch");

    checkpoint.exact.rotation[3].generation = 6U;
    require(!mohu::nativeWorldCameraFromCheckpoint(checkpoint).valid,
            "mixed-generation camera was accepted");

    sf::psx::R3000Runtime runtime;
    runtime.setPgxpExactTransformTracking(true);
    require(runtime.setPgxpTransformTracking(true),
            "cannot enable exact-transform fixture");
    require(runtime.capturePgxpTransformCheckpoint()
                .exact.transform_twin_enabled,
            "exact-transform capture did not start enabled");
    runtime.setPgxpExactTransformCaptureEnabled(false);
    require(!runtime.capturePgxpTransformCheckpoint()
                 .exact.transform_twin_enabled,
            "exact-transform capture gate did not disable twin work");
    runtime.setPgxpExactTransformCaptureEnabled(true);
    require(runtime.capturePgxpTransformCheckpoint().exact.transform_twin_enabled,
            "exact-transform capture gate did not restore twin work");
    std::cout << "mohu_native_world_camera_tests: ok\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mohu_native_world_camera_tests: " << error.what() << '\n';
    return 1;
  }
}
