#pragma once

#include "sf/psx/memory_card_hle.hpp"
#include "sf/psx/r3000_runtime.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <utility>

namespace sf::psx {
class PsxMachine;

enum class BiosMemoryCardOperation : std::uint8_t {
  none,
  info,
  load,
  raw_read,
  raw_write,
};

struct BiosHleState {
  std::uint32_t interrupt_entry{};
  std::uint32_t clear_pad{};
  std::array<std::uint32_t, 4U> clear_root_counter{};
  std::array<std::uint32_t, 4U> interrupt_routines{};
  std::uint32_t last_gpu_command{};
  std::uint64_t gpu_command_count{};
  std::uint32_t memory_card_init_mode{};
  std::uint32_t memory_card_channel{};
  std::uint32_t memory_card_status{1U};
  std::uint32_t memory_card_slot_2_status{1U};
  std::uint32_t memory_card_last_event_class{};
  std::uint32_t memory_card_last_event_spec{};
  std::uint64_t memory_card_submissions{};
  std::uint64_t memory_card_completions{};
  std::uint64_t event_callbacks_started{};
  std::uint64_t event_callbacks_completed{};
  BiosMemoryCardOperation memory_card_operation{BiosMemoryCardOperation::none};
  bool memory_card_initialized{};
  bool memory_card_started{};
  bool memory_card_filesystem_initialized{};
  bool memory_card_auto_format{};
  MemoryCardHleState memory_card{};
  MemoryCardHleState memory_card_slot_2{};
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
  explicit BiosHle(
      R3000Runtime &runtime, PsxMachine *machine = nullptr,
      MemoryCardCommitCallback memory_card_commit = {},
      MemoryCardCommitCallback memory_card_slot_2_commit = {}) noexcept
      : runtime_(runtime), machine_(machine),
        memory_card_commits_{std::move(memory_card_commit),
                             std::move(memory_card_slot_2_commit)} {}

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
            state_.memory_card_slot_2.pending.kind !=
                MemoryCardPendingKind::none ||
            state_.memory_card_operation != BiosMemoryCardOperation::none ||
            event_callback_count_ != 0U);
  }
  [[nodiscard]] std::uint64_t asyncServiceTicksRemaining() const noexcept;
  [[nodiscard]] bool atCallBoundary() const noexcept;
  [[nodiscard]] bool handleCall() noexcept;
  [[nodiscard]] bool handleKernelSyscall() noexcept;
  [[nodiscard]] bool exceptionActive() const noexcept {
    return exception_active_;
  }

  [[nodiscard]] const BiosHleState &state() const noexcept { return state_; }
  [[nodiscard]] bool restoreMemoryCardState(const MemoryCardHleState &state,
                                            std::uint8_t slot = 0U) noexcept;

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
  [[nodiscard]] bool ensureCardKernelTables() noexcept;
  [[nodiscard]] MemoryCardHleState &memoryCard(std::uint8_t slot) noexcept;
  [[nodiscard]] const MemoryCardHleState &
  memoryCard(std::uint8_t slot) const noexcept;
  [[nodiscard]] std::uint32_t &memoryCardStatus(std::uint8_t slot) noexcept;
  [[nodiscard]] bool commitMemoryCard(std::uint8_t slot) noexcept;
  [[nodiscard]] bool
  beginMemoryCardOperation(BiosMemoryCardOperation operation, std::uint8_t slot,
                           std::uint32_t event_class, std::uint32_t sector = 0U,
                           std::uint32_t guest_buffer = 0U) noexcept;
  [[nodiscard]] static std::uint64_t
  memoryCardOperationTicks(BiosMemoryCardOperation operation) noexcept;
  [[nodiscard]] InterruptAdvanceResult beginNextInterruptRoutine() noexcept;
  [[nodiscard]] bool beginHookedException() noexcept;
  [[nodiscard]] bool restoreInterruptedCpu() noexcept;
  [[nodiscard]] bool acknowledgeFallbackInterrupts() noexcept;

  R3000Runtime &runtime_;
  BiosHleState state_{};
  PsxMachine *machine_{};
  std::array<MemoryCardCommitCallback, memory_card_slot_count>
      memory_card_commits_{};
  std::array<std::uint32_t, 32U> event_callback_queue_{};
  R3000State event_callback_resume_cpu_{};
  R3000PgxpTransformCheckpoint event_callback_resume_pgxp_{};
  std::uint8_t event_callback_head_{};
  std::uint64_t memory_card_completion_tick_{};
  std::uint32_t memory_card_event_class_{};
  std::uint32_t memory_card_sector_{};
  std::uint32_t memory_card_buffer_{};
  std::uint8_t event_callback_count_{};
  bool card_kernel_tables_initialized_{};
  std::uint8_t memory_card_operation_slot_{memory_card_invalid_slot};
  std::uint8_t memory_card_find_slot_{memory_card_invalid_slot};
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
