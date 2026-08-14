#include "PsyX/PsyX_gte.h"

#include "PsyX/PsyX_globals.h"
#include "psx/types.h"
#include "PsyX/common/pgxp_defs.h"
#include "psx/gtereg.h"
#include "psx/inline_c.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace {

[[nodiscard]] std::uint64_t mixMeshIdentity(std::uint64_t identity,
                                             std::uint32_t value) noexcept {
  identity ^= static_cast<std::uint64_t>(value) + 0x9e3779b97f4a7c15ULL +
              (identity << 6U) + (identity >> 2U);
  identity ^= identity >> 29U;
  identity *= 0xbf58476d1ce4e5b9ULL;
  identity ^= identity >> 31U;
  return identity;
}

[[nodiscard]] std::uint64_t
meshVertexIdentity(const std::uint32_t data[32],
                   const std::uint32_t control[32], std::uint32_t instruction,
                   std::uint32_t vector_index) noexcept {
  auto identity = std::uint64_t{0x505359584d455348ULL};
  identity = mixMeshIdentity(identity, data[vector_index * 2U]);
  identity =
      mixMeshIdentity(identity, data[vector_index * 2U + 1U] & 0xffffU);
  for (std::uint32_t index{}; index <= 7U; ++index) {
    identity = mixMeshIdentity(identity, control[index]);
  }
  identity = mixMeshIdentity(identity, control[24U]);
  identity = mixMeshIdentity(identity, control[25U]);
  identity = mixMeshIdentity(identity, control[26U] & 0xffffU);
  constexpr auto transform_mode_mask = (1U << 19U) | (1U << 10U);
  identity = mixMeshIdentity(identity, instruction & transform_mode_mask);
  return identity == 0U ? 1U : identity;
}

} // namespace

extern "C" int PsyX_GteExecuteGuestProjection(
    std::uint32_t data[32], std::uint32_t control[32],
    std::uint32_t instruction, PsyXGuestGteProjection projections[3],
    std::uint32_t *projection_count) {
  if (data == nullptr || control == nullptr || projections == nullptr ||
      projection_count == nullptr) {
    return 0;
  }

  const auto function = instruction & 0x3fU;
  const auto expected_count =
      function == 0x01U ? std::uint32_t{1U}
                        : (function == 0x30U ? std::uint32_t{3U} : 0U);
  if (expected_count == 0U) {
    *projection_count = 0U;
    return 0;
  }

  std::array<std::uint64_t, 3U> mesh_vertex_ids{};
  for (std::uint32_t index{}; index < expected_count; ++index) {
    mesh_vertex_ids[index] =
        meshVertexIdentity(data, control, instruction, index);
  }

  std::copy_n(data, 32U, gteRegs.CP2D.r);
  std::copy_n(control, 32U, gteRegs.CP2C.r);

#if USE_PGXP
  PGXP_MatrixInvalidateCurrent();
  PGXP_MatrixInvalidateCurrentTranslation();
  for (auto slot = 0; slot < 3; ++slot) {
    PGXP_VectorInvalidateCurrent(slot);
  }
  const auto cache_mark = PGXP_MarkCache();
#else
  const auto cache_mark = 0U;
#endif

  const auto executed = doCOP2(static_cast<int>(instruction));
  std::copy_n(gteRegs.CP2D.r, 32U, data);
  std::copy_n(gteRegs.CP2C.r, 32U, control);

  *projection_count = 0U;
#if USE_PGXP
  std::array<PsyXGuestGteProjection, 3U> captured{};
  auto complete = executed != 0;
  for (std::uint32_t index{}; complete && index < expected_count; ++index) {
    PGXPVData source{};
    const auto cache_index = cache_mark + index;
    complete = cache_index < 0xffffU &&
               PGXP_GetCacheDataExact(
                   &source, static_cast<unsigned short>(cache_index)) != 0;
    if (!complete) {
      break;
    }
    auto &destination = captured[index];
    const auto register_index = function == 0x01U ? 14U : 12U + index;
    destination.packed_sxy = data[register_index];
    destination.view_x = source.px * 128.0F;
    destination.view_y = source.py * 128.0F;
    destination.view_z = source.pz * 128.0F;
    const auto depth_register =
        function == 0x01U ? 19U : 17U + index;
    destination.projective_depth = std::max(
        source.scr_h * 0.5F, static_cast<float>(data[depth_register] & 0xffffU));
    destination.screen_x = source.sx;
    destination.screen_y = source.sy;
    destination.screen_h = source.scr_h;
    destination.screen_offset_x = source.ofx;
    destination.screen_offset_y = source.ofy;
    destination.mesh_vertex_id = mesh_vertex_ids[index];
    destination.exact_projection = source.exact_projection;
    destination.valid = 1U;
  }
  PGXP_RewindCache(cache_mark);
  if (complete) {
    std::copy_n(captured.begin(), expected_count, projections);
    *projection_count = expected_count;
  }
#endif
  return executed;
}
