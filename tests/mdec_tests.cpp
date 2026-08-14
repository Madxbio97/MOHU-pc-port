#include "sf/psx/machine.hpp"
#include "sf/psx/mdec.hpp"
#include "sf/psx/r3000_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::uint32_t mdec = 0x1f801820U;
constexpr std::uint32_t dma0 = 0x1f801080U;
constexpr std::uint32_t dma1 = 0x1f801090U;
constexpr std::uint32_t dpcr = 0x1f8010f0U;
constexpr std::uint32_t dma_enable_mdec = (1U << 3U) | (1U << 7U);
constexpr std::uint32_t dma_from_ram = 0x01000201U;
constexpr std::uint32_t dma_to_ram = 0x01000200U;
constexpr std::uint32_t set_quantization = (2U << 29U) | 1U;
constexpr std::uint32_t set_scale = 3U << 29U;
constexpr std::uint32_t decode_24_bit = 0x30000006U;
constexpr std::uint32_t decode_15_bit = 0x38000006U;
constexpr std::uint32_t decode_15_bit_with_stp = 0x3a000006U;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

void loadScaleTable(sf::psx::Mdec &decoder) {
  require(decoder.writeData(set_scale), "MDEC rejected the scale command");
  for (std::size_t word_index = 0U; word_index < 32U; ++word_index) {
    const auto first_index = word_index * 2U;
    const auto low = first_index < 8U ? 0x5a82U : 0U;
    const auto high = first_index + 1U < 8U ? 0x5a82U : 0U;
    require(decoder.writeData(low | (high << 16U)), "MDEC rejected scale data");
  }
}

void submitMacroblock(sf::psx::Mdec &decoder, std::uint32_t command,
                      const std::array<std::uint16_t, 6U> &dc) {
  require(decoder.writeData(command), "MDEC rejected a decode command");
  for (const auto value : dc) {
    require(decoder.writeData(static_cast<std::uint32_t>(value) | 0xfe000000U),
            "MDEC rejected macroblock data");
  }
}

std::vector<std::uint32_t> drain(sf::psx::Mdec &decoder,
                                 std::size_t expected_words) {
  std::vector<std::uint32_t> result;
  result.reserve(expected_words);
  for (std::size_t index = 0U; index < expected_words; ++index) {
    std::uint32_t value{};
    require(decoder.readData(value), "MDEC output ended early");
    result.push_back(value);
  }
  std::uint32_t extra{};
  require(!decoder.readData(extra), "MDEC output exceeded the macroblock");
  return result;
}

void testTablesAndRegisterFrontEnd() {
  sf::psx::Mdec decoder;
  require((decoder.status() & (1U << 31U)) != 0U,
          "MDEC reset status did not report an empty output FIFO");
  require((decoder.status() & 0xffffU) == 0xffffU,
          "MDEC reset parameter count was not idle");

  decoder.writeControl(0x60000000U);
  require(decoder.dmaInRequest() && !decoder.dmaOutRequest(),
          "MDEC DMA request enables were not honored");

  require(decoder.writeData(set_quantization),
          "MDEC rejected quantization tables");
  for (std::uint32_t word = 0U; word < 32U; ++word) {
    const auto first = word * 4U;
    require(decoder.writeData(first | ((first + 1U) << 8U) |
                              ((first + 2U) << 16U) | ((first + 3U) << 24U)),
            "MDEC rejected quantization data");
  }
  const auto tables = decoder.captureState();
  for (std::size_t index = 0U; index < 64U; ++index) {
    require(tables.luminance_quantization[index] == index,
            "MDEC luminance table byte order was wrong");
    require(tables.color_quantization[index] == index + 64U,
            "MDEC color table byte order was wrong");
  }

  loadScaleTable(decoder);
  const auto scale = decoder.captureState();
  for (std::size_t index = 0U; index < scale.scale_table.size(); ++index) {
    require(scale.scale_table[index] ==
                static_cast<std::int16_t>(index < 8U ? 0x5a82U : 0U),
            "MDEC scale table halfword order was wrong");
  }
}

void testIdctPackingAndSnapshot() {
  sf::psx::Mdec decoder;
  decoder.writeControl(0x60000000U);
  loadScaleTable(decoder);
  submitMacroblock(decoder, decode_15_bit_with_stp,
                   {0U, 0U, 64U, 64U, 64U, 64U});
  require(decoder.dmaOutRequest(), "MDEC did not raise DMA-out");

  for (std::size_t index = 0U; index < 37U; ++index) {
    std::uint32_t value{};
    require(decoder.readData(value) && value == 0xca52ca52U,
            "MDEC 15-bit IDCT output was wrong");
  }
  const auto checkpoint = decoder.captureState();
  sf::psx::Mdec restored;
  require(restored.restoreState(checkpoint),
          "MDEC rejected a valid output snapshot");
  require(restored.captureState() == checkpoint,
          "MDEC output snapshot did not round-trip");

  for (std::size_t index = 37U; index < 128U; ++index) {
    std::uint32_t value{};
    require(restored.readData(value) && value == 0xca52ca52U,
            "MDEC restored output changed");
  }
  std::uint32_t extra{};
  require(!restored.readData(extra) && !restored.dmaOutRequest(),
          "MDEC output request did not clear after draining");
}

void testYuvAnd24BitPacking() {
  sf::psx::Mdec decoder;
  loadScaleTable(decoder);
  submitMacroblock(decoder, decode_24_bit, {0x0100U, 0U, 0U, 0U, 0U, 0U});
  const auto output = drain(decoder, 192U);
  constexpr std::array cadence{0xda8052daU, 0x52da8052U, 0x8052da80U};
  for (std::size_t index = 0U; index < output.size(); ++index) {
    require(output[index] == cadence[index % cadence.size()],
            "MDEC 24-bit RGB byte packing was wrong");
  }
}

void testMachineMmioAndDma() {
  auto runtime = std::make_unique<sf::psx::R3000Runtime>();
  auto machine = std::make_unique<sf::psx::PsxMachine>(*runtime);

  require(runtime->write32(mdec + 4U, 0x60000000U),
          "MDEC control MMIO write failed");
  loadScaleTable(machine->mdec());
  require(runtime->write32(dpcr, machine->dma().dpcr() | dma_enable_mdec),
          "Could not enable MDEC DMA channels");

  constexpr std::uint32_t source = 0x00002000U;
  constexpr std::uint32_t destination = 0x00003000U;
  constexpr std::array input{
      decode_15_bit, 0xfe000000U, 0xfe000000U, 0xfe000000U,
      0xfe000000U,   0xfe000000U, 0xfe000000U,
  };
  for (std::size_t index = 0U; index < input.size(); ++index) {
    require(runtime->write32(source + static_cast<std::uint32_t>(index * 4U),
                             input[index]),
            "Could not prepare MDEC input DMA payload");
  }

  require(runtime->write32(dma0, source) &&
              runtime->write32(dma0 + 4U, 0x00010007U) &&
              runtime->write32(dma0 + 8U, dma_from_ram) &&
              machine->completePendingDmaTransfers(),
          "MDEC input DMA did not complete");
  require(machine->mdec().dmaOutRequest(),
          "MDEC input DMA did not produce an output request");

  require(runtime->write32(dma1, destination) &&
              runtime->write32(dma1 + 4U, 0x00010080U) &&
              runtime->write32(dma1 + 8U, dma_to_ram) &&
              machine->completePendingDmaTransfers(),
          "MDEC output DMA did not complete");

  for (std::size_t index = 0U; index < 128U; ++index) {
    std::uint32_t value{};
    require(runtime->read32(
                destination + static_cast<std::uint32_t>(index * 4U), value) &&
                value == 0x42104210U,
            "MDEC output DMA wrote wrong pixels to RAM");
  }
  require(!machine->mdec().dmaOutRequest() &&
              (machine->dma().chcr(sf::psx::DmaChannel::mdec_out) &
               (1U << 24U)) == 0U,
          "MDEC output DMA did not become idle");
}

} // namespace

int main() {
  try {
    testTablesAndRegisterFrontEnd();
    testIdctPackingAndSnapshot();
    testYuvAnd24BitPacking();
    testMachineMmioAndDma();
  } catch (const std::exception &error) {
    std::cerr << "mdec_tests failed: " << error.what() << '\n';
    return 1;
  }

  std::cout << "mdec_tests passed\n";
  return 0;
}
