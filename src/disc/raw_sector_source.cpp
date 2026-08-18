#include "sf/disc/raw_sector_source.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace sf::disc {

RawSectorSource::RawSectorSource(DataTrack track, std::ifstream stream,
                                 std::uint32_t sector_count)
    : track_(std::move(track)), stream_(std::move(stream)),
      sector_count_(sector_count),
      read_ahead_(read_ahead_sector_count * raw_sector_size) {}

RawSectorSource RawSectorSource::open(const std::filesystem::path &cue_path) {
  auto track = CueSheet::load(cue_path).dataTrack();
  if (track.mode != TrackMode::mode2_2352) {
    throw core::Error{core::ErrorCode::unsupported,
                      "Raw XA streaming requires a MODE2/2352 data track"};
  }

  std::error_code file_error;
  const auto file_size =
      std::filesystem::file_size(track.binary_path, file_error);
  if (file_error) {
    throw core::Error{core::ErrorCode::io,
                      "Cannot query track size: " + track.binary_path.string()};
  }
  if (file_size % raw_sector_size != 0) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "MODE2/2352 track has a partial trailing sector"};
  }

  const auto physical_sector_count = file_size / raw_sector_size;
  if (physical_sector_count < track.index_lba) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "CUE INDEX lies beyond the end of the track"};
  }
  const auto logical_sector_count = physical_sector_count - track.index_lba;
  if (logical_sector_count > std::numeric_limits<std::uint32_t>::max()) {
    throw core::Error{core::ErrorCode::unsupported,
                      "Track contains too many sectors"};
  }

  std::ifstream stream{track.binary_path, std::ios::binary};
  if (!stream) {
    throw core::Error{core::ErrorCode::io, "Cannot open track binary: " +
                                               track.binary_path.string()};
  }
  return RawSectorSource{std::move(track), std::move(stream),
                         static_cast<std::uint32_t>(logical_sector_count)};
}

std::uint64_t RawSectorSource::byteOffset(std::uint32_t lba) const {
  if (lba >= sector_count_) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "Raw sector LBA is outside the data track: " +
                          std::to_string(lba)};
  }
  return (static_cast<std::uint64_t>(track_.index_lba) + lba) * raw_sector_size;
}

RawSectorSource::Sector RawSectorSource::readSector(std::uint32_t lba) {
  Sector sector{};
  readSectors(lba, sector);
  return sector;
}

bool RawSectorSource::addUserDataOverlay(std::uint32_t first_lba,
                                         std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return false;
  }
  const auto overlay_sector_count =
      (bytes.size() + user_data_size - 1U) / user_data_size;
  if (first_lba >= sector_count_ ||
      overlay_sector_count > sector_count_ - first_lba) {
    return false;
  }
  const auto end_lba =
      static_cast<std::uint64_t>(first_lba) + overlay_sector_count;
  for (const auto &overlay : user_data_overlays_) {
    const auto existing_sector_count =
        (overlay.bytes.size() + user_data_size - 1U) / user_data_size;
    const auto existing_end =
        static_cast<std::uint64_t>(overlay.first_lba) + existing_sector_count;
    if (first_lba < existing_end && overlay.first_lba < end_lba) {
      return false;
    }
  }
  user_data_overlays_.push_back(
      UserDataOverlay{first_lba, {bytes.begin(), bytes.end()}});
  return true;
}

void RawSectorSource::applyUserDataOverlays(
    std::uint32_t first_lba, std::span<std::byte> destination) const {
  const auto requested_sector_count = destination.size() / raw_sector_size;
  const auto requested_end =
      static_cast<std::uint64_t>(first_lba) + requested_sector_count;
  for (const auto &overlay : user_data_overlays_) {
    const auto overlay_sector_count =
        (overlay.bytes.size() + user_data_size - 1U) / user_data_size;
    const auto overlay_end =
        static_cast<std::uint64_t>(overlay.first_lba) + overlay_sector_count;
    const auto overlap_begin =
        std::max<std::uint64_t>(first_lba, overlay.first_lba);
    const auto overlap_end = std::min(requested_end, overlay_end);
    for (auto lba = overlap_begin; lba < overlap_end; ++lba) {
      const auto source_offset =
          static_cast<std::size_t>(lba - overlay.first_lba) * user_data_size;
      const auto copy_size =
          std::min(user_data_size, overlay.bytes.size() - source_offset);
      const auto destination_offset =
          static_cast<std::size_t>(lba - first_lba) * raw_sector_size +
          user_data_offset;
      std::copy_n(overlay.bytes.begin() +
                      static_cast<std::ptrdiff_t>(source_offset),
                  copy_size,
                  destination.begin() +
                      static_cast<std::ptrdiff_t>(destination_offset));
    }
  }
}

bool RawSectorSource::readAheadContains(std::uint32_t lba) const noexcept {
  return lba >= read_ahead_first_lba_ &&
         lba - read_ahead_first_lba_ < read_ahead_count_;
}

void RawSectorSource::readTrackSectors(std::uint32_t first_lba,
                                       std::span<std::byte> destination) {
  const auto offset = byteOffset(first_lba);
  if (offset > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamoff>::max()) ||
      destination.size() > static_cast<std::size_t>(
                               std::numeric_limits<std::streamsize>::max())) {
    throw core::Error{core::ErrorCode::io,
                      "Raw sector read exceeds host stream limits"};
  }

  if (!stream_position_valid_ || next_stream_lba_ != first_lba) {
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  }
  stream_.read(reinterpret_cast<char *>(destination.data()),
               static_cast<std::streamsize>(destination.size()));
  if (stream_.gcount() != static_cast<std::streamsize>(destination.size())) {
    stream_position_valid_ = false;
    throw core::Error{core::ErrorCode::io,
                      "Unexpected end of track while reading raw LBA " +
                          std::to_string(first_lba)};
  }

  const auto sectors_read = destination.size() / raw_sector_size;
  next_stream_lba_ = first_lba + static_cast<std::uint32_t>(sectors_read);
  stream_position_valid_ = true;
}

void RawSectorSource::refillReadAhead(std::uint32_t first_lba) {
  const auto count = std::min<std::size_t>(
      read_ahead_sector_count,
      static_cast<std::size_t>(sector_count_ - first_lba));
  readTrackSectors(first_lba, std::span<std::byte>{read_ahead_}.first(
                                  count * raw_sector_size));
  read_ahead_first_lba_ = first_lba;
  read_ahead_count_ = static_cast<std::uint32_t>(count);
}

void RawSectorSource::readSectors(std::uint32_t first_lba,
                                  std::span<std::byte> destination) {
  if (destination.empty() || destination.size() % raw_sector_size != 0) {
    throw core::Error{
        core::ErrorCode::invalid_argument,
        "Raw sector destination must contain one or more complete sectors"};
  }

  const auto requested_sectors = destination.size() / raw_sector_size;
  if (requested_sectors > sector_count_ ||
      first_lba > sector_count_ - requested_sectors) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "Raw sector read crosses the end of the data track"};
  }

  // Large callers already amortize the host read themselves. Keep their
  // exact one-read behavior and use the cache for the CD controller's
  // sector-at-a-time 75/150 Hz stream.
  if (requested_sectors >= read_ahead_sector_count) {
    readTrackSectors(first_lba, destination);
    applyUserDataOverlays(first_lba, destination);
    return;
  }

  std::size_t copied_sectors = 0U;
  while (copied_sectors < requested_sectors) {
    const auto lba = first_lba + static_cast<std::uint32_t>(copied_sectors);
    if (!readAheadContains(lba)) {
      refillReadAhead(lba);
    }

    const auto cache_offset =
        static_cast<std::size_t>(lba - read_ahead_first_lba_);
    const auto available = std::min<std::size_t>(
        requested_sectors - copied_sectors,
        static_cast<std::size_t>(read_ahead_count_) - cache_offset);
    const auto byte_count = available * raw_sector_size;
    std::copy_n(read_ahead_.begin() +
                    static_cast<std::ptrdiff_t>(cache_offset * raw_sector_size),
                byte_count,
                destination.begin() + static_cast<std::ptrdiff_t>(
                                          copied_sectors * raw_sector_size));
    copied_sectors += available;
  }
  applyUserDataOverlays(first_lba, destination);
}

} // namespace sf::disc
