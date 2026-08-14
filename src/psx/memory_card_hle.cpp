#include "sf/psx/memory_card_hle.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sf::psx {
namespace {

constexpr std::uint32_t a0_vector = 0x000000a0U;
constexpr std::uint32_t b0_vector = 0x000000b0U;
constexpr std::uint32_t open_call = 0x32U;
constexpr std::uint32_t seek_call = 0x33U;
constexpr std::uint32_t read_call = 0x34U;
constexpr std::uint32_t write_call = 0x35U;
constexpr std::uint32_t close_call = 0x36U;
constexpr std::uint32_t format_call = 0x41U;
constexpr std::uint32_t first_file_call = 0x42U;
constexpr std::uint32_t next_file_call = 0x43U;
constexpr std::uint32_t erase_call = 0x45U;
constexpr std::uint32_t create_mode = 0x0200U;
constexpr std::uint32_t async_mode = 0x8000U;
constexpr std::uint32_t error_result = 0xffffffffU;
constexpr std::uint32_t directory_entry_size = 0x28U;
constexpr std::uint32_t directory_attribute_active = 0x50U;

struct ParsedPath {
  std::array<char, 21U> name{};
};

[[nodiscard]] bool isFileCall(std::uint32_t vector, std::uint32_t call,
                              std::uint32_t &normalized) noexcept {
  if (vector == a0_vector && call <= 4U) {
    normalized = open_call + call;
    return true;
  }
  if (vector != b0_vector) {
    return false;
  }
  switch (call) {
  case open_call:
  case seek_call:
  case read_call:
  case write_call:
  case close_call:
  case format_call:
  case first_file_call:
  case next_file_call:
  case erase_call:
    normalized = call;
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool readGuestString(const R3000Runtime &runtime,
                                   std::uint32_t address,
                                   std::array<char, 27U> &text,
                                   std::size_t &length) noexcept {
  text.fill('\0');
  length = 0U;
  for (; length < text.size(); ++length) {
    if (address > std::numeric_limits<std::uint32_t>::max() -
                      static_cast<std::uint32_t>(length)) {
      return false;
    }
    std::uint8_t value{};
    if (!runtime.read8(address + static_cast<std::uint32_t>(length), value)) {
      return false;
    }
    text[length] = static_cast<char>(value);
    if (value == 0U) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool parsePath(const R3000Runtime &runtime, std::uint32_t address,
                             bool allow_wildcards, ParsedPath &path) noexcept {
  std::array<char, 27U> text{};
  std::size_t length{};
  if (!readGuestString(runtime, address, text, length) || length <= 5U ||
      length > 25U || text[0] != 'b' || text[1] != 'u' || text[2] != '0' ||
      text[3] != '0' || text[4] != ':') {
    return false;
  }

  path = {};
  const auto name_length = length - 5U;
  for (std::size_t index{}; index < name_length; ++index) {
    const auto value = text[5U + index];
    if (value == ':' || value == '/' || value == '\\' ||
        (!allow_wildcards && (value == '*' || value == '?'))) {
      return false;
    }
    path.name[index] = value;
  }
  path.name[name_length] = '\0';
  return true;
}

[[nodiscard]] bool sameName(const std::array<char, 21U> &left,
                            const std::array<char, 21U> &right) noexcept {
  return std::equal(left.begin(), left.end(), right.begin());
}

[[nodiscard]] bool wildcardMatch(const std::array<char, 21U> &pattern,
                                 const std::array<char, 21U> &text) noexcept {
  std::size_t pattern_index{};
  std::size_t text_index{};
  std::size_t star = pattern.size();
  std::size_t star_text{};

  while (text_index < text.size() && text[text_index] != '\0') {
    if (pattern_index < pattern.size() &&
        (pattern[pattern_index] == '?' ||
         pattern[pattern_index] == text[text_index])) {
      ++pattern_index;
      ++text_index;
      continue;
    }
    if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
      star = pattern_index++;
      star_text = text_index;
      continue;
    }
    if (star != pattern.size()) {
      pattern_index = star + 1U;
      text_index = ++star_text;
      continue;
    }
    return false;
  }
  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
    ++pattern_index;
  }
  if (pattern_index >= pattern.size()) {
    return true;
  }
  return pattern[pattern_index] == '\0';
}

[[nodiscard]] std::uint8_t
findFile(const MemoryCardHleState &state,
         const std::array<char, 21U> &name) noexcept {
  for (std::uint8_t index{}; index < state.files.size(); ++index) {
    if (state.files[index].used && sameName(state.files[index].name, name)) {
      return index;
    }
  }
  return memory_card_invalid_index;
}

[[nodiscard]] std::uint8_t
findFreeDescriptor(const MemoryCardHleState &state) noexcept {
  for (std::uint8_t index = 2U; index < state.descriptors.size(); ++index) {
    if (state.descriptors[index].file_index == memory_card_invalid_index) {
      return index;
    }
  }
  return memory_card_invalid_index;
}

[[nodiscard]] bool descriptorValid(const MemoryCardHleState &state,
                                   std::uint32_t descriptor) noexcept {
  if (descriptor >= state.descriptors.size()) {
    return false;
  }
  const auto file = state.descriptors[descriptor].file_index;
  return file < state.files.size() && state.files[file].used;
}

[[nodiscard]] bool blockIsUsed(const MemoryCardHleState &state,
                               std::uint8_t block) noexcept {
  for (const auto &file : state.files) {
    if (!file.used) {
      continue;
    }
    for (std::uint8_t index{}; index < file.block_count; ++index) {
      if (file.blocks[index] == block) {
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] bool guestRangeValid(const R3000Runtime &runtime,
                                   std::uint32_t address,
                                   std::uint32_t length) noexcept {
  if (length == 0U ||
      address > std::numeric_limits<std::uint32_t>::max() - (length - 1U)) {
    return false;
  }
  for (std::uint32_t index{}; index < length; ++index) {
    std::uint8_t ignored{};
    if (!runtime.read8(address + index, ignored)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool transfer(R3000Runtime &runtime, MemoryCardHleState &state,
                            std::uint8_t file_index, std::uint32_t offset,
                            std::uint32_t guest_address, std::uint32_t length,
                            bool write) noexcept {
  if (!guestRangeValid(runtime, guest_address, length)) {
    return false;
  }
  const auto &file = state.files[file_index];
  for (std::uint32_t index{}; index < length; ++index) {
    const auto file_offset = offset + index;
    const auto logical_block =
        static_cast<std::size_t>(file_offset / memory_card_block_size);
    const auto block_offset =
        static_cast<std::size_t>(file_offset % memory_card_block_size);
    const auto physical_block = file.blocks[logical_block];

    if (write) {
      std::uint8_t value{};
      if (!runtime.read8(guest_address + index, value)) {
        return false;
      }
      state.blocks[physical_block][block_offset] = std::byte{value};
    } else {
      const auto value = std::to_integer<std::uint8_t>(
          state.blocks[physical_block][block_offset]);
      if (!runtime.write8(guest_address + index, value)) {
        return false;
      }
    }
  }
  if (write) {
    ++state.dirty_generation;
  }
  return true;
}

[[nodiscard]] bool prepareTransfer(const R3000Runtime &runtime,
                                   const MemoryCardHleState &state,
                                   std::uint32_t descriptor,
                                   std::uint32_t guest_address,
                                   std::uint32_t length) noexcept {
  if (!descriptorValid(state, descriptor) || length == 0U ||
      (length % 0x80U) != 0U) {
    return false;
  }
  const auto &fd = state.descriptors[descriptor];
  const auto &file = state.files[fd.file_index];
  const auto file_size = static_cast<std::uint32_t>(file.block_count) *
                         static_cast<std::uint32_t>(memory_card_block_size);
  return (fd.offset % 0x80U) == 0U && fd.offset < file_size &&
         length <= file_size - fd.offset &&
         guestRangeValid(runtime, guest_address, length);
}

[[nodiscard]] bool descriptorReferencesFile(const MemoryCardHleState &state,
                                            std::uint8_t file_index) noexcept {
  return std::any_of(state.descriptors.begin() + 2U, state.descriptors.end(),
                     [file_index](const MemoryCardDescriptorState &descriptor) {
                       return descriptor.file_index == file_index;
                     });
}

[[nodiscard]] bool writeDirectoryEntry(R3000Runtime &runtime,
                                       const MemoryCardHleState &state,
                                       std::uint8_t file_index,
                                       std::uint32_t address) noexcept {
  if (!guestRangeValid(runtime, address, directory_entry_size)) {
    return false;
  }
  const auto &file = state.files[file_index];
  for (std::size_t index{}; index < 20U; ++index) {
    if (!runtime.write8(address + static_cast<std::uint32_t>(index),
                        static_cast<std::uint8_t>(file.name[index]))) {
      return false;
    }
  }
  const auto file_size = static_cast<std::uint32_t>(file.block_count) *
                         static_cast<std::uint32_t>(memory_card_block_size);
  const auto lba = (static_cast<std::uint32_t>(file.blocks[0]) + 1U) * 0x40U;
  return runtime.write32(address + 0x14U, directory_attribute_active) &&
         runtime.write32(address + 0x18U, file_size) &&
         runtime.write32(address + 0x20U, lba);
}

[[nodiscard]] std::uint32_t nextFile(R3000Runtime &runtime,
                                     MemoryCardHleState &state,
                                     std::uint32_t address) noexcept {
  if (!state.find.active) {
    return 0U;
  }
  for (std::uint8_t index = state.find.next_file; index < state.files.size();
       ++index) {
    state.find.next_file = static_cast<std::uint8_t>(index + 1U);
    if (!state.files[index].used ||
        !wildcardMatch(state.find.pattern, state.files[index].name)) {
      continue;
    }
    if (!writeDirectoryEntry(runtime, state, index, address)) {
      state.find.active = false;
      return 0U;
    }
    return address;
  }
  state.find.active = false;
  return 0U;
}

[[nodiscard]] std::uint32_t openFile(R3000Runtime &runtime,
                                     MemoryCardHleState &state,
                                     std::uint32_t path_address,
                                     std::uint32_t mode) noexcept {
  if (!state.present || !state.formatted) {
    return error_result;
  }
  ParsedPath path{};
  if (!parsePath(runtime, path_address, false, path)) {
    return error_result;
  }
  const auto descriptor = findFreeDescriptor(state);
  if (descriptor == memory_card_invalid_index) {
    return error_result;
  }

  auto file_index = findFile(state, path.name);
  if ((mode & create_mode) != 0U) {
    if (file_index != memory_card_invalid_index) {
      return error_result;
    }
    const auto requested_blocks = static_cast<std::uint32_t>(mode >> 16U);
    if (requested_blocks == 0U || requested_blocks > memory_card_block_count) {
      return error_result;
    }

    std::array<std::uint8_t, memory_card_block_count> free_blocks{};
    std::uint8_t free_count{};
    for (std::uint8_t block{}; block < memory_card_block_count; ++block) {
      if (!blockIsUsed(state, block)) {
        free_blocks[free_count++] = block;
      }
    }
    if (free_count < requested_blocks) {
      return error_result;
    }

    file_index = memory_card_invalid_index;
    for (std::uint8_t index{}; index < state.files.size(); ++index) {
      if (!state.files[index].used) {
        file_index = index;
        break;
      }
    }
    if (file_index == memory_card_invalid_index) {
      return error_result;
    }

    auto file = MemoryCardFileState{};
    file.name = path.name;
    file.block_count = static_cast<std::uint8_t>(requested_blocks);
    file.used = true;
    for (std::uint8_t index{}; index < file.block_count; ++index) {
      file.blocks[index] = free_blocks[index];
      state.blocks[free_blocks[index]].fill(std::byte{0xffU});
    }
    state.files[file_index] = file;
    ++state.dirty_generation;
  } else if (file_index == memory_card_invalid_index) {
    return error_result;
  }

  state.descriptors[descriptor] =
      MemoryCardDescriptorState{0U, mode, file_index};
  return descriptor;
}

[[nodiscard]] std::uint32_t seekFile(MemoryCardHleState &state,
                                     std::uint32_t descriptor,
                                     std::int32_t offset,
                                     std::uint32_t whence) noexcept {
  if (!descriptorValid(state, descriptor)) {
    return error_result;
  }
  auto &fd = state.descriptors[descriptor];
  std::int64_t position = fd.offset;
  if (whence == 0U) {
    position = offset;
  } else if (whence == 1U) {
    position += offset;
  } else if (whence != 2U) {
    return error_result;
  }
  if (position < 0 || position > std::numeric_limits<std::uint32_t>::max()) {
    return error_result;
  }
  fd.offset = static_cast<std::uint32_t>(position);
  return fd.offset;
}

[[nodiscard]] std::uint32_t
submitTransfer(R3000Runtime &runtime, MemoryCardHleState &state,
               std::uint32_t descriptor, std::uint32_t guest_address,
               std::uint32_t length, bool write) noexcept {
  if (state.pending.kind != MemoryCardPendingKind::none ||
      !prepareTransfer(runtime, state, descriptor, guest_address, length)) {
    return error_result;
  }
  auto &fd = state.descriptors[descriptor];
  if ((fd.mode & async_mode) != 0U) {
    state.pending.guest_address = guest_address;
    state.pending.length = length;
    state.pending.offset = fd.offset;
    state.pending.descriptor = static_cast<std::uint8_t>(descriptor);
    state.pending.file_index = fd.file_index;
    state.pending.kind =
        write ? MemoryCardPendingKind::write : MemoryCardPendingKind::read;
    return 0U;
  }
  if (!transfer(runtime, state, fd.file_index, fd.offset, guest_address, length,
                write)) {
    return error_result;
  }
  fd.offset += length;
  return length;
}

[[nodiscard]] std::uint32_t closeFile(MemoryCardHleState &state,
                                      std::uint32_t descriptor) noexcept {
  if (!descriptorValid(state, descriptor) ||
      (state.pending.kind != MemoryCardPendingKind::none &&
       state.pending.descriptor == descriptor)) {
    return error_result;
  }
  state.descriptors[descriptor] = MemoryCardDescriptorState{};
  return descriptor;
}

[[nodiscard]] std::uint32_t formatCard(R3000Runtime &runtime,
                                       MemoryCardHleState &state,
                                       std::uint32_t path_address) noexcept {
  std::array<char, 27U> text{};
  std::size_t length{};
  if (!state.present || !readGuestString(runtime, path_address, text, length) ||
      length != 5U || text[0] != 'b' || text[1] != 'u' || text[2] != '0' ||
      text[3] != '0' || text[4] != ':') {
    return 0U;
  }

  state.files = {};
  state.descriptors = {};
  state.find = {};
  state.pending = {};
  for (auto &block : state.blocks) {
    block.fill(std::byte{0xffU});
  }
  state.formatted = true;
  ++state.dirty_generation;
  return 1U;
}

[[nodiscard]] std::uint32_t firstFile(R3000Runtime &runtime,
                                      MemoryCardHleState &state,
                                      std::uint32_t pattern_address,
                                      std::uint32_t entry_address) noexcept {
  ParsedPath path{};
  if (!state.present || !state.formatted ||
      !parsePath(runtime, pattern_address, true, path)) {
    state.find = {};
    return 0U;
  }
  state.find.pattern = path.name;
  state.find.next_file = 0U;
  state.find.active = true;
  return nextFile(runtime, state, entry_address);
}

[[nodiscard]] std::uint32_t eraseFile(R3000Runtime &runtime,
                                      MemoryCardHleState &state,
                                      std::uint32_t path_address) noexcept {
  ParsedPath path{};
  if (!state.present || !state.formatted ||
      !parsePath(runtime, path_address, true, path)) {
    return 0U;
  }
  for (std::uint8_t index{}; index < state.files.size(); ++index) {
    if (!state.files[index].used ||
        !wildcardMatch(path.name, state.files[index].name) ||
        descriptorReferencesFile(state, index)) {
      continue;
    }
    state.files[index] = {};
    ++state.dirty_generation;
    return 1U;
  }
  return 0U;
}
} // namespace
bool MemoryCardHle::handlesCall(std::uint32_t vector,
                                std::uint32_t call) noexcept {
  std::uint32_t normalized{};
  return isFileCall(vector, call, normalized);
}

MemoryCardCallResult MemoryCardHle::handleCall(R3000Runtime &runtime,
                                               MemoryCardHleState &state,
                                               std::uint32_t vector,
                                               std::uint32_t call) noexcept {
  std::uint32_t normalized{};
  if (!isFileCall(vector, call, normalized)) {
    return {};
  }

  const auto &cpu = runtime.state();
  std::uint32_t result = error_result;
  switch (normalized) {
  case open_call:
    result = openFile(runtime, state, cpu.gpr[4U], cpu.gpr[5U]);
    break;
  case seek_call:
    result = seekFile(state, cpu.gpr[4U],
                      static_cast<std::int32_t>(cpu.gpr[5U]), cpu.gpr[6U]);
    break;
  case read_call:
    result = submitTransfer(runtime, state, cpu.gpr[4U], cpu.gpr[5U],
                            cpu.gpr[6U], false);
    break;
  case write_call:
    result = submitTransfer(runtime, state, cpu.gpr[4U], cpu.gpr[5U],
                            cpu.gpr[6U], true);
    break;
  case close_call:
    result = closeFile(state, cpu.gpr[4U]);
    break;
  case format_call:
    result = formatCard(runtime, state, cpu.gpr[4U]);
    break;
  case first_file_call:
    result = firstFile(runtime, state, cpu.gpr[4U], cpu.gpr[5U]);
    break;
  case next_file_call:
    result = nextFile(runtime, state, cpu.gpr[4U]);
    break;
  case erase_call:
    result = eraseFile(runtime, state, cpu.gpr[4U]);
    break;
  default:
    return {};
  }
  const auto failure =
      normalized == format_call || normalized == erase_call ? 0U : error_result;
  return {true, result, failure};
}

MemoryCardAsyncCompletion
MemoryCardHle::servicePending(R3000Runtime &runtime,
                              MemoryCardHleState &state) noexcept {
  if (state.pending.kind == MemoryCardPendingKind::none) {
    return {};
  }

  const auto pending = state.pending;
  state.pending = {};
  MemoryCardAsyncCompletion completion{
      true, false, pending.kind == MemoryCardPendingKind::write,
      pending.descriptor};
  if (!descriptorValid(state, pending.descriptor)) {
    return completion;
  }
  auto &fd = state.descriptors[pending.descriptor];
  if (fd.file_index != pending.file_index || fd.offset != pending.offset) {
    return completion;
  }

  completion.success =
      transfer(runtime, state, pending.file_index, pending.offset,
               pending.guest_address, pending.length, completion.write);
  if (completion.success) {
    fd.offset += pending.length;
  }
  return completion;
}

bool MemoryCardHle::validateState(const MemoryCardHleState &state) noexcept {
  std::array<bool, memory_card_block_count> used_blocks{};
  for (std::size_t file_index{}; file_index < state.files.size();
       ++file_index) {
    const auto &file = state.files[file_index];
    if (!file.used) {
      continue;
    }
    if (file.name[0] == '\0' || file.name.back() != '\0' ||
        file.block_count == 0U || file.block_count > memory_card_block_count) {
      return false;
    }
    for (std::size_t other{}; other < file_index; ++other) {
      if (state.files[other].used &&
          sameName(file.name, state.files[other].name)) {
        return false;
      }
    }
    for (std::uint8_t index{}; index < file.block_count; ++index) {
      const auto block = file.blocks[index];
      if (block >= memory_card_block_count || used_blocks[block]) {
        return false;
      }
      used_blocks[block] = true;
    }
  }

  for (std::size_t descriptor{}; descriptor < state.descriptors.size();
       ++descriptor) {
    const auto file = state.descriptors[descriptor].file_index;
    if (file == memory_card_invalid_index) {
      continue;
    }
    if (descriptor < 2U || file >= state.files.size() ||
        !state.files[file].used) {
      return false;
    }
  }
  if (state.find.next_file > memory_card_file_count) {
    return false;
  }

  if (state.pending.kind == MemoryCardPendingKind::none) {
    return state.pending.descriptor == memory_card_invalid_index &&
           state.pending.file_index == memory_card_invalid_index;
  }
  if (state.pending.kind != MemoryCardPendingKind::read &&
      state.pending.kind != MemoryCardPendingKind::write) {
    return false;
  }
  if (!descriptorValid(state, state.pending.descriptor)) {
    return false;
  }
  const auto &fd = state.descriptors[state.pending.descriptor];
  const auto &file = state.files[fd.file_index];
  const auto file_size = static_cast<std::uint32_t>(file.block_count) *
                         static_cast<std::uint32_t>(memory_card_block_size);
  return state.pending.file_index == fd.file_index &&
         state.pending.offset == fd.offset &&
         (state.pending.offset % 0x80U) == 0U && state.pending.length != 0U &&
         (state.pending.length % 0x80U) == 0U &&
         state.pending.offset < file_size &&
         state.pending.length <= file_size - state.pending.offset;
}

} // namespace sf::psx
