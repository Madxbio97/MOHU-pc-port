#pragma once

#include "sf/psx/machine.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace mohu {

struct GpuDisplayState {
  std::uint16_t x{};
  std::uint16_t y{};
  std::uint16_t width{256U};
  std::uint16_t height{240U};
  bool enabled{true};
  bool rgb24{};
  bool interlaced{};
};

class GpuCommandStream final : public sf::psx::GpuPort {
public:
  void beginFrame() noexcept;
  void setProjectionTracking(bool enabled) noexcept {
    if (projection_tracking_ != enabled) {
      projection_tracking_ = enabled;
      frame_projections_.clear();
    }
  }
  [[nodiscard]] bool projectionTracking() const noexcept {
    return projection_tracking_;
  }

  void setProjectionIdentityTracking(bool enabled) noexcept {
    if (projection_identity_tracking_ != enabled) {
      projection_identity_tracking_ = enabled;
      frame_projection_identities_.clear();
    }
  }
  [[nodiscard]] bool projectionIdentityTracking() const noexcept {
    return projection_identity_tracking_;
  }
  [[nodiscard]] bool writeGp0(std::uint32_t value) noexcept override;
  [[nodiscard]] bool
  writeGp0Projected(std::uint32_t value,
                    const sf::psx::GteProjectedVertex *projected,
                    std::uint64_t source_identity) noexcept override;
  [[nodiscard]] bool
  writeGp0FromRam(std::uint32_t value, std::uint32_t source_address,
                  const sf::psx::GteProjectedVertex *projected,
                  std::uint64_t source_identity = 0U) noexcept override;
  [[nodiscard]] bool
  writeGp0FromRam(std::uint32_t value, sf::psx::GpuDmaWordSource source,
                  const sf::psx::GteProjectedVertex *projected,
                  std::uint64_t source_identity) noexcept override;
  void writeGp1(std::uint32_t value) noexcept override;
  [[nodiscard]] bool readGp0(std::uint32_t &value) noexcept override;
  [[nodiscard]] std::uint32_t readStatus() const noexcept override;
  [[nodiscard]] std::span<const std::uint32_t> frameWords() const noexcept {
    return frame_words_;
  }
  [[nodiscard]] std::span<const sf::psx::GteProjectedVertex>
  frameProjections() const noexcept {
    return frame_projections_;
  }
  [[nodiscard]] std::span<const std::uint64_t>
  frameProjectionIdentities() const noexcept {
    return frame_projection_identities_;
  }
  [[nodiscard]] std::span<const sf::psx::GpuDmaWordSource>
  frameDmaSources() const noexcept {
    return frame_dma_sources_;
  }
  [[nodiscard]] bool overrideFrameProjectionIdentities(
      std::span<const std::size_t> word_indices,
      std::span<const std::uint16_t> identities) noexcept {
    if (!projection_identity_tracking_ ||
        word_indices.size() != identities.size()) {
      return false;
    }
    for (std::size_t index{}; index < word_indices.size(); ++index) {
      if (identities[index] == 0U ||
          word_indices[index] >= frame_projection_identities_.size()) {
        return false;
      }
    }
    for (std::size_t index{}; index < word_indices.size(); ++index) {
      frame_projection_identities_[word_indices[index]] = identities[index];
    }
    return true;
  }
  [[nodiscard]] std::span<sf::psx::GteProjectedVertex>
  mutableFrameProjections() noexcept {
    return frame_projections_;
  }
  [[nodiscard]] std::span<const std::uint32_t> firstWords() const noexcept {
    return std::span{first_words_}.first(first_word_count_);
  }
  [[nodiscard]] std::uint64_t totalGp0Words() const noexcept {
    return total_gp0_words_;
  }
  [[nodiscard]] std::uint64_t totalGp1Words() const noexcept {
    return total_gp1_words_;
  }
  [[nodiscard]] std::uint64_t commandBufferEpoch() const noexcept {
    return command_buffer_epoch_;
  }
  [[nodiscard]] const GpuDisplayState &displayState() const noexcept {
    return display_state_;
  }

private:
  [[nodiscard]] bool appendGp0(std::uint32_t value,
                               const sf::psx::GteProjectedVertex *projected,
                               std::uint64_t source_identity,
                               sf::psx::GpuDmaWordSource dma_source) noexcept;
  static constexpr std::uint32_t reset_status = 0x14802000U;
  std::vector<std::uint32_t> frame_words_;
  std::vector<sf::psx::GteProjectedVertex> frame_projections_;
  std::vector<std::uint64_t> frame_projection_identities_;
  std::vector<sf::psx::GpuDmaWordSource> frame_dma_sources_;
  bool projection_tracking_{true};
  bool projection_identity_tracking_{true};
  std::array<std::uint32_t, 16U> first_words_{};
  std::size_t first_word_count_{};
  std::uint32_t status_{reset_status};
  GpuDisplayState display_state_{};
  std::uint64_t total_gp0_words_{};
  std::uint64_t total_gp1_words_{};
  std::uint64_t command_buffer_epoch_{};
};

} // namespace mohu
