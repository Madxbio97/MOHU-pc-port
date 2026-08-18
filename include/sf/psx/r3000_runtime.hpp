#pragma once

#include "sf/psx/executable.hpp"
#include "sf/psx/gte_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace sf::psx {

enum class R3000StopReason {
  running,
  returned,
  instruction_budget,
  unsupported_instruction,
  memory_fault,
  alignment_fault,
  arithmetic_overflow,
  syscall,
  breakpoint,
};

[[nodiscard]] std::string_view toString(R3000StopReason reason) noexcept;

struct R3000RunResult {
  R3000StopReason reason{R3000StopReason::running};
  std::uint64_t instructions{};
  std::uint32_t pc{};
  std::uint32_t instruction{};
};

enum class R3000AccessWidth : std::uint8_t {
  byte = 1U,
  halfword = 2U,
  word = 4U,
};

// Width-aware MMIO boundary. A false return leaves the address to the
// interpreter's passive compatibility shadow.
class R3000MmioBus {
public:
  virtual ~R3000MmioBus() = default;
  [[nodiscard]] virtual bool readMmio(std::uint32_t physical_address,
                                      R3000AccessWidth width,
                                      std::uint32_t &value) noexcept = 0;
  // True only for read-only status registers whose observable state changes
  // exclusively at machine scheduler boundaries.
  [[nodiscard]] virtual bool idleSafeReadMmio(std::uint32_t,
                                              R3000AccessWidth) const noexcept {
    return false;
  }
  [[nodiscard]] virtual bool
  writeMmio(std::uint32_t physical_address, R3000AccessWidth width,
            std::uint32_t value, const GteProjectedVertex *projected = nullptr,
            std::uint64_t projection_identity = 0U,
            std::uint32_t producer_pc = 0xffffffffU) noexcept = 0;
};

struct R3000DelayedLoadState {
  std::uint8_t reg{};
  std::uint32_t value{};
  bool valid{};
};

struct R3000State {
  std::array<std::uint32_t, 32> gpr{};
  GteState gte{};
  std::uint32_t cop0_status{};
  std::uint32_t cop0_cause{};
  std::uint32_t cop0_epc{};
  std::uint32_t cop0_bad_vaddr{};
  std::uint32_t hi{};
  std::uint32_t lo{};
  std::uint32_t pc{};
  std::uint32_t next_pc{};
  std::uint32_t branch_pc{};
  bool branch_delay_slot{};
  R3000DelayedLoadState load_delay{};
  R3000DelayedLoadState next_load_delay{};
};

struct R3000IdleLoopSnapshot {
  R3000State state{};
  std::uint64_t safety_epoch{};
  bool eligible{};
};

struct R3000CachedInstruction {
  std::uint32_t raw{};
  std::uint32_t source_mask{};
  std::uint8_t operation{};
  bool synchronization_boundary{};
  bool carrier_mask_required{};
};

struct R3000CachedBlockView {
  std::span<const R3000CachedInstruction> instructions{};
  std::uint32_t start_pc{};
  std::uint64_t cache_epoch{};
};

struct R3000PgxpTransformCheckpoint {
  GteState gte_witness{};
  GteExactState exact{};
  std::uint32_t generation{};
  bool valid{};
};

struct ProjectedVertexProvenance {
  const GteProjectedVertex *projected{};
  std::uint64_t identity{};
};

// Deterministic interpreter for the user-code portion of the original R3000A.
// Hardware effects are supplied by an optional width-aware machine bus. Any
// unclaimed MMIO byte remains available through the compatibility shadow.
class R3000Runtime final {
public:
  static constexpr std::size_t ram_size = 2U * 1024U * 1024U;
  static constexpr std::size_t scratchpad_size = 1024U;
  static constexpr std::size_t mmio_size = 4U * 1024U;
  static constexpr std::uint32_t return_sentinel = 0xfffffff0U;

  R3000Runtime();

  void clearMemory() noexcept;
  void loadExecutable(const Executable &executable);
  [[nodiscard]] bool loadBytes(std::uint32_t address,
                               std::span<const std::byte> bytes) noexcept;
  [[nodiscard]] bool copyBytes(std::uint32_t address,
                               std::span<std::byte> destination) const noexcept;
  [[nodiscard]] bool restoreRam(std::span<const std::byte> bytes) noexcept;
  [[nodiscard]] bool
  restoreScratchpad(std::span<const std::byte> bytes) noexcept;
  [[nodiscard]] bool restoreMmio(std::span<const std::byte> bytes) noexcept;

  void reset(std::uint32_t pc, std::uint32_t gp = 0U,
             std::uint32_t sp = 0U) noexcept;
  void restoreCpuState(const R3000State &state) noexcept;
  [[nodiscard]] R3000PgxpTransformCheckpoint
  capturePgxpTransformCheckpoint() const noexcept;
  void restoreCpuState(const R3000State &state,
                       const R3000PgxpTransformCheckpoint &checkpoint) noexcept;
  [[nodiscard]] bool
  beginCall(std::uint32_t address,
            std::span<const std::uint32_t> arguments = {}) noexcept;
  void completeHostCall() noexcept;
  void settleLoadDelay() noexcept;
  void setRegister(std::uint8_t reg, std::uint32_t value) noexcept;
  [[nodiscard]] bool setPgxpTransformTracking(bool enabled) noexcept;
  void setPgxpCpuTracking(bool enabled) noexcept;
  void setPgxpExactTransformTracking(bool enabled) noexcept;
  void setPgxpExactTransformCaptureEnabled(bool enabled) noexcept;
  void setPgxpVertexIdentityTracking(bool enabled) noexcept;
  void setPgxpPreserveProjectionPrecision(bool enabled) noexcept {
    pgxp_preserve_projection_precision_ = enabled;
  }
  void beginPgxpTransformGeneration() noexcept;
  void
  setGteProjectionCommandBackend(GteProjectionCommandBackend backend) noexcept {
    gte_projection_backend_ = backend;
  }
  [[nodiscard]] bool setGpuProjectionCatalogTracking(bool enabled) noexcept;
  void beginGpuProjectionFrame() noexcept;
  [[nodiscard]] bool gpuProjectionCatalogTracking() const noexcept {
    return gpu_projection_catalog_tracking_;
  }
  [[nodiscard]] std::span<const GteProjectedVertex>
  gpuProjectionCatalog() const noexcept {
    if (gpu_projection_catalog_ == nullptr) {
      return {};
    }
    return {gpu_projection_catalog_.get(), gpu_projection_catalog_size_};
  }
  [[nodiscard]] bool gpuProjectionCatalogOverflowed() const noexcept {
    return gpu_projection_catalog_overflow_;
  }
  [[nodiscard]] std::uint16_t
  publishGpuProjectionHandle(GteProjectedVertex projection) noexcept;
  [[nodiscard]] bool
  publishExactTransform(std::uint32_t address,
                        const std::array<double, 9U> &rotation,
                        const std::array<double, 3U> &translation) noexcept;
  [[nodiscard]] bool
  captureExactTransform(std::uint32_t address, std::array<double, 9U> &rotation,
                        std::array<double, 3U> &translation) const noexcept;
  [[nodiscard]] bool captureTransformForComposition(
      std::uint32_t address, std::array<double, 9U> &rotation,
      std::array<double, 3U> &translation, bool &enhanced) const noexcept;
  [[nodiscard]] bool
  publishComposedExactTransform(const std::array<double, 9U> &lhs_rotation,
                                const std::array<double, 3U> &lhs_translation,
                                const std::array<double, 9U> &rhs_rotation,
                                const std::array<double, 3U> &rhs_translation,
                                std::uint32_t output_address,
                                bool compose_translation = true) noexcept;

  [[nodiscard]] bool pgxpTransformTracking() const noexcept {
    return pgxp_transform_tracking_;
  }
  [[nodiscard]] bool pgxpCpuTracking() const noexcept {
    return pgxp_cpu_tracking_;
  }
  [[nodiscard]] bool pgxpExactTransformTracking() const noexcept {
    return pgxp_exact_transform_tracking_;
  }
  [[nodiscard]] bool pgxpVertexIdentityTracking() const noexcept {
    return pgxp_vertex_identity_tracking_;
  }
  [[nodiscard]] bool pgxpPreserveProjectionPrecision() const noexcept {
    return pgxp_preserve_projection_precision_;
  }
  [[nodiscard]] std::size_t pgxpTransformStorageCapacity() const noexcept {
    return pgxp_memory_pages_.empty() ? 0U : tracked_key_count;
  }
  void attachMmioBus(R3000MmioBus *bus) noexcept { mmio_bus_ = bus; }
  void setExternalInterrupt(bool active) noexcept;

  [[nodiscard]] bool interruptPending() const noexcept;

  [[nodiscard]] R3000IdleLoopSnapshot captureIdleLoopSnapshot() const noexcept;
  [[nodiscard]] bool
  matchesIdleLoopSnapshot(const R3000IdleLoopSnapshot &snapshot) const noexcept;

  [[nodiscard]] bool atReturnSentinel() const noexcept {
    return state_.pc == return_sentinel;
  }

  [[nodiscard]] R3000RunResult step() noexcept;
  [[nodiscard]] R3000RunResult
  stepCachedInstruction(const R3000CachedInstruction &instruction) noexcept;
  [[nodiscard]] R3000CachedBlockView cachedBlock() noexcept;
  void setExecutionBreakpoint(std::uint32_t pc, bool enabled) noexcept;
  [[nodiscard]] bool executionBreakpoint(std::uint32_t pc) const noexcept;
  [[nodiscard]] std::uint64_t codeCacheEpoch() const noexcept {
    return code_cache_epoch_;
  }
  [[nodiscard]] std::uint64_t cachedFastFallbacks() const noexcept {
    return cached_fast_fallbacks_;
  }
  void clearCodeCache() noexcept;
  [[nodiscard]] R3000RunResult
  call(std::uint32_t address, std::span<const std::uint32_t> arguments = {},
       std::uint64_t instruction_budget = 1'000'000U) noexcept;

  [[nodiscard]] bool read8(std::uint32_t address,
                           std::uint8_t &value) const noexcept;
  [[nodiscard]] bool read16(std::uint32_t address,
                            std::uint16_t &value) const noexcept;
  [[nodiscard]] bool read32(std::uint32_t address,
                            std::uint32_t &value) const noexcept;
  [[nodiscard]] bool write8(std::uint32_t address, std::uint8_t value) noexcept;
  [[nodiscard]] bool write16(std::uint32_t address,
                             std::uint16_t value) noexcept;
  [[nodiscard]] bool write32(std::uint32_t address,
                             std::uint32_t value) noexcept;
  [[nodiscard]] ProjectedVertexProvenance
  projectedVertexProvenanceAt(std::uint32_t address,
                              std::uint32_t raw_witness) const noexcept;
  [[nodiscard]] ProjectedVertexProvenance
  projectedVertexProvenanceAt(std::uint32_t address) const noexcept;
  [[nodiscard]] const GteProjectedVertex *
  projectedVertexAt(std::uint32_t address) const noexcept;
  [[nodiscard]] std::uint64_t
  projectedVertexIdentityAt(std::uint32_t address) const noexcept;

  [[nodiscard]] const R3000State &state() const noexcept { return state_; }
  [[nodiscard]] std::span<const std::byte> ram() const noexcept { return ram_; }
  [[nodiscard]] std::span<const std::byte> scratchpad() const noexcept {
    return scratchpad_;
  }
  [[nodiscard]] std::span<const std::byte> mmio() const noexcept {
    return mmio_;
  }

private:
  struct PgxpProjectionContext {
    std::uint64_t mesh_vertex_id{};
    std::uint64_t projection_epoch{};
    float screen_h{};
    float screen_offset_x{};
    float screen_offset_y{};
    float view_x{};
    float view_y{};
    float projective_depth{};
    std::uint8_t flags{};
    std::uint8_t enhanced_sources{};

    friend bool operator==(const PgxpProjectionContext &,
                           const PgxpProjectionContext &) = default;

    static constexpr std::uint8_t valid = 1U << 0U;
    static constexpr std::uint8_t ir_saturated = 1U << 1U;
    static constexpr std::uint8_t depth_saturated = 1U << 2U;
    static constexpr std::uint8_t divide_overflow = 1U << 3U;
    static constexpr std::uint8_t screen_saturated = 1U << 4U;
    static constexpr std::uint8_t exact_transform = 1U << 5U;
    static constexpr std::uint8_t fractional_transform = 1U << 6U;

    [[nodiscard]] bool has(std::uint8_t mask) const noexcept {
      return (flags & mask) == mask;
    }
  };
  static_assert(sizeof(PgxpProjectionContext) == 48U);

  // CPU-side PGXP shadow value. Architectural integer state remains the
  // witness; floating components are ignored as soon as that witness differs.
  struct PgxpValue {
    double x{};
    double y{};
    std::uint64_t source_vertex_id{};
    std::uint64_t lineage_x{};
    std::uint64_t lineage_y{};
    float z{};
    std::uint32_t raw{};
    std::uint32_t generation{};
    std::uint32_t context_handle{};
    std::uint8_t flags{};
    bool derived_x : 1 {};
    bool derived_y : 1 {};

    static constexpr std::uint8_t valid_x = 1U << 0U;
    static constexpr std::uint8_t valid_y = 1U << 1U;
    static constexpr std::uint8_t valid_z = 1U << 2U;
    static constexpr std::uint8_t z_from_low = 1U << 3U;
    static constexpr std::uint8_t z_from_high = 1U << 4U;
    static constexpr std::uint8_t tainted_z = 1U << 5U;

    [[nodiscard]] bool has(std::uint8_t mask) const noexcept {
      return (flags & mask) == mask;
    }
  };
  static_assert(sizeof(PgxpValue) == 64U);

  [[nodiscard]] bool
  sameProjectionContext(const PgxpValue &first,
                        const PgxpValue &second) const noexcept;

  struct PgxpContextSlot {
    PgxpProjectionContext context{};
    std::uint32_t handle{};
  };
  static_assert(sizeof(PgxpContextSlot) == 56U);

  struct PgxpMemoryValue {
    PgxpValue value{};
    GteProjectedVertex projected{};
  };
  static_assert(sizeof(PgxpMemoryValue) == 144U);

  struct ProjectedHalf {
    GteProjectedVertex projected{};
    float screen_position{};
    std::uint32_t register_value{};
    std::uint16_t value{};
    std::uint8_t slot{};
    std::uint8_t register_slot{};
    bool derived{};
    bool valid{};
  };

  struct DirectProjectionMemoryValue {
    GteProjectedVertex projected{};
    std::uint32_t raw{};
  };

  struct ProjectedHalfPair {
    std::array<ProjectedHalf, 2> halves{};
  };

  struct TrackedWord {
    std::uint32_t key{};
    std::uint32_t raw{};
    std::uint32_t generation{};
    std::uint32_t age{};
    GteProjectedVertex projected{};
    ProjectedHalfPair projected_halves{};
    PgxpValue pgxp{};
  };

  struct ExactTrackingState {
    std::array<GteExactWord, 32> gpr{};
    std::array<PgxpValue, 34> pgxp_gpr{};
    PgxpValue pgxp_load_delay{};
    PgxpValue pgxp_next_load_delay{};
    GteExactWord load_delay{};
    GteExactWord next_load_delay{};
    GteExactState gte{};
    std::uint64_t pgxp_gpr_valid_mask{};
    std::uint32_t gpr_valid_mask{};
    bool pgxp_load_delay_valid{};
    bool pgxp_next_load_delay_valid{};
    bool load_delay_valid{};
    bool next_load_delay_valid{};
  };

  static constexpr std::size_t tracked_word_capacity = 8192U;
  static constexpr std::size_t tracked_key_count =
      (ram_size + scratchpad_size) / sizeof(std::uint32_t);
  static constexpr std::size_t tracked_presence_words =
      (tracked_key_count + 63U) / 64U;
  static constexpr std::size_t tracked_word_ways = 4U;
  static constexpr std::size_t pgxp_page_words = 256U;
  static constexpr std::size_t pgxp_page_count =
      (tracked_key_count + pgxp_page_words - 1U) / pgxp_page_words;
  static constexpr std::size_t gpu_projection_catalog_capacity = 65535U;
  static constexpr std::size_t pgxp_context_capacity = 32768U;
  static_assert((pgxp_context_capacity & (pgxp_context_capacity - 1U)) == 0U);

  static constexpr std::size_t code_page_size = 4096U;
  static constexpr std::size_t code_page_count = ram_size / code_page_size;
  static constexpr std::size_t cached_block_capacity = 8192U;
  static constexpr std::size_t cached_block_max_instructions = 32U;
  static constexpr std::size_t execution_breakpoint_capacity = 32U;
  static_assert((cached_block_capacity & (cached_block_capacity - 1U)) == 0U);

  struct CachedBlock {
    std::array<R3000CachedInstruction, cached_block_max_instructions>
        instructions{};
    std::array<std::uint32_t, 2U> page_generations{};
    std::array<std::uint16_t, 2U> pages{};
    std::uint32_t start_pc{0xffffffffU};
    std::uint32_t reset_generation{};
    std::uint8_t instruction_count{};
    std::uint8_t page_count{};
  };

  [[nodiscard]] R3000RunResult
  stepImpl(const std::uint32_t *cached_instruction) noexcept;
  [[nodiscard]] static R3000CachedInstruction
  decodeCachedInstruction(std::uint32_t instruction) noexcept;
  [[nodiscard]] bool stepCachedFast(const R3000CachedInstruction &instruction,
                                    R3000RunResult &result) noexcept;
  [[nodiscard]] R3000StopReason loadHalfword(std::uint32_t address,
                                             std::uint8_t reg,
                                             bool sign_extend) noexcept;
  [[nodiscard]] R3000StopReason
  storeHalfword(std::uint32_t address, std::uint8_t reg, std::uint32_t value,
                std::uint32_t producer_pc) noexcept;
  void writeAddImmediate(std::uint8_t source, std::uint8_t destination,
                         std::uint32_t source_value,
                         std::uint32_t immediate_value, std::uint32_t result,
                         bool preserve_direct_projection) noexcept;
  [[nodiscard]] CachedBlock &buildCachedBlock(CachedBlock &block,
                                              std::uint32_t start_pc) noexcept;
  void invalidateCodePage(std::uint32_t physical_address) noexcept;
  void rebuildBreakpointPages() noexcept;

  [[nodiscard]] std::byte *memoryByte(std::uint32_t address) noexcept;
  [[nodiscard]] const std::byte *
  memoryByte(std::uint32_t address) const noexcept;
  [[nodiscard]] static bool physicalAddress(std::uint32_t address,
                                            std::uint32_t &physical) noexcept;
  [[nodiscard]] bool readMmio(std::uint32_t address, R3000AccessWidth width,
                              std::uint32_t &value) const noexcept;
  [[nodiscard]] bool
  writeMmio(std::uint32_t address, R3000AccessWidth width, std::uint32_t value,
            const GteProjectedVertex *projected = nullptr,
            std::uint64_t projection_identity = 0U,
            std::uint32_t producer_pc = 0xffffffffU) noexcept;
  [[nodiscard]] bool
  write32Projected(std::uint32_t address, std::uint32_t value,
                   const GteProjectedVertex *projected,
                   std::uint32_t producer_pc = 0xffffffffU) noexcept;
  [[nodiscard]] bool
  write16Projected(std::uint32_t address, std::uint16_t value,
                   const ProjectedHalf *projected_half,
                   const GteExactWord *exact_word = nullptr,
                   const PgxpValue *pgxp = nullptr,
                   std::uint32_t producer_pc = 0xffffffffU) noexcept;
  void writeRegister(std::uint8_t reg, std::uint32_t value,
                     const GteProjectedVertex *projected = nullptr,
                     const GteExactWord *exact_word = nullptr,
                     const PgxpValue *pgxp = nullptr,
                     const ProjectedHalf *projected_half = nullptr) noexcept;
  void scheduleLoad(std::uint8_t reg, std::uint32_t value,
                    const GteProjectedVertex *projected = nullptr,
                    const ProjectedHalf *projected_half = nullptr,
                    const GteExactWord *exact_word = nullptr,
                    const PgxpValue *pgxp = nullptr) noexcept;
  void advanceLoadDelay() noexcept;
  void flushLoadDelay() noexcept;
  void clearLoadDelay() noexcept;
  void takeInterrupt() noexcept;
  [[nodiscard]] static bool trackedWordKey(std::uint32_t address,
                                           std::uint32_t &key) noexcept;
  [[nodiscard]] const GteProjectedVertex *
  projectedRegister(std::uint8_t reg, std::uint32_t value) noexcept;
  [[nodiscard]] ProjectedHalf
  projectedHalfAt(std::uint32_t address, std::uint16_t value,
                  std::uint32_t register_value,
                  std::uint32_t packed) const noexcept;
  [[nodiscard]] const ProjectedHalf *
  projectedHalfRegister(std::uint8_t reg, std::uint32_t value) const noexcept;
  [[nodiscard]] ProjectedHalf
  screenProjectionHalf(std::uint8_t reg, std::uint32_t value,
                       std::uint8_t register_slot,
                       std::uint32_t source_mask) noexcept;
  [[nodiscard]] const GteExactWord *
  exactRegister(std::uint8_t reg, std::uint32_t value) const noexcept;
  enum class ExactCarrierOperation : std::uint8_t {
    identity,
    add,
    subtract,
    shift_left_16,
    shift_right_logical_16,
    shift_right_arithmetic_16,
    and_low_16,
    or_nonoverlap,
  };
  [[nodiscard]] bool
  buildExactCarrier(ExactCarrierOperation operation, std::uint8_t first_reg,
                    std::uint32_t first_raw, std::uint8_t second_reg,
                    std::uint32_t second_raw, std::uint32_t result_raw,
                    std::uint8_t shift, GteExactWord &result) const noexcept;
  [[nodiscard]] bool
  tryWriteExactCarrier(std::uint8_t destination, std::uint32_t result_raw,
                       ExactCarrierOperation operation, std::uint8_t first_reg,
                       std::uint32_t first_raw, std::uint8_t second_reg,
                       std::uint32_t second_raw, std::uint8_t shift,
                       const GteProjectedVertex *projected,
                       const PgxpValue *pgxp,
                       const ProjectedHalf *projected_half) noexcept;
  [[nodiscard]] const GteExactWord *
  exactWordAt(std::uint32_t address, std::uint32_t value) const noexcept;
  [[nodiscard]] const PgxpValue *
  pgxpRegister(std::uint8_t reg, std::uint32_t value) const noexcept;
  [[nodiscard]] const PgxpValue *pgxpWordAt(std::uint32_t address,
                                            std::uint32_t value) const noexcept;
  [[nodiscard]] PgxpMemoryValue *pgxpMemoryWord(std::uint32_t address,
                                                bool create) noexcept;
  [[nodiscard]] const PgxpMemoryValue *
  pgxpMemoryWord(std::uint32_t address) const noexcept;
  [[nodiscard]] DirectProjectionMemoryValue *
  directProjectionMemoryWord(std::uint32_t address, bool create) noexcept;
  [[nodiscard]] const DirectProjectionMemoryValue *
  directProjectionMemoryWord(std::uint32_t address) const noexcept;
  void clearDirectProjectionMemory() noexcept;
  [[nodiscard]] GteExactWord *exactMemoryWord(std::uint32_t address,
                                              bool create) noexcept;
  [[nodiscard]] const GteExactWord *
  exactMemoryWord(std::uint32_t address) const noexcept;
  [[nodiscard]] std::uint32_t
  publishPgxpContext(const PgxpProjectionContext &context) noexcept;
  [[nodiscard]] const PgxpProjectionContext *
  pgxpContext(const PgxpValue &value) const noexcept;
  [[nodiscard]] PgxpValue
  pgxpFromProjected(const GteProjectedVertex &projected) noexcept;
  [[nodiscard]] GteProjectedVertex
  pgxpToProjected(const PgxpValue &value) const noexcept;
  void storePgxpWord(std::uint32_t address, std::uint32_t raw,
                     const PgxpValue *value) noexcept;
  void storePgxpHalf(std::uint32_t address, std::uint16_t raw,
                     std::uint32_t packed, const PgxpValue *value) noexcept;
  void invalidateProjectedWord(std::uint32_t address) noexcept;
  void storeProjectedVertex(std::uint32_t address,
                            const GteProjectedVertex &projected) noexcept;
  void storeProjectedHalf(std::uint32_t address, std::uint16_t value,
                          std::uint32_t packed,
                          const ProjectedHalf *projected_half) noexcept;
  void storeExactWord(std::uint32_t address, std::uint32_t value,
                      const GteExactWord *exact_word) noexcept;
  void storeExactHalf(std::uint32_t address, std::uint16_t value,
                      std::uint32_t packed,
                      const GteExactWord *exact_word) noexcept;
  [[nodiscard]] TrackedWord *findTrackedWord(std::uint32_t key) noexcept;
  [[nodiscard]] const TrackedWord *
  findTrackedWord(std::uint32_t key) const noexcept;
  [[nodiscard]] TrackedWord *acquireTrackedWord(std::uint32_t key,
                                                std::uint32_t raw) noexcept;
  [[nodiscard]] bool trackedWordMarked(std::uint32_t key) const noexcept;
  void setTrackedWordMarked(std::uint32_t key, bool marked) noexcept;
  void clearPgxpCarriers(bool clear_full_projected = true) noexcept;
  [[nodiscard]] bool exactTransformCarrierTracking() const noexcept;
  void recordGpuProjection(GteProjectedVertex &projection) noexcept;
  [[nodiscard]] std::uint16_t
  ensureGpuProjectionHandle(GteProjectedVertex &projection) noexcept;
  [[nodiscard]] std::uint16_t
  compactProjectionHandle(const GteProjectedVertex *projection,
                          std::uint32_t raw_witness) const noexcept;
  [[nodiscard]] std::uint16_t
  compactProjectionHandleAt(std::uint32_t address) const noexcept;
  void storeCompactProjectionHandle(std::uint32_t address,
                                    std::uint16_t handle) noexcept;
  void invalidateCompactProjectionHandle(std::uint32_t address) noexcept;
  [[nodiscard]] bool idleProjectionCarriersClear() const noexcept;
  void invalidateIdleLoopProof() const noexcept;
  [[nodiscard]] bool readExactTransformValues(
      std::uint32_t address, std::array<double, 9U> &rotation,
      std::array<double, 3U> &translation, bool &enhanced) const noexcept;

  std::vector<std::byte> ram_;
  std::array<std::byte, scratchpad_size> scratchpad_{};
  std::array<std::byte, mmio_size> mmio_{};
  R3000State state_{};
  R3000MmioBus *mmio_bus_{};
  std::unique_ptr<CachedBlock[]> cached_blocks_;
  std::array<std::uint32_t, code_page_count> code_page_generations_{};
  std::array<std::uint8_t, code_page_count> code_page_cached_{};
  std::array<std::uint8_t, code_page_count> breakpoint_pages_{};
  std::array<std::uint32_t, execution_breakpoint_capacity>
      execution_breakpoints_{};
  std::size_t execution_breakpoint_count_{};
  std::uint32_t code_cache_reset_generation_{1U};
  std::uint64_t code_cache_epoch_{1U};
  std::uint64_t cached_fast_fallbacks_{};
  std::array<GteProjectedVertex, 32> projected_gpr_{};
  std::uint32_t projected_gpr_valid_mask_{};
  GteProjectedVertex projected_load_delay_{};
  GteProjectedVertex projected_next_load_delay_{};
  std::array<ProjectedHalf, 32> projected_half_gpr_{};
  std::uint32_t projected_half_gpr_valid_mask_{};
  ProjectedHalf projected_half_load_delay_{};
  ProjectedHalf projected_half_next_load_delay_{};
  std::unique_ptr<ExactTrackingState> exact_tracking_;
  std::vector<std::unique_ptr<DirectProjectionMemoryValue[]>>
      direct_projection_pages_;
  std::unique_ptr<PgxpContextSlot[]> pgxp_contexts_;
  std::unique_ptr<TrackedWord[]> tracked_words_;
  std::unique_ptr<std::uint64_t[]> tracked_word_presence_;
  std::vector<std::unique_ptr<PgxpMemoryValue[]>> pgxp_memory_pages_;
  std::vector<std::unique_ptr<GteExactWord[]>> exact_memory_pages_;
  std::unique_ptr<GteProjectedVertex[]> gpu_projection_catalog_;
  std::unique_ptr<std::uint32_t[]> gpu_projection_handle_words_;
  GteProjectionCommandBackend gte_projection_backend_{};
  std::array<std::uint16_t, 32> gpu_projection_handle_gpr_{};
  std::uint16_t gpu_projection_handle_load_delay_{};
  std::uint16_t gpu_projection_handle_next_load_delay_{};
  std::uint16_t gpu_projection_handle_generation_{1U};
  std::size_t gpu_projection_catalog_size_{};
  std::uint32_t gpu_projection_handle_gpr_mask_{};
  std::uint32_t tracked_generation_{1U};
  mutable std::uint64_t idle_loop_safety_epoch_{1U};
  std::uint32_t tracked_age_{1U};
  std::uint32_t next_pgxp_context_handle_{1U};
  bool tracked_words_present_{};
  bool pgxp_transform_tracking_{};
  bool pgxp_cpu_tracking_{true};
  bool pgxp_exact_transform_tracking_{};
  bool pgxp_exact_transform_capture_enabled_{true};
  bool pgxp_vertex_identity_tracking_{true};
  bool pgxp_preserve_projection_precision_{};
  bool pgxp_memory_overflow_{};
  bool exact_memory_overflow_{};
  bool direct_projection_memory_overflow_{};
  bool gpu_projection_catalog_tracking_{};
  bool gpu_projection_catalog_overflow_{};
  bool gpu_projection_handle_words_present_{};
};

} // namespace sf::psx
