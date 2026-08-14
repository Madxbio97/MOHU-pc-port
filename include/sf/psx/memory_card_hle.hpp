#pragma once

#include "sf/psx/r3000_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace sf::psx {

inline constexpr std::size_t memory_card_block_size = 8U * 1024U;
inline constexpr std::size_t memory_card_block_count = 15U;
inline constexpr std::size_t memory_card_file_count = 15U;
inline constexpr std::size_t memory_card_descriptor_count = 16U;
inline constexpr std::uint8_t memory_card_invalid_index = 0xffU;

struct MemoryCardFileState {
  std::array<char, 21U> name{};
  std::array<std::uint8_t, memory_card_block_count> blocks{};
  std::uint8_t block_count{};
  bool used{};

  bool operator==(const MemoryCardFileState &) const = default;
};

struct MemoryCardDescriptorState {
  std::uint32_t offset{};
  std::uint32_t mode{};
  std::uint8_t file_index{memory_card_invalid_index};

  bool operator==(const MemoryCardDescriptorState &) const = default;
};

struct MemoryCardFindState {
  std::array<char, 21U> pattern{};
  std::uint8_t next_file{};
  bool active{};

  bool operator==(const MemoryCardFindState &) const = default;
};

enum class MemoryCardPendingKind : std::uint8_t {
  none,
  read,
  write,
};

struct MemoryCardPendingState {
  std::uint32_t guest_address{};
  std::uint32_t length{};
  std::uint32_t offset{};
  std::uint8_t descriptor{memory_card_invalid_index};
  std::uint8_t file_index{memory_card_invalid_index};
  MemoryCardPendingKind kind{MemoryCardPendingKind::none};

  bool operator==(const MemoryCardPendingState &) const = default;
};

struct MemoryCardHleState {
  std::array<MemoryCardFileState, memory_card_file_count> files{};
  std::array<std::array<std::byte, memory_card_block_size>,
             memory_card_block_count>
      blocks{};
  std::array<MemoryCardDescriptorState, memory_card_descriptor_count>
      descriptors{};
  MemoryCardFindState find{};
  MemoryCardPendingState pending{};
  std::uint64_t dirty_generation{};
  bool present{true};
  bool formatted{true};

  bool operator==(const MemoryCardHleState &) const = default;
};

struct MemoryCardCallResult {
  bool handled{};
  std::uint32_t result{};
  std::uint32_t failure_result{0xffffffffU};
};

struct MemoryCardAsyncCompletion {
  bool completed{};
  bool success{};
  bool write{};
  std::uint32_t descriptor{};
};

class MemoryCardHle final {
public:
  [[nodiscard]] static bool handlesCall(std::uint32_t vector,
                                        std::uint32_t call) noexcept;

  [[nodiscard]] static MemoryCardCallResult
  handleCall(R3000Runtime &runtime, MemoryCardHleState &state,
             std::uint32_t vector, std::uint32_t call) noexcept;

  [[nodiscard]] static MemoryCardAsyncCompletion
  servicePending(R3000Runtime &runtime, MemoryCardHleState &state) noexcept;

  [[nodiscard]] static bool
  validateState(const MemoryCardHleState &state) noexcept;
};

} // namespace sf::psx
