#include "sf/psx/r3000_runtime.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace sf::psx {
namespace {

constexpr std::uint32_t ram_mirror_end = 0x00800000U;

enum class BlockTermination : std::uint8_t {
  none,
  after_instruction,
  after_delay_slot,
};

BlockTermination blockTermination(std::uint32_t instruction) noexcept {
  const auto opcode = static_cast<std::uint8_t>(instruction >> 26U);
  if (opcode == 0U) {
    const auto function = static_cast<std::uint8_t>(instruction & 63U);
    if (function == 0x08U || function == 0x09U) {
      return BlockTermination::after_delay_slot;
    }
    if (function == 0x0cU || function == 0x0dU) {
      return BlockTermination::after_instruction;
    }
    return BlockTermination::none;
  }
  if (opcode >= 0x01U && opcode <= 0x07U) {
    return BlockTermination::after_delay_slot;
  }
  if ((opcode == 0x10U || opcode == 0x12U) &&
      ((instruction >> 21U) & 31U) == 0x08U) {
    return BlockTermination::after_delay_slot;
  }
  return BlockTermination::none;
}

} // namespace

void R3000Runtime::clearCodeCache() noexcept {
  code_page_cached_.fill(0U);
  if (++code_cache_reset_generation_ == 0U) {
    code_cache_reset_generation_ = 1U;
    if (cached_blocks_ != nullptr) {
      std::fill_n(cached_blocks_.get(), cached_block_capacity, CachedBlock{});
    }
  }
  if (++code_cache_epoch_ == 0U) {
    code_cache_epoch_ = 1U;
  }
}

void R3000Runtime::rebuildBreakpointPages() noexcept {
  breakpoint_pages_.fill(0U);
  for (std::size_t index{}; index < execution_breakpoint_count_; ++index) {
    std::uint32_t physical{};
    if (physicalAddress(execution_breakpoints_[index], physical) &&
        physical < ram_mirror_end) {
      const auto offset = physical & static_cast<std::uint32_t>(ram_size - 1U);
      breakpoint_pages_[offset / code_page_size] = 1U;
    }
  }
}

void R3000Runtime::setExecutionBreakpoint(std::uint32_t pc,
                                          bool enabled) noexcept {
  const auto first = execution_breakpoints_.begin();
  const auto last = first + execution_breakpoint_count_;
  const auto found = std::find(first, last, pc);
  if (enabled) {
    if (found != last ||
        execution_breakpoint_count_ == execution_breakpoint_capacity) {
      return;
    }
    execution_breakpoints_[execution_breakpoint_count_++] = pc;
  } else {
    if (found == last) {
      return;
    }
    *found = execution_breakpoints_[--execution_breakpoint_count_];
  }
  rebuildBreakpointPages();
  clearCodeCache();
}

bool R3000Runtime::executionBreakpoint(std::uint32_t pc) const noexcept {
  std::uint32_t physical{};
  if (physicalAddress(pc, physical) && physical < ram_mirror_end) {
    const auto offset = physical & static_cast<std::uint32_t>(ram_size - 1U);
    if (breakpoint_pages_[offset / code_page_size] == 0U) {
      return false;
    }
  }
  return std::find(execution_breakpoints_.begin(),
                   execution_breakpoints_.begin() + execution_breakpoint_count_,
                   pc) !=
         execution_breakpoints_.begin() + execution_breakpoint_count_;
}

void R3000Runtime::invalidateCodePage(std::uint32_t physical_address) noexcept {
  if (physical_address >= ram_mirror_end) {
    return;
  }
  const auto offset =
      physical_address & static_cast<std::uint32_t>(ram_size - 1U);
  const auto page = static_cast<std::size_t>(offset / code_page_size);
  if (code_page_cached_[page] == 0U) {
    return;
  }
  code_page_cached_[page] = 0U;
  if (++code_page_generations_[page] == 0U) {
    code_page_generations_[page] = 1U;
    clearCodeCache();
    return;
  }
  if (++code_cache_epoch_ == 0U) {
    code_cache_epoch_ = 1U;
  }
}

R3000Runtime::CachedBlock &
R3000Runtime::buildCachedBlock(CachedBlock &block,
                               std::uint32_t start_pc) noexcept {
  block = {};
  block.start_pc = start_pc;
  block.reset_generation = code_cache_reset_generation_;

  auto pc = start_pc;
  auto finish_after_instruction = false;
  while (block.instruction_count < cached_block_max_instructions) {
    if (block.instruction_count != 0U && executionBreakpoint(pc)) {
      break;
    }

    std::uint32_t physical{};
    if ((pc & 3U) != 0U || !physicalAddress(pc, physical) ||
        physical >= ram_mirror_end) {
      break;
    }
    const auto offset = physical & static_cast<std::uint32_t>(ram_size - 1U);
    const auto page = static_cast<std::uint16_t>(offset / code_page_size);
    if (block.page_count == 0U || block.pages[block.page_count - 1U] != page) {
      if (block.page_count == block.pages.size()) {
        break;
      }
      block.pages[block.page_count] = page;
      block.page_generations[block.page_count] = code_page_generations_[page];
      ++block.page_count;
    }
    code_page_cached_[page] = 1U;

    std::uint32_t instruction{};
    std::memcpy(&instruction, ram_.data() + offset, sizeof(instruction));
    block.instructions[block.instruction_count++] =
        decodeCachedInstruction(instruction);

    if (finish_after_instruction) {
      break;
    }
    switch (blockTermination(instruction)) {
    case BlockTermination::after_instruction:
      return block;
    case BlockTermination::after_delay_slot:
      finish_after_instruction = true;
      break;
    case BlockTermination::none:
      break;
    }
    if (pc > std::numeric_limits<std::uint32_t>::max() - 4U) {
      break;
    }
    pc += 4U;
  }
  return block;
}

R3000CachedBlockView R3000Runtime::cachedBlock() noexcept {
  const auto pc = state_.pc;
  if (cached_blocks_ == nullptr || executionBreakpoint(pc)) {
    return {};
  }
  std::uint32_t physical{};
  if ((pc & 3U) != 0U || !physicalAddress(pc, physical) ||
      physical >= ram_mirror_end) {
    return {};
  }

  const auto word_pc = pc >> 2U;
  const auto index =
      (word_pc ^ (word_pc >> 13U)) & (cached_block_capacity - 1U);
  auto &block = cached_blocks_[index];
  auto valid = block.start_pc == pc &&
               block.reset_generation == code_cache_reset_generation_ &&
               block.instruction_count != 0U;
  for (std::size_t page{}; valid && page < block.page_count; ++page) {
    valid = code_page_generations_[block.pages[page]] ==
            block.page_generations[page];
  }
  if (!valid) {
    static_cast<void>(buildCachedBlock(block, pc));
  }
  return {
      .instructions = {block.instructions.data(), block.instruction_count},
      .start_pc = block.start_pc,
      .cache_epoch = code_cache_epoch_,
  };
}

} // namespace sf::psx
