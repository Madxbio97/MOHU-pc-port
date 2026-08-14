#include "sf/game/game_disc.hpp"
#include "sf/psx/bios_hle.hpp"
#include "sf/psx/machine.hpp"
#include "sf/psx/r3000_runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {


void printCpuState(const sf::psx::R3000Runtime &runtime) {
  const auto &state = runtime.state();
  const auto old_flags = std::cout.flags();
  const auto old_fill = std::cout.fill();
  std::cout << std::hex << std::setfill('0')
            << "pc=0x" << std::setw(8) << state.pc
            << " next=0x" << std::setw(8) << state.next_pc
            << " ra=0x" << std::setw(8) << state.gpr[31]
            << " sp=0x" << std::setw(8) << state.gpr[29]
            << " gp=0x" << std::setw(8) << state.gpr[28] << '\n'
            << "a0=0x" << std::setw(8) << state.gpr[4]
            << " a1=0x" << std::setw(8) << state.gpr[5]
            << " a2=0x" << std::setw(8) << state.gpr[6]
            << " a3=0x" << std::setw(8) << state.gpr[7]
            << " t1=0x" << std::setw(8) << state.gpr[9] << '\n';
  std::cout.flags(old_flags);
  std::cout.fill(old_fill);
}


} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2 || argc > 3) {
      std::cerr << "Usage: mohu_boot_probe <game.bin|game.cue> [budget]\n";
      return 64;
    }
    const auto budget =
        argc == 3 ? std::stoull(argv[2]) : std::uint64_t{10'000'000U};

    auto disc = sf::game::GameDisc::open(std::filesystem::path{argv[1]});
    if (!disc.game() || disc.game()->serial != "SLUS-01270") {
      std::cerr << "Unsupported disc build\n";
      return 2;
    }

    sf::psx::R3000Runtime runtime;
    runtime.loadExecutable(disc.executable());
    sf::psx::PsxMachine machine{runtime};
    sf::psx::BiosHle bios{runtime};

    std::uint64_t instructions{};
    std::uint64_t irq_dispatches{};
    for (; instructions < budget;) {
      if (bios.atExceptionReturn()) {
        if (!bios.completeException()) {
          std::cerr << "Cannot complete BIOS exception\n";
          return 3;
        }
        continue;
      }
      if (bios.atExceptionBoundary()) {
        if (!bios.dispatchException()) {
          std::cerr << "Cannot dispatch BIOS exception\n";
          printCpuState(runtime);
          return 3;
        }
        ++irq_dispatches;
        continue;
      }
      if (bios.atCallBoundary()) {
        const auto vector = runtime.state().pc;
        const auto call = runtime.state().gpr[9];
        if (bios.handleCall()) {
          const auto old_flags = std::cout.flags();
          const auto old_fill = std::cout.fill();
          std::cout << std::hex << std::setfill('0')
                    << "HLE BIOS vector=0x" << std::setw(2) << vector
                    << " call=0x" << std::setw(2) << call << '\n';
          std::cout.flags(old_flags);
          std::cout.fill(old_fill);
          continue;
        }
        std::cout << "Unsupported BIOS call after " << instructions << "\n";
        printCpuState(runtime);
        return 0;
      }

      const auto execution = machine.step();
      instructions += execution.instructions;
      if (execution.reason == sf::psx::R3000StopReason::running) {
        continue;
      }

      if (execution.reason == sf::psx::R3000StopReason::syscall) {
        const auto function = runtime.state().gpr[4];
        if (bios.handleKernelSyscall()) {
          std::cout << "HLE syscall function=" << function << '\n';
          continue;
        }
      }
      std::cout << "Guest stopped after " << instructions
                << " instructions: " << sf::psx::toString(execution.reason)
                << '\n';
      const auto old_flags = std::cout.flags();
      const auto old_fill = std::cout.fill();
      std::cout << std::hex << std::setfill('0')
                << "stop-pc=0x" << std::setw(8) << execution.pc
                << " instruction=0x" << std::setw(8)
                << execution.instruction << '\n';
      std::cout.flags(old_flags);
      std::cout.fill(old_fill);
      printCpuState(runtime);
      return execution.reason == sf::psx::R3000StopReason::instruction_budget
                 ? 0
                 : 3;
    }

    std::cout << "Instruction budget reached: " << instructions << '\n';
    printCpuState(runtime);
    std::cout << "IRQ dispatches: " << irq_dispatches << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mohu_boot_probe: " << error.what() << '\n';
    return 1;
  }
}
