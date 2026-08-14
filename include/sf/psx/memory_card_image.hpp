#pragma once

#include "sf/psx/memory_card_hle.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <system_error>

namespace sf::psx {

inline constexpr std::size_t memory_card_raw_image_size = 128U * 1024U;
using MemoryCardRawImage = std::array<std::uint8_t, memory_card_raw_image_size>;

enum class MemoryCardImageError : std::uint8_t {
  none,
  card_absent,
  not_formatted,
  invalid_state,
  invalid_size,
  invalid_header,
  invalid_checksum,
  invalid_directory,
  invalid_broken_sector,
  io_error,
};

[[nodiscard]] constexpr std::string_view
toString(MemoryCardImageError error) noexcept {
  switch (error) {
  case MemoryCardImageError::none:
    return "none";
  case MemoryCardImageError::card_absent:
    return "card absent";
  case MemoryCardImageError::not_formatted:
    return "card not formatted";
  case MemoryCardImageError::invalid_state:
    return "invalid in-memory card state";
  case MemoryCardImageError::invalid_size:
    return "invalid raw image size";
  case MemoryCardImageError::invalid_header:
    return "invalid raw image header";
  case MemoryCardImageError::invalid_checksum:
    return "invalid raw image checksum";
  case MemoryCardImageError::invalid_directory:
    return "invalid raw image directory";
  case MemoryCardImageError::invalid_broken_sector:
    return "invalid broken-sector table";
  case MemoryCardImageError::io_error:
    return "host I/O error";
  }
  return "unknown Memory Card image error";
}

enum class MemoryCardImageLoadSource : std::uint8_t {
  none,
  primary,
  backup,
  blank,
};

struct MemoryCardImageResult {
  MemoryCardImageError error{MemoryCardImageError::none};
  std::error_code system_error{};
  MemoryCardImageLoadSource load_source{MemoryCardImageLoadSource::none};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == MemoryCardImageError::none;
  }
};

class MemoryCardImage final {
public:
  [[nodiscard]] static MemoryCardImageResult
  encode(const MemoryCardHleState &state, MemoryCardRawImage &image) noexcept;

  [[nodiscard]] static MemoryCardImageResult
  decode(std::span<const std::uint8_t> image,
         MemoryCardHleState &state) noexcept;

  [[nodiscard]] static MemoryCardImageResult
  load(const std::filesystem::path &path, MemoryCardHleState &state) noexcept;

  [[nodiscard]] static MemoryCardImageResult
  storeAtomic(const std::filesystem::path &path,
              const MemoryCardHleState &state) noexcept;
};

} // namespace sf::psx
