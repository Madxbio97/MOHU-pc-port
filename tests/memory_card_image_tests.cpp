#include "sf/psx/memory_card_image.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::size_t frame_size = 0x80U;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

[[nodiscard]] std::uint16_t read16(const std::uint8_t *value) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(value[0U]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(value[1U]) << 8U));
}

[[nodiscard]] std::uint32_t read32(const std::uint8_t *value) {
  return static_cast<std::uint32_t>(value[0U]) |
         (static_cast<std::uint32_t>(value[1U]) << 8U) |
         (static_cast<std::uint32_t>(value[2U]) << 16U) |
         (static_cast<std::uint32_t>(value[3U]) << 24U);
}

void write16(std::uint8_t *destination, std::uint16_t value) {
  destination[0U] = static_cast<std::uint8_t>(value);
  destination[1U] = static_cast<std::uint8_t>(value >> 8U);
}

void write32(std::uint8_t *destination, std::uint32_t value) {
  destination[0U] = static_cast<std::uint8_t>(value);
  destination[1U] = static_cast<std::uint8_t>(value >> 8U);
  destination[2U] = static_cast<std::uint8_t>(value >> 16U);
  destination[3U] = static_cast<std::uint8_t>(value >> 24U);
}

[[nodiscard]] std::uint8_t checksum(const std::uint8_t *frame) {
  std::uint8_t value{};
  for (std::size_t index{}; index < frame_size - 1U; ++index) {
    value = static_cast<std::uint8_t>(value ^ frame[index]);
  }
  return value;
}

void finish(std::uint8_t *frame) { frame[frame_size - 1U] = checksum(frame); }

void setName(sf::psx::MemoryCardFileState &file, const char *name) {
  std::size_t index{};
  while (name[index] != '\0') {
    require(index < file.name.size() - 1U, "Fixture name is too long");
    file.name[index] = name[index];
    ++index;
  }
  file.name[index] = '\0';
}

[[nodiscard]] sf::psx::MemoryCardHleState makeState() {
  sf::psx::MemoryCardHleState state{};
  auto &first = state.files[0U];
  setName(first, "BASLUS-01270A");
  first.blocks[0U] = 2U;
  first.blocks[1U] = 0U;
  first.block_count = 2U;
  first.used = true;

  auto &second = state.files[1U];
  setName(second, "SECOND");
  second.blocks[0U] = 4U;
  second.block_count = 1U;
  second.used = true;

  for (std::size_t block{}; block < state.blocks.size(); ++block) {
    for (std::size_t offset{}; offset < state.blocks[block].size(); ++offset) {
      const auto value =
          static_cast<std::uint8_t>((block * 17U + offset * 3U) & 0xffU);
      state.blocks[block][offset] = std::byte{value};
    }
  }
  state.dirty_generation = 9U;
  require(sf::psx::MemoryCardHle::validateState(state),
          "Memory Card fixture is invalid");
  return state;
}

[[nodiscard]] bool durableEqual(const sf::psx::MemoryCardHleState &left,
                                const sf::psx::MemoryCardHleState &right) {
  return left.files == right.files && left.blocks == right.blocks &&
         left.present == right.present && left.formatted == right.formatted;
}

void corruptHeader(const std::filesystem::path &path) {
  std::fstream stream{path, std::ios::binary | std::ios::in | std::ios::out};
  require(static_cast<bool>(stream), "Could not open test card for corruption");
  const std::array<char, 1U> value{'X'};
  stream.write(value.data(), static_cast<std::streamsize>(value.size()));
  stream.flush();
  require(static_cast<bool>(stream), "Could not corrupt test card header");
}

void testCanonicalEncodeAndRoundTrip() {
  const auto state = makeState();
  sf::psx::MemoryCardRawImage image{};
  require(static_cast<bool>(sf::psx::MemoryCardImage::encode(state, image)),
          "Memory Card encode failed");

  require(image.size() == 128U * 1024U && image[0U] == 'M' &&
              image[1U] == 'C' && image[0x7fU] == 0x0eU,
          "Memory Card header frame is not canonical");
  const auto *tail = image.data() + 1U * frame_size;
  const auto *free = image.data() + 2U * frame_size;
  const auto *head = image.data() + 3U * frame_size;
  require(read32(tail) == 0x53U && read16(tail + 0x08U) == 0xffffU &&
              read32(free) == 0xa0U && free[0x7fU] == 0xa0U &&
              read32(head) == 0x51U && read32(head + 0x04U) == 0x4000U &&
              read16(head + 0x08U) == 0U,
          "Memory Card directory chain encoding mismatch");
  const auto *broken = image.data() + 16U * frame_size;
  require(read32(broken) == 0xffffffffU && read16(broken + 0x08U) == 0xffffU &&
              broken[0x7fU] == 0U,
          "Memory Card unused broken-sector frame mismatch");
  const auto *write_test = image.data() + 63U * frame_size;
  require(write_test[0U] == 'M' && write_test[1U] == 'C' &&
              write_test[0x7fU] == 0x0eU,
          "Memory Card write-test frame mismatch");
  require(image[3U * sf::psx::memory_card_block_size] ==
              std::to_integer<std::uint8_t>(state.blocks[2U][0U]),
          "Memory Card physical block payload mapping mismatch");

  sf::psx::MemoryCardHleState decoded{};
  decoded.present = false;
  decoded.formatted = false;
  require(static_cast<bool>(sf::psx::MemoryCardImage::decode(image, decoded)),
          "Memory Card decode failed");
  require(durableEqual(state, decoded) && decoded.dirty_generation == 0U &&
              decoded.pending.kind == sf::psx::MemoryCardPendingKind::none,
          "Memory Card durable round-trip mismatch");
}

void testBrokenSectorRemap() {
  const auto state = makeState();
  sf::psx::MemoryCardRawImage image{};
  require(static_cast<bool>(sf::psx::MemoryCardImage::encode(state, image)),
          "Memory Card remap fixture encode failed");

  auto *broken = image.data() + 16U * frame_size;
  write32(broken, 64U);
  finish(broken);
  auto *replacement = image.data() + 36U * frame_size;
  std::fill_n(replacement, frame_size, static_cast<std::uint8_t>(0x5cU));

  sf::psx::MemoryCardHleState decoded{};
  require(static_cast<bool>(sf::psx::MemoryCardImage::decode(image, decoded)),
          "Memory Card broken-sector remap decode failed");
  for (std::size_t index{}; index < frame_size; ++index) {
    require(std::to_integer<std::uint8_t>(decoded.blocks[0U][index]) == 0x5cU,
            "Memory Card replacement sector was not applied");
  }
}

void testCorruptionDoesNotCommit() {
  const auto state = makeState();
  sf::psx::MemoryCardRawImage image{};
  require(static_cast<bool>(sf::psx::MemoryCardImage::encode(state, image)),
          "Memory Card corruption fixture encode failed");

  auto destination = makeState();
  destination.dirty_generation = 77U;
  const auto original = destination;
  image[3U * frame_size + 0x7fU] ^= 1U;
  const auto checksum_result =
      sf::psx::MemoryCardImage::decode(image, destination);
  require(checksum_result.error ==
                  sf::psx::MemoryCardImageError::invalid_checksum &&
              destination == original,
          "Bad Memory Card checksum changed live state");

  require(static_cast<bool>(sf::psx::MemoryCardImage::encode(state, image)),
          "Memory Card bad-chain fixture encode failed");
  auto *head = image.data() + 3U * frame_size;
  write16(head + 0x08U, 15U);
  finish(head);
  const auto chain_result =
      sf::psx::MemoryCardImage::decode(image, destination);
  require(chain_result.error ==
                  sf::psx::MemoryCardImageError::invalid_directory &&
              destination == original,
          "Bad Memory Card chain changed live state");
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mohu-memory-card-image-" + std::to_string(stamp));
    require(std::filesystem::create_directory(path_),
            "Could not create Memory Card test directory");
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_{};
};

void testAtomicStoreLoadAndRecovery() {
  TemporaryDirectory directory;
  const auto path = directory.path() / "fresh-install" / "Saves" / "card.mcr";
  auto state = makeState();

  sf::psx::MemoryCardHleState blank{};
  const auto blank_result = sf::psx::MemoryCardImage::load(path, blank);
  require(blank_result &&
              blank_result.load_source ==
                  sf::psx::MemoryCardImageLoadSource::blank &&
              blank.files[0U].used == false &&
              std::to_integer<std::uint8_t>(blank.blocks[0U][0U]) == 0xffU,
          "Missing Memory Card did not produce a blank formatted card");

  require(static_cast<bool>(sf::psx::MemoryCardImage::storeAtomic(path, state)),
          "Atomic Memory Card store failed");
  auto backup_path = path;
  backup_path += ".bak";
  require(std::filesystem::file_size(path) ==
                  sf::psx::memory_card_raw_image_size &&
              std::filesystem::file_size(backup_path) ==
                  sf::psx::memory_card_raw_image_size,
          "Atomic Memory Card store did not maintain primary and backup");

  sf::psx::MemoryCardHleState loaded{};
  const auto primary_result = sf::psx::MemoryCardImage::load(path, loaded);
  require(primary_result &&
              primary_result.load_source ==
                  sf::psx::MemoryCardImageLoadSource::primary &&
              durableEqual(state, loaded),
          "Stored Memory Card did not load from primary");

  const auto previous = state;
  state.blocks[0U][0U] = std::byte{0x42U};
  ++state.dirty_generation;
  require(static_cast<bool>(sf::psx::MemoryCardImage::storeAtomic(path, state)),
          "Second atomic Memory Card store failed");
  sf::psx::MemoryCardHleState backup_state{};
  require(static_cast<bool>(
              sf::psx::MemoryCardImage::load(backup_path, backup_state)) &&
              durableEqual(previous, backup_state),
          "Memory Card backup does not contain the previous valid primary");

  auto invalid = state;
  invalid.formatted = false;
  const auto invalid_result =
      sf::psx::MemoryCardImage::storeAtomic(path, invalid);
  require(invalid_result.error == sf::psx::MemoryCardImageError::not_formatted,
          "Invalid Memory Card state was stored");
  sf::psx::MemoryCardHleState preserved{};
  require(static_cast<bool>(sf::psx::MemoryCardImage::load(path, preserved)) &&
              durableEqual(state, preserved),
          "Failed atomic store damaged the previous image");

  const auto degraded_path =
      directory.path() / "backup-unavailable" / "card.mcr";
  auto degraded_backup = degraded_path;
  degraded_backup += ".bak";
  require(std::filesystem::create_directories(degraded_backup),
          "Could not create unavailable backup fixture");
  require(static_cast<bool>(
              sf::psx::MemoryCardImage::storeAtomic(degraded_path, state)),
          "A post-commit backup failure incorrectly failed the save");
  sf::psx::MemoryCardHleState degraded_loaded{};
  const auto degraded_result =
      sf::psx::MemoryCardImage::load(degraded_path, degraded_loaded);
  require(degraded_result &&
              degraded_result.load_source ==
                  sf::psx::MemoryCardImageLoadSource::primary &&
              durableEqual(state, degraded_loaded) &&
              std::filesystem::is_directory(degraded_backup),
          "Post-commit backup failure damaged the durable primary");

  corruptHeader(path);
  sf::psx::MemoryCardHleState recovered{};
  const auto recovered_result = sf::psx::MemoryCardImage::load(path, recovered);
  require(recovered_result &&
              recovered_result.load_source ==
                  sf::psx::MemoryCardImageLoadSource::backup &&
              durableEqual(previous, recovered),
          "Memory Card did not recover from its valid backup");

  corruptHeader(backup_path);
  auto untouched = makeState();
  untouched.dirty_generation = 99U;
  const auto before = untouched;
  const auto corrupt_result = sf::psx::MemoryCardImage::load(path, untouched);
  require(corrupt_result.error ==
                  sf::psx::MemoryCardImageError::invalid_header &&
              untouched == before,
          "Two corrupt Memory Card images changed live state");
}

} // namespace

int main() {
  try {
    testCanonicalEncodeAndRoundTrip();
    testBrokenSectorRemap();
    testCorruptionDoesNotCommit();
    testAtomicStoreLoadAndRecovery();
  } catch (const std::exception &exception) {
    std::cerr << "memory_card_image_tests: " << exception.what() << '\n';
    return 1;
  }
  std::cout << "memory_card_image_tests: all checks passed\n";
  return 0;
}
