#pragma once

#include "sf/psx/gpu_dma_source.hpp"
#include "sf/psx/gte_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sf::platform::detail {

struct PgxpDrawContext {
  std::uint32_t draw_mode{};
  std::uint32_t draw_area_top_left{};
  std::uint32_t draw_area_bottom_right{};
  std::uint32_t draw_offset{};
  std::uint32_t mask_setting{};

  friend bool operator==(const PgxpDrawContext &,
                         const PgxpDrawContext &) = default;
};

struct PresentationReplayDrawTarget {
  std::uint16_t x{};
  std::uint16_t y{};
  std::uint16_t width{};
  std::uint16_t height{};

  friend bool operator==(const PresentationReplayDrawTarget &,
                         const PresentationReplayDrawTarget &) = default;
};

enum class PresentationReplayEventKind : std::uint8_t {
  draw,
  clear,
};

struct PresentationReplayDrawEvent {
  PresentationReplayEventKind kind{PresentationReplayEventKind::draw};
  std::size_t word_offset{};
  std::size_t word_count{};
  std::size_t projection_offset{};
  std::size_t projection_count{};
  std::size_t dma_source_offset{};
  std::size_t dma_source_count{};
  PgxpDrawContext draw_context{};
  std::uint32_t texture_window{};
  bool precise_candidate{};
  bool allow_perspective{};
  bool allow_precise_screen{};
  bool use_projective_depth{};
};

struct PresentationReplayDrawPage {
  PresentationReplayDrawTarget target{};
  std::vector<std::uint32_t> word_storage;
  std::vector<psx::GteProjectedVertex> projection_storage;
  std::vector<psx::GpuDmaWordSource> dma_source_storage;
  std::vector<PresentationReplayDrawEvent> events;

  [[nodiscard]] std::span<const std::uint32_t>
  commandWords(const PresentationReplayDrawEvent &event) const noexcept {
    return event.word_offset <= word_storage.size() &&
                   event.word_count <= word_storage.size() - event.word_offset
               ? std::span<const std::uint32_t>{word_storage}.subspan(
                     event.word_offset, event.word_count)
               : std::span<const std::uint32_t>{};
  }
  [[nodiscard]] std::span<const psx::GteProjectedVertex>
  resolvedProjections(const PresentationReplayDrawEvent &event) const noexcept {
    return event.projection_offset <= projection_storage.size() &&
                   event.projection_count <=
                       projection_storage.size() - event.projection_offset
               ? std::span<const psx::GteProjectedVertex>{projection_storage}
                     .subspan(event.projection_offset, event.projection_count)
               : std::span<const psx::GteProjectedVertex>{};
  }
  [[nodiscard]] std::span<const psx::GpuDmaWordSource>
  dmaSources(const PresentationReplayDrawEvent &event) const noexcept {
    return event.dma_source_offset <= dma_source_storage.size() &&
                   event.dma_source_count <=
                       dma_source_storage.size() - event.dma_source_offset
               ? std::span<const psx::GpuDmaWordSource>{dma_source_storage}
                     .subspan(event.dma_source_offset, event.dma_source_count)
               : std::span<const psx::GpuDmaWordSource>{};
  }
};

struct PresentationReplayFrame {
  std::uint64_t generation{};
  std::uint16_t display_x{};
  std::uint16_t display_y{};
  std::uint16_t display_width{};
  std::uint16_t display_height{};
  bool display_enabled{};
  bool display_rgb24{};
  bool display_interlaced{};
  bool contains_vram_commands{};
  std::vector<PresentationReplayDrawPage> pages;
};

class PsyCrossGuestGpu final {
public:
  void presentDisplay(std::uint16_t source_x, std::uint16_t source_y,
                      std::uint16_t width, std::uint16_t height, bool enabled,
                      bool rgb24, bool interlaced);
  void submit(std::span<const std::uint32_t> words,
              std::span<const psx::GteProjectedVertex> projections = {},
              std::span<const std::uint64_t> projection_identities = {},
              std::uint64_t command_buffer_epoch = 0U,
              std::span<const psx::GteProjectedVertex> projection_catalog = {},
              std::span<const psx::GpuDmaWordSource> dma_sources = {});
  [[nodiscard]] bool presentInterpolatedDisplay(float alpha);

  void setPresentationInterpolationEnabled(bool enabled) noexcept {
    if (presentation_interpolation_enabled_ == enabled) {
      return;
    }
    presentation_interpolation_enabled_ = enabled;
    resetPresentationReplayHistory();
  }

  void setCoherenceEdgeSnapping(bool enabled) noexcept {
    coherence_edge_snapping_enabled_ = enabled;
  }

  void setRuntimeGeometryPolicy(bool coherence_near_only,
                                bool shared_mesh_canonicalization,
                                bool shared_mesh_prepass) noexcept {
    coherence_near_only_enabled_ = coherence_near_only;
    shared_mesh_canonicalization_enabled_ = shared_mesh_canonicalization;
    shared_mesh_prepass_enabled_ = shared_mesh_prepass;
  }

  [[nodiscard]] bool presentationReplayReady() const noexcept;
  [[nodiscard]] const PresentationReplayFrame &
  currentPresentationReplayFrame() const noexcept {
    return current_presentation_replay_frame_;
  }
  [[nodiscard]] const PresentationReplayFrame &
  previousPresentationReplayFrame() const noexcept {
    return previous_presentation_replay_frame_;
  }
  [[nodiscard]] std::uint64_t
  capturedPresentationReplayEvents() const noexcept {
    return captured_presentation_replay_events_;
  }
  [[nodiscard]] std::uint64_t
  promotedPresentationReplayFrames() const noexcept {
    return promoted_presentation_replay_frames_;
  }
  [[nodiscard]] std::uint64_t
  skippedPresentationReplayVramCommands() const noexcept {
    return skipped_presentation_replay_vram_commands_;
  }
  [[nodiscard]] std::uint64_t
  interpolatedPresentationReplayFrames() const noexcept {
    return interpolated_presentation_replay_frames_;
  }
  [[nodiscard]] std::uint64_t
  rejectedPresentationReplayFrames() const noexcept {
    return rejected_presentation_replay_frames_;
  }
  [[nodiscard]] std::uint64_t
  replayedPresentationStateCommands() const noexcept {
    return replayed_presentation_state_commands_;
  }

  void setGeometryOptions(bool master, bool perspective_correction,
                          bool precise_screen_position,
                          bool projective_depth_clamp, bool quad_recovery,
                          bool coherence_recovery,
                          bool atomic_primitive_fallback) noexcept {
    geometry_enabled_ = master;
    perspective_correction_enabled_ = perspective_correction;
    precise_screen_position_enabled_ = precise_screen_position;
    projective_depth_enabled_ = projective_depth_clamp;
    quad_recovery_enabled_ = quad_recovery;
    coherence_edge_snapping_enabled_ = coherence_recovery;
    atomic_fallback_enabled_ = atomic_primitive_fallback;
  }

  [[nodiscard]] std::uint64_t submittedCommands() const noexcept {
    return submitted_commands_;
  }
  [[nodiscard]] std::uint64_t unsupportedCommands() const noexcept {
    return unsupported_commands_;
  }
  [[nodiscard]] std::uint64_t precisePrimitives() const noexcept {
    return precise_primitives_;
  }
  [[nodiscard]] std::uint64_t polygonPrimitives() const noexcept {
    return polygon_primitives_;
  }
  [[nodiscard]] std::uint64_t exactPrecisePrimitives() const noexcept {
    return exact_precise_primitives_;
  }
  [[nodiscard]] std::uint64_t enhancedPrecisePrimitives() const noexcept {
    return enhanced_precise_primitives_;
  }
  [[nodiscard]] std::uint64_t enhancedRotationPrimitives() const noexcept {
    return enhanced_rotation_primitives_;
  }
  [[nodiscard]] std::uint64_t enhancedTranslationPrimitives() const noexcept {
    return enhanced_translation_primitives_;
  }
  [[nodiscard]] std::uint64_t enhancedVectorPrimitives() const noexcept {
    return enhanced_vector_primitives_;
  }
  [[nodiscard]] std::uint64_t exactViewDepthPrimitives() const noexcept {
    return exact_view_depth_primitives_;
  }
  [[nodiscard]] std::uint64_t integerPrecisePrimitives() const noexcept {
    return integer_precise_primitives_;
  }
  [[nodiscard]] std::uint64_t
  missingProjectionVertexBucket(std::size_t missing_vertices) const noexcept {
    return missing_vertices < missing_projection_vertex_buckets_.size()
               ? missing_projection_vertex_buckets_[missing_vertices]
               : 0U;
  }
  [[nodiscard]] std::uint64_t preciseCandidates() const noexcept {
    return precise_candidates_;
  }
  [[nodiscard]] std::uint64_t partialProjectionPrimitives() const noexcept {
    return partial_projection_primitives_;
  }
  [[nodiscard]] std::uint64_t coherenceFallbackPrimitives() const noexcept {
    return coherence_fallback_primitives_;
  }
  [[nodiscard]] std::uint64_t coherenceEdgeBuilds() const noexcept {
    return coherence_edge_builds_;
  }
  [[nodiscard]] std::uint64_t coherenceSnappedPrimitives() const noexcept {
    return coherence_snapped_primitives_;
  }
  [[nodiscard]] std::uint64_t coherenceSnappedVertices() const noexcept {
    return coherence_snapped_vertices_;
  }
  [[nodiscard]] std::uint64_t missingProjectionPrimitives() const noexcept {
    return missing_projection_primitives_;
  }
  [[nodiscard]] std::uint64_t nonfiniteProjectionPrimitives() const noexcept {
    return nonfinite_projection_primitives_;
  }
  [[nodiscard]] std::uint64_t packetMismatchPrimitives() const noexcept {
    return packet_mismatch_primitives_;
  }
  [[nodiscard]] std::uint64_t projectionHazardPrimitives() const noexcept {
    return projection_hazard_primitives_;
  }
  [[nodiscard]] std::uint64_t irSaturationPrimitives() const noexcept {
    return ir_saturation_primitives_;
  }
  [[nodiscard]] std::uint64_t depthSaturationPrimitives() const noexcept {
    return depth_saturation_primitives_;
  }
  [[nodiscard]] std::uint64_t divideOverflowPrimitives() const noexcept {
    return divide_overflow_primitives_;
  }
  [[nodiscard]] std::uint64_t screenSaturationPrimitives() const noexcept {
    return screen_saturation_primitives_;
  }
  [[nodiscard]] std::uint64_t screenMismatchPrimitives() const noexcept {
    return screen_mismatch_primitives_;
  }
  [[nodiscard]] std::uint64_t reprojectionMismatchPrimitives() const noexcept {
    return reprojection_mismatch_primitives_;
  }
  [[nodiscard]] std::uint64_t cameraMismatchPrimitives() const noexcept {
    return camera_mismatch_primitives_;
  }
  [[nodiscard]] std::uint64_t nearPlanePrimitives() const noexcept {
    return near_plane_primitives_;
  }
  [[nodiscard]] std::uint64_t identityRecoveredVertices() const noexcept {
    return identity_recovered_vertices_;
  }
  [[nodiscard]] std::uint64_t identityRecoveredPrimitives() const noexcept {
    return identity_recovered_primitives_;
  }
  [[nodiscard]] std::uint64_t identityCanonicalBuilds() const noexcept {
    return identity_canonical_builds_;
  }
  [[nodiscard]] std::uint64_t identityAmbiguousPrimitives() const noexcept {
    return identity_ambiguous_primitives_;
  }
  [[nodiscard]] std::uint64_t identityConflictPrimitives() const noexcept {
    return identity_ambiguous_primitives_;
  }
  [[nodiscard]] std::uint64_t projectionCatalogBuilds() const noexcept {
    return projection_catalog_builds_;
  }
  [[nodiscard]] std::uint64_t projectionCatalogPrimitives() const noexcept {
    return projection_catalog_primitives_;
  }
  [[nodiscard]] std::uint64_t projectionCatalogAmbiguities() const noexcept {
    return projection_catalog_ambiguities_;
  }
  [[nodiscard]] std::uint64_t planeRecoveredVertices() const noexcept {
    return plane_recovered_vertices_;
  }
  [[nodiscard]] std::uint64_t planeRecoveredPrimitives() const noexcept {
    return plane_recovered_primitives_;
  }
  [[nodiscard]] std::uint64_t planeRecoveryRejectedPrimitives() const noexcept {
    return plane_recovery_rejected_primitives_;
  }
  [[nodiscard]] std::uint64_t sharedMeshBuilds() const noexcept {
    return shared_mesh_builds_;
  }
  [[nodiscard]] std::uint64_t sharedMeshConflicts() const noexcept {
    return shared_mesh_conflicts_;
  }
  [[nodiscard]] std::uint64_t sharedMeshFractionalVertices() const noexcept {
    return shared_mesh_fractional_vertices_;
  }
  [[nodiscard]] std::uint64_t sharedMeshSnappedVertices() const noexcept {
    return shared_mesh_snapped_vertices_;
  }
  [[nodiscard]] std::uint64_t sharedMeshUniqueVertices() const noexcept {
    return shared_mesh_unique_vertices_;
  }
  [[nodiscard]] std::uint64_t sharedMeshReusedVertices() const noexcept {
    return shared_mesh_reused_vertices_;
  }
  [[nodiscard]] std::uint64_t sharedMeshSyntheticVertices() const noexcept {
    return shared_mesh_synthetic_vertices_;
  }
  [[nodiscard]] std::uint64_t
  sharedMeshPacketFallbackVertices() const noexcept {
    return shared_mesh_packet_fallback_vertices_;
  }
  [[nodiscard]] std::uint64_t
  missingProjectionPrimitives(std::uint8_t opcode) const noexcept {
    return opcode >= 0x20U && opcode < 0x40U
               ? missing_projection_by_opcode_[opcode - 0x20U]
               : 0U;
  }
  [[nodiscard]] std::uint64_t highResolutionPresents() const noexcept {
    return high_resolution_presents_;
  }
  [[nodiscard]] std::uint64_t fallbackPresents() const noexcept {
    return fallback_presents_;
  }

private:
  struct CanonicalProjectionEntry {
    std::uint64_t identity{};
    psx::GteProjectedVertex projection{};
    std::uint32_t generation{};
    bool ambiguous{};
  };

  struct PackedProjectionEntry {
    std::uint32_t packed_sxy{};
    psx::GteProjectedVertex projection{};
    std::uint32_t generation{};
    bool ambiguous{};
  };

  struct SharedMeshVertexEntry {
    std::uint64_t mesh_vertex_id{};
    std::uint32_t draw_offset{};
    psx::GteProjectedVertex projection{};
    std::uint32_t generation{};
    bool has_projection{};
    bool ambiguous{};
  };

  struct CoherenceEdgeEntry {
    std::int32_t direction_x{};
    std::int32_t direction_y{};
    std::int64_t line_constant{};
    std::int64_t interval_begin{};
    std::int64_t interval_end{};
    PgxpDrawContext draw_context{};
    std::uint32_t generation{};
  };

  struct ProjectionPlane {
    double normal_x{};
    double normal_y{};
    double normal_z{};
    double distance{};
    float screen_h{};
    float screen_offset_x{};
    float screen_offset_y{};
    bool valid{};
  };

  struct ProjectionPlaneEdgeEntry {
    std::uint32_t first_sxy{};
    std::uint32_t second_sxy{};
    std::uint32_t draw_area_top_left{};
    std::uint32_t draw_area_bottom_right{};
    std::uint32_t draw_offset{};
    ProjectionPlane plane{};
    bool ambiguous{};
  };

  [[nodiscard]] static std::size_t
  commandLength(std::span<const std::uint32_t> words) noexcept;
  void rebuildCanonicalProjectionTable();
  [[nodiscard]] const CanonicalProjectionEntry *
  findCanonicalProjection(std::uint64_t identity) const noexcept;
  [[nodiscard]] bool rebuildPackedProjectionTable(
      std::span<const psx::GteProjectedVertex> catalog);
  [[nodiscard]] const PackedProjectionEntry *
  findPackedProjection(std::uint32_t packed_sxy) const noexcept;
  void beginSharedMeshTable(std::size_t word_count) noexcept;
  void recordSharedMeshVertex(std::uint32_t draw_offset,
                              const psx::GteProjectedVertex *projection,
                              bool precise) noexcept;
  [[nodiscard]] const SharedMeshVertexEntry *
  findSharedMeshVertex(std::uint64_t mesh_vertex_id,
                       std::uint32_t draw_offset) const noexcept;
  [[nodiscard]] bool coherencePolicyDemanded(bool missing_only) const noexcept;
  void prepareCoherenceEdgePolicy(bool use_packed_catalog,
                                  std::size_t catalog_first_word,
                                  bool missing_only = false,
                                  bool near_only = false);
  [[nodiscard]] bool
  hasFallbackEdge(std::uint32_t first, std::uint32_t second,
                  const PgxpDrawContext &draw_context) const noexcept;
  void dispatch(std::span<const std::uint32_t> command,
                std::span<const psx::GteProjectedVertex> projections,
                bool precise_candidate, bool allow_precise,
                bool allow_precise_screen, bool use_projective_depth,
                bool record_statistics = true);
  void clearWrapped(std::uint16_t x, std::uint16_t y, std::uint16_t width,
                    std::uint16_t height, std::uint32_t color);
  void uploadWrapped(std::uint16_t x, std::uint16_t y, std::uint16_t width,
                     std::uint16_t height, const std::uint16_t *pixels);
  void moveWrapped(std::uint16_t source_x, std::uint16_t source_y,
                   std::uint16_t destination_x, std::uint16_t destination_y,
                   std::uint16_t width, std::uint16_t height);
  void capturePresentationReplayDraw(
      std::span<const std::uint32_t> command,
      std::span<const psx::GteProjectedVertex> projections,
      std::span<const psx::GpuDmaWordSource> dma_sources,
      const PgxpDrawContext &draw_context, std::uint32_t texture_window,
      bool precise_candidate, bool allow_perspective, bool allow_precise_screen,
      bool use_projective_depth);
  void capturePresentationReplayClear(
      std::span<const std::uint32_t> command,
      std::span<const psx::GpuDmaWordSource> dma_sources,
      const PgxpDrawContext &draw_context, std::uint32_t texture_window);
  void notePresentationReplayVramCommand();
  void promotePresentationReplayFrame(std::uint16_t source_x,
                                      std::uint16_t source_y,
                                      std::uint16_t width, std::uint16_t height,
                                      bool enabled, bool rgb24,
                                      bool interlaced);
  void rebuildPresentationReplayPlan();
  void resetPresentationReplayHistory() noexcept;
  struct PresentationReplayInterpolationPlan {
    static constexpr std::size_t invalid_index = static_cast<std::size_t>(-1);

    std::size_t previous_page{invalid_index};
    std::size_t current_page{invalid_index};
    std::vector<std::size_t> previous_projection_for_current_word;
    bool ready{};
  };
  std::vector<std::uint32_t> pending_;
  std::vector<psx::GteProjectedVertex> pending_projections_;
  std::vector<std::uint64_t> pending_projection_identities_;
  std::vector<psx::GpuDmaWordSource> pending_dma_sources_;
  PresentationReplayFrame pending_presentation_replay_frame_;
  PresentationReplayFrame previous_presentation_replay_frame_;
  PresentationReplayFrame current_presentation_replay_frame_;
  PresentationReplayInterpolationPlan presentation_replay_plan_;
  std::uint64_t next_presentation_replay_generation_{1U};
  std::uint64_t captured_presentation_replay_events_{};
  std::uint64_t promoted_presentation_replay_frames_{};
  std::uint64_t skipped_presentation_replay_vram_commands_{};
  std::uint64_t interpolated_presentation_replay_frames_{};
  std::uint64_t rejected_presentation_replay_frames_{};
  std::uint64_t replayed_presentation_state_commands_{};
  std::uint32_t presentation_mask_setting_{};
  bool presentation_interpolation_enabled_{};
  std::vector<CanonicalProjectionEntry> canonical_projection_table_;
  std::uint32_t canonical_projection_generation_{};
  bool canonical_projection_overflow_{};
  std::vector<PackedProjectionEntry> packed_projection_table_;
  std::uint32_t packed_projection_generation_{};
  bool packed_projection_overflow_{};
  std::vector<SharedMeshVertexEntry> shared_mesh_vertex_table_;
  std::uint32_t shared_mesh_generation_{};
  bool shared_mesh_overflow_{};
  bool shared_mesh_policy_active_{};
  std::vector<CoherenceEdgeEntry> coherence_edge_table_;
  std::vector<CoherenceEdgeEntry> coherence_edge_scratch_;
  std::vector<PgxpDrawContext> coherence_draw_contexts_;
  std::vector<std::uint8_t> coherence_precise_status_;
  std::vector<std::uint8_t> coherence_reject_reasons_;
  std::vector<ProjectionPlaneEdgeEntry> projection_plane_edges_;
  std::vector<psx::GteProjectedVertex> plane_recovered_word_projections_;
  std::uint32_t coherence_edge_generation_{};
  bool coherence_edge_active_{};
  bool coherence_policy_active_{};
  bool coherence_edge_near_only_{};
  bool coherence_edge_snapping_enabled_{};
  bool coherence_near_only_enabled_{};
  bool shared_mesh_canonicalization_enabled_{true};
  bool shared_mesh_prepass_enabled_{true};
  bool geometry_enabled_{true};
  bool perspective_correction_enabled_{true};
  bool precise_screen_position_enabled_{true};
  bool projective_depth_enabled_{true};
  bool quad_recovery_enabled_{};
  bool atomic_fallback_enabled_{true};
  std::uint64_t command_buffer_epoch_{};
  std::uint64_t submitted_commands_{};
  std::vector<std::uint16_t> transfer_words_;
  std::uint64_t unsupported_commands_{};
  std::uint64_t precise_primitives_{};
  std::uint64_t polygon_primitives_{};
  std::uint64_t exact_precise_primitives_{};
  std::uint64_t enhanced_precise_primitives_{};
  std::uint64_t enhanced_rotation_primitives_{};
  std::uint64_t enhanced_translation_primitives_{};
  std::uint64_t enhanced_vector_primitives_{};
  std::uint64_t exact_view_depth_primitives_{};
  std::uint64_t integer_precise_primitives_{};
  std::uint64_t precise_candidates_{};
  std::uint64_t partial_projection_primitives_{};
  std::uint64_t coherence_fallback_primitives_{};
  std::uint64_t coherence_edge_builds_{};
  std::uint64_t coherence_snapped_primitives_{};
  std::uint64_t coherence_snapped_vertices_{};
  std::uint64_t missing_projection_primitives_{};
  std::uint64_t nonfinite_projection_primitives_{};
  std::uint64_t packet_mismatch_primitives_{};
  std::uint64_t projection_hazard_primitives_{};
  std::uint64_t ir_saturation_primitives_{};
  std::uint64_t depth_saturation_primitives_{};
  std::uint64_t divide_overflow_primitives_{};
  std::uint64_t screen_saturation_primitives_{};
  std::uint64_t screen_mismatch_primitives_{};
  std::uint64_t reprojection_mismatch_primitives_{};
  std::uint64_t camera_mismatch_primitives_{};
  std::uint64_t near_plane_primitives_{};
  std::uint64_t identity_recovered_vertices_{};
  std::uint64_t identity_recovered_primitives_{};
  std::uint64_t identity_canonical_builds_{};
  std::uint64_t identity_ambiguous_primitives_{};
  std::uint64_t projection_catalog_builds_{};
  std::uint64_t projection_catalog_primitives_{};
  std::uint64_t projection_catalog_ambiguities_{};
  std::uint64_t plane_recovered_vertices_{};
  std::uint64_t plane_recovered_primitives_{};
  std::uint64_t plane_recovery_rejected_primitives_{};
  std::array<std::uint64_t, 5U> missing_projection_vertex_buckets_{};
  std::uint64_t shared_mesh_builds_{};
  std::uint64_t shared_mesh_conflicts_{};
  std::uint64_t shared_mesh_fractional_vertices_{};
  std::uint64_t shared_mesh_snapped_vertices_{};
  std::uint64_t shared_mesh_unique_vertices_{};
  std::uint64_t shared_mesh_reused_vertices_{};
  std::uint64_t shared_mesh_synthetic_vertices_{};
  std::uint64_t shared_mesh_packet_fallback_vertices_{};
  std::array<std::uint64_t, 0x20U> missing_projection_by_opcode_{};
  std::uint64_t high_resolution_presents_{};
  std::uint64_t fallback_presents_{};
};

} // namespace sf::platform::detail
