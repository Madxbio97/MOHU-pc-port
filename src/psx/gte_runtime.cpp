#include "sf/psx/gte_runtime.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace sf::psx {
namespace {

constexpr std::uint32_t flag_error = 1U << 31U;
constexpr std::uint32_t flag_ir1_saturated = 1U << 24U;
constexpr std::uint32_t flag_ir2_saturated = 1U << 23U;
constexpr std::uint32_t flag_ir3_saturated = 1U << 22U;
constexpr std::uint32_t flag_color_r_saturated = 1U << 21U;
constexpr std::uint32_t flag_color_g_saturated = 1U << 20U;
constexpr std::uint32_t flag_color_b_saturated = 1U << 19U;
constexpr std::uint32_t flag_sz_otz_saturated = 1U << 18U;
constexpr std::uint32_t flag_divide_overflow = 1U << 17U;
constexpr std::uint32_t flag_mac0_overflow = 1U << 16U;
constexpr std::uint32_t flag_mac0_underflow = 1U << 15U;
constexpr std::uint32_t flag_sx_saturated = 1U << 14U;
constexpr std::uint32_t flag_sy_saturated = 1U << 13U;
constexpr std::uint32_t flag_ir0_saturated = 1U << 12U;
constexpr std::uint32_t error_source_mask = 0x7f87e000U;

std::uint32_t signExtend16(std::uint32_t value) noexcept {
  return static_cast<std::uint32_t>(
      static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
}

std::int32_t signedWord(std::uint32_t value) noexcept {
  return std::bit_cast<std::int32_t>(value);
}

std::int32_t signedHalf(std::uint32_t value) noexcept {
  return static_cast<std::int16_t>(value);
}

std::int32_t lowSignedWord(std::int64_t value) noexcept {
  return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

void updateErrorFlag(GteState &state) noexcept {
  auto &flags = state.control[31];
  flags &= ~flag_error;
  if ((flags & error_source_mask) != 0U) {
    flags |= flag_error;
  }
}

std::int32_t clampIr(GteState &state, std::uint8_t component,
                     std::int32_t value, bool limit_mode) noexcept {
  const auto minimum = limit_mode ? 0 : -0x8000;
  constexpr auto maximum = 0x7fff;
  if (value < minimum) {
    state.control[31] |= flag_ir1_saturated >> (component - 1U);
    return minimum;
  }
  if (value > maximum) {
    state.control[31] |= flag_ir1_saturated >> (component - 1U);
    return maximum;
  }
  return value;
}

void setMacAndIr(GteState &state, std::uint8_t component, std::int64_t value,
                 std::uint8_t shift, bool limit_mode) noexcept {
  constexpr std::int64_t minimum_mac = -(std::int64_t{1} << 43U);
  constexpr std::int64_t maximum_mac = (std::int64_t{1} << 43U) - 1;
  if (value < minimum_mac) {
    state.control[31] |= 1U << (28U - component);
  } else if (value > maximum_mac) {
    state.control[31] |= 1U << (31U - component);
  }
  const auto shifted = lowSignedWord(value >> shift);
  state.data[24U + component] = std::bit_cast<std::uint32_t>(shifted);
  state.data[8U + component] = std::bit_cast<std::uint32_t>(
      clampIr(state, component, shifted, limit_mode));
}

std::int64_t signExtendMac(GteState &state, std::uint8_t component,
                           std::int64_t value) noexcept {
  constexpr std::int64_t minimum_mac = -(std::int64_t{1} << 43U);
  constexpr std::int64_t maximum_mac = (std::int64_t{1} << 43U) - 1;
  if (value < minimum_mac) {
    state.control[31] |= 1U << (28U - component);
  } else if (value > maximum_mac) {
    state.control[31] |= 1U << (31U - component);
  }
  constexpr auto mask = (std::uint64_t{1} << 44U) - 1U;
  auto truncated = static_cast<std::uint64_t>(value) & mask;
  if ((truncated & (std::uint64_t{1} << 43U)) != 0U) {
    truncated |= ~mask;
  }
  return std::bit_cast<std::int64_t>(truncated);
}

std::int16_t packedHalf(std::uint32_t value, bool high) noexcept {
  return static_cast<std::int16_t>(high ? value >> 16U : value);
}

std::int16_t matrixElement(const GteState &state, std::uint8_t matrix,
                           std::uint8_t row, std::uint8_t column) noexcept {
  if (matrix == 3U) {
    const auto red = static_cast<std::int16_t>((state.data[6] & 0xffU) << 4U);
    if (row == 0U) {
      if (column == 0U) {
        return static_cast<std::int16_t>(-red);
      }
      if (column == 1U) {
        return red;
      }
      return static_cast<std::int16_t>(state.data[8]);
    }
    const auto value = row == 1U ? packedHalf(state.control[1], false)
                                 : packedHalf(state.control[2], false);
    return value;
  }

  const auto base = static_cast<std::uint8_t>(matrix * 8U);
  const auto element = static_cast<std::uint8_t>(row * 3U + column);
  return packedHalf(state.control[base + element / 2U], (element & 1U) != 0U);
}

std::int16_t vectorElement(const GteState &state, std::uint8_t vector,
                           std::uint8_t component) noexcept {
  if (vector == 3U) {
    return static_cast<std::int16_t>(state.data[9U + component]);
  }
  const auto base = static_cast<std::uint8_t>(vector * 2U);
  if (component < 2U) {
    return packedHalf(state.data[base], component == 1U);
  }
  return static_cast<std::int16_t>(state.data[base + 1U]);
}

std::int32_t translationElement(const GteState &state, std::uint8_t translation,
                                std::uint8_t component) noexcept {
  switch (translation) {
  case 0U:
    return signedWord(state.control[5U + component]);
  case 1U:
    return signedWord(state.control[13U + component]);
  case 2U:
    return signedWord(state.control[21U + component]);
  default:
    return 0;
  }
}

constexpr std::uint64_t lineage_offset = 1469598103934665603ULL;
constexpr std::uint64_t lineage_prime = 1099511628211ULL;

std::uint64_t mixLineage(std::uint64_t value, std::uint64_t part) noexcept {
  value ^= part;
  value *= lineage_prime;
  return value;
}

std::uint64_t rawLineage(std::uint32_t generation, std::uint32_t raw,
                         std::uint32_t domain) noexcept {
  auto value = mixLineage(lineage_offset, generation);
  value = mixLineage(value, raw);
  return mixLineage(value, domain);
}

bool exactCurrent(const GteExactComponent &component,
                  const GteExactState &exact) noexcept {
  return component.valid && component.generation == exact.generation &&
         component.lineage != 0U && std::isfinite(component.value);
}

bool exactHalfWitness(const GteExactComponent &component, std::uint32_t raw,
                      std::uint8_t slot, const GteExactState &exact) noexcept {
  if (!exactCurrent(component, exact) || slot > 1U)
    return false;
  const auto value = component.value;
  const auto witness = static_cast<double>(packedHalf(raw, slot != 0U));
  return value >= witness && value < witness + 1.0;
}

GteExactComponent rawExact(GteExactState &exact, double value,
                           std::uint32_t raw, std::uint32_t domain) noexcept {
  return {value, rawLineage(exact.generation, raw, domain), exact.generation,
          std::isfinite(value), false};
}

GteExactComponent exactHalfOrRaw(const GteExactWord *word, std::uint32_t raw,
                                 std::uint8_t slot, double fallback,
                                 std::uint32_t domain,
                                 GteExactState &exact) noexcept {
  if (word != nullptr && word->raw == raw && slot < word->halves.size()) {
    if (exactCurrent(word->halves[slot], exact)) {
      return word->halves[slot];
    }
    if (slot == 0U && exactHalfWitness(word->scalar, raw, slot, exact)) {
      return word->scalar;
    }
  }
  return rawExact(exact, fallback, raw, domain);
}

GteExactComponent exactScalarOrRaw(const GteExactWord *word, std::uint32_t raw,
                                   double fallback, std::uint32_t domain,
                                   GteExactState &exact) noexcept {
  if (word != nullptr && word->raw == raw &&
      exactCurrent(word->scalar, exact)) {
    return word->scalar;
  }
  return rawExact(exact, fallback, raw, domain);
}

template <std::size_t Size>
std::uint64_t
tupleLineage(const std::array<GteExactComponent, Size> &components,
             std::uint32_t generation, std::uint64_t domain) noexcept {
  auto lineage = mixLineage(lineage_offset, generation);
  lineage = mixLineage(lineage, domain);
  for (const auto &component : components) {
    lineage = mixLineage(lineage, component.lineage);
    lineage =
        mixLineage(lineage, std::bit_cast<std::uint64_t>(component.value));
  }
  return lineage == 0U ? 1U : lineage;
}

template <std::size_t Size>
std::uint64_t
tupleValueIdentity(const std::array<GteExactComponent, Size> &components,
                   std::uint64_t domain) noexcept {
  auto identity = mixLineage(lineage_offset, domain);
  for (const auto &component : components) {
    identity =
        mixLineage(identity, std::bit_cast<std::uint64_t>(component.value));
    identity = mixLineage(identity, component.enhanced ? 1U : 0U);
  }
  return identity == 0U ? 1U : identity;
}
template <std::size_t Size>
bool tupleEnhanced(
    const std::array<GteExactComponent, Size> &components) noexcept {
  return std::ranges::any_of(
      components, [](const auto &component) { return component.enhanced; });
}

template <std::size_t Size>
void publishTuple(std::array<GteExactComponent, Size> &destination,
                  const std::array<GteExactComponent, Size> &source,
                  std::uint32_t generation, std::uint64_t domain) noexcept {
  const auto lineage = tupleLineage(source, generation, domain);
  destination = source;
  for (auto &component : destination) {
    component.lineage = lineage;
  }
}

template <std::size_t Size>
std::uint64_t
publishTupleIdentity(std::array<GteExactComponent, Size> &destination,
                     const std::array<GteExactComponent, Size> &source,
                     std::uint32_t generation, std::uint64_t domain) noexcept {
  publishTuple(destination, source, generation, domain);
  return tupleValueIdentity(destination, domain);
}

void invalidateExactResult(GteExactState *exact) noexcept {
  if (exact == nullptr) {
    return;
  }
  if (exact->result_mask == 0U && !exact->result[0].valid &&
      !exact->pending_result[0].valid) {
    return;
  }
  exact->result.fill({});
  exact->pending_result.fill({});
  exact->result_mask = 0U;
}

std::uint64_t allocateRevision(GteExactState &exact) noexcept {
  if (exact.next_revision == 0U ||
      exact.next_revision == std::numeric_limits<std::uint64_t>::max()) {
    exact.rotation.fill({});
    exact.translation.fill({});
    for (auto &vector : exact.vectors) {
      vector.fill({});
    }
    invalidateExactResult(&exact);
    exact.pending_rotation.fill({});
    exact.pending_translation.fill({});
    for (auto &vector : exact.pending_vectors) {
      vector.fill({});
    }
    exact.rotation_mask = 0U;
    exact.translation_mask = 0U;
    exact.vector_masks.fill(0U);
    exact.rotation_value_identity = 0U;
    exact.translation_value_identity = 0U;
    exact.vector_value_identities.fill(0U);
    exact.rotation_values_enhanced = false;
    exact.translation_values_enhanced = false;
    exact.vector_values_enhanced.fill(false);
    if (++exact.generation == 0U) {
      exact.generation = 1U;
    }
    exact.camera_revision = 0U;
    exact.projection_revision = 0U;
    exact.next_revision = 2U;
    return 0U;
  }
  return exact.next_revision++;
}

void publishCameraTuple(GteExactState &exact) noexcept {
  exact.camera_transform_lineage = 0U;
  exact.camera_value_identity = 0U;
  exact.projection_value_identity = 0U;
  const auto revision = allocateRevision(exact);
  if (revision != 0U) {
    exact.camera_revision = revision;
    exact.projection_revision = revision;
  }
  if (exact.rotation_value_identity == 0U ||
      exact.translation_value_identity == 0U ||
      !exactCurrent(exact.rotation[0], exact) ||
      !exactCurrent(exact.translation[0], exact)) {
    return;
  }
  auto lineage = mixLineage(lineage_offset, exact.generation);
  lineage = mixLineage(lineage, exact.rotation[0].lineage);
  lineage = mixLineage(lineage, exact.translation[0].lineage);
  exact.camera_transform_lineage = lineage == 0U ? 1U : lineage;

  auto identity = mixLineage(lineage_offset, 0x43414d4552415641ULL);
  identity = mixLineage(identity, exact.rotation_value_identity);
  identity = mixLineage(identity, exact.translation_value_identity);
  exact.camera_value_identity = identity == 0U ? 1U : identity;
}

void publishProjectionState(GteExactState &exact) noexcept {
  exact.projection_value_identity = 0U;
  const auto revision = allocateRevision(exact);
  if (revision != 0U) {
    exact.projection_revision = revision;
  }
}

void captureRotationWrite(GteExactState &exact, std::uint8_t index,
                          std::uint32_t raw,
                          const GteExactWord *word) noexcept {
  if (index == 0U) {
    exact.rotation.fill({});
    exact.pending_rotation.fill({});
    exact.rotation_mask = 0U;
    exact.rotation_value_identity = 0U;
    exact.rotation_values_enhanced = false;
  }
  const auto expected = static_cast<std::uint16_t>(
      index == 0U ? 0U : (std::uint16_t{1} << (index * 2U)) - 1U);
  if (exact.rotation_mask != expected) {
    exact.rotation.fill({});
    exact.pending_rotation.fill({});
    exact.rotation_mask = 0U;
    exact.rotation_value_identity = 0U;
    exact.rotation_values_enhanced = false;
    return;
  }
  const auto first = static_cast<std::size_t>(index * 2U);
  exact.pending_rotation[first] =
      exactHalfOrRaw(word, raw, 0U, static_cast<double>(packedHalf(raw, false)),
                     0x100U + index * 2U, exact);
  exact.rotation_mask |= static_cast<std::uint16_t>(1U << first);
  if (first + 1U < exact.pending_rotation.size()) {
    exact.pending_rotation[first + 1U] = exactHalfOrRaw(
        word, raw, 1U, static_cast<double>(packedHalf(raw, true)),
        0x101U + index * 2U, exact);
    exact.rotation_mask |= static_cast<std::uint16_t>(1U << (first + 1U));
  }
  if (exact.rotation_mask == 0x1ffU) {
    exact.rotation_value_identity =
        publishTupleIdentity(exact.rotation, exact.pending_rotation,
                             exact.generation, 0x524f544154494f4eULL);
    exact.pending_rotation.fill({});
    exact.rotation_mask = 0U;
    exact.rotation_values_enhanced = tupleEnhanced(exact.rotation);
    publishCameraTuple(exact);
  }
}

void captureTranslationWrite(GteExactState &exact, std::uint8_t index,
                             std::uint32_t raw, double fallback,
                             const GteExactWord *word) noexcept {
  const auto component = static_cast<std::uint8_t>(index - 5U);
  if (component == 0U) {
    exact.translation.fill({});
    exact.pending_translation.fill({});
    exact.translation_mask = 0U;
    exact.translation_value_identity = 0U;
    exact.translation_values_enhanced = false;
  }
  const auto expected = static_cast<std::uint8_t>((1U << component) - 1U);
  if (exact.translation_mask != expected) {
    exact.translation.fill({});
    exact.pending_translation.fill({});
    exact.translation_mask = 0U;
    exact.translation_value_identity = 0U;
    exact.translation_values_enhanced = false;
    return;
  }
  exact.pending_translation[component] =
      exactScalarOrRaw(word, raw, fallback, 0x200U + component, exact);
  exact.translation_mask |= static_cast<std::uint8_t>(1U << component);
  if (exact.translation_mask == 0x7U) {
    exact.translation_value_identity =
        publishTupleIdentity(exact.translation, exact.pending_translation,
                             exact.generation, 0x5452414e534c4154ULL);
    exact.pending_translation.fill({});
    exact.translation_mask = 0U;
    exact.translation_values_enhanced = tupleEnhanced(exact.translation);
    publishCameraTuple(exact);
  }
}

void captureVectorWrite(GteExactState &exact, std::uint8_t index,
                        std::uint32_t raw, const GteState &state,
                        const GteExactWord *word) noexcept {
  const auto vector = static_cast<std::uint8_t>(index / 2U);
  auto &mask = exact.vector_masks[vector];
  if ((index & 1U) == 0U) {
    exact.vectors[vector].fill({});
    exact.pending_vectors[vector].fill({});
    mask = 0U;
    exact.vector_value_identities[vector] = 0U;
    exact.vector_values_enhanced[vector] = false;
    exact.pending_vectors[vector][0] = exactHalfOrRaw(
        word, raw, 0U,
        static_cast<double>(packedHalf(state.data[index], false)),
        0x300U + vector * 3U, exact);
    exact.pending_vectors[vector][1] = exactHalfOrRaw(
        word, raw, 1U, static_cast<double>(packedHalf(state.data[index], true)),
        0x301U + vector * 3U, exact);
    mask = 0x3U;
    return;
  }
  if (mask != 0x3U) {
    exact.vectors[vector].fill({});
    exact.pending_vectors[vector].fill({});
    mask = 0U;
    exact.vector_value_identities[vector] = 0U;
    exact.vector_values_enhanced[vector] = false;
    return;
  }
  exact.pending_vectors[vector][2] = exactScalarOrRaw(
      word, raw, static_cast<double>(signedHalf(state.data[index])),
      0x302U + vector * 3U, exact);
  mask = 0x7U;
  exact.vector_value_identities[vector] =
      publishTupleIdentity(exact.vectors[vector], exact.pending_vectors[vector],
                           exact.generation, 0x564543544f520000ULL + vector);
  exact.pending_vectors[vector].fill({});
  mask = 0U;
  exact.vector_values_enhanced[vector] = tupleEnhanced(exact.vectors[vector]);
}

void captureResultWrite(GteExactState &exact, std::uint8_t index,
                        std::uint32_t raw, double fallback,
                        const GteExactWord *word) noexcept {
  const auto component = static_cast<std::uint8_t>(index - 9U);
  if (component == 0U) {
    invalidateExactResult(&exact);
  }
  const auto expected = static_cast<std::uint8_t>((1U << component) - 1U);
  if (exact.result_mask != expected) {
    invalidateExactResult(&exact);
    return;
  }
  exact.pending_result[component] =
      exactScalarOrRaw(word, raw, fallback, 0x400U + component, exact);
  exact.result_mask |= static_cast<std::uint8_t>(1U << component);
  if (exact.result_mask == 0x7U) {
    publishTuple(exact.result, exact.pending_result, exact.generation,
                 0x524553554c540000ULL);
    exact.pending_result.fill({});
    exact.result_mask = 0U;
  }
}

bool withinMac44(double value) noexcept {
  constexpr auto minimum = -static_cast<double>(std::uint64_t{1} << 43U);
  constexpr auto maximum = static_cast<double>((std::uint64_t{1} << 43U) - 1U);
  return value >= minimum && value <= maximum;
}

std::uint64_t combineTransformLineage(const GteExactState &exact,
                                      std::uint8_t vector,
                                      std::uint32_t instruction) noexcept {
  const auto &input = vector == 3U ? exact.result : exact.vectors[vector];
  auto lineage = exact.camera_transform_lineage;
  if (lineage == 0U) {
    lineage = mixLineage(lineage_offset, exact.generation);
    lineage = mixLineage(lineage, exact.rotation[0].lineage);
    lineage = mixLineage(lineage, exact.translation[0].lineage);
  }
  lineage = mixLineage(lineage, input[0].lineage);
  lineage = mixLineage(lineage, instruction);
  return lineage == 0U ? 1U : lineage;
}

bool exactMatrixVector(const GteExactState &exact, std::uint8_t matrix,
                       std::uint8_t vector, std::uint8_t translation,
                       std::uint8_t shift, std::uint32_t instruction,
                       std::array<double, 3U> &result,
                       std::array<bool, 3U> &enhanced,
                       std::uint64_t &lineage) noexcept {
  if (matrix != 0U || vector > 3U || (translation != 0U && translation != 3U)) {
    return false;
  }
  const auto &input = vector == 3U ? exact.result : exact.vectors[vector];
  const auto divisor = static_cast<double>(std::uint64_t{1} << shift);
  for (std::uint8_t row{}; row < 3U; ++row) {
    auto accumulator = 0.0;
    auto row_enhanced = false;
    if (translation == 0U) {
      if (!exactCurrent(exact.translation[row], exact)) {
        return false;
      }
      accumulator = exact.translation[row].value * 4096.0;
      row_enhanced = exact.translation[row].enhanced;
      if (!withinMac44(accumulator)) {
        return false;
      }
    }
    for (std::uint8_t column{}; column < 3U; ++column) {
      const auto &matrix_component = exact.rotation[row * 3U + column];
      const auto &input_component = input[column];
      if (!exactCurrent(matrix_component, exact) ||
          !exactCurrent(input_component, exact)) {
        return false;
      }
      accumulator += matrix_component.value * input_component.value;
      if (!withinMac44(accumulator)) {
        return false;
      }
      row_enhanced =
          row_enhanced || matrix_component.enhanced || input_component.enhanced;
    }
    result[row] = accumulator / divisor;
    enhanced[row] = row_enhanced;
    if (!std::isfinite(result[row])) {
      return false;
    }
  }
  lineage = combineTransformLineage(exact, vector, instruction);
  return true;
}

bool exactPerspectiveView(const GteState &state, const GteExactState &exact,
                          std::uint8_t vector, std::uint32_t instruction,
                          std::array<double, 3U> &view, bool &enhanced,
                          std::uint8_t &enhanced_sources, bool &view_computed,
                          std::uint64_t &lineage) noexcept {
  if (vector >= exact.vectors.size() || exact.camera_revision == 0U ||
      exact.projection_revision == 0U) {
    return false;
  }
  enhanced = false;
  enhanced_sources = 0U;
  view_computed = false;
  auto requires_transform = false;
  const auto cached_tuple = exact.rotation_value_identity != 0U &&
                            exact.translation_value_identity != 0U &&
                            exact.vector_value_identities[vector] != 0U;
  if (cached_tuple) {
    enhanced_sources =
        (exact.rotation_values_enhanced ? GteProjectedVertex::enhanced_rotation
                                        : 0U) |
        (exact.translation_values_enhanced
             ? GteProjectedVertex::enhanced_translation
             : 0U) |
        (exact.vector_values_enhanced[vector]
             ? GteProjectedVertex::enhanced_vector
             : 0U);
    enhanced = enhanced_sources != 0U;
    lineage = combineTransformLineage(exact, vector, instruction);
    if (!enhanced)
      return true;
    for (std::uint8_t row{}; row < 3U; ++row) {
      auto accumulator = exact.translation[row].value * 4096.0;
      if (!withinMac44(accumulator))
        return false;
      for (std::uint8_t column{}; column < 3U; ++column) {
        accumulator += exact.rotation[row * 3U + column].value *
                       exact.vectors[vector][column].value;
        if (!withinMac44(accumulator))
          return false;
      }
      view[row] = accumulator / 4096.0;
    }
    view_computed = true;
    return true;
  }
  // Raw components still form an exact, coherent publication tuple.  The
  // integer GTE pass below already computes their view without precision
  // loss, so reserve the second 3x3 transform for genuinely enhanced (or
  // defensively mismatched) components.
  for (std::uint8_t row{}; row < 3U; ++row) {
    const auto &translation = exact.translation[row];
    if (!exactCurrent(translation, exact)) {
      return false;
    }
    enhanced = enhanced || translation.enhanced;
    if (translation.enhanced)
      enhanced_sources |= GteProjectedVertex::enhanced_translation;
    requires_transform =
        requires_transform || translation.enhanced ||
        translation.value !=
            static_cast<double>(translationElement(state, 0U, row));
    for (std::uint8_t column{}; column < 3U; ++column) {
      const auto &matrix_component = exact.rotation[row * 3U + column];
      const auto &input_component = exact.vectors[vector][column];
      if (!exactCurrent(matrix_component, exact) ||
          !exactCurrent(input_component, exact)) {
        return false;
      }
      enhanced =
          enhanced || matrix_component.enhanced || input_component.enhanced;
      if (matrix_component.enhanced)
        enhanced_sources |= GteProjectedVertex::enhanced_rotation;
      if (input_component.enhanced)
        enhanced_sources |= GteProjectedVertex::enhanced_vector;
      requires_transform =
          requires_transform || matrix_component.enhanced ||
          input_component.enhanced ||
          matrix_component.value !=
              static_cast<double>(matrixElement(state, 0U, row, column)) ||
          input_component.value !=
              static_cast<double>(vectorElement(state, vector, column));
    }
  }
  lineage = combineTransformLineage(exact, vector, instruction);
  if (!requires_transform) {
    return true;
  }

  for (std::uint8_t row{}; row < 3U; ++row) {
    auto accumulator = exact.translation[row].value * 4096.0;
    if (!withinMac44(accumulator)) {
      return false;
    }
    for (std::uint8_t column{}; column < 3U; ++column) {
      const auto &matrix_component = exact.rotation[row * 3U + column];
      const auto &input_component = exact.vectors[vector][column];
      accumulator += matrix_component.value * input_component.value;
      if (!withinMac44(accumulator)) {
        return false;
      }
    }
    view[row] = accumulator / 4096.0;
  }
  view_computed = true;
  return true;
}

std::uint64_t sourceVertexIdentity(const GteState &state, std::uint8_t vector,
                                   std::uint32_t instruction,
                                   GteExactState *exact) noexcept {
  // This is deliberately a value identity, not an execution-sequence ID.
  // RTPS and the corresponding RTPT lane must therefore agree.  Lane and
  // opcode are omitted while the transform mode bits are retained.
  const auto exact_coherent = exact != nullptr &&
                              vector < exact->vector_value_identities.size() &&
                              exact->rotation_value_identity != 0U &&
                              exact->translation_value_identity != 0U &&
                              exact->vector_value_identities[vector] != 0U;
  auto identity = lineage_offset;
  if (exact_coherent) {
    // Value identities are refreshed only when a complete tuple is
    // published. Cache the camera/projection portion because it is shared by
    // every RTPS/RTPT lane until a control-register write invalidates it.
    identity = exact->projection_value_identity;
    if (identity == 0U) {
      identity = mixLineage(lineage_offset, 0x534f555243455649ULL);
      identity = mixLineage(identity, exact->camera_value_identity);
      identity = mixLineage(identity, state.control[24]);
      identity = mixLineage(identity, state.control[25]);
      identity = mixLineage(identity, state.control[26] & 0xffffU);
      exact->projection_value_identity = identity == 0U ? 1U : identity;
    }
    identity = mixLineage(identity, exact->vector_value_identities[vector]);
  } else {
    // Memory-PGXP has no exact twin.  Hash the semantic integer witnesses,
    // not addresses or FIFO slots, so repeated projections remain stable.
    identity = mixLineage(lineage_offset, 0x534f555243455649ULL);
    for (std::uint8_t row{}; row < 3U; ++row) {
      for (std::uint8_t column{}; column < 3U; ++column) {
        identity = mixLineage(
            identity,
            static_cast<std::uint16_t>(matrixElement(state, 0U, row, column)));
      }
    }
    for (std::uint8_t component{}; component < 3U; ++component) {
      identity = mixLineage(
          identity,
          static_cast<std::uint32_t>(translationElement(state, 0U, component)));
      identity = mixLineage(identity, static_cast<std::uint16_t>(vectorElement(
                                          state, vector, component)));
    }
    identity = mixLineage(identity, state.control[24]);
    identity = mixLineage(identity, state.control[25]);
    identity = mixLineage(identity, state.control[26] & 0xffffU);
  }
  constexpr std::uint32_t transform_mode_mask = (1U << 19U) | (1U << 10U);
  identity = mixLineage(identity, instruction & transform_mode_mask);
  return identity == 0U ? 1U : identity;
}

void setExactResult(GteExactState &exact, const std::array<double, 3U> &result,
                    const std::array<bool, 3U> &enhanced,
                    std::uint64_t lineage) noexcept {
  exact.result_mask = 0U;
  for (std::size_t index{}; index < result.size(); ++index) {
    exact.result[index] = GteExactComponent{
        result[index], lineage, exact.generation, true, enhanced[index]};
  }
}

std::uint32_t clampColor(GteState &state, std::uint8_t component,
                         std::int32_t value) noexcept {
  if (value < 0) {
    state.control[31] |= flag_color_r_saturated >> component;
    return 0U;
  }
  if (value > 0xff) {
    state.control[31] |= flag_color_r_saturated >> component;
    return 0xffU;
  }
  return static_cast<std::uint32_t>(value);
}

void pushRgbFromMac(GteState &state) noexcept {
  const auto red = clampColor(state, 0U, signedWord(state.data[25]) >> 4U);
  const auto green = clampColor(state, 1U, signedWord(state.data[26]) >> 4U);
  const auto blue = clampColor(state, 2U, signedWord(state.data[27]) >> 4U);
  const auto code = state.data[6] & 0xff000000U;
  state.data[20] = state.data[21];
  state.data[21] = state.data[22];
  state.data[22] = red | (green << 8U) | (blue << 16U) | code;
}

void interpolateColor(GteState &state,
                      const std::array<std::int64_t, 3U> &input_mac,
                      std::uint8_t shift, bool limit_mode) noexcept {
  for (std::uint8_t component = 0U; component < input_mac.size(); ++component) {
    setMacAndIr(
        state, static_cast<std::uint8_t>(component + 1U),
        static_cast<std::int64_t>(translationElement(state, 2U, component)) *
                4096 -
            input_mac[component],
        shift, false);
  }

  const auto interpolation =
      static_cast<std::int64_t>(signedHalf(state.data[8]));
  for (std::uint8_t component = 0U; component < input_mac.size(); ++component) {
    setMacAndIr(
        state, static_cast<std::uint8_t>(component + 1U),
        static_cast<std::int64_t>(signedHalf(state.data[9U + component])) *
                interpolation +
            input_mac[component],
        shift, limit_mode);
  }
}

void depthCueColor(GteState &state, std::uint32_t color, std::uint8_t shift,
                   bool limit_mode) noexcept {
  std::array<std::int64_t, 3U> input_mac{};
  for (std::uint8_t component = 0U; component < input_mac.size(); ++component) {
    input_mac[component] =
        static_cast<std::int64_t>((color >> (component * 8U)) & 0xffU) << 16U;
  }
  interpolateColor(state, input_mac, shift, limit_mode);
  pushRgbFromMac(state);
}

void setOtz(GteState &state, std::int32_t value) noexcept {
  if (value < 0) {
    state.control[31] |= flag_sz_otz_saturated;
    value = 0;
  } else if (value > 0xffff) {
    state.control[31] |= flag_sz_otz_saturated;
    value = 0xffff;
  }
  state.data[7] = static_cast<std::uint32_t>(value);
}

void multiplyMatrixVector(GteState &state, std::uint8_t matrix,
                          std::uint8_t translation,
                          const std::array<std::int16_t, 3U> &vector,
                          std::uint8_t shift, bool limit_mode) noexcept {
  for (std::uint8_t row = 0U; row < 3U; ++row) {
    const auto component = static_cast<std::uint8_t>(row + 1U);
    const auto x_product =
        static_cast<std::int64_t>(matrixElement(state, matrix, row, 0U)) *
        vector[0];
    const auto y_product =
        static_cast<std::int64_t>(matrixElement(state, matrix, row, 1U)) *
        vector[1];
    const auto z_product =
        static_cast<std::int64_t>(matrixElement(state, matrix, row, 2U)) *
        vector[2];
    auto value = signExtendMac(
        state, component,
        static_cast<std::int64_t>(translationElement(state, translation, row)) *
                4096 +
            x_product);
    value = signExtendMac(state, component, value + y_product);
    value = signExtendMac(state, component, value + z_product);
    setMacAndIr(state, component, value, shift, limit_mode);
  }
}

void normalColor(GteState &state, std::uint8_t vector, std::uint8_t shift,
                 bool limit_mode) noexcept {
  const std::array normal{
      vectorElement(state, vector, 0U),
      vectorElement(state, vector, 1U),
      vectorElement(state, vector, 2U),
  };
  multiplyMatrixVector(state, 1U, 3U, normal, shift, limit_mode);
  const std::array lit_normal{
      static_cast<std::int16_t>(state.data[9]),
      static_cast<std::int16_t>(state.data[10]),
      static_cast<std::int16_t>(state.data[11]),
  };
  multiplyMatrixVector(state, 2U, 1U, lit_normal, shift, limit_mode);

  for (std::uint8_t component = 0U; component < 3U; ++component) {
    const auto color =
        static_cast<std::int64_t>((state.data[6] >> (component * 8U)) & 0xffU);
    const auto value = color * signedHalf(state.data[9U + component]) * 16;
    setMacAndIr(state, static_cast<std::uint8_t>(component + 1U), value, shift,
                limit_mode);
  }
  pushRgbFromMac(state);
}

void normalColorDepthCue(GteState &state, std::uint8_t vector,
                         std::uint8_t shift, bool limit_mode) noexcept {
  const std::array normal{
      vectorElement(state, vector, 0U),
      vectorElement(state, vector, 1U),
      vectorElement(state, vector, 2U),
  };
  multiplyMatrixVector(state, 1U, 3U, normal, shift, limit_mode);
  const std::array lit_normal{
      static_cast<std::int16_t>(state.data[9]),
      static_cast<std::int16_t>(state.data[10]),
      static_cast<std::int16_t>(state.data[11]),
  };
  multiplyMatrixVector(state, 2U, 1U, lit_normal, shift, limit_mode);

  std::array<std::int64_t, 3U> color_mac{};
  for (std::uint8_t component = 0U; component < 3U; ++component) {
    const auto color =
        static_cast<std::int64_t>((state.data[6] >> (component * 8U)) & 0xffU);
    color_mac[component] =
        color * static_cast<std::int16_t>(state.data[9U + component]) * 16;
    setMacAndIr(
        state, static_cast<std::uint8_t>(component + 1U),
        static_cast<std::int64_t>(translationElement(state, 2U, component)) *
                4096 -
            color_mac[component],
        shift, false);
  }
  const auto depth_cue =
      static_cast<std::int64_t>(static_cast<std::int16_t>(state.data[8]));
  for (std::uint8_t component = 0U; component < 3U; ++component) {
    setMacAndIr(state, static_cast<std::uint8_t>(component + 1U),
                static_cast<std::int64_t>(
                    static_cast<std::int16_t>(state.data[9U + component])) *
                        depth_cue +
                    color_mac[component],
                shift, limit_mode);
  }
  pushRgbFromMac(state);
}

void executeNormalColorDepthCueSingle(GteState &state,
                                      std::uint32_t instruction) noexcept {
  state.control[31] = 0U;
  const auto shift =
      static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
  const auto limit_mode = (instruction & (1U << 10U)) != 0U;
  normalColorDepthCue(state, 0U, shift, limit_mode);
  updateErrorFlag(state);
}

void executeNormalColorDepthCueTriple(GteState &state,
                                      std::uint32_t instruction) noexcept {
  state.control[31] = 0U;
  const auto shift =
      static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
  const auto limit_mode = (instruction & (1U << 10U)) != 0U;
  for (std::uint8_t vector = 0U; vector < 3U; ++vector) {
    normalColorDepthCue(state, vector, shift, limit_mode);
  }
  updateErrorFlag(state);
}
void executeNormalColorTriple(GteState &state,
                              std::uint32_t instruction) noexcept {
  state.control[31] = 0U;
  const auto shift =
      static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
  const auto limit_mode = (instruction & (1U << 10U)) != 0U;
  for (std::uint8_t vector = 0U; vector < 3U; ++vector) {
    normalColor(state, vector, shift, limit_mode);
  }
  updateErrorFlag(state);
}

void checkMac0Overflow(GteState &state, std::int64_t value) noexcept {
  if (value < std::numeric_limits<std::int32_t>::min()) {
    state.control[31] |= flag_mac0_underflow;
  } else if (value > std::numeric_limits<std::int32_t>::max()) {
    state.control[31] |= flag_mac0_overflow;
  }
}

bool pushScreenDepth(GteState &state, std::int32_t value) noexcept {
  auto saturated = false;
  if (value < 0) {
    state.control[31] |= flag_sz_otz_saturated;
    saturated = true;
    value = 0;
  } else if (value > 0xffff) {
    state.control[31] |= flag_sz_otz_saturated;
    saturated = true;
    value = 0xffff;
  }
  state.data[16] = state.data[17];
  state.data[17] = state.data[18];
  state.data[18] = state.data[19];
  state.data[19] = static_cast<std::uint32_t>(value);
  return saturated;
}

bool pushScreenPosition(GteState &state, std::int32_t x,
                        std::int32_t y) noexcept {
  auto saturated = false;
  if (x < -1024) {
    state.control[31] |= flag_sx_saturated;
    saturated = true;
    x = -1024;
  } else if (x > 1023) {
    state.control[31] |= flag_sx_saturated;
    saturated = true;
    x = 1023;
  }
  if (y < -1024) {
    state.control[31] |= flag_sy_saturated;
    saturated = true;
    y = -1024;
  } else if (y > 1023) {
    state.control[31] |= flag_sy_saturated;
    saturated = true;
    y = 1023;
  }
  state.data[12] = state.data[13];
  state.data[13] = state.data[14];
  state.data[14] =
      static_cast<std::uint16_t>(x) |
      (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y)) << 16U);
  return saturated;
}

void pushProjectedVertex(
    GteState &state, const std::array<std::int64_t, 3U> &coordinate,
    bool ir_saturated, bool depth_saturated, bool divide_overflow,
    bool screen_saturated, const std::array<double, 3U> *exact_view = nullptr,
    bool preserve_projection_precision = false,
    bool fractional_transform = false, std::uint8_t enhanced_sources = 0U,
    std::uint64_t source_vertex_id = 0U, std::uint64_t mesh_vertex_id = 0U,
    std::uint64_t transform_lineage = 0U,
    std::uint64_t projection_epoch = 0U) noexcept {
  state.projected[0] = state.projected[1];
  state.projected[1] = state.projected[2];
  auto &projected = state.projected[2];
  projected = {};
  projected.packed_sxy = state.data[14];
  projected.ir_saturated = ir_saturated;
  projected.depth_saturated = depth_saturated;
  projected.divide_overflow = divide_overflow;
  projected.screen_saturated = screen_saturated;

  const auto screen_h = static_cast<double>(state.control[26] & 0xffffU);
  const auto hardware_depth = static_cast<double>(state.data[19] & 0xffffU);
  // Default Memory-PGXP deliberately starts from the hardware-visible IR/SZ
  // registers.  Reusing the unshifted MAC values here silently enabled the
  // more aggressive "preserve projection precision" mode and could disagree
  // with clipping/saturation performed by the real GTE.
  constexpr auto q12_scale = 1.0 / 4096.0;
  const auto mac_view_x = static_cast<double>(coordinate[0]) * q12_scale;
  const auto mac_view_y = static_cast<double>(coordinate[1]) * q12_scale;
  const auto mac_view_z = static_cast<double>(coordinate[2]) * q12_scale;
  const auto view_x = exact_view != nullptr ? (*exact_view)[0]
                      : preserve_projection_precision
                          ? mac_view_x
                          : static_cast<double>(signedHalf(state.data[9]));
  const auto view_y = exact_view != nullptr ? (*exact_view)[1]
                      : preserve_projection_precision
                          ? mac_view_y
                          : static_cast<double>(signedHalf(state.data[10]));
  const auto view_z = exact_view != nullptr           ? (*exact_view)[2]
                      : preserve_projection_precision ? mac_view_z
                                                      : hardware_depth;
  if (exact_view == nullptr && view_z <= 0.0) {
    return;
  }
  const auto offset_x =
      static_cast<double>(signedWord(state.control[24])) / 65536.0;
  const auto offset_y =
      static_cast<double>(signedWord(state.control[25])) / 65536.0;
  auto screen_x = static_cast<double>(packedHalf(projected.packed_sxy, false));
  auto screen_y = static_cast<double>(packedHalf(projected.packed_sxy, true));
  if (view_z != 0.0) {
    const auto candidate_x = offset_x + view_x * screen_h / view_z;
    const auto candidate_y = offset_y + view_y * screen_h / view_z;
    if (std::isfinite(candidate_x) && std::isfinite(candidate_y)) {
      constexpr auto minimum_screen_coordinate = -1024.0;
      constexpr auto maximum_screen_coordinate = 1023.0;
      screen_x = std::clamp(candidate_x, minimum_screen_coordinate,
                            maximum_screen_coordinate);
      screen_y = std::clamp(candidate_y, minimum_screen_coordinate,
                            maximum_screen_coordinate);
    } else if (exact_view == nullptr) {
      return;
    }
  }

  projected.view_x = static_cast<float>(view_x);
  projected.view_y = static_cast<float>(view_y);
  projected.view_z = static_cast<float>(view_z);
  // Match the PS1 reciprocal input used by DuckStation's default PGXP mode.
  // Exact transforms refine XY, but texture W remains the hardware-visible SZ.
  projected.projective_depth =
      static_cast<float>(std::max(screen_h * 0.5, hardware_depth));
  projected.screen_x = static_cast<float>(screen_x);
  projected.screen_y = static_cast<float>(screen_y);
  projected.screen_h = static_cast<float>(screen_h);
  projected.screen_offset_x = static_cast<float>(offset_x);
  projected.screen_offset_y = static_cast<float>(offset_y);
  projected.valid = true;
  projected.exact_transform = exact_view != nullptr &&
                              transform_lineage != 0U && projection_epoch != 0U;
  projected.fractional_transform = fractional_transform;
  projected.enhanced_sources = enhanced_sources;
  projected.source_vertex_id = source_vertex_id;
  projected.mesh_vertex_id = mesh_vertex_id;
  projected.transform_lineage = transform_lineage;
  projected.projection_epoch = projection_epoch;
}

std::uint32_t dividePerspective(GteState &state, std::uint32_t numerator,
                                std::uint32_t denominator) noexcept {
  if (denominator * 2U <= numerator) {
    state.control[31] |= flag_divide_overflow;
    return 0x1ffffU;
  }

  const auto shift = static_cast<std::uint32_t>(
      std::countl_zero(static_cast<std::uint16_t>(denominator)));
  numerator <<= shift;
  denominator <<= shift;
  const auto index = ((denominator & 0x7fffU) + 0x40U) >> 7U;
  const auto table_value = std::clamp(
      ((0x40000 / static_cast<int>(index + 0x100U) + 1) / 2) - 0x101, 0, 0xff);
  const auto estimate = 0x101 + table_value;
  const auto delta =
      (static_cast<std::int32_t>(denominator) * -estimate + 0x80) >> 8U;
  const auto reciprocal = (estimate * (0x20000 + delta) + 0x80) >> 8U;
  const auto result =
      static_cast<std::uint32_t>((static_cast<std::uint64_t>(numerator) *
                                      static_cast<std::uint32_t>(reciprocal) +
                                  0x8000U) >>
                                 16U);
  return std::min(0x1ffffU, result);
}

void executeSquare(GteState &state, std::uint32_t instruction) noexcept {
  state.control[31] = 0U;
  const auto shift =
      static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
  const auto limit_mode = (instruction & (1U << 10U)) != 0U;
  for (std::uint8_t component = 1U; component <= 3U; ++component) {
    const auto value =
        static_cast<std::int64_t>(signedHalf(state.data[8U + component]));
    setMacAndIr(state, component, value * value, shift, limit_mode);
  }
  updateErrorFlag(state);
}

void executeGeneralPurposeMultiply(GteState &state,
                                   std::uint32_t instruction) noexcept {
  state.control[31] = 0U;
  const auto shift =
      static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
  const auto limit_mode = (instruction & (1U << 10U)) != 0U;
  const auto interpolation =
      static_cast<std::int64_t>(signedHalf(state.data[8]));
  for (std::uint8_t component = 1U; component <= 3U; ++component) {
    const auto value =
        static_cast<std::int64_t>(signedHalf(state.data[8U + component]));
    setMacAndIr(state, component, interpolation * value, shift, limit_mode);
  }
  pushRgbFromMac(state);
  updateErrorFlag(state);
}

void executeDepthCueColorSingle(GteState &state,
                                std::uint32_t instruction) noexcept {
  state.control[31] = 0U;
  const auto shift =
      static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
  const auto limit_mode = (instruction & (1U << 10U)) != 0U;
  depthCueColor(state, state.data[6], shift, limit_mode);
  updateErrorFlag(state);
}

void executeDepthCueColorTriple(GteState &state,
                                std::uint32_t instruction) noexcept {
  state.control[31] = 0U;
  const auto shift =
      static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
  const auto limit_mode = (instruction & (1U << 10U)) != 0U;
  for (std::uint8_t iteration = 0U; iteration < 3U; ++iteration) {
    depthCueColor(state, state.data[20], shift, limit_mode);
  }
  updateErrorFlag(state);
}

void executeDepthCueLight(GteState &state, std::uint32_t instruction) noexcept {
  state.control[31] = 0U;
  const auto shift =
      static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
  const auto limit_mode = (instruction & (1U << 10U)) != 0U;
  std::array<std::int64_t, 3U> input_mac{};
  for (std::uint8_t component = 0U; component < input_mac.size(); ++component) {
    const auto color =
        static_cast<std::int64_t>((state.data[6] >> (component * 8U)) & 0xffU);
    input_mac[component] =
        color *
        static_cast<std::int64_t>(signedHalf(state.data[9U + component])) * 16;
  }
  interpolateColor(state, input_mac, shift, limit_mode);
  pushRgbFromMac(state);
  updateErrorFlag(state);
}

void executeInterpolate(GteState &state, std::uint32_t instruction) noexcept {
  state.control[31] = 0U;
  const auto shift =
      static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
  const auto limit_mode = (instruction & (1U << 10U)) != 0U;
  std::array<std::int64_t, 3U> input_mac{};
  for (std::uint8_t component = 0U; component < input_mac.size(); ++component) {
    input_mac[component] =
        static_cast<std::int64_t>(signedHalf(state.data[9U + component])) *
        4096;
  }
  interpolateColor(state, input_mac, shift, limit_mode);
  pushRgbFromMac(state);
  updateErrorFlag(state);
}

void executeAverageDepth(GteState &state, std::uint8_t first_depth,
                         std::uint8_t depth_count,
                         std::uint8_t scale_control) noexcept {
  state.control[31] = 0U;
  auto depth_sum = std::uint32_t{};
  for (std::uint8_t index = 0U; index < depth_count; ++index) {
    depth_sum += state.data[first_depth + index] & 0xffffU;
  }
  const auto result =
      static_cast<std::int64_t>(signedHalf(state.control[scale_control])) *
      static_cast<std::int32_t>(depth_sum);
  checkMac0Overflow(state, result);
  state.data[24] = std::bit_cast<std::uint32_t>(lowSignedWord(result));
  setOtz(state, lowSignedWord(result >> 12U));
  updateErrorFlag(state);
}

void executeMatrixVectorMultiply(GteState &state, std::uint32_t instruction,
                                 GteExactState *exact) noexcept {
  state.control[31] = 0U;
  const auto shift =
      static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
  const auto matrix = static_cast<std::uint8_t>((instruction >> 17U) & 3U);
  const auto vector = static_cast<std::uint8_t>((instruction >> 15U) & 3U);
  const auto translation = static_cast<std::uint8_t>((instruction >> 13U) & 3U);
  const auto limit_mode = (instruction & (1U << 10U)) != 0U;
  const std::array vector_value{
      vectorElement(state, vector, 0U),
      vectorElement(state, vector, 1U),
      vectorElement(state, vector, 2U),
  };
  std::array<double, 3U> exact_result{};
  std::array<bool, 3U> exact_enhanced{};
  std::uint64_t exact_lineage{};
  const auto has_exact_result =
      exact != nullptr && exact->transform_twin_enabled &&
      exactMatrixVector(*exact, matrix, vector, translation, shift, instruction,
                        exact_result, exact_enhanced, exact_lineage);

  for (std::uint8_t row = 0U; row < 3U; ++row) {
    const auto component = static_cast<std::uint8_t>(row + 1U);
    const auto x_product =
        static_cast<std::int64_t>(matrixElement(state, matrix, row, 0U)) *
        vector_value[0];
    const auto y_product =
        static_cast<std::int64_t>(matrixElement(state, matrix, row, 1U)) *
        vector_value[1];
    const auto z_product =
        static_cast<std::int64_t>(matrixElement(state, matrix, row, 2U)) *
        vector_value[2];

    if (translation == 2U) {
      const auto first =
          signExtendMac(state, component,
                        static_cast<std::int64_t>(
                            translationElement(state, translation, row)) *
                                4096 +
                            x_product);
      static_cast<void>(
          clampIr(state, component, lowSignedWord(first >> shift), false));
      const auto value = signExtendMac(state, component, y_product) + z_product;
      setMacAndIr(state, component, signExtendMac(state, component, value),
                  shift, limit_mode);
      continue;
    }

    auto value = signExtendMac(state, component,
                               (static_cast<std::int64_t>(translationElement(
                                    state, translation, row)) *
                                4096) +
                                   x_product);
    value = signExtendMac(state, component, value + y_product);
    value = signExtendMac(state, component, value + z_product);
    setMacAndIr(state, component, value, shift, limit_mode);
  }
  constexpr std::uint32_t mac123_overflow_mask = 0x7e000000U;
  if (has_exact_result && (state.control[31] & mac123_overflow_mask) == 0U) {
    for (std::size_t row{}; row < exact_result.size(); ++row) {
      exact_enhanced[row] =
          exact_enhanced[row] ||
          std::abs(exact_result[row] -
                   static_cast<double>(signedWord(state.data[25U + row]))) >
              1.0e-9;
    }
    setExactResult(*exact, exact_result, exact_enhanced, exact_lineage);
  } else {
    invalidateExactResult(exact);
  }
  updateErrorFlag(state);
}

void executeOuterProduct(GteState &state, std::uint32_t instruction) noexcept {
  state.control[31] = 0U;
  const auto shift =
      static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
  const auto limit_mode = (instruction & (1U << 10U)) != 0U;
  const auto diagonal1 =
      static_cast<std::int64_t>(packedHalf(state.control[0], false));
  const auto diagonal2 =
      static_cast<std::int64_t>(packedHalf(state.control[2], false));
  const auto diagonal3 =
      static_cast<std::int64_t>(packedHalf(state.control[4], false));
  const auto ir1 = static_cast<std::int64_t>(signedHalf(state.data[9]));
  const auto ir2 = static_cast<std::int64_t>(signedHalf(state.data[10]));
  const auto ir3 = static_cast<std::int64_t>(signedHalf(state.data[11]));
  setMacAndIr(state, 1U, ir3 * diagonal2 - ir2 * diagonal3, shift, limit_mode);
  setMacAndIr(state, 2U, ir1 * diagonal3 - ir3 * diagonal1, shift, limit_mode);
  setMacAndIr(state, 3U, ir2 * diagonal1 - ir1 * diagonal2, shift, limit_mode);
  updateErrorFlag(state);
}

void perspectiveTransformVector(GteState &state, std::uint8_t vector_index,
                                std::uint8_t shift, bool limit_mode, bool last,
                                std::uint32_t instruction, GteExactState *exact,
                                bool capture_projection,
                                bool preserve_projection_precision) noexcept {
  std::array<double, 3U> exact_view{};
  auto fractional_transform = false;
  std::uint8_t enhanced_sources{};
  auto exact_view_computed = false;
  std::uint64_t exact_lineage{};
  const auto exact_twin_enabled =
      exact != nullptr && exact->transform_twin_enabled;
  auto has_exact_view =
      exact_twin_enabled &&
      exactPerspectiveView(state, *exact, vector_index, instruction, exact_view,
                           fractional_transform, enhanced_sources,
                           exact_view_computed, exact_lineage);
  const std::array vector{
      vectorElement(state, vector_index, 0U),
      vectorElement(state, vector_index, 1U),
      vectorElement(state, vector_index, 2U),
  };
  std::array<std::int64_t, 3> coordinate{};
  for (std::uint8_t row = 0U; row < 3U; ++row) {
    const auto component = static_cast<std::uint8_t>(row + 1U);
    auto value = signExtendMac(
        state, component,
        static_cast<std::int64_t>(translationElement(state, 0U, row)) * 4096 +
            static_cast<std::int64_t>(matrixElement(state, 0U, row, 0U)) *
                vector[0]);
    value = signExtendMac(
        state, component,
        value + static_cast<std::int64_t>(matrixElement(state, 0U, row, 1U)) *
                    vector[1]);
    coordinate[row] = signExtendMac(
        state, component,
        value + static_cast<std::int64_t>(matrixElement(state, 0U, row, 2U)) *
                    vector[2]);
    state.data[25U + row] =
        std::bit_cast<std::uint32_t>(lowSignedWord(coordinate[row] >> shift));
  }
  state.data[9] = std::bit_cast<std::uint32_t>(
      clampIr(state, 1U, signedWord(state.data[25]), limit_mode));
  state.data[10] = std::bit_cast<std::uint32_t>(
      clampIr(state, 2U, signedWord(state.data[26]), limit_mode));
  static_cast<void>(
      clampIr(state, 3U, lowSignedWord(coordinate[2] >> 12U), false));
  state.data[11] = std::bit_cast<std::uint32_t>(
      std::clamp(signedWord(state.data[27]), limit_mode ? 0 : -0x8000, 0x7fff));

  const auto ir_saturated =
      signedHalf(state.data[9]) != signedWord(state.data[25]) ||
      signedHalf(state.data[10]) != signedWord(state.data[26]);
  const auto depth_saturated =
      pushScreenDepth(state, lowSignedWord(coordinate[2] >> 12U));
  const auto projection_h = state.control[26] & 0xffffU;
  const auto projection_depth = state.data[19] & 0xffffU;
  const auto divide_overflow = projection_depth * 2U <= projection_h;
  const auto quotient =
      dividePerspective(state, projection_h, projection_depth);
  const auto screen_x =
      static_cast<std::int64_t>(quotient) * signedHalf(state.data[9]) +
      signedWord(state.control[24]);
  const auto screen_y =
      static_cast<std::int64_t>(quotient) * signedHalf(state.data[10]) +
      signedWord(state.control[25]);
  checkMac0Overflow(state, screen_x);
  checkMac0Overflow(state, screen_y);
  const auto screen_saturated = pushScreenPosition(
      state, lowSignedWord(screen_x >> 16U), lowSignedWord(screen_y >> 16U));
  // If an enhanced publication is incomplete, MAC44 still contains the
  // coherent current transform before IR/SZ truncation and saturation.
  // Publish that unclamped tuple instead of falling back to Q8 registers.
  if (exact_twin_enabled && (!has_exact_view || !exact_view_computed)) {
    for (std::size_t row{}; row < exact_view.size(); ++row) {
      exact_view[row] = static_cast<double>(coordinate[row]) / 4096.0;
    }
  }
  const auto has_unclamped_view = has_exact_view || exact_twin_enabled;
  if (exact != nullptr || capture_projection) {
    const auto mesh_vertex_id =
        exact != nullptr
            ? sourceVertexIdentity(state, vector_index, instruction, exact)
            : 0U;
    const auto source_vertex_id =
        exact != nullptr && exact->source_identity_enabled ? mesh_vertex_id
                                                           : 0U;
    pushProjectedVertex(state, coordinate, ir_saturated, depth_saturated,
                        divide_overflow, screen_saturated,
                        has_unclamped_view ? &exact_view : nullptr,
                        preserve_projection_precision, fractional_transform,
                        enhanced_sources, source_vertex_id, mesh_vertex_id,
                        has_exact_view ? exact->camera_revision : 0U,
                        has_exact_view ? exact->projection_revision : 0U);
  }

  if (has_exact_view) {
    std::array<double, 3U> exact_result{};
    std::array<bool, 3U> enhanced{};
    const auto result_scale =
        4096.0 / static_cast<double>(std::uint64_t{1} << shift);
    for (std::size_t row{}; row < exact_result.size(); ++row) {
      exact_result[row] = exact_view[row] * result_scale;
      enhanced[row] =
          fractional_transform ||
          std::abs(exact_result[row] -
                   static_cast<double>(signedWord(state.data[25U + row]))) >
              1.0e-9;
    }
    setExactResult(*exact, exact_result, enhanced, exact_lineage);
  } else {
    invalidateExactResult(exact);
  }

  if (last) {
    const auto depth_cue = static_cast<std::int64_t>(quotient) *
                               static_cast<std::int16_t>(state.control[27]) +
                           signedWord(state.control[28]);
    checkMac0Overflow(state, depth_cue);
    state.data[24] = std::bit_cast<std::uint32_t>(lowSignedWord(depth_cue));
    auto ir0 = depth_cue >> 12U;
    if (ir0 < 0) {
      state.control[31] |= flag_ir0_saturated;
      ir0 = 0;
    } else if (ir0 > 0x1000) {
      state.control[31] |= flag_ir0_saturated;
      ir0 = 0x1000;
    }
    state.data[8] = static_cast<std::uint32_t>(ir0);
  }
}

void executePerspectiveTransform(GteState &state, std::uint32_t instruction,
                                 GteExactState *exact, bool capture_projection,
                                 bool preserve_projection_precision) noexcept {
  state.control[31] = 0U;
  const auto shift =
      static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
  const auto limit_mode = (instruction & (1U << 10U)) != 0U;
  perspectiveTransformVector(state, 0U, shift, limit_mode, true, instruction,
                             exact, capture_projection,
                             preserve_projection_precision);
  updateErrorFlag(state);
}

void executeTriplePerspectiveTransform(
    GteState &state, std::uint32_t instruction, GteExactState *exact,
    bool capture_projection, bool preserve_projection_precision) noexcept {
  state.control[31] = 0U;
  const auto shift =
      static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
  const auto limit_mode = (instruction & (1U << 10U)) != 0U;
  perspectiveTransformVector(state, 0U, shift, limit_mode, false, instruction,
                             exact, capture_projection,
                             preserve_projection_precision);
  perspectiveTransformVector(state, 1U, shift, limit_mode, false, instruction,
                             exact, capture_projection,
                             preserve_projection_precision);
  perspectiveTransformVector(state, 2U, shift, limit_mode, true, instruction,
                             exact, capture_projection,
                             preserve_projection_precision);
  updateErrorFlag(state);
}

void executeNormalClip(GteState &state, const GteExactState *) noexcept {
  state.control[31] = 0U;
  state.precise_nclip_area = 0.0;
  state.precise_nclip_valid = false;
  const auto precise = [&state] {
    for (std::size_t index{}; index < state.projected.size(); ++index) {
      const auto &vertex = state.projected[index];
      if (!vertex.valid || !vertex.pgxpEligible() ||
          vertex.packed_sxy != state.data[12U + index] ||
          !std::isfinite(vertex.screen_x) || !std::isfinite(vertex.screen_y)) {
        return false;
      }
    }
    return true;
  }();
  if (precise) {
    const auto &v0 = state.projected[0U];
    const auto &v1 = state.projected[1U];
    const auto &v2 = state.projected[2U];
    auto area = static_cast<double>(v0.screen_x) * v1.screen_y +
                static_cast<double>(v1.screen_x) * v2.screen_y +
                static_cast<double>(v2.screen_x) * v0.screen_y -
                static_cast<double>(v0.screen_x) * v2.screen_y -
                static_cast<double>(v1.screen_x) * v0.screen_y -
                static_cast<double>(v2.screen_x) * v1.screen_y;
    // A non-positive signed W is only published by the exact-transform path.
    // Legacy fractional PGXP vertices can carry a zero-initialized view tuple,
    // which must continue to use their precise post-divide screen area.
    const auto has_exact_transform =
        v0.exact_transform || v1.exact_transform || v2.exact_transform;
    const auto eye_crossing =
        has_exact_transform &&
        (v0.view_z <= 0.0F || v1.view_z <= 0.0F || v2.view_z <= 0.0F);
    const auto homogeneous_context = [&] {
      if (!eye_crossing) {
        return true;
      }
      constexpr auto camera_tolerance = 1.0F / 65536.0F;
      constexpr auto reprojection_tolerance = 0.25F;
      const std::array vertices{&v0, &v1, &v2};
      for (const auto *vertex : vertices) {
        if (!std::isfinite(vertex->view_x) || !std::isfinite(vertex->view_y) ||
            !std::isfinite(vertex->view_z) ||
            !std::isfinite(vertex->screen_h) ||
            !std::isfinite(vertex->screen_offset_x) ||
            !std::isfinite(vertex->screen_offset_y) ||
            vertex->screen_h <= 0.0F ||
            (vertex->view_z <= 0.0F && !vertex->exact_transform)) {
          return false;
        }
        if (std::abs(vertex->screen_h - v0.screen_h) > camera_tolerance ||
            std::abs(vertex->screen_offset_x - v0.screen_offset_x) >
                camera_tolerance ||
            std::abs(vertex->screen_offset_y - v0.screen_offset_y) >
                camera_tolerance) {
          return false;
        }
        if (vertex->view_z > 0.0F) {
          const auto projected_x =
              vertex->screen_offset_x +
              vertex->view_x * vertex->screen_h / vertex->view_z;
          const auto projected_y =
              vertex->screen_offset_y +
              vertex->view_y * vertex->screen_h / vertex->view_z;
          if (!std::isfinite(projected_x) || !std::isfinite(projected_y) ||
              std::abs(projected_x - vertex->screen_x) >
                  reprojection_tolerance ||
              std::abs(projected_y - vertex->screen_y) >
                  reprojection_tolerance) {
            return false;
          }
        }
      }
      return true;
    }();
    if (eye_crossing && homogeneous_context) {
      // Post-divide screen area changes sign whenever an odd number of W
      // values is negative. GPU clipping preserves the original homogeneous
      // winding, so use det(view.xyz) and an absolute depth denominator.
      const auto homogeneous_area =
          static_cast<double>(v0.view_x) *
              (static_cast<double>(v1.view_y) * v2.view_z -
               static_cast<double>(v1.view_z) * v2.view_y) +
          static_cast<double>(v0.view_y) *
              (static_cast<double>(v1.view_z) * v2.view_x -
               static_cast<double>(v1.view_x) * v2.view_z) +
          static_cast<double>(v0.view_z) *
              (static_cast<double>(v1.view_x) * v2.view_y -
               static_cast<double>(v1.view_y) * v2.view_x);
      const auto depth_product =
          std::abs(static_cast<double>(v0.view_z) * v1.view_z * v2.view_z);
      const auto safe_depth =
          std::max(depth_product, std::numeric_limits<double>::min());
      area = homogeneous_area * static_cast<double>(v0.screen_h) *
             static_cast<double>(v0.screen_h) / safe_depth;
    }
    if ((!eye_crossing || homogeneous_context) && std::isfinite(area)) {
      state.precise_nclip_area = area;
      state.precise_nclip_valid = true;
    }
  }
  const auto sx0 = static_cast<std::int64_t>(packedHalf(state.data[12], false));
  const auto sy0 = static_cast<std::int64_t>(packedHalf(state.data[12], true));
  const auto sx1 = static_cast<std::int64_t>(packedHalf(state.data[13], false));
  const auto sy1 = static_cast<std::int64_t>(packedHalf(state.data[13], true));
  const auto sx2 = static_cast<std::int64_t>(packedHalf(state.data[14], false));
  const auto sy2 = static_cast<std::int64_t>(packedHalf(state.data[14], true));
  const auto area =
      sx0 * sy1 + sx1 * sy2 + sx2 * sy0 - sx0 * sy2 - sx1 * sy0 - sx2 * sy1;
  checkMac0Overflow(state, area);
  state.data[24] = std::bit_cast<std::uint32_t>(lowSignedWord(area));
  updateErrorFlag(state);
}

} // namespace

std::uint32_t GteRuntime::readData(const GteState &state,
                                   std::uint8_t index) noexcept {
  if (index == 15U) {
    return state.data[14];
  }
  if (index == 28U || index == 29U) {
    const auto component = [&state](std::uint8_t ir) {
      return static_cast<std::uint32_t>(
          std::clamp(signedHalf(state.data[ir]) / 0x80, 0, 0x1f));
    };
    return component(9U) | (component(10U) << 5U) | (component(11U) << 10U);
  }
  return state.data[index & 31U];
}

std::uint32_t GteRuntime::readControl(const GteState &state,
                                      std::uint8_t index) noexcept {
  return state.control[index & 31U];
}

void GteRuntime::writeData(GteState &state, std::uint8_t index,
                           std::uint32_t value,
                           const GteProjectedVertex *projected,
                           GteExactState *exact_state,
                           const GteExactWord *exact_word) noexcept {
  index &= 31U;
  const auto retained =
      projected != nullptr && projected->valid && projected->packed_sxy == value
          ? *projected
          : GteProjectedVertex{};
  switch (index) {
  case 1U:
  case 3U:
  case 5U:
  case 8U:
  case 9U:
  case 10U:
  case 11U:
    state.data[index] = signExtend16(value);
    break;
  case 7U:
  case 16U:
  case 17U:
  case 18U:
  case 19U:
    state.data[index] = value & 0xffffU;
    break;
  case 12U:
  case 13U:
  case 14U:
    state.data[index] = value;
    state.projected[index - 12U] = retained;
    break;
  case 15U:
    state.data[12] = state.data[13];
    state.data[13] = state.data[14];
    state.data[14] = value;
    state.projected[0] = state.projected[1];
    state.projected[1] = state.projected[2];
    state.projected[2] = retained;
    break;
  case 28U:
    state.data[28] = value & 0x7fffU;
    state.data[9] = signExtend16((value & 0x1fU) * 0x80U);
    state.data[10] = signExtend16(((value >> 5U) & 0x1fU) * 0x80U);
    state.data[11] = signExtend16(((value >> 10U) & 0x1fU) * 0x80U);
    break;
  case 30U: {
    state.data[30] = value;
    const auto sign_mask = (value & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    state.data[31] =
        static_cast<std::uint32_t>(std::countl_zero(value ^ sign_mask));
    break;
  }
  case 29U:
  case 31U:
    break;
  default:
    state.data[index] = value;
    break;
  }

  if (exact_state == nullptr || !exact_state->transform_twin_enabled) {
    return;
  }
  if (index <= 5U) {
    captureVectorWrite(*exact_state, index, value, state, exact_word);
    return;
  }
  if (index >= 9U && index <= 11U) {
    captureResultWrite(*exact_state, index, value,
                       static_cast<double>(signedHalf(state.data[index])),
                       exact_word);
    return;
  }
  if (index == 28U || (index >= 25U && index <= 27U)) {
    invalidateExactResult(exact_state);
  }
}

void GteRuntime::writeControl(GteState &state, std::uint8_t index,
                              std::uint32_t value, GteExactState *exact_state,
                              const GteExactWord *exact_word) noexcept {
  index &= 31U;
  const auto previous_value = state.control[index];
  switch (index) {
  case 4U:
  case 12U:
  case 20U:
  case 26U:
  case 27U:
  case 29U:
  case 30U:
    state.control[index] = signExtend16(value);
    break;
  case 31U:
    state.control[31] = value & 0x7ffff000U;
    updateErrorFlag(state);
    break;
  default:
    state.control[index] = value;
    break;
  }

  if (exact_state == nullptr || !exact_state->transform_twin_enabled) {
    return;
  }
  if (index <= 4U) {
    captureRotationWrite(*exact_state, index, value, exact_word);
    return;
  }
  if (index >= 5U && index <= 7U) {
    captureTranslationWrite(
        *exact_state, index, value,
        static_cast<double>(signedWord(state.control[index])), exact_word);
    return;
  }
  if (index >= 24U && index <= 26U && state.control[index] != previous_value) {
    publishProjectionState(*exact_state);
  }
}

bool GteRuntime::executeCommand(GteState &state, std::uint32_t instruction,
                                GteExactState *exact_state,
                                bool capture_projection,
                                bool preserve_projection_precision) noexcept {
  const auto command = instruction & 0x3fU;
  const auto preserves_exact_result = command == 0x01U || command == 0x06U ||
                                      command == 0x12U || command == 0x2dU ||
                                      command == 0x2eU || command == 0x30U;
  if (exact_state != nullptr && exact_state->transform_twin_enabled &&
      !preserves_exact_result) {
    invalidateExactResult(exact_state);
  }
  switch (command) {
  case 0x01U:
    executePerspectiveTransform(state, instruction, exact_state,
                                capture_projection,
                                preserve_projection_precision);
    return true;
  case 0x06U:
    executeNormalClip(state, exact_state);
    return true;
  case 0x0cU:
    executeOuterProduct(state, instruction);
    return true;
  case 0x10U:
    executeDepthCueColorSingle(state, instruction);
    return true;
  case 0x11U:
    executeInterpolate(state, instruction);
    return true;
  case 0x12U:
    executeMatrixVectorMultiply(state, instruction, exact_state);
    return true;
  case 0x13U:
    executeNormalColorDepthCueSingle(state, instruction);
    return true;
  case 0x16U:
    executeNormalColorDepthCueTriple(state, instruction);
    return true;
  case 0x28U:
    executeSquare(state, instruction);
    return true;
  case 0x29U:
    executeDepthCueLight(state, instruction);
    return true;
  case 0x2aU:
    executeDepthCueColorTriple(state, instruction);
    return true;
  case 0x2dU:
    executeAverageDepth(state, 17U, 3U, 29U);
    return true;
  case 0x2eU:
    executeAverageDepth(state, 16U, 4U, 30U);
    return true;
  case 0x30U:
    executeTriplePerspectiveTransform(state, instruction, exact_state,
                                      capture_projection,
                                      preserve_projection_precision);
    return true;
  case 0x3dU:
    executeGeneralPurposeMultiply(state, instruction);
    return true;
  case 0x3fU:
    executeNormalColorTriple(state, instruction);
    return true;
  default:
    return false;
  }
}

GteExactWord GteRuntime::exactData(const GteState &state,
                                   const GteExactState &exact_state,
                                   std::uint8_t index) noexcept {
  GteExactWord word{};
  index &= 31U;
  word.raw = readData(state, index);
  const auto set_scalar = [&word,
                           &exact_state](const GteExactComponent &component) {
    if (!exactCurrent(component, exact_state)) {
      return;
    }
    word.scalar = component;
    word.halves[0] = component;
  };
  if (index >= 9U && index <= 11U) {
    set_scalar(exact_state.result[index - 9U]);
    return word;
  }
  if (index >= 25U && index <= 27U) {
    set_scalar(exact_state.result[index - 25U]);
    return word;
  }
  if (index <= 5U) {
    const auto vector = static_cast<std::size_t>(index / 2U);
    const auto first_component = static_cast<std::size_t>((index & 1U) * 2U);
    if (exactCurrent(exact_state.vectors[vector][first_component],
                     exact_state)) {
      word.halves[0] = exact_state.vectors[vector][first_component];
      if ((index & 1U) != 0U) {
        word.scalar = word.halves[0];
      }
    }
    if ((index & 1U) == 0U &&
        exactCurrent(exact_state.vectors[vector][first_component + 1U],
                     exact_state)) {
      word.halves[1] = exact_state.vectors[vector][first_component + 1U];
    }
  }
  return word;
}

const GteProjectedVertex *
GteRuntime::projectedVertex(const GteState &state,
                            std::uint8_t index) noexcept {
  index &= 31U;
  if (index == 15U) {
    index = 14U;
  }
  if (index < 12U || index > 14U) {
    return nullptr;
  }
  const auto &projected = state.projected[index - 12U];
  if (!projected.valid || projected.packed_sxy != readData(state, index)) {
    return nullptr;
  }
  return &projected;
}

} // namespace sf::psx
