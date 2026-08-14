#include "sf/psx/memory_card_image.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sf::psx {
namespace {

constexpr std::size_t frame_size = 0x80U;
constexpr std::size_t frames_per_block = 0x40U;
constexpr std::size_t raw_frame_count = memory_card_raw_image_size / frame_size;
constexpr std::size_t directory_first_frame = 1U;
constexpr std::size_t broken_first_frame = 16U;
constexpr std::size_t broken_frame_count = 20U;
constexpr std::size_t replacement_first_frame = 36U;
constexpr std::size_t write_test_frame = 63U;
constexpr std::uint32_t state_first = 0x51U;
constexpr std::uint32_t state_middle = 0x52U;
constexpr std::uint32_t state_last = 0x53U;
constexpr std::uint32_t state_free = 0xa0U;
constexpr std::uint32_t state_deleted_first = 0xa1U;
constexpr std::uint32_t state_deleted_middle = 0xa2U;
constexpr std::uint32_t state_deleted_last = 0xa3U;
constexpr std::uint16_t chain_end = 0xffffU;

[[nodiscard]] MemoryCardImageResult
failure(MemoryCardImageError error,
        std::error_code system_error = {}) noexcept {
  return {error, system_error, MemoryCardImageLoadSource::none};
}

[[nodiscard]] std::uint16_t read16(const std::uint8_t *value) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(value[0U]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(value[1U]) << 8U));
}

[[nodiscard]] std::uint32_t read32(const std::uint8_t *value) noexcept {
  return static_cast<std::uint32_t>(value[0U]) |
         (static_cast<std::uint32_t>(value[1U]) << 8U) |
         (static_cast<std::uint32_t>(value[2U]) << 16U) |
         (static_cast<std::uint32_t>(value[3U]) << 24U);
}

void write16(std::uint8_t *destination, std::uint16_t value) noexcept {
  destination[0U] = static_cast<std::uint8_t>(value);
  destination[1U] = static_cast<std::uint8_t>(value >> 8U);
}

void write32(std::uint8_t *destination, std::uint32_t value) noexcept {
  destination[0U] = static_cast<std::uint8_t>(value);
  destination[1U] = static_cast<std::uint8_t>(value >> 8U);
  destination[2U] = static_cast<std::uint8_t>(value >> 16U);
  destination[3U] = static_cast<std::uint8_t>(value >> 24U);
}

[[nodiscard]] std::uint8_t frameChecksum(const std::uint8_t *frame) noexcept {
  std::uint8_t checksum{};
  for (std::size_t index{}; index < frame_size - 1U; ++index) {
    checksum = static_cast<std::uint8_t>(checksum ^ frame[index]);
  }
  return checksum;
}

void finishFrame(std::uint8_t *frame) noexcept {
  frame[frame_size - 1U] = frameChecksum(frame);
}

[[nodiscard]] bool frameValid(const std::uint8_t *frame) noexcept {
  return frame[frame_size - 1U] == frameChecksum(frame);
}

[[nodiscard]] bool knownDirectoryState(std::uint32_t state) noexcept {
  switch (state) {
  case state_first:
  case state_middle:
  case state_last:
  case state_free:
  case state_deleted_first:
  case state_deleted_middle:
  case state_deleted_last:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool canonicalName(const MemoryCardFileState &file) noexcept {
  bool terminated{};
  for (const auto value : file.name) {
    if (value == '\0') {
      terminated = true;
    } else if (terminated) {
      return false;
    }
  }
  return terminated && file.name[0U] != '\0';
}

[[nodiscard]] bool
samePersistedName(const MemoryCardFileState &left,
                  const MemoryCardFileState &right) noexcept {
  for (std::size_t index{}; index < 20U; ++index) {
    if (left.name[index] != right.name[index]) {
      return false;
    }
    if (left.name[index] == '\0') {
      return true;
    }
  }
  return true;
}

[[nodiscard]] bool durableStateValid(const MemoryCardHleState &state) noexcept {
  if (!MemoryCardHle::validateState(state)) {
    return false;
  }
  for (std::size_t index{}; index < state.files.size(); ++index) {
    const auto &file = state.files[index];
    if (!file.used) {
      continue;
    }
    if (!canonicalName(file)) {
      return false;
    }
    for (std::size_t other{}; other < index; ++other) {
      if (state.files[other].used &&
          samePersistedName(file, state.files[other])) {
        return false;
      }
    }
  }
  return true;
}

void resetState(MemoryCardHleState &state) noexcept {
  std::destroy_at(std::addressof(state));
  std::construct_at(std::addressof(state));
}

void initializeBlankState(MemoryCardHleState &state) noexcept {
  resetState(state);
  for (auto &block : state.blocks) {
    block.fill(std::byte{0xffU});
  }
}

[[nodiscard]] std::filesystem::path
backupPath(const std::filesystem::path &primary) {
  auto backup = primary;
  backup += ".bak";
  return backup;
}

[[nodiscard]] bool isMissing(const MemoryCardImageResult &result) noexcept {
  return result.error == MemoryCardImageError::io_error &&
         result.system_error ==
             std::make_error_condition(std::errc::no_such_file_or_directory);
}

[[nodiscard]] MemoryCardImageResult
readRawImage(const std::filesystem::path &path,
             MemoryCardRawImage &image) noexcept {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return failure(MemoryCardImageError::io_error, error);
  }
  if (size != static_cast<std::uintmax_t>(memory_card_raw_image_size)) {
    return failure(MemoryCardImageError::invalid_size);
  }

  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return failure(MemoryCardImageError::io_error,
                   std::make_error_code(std::errc::io_error));
  }
  stream.read(reinterpret_cast<char *>(image.data()),
              static_cast<std::streamsize>(image.size()));
  if (stream.gcount() != static_cast<std::streamsize>(image.size())) {
    return failure(MemoryCardImageError::io_error,
                   std::make_error_code(std::errc::io_error));
  }
  return {};
}

[[nodiscard]] MemoryCardImageResult
loadSingle(const std::filesystem::path &path,
           MemoryCardHleState &state) noexcept {
  auto image = std::make_unique<MemoryCardRawImage>();
  const auto read = readRawImage(path, *image);
  if (!read) {
    return read;
  }
  return MemoryCardImage::decode(*image, state);
}

[[nodiscard]] std::filesystem::path
temporaryPath(const std::filesystem::path &destination, std::uint64_t serial,
              std::uint64_t process) {
  auto path = destination;
  path += ".tmp.";
  path += std::to_string(process);
  path += ".";
  path += std::to_string(serial);
  return path;
}

#ifdef _WIN32

[[nodiscard]] std::error_code windowsError(DWORD value) noexcept {
  return {static_cast<int>(value), std::system_category()};
}

[[nodiscard]] MemoryCardImageResult
writeAtomic(const std::filesystem::path &destination,
            const MemoryCardRawImage &image) noexcept {
  static std::atomic<std::uint64_t> serial{};
  const auto process = static_cast<std::uint64_t>(GetCurrentProcessId());

  for (unsigned attempt{}; attempt < 64U; ++attempt) {
    const auto temporary =
        temporaryPath(destination, serial.fetch_add(1U), process);
    const auto file =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      const auto error = GetLastError();
      if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
        continue;
      }
      return failure(MemoryCardImageError::io_error, windowsError(error));
    }

    std::size_t offset{};
    DWORD error{};
    while (offset < image.size()) {
      const auto remaining = image.size() - offset;
      const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
          remaining,
          static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
      DWORD written{};
      if (WriteFile(file, image.data() + offset, chunk, &written, nullptr) ==
              0 ||
          written == 0U) {
        error = GetLastError();
        break;
      }
      offset += static_cast<std::size_t>(written);
    }
    if (error == ERROR_SUCCESS && FlushFileBuffers(file) == 0) {
      error = GetLastError();
    }
    if (CloseHandle(file) == 0 && error == ERROR_SUCCESS) {
      error = GetLastError();
    }
    if (error != ERROR_SUCCESS) {
      DeleteFileW(temporary.c_str());
      return failure(MemoryCardImageError::io_error, windowsError(error));
    }
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
      error = GetLastError();
      DeleteFileW(temporary.c_str());
      return failure(MemoryCardImageError::io_error, windowsError(error));
    }
    return {};
  }
  return failure(MemoryCardImageError::io_error,
                 windowsError(ERROR_FILE_EXISTS));
}

#else

[[nodiscard]] std::uint64_t processId() noexcept {
  return static_cast<std::uint64_t>(getpid());
}

[[nodiscard]] std::error_code posixError(int value) noexcept {
  return {value, std::generic_category()};
}

[[nodiscard]] MemoryCardImageResult
writeAtomic(const std::filesystem::path &destination,
            const MemoryCardRawImage &image) noexcept {
  static std::atomic<std::uint64_t> serial{};
  for (unsigned attempt{}; attempt < 64U; ++attempt) {
    const auto temporary =
        temporaryPath(destination, serial.fetch_add(1U), processId());
    const auto descriptor =
        ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) {
      const auto error = errno;
      if (error == EEXIST) {
        continue;
      }
      return failure(MemoryCardImageError::io_error, posixError(error));
    }

    std::size_t offset{};
    int error{};
    while (offset < image.size()) {
      const auto written =
          ::write(descriptor, image.data() + offset, image.size() - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        error = errno;
        break;
      }
      if (written == 0) {
        error = EIO;
        break;
      }
      offset += static_cast<std::size_t>(written);
    }
    if (error == 0 && ::fsync(descriptor) != 0) {
      error = errno;
    }
    if (::close(descriptor) != 0 && error == 0) {
      error = errno;
    }
    if (error != 0) {
      ::unlink(temporary.c_str());
      return failure(MemoryCardImageError::io_error, posixError(error));
    }
    if (::rename(temporary.c_str(), destination.c_str()) != 0) {
      error = errno;
      ::unlink(temporary.c_str());
      return failure(MemoryCardImageError::io_error, posixError(error));
    }

    auto parent = destination.parent_path();
    if (parent.empty()) {
      parent = ".";
    }
    const auto directory = ::open(parent.c_str(), O_RDONLY);
    if (directory >= 0) {
      if (::fsync(directory) != 0) {
        error = errno;
      }
      if (::close(directory) != 0 && error == 0) {
        error = errno;
      }
    }
    if (error != 0) {
      return failure(MemoryCardImageError::io_error, posixError(error));
    }
    return {};
  }
  return failure(MemoryCardImageError::io_error, posixError(EEXIST));
}

#endif

} // namespace

MemoryCardImageResult
MemoryCardImage::encode(const MemoryCardHleState &state,
                        MemoryCardRawImage &image) noexcept {
  if (!state.present) {
    return failure(MemoryCardImageError::card_absent);
  }
  if (!state.formatted) {
    return failure(MemoryCardImageError::not_formatted);
  }
  if (!durableStateValid(state)) {
    return failure(MemoryCardImageError::invalid_state);
  }

  image.fill(0U);
  image[0U] = static_cast<std::uint8_t>('M');
  image[1U] = static_cast<std::uint8_t>('C');
  finishFrame(image.data());

  for (std::size_t slot{}; slot < memory_card_block_count; ++slot) {
    auto *frame = image.data() + (directory_first_frame + slot) * frame_size;
    write32(frame, state_free);
    write16(frame + 0x08U, chain_end);
    finishFrame(frame);
  }
  for (std::size_t index{}; index < broken_frame_count; ++index) {
    auto *frame = image.data() + (broken_first_frame + index) * frame_size;
    write32(frame, std::numeric_limits<std::uint32_t>::max());
    write16(frame + 0x08U, chain_end);
    finishFrame(frame);
  }

  auto *write_test = image.data() + write_test_frame * frame_size;
  write_test[0U] = static_cast<std::uint8_t>('M');
  write_test[1U] = static_cast<std::uint8_t>('C');
  finishFrame(write_test);

  for (const auto &file : state.files) {
    if (!file.used) {
      continue;
    }
    for (std::size_t position{}; position < file.block_count; ++position) {
      const auto slot = static_cast<std::size_t>(file.blocks[position]);
      auto *frame = image.data() + (directory_first_frame + slot) * frame_size;
      std::fill_n(frame, frame_size, std::uint8_t{});

      std::uint32_t allocation_state = state_middle;
      if (position == 0U) {
        allocation_state = state_first;
      } else if (position + 1U == file.block_count) {
        allocation_state = state_last;
      }
      write32(frame, allocation_state);
      if (position == 0U) {
        const auto size = static_cast<std::uint32_t>(file.block_count) *
                          static_cast<std::uint32_t>(memory_card_block_size);
        write32(frame + 0x04U, size);
        for (std::size_t index{}; index < 20U && file.name[index] != '\0';
             ++index) {
          frame[0x0aU + index] = static_cast<std::uint8_t>(file.name[index]);
        }
      }
      const auto next =
          position + 1U == file.block_count
              ? chain_end
              : static_cast<std::uint16_t>(file.blocks[position + 1U]);
      write16(frame + 0x08U, next);
      finishFrame(frame);
    }
  }

  for (std::size_t block{}; block < memory_card_block_count; ++block) {
    std::memcpy(image.data() + (block + 1U) * memory_card_block_size,
                state.blocks[block].data(), memory_card_block_size);
  }
  return {};
}

MemoryCardImageResult
MemoryCardImage::decode(std::span<const std::uint8_t> image,
                        MemoryCardHleState &state) noexcept {
  if (image.size() != memory_card_raw_image_size) {
    return failure(MemoryCardImageError::invalid_size);
  }
  if (image[0U] != static_cast<std::uint8_t>('M') ||
      image[1U] != static_cast<std::uint8_t>('C')) {
    return failure(MemoryCardImageError::invalid_header);
  }
  if (!frameValid(image.data())) {
    return failure(MemoryCardImageError::invalid_checksum);
  }

  std::array<std::uint32_t, memory_card_block_count> allocation_states{};
  std::array<std::uint32_t, memory_card_block_count> file_sizes{};
  std::array<std::uint16_t, memory_card_block_count> next_blocks{};
  for (std::size_t slot{}; slot < memory_card_block_count; ++slot) {
    const auto *frame =
        image.data() + (directory_first_frame + slot) * frame_size;
    if (!frameValid(frame)) {
      return failure(MemoryCardImageError::invalid_checksum);
    }
    allocation_states[slot] = read32(frame);
    file_sizes[slot] = read32(frame + 0x04U);
    next_blocks[slot] = read16(frame + 0x08U);
    if (!knownDirectoryState(allocation_states[slot])) {
      return failure(MemoryCardImageError::invalid_directory);
    }
  }

  std::array<std::uint16_t, raw_frame_count> remapped_frames{};
  for (std::size_t frame{}; frame < remapped_frames.size(); ++frame) {
    remapped_frames[frame] = static_cast<std::uint16_t>(frame);
  }
  for (std::size_t index{}; index < broken_frame_count; ++index) {
    const auto *frame =
        image.data() + (broken_first_frame + index) * frame_size;
    if (!frameValid(frame)) {
      return failure(MemoryCardImageError::invalid_checksum);
    }
    const auto broken = read32(frame);
    if (broken == std::numeric_limits<std::uint32_t>::max()) {
      continue;
    }
    if (broken < frames_per_block || broken >= raw_frame_count ||
        remapped_frames[broken] != broken) {
      return failure(MemoryCardImageError::invalid_broken_sector);
    }
    remapped_frames[broken] =
        static_cast<std::uint16_t>(replacement_first_frame + index);
  }

  auto decoded_storage = std::make_unique<MemoryCardHleState>();
  auto &decoded = *decoded_storage;
  std::array<bool, memory_card_block_count> owned{};
  std::size_t file_index{};
  for (std::size_t head{}; head < memory_card_block_count; ++head) {
    if (allocation_states[head] != state_first) {
      continue;
    }
    const auto size = file_sizes[head];
    if (size == 0U || (size % memory_card_block_size) != 0U ||
        size > memory_card_block_count * memory_card_block_size ||
        file_index >= decoded.files.size()) {
      return failure(MemoryCardImageError::invalid_directory);
    }
    const auto block_count =
        static_cast<std::size_t>(size / memory_card_block_size);
    auto &file = decoded.files[file_index];
    const auto *head_frame =
        image.data() + (directory_first_frame + head) * frame_size;
    std::size_t name_length{};
    while (name_length < 20U && head_frame[0x0aU + name_length] != 0U) {
      file.name[name_length] =
          static_cast<char>(head_frame[0x0aU + name_length]);
      ++name_length;
    }
    if (name_length == 0U) {
      return failure(MemoryCardImageError::invalid_directory);
    }
    file.name[name_length] = '\0';
    file.block_count = static_cast<std::uint8_t>(block_count);
    file.used = true;

    auto slot = head;
    for (std::size_t position{}; position < block_count; ++position) {
      if (slot >= memory_card_block_count || owned[slot]) {
        return failure(MemoryCardImageError::invalid_directory);
      }
      const auto expected =
          position == 0U
              ? state_first
              : (position + 1U == block_count ? state_last : state_middle);
      if (allocation_states[slot] != expected) {
        return failure(MemoryCardImageError::invalid_directory);
      }
      owned[slot] = true;
      file.blocks[position] = static_cast<std::uint8_t>(slot);
      const auto next = next_blocks[slot];
      if (position + 1U == block_count) {
        if (next != chain_end) {
          return failure(MemoryCardImageError::invalid_directory);
        }
      } else {
        if (next >= memory_card_block_count) {
          return failure(MemoryCardImageError::invalid_directory);
        }
        slot = next;
      }
    }

    for (std::size_t other{}; other < file_index; ++other) {
      if (samePersistedName(file, decoded.files[other])) {
        return failure(MemoryCardImageError::invalid_directory);
      }
    }
    ++file_index;
  }

  for (std::size_t slot{}; slot < memory_card_block_count; ++slot) {
    if ((allocation_states[slot] == state_middle ||
         allocation_states[slot] == state_last) &&
        !owned[slot]) {
      return failure(MemoryCardImageError::invalid_directory);
    }
  }

  for (std::size_t block{}; block < memory_card_block_count; ++block) {
    for (std::size_t sector{}; sector < frames_per_block; ++sector) {
      const auto logical_frame = (block + 1U) * frames_per_block + sector;
      const auto source_frame =
          static_cast<std::size_t>(remapped_frames[logical_frame]);
      std::memcpy(decoded.blocks[block].data() + sector * frame_size,
                  image.data() + source_frame * frame_size, frame_size);
    }
  }
  if (!durableStateValid(decoded)) {
    return failure(MemoryCardImageError::invalid_directory);
  }
  state = decoded;
  return {};
}

MemoryCardImageResult
MemoryCardImage::load(const std::filesystem::path &path,
                      MemoryCardHleState &state) noexcept {
  auto loaded_storage = std::make_unique<MemoryCardHleState>();
  auto &loaded = *loaded_storage;
  auto primary = loadSingle(path, loaded);
  if (primary) {
    state = loaded;
    primary.load_source = MemoryCardImageLoadSource::primary;
    return primary;
  }

  resetState(loaded);
  auto backup = loadSingle(backupPath(path), loaded);
  if (backup) {
    state = loaded;
    backup.load_source = MemoryCardImageLoadSource::backup;
    return backup;
  }
  if (isMissing(primary) && isMissing(backup)) {
    initializeBlankState(state);
    return {MemoryCardImageError::none, {}, MemoryCardImageLoadSource::blank};
  }
  return isMissing(primary) ? backup : primary;
}

MemoryCardImageResult
MemoryCardImage::storeAtomic(const std::filesystem::path &path,
                             const MemoryCardHleState &state) noexcept {
  auto image_storage = std::make_unique<MemoryCardRawImage>();
  auto &image = *image_storage;
  const auto encoded = encode(state, image);
  if (!encoded) {
    return encoded;
  }

  if (path.empty() || path.filename().empty()) {
    return failure(MemoryCardImageError::io_error,
                   std::make_error_code(std::errc::invalid_argument));
  }
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::error_code directory_error;
    std::filesystem::create_directories(parent, directory_error);
    if (directory_error) {
      return failure(MemoryCardImageError::io_error, directory_error);
    }
  }

  const auto backup_path = backupPath(path);
  auto backup_valid = false;
  auto primary_image_storage = std::make_unique<MemoryCardRawImage>();
  auto &primary_image = *primary_image_storage;
  const auto primary_read = readRawImage(path, primary_image);
  if (primary_read) {
    auto ignored = std::make_unique<MemoryCardHleState>();
    if (decode(primary_image, *ignored)) {
      const auto backup = writeAtomic(backup_path, primary_image);
      if (!backup) {
        return backup;
      }
      backup_valid = true;
    }
  } else if (!isMissing(primary_read) &&
             primary_read.error == MemoryCardImageError::io_error) {
    return primary_read;
  }

  if (!backup_valid) {
    auto backup_image = std::make_unique<MemoryCardRawImage>();
    const auto backup_read = readRawImage(backup_path, *backup_image);
    if (backup_read) {
      auto ignored = std::make_unique<MemoryCardHleState>();
      backup_valid = static_cast<bool>(decode(*backup_image, *ignored));
    }
  }

  const auto stored = writeAtomic(path, image);
  if (!stored) {
    return stored;
  }
  if (!backup_valid) {
    // The primary image is already the durable commit point. A backup repair
    // failure must not make the BIOS roll back a save that will load again.
    static_cast<void>(writeAtomic(backup_path, image));
  }
  return stored;
}

} // namespace sf::psx
