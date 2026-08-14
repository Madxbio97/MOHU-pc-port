#include "sf/psx/mdec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace sf::psx {
namespace {

constexpr std::uint32_t control_reset = 1U << 31U;
constexpr std::uint32_t control_dma_in_enable = 1U << 30U;
constexpr std::uint32_t control_dma_out_enable = 1U << 29U;
constexpr std::uint32_t control_mask =
    control_dma_in_enable | control_dma_out_enable;

constexpr std::uint32_t status_output_empty = 1U << 31U;
constexpr std::uint32_t status_command_busy = 1U << 29U;
constexpr std::uint32_t status_input_request = 1U << 28U;
constexpr std::uint32_t status_output_request = 1U << 27U;
constexpr std::uint32_t status_output_signed = 1U << 24U;
constexpr std::uint32_t status_output_bit15 = 1U << 23U;
constexpr std::uint32_t status_idle_block = 4U << 16U;

constexpr std::array<std::uint8_t, 64U> zigzag_to_linear{
    0U,  1U,  8U,  16U, 9U,  2U,  3U,  10U, 17U, 24U, 32U, 25U, 18U,
    11U, 4U,  5U,  12U, 19U, 26U, 33U, 40U, 48U, 41U, 34U, 27U, 20U,
    13U, 6U,  7U,  14U, 21U, 28U, 35U, 42U, 49U, 56U, 57U, 50U, 43U,
    36U, 29U, 22U, 15U, 23U, 30U, 37U, 44U, 51U, 58U, 59U, 52U, 45U,
    38U, 31U, 39U, 46U, 53U, 60U, 61U, 54U, 47U, 55U, 62U, 63U};

constexpr std::uint32_t commandDecode(std::uint32_t value) noexcept {
  return value >> 29U;
}

constexpr std::uint32_t parameterWordCount(std::uint32_t command) noexcept {
  switch (commandDecode(command)) {
  case 1U:
    return command & 0xffffU;
  case 2U:
    return (command & 1U) != 0U ? 32U : 16U;
  case 3U:
    return 32U;
  default:
    return 0U;
  }
}

constexpr std::int32_t signExtend10(std::uint16_t value) noexcept {
  const auto narrowed = static_cast<std::int32_t>(value & 0x03ffU);
  return (narrowed & 0x0200) != 0 ? narrowed - 0x0400 : narrowed;
}

std::uint16_t inputHalfword(const std::vector<std::uint32_t> &input,
                            std::size_t index) noexcept {
  const auto word = input[index / 2U];
  return static_cast<std::uint16_t>(
      index % 2U == 0U ? word : static_cast<std::uint32_t>(word >> 16U));
}

std::int64_t roundShift32(std::int64_t value) noexcept {
  constexpr auto half = std::int64_t{1} << 31U;
  constexpr auto divisor = std::int64_t{1} << 32U;
  return value >= 0 ? (value + half) / divisor : -((-value + half) / divisor);
}

std::int32_t arithmeticShift(std::int32_t value, unsigned bits) noexcept {
  const auto divisor = static_cast<std::int32_t>(1U << bits);
  return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

void inverseDct(std::array<std::int32_t, 64U> &block,
                const std::array<std::int16_t, 64U> &scale) noexcept {
  std::array<std::int64_t, 64U> horizontal{};
  for (std::size_t frequency_y = 0U; frequency_y < 8U; ++frequency_y) {
    for (std::size_t x = 0U; x < 8U; ++x) {
      std::int64_t sum{};
      for (std::size_t frequency_x = 0U; frequency_x < 8U; ++frequency_x) {
        sum +=
            static_cast<std::int64_t>(block[frequency_y * 8U + frequency_x]) *
            scale[frequency_x * 8U + x];
      }
      horizontal[frequency_y * 8U + x] = sum;
    }
  }

  std::array<std::int32_t, 64U> pixels{};
  for (std::size_t y = 0U; y < 8U; ++y) {
    for (std::size_t x = 0U; x < 8U; ++x) {
      std::int64_t sum{};
      for (std::size_t frequency_y = 0U; frequency_y < 8U; ++frequency_y) {
        sum += horizontal[frequency_y * 8U + x] * scale[frequency_y * 8U + y];
      }
      pixels[y * 8U + x] = static_cast<std::int32_t>(
          std::clamp<std::int64_t>(roundShift32(sum), -128, 127));
    }
  }
  block = pixels;
}

enum class BlockResult : std::uint8_t { decoded, end_of_stream, malformed };

BlockResult decodeBlock(const std::vector<std::uint32_t> &input,
                        std::size_t &cursor,
                        const std::array<std::uint8_t, 64U> &quantization,
                        const std::array<std::int16_t, 64U> &scale,
                        std::array<std::int32_t, 64U> &block) noexcept {
  block.fill(0);
  const auto halfword_count = input.size() * 2U;
  std::uint16_t code{};
  do {
    if (cursor >= halfword_count) {
      return BlockResult::end_of_stream;
    }
    code = inputHalfword(input, cursor++);
  } while (code == 0xfe00U);

  const auto quantization_scale = static_cast<std::uint8_t>(code >> 10U);
  auto coefficient = std::size_t{};
  const auto storeCoefficient = [&](std::uint16_t item) noexcept {
    const auto amplitude = signExtend10(item);
    auto value = quantization_scale == 0U
                     ? amplitude * 2
                     : amplitude * quantization[coefficient];
    if (coefficient != 0U && quantization_scale != 0U) {
      value = (amplitude * quantization[coefficient] *
                   static_cast<std::int32_t>(quantization_scale) +
               4) /
              8;
    }
    value = std::clamp(value, -0x400, 0x3ff);
    const auto destination =
        quantization_scale == 0U ? coefficient : zigzag_to_linear[coefficient];
    block[destination] = value;
  };

  storeCoefficient(code);
  while (coefficient < 63U) {
    if (cursor >= halfword_count) {
      return BlockResult::malformed;
    }
    code = inputHalfword(input, cursor++);
    if (code == 0xfe00U) {
      inverseDct(block, scale);
      return BlockResult::decoded;
    }
    coefficient += static_cast<std::size_t>(code >> 10U) + 1U;
    if (coefficient >= 64U) {
      inverseDct(block, scale);
      return BlockResult::decoded;
    }
    storeCoefficient(code);
  }

  inverseDct(block, scale);
  return BlockResult::decoded;
}

std::uint8_t encodeComponent(std::int32_t value, bool signed_output) noexcept {
  value = std::clamp(value, -128, 127);
  if (signed_output) {
    return static_cast<std::uint8_t>(static_cast<std::int8_t>(value));
  }
  return static_cast<std::uint8_t>(value + 128);
}

std::uint16_t pack15(std::uint32_t rgb, bool bit15) noexcept {
  const auto convert = [](std::uint32_t component) noexcept {
    return std::min((component + 4U) >> 3U, 31U);
  };
  const auto red = convert(rgb & 0xffU);
  const auto green = convert((rgb >> 8U) & 0xffU);
  const auto blue = convert((rgb >> 16U) & 0xffU);
  return static_cast<std::uint16_t>(red | (green << 5U) | (blue << 10U) |
                                    (bit15 ? 0x8000U : 0U));
}

std::uint32_t neutralWord(std::uint8_t depth, bool signed_output,
                          bool bit15) noexcept {
  if (depth == 3U) {
    const auto component = signed_output ? 0U : 128U;
    const auto pixel =
        pack15(component | (component << 8U) | (component << 16U), bit15);
    return static_cast<std::uint32_t>(pixel) |
           (static_cast<std::uint32_t>(pixel) << 16U);
  }
  if (signed_output) {
    return 0U;
  }
  return depth == 0U ? 0x88888888U : 0x80808080U;
}

} // namespace

Mdec::Mdec() {
  state_.input_words.reserve(maximum_input_words);
  state_.output_words.reserve(maximum_output_words);
  reset();
}

void Mdec::reset() noexcept {
  state_.control = 0U;
  state_.command = 0U;
  state_.parameter_words_remaining = 0U;
  state_.output_depth = 0U;
  state_.output_signed = false;
  state_.output_bit15 = false;
  state_.command_busy = false;
  state_.output_ready = false;
  state_.decode_error = false;
  state_.luminance_quantization.fill(0U);
  state_.color_quantization.fill(0U);
  state_.scale_table.fill(0);
  state_.input_words.clear();
  state_.output_words.clear();
  state_.output_word_position = 0U;
}

std::uint32_t Mdec::status() const noexcept {
  auto value = static_cast<std::uint32_t>(state_.output_depth & 3U) << 25U;
  value |= state_.output_signed ? status_output_signed : 0U;
  value |= state_.output_bit15 ? status_output_bit15 : 0U;
  value |= state_.command_busy ? status_command_busy : 0U;
  value |= dmaInRequest() ? status_input_request : 0U;
  value |= dmaOutRequest() ? status_output_request : 0U;
  value |= state_.output_ready ? 0U : status_output_empty;
  value |= status_idle_block;
  value |= state_.command_busy
               ? (state_.parameter_words_remaining - 1U) & 0xffffU
               : 0xffffU;
  return value;
}

void Mdec::writeControl(std::uint32_t value) noexcept {
  const auto enabled = value & control_mask;
  if ((value & control_reset) != 0U) {
    reset();
  }
  state_.control = enabled;
}

bool Mdec::writeData(std::uint32_t value) noexcept {
  if (!state_.command_busy) {
    beginCommand(value);
    return true;
  }
  if (state_.input_words.size() >= maximum_input_words) {
    return false;
  }
  state_.input_words.push_back(value);
  --state_.parameter_words_remaining;
  if (state_.parameter_words_remaining == 0U) {
    finishCommand();
  }
  return true;
}

bool Mdec::readData(std::uint32_t &value) noexcept {
  if (!state_.output_ready) {
    return false;
  }
  if (state_.output_word_position < state_.output_words.size()) {
    value = state_.output_words[state_.output_word_position++];
    if (state_.output_word_position == state_.output_words.size()) {
      state_.output_words.clear();
      state_.output_word_position = 0U;
      state_.output_ready = state_.decode_error;
    }
    return true;
  }
  value = neutralWord(state_.output_depth, state_.output_signed,
                      state_.output_bit15);
  return state_.decode_error;
}

bool Mdec::dmaInRequest() const noexcept {
  return (state_.control & control_dma_in_enable) != 0U;
}

bool Mdec::dmaOutRequest() const noexcept {
  return (state_.control & control_dma_out_enable) != 0U && state_.output_ready;
}

bool Mdec::validateState(const MdecState &state) const noexcept {
  if ((state.control & ~control_mask) != 0U || state.output_depth > 3U ||
      state.input_words.size() > maximum_input_words ||
      state.output_words.size() > maximum_output_words ||
      state.output_word_position > state.output_words.size()) {
    return false;
  }
  if (state.command_busy != (state.parameter_words_remaining != 0U)) {
    return false;
  }
  if (state.command_busy &&
      state.input_words.size() + state.parameter_words_remaining !=
          parameterWordCount(state.command)) {
    return false;
  }
  if (!state.command_busy && !state.input_words.empty()) {
    return false;
  }
  const auto has_output =
      state.output_word_position < state.output_words.size() ||
      state.decode_error;
  return state.output_ready == has_output;
}

bool Mdec::restoreState(const MdecState &state) noexcept {
  if (!validateState(state)) {
    return false;
  }
  state_.control = state.control;
  state_.command = state.command;
  state_.parameter_words_remaining = state.parameter_words_remaining;
  state_.output_depth = state.output_depth;
  state_.output_signed = state.output_signed;
  state_.output_bit15 = state.output_bit15;
  state_.command_busy = state.command_busy;
  state_.output_ready = state.output_ready;
  state_.decode_error = state.decode_error;
  state_.luminance_quantization = state.luminance_quantization;
  state_.color_quantization = state.color_quantization;
  state_.scale_table = state.scale_table;
  state_.input_words.assign(state.input_words.begin(), state.input_words.end());
  state_.output_words.assign(state.output_words.begin(),
                             state.output_words.end());
  state_.output_word_position = state.output_word_position;
  return true;
}

void Mdec::beginCommand(std::uint32_t value) noexcept {
  state_.command = value;
  state_.output_ready = false;
  state_.decode_error = false;
  state_.input_words.clear();
  state_.output_words.clear();
  state_.output_word_position = 0U;
  state_.output_depth = static_cast<std::uint8_t>((value >> 27U) & 3U);
  state_.output_signed = (value & (1U << 26U)) != 0U;
  state_.output_bit15 = (value & (1U << 25U)) != 0U;
  state_.parameter_words_remaining = parameterWordCount(value);
  state_.command_busy = state_.parameter_words_remaining != 0U;
  if (!state_.command_busy) {
    finishCommand();
  }
}

void Mdec::finishCommand() noexcept {
  const auto command = commandDecode(state_.command);
  state_.parameter_words_remaining = 0U;
  state_.command_busy = false;
  switch (command) {
  case 1U:
    if (!state_.input_words.empty() && !decodeMacroblocks()) {
      state_.decode_error = true;
      if (state_.output_words.empty()) {
        appendNeutralMacroblock();
      }
    }
    break;
  case 2U:
    loadQuantizationTables();
    break;
  case 3U:
    loadScaleTable();
    break;
  default:
    break;
  }
  state_.input_words.clear();
  state_.output_word_position = 0U;
  state_.output_ready = !state_.output_words.empty() || state_.decode_error;
}

void Mdec::loadQuantizationTables() noexcept {
  for (std::size_t index = 0U; index < 64U; ++index) {
    const auto word = state_.input_words[index / 4U];
    state_.luminance_quantization[index] = static_cast<std::uint8_t>(
        word >> static_cast<unsigned>((index % 4U) * 8U));
  }
  if ((state_.command & 1U) == 0U) {
    return;
  }
  for (std::size_t index = 0U; index < 64U; ++index) {
    const auto byte_index = index + 64U;
    const auto word = state_.input_words[byte_index / 4U];
    state_.color_quantization[index] = static_cast<std::uint8_t>(
        word >> static_cast<unsigned>((byte_index % 4U) * 8U));
  }
}

void Mdec::loadScaleTable() noexcept {
  for (std::size_t index = 0U; index < 64U; ++index) {
    state_.scale_table[index] =
        static_cast<std::int16_t>(inputHalfword(state_.input_words, index));
  }
}

bool Mdec::decodeMacroblocks() noexcept {
  auto cursor = std::size_t{};
  auto decoded_any = false;
  while (cursor < state_.input_words.size() * 2U) {
    if (state_.output_depth <= 1U) {
      const auto words_per_block = state_.output_depth == 0U ? 8U : 16U;
      if (state_.output_words.size() + words_per_block > maximum_output_words) {
        return false;
      }
      std::array<std::int32_t, 64U> block{};
      const auto result =
          decodeBlock(state_.input_words, cursor, state_.luminance_quantization,
                      state_.scale_table, block);
      if (result == BlockResult::end_of_stream) {
        return decoded_any;
      }
      if (result == BlockResult::malformed) {
        return false;
      }
      decoded_any = true;
      std::array<std::uint8_t, 64U> pixels{};
      for (std::size_t index = 0U; index < pixels.size(); ++index) {
        pixels[index] = encodeComponent(block[index], state_.output_signed);
      }
      if (state_.output_depth == 0U) {
        for (std::size_t index = 0U; index < pixels.size(); index += 8U) {
          auto word = std::uint32_t{};
          for (std::size_t item = 0U; item < 8U; ++item) {
            word |=
                static_cast<std::uint32_t>((pixels[index + item] >> 4U) & 0x0fU)
                << static_cast<unsigned>(item * 4U);
          }
          state_.output_words.push_back(word);
        }
      } else {
        for (std::size_t index = 0U; index < pixels.size(); index += 4U) {
          state_.output_words.push_back(
              static_cast<std::uint32_t>(pixels[index]) |
              (static_cast<std::uint32_t>(pixels[index + 1U]) << 8U) |
              (static_cast<std::uint32_t>(pixels[index + 2U]) << 16U) |
              (static_cast<std::uint32_t>(pixels[index + 3U]) << 24U));
        }
      }
      continue;
    }

    const auto words_per_block = state_.output_depth == 2U ? 192U : 128U;
    if (state_.output_words.size() + words_per_block > maximum_output_words) {
      return false;
    }
    std::array<std::array<std::int32_t, 64U>, 6U> blocks{};
    for (std::size_t block_index = 0U; block_index < blocks.size();
         ++block_index) {
      const auto &quantization = block_index < 2U
                                     ? state_.color_quantization
                                     : state_.luminance_quantization;
      const auto result = decodeBlock(state_.input_words, cursor, quantization,
                                      state_.scale_table, blocks[block_index]);
      if (result == BlockResult::end_of_stream) {
        return block_index == 0U ? decoded_any : false;
      }
      if (result == BlockResult::malformed) {
        return false;
      }
    }
    decoded_any = true;

    std::array<std::uint32_t, 256U> rgb{};
    for (std::size_t y = 0U; y < 16U; ++y) {
      for (std::size_t x = 0U; x < 16U; ++x) {
        const auto luminance_block =
            2U + (y >= 8U ? 2U : 0U) + (x >= 8U ? 1U : 0U);
        const auto luminance =
            blocks[luminance_block][(y & 7U) * 8U + (x & 7U)];
        const auto chroma_index = (y / 2U) * 8U + x / 2U;
        const auto red_chroma = blocks[0U][chroma_index];
        const auto blue_chroma = blocks[1U][chroma_index];
        const auto red =
            luminance + arithmeticShift(359 * red_chroma + 128, 8U);
        const auto green_terms =
            ((-88 * blue_chroma) & ~31) + ((-183 * red_chroma) & ~7) + 128;
        const auto green = luminance + arithmeticShift(green_terms, 8U);
        const auto blue =
            luminance + arithmeticShift(454 * blue_chroma + 128, 8U);
        rgb[y * 16U + x] = static_cast<std::uint32_t>(
                               encodeComponent(red, state_.output_signed)) |
                           (static_cast<std::uint32_t>(
                                encodeComponent(green, state_.output_signed))
                            << 8U) |
                           (static_cast<std::uint32_t>(
                                encodeComponent(blue, state_.output_signed))
                            << 16U);
      }
    }

    if (state_.output_depth == 3U) {
      for (std::size_t index = 0U; index < rgb.size(); index += 2U) {
        const auto first = pack15(rgb[index], state_.output_bit15);
        const auto second = pack15(rgb[index + 1U], state_.output_bit15);
        state_.output_words.push_back(
            static_cast<std::uint32_t>(first) |
            (static_cast<std::uint32_t>(second) << 16U));
      }
    } else {
      auto word = std::uint32_t{};
      auto packed_bytes = std::size_t{};
      for (const auto color : rgb) {
        for (unsigned channel = 0U; channel < 3U; ++channel) {
          word |= ((color >> (channel * 8U)) & 0xffU)
                  << static_cast<unsigned>((packed_bytes % 4U) * 8U);
          ++packed_bytes;
          if (packed_bytes % 4U == 0U) {
            state_.output_words.push_back(word);
            word = 0U;
          }
        }
      }
    }
  }
  return decoded_any;
}

void Mdec::appendNeutralMacroblock() noexcept {
  const auto word_count = state_.output_depth == 0U   ? 8U
                          : state_.output_depth == 1U ? 16U
                          : state_.output_depth == 2U ? 192U
                                                      : 128U;
  const auto word = neutralWord(state_.output_depth, state_.output_signed,
                                state_.output_bit15);
  for (std::size_t index = 0U;
       index < word_count && state_.output_words.size() < maximum_output_words;
       ++index) {
    state_.output_words.push_back(word);
  }
}

} // namespace sf::psx
