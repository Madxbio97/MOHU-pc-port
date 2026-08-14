#include "sf/psx/bios_hle.hpp"
#include "sf/psx/machine.hpp"

#include <memory>
#include <new>

namespace sf::psx {
namespace {

constexpr std::uint32_t a0_vector = 0x000000a0U;
constexpr std::uint32_t b0_vector = 0x000000b0U;
constexpr std::uint32_t c0_vector = 0x000000c0U;
constexpr std::uint32_t gpu_gp0 = 0x1f801810U;
constexpr std::uint32_t interrupt_status = 0x1f801070U;
constexpr std::uint32_t exception_vector = 0x80000080U;
constexpr std::uint32_t c0_table = 0x00000674U;
constexpr std::uint32_t b0_table = 0x00000874U;
constexpr std::uint32_t kernel_interrupt_stack = 0x000085d8U;
constexpr std::uint32_t kernel_interrupt_global_pointer = 0x0000f450U;
constexpr std::uint32_t event_table_anchor = 0x00000120U;
constexpr std::uint32_t event_table_size_anchor = 0x00000124U;
constexpr std::uint32_t default_event_table = 0x0000e000U;
constexpr std::uint32_t event_stride = 0x1cU;
constexpr std::uint32_t default_event_count = 16U;
constexpr std::uint32_t event_handle_base = 0xf1000000U;
constexpr std::uint32_t default_interrupt_entry = 0x00006cf4U;
constexpr std::uint32_t event_status_offset = 0x04U;
constexpr std::uint32_t event_spec_offset = 0x08U;
constexpr std::uint32_t event_mode_offset = 0x0cU;
constexpr std::uint32_t event_handler_offset = 0x10U;
constexpr std::uint32_t event_free = 0x0000U;
constexpr std::uint32_t event_disabled = 0x1000U;
constexpr std::uint32_t event_enabled = 0x2000U;
constexpr std::uint32_t event_ready = 0x4000U;
constexpr std::uint32_t event_callback_mode = 0x1000U;
constexpr std::uint32_t event_mark_mode = 0x2000U;

} // namespace

bool BiosHle::atCallBoundary() const noexcept {
  const auto pc = runtime_.state().pc;
  return pc == a0_vector || pc == b0_vector || pc == c0_vector;
}

bool BiosHle::atExceptionBoundary() const noexcept {
  return runtime_.state().pc == exception_vector;
}

bool BiosHle::dispatchException() noexcept {
  if (!atExceptionBoundary() || exception_active_) {
    return false;
  }

  interrupted_cpu_ = runtime_.state();
  interrupted_pgxp_ = runtime_.capturePgxpTransformCheckpoint();
  exception_pending_interrupts_ = 0U;
  static_cast<void>(
      runtime_.read16(interrupt_status, exception_pending_interrupts_));
  exception_active_ = true;
  exception_uses_hook_ = false;
  exception_priority_ = 0U;
  exception_routine_ = state_.interrupt_routines[0U];
  exception_second_handler_ = 0U;
  exception_routine_budget_ = 64U;
  exception_stage_ = ExceptionDispatchStage::none;

  // The retail BIOS saves the interrupted thread first, then runs every IntRP
  // callback on its private kernel stack. Reusing the interrupted SP is not
  // safe: games may be interrupted exactly at the lower scratchpad boundary.
  auto callback_cpu = runtime_.state();
  callback_cpu.gpr[29U] = kernel_interrupt_stack;
  callback_cpu.gpr[30U] = kernel_interrupt_stack;
  callback_cpu.gpr[28U] = kernel_interrupt_global_pointer;
  runtime_.restoreCpuState(callback_cpu);

  const auto advance = beginNextInterruptRoutine();
  if (advance == InterruptAdvanceResult::started) {
    return true;
  }
  if (advance == InterruptAdvanceResult::failed) {
    exception_active_ = false;
    return false;
  }
  if (state_.interrupt_entry != 0U && beginHookedException()) {
    exception_uses_hook_ = true;
    exception_stage_ = ExceptionDispatchStage::hook;
    return true;
  }

  exception_active_ = false;
  exception_stage_ = ExceptionDispatchStage::none;
  return false;
}

bool BiosHle::atExceptionReturn() const noexcept {
  return exception_active_ && runtime_.atReturnSentinel();
}

bool BiosHle::completeException() noexcept {
  if (!atExceptionReturn()) {
    return false;
  }

  runtime_.settleLoadDelay();
  if (exception_stage_ == ExceptionDispatchStage::first) {
    const auto first_result = runtime_.state().gpr[2U];
    if (first_result != 0U && exception_second_handler_ != 0U) {
      const std::array<std::uint32_t, 1U> arguments{first_result};
      if (!runtime_.beginCall(exception_second_handler_, arguments)) {
        return false;
      }
      exception_stage_ = ExceptionDispatchStage::second;
      return true;
    }
  }

  exception_second_handler_ = 0U;
  exception_stage_ = ExceptionDispatchStage::none;
  const auto advance = beginNextInterruptRoutine();
  if (advance == InterruptAdvanceResult::started) {
    return true;
  }
  if (advance == InterruptAdvanceResult::failed) {
    return false;
  }

  if (state_.interrupt_entry != 0U) {
    if (!beginHookedException()) {
      return false;
    }
    exception_uses_hook_ = true;
    exception_stage_ = ExceptionDispatchStage::hook;
    return true;
  }

  if (!acknowledgeFallbackInterrupts()) {
    return false;
  }
  return restoreInterruptedCpu();
}

BiosHle::InterruptAdvanceResult BiosHle::beginNextInterruptRoutine() noexcept {
  while (static_cast<std::size_t>(exception_priority_) <
         state_.interrupt_routines.size()) {
    if (exception_routine_ == 0U) {
      ++exception_priority_;
      if (static_cast<std::size_t>(exception_priority_) <
          state_.interrupt_routines.size()) {
        exception_routine_ = state_.interrupt_routines[static_cast<std::size_t>(
            exception_priority_)];
      }
      continue;
    }
    if (exception_routine_budget_ == 0U) {
      return InterruptAdvanceResult::failed;
    }

    const auto node = exception_routine_;
    std::uint32_t next{};
    std::uint32_t second{};
    std::uint32_t first{};
    if (!runtime_.read32(node, next) || !runtime_.read32(node + 4U, second) ||
        !runtime_.read32(node + 8U, first)) {
      return InterruptAdvanceResult::failed;
    }
    exception_routine_ = next;
    --exception_routine_budget_;
    if (first == 0U) {
      continue;
    }

    exception_second_handler_ = second;
    if (!runtime_.beginCall(first)) {
      return InterruptAdvanceResult::failed;
    }
    exception_stage_ = ExceptionDispatchStage::first;
    return InterruptAdvanceResult::started;
  }
  return InterruptAdvanceResult::exhausted;
}

bool BiosHle::beginHookedException() noexcept {
  auto hooked = runtime_.state();
  const auto context = state_.interrupt_entry;
  std::uint32_t saved_ra{};
  std::uint32_t saved_sp{};
  std::uint32_t saved_fp{};
  std::uint32_t saved_gp{};
  if (!runtime_.read32(context, saved_ra) ||
      !runtime_.read32(context + 4U, saved_sp) ||
      !runtime_.read32(context + 8U, saved_fp) ||
      !runtime_.read32(context + 0x2cU, saved_gp) || saved_ra == 0U) {
    return false;
  }
  hooked.gpr[31U] = saved_ra;
  hooked.gpr[29U] = saved_sp;
  hooked.gpr[30U] = saved_fp;
  hooked.gpr[28U] = saved_gp;
  for (std::uint32_t index = 0U; index < 8U; ++index) {
    if (!runtime_.read32(context + 0x0cU + index * 4U,
                         hooked.gpr[16U + index])) {
      return false;
    }
  }
  hooked.gpr[2U] = 1U;
  hooked.pc = saved_ra;
  hooked.next_pc = saved_ra + 4U;
  hooked.branch_pc = 0U;
  hooked.branch_delay_slot = false;
  hooked.load_delay = {};
  hooked.next_load_delay = {};
  runtime_.restoreCpuState(hooked);
  exception_pending_interrupts_ = 0U;
  return true;
}

bool BiosHle::acknowledgeFallbackInterrupts() noexcept {
  if (exception_pending_interrupts_ != 0U) {
    const auto preserved =
        static_cast<std::uint16_t>(~exception_pending_interrupts_);
    if (!runtime_.write16(interrupt_status, preserved)) {
      return false;
    }
    exception_pending_interrupts_ = 0U;
  }
  return true;
}

bool BiosHle::restoreInterruptedCpu() noexcept {
  auto resumed = interrupted_cpu_;
  constexpr std::uint32_t hardware_interrupt_bit = 1U << 10U;
  resumed.cop0_cause = (resumed.cop0_cause & ~hardware_interrupt_bit) |
                       (runtime_.state().cop0_cause & hardware_interrupt_bit);
  resumed.cop0_status =
      (resumed.cop0_status & ~0x0fU) | ((resumed.cop0_status >> 2U) & 0x0fU);
  resumed.pc = resumed.cop0_epc;
  resumed.next_pc = resumed.pc + 4U;
  resumed.branch_pc = 0U;
  resumed.branch_delay_slot = false;
  runtime_.restoreCpuState(resumed, interrupted_pgxp_);
  interrupted_pgxp_ = {};
  exception_routine_ = 0U;
  exception_second_handler_ = 0U;
  exception_routine_budget_ = 0U;
  exception_priority_ = 0U;
  exception_stage_ = ExceptionDispatchStage::none;
  exception_active_ = false;
  exception_uses_hook_ = false;
  return true;
}

void BiosHle::completeCall(std::uint32_t result) noexcept {
  runtime_.setRegister(2U, result);
  runtime_.completeHostCall();
}

bool BiosHle::restoreMemoryCardState(const MemoryCardHleState &state) noexcept {
  auto persistent = std::unique_ptr<MemoryCardHleState>{
      new (std::nothrow) MemoryCardHleState{state}};
  if (!persistent) {
    return false;
  }
  persistent->descriptors = {};
  persistent->find = {};
  persistent->pending = {};
  if (!MemoryCardHle::validateState(*persistent)) {
    return false;
  }
  state_.memory_card = std::move(*persistent);
  return true;
}

bool BiosHle::commitMemoryCard() noexcept {
  if (!memory_card_commit_) {
    return true;
  }
  try {
    return memory_card_commit_(state_.memory_card);
  } catch (...) {
    return false;
  }
}

bool BiosHle::eventTable(std::uint32_t &table, std::uint32_t &count) noexcept {
  std::uint32_t table_bytes{};
  if (!runtime_.read32(event_table_anchor, table) ||
      !runtime_.read32(event_table_size_anchor, table_bytes)) {
    return false;
  }
  count = table_bytes / event_stride;
  if (table != 0U && table_bytes % event_stride == 0U && count != 0U &&
      count <= 0x1000U) {
    return true;
  }

  table = default_event_table;
  count = default_event_count;
  for (std::uint32_t index{}; index < count; ++index) {
    for (std::uint32_t word{}; word < event_stride; word += 4U) {
      if (!runtime_.write32(table + index * event_stride + word, 0U)) {
        return false;
      }
    }
  }
  return runtime_.write32(event_table_anchor, table) &&
         runtime_.write32(event_table_size_anchor, count * event_stride);
}

bool BiosHle::queueEventCallback(std::uint32_t handler) noexcept {
  if (handler == 0U || event_callback_count_ >= event_callback_queue_.size()) {
    return false;
  }
  const auto tail =
      static_cast<std::size_t>((event_callback_head_ + event_callback_count_) %
                               event_callback_queue_.size());
  event_callback_queue_[tail] = handler;
  ++event_callback_count_;
  return true;
}

bool BiosHle::deliverEvent(std::uint32_t event_class,
                           std::uint32_t spec) noexcept {
  std::uint32_t table{};
  std::uint32_t count{};
  if (!eventTable(table, count)) {
    return false;
  }
  for (std::uint32_t index{}; index < count; ++index) {
    const auto candidate = table + index * event_stride;
    std::uint32_t stored_class{};
    std::uint32_t status{};
    std::uint32_t stored_spec{};
    std::uint32_t mode{};
    std::uint32_t handler{};
    if (!runtime_.read32(candidate, stored_class) ||
        !runtime_.read32(candidate + event_status_offset, status) ||
        !runtime_.read32(candidate + event_spec_offset, stored_spec) ||
        !runtime_.read32(candidate + event_mode_offset, mode) ||
        !runtime_.read32(candidate + event_handler_offset, handler)) {
      return false;
    }
    if (status != event_enabled || stored_class != event_class ||
        stored_spec != spec) {
      continue;
    }
    if (mode == event_callback_mode && handler != 0U) {
      if (!queueEventCallback(handler)) {
        return false;
      }
    } else if (mode == event_mark_mode &&
               !runtime_.write32(candidate + event_status_offset,
                                 event_ready)) {
      return false;
    }
  }
  return true;
}

bool BiosHle::atEventCallbackReturn() const noexcept {
  return event_callback_active_ && runtime_.atReturnSentinel();
}

bool BiosHle::completeEventCallback() noexcept {
  if (!atEventCallbackReturn()) {
    return false;
  }
  runtime_.settleLoadDelay();
  runtime_.restoreCpuState(event_callback_resume_cpu_,
                           event_callback_resume_pgxp_);
  event_callback_resume_pgxp_ = {};
  event_callback_active_ = false;
  return true;
}

BiosAsyncServiceResult BiosHle::serviceAsync() noexcept {
  if (exception_active_ || event_callback_active_) {
    return BiosAsyncServiceResult::idle;
  }

  if (state_.memory_card.pending.kind != MemoryCardPendingKind::none) {
    const auto before = std::unique_ptr<MemoryCardHleState>{
        new (std::nothrow) MemoryCardHleState{state_.memory_card}};
    if (!before) {
      return BiosAsyncServiceResult::failed;
    }
    auto completion =
        MemoryCardHle::servicePending(runtime_, state_.memory_card);
    if (!completion.completed) {
      return BiosAsyncServiceResult::failed;
    }
    if (completion.success && completion.write && !commitMemoryCard()) {
      state_.memory_card = *before;
      state_.memory_card.pending = {};
      completion.success = false;
    }
    const auto spec = completion.success ? 0x00000004U : 0x00008000U;
    constexpr std::uint32_t low_level_card = 0xf0000011U;
    constexpr std::uint32_t backup_unit = 0xf4000001U;
    if (!deliverEvent(low_level_card, spec)) {
      return BiosAsyncServiceResult::failed;
    }
    if (completion.write) {
      if (!deliverEvent(completion.descriptor, spec) ||
          !deliverEvent(backup_unit, spec)) {
        return BiosAsyncServiceResult::failed;
      }
    } else if (!deliverEvent(backup_unit, spec) ||
               !deliverEvent(completion.descriptor, spec)) {
      return BiosAsyncServiceResult::failed;
    }
    return BiosAsyncServiceResult::progressed;
  }

  if (event_callback_count_ == 0U) {
    return BiosAsyncServiceResult::idle;
  }
  const auto handler = event_callback_queue_[event_callback_head_];
  event_callback_resume_cpu_ = runtime_.state();
  event_callback_resume_pgxp_ = runtime_.capturePgxpTransformCheckpoint();
  auto callback_cpu = event_callback_resume_cpu_;
  callback_cpu.gpr[29U] = kernel_interrupt_stack;
  callback_cpu.gpr[30U] = kernel_interrupt_stack;
  callback_cpu.gpr[28U] = kernel_interrupt_global_pointer;
  runtime_.restoreCpuState(callback_cpu);
  if (!runtime_.beginCall(handler)) {
    runtime_.restoreCpuState(event_callback_resume_cpu_,
                             event_callback_resume_pgxp_);
    event_callback_resume_pgxp_ = {};
    return BiosAsyncServiceResult::failed;
  }
  event_callback_head_ = static_cast<std::uint8_t>(
      (event_callback_head_ + 1U) % event_callback_queue_.size());
  --event_callback_count_;
  event_callback_active_ = true;
  return BiosAsyncServiceResult::progressed;
}

bool BiosHle::handleCall() noexcept {
  const auto &cpu = runtime_.state();
  const auto vector = cpu.pc;
  const auto call = cpu.gpr[9];

  if (vector == b0_vector && call >= 0x07U && call <= 0x0dU) {
    std::uint32_t table{};
    std::uint32_t table_bytes{};
    if (!runtime_.read32(event_table_anchor, table) ||
        !runtime_.read32(event_table_size_anchor, table_bytes)) {
      return false;
    }
    auto event_count = table_bytes / event_stride;
    if (table == 0U || table_bytes % event_stride != 0U || event_count == 0U ||
        event_count > 0x1000U) {
      table = default_event_table;
      event_count = default_event_count;
      for (std::uint32_t index = 0U; index < event_count; ++index) {
        for (std::uint32_t word = 0U; word < event_stride; word += 4U) {
          if (!runtime_.write32(table + index * event_stride + word, 0U)) {
            return false;
          }
        }
      }
      if (!runtime_.write32(event_table_anchor, table) ||
          !runtime_.write32(event_table_size_anchor,
                            event_count * event_stride)) {
        return false;
      }
    }
    const auto eventAddress = [&](std::uint32_t handle,
                                  std::uint32_t &address) {
      if ((handle & 0xffff0000U) != event_handle_base) {
        return false;
      }
      const auto index = handle & 0xffffU;
      if (index >= event_count) {
        return false;
      }
      address = table + index * event_stride;
      return true;
    };

    if (call == 0x08U) {
      std::uint32_t slot = event_count;
      for (std::uint32_t index = 0U; index < event_count; ++index) {
        std::uint32_t status{};
        if (!runtime_.read32(table + index * event_stride + event_status_offset,
                             status)) {
          return false;
        }
        if (status == event_free) {
          slot = index;
          break;
        }
      }
      if (slot == event_count) {
        completeCall(0xffffffffU);
        return true;
      }
      const auto event = table + slot * event_stride;
      if (!runtime_.write32(event, cpu.gpr[4]) ||
          !runtime_.write32(event + event_spec_offset, cpu.gpr[5]) ||
          !runtime_.write32(event + event_mode_offset, cpu.gpr[6]) ||
          !runtime_.write32(event + event_handler_offset, cpu.gpr[7]) ||
          !runtime_.write32(event + event_status_offset, event_disabled)) {
        return false;
      }
      completeCall(event_handle_base | slot);
      return true;
    }

    std::uint32_t event{};
    if (call != 0x07U && !eventAddress(cpu.gpr[4], event)) {
      // The retail BIOS reports success for CloseEvent, EnableEvent and
      // DisableEvent even when the descriptor is unused or invalid.  Games
      // rely on this to tear down optional subsystems whose event handle is
      // still zero.  Invalid descriptors must not address or mutate an EvCB.
      if (call == 0x09U || call == 0x0cU || call == 0x0dU) {
        completeCall(1U);
        return true;
      }
      return false;
    }
    if (call == 0x09U) {
      if (!runtime_.write32(event + event_status_offset, event_free)) {
        return false;
      }
      completeCall(1U);
      return true;
    }
    if (call == 0x0aU || call == 0x0bU) {
      std::uint32_t status{};
      if (!runtime_.read32(event + event_status_offset, status)) {
        return false;
      }
      if (call == 0x0aU && status != event_ready && machine_ != nullptr) {
        std::uint32_t event_class{};
        std::uint32_t spec{};
        std::uint32_t mode{};
        if (!runtime_.read32(event, event_class) ||
            !runtime_.read32(event + event_spec_offset, spec) ||
            !runtime_.read32(event + event_mode_offset, mode)) {
          return false;
        }
        constexpr std::uint32_t hardware_spu = 0xf0000009U;
        constexpr std::uint32_t command_completed = 0x0020U;
        if (event_class == hardware_spu && spec == command_completed &&
            mode == event_mark_mode) {
          if (const auto deadline =
                  machine_->dmaCompletionTick(DmaChannel::spu)) {
            const auto now = machine_->currentTick();
            if (*deadline > now) {
              machine_->advanceTicks(*deadline - now);
            }
          }
          if (machine_->dma().scheduledToken(DmaChannel::spu) == 0U) {
            status = event_ready;
          }
        }
      }
      if (status == event_ready) {
        if (!runtime_.write32(event + event_status_offset, event_enabled)) {
          return false;
        }
        completeCall(1U);
        return true;
      }
      if (call == 0x0aU) {
        return false;
      }
      completeCall(0U);
      return true;
    }
    if (call == 0x0cU || call == 0x0dU) {
      std::uint32_t status{};
      if (!runtime_.read32(event + event_status_offset, status)) {
        return false;
      }
      if (status != event_free &&
          !runtime_.write32(event + event_status_offset,
                            call == 0x0cU ? event_enabled : event_disabled)) {
        return false;
      }
      completeCall(1U);
      return true;
    }
    if (call == 0x07U) {
      if (!deliverEvent(cpu.gpr[4U], cpu.gpr[5U])) {
        return false;
      }
      completeCall(0U);
      return true;
    }
  }

  if (vector == a0_vector && call == 0x44U) {
    completeCall(0U);
    return true;
  }

  if (vector == a0_vector && call == 0x49U) {
    const auto command = cpu.gpr[4];
    if (!runtime_.write32(gpu_gp0, command)) {
      return false;
    }
    state_.last_gpu_command = command;
    ++state_.gpu_command_count;
    completeCall(0U);
    return true;
  }
  if (vector == a0_vector && call == 0x70U) {
    state_.memory_card_filesystem_initialized = true;
    completeCall(0U);
    return true;
  }
  if (vector == a0_vector && call == 0x72U) {
    completeCall(0U);
    return true;
  }
  if (vector == b0_vector && call == 0x17U && exception_active_) {
    exception_pending_interrupts_ = 0U;
    return restoreInterruptedCpu();
  }
  if (vector == b0_vector && call == 0x18U) {
    state_.interrupt_entry = 0U;
    completeCall(default_interrupt_entry);
    return true;
  }

  if (vector == b0_vector && call == 0x19U) {
    state_.interrupt_entry =
        cpu.gpr[4] == default_interrupt_entry ? 0U : cpu.gpr[4];
    completeCall(0U);
    return true;
  }
  if (((vector == b0_vector && call == 0x35U) ||
       (vector == a0_vector && call == 0x03U)) &&
      cpu.gpr[4U] < 2U) {
    completeCall(cpu.gpr[6U]);
    return true;
  }
  if (MemoryCardHle::handlesCall(vector, call)) {
    const auto memory_card_before = std::unique_ptr<MemoryCardHleState>{
        new (std::nothrow) MemoryCardHleState{state_.memory_card}};
    if (!memory_card_before) {
      return false;
    }
    const auto memory_card =
        MemoryCardHle::handleCall(runtime_, state_.memory_card, vector, call);
    if (!memory_card.handled) {
      return false;
    }
    auto result = memory_card.result;
    if (state_.memory_card.dirty_generation !=
            memory_card_before->dirty_generation &&
        !commitMemoryCard()) {
      state_.memory_card = *memory_card_before;
      result = memory_card.failure_result;
    }
    completeCall(result);
    return true;
  }
  if (vector == b0_vector && call == 0x4aU) {
    state_.memory_card_init_mode = cpu.gpr[4];
    state_.memory_card_initialized = true;
    completeCall(0U);
    return true;
  }
  if (vector == b0_vector && call == 0x4bU) {
    state_.memory_card_started = true;
    state_.clear_pad = 1U;
    completeCall(1U);
    return true;
  }
  if (vector == b0_vector && call == 0x4cU) {
    state_.memory_card_started = false;
    completeCall(1U);
    return true;
  }
  if (vector == b0_vector && call == 0x56U) {
    completeCall(c0_table);
    return true;
  }
  if (vector == b0_vector && call == 0x57U) {
    completeCall(b0_table);
    return true;
  }
  if (vector == b0_vector && call == 0x5bU) {
    state_.clear_pad = cpu.gpr[4];
    completeCall(0U);
    return true;
  }
  if (vector == c0_vector && call == 0x02U) {
    const auto priority = cpu.gpr[4];
    if (priority >= state_.interrupt_routines.size()) {
      return false;
    }
    if (cpu.gpr[5] == 0U ||
        !runtime_.write32(cpu.gpr[5], state_.interrupt_routines[priority])) {
      return false;
    }
    state_.interrupt_routines[priority] = cpu.gpr[5];
    completeCall(0U);
    return true;
  }
  if (vector == c0_vector && call == 0x03U) {
    const auto priority = cpu.gpr[4];
    if (priority >= state_.interrupt_routines.size()) {
      return false;
    }
    auto current = state_.interrupt_routines[priority];
    std::uint32_t previous{};
    for (std::uint32_t count = 0U; current != 0U && count < 64U; ++count) {
      std::uint32_t next{};
      if (!runtime_.read32(current, next)) {
        return false;
      }
      if (current == cpu.gpr[5]) {
        if (previous == 0U) {
          state_.interrupt_routines[priority] = next;
        } else if (!runtime_.write32(previous, next)) {
          return false;
        }
        current = 0U;
        break;
      }
      previous = current;
      current = next;
    }
    if (current != 0U) {
      return false;
    }
    completeCall(0U);
    return true;
  }
  if (vector == c0_vector && call == 0x0aU) {
    const auto counter = cpu.gpr[4];
    if (counter >= state_.clear_root_counter.size()) {
      return false;
    }
    state_.clear_root_counter[counter] = cpu.gpr[5];
    completeCall(0U);
    return true;
  }
  return false;
}

bool BiosHle::handleKernelSyscall() noexcept {
  const auto pgxp = runtime_.capturePgxpTransformCheckpoint();
  auto cpu = runtime_.state();
  const auto function = cpu.gpr[4];
  if (function == 1U) {
    const auto old_interrupt_enable = cpu.cop0_status & 1U;
    cpu.cop0_status &= ~1U;
    cpu.gpr[2] = old_interrupt_enable;
    runtime_.restoreCpuState(cpu, pgxp);
    return true;
  }
  if (function == 2U) {
    cpu.cop0_status |= 0x0401U;
    cpu.gpr[2] = 0U;
    runtime_.restoreCpuState(cpu, pgxp);
    return true;
  }
  return false;
}

} // namespace sf::psx
