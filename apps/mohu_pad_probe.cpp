#include "mohu/runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {

constexpr std::uint32_t pad_zero_buffer = 0x8003a65cU;
constexpr std::uint16_t neutral_buttons = 0xffffU;
constexpr std::uint16_t cross_buttons = 0xbfffU;
constexpr std::uint64_t default_warmup_frames = 9'000U;
constexpr std::uint64_t input_timeout_frames = 30U;

struct PadSample {
  std::uint8_t status{};
  std::uint8_t id{};
  std::uint16_t buttons{};
};

PadSample readPad(const mohu::Runtime &runtime) {
  PadSample sample;
  if (!runtime.cpu().read8(pad_zero_buffer, sample.status) ||
      !runtime.cpu().read8(pad_zero_buffer + 1U, sample.id) ||
      !runtime.cpu().read16(pad_zero_buffer + 2U, sample.buttons)) {
    throw std::runtime_error{"Could not read the retail PAD buffer"};
  }
  return sample;
}

bool connected(const PadSample &sample) noexcept {
  return sample.status == 0U && (sample.id == 0x41U || sample.id == 0x73U);
}

void runFrame(mohu::Runtime &runtime, std::uint16_t buttons) {
  const auto result = runtime.runFrame(buttons);
  if (!result.running()) {
    throw std::runtime_error{result.detail.empty() ? "Runtime stopped"
                                                   : result.detail};
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2 || argc > 3) {
      std::cerr << "Usage: mohu_pad_probe <game.bin|game.cue> "
                   "[max-warmup-frames]\n";
      return 64;
    }
    const auto maximum_warmup =
        argc == 3 ? std::stoull(argv[2]) : default_warmup_frames;
    if (maximum_warmup == 0U) {
      throw std::invalid_argument{"Warmup frame limit must be positive"};
    }

    auto runtime =
        mohu::Runtime{sf::game::GameDisc::open(std::filesystem::path{argv[1]})};

    std::uint64_t ready_frame{};
    PadSample neutral{};
    for (std::uint64_t frame = 1U; frame <= maximum_warmup; ++frame) {
      runFrame(runtime, neutral_buttons);
      const auto sample = readPad(runtime);
      if (connected(sample) && sample.buttons == neutral_buttons) {
        ready_frame = frame;
        neutral = sample;
        break;
      }
    }
    if (ready_frame == 0U) {
      throw std::runtime_error{"Retail PAD driver did not publish a connected "
                               "neutral controller"};
    }

    std::uint64_t press_latency{};
    PadSample pressed{};
    for (std::uint64_t frame = 1U; frame <= input_timeout_frames; ++frame) {
      runFrame(runtime, cross_buttons);
      const auto sample = readPad(runtime);
      if (connected(sample) && sample.buttons == cross_buttons) {
        press_latency = frame;
        pressed = sample;
        break;
      }
    }
    if (press_latency == 0U) {
      throw std::runtime_error{"Retail PAD buffer did not observe Cross"};
    }

    std::uint64_t release_latency{};
    PadSample released{};
    for (std::uint64_t frame = 1U; frame <= input_timeout_frames; ++frame) {
      runFrame(runtime, neutral_buttons);
      const auto sample = readPad(runtime);
      if (connected(sample) && sample.buttons == neutral_buttons) {
        release_latency = frame;
        released = sample;
        break;
      }
    }
    if (release_latency == 0U) {
      throw std::runtime_error{"Retail PAD buffer did not observe release"};
    }

    std::cout << "MOHU retail PAD probe passed: ready_frame=" << ready_frame
              << " id=0x" << std::hex << static_cast<unsigned>(pressed.id)
              << " neutral=" << std::setw(4) << std::setfill('0')
              << neutral.buttons << " cross=" << std::setw(4) << pressed.buttons
              << " released=" << std::setw(4) << released.buttons << std::dec
              << " press_latency=" << press_latency
              << " release_latency=" << release_latency << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mohu_pad_probe: " << error.what() << '\n';
    return 1;
  }
}
