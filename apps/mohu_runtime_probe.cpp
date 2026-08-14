#include "mohu/runtime.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv) {
  try {
    if (argc < 2 || argc > 4) {
      std::cerr << "Usage: mohu_runtime_probe <game.bin|game.cue> [frames] "
                   "[active-low-buttons]\n";
      return 64;
    }
    const auto frames = argc >= 3 ? std::stoull(argv[2]) : std::uint64_t{120U};
    const auto parsed_buttons =
        argc == 4 ? std::stoul(argv[3], nullptr, 0) : 0xffffUL;
    if (parsed_buttons > 0xffffUL) {
      throw std::out_of_range{"Controller button mask exceeds 16 bits"};
    }
    const auto active_low_buttons = static_cast<std::uint16_t>(parsed_buttons);
    auto runtime =
        mohu::Runtime{sf::game::GameDisc::open(std::filesystem::path{argv[1]})};
    auto disable_pgxp = false;
#if defined(_WIN32)
    char *disable_pgxp_value{};
    std::size_t disable_pgxp_length{};
    if (_dupenv_s(&disable_pgxp_value, &disable_pgxp_length,
                  "MOHU_PROBE_DISABLE_PGXP") == 0) {
      disable_pgxp = disable_pgxp_value != nullptr;
    }
    std::free(disable_pgxp_value);
#else
    disable_pgxp = std::getenv("MOHU_PROBE_DISABLE_PGXP") != nullptr;
#endif
    if (disable_pgxp) {
      static_cast<void>(const_cast<sf::psx::R3000Runtime &>(runtime.cpu())
                            .setPgxpTransformTracking(false));
    }
#if defined(_WIN32)
    char *disable_exact_value{};
    std::size_t disable_exact_length{};
    const auto disable_exact =
        _dupenv_s(&disable_exact_value, &disable_exact_length,
                  "MOHU_PROBE_DISABLE_EXACT") == 0 &&
        disable_exact_value != nullptr;
    std::free(disable_exact_value);
#else
    const auto disable_exact =
        std::getenv("MOHU_PROBE_DISABLE_EXACT") != nullptr;
#endif
    if (disable_exact) {
      auto &cpu = const_cast<sf::psx::R3000Runtime &>(runtime.cpu());
      cpu.setPgxpExactTransformCaptureEnabled(false);
      cpu.setPgxpExactTransformTracking(false);
    }
    for (std::uint64_t frame = 0U; frame < frames; ++frame) {
      const auto result = runtime.runFrame(active_low_buttons);
      if (!result.running()) {
        std::cerr << result.detail << '\n';
        return 3;
      }
    }
    const auto &stats = runtime.stats();
    const auto &display = runtime.gpuDisplayState();
    std::cout << "MOHU runtime smoke passed: frames=" << stats.frames
              << " instructions=" << stats.instructions
              << " bios=" << stats.bios_calls << " irq=" << stats.interrupts
              << " gpu_words=" << stats.gpu_words
              << " gp1_words=" << stats.gpu_control_words
              << " gpu_display=" << display.x << ',' << display.y << ','
              << display.width << 'x' << display.height
              << " gpu_enabled=" << display.enabled
              << " gpu_rgb24=" << display.rgb24
              << " gpu_interlaced=" << display.interlaced
              << " cd_searches=" << stats.disc_search_attempts
              << " cd_search_fail=" << stats.disc_search_failures
              << " cd_search_ok=" << stats.disc_search_successes
              << " cd_search_exhausted=" << stats.disc_search_exhausted
              << " cd_search_high=" << stats.disc_search_high_water
              << " cd_search_last=" << stats.last_disc_search_result
              << " cd_dirs=" << stats.disc_directory_scans
              << " cd_dir_extent=" << stats.last_directory_extent
              << " cd_dir_sectors=" << stats.last_directory_sectors
              << " cd_dir_max=" << stats.max_directory_sectors << " pc=0x"
              << std::hex << std::setw(8) << std::setfill('0')
              << runtime.cpu().state().pc << " epc=0x" << std::setw(8)
              << runtime.cpu().state().cop0_epc << " ra=0x" << std::setw(8)
              << runtime.cpu().state().gpr[31] << " v0=0x" << std::setw(8)
              << runtime.cpu().state().gpr[2] << " a2=0x" << std::setw(8)
              << runtime.cpu().state().gpr[6] << " a3=0x" << std::setw(8)
              << runtime.cpu().state().gpr[7] << std::dec;
    std::uint32_t stack_return{};
    if (runtime.cpu().read32(runtime.cpu().state().gpr[29U] + 0x78U,
                             stack_return)) {
      std::cout << " stack_ra=0x" << std::hex << stack_return << std::dec;
    }
    const auto cd = runtime.machine().cdrom().captureState();
    std::cout << " cd_lba=" << cd.current_lba
              << " cd_sectors=" << cd.sectors_read << " cd_mode=0x" << std::hex
              << static_cast<unsigned>(cd.mode)
              << " cd_reading=" << static_cast<unsigned>(cd.reading)
              << " cd_irq=" << static_cast<unsigned>(cd.interrupt_flags)
              << " cd_cmd=0x" << std::hex
              << static_cast<unsigned>(cd.pending_command)
              << " cd_params=" << std::dec
              << static_cast<unsigned>(cd.parameter_count) << " cd_response=0x"
              << std::hex << static_cast<unsigned>(cd.response[0]) << ",0x"
              << static_cast<unsigned>(cd.response[1]) << " istat=0x"
              << std::hex << runtime.machine().interrupts().status()
              << " imask=0x" << runtime.machine().interrupts().mask();
    constexpr std::array traced_cd_commands{0x01U, 0x02U, 0x06U, 0x09U, 0x0aU,
                                            0x0eU, 0x13U, 0x14U, 0x15U, 0x1bU};
    for (const auto command : traced_cd_commands) {
      std::cout << " cd_c" << std::setw(2) << std::setfill('0') << command
                << '=' << std::dec << cd.command_counts[command] << std::hex;
    }
    std::cout << " cd_recent=";
    const auto oldest = static_cast<std::size_t>(
        (cd.recent_lba_cursor + cd.recent_lbas.size() - cd.recent_lba_count) %
        cd.recent_lbas.size());
    for (std::size_t index = 0U; index < cd.recent_lba_count; ++index) {
      std::cout << (index == 0U ? "" : ",") << std::dec
                << cd.recent_lbas[(oldest + index) % cd.recent_lbas.size()];
    }
    std::cout << std::hex;
    std::cout << " dpcr=0x" << runtime.machine().dma().dpcr() << " gpu_chcr=0x"
              << runtime.machine().dma().chcr(sf::psx::DmaChannel::gpu)
              << " gpu_madr=0x"
              << runtime.machine().dma().madr(sf::psx::DmaChannel::gpu)
              << " mdec_in_chcr=0x"
              << runtime.machine().dma().chcr(sf::psx::DmaChannel::mdec_in)
              << " mdec_out_chcr=0x"
              << runtime.machine().dma().chcr(sf::psx::DmaChannel::mdec_out);
    for (std::size_t priority = 0U;
         priority < runtime.biosState().interrupt_routines.size(); ++priority) {
      const auto node = runtime.biosState().interrupt_routines[priority];
      if (node != 0U) {
        std::uint32_t handler{};
        static_cast<void>(runtime.cpu().read32(node + 4U, handler));
        std::cout << " irq_node" << priority << "=0x" << node
                  << " irq_handler=0x" << handler;
      }
    }
    constexpr std::uint32_t interrupt_callbacks = 0x8002f854U;
    for (std::uint32_t source = 0U; source < 11U; ++source) {
      std::uint32_t callback{};
      if (runtime.cpu().read32(interrupt_callbacks + source * 4U, callback) &&
          callback != 0U) {
        std::cout << " irq_cb" << source << "=0x" << callback;
      }
    }
    const auto print_guest_byte = [&](const char *name, std::uint32_t address) {
      std::uint8_t value{};
      if (runtime.cpu().read8(address, value)) {
        std::cout << ' ' << name << "=0x" << static_cast<unsigned>(value);
      }
    };
    print_guest_byte("cd_driver_cmd", 0x800366e1U);
    print_guest_byte("cd_driver_sync", 0x800369a0U);
    print_guest_byte("cd_driver_ready", 0x800369a1U);
    print_guest_byte("cd_driver_end", 0x800369a2U);
    const auto &sio = runtime.machine().controllerSio();
    std::cout << " sio_mode=0x" << std::hex << sio.mode << " sio_ctrl=0x"
              << sio.control << " sio_baud=0x" << sio.baud
              << " sio_phase=" << std::dec << static_cast<unsigned>(sio.phase)
              << " sio_cmd=0x" << std::hex << static_cast<unsigned>(sio.command)
              << " sio_response=0x" << static_cast<unsigned>(sio.response)
              << " sio_rx=" << std::dec << sio.rx_ready
              << " sio_ack=" << sio.ack_input << " sio_irq=" << sio.irq_pending
              << " sio_transfer=" << sio.transfer_token
              << " sio_release=" << sio.ack_release_token;
    constexpr std::uint32_t pad_zero_buffer = 0x8003a65cU;
    std::cout << " pad0=" << std::hex << std::setfill('0');
    for (std::uint32_t offset = 0U; offset < 9U; ++offset) {
      std::uint8_t byte{};
      std::cout << (offset == 0U ? "" : ",");
      if (runtime.cpu().read8(pad_zero_buffer + offset, byte)) {
        std::cout << std::setw(2) << static_cast<unsigned>(byte);
      } else {
        std::cout << "??";
      }
    }
    const auto frame_commands = runtime.gpuCommands();
    std::cout << " frame_gp0_words=" << std::dec << frame_commands.size();
    auto frame_word_index = std::size_t{};
    for (const auto word : frame_commands) {
      if (frame_word_index++ >= 64U) {
        break;
      }
      std::cout << " frame_gp0=0x" << std::hex << word;
    }
    for (const auto word : runtime.firstGpuWords()) {
      std::cout << " gp0=0x" << std::hex << word;
    }
    std::cout << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mohu_runtime_probe: " << error.what() << '\n';
    return 1;
  }
}
