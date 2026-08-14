#include "sf/psx/bios_hle.hpp"
#include "sf/psx/machine.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

void prepareCall(sf::psx::R3000Runtime &runtime, std::uint32_t vector,
                 std::uint32_t call) {
  runtime.reset(vector);
  runtime.setRegister(9U, call);
  runtime.setRegister(31U, 0x80012340U);
}

void writeCString(sf::psx::R3000Runtime &runtime, std::uint32_t address,
                  std::string_view text) {
  for (std::size_t index{}; index < text.size(); ++index) {
    require(runtime.write8(address + static_cast<std::uint32_t>(index),
                           static_cast<std::uint8_t>(text[index])),
            "Could not write a guest string");
  }
  require(runtime.write8(address + static_cast<std::uint32_t>(text.size()), 0U),
          "Could not terminate a guest string");
}

std::uint32_t invokeBios(sf::psx::BiosHle &bios, sf::psx::R3000Runtime &runtime,
                         std::uint32_t vector, std::uint32_t call,
                         std::uint32_t a0 = 0U, std::uint32_t a1 = 0U,
                         std::uint32_t a2 = 0U, std::uint32_t a3 = 0U) {
  prepareCall(runtime, vector, call);
  runtime.setRegister(4U, a0);
  runtime.setRegister(5U, a1);
  runtime.setRegister(6U, a2);
  runtime.setRegister(7U, a3);
  require(bios.handleCall(), "Expected BIOS call was not handled");
  return runtime.state().gpr[2U];
}

void testBootCalls() {
  sf::psx::R3000Runtime runtime;
  sf::psx::BiosHle bios{runtime};
  prepareCall(runtime, 0x000000b0U, 0x35U);
  runtime.setRegister(6U, 27U);
  require(bios.atCallBoundary() && bios.handleCall(),
          "B0 write was not handled");
  require(runtime.state().gpr[2] == 27U && runtime.state().pc == 0x80012340U,
          "B0 write result or return address mismatch");

  prepareCall(runtime, 0x000000b0U, 0x19U);
  runtime.setRegister(4U, 0x8002f888U);
  require(bios.handleCall() && bios.state().interrupt_entry == 0x8002f888U,
          "HookEntryInt state mismatch");
  prepareCall(runtime, 0x000000b0U, 0x18U);
  require(bios.handleCall() && bios.state().interrupt_entry == 0U &&
              runtime.state().gpr[2U] == 0x00006cf4U &&
              runtime.state().pc == 0x80012340U,
          "ResetEntryInt did not restore the default exception entry");

  prepareCall(runtime, 0x000000b0U, 0x5bU);
  runtime.setRegister(4U, 1U);
  require(bios.handleCall() && bios.state().clear_pad == 1U,
          "ChangeClearPAD state mismatch");

  prepareCall(runtime, 0x000000c0U, 0x0aU);
  runtime.setRegister(4U, 3U);
  runtime.setRegister(5U, 1U);
  require(bios.handleCall() && bios.state().clear_root_counter[3U] == 1U,
          "ChangeClearRCnt state mismatch");
  constexpr std::uint32_t interrupt_node_a = 0x80035e14U;
  constexpr std::uint32_t interrupt_node_b = 0x80035e24U;
  std::uint32_t next_interrupt_node{};

  prepareCall(runtime, 0x000000c0U, 0x02U);
  runtime.setRegister(4U, 2U);
  runtime.setRegister(5U, interrupt_node_a);
  require(bios.handleCall() &&
              bios.state().interrupt_routines[2U] == interrupt_node_a &&
              runtime.read32(interrupt_node_a, next_interrupt_node) &&
              next_interrupt_node == 0U,
          "SysEnqIntRP first-node state mismatch");

  prepareCall(runtime, 0x000000c0U, 0x02U);
  runtime.setRegister(4U, 2U);
  runtime.setRegister(5U, interrupt_node_b);
  require(bios.handleCall() &&
              bios.state().interrupt_routines[2U] == interrupt_node_b &&
              runtime.read32(interrupt_node_b, next_interrupt_node) &&
              next_interrupt_node == interrupt_node_a,
          "SysEnqIntRP did not link the previous priority head");

  prepareCall(runtime, 0x000000c0U, 0x03U);
  runtime.setRegister(4U, 2U);
  runtime.setRegister(5U, interrupt_node_a);
  require(bios.handleCall() &&
              bios.state().interrupt_routines[2U] == interrupt_node_b &&
              runtime.read32(interrupt_node_b, next_interrupt_node) &&
              next_interrupt_node == 0U,
          "SysDeqIntRP did not unlink a middle node");

  prepareCall(runtime, 0x000000c0U, 0x03U);
  runtime.setRegister(4U, 2U);
  runtime.setRegister(5U, interrupt_node_b);
  require(bios.handleCall() && bios.state().interrupt_routines[2U] == 0U,
          "SysDeqIntRP did not unlink the priority head");

  prepareCall(runtime, 0x000000a0U, 0x44U);
  require(bios.handleCall() && runtime.state().gpr[2U] == 0U &&
              runtime.state().pc == 0x80012340U,
          "FlushCache was not handled as a completed no-op");

  prepareCall(runtime, 0x000000b0U, 0x4aU);
  runtime.setRegister(4U, 1U);
  require(bios.handleCall() && bios.state().memory_card_initialized &&
              bios.state().memory_card_init_mode == 1U &&
              runtime.state().pc == 0x80012340U,
          "InitCARD lifecycle state mismatch");

  prepareCall(runtime, 0x000000b0U, 0x4bU);
  require(bios.handleCall() && bios.state().memory_card_started &&
              bios.state().clear_pad == 1U && runtime.state().gpr[2U] == 1U &&
              runtime.state().pc == 0x80012340U,
          "StartCARD did not enter run state or apply ChangeClearPAD(1)");

  prepareCall(runtime, 0x000000b0U, 0x4cU);
  runtime.setRegister(2U, 0xffffffffU);
  require(bios.handleCall() && !bios.state().memory_card_started &&
              bios.state().clear_pad == 1U && runtime.state().gpr[2U] == 1U &&
              runtime.state().pc == 0x80012340U,
          "StopCARD did not leave the card driver in a stopped state");

  prepareCall(runtime, 0x000000b0U, 0x56U);
  require(bios.handleCall() && runtime.state().gpr[2U] == 0x00000674U &&
              runtime.state().pc == 0x80012340U,
          "GetC0Table did not return the retail kernel table address");

  prepareCall(runtime, 0x000000b0U, 0x57U);
  require(bios.handleCall() && runtime.state().gpr[2U] == 0x00000874U &&
              runtime.state().pc == 0x80012340U,
          "GetB0Table did not return the retail kernel table address");

  prepareCall(runtime, 0x000000a0U, 0x70U);
  require(bios.handleCall() &&
              bios.state().memory_card_filesystem_initialized &&
              runtime.state().pc == 0x80012340U,
          "_bu_init did not initialize the Memory Card file system");

  prepareCall(runtime, 0x000000a0U, 0x72U);
  require(bios.handleCall(), "_96_remove was not handled");
}

void testMemoryCardFiles() {
  sf::psx::R3000Runtime runtime;
  sf::psx::BiosHle bios{runtime};
  constexpr std::uint32_t b0 = 0x000000b0U;
  constexpr std::uint32_t card_path = 0x00004000U;
  constexpr std::uint32_t pattern_path = 0x00004100U;
  constexpr std::uint32_t source = 0x00005000U;
  constexpr std::uint32_t destination = 0x00005200U;
  constexpr std::uint32_t directory_entry = 0x00005400U;
  constexpr std::uint32_t error = 0xffffffffU;

  writeCString(runtime, card_path, "bu00:BASLUS-01270A");
  writeCString(runtime, pattern_path, "bu00:BASLUS-01270*");
  require(invokeBios(bios, runtime, b0, 0x32U, card_path, 0x00010203U) == 2U,
          "Memory Card create did not allocate the first file descriptor");
  const auto descriptor = runtime.state().gpr[2U];

  for (std::uint32_t index{}; index < 0x80U; ++index) {
    require(runtime.write8(source + index,
                           static_cast<std::uint8_t>(index ^ 0x5aU)),
            "Memory Card source fixture setup failed");
  }
  require(invokeBios(bios, runtime, b0, 0x35U, descriptor, source, 0x80U) ==
              0x80U,
          "Synchronous Memory Card write result mismatch");
  require(invokeBios(bios, runtime, b0, 0x33U, descriptor, 0U, 0U) == 0U,
          "Memory Card SEEK_SET failed");
  require(invokeBios(bios, runtime, b0, 0x34U, descriptor, destination,
                     0x80U) == 0x80U,
          "Synchronous Memory Card read result mismatch");
  for (std::uint32_t index{}; index < 0x80U; ++index) {
    std::uint8_t value{};
    require(runtime.read8(destination + index, value) &&
                value == static_cast<std::uint8_t>(index ^ 0x5aU),
            "Memory Card readback data mismatch");
  }

  require(invokeBios(bios, runtime, b0, 0x33U, descriptor, 0U, 0U) == 0U &&
              invokeBios(bios, runtime, b0, 0x33U, descriptor, 123U, 2U) == 0U,
          "Retail Memory Card SEEK_END no-op mismatch");
  require(invokeBios(bios, runtime, b0, 0x35U, descriptor, source, 0x7fU) ==
                  error &&
              bios.state().memory_card.descriptors[descriptor].offset == 0U,
          "Unaligned Memory Card write changed descriptor state");
  require(invokeBios(bios, runtime, b0, 0x32U, card_path, 0x00010203U) == error,
          "Memory Card duplicate create was accepted");

  require(invokeBios(bios, runtime, b0, 0x36U, descriptor) == descriptor,
          "Memory Card close did not return the descriptor");
  const auto async_descriptor =
      invokeBios(bios, runtime, b0, 0x32U, card_path, 0x8001U);
  require(async_descriptor >= 2U && async_descriptor < 16U,
          "Memory Card async reopen failed");
  require(invokeBios(bios, runtime, b0, 0x35U, async_descriptor, source,
                     0x80U) == 0U &&
              bios.state().memory_card.pending.kind ==
                  sf::psx::MemoryCardPendingKind::write &&
              sf::psx::MemoryCardHle::validateState(bios.state().memory_card),
          "Async Memory Card write was not queued with result zero");
  require(invokeBios(bios, runtime, b0, 0x35U, async_descriptor, source,
                     0x80U) == error,
          "Memory Card accepted a second operation while async I/O was busy");

  const auto open_mark_event = [&](std::uint32_t event_class) {
    const auto handle =
        invokeBios(bios, runtime, b0, 0x08U, event_class, 4U, 0x2000U, 0U);
    require((handle & 0xffff0000U) == 0xf1000000U,
            "Memory Card mark event allocation failed");
    require(invokeBios(bios, runtime, b0, 0x0cU, handle) == 1U,
            "Memory Card mark event enable failed");
    return handle;
  };
  const auto backup_event = open_mark_event(0xf4000001U);
  const auto file_event = open_mark_event(async_descriptor);
  require(bios.serviceAsync() == sf::psx::BiosAsyncServiceResult::progressed &&
              bios.state().memory_card.pending.kind ==
                  sf::psx::MemoryCardPendingKind::none &&
              bios.state().memory_card.descriptors[async_descriptor].offset ==
                  0x80U,
          "Async Memory Card write did not complete at the service point");
  require(invokeBios(bios, runtime, b0, 0x0bU, backup_event) == 1U &&
              invokeBios(bios, runtime, b0, 0x0bU, file_event) == 1U,
          "Async Memory Card success events were not delivered");

  for (std::uint32_t index{}; index < 0x28U; ++index) {
    require(runtime.write8(directory_entry + index, 0xccU),
            "DIRENTRY sentinel setup failed");
  }
  require(invokeBios(bios, runtime, b0, 0x42U, pattern_path, directory_entry) ==
              directory_entry,
          "Memory Card firstfile did not find the save");
  for (std::size_t index{}; index < 14U; ++index) {
    std::uint8_t value{};
    require(runtime.read8(directory_entry + static_cast<std::uint32_t>(index),
                          value) &&
                value == static_cast<std::uint8_t>("BASLUS-01270A"[index]),
            "Memory Card DIRENTRY name mismatch");
  }
  std::uint32_t attributes{};
  std::uint32_t size{};
  std::uint32_t untouched_next{};
  std::uint32_t lba{};
  std::uint32_t untouched_fourcc{};
  require(runtime.read32(directory_entry + 0x14U, attributes) &&
              runtime.read32(directory_entry + 0x18U, size) &&
              runtime.read32(directory_entry + 0x1cU, untouched_next) &&
              runtime.read32(directory_entry + 0x20U, lba) &&
              runtime.read32(directory_entry + 0x24U, untouched_fourcc) &&
              attributes == 0x50U && size == sf::psx::memory_card_block_size &&
              lba == 0x40U && untouched_next == 0xccccccccU &&
              untouched_fourcc == 0xccccccccU,
          "Memory Card DIRENTRY layout mismatch");
  require(invokeBios(bios, runtime, b0, 0x43U, directory_entry) == 0U,
          "Memory Card nextfile did not report enumeration exhaustion");

  auto invalid_state = bios.state().memory_card;
  invalid_state.descriptors[async_descriptor].file_index = 14U;
  require(!sf::psx::MemoryCardHle::validateState(invalid_state),
          "Memory Card accepted a descriptor referencing an unused file");
  require(invokeBios(bios, runtime, b0, 0x45U, card_path) == 0U,
          "Memory Card erased an open file");
  require(invokeBios(bios, runtime, b0, 0x36U, async_descriptor) ==
                  async_descriptor &&
              invokeBios(bios, runtime, b0, 0x45U, card_path) == 1U &&
              invokeBios(bios, runtime, b0, 0x32U, card_path, 1U) == error,
          "Memory Card erase/reopen lifecycle mismatch");
}

void testMemoryCardCommitRollback() {
  sf::psx::R3000Runtime runtime;
  bool allow_commit{};
  std::size_t commit_count{};
  sf::psx::BiosHle bios{runtime, nullptr,
                        [&](const sf::psx::MemoryCardHleState &) {
                          ++commit_count;
                          return allow_commit;
                        }};
  constexpr std::uint32_t b0 = 0x000000b0U;
  constexpr std::uint32_t card_path = 0x00006000U;
  constexpr std::uint32_t source = 0x00006100U;
  constexpr std::uint32_t error = 0xffffffffU;

  writeCString(runtime, card_path, "bu00:BASLUS-01270C");
  for (std::uint32_t index{}; index < 0x80U; ++index) {
    require(runtime.write8(source + index,
                           static_cast<std::uint8_t>(index ^ 0xa5U)),
            "Memory Card commit fixture setup failed");
  }

  const auto blank = bios.state().memory_card;
  require(invokeBios(bios, runtime, b0, 0x32U, card_path, 0x00010203U) ==
                  error &&
              commit_count == 1U && bios.state().memory_card == blank,
          "Failed Memory Card create commit was not rolled back");

  allow_commit = true;
  const auto descriptor =
      invokeBios(bios, runtime, b0, 0x32U, card_path, 0x00010203U);
  require(descriptor == 2U && commit_count == 2U,
          "Successful Memory Card create was not committed");

  allow_commit = false;
  const auto before_sync_write = bios.state().memory_card;
  require(invokeBios(bios, runtime, b0, 0x35U, descriptor, source, 0x80U) ==
                  error &&
              commit_count == 3U &&
              bios.state().memory_card == before_sync_write,
          "Failed synchronous Memory Card commit changed guest state");

  allow_commit = true;
  require(invokeBios(bios, runtime, b0, 0x36U, descriptor) == descriptor,
          "Memory Card commit test close failed");
  const auto async_descriptor =
      invokeBios(bios, runtime, b0, 0x32U, card_path, 0x8001U);
  require(async_descriptor >= 2U && async_descriptor < 16U,
          "Memory Card commit test async reopen failed");

  const auto open_error_event = [&](std::uint32_t event_class) {
    const auto handle =
        invokeBios(bios, runtime, b0, 0x08U, event_class, 0x8000U, 0x2000U, 0U);
    require((handle & 0xffff0000U) == 0xf1000000U &&
                invokeBios(bios, runtime, b0, 0x0cU, handle) == 1U,
            "Memory Card commit error event setup failed");
    return handle;
  };
  const auto low_error = open_error_event(0xf0000011U);
  const auto backup_error = open_error_event(0xf4000001U);
  const auto file_error = open_error_event(async_descriptor);

  require(invokeBios(bios, runtime, b0, 0x35U, async_descriptor, source,
                     0x80U) == 0U,
          "Async Memory Card commit test submission failed");
  auto expected_after_failure = bios.state().memory_card;
  expected_after_failure.pending = {};
  allow_commit = false;
  const auto commits_before_async = commit_count;
  require(bios.serviceAsync() == sf::psx::BiosAsyncServiceResult::progressed &&
              commit_count == commits_before_async + 1U &&
              bios.state().memory_card == expected_after_failure,
          "Failed async Memory Card commit was not rolled back");
  require(invokeBios(bios, runtime, b0, 0x0bU, low_error) == 1U &&
              invokeBios(bios, runtime, b0, 0x0bU, file_error) == 1U &&
              invokeBios(bios, runtime, b0, 0x0bU, backup_error) == 1U,
          "Failed async Memory Card commit did not deliver error events");
}

void testEventCallbackDispatch() {
  sf::psx::R3000Runtime runtime;
  sf::psx::PsxMachine machine{runtime};
  sf::psx::BiosHle bios{runtime, &machine};
  constexpr std::uint32_t b0 = 0x000000b0U;
  constexpr std::uint32_t event_class = 0xf4000001U;
  constexpr std::uint32_t handler = 0x00010000U;
  constexpr std::uint32_t marker = 0x00011000U;
  constexpr std::array code{
      0x27bdffe8U, // addiu sp, sp, -24
      0xafbf0010U, // sw ra, 16(sp)
      0x24080001U, // addiu t0, zero, 1
      0x3c090001U, // lui t1, 1
      0x35291000U, // ori t1, t1, 0x1000
      0xad280000U, // sw t0, 0(t1)
      0x8fbf0010U, // lw ra, 16(sp)
      0x03e00008U, // jr ra
      0x27bd0018U, // addiu sp, sp, 24 (delay slot)
  };
  for (std::size_t index{}; index < code.size(); ++index) {
    require(runtime.write32(handler + static_cast<std::uint32_t>(index * 4U),
                            code[index]),
            "Event callback code setup failed");
  }

  const auto event =
      invokeBios(bios, runtime, b0, 0x08U, event_class, 4U, 0x1000U, handler);
  require(invokeBios(bios, runtime, b0, 0x0cU, event) == 1U &&
              invokeBios(bios, runtime, b0, 0x07U, event_class, 4U) == 0U,
          "Callback-mode event delivery failed");
  std::uint32_t marker_value{};
  require(runtime.read32(marker, marker_value) && marker_value == 0U &&
              runtime.state().pc == 0x80012340U,
          "Event callback executed inline inside DeliverEvent");

  auto callback_caller = runtime.state();
  callback_caller.gpr[29U] = 0x1f800000U;
  runtime.restoreCpuState(callback_caller);
  require(bios.serviceAsync() == sf::psx::BiosAsyncServiceResult::progressed &&
              runtime.state().pc == handler &&
              runtime.state().gpr[31U] ==
                  sf::psx::R3000Runtime::return_sentinel &&
              runtime.state().gpr[29U] == 0x000085d8U &&
              runtime.state().gpr[30U] == 0x000085d8U &&
              runtime.state().gpr[28U] == 0x0000f450U,
          "Queued event callback did not switch to the BIOS kernel stack");
  for (std::size_t instruction{};
       instruction < 12U && !runtime.atReturnSentinel(); ++instruction) {
    require(machine.step().reason == sf::psx::R3000StopReason::running,
            "Guest event callback faulted");
  }
  std::uint32_t saved_return{};
  require(bios.atEventCallbackReturn() &&
              runtime.read32(0x000085d0U, saved_return) &&
              saved_return == sf::psx::R3000Runtime::return_sentinel &&
              bios.completeEventCallback() &&
              runtime.read32(marker, marker_value) && marker_value == 1U &&
              runtime.state().pc == 0x80012340U &&
              runtime.state().gpr[29U] == 0x1f800000U,
          "Guest event callback did not run or restore CPU context");
}

void testGpuCommand() {
  sf::psx::R3000Runtime runtime;
  sf::psx::BiosHle bios{runtime};
  constexpr std::uint32_t command = 0xe1000400U;

  prepareCall(runtime, 0x000000a0U, 0x49U);
  runtime.setRegister(4U, command);
  require(bios.handleCall(), "GPU_cw was not handled");

  std::uint32_t mmio_value{};
  require(runtime.read32(0x1f801810U, mmio_value) && mmio_value == command &&
              bios.state().last_gpu_command == command &&
              bios.state().gpu_command_count == 1U,
          "GPU_cw did not reach GP0");
}

void testEventCalls() {
  sf::psx::R3000Runtime runtime;
  sf::psx::PsxMachine machine{runtime};
  sf::psx::BiosHle bios{runtime, &machine};
  constexpr std::uint32_t event_class = 0xf0000003U;
  constexpr std::uint32_t event_spec = 0x00000004U;
  constexpr std::uint32_t event_table = 0x0000e000U;

  prepareCall(runtime, 0x000000b0U, 0x08U);
  runtime.setRegister(4U, event_class);
  runtime.setRegister(5U, event_spec);
  runtime.setRegister(6U, 0x2000U);
  runtime.setRegister(7U, 0U);
  require(bios.handleCall() && runtime.state().gpr[2U] == 0xf1000000U,
          "OpenEvent did not allocate the first retail EvCB handle");

  std::uint32_t table_anchor{};
  std::uint32_t table_size{};
  std::uint32_t stored_class{};
  std::uint32_t stored_status{};
  std::uint32_t stored_spec{};
  std::uint32_t stored_mode{};
  require(runtime.read32(0x00000120U, table_anchor) &&
              runtime.read32(0x00000124U, table_size) &&
              runtime.read32(event_table, stored_class) &&
              runtime.read32(event_table + 4U, stored_status) &&
              runtime.read32(event_table + 8U, stored_spec) &&
              runtime.read32(event_table + 0x0cU, stored_mode) &&
              table_anchor == event_table && table_size == 16U * 0x1cU &&
              stored_class == event_class && stored_status == 0x1000U &&
              stored_spec == event_spec && stored_mode == 0x2000U,
          "OpenEvent did not materialize the retail EvCB table");

  prepareCall(runtime, 0x000000b0U, 0x0cU);
  runtime.setRegister(4U, 0xf1000000U);
  require(bios.handleCall() && runtime.state().gpr[2U] == 1U &&
              runtime.read32(event_table + 4U, stored_status) &&
              stored_status == 0x2000U,
          "EnableEvent did not enable the EvCB");

  prepareCall(runtime, 0x000000b0U, 0x07U);
  runtime.setRegister(4U, event_class);
  runtime.setRegister(5U, event_spec);
  require(bios.handleCall() &&
              runtime.read32(event_table + 4U, stored_status) &&
              stored_status == 0x4000U,
          "DeliverEvent did not mark the matching EvCB ready");

  prepareCall(runtime, 0x000000b0U, 0x0bU);
  runtime.setRegister(4U, 0xf1000000U);
  require(bios.handleCall() && runtime.state().gpr[2U] == 1U &&
              runtime.read32(event_table + 4U, stored_status) &&
              stored_status == 0x2000U,
          "TestEvent did not consume and re-arm the ready EvCB");

  prepareCall(runtime, 0x000000b0U, 0x0bU);
  runtime.setRegister(4U, 0xf1000000U);
  require(bios.handleCall() && runtime.state().gpr[2U] == 0U,
          "TestEvent reported an undelivered EvCB as ready");

  prepareCall(runtime, 0x000000b0U, 0x09U);
  runtime.setRegister(4U, 0xf1000000U);
  require(bios.handleCall() && runtime.state().gpr[2U] == 1U &&
              runtime.read32(event_table + 4U, stored_status) &&
              stored_status == 0U,
          "CloseEvent did not release the EvCB");

  prepareCall(runtime, 0x000000b0U, 0x08U);
  runtime.setRegister(4U, 0xf0000009U);
  runtime.setRegister(5U, 0x0020U);
  runtime.setRegister(6U, 0x2000U);
  runtime.setRegister(7U, 0U);
  require(bios.handleCall() && runtime.state().gpr[2U] == 0xf1000000U,
          "OpenEvent did not reuse the released EvCB for SPU completion");
  prepareCall(runtime, 0x000000b0U, 0x0cU);
  runtime.setRegister(4U, 0xf1000000U);
  require(bios.handleCall(), "SPU completion event was not enabled");

  constexpr std::uint32_t dma_source = 0x00006000U;
  constexpr std::uint32_t dma4_base = 0x1f8010c0U;
  constexpr std::uint32_t dma_dpcr = 0x1f8010f0U;
  constexpr std::uint32_t spu_transfer_address = 0x1f801da6U;
  constexpr std::uint32_t spu_control = 0x1f801daaU;
  require(runtime.write32(dma_source, 0x44332211U) &&
              runtime.write16(spu_transfer_address, 0U) &&
              runtime.write16(spu_control, 2U << 4U) &&
              runtime.write32(dma_dpcr, machine.dma().dpcr() | (1U << 19U)) &&
              runtime.write32(dma4_base, dma_source) &&
              runtime.write32(dma4_base + 4U, 0x00010001U) &&
              runtime.write32(dma4_base + 8U, 0x01000201U) &&
              machine.dmaCompletionTick(sf::psx::DmaChannel::spu),
          "Could not schedule the BIOS SPU WaitEvent fixture");

  prepareCall(runtime, 0x000000b0U, 0x0aU);
  runtime.setRegister(4U, 0xf1000000U);
  require(bios.handleCall() && runtime.state().gpr[2U] == 1U &&
              !machine.dmaCompletionTick(sf::psx::DmaChannel::spu) &&
              machine.spu().ram()[0] == std::byte{0x11U} &&
              machine.spu().ram()[3] == std::byte{0x44U} &&
              runtime.read32(event_table + 4U, stored_status) &&
              stored_status == 0x2000U,
          "WaitEvent did not complete DMA4 and re-arm the SPU EvCB");
}

void testDisableEventContract() {
  sf::psx::R3000Runtime runtime;
  sf::psx::BiosHle bios{runtime};
  constexpr std::uint32_t event_table = 0x0000e000U;
  constexpr std::uint32_t event_status = event_table + 4U;
  constexpr std::uint32_t event_handle = 0xf1000000U;

  prepareCall(runtime, 0x000000b0U, 0x08U);
  runtime.setRegister(4U, 0xf0000003U);
  runtime.setRegister(5U, 0x00000004U);
  runtime.setRegister(6U, 0x2000U);
  require(bios.handleCall() && runtime.state().gpr[2U] == event_handle,
          "Could not open the DisableEvent fixture");

  prepareCall(runtime, 0x000000b0U, 0x0cU);
  runtime.setRegister(4U, event_handle);
  std::uint32_t stored_status{};
  require(bios.handleCall() && runtime.state().gpr[2U] == 1U &&
              runtime.read32(event_status, stored_status) &&
              stored_status == 0x2000U,
          "Could not enable the DisableEvent fixture");

  prepareCall(runtime, 0x000000b0U, 0x0dU);
  runtime.setRegister(4U, event_handle);
  require(bios.handleCall() && runtime.state().gpr[2U] == 1U &&
              runtime.state().pc == 0x80012340U &&
              runtime.read32(event_status, stored_status) &&
              stored_status == 0x1000U,
          "DisableEvent did not disable a valid EvCB");

  // Exact retail teardown call-site reported by the runtime: the optional
  // event handle is still zero and the BIOS must return normally to this RA.
  prepareCall(runtime, 0x000000b0U, 0x0dU);
  runtime.setRegister(31U, 0x8006d5c4U);
  runtime.setRegister(4U, 0U);
  require(bios.handleCall() && runtime.state().gpr[2U] == 1U &&
              runtime.state().pc == 0x8006d5c4U &&
              runtime.read32(event_status, stored_status) &&
              stored_status == 0x1000U,
          "Retail zero-handle DisableEvent call did not return safely");

  constexpr std::array invalid_handles{
      0xf0ffffffU,
      0xf1000010U,
      0xffffffffU,
  };
  for (const auto invalid_handle : invalid_handles) {
    prepareCall(runtime, 0x000000b0U, 0x0dU);
    runtime.setRegister(4U, invalid_handle);
    require(bios.handleCall() && runtime.state().gpr[2U] == 1U &&
                runtime.state().pc == 0x80012340U &&
                runtime.read32(event_status, stored_status) &&
                stored_status == 0x1000U,
            "DisableEvent rejected or mutated state for an invalid handle");
  }

  prepareCall(runtime, 0x000000b0U, 0x09U);
  runtime.setRegister(4U, event_handle);
  require(bios.handleCall() && runtime.read32(event_status, stored_status) &&
              stored_status == 0U,
          "Could not release the DisableEvent fixture");

  prepareCall(runtime, 0x000000b0U, 0x0dU);
  runtime.setRegister(4U, event_handle);
  require(bios.handleCall() && runtime.state().gpr[2U] == 1U &&
              runtime.read32(event_status, stored_status) &&
              stored_status == 0U,
          "DisableEvent changed a released EvCB");
}

void testControllerSio() {
  sf::psx::R3000Runtime runtime;
  sf::psx::PsxMachine machine{runtime};
  machine.setControllerButtons(0xfff8U);

  constexpr std::array<std::uint8_t, 4U> neutral_axes{0x80U, 0x80U, 0x80U,
                                                      0x80U};
  require(machine.controllerSio().analog == neutral_axes,
          "Controller SIO axes did not reset to neutral");

  const auto select_controller = [&] {
    require(runtime.write16(0x1f80104aU, 0x0000U) &&
                runtime.write16(0x1f80104aU, 0x0003U),
            "Controller SIO select failed");
  };
  const auto exchange = [&](std::uint8_t command) {
    require(runtime.write8(0x1f801040U, command),
            "Controller SIO command write failed");
    std::uint16_t status{};
    require(runtime.read16(0x1f801044U, status) && (status & 0x0007U) == 0U,
            "Controller SIO transmitter did not become busy");
    machine.advanceHardwareTicks(1'088U);
    require(runtime.read16(0x1f801044U, status) &&
                (status & 0x0007U) == 0x0007U,
            "Controller SIO response did not become ready");
    std::uint8_t response{};
    require(runtime.read8(0x1f801040U, response),
            "Controller SIO response read failed");
    machine.advanceHardwareTicks(100U);
    require(runtime.read16(0x1f801044U, status) &&
                (status & 0x0085U) == 0x0005U,
            "Controller SIO transmitter did not return to idle");
    return response;
  };
  const auto transaction = [&](const auto &request, const auto &expected,
                               const char *message) {
    require(request.size() == expected.size(), message);
    select_controller();
    for (std::size_t index = 0U; index < request.size(); ++index) {
      require(exchange(request[index]) == expected[index], message);
    }
  };

  transaction(std::array<std::uint8_t, 5U>{0x01U, 0x42U, 0x00U, 0x00U, 0x00U},
              std::array<std::uint8_t, 5U>{0xffU, 0x41U, 0x5aU, 0xfeU, 0xffU},
              "Controller SIO digital poll mismatch");

  constexpr std::uint32_t joy_data = 0x1f801040U;
  constexpr std::uint32_t joy_status = 0x1f801044U;
  constexpr std::uint32_t joy_control = 0x1f80104aU;
  constexpr std::uint32_t interrupt_status = 0x1f801070U;
  constexpr std::uint32_t interrupt_mask = 0x1f801074U;
  constexpr std::uint16_t controller_irq = 0x0080U;

  require(runtime.write16(interrupt_mask, controller_irq) &&
              runtime.write16(joy_control, 0x0000U) &&
              runtime.write16(joy_control, 0x1003U) &&
              runtime.write8(joy_data, 0x01U),
          "Controller SIO IRQ fixture setup failed");
  std::uint16_t irq_status{};
  std::uint16_t irq_register{};
  require(runtime.read16(joy_status, irq_status) &&
              (irq_status & 0x0282U) == 0U &&
              runtime.read16(interrupt_status, irq_register) &&
              (irq_register & controller_irq) == 0U,
          "Controller SIO ACK completed without serial delay");
  require(runtime.write16(interrupt_status, 0xff7fU) &&
              runtime.write16(joy_control, 0x1013U),
          "Controller SIO pre-ACK guest sequence failed");
  std::uint16_t control{};
  require(runtime.read16(joy_control, control) && control == 0x1003U,
          "Controller SIO ACK strobe did not self-clear");

  machine.advanceHardwareTicks(1'087U);
  require(runtime.read16(joy_status, irq_status) &&
              (irq_status & 0x0282U) == 0U &&
              runtime.read16(interrupt_status, irq_register) &&
              (irq_register & controller_irq) == 0U,
          "Controller SIO ACK arrived before byte transfer completed");
  machine.advanceHardwareTicks(1U);
  require(runtime.read16(joy_status, irq_status) &&
              (irq_status & 0x0282U) == 0x0282U &&
              runtime.read16(interrupt_status, irq_register) &&
              (irq_register & controller_irq) != 0U &&
              machine.interrupts().cpuLine(),
          "Controller SIO delayed ACK/IRQ7 did not assert");
  std::uint8_t irq_response{};
  require(runtime.read8(joy_data, irq_response) && irq_response == 0xffU,
          "Controller SIO delayed RX response mismatch");
  require(runtime.read16(joy_status, irq_status) &&
              (irq_status & 0x0085U) == 0x0085U &&
              runtime.write8(joy_data, 0x42U) &&
              machine.controllerSio().transfer_token != 0U &&
              machine.controllerSio().ack_input,
          "Controller SIO dropped the next byte during active /ACK");
  require(runtime.write16(joy_control, 0x1013U) &&
              runtime.read16(joy_status, irq_status) &&
              (irq_status & 0x0200U) != 0U,
          "Controller SIO cleared IRQ while /ACK was still active");

  machine.advanceHardwareTicks(99U);
  require(runtime.read16(joy_status, irq_status) &&
              (irq_status & 0x0280U) == 0x0280U,
          "Controller SIO ACK pulse ended too early");
  machine.advanceHardwareTicks(1U);
  require(runtime.read16(joy_status, irq_status) &&
              (irq_status & 0x0080U) == 0U && (irq_status & 0x0200U) != 0U,
          "Controller SIO ACK pulse timing or sticky IRQ mismatch");
  require(runtime.write16(interrupt_status, 0xff7fU) &&
              !machine.interrupts().cpuLine() &&
              runtime.read16(joy_status, irq_status) &&
              (irq_status & 0x0200U) != 0U,
          "Controller I_STAT acknowledgement affected JOY_STAT IRQ");
  require(runtime.write16(joy_control, 0x1013U) &&
              runtime.read16(joy_status, irq_status) &&
              (irq_status & 0x0200U) == 0U &&
              runtime.read16(joy_control, control) && control == 0x1003U,
          "Controller JOY_CTRL acknowledgement mismatch");

  require(runtime.write16(joy_control, 0x0000U) &&
              runtime.write16(joy_control, 0x0003U) &&
              runtime.write8(joy_data, 0x01U),
          "Controller SIO non-IRQ ACK fixture setup failed");
  machine.advanceHardwareTicks(1'088U);
  require(runtime.read16(joy_status, irq_status) &&
              (irq_status & 0x0082U) == 0x0082U &&
              (irq_status & 0x0200U) == 0U &&
              runtime.read16(interrupt_status, irq_register) &&
              (irq_register & controller_irq) == 0U,
          "Controller SIO DSR IRQ gating mismatch");
  machine.advanceHardwareTicks(100U);
  require(runtime.read8(joy_data, irq_response) &&
              runtime.write16(interrupt_mask, 0x0000U),
          "Controller SIO IRQ fixture cleanup failed");

  transaction(std::array<std::uint8_t, 5U>{0x01U, 0x43U, 0x00U, 0x01U, 0x00U},
              std::array<std::uint8_t, 5U>{0xffU, 0x41U, 0x5aU, 0xfeU, 0xffU},
              "Controller SIO config-entry mismatch");
  require(machine.controllerSio().configuration_mode,
          "Controller SIO did not enter config mode");

  constexpr std::array<std::uint8_t, 9U> config_poll_request{
      0x01U, 0x42U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
  constexpr std::array<std::uint8_t, 9U> config_poll_response{
      0xffU, 0xf3U, 0x5aU, 0xf8U, 0xffU, 0x80U, 0x80U, 0x80U, 0x80U};
  transaction(config_poll_request, config_poll_response,
              "Controller SIO config-mode poll mismatch");

  transaction(std::array<std::uint8_t, 9U>{0x01U, 0x46U, 0x00U, 0x00U, 0x00U,
                                           0x00U, 0x00U, 0x00U, 0x00U},
              std::array<std::uint8_t, 9U>{0xffU, 0xf3U, 0x5aU, 0x00U, 0x00U,
                                           0x01U, 0x02U, 0x00U, 0x0aU},
              "Controller SIO actuator-zero query mismatch");
  transaction(std::array<std::uint8_t, 9U>{0x01U, 0x46U, 0x00U, 0x01U, 0x00U,
                                           0x00U, 0x00U, 0x00U, 0x00U},
              std::array<std::uint8_t, 9U>{0xffU, 0xf3U, 0x5aU, 0x00U, 0x00U,
                                           0x01U, 0x01U, 0x01U, 0x14U},
              "Controller SIO actuator-one query mismatch");
  transaction(std::array<std::uint8_t, 9U>{0x01U, 0x47U, 0x00U, 0x00U, 0x00U,
                                           0x00U, 0x00U, 0x00U, 0x00U},
              std::array<std::uint8_t, 9U>{0xffU, 0xf3U, 0x5aU, 0x00U, 0x00U,
                                           0x02U, 0x00U, 0x01U, 0x00U},
              "Controller SIO capability query mismatch");
  transaction(std::array<std::uint8_t, 9U>{0x01U, 0x48U, 0x00U, 0x00U, 0x00U,
                                           0x00U, 0x00U, 0x00U, 0x00U},
              std::array<std::uint8_t, 9U>{0xffU, 0xf3U, 0x5aU, 0x00U, 0x00U,
                                           0x00U, 0x00U, 0x01U, 0x00U},
              "Controller SIO 0x48 query mismatch");
  transaction(std::array<std::uint8_t, 9U>{0x01U, 0x4bU, 0x00U, 0x00U, 0x00U,
                                           0x00U, 0x00U, 0x00U, 0x00U},
              std::array<std::uint8_t, 9U>{0xffU, 0xf3U, 0x5aU, 0x00U, 0x00U,
                                           0x00U, 0x00U, 0x00U, 0x00U},
              "Controller SIO unused config command mismatch");
  transaction(std::array<std::uint8_t, 9U>{0x01U, 0x4cU, 0x00U, 0x00U, 0x00U,
                                           0x00U, 0x00U, 0x00U, 0x00U},
              std::array<std::uint8_t, 9U>{0xffU, 0xf3U, 0x5aU, 0x00U, 0x00U,
                                           0x00U, 0x04U, 0x00U, 0x00U},
              "Controller SIO mode-zero query mismatch");
  transaction(std::array<std::uint8_t, 9U>{0x01U, 0x4cU, 0x00U, 0x01U, 0x00U,
                                           0x00U, 0x00U, 0x00U, 0x00U},
              std::array<std::uint8_t, 9U>{0xffU, 0xf3U, 0x5aU, 0x00U, 0x00U,
                                           0x00U, 0x07U, 0x00U, 0x00U},
              "Controller SIO mode-one query mismatch");

  constexpr std::array<std::uint8_t, 9U> rumble_map_request{
      0x01U, 0x4dU, 0x00U, 0x00U, 0x01U, 0xffU, 0xffU, 0xffU, 0xffU};
  transaction(rumble_map_request,
              std::array<std::uint8_t, 9U>{0xffU, 0xf3U, 0x5aU, 0xffU, 0xffU,
                                           0xffU, 0xffU, 0xffU, 0xffU},
              "Controller SIO initial rumble map mismatch");
  transaction(rumble_map_request,
              std::array<std::uint8_t, 9U>{0xffU, 0xf3U, 0x5aU, 0x00U, 0x01U,
                                           0xffU, 0xffU, 0xffU, 0xffU},
              "Controller SIO persisted rumble map mismatch");

  constexpr std::array<std::uint8_t, 9U> config_request{
      0x01U, 0x44U, 0x00U, 0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U};
  constexpr std::array<std::uint8_t, 9U> config_response{
      0xffU, 0xf3U, 0x5aU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
  constexpr std::array<std::uint8_t, 9U> config_exit_request{
      0x01U, 0x43U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
  transaction(config_request, config_response,
              "Controller SIO analog-mode command mismatch");
  require(machine.controllerSio().analog_mode &&
              machine.controllerSio().analog_locked,
          "Controller SIO did not enable and lock analog mode");

  constexpr std::array<std::uint8_t, 9U> mode_query_request{
      0x01U, 0x45U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
  constexpr std::array<std::uint8_t, 9U> mode_query_response{
      0xffU, 0xf3U, 0x5aU, 0x01U, 0x02U, 0x01U, 0x02U, 0x01U, 0x00U};
  transaction(mode_query_request, mode_query_response,
              "Controller SIO analog-mode query mismatch");

  transaction(config_exit_request, config_response,
              "Controller SIO config-exit mismatch");
  require(!machine.controllerSio().configuration_mode,
          "Controller SIO did not leave config mode");

  constexpr std::array<std::uint8_t, 4U> analog_axes{0x10U, 0x20U, 0x30U,
                                                     0x40U};
  machine.setControllerState(0x1234U, analog_axes);
  constexpr std::array<std::uint8_t, 9U> poll_request{
      0x01U, 0x42U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
  constexpr std::array<std::uint8_t, 9U> analog_response{
      0xffU, 0x73U, 0x5aU, 0x34U, 0x12U, 0x10U, 0x20U, 0x30U, 0x40U};
  transaction(poll_request, analog_response,
              "Controller SIO analog poll mismatch");

  select_controller();
  require(exchange(0x01U) == 0xffU && exchange(0x42U) == 0x73U &&
              exchange(0x00U) == 0x5aU && exchange(0x00U) == 0x34U,
          "Controller SIO pre-snapshot poll mismatch");
  const auto snapshot = machine.captureState();
  machine.setControllerState(0xffffU, neutral_axes);
  require(machine.restoreState(snapshot) &&
              machine.controllerSio() == snapshot.controller_sio,
          "Controller SIO mid-packet snapshot restore mismatch");
  require(exchange(0x00U) == 0x12U && exchange(0x00U) == 0x10U &&
              exchange(0x00U) == 0x20U && exchange(0x00U) == 0x30U &&
              exchange(0x00U) == 0x40U,
          "Controller SIO restored analog poll mismatch");

  constexpr std::array<std::uint8_t, 9U> config_enter_request{
      0x01U, 0x43U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
  transaction(config_enter_request, analog_response,
              "Controller SIO analog config-entry mismatch");
  constexpr std::array<std::uint8_t, 9U> digital_mode_request{
      0x01U, 0x44U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
  transaction(digital_mode_request, config_response,
              "Controller SIO digital-mode command mismatch");
  require(!machine.controllerSio().analog_mode &&
              !machine.controllerSio().analog_locked,
          "Controller SIO did not disable and unlock analog mode");
  transaction(config_exit_request, config_response,
              "Controller SIO digital config-exit mismatch");
  transaction(std::array<std::uint8_t, 5U>{0x01U, 0x42U, 0x00U, 0x00U, 0x00U},
              std::array<std::uint8_t, 5U>{0xffU, 0x41U, 0x5aU, 0x36U, 0x12U},
              "Controller SIO post-config digital poll mismatch");

  std::uint16_t status{};
  require(runtime.read16(0x1f801044U, status) && (status & 0x0002U) == 0U,
          "Controller SIO RX-ready did not clear after data read");

  select_controller();
  require(runtime.write8(joy_data, 0x01U),
          "Controller SIO pending snapshot fixture failed");
  const auto pending_snapshot = machine.captureState();
  require(machine.validateState(pending_snapshot),
          "Controller SIO rejected a valid pending transfer snapshot");
  auto corrupt_deadline = pending_snapshot;
  auto corrupted_event_found = false;
  for (std::size_t index = 0U; index < corrupt_deadline.scheduler.event_count;
       ++index) {
    auto &event = corrupt_deadline.scheduler.events[index];
    if (event.token == corrupt_deadline.controller_sio.transfer_token) {
      event.deadline = std::numeric_limits<std::uint64_t>::max();
      corrupted_event_found = true;
      break;
    }
  }
  require(corrupted_event_found && !machine.validateState(corrupt_deadline) &&
              !machine.restoreState(corrupt_deadline) &&
              machine.controllerSio() == pending_snapshot.controller_sio,
          "Controller SIO accepted a transfer with an unbounded deadline");
  require(runtime.write16(joy_control, 0x0000U),
          "Controller SIO pending snapshot cleanup failed");
}

void testCriticalSections() {
  sf::psx::R3000Runtime runtime;
  sf::psx::BiosHle bios{runtime};
  runtime.reset(0x80010004U);
  auto cpu = runtime.state();
  cpu.cop0_status = 0x0401U;
  cpu.gpr[4] = 1U;
  runtime.restoreCpuState(cpu);

  require(bios.handleKernelSyscall(), "EnterCriticalSection was not handled");
  require((runtime.state().cop0_status & 1U) == 0U &&
              runtime.state().gpr[2] == 1U,
          "EnterCriticalSection state mismatch");

  runtime.setRegister(4U, 2U);
  require(bios.handleKernelSyscall(), "ExitCriticalSection was not handled");
  require((runtime.state().cop0_status & 0x0401U) == 0x0401U &&
              runtime.state().gpr[2] == 0U,
          "ExitCriticalSection state mismatch");
}

void testInterruptDispatch() {
  sf::psx::R3000Runtime runtime;
  sf::psx::PsxMachine machine{runtime};
  sf::psx::BiosHle bios{runtime};

  prepareCall(runtime, 0x000000b0U, 0x18U);
  require(bios.handleCall(), "ResetEntryInt default fixture failed");
  const auto default_entry = runtime.state().gpr[2U];
  prepareCall(runtime, 0x000000b0U, 0x19U);
  runtime.setRegister(4U, default_entry);
  require(bios.handleCall() && bios.state().interrupt_entry == 0U,
          "HookEntryInt did not recognize ResetEntryInt's default context");

  machine.pulseVBlank();
  constexpr std::uint32_t node = 0x80035e14U;
  constexpr std::uint32_t predicate = 0x8002533cU;
  constexpr std::uint32_t handler = 0x800253a4U;

  prepareCall(runtime, 0x000000c0U, 0x02U);
  runtime.setRegister(4U, 2U);
  runtime.setRegister(5U, node);
  require(bios.handleCall() && runtime.write32(node + 4U, handler) &&
              runtime.write32(node + 8U, predicate),
          "Interrupt routine setup failed");

  constexpr std::array predicate_code{
      0x27bdffe8U, // addiu sp, sp, -24
      0xafbf0010U, // sw ra, 16(sp)
      0x24020001U, // addiu v0, zero, 1
      0x8fbf0010U, // lw ra, 16(sp)
      0x03e00008U, // jr ra
      0x27bd0018U, // addiu sp, sp, 24 (delay slot)
  };
  for (std::size_t index{}; index < predicate_code.size(); ++index) {
    require(runtime.write32(predicate + static_cast<std::uint32_t>(index * 4U),
                            predicate_code[index]),
            "Interrupt FIRST callback code setup failed");
  }

  runtime.reset(0x80000080U);
  auto interrupted = runtime.state();
  interrupted.gpr[31] = 0x80016600U;
  interrupted.gpr[29] = 0x1f800000U;
  interrupted.cop0_epc = 0x80017780U;
  interrupted.cop0_status = 0x0404U;
  runtime.restoreCpuState(interrupted);

  require(bios.atExceptionBoundary() && bios.dispatchException() &&
              runtime.state().pc == predicate &&
              runtime.state().gpr[31] == sf::psx::R3000Runtime::return_sentinel,
          "Interrupt FIRST callback was not dispatched");
  require(runtime.state().gpr[29U] == 0x000085d8U &&
              runtime.state().gpr[30U] == 0x000085d8U &&
              runtime.state().gpr[28U] == 0x0000f450U,
          "Interrupt callbacks did not switch to the BIOS kernel stack");
  for (std::size_t instruction{};
       instruction < 8U && !runtime.atReturnSentinel(); ++instruction) {
    const auto execution = machine.step();
    require(execution.reason == sf::psx::R3000StopReason::running,
            "Interrupt FIRST callback faulted on the BIOS kernel stack");
  }
  std::uint32_t saved_return{};
  require(runtime.atReturnSentinel() &&
              runtime.read32(0x000085d0U, saved_return) &&
              saved_return == sf::psx::R3000Runtime::return_sentinel,
          "Interrupt FIRST callback did not preserve its return on kernel RAM");
  require(bios.atExceptionReturn() && bios.completeException() &&
              runtime.state().pc == handler && runtime.state().gpr[4U] == 1U &&
              runtime.state().gpr[31U] ==
                  sf::psx::R3000Runtime::return_sentinel,
          "Interrupt SECOND callback was not dispatched");
  runtime.completeHostCall();
  require(bios.atExceptionReturn() && bios.completeException(),
          "Interrupt SECOND callback return was not completed");
  require(runtime.state().pc == 0x80017780U &&
              runtime.state().gpr[31] == 0x80016600U &&
              runtime.state().gpr[29] == 0x1f800000U &&
              runtime.state().cop0_status == 0x0401U &&
              machine.interrupts().status() == 0U &&
              !runtime.interruptPending(),
          "Interrupt CPU context or RFE status mismatch");
}

void testHookedInterruptDispatch() {
  sf::psx::R3000Runtime runtime;
  sf::psx::PsxMachine machine{runtime};
  sf::psx::BiosHle bios{runtime};
  constexpr std::uint32_t context = 0x80035e40U;
  constexpr std::uint32_t saved_ra = 0x8001e49cU;
  constexpr std::uint32_t node = 0x80035e14U;
  constexpr std::uint32_t predicate = 0x8002533cU;
  constexpr std::uint32_t handler = 0x800253a4U;

  prepareCall(runtime, 0x000000b0U, 0x19U);
  runtime.setRegister(4U, context);
  require(bios.handleCall(), "HookEntryInt was not handled");
  require(runtime.write32(context, saved_ra) &&
              runtime.write32(context + 4U, 0x801ffe00U) &&
              runtime.write32(context + 8U, 0x801ffe40U) &&
              runtime.write32(context + 0x2cU, 0x8003739cU),
          "HookEntryInt context setup failed");
  for (std::uint32_t index = 0U; index < 8U; ++index) {
    require(runtime.write32(context + 0x0cU + index * 4U, 0x11110000U + index),
            "HookEntryInt saved register setup failed");
  }

  prepareCall(runtime, 0x000000c0U, 0x02U);
  runtime.setRegister(4U, 2U);
  runtime.setRegister(5U, node);
  require(bios.handleCall() && runtime.write32(node + 4U, handler) &&
              runtime.write32(node + 8U, predicate),
          "Interrupt routine setup with HookEntryInt failed");

  machine.pulseVBlank();
  runtime.reset(0x80000080U);
  auto interrupted = runtime.state();
  interrupted.gpr[31U] = 0x80016600U;
  interrupted.cop0_epc = 0x80017780U;
  interrupted.cop0_status = 0x0404U;
  runtime.restoreCpuState(interrupted);

  require(bios.dispatchException() && runtime.state().pc == predicate &&
              runtime.state().gpr[31U] ==
                  sf::psx::R3000Runtime::return_sentinel,
          "HookEntryInt ran before interrupt routines");

  runtime.setRegister(2U, 1U);
  runtime.completeHostCall();
  require(bios.atExceptionReturn() && bios.completeException() &&
              runtime.state().pc == handler && runtime.state().gpr[4U] == 1U,
          "Interrupt SECOND did not follow FIRST before HookEntryInt");
  runtime.completeHostCall();
  require(bios.atExceptionReturn() && bios.completeException() &&
              runtime.state().pc == saved_ra && runtime.state().gpr[2U] == 1U &&
              runtime.state().gpr[29U] == 0x801ffe00U &&
              runtime.state().gpr[16U] == 0x11110000U,
          "HookEntryInt context was not restored after interrupt routines");

  require(runtime.write16(0x1f801070U, 0xfffeU),
          "Guest interrupt acknowledgement failed");
  auto return_call = runtime.state();
  return_call.pc = 0x000000b0U;
  return_call.next_pc = 0x000000b4U;
  return_call.gpr[9U] = 0x17U;
  runtime.restoreCpuState(return_call);
  require(bios.handleCall() && runtime.state().pc == 0x80017780U &&
              runtime.state().gpr[31U] == 0x80016600U &&
              runtime.state().cop0_status == 0x0401U &&
              machine.interrupts().status() == 0U &&
              !runtime.interruptPending(),
          "ReturnFromException did not restore interrupted CPU");
}

void testUnsupportedCall() {
  sf::psx::R3000Runtime runtime;
  sf::psx::BiosHle bios{runtime};
  prepareCall(runtime, 0x000000a0U, 0xffU);
  require(!bios.handleCall() && runtime.state().pc == 0x000000a0U,
          "Unsupported BIOS call changed CPU state");
  runtime.setRegister(4U, 99U);
  require(!bios.handleKernelSyscall(),
          "Unsupported kernel syscall was accepted");
}

} // namespace

int main() {
  try {
    testBootCalls();
    testMemoryCardFiles();
    testMemoryCardCommitRollback();
    testGpuCommand();
    testEventCalls();
    testDisableEventContract();
    testEventCallbackDispatch();
    testCriticalSections();
    testControllerSio();
    testInterruptDispatch();
    testHookedInterruptDispatch();
    testUnsupportedCall();
    std::cout << "BIOS HLE tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "BIOS HLE test failure: " << error.what() << '\n';
    return 1;
  }
}
