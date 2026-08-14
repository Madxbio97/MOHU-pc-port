#include "sf/psx/machine.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace sf::psx {
namespace {

constexpr std::uint32_t interrupt_base = 0x1f801070U;
constexpr std::uint32_t dma_base = 0x1f801080U;
constexpr std::uint32_t timer_base = 0x1f801100U;
constexpr std::uint32_t sio_base = 0x1f801040U;
constexpr std::uint32_t cdrom_base = 0x1f801800U;
constexpr std::uint32_t gpu_base = 0x1f801810U;
constexpr std::uint32_t mdec_base = 0x1f801820U;
constexpr std::uint32_t spu_base = 0x1f801c00U;
constexpr std::uint32_t default_gpu_status = 0x14802000U;
constexpr std::uint32_t ram_address_mask =
    static_cast<std::uint32_t>(R3000Runtime::ram_size - 1U);
constexpr std::uint64_t maximum_dma_words = 16U * 1024U * 1024U;
constexpr std::uint64_t maximum_linked_list_nodes = 65'536U;
constexpr std::size_t spu_control_register_index = 0x1aaU / 2U;
// Scheduled deadlines and MMIO flush exactly. This fallback only bounds
// free-running device advancement when no earlier event is pending.
constexpr std::uint64_t maximum_cpu_slice_ticks = 4'096U;
// Standard SIO0 settings (MODE=000d, BAUD=0088) clock one eight-bit
// controller byte in 1,088 CPU clocks. The pad then holds /ACK low briefly.
constexpr std::uint64_t controller_transfer_ticks = 1'088U;
constexpr std::uint64_t controller_ack_pulse_ticks = 100U;

std::uint32_t accessBits(R3000AccessWidth width) noexcept {
  switch (width) {
  case R3000AccessWidth::byte:
    return 8U;
  case R3000AccessWidth::halfword:
    return 16U;
  case R3000AccessWidth::word:
    return 32U;
  }
  return 0U;
}

std::uint32_t laneShift(std::uint32_t address) noexcept {
  return (address & 3U) * 8U;
}

std::uint32_t laneMask(std::uint32_t address, R3000AccessWidth width) noexcept {
  const auto bits = accessBits(width);
  if (bits == 32U) {
    return 0xffffffffU;
  }
  return ((1U << bits) - 1U) << laneShift(address);
}

std::uint32_t placeLane(std::uint32_t address, R3000AccessWidth width,
                        std::uint32_t value) noexcept {
  return (value << laneShift(address)) & laneMask(address, width);
}

std::uint32_t extractLane(std::uint32_t address, R3000AccessWidth width,
                          std::uint32_t value) noexcept {
  const auto bits = accessBits(width);
  const auto shifted = value >> laneShift(address);
  return bits == 32U ? shifted : shifted & ((1U << bits) - 1U);
}

std::size_t channelIndex(DmaChannel channel) noexcept {
  return static_cast<std::size_t>(channel);
}

DmaChannel channelFromIndex(std::size_t index) noexcept {
  return static_cast<DmaChannel>(static_cast<std::uint8_t>(index));
}

bool spuInterruptLine(const SpuState &state) noexcept {
  constexpr std::uint16_t irq_enable = 1U << 6U;
  return state.irq_latched != 0U &&
         (state.registers[spu_control_register_index] & irq_enable) != 0U;
}

} // namespace

PsxMachine::PsxMachine(R3000Runtime &cpu, CpuClockScale cpu_clock_scale)
    : cpu_(cpu), cpu_clock_scale_(cpu_clock_scale) {
  if (!cpu_clock_scale_.valid()) {
    throw std::invalid_argument{"Invalid PSX CPU clock scale"};
  }
  const auto divisor =
      std::gcd(cpu_clock_scale_.numerator, cpu_clock_scale_.denominator);
  cpu_clock_scale_.numerator /= divisor;
  cpu_clock_scale_.denominator /= divisor;
  cpu_.attachMmioBus(this);
  cdrom_.setXaAudioSink(this);
  dma_ports_[channelIndex(DmaChannel::mdec_in)] = &mdec_input_dma_port_;
  dma_ports_[channelIndex(DmaChannel::mdec_out)] = &mdec_output_dma_port_;
  dma_ports_[channelIndex(DmaChannel::spu)] = &spu_dma_port_;
  reset();
}

PsxMachine::~PsxMachine() { cpu_.attachMmioBus(nullptr); }

void PsxMachine::reset() noexcept {
  pending_cpu_ticks_ = 0U;
  controller_sio_ = {};
  device_tick_remainder_ = 0U;
  scheduler_.reset();
  interrupts_.reset();
  dma_.reset();
  cdrom_.reset();
  mdec_.reset();
  spu_.reset();
  xa_decoder_.reset();
  timers_.reset();
  refreshCpuSliceLimit();
  syncCpuInterruptLine();
}

R3000RunResult PsxMachine::step() noexcept {
  auto result = cpu_.step();
  if (result.reason == R3000StopReason::running) {
    ++pending_cpu_ticks_;
    if (cpu_ticks_until_flush_ <= 1U) {
      flushPendingCpuTicks();
    } else {
      --cpu_ticks_until_flush_;
    }
  }
  return result;
}

void PsxMachine::advanceTicks(std::uint64_t ticks) noexcept {
  flushPendingCpuTicks();
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto target =
      ticks > maximum - scheduler_.now() ? maximum : scheduler_.now() + ticks;

  MachineEvent event;
  while (scheduler_.popNextDue(target, event)) {
    advanceDevicesTo(event.deadline);
    dispatchEvent(event);
  }
  advanceDevicesTo(target);
  syncCpuInterruptLine();
  refreshCpuSliceLimit();
}

std::uint64_t PsxMachine::currentTick() const noexcept {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  return pending_cpu_ticks_ > maximum - scheduler_.now()
             ? maximum
             : scheduler_.now() + pending_cpu_ticks_;
}

std::uint64_t PsxMachine::cpuTicksPerSecond() const noexcept {
  return scaleDeviceTicks(cpu_clock_hz);
}

std::optional<std::uint64_t>
PsxMachine::dmaCompletionTick(DmaChannel channel) const noexcept {
  const auto token = dma_.scheduledToken(channel);
  if (token == 0U) {
    return std::nullopt;
  }
  const auto scheduler = scheduler_.captureState();
  const auto index = channelIndex(channel);
  for (std::size_t event_index = 0U; event_index < scheduler.event_count;
       ++event_index) {
    const auto &event = scheduler.events[event_index];
    if (event.type == MachineEventType::dma_complete && event.index == index &&
        event.token == token) {
      return event.deadline;
    }
  }
  return std::nullopt;
}

bool PsxMachine::completePendingDmaTransfer(DmaChannel channel) noexcept {
  const auto token = dma_.scheduledToken(channel);
  if (token == 0U) {
    return true;
  }
  if (!scheduler_.cancel(token)) {
    return false;
  }
  dispatchEvent(MachineEvent{
      .deadline = scheduler_.now(),
      .token = token,
      .payload = 0U,
      .type = MachineEventType::dma_complete,
      .index = static_cast<std::uint8_t>(channelIndex(channel)),
  });
  return dma_.scheduledToken(channel) != token;
}

bool PsxMachine::completePendingDmaTransfers() noexcept {
  // One completion can make a different channel startable, so allow a bounded
  // number of passes. DMA completion itself clears CHCR busy and therefore
  // cannot reschedule the same request indefinitely.
  for (std::size_t pass = 0U; pass < DmaController::channel_count; ++pass) {
    auto completed_any = false;
    for (std::size_t index = 0U; index < DmaController::channel_count;
         ++index) {
      const auto channel = channelFromIndex(index);
      if (dma_.scheduledToken(channel) == 0U) {
        continue;
      }
      if (!completePendingDmaTransfer(channel)) {
        return false;
      }
      completed_any = true;
    }
    if (!completed_any) {
      return true;
    }
  }

  for (std::size_t index = 0U; index < DmaController::channel_count; ++index) {
    if (dma_.scheduledToken(channelFromIndex(index)) != 0U) {
      return false;
    }
  }
  return true;
}

bool PsxMachine::completeNextPendingCdRomEvent() noexcept {
  const auto command = cdrom_.commandSchedule();
  const auto sector = cdrom_.sectorSchedule();
  const auto state = scheduler_.captureState();
  for (std::size_t index = 0U; index < state.event_count; ++index) {
    auto event = state.events[index];
    const auto current_command =
        event.type == MachineEventType::cdrom_command && event.index == 0U &&
        command.pending != 0U && event.payload == command.generation;
    const auto current_sector = event.type == MachineEventType::cdrom_sector &&
                                event.index == 0U && sector.pending != 0U &&
                                event.payload == sector.generation;
    if (!current_command && !current_sector) {
      continue;
    }
    if (!scheduler_.cancel(event.token)) {
      return false;
    }
    event.deadline = scheduler_.now();
    dispatchEvent(event);
    return true;
  }
  return false;
}

void PsxMachine::attachDmaPort(DmaChannel channel, DmaPort *port) noexcept {
  const auto index = channelIndex(channel);
  if (index >= dma_ports_.size()) {
    return;
  }
  if (port == nullptr) {
    const auto token = dma_.scheduledToken(channel);
    if (token != 0U) {
      static_cast<void>(scheduler_.cancel(token));
      static_cast<void>(dma_.cancelScheduled(channel, token));
      if (channel == DmaChannel::spu) {
        spu_.setDmaTransferBusy(false);
      }
    }
  }
  dma_ports_[index] = port;
  kickDmaChannels();
}

void PsxMachine::attachGpuPort(GpuPort *port) noexcept {
  gpu_port_ = port;
  attachDmaPort(DmaChannel::gpu, port);
}

void PsxMachine::setCdRomMedia(CdRomMedia *media) noexcept {
  cdrom_.setMedia(media);
  syncCdRomSchedules();
  syncCdRomInterruptLine();
  kickDmaChannels();
}

void PsxMachine::serviceDmaRequests() noexcept { kickDmaChannels(); }

void PsxMachine::setVBlank(bool active) noexcept {
  routeTimerInterrupts(timers_.setVBlank(active));
  interrupts_.setLine(InterruptSource::vblank, active);
  syncCpuInterruptLine();
}

void PsxMachine::pulseVBlank() noexcept {
  setVBlank(false);
  setVBlank(true);
  setVBlank(false);
}

void PsxMachine::setHBlank(bool active) noexcept {
  routeTimerInterrupts(timers_.setHBlank(active));
  syncCpuInterruptLine();
}

void PsxMachine::advanceDotClocks(std::uint64_t clocks) noexcept {
  routeTimerInterrupts(timers_.advanceDotClocks(clocks));
  syncCpuInterruptLine();
}

PsxMachineState PsxMachine::captureState() const {
  PsxMachineState state;
  state.cpu_clock_scale = cpu_clock_scale_;
  state.controller_sio = controller_sio_;
  state.scheduler = scheduler_.captureState();
  state.interrupts = interrupts_.captureState();
  state.dma = dma_.captureState();
  state.cdrom = cdrom_.captureState();
  state.mdec = mdec_.captureState();
  *state.spu = spu_.state();
  state.xa_decoder = xa_decoder_.captureState();
  state.timers = timers_.snapshot();
  state.pending_cpu_ticks = pending_cpu_ticks_;
  state.device_tick_remainder = device_tick_remainder_;
  return state;
}

bool PsxMachine::validateState(const PsxMachineState &state) const noexcept {
  EventScheduler scheduler;
  InterruptController interrupts;
  DmaController dma;
  CdRomController cdrom{cdrom_.media()};
  if (state.cpu_clock_scale != cpu_clock_scale_ ||
      state.pending_cpu_ticks >= maximum_cpu_slice_ticks ||
      state.device_tick_remainder >= cpu_clock_scale_.numerator ||
      !scheduler.restoreState(state.scheduler) ||
      !interrupts.restoreState(state.interrupts) ||
      !dma.restoreState(state.dma) || !cdrom.restoreState(state.cdrom) ||
      !mdec_.validateState(state.mdec) || state.spu == nullptr ||
      !spu_.validateState(*state.spu) ||
      !xa_decoder_.validateState(state.xa_decoder)) {
    return false;
  }

  std::array<bool, DmaController::channel_count> found_tokens{};
  bool found_cdrom_command{};
  bool found_cdrom_sector{};
  bool found_controller_transfer{};
  bool found_controller_ack_release{};
  for (std::size_t event_index = 0U; event_index < state.scheduler.event_count;
       ++event_index) {
    const auto &event = state.scheduler.events[event_index];
    const auto logical_now =
        state.pending_cpu_ticks >
                std::numeric_limits<std::uint64_t>::max() - state.scheduler.now
            ? std::numeric_limits<std::uint64_t>::max()
            : state.scheduler.now + state.pending_cpu_ticks;
    if (event.deadline <= logical_now) {
      return false;
    }
    if (event.type == MachineEventType::dma_complete) {
      if (event.index >= DmaController::channel_count || event.payload != 0U) {
        return false;
      }
      const auto channel = channelFromIndex(event.index);
      if (dma.scheduledToken(channel) != event.token ||
          found_tokens[event.index]) {
        return false;
      }
      found_tokens[event.index] = true;
      continue;
    }
    if (event.type == MachineEventType::controller_transfer) {
      if (event.index > 1U) {
        return false;
      }
      const auto delay =
          scaleDeviceTicks(event.index == 0U ? controller_transfer_ticks
                                             : controller_ack_pulse_ticks);
      const auto latest_deadline =
          delay > std::numeric_limits<std::uint64_t>::max() -
                      state.scheduler.now
              ? std::numeric_limits<std::uint64_t>::max()
              : state.scheduler.now + delay;
      if (event.deadline > latest_deadline) {
        return false;
      }
      if (event.index == 0U) {
        if (found_controller_transfer ||
            state.controller_sio.transfer_token != event.token ||
            event.payload != static_cast<std::uint64_t>(
                                 state.controller_sio.transfer_acknowledged)) {
          return false;
        }
        found_controller_transfer = true;
      } else if (event.index == 1U) {
        if (found_controller_ack_release ||
            state.controller_sio.ack_release_token != event.token ||
            event.payload != 0U || !state.controller_sio.ack_input) {
          return false;
        }
        found_controller_ack_release = true;
      } else {
        return false;
      }
      continue;
    }
    if (event.index != 0U) {
      return false;
    }
    if (event.type == MachineEventType::cdrom_command) {
      const auto request = cdrom.commandSchedule();
      const auto delay = scaleDeviceTicks(request.delay_ticks);
      const auto latest_deadline =
          delay > std::numeric_limits<std::uint64_t>::max() -
                      state.scheduler.now
              ? std::numeric_limits<std::uint64_t>::max()
              : state.scheduler.now + delay;
      if (found_cdrom_command || request.pending == 0U ||
          request.generation != event.payload ||
          event.deadline <= state.scheduler.now ||
          event.deadline > latest_deadline) {
        return false;
      }
      found_cdrom_command = true;
      continue;
    }
    if (event.type == MachineEventType::cdrom_sector) {
      const auto request = cdrom.sectorSchedule();
      const auto delay = scaleDeviceTicks(request.delay_ticks);
      const auto latest_deadline =
          delay > std::numeric_limits<std::uint64_t>::max() -
                      state.scheduler.now
              ? std::numeric_limits<std::uint64_t>::max()
              : state.scheduler.now + delay;
      if (found_cdrom_sector || request.pending == 0U ||
          request.generation != event.payload ||
          event.deadline <= state.scheduler.now ||
          event.deadline > latest_deadline) {
        return false;
      }
      found_cdrom_sector = true;
      continue;
    }
    return false;
  }
  if ((cdrom.commandSchedule().pending != 0U) != found_cdrom_command ||
      (cdrom.sectorSchedule().pending != 0U) != found_cdrom_sector) {
    return false;
  }
  if ((state.controller_sio.transfer_token != 0U) !=
          found_controller_transfer ||
      (state.controller_sio.ack_release_token != 0U) !=
          found_controller_ack_release ||
      state.controller_sio.ack_input != found_controller_ack_release ||
      (state.controller_sio.transfer_token == 0U &&
       state.controller_sio.transfer_acknowledged) ||
      (state.controller_sio.transfer_token != 0U &&
       state.controller_sio.rx_ready)) {
    return false;
  }
  for (std::size_t index = 0U; index < DmaController::channel_count; ++index) {
    const auto token = dma.scheduledToken(channelFromIndex(index));
    if ((token != 0U) != found_tokens[index]) {
      return false;
    }
  }
  if ((state.spu->transfer_busy != 0U) !=
      found_tokens[channelIndex(DmaChannel::spu)]) {
    return false;
  }
  if (state.spu->cd_input_matrix != state.cdrom.cd_volume_matrix) {
    return false;
  }

  const auto dma_bit = static_cast<std::uint16_t>(
      1U << static_cast<std::uint8_t>(InterruptSource::dma));
  if (((interrupts.inputLines() & dma_bit) != 0U) != dma.interruptLine()) {
    return false;
  }
  const auto cdrom_bit = static_cast<std::uint16_t>(
      1U << static_cast<std::uint8_t>(InterruptSource::cdrom));
  if (((interrupts.inputLines() & cdrom_bit) != 0U) != cdrom.interruptLine()) {
    return false;
  }
  const auto spu_bit = static_cast<std::uint16_t>(
      1U << static_cast<std::uint8_t>(InterruptSource::spu));
  if (((interrupts.inputLines() & spu_bit) != 0U) !=
      spuInterruptLine(*state.spu)) {
    return false;
  }

  return true;
}

bool PsxMachine::restoreState(const PsxMachineState &state) noexcept {
  if (!validateState(state)) {
    return false;
  }

  static_cast<void>(scheduler_.restoreState(state.scheduler));
  static_cast<void>(interrupts_.restoreState(state.interrupts));
  static_cast<void>(dma_.restoreState(state.dma));
  controller_sio_ = state.controller_sio;
  static_cast<void>(cdrom_.restoreState(state.cdrom));
  static_cast<void>(mdec_.restoreState(state.mdec));
  static_cast<void>(spu_.restoreState(*state.spu));
  static_cast<void>(xa_decoder_.restoreState(state.xa_decoder));
  timers_.restoreState(state.timers);
  pending_cpu_ticks_ = state.pending_cpu_ticks;
  refreshCpuSliceLimit();
  device_tick_remainder_ = state.device_tick_remainder;
  syncCpuInterruptLine();
  return true;
}

void PsxMachine::consumeXaSector(
    std::span<const std::byte, CdRomMedia::raw_sector_size> sector,
    bool muted) noexcept {
  // DuckStation/hardware-compatible whole-sector admission. Decoding a sector
  // while the previous one is still buffered mutates XA predictor and
  // interpolation history even though its PCM cannot be consumed; that is a
  // direct source of recurring clicks and broken speech/music continuity.
  constexpr std::size_t xa_fifo_low_watermark = 10U;
  if (spu_.queuedCdFrames() > xa_fifo_low_watermark) {
    return;
  }
  const auto decoded = xa_decoder_.decodeSector(sector, xa_decode_scratch_);
  if (!decoded.succeeded() || muted) {
    return;
  }
  static_cast<void>(
      spu_.pushCdAudio(std::span<const SpuPcmFrame>{xa_decode_scratch_}.first(
          decoded.frames_written)));
}

void PsxMachine::resetXaStream() noexcept {
  xa_decoder_.reset();
  spu_.clearCdAudio();
}

void PsxMachine::setXaOutputMixer(
    std::array<std::uint8_t, 4U> matrix) noexcept {
  spu_.setCdInputMixer(matrix);
}

bool PsxMachine::readMmio(std::uint32_t physical_address,
                          R3000AccessWidth width,
                          std::uint32_t &value) noexcept {
  if (physical_address >= sio_base && physical_address < sio_base + 0x10U) {
    const auto aligned = physical_address & ~3U;
    if (aligned == sio_base) {
      if (physical_address != sio_base || width != R3000AccessWidth::byte) {
        return false;
      }
      value = controller_sio_.response;
      controller_sio_.rx_ready = false;
      return true;
    }

    std::uint32_t register_value{};
    if (aligned == sio_base + 4U) {
      const auto transmitter_ready = controller_sio_.transfer_token == 0U;
      register_value = (transmitter_ready ? 0x0005U : 0U) |
                       (controller_sio_.rx_ready ? 0x0002U : 0U) |
                       (controller_sio_.ack_input ? 0x0080U : 0U) |
                       (controller_sio_.irq_pending ? 0x0200U : 0U);
    } else if (aligned == sio_base + 8U) {
      register_value =
          controller_sio_.mode |
          (static_cast<std::uint32_t>(controller_sio_.control) << 16U);
    } else if (aligned == sio_base + 0x0cU) {
      register_value = static_cast<std::uint32_t>(controller_sio_.baud) << 16U;
    } else {
      return false;
    }
    value = extractLane(physical_address, width, register_value);
    return true;
  }

  if ((physical_address == gpu_base || physical_address == gpu_base + 4U) &&
      width == R3000AccessWidth::word) {
    if (physical_address == gpu_base) {
      if (gpu_port_ == nullptr || !gpu_port_->readGp0(value)) {
        value = 0U;
      }
    } else {
      value =
          gpu_port_ != nullptr ? gpu_port_->readStatus() : default_gpu_status;
    }
    return true;
  }

  if ((physical_address == mdec_base || physical_address == mdec_base + 4U) &&
      width == R3000AccessWidth::word) {
    flushPendingCpuTicks();
    if (physical_address == mdec_base) {
      if (!mdec_.readData(value)) {
        value = 0U;
      }
    } else {
      value = mdec_.status();
    }
    kickDmaChannels();
    return true;
  }

  flushPendingCpuTicks();
  if (physical_address >= spu_base &&
      physical_address < spu_base + Spu::register_span) {
    const auto byte_count = accessBits(width) / 8U;
    const auto first_offset = physical_address - spu_base;
    if (first_offset + byte_count > Spu::register_span) {
      return false;
    }
    value = 0U;
    for (std::uint32_t index = 0U; index < byte_count; ++index) {
      const auto byte_offset = first_offset + index;
      std::uint16_t halfword{};
      if (!spu_.readRegister(byte_offset & ~1U, halfword)) {
        return false;
      }
      const auto byte =
          static_cast<std::uint8_t>(halfword >> ((byte_offset & 1U) * 8U));
      value |= static_cast<std::uint32_t>(byte) << (index * 8U);
    }
    syncSpuInterruptLine();
    return true;
  }

  if (physical_address >= cdrom_base &&
      physical_address < cdrom_base + CdRomController::register_span) {
    const auto byte_count = accessBits(width) / 8U;
    const auto first_offset = physical_address - cdrom_base;
    const auto fifo_read = first_offset == 1U || first_offset == 2U;
    if (!fifo_read &&
        first_offset + byte_count > CdRomController::register_span) {
      return false;
    }
    value = 0U;
    for (std::uint32_t index = 0U; index < byte_count; ++index) {
      const auto offset = fifo_read ? first_offset : first_offset + index;
      std::uint8_t byte{};
      if (offset >= CdRomController::register_span ||
          !cdrom_.readRegister(offset, byte)) {
        return false;
      }
      value |= static_cast<std::uint32_t>(byte) << (index * 8U);
    }
    syncCdRomSchedules();
    syncCdRomInterruptLine();
    kickDmaChannels();
    return true;
  }

  const auto aligned = physical_address & ~3U;
  std::uint32_t register_value{};

  if (aligned == interrupt_base) {
    register_value = interrupts_.status();
  } else if (aligned == interrupt_base + 4U) {
    register_value = interrupts_.mask();
  } else if (aligned >= dma_base &&
             aligned < dma_base + DmaController::register_span) {
    if (!dma_.readRegister(aligned - dma_base, register_value)) {
      return false;
    }
  } else if (aligned >= timer_base && aligned < timer_base + 0x30U) {
    const auto timer_offset = aligned - timer_base;
    const auto channel = static_cast<std::size_t>(timer_offset / 0x10U);
    switch (timer_offset & 0x0fU) {
    case 0x00U:
      register_value = timers_.readCounter(channel);
      break;
    case 0x04U:
      register_value = timers_.readMode(channel);
      break;
    case 0x08U:
      register_value = timers_.readTarget(channel);
      break;
    default:
      return false;
    }
  } else {
    return false;
  }

  value = extractLane(physical_address, width, register_value);
  return true;
}

bool PsxMachine::writeMmio(std::uint32_t physical_address,
                           R3000AccessWidth width, std::uint32_t value,
                           const GteProjectedVertex *projected,
                           std::uint64_t projection_identity) noexcept {
  flushPendingCpuTicks();
  if ((physical_address == gpu_base || physical_address == gpu_base + 4U) &&
      width == R3000AccessWidth::word) {
    if (gpu_port_ == nullptr) {
      return true;
    }
    if (physical_address == gpu_base) {
      return gpu_port_->writeGp0Projected(value, projected,
                                          projection_identity);
    }
    gpu_port_->writeGp1(value);
    return true;
  }

  if ((physical_address == mdec_base || physical_address == mdec_base + 4U) &&
      width == R3000AccessWidth::word) {
    if (physical_address == mdec_base) {
      if (!mdec_.writeData(value)) {
        return false;
      }
    } else {
      mdec_.writeControl(value);
    }
    kickDmaChannels();
    return true;
  }

  if (physical_address >= sio_base && physical_address < sio_base + 0x10U) {
    const auto aligned = physical_address & ~3U;
    const auto write_mask = laneMask(physical_address, width);
    const auto placed_value = placeLane(physical_address, width, value);
    if (aligned == sio_base) {
      if (physical_address != sio_base) {
        return false;
      }
      ++controller_sio_.data_writes;
      if (controller_sio_.transfer_token != 0U) {
        ++controller_sio_.ignored_busy_writes;
        return true;
      }
      const auto port_one_selected =
          (controller_sio_.control & 0x2003U) == 0x0003U;
      auto acknowledged = false;
      if (port_one_selected) {
        ++controller_sio_.port_one_bytes;
        acknowledged = writeControllerByte(static_cast<std::uint8_t>(value));
      } else {
        controller_sio_.phase = 0U;
        controller_sio_.command = 0U;
        controller_sio_.packet_id = 0U;
        controller_sio_.command_parameter = 0U;
        controller_sio_.response = 0xffU;
      }
      scheduleControllerTransfer(acknowledged);
      return true;
    }
    if (aligned == sio_base + 4U) {
      return true;
    }
    if (aligned == sio_base + 8U) {
      ++controller_sio_.control_writes;
      const auto previous =
          static_cast<std::uint32_t>(controller_sio_.mode) |
          (static_cast<std::uint32_t>(controller_sio_.control) << 16U);
      const auto merged =
          (previous & ~write_mask) | (placed_value & write_mask);
      controller_sio_.mode = static_cast<std::uint16_t>(merged);
      const auto previous_control = static_cast<std::uint16_t>(previous >> 16U);
      const auto requested_control = static_cast<std::uint16_t>(merged >> 16U);
      const auto acknowledge_irq = (requested_control & 0x0010U) != 0U;
      const auto reset_requested = (requested_control & 0x0040U) != 0U;
      if (reset_requested) {
        ++controller_sio_.reset_writes;
      }
      const auto port_changed =
          ((previous_control ^ requested_control) & 0x2000U) != 0U;
      // ACK and RESET are write strobes and therefore never read back as set.
      controller_sio_.control =
          static_cast<std::uint16_t>(requested_control & ~0x0050U);
      if (acknowledge_irq && !controller_sio_.ack_input) {
        controller_sio_.irq_pending = false;
      }
      if ((controller_sio_.control & 0x0002U) == 0U || reset_requested ||
          port_changed) {
        cancelControllerTransfer();
        cancelControllerAckRelease();
        controller_sio_.phase = 0U;
        controller_sio_.command = 0U;
        controller_sio_.packet_id = 0U;
        controller_sio_.command_parameter = 0U;
        controller_sio_.response = 0xffU;
        controller_sio_.rx_ready = false;
      }
      if (reset_requested) {
        controller_sio_.irq_pending = false;
      }
      syncCpuInterruptLine();
      return true;
    }
    if (aligned == sio_base + 0x0cU) {
      const auto previous = static_cast<std::uint32_t>(controller_sio_.baud)
                            << 16U;
      const auto merged =
          (previous & ~write_mask) | (placed_value & write_mask);
      controller_sio_.baud = static_cast<std::uint16_t>(merged >> 16U);
      return true;
    }
    return false;
  }

  if (physical_address >= spu_base &&
      physical_address < spu_base + Spu::register_span) {
    const auto byte_count = accessBits(width) / 8U;
    const auto first_offset = physical_address - spu_base;
    if (first_offset + byte_count > Spu::register_span) {
      return false;
    }

    if (width == R3000AccessWidth::byte) {
      const auto register_offset = first_offset & ~1U;
      std::uint16_t previous{};
      if (!spu_.readRegister(register_offset, previous)) {
        return false;
      }
      const auto shift = (first_offset & 1U) * 8U;
      const auto mask = static_cast<std::uint16_t>(0xffU << shift);
      const auto merged = static_cast<std::uint16_t>(
          (previous & ~mask) |
          ((static_cast<std::uint16_t>(value) << shift) & mask));
      if (!spu_.writeRegister(register_offset, merged)) {
        return false;
      }
    } else {
      if ((first_offset & 1U) != 0U ||
          !spu_.writeRegister(first_offset,
                              static_cast<std::uint16_t>(value))) {
        return false;
      }
      if (width == R3000AccessWidth::word &&
          !spu_.writeRegister(first_offset + 2U,
                              static_cast<std::uint16_t>(value >> 16U))) {
        return false;
      }
    }

    syncSpuInterruptLine();
    kickDmaChannels();
    syncCpuInterruptLine();
    return true;
  }

  if (physical_address >= cdrom_base &&
      physical_address < cdrom_base + CdRomController::register_span) {
    const auto byte_count = accessBits(width) / 8U;
    const auto first_offset = physical_address - cdrom_base;
    const auto parameter_write = first_offset == 2U;
    if (!parameter_write &&
        first_offset + byte_count > CdRomController::register_span) {
      return false;
    }
    for (std::uint32_t index = 0U; index < byte_count; ++index) {
      const auto offset = parameter_write ? first_offset : first_offset + index;
      if (offset >= CdRomController::register_span ||
          !cdrom_.writeRegister(
              offset, static_cast<std::uint8_t>(value >> (index * 8U)))) {
        return false;
      }
    }
    syncCdRomSchedules();
    syncCdRomInterruptLine();
    kickDmaChannels();
    syncCpuInterruptLine();
    return true;
  }

  const auto aligned = physical_address & ~3U;
  const auto write_mask = laneMask(physical_address, width);
  const auto placed_value = placeLane(physical_address, width, value);

  if (aligned == interrupt_base) {
    interrupts_.acknowledge(static_cast<std::uint16_t>(placed_value),
                            static_cast<std::uint16_t>(write_mask));
  } else if (aligned == interrupt_base + 4U) {
    interrupts_.writeMask(static_cast<std::uint16_t>(placed_value),
                          static_cast<std::uint16_t>(write_mask));
  } else if (aligned >= dma_base &&
             aligned < dma_base + DmaController::register_span) {
    std::array<std::uint64_t, DmaController::channel_count> previous_tokens{};
    for (std::size_t index = 0U; index < previous_tokens.size(); ++index) {
      previous_tokens[index] = dma_.scheduledToken(channelFromIndex(index));
    }
    if (!dma_.writeRegister(aligned - dma_base, placed_value, write_mask)) {
      return false;
    }
    for (std::size_t index = 0U; index < previous_tokens.size(); ++index) {
      const auto token = previous_tokens[index];
      if (token != 0U &&
          dma_.scheduledToken(channelFromIndex(index)) != token) {
        static_cast<void>(scheduler_.cancel(token));
        if (channelFromIndex(index) == DmaChannel::spu) {
          spu_.setDmaTransferBusy(false);
        }
      }
    }
    syncDmaInterruptLine();
    kickDmaChannels();
  } else if (aligned >= timer_base && aligned < timer_base + 0x30U) {
    const auto timer_offset = aligned - timer_base;
    const auto channel = static_cast<std::size_t>(timer_offset / 0x10U);
    std::uint16_t previous{};
    switch (timer_offset & 0x0fU) {
    case 0x00U:
      previous = timers_.readCounter(channel);
      break;
    case 0x04U:
      previous = timers_.peekMode(channel);
      break;
    case 0x08U:
      previous = timers_.readTarget(channel);
      break;
    default:
      return false;
    }
    const auto merged = static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(previous) & ~write_mask) |
        (placed_value & write_mask));
    switch (timer_offset & 0x0fU) {
    case 0x00U:
      timers_.writeCounter(channel, merged);
      break;
    case 0x04U:
      timers_.writeMode(channel, merged);
      break;
    case 0x08U:
      timers_.writeTarget(channel, merged);
      break;
    default:
      return false;
    }
  } else {
    return false;
  }

  syncCpuInterruptLine();
  return true;
}

void PsxMachine::advanceDevicesTo(std::uint64_t tick) noexcept {
  const auto elapsed = unscaleCpuTicks(tick - scheduler_.now());
  routeTimerInterrupts(timers_.advanceCpuCycles(elapsed));
  const auto mixed_frames = spu_.state().mixed_frames;
  spu_.advanceCpuTicks(elapsed);
  if (spu_.state().mixed_frames != mixed_frames) {
    interrupts_.setLine(InterruptSource::spu, spu_.interruptLine());
  }
  scheduler_.advanceTo(tick);
}

bool PsxMachine::writeControllerByte(std::uint8_t value) noexcept {
  constexpr std::uint8_t digital_pad_id = 0x41U;
  constexpr std::uint8_t analog_pad_id = 0x73U;
  constexpr std::uint8_t configuration_id = 0xf3U;
  auto acknowledged = false;

  switch (controller_sio_.phase) {
  case 0U:
    controller_sio_.response = 0xffU;
    controller_sio_.command = 0U;
    controller_sio_.packet_id = 0U;
    controller_sio_.command_parameter = 0U;
    acknowledged = value == 0x01U;
    controller_sio_.phase = acknowledged ? 1U : 0U;
    break;
  case 1U: {
    controller_sio_.command = value;
    const auto supported =
        value == 0x42U || value == 0x43U ||
        (controller_sio_.configuration_mode &&
         (value == 0x44U || value == 0x45U || value == 0x46U ||
          value == 0x47U || value == 0x48U || value == 0x4bU ||
          value == 0x4cU || value == 0x4dU));
    if (!supported) {
      controller_sio_.packet_id = 0U;
      controller_sio_.response = 0xffU;
      controller_sio_.phase = 0U;
      break;
    }

    acknowledged = true;
    controller_sio_.packet_id =
        controller_sio_.configuration_mode
            ? configuration_id
            : (controller_sio_.analog_mode ? analog_pad_id : digital_pad_id);
    controller_sio_.response = controller_sio_.packet_id;
    controller_sio_.phase = 2U;
    break;
  }
  case 2U:
    acknowledged = true;
    controller_sio_.response = 0x5aU;
    controller_sio_.phase = 3U;
    break;
  default: {
    const auto payload_index =
        static_cast<std::size_t>(controller_sio_.phase) - 3U;

    if (payload_index == 0U) {
      controller_sio_.command_parameter = value;
    }
    if (controller_sio_.command == 0x43U && payload_index == 0U) {
      controller_sio_.configuration_mode = value != 0U;
    } else if (controller_sio_.command == 0x44U) {
      if (payload_index == 0U && value <= 0x01U) {
        controller_sio_.analog_mode = value == 0x01U;
      } else if (payload_index == 1U) {
        controller_sio_.analog_locked = (value & 0x03U) == 0x03U;
      }
    }

    const auto payload_size =
        controller_sio_.packet_id == digital_pad_id
            ? 2U
            : ((controller_sio_.packet_id == analog_pad_id ||
                controller_sio_.packet_id == configuration_id)
                   ? 6U
                   : 0U);
    acknowledged = payload_index + 1U < payload_size;
    controller_sio_.response = 0xffU;
    if (payload_index < payload_size) {
      const auto set_input_response = [&] {
        const auto reported_buttons =
            controller_sio_.packet_id == digital_pad_id
                ? static_cast<std::uint16_t>(controller_sio_.buttons | 0x0006U)
                : controller_sio_.buttons;
        switch (payload_index) {
        case 0U:
          controller_sio_.response =
              static_cast<std::uint8_t>(reported_buttons);
          break;
        case 1U:
          controller_sio_.response =
              static_cast<std::uint8_t>(reported_buttons >> 8U);
          break;
        default:
          controller_sio_.response = controller_sio_.analog[payload_index - 2U];
          break;
        }
      };

      if (controller_sio_.packet_id != configuration_id ||
          controller_sio_.command == 0x42U) {
        set_input_response();
      } else {
        switch (controller_sio_.command) {
        case 0x45U: {
          constexpr std::array<std::uint8_t, 6U> model_response{
              0x01U, 0x02U, 0x00U, 0x02U, 0x01U, 0x00U};
          controller_sio_.response =
              payload_index == 2U
                  ? static_cast<std::uint8_t>(controller_sio_.analog_mode)
                  : model_response[payload_index];
          break;
        }
        case 0x46U: {
          constexpr std::array response_by_motor{
              std::array<std::uint8_t, 4U>{0x01U, 0x02U, 0x00U, 0x0aU},
              std::array<std::uint8_t, 4U>{0x01U, 0x01U, 0x01U, 0x14U}};
          controller_sio_.response = 0x00U;
          if (controller_sio_.command_parameter < response_by_motor.size() &&
              payload_index >= 2U) {
            controller_sio_.response =
                response_by_motor[controller_sio_.command_parameter]
                                 [payload_index - 2U];
          }
          break;
        }
        case 0x47U: {
          constexpr std::array<std::uint8_t, 6U> response{0x00U, 0x00U, 0x02U,
                                                          0x00U, 0x01U, 0x00U};
          controller_sio_.response = response[payload_index];
          break;
        }
        case 0x48U:
          controller_sio_.response =
              payload_index == 4U && controller_sio_.command_parameter <= 1U
                  ? 0x01U
                  : 0x00U;
          break;
        case 0x4cU:
          controller_sio_.response =
              payload_index == 3U && controller_sio_.command_parameter <= 1U
                  ? static_cast<std::uint8_t>(
                        controller_sio_.command_parameter == 0U ? 0x04U : 0x07U)
                  : 0x00U;
          break;
        case 0x4dU:
          controller_sio_.response =
              controller_sio_.rumble_protocol[payload_index];
          controller_sio_.rumble_protocol[payload_index] = value;
          break;
        default:
          controller_sio_.response = 0x00U;
          break;
        }
      }
    }
    if (controller_sio_.phase < std::numeric_limits<std::uint8_t>::max()) {
      ++controller_sio_.phase;
    }
    break;
  }
  }
  return acknowledged;
}

void PsxMachine::scheduleControllerTransfer(bool acknowledged) noexcept {
  cancelControllerTransfer();
  controller_sio_.rx_ready = false;
  controller_sio_.transfer_acknowledged = acknowledged;
  controller_sio_.transfer_token =
      scheduler_.scheduleAfter(scaleDeviceTicks(controller_transfer_ticks),
                               MachineEventType::controller_transfer, 0U,
                               static_cast<std::uint64_t>(acknowledged));
  if (controller_sio_.transfer_token != 0U) {
    refreshCpuSliceLimit();
    return;
  }
  controller_sio_.transfer_acknowledged = false;
  controller_sio_.rx_ready = true;
}

void PsxMachine::cancelControllerTransfer() noexcept {
  if (controller_sio_.transfer_token != 0U) {
    static_cast<void>(scheduler_.cancel(controller_sio_.transfer_token));
  }
  controller_sio_.transfer_token = 0U;
  controller_sio_.transfer_acknowledged = false;
}

void PsxMachine::cancelControllerAckRelease() noexcept {
  if (controller_sio_.ack_release_token != 0U) {
    static_cast<void>(scheduler_.cancel(controller_sio_.ack_release_token));
  }
  controller_sio_.ack_release_token = 0U;
  controller_sio_.ack_input = false;
}

void PsxMachine::flushPendingCpuTicks() noexcept {
  if (pending_cpu_ticks_ == 0U) {
    return;
  }
  const auto ticks = pending_cpu_ticks_;
  pending_cpu_ticks_ = 0U;

  advanceTicks(ticks);
}

void PsxMachine::refreshCpuSliceLimit() noexcept {
  const auto slice_remaining =
      pending_cpu_ticks_ >= maximum_cpu_slice_ticks
          ? std::uint64_t{1U}
          : maximum_cpu_slice_ticks - pending_cpu_ticks_;
  const auto deadline = scheduler_.nextDeadline();
  if (deadline == std::numeric_limits<std::uint64_t>::max()) {
    cpu_ticks_until_flush_ = slice_remaining;
    return;
  }

  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto now = pending_cpu_ticks_ > maximum - scheduler_.now()
                       ? maximum
                       : scheduler_.now() + pending_cpu_ticks_;
  const auto event_remaining =
      deadline <= now ? std::uint64_t{1U} : deadline - now;
  cpu_ticks_until_flush_ = std::min(slice_remaining, event_remaining);
}

void PsxMachine::queueCpuTicks(std::uint64_t ticks) noexcept {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  pending_cpu_ticks_ = ticks > maximum - pending_cpu_ticks_
                           ? maximum
                           : pending_cpu_ticks_ + ticks;
  refreshCpuSliceLimit();
  if (cpu_ticks_until_flush_ <= 1U) {
    flushPendingCpuTicks();
  }
}

std::uint64_t PsxMachine::scaleDeviceTicks(std::uint64_t ticks) const noexcept {
  if (cpu_clock_scale_.numerator == cpu_clock_scale_.denominator) {
    return ticks;
  }
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (ticks > maximum / cpu_clock_scale_.numerator) {
    return maximum;
  }
  const auto scaled = ticks * cpu_clock_scale_.numerator;
  const auto rounding = cpu_clock_scale_.denominator - 1U;
  return scaled > maximum - rounding
             ? maximum
             : (scaled + rounding) / cpu_clock_scale_.denominator;
}

std::uint64_t PsxMachine::unscaleCpuTicks(std::uint64_t ticks) noexcept {
  if (cpu_clock_scale_.numerator == cpu_clock_scale_.denominator) {
    return ticks;
  }
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (ticks >
      (maximum - device_tick_remainder_) / cpu_clock_scale_.denominator) {
    device_tick_remainder_ = 0U;
    return maximum;
  }
  const auto scaled =
      ticks * cpu_clock_scale_.denominator + device_tick_remainder_;
  const auto result = scaled / cpu_clock_scale_.numerator;
  device_tick_remainder_ =
      static_cast<std::uint32_t>(scaled % cpu_clock_scale_.numerator);
  return result;
}

void PsxMachine::dispatchEvent(const MachineEvent &event) noexcept {
  if (event.type == MachineEventType::controller_transfer) {
    if (event.index == 0U && event.token == controller_sio_.transfer_token) {
      controller_sio_.transfer_token = 0U;
      ++controller_sio_.transfer_completions;
      const auto acknowledged = controller_sio_.transfer_acknowledged;
      controller_sio_.transfer_acknowledged = false;
      controller_sio_.rx_ready = true;
      if (acknowledged) {
        ++controller_sio_.ack_pulses;
        controller_sio_.ack_input = true;
        if ((controller_sio_.control & 0x1000U) != 0U) {
          controller_sio_.irq_pending = true;
          interrupts_.pulse(InterruptSource::controller);
          syncCpuInterruptLine();
        }
        controller_sio_.ack_release_token = scheduler_.scheduleAfter(
            scaleDeviceTicks(controller_ack_pulse_ticks),
            MachineEventType::controller_transfer, 1U);
        if (controller_sio_.ack_release_token == 0U) {
          controller_sio_.ack_input = false;
        }
        refreshCpuSliceLimit();
      }
      return;
    }
    if (event.index == 1U && event.token == controller_sio_.ack_release_token) {
      controller_sio_.ack_release_token = 0U;
      controller_sio_.ack_input = false;
      return;
    }
    return;
  }
  if (event.type == MachineEventType::cdrom_command) {
    if (event.index == 0U) {
      cdrom_.eventCommand(event.payload);
      syncCdRomSchedules();
      syncCdRomInterruptLine();
      kickDmaChannels();
    }
    return;
  }
  if (event.type == MachineEventType::cdrom_sector) {
    if (event.index == 0U) {
      cdrom_.eventSector(event.payload);
      syncCdRomSchedules();
      syncCdRomInterruptLine();
      kickDmaChannels();
    }
    return;
  }
  if (event.type != MachineEventType::dma_complete ||
      event.index >= DmaController::channel_count) {
    return;
  }
  const auto channel = channelFromIndex(event.index);
  if (dma_.scheduledToken(channel) != event.token) {
    return;
  }
  if (!executeDmaTransfer(channel)) {
    static_cast<void>(dma_.cancelScheduled(channel, event.token));
    if (channel == DmaChannel::spu) {
      spu_.setDmaTransferBusy(false);
    }
    syncSpuInterruptLine();
    const auto index = channelIndex(channel);
    if (channel == DmaChannel::cdrom && index < dma_ports_.size() &&
        dma_ports_[index] == nullptr) {
      static_cast<void>(dma_.complete(channel));
      syncDmaInterruptLine();
      kickDmaChannels();
    }
    return;
  }
  static_cast<void>(dma_.complete(channel));
  if (channel == DmaChannel::spu) {
    spu_.setDmaTransferBusy(false);
  }
  syncSpuInterruptLine();
  syncDmaInterruptLine();
  kickDmaChannels();
}

void PsxMachine::syncCdRomSchedules() noexcept {
  auto command_found = false;
  auto sector_found = false;
  const auto command = cdrom_.commandSchedule();
  const auto sector = cdrom_.sectorSchedule();
  const auto scheduler_state = scheduler_.captureState();
  for (std::size_t index = 0U; index < scheduler_state.event_count; ++index) {
    const auto &event = scheduler_state.events[index];
    if (event.type == MachineEventType::cdrom_command) {
      const auto current = command.pending != 0U && event.index == 0U &&
                           event.payload == command.generation &&
                           !command_found;
      if (current) {
        command_found = true;
      } else {
        static_cast<void>(scheduler_.cancel(event.token));
      }
    } else if (event.type == MachineEventType::cdrom_sector) {
      const auto current = sector.pending != 0U && event.index == 0U &&
                           event.payload == sector.generation && !sector_found;
      if (current) {
        sector_found = true;
      } else {
        static_cast<void>(scheduler_.cancel(event.token));
      }
    }
  }
  if (command.pending != 0U && !command_found) {
    static_cast<void>(scheduler_.scheduleAfter(
        scaleDeviceTicks(command.delay_ticks), MachineEventType::cdrom_command,
        0U, command.generation));
  }
  if (sector.pending != 0U && !sector_found) {
    static_cast<void>(scheduler_.scheduleAfter(
        scaleDeviceTicks(sector.delay_ticks), MachineEventType::cdrom_sector,
        0U, sector.generation));
  }
  refreshCpuSliceLimit();
}

void PsxMachine::syncCdRomInterruptLine() noexcept {
  interrupts_.setLine(InterruptSource::cdrom, cdrom_.interruptLine());
  syncCpuInterruptLine();
}

void PsxMachine::routeTimerInterrupts(RootTimers::IrqMask mask) noexcept {
  constexpr std::array sources{
      InterruptSource::timer0,
      InterruptSource::timer1,
      InterruptSource::timer2,
  };
  for (std::size_t index = 0U; index < sources.size(); ++index) {
    if ((mask & (1U << index)) != 0U) {
      interrupts_.pulse(sources[index]);
    }
  }
}

void PsxMachine::syncDmaInterruptLine() noexcept {
  interrupts_.setLine(InterruptSource::dma, dma_.interruptLine());
  syncCpuInterruptLine();
}

void PsxMachine::syncSpuInterruptLine() noexcept {
  interrupts_.setLine(InterruptSource::spu, spu_.interruptLine());
  syncCpuInterruptLine();
}

void PsxMachine::syncCpuInterruptLine() noexcept {
  cpu_.setExternalInterrupt(interrupts_.cpuLine());
}

void PsxMachine::kickDmaChannels() noexcept {
  for (std::size_t index = 0U; index < DmaController::channel_count; ++index) {
    const auto channel = channelFromIndex(index);
    if (!dma_.channelStartable(channel)) {
      continue;
    }

    const auto is_otc = channel == DmaChannel::otc;
    auto *port = dma_ports_[index];
    const auto internal_cdrom = channel == DmaChannel::cdrom && port == nullptr;
    if (internal_cdrom && (dma_.chcr(channel) & 1U) != 0U) {
      static_cast<void>(dma_.complete(channel));
      syncDmaInterruptLine();
      continue;
    }
    if (!is_otc) {
      const auto request =
          port != nullptr ? port->dmaRequest()
                          : channel == DmaChannel::cdrom && cdrom_.dmaRequest();
      if (!request) {
        continue;
      }
    }

    std::uint64_t delay{};
    if (dma_.syncMode(channel) == DmaSyncMode::linked_list) {
      if (channel != DmaChannel::gpu || !linkedListTicks(delay)) {
        if (internal_cdrom) {
          static_cast<void>(dma_.complete(channel));
          syncDmaInterruptLine();
        }
        continue;
      }
    } else {
      const auto words = dma_.estimatedWordCount(channel);
      if (!words.has_value() || *words > maximum_dma_words) {
        if (internal_cdrom) {
          static_cast<void>(dma_.complete(channel));
          syncDmaInterruptLine();
        }
        continue;
      }
      if (internal_cdrom && !cdrom_.canReadDmaWords(*words)) {
        static_cast<void>(dma_.complete(channel));
        syncDmaInterruptLine();
        continue;
      }
      delay = channel == DmaChannel::spu ? *words * 4U : *words;
    }
    delay = scaleDeviceTicks(std::max<std::uint64_t>(1U, delay));
    const auto token =
        scheduler_.scheduleAfter(delay, MachineEventType::dma_complete,
                                 static_cast<std::uint8_t>(index));
    if (token != 0U) {
      refreshCpuSliceLimit();
      if (!dma_.markScheduled(channel, token)) {
        static_cast<void>(scheduler_.cancel(token));
        if (internal_cdrom) {
          static_cast<void>(dma_.complete(channel));
          syncDmaInterruptLine();
        }
      } else if (channel == DmaChannel::spu) {
        spu_.setDmaTransferBusy(true);
      }
    } else if (internal_cdrom) {
      static_cast<void>(dma_.complete(channel));
      syncDmaInterruptLine();
    }
  }
}

bool PsxMachine::executeDmaTransfer(DmaChannel channel) noexcept {
  if (channel == DmaChannel::otc) {
    return executeOtcDma();
  }
  if (dma_.syncMode(channel) == DmaSyncMode::linked_list) {
    return executeLinkedListDma(channel);
  }
  return executeLinearDma(channel);
}

bool PsxMachine::executeLinearDma(DmaChannel channel) noexcept {
  const auto index = channelIndex(channel);
  if (index >= dma_ports_.size() ||
      (dma_ports_[index] == nullptr && channel != DmaChannel::cdrom)) {
    return false;
  }
  const auto words = dma_.estimatedWordCount(channel);
  if (!words.has_value() || *words > maximum_dma_words) {
    return false;
  }

  auto *port = dma_ports_[index];
  auto address = dma_.madr(channel) & 0x00fffffcU;
  const auto transfer_root = address & ram_address_mask & ~3U;
  const auto control = dma_.chcr(channel);
  const auto from_ram = (control & 1U) != 0U;
  const auto reverse = (control & 2U) != 0U;
  if (from_ram && port == nullptr) {
    return false;
  }
  if (!from_ram && channel == DmaChannel::cdrom && port == nullptr &&
      !cdrom_.prepareDmaRead(*words)) {
    return false;
  }
  for (std::uint64_t word_index = 0U; word_index < *words; ++word_index) {
    const auto ram_address = address & ram_address_mask & ~3U;
    std::uint32_t value{};
    if (from_ram) {
      if (!cpu_.read32(ram_address, value)) {
        return false;
      }
      bool written{};
      if (channel == DmaChannel::gpu && port == gpu_port_) {
        const auto provenance =
            cpu_.projectedVertexProvenanceAt(ram_address, value);
        written = gpu_port_->writeGp0FromRam(
            value,
            GpuDmaWordSource{ram_address, transfer_root,
                             GpuDmaSourceKind::linear},
            provenance.projected, provenance.identity);
      } else {
        written = port->writeDmaWord(value);
      }
      if (!written) {
        return false;
      }
    } else {
      const auto read = port != nullptr ? port->readDmaWord(value)
                                        : cdrom_.readDmaWord(value);
      if (!read || !cpu_.write32(ram_address, value)) {
        return false;
      }
    }
    address =
        reverse ? (address - 4U) & 0x00ffffffU : (address + 4U) & 0x00ffffffU;
  }
  return dma_.setMadr(channel, address);
}

bool PsxMachine::executeLinkedListDma(DmaChannel channel) noexcept {
  if (channel != DmaChannel::gpu || (dma_.chcr(channel) & 1U) == 0U) {
    return false;
  }
  auto *port = dma_ports_[channelIndex(channel)];
  if (port == nullptr) {
    return false;
  }

  auto address = dma_.madr(channel) & ram_address_mask & ~3U;
  const auto transfer_root = address;
  std::uint64_t transferred_words{};
  for (std::uint64_t node = 0U; node < maximum_linked_list_nodes; ++node) {
    std::uint32_t header{};
    if (!cpu_.read32(address, header)) {
      return false;
    }
    const auto word_count = static_cast<std::uint32_t>(header >> 24U);
    if (transferred_words + word_count > maximum_dma_words) {
      return false;
    }
    for (std::uint32_t index = 0U; index < word_count; ++index) {
      const auto word_address =
          (address + (index + 1U) * 4U) & ram_address_mask & ~3U;
      std::uint32_t value{};
      if (!cpu_.read32(word_address, value)) {
        return false;
      }
      bool written{};
      if (port == gpu_port_) {
        const auto provenance =
            cpu_.projectedVertexProvenanceAt(word_address, value);
        written = gpu_port_->writeGp0FromRam(
            value,
            GpuDmaWordSource{word_address, transfer_root,
                             GpuDmaSourceKind::linked_list},
            provenance.projected, provenance.identity);
      } else {
        written = port->writeDmaWord(value);
      }
      if (!written) {
        return false;
      }
    }
    transferred_words += word_count;
    const auto next = header & 0x00ffffffU;
    if ((next & 0x00800000U) != 0U) {
      return dma_.setMadr(channel, next);
    }
    address = next & ram_address_mask & ~3U;
  }
  return false;
}

bool PsxMachine::executeOtcDma() noexcept {
  const auto words = dma_.estimatedWordCount(DmaChannel::otc);
  if (!words.has_value() || *words > maximum_dma_words) {
    return false;
  }
  auto address = dma_.madr(DmaChannel::otc) & ram_address_mask & ~3U;
  for (std::uint64_t index = 0U; index < *words; ++index) {
    const auto link = index + 1U == *words
                          ? 0x00ffffffU
                          : (address - 4U) & ram_address_mask & ~3U;
    if (!cpu_.write32(address, link)) {
      return false;
    }
    address = (address - 4U) & ram_address_mask & ~3U;
  }
  return true;
}

bool PsxMachine::linkedListTicks(std::uint64_t &ticks) const noexcept {
  auto address = dma_.madr(DmaChannel::gpu) & ram_address_mask & ~3U;
  ticks = 0U;
  std::uint64_t transferred_words{};
  for (std::uint64_t node = 0U; node < maximum_linked_list_nodes; ++node) {
    std::uint32_t header{};
    if (!cpu_.read32(address, header)) {
      return false;
    }
    const auto word_count = static_cast<std::uint64_t>(header >> 24U);
    if (transferred_words + word_count > maximum_dma_words ||
        ticks > std::numeric_limits<std::uint64_t>::max() - 14U - word_count) {
      return false;
    }
    transferred_words += word_count;
    ticks += 14U + word_count;
    const auto next = header & 0x00ffffffU;
    if ((next & 0x00800000U) != 0U) {
      return true;
    }
    address = next & ram_address_mask & ~3U;
  }
  return false;
}

} // namespace sf::psx
