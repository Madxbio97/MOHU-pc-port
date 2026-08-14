#include "mohu/runtime.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

constexpr std::uint32_t pad_zero_buffer = 0x8003a65cU;
constexpr std::uint16_t neutral_buttons = 0xffffU;
constexpr std::uint16_t cross_buttons = 0xbfffU;
constexpr std::array<std::uint8_t, 4U> centered_axes{128U, 128U, 128U, 128U};
constexpr std::array<std::uint8_t, 4U> test_axes{16U, 224U, 64U, 192U};
constexpr std::uint64_t default_maximum_frames = 18'000U;
constexpr std::uint64_t menu_pulse_start_frame = 6'000U;
constexpr std::uint64_t menu_pulse_period_frames = 240U;
constexpr std::uint64_t menu_pulse_hold_frames = 30U;
constexpr std::uint64_t input_timeout_frames = 60U;

struct PadSample {
  std::uint8_t status{};
  std::uint8_t id{};
  std::uint16_t buttons{};
  std::array<std::uint8_t, 4U> axes{};
};

PadSample readPad(const mohu::Runtime &runtime) {
  PadSample sample;
  if (!runtime.cpu().read8(pad_zero_buffer, sample.status) ||
      !runtime.cpu().read8(pad_zero_buffer + 1U, sample.id) ||
      !runtime.cpu().read16(pad_zero_buffer + 2U, sample.buttons)) {
    throw std::runtime_error{"Could not read the retail PAD buffer"};
  }
  for (std::size_t index{}; index < sample.axes.size(); ++index) {
    if (!runtime.cpu().read8(pad_zero_buffer + 4U +
                                 static_cast<std::uint32_t>(index),
                             sample.axes[index])) {
      throw std::runtime_error{"Could not read retail PAD analog axes"};
    }
  }
  return sample;
}

void runFrame(mohu::Runtime &runtime, std::uint16_t buttons,
              std::array<std::uint8_t, 4U> axes) {
  const auto result = runtime.runFrame(buttons, axes);
  if (!result.running()) {
    throw std::runtime_error{result.detail.empty() ? "Runtime stopped"
                                                   : result.detail};
  }
}

bool analogConnected(const PadSample &sample) noexcept {
  return sample.status == 0U && sample.id == 0x73U;
}

std::string sampleDetail(const PadSample &sample) {
  std::ostringstream stream;
  stream << "status=0x" << std::hex << static_cast<unsigned>(sample.status)
         << " id=0x" << static_cast<unsigned>(sample.id) << " buttons=0x"
         << std::setw(4) << std::setfill('0') << sample.buttons
         << " axes=" << static_cast<unsigned>(sample.axes[0U]) << ','
         << static_cast<unsigned>(sample.axes[1U]) << ','
         << static_cast<unsigned>(sample.axes[2U]) << ','
         << static_cast<unsigned>(sample.axes[3U]);
  return stream.str();
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2 || argc > 3) {
      std::cerr << "Usage: mohu_analog_pad_probe <game.bin|game.cue> "
                   "[maximum-frames]\n";
      return 64;
    }
    const auto maximum_frames =
        argc == 3 ? std::stoull(argv[2]) : default_maximum_frames;
    if (maximum_frames <= menu_pulse_start_frame) {
      throw std::invalid_argument{
          "Maximum frame limit must extend beyond the title menu"};
    }

    auto runtime =
        mohu::Runtime{sf::game::GameDisc::open(std::filesystem::path{argv[1]})};

    std::uint64_t analog_frame{};
    bool saw_digital{};
    PadSample last_sample{};
    for (std::uint64_t frame = 1U; frame <= maximum_frames; ++frame) {
      const auto pulse_offset =
          frame >= menu_pulse_start_frame
              ? (frame - menu_pulse_start_frame) % menu_pulse_period_frames
              : menu_pulse_hold_frames;
      const auto buttons = pulse_offset < menu_pulse_hold_frames
                               ? cross_buttons
                               : neutral_buttons;
      runFrame(runtime, buttons, centered_axes);
      last_sample = readPad(runtime);
      if (last_sample.status == 0U && last_sample.id == 0x41U) {
        saw_digital = true;
      }
      if (saw_digital && analogConnected(last_sample)) {
        analog_frame = frame;
        break;
      }
    }
    if (!saw_digital) {
      throw std::runtime_error{
          "Retail game did not expose P1 as a digital pad before DualShock: " +
          sampleDetail(last_sample)};
    }
    if (analog_frame == 0U) {
      throw std::runtime_error{
          "Retail game did not switch P1 to DualShock analog mode: " +
          sampleDetail(last_sample)};
    }

    std::uint64_t axes_latency{};
    PadSample moved{};
    for (std::uint64_t frame = 1U; frame <= input_timeout_frames; ++frame) {
      runFrame(runtime, neutral_buttons, test_axes);
      const auto sample = readPad(runtime);
      if (analogConnected(sample) && sample.buttons == neutral_buttons &&
          sample.axes == test_axes) {
        axes_latency = frame;
        moved = sample;
        break;
      }
    }
    if (axes_latency == 0U) {
      throw std::runtime_error{
          "Retail PAD buffer did not observe host analog axes: " +
          sampleDetail(readPad(runtime))};
    }

    std::uint64_t center_latency{};
    PadSample centered{};
    for (std::uint64_t frame = 1U; frame <= input_timeout_frames; ++frame) {
      runFrame(runtime, neutral_buttons, centered_axes);
      const auto sample = readPad(runtime);
      if (analogConnected(sample) && sample.buttons == neutral_buttons &&
          sample.axes == centered_axes) {
        center_latency = frame;
        centered = sample;
        break;
      }
    }
    if (center_latency == 0U) {
      throw std::runtime_error{
          "Retail PAD buffer did not observe centered analog release: " +
          sampleDetail(readPad(runtime))};
    }

    std::cout << "MOHU retail analog PAD probe passed: analog_frame="
              << analog_frame << " id=0x" << std::hex
              << static_cast<unsigned>(moved.id)
              << " axes=" << static_cast<unsigned>(moved.axes[0U]) << ','
              << static_cast<unsigned>(moved.axes[1U]) << ','
              << static_cast<unsigned>(moved.axes[2U]) << ','
              << static_cast<unsigned>(moved.axes[3U])
              << " centered=" << static_cast<unsigned>(centered.axes[0U]) << ','
              << static_cast<unsigned>(centered.axes[1U]) << ','
              << static_cast<unsigned>(centered.axes[2U]) << ','
              << static_cast<unsigned>(centered.axes[3U]) << std::dec
              << " axes_latency=" << axes_latency
              << " center_latency=" << center_latency << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mohu_analog_pad_probe: " << error.what() << '\n';
    return 1;
  }
}
