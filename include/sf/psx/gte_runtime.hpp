#pragma once

#include <array>
#include <cstdint>

namespace sf::psx {

struct GteExactComponent {
  double value{};
  std::uint64_t lineage{};
  std::uint32_t generation{};
  bool valid{};
  bool enhanced{};
};

struct GteExactWord {
  std::uint32_t raw{};
  GteExactComponent scalar{};
  std::array<GteExactComponent, 2> halves{};
};

struct GteExactState {
  std::array<GteExactComponent, 9> rotation{};
  std::array<GteExactComponent, 3> translation{};
  std::array<std::array<GteExactComponent, 3>, 3> vectors{};
  std::array<GteExactComponent, 3> result{};
  std::array<GteExactComponent, 9> pending_rotation{};
  std::array<GteExactComponent, 3> pending_translation{};
  std::array<std::array<GteExactComponent, 3>, 3> pending_vectors{};
  std::array<GteExactComponent, 3> pending_result{};
  std::uint16_t rotation_mask{};
  std::uint8_t translation_mask{};
  std::array<std::uint8_t, 3> vector_masks{};
  std::uint8_t result_mask{};
  std::uint32_t generation{1U};
  std::uint64_t camera_revision{1U};
  std::uint64_t projection_revision{1U};
  std::uint64_t next_revision{2U};
  std::uint64_t rotation_value_identity{};
  std::uint64_t translation_value_identity{};
  std::array<std::uint64_t, 3> vector_value_identities{};
  std::uint64_t camera_transform_lineage{};
  std::uint64_t camera_value_identity{};
  std::uint64_t projection_value_identity{};
  bool rotation_values_enhanced{};
  bool translation_values_enhanced{};
  std::array<bool, 3> vector_values_enhanced{};
  bool transform_twin_enabled{true};
  bool source_identity_enabled{true};
};

struct GteProjectedVertex {
  std::uint32_t packed_sxy{};
  float view_x{};
  float view_y{};
  float view_z{};
  float projective_depth{};
  float screen_x{};
  float screen_y{};
  float screen_h{};
  float screen_offset_x{};
  float screen_offset_y{};
  bool ir_saturated{};
  bool depth_saturated{};
  bool divide_overflow{};
  bool screen_saturated{};
  bool valid{};
  bool exact_transform{};
  bool fractional_transform{};
  std::uint8_t enhanced_sources{};
  std::uint64_t source_vertex_id{};

  static constexpr std::uint8_t enhanced_rotation = 1U << 0U;
  static constexpr std::uint8_t enhanced_translation = 1U << 1U;
  static constexpr std::uint8_t enhanced_vector = 1U << 2U;
  std::uint64_t mesh_vertex_id{};
  std::uint64_t transform_lineage{};
  std::uint64_t projection_epoch{};

  [[nodiscard]] constexpr bool hasExactTransformProvenance() const noexcept {
    return exact_transform && transform_lineage != 0U && projection_epoch != 0U;
  }

  [[nodiscard]] constexpr bool pgxpEligible() const noexcept {
    if (!valid) {
      return false;
    }
    return exact_transform || (!ir_saturated && !depth_saturated &&
                               !divide_overflow && !screen_saturated);
  }

  friend bool operator==(const GteProjectedVertex &,
                         const GteProjectedVertex &) = default;
};

struct GteState {
  std::array<std::uint32_t, 32> data{};
  std::array<std::uint32_t, 32> control{};
  std::array<GteProjectedVertex, 3> projected{};
  double precise_nclip_area{};
  bool precise_nclip_valid{};
};

struct GteProjectionCommandBackend {
  using Execute = bool (*)(void *context, GteState &state,
                           std::uint32_t instruction,
                           bool capture_projection) noexcept;

  void *context{};
  Execute execute{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return execute != nullptr;
  }
};

// Integer Geometry Transformation Engine state used by original gameplay math.
// Unsupported commands remain an explicit deterministic VM stop.
class GteRuntime final {
public:
  [[nodiscard]] static std::uint32_t readData(const GteState &state,
                                              std::uint8_t index) noexcept;
  [[nodiscard]] static std::uint32_t readControl(const GteState &state,
                                                 std::uint8_t index) noexcept;
  static void writeData(GteState &state, std::uint8_t index,
                        std::uint32_t value,
                        const GteProjectedVertex *projected = nullptr,
                        GteExactState *exact_state = nullptr,
                        const GteExactWord *exact_word = nullptr) noexcept;
  static void writeControl(GteState &state, std::uint8_t index,
                           std::uint32_t value,
                           GteExactState *exact_state = nullptr,
                           const GteExactWord *exact_word = nullptr) noexcept;
  [[nodiscard]] static bool
  executeCommand(GteState &state, std::uint32_t instruction,
                 GteExactState *exact_state = nullptr,
                 bool capture_projection = false,
                 bool preserve_projection_precision = false) noexcept;
  [[nodiscard]] static GteExactWord exactData(const GteState &state,
                                              const GteExactState &exact_state,
                                              std::uint8_t index) noexcept;
  [[nodiscard]] static const GteProjectedVertex *
  projectedVertex(const GteState &state, std::uint8_t index) noexcept;
};

} // namespace sf::psx
