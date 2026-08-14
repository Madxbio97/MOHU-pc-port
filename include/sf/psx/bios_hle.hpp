#pragma once

#include "sf/psx/memory_card_hle.hpp"
#include "sf/psx/r3000_runtime.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <utility>

namespace sf::psx {
class PsxMachine;

struct BiosHleState {
  std::uint32_t interrupt_entry{};
  std::uint32_t clear_pad{};
  std::array<std::uint32_t, 4U> clear_root_counter{};
  std::array<std::uint32_t, 4U> interrupt_routines{};
  std::uint32_t last_gpu_command{};
  std::uint64_t gpu_command_count{};
  std::uint32_t memory_card_init_mode{};
  bool memory_card_initialized{};
  bool memory_card_started{};
  bool memory_card_filesystem_initialized{};
  MemoryCardHleState memory_card{};
};

enum class BiosAsyncServiceResult : std::uint8_t {
  idle,
  progressed,
  failed,
};

using MemoryCardCommitCallback =
    std::function<bool(const MemoryCardHleState &)>;

// Small HLE boundary for BIOS services used during original executable
// bootstrap. Game code still runs in the R3000 interpreter; only firmware and
// hardware-facing services are replaced.
class BiosHle final {
public:
  explicit BiosHle(R3000Runtime &runtime, PsxMachine *machine = nullptr,
                   MemoryCardCommitCallback memory_card_commit = {}) noexcept
      : runtime_(runtime), machine_(machine),
        memory_card_commit_(std::move(memory_card_commit)) {}

  [[nodiscard]] bool atExceptionBoundary() const noexcept;
  [[nodiscard]] bool dispatchException() noexcept;
  [[nodiscard]] bool atExceptionReturn() const noexcept;
  [[nodiscard]] bool completeException() noexcept;
  [[nodiscard]] bool atEventCallbackReturn() const noexcept;
  [[nodiscard]] bool completeEventCallback() noexcept;
  [[nodiscard]] BiosAsyncServiceResult serviceAsync() noexcept;
  [[nodiscard]] bool asyncServicePending() const noexcept {
    return !exception_active_ && !event_callback_active_ &&
           (state_.memory_card.pending.kind != MemoryCardPendingKind::none ||
            event_callback_count_ != 0U);
  }
  [[nodiscard]] bool atCallBoundary() const noexcept;
  [[nodiscard]] bool handleCall() noexcept;
  [[nodiscard]] bool handleKernelSyscall() noexcept;
  [[nodiscard]] bool exceptionActive() const noexcept {
    return exception_active_;
  }

  [[nodiscard]] const BiosHleState &state() const noexcept { return state_; }
  [[nodiscard]] bool
  restoreMemoryCardState(const MemoryCardHleState &state) noexcept;

private:
  enum class ExceptionDispatchStage : std::uint8_t {
    none,
    first,
    second,
    hook,
  };

  enum class InterruptAdvanceResult : std::uint8_t {
    started,
    exhausted,
    failed,
  };

  void completeCall(std::uint32_t result) noexcept;
  [[nodiscard]] bool eventTable(std::uint32_t &table,
                                std::uint32_t &count) noexcept;
  [[nodiscard]] bool deliverEvent(std::uint32_t event_class,
                                  std::uint32_t spec) noexcept;
  [[nodiscard]] bool queueEventCallback(std::uint32_t handler) noexcept;
  [[nodiscard]] bool commitMemoryCard() noexcept;
  [[nodiscard]] InterruptAdvanceResult beginNextInterruptRoutine() noexcept;
  [[nodiscard]] bool beginHookedException() noexcept;
  [[nodiscard]] bool restoreInterruptedCpu() noexcept;
  [[nodiscard]] bool acknowledgeFallbackInterrupts() noexcept;

  R3000Runtime &runtime_;
  BiosHleState state_{};
  PsxMachine *machine_{};
  MemoryCardCommitCallback memory_card_commit_{};
  std::array<std::uint32_t, 32U> event_callback_queue_{};
  R3000State event_callback_resume_cpu_{};
  R3000PgxpTransformCheckpoint event_callback_resume_pgxp_{};
  std::uint8_t event_callback_head_{};
  std::uint8_t event_callback_count_{};
  bool event_callback_active_{};
  R3000State interrupted_cpu_{};
  R3000PgxpTransformCheckpoint interrupted_pgxp_{};
  std::uint16_t exception_pending_interrupts_{};
  std::uint32_t exception_routine_{};
  std::uint32_t exception_second_handler_{};
  std::uint32_t exception_routine_budget_{};
  std::uint8_t exception_priority_{};
  ExceptionDispatchStage exception_stage_{ExceptionDispatchStage::none};
  bool exception_active_{};
  bool exception_uses_hook_{};
};

} // namespace sf::psx
