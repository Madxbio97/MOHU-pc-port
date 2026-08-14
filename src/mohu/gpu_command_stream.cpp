#include "mohu/gpu_command_stream.hpp"
#include "sf/psx/gp0_command.hpp"

namespace mohu {

void GpuCommandStream::resetFrameCapture() noexcept {
  frame_words_.clear();
  frame_projections_.clear();
  frame_projection_identities_.clear();
  frame_dma_sources_.clear();
  dma_sidecar_disabled_ = false;
  gp0_boundary_state_ = Gp0BoundaryState::command_start;
  gp0_words_remaining_ = 0U;
  gp0_polyline_minimum_remaining_ = 0U;
}

void GpuCommandStream::beginFrame() noexcept { resetFrameCapture(); }

void GpuCommandStream::advanceGp0Boundary(std::uint32_t value) noexcept {
  switch (gp0_boundary_state_) {
  case Gp0BoundaryState::command_start: {
    const auto opcode = static_cast<std::uint8_t>(value >> 24U);
    if (opcode >= 0xa0U && opcode < 0xc0U) {
      gp0_boundary_state_ = Gp0BoundaryState::cpu_to_vram_header;
      gp0_words_remaining_ = 2U;
      return;
    }
    if ((opcode >= 0x48U && opcode < 0x50U) ||
        (opcode >= 0x58U && opcode < 0x60U)) {
      gp0_boundary_state_ = Gp0BoundaryState::polyline;
      gp0_polyline_minimum_remaining_ = opcode < 0x50U ? 2U : 3U;
      return;
    }
    const auto length = opcode >= 0xc0U && opcode < 0xe0U
                            ? 3U
                            : sf::psx::gp0FixedCommandLength(opcode);
    if (length > 1U) {
      gp0_boundary_state_ = Gp0BoundaryState::fixed_payload;
      gp0_words_remaining_ = length - 1U;
    }
    return;
  }
  case Gp0BoundaryState::fixed_payload:
  case Gp0BoundaryState::cpu_to_vram_payload:
    if (--gp0_words_remaining_ == 0U) {
      gp0_boundary_state_ = Gp0BoundaryState::command_start;
    }
    return;
  case Gp0BoundaryState::polyline:
    if (gp0_polyline_minimum_remaining_ != 0U) {
      --gp0_polyline_minimum_remaining_;
    } else if (sf::psx::gp0IsPolylineTerminator(value)) {
      gp0_boundary_state_ = Gp0BoundaryState::command_start;
    }
    return;
  case Gp0BoundaryState::cpu_to_vram_header:
    if (--gp0_words_remaining_ != 0U) {
      return;
    }
    const auto pixels =
        static_cast<std::size_t>(sf::psx::gp0TransferWidth(value)) *
        sf::psx::gp0TransferHeight(value);
    gp0_words_remaining_ = (pixels + 1U) / 2U;
    gp0_boundary_state_ = Gp0BoundaryState::cpu_to_vram_payload;
    return;
  }
}

bool GpuCommandStream::appendGp0(
    std::uint32_t value, const sf::psx::GteProjectedVertex *projected,
    std::uint64_t source_identity,
    sf::psx::GpuDmaWordSource dma_source) noexcept {
  const auto words_size = frame_words_.size();
  const auto projections_size = frame_projections_.size();
  const auto identities_size = frame_projection_identities_.size();
  const auto sources_size = frame_dma_sources_.size();
  const auto command_start =
      gp0_boundary_state_ == Gp0BoundaryState::command_start;
  const auto opcode = static_cast<std::uint8_t>(value >> 24U);
  const auto disable_dma_sidecar =
      command_start && opcode >= 0x80U && opcode < 0xc0U;
  try {
    frame_words_.push_back(value);
    if (projection_tracking_) {
      frame_projections_.push_back(
          projected != nullptr ? *projected : sf::psx::GteProjectedVertex{});
    }
    if (projection_identity_tracking_ &&
        (!frame_projection_identities_.empty() || source_identity != 0U)) {
      if (frame_projection_identities_.empty()) {
        frame_projection_identities_.resize(words_size);
      }
      frame_projection_identities_.push_back(source_identity);
    }
    if (!dma_sidecar_disabled_ && !disable_dma_sidecar &&
        (!frame_dma_sources_.empty() || dma_source.valid())) {
      if (frame_dma_sources_.empty()) {
        frame_dma_sources_.resize(words_size);
      }
      frame_dma_sources_.push_back(dma_source);
    }
  } catch (...) {
    while (frame_dma_sources_.size() > sources_size) {
      frame_dma_sources_.pop_back();
    }
    while (frame_projection_identities_.size() > identities_size) {
      frame_projection_identities_.pop_back();
    }
    while (frame_projections_.size() > projections_size) {
      frame_projections_.pop_back();
    }
    while (frame_words_.size() > words_size) {
      frame_words_.pop_back();
    }
    return false;
  }
  if (disable_dma_sidecar) {
    frame_dma_sources_.clear();
    dma_sidecar_disabled_ = true;
  }
  advanceGp0Boundary(value);
  if (first_word_count_ < first_words_.size()) {
    first_words_[first_word_count_] = value;
    ++first_word_count_;
  }
  ++total_gp0_words_;
  return true;
}

bool GpuCommandStream::writeGp0(std::uint32_t value) noexcept {
  return appendGp0(value, nullptr, 0U, {});
}

bool GpuCommandStream::writeGp0Projected(
    std::uint32_t value, const sf::psx::GteProjectedVertex *projected,
    std::uint64_t source_identity) noexcept {
  return appendGp0(value, projected, source_identity, {});
}

bool GpuCommandStream::writeGp0FromRam(
    std::uint32_t value, std::uint32_t source_address,
    const sf::psx::GteProjectedVertex *projected,
    std::uint64_t source_identity) noexcept {
  return writeGp0FromRam(
      value,
      sf::psx::GpuDmaWordSource{source_address, source_address,
                                sf::psx::GpuDmaSourceKind::linear},
      projected, source_identity);
}

bool GpuCommandStream::writeGp0FromRam(
    std::uint32_t value, sf::psx::GpuDmaWordSource source,
    const sf::psx::GteProjectedVertex *projected,
    std::uint64_t source_identity) noexcept {
  return appendGp0(value, projected, source_identity, source);
}

void GpuCommandStream::writeGp1(std::uint32_t value) noexcept {
  ++total_gp1_words_;
  const auto command = static_cast<std::uint8_t>(value >> 24U);
  switch (command) {
  case 0x00U:
    status_ = reset_status;
    display_state_ = {};
    ++command_buffer_epoch_;
    resetFrameCapture();
    break;
  case 0x01U:
    ++command_buffer_epoch_;
    resetFrameCapture();
    break;
  case 0x02U:
    status_ &= ~(1U << 24U);
    break;
  case 0x03U:
    status_ = (status_ & ~(1U << 23U)) | ((value & 1U) << 23U);
    display_state_.enabled = (value & 1U) == 0U;
    break;
  case 0x04U: {
    status_ = (status_ & ~(3U << 29U)) | ((value & 3U) << 29U);
    const auto direction = value & 3U;
    const auto dma_request = direction == 1U   ? (status_ >> 27U) & 1U
                             : direction == 2U ? (status_ >> 28U) & 1U
                             : direction == 3U ? (status_ >> 27U) & 1U
                                               : 0U;
    status_ = (status_ & ~(1U << 25U)) | (dma_request << 25U);
    break;
  }
  case 0x05U:
    display_state_.x = static_cast<std::uint16_t>(value & 0x03ffU);
    display_state_.y = static_cast<std::uint16_t>((value >> 10U) & 0x01ffU);
    break;
  case 0x08U: {
    constexpr std::array<std::uint16_t, 4U> horizontal_resolutions{256U, 320U,
                                                                   512U, 640U};
    display_state_.width =
        (value & 0x40U) != 0U
            ? 368U
            : horizontal_resolutions[static_cast<std::size_t>(value & 3U)];
    display_state_.interlaced = (value & 0x20U) != 0U;
    display_state_.height =
        (value & 0x04U) != 0U && display_state_.interlaced ? 480U : 240U;
    display_state_.rgb24 = (value & 0x10U) != 0U;
    status_ = (status_ & ~0x007f0000U) | ((value & 0x3fU) << 17U) |
              ((value & 0x40U) << 10U);
    break;
  }
  default:
    break;
  }
}

bool GpuCommandStream::readGp0(std::uint32_t &value) noexcept {
  value = 0U;
  return false;
}

std::uint32_t GpuCommandStream::readStatus() const noexcept { return status_; }
} // namespace mohu
