#include "sf/psx/r3000_runtime.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace sf::psx {
namespace {

constexpr std::uint32_t physical_address_mask = 0x1fffffffU;
constexpr std::uint32_t ram_mirror_end = 0x00800000U;
constexpr std::uint32_t scratchpad_address = 0x1f800000U;
constexpr std::uint32_t mmio_address = 0x1f801000U;

std::uint32_t signExtend16(std::uint32_t value) noexcept {
  const auto signed_value = static_cast<std::int16_t>(value & 0xffffU);
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(signed_value));
}

std::int32_t asSigned(std::uint32_t value) noexcept {
  return std::bit_cast<std::int32_t>(value);
}

std::uint64_t pgxpLineageMix(std::uint64_t first,
                             std::uint64_t second) noexcept {
  auto value =
      first ^ (second + 0x9e3779b97f4a7c15ULL + (first << 6U) + (first >> 2U));
  value ^= value >> 30U;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27U;
  value *= 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

std::uint32_t arithmeticShiftRight(std::uint32_t value,
                                   std::uint32_t amount) noexcept {
  amount &= 31U;
  if (amount == 0U) {
    return value;
  }
  const auto fill = (value & 0x80000000U) != 0U
                        ? std::numeric_limits<std::uint32_t>::max()
                              << (32U - amount)
                        : 0U;
  return (value >> amount) | fill;
}

bool addOverflows(std::uint32_t left, std::uint32_t right,
                  std::uint32_t &result) noexcept {
  const auto wide = static_cast<std::int64_t>(asSigned(left)) +
                    static_cast<std::int64_t>(asSigned(right));
  if (wide < std::numeric_limits<std::int32_t>::min() ||
      wide > std::numeric_limits<std::int32_t>::max()) {
    return true;
  }
  result = static_cast<std::uint32_t>(static_cast<std::int32_t>(wide));
  return false;
}

bool subtractOverflows(std::uint32_t left, std::uint32_t right,
                       std::uint32_t &result) noexcept {
  const auto wide = static_cast<std::int64_t>(asSigned(left)) -
                    static_cast<std::int64_t>(asSigned(right));
  if (wide < std::numeric_limits<std::int32_t>::min() ||
      wide > std::numeric_limits<std::int32_t>::max()) {
    return true;
  }
  result = static_cast<std::uint32_t>(static_cast<std::int32_t>(wide));
  return false;
}

} // namespace

std::string_view toString(R3000StopReason reason) noexcept {
  switch (reason) {
  case R3000StopReason::running:
    return "running";
  case R3000StopReason::returned:
    return "returned";
  case R3000StopReason::instruction_budget:
    return "instruction budget";
  case R3000StopReason::unsupported_instruction:
    return "unsupported instruction";
  case R3000StopReason::memory_fault:
    return "memory fault";
  case R3000StopReason::alignment_fault:
    return "alignment fault";
  case R3000StopReason::arithmetic_overflow:
    return "arithmetic overflow";
  case R3000StopReason::syscall:
    return "syscall";
  case R3000StopReason::breakpoint:
    return "breakpoint";
  }
  return "unknown";
}

R3000Runtime::R3000Runtime() : ram_(ram_size) {}

bool R3000Runtime::setGpuProjectionCatalogTracking(bool enabled) noexcept {
  if (enabled == gpu_projection_catalog_tracking_) {
    return !enabled || (gpu_projection_catalog_ != nullptr &&
                        gpu_projection_handle_words_ != nullptr &&
                        !direct_projection_pages_.empty());
  }
  gpu_projection_catalog_size_ = 0U;
  gpu_projection_catalog_overflow_ = false;
  if (!enabled) {
    gpu_projection_catalog_tracking_ = false;
    gpu_projection_catalog_.reset();
    gpu_projection_handle_words_.reset();
    direct_projection_pages_.clear();
    direct_projection_memory_overflow_ = false;
    gpu_projection_handle_gpr_.fill(0U);
    gpu_projection_handle_gpr_mask_ = 0U;
    gpu_projection_handle_load_delay_ = 0U;
    gpu_projection_handle_next_load_delay_ = 0U;
    gpu_projection_handle_words_present_ = false;
    if (!pgxp_transform_tracking_) {
      projected_gpr_.fill({});
      projected_gpr_valid_mask_ = 0U;
      projected_load_delay_ = {};
      projected_next_load_delay_ = {};
    }
    return true;
  }
  auto storage = std::unique_ptr<GteProjectedVertex[]>(
      new (std::nothrow) GteProjectedVertex[gpu_projection_catalog_capacity]);
  auto handle_words = std::unique_ptr<std::uint32_t[]>(
      new (std::nothrow) std::uint32_t[tracked_key_count]{});
  if (storage == nullptr || handle_words == nullptr) {
    return false;
  }
  std::vector<std::unique_ptr<DirectProjectionMemoryValue[]>> direct_pages;
  try {
    direct_pages.resize(pgxp_page_count);
  } catch (...) {
    return false;
  }
  gpu_projection_catalog_ = std::move(storage);
  gpu_projection_handle_words_ = std::move(handle_words);
  direct_projection_pages_ = std::move(direct_pages);
  direct_projection_memory_overflow_ = false;
  gpu_projection_catalog_tracking_ = true;
  return true;
}

void R3000Runtime::beginGpuProjectionFrame() noexcept {
  // Materialize only the few architectural carriers that survive this host
  // boundary. Normal in-frame transport remains a 16-bit catalog handle and
  // never copies the full projection through every GPR/load-delay hop.
  if (gpu_projection_catalog_ != nullptr) {
    for (auto reg = std::size_t{}; reg < projected_gpr_.size(); ++reg) {
      const auto bit = std::uint32_t{1U} << reg;
      if ((gpu_projection_handle_gpr_mask_ & bit) == 0U)
        continue;
      const auto handle = gpu_projection_handle_gpr_[reg];
      if (handle != 0U && handle <= gpu_projection_catalog_size_ &&
          gpu_projection_catalog_[handle - 1U].packed_sxy == state_.gpr[reg]) {
        projected_gpr_[reg] = gpu_projection_catalog_[handle - 1U];
        projected_gpr_valid_mask_ |= bit;
      } else {
        projected_gpr_[reg].valid = false;
        projected_gpr_valid_mask_ &= ~bit;
      }
    }
    const auto retain_delayed = [this](std::uint16_t handle,
                                       const R3000DelayedLoadState &load,
                                       GteProjectedVertex &destination) {
      if (load.valid && handle != 0U &&
          handle <= gpu_projection_catalog_size_ &&
          gpu_projection_catalog_[handle - 1U].packed_sxy == load.value) {
        destination = gpu_projection_catalog_[handle - 1U];
      } else {
        destination.valid = false;
      }
    };
    retain_delayed(gpu_projection_handle_load_delay_, state_.load_delay,
                   projected_load_delay_);
    retain_delayed(gpu_projection_handle_next_load_delay_,
                   state_.next_load_delay, projected_next_load_delay_);
  }
  gpu_projection_catalog_size_ = 0U;
  gpu_projection_catalog_overflow_ = false;
  gpu_projection_handle_gpr_.fill(0U);
  gpu_projection_handle_gpr_mask_ = 0U;
  gpu_projection_handle_load_delay_ = 0U;
  gpu_projection_handle_next_load_delay_ = 0U;
  gpu_projection_handle_words_present_ = false;
  if (++gpu_projection_handle_generation_ == 0U) {
    if (gpu_projection_handle_words_ != nullptr) {
      std::fill_n(gpu_projection_handle_words_.get(), tracked_key_count, 0U);
    }
    gpu_projection_handle_generation_ = 1U;
  }
}

void R3000Runtime::recordGpuProjection(
    GteProjectedVertex &projection) noexcept {
  if (projection.exact_transform && !projection.hasExactTransformProvenance()) {
    projection.exact_transform = false;
  }
  if (!gpu_projection_catalog_tracking_ || gpu_projection_catalog_ == nullptr ||
      gpu_projection_catalog_overflow_ || !projection.valid) {
    projection.source_vertex_id = 0U;
    return;
  }
  if (gpu_projection_catalog_size_ == gpu_projection_catalog_capacity) {
    gpu_projection_catalog_overflow_ = true;
    projection.source_vertex_id = 0U;
    return;
  }
  projection.source_vertex_id = gpu_projection_catalog_size_ + 1U;
  gpu_projection_catalog_[gpu_projection_catalog_size_++] = projection;
}

std::uint16_t R3000Runtime::ensureGpuProjectionHandle(
    GteProjectedVertex &projection) noexcept {
  if (!gpu_projection_catalog_tracking_ || !projection.valid)
    return 0U;
  const auto candidate = projection.source_vertex_id;
  if (candidate != 0U && candidate <= gpu_projection_catalog_size_ &&
      gpu_projection_catalog_[candidate - 1U] == projection) {
    return static_cast<std::uint16_t>(candidate);
  }

  // CPU shadow values can outlive the frame-local catalog. Republish them
  // instead of mistaking an old handle for a current vertex.
  projection.source_vertex_id = 0U;
  recordGpuProjection(projection);
  return projection.source_vertex_id != 0U
             ? static_cast<std::uint16_t>(projection.source_vertex_id)
             : 0U;
}

std::uint16_t R3000Runtime::publishGpuProjectionHandle(
    GteProjectedVertex projection) noexcept {
  projection.source_vertex_id = 0U;
  return ensureGpuProjectionHandle(projection);
}

std::uint16_t R3000Runtime::compactProjectionHandle(
    const GteProjectedVertex *projection,
    std::uint32_t raw_witness) const noexcept {
  if (!gpu_projection_catalog_tracking_ || projection == nullptr ||
      gpu_projection_catalog_ == nullptr || !projection->valid ||
      projection->packed_sxy != raw_witness ||
      projection->source_vertex_id == 0U ||
      projection->source_vertex_id > gpu_projection_catalog_size_) {
    return 0U;
  }
  const auto handle = static_cast<std::uint16_t>(projection->source_vertex_id);
  const auto &catalog_projection = gpu_projection_catalog_[handle - 1U];
  // Handles are frame-local. A retained carrier may have the same numeric
  // handle and packed SXY as an unrelated vertex published next frame; only
  // the complete tuple can prove that the handle still names this vertex.
  return projection == &catalog_projection || *projection == catalog_projection
             ? handle
             : 0U;
}

std::uint16_t
R3000Runtime::compactProjectionHandleAt(std::uint32_t address) const noexcept {
  if (!gpu_projection_catalog_tracking_ ||
      !gpu_projection_handle_words_present_ ||
      gpu_projection_handle_words_ == nullptr) {
    return 0U;
  }
  std::uint32_t key{};
  if (!trackedWordKey(address, key))
    return 0U;
  const auto entry = gpu_projection_handle_words_[key >> 2U];
  if (static_cast<std::uint16_t>(entry >> 16U) !=
      gpu_projection_handle_generation_) {
    return 0U;
  }
  return static_cast<std::uint16_t>(entry);
}

void R3000Runtime::storeCompactProjectionHandle(std::uint32_t address,
                                                std::uint16_t handle) noexcept {
  if (!gpu_projection_catalog_tracking_ || handle == 0U ||
      gpu_projection_handle_words_ == nullptr) {
    return;
  }
  std::uint32_t key{};
  if (!trackedWordKey(address, key))
    return;
  gpu_projection_handle_words_[key >> 2U] =
      (static_cast<std::uint32_t>(gpu_projection_handle_generation_) << 16U) |
      handle;
  gpu_projection_handle_words_present_ = true;
}

void R3000Runtime::invalidateCompactProjectionHandle(
    std::uint32_t address) noexcept {
  if (!gpu_projection_handle_words_present_ ||
      gpu_projection_handle_words_ == nullptr) {
    return;
  }
  std::uint32_t key{};
  if (!trackedWordKey(address, key))
    return;
  auto &entry = gpu_projection_handle_words_[key >> 2U];
  if (static_cast<std::uint16_t>(entry >> 16U) ==
      gpu_projection_handle_generation_) {
    entry = 0U;
  }
}

bool R3000Runtime::exactTransformCarrierTracking() const noexcept {
  return pgxp_transform_tracking_ && exact_tracking_ != nullptr &&
         pgxp_exact_transform_tracking_;
}

void R3000Runtime::clearPgxpCarriers(bool clear_full_projected) noexcept {
  if (clear_full_projected) {
    projected_gpr_.fill({});
    projected_gpr_valid_mask_ = 0U;
    projected_load_delay_ = {};
    projected_next_load_delay_ = {};
  }
  projected_half_gpr_.fill({});
  projected_half_gpr_valid_mask_ = 0U;
  projected_half_load_delay_ = {};
  projected_half_next_load_delay_ = {};
  if (exact_tracking_ != nullptr) {
    exact_tracking_->pgxp_gpr.fill({});
    exact_tracking_->pgxp_load_delay = {};
    exact_tracking_->pgxp_next_load_delay = {};
    exact_tracking_->gpr.fill({});
    exact_tracking_->load_delay = {};
    exact_tracking_->next_load_delay = {};
    exact_tracking_->pgxp_gpr_valid_mask = 0U;
    exact_tracking_->gpr_valid_mask = 0U;
    exact_tracking_->pgxp_load_delay_valid = false;
    exact_tracking_->pgxp_next_load_delay_valid = false;
    exact_tracking_->load_delay_valid = false;
    exact_tracking_->next_load_delay_valid = false;
  }
  if (clear_full_projected)
    state_.gte.projected.fill({});
}

bool R3000Runtime::setPgxpTransformTracking(bool enabled) noexcept {
  if (enabled == pgxp_transform_tracking_) {
    return !enabled ||
           (tracked_words_ != nullptr && tracked_word_presence_ != nullptr &&
            pgxp_contexts_ != nullptr && exact_tracking_ != nullptr &&
            !pgxp_memory_pages_.empty() && !exact_memory_pages_.empty());
  }
  if (!enabled) {
    clearPgxpCarriers(!gpu_projection_catalog_tracking_);
    pgxp_transform_tracking_ = false;
    tracked_words_present_ = false;
    tracked_words_.reset();
    tracked_word_presence_.reset();
    pgxp_contexts_.reset();
    pgxp_memory_pages_.clear();
    exact_memory_pages_.clear();
    exact_tracking_.reset();
    return true;
  }
  auto storage = std::unique_ptr<TrackedWord[]>(
      new (std::nothrow) TrackedWord[tracked_word_capacity]);
  auto presence = std::unique_ptr<std::uint64_t[]>(
      new (std::nothrow) std::uint64_t[tracked_presence_words]{});
  auto exact = std::unique_ptr<ExactTrackingState>(new (std::nothrow)
                                                       ExactTrackingState{});
  auto contexts = std::unique_ptr<PgxpContextSlot[]>(
      new (std::nothrow) PgxpContextSlot[pgxp_context_capacity]);
  if (storage == nullptr || presence == nullptr || exact == nullptr ||
      contexts == nullptr) {
    return false;
  }
  try {
    pgxp_memory_pages_.clear();
    pgxp_memory_pages_.resize(pgxp_page_count);
    exact_memory_pages_.clear();
    exact_memory_pages_.resize(pgxp_page_count);
  } catch (...) {
    pgxp_memory_pages_.clear();
    exact_memory_pages_.clear();
    return false;
  }
  tracked_words_ = std::move(storage);
  tracked_word_presence_ = std::move(presence);
  exact_tracking_ = std::move(exact);
  pgxp_contexts_ = std::move(contexts);
  pgxp_transform_tracking_ = true;
  pgxp_memory_overflow_ = false;
  exact_memory_overflow_ = false;
  tracked_generation_ = 1U;
  tracked_age_ = 1U;
  beginPgxpTransformGeneration();
  return true;
}

void R3000Runtime::setPgxpCpuTracking(bool enabled) noexcept {
  if (enabled == pgxp_cpu_tracking_) {
    return;
  }
  pgxp_cpu_tracking_ = enabled;
  if (pgxp_transform_tracking_) {
    // Mode changes are explicit provenance boundaries. Fail closed instead of
    // letting a CPU-derived carrier survive into Memory-PGXP or vice versa.
    beginPgxpTransformGeneration();
  }
}

void R3000Runtime::setPgxpExactTransformTracking(bool enabled) noexcept {
  if (enabled == pgxp_exact_transform_tracking_)
    return;
  pgxp_exact_transform_tracking_ = enabled;
  if (pgxp_transform_tracking_)
    beginPgxpTransformGeneration();
}

void R3000Runtime::setPgxpExactTransformCaptureEnabled(bool enabled) noexcept {
  pgxp_exact_transform_capture_enabled_ = enabled;
  if (exact_tracking_ != nullptr) {
    exact_tracking_->gte.transform_twin_enabled =
        pgxp_exact_transform_tracking_ && enabled;
  }
}


void R3000Runtime::setPgxpVertexIdentityTracking(bool enabled) noexcept {
  if (enabled == pgxp_vertex_identity_tracking_)
    return;
  pgxp_vertex_identity_tracking_ = enabled;
  if (pgxp_transform_tracking_)
    beginPgxpTransformGeneration();
}

void R3000Runtime::beginPgxpTransformGeneration() noexcept {
  clearPgxpCarriers();
  if (!pgxp_transform_tracking_) {
    return;
  }
  if (++tracked_generation_ == 0U) {
    tracked_generation_ = 1U;
    if (tracked_words_ != nullptr) {
      std::fill_n(tracked_words_.get(), tracked_word_capacity, TrackedWord{});
    }
  }
  tracked_age_ = 1U;
  tracked_words_present_ = false;
  pgxp_memory_overflow_ = false;
  next_pgxp_context_handle_ = 1U;
  exact_memory_overflow_ = false;
  if (tracked_word_presence_ != nullptr) {
    std::fill_n(tracked_word_presence_.get(), tracked_presence_words, 0U);
  }
  exact_tracking_->gte = {};
  exact_tracking_->gte.generation = tracked_generation_;
  exact_tracking_->gte.transform_twin_enabled =
      pgxp_exact_transform_tracking_ && pgxp_exact_transform_capture_enabled_;
  exact_tracking_->gte.source_identity_enabled = pgxp_vertex_identity_tracking_;
}

void R3000Runtime::clearMemory() noexcept {
  std::ranges::fill(ram_, std::byte{0});
  std::ranges::fill(scratchpad_, std::byte{0});
  std::ranges::fill(mmio_, std::byte{0});
  clearDirectProjectionMemory();
  if (pgxp_transform_tracking_) {
    beginPgxpTransformGeneration();
  } else {
    clearPgxpCarriers();
  }
}

void R3000Runtime::loadExecutable(const Executable &executable) {
  clearMemory();
  const auto &header = executable.header();
  if (!loadBytes(header.text_address, executable.text())) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "PS-X EXE text does not fit emulated RAM"};
  }
  const auto stack_pointer = header.stack_address == 0U
                                 ? 0U
                                 : header.stack_address + header.stack_size;
  reset(header.initial_pc, header.initial_gp, stack_pointer);
}

bool R3000Runtime::loadBytes(std::uint32_t address,
                             std::span<const std::byte> bytes) noexcept {
  auto candidate = address;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (memoryByte(candidate) == nullptr ||
        (candidate == std::numeric_limits<std::uint32_t>::max() &&
         index + 1U < bytes.size())) {
      return false;
    }
    ++candidate;
  }
  if (pgxp_transform_tracking_) {
    beginPgxpTransformGeneration();
  }
  clearDirectProjectionMemory();
  for (const auto byte : bytes) {
    *memoryByte(address) = byte;
    ++address;
  }
  return true;
}

bool R3000Runtime::copyBytes(std::uint32_t address,
                             std::span<std::byte> destination) const noexcept {
  auto candidate = address;
  for (std::size_t index = 0; index < destination.size(); ++index) {
    if (memoryByte(candidate) == nullptr ||
        (candidate == std::numeric_limits<std::uint32_t>::max() &&
         index + 1U < destination.size())) {
      return false;
    }
    ++candidate;
  }
  for (auto &byte : destination) {
    byte = *memoryByte(address);
    ++address;
  }
  return true;
}

bool R3000Runtime::restoreRam(std::span<const std::byte> bytes) noexcept {
  if (bytes.size() != ram_.size()) {
    return false;
  }
  std::ranges::copy(bytes, ram_.begin());
  if (pgxp_transform_tracking_) {
    beginPgxpTransformGeneration();
  }
  return true;
}

bool R3000Runtime::restoreScratchpad(
    std::span<const std::byte> bytes) noexcept {
  if (bytes.size() != scratchpad_.size()) {
    return false;
  }
  std::ranges::copy(bytes, scratchpad_.begin());
  if (pgxp_transform_tracking_) {
    beginPgxpTransformGeneration();
  }
  return true;
}

bool R3000Runtime::restoreMmio(std::span<const std::byte> bytes) noexcept {
  if (bytes.size() != mmio_.size()) {
    return false;
  }
  std::ranges::copy(bytes, mmio_.begin());
  return true;
}

void R3000Runtime::reset(std::uint32_t pc, std::uint32_t gp,
                         std::uint32_t sp) noexcept {
  state_ = {};
  state_.pc = pc;
  state_.next_pc = pc + 4U;
  state_.gpr[28] = gp;
  state_.gpr[29] = sp;
  clearLoadDelay();
  if (pgxp_transform_tracking_) {
    beginPgxpTransformGeneration();
  } else {
    clearPgxpCarriers();
  }
}

R3000PgxpTransformCheckpoint
R3000Runtime::capturePgxpTransformCheckpoint() const noexcept {
  if (!pgxp_transform_tracking_ || exact_tracking_ == nullptr) {
    return {};
  }
  return {
      .gte_witness = state_.gte,
      .exact = exact_tracking_->gte,
      .generation = tracked_generation_,
      .valid = true,
  };
}

void R3000Runtime::restoreCpuState(const R3000State &state) noexcept {
  state_ = state;
  state_.gpr[0] = 0U;
  const auto restored_projected = state_.gte.projected;
  // Exact-transform witnesses live outside the serializable CPU state. Drop
  // them on restore, while retaining legacy integer-derived projections whose
  // packed SXY witness is fully contained in the snapshot itself.
  if (pgxp_transform_tracking_) {
    // A BIOS interrupt/callback swaps only CPU state. RAM and scratchpad are
    // unchanged, so their witnessed PGXP words must survive the swap until
    // the queued GPU DMA consumes them. Drop transient register/GTE twins,
    // but keep the memory generation and its raw-value witnesses intact.
    clearPgxpCarriers();
    exact_tracking_->gte = {};
    exact_tracking_->gte.generation = tracked_generation_;
    exact_tracking_->gte.transform_twin_enabled =
        pgxp_exact_transform_tracking_ && pgxp_exact_transform_capture_enabled_;
    exact_tracking_->gte.source_identity_enabled =
        pgxp_vertex_identity_tracking_;
    for (std::size_t index{}; index < restored_projected.size(); ++index) {
      if (restored_projected[index].valid &&
          !restored_projected[index].exact_transform) {
        state_.gte.projected[index] = restored_projected[index];
      }
    }
  } else {
    clearPgxpCarriers();
  }
}

void R3000Runtime::restoreCpuState(
    const R3000State &state,
    const R3000PgxpTransformCheckpoint &checkpoint) noexcept {
  restoreCpuState(state);
  if (!pgxp_transform_tracking_ || exact_tracking_ == nullptr ||
      !checkpoint.valid || checkpoint.generation != tracked_generation_ ||
      checkpoint.exact.generation != tracked_generation_ ||
      checkpoint.gte_witness.data != state.gte.data ||
      checkpoint.gte_witness.control != state.gte.control) {
    return;
  }

  exact_tracking_->gte = checkpoint.exact;
  state_.gte.projected = checkpoint.gte_witness.projected;
}

bool R3000Runtime::beginCall(
    std::uint32_t address, std::span<const std::uint32_t> arguments) noexcept {
  if (arguments.size() > 4U) {
    const auto extra_count = arguments.size() - 4U;
    if (extra_count > (std::numeric_limits<std::uint32_t>::max() - 16U) /
                          sizeof(std::uint32_t)) {
      return false;
    }
    const auto required_bytes =
        16U + static_cast<std::uint32_t>(extra_count * sizeof(std::uint32_t));
    const auto stack_offset =
        state_.gpr[29] & static_cast<std::uint32_t>(ram_size - 1U);
    if (required_bytes > ram_size || (state_.gpr[29] & 3U) != 0U ||
        stack_offset > ram_size - required_bytes) {
      return false;
    }
    for (std::size_t index = 4U; index < arguments.size(); ++index) {
      const auto offset = 16U + static_cast<std::uint32_t>((index - 4U) * 4U);
      std::uint32_t existing{};
      if (!read32(state_.gpr[29] + offset, existing)) {
        return false;
      }
    }
  }

  clearLoadDelay();
  for (std::size_t index = 0; index < 4U; ++index) {
    const auto reg = static_cast<std::uint8_t>(4U + index);
    writeRegister(reg, index < arguments.size() ? arguments[index] : 0U);
  }
  for (std::size_t index = 4U; index < arguments.size(); ++index) {
    const auto stack_offset =
        16U + static_cast<std::uint32_t>((index - 4U) * 4U);
    if (!write32(state_.gpr[29] + stack_offset, arguments[index])) {
      return false;
    }
  }
  writeRegister(31U, return_sentinel);
  state_.pc = address;
  state_.next_pc = address + 4U;
  state_.branch_pc = 0U;
  state_.branch_delay_slot = false;
  return true;
}

void R3000Runtime::completeHostCall() noexcept {
  flushLoadDelay();
  state_.pc = state_.gpr[31];
  state_.next_pc = state_.pc + 4U;
  state_.branch_pc = 0U;
  state_.branch_delay_slot = false;
}

void R3000Runtime::settleLoadDelay() noexcept { flushLoadDelay(); }

void R3000Runtime::setRegister(std::uint8_t reg, std::uint32_t value) noexcept {
  if (reg < state_.gpr.size()) {
    writeRegister(reg, value);
  }
}

bool R3000Runtime::physicalAddress(std::uint32_t address,
                                   std::uint32_t &physical) noexcept {
  physical = address;
  if (address >= 0x80000000U && address < 0xc0000000U) {
    physical &= physical_address_mask;
    // The scratchpad is cached-only on the R3000A and has no KSEG1 alias.
    if (address >= 0xa0000000U && physical >= scratchpad_address &&
        physical < scratchpad_address + scratchpad_size) {
      return false;
    }
  } else if (address >= 0x20000000U) {
    return false;
  }
  return true;
}

std::byte *R3000Runtime::memoryByte(std::uint32_t address) noexcept {
  std::uint32_t physical{};
  if (!physicalAddress(address, physical)) {
    return nullptr;
  }

  if (physical < ram_mirror_end) {
    return &ram_[physical & static_cast<std::uint32_t>(ram_size - 1U)];
  }
  if (physical >= scratchpad_address &&
      physical < scratchpad_address + scratchpad_size) {
    return &scratchpad_[physical - scratchpad_address];
  }
  if (physical >= mmio_address && physical < mmio_address + mmio_size) {
    return &mmio_[physical - mmio_address];
  }
  return nullptr;
}

const std::byte *
R3000Runtime::memoryByte(std::uint32_t address) const noexcept {
  return const_cast<R3000Runtime *>(this)->memoryByte(address);
}

bool R3000Runtime::readMmio(std::uint32_t address, R3000AccessWidth width,
                            std::uint32_t &value) const noexcept {
  std::uint32_t physical{};
  return mmio_bus_ != nullptr && physicalAddress(address, physical) &&
         physical >= mmio_address && physical < mmio_address + mmio_size &&
         mmio_bus_->readMmio(physical, width, value);
}

bool R3000Runtime::writeMmio(std::uint32_t address, R3000AccessWidth width,
                             std::uint32_t value,
                             const GteProjectedVertex *projected,
                             std::uint64_t projection_identity) noexcept {
  std::uint32_t physical{};
  return mmio_bus_ != nullptr && physicalAddress(address, physical) &&
         physical >= mmio_address && physical < mmio_address + mmio_size &&
         mmio_bus_->writeMmio(physical, width, value, projected,
                              projection_identity);
}

bool R3000Runtime::read8(std::uint32_t address,
                         std::uint8_t &value) const noexcept {
  std::uint32_t physical{};
  if (physicalAddress(address, physical) && physical < ram_mirror_end) {
    value = std::to_integer<std::uint8_t>(
        ram_[physical & static_cast<std::uint32_t>(ram_size - 1U)]);
    return true;
  }
  std::uint32_t mmio_value{};
  if (readMmio(address, R3000AccessWidth::byte, mmio_value)) {
    value = static_cast<std::uint8_t>(mmio_value);
    return true;
  }
  const auto *source = memoryByte(address);
  if (source == nullptr) {
    return false;
  }
  value = std::to_integer<std::uint8_t>(*source);
  return true;
}

bool R3000Runtime::read16(std::uint32_t address,
                          std::uint16_t &value) const noexcept {
  if ((address & 1U) != 0U) {
    return false;
  }
  std::uint32_t physical{};
  if (physicalAddress(address, physical) && physical < ram_mirror_end) {
    const auto offset = physical & static_cast<std::uint32_t>(ram_size - 1U);
    value = static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(ram_[offset]) |
        (static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(ram_[offset + 1U]))
         << 8U));
    return true;
  }
  std::uint32_t mmio_value{};
  if (readMmio(address, R3000AccessWidth::halfword, mmio_value)) {
    value = static_cast<std::uint16_t>(mmio_value);
    return true;
  }
  const auto *byte0 = memoryByte(address);
  const auto *byte1 = memoryByte(address + 1U);
  if (byte0 == nullptr || byte1 == nullptr) {
    return false;
  }
  value =
      std::to_integer<std::uint8_t>(*byte0) |
      static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(*byte1) << 8U);
  return true;
}

bool R3000Runtime::read32(std::uint32_t address,
                          std::uint32_t &value) const noexcept {
  if ((address & 3U) != 0U) {
    return false;
  }
  std::uint32_t physical{};
  if (physicalAddress(address, physical) && physical < ram_mirror_end) {
    const auto offset = physical & static_cast<std::uint32_t>(ram_size - 1U);
    value = std::to_integer<std::uint8_t>(ram_[offset]) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 1U]))
             << 8U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 2U]))
             << 16U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 3U]))
             << 24U);
    return true;
  }
  std::uint32_t mmio_value{};
  if (readMmio(address, R3000AccessWidth::word, mmio_value)) {
    value = mmio_value;
    return true;
  }
  const auto *byte0 = memoryByte(address);
  const auto *byte1 = memoryByte(address + 1U);
  const auto *byte2 = memoryByte(address + 2U);
  const auto *byte3 = memoryByte(address + 3U);
  if (byte0 == nullptr || byte1 == nullptr || byte2 == nullptr ||
      byte3 == nullptr) {
    return false;
  }
  value = std::to_integer<std::uint8_t>(*byte0) |
          (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(*byte1))
           << 8U) |
          (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(*byte2))
           << 16U) |
          (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(*byte3))
           << 24U);
  return true;
}

bool R3000Runtime::write8(std::uint32_t address, std::uint8_t value) noexcept {
  std::uint32_t physical{};
  if (physicalAddress(address, physical) && physical < ram_mirror_end) {
    invalidateProjectedWord(address);
    ram_[physical & static_cast<std::uint32_t>(ram_size - 1U)] =
        static_cast<std::byte>(value);
    return true;
  }
  if (writeMmio(address, R3000AccessWidth::byte, value)) {
    return true;
  }
  auto *destination = memoryByte(address);
  if (destination == nullptr) {
    return false;
  }
  invalidateProjectedWord(address);
  *destination = static_cast<std::byte>(value);
  return true;
}

bool R3000Runtime::write16(std::uint32_t address,
                           std::uint16_t value) noexcept {
  return write16Projected(address, value, nullptr, nullptr);
}

bool R3000Runtime::write16Projected(std::uint32_t address, std::uint16_t value,
                                    const ProjectedHalf *projected_half,
                                    const GteExactWord *exact_word,
                                    const PgxpValue *pgxp) noexcept {
  if ((address & 1U) != 0U) {
    return false;
  }
  std::uint32_t physical{};
  if (physicalAddress(address, physical) && physical < ram_mirror_end) {
    const auto offset = physical & static_cast<std::uint32_t>(ram_size - 1U);
    ram_[offset] = static_cast<std::byte>(value);
    ram_[offset + 1U] = static_cast<std::byte>(value >> 8U);
    if (projected_half == nullptr && exact_word == nullptr && pgxp == nullptr) {
      invalidateProjectedWord(address);
    } else {
      std::uint32_t packed{};
      if (!read32(address & ~3U, packed))
        return false;
      storeProjectedHalf(address, value, packed, projected_half);
      storeExactHalf(address, value, packed, exact_word);
      storePgxpHalf(address, value, packed, pgxp);
    }
    return true;
  }
  if (writeMmio(address, R3000AccessWidth::halfword, value)) {
    return true;
  }
  auto *byte0 = memoryByte(address);
  auto *byte1 = memoryByte(address + 1U);
  if (byte0 == nullptr || byte1 == nullptr) {
    return false;
  }
  *byte0 = static_cast<std::byte>(value);
  *byte1 = static_cast<std::byte>(value >> 8U);
  if (projected_half == nullptr && exact_word == nullptr && pgxp == nullptr) {
    invalidateProjectedWord(address);
  } else {
    std::uint32_t packed{};
    if (!read32(address & ~3U, packed))
      return false;
    storeProjectedHalf(address, value, packed, projected_half);
    storeExactHalf(address, value, packed, exact_word);
    storePgxpHalf(address, value, packed, pgxp);
  }
  return true;
}

bool R3000Runtime::write32(std::uint32_t address,
                           std::uint32_t value) noexcept {
  return write32Projected(address, value, nullptr);
}

bool R3000Runtime::write32Projected(
    std::uint32_t address, std::uint32_t value,
    const GteProjectedVertex *projected) noexcept {
  if ((address & 3U) != 0U) {
    return false;
  }
  std::uint32_t physical{};
  if (physicalAddress(address, physical) && physical < ram_mirror_end) {
    invalidateProjectedWord(address);
    const auto offset = physical & static_cast<std::uint32_t>(ram_size - 1U);
    ram_[offset] = static_cast<std::byte>(value);
    ram_[offset + 1U] = static_cast<std::byte>(value >> 8U);
    ram_[offset + 2U] = static_cast<std::byte>(value >> 16U);
    ram_[offset + 3U] = static_cast<std::byte>(value >> 24U);
    return true;
  }
  const auto valid_projection = projected != nullptr && projected->valid &&
                                projected->packed_sxy == value;
  const auto projection_identity =
      valid_projection ? compactProjectionHandle(projected, value)
                       : std::uint16_t{};
  if (writeMmio(address, R3000AccessWidth::word, value,
                valid_projection ? projected : nullptr, projection_identity)) {
    return true;
  }
  auto *byte0 = memoryByte(address);
  auto *byte1 = memoryByte(address + 1U);
  auto *byte2 = memoryByte(address + 2U);
  auto *byte3 = memoryByte(address + 3U);
  if (byte0 == nullptr || byte1 == nullptr || byte2 == nullptr ||
      byte3 == nullptr) {
    return false;
  }
  invalidateProjectedWord(address);
  *byte0 = static_cast<std::byte>(value);
  *byte1 = static_cast<std::byte>(value >> 8U);
  *byte2 = static_cast<std::byte>(value >> 16U);
  *byte3 = static_cast<std::byte>(value >> 24U);
  return true;
}

bool R3000Runtime::trackedWordKey(std::uint32_t address,
                                  std::uint32_t &key) noexcept {
  std::uint32_t physical{};
  if (!physicalAddress(address, physical)) {
    return false;
  }
  if (physical < ram_mirror_end) {
    key = (physical & static_cast<std::uint32_t>(ram_size - 1U)) & ~3U;
    return true;
  }
  if (physical >= scratchpad_address &&
      physical < scratchpad_address + scratchpad_size) {
    key = static_cast<std::uint32_t>(ram_size) +
          ((physical - scratchpad_address) & ~3U);
    return true;
  }
  return false;
}

bool R3000Runtime::trackedWordMarked(std::uint32_t key) const noexcept {
  if (tracked_word_presence_ == nullptr)
    return false;
  const auto word_index = static_cast<std::size_t>(key >> 2U);
  if (word_index >= tracked_key_count)
    return false;
  const auto mask = std::uint64_t{1} << (word_index & 63U);
  return (tracked_word_presence_[word_index >> 6U] & mask) != 0U;
}

void R3000Runtime::setTrackedWordMarked(std::uint32_t key,
                                        bool marked) noexcept {
  if (tracked_word_presence_ == nullptr)
    return;
  const auto word_index = static_cast<std::size_t>(key >> 2U);
  if (word_index >= tracked_key_count)
    return;
  auto &bits = tracked_word_presence_[word_index >> 6U];
  const auto mask = std::uint64_t{1} << (word_index & 63U);
  if (marked) {
    bits |= mask;
  } else {
    bits &= ~mask;
  }
}

R3000Runtime::TrackedWord *
R3000Runtime::findTrackedWord(std::uint32_t key) noexcept {
  if (!pgxp_transform_tracking_ || !tracked_words_present_ ||
      tracked_words_ == nullptr || !trackedWordMarked(key))
    return nullptr;
  constexpr auto set_count = tracked_word_capacity / tracked_word_ways;
  auto hash = key >> 2U;
  hash ^= hash >> 16U;
  hash *= 0x7feb352dU;
  hash ^= hash >> 15U;
  const auto base = static_cast<std::size_t>(hash) & (set_count - 1U);
  for (std::size_t way{}; way < tracked_word_ways; ++way) {
    auto &slot = tracked_words_[base * tracked_word_ways + way];
    if (slot.generation == tracked_generation_ && slot.key == key)
      return &slot;
  }
  return nullptr;
}

const R3000Runtime::TrackedWord *
R3000Runtime::findTrackedWord(std::uint32_t key) const noexcept {
  return const_cast<R3000Runtime *>(this)->findTrackedWord(key);
}

R3000Runtime::TrackedWord *
R3000Runtime::acquireTrackedWord(std::uint32_t key,
                                 std::uint32_t raw) noexcept {
  if (!pgxp_transform_tracking_ || tracked_words_ == nullptr)
    return nullptr;
  if (auto *existing = findTrackedWord(key); existing != nullptr) {
    existing->raw = raw;
    existing->age = tracked_age_++;
    if (tracked_age_ == 0U)
      tracked_age_ = 1U;
    return existing;
  }
  constexpr auto set_count = tracked_word_capacity / tracked_word_ways;
  auto hash = key >> 2U;
  hash ^= hash >> 16U;
  hash *= 0x7feb352dU;
  hash ^= hash >> 15U;
  const auto base = static_cast<std::size_t>(hash) & (set_count - 1U);
  TrackedWord *destination = nullptr;
  for (std::size_t way{}; way < tracked_word_ways; ++way) {
    auto &candidate = tracked_words_[base * tracked_word_ways + way];
    if (candidate.generation != tracked_generation_) {
      destination = &candidate;
      break;
    }
    if (destination == nullptr || candidate.age < destination->age)
      destination = &candidate;
  }
  if (destination == nullptr)
    return nullptr;
  if (destination->generation == tracked_generation_) {
    setTrackedWordMarked(destination->key, false);
  }
  *destination = {};
  destination->key = key;
  destination->raw = raw;
  destination->generation = tracked_generation_;
  destination->age = tracked_age_++;
  setTrackedWordMarked(key, true);
  tracked_words_present_ = true;
  if (tracked_age_ == 0U)
    tracked_age_ = 1U;
  return destination;
}

void R3000Runtime::invalidateProjectedWord(std::uint32_t address) noexcept {
  std::uint32_t key{};
  if (!trackedWordKey(address, key))
    return;
  const auto word_index = static_cast<std::size_t>(key >> 2U);

  if (gpu_projection_handle_words_present_ &&
      gpu_projection_handle_words_ != nullptr) {
    auto &entry = gpu_projection_handle_words_[word_index];
    if (static_cast<std::uint16_t>(entry >> 16U) ==
        gpu_projection_handle_generation_) {
      entry = 0U;
    }
  }
  const auto page_index = word_index / pgxp_page_words;
  const auto page_offset = word_index % pgxp_page_words;
  if (page_index < direct_projection_pages_.size()) {
    const auto &page = direct_projection_pages_[page_index];
    if (page != nullptr) {
      // Raw payload is ignored unless the projection carries a valid witness.
      page[page_offset].projected.valid = false;
    }
  }
  if (!pgxp_transform_tracking_)
    return;

  if (page_index < pgxp_memory_pages_.size()) {
    const auto &page = pgxp_memory_pages_[page_index];
    if (page != nullptr) {
      page[page_offset].value.generation = 0U;
      page[page_offset].projected.valid = false;
    }
  }
  if (page_index < exact_memory_pages_.size()) {
    const auto &page = exact_memory_pages_[page_index];
    if (page != nullptr) {
      // Clearing three validity bytes is enough to invalidate a sparse exact
      // word; zeroing the complete sidecar made every ordinary SW expensive.
      auto &word = page[page_offset];
      word.scalar.valid = false;
      word.halves[0].valid = false;
      word.halves[1].valid = false;
    }
  }
  if (auto *slot = findTrackedWord(key); slot != nullptr) {
    slot->generation = 0U;
    if (tracked_word_presence_ != nullptr) {
      auto &bits = tracked_word_presence_[word_index >> 6U];
      bits &= ~(std::uint64_t{1} << (word_index & 63U));
    }
  }
}

void R3000Runtime::storeProjectedVertex(
    std::uint32_t address, const GteProjectedVertex &projected) noexcept {
  storeCompactProjectionHandle(
      address, compactProjectionHandle(&projected, projected.packed_sxy));
  if (!projected.valid)
    return;
  if (auto *persistent = directProjectionMemoryWord(address, true);
      persistent != nullptr) {
    persistent->raw = projected.packed_sxy;
    persistent->projected = projected;
  }
  if (!pgxp_transform_tracking_)
    return;
  PgxpValue value{};
  if (pgxp_cpu_tracking_) {
    value = pgxpFromProjected(projected);
    storePgxpWord(address, projected.packed_sxy, &value);
    // A direct GTE tuple already contains unclamped view X/Y, including the
    // Z==0 case which cannot be reconstructed from screen XY. Keep it as the
    // lazy cache payload instead of throwing it away and dividing later.
    if (auto *memory_value = pgxpMemoryWord(address, false);
        memory_value != nullptr)
      memory_value->projected = projected;
  }
  if (!pgxp_cpu_tracking_ && gpu_projection_catalog_tracking_)
    return;
  std::uint32_t key{};
  if (!trackedWordKey(address, key))
    return;
  auto *slot = acquireTrackedWord(key, projected.packed_sxy);
  if (slot == nullptr)
    return;
  slot->projected = projected;
  slot->projected_halves = {};
  slot->pgxp = value;
}

void R3000Runtime::storeProjectedHalf(
    std::uint32_t address, std::uint16_t value, std::uint32_t packed,
    const ProjectedHalf *projected_half) noexcept {
  // A partial packet write can never retain the previous complete handle.
  invalidateCompactProjectionHandle(address);
  if (!pgxp_transform_tracking_)
    return;
  std::uint32_t key{};
  if (!trackedWordKey(address, key))
    return;
  const auto slot = static_cast<std::uint8_t>((address >> 1U) & 1U);
  if (auto *persistent = directProjectionMemoryWord(address, false);
      persistent != nullptr) {
    persistent->projected.valid = false;
  }
  const auto valid_source =
      projected_half != nullptr && projected_half->valid &&
      projected_half->projected.valid && projected_half->slot == slot &&
      projected_half->register_slot == 0U && projected_half->value == value &&
      std::isfinite(projected_half->screen_position);
  auto *tracked =
      valid_source ? acquireTrackedWord(key, packed) : findTrackedWord(key);
  if (tracked == nullptr)
    return;
  tracked->raw = packed;
  tracked->projected = {};
  if (!valid_source) {
    tracked->projected_halves = {};
    return;
  }
  auto &same = tracked->projected_halves.halves[slot];
  auto &other = tracked->projected_halves.halves[slot ^ 1U];
  if ((same.valid && same.projected != projected_half->projected) ||
      (other.valid && other.projected != projected_half->projected)) {
    tracked->projected_halves = {};
  }
  same = *projected_half;
  if (other.valid) {
    const auto &low = tracked->projected_halves.halves[0U];
    const auto &high = tracked->projected_halves.halves[1U];
    auto combined = low.projected;
    const auto derived =
        low.derived || high.derived || packed != combined.packed_sxy;
    combined.packed_sxy = packed;
    if (derived) {
      combined.screen_x = low.screen_position;
      combined.screen_y = high.screen_position;
      combined.view_x = (combined.screen_x - combined.screen_offset_x) *
                        combined.view_z / combined.screen_h;
      combined.view_y = (combined.screen_y - combined.screen_offset_y) *
                        combined.view_z / combined.screen_h;
      combined.exact_transform = false;
      combined.fractional_transform = true;
      combined.source_vertex_id = 0U;
      combined.mesh_vertex_id = 0U;
      combined.transform_lineage = 0U;
      combined.valid =
          std::isfinite(combined.view_x) && std::isfinite(combined.view_y);
    }
    tracked->projected = combined;
    tracked->projected_halves = {};
    storeCompactProjectionHandle(
        address, compactProjectionHandle(&tracked->projected,
                                         tracked->projected.packed_sxy));
    if (tracked->projected.valid) {
      if (auto *persistent = directProjectionMemoryWord(address, true);
          persistent != nullptr) {
        persistent->raw = packed;
        persistent->projected = tracked->projected;
      }
    }
  }
}

void R3000Runtime::storeExactWord(std::uint32_t address, std::uint32_t value,
                                  const GteExactWord *exact_word) noexcept {
  if (!exactTransformCarrierTracking())
    return;
  const auto current = [this](const GteExactComponent &component) {
    return component.valid &&
           component.generation == exact_tracking_->gte.generation &&
           std::isfinite(component.value);
  };
  const auto valid_source =
      exact_word != nullptr && exact_word->raw == value &&
      (current(exact_word->scalar) || current(exact_word->halves[0]) ||
       current(exact_word->halves[1]));
  if (!valid_source)
    return;
  auto *slot = exactMemoryWord(address, true);
  if (slot != nullptr)
    *slot = *exact_word;
}

void R3000Runtime::storeExactHalf(std::uint32_t address, std::uint16_t value,
                                  std::uint32_t packed,
                                  const GteExactWord *exact_word) noexcept {
  if (!exactTransformCarrierTracking())
    return;
  const auto destination = static_cast<std::uint8_t>((address >> 1U) & 1U);
  const auto component = exact_word != nullptr && exact_word->scalar.valid
                             ? exact_word->scalar
                         : exact_word != nullptr ? exact_word->halves[0]
                                                 : GteExactComponent{};
  const auto valid_source =
      exact_word != nullptr &&
      static_cast<std::uint16_t>(exact_word->raw) == value && component.valid &&
      component.generation == exact_tracking_->gte.generation &&
      std::isfinite(component.value);
  auto *tracked = exactMemoryWord(address, valid_source);
  if (tracked == nullptr)
    return;
  tracked->raw = packed;
  tracked->scalar = {};
  tracked->halves[destination] = {};
  if (!valid_source)
    return;
  tracked->halves[destination] = component;
}

ProjectedVertexProvenance R3000Runtime::projectedVertexProvenanceAt(
    std::uint32_t address, std::uint32_t raw_witness) const noexcept {
  ProjectedVertexProvenance provenance{};
  std::uint32_t key{};
  const auto tracked_address = trackedWordKey(address, key);
  const auto publishCurrentCatalog =
      [this, key, tracked_address, raw_witness,
       &provenance](GteProjectedVertex &projection) noexcept {
        if (!gpu_projection_catalog_tracking_ || !projection.valid ||
            projection.packed_sxy != raw_witness) {
          return false;
        }
        auto *runtime = const_cast<R3000Runtime *>(this);
        const auto fresh = runtime->ensureGpuProjectionHandle(projection);
        if (fresh == 0U)
          return false;
        if (tracked_address && gpu_projection_handle_words_ != nullptr) {
          gpu_projection_handle_words_[key >> 2U] =
              (static_cast<std::uint32_t>(gpu_projection_handle_generation_)
               << 16U) |
              fresh;
          runtime->gpu_projection_handle_words_present_ = true;
        }
        provenance.projected = &gpu_projection_catalog_[fresh - 1U];
        provenance.identity = fresh;
        return true;
      };
  auto handle = std::uint16_t{};
  if (tracked_address) {
    if (auto *persistent = directProjectionMemoryWord(address);
        persistent != nullptr && persistent->projected.valid &&
        persistent->raw == raw_witness &&
        persistent->projected.packed_sxy == raw_witness &&
        publishCurrentCatalog(
            const_cast<GteProjectedVertex &>(persistent->projected))) {
      return provenance;
    }
  }

  if (tracked_address && gpu_projection_handle_words_present_ &&
      gpu_projection_handle_words_ != nullptr) {
    const auto entry = gpu_projection_handle_words_[key >> 2U];
    if (static_cast<std::uint16_t>(entry >> 16U) ==
        gpu_projection_handle_generation_) {
      handle = static_cast<std::uint16_t>(entry);
    }
  }
  if (handle != 0U && handle <= gpu_projection_catalog_size_) {
    const auto &projection = gpu_projection_catalog_[handle - 1U];
    if (projection.valid && projection.packed_sxy == raw_witness &&
        projection.source_vertex_id == handle) {
      provenance.projected = &projection;
      provenance.identity = handle;
      return provenance;
    }
  }
  if (!pgxp_transform_tracking_)
    return provenance;
  if (pgxp_cpu_tracking_) {
    const PgxpMemoryValue *memory_value{};
    if (tracked_address) {
      const auto word_index = static_cast<std::size_t>(key >> 2U);
      const auto page_index = word_index / pgxp_page_words;
      if (page_index < pgxp_memory_pages_.size()) {
        const auto &page = pgxp_memory_pages_[page_index];
        if (page != nullptr)
          memory_value = &page[word_index % pgxp_page_words];
      }
    }
    if (memory_value != nullptr &&
        memory_value->value.generation == tracked_generation_ &&
        memory_value->value.raw == raw_witness) {
      const auto &value = memory_value->value;
      auto *cached = const_cast<PgxpMemoryValue *>(memory_value);
      if (!cached->projected.valid ||
          cached->projected.packed_sxy != raw_witness) {
        cached->projected = pgxpToProjected(value);
      }
      if (cached->projected.valid &&
          cached->projected.packed_sxy == raw_witness) {
        if (publishCurrentCatalog(cached->projected))
          return provenance;
        provenance.projected = &cached->projected;
        provenance.identity = !gpu_projection_catalog_tracking_ &&
                                      !value.derived_x && !value.derived_y
                                  ? value.source_vertex_id
                                  : 0U;
        return provenance;
      }
    }
  }
  if (!tracked_address)
    return provenance;
  const auto *slot = findTrackedWord(key);
  if (slot == nullptr || slot->raw != raw_witness)
    return provenance;
  if (slot->projected.valid && slot->projected.packed_sxy == raw_witness) {
    auto &projection = const_cast<GteProjectedVertex &>(slot->projected);
    if (publishCurrentCatalog(projection))
      return provenance;
    provenance.projected = &slot->projected;
  }
  if (!gpu_projection_catalog_tracking_ && provenance.projected != nullptr &&
      slot->projected.source_vertex_id != 0U) {
    provenance.identity = slot->projected.source_vertex_id;
    return provenance;
  }
  // A pair assembled through LH/LHU+SH can retain source identity before a
  // complete depth/projection context is available.  Publish only when both
  // raw X/Y witnesses are original and compatible.
  const auto &value = slot->pgxp;
  if (!gpu_projection_catalog_tracking_ && value.raw == raw_witness &&
      value.has(PgxpValue::valid_x | PgxpValue::valid_y) && !value.derived_x &&
      !value.derived_y && value.source_vertex_id != 0U) {
    provenance.identity = value.source_vertex_id;
  }
  return provenance;
}

ProjectedVertexProvenance R3000Runtime::projectedVertexProvenanceAt(
    std::uint32_t address) const noexcept {
  if (!pgxp_transform_tracking_ && !gpu_projection_catalog_tracking_)
    return {};
  std::uint32_t raw{};
  return read32(address & ~3U, raw) ? projectedVertexProvenanceAt(address, raw)
                                    : ProjectedVertexProvenance{};
}

const GteProjectedVertex *
R3000Runtime::projectedVertexAt(std::uint32_t address) const noexcept {
  return projectedVertexProvenanceAt(address).projected;
}

std::uint64_t
R3000Runtime::projectedVertexIdentityAt(std::uint32_t address) const noexcept {
  return projectedVertexProvenanceAt(address).identity;
}

const GteExactWord *
R3000Runtime::exactWordAt(std::uint32_t address,
                          std::uint32_t value) const noexcept {
  if (!exactTransformCarrierTracking())
    return nullptr;
  const auto *slot = exactMemoryWord(address);
  if (slot == nullptr || slot->raw != value)
    return nullptr;
  const auto current = [this](const GteExactComponent &component) {
    return component.valid &&
           component.generation == exact_tracking_->gte.generation &&
           std::isfinite(component.value);
  };
  return current(slot->scalar) || current(slot->halves[0]) ||
                 current(slot->halves[1])
             ? slot
             : nullptr;
}
const R3000Runtime::PgxpValue *
R3000Runtime::pgxpRegister(std::uint8_t reg,
                           std::uint32_t value) const noexcept {
  if (!pgxp_transform_tracking_ || !pgxp_cpu_tracking_ ||
      reg >= exact_tracking_->pgxp_gpr.size()) {
    return nullptr;
  }
  const auto bit = std::uint64_t{1} << reg;
  if ((exact_tracking_->pgxp_gpr_valid_mask & bit) == 0U)
    return nullptr;
  const auto &tracked = exact_tracking_->pgxp_gpr[reg];
  return tracked.generation == tracked_generation_ && tracked.raw == value &&
                 (tracked.flags & (PgxpValue::valid_x | PgxpValue::valid_y)) !=
                     0U
             ? &tracked
             : nullptr;
}

R3000Runtime::PgxpMemoryValue *
R3000Runtime::pgxpMemoryWord(std::uint32_t address, bool create) noexcept {
  if (!pgxp_transform_tracking_ || pgxp_memory_pages_.empty() ||
      (create && pgxp_memory_overflow_)) {
    return nullptr;
  }
  std::uint32_t key{};
  if (!trackedWordKey(address, key))
    return nullptr;
  const auto index = static_cast<std::size_t>(key >> 2U);
  const auto page_index = index / pgxp_page_words;
  if (page_index >= pgxp_memory_pages_.size())
    return nullptr;
  auto &page = pgxp_memory_pages_[page_index];
  if (page == nullptr && create) {
    page.reset(new (std::nothrow) PgxpMemoryValue[pgxp_page_words]);
    if (page == nullptr) {
      pgxp_memory_overflow_ = true;
      return nullptr;
    }
  }
  return page != nullptr ? &page[index % pgxp_page_words] : nullptr;
}

const R3000Runtime::PgxpMemoryValue *
R3000Runtime::pgxpMemoryWord(std::uint32_t address) const noexcept {
  return const_cast<R3000Runtime *>(this)->pgxpMemoryWord(address, false);
}

GteExactWord *R3000Runtime::exactMemoryWord(std::uint32_t address,
                                            bool create) noexcept {
  if (!exactTransformCarrierTracking() || exact_memory_pages_.empty() ||
      (create && exact_memory_overflow_)) {
    return nullptr;
  }
  std::uint32_t key{};
  if (!trackedWordKey(address, key))
    return nullptr;
  const auto index = static_cast<std::size_t>(key >> 2U);
  const auto page_index = index / pgxp_page_words;
  if (page_index >= exact_memory_pages_.size())
    return nullptr;
  auto &page = exact_memory_pages_[page_index];
  if (page == nullptr && create) {
    page.reset(new (std::nothrow) GteExactWord[pgxp_page_words]);
    if (page == nullptr) {
      exact_memory_overflow_ = true;
      return nullptr;
    }
  }
  return page != nullptr ? &page[index % pgxp_page_words] : nullptr;
}

const GteExactWord *
R3000Runtime::exactMemoryWord(std::uint32_t address) const noexcept {
  return const_cast<R3000Runtime *>(this)->exactMemoryWord(address, false);
}

R3000Runtime::DirectProjectionMemoryValue *
R3000Runtime::directProjectionMemoryWord(std::uint32_t address,
                                         bool create) noexcept {
  if (!gpu_projection_catalog_tracking_ || direct_projection_pages_.empty() ||
      (create && direct_projection_memory_overflow_)) {
    return nullptr;
  }
  std::uint32_t key{};
  if (!trackedWordKey(address, key))
    return nullptr;
  const auto index = static_cast<std::size_t>(key >> 2U);
  const auto page_index = index / pgxp_page_words;
  if (page_index >= direct_projection_pages_.size())
    return nullptr;
  auto &page = direct_projection_pages_[page_index];
  if (page == nullptr && create) {
    page.reset(new (std::nothrow) DirectProjectionMemoryValue[pgxp_page_words]);
    if (page == nullptr) {
      direct_projection_memory_overflow_ = true;
      return nullptr;
    }
  }
  return page != nullptr ? &page[index % pgxp_page_words] : nullptr;
}

const R3000Runtime::DirectProjectionMemoryValue *
R3000Runtime::directProjectionMemoryWord(std::uint32_t address) const noexcept {
  return const_cast<R3000Runtime *>(this)->directProjectionMemoryWord(address,
                                                                      false);
}

void R3000Runtime::clearDirectProjectionMemory() noexcept {
  for (auto &page : direct_projection_pages_) {
    page.reset();
  }
  direct_projection_memory_overflow_ = false;
}

const R3000Runtime::PgxpValue *
R3000Runtime::pgxpWordAt(std::uint32_t address,
                         std::uint32_t value) const noexcept {
  if (!pgxp_transform_tracking_ || !pgxp_cpu_tracking_)
    return nullptr;
  const auto *memory_value = pgxpMemoryWord(address);
  if (memory_value == nullptr)
    return nullptr;
  const auto &word = memory_value->value;
  return word.generation == tracked_generation_ && word.raw == value &&
                 (word.flags & (PgxpValue::valid_x | PgxpValue::valid_y)) != 0U
             ? &word
             : nullptr;
}

std::uint32_t R3000Runtime::publishPgxpContext(
    const PgxpProjectionContext &context) noexcept {
  if (pgxp_contexts_ == nullptr || !context.has(PgxpProjectionContext::valid)) {
    return 0U;
  }
  const auto handle = next_pgxp_context_handle_;
  if (handle == 0U) {
    return 0U;
  }
  ++next_pgxp_context_handle_;
  auto &slot = pgxp_contexts_[(handle - 1U) & (pgxp_context_capacity - 1U)];
  slot.context = context;
  slot.handle = handle;
  return handle;
}

const R3000Runtime::PgxpProjectionContext *
R3000Runtime::pgxpContext(const PgxpValue &value) const noexcept {
  if (pgxp_contexts_ == nullptr || value.context_handle == 0U ||
      value.generation != tracked_generation_) {
    return nullptr;
  }
  const auto &slot = pgxp_contexts_[(value.context_handle - 1U) &
                                    (pgxp_context_capacity - 1U)];
  return slot.handle == value.context_handle ? &slot.context : nullptr;
}

bool R3000Runtime::sameProjectionContext(
    const PgxpValue &first, const PgxpValue &second) const noexcept {
  if (!first.has(PgxpValue::valid_z) || !second.has(PgxpValue::valid_z)) {
    return false;
  }
  const auto *first_context = pgxpContext(first);
  const auto *second_context = pgxpContext(second);
  return first_context != nullptr && second_context != nullptr &&
         *first_context == *second_context && first.z == second.z;
}

R3000Runtime::PgxpValue
R3000Runtime::pgxpFromProjected(const GteProjectedVertex &projected) noexcept {
  PgxpValue value{};
  if (!projected.valid || !std::isfinite(projected.screen_x) ||
      !std::isfinite(projected.screen_y) || !std::isfinite(projected.view_x) ||
      !std::isfinite(projected.view_y) || !std::isfinite(projected.view_z)) {
    return value;
  }
  PgxpProjectionContext context{};
  context.screen_h = projected.screen_h;
  context.screen_offset_x = projected.screen_offset_x;
  context.screen_offset_y = projected.screen_offset_y;
  context.view_x = projected.view_x;
  context.view_y = projected.view_y;
  context.projective_depth = projected.projective_depth;
  context.enhanced_sources = projected.enhanced_sources;
  context.mesh_vertex_id = projected.mesh_vertex_id;
  // Projection revision is the coherence epoch for both camera and projection
  // changes. publishCameraTuple() deliberately gives camera_revision and
  // projection_revision the same value, so XOR would collapse every freshly
  // published camera tuple to zero.
  context.projection_epoch = projected.projection_epoch;
  context.flags =
      PgxpProjectionContext::valid |
      (projected.ir_saturated ? PgxpProjectionContext::ir_saturated : 0U) |
      (projected.depth_saturated ? PgxpProjectionContext::depth_saturated
                                 : 0U) |
      (projected.divide_overflow ? PgxpProjectionContext::divide_overflow
                                 : 0U) |
      (projected.screen_saturated ? PgxpProjectionContext::screen_saturated
                                  : 0U) |
      (projected.hasExactTransformProvenance()
           ? PgxpProjectionContext::exact_transform
           : 0U) |
      (projected.fractional_transform
           ? PgxpProjectionContext::fractional_transform
           : 0U);
  value.x = projected.screen_x;
  value.y = projected.screen_y;
  value.z = projected.view_z;
  value.raw = projected.packed_sxy;
  value.source_vertex_id = projected.source_vertex_id;
  value.generation = tracked_generation_;
  value.context_handle = publishPgxpContext(context);
  if (value.context_handle == 0U) {
    return {};
  }
  const auto lineage =
      pgxpLineageMix(context.projection_epoch, projected.packed_sxy);
  value.lineage_x = lineage;
  value.lineage_y = lineage;
  value.flags = PgxpValue::valid_x | PgxpValue::valid_y | PgxpValue::valid_z |
                PgxpValue::z_from_low | PgxpValue::z_from_high;
  return value;
}

GteProjectedVertex
R3000Runtime::pgxpToProjected(const PgxpValue &value) const noexcept {
  const auto *context = pgxpContext(value);
  if (!value.has(PgxpValue::valid_x | PgxpValue::valid_y |
                 PgxpValue::valid_z) ||
      !std::isfinite(value.x) || !std::isfinite(value.y) ||
      !std::isfinite(value.z) || context == nullptr ||
      !context->has(PgxpProjectionContext::valid) ||
      !std::isfinite(context->screen_h) || context->screen_h <= 0.0F) {
    return {};
  }
  GteProjectedVertex projected{};
  projected.packed_sxy = value.raw;
  projected.screen_x = static_cast<float>(value.x);
  projected.screen_y = static_cast<float>(value.y);
  projected.view_z = value.z;
  projected.projective_depth = context->projective_depth;
  projected.screen_h = context->screen_h;
  projected.screen_offset_x = context->screen_offset_x;
  projected.screen_offset_y = context->screen_offset_y;
  projected.ir_saturated = context->has(PgxpProjectionContext::ir_saturated);
  projected.depth_saturated =
      context->has(PgxpProjectionContext::depth_saturated);
  projected.divide_overflow =
      context->has(PgxpProjectionContext::divide_overflow);
  projected.screen_saturated =
      context->has(PgxpProjectionContext::screen_saturated);
  projected.exact_transform =
      !value.derived_x && !value.derived_y && context->projection_epoch != 0U &&
      context->has(PgxpProjectionContext::exact_transform);
  const auto retained_exact_view = projected.exact_transform &&
                                   std::isfinite(context->view_x) &&
                                   std::isfinite(context->view_y);
  if (retained_exact_view) {
    // An unmodified exact carrier already owns a coherent view tuple. Reusing
    // it avoids two divides and is required at W/Z == 0, where screen XY is
    // not invertible.
    projected.view_x = context->view_x;
    projected.view_y = context->view_y;
  } else {
    projected.view_x = (projected.screen_x - projected.screen_offset_x) *
                       projected.view_z / projected.screen_h;
    projected.view_y = (projected.screen_y - projected.screen_offset_y) *
                       projected.view_z / projected.screen_h;
  }
  projected.fractional_transform =
      value.derived_x || value.derived_y ||
      context->has(PgxpProjectionContext::fractional_transform);
  projected.enhanced_sources = context->enhanced_sources;
  projected.source_vertex_id =
      !value.derived_x && !value.derived_y ? value.source_vertex_id : 0U;
  projected.mesh_vertex_id =
      !value.derived_x && !value.derived_y ? context->mesh_vertex_id : 0U;
  // The compact CPU context retains one coherence epoch rather than both
  // publication revisions. For an unmodified exact carrier this nonzero epoch
  // is a provenance witness; it is not used as geometric vertex identity.
  projected.transform_lineage =
      projected.exact_transform ? context->projection_epoch : 0U;
  projected.projection_epoch = context->projection_epoch;
  projected.valid = std::isfinite(projected.view_x) &&
                    std::isfinite(projected.view_y) &&
                    (projected.exact_transform ? retained_exact_view
                                               : projected.view_z > 0.0F);
  return projected;
}

void R3000Runtime::storePgxpWord(std::uint32_t address, std::uint32_t raw,
                                 const PgxpValue *value) noexcept {
  if (!pgxp_transform_tracking_ || !pgxp_cpu_tracking_)
    return;
  const auto valid =
      value != nullptr && value->raw == raw &&
      (value->flags & (PgxpValue::valid_x | PgxpValue::valid_y)) != 0U;
  // Every word-store path invalidates the destination before publishing its
  // companion. An untracked source therefore needs no second shadow lookup.
  if (!valid)
    return;
  auto *memory_value = pgxpMemoryWord(address, true);
  if (memory_value == nullptr)
    return;

  auto &word = memory_value->value;
  word = *value;
  word.raw = raw;
  word.generation = tracked_generation_;
  memory_value->projected.valid = false;
  if (!gpu_projection_catalog_tracking_)
    return;

  auto projected = pgxpToProjected(word);
  if (!projected.valid) {
    return;
  }
  memory_value->projected = projected;
  if (gpu_projection_catalog_tracking_) {
    const auto handle = ensureGpuProjectionHandle(projected);
    storeCompactProjectionHandle(address, handle);
  }
}

void R3000Runtime::storePgxpHalf(std::uint32_t address, std::uint16_t raw,
                                 std::uint32_t packed,
                                 const PgxpValue *value) noexcept {
  if (!pgxp_transform_tracking_ || !pgxp_cpu_tracking_)
    return;
  const auto half = static_cast<std::uint8_t>((address >> 1U) & 1U);
  const auto valid = value != nullptr && value->has(PgxpValue::valid_x) &&
                     static_cast<std::uint16_t>(value->raw) == raw;
  auto *memory_value = pgxpMemoryWord(address, valid);
  if (memory_value == nullptr)
    return;
  auto &word = memory_value->value;
  if (word.generation != tracked_generation_) {
    *memory_value = {};
    word.generation = tracked_generation_;
  } else {
    const auto other = static_cast<std::uint8_t>(half ^ 1U);
    const auto previous_other =
        static_cast<std::uint16_t>(word.raw >> (other * 16U));
    const auto current_other =
        static_cast<std::uint16_t>(packed >> (other * 16U));
    if (previous_other != current_other) {
      if (other == 0U) {
        word.flags &= ~(PgxpValue::valid_x | PgxpValue::z_from_low);
        word.derived_x = false;
        word.lineage_x = 0U;
      } else {
        word.flags &= ~(PgxpValue::valid_y | PgxpValue::z_from_high);
        word.derived_y = false;
        word.lineage_y = 0U;
      }
      word.source_vertex_id = 0U;
    }
  }
  word.raw = packed;
  if (!valid) {
    if (half == 0U) {
      word.flags &= ~(PgxpValue::valid_x | PgxpValue::z_from_low);
      word.derived_x = false;
      word.lineage_x = 0U;
    } else {
      word.flags &= ~(PgxpValue::valid_y | PgxpValue::z_from_high);
      word.derived_y = false;
      word.lineage_y = 0U;
    }
    word.source_vertex_id = 0U;
    if ((word.flags & (PgxpValue::z_from_low | PgxpValue::z_from_high)) == 0U) {
      word.flags &= ~PgxpValue::valid_z;
    }
    word.context_handle = 0U;
    memory_value->projected.valid = false;
    return;
  }
  auto &destination = word;
  destination.raw = packed;
  destination.generation = tracked_generation_;
  const auto incoming_lineage = value->lineage_x;
  auto other_valid =
      destination.has(half == 0U ? PgxpValue::valid_y : PgxpValue::valid_x);
  const auto other_lineage =
      half == 0U ? destination.lineage_y : destination.lineage_x;
  const auto incoming_derived = value->derived_x;
  auto other_derived =
      half == 0U ? destination.derived_y : destination.derived_x;
  const auto incoming_identity =
      !incoming_derived ? value->source_vertex_id : 0U;
  auto other_identity = destination.source_vertex_id;
  const auto identities_present =
      incoming_identity != 0U || other_identity != 0U;
  const auto incompatible_identity =
      identities_present
          ? incoming_identity == 0U || other_identity != incoming_identity
          : incoming_lineage == 0U || other_lineage != incoming_lineage;
  if (other_valid && !incoming_derived && !other_derived &&
      incompatible_identity) {
    // A reused packet address commonly still carries the previous frame's
    // complete word.  Drop that stale peer but retain the incoming half.
    destination = {};
    memory_value->projected.valid = false;
    destination.raw = packed;
    destination.generation = tracked_generation_;
    other_valid = false;
    other_derived = false;
    other_identity = 0U;
  }
  destination.source_vertex_id =
      !incoming_derived && incoming_identity != 0U &&
              (!other_valid ||
               (!other_derived && other_identity == incoming_identity))
          ? incoming_identity
          : 0U;
  if (half == 0U) {
    destination.x = value->x;
    destination.lineage_x = incoming_lineage;
    destination.derived_x = incoming_derived;
    destination.flags = static_cast<std::uint8_t>(
        (destination.flags & ~(PgxpValue::valid_x | PgxpValue::z_from_low)) |
        PgxpValue::valid_x);
  } else {
    destination.y = value->x;
    destination.lineage_y = incoming_lineage;
    destination.derived_y = incoming_derived;
    destination.flags = static_cast<std::uint8_t>(
        (destination.flags & ~(PgxpValue::valid_y | PgxpValue::z_from_high)) |
        PgxpValue::valid_y);
  }
  if (value->has(PgxpValue::tainted_z)) {
    destination.flags |= PgxpValue::tainted_z;
  }
  if (value->has(PgxpValue::valid_z)) {
    if (!destination.has(PgxpValue::valid_z) ||
        sameProjectionContext(destination, *value)) {
      destination.z = value->z;
      destination.context_handle = value->context_handle;
      destination.flags |=
          PgxpValue::valid_z |
          (half == 0U ? PgxpValue::z_from_low : PgxpValue::z_from_high);
    } else {
      destination.flags &= ~(PgxpValue::valid_z | PgxpValue::z_from_low |
                             PgxpValue::z_from_high);
    }
  }
  memory_value->projected.valid = false;
  if (!gpu_projection_catalog_tracking_)
    return;
  if (destination.has(PgxpValue::valid_x | PgxpValue::valid_y |
                      PgxpValue::valid_z)) {
    auto projected = pgxpToProjected(destination);
    if (projected.valid) {
      const auto handle = ensureGpuProjectionHandle(projected);
      storeCompactProjectionHandle(address, handle);
      memory_value->projected = projected;
    }
  }
}
const GteProjectedVertex *
R3000Runtime::projectedRegister(std::uint8_t reg,
                                std::uint32_t value) noexcept {
  if (reg >= projected_gpr_.size()) {
    return nullptr;
  }
  const auto bit = 1U << reg;
  if ((gpu_projection_handle_gpr_mask_ & bit) != 0U) {
    const auto handle = gpu_projection_handle_gpr_[reg];
    if (handle != 0U && handle <= gpu_projection_catalog_size_) {
      const auto &projection = gpu_projection_catalog_[handle - 1U];
      if (projection.valid && projection.packed_sxy == value &&
          projection.source_vertex_id == handle) {
        return &projection;
      }
    }
  }
  if ((projected_gpr_valid_mask_ & bit) != 0U) {
    const auto &projected = projected_gpr_[reg];
    if (projected.valid && projected.packed_sxy == value)
      return &projected;
  }
  if (pgxp_cpu_tracking_) {
    if (const auto *pgxp = pgxpRegister(reg, value); pgxp != nullptr) {
      const auto synthesized = pgxpToProjected(*pgxp);
      if (synthesized.valid) {
        projected_gpr_[reg] = synthesized;
        projected_gpr_valid_mask_ |= bit;
        return &projected_gpr_[reg];
      }
    }
  }
  return nullptr;
}

R3000Runtime::ProjectedHalf
R3000Runtime::projectedHalfAt(std::uint32_t address, std::uint16_t value,
                              std::uint32_t register_value,
                              std::uint32_t packed) const noexcept {
  ProjectedHalf half{};
  const auto *projected = projectedVertexProvenanceAt(address, packed).projected;
  if (projected == nullptr) {
    return half;
  }
  const auto slot = static_cast<std::uint8_t>((address >> 1U) & 1U);
  if (static_cast<std::uint16_t>(projected->packed_sxy >> (slot * 16U)) !=
      value) {
    return half;
  }
  half.projected = *projected;
  half.screen_position = slot == 0U ? projected->screen_x : projected->screen_y;
  half.register_value = register_value;
  half.value = value;
  half.slot = slot;
  half.register_slot = 0U;
  half.valid = true;
  return half;
}

const R3000Runtime::ProjectedHalf *
R3000Runtime::projectedHalfRegister(std::uint8_t reg,
                                    std::uint32_t value) const noexcept {
  if (reg >= projected_half_gpr_.size() ||
      (projected_half_gpr_valid_mask_ & (1U << reg)) == 0U) {
    return nullptr;
  }
  const auto &projected_half = projected_half_gpr_[reg];
  return projected_half.valid && projected_half.register_value == value
             ? &projected_half
             : nullptr;
}

const GteExactWord *
R3000Runtime::exactRegister(std::uint8_t reg,
                            std::uint32_t value) const noexcept {
  if (!exactTransformCarrierTracking() || reg >= exact_tracking_->gpr.size() ||
      (exact_tracking_->gpr_valid_mask & (1U << reg)) == 0U)
    return nullptr;
  const auto &word = exact_tracking_->gpr[reg];
  if (word.raw != value)
    return nullptr;
  const auto current = [this](const GteExactComponent &component) {
    return component.valid &&
           component.generation == exact_tracking_->gte.generation &&
           component.lineage != 0U && std::isfinite(component.value);
  };
  return current(word.scalar) || current(word.halves[0]) ||
                 current(word.halves[1])
             ? &word
             : nullptr;
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
bool R3000Runtime::buildExactCarrier(
    ExactCarrierOperation operation, std::uint8_t first_reg,
    std::uint32_t first_raw, std::uint8_t second_reg,
    std::uint32_t second_raw, std::uint32_t result_raw, std::uint8_t shift,
    GteExactWord &result) const noexcept {
  result = {};
  if (!exactTransformCarrierTracking())
    return false;

  const auto current = [this](const GteExactComponent &component) noexcept {
    return component.valid && component.lineage != 0U &&
           component.generation == exact_tracking_->gte.generation &&
           std::isfinite(component.value);
  };
  const auto scalarWitness = [&](const GteExactComponent &component,
                                 std::int32_t raw) noexcept {
    if (!current(component))
      return false;
    const auto witness = static_cast<double>(raw);
    return component.value >= witness && component.value < witness + 1.0;
  };
  const auto halfWitness = [&](const GteExactComponent &component,
                               std::uint16_t raw) noexcept {
    return scalarWitness(component, static_cast<std::int16_t>(raw));
  };
  const auto *first =
      first_reg < 32U ? exactRegister(first_reg, first_raw) : nullptr;
  const auto *second =
      second_reg < 32U ? exactRegister(second_reg, second_raw) : nullptr;

  if (operation == ExactCarrierOperation::identity) {
    if (first == nullptr || first_raw != result_raw)
      return false;
    result = *first;
    return true;
  }

  const auto one_exact = (first != nullptr) != (second != nullptr);
  const auto publishScalar = [&](const GteExactComponent &source, double value,
                                 std::uint64_t domain) noexcept {
    const auto result_integer = static_cast<double>(asSigned(result_raw));
    if (!std::isfinite(value) || value < result_integer ||
        value >= result_integer + 1.0) {
      return false;
    }
    domain = pgxpLineageMix(pgxpLineageMix(domain, first_raw), second_raw);
    const auto lineage = pgxpLineageMix(source.lineage, domain);
    if (lineage == 0U)
      return false;
    result.raw = result_raw;
    result.scalar = {value, lineage, source.generation, true,
                     source.enhanced || value != result_integer};
    const auto low_half =
        static_cast<double>(static_cast<std::int16_t>(result_raw));
    if (value >= low_half && value < low_half + 1.0)
      result.halves[0] = result.scalar;
    return true;
  };
  const auto copyComponent = [&](const GteExactComponent &source,
                                 std::uint64_t domain,
                                 GteExactComponent &destination) noexcept {
    if (!current(source))
      return false;
    const auto lineage = pgxpLineageMix(source.lineage, domain);
    if (lineage == 0U)
      return false;
    destination = source;
    destination.lineage = lineage;
    return true;
  };

  switch (operation) {
  case ExactCarrierOperation::add:
  case ExactCarrierOperation::subtract: {
    if (!one_exact)
      return false;
    const auto left_integer = static_cast<std::int64_t>(asSigned(first_raw));
    const auto right_integer = static_cast<std::int64_t>(asSigned(second_raw));
    const auto integer = operation == ExactCarrierOperation::subtract
                             ? left_integer - right_integer
                             : left_integer + right_integer;
    if (integer < std::numeric_limits<std::int32_t>::min() ||
        integer > std::numeric_limits<std::int32_t>::max() ||
        static_cast<std::uint32_t>(static_cast<std::int32_t>(integer)) !=
            result_raw) {
      return false;
    }
    const auto &source = first != nullptr ? first->scalar : second->scalar;
    const auto source_raw =
        first != nullptr ? asSigned(first_raw) : asSigned(second_raw);
    if (!scalarWitness(source, source_raw))
      return false;
    const auto exact_left =
        first != nullptr ? source.value : static_cast<double>(left_integer);
    const auto exact_right =
        second != nullptr ? source.value : static_cast<double>(right_integer);
    const auto value = operation == ExactCarrierOperation::subtract
                           ? exact_left - exact_right
                           : exact_left + exact_right;
    return publishScalar(source, value,
                         operation == ExactCarrierOperation::subtract
                             ? 0x4558535542ULL
                             : 0x4558414444ULL);
  }
  case ExactCarrierOperation::shift_left_16: {
    if (first == nullptr || shift != 16U || result_raw != (first_raw << 16U))
      return false;
    const auto &source =
        current(first->halves[0]) ? first->halves[0] : first->scalar;
    if (!halfWitness(source, static_cast<std::uint16_t>(first_raw)))
      return false;
    result.raw = result_raw;
    return copyComponent(source, 0x4558534c4c10ULL, result.halves[1]);
  }
  case ExactCarrierOperation::shift_right_logical_16:
  case ExactCarrierOperation::shift_right_arithmetic_16: {
    if (first == nullptr || shift != 16U ||
        !halfWitness(first->halves[1],
                     static_cast<std::uint16_t>(first_raw >> 16U))) {
      return false;
    }
    const auto expected =
        operation == ExactCarrierOperation::shift_right_logical_16
            ? first_raw >> 16U
            : arithmeticShiftRight(first_raw, 16U);
    if (expected != result_raw)
      return false;
    result.raw = result_raw;
    const auto domain =
        operation == ExactCarrierOperation::shift_right_logical_16
            ? 0x455853524c10ULL
            : 0x455853524110ULL;
    if (!copyComponent(first->halves[1], domain, result.halves[0]))
      return false;
    if (operation == ExactCarrierOperation::shift_right_arithmetic_16)
      result.scalar = result.halves[0];
    return true;
  }
  case ExactCarrierOperation::and_low_16: {
    if (first == nullptr || second_raw != 0xffffU ||
        result_raw != (first_raw & 0xffffU)) {
      return false;
    }
    const auto &source =
        current(first->halves[0]) ? first->halves[0] : first->scalar;
    if (!halfWitness(source, static_cast<std::uint16_t>(first_raw)))
      return false;
    result.raw = result_raw;
    if (!copyComponent(source, 0x4558414e443136ULL, result.halves[0]))
      return false;
    const auto result_integer = static_cast<double>(asSigned(result_raw));
    if (source.value >= result_integer &&
        source.value < result_integer + 1.0)
      result.scalar = result.halves[0];
    return true;
  }
  case ExactCarrierOperation::or_nonoverlap: {
    if (!one_exact || (first_raw & second_raw) != 0U ||
        (first_raw | second_raw) != result_raw) {
      return false;
    }
    const auto &source = first != nullptr ? *first : *second;
    const auto source_raw = first != nullptr ? first_raw : second_raw;
    const auto other_raw = first != nullptr ? second_raw : first_raw;
    result.raw = result_raw;
    bool valid{};
    for (std::uint8_t slot{}; slot < 2U; ++slot) {
      const auto shift_bits = static_cast<std::uint8_t>(slot * 16U);
      const auto source_half =
          static_cast<std::uint16_t>(source_raw >> shift_bits);
      const auto other_half =
          static_cast<std::uint16_t>(other_raw >> shift_bits);
      if (other_half == 0U && halfWitness(source.halves[slot], source_half)) {
        valid |=
            copyComponent(source.halves[slot], 0x45584f5248414c46ULL + slot,
                          result.halves[slot]);
      }
    }
    return valid;
  }
  case ExactCarrierOperation::identity:
    break;
  }
  return false;
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
bool R3000Runtime::tryWriteExactCarrier(
    std::uint8_t destination, std::uint32_t result_raw,
    ExactCarrierOperation operation, std::uint8_t first_reg,
    std::uint32_t first_raw, std::uint8_t second_reg,
    std::uint32_t second_raw, std::uint8_t shift,
    const GteProjectedVertex *projected, const PgxpValue *pgxp,
    const ProjectedHalf *projected_half) noexcept {
  if (operation == ExactCarrierOperation::identity) {
    const auto *exact = exactRegister(first_reg, first_raw);
    if (exact == nullptr || first_raw != result_raw)
      return false;
    writeRegister(destination, result_raw, projected, exact, pgxp,
                  projected_half);
    return true;
  }
  GteExactWord exact;
  if (!buildExactCarrier(operation, first_reg, first_raw, second_reg,
                         second_raw, result_raw, shift, exact)) {
    return false;
  }
  writeRegister(destination, result_raw, projected, &exact, pgxp,
                projected_half);
  return true;
}

void R3000Runtime::writeRegister(std::uint8_t reg, std::uint32_t value,
                                 const GteProjectedVertex *projected,
                                 const GteExactWord *exact_word,
                                 const PgxpValue *pgxp,
                                 const ProjectedHalf *projected_half) noexcept {
  if (reg == 0U)
    return;
  state_.gpr[reg] = value;
  const auto register_bit = 1U << reg;
  const auto active_carrier =
      (gpu_projection_catalog_tracking_ &&
       (gpu_projection_handle_gpr_mask_ & register_bit) != 0U) ||
      ((projected_gpr_valid_mask_ | projected_half_gpr_valid_mask_) &
       register_bit) != 0U ||
      (pgxp_transform_tracking_ &&
       ((exactTransformCarrierTracking() &&
         (exact_tracking_->gpr_valid_mask & register_bit) != 0U) ||
        (pgxp_cpu_tracking_ && (exact_tracking_->pgxp_gpr_valid_mask &
                                (std::uint64_t{1} << reg)) != 0U)));
  const auto incoming_carrier = projected != nullptr || exact_word != nullptr ||
                                pgxp != nullptr || projected_half != nullptr;
  const auto cancels_delayed_load =
      state_.load_delay.valid && state_.load_delay.reg == reg;
  if (!active_carrier && !incoming_carrier && !cancels_delayed_load)
    return;
  std::uint16_t compact_handle{};
  if (gpu_projection_catalog_tracking_) {
    const auto bit = 1U << reg;
    const auto handle = compactProjectionHandle(projected, value);
    compact_handle = handle;
    gpu_projection_handle_gpr_[reg] = handle;
    if (handle != 0U) {
      gpu_projection_handle_gpr_mask_ |= bit;
    } else {
      gpu_projection_handle_gpr_mask_ &= ~bit;
    }
  }
  const auto valid_projected = projected != nullptr && projected->valid &&
                               projected->packed_sxy == value;
  const auto retain_direct_projected =
      valid_projected &&
      (compact_handle == 0U ||
       (pgxp_transform_tracking_ && pgxp_cpu_tracking_));
  if (retain_direct_projected) {
    projected_gpr_[reg] = *projected;
    projected_gpr_valid_mask_ |= register_bit;
  } else if ((projected_gpr_valid_mask_ & register_bit) != 0U) {
    projected_gpr_[reg].valid = false;
    projected_gpr_valid_mask_ &= ~register_bit;
  }
  if (pgxp_transform_tracking_) {
    if (pgxp_cpu_tracking_) {
      const auto bit = std::uint64_t{1} << reg;
      const auto valid_pgxp = pgxp != nullptr && pgxp->raw == value;
      const auto had_pgxp = (exact_tracking_->pgxp_gpr_valid_mask & bit) != 0U;
      if (valid_pgxp || valid_projected || had_pgxp) {
        if (valid_pgxp) {
          exact_tracking_->pgxp_gpr[reg] = *pgxp;
          exact_tracking_->pgxp_gpr[reg].generation = tracked_generation_;
          exact_tracking_->pgxp_gpr_valid_mask |= bit;
        } else if (valid_projected) {
          exact_tracking_->pgxp_gpr[reg] = pgxpFromProjected(*projected);
          exact_tracking_->pgxp_gpr_valid_mask |= bit;
        } else {
          exact_tracking_->pgxp_gpr_valid_mask &= ~bit;
        }
      }
    }
    if (projected_half != nullptr && projected_half->valid &&
        projected_half->register_value == value) {
      projected_half_gpr_[reg] = *projected_half;
      projected_half_gpr_valid_mask_ |= register_bit;
    } else if ((projected_half_gpr_valid_mask_ & register_bit) != 0U) {
      projected_half_gpr_[reg] = {};
      projected_half_gpr_valid_mask_ &= ~register_bit;
    }
    if (exactTransformCarrierTracking()) {
      const auto bit = 1U << reg;
      if (exact_word != nullptr && exact_word->raw == value) {
        exact_tracking_->gpr[reg] = *exact_word;
        exact_tracking_->gpr_valid_mask |= bit;
      } else if ((exact_tracking_->gpr_valid_mask & bit) != 0U) {
        exact_tracking_->gpr_valid_mask &= ~bit;
      }
    }
  }
  if (state_.load_delay.valid && state_.load_delay.reg == reg) {
    state_.load_delay.valid = false;
    if (gpu_projection_catalog_tracking_) {
      gpu_projection_handle_load_delay_ = 0U;
    }
    projected_load_delay_.valid = false;
    if (pgxp_transform_tracking_) {
      projected_half_load_delay_.valid = false;
      if (pgxp_cpu_tracking_ && exact_tracking_->pgxp_load_delay_valid) {
        exact_tracking_->pgxp_load_delay_valid = false;
      }
      if (exactTransformCarrierTracking()) {
        exact_tracking_->load_delay_valid = false;
      }
    }
  }
}

void R3000Runtime::scheduleLoad(std::uint8_t reg, std::uint32_t value,
                                const GteProjectedVertex *projected,
                                const ProjectedHalf *projected_half,
                                const GteExactWord *exact_word,
                                const PgxpValue *pgxp) noexcept {
  if (reg == 0U)
    return;
  if (state_.load_delay.valid && state_.load_delay.reg == reg) {
    state_.load_delay.valid = false;
    if (gpu_projection_catalog_tracking_) {
      gpu_projection_handle_load_delay_ = 0U;
    }
    projected_load_delay_.valid = false;
    if (pgxp_transform_tracking_) {
      projected_half_load_delay_.valid = false;
      if (pgxp_cpu_tracking_ && exact_tracking_->pgxp_load_delay_valid) {
        exact_tracking_->pgxp_load_delay_valid = false;
      }
      if (exactTransformCarrierTracking()) {
        exact_tracking_->load_delay_valid = false;
      }
    }
  }
  state_.next_load_delay = R3000DelayedLoadState{reg, value, true};
  std::uint16_t compact_handle{};
  if (gpu_projection_catalog_tracking_) {
    compact_handle = compactProjectionHandle(projected, value);
    gpu_projection_handle_next_load_delay_ = compact_handle;
  }
  if (compact_handle != 0U) {
    projected_next_load_delay_.valid = false;
  } else if (projected != nullptr && projected->valid &&
             projected->packed_sxy == value) {
    projected_next_load_delay_ = *projected;
  } else {
    projected_next_load_delay_.valid = false;
  }
  if (!pgxp_transform_tracking_)
    return;
  if (projected_half != nullptr && projected_half->valid &&
      projected_half->register_value == value) {
    projected_half_next_load_delay_ = *projected_half;
  } else {
    projected_half_next_load_delay_.valid = false;
  }
  if (pgxp_cpu_tracking_) {
    const auto valid_pgxp = pgxp != nullptr && pgxp->raw == value;
    const auto valid_projected = projected != nullptr && projected->valid &&
                                 projected->packed_sxy == value;
    if (valid_pgxp) {
      exact_tracking_->pgxp_next_load_delay = *pgxp;
      exact_tracking_->pgxp_next_load_delay.generation = tracked_generation_;
      exact_tracking_->pgxp_next_load_delay_valid = true;
    } else if (valid_projected) {
      exact_tracking_->pgxp_next_load_delay = pgxpFromProjected(*projected);
      exact_tracking_->pgxp_next_load_delay_valid = true;
    } else if (exact_tracking_->pgxp_next_load_delay_valid) {
      exact_tracking_->pgxp_next_load_delay_valid = false;
    }
  }
  if (exactTransformCarrierTracking()) {
    const auto valid_exact = exact_word != nullptr && exact_word->raw == value;
    if (valid_exact) {
      exact_tracking_->next_load_delay = *exact_word;
    }
    exact_tracking_->next_load_delay_valid = valid_exact;
  }
}

void R3000Runtime::advanceLoadDelay() noexcept {
  const bool current_valid = state_.load_delay.valid;
  const bool next_valid = state_.next_load_delay.valid;
  if (!current_valid && !next_valid) {
    state_.gpr[0] = 0U;
    return;
  }
  bool committed_compact_projection{};
  if (current_valid && state_.load_delay.reg != 0U) {
    state_.gpr[state_.load_delay.reg] = state_.load_delay.value;
    if (gpu_projection_catalog_tracking_) {
      const auto reg = state_.load_delay.reg;
      const auto bit = 1U << reg;
      const auto handle = gpu_projection_handle_load_delay_;
      const auto valid_handle =
          handle != 0U && handle <= gpu_projection_catalog_size_ &&
          gpu_projection_catalog_[handle - 1U].packed_sxy ==
              state_.load_delay.value;
      gpu_projection_handle_gpr_[reg] = valid_handle ? handle : 0U;
      if (valid_handle) {
        gpu_projection_handle_gpr_mask_ |= bit;
      } else {
        gpu_projection_handle_gpr_mask_ &= ~bit;
      }
      committed_compact_projection = valid_handle;
    }
    const auto load_reg = state_.load_delay.reg;
    const auto load_bit = 1U << load_reg;
    if (committed_compact_projection) {
      if ((projected_gpr_valid_mask_ & load_bit) != 0U)
        projected_gpr_[load_reg].valid = false;
      projected_gpr_valid_mask_ &= ~load_bit;
    } else if (projected_load_delay_.valid &&
               projected_load_delay_.packed_sxy == state_.load_delay.value) {
      projected_gpr_[load_reg] = projected_load_delay_;
      projected_gpr_valid_mask_ |= load_bit;
    } else if ((projected_gpr_valid_mask_ & load_bit) != 0U) {
      projected_gpr_[load_reg].valid = false;
      projected_gpr_valid_mask_ &= ~load_bit;
    }
    if (pgxp_transform_tracking_) {
      if (projected_half_load_delay_.valid &&
          projected_half_load_delay_.register_value ==
              state_.load_delay.value) {
        projected_half_gpr_[load_reg] = projected_half_load_delay_;
        projected_half_gpr_valid_mask_ |= load_bit;
      } else if ((projected_half_gpr_valid_mask_ & load_bit) != 0U) {
        projected_half_gpr_[load_reg].valid = false;
        projected_half_gpr_valid_mask_ &= ~load_bit;
      }
      if (pgxp_cpu_tracking_) {
        const auto reg = state_.load_delay.reg;
        const auto bit = std::uint64_t{1} << reg;
        const auto valid_pgxp =
            exact_tracking_->pgxp_load_delay_valid &&
            exact_tracking_->pgxp_load_delay.generation ==
                tracked_generation_ &&
            exact_tracking_->pgxp_load_delay.raw == state_.load_delay.value;
        if (valid_pgxp) {
          exact_tracking_->pgxp_gpr[reg] = exact_tracking_->pgxp_load_delay;
          exact_tracking_->pgxp_gpr_valid_mask |= bit;
        } else if ((exact_tracking_->pgxp_gpr_valid_mask & bit) != 0U) {
          exact_tracking_->pgxp_gpr_valid_mask &= ~bit;
        }
      }
      if (exactTransformCarrierTracking()) {
        const auto reg = state_.load_delay.reg;
        const auto bit = 1U << reg;
        if (exact_tracking_->load_delay_valid &&
            exact_tracking_->load_delay.raw == state_.load_delay.value) {
          exact_tracking_->gpr[reg] = exact_tracking_->load_delay;
          exact_tracking_->gpr_valid_mask |= bit;
        } else if ((exact_tracking_->gpr_valid_mask & bit) != 0U) {
          exact_tracking_->gpr_valid_mask &= ~bit;
        }
      }
    }
  }
  state_.load_delay = state_.next_load_delay;
  state_.next_load_delay = {};
  if (gpu_projection_catalog_tracking_) {
    gpu_projection_handle_load_delay_ =
        next_valid ? gpu_projection_handle_next_load_delay_ : 0U;
    gpu_projection_handle_next_load_delay_ = 0U;
  }
  if (next_valid) {
    if (projected_next_load_delay_.valid) {
      projected_load_delay_ = projected_next_load_delay_;
    } else {
      projected_load_delay_.valid = false;
    }
    projected_next_load_delay_.valid = false;
  } else if (current_valid) {
    projected_load_delay_.valid = false;
  }
  if (pgxp_transform_tracking_) {
    if (next_valid) {
      if (projected_half_next_load_delay_.valid) {
        projected_half_load_delay_ = projected_half_next_load_delay_;
      } else {
        projected_half_load_delay_.valid = false;
      }
      projected_half_next_load_delay_.valid = false;
      if (pgxp_cpu_tracking_) {
        exact_tracking_->pgxp_load_delay_valid =
            exact_tracking_->pgxp_next_load_delay_valid;
        if (exact_tracking_->pgxp_load_delay_valid)
          exact_tracking_->pgxp_load_delay =
              exact_tracking_->pgxp_next_load_delay;
        exact_tracking_->pgxp_next_load_delay_valid = false;
      }
      if (exactTransformCarrierTracking()) {
        if (exact_tracking_->next_load_delay_valid) {
          exact_tracking_->load_delay = exact_tracking_->next_load_delay;
        }
        exact_tracking_->load_delay_valid =
            exact_tracking_->next_load_delay_valid;
        exact_tracking_->next_load_delay_valid = false;
      }
    } else if (current_valid) {
      projected_half_load_delay_.valid = false;
      if (pgxp_cpu_tracking_) {
        exact_tracking_->pgxp_load_delay_valid = false;
      }
      if (exactTransformCarrierTracking()) {
        exact_tracking_->load_delay_valid = false;
      }
    }
  }
  state_.gpr[0] = 0U;
}

void R3000Runtime::flushLoadDelay() noexcept {
  advanceLoadDelay();
  advanceLoadDelay();
  clearLoadDelay();
}

void R3000Runtime::clearLoadDelay() noexcept {
  state_.load_delay = {};
  state_.next_load_delay = {};
  gpu_projection_handle_load_delay_ = 0U;
  gpu_projection_handle_next_load_delay_ = 0U;
  projected_load_delay_ = {};
  projected_next_load_delay_ = {};
  if (!pgxp_transform_tracking_)
    return;
  projected_half_load_delay_ = {};
  projected_half_next_load_delay_ = {};
  if (pgxp_cpu_tracking_) {
    exact_tracking_->pgxp_load_delay_valid = false;
    exact_tracking_->pgxp_next_load_delay_valid = false;
  }
  if (exactTransformCarrierTracking()) {
    exact_tracking_->load_delay_valid = false;
    exact_tracking_->next_load_delay_valid = false;
  }
}
void R3000Runtime::setExternalInterrupt(bool active) noexcept {
  constexpr std::uint32_t hardware_interrupt_bit = 1U << 10U;
  if (active) {
    state_.cop0_cause |= hardware_interrupt_bit;
  } else {
    state_.cop0_cause &= ~hardware_interrupt_bit;
  }
}

bool R3000Runtime::interruptPending() const noexcept {
  constexpr std::uint32_t interrupt_enable_current = 1U;
  constexpr std::uint32_t interrupt_mask = 0x0000ff00U;
  return (state_.cop0_status & interrupt_enable_current) != 0U &&
         (state_.cop0_status & state_.cop0_cause & interrupt_mask) != 0U;
}

void R3000Runtime::takeInterrupt() noexcept {
  constexpr std::uint32_t bootstrap_exception_vector = 1U << 22U;
  constexpr std::uint32_t branch_delay_bit = 1U << 31U;
  constexpr std::uint32_t exception_code_mask = 0x0000007cU;
  constexpr std::uint32_t mode_stack_mask = 0x0000003fU;

  const auto interrupted_pc = state_.pc;
  state_.cop0_epc =
      state_.branch_delay_slot ? state_.branch_pc : interrupted_pc;
  state_.cop0_cause &= ~(branch_delay_bit | exception_code_mask);
  if (state_.branch_delay_slot) {
    state_.cop0_cause |= branch_delay_bit;
  }
  state_.cop0_status = (state_.cop0_status & ~mode_stack_mask) |
                       ((state_.cop0_status << 2U) & mode_stack_mask);
  const auto vector = (state_.cop0_status & bootstrap_exception_vector) != 0U
                          ? 0xbfc00180U
                          : 0x80000080U;
  flushLoadDelay();
  state_.pc = vector;
  state_.next_pc = vector + 4U;
  state_.branch_pc = 0U;
  state_.branch_delay_slot = false;
}

R3000RunResult R3000Runtime::step() noexcept {
  const auto instruction_pc = state_.pc;
  if (interruptPending()) {
    takeInterrupt();
    return {R3000StopReason::running, 0U, instruction_pc, 0U};
  }
  std::uint32_t instruction{};
  if ((instruction_pc & 3U) != 0U) {
    return {R3000StopReason::alignment_fault, 0U, instruction_pc, 0U};
  }
  const auto fetch_physical =
      instruction_pc >= 0x80000000U && instruction_pc < 0xc0000000U
          ? instruction_pc & physical_address_mask
          : instruction_pc;
  if (fetch_physical < ram_mirror_end) {
    const auto offset =
        fetch_physical & static_cast<std::uint32_t>(ram_size - 1U);
    std::memcpy(&instruction, ram_.data() + offset, sizeof(instruction));
  } else if (!read32(instruction_pc, instruction)) {
    return {R3000StopReason::memory_fault, 0U, instruction_pc, 0U};
  }

  const auto opcode = static_cast<std::uint8_t>(instruction >> 26U);
  const auto rs = static_cast<std::uint8_t>((instruction >> 21U) & 31U);
  const auto rt = static_cast<std::uint8_t>((instruction >> 16U) & 31U);
  auto pgxpSourceMarked = [&](std::uint8_t reg) noexcept {
    return pgxp_transform_tracking_ && pgxp_cpu_tracking_ &&
           (exact_tracking_->pgxp_gpr_valid_mask & (std::uint64_t{1} << reg)) !=
               0U;
  };
  auto pgxpSourcesMarked = [&](std::uint8_t first,
                               std::uint8_t second) noexcept {
    return pgxp_transform_tracking_ && pgxp_cpu_tracking_ &&
           (exact_tracking_->pgxp_gpr_valid_mask &
            ((std::uint64_t{1} << first) | (std::uint64_t{1} << second))) != 0U;
  };
  const auto rd = static_cast<std::uint8_t>((instruction >> 11U) & 31U);
  const auto shift = static_cast<std::uint8_t>((instruction >> 6U) & 31U);
  const auto function = static_cast<std::uint8_t>(instruction & 63U);
  const auto immediate = instruction & 0xffffU;
  const auto left = state_.gpr[rs];
  const auto right = state_.gpr[rt];

  state_.branch_delay_slot = false;
  state_.pc = state_.next_pc;
  state_.next_pc += 4U;

  auto stop = R3000StopReason::running;
  auto memoryAddress = [&]() noexcept {
    return left + signExtend16(immediate);
  };
  auto branch = [&](bool taken) noexcept {
    state_.branch_pc = instruction_pc;
    state_.branch_delay_slot = true;
    if (taken) {
      state_.next_pc = instruction_pc + 4U + (signExtend16(immediate) << 2U);
    }
  };
  auto loadWord = [&](std::uint32_t address, std::uint32_t &value) noexcept {
    if ((address & 3U) != 0U) {
      stop = R3000StopReason::alignment_fault;
      return false;
    }
    if (!read32(address, value)) {
      stop = R3000StopReason::memory_fault;
      return false;
    }
    return true;
  };
  auto storeWord = [&](std::uint32_t address, std::uint32_t value,
                       const GteProjectedVertex *projected) noexcept {
    if ((address & 3U) != 0U) {
      stop = R3000StopReason::alignment_fault;
      return false;
    }
    if (!write32Projected(address, value, projected)) {
      stop = R3000StopReason::memory_fault;
      return false;
    }
    return true;
  };

  auto signedHalf = [](std::uint32_t value, bool high) noexcept {
    const auto raw = static_cast<std::uint16_t>(value >> (high ? 16U : 0U));
    return static_cast<double>(static_cast<std::int16_t>(raw));
  };
  auto component = [&](const PgxpValue *value, std::uint32_t raw,
                       bool high) noexcept {
    if (value != nullptr &&
        value->has(high ? PgxpValue::valid_y : PgxpValue::valid_x)) {
      return high ? value->y : value->x;
    }
    return signedHalf(raw, high);
  };
  auto unsignedLow = [](double value) noexcept {
    return value < 0.0 ? value + 65536.0 : value;
  };
  auto signedHalfValue = [](double value) noexcept {
    // ADD/SUB/shift component values normally stay within two halfword
    // periods. Avoid the CRT fmod call in the interpreter's hottest PGXP
    // carrier path while retaining the general fallback for MULT/DIV data.
    if (value >= -131072.0 && value < 131072.0) {
      if (value < -32768.0)
        value += 65536.0;
      if (value < -32768.0)
        value += 65536.0;
      if (value >= 32768.0)
        value -= 65536.0;
      if (value >= 32768.0)
        value -= 65536.0;
      return value;
    }
    auto wrapped = std::fmod(value + 32768.0, 65536.0);
    if (wrapped < 0.0)
      wrapped += 65536.0;
    return wrapped - 32768.0;
  };
  auto halfOverflow = [](double value) noexcept {
    return std::floor(value / 65536.0);
  };
  auto continuous = [&](const PgxpValue *value, std::uint32_t raw,
                        bool logical) noexcept {
    const auto low = unsignedLow(component(value, raw, false));
    auto high = component(value, raw, true);
    if (logical && high < 0.0)
      high += 65536.0;
    return low + high * 65536.0;
  };
  auto inheritContext = [](PgxpValue &destination, const PgxpValue *first,
                           const PgxpValue *second) noexcept {
    const auto first_valid = first != nullptr && first->has(PgxpValue::valid_z);
    const auto second_valid =
        second != nullptr && second->has(PgxpValue::valid_z);
    const auto prefer_second =
        !first_valid || (first->has(PgxpValue::tainted_z) && second_valid &&
                         !second->has(PgxpValue::tainted_z));
    const auto *source =
        prefer_second ? (second_valid ? second : nullptr) : first;
    if (source == nullptr)
      return;
    destination.context_handle = source->context_handle;
    destination.z = source->z;
    destination.flags |=
        PgxpValue::valid_z | PgxpValue::z_from_low | PgxpValue::z_from_high;
    if ((first != nullptr && first->has(PgxpValue::tainted_z)) ||
        (second != nullptr && second->has(PgxpValue::tainted_z))) {
      destination.flags |= PgxpValue::tainted_z;
    }
  };
  auto decompose = [&](double scalar, std::uint32_t raw, const PgxpValue *first,
                       const PgxpValue *second) noexcept {
    PgxpValue result{};
    constexpr double word_modulus = 4294967296.0;
    auto wrapped = std::fmod(scalar, word_modulus);
    if (wrapped < 0.0)
      wrapped += word_modulus;
    auto low = std::fmod(wrapped, 65536.0);
    if (low < 0.0)
      low += 65536.0;
    auto high = std::floor(wrapped / 65536.0);
    result.x = low >= 32768.0 ? low - 65536.0 : low;
    result.y = high >= 32768.0 ? high - 65536.0 : high;
    result.raw = raw;
    result.flags = PgxpValue::valid_x | PgxpValue::valid_y;
    inheritContext(result, first, second);
    return result;
  };
  const auto unmodified = [](const PgxpValue *value,
                             std::uint32_t raw) noexcept {
    return value != nullptr && value->raw == raw && !value->derived_x &&
           !value->derived_y;
  };
  auto valueLineage = [](const PgxpValue *value) noexcept {
    return value != nullptr ? pgxpLineageMix(value->lineage_x, value->lineage_y)
                            : 0ULL;
  };
  auto arithmetic = [&](const PgxpValue *first, std::uint32_t first_raw,
                        const PgxpValue *second, std::uint32_t second_raw,
                        std::uint32_t raw, bool subtract) noexcept {
    if (first == nullptr && second == nullptr)
      return PgxpValue{};
    if (unmodified(first, raw) && second == nullptr && second_raw == 0U) {
      auto result = *first;
      result.raw = raw;
      return result;
    }
    if (!subtract && unmodified(second, raw) && first == nullptr &&
        first_raw == 0U) {
      auto result = *second;
      result.raw = raw;
      return result;
    }
    const auto low = unsignedLow(component(first, first_raw, false)) +
                     (subtract ? -1.0 : 1.0) *
                         unsignedLow(component(second, second_raw, false));
    const auto carry = low > 65535.0 ? 1.0 : low < 0.0 ? -1.0 : 0.0;
    auto result = PgxpValue{};
    result.x = signedHalfValue(low);
    result.y =
        signedHalfValue(component(first, first_raw, true) +
                        (subtract ? -component(second, second_raw, true)
                                  : component(second, second_raw, true)) +
                        carry);
    result.raw = raw;
    result.flags = PgxpValue::valid_x | PgxpValue::valid_y;
    inheritContext(result, first, second);
    const auto second_lineage = second != nullptr
                                    ? valueLineage(second)
                                    : pgxpLineageMix(0xc05a7ULL, second_raw);
    const auto lineage =
        pgxpLineageMix(pgxpLineageMix(valueLineage(first), second_lineage),
                       subtract ? 0x535542ULL : 0x414444ULL);
    result.lineage_x = lineage;
    result.lineage_y = lineage;
    result.derived_x = true;
    result.derived_y = true;
    result.flags |= PgxpValue::tainted_z;
    return result;
  };
  auto shiftValue = [&](const PgxpValue *source, std::uint32_t source_raw,
                        std::uint32_t raw, std::uint32_t amount,
                        bool left_shift, bool arithmetic_right) noexcept {
    if (source == nullptr)
      return PgxpValue{};
    amount &= 31U;
    if (amount == 0U) {
      auto result = *source;
      result.raw = raw;
      return result;
    }
    const auto scale = static_cast<double>(std::uint64_t{1} << amount);
    PgxpValue result{};
    result.raw = raw;
    result.flags = PgxpValue::valid_x | PgxpValue::valid_y;
    inheritContext(result, source, nullptr);
    if (left_shift) {
      if (amount >= 16U) {
        result.x = 0.0;
        result.y = signedHalfValue(
            unsignedLow(component(source, source_raw, false)) *
            static_cast<double>(std::uint64_t{1} << (amount - 16U)));
      } else {
        const auto low =
            unsignedLow(component(source, source_raw, false)) * scale;
        const auto high =
            unsignedLow(component(source, source_raw, true)) * scale +
            halfOverflow(low);
        result.x = signedHalfValue(low);
        result.y = signedHalfValue(high);
      }
    } else {
      auto low = component(source, source_raw, false);
      auto high = arithmetic_right
                      ? component(source, source_raw, true)
                      : unsignedLow(component(source, source_raw, true));
      const auto signed_low =
          static_cast<std::int32_t>(static_cast<std::int16_t>(source_raw));
      const auto low_sign_word = static_cast<std::uint32_t>(signed_low);
      const auto high_test_word =
          (source_raw & 0xffff0000U) | (low_sign_word >> 16U);
      const auto shifted_low = static_cast<std::uint32_t>(
          static_cast<std::int32_t>(low_sign_word) >> amount);
      const auto shifted_high =
          arithmetic_right
              ? static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(high_test_word) >> amount)
              : high_test_word >> amount;
      const auto low_sign = static_cast<std::int16_t>(low_sign_word >> 16U);
      if (static_cast<std::int16_t>(shifted_low) != low_sign) {
        low /= scale;
      } else {
        low = static_cast<std::int16_t>(shifted_low);
      }
      if (static_cast<std::int16_t>(shifted_high) != low_sign) {
        if (amount == 16U) {
          low = high;
        } else if (amount < 16U) {
          low += high * static_cast<double>(std::uint64_t{1} << (16U - amount));
          if (component(source, source_raw, false) < 0.0) {
            low += static_cast<double>(std::uint64_t{1} << (16U - amount));
          }
        } else {
          low += high / static_cast<double>(std::uint64_t{1} << (amount - 16U));
        }
      }
      const auto shifted_high_half =
          static_cast<std::int16_t>(shifted_high >> 16U);
      if (shifted_high_half == 0 || shifted_high_half == -1) {
        high = shifted_high_half;
      } else {
        high /= scale;
      }
      if (arithmetic_right && amount < 16U &&
          !source->has(PgxpValue::valid_z)) {
        result.x = static_cast<std::int16_t>(raw);
        result.y = static_cast<std::int16_t>(raw >> 16U);
        result.z = 0.0F;
        result.context_handle = 0U;
        result.flags =
            PgxpValue::valid_x | PgxpValue::valid_y | PgxpValue::tainted_z;
      } else {
        result.x = signedHalfValue(low);
        result.y = signedHalfValue(high);
      }
    }
    const auto lineage = pgxpLineageMix(valueLineage(source),
                                        (left_shift         ? 0x534c4cULL
                                         : arithmetic_right ? 0x535241ULL
                                                            : 0x53524cULL) ^
                                            amount);
    result.lineage_x = lineage;
    result.lineage_y = lineage;
    result.derived_x = true;
    result.derived_y = true;
    result.flags |= PgxpValue::tainted_z;
    return result;
  };
  auto bitwise = [&](const PgxpValue *first, std::uint32_t first_raw,
                     const PgxpValue *second, std::uint32_t second_raw,
                     std::uint32_t raw) noexcept {
    if (first == nullptr && second == nullptr)
      return PgxpValue{};
    if (first != nullptr && unmodified(first, raw)) {
      auto result = *first;
      result.raw = raw;
      return result;
    }
    if (second != nullptr && unmodified(second, raw)) {
      auto result = *second;
      result.raw = raw;
      return result;
    }
    PgxpValue result{};
    result.raw = raw;
    const auto result_low = static_cast<std::uint16_t>(raw);
    const auto result_high = static_cast<std::uint16_t>(raw >> 16U);
    const auto pick = [&](bool high, std::uint16_t expected) {
      if (expected == 0U)
        return 0.0;
      const auto first_half =
          static_cast<std::uint16_t>(first_raw >> (high ? 16U : 0U));
      const auto second_half =
          static_cast<std::uint16_t>(second_raw >> (high ? 16U : 0U));
      if (first != nullptr && expected == first_half)
        return component(first, first_raw, high);
      if (second != nullptr && expected == second_half)
        return component(second, second_raw, high);
      return static_cast<double>(static_cast<std::int16_t>(expected));
    };
    result.x = pick(false, result_low);
    result.y = pick(true, result_high);
    result.flags =
        PgxpValue::valid_x | PgxpValue::valid_y | PgxpValue::tainted_z;
    inheritContext(result, first, second);
    const auto lineage =
        pgxpLineageMix(valueLineage(first), valueLineage(second));
    result.lineage_x = lineage;
    result.lineage_y = lineage;
    result.derived_x = true;
    result.derived_y = true;
    return result;
  };
  constexpr auto pgxp_lo = std::uint8_t{32U};
  constexpr auto pgxp_hi = std::uint8_t{33U};
  auto writePgxpSpecial = [&](std::uint8_t reg, std::uint32_t raw,
                              const PgxpValue *value) noexcept {
    if (!pgxp_transform_tracking_ || !pgxp_cpu_tracking_)
      return;
    const auto bit = std::uint64_t{1} << reg;
    const auto valid =
        value != nullptr && value->raw == raw &&
        (value->flags & (PgxpValue::valid_x | PgxpValue::valid_y)) != 0U;
    if (!valid) {
      if ((exact_tracking_->pgxp_gpr_valid_mask & bit) != 0U) {
        exact_tracking_->pgxp_gpr_valid_mask &= ~bit;
      }
      return;
    }
    exact_tracking_->pgxp_gpr[reg] = *value;
    exact_tracking_->pgxp_gpr[reg].generation = tracked_generation_;
    exact_tracking_->pgxp_gpr_valid_mask |= bit;
  };
  auto mulDivResult = [&](double scalar, std::uint32_t raw,
                          const PgxpValue *first, const PgxpValue *second,
                          std::uint64_t operation) noexcept {
    if (!std::isfinite(scalar))
      return PgxpValue{};
    auto result = decompose(scalar, raw, first, second);
    const auto lineage = pgxpLineageMix(
        pgxpLineageMix(valueLineage(first), valueLineage(second)), operation);
    result.lineage_x = lineage;
    result.lineage_y = lineage;
    result.source_vertex_id = 0U;
    result.derived_x = true;
    result.derived_y = true;
    result.flags |= PgxpValue::tainted_z;
    return result;
  };
  auto multiplyResult = [&](double low, double high, std::uint32_t raw,
                            const PgxpValue *first, const PgxpValue *second,
                            std::uint64_t operation) noexcept {
    PgxpValue result{};
    result.x = signedHalfValue(low);
    result.y = signedHalfValue(high);
    result.raw = raw;
    result.flags =
        PgxpValue::valid_x | PgxpValue::valid_y | PgxpValue::tainted_z;
    inheritContext(result, first, second);
    const auto lineage = pgxpLineageMix(
        pgxpLineageMix(valueLineage(first), valueLineage(second)), operation);
    result.lineage_x = lineage;
    result.lineage_y = lineage;
    result.source_vertex_id = 0U;
    result.derived_x = true;
    result.derived_y = true;
    return result;
  };
  auto screen_source_mask =
      gpu_projection_handle_gpr_mask_ | projected_gpr_valid_mask_ |
      projected_half_gpr_valid_mask_;
  if (pgxp_transform_tracking_ && pgxp_cpu_tracking_ &&
      exact_tracking_ != nullptr) {
    screen_source_mask |= static_cast<std::uint32_t>(
        exact_tracking_->pgxp_gpr_valid_mask);
  }
  const auto screenSourceMarked = [screen_source_mask](
                                      std::uint8_t reg) noexcept {
    return reg < 32U && (screen_source_mask & (1U << reg)) != 0U;
  };
  const auto screenProjectedRegister =
      [&](std::uint8_t reg, std::uint32_t raw) noexcept
      -> const GteProjectedVertex * {
    return screenSourceMarked(reg) ? projectedRegister(reg, raw) : nullptr;
  };
  const auto screenSourcesMarked =
      [screen_source_mask](std::uint8_t first,
                           std::uint8_t second) noexcept {
        return first < 32U && second < 32U &&
               (screen_source_mask & ((1U << first) | (1U << second))) != 0U;
      };
  const auto projectionHalf = [&](std::uint8_t reg, std::uint32_t raw,
                                  std::uint8_t register_slot) noexcept {
    if (!screenSourceMarked(reg))
      return ProjectedHalf{};
    if (const auto *half = projectedHalfRegister(reg, raw);
        half != nullptr && half->register_slot == register_slot) {
      return *half;
    }
    ProjectedHalf half{};
    const auto *projected = screenProjectedRegister(reg, raw);
    if (projected == nullptr || register_slot > 1U)
      return half;
    const auto component = static_cast<std::uint16_t>(projected->packed_sxy >>
                                                      (register_slot * 16U));
    if (static_cast<std::uint16_t>(raw >> (register_slot * 16U)) != component) {
      return half;
    }
    half.projected = *projected;
    half.screen_position =
        register_slot == 0U ? projected->screen_x : projected->screen_y;
    half.register_value = raw;
    half.value = component;
    half.slot = register_slot;
    half.register_slot = register_slot;
    half.valid = std::isfinite(half.screen_position);
    return half;
  };
  const auto shiftedProjectionHalf = [&](std::uint8_t reg, std::uint32_t raw,
                                         std::uint32_t result,
                                         std::uint32_t amount,
                                         bool left_shift) noexcept {
    if (!screenSourceMarked(reg))
      return ProjectedHalf{};
    ProjectedHalf half{};
    if (amount == 0U) {
      if (const auto *existing = projectedHalfRegister(reg, raw);
          existing != nullptr) {
        half = *existing;
        half.register_value = result;
      }
      return half;
    }
    if (amount != 16U)
      return half;
    const auto source_slot = static_cast<std::uint8_t>(left_shift ? 0U : 1U);
    half = projectionHalf(reg, raw, source_slot);
    if (!half.valid)
      return half;
    half.register_slot = static_cast<std::uint8_t>(left_shift ? 1U : 0U);
    half.register_value = result;
    if (static_cast<std::uint16_t>(result >> (half.register_slot * 16U)) !=
        half.value) {
      half = {};
    }
    return half;
  };
  const auto offsetProjectionHalf = [&](std::uint8_t reg, std::uint32_t raw,
                                        std::uint32_t result,
                                        std::int64_t delta) noexcept {
    if (!screenSourceMarked(reg))
      return ProjectedHalf{};
    auto half = projectionHalf(reg, raw, 0U);
    if (!half.valid)
      return half;
    const auto adjusted =
        static_cast<std::int64_t>(static_cast<std::int16_t>(half.value)) +
        delta;
    if (adjusted < std::numeric_limits<std::int16_t>::min() ||
        adjusted > std::numeric_limits<std::int16_t>::max() ||
        static_cast<std::uint16_t>(adjusted) !=
            static_cast<std::uint16_t>(result)) {
      return ProjectedHalf{};
    }
    half.screen_position += static_cast<float>(delta);
    half.register_value = result;
    half.value = static_cast<std::uint16_t>(adjusted);
    half.register_slot = 0U;
    half.derived = half.derived || delta != 0;
    return half;
  };
  const auto binaryProjectionHalf =
      [&](std::uint8_t first_reg, std::uint32_t first_raw,
          std::uint8_t second_reg, std::uint32_t second_raw,
          std::uint32_t result, bool subtract) noexcept {
        if (!screenSourcesMarked(first_reg, second_reg))
          return ProjectedHalf{};
        const auto first = projectionHalf(first_reg, first_raw, 0U);
        const auto second = projectionHalf(second_reg, second_raw, 0U);
        if (first.valid == second.valid)
          return ProjectedHalf{};
        if (first.valid) {
          const auto delta = subtract
                                 ? -static_cast<std::int64_t>(
                                       static_cast<std::int32_t>(second_raw))
                                 : static_cast<std::int64_t>(
                                       static_cast<std::int32_t>(second_raw));
          return offsetProjectionHalf(first_reg, first_raw, result, delta);
        }
        if (subtract)
          return ProjectedHalf{};
        return offsetProjectionHalf(
            second_reg, second_raw, result,
            static_cast<std::int64_t>(static_cast<std::int32_t>(first_raw)));
      };

  const auto withProjectedHalf =
      [&](bool source_marked, auto &&project_half, auto &&consume) noexcept {
        if (!source_marked) {
          consume(nullptr);
          return;
        }
        const auto projected_half = project_half();
        consume(projected_half.valid ? &projected_half : nullptr);
      };

  const auto writeRegisterWithProjectedHalf =
      [&](std::uint8_t destination, std::uint32_t value,
          const GteProjectedVertex *projected,
          const GteExactWord *exact_word, const PgxpValue *pgxp,
          bool source_marked, auto &&project_half) noexcept {
        if (!source_marked) {
          writeRegister(destination, value, projected, exact_word, pgxp);
          return;
        }
        const auto projected_half = project_half();
        writeRegister(destination, value, projected, exact_word, pgxp,
                      projected_half.valid ? &projected_half : nullptr);
      };

  switch (opcode) {
  case 0x00: {
    switch (function) {
    case 0x00: {
      if (rd == 0U)
        break;
      const auto value = right << shift;
      const auto pgxp = pgxpSourceMarked(rt)
                            ? shiftValue(pgxpRegister(rt, right), right, value,
                                         shift, true, false)
                            : PgxpValue{};
      withProjectedHalf(
          screenSourceMarked(rt),
          [&] { return shiftedProjectionHalf(rt, right, value, shift, true); },
          [&](const ProjectedHalf *projected_half) {
            if (exact_tracking_ != nullptr &&
                (exact_tracking_->gpr_valid_mask & (1U << rt)) != 0U)
                [[unlikely]] {
              if (shift == 0U &&
                  tryWriteExactCarrier(
                      rd, value, ExactCarrierOperation::identity, rt, right,
                      0xffU, 0U, shift,
                      shift == 0U && screenSourceMarked(rt)
                          ? projectedRegister(rt, right)
                          : nullptr,
                      pgxp.flags != 0U ? &pgxp : nullptr, projected_half)) {
                return;
              }
              if (shift == 16U &&
                  tryWriteExactCarrier(
                      rd, value, ExactCarrierOperation::shift_left_16, rt,
                      right, 0xffU, 0U, shift, nullptr,
                      pgxp.flags != 0U ? &pgxp : nullptr, projected_half)) {
                return;
              }
            }
            writeRegister(
                rd, value,
                shift == 0U && screenSourceMarked(rt)
                    ? projectedRegister(rt, right) : nullptr, nullptr,
                pgxp.flags != 0U ? &pgxp : nullptr, projected_half);
          });
      break;
    }
    case 0x02: {
      const auto value = right >> shift;
      const auto pgxp = pgxpSourceMarked(rt)
                            ? shiftValue(pgxpRegister(rt, right), right, value,
                                         shift, false, false)
                            : PgxpValue{};
      withProjectedHalf(
          screenSourceMarked(rt),
          [&] { return shiftedProjectionHalf(rt, right, value, shift, false); },
          [&](const ProjectedHalf *projected_half) {
            if (exact_tracking_ != nullptr && shift <= 16U &&
                (exact_tracking_->gpr_valid_mask & (1U << rt)) != 0U)
                [[unlikely]] {
              const auto operation =
                  shift == 0U ? ExactCarrierOperation::identity
                              : ExactCarrierOperation::shift_right_logical_16;
              if ((shift == 0U || shift == 16U) &&
                  tryWriteExactCarrier(
                      rd, value, operation, rt, right, 0xffU, 0U, shift,
                      shift == 0U && screenSourceMarked(rt)
                          ? projectedRegister(rt, right) : nullptr,
                      pgxp.flags != 0U ? &pgxp : nullptr, projected_half)) {
                return;
              }
            }
            writeRegister(
                rd, value,
                shift == 0U && screenSourceMarked(rt)
                    ? projectedRegister(rt, right) : nullptr, nullptr,
                pgxp.flags != 0U ? &pgxp : nullptr, projected_half);
          });
      break;
    }
    case 0x03: {
      const auto value = arithmeticShiftRight(right, shift);
      const auto pgxp = pgxpSourceMarked(rt)
                            ? shiftValue(pgxpRegister(rt, right), right, value,
                                         shift, false, true)
                            : PgxpValue{};
      withProjectedHalf(
          screenSourceMarked(rt),
          [&] { return shiftedProjectionHalf(rt, right, value, shift, false); },
          [&](const ProjectedHalf *projected_half) {
            if (exact_tracking_ != nullptr && shift <= 16U &&
                (exact_tracking_->gpr_valid_mask & (1U << rt)) != 0U)
                [[unlikely]] {
              const auto operation =
                  shift == 0U ? ExactCarrierOperation::identity
                              : ExactCarrierOperation::shift_right_arithmetic_16;
              if ((shift == 0U || shift == 16U) &&
                  tryWriteExactCarrier(
                      rd, value, operation, rt, right, 0xffU, 0U, shift,
                      shift == 0U && screenSourceMarked(rt)
                          ? projectedRegister(rt, right) : nullptr,
                      pgxp.flags != 0U ? &pgxp : nullptr, projected_half)) {
                return;
              }
            }
            writeRegister(
                rd, value,
                shift == 0U && screenSourceMarked(rt)
                    ? projectedRegister(rt, right) : nullptr, nullptr,
                pgxp.flags != 0U ? &pgxp : nullptr, projected_half);
          });
      break;
    }
    case 0x04: {
      const auto amount = left & 31U;
      const auto value = right << amount;
      const auto pgxp = pgxpSourceMarked(rt)
                            ? shiftValue(pgxpRegister(rt, right), right, value,
                                         amount, true, false)
                            : PgxpValue{};
      writeRegisterWithProjectedHalf(
          rd, value,
          amount == 0U && screenSourceMarked(rt)
              ? projectedRegister(rt, right) : nullptr, nullptr,
          pgxp.flags != 0U ? &pgxp : nullptr, screenSourceMarked(rt), [&] {
            return shiftedProjectionHalf(rt, right, value, amount, true);
          });
      break;
    }
    case 0x06: {
      const auto amount = left & 31U;
      const auto value = right >> amount;
      const auto pgxp = pgxpSourceMarked(rt)
                            ? shiftValue(pgxpRegister(rt, right), right, value,
                                         amount, false, false)
                            : PgxpValue{};
      writeRegisterWithProjectedHalf(
          rd, value,
          amount == 0U && screenSourceMarked(rt)
              ? projectedRegister(rt, right) : nullptr, nullptr,
          pgxp.flags != 0U ? &pgxp : nullptr, screenSourceMarked(rt), [&] {
            return shiftedProjectionHalf(rt, right, value, amount, false);
          });
      break;
    }
    case 0x07: {
      const auto amount = left & 31U;
      const auto value = arithmeticShiftRight(right, amount);
      const auto pgxp = pgxpSourceMarked(rt)
                            ? shiftValue(pgxpRegister(rt, right), right, value,
                                         amount, false, true)
                            : PgxpValue{};
      writeRegisterWithProjectedHalf(
          rd, value,
          amount == 0U && screenSourceMarked(rt)
              ? projectedRegister(rt, right) : nullptr, nullptr,
          pgxp.flags != 0U ? &pgxp : nullptr, screenSourceMarked(rt), [&] {
            return shiftedProjectionHalf(rt, right, value, amount, false);
          });
      break;
    }
    case 0x08:
      state_.branch_pc = instruction_pc;
      state_.branch_delay_slot = true;
      state_.next_pc = left;
      break;
    case 0x09: {
      const auto target = left;
      writeRegister(rd, instruction_pc + 8U);
      state_.branch_pc = instruction_pc;
      state_.branch_delay_slot = true;
      state_.next_pc = target;
      break;
    }
    case 0x0c:
      stop = R3000StopReason::syscall;
      break;
    case 0x0d:
      stop = R3000StopReason::breakpoint;
      break;
    case 0x10:
      writeRegister(rd, state_.hi, nullptr, nullptr,
                    pgxpRegister(pgxp_hi, state_.hi));
      break;
    case 0x11:
      state_.hi = left;
      writePgxpSpecial(pgxp_hi, state_.hi, pgxpRegister(rs, left));
      break;
    case 0x12:
      writeRegister(rd, state_.lo, nullptr, nullptr,
                    pgxpRegister(pgxp_lo, state_.lo));
      break;
    case 0x13:
      state_.lo = left;
      writePgxpSpecial(pgxp_lo, state_.lo, pgxpRegister(rs, left));
      break;
    case 0x18: {
      const auto product = static_cast<std::int64_t>(asSigned(left)) *
                           static_cast<std::int64_t>(asSigned(right));
      const auto bits = static_cast<std::uint64_t>(product);
      state_.lo = static_cast<std::uint32_t>(bits);
      state_.hi = static_cast<std::uint32_t>(bits >> 32U);
      const auto marked = pgxpSourcesMarked(rs, rt);
      const auto *left_pgxp = marked ? pgxpRegister(rs, left) : nullptr;
      const auto *right_pgxp = marked ? pgxpRegister(rt, right) : nullptr;
      if (left_pgxp != nullptr || right_pgxp != nullptr) {
        const auto left_x = unsignedLow(component(left_pgxp, left, false));
        const auto left_y = component(left_pgxp, left, true);
        const auto right_x = unsignedLow(component(right_pgxp, right, false));
        const auto right_y = component(right_pgxp, right, true);
        const auto xx = left_x * right_x;
        const auto xy = left_x * right_y;
        const auto yx = left_y * right_x;
        const auto yy = left_y * right_y;
        const auto lo_y = halfOverflow(xx) + xy + yx;
        const auto hi_x = halfOverflow(lo_y) + yy;
        const auto hi_y = halfOverflow(hi_x);
        const auto lo = multiplyResult(xx, lo_y, state_.lo, left_pgxp,
                                       right_pgxp, 0x4d554c54ULL);
        const auto hi = multiplyResult(hi_x, hi_y, state_.hi, left_pgxp,
                                       right_pgxp, 0x4d554c544849ULL);
        writePgxpSpecial(pgxp_lo, state_.lo, &lo);
        writePgxpSpecial(pgxp_hi, state_.hi, &hi);
      } else {
        writePgxpSpecial(pgxp_lo, state_.lo, nullptr);
        writePgxpSpecial(pgxp_hi, state_.hi, nullptr);
      }
      break;
    }
    case 0x19: {
      const auto product = static_cast<std::uint64_t>(left) * right;
      state_.lo = static_cast<std::uint32_t>(product);
      state_.hi = static_cast<std::uint32_t>(product >> 32U);
      const auto marked = pgxpSourcesMarked(rs, rt);
      const auto *left_pgxp = marked ? pgxpRegister(rs, left) : nullptr;
      const auto *right_pgxp = marked ? pgxpRegister(rt, right) : nullptr;
      if (left_pgxp != nullptr || right_pgxp != nullptr) {
        const auto left_x = unsignedLow(component(left_pgxp, left, false));
        const auto left_y = unsignedLow(component(left_pgxp, left, true));
        const auto right_x = unsignedLow(component(right_pgxp, right, false));
        const auto right_y = unsignedLow(component(right_pgxp, right, true));
        const auto xx = left_x * right_x;
        const auto xy = left_x * right_y;
        const auto yx = left_y * right_x;
        const auto yy = left_y * right_y;
        const auto lo_y = halfOverflow(xx) + xy + yx;
        const auto hi_x = halfOverflow(lo_y) + yy;
        const auto hi_y = halfOverflow(hi_x);
        const auto lo = multiplyResult(xx, lo_y, state_.lo, left_pgxp,
                                       right_pgxp, 0x4d554c5455ULL);
        const auto hi = multiplyResult(hi_x, hi_y, state_.hi, left_pgxp,
                                       right_pgxp, 0x4d554c54554849ULL);
        writePgxpSpecial(pgxp_lo, state_.lo, &lo);
        writePgxpSpecial(pgxp_hi, state_.hi, &hi);
      } else {
        writePgxpSpecial(pgxp_lo, state_.lo, nullptr);
        writePgxpSpecial(pgxp_hi, state_.hi, nullptr);
      }
      break;
    }
    case 0x1a: {
      const auto numerator = asSigned(left);
      const auto denominator = asSigned(right);
      if (denominator == 0) {
        state_.lo = numerator >= 0 ? 0xffffffffU : 1U;
        state_.hi = left;
      } else if (left == 0x80000000U && denominator == -1) {
        state_.lo = 0x80000000U;
        state_.hi = 0U;
      } else {
        state_.lo = static_cast<std::uint32_t>(numerator / denominator);
        state_.hi = static_cast<std::uint32_t>(numerator % denominator);
      }
      const auto marked = pgxpSourcesMarked(rs, rt);
      const auto *left_pgxp = marked ? pgxpRegister(rs, left) : nullptr;
      const auto *right_pgxp = marked ? pgxpRegister(rt, right) : nullptr;
      if ((left_pgxp != nullptr || right_pgxp != nullptr) && denominator != 0) {
        if (denominator == 1 && left_pgxp != nullptr && right_pgxp == nullptr) {
          auto lo = *left_pgxp;
          lo.raw = state_.lo;
          writePgxpSpecial(pgxp_lo, state_.lo, &lo);
          writePgxpSpecial(pgxp_hi, state_.hi, nullptr);
          break;
        }
        const auto left_value = continuous(left_pgxp, left, false);
        const auto right_value = continuous(right_pgxp, right, false);
        const auto lo = mulDivResult(left_value / right_value, state_.lo,
                                     left_pgxp, right_pgxp, 0x444956ULL);
        const auto hi =
            mulDivResult(std::fmod(left_value, right_value), state_.hi,
                         left_pgxp, right_pgxp, 0x52454dULL);
        writePgxpSpecial(pgxp_lo, state_.lo, &lo);
        writePgxpSpecial(pgxp_hi, state_.hi, &hi);
      } else {
        writePgxpSpecial(pgxp_lo, state_.lo, nullptr);
        writePgxpSpecial(pgxp_hi, state_.hi, nullptr);
      }
      break;
    }
    case 0x1b: {
      if (right == 0U) {
        state_.lo = 0xffffffffU;
        state_.hi = left;
      } else {
        state_.lo = left / right;
        state_.hi = left % right;
      }
      const auto marked = pgxpSourcesMarked(rs, rt);
      const auto *left_pgxp = marked ? pgxpRegister(rs, left) : nullptr;
      const auto *right_pgxp = marked ? pgxpRegister(rt, right) : nullptr;
      if ((left_pgxp != nullptr || right_pgxp != nullptr) && right != 0U) {
        if (right == 1U && left_pgxp != nullptr && right_pgxp == nullptr) {
          auto lo = *left_pgxp;
          lo.raw = state_.lo;
          writePgxpSpecial(pgxp_lo, state_.lo, &lo);
          writePgxpSpecial(pgxp_hi, state_.hi, nullptr);
          break;
        }
        const auto left_value = continuous(left_pgxp, left, true);
        const auto right_value = continuous(right_pgxp, right, true);
        const auto lo = mulDivResult(left_value / right_value, state_.lo,
                                     left_pgxp, right_pgxp, 0x44495655ULL);
        const auto hi =
            mulDivResult(std::fmod(left_value, right_value), state_.hi,
                         left_pgxp, right_pgxp, 0x52454d55ULL);
        writePgxpSpecial(pgxp_lo, state_.lo, &lo);
        writePgxpSpecial(pgxp_hi, state_.hi, &hi);
      } else {
        writePgxpSpecial(pgxp_lo, state_.lo, nullptr);
        writePgxpSpecial(pgxp_hi, state_.hi, nullptr);
      }
      break;
    }
    case 0x20: {
      std::uint32_t value{};
      if (addOverflows(left, right, value)) {
        stop = R3000StopReason::arithmetic_overflow;
      } else {
        const auto pgxp =
            pgxpSourcesMarked(rs, rt)
                ? arithmetic(pgxpRegister(rs, left), left,
                             pgxpRegister(rt, right), right, value, false)
                : PgxpValue{};
        withProjectedHalf(
            screenSourcesMarked(rs, rt),
            [&] {
              return binaryProjectionHalf(rs, left, rt, right, value, false);
            },
            [&](const ProjectedHalf *projected_half) {
              if (exact_tracking_ != nullptr &&
                  (exact_tracking_->gpr_valid_mask &
                   ((1U << rs) | (1U << rt))) != 0U) [[unlikely]] {
                const auto identity_reg =
                    static_cast<std::uint8_t>(rs == 0U   ? rt
                                              : rt == 0U ? rs
                                                         : 0xffU);
                const auto operation = identity_reg < 32U
                                           ? ExactCarrierOperation::identity
                                           : ExactCarrierOperation::add;
                if (tryWriteExactCarrier(
                        rd, value, operation,
                        identity_reg < 32U ? identity_reg : rs,
                        identity_reg < 32U ? state_.gpr[identity_reg] : left,
                        identity_reg < 32U ? 0xffU : rt,
                        identity_reg < 32U ? 0U : right, 0U, nullptr,
                        pgxp.flags != 0U ? &pgxp : nullptr, projected_half)) {
                  return;
                }
              }
              writeRegister(rd, value, nullptr, nullptr,
                            pgxp.flags != 0U ? &pgxp : nullptr,
                            projected_half);
            });
      }
      break;
    }
    case 0x21: {
      const auto value = left + right;
      const auto *projected = rs == 0U   ? screenProjectedRegister(rt, right)
                              : rt == 0U ? screenProjectedRegister(rs, left)
                                         : nullptr;
      const auto pgxp =
          pgxpSourcesMarked(rs, rt)
              ? arithmetic(pgxpRegister(rs, left), left,
                           pgxpRegister(rt, right), right, value, false)
              : PgxpValue{};
      withProjectedHalf(
          screenSourcesMarked(rs, rt),
          [&] {
            return binaryProjectionHalf(rs, left, rt, right, value, false);
          },
          [&](const ProjectedHalf *projected_half) {
            if (exact_tracking_ != nullptr &&
                (exact_tracking_->gpr_valid_mask &
                 ((1U << rs) | (1U << rt))) != 0U) [[unlikely]] {
              const auto identity_reg =
                  static_cast<std::uint8_t>(rs == 0U   ? rt
                                            : rt == 0U ? rs
                                                       : 0xffU);
              const auto operation = identity_reg < 32U
                                         ? ExactCarrierOperation::identity
                                         : ExactCarrierOperation::add;
              if (tryWriteExactCarrier(
                      rd, value, operation,
                      identity_reg < 32U ? identity_reg : rs,
                      identity_reg < 32U ? state_.gpr[identity_reg] : left,
                      identity_reg < 32U ? 0xffU : rt,
                      identity_reg < 32U ? 0U : right, 0U, projected,
                      pgxp.flags != 0U ? &pgxp : nullptr, projected_half)) {
                return;
              }
            }
            writeRegister(rd, value, projected, nullptr,
                          pgxp.flags != 0U ? &pgxp : nullptr, projected_half);
          });
      break;
    }
    case 0x22: {
      std::uint32_t value{};
      if (subtractOverflows(left, right, value)) {
        stop = R3000StopReason::arithmetic_overflow;
      } else {
        const auto pgxp =
            pgxpSourcesMarked(rs, rt)
                ? arithmetic(pgxpRegister(rs, left), left,
                             pgxpRegister(rt, right), right, value, true)
                : PgxpValue{};
        withProjectedHalf(
            screenSourcesMarked(rs, rt),
            [&] {
              return binaryProjectionHalf(rs, left, rt, right, value, true);
            },
            [&](const ProjectedHalf *projected_half) {
              if (exact_tracking_ != nullptr &&
                  (exact_tracking_->gpr_valid_mask &
                   ((1U << rs) | (1U << rt))) != 0U &&
                  tryWriteExactCarrier(
                      rd, value, ExactCarrierOperation::subtract, rs, left, rt,
                      right, 0U, nullptr,
                      pgxp.flags != 0U ? &pgxp : nullptr, projected_half))
                  [[unlikely]] {
                return;
              }
              writeRegister(rd, value, nullptr, nullptr,
                            pgxp.flags != 0U ? &pgxp : nullptr,
                            projected_half);
            });
      }
      break;
    }
    case 0x23: {
      const auto value = left - right;
      const auto pgxp =
          pgxpSourcesMarked(rs, rt)
              ? arithmetic(pgxpRegister(rs, left), left,
                           pgxpRegister(rt, right), right, value, true)
              : PgxpValue{};
      withProjectedHalf(
          screenSourcesMarked(rs, rt),
          [&] {
            return binaryProjectionHalf(rs, left, rt, right, value, true);
          },
          [&](const ProjectedHalf *projected_half) {
            if (exact_tracking_ != nullptr &&
                (exact_tracking_->gpr_valid_mask &
                 ((1U << rs) | (1U << rt))) != 0U) [[unlikely]] {
              const auto operation =
                  rt == 0U ? ExactCarrierOperation::identity
                           : ExactCarrierOperation::subtract;
              if (tryWriteExactCarrier(
                      rd, value, operation, rs, left,
                      rt == 0U ? 0xffU : rt, rt == 0U ? 0U : right, 0U,
                      rt == 0U ? screenProjectedRegister(rs, left) : nullptr,
                      pgxp.flags != 0U ? &pgxp : nullptr, projected_half)) {
                return;
              }
            }
            writeRegister(
                rd, value,
                rt == 0U ? screenProjectedRegister(rs, left) : nullptr, nullptr,
                pgxp.flags != 0U ? &pgxp : nullptr, projected_half);
          });
      break;
    }
    case 0x24: {
      const auto value = left & right;
      const auto pgxp = pgxpSourcesMarked(rs, rt)
                            ? bitwise(pgxpRegister(rs, left), left,
                                      pgxpRegister(rt, right), right, value)
                            : PgxpValue{};
      writeRegister(rd, value, nullptr, nullptr,
                    pgxp.flags != 0U ? &pgxp : nullptr);
      break;
    }
    case 0x25: {
      const auto *projected = rs == 0U   ? screenProjectedRegister(rt, right)
                              : rt == 0U ? screenProjectedRegister(rs, left)
                                         : nullptr;
      const auto value = left | right;
      const auto pgxp = pgxpSourcesMarked(rs, rt)
                            ? bitwise(pgxpRegister(rs, left), left,
                                      pgxpRegister(rt, right), right, value)
                            : PgxpValue{};
      if (exact_tracking_ != nullptr &&
          (exact_tracking_->gpr_valid_mask & ((1U << rs) | (1U << rt))) != 0U)
          [[unlikely]] {
        const auto identity_reg = static_cast<std::uint8_t>(rs == 0U   ? rt
                                                            : rt == 0U ? rs
                                                                       : 0xffU);
        const auto operation = identity_reg < 32U
                                   ? ExactCarrierOperation::identity
                                   : ExactCarrierOperation::or_nonoverlap;
        if (tryWriteExactCarrier(
                rd, value, operation, identity_reg < 32U ? identity_reg : rs,
                identity_reg < 32U ? state_.gpr[identity_reg] : left,
                identity_reg < 32U ? 0xffU : rt,
                identity_reg < 32U ? 0U : right, 0U, projected,
                pgxp.flags != 0U ? &pgxp : nullptr, nullptr)) {
          break;
        }
      }
      writeRegister(rd, value, projected, nullptr,
                    pgxp.flags != 0U ? &pgxp : nullptr);
      break;
    }
    case 0x26: {
      const auto *projected = rs == 0U   ? screenProjectedRegister(rt, right)
                              : rt == 0U ? screenProjectedRegister(rs, left)
                                         : nullptr;
      const auto value = left ^ right;
      const auto pgxp = pgxpSourcesMarked(rs, rt)
                            ? bitwise(pgxpRegister(rs, left), left,
                                      pgxpRegister(rt, right), right, value)
                            : PgxpValue{};
      writeRegister(rd, value, projected, nullptr,
                    pgxp.flags != 0U ? &pgxp : nullptr);
      break;
    }
    case 0x27: {
      const auto value = ~(left | right);
      const auto pgxp = pgxpSourcesMarked(rs, rt)
                            ? bitwise(pgxpRegister(rs, left), left,
                                      pgxpRegister(rt, right), right, value)
                            : PgxpValue{};
      writeRegister(rd, value, nullptr, nullptr,
                    pgxp.flags != 0U ? &pgxp : nullptr);
      break;
    }
    case 0x2a:
      writeRegister(rd, asSigned(left) < asSigned(right) ? 1U : 0U);
      break;
    case 0x2b:
      writeRegister(rd, left < right ? 1U : 0U);
      break;
    default:
      stop = R3000StopReason::unsupported_instruction;
      break;
    }
    break;
  }
  case 0x01: {
    const auto kind = rt;
    const auto greater_equal = (kind & 1U) != 0U;
    const auto link = (kind & 0x1eU) == 0x10U;
    if ((kind & 0x0eU) != 0U && !link) {
      stop = R3000StopReason::unsupported_instruction;
      break;
    }
    if (link) {
      writeRegister(31U, instruction_pc + 8U);
    }
    const auto is_negative = asSigned(left) < 0;
    branch(is_negative != greater_equal);
    break;
  }
  case 0x02:
    state_.branch_pc = instruction_pc;
    state_.branch_delay_slot = true;
    state_.next_pc =
        (state_.pc & 0xf0000000U) | ((instruction & 0x03ffffffU) << 2U);
    break;
  case 0x03:
    writeRegister(31U, instruction_pc + 8U);
    state_.branch_pc = instruction_pc;
    state_.branch_delay_slot = true;
    state_.next_pc =
        (state_.pc & 0xf0000000U) | ((instruction & 0x03ffffffU) << 2U);
    break;
  case 0x04:
    branch(left == right);
    break;
  case 0x05:
    branch(left != right);
    break;
  case 0x06:
    branch(asSigned(left) <= 0);
    break;
  case 0x07:
    branch(asSigned(left) > 0);
    break;
  case 0x08: {
    std::uint32_t value{};
    if (addOverflows(left, signExtend16(immediate), value)) {
      stop = R3000StopReason::arithmetic_overflow;
    } else {
      const auto immediate_value = signExtend16(immediate);
      const auto pgxp = pgxpSourceMarked(rs)
                            ? arithmetic(pgxpRegister(rs, left), left, nullptr,
                                         immediate_value, value, false)
                            : PgxpValue{};
      withProjectedHalf(
          screenSourceMarked(rs),
          [&] {
            return offsetProjectionHalf(
                rs, left, value, static_cast<std::int32_t>(immediate_value));
          },
          [&](const ProjectedHalf *projected_half) {
            if (exact_tracking_ != nullptr &&
                (exact_tracking_->gpr_valid_mask & (1U << rs)) != 0U)
                [[unlikely]] {
              const auto operation = immediate_value == 0U
                                         ? ExactCarrierOperation::identity
                                         : ExactCarrierOperation::add;
              if (tryWriteExactCarrier(
                      rt, value, operation, rs, left, 0xffU, immediate_value,
                      0U, nullptr, pgxp.flags != 0U ? &pgxp : nullptr,
                      projected_half)) {
                return;
              }
            }
            writeRegister(rt, value, nullptr, nullptr,
                          pgxp.flags != 0U ? &pgxp : nullptr, projected_half);
          });
    }
    break;
  }
  case 0x09: {
    const auto immediate_value = signExtend16(immediate);
    const auto value = left + immediate_value;
    const auto pgxp = pgxpSourceMarked(rs)
                          ? arithmetic(pgxpRegister(rs, left), left, nullptr,
                                       immediate_value, value, false)
                          : PgxpValue{};
    withProjectedHalf(
        screenSourceMarked(rs),
        [&] {
          return offsetProjectionHalf(
              rs, left, value, static_cast<std::int32_t>(immediate_value));
        },
        [&](const ProjectedHalf *projected_half) {
          if (exact_tracking_ != nullptr &&
              (exact_tracking_->gpr_valid_mask & (1U << rs)) != 0U)
              [[unlikely]] {
            const auto operation = immediate_value == 0U
                                       ? ExactCarrierOperation::identity
                                       : ExactCarrierOperation::add;
            if (tryWriteExactCarrier(
                    rt, value, operation, rs, left, 0xffU, immediate_value, 0U,
                    immediate == 0U ? screenProjectedRegister(rs, left)
                                    : nullptr,
                    pgxp.flags != 0U ? &pgxp : nullptr, projected_half)) {
              return;
            }
          }
          writeRegister(rt, value,
                        immediate == 0U ? screenProjectedRegister(rs, left)
                                        : nullptr,
                        nullptr, pgxp.flags != 0U ? &pgxp : nullptr,
                        projected_half);
        });
    break;
  }
  case 0x0a:
    writeRegister(rt,
                  asSigned(left) < asSigned(signExtend16(immediate)) ? 1U : 0U);
    break;
  case 0x0b:
    writeRegister(rt, left < signExtend16(immediate) ? 1U : 0U);
    break;
  case 0x0c: {
    const auto value = left & immediate;
    const auto pgxp =
        pgxpSourceMarked(rs)
            ? bitwise(pgxpRegister(rs, left), left, nullptr, immediate, value)
            : PgxpValue{};
    withProjectedHalf(
        screenSourceMarked(rs),
        [&] { return offsetProjectionHalf(rs, left, value, 0); },
        [&](const ProjectedHalf *projected_half) {
          if (exact_tracking_ != nullptr && immediate == 0xffffU &&
              (exact_tracking_->gpr_valid_mask & (1U << rs)) != 0U &&
              tryWriteExactCarrier(
                  rt, value, ExactCarrierOperation::and_low_16, rs, left, 0xffU,
                  immediate, 0U, nullptr, pgxp.flags != 0U ? &pgxp : nullptr,
                  projected_half)) [[unlikely]] {
            return;
          }
          writeRegister(rt, value, nullptr, nullptr,
                        pgxp.flags != 0U ? &pgxp : nullptr, projected_half);
        });
    break;
  }
  case 0x0d: {
    const auto value = left | immediate;
    const auto pgxp =
        pgxpSourceMarked(rs)
            ? bitwise(pgxpRegister(rs, left), left, nullptr, immediate, value)
            : PgxpValue{};
    withProjectedHalf(
        screenSourceMarked(rs),
        [&] { return offsetProjectionHalf(rs, left, value, 0); },
        [&](const ProjectedHalf *projected_half) {
          if (exact_tracking_ != nullptr &&
              (exact_tracking_->gpr_valid_mask & (1U << rs)) != 0U)
              [[unlikely]] {
            const auto operation = immediate == 0U
                                       ? ExactCarrierOperation::identity
                                       : ExactCarrierOperation::or_nonoverlap;
            if (tryWriteExactCarrier(
                    rt, value, operation, rs, left, 0xffU, immediate, 0U,
                    immediate == 0U ? screenProjectedRegister(rs, left)
                                    : nullptr,
                    pgxp.flags != 0U ? &pgxp : nullptr, projected_half)) {
              return;
            }
          }
          writeRegister(rt, value,
                        immediate == 0U ? screenProjectedRegister(rs, left)
                                        : nullptr,
                        nullptr, pgxp.flags != 0U ? &pgxp : nullptr,
                        projected_half);
        });
  } break;
  case 0x0e: {
    const auto value = left ^ immediate;
    const auto pgxp =
        pgxpSourceMarked(rs)
            ? bitwise(pgxpRegister(rs, left), left, nullptr, immediate, value)
            : PgxpValue{};
    writeRegisterWithProjectedHalf(
        rt, value,
        immediate == 0U ? screenProjectedRegister(rs, left) : nullptr, nullptr,
        pgxp.flags != 0U ? &pgxp : nullptr, screenSourceMarked(rs), [&] {
          return offsetProjectionHalf(rs, left, value, 0);
        });
  } break;
  case 0x0f:
    writeRegister(rt, immediate << 16U);
    break;
  case 0x10: {
    if (instruction == 0x42000010U) {
      state_.cop0_status =
          (state_.cop0_status & ~0x0fU) | ((state_.cop0_status >> 2U) & 0x0fU);
      break;
    }
    if ((instruction & 0x7ffU) != 0U) {
      stop = R3000StopReason::unsupported_instruction;
      break;
    }
    if (rs == 0U) {
      std::uint32_t value{};
      switch (rd) {
      case 8U:
        value = state_.cop0_bad_vaddr;
        break;
      case 12U:
        value = state_.cop0_status;
        break;
      case 13U:
        value = state_.cop0_cause;
        break;
      case 14U:
        value = state_.cop0_epc;
        break;
      default:
        stop = R3000StopReason::unsupported_instruction;
        break;
      }
      if (stop == R3000StopReason::running) {
        scheduleLoad(rt, value);
      }
    } else if (rs == 4U) {
      switch (rd) {
      case 12U:
        state_.cop0_status = right;
        break;
      case 13U:
        state_.cop0_cause =
            (state_.cop0_cause & ~0x00000300U) | (right & 0x00000300U);
        break;
      case 14U:
        state_.cop0_epc = right;
        break;
      default:
        stop = R3000StopReason::unsupported_instruction;
        break;
      }
    } else {
      stop = R3000StopReason::unsupported_instruction;
    }
    break;
  }
  case 0x12:
    if (rs == 0U) {
      const auto value = GteRuntime::readData(state_.gte, rd);
      const auto *projected =
          pgxp_transform_tracking_ || gpu_projection_catalog_tracking_
              ? GteRuntime::projectedVertex(state_.gte, rd)
              : nullptr;
      GteExactWord exact{};
      const GteExactWord *exact_ptr = nullptr;
      if (exactTransformCarrierTracking()) {
        exact = GteRuntime::exactData(state_.gte, exact_tracking_->gte, rd);
        exact_ptr = exact.raw == value ? &exact : nullptr;
      }
      scheduleLoad(rt, value,
                   projected != nullptr && projected->packed_sxy == value
                       ? projected
                       : nullptr,
                   nullptr, exact_ptr);
    } else if (rs == 2U) {
      scheduleLoad(rt, GteRuntime::readControl(state_.gte, rd));
    } else if (rs == 4U) {
      GteRuntime::writeData(
          state_.gte, rd, right,
          (pgxp_transform_tracking_ || gpu_projection_catalog_tracking_)
              ? screenProjectedRegister(rt, right) : nullptr,
          pgxp_transform_tracking_ ? &exact_tracking_->gte : nullptr,
          pgxp_transform_tracking_ ? exactRegister(rt, right) : nullptr);
    } else if (rs == 6U) {
      GteRuntime::writeControl(
          state_.gte, rd, right,
          pgxp_transform_tracking_ ? &exact_tracking_->gte : nullptr,
          pgxp_transform_tracking_ ? exactRegister(rt, right) : nullptr);
    } else if ((rs & 0x10U) != 0U) {
      const auto gte_command = instruction & 0x3fU;
      const auto use_psycross_projection =
          static_cast<bool>(gte_projection_backend_) &&
          !pgxp_exact_transform_tracking_ &&
          !pgxp_preserve_projection_precision_ &&
          (gte_command == 0x01U || gte_command == 0x30U);
      const auto executed =
          use_psycross_projection
              ? gte_projection_backend_.execute(
                    gte_projection_backend_.context, state_.gte, instruction,
                    gpu_projection_catalog_tracking_)
              : GteRuntime::executeCommand(
                    state_.gte, instruction,
                    pgxp_transform_tracking_ ? &exact_tracking_->gte : nullptr,
                    gpu_projection_catalog_tracking_,
                    pgxp_preserve_projection_precision_);
      if (!executed) {
        stop = R3000StopReason::unsupported_instruction;
      } else if (gpu_projection_catalog_tracking_) {
        if (gte_command == 0x01U) {
          recordGpuProjection(state_.gte.projected[2]);
        } else if (gte_command == 0x30U) {
          recordGpuProjection(state_.gte.projected[0]);
          recordGpuProjection(state_.gte.projected[1]);
          recordGpuProjection(state_.gte.projected[2]);
        }
      }
    } else {
      stop = R3000StopReason::unsupported_instruction;
    }
    break;
  case 0x20:
  case 0x24: {
    std::uint8_t value{};
    if (!read8(memoryAddress(), value)) {
      stop = R3000StopReason::memory_fault;
    } else if (opcode == 0x20) {
      scheduleLoad(rt, static_cast<std::uint32_t>(static_cast<std::int32_t>(
                           static_cast<std::int8_t>(value))));
    } else {
      scheduleLoad(rt, value);
    }
    break;
  }
  case 0x21:
  case 0x25: {
    const auto address = memoryAddress();
    if ((address & 1U) != 0U) {
      stop = R3000StopReason::alignment_fault;
      break;
    }
    std::uint16_t value{};
    if (!read16(address, value)) {
      stop = R3000StopReason::memory_fault;
    } else {
      const auto register_value =
          opcode == 0x21 ? static_cast<std::uint32_t>(static_cast<std::int32_t>(
                               static_cast<std::int16_t>(value)))
                         : static_cast<std::uint32_t>(value);
      const auto exact_half_tracking = exactTransformCarrierTracking();
      const auto pgxp_half_tracking =
          pgxp_transform_tracking_ && pgxp_cpu_tracking_;
      std::uint32_t packed{};
      const auto have_packed =
          (pgxp_transform_tracking_ || gpu_projection_catalog_tracking_) &&
          read32(address & ~3U, packed);
      const auto projected_half =
          have_packed ? projectedHalfAt(address, value, register_value, packed)
                      : ProjectedHalf{};
      GteExactWord exact_half{};
      const GteExactWord *exact_half_ptr = nullptr;
      if (exact_half_tracking && have_packed) {
        if (const auto *word = exactWordAt(address & ~3U, packed);
            word != nullptr) {
          const auto slot = static_cast<std::uint8_t>((address >> 1U) & 1U);
          if (word->halves[slot].valid) {
            exact_half.raw = register_value;
            exact_half.scalar = word->halves[slot];
            exact_half.halves[0] = word->halves[slot];
            exact_half_ptr = &exact_half;
          }
        }
      }
      PgxpValue pgxp_half{};
      if (pgxp_half_tracking && have_packed) {
        if (const auto *word = pgxpWordAt(address & ~3U, packed);
            word != nullptr) {
          const auto high = (address & 2U) != 0U;
          const auto component_valid =
              word->has(high ? PgxpValue::valid_y : PgxpValue::valid_x);
          if (component_valid) {
            pgxp_half.context_handle = word->context_handle;
            pgxp_half.x = high ? word->y : word->x;
            pgxp_half.y = opcode == 0x21 && static_cast<std::int16_t>(value) < 0
                              ? -1.0
                              : 0.0;
            pgxp_half.z = word->z;
            pgxp_half.raw = register_value;
            const auto lineage = high ? word->lineage_y : word->lineage_x;
            pgxp_half.lineage_x = lineage;
            pgxp_half.lineage_y = lineage;
            const auto component_derived =
                high ? word->derived_y : word->derived_x;
            pgxp_half.derived_x = component_derived;
            pgxp_half.derived_y = true;
            pgxp_half.source_vertex_id =
                !component_derived ? word->source_vertex_id : 0U;
            pgxp_half.flags = PgxpValue::valid_x | PgxpValue::valid_y;
            if (word->has(PgxpValue::valid_z))
              pgxp_half.flags |= PgxpValue::valid_z | PgxpValue::z_from_low |
                                 PgxpValue::z_from_high;
            if (word->has(PgxpValue::tainted_z))
              pgxp_half.flags |= PgxpValue::tainted_z;
          }
        }
      }
      scheduleLoad(rt, register_value, nullptr,
                   projected_half.valid ? &projected_half : nullptr,
                   exact_half_ptr,
                   pgxp_half.flags != 0U ? &pgxp_half : nullptr);
    }
    break;
  }
  case 0x22:
  case 0x26: {
    const auto address = memoryAddress();
    std::uint32_t aligned_value{};
    if (!loadWord(address & ~3U, aligned_value)) {
      break;
    }
    const auto existing = state_.load_delay.valid && state_.load_delay.reg == rt
                              ? state_.load_delay.value
                              : right;
    const auto amount = (address & 3U) * 8U;
    std::uint32_t value{};
    if (opcode == 0x22) {
      const auto mask = 0x00ffffffU >> amount;
      value = (existing & mask) | (aligned_value << (24U - amount));
    } else {
      const auto mask = 0xffffff00U << (24U - amount);
      value = (existing & mask) | (aligned_value >> amount);
    }
    const auto byte = address & 3U;
    const auto complete_word =
        (opcode == 0x22U && byte == 3U) || (opcode == 0x26U && byte == 0U);
    const GteProjectedVertex *compact_projection{};
    if (complete_word && gpu_projection_catalog_tracking_) {
      const auto *candidate =
          projectedVertexProvenanceAt(address & ~3U, aligned_value)
              .projected;
      if (candidate != nullptr && candidate->packed_sxy == value &&
          compactProjectionHandle(candidate, value) != 0U) {
        compact_projection = candidate;
      }
    }
    PgxpValue merged{};
    if (pgxp_transform_tracking_) {
      const auto *memory_pgxp = pgxpWordAt(address & ~3U, aligned_value);
      const auto *register_pgxp = pgxpRegister(rt, existing);
      if (opcode == 0x22U) {
        if (byte == 3U && memory_pgxp != nullptr) {
          merged = *memory_pgxp;
        } else if (byte == 1U && memory_pgxp != nullptr) {
          if (register_pgxp != nullptr) {
            merged = *register_pgxp;
            if (!sameProjectionContext(merged, *memory_pgxp)) {
              merged.flags &= ~(PgxpValue::valid_x | PgxpValue::valid_z |
                                PgxpValue::z_from_low | PgxpValue::z_from_high);
              merged.lineage_x = 0U;
              merged.derived_x = false;
              merged.source_vertex_id = 0U;
            }
          }
          merged.raw = value;
          merged.y = memory_pgxp->x;
          merged.lineage_y = memory_pgxp->lineage_x;
          merged.derived_y = memory_pgxp->derived_x;
          merged.context_handle = memory_pgxp->context_handle;
          merged.z = memory_pgxp->z;
          merged.flags = static_cast<std::uint8_t>(
              (merged.flags & ~PgxpValue::valid_y) |
              (memory_pgxp->has(PgxpValue::valid_x) ? PgxpValue::valid_y : 0U));
          merged.source_vertex_id =
              register_pgxp != nullptr && memory_pgxp->source_vertex_id != 0U &&
                      register_pgxp->source_vertex_id ==
                          memory_pgxp->source_vertex_id &&
                      !register_pgxp->derived_x && !memory_pgxp->derived_x
                  ? memory_pgxp->source_vertex_id
                  : 0U;
          if (memory_pgxp->has(PgxpValue::valid_z))
            merged.flags |= PgxpValue::valid_z | PgxpValue::z_from_high;
        }
      } else {
        if (byte == 0U && memory_pgxp != nullptr) {
          merged = *memory_pgxp;
        } else if (byte == 2U && memory_pgxp != nullptr) {
          if (register_pgxp != nullptr) {
            merged = *register_pgxp;
            if (!sameProjectionContext(merged, *memory_pgxp)) {
              merged.flags &= ~(PgxpValue::valid_y | PgxpValue::valid_z |
                                PgxpValue::z_from_low | PgxpValue::z_from_high);
              merged.lineage_y = 0U;
              merged.derived_y = false;
              merged.source_vertex_id = 0U;
            }
          }
          merged.raw = value;
          merged.x = memory_pgxp->y;
          merged.lineage_x = memory_pgxp->lineage_y;
          merged.derived_x = memory_pgxp->derived_y;
          merged.context_handle = memory_pgxp->context_handle;
          merged.z = memory_pgxp->z;
          merged.flags = static_cast<std::uint8_t>(
              (merged.flags & ~PgxpValue::valid_x) |
              (memory_pgxp->has(PgxpValue::valid_y) ? PgxpValue::valid_x : 0U));
          merged.source_vertex_id =
              register_pgxp != nullptr && memory_pgxp->source_vertex_id != 0U &&
                      register_pgxp->source_vertex_id ==
                          memory_pgxp->source_vertex_id &&
                      !register_pgxp->derived_y && !memory_pgxp->derived_y
                  ? memory_pgxp->source_vertex_id
                  : 0U;
          if (memory_pgxp->has(PgxpValue::valid_z))
            merged.flags |= PgxpValue::valid_z | PgxpValue::z_from_low;
        }
      }
    }
    scheduleLoad(rt, value, compact_projection, nullptr, nullptr,
                 merged.flags != 0U ? &merged : nullptr);
    break;
  }
  case 0x23: {
    const auto address = memoryAddress();
    std::uint32_t value{};
    if (loadWord(address, value)) {
      const auto *projected =
          projectedVertexProvenanceAt(address, value).projected;
      scheduleLoad(rt, value, projected, nullptr, exactWordAt(address, value),
                   pgxpWordAt(address, value));
    }
    break;
  }
  case 0x28: {
    const auto address = memoryAddress();
    if (!write8(address, static_cast<std::uint8_t>(right))) {
      stop = R3000StopReason::memory_fault;
    } else {
    }
    break;
  }
  case 0x29: {
    const auto address = memoryAddress();
    auto projected_half = screenSourceMarked(rt)
                              ? projectionHalf(rt, right, 0U) : ProjectedHalf{};
    const auto destination_slot =
        static_cast<std::uint8_t>((address >> 1U) & 1U);
    if (!projected_half.valid || projected_half.slot != destination_slot) {
      projected_half = {};
    }
    const auto *projected_half_ptr =
        projected_half.valid ? &projected_half : nullptr;
    if ((address & 1U) != 0U) {
      stop = R3000StopReason::alignment_fault;
    } else if (!write16Projected(address, static_cast<std::uint16_t>(right),
                                 projected_half_ptr, exactRegister(rt, right),
                                 pgxpRegister(rt, right))) {
      stop = R3000StopReason::memory_fault;
    } else {
    }
    break;
  }
  case 0x2a:
  case 0x2e: {
    const auto address = memoryAddress();
    const auto aligned_address = address & ~3U;
    std::uint32_t memory_value{};
    if (!loadWord(aligned_address, memory_value)) {
      break;
    }
    const auto destination_pgxp = pgxpWordAt(aligned_address, memory_value);
    const auto source_pgxp = pgxpRegister(rt, right);
    const auto byte = address & 3U;
    const auto complete_word =
        (opcode == 0x2aU && byte == 3U) || (opcode == 0x2eU && byte == 0U);
    const GteProjectedVertex *compact_projection{};
    if (complete_word && gpu_projection_catalog_tracking_) {
      const auto *candidate = screenProjectedRegister(rt, right);
      if (candidate != nullptr && candidate->packed_sxy == right &&
          compactProjectionHandle(candidate, right) != 0U) {
        compact_projection = candidate;
      }
    }
    const auto amount = (address & 3U) * 8U;
    std::uint32_t value{};
    if (opcode == 0x2a) {
      const auto mask = 0xffffff00U << amount;
      value = (memory_value & mask) | (right >> (24U - amount));
    } else {
      const auto mask = 0x00ffffffU >> (24U - amount);
      value = (memory_value & mask) | (right << amount);
    }
    if (storeWord(aligned_address, value, nullptr)) {
      PgxpValue merged{};
      if (opcode == 0x2aU) {
        if (byte == 3U && source_pgxp != nullptr) {
          merged = *source_pgxp;
        } else if (byte == 1U && source_pgxp != nullptr) {
          if (destination_pgxp != nullptr) {
            merged = *destination_pgxp;
            if (!sameProjectionContext(merged, *source_pgxp)) {
              merged.flags &= ~(PgxpValue::valid_y | PgxpValue::valid_z |
                                PgxpValue::z_from_low | PgxpValue::z_from_high);
              merged.lineage_y = 0U;
              merged.derived_y = false;
              merged.source_vertex_id = 0U;
            }
          }
          merged.raw = value;
          merged.x = source_pgxp->y;
          merged.lineage_x = source_pgxp->lineage_y;
          merged.derived_x = source_pgxp->derived_y;
          merged.context_handle = source_pgxp->context_handle;
          merged.z = source_pgxp->z;
          merged.flags = static_cast<std::uint8_t>(
              (merged.flags & ~PgxpValue::valid_x) |
              (source_pgxp->has(PgxpValue::valid_y) ? PgxpValue::valid_x : 0U));
          merged.source_vertex_id =
              destination_pgxp != nullptr &&
                      source_pgxp->source_vertex_id != 0U &&
                      destination_pgxp->source_vertex_id ==
                          source_pgxp->source_vertex_id &&
                      !destination_pgxp->derived_y && !source_pgxp->derived_y
                  ? source_pgxp->source_vertex_id
                  : 0U;
          if (source_pgxp->has(PgxpValue::valid_z))
            merged.flags |= PgxpValue::valid_z | PgxpValue::z_from_low;
        }
      } else {
        if (byte == 0U && source_pgxp != nullptr) {
          merged = *source_pgxp;
        } else if (byte == 2U && source_pgxp != nullptr) {
          if (destination_pgxp != nullptr) {
            merged = *destination_pgxp;
            if (!sameProjectionContext(merged, *source_pgxp)) {
              merged.flags &= ~(PgxpValue::valid_x | PgxpValue::valid_z |
                                PgxpValue::z_from_low | PgxpValue::z_from_high);
              merged.lineage_x = 0U;
              merged.derived_x = false;
              merged.source_vertex_id = 0U;
            }
          }
          merged.raw = value;
          merged.y = source_pgxp->x;
          merged.lineage_y = source_pgxp->lineage_x;
          merged.derived_y = source_pgxp->derived_x;
          merged.context_handle = source_pgxp->context_handle;
          merged.z = source_pgxp->z;
          merged.flags = static_cast<std::uint8_t>(
              (merged.flags & ~PgxpValue::valid_y) |
              (source_pgxp->has(PgxpValue::valid_x) ? PgxpValue::valid_y : 0U));
          merged.source_vertex_id =
              destination_pgxp != nullptr &&
                      source_pgxp->source_vertex_id != 0U &&
                      destination_pgxp->source_vertex_id ==
                          source_pgxp->source_vertex_id &&
                      !destination_pgxp->derived_x && !source_pgxp->derived_x
                  ? source_pgxp->source_vertex_id
                  : 0U;
          if (source_pgxp->has(PgxpValue::valid_z))
            merged.flags |= PgxpValue::valid_z | PgxpValue::z_from_high;
        }
      }
      storePgxpWord(aligned_address, value,
                    merged.flags != 0U ? &merged : nullptr);
      if (compact_projection != nullptr &&
          compact_projection->packed_sxy == value) {
        storeProjectedVertex(aligned_address, *compact_projection);
      }
    }
    break;
  }
  case 0x2b: {
    const auto address = memoryAddress();
    const GteProjectedVertex *projected{};
    if (pgxp_cpu_tracking_) {
      // Preserve only the untouched GTE tuple. projectedRegister() may
      // synthesize a derived CPU-PGXP value, which must retain its component
      // lineage instead of being republished as a direct projection.
      if ((projected_gpr_valid_mask_ & (1U << rt)) != 0U) {
        const auto &candidate = projected_gpr_[rt];
        if (candidate.valid && candidate.packed_sxy == right)
          projected = &candidate;
      }
    } else {
      projected = screenProjectedRegister(rt, right);
    }
    const auto *exact = exactRegister(rt, right);
    if (storeWord(address, right, projected)) {
      if (projected != nullptr)
        storeProjectedVertex(address, *projected);
      if (exact != nullptr)
        storeExactWord(address, right, exact);
      if (projected == nullptr)
        storePgxpWord(address, right, pgxpRegister(rt, right));
    }
    break;
  }
  case 0x32: {
    std::uint32_t value{};
    const auto address = memoryAddress();
    if (loadWord(address, value)) {
      GteRuntime::writeData(
          state_.gte, rt, value,
          pgxp_transform_tracking_ || gpu_projection_catalog_tracking_
              ? projectedVertexProvenanceAt(address, value).projected
              : nullptr,
          pgxp_transform_tracking_ ? &exact_tracking_->gte : nullptr,
          exactWordAt(address, value));
    }
    break;
  }
  case 0x3a: {
    const auto address = memoryAddress();
    const auto value = GteRuntime::readData(state_.gte, rt);
    const auto *projected =
        pgxp_transform_tracking_ || gpu_projection_catalog_tracking_
            ? GteRuntime::projectedVertex(state_.gte, rt)
            : nullptr;
    if (storeWord(address, value, nullptr)) {
      if (projected != nullptr && projected->packed_sxy == value)
        storeProjectedVertex(address, *projected);
      if (exactTransformCarrierTracking()) {
        const auto exact =
            GteRuntime::exactData(state_.gte, exact_tracking_->gte, rt);
        storeExactWord(address, value, exact.raw == value ? &exact : nullptr);
      }
    }
    break;
  }
  default:
    stop = R3000StopReason::unsupported_instruction;
    break;
  }

  if (stop != R3000StopReason::running) {
    state_.next_load_delay = {};
    gpu_projection_handle_next_load_delay_ = 0U;
    projected_next_load_delay_ = {};
    if (pgxp_transform_tracking_) {
      projected_half_next_load_delay_ = {};
      exact_tracking_->next_load_delay = {};
    }
    advanceLoadDelay();
    return {stop, 0U, instruction_pc, instruction};
  }
  advanceLoadDelay();
  return {R3000StopReason::running, 1U, instruction_pc, instruction};
}

R3000RunResult R3000Runtime::call(std::uint32_t address,
                                  std::span<const std::uint32_t> arguments,
                                  std::uint64_t instruction_budget) noexcept {
  if (!beginCall(address, arguments)) {
    return {R3000StopReason::memory_fault, 0U, address, 0U};
  }

  for (std::uint64_t count = 0; count < instruction_budget; ++count) {
    if (atReturnSentinel()) {
      settleLoadDelay();
      return {R3000StopReason::returned, count, state_.pc, 0U};
    }
    auto result = step();
    if (result.reason != R3000StopReason::running) {
      result.instructions = count;
      return result;
    }
  }
  if (atReturnSentinel()) {
    settleLoadDelay();
    return {
        R3000StopReason::returned,
        instruction_budget,
        state_.pc,
        0U,
    };
  }
  return {R3000StopReason::instruction_budget, instruction_budget, state_.pc,
          0U};
}

} // namespace sf::psx
