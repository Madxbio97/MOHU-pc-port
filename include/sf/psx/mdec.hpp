#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sf::psx {

struct MdecState {
  std::uint32_t control{};
  std::uint32_t command{};
  std::uint32_t parameter_words_remaining{};
  std::uint8_t output_depth{};
  bool output_signed{};
  bool output_bit15{};
  bool command_busy{};
  bool output_ready{};
  bool decode_error{};
  std::array<std::uint8_t, 64U> luminance_quantization{};
  std::array<std::uint8_t, 64U> color_quantization{};
  std::array<std::int16_t, 64U> scale_table{};
  std::vector<std::uint32_t> input_words;
  std::vector<std::uint32_t> output_words;
  std::uint32_t output_word_position{};

  [[nodiscard]] friend bool operator==(const MdecState &,
                                       const MdecState &) = default;
};

// PS1 motion decoder register, DMA front-end and macroblock reconstruction.
class Mdec final {
public:
  static constexpr std::uint32_t register_span = 8U;

  Mdec();

  void reset() noexcept;
  [[nodiscard]] std::uint32_t status() const noexcept;
  void writeControl(std::uint32_t value) noexcept;
  [[nodiscard]] bool writeData(std::uint32_t value) noexcept;
  [[nodiscard]] bool readData(std::uint32_t &value) noexcept;

  [[nodiscard]] bool dmaInRequest() const noexcept;
  [[nodiscard]] bool dmaOutRequest() const noexcept;

  [[nodiscard]] MdecState captureState() const { return state_; }
  [[nodiscard]] bool validateState(const MdecState &state) const noexcept;
  [[nodiscard]] bool restoreState(const MdecState &state) noexcept;

private:
  static constexpr std::size_t maximum_input_words = 65'536U;
  static constexpr std::size_t maximum_output_words = 65'536U;

  void beginCommand(std::uint32_t value) noexcept;
  void finishCommand() noexcept;
  void loadQuantizationTables() noexcept;
  void loadScaleTable() noexcept;
  [[nodiscard]] bool decodeMacroblocks() noexcept;
  void appendNeutralMacroblock() noexcept;

  MdecState state_{};
};

} // namespace sf::psx
