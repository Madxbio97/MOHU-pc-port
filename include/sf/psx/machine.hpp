#pragma once

#include "sf/psx/cdrom.hpp"
#include "sf/psx/dma.hpp"
#include "sf/psx/event_scheduler.hpp"
#include "sf/psx/gpu_dma_source.hpp"
#include "sf/psx/interrupt_controller.hpp"
#include "sf/psx/mdec.hpp"
#include "sf/psx/r3000_runtime.hpp"
#include "sf/psx/spu.hpp"
#include "sf/psx/timers.hpp"
#include "sf/psx/xa_decoder.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace sf::psx {

struct CpuClockScale {
  std::uint32_t numerator{1U};
  std::uint32_t denominator{1U};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return numerator != 0U && denominator != 0U;
  }

  [[nodiscard]] friend constexpr bool
  operator==(const CpuClockScale &, const CpuClockScale &) = default;
};

class DmaPort {
public:
  virtual ~DmaPort() = default;
  [[nodiscard]] virtual bool dmaRequest() const noexcept { return true; }
  [[nodiscard]] virtual bool readDmaWord(std::uint32_t &value) noexcept = 0;
  [[nodiscard]] virtual bool writeDmaWord(std::uint32_t value) noexcept = 0;
};

class GpuPort : public DmaPort {
public:
  ~GpuPort() override = default;

  [[nodiscard]] virtual bool writeGp0(std::uint32_t value) noexcept = 0;
  [[nodiscard]] virtual bool
  writeGp0Projected(std::uint32_t value, const GteProjectedVertex *projected,
                    std::uint64_t source_identity) noexcept {
    static_cast<void>(projected);
    static_cast<void>(source_identity);
    return writeGp0(value);
  }
  virtual void writeGp1(std::uint32_t value) noexcept = 0;
  [[nodiscard]] virtual bool readGp0(std::uint32_t &value) noexcept = 0;
  [[nodiscard]] virtual std::uint32_t readStatus() const noexcept = 0;
  [[nodiscard]] virtual bool
  writeGp0FromRam(std::uint32_t value, std::uint32_t source_address,
                  const GteProjectedVertex *projected) noexcept {
    static_cast<void>(source_address);
    static_cast<void>(projected);
    return writeGp0(value);
  }
  [[nodiscard]] virtual bool
  writeGp0FromRam(std::uint32_t value, std::uint32_t source_address,
                  const GteProjectedVertex *projected,
                  std::uint64_t source_identity) noexcept {
    static_cast<void>(source_identity);
    return writeGp0FromRam(value, source_address, projected);
  }
  [[nodiscard]] virtual bool
  writeGp0FromRam(std::uint32_t value, GpuDmaWordSource source,
                  const GteProjectedVertex *projected,
                  std::uint64_t source_identity) noexcept {
    return writeGp0FromRam(value, source.word_address, projected,
                           source_identity);
  }

  [[nodiscard]] bool readDmaWord(std::uint32_t &value) noexcept final {
    return readGp0(value);
  }
  [[nodiscard]] bool writeDmaWord(std::uint32_t value) noexcept final {
    return writeGp0(value);
  }
};

inline constexpr std::size_t controller_port_count = 2U;

struct ControllerPortState {
  std::uint16_t buttons{0xffffU};
  // DualShock wire order: right X/Y, then left X/Y.
  std::array<std::uint8_t, 4U> analog{0x80U, 0x80U, 0x80U, 0x80U};
  std::array<std::uint8_t, 6U> rumble_protocol{0xffU, 0xffU, 0xffU,
                                               0xffU, 0xffU, 0xffU};
  bool connected{};
  bool configuration_mode{};
  bool analog_mode{};
  bool analog_locked{};

  [[nodiscard]] friend bool operator==(const ControllerPortState &,
                                       const ControllerPortState &) = default;
};

struct ControllerSioState {
  std::uint16_t mode{};
  std::uint16_t control{};
  std::uint16_t baud{};
  std::array<ControllerPortState, controller_port_count> ports{
      ControllerPortState{.connected = true}, ControllerPortState{}};
  std::uint64_t data_writes{};
  std::uint64_t ignored_busy_writes{};
  std::array<std::uint64_t, controller_port_count> port_bytes{};
  std::uint64_t transfer_completions{};
  std::uint64_t ack_pulses{};
  std::uint64_t control_writes{};
  std::uint64_t reset_writes{};
  std::uint64_t transfer_token{};
  std::uint64_t ack_release_token{};
  std::uint8_t response{0xffU};
  std::uint8_t command{};
  std::uint8_t packet_id{};
  std::uint8_t phase{};
  std::uint8_t command_parameter{};
  bool transfer_acknowledged{};
  bool ack_input{};
  bool irq_pending{};
  bool rx_ready{};

  [[nodiscard]] friend bool operator==(const ControllerSioState &,
                                       const ControllerSioState &) = default;
};

struct PsxMachineState {
  CpuClockScale cpu_clock_scale{};
  EventSchedulerState scheduler;
  InterruptControllerState interrupts;
  DmaControllerState dma;
  CdRomState cdrom;
  MdecState mdec;
  std::unique_ptr<SpuState> spu{std::make_unique<SpuState>()};
  ControllerSioState controller_sio;
  XaDecoderState xa_decoder;
  RootTimersState timers;
  std::uint64_t pending_cpu_ticks{};
  std::uint32_t device_tick_remainder{};

  PsxMachineState() = default;
  PsxMachineState(PsxMachineState &&) noexcept = default;
  PsxMachineState &operator=(PsxMachineState &&) noexcept = default;

  PsxMachineState(const PsxMachineState &other)
      : cpu_clock_scale(other.cpu_clock_scale), scheduler(other.scheduler),
        interrupts(other.interrupts), dma(other.dma), cdrom(other.cdrom),
        mdec(other.mdec),
        spu(other.spu ? std::make_unique<SpuState>(*other.spu) : nullptr),
        controller_sio(other.controller_sio), xa_decoder(other.xa_decoder),
        timers(other.timers), pending_cpu_ticks(other.pending_cpu_ticks),
        device_tick_remainder(other.device_tick_remainder) {}

  PsxMachineState &operator=(const PsxMachineState &other) {
    if (this == &other) {
      return *this;
    }
    cpu_clock_scale = other.cpu_clock_scale;
    scheduler = other.scheduler;
    interrupts = other.interrupts;
    dma = other.dma;
    cdrom = other.cdrom;
    mdec = other.mdec;
    controller_sio = other.controller_sio;
    if (other.spu) {
      if (!spu) {
        spu = std::make_unique<SpuState>();
      }
      *spu = *other.spu;
    } else {
      spu.reset();
    }
    xa_decoder = other.xa_decoder;
    timers = other.timers;
    pending_cpu_ticks = other.pending_cpu_ticks;
    device_tick_remainder = other.device_tick_remainder;
    return *this;
  }
};

// Hardware layer around the interpreter. The scheduler uses CPU-domain ticks.
// Device clocks are unscaled with a serialized carry, matching DuckStation's
// overclock model: a faster CPU receives more work in the same hardware time.
class PsxMachine final : private R3000MmioBus, private CdRomXaAudioSink {
public:
  static constexpr std::uint64_t cpu_clock_hz = 33'868'800U;

  explicit PsxMachine(R3000Runtime &cpu, CpuClockScale cpu_clock_scale = {});
  ~PsxMachine() override;
  PsxMachine(const PsxMachine &) = delete;
  PsxMachine &operator=(const PsxMachine &) = delete;

  void reset() noexcept;
  [[nodiscard]] R3000RunResult step() noexcept;
  [[nodiscard]] R3000RunResult
  runCached(std::uint64_t instruction_budget) noexcept;
  // Skip only a CPU loop already proven side-effect-free by R3000Runtime.
  // The bound ends at the next device event or periodic hardware flush.
  [[nodiscard]] std::uint64_t
  fastForwardIdleTicks(std::uint64_t maximum_ticks) noexcept;
  void advanceTicks(std::uint64_t ticks) noexcept;
  // Commit already-accounted interpreter ticks to hardware devices without
  // consuming any additional guest CPU time.
  void synchronizeDevices() noexcept { flushPendingCpuTicks(); }
  void advanceHardwareTicks(std::uint64_t ticks) noexcept {
    advanceTicks(scaleDeviceTicks(ticks));
  }

  void attachDmaPort(DmaChannel channel, DmaPort *port) noexcept;
  void setCdRomMedia(CdRomMedia *media) noexcept;
  void serviceDmaRequests() noexcept;
  void attachGpuPort(GpuPort *port) noexcept;
  void setVBlank(bool active) noexcept;
  void pulseVBlank() noexcept;
  void setControllerButtons(std::uint16_t active_low_buttons) noexcept {
    setControllerState(0U, active_low_buttons,
                       controller_sio_.ports[0U].analog, true);
  }
  void setControllerState(std::uint16_t active_low_buttons,
                          std::array<std::uint8_t, 4U> analog) noexcept {
    setControllerState(0U, active_low_buttons, analog, true);
  }
  void setControllerState(std::size_t port,
                          std::uint16_t active_low_buttons,
                          std::array<std::uint8_t, 4U> analog,
                          bool connected = true) noexcept {
    if (port >= controller_sio_.ports.size()) {
      return;
    }
    auto &state = controller_sio_.ports[port];
    state.buttons = active_low_buttons;
    state.analog = analog;
    state.connected = connected;
  }
  void setControllerConnected(std::size_t port, bool connected) noexcept {
    if (port < controller_sio_.ports.size()) {
      controller_sio_.ports[port].connected = connected;
    }
  }
  [[nodiscard]] const ControllerSioState &controllerSio() const noexcept {
    return controller_sio_;
  }
  void setHBlank(bool active) noexcept;
  void advanceDotClocks(std::uint64_t clocks) noexcept;

  [[nodiscard]] std::uint64_t currentTick() const noexcept;
  [[nodiscard]] std::uint64_t cpuTicksPerSecond() const noexcept;
  [[nodiscard]] CpuClockScale cpuClockScale() const noexcept {
    return cpu_clock_scale_;
  }
  [[nodiscard]] std::optional<std::uint64_t>
  dmaCompletionTick(DmaChannel channel) const noexcept;
  // Advance the hardware timeline until every block of one scheduled DMA
  // request has completed.
  [[nodiscard]] bool advanceToDmaCompletion(DmaChannel channel) noexcept;
  // Complete one already-scheduled transfer without advancing the hardware
  // timeline. Frame-owned DMA sidecars must remain alive until GPU DMA has
  // consumed the command list that refers to them.
  [[nodiscard]] bool completePendingDmaTransfer(DmaChannel channel) noexcept;
  // Clock-neutral guest callbacks still need MMIO DMA requests to become
  // observable before a retail busy-wait can return. Complete those scheduled
  // transfers without advancing the CPU/SPU/CD timeline.
  [[nodiscard]] bool completePendingDmaTransfers() noexcept;
  // Synchronous HLE CD reads cannot yield back to the realtime 120 Hz clock.
  // Complete one scheduled CD event in-place so their busy-waits make forward
  // progress without rendering a block of future SPU audio in one host frame.
  [[nodiscard]] bool completeNextPendingCdRomEvent() noexcept;
  [[nodiscard]] const InterruptController &interrupts() const noexcept {
    return interrupts_;
  }
  [[nodiscard]] const DmaController &dma() const noexcept { return dma_; }
  [[nodiscard]] const CdRomController &cdrom() const noexcept { return cdrom_; }
  [[nodiscard]] CdRomController &cdrom() noexcept { return cdrom_; }
  [[nodiscard]] const Mdec &mdec() const noexcept { return mdec_; }
  [[nodiscard]] Mdec &mdec() noexcept { return mdec_; }
  [[nodiscard]] const Spu &spu() const noexcept { return spu_; }
  [[nodiscard]] Spu &spu() noexcept { return spu_; }
  [[nodiscard]] const RootTimers &timers() const noexcept { return timers_; }
  [[nodiscard]] PsxMachineState captureState() const;
  [[nodiscard]] bool validateState(const PsxMachineState &state) const noexcept;
  [[nodiscard]] bool restoreState(const PsxMachineState &state) noexcept;

private:
  [[nodiscard]] bool readMmio(std::uint32_t physical_address,
                              R3000AccessWidth width,
                              std::uint32_t &value) noexcept override;
  [[nodiscard]] bool
  idleSafeReadMmio(std::uint32_t physical_address,
                   R3000AccessWidth width) const noexcept override;
  [[nodiscard]] bool writeMmio(std::uint32_t physical_address,
                               R3000AccessWidth width, std::uint32_t value,
                               const GteProjectedVertex *projected,
                               std::uint64_t projection_identity,
                               std::uint32_t producer_pc) noexcept override;
  void consumeXaSector(
      std::span<const std::byte, CdRomMedia::raw_sector_size> sector,
      bool muted) noexcept override;
  void resetXaStream() noexcept override;
  void setXaOutputMixer(std::array<std::uint8_t, 4U> matrix) noexcept override;

  void advanceDevicesTo(std::uint64_t tick) noexcept;
  void refreshCpuSliceLimit() noexcept;
  void flushPendingCpuTicks() noexcept;
  [[nodiscard]] bool writeControllerByte(std::uint8_t value,
                                         ControllerPortState &port) noexcept;
  void scheduleControllerTransfer(bool acknowledged) noexcept;
  void cancelControllerTransfer() noexcept;
  void cancelControllerAckRelease() noexcept;
  void queueCpuTicks(std::uint64_t ticks) noexcept;
  [[nodiscard]] std::uint64_t
  scaleDeviceTicks(std::uint64_t ticks) const noexcept;
  [[nodiscard]] std::uint64_t unscaleCpuTicks(std::uint64_t ticks) noexcept;
  void dispatchEvent(const MachineEvent &event) noexcept;
  void routeTimerInterrupts(RootTimers::IrqMask mask) noexcept;
  void syncCdRomSchedules() noexcept;
  void syncCdRomInterruptLine() noexcept;
  void syncDmaInterruptLine() noexcept;
  void syncSpuInterruptLine() noexcept;
  void syncCpuInterruptLine() noexcept;
  void kickDmaChannels() noexcept;
  [[nodiscard]] bool executeDmaTransfer(DmaChannel channel) noexcept;
  [[nodiscard]] bool executeLinearDma(DmaChannel channel) noexcept;
  [[nodiscard]] bool executeLinearDma(DmaChannel channel,
                                      std::uint64_t word_count) noexcept;
  [[nodiscard]] bool executeLinkedListDma(DmaChannel channel) noexcept;
  [[nodiscard]] bool executeOtcDma() noexcept;
  [[nodiscard]] bool linkedListTicks(std::uint64_t &ticks) const noexcept;

  R3000Runtime &cpu_;
  EventScheduler scheduler_;
  InterruptController interrupts_;
  DmaController dma_;
  CdRomController cdrom_;
  Mdec mdec_;
  Spu spu_;
  XaAudioDecoder xa_decoder_;
  std::array<SpuPcmFrame, XaAudioDecoder::maximum_output_frames>
      xa_decode_scratch_{};
  ControllerSioState controller_sio_{};
  RootTimers timers_;
  CpuClockScale cpu_clock_scale_{};
  std::uint64_t pending_cpu_ticks_{};
  std::uint64_t cpu_ticks_until_flush_{1U};
  std::uint32_t device_tick_remainder_{};

  class SpuDmaPort final : public DmaPort {
  public:
    explicit SpuDmaPort(Spu &spu) noexcept : spu_(spu) {}

    [[nodiscard]] bool dmaRequest() const noexcept override {
      return spu_.dmaRequest();
    }
    [[nodiscard]] bool readDmaWord(std::uint32_t &value) noexcept override {
      return spu_.readDmaWord(value);
    }
    [[nodiscard]] bool writeDmaWord(std::uint32_t value) noexcept override {
      return spu_.writeDmaWord(value);
    }

  private:
    Spu &spu_;
  };

  class MdecInputDmaPort final : public DmaPort {
  public:
    explicit MdecInputDmaPort(Mdec &mdec) noexcept : mdec_(mdec) {}

    [[nodiscard]] bool dmaRequest() const noexcept override {
      return mdec_.dmaInRequest();
    }
    [[nodiscard]] bool readDmaWord(std::uint32_t &) noexcept override {
      return false;
    }
    [[nodiscard]] bool writeDmaWord(std::uint32_t value) noexcept override {
      return mdec_.writeData(value);
    }

  private:
    Mdec &mdec_;
  };

  class MdecOutputDmaPort final : public DmaPort {
  public:
    explicit MdecOutputDmaPort(Mdec &mdec) noexcept : mdec_(mdec) {}

    [[nodiscard]] bool dmaRequest() const noexcept override {
      return mdec_.dmaOutRequest();
    }
    [[nodiscard]] bool readDmaWord(std::uint32_t &value) noexcept override {
      return mdec_.readData(value);
    }
    [[nodiscard]] bool writeDmaWord(std::uint32_t) noexcept override {
      return false;
    }

  private:
    Mdec &mdec_;
  };

  MdecInputDmaPort mdec_input_dma_port_{mdec_};
  MdecOutputDmaPort mdec_output_dma_port_{mdec_};
  SpuDmaPort spu_dma_port_{spu_};
  GpuPort *gpu_port_{};
  std::array<DmaPort *, DmaController::channel_count> dma_ports_{};
};
} // namespace sf::psx
